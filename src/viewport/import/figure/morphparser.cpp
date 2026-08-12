/**
 * @file morphparser.cpp
 * @brief Implementation of parseMorphDeltas. See morphparser.h.
 */

#include "morphparser.h"

#include <nlohmann/json.hpp>

namespace pose {

namespace {
// A morph's deltas are stored either as {"count":N,"values":[...]} or a bare array.
const nlohmann::json& valuesArray(const nlohmann::json& node) {
    if (node.is_object()) {
        const auto it = node.find("values");
        if (it != node.end()) {
            return *it;
        }
    }
    return node;
}
} // namespace

std::vector<MorphDelta> parseMorphDeltas(const nlohmann::json& morphDoc,
                                         const std::string& fragment) {
    std::vector<MorphDelta> out;

    const auto lib = morphDoc.find("modifier_library");
    if (lib == morphDoc.end() || !lib->is_array()) {
        return out;
    }

    // Prefer the modifier whose id matches the URI fragment; else the first one carrying a morph.
    const nlohmann::json* mod = nullptr;
    if (!fragment.empty()) {
        for (const auto& m : *lib) {
            if (m.value("id", std::string()) == fragment && m.contains("morph")) {
                mod = &m;
                break;
            }
        }
    }
    if (!mod) {
        for (const auto& m : *lib) {
            if (m.contains("morph")) {
                mod = &m;
                break;
            }
        }
    }
    if (!mod) {
        return out; // no shape here (e.g. a formula-only driver modifier)
    }

    const nlohmann::json& morph = (*mod)["morph"];
    const auto deltas = morph.find("deltas");
    if (deltas == morph.end()) {
        return out;
    }

    const nlohmann::json& values = valuesArray(*deltas);
    out.reserve(values.size());
    for (const auto& entry : values) {
        if (entry.is_array() && entry.size() >= 4) {
            out.emplace_back(entry[0].get<uint32_t>(),
                             glm::vec3(entry[1].get<float>(), entry[2].get<float>(),
                                       entry[3].get<float>()));
        }
    }
    return out;
}

} // namespace pose
