/**
 * @file geometryparser.h
 * @brief Parses a figure geometry definition and assembles it into renderable per-zone meshes.
 *
 * The figure format stores geometry as a shared vertex pool plus a polygon list of quads/tris, each
 * tagged with a material-zone index. parseGeometry() lifts that into a GeometryData; assembleFigure-
 * Meshes() then triangulates it, de-indexes unique (position, uv) pairs — splitting at UV seams —
 * generates smooth normals over the whole surface (so shading is seamless across zone boundaries),
 * and emits one FigureMesh per material zone. Pure std + GLM + nlohmann — no Qt.
 */

#ifndef GEOMETRYPARSER_H
#define GEOMETRYPARSER_H

#include "figuredata.h"

#include <glm/glm.hpp>

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pose {

struct UvSet;

/// Raw geometry lifted from a geometry_library entry: the shared vertex pool, the polygon list
/// (each face 3 or 4 vertices + its material-zone index), and the zone names.
struct GeometryData {
    /// One polygon: 3 (tri) or 4 (quad) vertex indices into `positions`, plus its material zone.
    struct Face {
        int                      material = 0;
        int                      count    = 0; ///< 3 or 4.
        std::array<uint32_t, 4>  v{};
    };

    std::vector<glm::vec3>   positions;
    std::vector<Face>        faces;
    std::vector<std::string> materialZones;   ///< polygon_material_groups (indexed by Face::material).
    std::string              defaultUvSetUri; ///< URI of the UV-set file, or empty.
};

/// Parses one geometry_library entry (its `vertices`, `polylist`, `polygon_material_groups`, and
/// `default_uv_set`). Throws std::runtime_error if the required arrays are missing/malformed.
GeometryData parseGeometry(const nlohmann::json& geometryEntry);

/// Triangulates @p geo, de-indexes it against @p uv (splitting at seams), generates smooth normals
/// over the whole surface, attaches per-vertex skin weights from @p skins (indexed by base vertex;
/// pass an empty vector for an unrigged mesh), and returns one FigureMesh per material zone (empty
/// zones included so the index lines up with the material list). If @p uv is empty, UVs are (0,0).
std::vector<FigureMesh> assembleFigureMeshes(const GeometryData& geo, const UvSet& uv,
                                             const std::vector<VertexSkin>& skins);

} // namespace pose

#endif // GEOMETRYPARSER_H
