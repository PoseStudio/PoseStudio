/**
 * @file morphresolver.cpp
 * @brief Implementation of resolveDialedMorphs. See morphresolver.h.
 */

#include "morphresolver.h"

#include "figuredocument.h"
#include "uriresolver.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pose {

namespace {

// A parsed channel reference: its key (the fragment id after '#'), the file+fragment url used to
// load it ("/path.dsf#frag", or "#frag" for a same-file reference), and the driven property (after
// '?', e.g. "value", "scale/general", "rotation/y").
struct ChannelRef {
    std::string key;
    std::string url;
    std::string property;
};

ChannelRef parseChannelRef(const std::string& ref) {
    std::string rest = ref;
    if (const std::size_t colon = rest.find(':'); colon != std::string::npos) {
        rest = rest.substr(colon + 1); // strip a leading "NodeName:" scope
    }
    const std::size_t q = rest.find('?');
    const std::string url = (q == std::string::npos) ? rest : rest.substr(0, q);
    const std::string property = (q == std::string::npos) ? std::string() : rest.substr(q + 1);
    const std::size_t hash = url.find('#');
    const std::string key = (hash == std::string::npos) ? url : url.substr(hash + 1);
    return {key, url, property};
}

double valueOf(const std::unordered_map<std::string, double>& values, const std::string& key) {
    const auto it = values.find(key);
    return it != values.end() ? it->second : 0.0;
}

// Evaluates a formula's operation list — a small stack machine — against the current channel values.
double evalOperations(const nlohmann::json& operations,
                      const std::unordered_map<std::string, double>& values) {
    std::vector<double> stack;
    auto pop = [&stack]() -> double {
        if (stack.empty()) {
            return 0.0;
        }
        const double v = stack.back();
        stack.pop_back();
        return v;
    };
    for (const auto& op : operations) {
        const std::string o = op.value("op", std::string());
        if (o == "push") {
            if (const auto v = op.find("val"); v != op.end() && v->is_number()) {
                stack.push_back(v->get<double>());
            } else if (const auto u = op.find("url"); u != op.end() && u->is_string()) {
                stack.push_back(valueOf(values, parseChannelRef(u->get<std::string>()).key));
            } else {
                stack.push_back(0.0);
            }
        } else if (o == "mult") {
            const double b = pop();
            stack.push_back(pop() * b);
        } else if (o == "add") {
            stack.push_back(pop() + pop());
        } else if (o == "sub") {
            const double b = pop();
            stack.push_back(pop() - b);
        } else if (o == "div") {
            const double b = pop();
            stack.push_back(b != 0.0 ? pop() / b : (pop(), 0.0));
        }
        // Rarer ops (spline, etc.) are uncommon for identity morphs; approximating them as no-ops
        // (leave the stack) is close enough until correctives (Phase 4).
    }
    return stack.empty() ? 0.0 : stack.back();
}

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

const nlohmann::json* findModifier(const nlohmann::json& doc, const std::string& fragment) {
    const auto lib = doc.find("modifier_library");
    if (lib == doc.end() || !lib->is_array()) {
        return nullptr;
    }
    for (const auto& m : *lib) {
        if (m.value("id", std::string()) == fragment) {
            return &m;
        }
    }
    return nullptr;
}

} // namespace

std::vector<DialedMorph> resolveDialedMorphs(const nlohmann::json& presetRoot, UriResolver& resolver,
                                             const std::string& presetDir, float& outFigureScale,
                                             JointCenterOffsets& outJointCenterOffsets) {
    std::vector<DialedMorph> result;
    double scaleAccum = 0.0; // formula contributions to the figure's uniform scale/general
    outFigureScale = 1.0f;
    outJointCenterOffsets.clear();

    const auto scene = presetRoot.find("scene");
    if (scene == presetRoot.end() || !scene->contains("modifiers")) {
        return result;
    }

    // The figure's uniform scale only comes from formulas targeting the FIGURE node itself, so
    // identify it up front: the scene node carrying the geometry (the same criterion the importer
    // uses to locate the base mesh), keyed by its asset-url fragment — the id formula outputs
    // reference. Bones' scale channels share the same "scale/general" property name, so without
    // this the node check below has nothing to compare against.
    std::string figureNodeKey;
    if (const auto nodes = scene->find("nodes"); nodes != scene->end() && nodes->is_array()) {
        for (const auto& node : *nodes) {
            const auto geos = node.find("geometries");
            if (geos != node.end() && geos->is_array() && !geos->empty()) {
                figureNodeKey = parseChannelRef(node.value("url", std::string())).key;
                break;
            }
        }
    }

    std::unordered_map<std::string, double> values;      // channel key -> accumulated value
    std::unordered_map<std::string, std::string> urlOf;  // channel key -> loadable "/file.dsf#frag"
    std::deque<std::string> queue;
    std::unordered_set<std::string> queued;

    auto enqueue = [&](const std::string& key, const std::string& url) {
        if (!url.empty() && url[0] == '/') {
            urlOf[key] = url; // only file-backed channels can be loaded (for formulas + deltas)
        }
        if (!queued.count(key)) {
            queued.insert(key);
            queue.push_back(key);
        }
    };

    // Seed from the scene's dialed modifiers.
    for (const auto& mod : (*scene)["modifiers"]) {
        const auto ch = mod.find("channel");
        if (ch == mod.end()) {
            continue;
        }
        double cv = 0.0;
        if (const auto v = ch->find("current_value"); v != ch->end() && v->is_number()) {
            cv = v->get<double>();
        } else if (const auto v2 = ch->find("value"); v2 != ch->end() && v2->is_number()) {
            cv = v2->get<double>();
        }
        const std::string url = mod.value("url", std::string());
        if (url.empty()) {
            continue;
        }
        values[parseChannelRef(url).key] += cv;
        enqueue(parseChannelRef(url).key, url);
    }

    // Propagate control values through the driver formulas. The visited set bounds the walk (the graph is a
    // shallow DAG); values seed before a channel is processed, so parents drive children in order.
    std::unordered_set<std::string> processed;
    while (!queue.empty()) {
        const std::string key = queue.front();
        queue.pop_front();
        if (processed.count(key)) {
            continue;
        }
        processed.insert(key);

        const auto uit = urlOf.find(key);
        if (uit == urlOf.end()) {
            continue; // same-file-only reference, nothing to load
        }
        std::shared_ptr<const FigureDocument> doc;
        try {
            doc = resolver.loadDocument(uit->second, presetDir);
        } catch (...) {
            continue;
        }
        const nlohmann::json* mod = findModifier(doc->root(), parseChannelRef(uit->second).key);
        if (!mod) {
            continue;
        }
        const auto formulas = mod->find("formulas");
        if (formulas == mod->end() || !formulas->is_array()) {
            continue;
        }
        for (const auto& formula : *formulas) {
            const auto ops = formula.find("operations");
            if (ops == formula.end()) {
                continue;
            }
            const ChannelRef out = parseChannelRef(formula.value("output", std::string()));
            if (out.key.empty()) {
                continue;
            }
            const double r = evalOperations(*ops, values); // the formula's default stage is additive

            // Joint-center adjustment: a full-body/head morph also drives each bone's center_point (the
            // joint's rest origin) so the skeleton follows the character's new proportions. Route these
            // to the bone-offset map — a bone is not a morph channel to propagate — keyed by bone id.
            if (out.property.rfind("center_point/", 0) == 0) {
                if (const int a = axisIndexOf(out.property); a >= 0) {
                    outJointCenterOffsets[out.key][a] += static_cast<float>(r);
                }
                continue;
            }
            // end_point/orientation don't affect our translation-only bind (orientation is read straight
            // from the node), so skip them cleanly rather than letting them masquerade as channel values.
            if (out.property.rfind("end_point/", 0) == 0 ||
                out.property.rfind("orientation/", 0) == 0) {
                continue;
            }

            // Scale outputs are routed out of channel propagation (like center_point above): a node's
            // scale channel is not a morph channel. Only the FIGURE node's scale/general sets character
            // height (e.g. a teen figure dialed shorter). A bone-targeted output is a propagating-scale
            // rig instead — a head-scale control drives ~80 per-bone scale/general channels — and
            // summing those into the figure scale imported one teen character at 3x size. Per-bone
            // scale isn't modeled (the bind is translation-only), so those outputs are dropped here.
            if (out.property.find("scale/general") != std::string::npos ||
                out.property.find("general_scale") != std::string::npos) {
                if (out.key == figureNodeKey) {
                    scaleAccum += r;
                }
                continue;
            }

            values[out.key] += r;
            enqueue(out.key, out.url);
        }
    }
    outFigureScale = static_cast<float>(1.0 + scaleAccum);

    // Emit every reached, file-backed channel with a meaningful weight; the caller loads each and
    // applies its deltas (controls/scales resolve to no deltas and thus contribute nothing).
    for (const auto& [key, value] : values) {
        if (std::abs(value) < 1e-4) {
            continue;
        }
        if (const auto uit = urlOf.find(key); uit != urlOf.end()) {
            result.push_back({uit->second, static_cast<float>(value)});
        }
    }
    return result;
}

} // namespace pose
