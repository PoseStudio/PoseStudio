// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file morphresolver.cpp
 * @brief Implementation of resolveDialedMorphs. See morphresolver.h.
 *
 * The walk in three steps:
 *   1. SEED    — the dials the scene sets on the figure being loaded (FigureScope).
 *   2. DISCOVER — from the seeds, load each reached channel's modifier and compile its formulas;
 *                 a formula's `?value` output is reached in turn. Reachability does not depend on
 *                 any value, so the graph is fixed before anything is evaluated.
 *   3. EVALUATE — to a fixed point: value = clamp((dial + sum-stage formulas) x mult-stage ones).
 *
 * Channels are keyed by their DECODED id (see figureutils.h): the scene's modifier urls, the
 * formulas' outputs and pushes and the files' own "id" fields then all name one channel the same
 * way, however each spells it.
 */

#include "morphresolver.h"

#include "figuredocument.h"
#include "figureutils.h"
#include "formulaops.h"
#include "uriresolver.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pose {

namespace {

// POSESTUDIO_MORPH_TRACE=1 prints the walk (see evaluate). POSESTUDIO_MORPH_NO_LIMITS=1 is the A/B
// of the channel limits: every value as its dial and formulas give it, however far past its range.
const bool kTrace = std::getenv("POSESTUDIO_MORPH_TRACE") != nullptr;
const bool kNoLimits = std::getenv("POSESTUDIO_MORPH_NO_LIMITS") != nullptr;

// The spline through @p knots (ascending in x) at @p x: flat outside the knot range; inside it a
// straight line for spline_linear, the earlier knot's value for spline_constant, else a
// Catmull-Rom Hermite clamped to its segment's end values — the corrective evaluator's reading of
// the same knots (scene/correctiveset.cpp), so a control and a corrective written alike come out
// alike.
double evalSpline(const std::vector<FormulaKnot>& knots, double x, FormulaOp::Interpolation interpolation) {
    if (knots.empty()) {
        return x;
    }
    if (knots.size() == 1 || x <= knots.front().x) {
        return knots.front().y;
    }
    if (x >= knots.back().x) {
        return knots.back().y;
    }
    std::size_t i = 0;
    while (i + 1 < knots.size() && x > knots[i + 1].x) {
        ++i;
    }
    const FormulaKnot& p1 = knots[i];
    const FormulaKnot& p2 = knots[i + 1];
    const double       h = p2.x - p1.x;
    if (h <= 1e-12) {
        return p1.y;
    }
    if (interpolation == FormulaOp::Interpolation::Step) {
        return x >= p2.x ? p2.y : p1.y; // a knot's value holds from the knot on
    }
    const double u = (x - p1.x) / h;
    if (interpolation == FormulaOp::Interpolation::Linear) {
        return p1.y + (p2.y - p1.y) * u;
    }
    const double y0 = (i > 0) ? knots[i - 1].y : p1.y; // one-sided at the ends
    const double y3 = (i + 2 < knots.size()) ? knots[i + 2].y : p2.y;
    const double m1 = 0.5 * (p2.y - y0);
    const double m2 = 0.5 * (y3 - p1.y);
    const double u2 = u * u;
    const double u3 = u2 * u;
    const double y = (2.0 * u3 - 3.0 * u2 + 1.0) * p1.y + (u3 - 2.0 * u2 + u) * m1 +
                     (-2.0 * u3 + 3.0 * u2) * p2.y + (u3 - u2) * m2;
    return std::clamp(y, std::min(p1.y, p2.y), std::max(p1.y, p2.y));
}

// One pre-compiled formula operation. The evaluation runs several fixed-point rounds over every
// reachable formula, so a formula's operations are translated ONCE at discovery into this form.
struct CompiledOp {
    enum class Kind { PushValue, PushChannel, Mult, Add, Sub, Div, Inv, Neg, Spline };
    Kind                     kind = Kind::PushValue;
    double                   value = 0.0;  ///< PushValue: the constant.
    int                      channel = -1; ///< PushChannel: the channel read (an index into the walk's list).
    FormulaOp::Interpolation interpolation = FormulaOp::Interpolation::Smooth; ///< Spline.
    std::vector<FormulaKnot> knots;        ///< Spline: ascending in x.
};

// Evaluates a compiled formula — a small stack machine — against the current channel values.
double evalOperations(const std::vector<CompiledOp>& operations, const std::vector<double>& values) {
    std::vector<double> stack;
    auto pop = [&stack]() -> double {
        if (stack.empty()) {
            return 0.0;
        }
        const double v = stack.back();
        stack.pop_back();
        return v;
    };
    for (const CompiledOp& op : operations) {
        switch (op.kind) {
            case CompiledOp::Kind::PushValue:
                stack.push_back(op.value);
                break;
            case CompiledOp::Kind::PushChannel:
                stack.push_back(values[static_cast<std::size_t>(op.channel)]);
                break;
            case CompiledOp::Kind::Mult: {
                const double b = pop();
                stack.push_back(pop() * b);
                break;
            }
            case CompiledOp::Kind::Add:
                stack.push_back(pop() + pop());
                break;
            case CompiledOp::Kind::Sub: {
                const double b = pop();
                stack.push_back(pop() - b);
                break;
            }
            case CompiledOp::Kind::Div: {
                const double b = pop();
                stack.push_back(b != 0.0 ? pop() / b : (pop(), 0.0));
                break;
            }
            case CompiledOp::Kind::Inv:
                stack.push_back(invert(pop()));
                break;
            case CompiledOp::Kind::Neg:
                stack.push_back(-pop());
                break;
            case CompiledOp::Kind::Spline:
                stack.push_back(evalSpline(op.knots, pop(), op.interpolation));
                break;
        }
    }
    return stack.empty() ? 0.0 : stack.back();
}

// A channel's limits as its file (and then the scene, where it overrides them) states them.
struct Limits {
    bool   clamped = false;
    bool   hasMin = false, hasMax = false;
    double lo = 0.0, hi = 0.0;

    void read(const nlohmann::json& channel) {
        if (const auto c = channel.find("clamped"); c != channel.end() && c->is_boolean()) {
            clamped = c->get<bool>();
        }
        hasMin = numberOf(channel, "min", lo) || hasMin;
        hasMax = numberOf(channel, "max", hi) || hasMax;
    }
    double apply(double v) const {
        if (!clamped || kNoLimits) {
            return v;
        }
        if (hasMin && hasMax && lo > hi) {
            return v; // limits that contradict each other limit nothing
        }
        if (hasMin) {
            v = std::max(v, lo);
        }
        if (hasMax) {
            v = std::min(v, hi);
        }
        return v;
    }
};

// One channel the walk knows of: a dial of the scene, the output of a formula, or merely something
// a formula reads.
struct Channel {
    std::string id;                 ///< Decoded id — the key.
    std::string rawId;              ///< The id as its first reference spelled it (for building a url).
    std::string url;                ///< "/file.dsf#frag" as a reference wrote it ("#frag": in the preset itself); empty if no reference named a file.
    bool        fileGuessed = false; ///< No reference named its file: `url` is the file the reference stood in.
    bool        dialed = false;     ///< The scene sets it on this figure …
    double      dial = 0.0;         ///< … to this.
    const nlohmann::json* sceneChannel = nullptr; ///< The scene's channel block (it may override the limits).
    bool        reached = false;    ///< Dialed, or driven by a compiled formula: its own formulas are part of the graph.
    bool        looked = false;     ///< Its file was looked for.
    const nlohmann::json* modifier = nullptr; ///< Its modifier in that file, when found.
    std::string why;                ///< Why not, when not.
    double      defaultValue = 0.0; ///< The channel's own value where the scene does not dial it.
    Limits      limits;
};

// One reachable formula, compiled: where its result goes plus its operation program.
struct CompiledFormula {
    enum class Target { Channel, JointCenter, FigureScale };
    Target                  target = Target::Channel;
    int                     channel = -1; ///< Target::Channel.
    std::string             bone;         ///< Target::JointCenter: the bone's id …
    int                     axis = 0;     ///< … and which of x/y/z.
    bool                    mult = false; ///< The "mult" stage: scales the channel instead of adding to it.
    std::vector<CompiledOp> ops;
};

// Maps a channel property's trailing axis ("center_point/x" -> 0, ".../y" -> 1, ".../z" -> 2).
int axisIndexOf(const std::string& property) {
    return property.empty() ? -1 : axisIndex(property.back());
}

// What a non-`value` property reads at the bind pose: a scale is 1, everything else (a rotation,
// a translation) 0.
double bindValueOf(const std::string& property) {
    return property.rfind("scale", 0) == 0 ? 1.0 : 0.0;
}

// The whole walk's state.
class Walk {
public:
    Walk(const nlohmann::json& presetRoot, UriResolver& resolver, const std::string& presetDir,
         const FigureScope& scope)
        : m_presetRoot(presetRoot), m_resolver(resolver), m_presetDir(presetDir), m_scope(scope) {}

    ResolvedShape run() {
        ResolvedShape out;
        const auto scene = m_presetRoot.find("scene");
        if (scene == m_presetRoot.end() || !scene->contains("modifiers")) {
            return out;
        }
        seed((*scene)["modifiers"]);
        discover();
        evaluate(out);
        emit(out);
        return out;
    }

private:
    // The channel a reference names, created on first mention. @p ownerUrl is the url of the
    // channel whose file the reference was written in: a reference with no file part ("#Other")
    // names a channel of that same file.
    int channelFor(const ChannelRef& ref, const std::string& ownerUrl) {
        const auto found = m_index.find(ref.decodedKey);
        int        index;
        if (found != m_index.end()) {
            index = found->second;
        } else {
            index = static_cast<int>(m_channels.size());
            m_channels.emplace_back();
            m_channels.back().id = ref.decodedKey;
            m_index.emplace(ref.decodedKey, index);
        }
        Channel&   c = m_channels[static_cast<std::size_t>(index)];
        const bool namesFile = !ref.fileUrl.empty() && ref.fileUrl[0] == '/';
        if (c.url.empty()) {
            c.rawId = ref.rawKey;
            if (namesFile) {
                c.url = ref.fileUrl;
            } else if (ref.fileUrl.empty() || ref.fileUrl[0] == '#') {
                // No file: first taken to be a channel of the file the reference stands in.
                const std::size_t hash = ownerUrl.find('#');
                c.url = (hash == std::string::npos ? ownerUrl : ownerUrl.substr(0, hash)) + '#' + ref.rawKey;
                c.fileGuessed = true;
            }
        } else if (namesFile && c.fileGuessed && !c.modifier) {
            // A later reference that NAMES the file settles a guess that found nothing.
            c.url = ref.fileUrl;
            c.fileGuessed = false;
            if (c.looked) {
                c.looked = false;
                c.why.clear();
                if (c.reached) {
                    m_queue.push_back(index); // walk it now that it can be read
                }
            }
        }
        return index;
    }

    void reach(int index) {
        Channel& c = m_channels[static_cast<std::size_t>(index)];
        if (!c.reached) {
            c.reached = true;
            m_queue.push_back(index);
        }
    }

    // 1. The dials the scene sets on this figure. A dial listed twice keeps its last value.
    void seed(const nlohmann::json& modifiers) {
        for (const auto& mod : modifiers) {
            const auto ch = mod.find("channel");
            const std::string url = stringOf(mod, "url");
            if (ch == mod.end() || !ch->is_object() || url.empty()) {
                continue;
            }
            if (!m_scope.owns(stringOf(mod, "parent"))) {
                continue; // another figure's dial: its eyelashes', its hair's, a second character's
            }
            double     value = 0.0;
            const bool hasValue = numberOf(*ch, "current_value", value) ||
                                  numberOf(*ch, "value", value); // the DIALED value: current_value over value
            const int index = channelFor(parseChannelRef(url), std::string());
            Channel&  c = m_channels[static_cast<std::size_t>(index)];
            // A block that carries NO value sets no dial — a scene that only changed a channel's
            // limits writes just those — and the channel keeps its file's default: taken for a
            // dial of 0, it switched a default-on channel off.
            if (hasValue) {
                c.dialed = true;
                c.dial = value;
            }
            c.sceneChannel = &(*ch);
            reach(index);
        }
    }

    // A reference that names a channel by id ALONE ("Figure:#Other?value") means "that channel of
    // this figure", whichever file defines it. The format's own loader knows every morph of the
    // figure; this one looks where the file nearly always is — under the figure's Morphs folder,
    // named after the id (the folder's names are listed once, on first need; nothing is opened
    // but the one file).
    const nlohmann::json* findByName(Channel& c) {
        if (!m_nameIndexBuilt) {
            m_nameIndexBuilt = true;
            const std::size_t slash = m_scope.assetFile.find_last_of('/');
            if (slash != std::string::npos) {
                for (std::string& uri :
                     m_resolver.listFiles(m_scope.assetFile.substr(0, slash) + "/Morphs", ".dsf")) {
                    const std::size_t nameAt = uri.find_last_of('/') + 1;
                    std::string stem = toLowerAscii(urlDecode(uri.substr(nameAt, uri.size() - nameAt - 4)));
                    m_nameIndex.emplace(std::move(stem), std::move(uri)); // the first content root's wins
                }
            }
        }
        const auto named = m_nameIndex.find(toLowerAscii(c.id));
        if (named == m_nameIndex.end()) {
            return nullptr;
        }
        try {
            std::shared_ptr<const FigureDocument> doc = m_resolver.loadDocument(named->second, m_presetDir);
            const nlohmann::json* modifier = findModifierById(doc->root(), c.id);
            if (modifier) {
                m_documents.push_back(std::move(doc));
                c.url = named->second + '#' + c.rawId;
                c.fileGuessed = false;
            }
            return modifier;
        } catch (const std::exception&) {
            return nullptr;
        }
    }

    // Finds a channel's modifier in its file and reads its default and limits. Once per channel.
    void look(Channel& c) {
        if (c.looked) {
            return;
        }
        c.looked = true;
        const nlohmann::json* root = nullptr;
        if (c.url.empty()) {
            c.why = "no reference names its file";
        } else if (c.url[0] == '#') {
            root = &m_presetRoot; // defined in the preset itself
        } else {
            try {
                std::shared_ptr<const FigureDocument> doc = m_resolver.loadDocument(c.url, m_presetDir);
                root = &doc->root();
                m_documents.push_back(std::move(doc)); // the modifier pointers below point into it
            } catch (const std::exception&) {
                c.why = "file not found under the content folders (" +
                        urlDecode(c.url.substr(0, c.url.find('#'))) + ")";
            }
        }
        if (root) {
            c.modifier = findModifierById(*root, c.id);
            if (!c.modifier) {
                c.why = "its file does not define it";
            }
        }
        if (!c.modifier && c.fileGuessed) {
            c.modifier = findByName(c);
            if (c.modifier) {
                c.why.clear();
            } else {
                c.why = "named by id alone, and no file of the figure's Morphs folder is named after it";
            }
        }
        if (c.modifier) {
            if (const auto ch = c.modifier->find("channel"); ch != c.modifier->end() && ch->is_object()) {
                if (!numberOf(*ch, "value", c.defaultValue)) {
                    numberOf(*ch, "current_value", c.defaultValue);
                }
                c.limits.read(*ch);
            }
        }
        if (c.sceneChannel) {
            c.limits.read(*c.sceneChannel); // a scene that changed a dial's limits says so here
        }
    }

    // Translates a formula's operations (read by formulaops) into the walk's program: a pushed
    // channel VALUE becomes a read of that channel of the walk. Sets @p poseDriven when a push
    // reads anything else — a joint's rotation: the mark of a pose corrective — and @p unknownOp
    // to the first operation outside the vocabulary (the program is then not one to run).
    std::vector<CompiledOp> compile(const nlohmann::json& operations, const std::string& ownerUrl,
                                    bool& poseDriven, std::string& unknownOp) {
        FormulaProgram          program = readFormulaOps(operations);
        unknownOp = program.unknownOp;
        std::vector<CompiledOp> out;
        out.reserve(program.ops.size());
        for (FormulaOp& op : program.ops) {
            CompiledOp c;
            switch (op.kind) {
                case FormulaOp::Kind::PushUrl:
                    if (op.ref.property == "value") {
                        c.kind = CompiledOp::Kind::PushChannel;
                        c.channel = channelFor(op.ref, ownerUrl);
                    } else {
                        poseDriven = true;
                        c.kind = CompiledOp::Kind::PushValue;
                        c.value = bindValueOf(op.ref.property);
                    }
                    break;
                case FormulaOp::Kind::PushConst:
                    c.kind = CompiledOp::Kind::PushValue;
                    c.value = op.value;
                    break;
                case FormulaOp::Kind::Spline:
                    c.kind = CompiledOp::Kind::Spline;
                    c.interpolation = op.interpolation;
                    c.knots = std::move(op.knots);
                    break;
                case FormulaOp::Kind::Mult:
                    c.kind = CompiledOp::Kind::Mult;
                    break;
                case FormulaOp::Kind::Add:
                    c.kind = CompiledOp::Kind::Add;
                    break;
                case FormulaOp::Kind::Sub:
                    c.kind = CompiledOp::Kind::Sub;
                    break;
                case FormulaOp::Kind::Div:
                    c.kind = CompiledOp::Kind::Div;
                    break;
                case FormulaOp::Kind::Inv:
                    c.kind = CompiledOp::Kind::Inv;
                    break;
                case FormulaOp::Kind::Neg:
                    c.kind = CompiledOp::Kind::Neg;
                    break;
                case FormulaOp::Kind::Unknown:
                    continue; // named in unknownOp: the caller does not run this program
            }
            out.push_back(std::move(c));
        }
        return out;
    }

    // Loads the files of the channels @p indices names that have not been looked for yet, across
    // cores, into the resolver's cache — where look() then finds them. Reading, inflating and
    // parsing a morph file is the walk's whole cost, and a scene of a hundred dials reaches a
    // hundred files; one at a time on this thread they were a large slice of such an import.
    // Changes nothing but the time: the same files are read, and looked at in the same order.
    template <typename Indices>
    void prefetch(const Indices& indices) {
        std::vector<std::string> urls;
        for (const int index : indices) {
            const Channel& c = m_channels[static_cast<std::size_t>(index)];
            if (!c.looked && !c.url.empty() && c.url[0] != '#') {
                urls.push_back(c.url);
            }
        }
        if (urls.size() > 1) {
            m_resolver.prefetchDocuments(urls, m_presetDir);
        }
    }

    // 2. Walk the driver graph from the dials: each reached channel's formulas are compiled, and
    // each `?value` they drive is reached in turn. Breadth first, a WAVE at a time — everything
    // queued when a pass begins — so that each wave's files can be loaded together (prefetch).
    void discover() {
        std::size_t wave = 0; // channels left of the wave being walked
        while (!m_queue.empty()) {
            if (wave == 0) {
                prefetch(m_queue);
                wave = m_queue.size();
            }
            --wave;
            const int index = m_queue.front();
            m_queue.pop_front();
            look(m_channels[static_cast<std::size_t>(index)]);
            // (channelFor below may grow m_channels: no reference into it is held across a call.)
            const nlohmann::json* modifier = m_channels[static_cast<std::size_t>(index)].modifier;
            const std::string     ownerUrl = m_channels[static_cast<std::size_t>(index)].url;
            if (!modifier) {
                continue;
            }
            const auto formulas = modifier->find("formulas");
            if (formulas == modifier->end() || !formulas->is_array()) {
                continue;
            }
            for (const auto& formula : *formulas) {
                const ChannelRef output = parseChannelRef(stringOf(formula, "output"));
                if (output.decodedKey.empty()) {
                    continue;
                }
                if (m_scope.namesAnotherFigure(urlDecode(output.scope))) {
                    continue; // a control on this figure driving a morph of her clothes: their shape
                }
                CompiledFormula cf;
                if (output.property == "value") {
                    cf.target = CompiledFormula::Target::Channel;
                    cf.channel = channelFor(output, ownerUrl);
                    reach(cf.channel); // reached whether or not the formula evaluates to anything
                } else if (output.property.rfind("center_point/", 0) == 0) {
                    // A shape morph also drives each bone's center_point (the joint's rest origin)
                    // so the skeleton follows the character's proportions.
                    cf.target = CompiledFormula::Target::JointCenter;
                    cf.bone = output.decodedKey;
                    cf.axis = axisIndexOf(output.property);
                    if (cf.axis < 0) {
                        continue;
                    }
                } else if (output.property.find("scale/general") != std::string::npos ||
                           output.property.find("general_scale") != std::string::npos) {
                    // Only the FIGURE node's scale/general sets a character's height. A bone's is a
                    // propagating-scale rig (a head-scale control drives ~80 of them): summed into
                    // the figure scale, one teen character imported at three times her size. Per-bone
                    // scale is not modelled (the bind is translation-only), so those are dropped.
                    if (output.decodedKey != m_scope.assetId && output.decodedKey != m_scope.nodeId) {
                        continue;
                    }
                    cf.target = CompiledFormula::Target::FigureScale;
                } else {
                    // end_point / orientation do not touch a translation-only bind (the orientation
                    // is read off the node); a bone's translation or rotation is a pose.
                    continue;
                }
                const auto operations = formula.find("operations");
                if (operations == formula.end() || !operations->is_array()) {
                    continue;
                }
                bool        poseDriven = false;
                std::string unknownOp;
                cf.ops = compile(*operations, ownerUrl, poseDriven, unknownOp);
                cf.mult = stringOf(formula, "stage", "sum") == "mult";
                if (poseDriven && cf.target == CompiledFormula::Target::Channel && !cf.mult) {
                    // A morph a JOINT drives is a pose corrective: correctivescan finds it and the
                    // engine blends it live. Baked here as well, it would be applied twice.
                    continue;
                }
                if (!unknownOp.empty()) {
                    // Run without the operation, the formula gives another number altogether (a
                    // "divide by" dropped is a "multiply by"): what it drives goes without it, and
                    // the report names it.
                    const std::string driven =
                        cf.target == CompiledFormula::Target::Channel
                            ? m_channels[static_cast<std::size_t>(cf.channel)].id
                            : cf.target == CompiledFormula::Target::JointCenter ? cf.bone + " (its joint centre)"
                                                                               : std::string("the figure's scale");
                    const std::string line = driven + ": a formula that drives it uses an operation this "
                                                      "importer does not know (" + unknownOp + ")";
                    if (std::find(m_unreadable.begin(), m_unreadable.end(), line) == m_unreadable.end()) {
                        m_unreadable.push_back(line);
                    }
                    continue;
                }
                m_formulas.push_back(std::move(cf));
            }
        }
        // What the formulas merely READ: its own value where nothing dials or drives it.
        std::vector<int> read;
        for (const CompiledFormula& f : m_formulas) {
            for (const CompiledOp& op : f.ops) {
                if (op.kind == CompiledOp::Kind::PushChannel) {
                    read.push_back(op.channel);
                }
            }
        }
        prefetch(read);
        for (const int index : read) {
            look(m_channels[static_cast<std::size_t>(index)]);
        }
    }

    // 3. To a fixed point. Discovery order is breadth-first, not topological: a channel can be
    // reached before everything that drives its own drivers, so each round re-derives every value
    // from the dials plus the previous round's values. A control → head/body controls → component
    // morphs graph is three or four deep; the cap also ends a cycle.
    void evaluate(ResolvedShape& out) {
        const std::size_t n = m_channels.size();
        std::vector<double> base(n), values(n), sum(n), product(n);
        for (std::size_t i = 0; i < n; ++i) {
            const Channel& c = m_channels[i];
            base[i] = c.dialed ? c.dial : c.defaultValue;
            values[i] = c.limits.apply(base[i]);
        }
        double scale = 0.0;
        for (int round = 0; round < 16; ++round) {
            std::fill(sum.begin(), sum.end(), 0.0);
            std::fill(product.begin(), product.end(), 1.0);
            double             roundScale = 0.0;
            JointCenterOffsets roundOffsets;
            for (const CompiledFormula& f : m_formulas) {
                const double r = evalOperations(f.ops, values);
                switch (f.target) {
                    case CompiledFormula::Target::Channel:
                        if (f.mult) {
                            product[static_cast<std::size_t>(f.channel)] *= r;
                        } else {
                            sum[static_cast<std::size_t>(f.channel)] += r;
                        }
                        break;
                    case CompiledFormula::Target::JointCenter:
                        roundOffsets[f.bone][f.axis] += static_cast<float>(r);
                        break;
                    case CompiledFormula::Target::FigureScale:
                        roundScale += r;
                        break;
                }
            }
            bool stable = true;
            for (std::size_t i = 0; i < n; ++i) {
                const double v = m_channels[i].limits.apply((base[i] + sum[i]) * product[i]);
                if (std::abs(v - values[i]) > 1e-9) {
                    stable = false;
                }
                values[i] = v;
            }
            scale = roundScale;
            out.jointCenterOffsets = std::move(roundOffsets);
            if (stable) {
                break;
            }
        }
        out.figureScale = static_cast<float>(1.0 + scale);
        if (kTrace) {
            // POSESTUDIO_MORPH_TRACE=1: every channel of the walk and what it came to — which dial
            // drives what, where a limit bit (raw != value), what could not be found.
            for (std::size_t i = 0; i < n; ++i) {
                const Channel& c = m_channels[i];
                const double   raw = (base[i] + sum[i]) * product[i];
                std::fprintf(stderr,
                             "[morph] %-44s %s%s dial %.4f default %.4f sum %.4f x %.4f = %.4f%s%s%s\n",
                             c.id.c_str(), c.dialed ? "DIALED " : (c.reached ? "driven " : "read   "),
                             c.modifier ? "" : "(not found) ", c.dial, c.defaultValue, sum[i], product[i],
                             values[i], std::abs(raw - values[i]) > 1e-9 ? "  LIMITED from " : "",
                             std::abs(raw - values[i]) > 1e-9 ? std::to_string(raw).c_str() : "",
                             c.modifier && c.modifier->contains("morph") ? "  [shape]" : "");
            }
        }
        m_values = std::move(values);
    }

    // Every reached channel with a meaningful value that was FOUND in a file: the caller loads each
    // and applies its deltas (a control has none); one that was not found is reported, and that is
    // all. In the walk's order — the dials first, as the scene lists
    // them — so the bake adds the same deltas in the same order from one run to the next.
    void emit(ResolvedShape& out) const {
        for (std::size_t i = 0; i < m_channels.size(); ++i) {
            const Channel& c = m_channels[i];
            const bool     negligible = std::abs(m_values[i]) < 1e-4;
            // What the walk KNOWS a channel to be — for the corrective scan's gates, ZERO
            // included: a switch the preset turns off must read 0 there, not its file's default.
            // A channel no file was found for is known only as far as the preset dials it or
            // something drives it away from zero (its own default is anybody's guess).
            if (c.modifier || c.dialed || (c.reached && !negligible)) {
                out.channelValues.emplace(c.id, static_cast<float>(m_values[i]));
            }
            if (!c.reached || negligible) {
                continue;
            }
            if (!c.modifier) {
                // Nothing was found to bake — and where no reference named a file, `url` is only a
                // guess: the file the reference STOOD in, whose own morph must not be taken for
                // this one's.
                out.missing.push_back(c.id + ": " + c.why);
                continue;
            }
            out.morphs.push_back({c.url, static_cast<float>(m_values[i]), c.id});
        }
        out.missing.insert(out.missing.end(), m_unreadable.begin(), m_unreadable.end());
    }

    const nlohmann::json& m_presetRoot;
    UriResolver&          m_resolver;
    const std::string&    m_presetDir;
    const FigureScope&    m_scope;

    std::vector<Channel>                               m_channels;
    std::unordered_map<std::string, int>               m_index; ///< decoded id -> m_channels index
    std::deque<int>                                    m_queue; ///< reached, not yet walked
    std::vector<CompiledFormula>                       m_formulas;
    std::vector<std::string>                           m_unreadable; ///< formulas left out, for the report
    std::vector<std::shared_ptr<const FigureDocument>> m_documents; ///< keeps the walked files alive
    std::vector<double>                                m_values;
    bool                                               m_nameIndexBuilt = false;
    std::unordered_map<std::string, std::string>       m_nameIndex; ///< lowercase file stem -> root-relative uri (findByName)
};

} // namespace

ResolvedShape resolveDialedMorphs(const nlohmann::json& presetRoot, UriResolver& resolver,
                                  const std::string& presetDir, const FigureScope& scope) {
    try {
        return Walk(presetRoot, resolver, presetDir, scope).run();
    } catch (const std::exception& e) {
        // Every read of the walk is a tolerant one (figureutils' stringOf / numberOf, formulaops),
        // so no file of any shape is known to get here. Should one: the base figure, and why —
        // never no figure at all for the sake of an optional shape.
        ResolvedShape none;
        none.missing.push_back(std::string("the preset's dials could not be evaluated (") + e.what() + ")");
        return none;
    }
}

} // namespace pose
