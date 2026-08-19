/**
 * @file uvparser.cpp
 * @brief Implementation of parseUvSet. See uvparser.h.
 */

#include "uvparser.h"

#include <nlohmann/json.hpp>

namespace pose {

namespace {
// Several figure-format arrays are stored as {"count":N, "values":[...]}, but a few (e.g.
// polygon_vertex_indices) are a bare array. Return the element array either way.
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

UvSet parseUvSet(const nlohmann::json& uvDoc, const std::string& fragmentId) {
    UvSet out;
    const auto libIt = uvDoc.find("uv_set_library");
    if (libIt == uvDoc.end() || !libIt->is_array() || libIt->empty()) {
        return out;
    }

    // Prefer the entry whose id matches the URI fragment; fall back to the first.
    const nlohmann::json* set = nullptr;
    for (const auto& s : *libIt) {
        if (!fragmentId.empty() && s.value("id", std::string()) == fragmentId) {
            set = &s;
            break;
        }
    }
    if (!set) {
        set = &(*libIt)[0];
    }

    if (const auto uvsIt = set->find("uvs"); uvsIt != set->end()) {
        const nlohmann::json& values = valuesArray(*uvsIt);
        out.uvs.reserve(values.size());
        for (const auto& uv : values) {
            // Guard before indexing (const operator[] past the end is UB in release builds).
            // Substitute (0,0) rather than skip: uvs are addressed by index, so dropping an
            // entry would shift every later index and scramble the whole mapping.
            if (uv.is_array() && uv.size() >= 2) {
                out.uvs.emplace_back(uv[0].get<float>(), uv[1].get<float>());
            } else {
                out.uvs.emplace_back(0.0f, 0.0f);
            }
        }
    }

    // Seam overrides: each entry is [polygonIndex, baseVertexIndex, uvIndex].
    if (const auto pviIt = set->find("polygon_vertex_indices"); pviIt != set->end()) {
        const nlohmann::json& values = valuesArray(*pviIt);
        out.overrides.reserve(values.size());
        for (const auto& e : values) {
            if (!e.is_array() || e.size() < 3) {
                continue; // overrides are a sparse map — a malformed entry is safely droppable
            }
            const uint32_t poly = e[0].get<uint32_t>();
            const uint32_t vert = e[1].get<uint32_t>();
            const uint32_t uvIdx = e[2].get<uint32_t>();
            out.overrides[(static_cast<uint64_t>(poly) << 32) | vert] = uvIdx;
        }
    }

    return out;
}

} // namespace pose
