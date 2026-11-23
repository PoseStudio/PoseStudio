// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file geometryparser.h
 * @brief Parses a figure geometry definition (the base cage) into the importer's GeometryData.
 *
 * The figure format stores geometry as a shared vertex pool plus a polygon list of quads/tris, each
 * tagged with a material-zone index. parseGeometry() lifts that into a GeometryData — the cage the
 * rest of the pipeline morphs, skins, subdivides and assembles (subdivision.h does the
 * triangulation / seam split / smooth normals for every level, including level 0). Each face
 * remembers its index in the SOURCE polylist: the UV set's seam overrides are keyed by that index,
 * and a polygon skipped as degenerate must not shift every later face's key. Pure std + GLM +
 * nlohmann — no Qt.
 */

#ifndef GEOMETRYPARSER_H
#define GEOMETRYPARSER_H

#include <glm/glm.hpp>

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pose {

/// Raw geometry lifted from a geometry_library entry: the shared vertex pool, the polygon list
/// (each face 3 or 4 vertices + its material-zone index), and the zone names.
struct GeometryData {
    /// One polygon: 3 (tri) or 4 (quad) vertex indices into `positions`, plus its material zone.
    struct Face {
        int                      material = 0;
        int                      count    = 0; ///< 3 or 4.
        std::array<uint32_t, 4>  v{};
        /// The polygon's index in the source polylist — the key the UV set's seam overrides
        /// (UvSet::uvIndexFor) use. Differs from the face's position in `faces` once a preceding
        /// polygon was skipped as degenerate/out-of-range.
        uint32_t                 sourceIndex = 0;
    };

    std::vector<glm::vec3>   positions;
    std::vector<Face>        faces;
    std::vector<std::string> materialZones;   ///< polygon_material_groups (indexed by Face::material).
    std::string              defaultUvSetUri; ///< URI of the UV-set file, or empty.
    /// True when the geometry declares a POPULATED graft (non-empty hidden_polys/vertex_pairs) —
    /// a surface replacement authored in exact correspondence with a specific target shape. The
    /// follower-addon path reads it: such addons are vendor-fitted in place and must NOT be
    /// shape-follow projected. A generic follower carries an EMPTY graft object, which doesn't count.
    bool                     graftPopulated = false;
};

/// Parses one geometry_library entry (its `vertices`, `polylist`, `polygon_material_groups`,
/// `default_uv_set`, and `graft`). Degenerate or out-of-range polygons are skipped (their source
/// index is preserved on the faces that follow). Throws std::runtime_error if the required arrays
/// are missing/malformed.
GeometryData parseGeometry(const nlohmann::json& geometryEntry);

} // namespace pose

#endif // GEOMETRYPARSER_H
