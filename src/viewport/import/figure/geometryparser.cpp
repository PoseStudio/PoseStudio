// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file geometryparser.cpp
 * @brief Implementation of parseGeometry. See geometryparser.h.
 */

#include "geometryparser.h"

#include "figureutils.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <stdexcept>

namespace pose {

namespace {

// True when a graft's @p key block names anything. Tolerates both array forms — a bare array or
// a {"count":N,"values":[...]} block — since value("count", ...) on a bare array would throw.
bool graftBlockPopulated(const nlohmann::json& graft, const char* key) {
    const auto it = graft.find(key);
    if (it == graft.end()) {
        return false;
    }
    if (it->is_array()) {
        return !it->empty();
    }
    return intOf(*it, "count", 0) > 0;
}

} // namespace

GeometryData parseGeometry(const nlohmann::json& g) {
    GeometryData out;

    const auto vertsIt = g.find("vertices");
    const auto polyIt = g.find("polylist");
    if (vertsIt == g.end() || polyIt == g.end()) {
        throw std::runtime_error("figure geometry: missing 'vertices' or 'polylist'");
    }

    const nlohmann::json& verts = valuesArray(*vertsIt);
    out.positions.reserve(verts.size());
    for (const auto& v : verts) {
        // Validate before indexing: nlohmann's const operator[] past the end (or on a non-array)
        // is undefined behavior in release builds — a corrupt file must fail cleanly, not crash.
        if (!v.is_array() || v.size() < 3) {
            throw std::runtime_error("figure geometry: malformed vertex entry");
        }
        out.positions.emplace_back(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
    }

    if (const auto zonesIt = g.find("polygon_material_groups"); zonesIt != g.end()) {
        for (const auto& s : valuesArray(*zonesIt)) {
            out.materialZones.push_back(s.get<std::string>());
        }
    }

    const nlohmann::json& polys = valuesArray(*polyIt);
    const auto vertCount = static_cast<uint32_t>(out.positions.size());
    out.faces.reserve(polys.size());
    uint32_t sourceIndex = 0; // position in the source polylist, skipped polygons included
    for (const auto& p : polys) {
        const uint32_t polyIndex = sourceIndex++;
        // p = [groupIndex, materialIndex, v0, v1, v2, (v3)] — 5 entries for a tri, 6 for a quad.
        const int n = p.is_array() ? static_cast<int>(p.size()) - 2 : 0;
        if (n < 3 || n > 4) {
            continue; // skip degenerate/unsupported polygons
        }
        GeometryData::Face f{};
        f.material = p[1].get<int>();
        f.count = n;
        f.sourceIndex = polyIndex;
        bool valid = true;
        for (int i = 0; i < n; ++i) {
            f.v[i] = p[2 + i].get<uint32_t>();
            // Reject out-of-range vertex indices HERE, not downstream: the subdivision cage
            // builder indexes per-vertex adjacency vectors with these values, so a hostile or
            // corrupt file would otherwise write out of bounds (the assembler merely clamps).
            if (f.v[i] >= vertCount) valid = false;
        }
        if (!valid) continue;
        out.faces.push_back(f);
    }

    if (const auto uvIt = g.find("default_uv_set"); uvIt != g.end()) {
        // Usually a string; tolerate an array form by taking its first element.
        if (uvIt->is_string()) {
            out.defaultUvSetUri = uvIt->get<std::string>();
        } else if (uvIt->is_array() && !uvIt->empty() && uvIt->front().is_string()) {
            out.defaultUvSetUri = uvIt->front().get<std::string>();
        }
    }

    // A POPULATED graft declaration (it names parent polygons to hide / vertices to weld) marks a
    // surface replacement authored in exact correspondence with a specific target shape — the signal
    // the follower-addon path uses to skip shape-follow projection. Generic followers carry an
    // EMPTY graft object, which does not count.
    if (const auto graft = g.find("graft"); graft != g.end()) {
        out.graftPopulated =
            graftBlockPopulated(*graft, "hidden_polys") || graftBlockPopulated(*graft, "vertex_pairs");
    }

    return out;
}

} // namespace pose
