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

// One knot of a formula's spline: at input x the output is y. (The format writes TCB knots as
// [x, y, tension, continuity, bias] with the last three ~0, and linear ones as [x, y].)
struct Knot {
    double x = 0.0;
    double y = 0.0;
};

// The spline through @p knots (ascending in x) at @p x: flat outside the knot range; inside it a
// straight line for spline_linear, else a Catmull-Rom Hermite clamped to its segment's end values
// — the corrective evaluator's reading of the same knots (scene/correctiveset.cpp), so a control
// and a corrective written alike come out alike.
double evalSpline(const std::vector<Knot>& knots, double x, bool linear) {
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
    const Knot&  p1 = knots[i];
    const Knot&  p2 = knots[i + 1];
    const double h = p2.x - p1.x;
    if (h <= 1e-12) {
        return p1.y;
    }
    const double u = (x - p1.x) / h;
    if (linear) {
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
    enum class Kind { PushValue, PushChannel, Mult, Add, Sub, Div, Spline, Other };
    Kind              kind = Kind::Other;
    double            value = 0.0;    ///< PushValue: the constant.
    int               channel = -1;   ///< PushChannel: the channel read (an index into the walk's list).
    bool              linear = false; ///< Spline: spline_linear (else the smooth one).
    std::vector<Knot> knots;          ///< Spline: ascending in x.
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
            case CompiledOp::Kind::Spline:
                stack.push_back(evalSpline(op.knots, pop(), op.linear));
                break;
            case CompiledOp::Kind::Other:
                break; // an op outside the vocabulary leaves the stack as it is
        }
    }
    return stack.empty() ? 0.0 : stack.back();
}

// A number out of a channel field that may be written as a number or (a switch) as a boolean.
bool numberOf(const nlohmann::json& object, const char* key, double& out) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return false;
    }
    if (it->is_number()) {
        out = it->get<double>();
        return true;
    }
    if (it->is_boolean()) {
        out = it->get<bool>() ? 1.0 : 0.0;
        return true;
    }
    return false;
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
    if (property.empty()) {
        return -1;
    }
    switch (property.back()) {
        case 'x': return 0;
        case 'y': return 1;
        case 'z': return 2;
        default:  return -1;
    }
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
            const std::string url = mod.value("url", std::string());
            if (ch == mod.end() || !ch->is_object() || url.empty()) {
                continue;
            }
            if (!m_scope.owns(mod.value("parent", std::string()))) {
                continue; // another figure's dial: its eyelashes', its hair's, a second character's
            }
            double value = 0.0;
            if (!numberOf(*ch, "current_value", value)) {
                numberOf(*ch, "value", value); // the DIALED value: current_value over value
            }
            const int index = channelFor(parseChannelRef(url), std::string());
            Channel&  c = m_channels[static_cast<std::size_t>(index)];
            c.dialed = true;
            c.dial = value;
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

    // Translates a formula's `operations`. Sets @p poseDriven when it reads anything but a
    // channel's value — a joint's rotation: the mark of a pose corrective.
    std::vector<CompiledOp> compile(const nlohmann::json& operations, const std::string& ownerUrl,
                                    bool& poseDriven) {
        std::vector<CompiledOp> out;
        std::vector<Knot>       pendingKnots; // knots accumulate until the spline op consumes them
        for (std::size_t i = 0; i < operations.size(); ++i) {
            const nlohmann::json& op = operations[i];
            const std::string     name = op.value("op", std::string());
            CompiledOp            c;
            if (name == "push") {
                const auto val = op.find("val");
                const auto url = op.find("url");
                if (val != op.end() && val->is_array()) {
                    Knot k;
                    if (val->size() > 0 && (*val)[0].is_number()) k.x = (*val)[0].get<double>();
                    if (val->size() > 1 && (*val)[1].is_number()) k.y = (*val)[1].get<double>();
                    pendingKnots.push_back(k);
                    continue;
                }
                if (val != op.end() && val->is_number()) {
                    // A bare number directly before a spline op is the knot count, not an operand.
                    if (i + 1 < operations.size() &&
                        operations[i + 1].value("op", std::string()).rfind("spline", 0) == 0) {
                        continue;
                    }
                    c.kind = CompiledOp::Kind::PushValue;
                    c.value = val->get<double>();
                } else if (url != op.end() && url->is_string()) {
                    const ChannelRef ref = parseChannelRef(url->get<std::string>());
                    if (ref.property == "value") {
                        c.kind = CompiledOp::Kind::PushChannel;
                        c.channel = channelFor(ref, ownerUrl);
                    } else {
                        poseDriven = true;
                        c.kind = CompiledOp::Kind::PushValue;
                        c.value = bindValueOf(ref.property);
                    }
                } else {
                    c.kind = CompiledOp::Kind::PushValue;
                    c.value = 0.0;
                }
            } else if (name.rfind("spline", 0) == 0) {
                c.kind = CompiledOp::Kind::Spline;
                c.linear = name == "spline_linear";
                c.knots = std::move(pendingKnots);
                pendingKnots.clear();
                std::stable_sort(c.knots.begin(), c.knots.end(),
                                 [](const Knot& a, const Knot& b) { return a.x < b.x; });
            } else if (name == "mult") {
                c.kind = CompiledOp::Kind::Mult;
            } else if (name == "add") {
                c.kind = CompiledOp::Kind::Add;
            } else if (name == "sub") {
                c.kind = CompiledOp::Kind::Sub;
            } else if (name == "div") {
                c.kind = CompiledOp::Kind::Div;
            }
            out.push_back(std::move(c));
        }
        return out;
    }

    // 2. Walk the driver graph from the dials: each reached channel's formulas are compiled, and
    // each `?value` they drive is reached in turn.
    void discover() {
        while (!m_queue.empty()) {
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
                const ChannelRef output = parseChannelRef(formula.value("output", std::string()));
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
                bool poseDriven = false;
                cf.ops = compile(*operations, ownerUrl, poseDriven);
                cf.mult = formula.value("stage", std::string("sum")) == "mult";
                if (poseDriven && cf.target == CompiledFormula::Target::Channel && !cf.mult) {
                    // A morph a JOINT drives is a pose corrective: correctivescan finds it and the
                    // engine blends it live. Baked here as well, it would be applied twice.
                    continue;
                }
                m_formulas.push_back(std::move(cf));
            }
        }
        // What the formulas merely READ: its own value where nothing dials or drives it.
        for (const CompiledFormula& f : m_formulas) {
            for (const CompiledOp& op : f.ops) {
                if (op.kind == CompiledOp::Kind::PushChannel) {
                    look(m_channels[static_cast<std::size_t>(op.channel)]);
                }
            }
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

    // Every reached channel with a meaningful value and a file: the caller loads each and applies
    // its deltas (a control has none). In the walk's order — the dials first, as the scene lists
    // them — so the bake adds the same deltas in the same order from one run to the next.
    void emit(ResolvedShape& out) const {
        for (std::size_t i = 0; i < m_channels.size(); ++i) {
            const Channel& c = m_channels[i];
            if (!c.reached || std::abs(m_values[i]) < 1e-4) {
                continue;
            }
            if (!c.modifier) {
                out.missing.push_back(c.id + ": " + c.why);
            }
            if (!c.url.empty()) {
                out.morphs.push_back({c.url, static_cast<float>(m_values[i]), c.id});
            }
        }
    }

    const nlohmann::json& m_presetRoot;
    UriResolver&          m_resolver;
    const std::string&    m_presetDir;
    const FigureScope&    m_scope;

    std::vector<Channel>                               m_channels;
    std::unordered_map<std::string, int>               m_index; ///< decoded id -> m_channels index
    std::deque<int>                                    m_queue; ///< reached, not yet walked
    std::vector<CompiledFormula>                       m_formulas;
    std::vector<std::shared_ptr<const FigureDocument>> m_documents; ///< keeps the walked files alive
    std::vector<double>                                m_values;
    bool                                               m_nameIndexBuilt = false;
    std::unordered_map<std::string, std::string>       m_nameIndex; ///< lowercase file stem -> root-relative uri (findByName)
};

} // namespace

ResolvedShape resolveDialedMorphs(const nlohmann::json& presetRoot, UriResolver& resolver,
                                  const std::string& presetDir, const FigureScope& scope) {
    return Walk(presetRoot, resolver, presetDir, scope).run();
}

} // namespace pose
