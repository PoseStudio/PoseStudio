/**
 * @file figuredata.h
 * @brief The CPU-side result of importing a native figure — the rich analogue of ModelData.
 *
 * A figure is far more than static geometry: it is a set of per-material-zone meshes bound to a
 * skeleton by skin weights, shaped by morphs, and shaded by multi-map materials. This struct is the
 * format-neutral contract the figure importer (import/figure/) produces and the engine consumes;
 * it is grown one phase at a time. Phase 1 populates the per-zone meshes (geometry + UVs +
 * materials); later phases add the skeleton, per-vertex skin weights, and morph channels. Pure
 * std + GLM — no Qt, no Vulkan.
 */

#ifndef FIGUREDATA_H
#define FIGUREDATA_H

#include "modeldata.h" // PoseCorrective (shared with the engine IR)
#include "vertex.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pose {

/// A figure zone's resolved material: a base-color tint plus resolved on-disk paths to its texture
/// maps. Translated from the source physically-based material; only the maps the real-time renderer uses
/// are kept. Paths are absolute (already resolved against the content roots) or empty when absent.
struct FigureMaterial {
    glm::vec3   baseColor{0.75f};  ///< Diffuse tint multiplier (× the diffuse map, if any).
    std::string diffuseMapPath;    ///< Base-color / diffuse map, or empty.
    std::string normalMapPath;     ///< Tangent-space normal map (resolved), or empty.
    std::string bumpMapPath;       ///< Grayscale bump/height map (resolved), or empty.
    float       roughness = 0.7f;  ///< Scalar roughness fallback when no roughness map is present.
    float       opacity   = 1.0f;  ///< < 1 => transparent (eye moisture/cornea, later hair/lashes).
};

/// One material zone of a figure (e.g. "Torso", "Face", "Irises"): a triangulated, UV'd, de-indexed
/// submesh. Zones map 1:1 to the figure's polygon_material_groups, which lets each carry its own
/// material (skin vs eyes vs teeth) — the reason the mesh is split by zone rather than kept whole.
struct FigureMesh {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> baseVertex;         ///< Source base-vertex index per render vertex (for correctives).
    std::string           materialZone;      ///< polygon_material_groups name this mesh came from.
    int                   materialIndex = -1; ///< Index into the zone list (and the material list).
    FigureMaterial        material;
};

/// One skeleton joint. `parent` indexes into FigureData::bones (-1 for the figure root). `origin` is
/// the joint's absolute rest position (the node's center_point) and `orientation` its rest Euler
/// orientation in degrees — together they give the bind transform linear-blend skinning needs.
struct FigureBone {
    std::string name;
    int         parent = -1;
    glm::vec3   origin{0.0f};       ///< center_point — the joint's rest position (native units).
    glm::vec3   orientation{0.0f};  ///< Rest orientation, Euler degrees; the frame pose rotations act in.
    std::string rotationOrder = "XYZ"; ///< Order the pose Euler angles compose in (per the node).
    // Per-axis rotation limits (degrees), from the node's rotation channels' min/max/clamped. These are
    // the joint's anatomical range of motion (e.g. a knee bends [-11°,155°] on one axis). Only axes
    // flagged in `rotationLimited` are enforced (the format's per-channel `clamped` / "use limits" flag);
    // free axes (twist bones, etc.) leave their flag false and are not constrained.
    glm::vec3   rotationMin{0.0f};
    glm::vec3   rotationMax{0.0f};
    glm::bvec3  rotationLimited{false, false, false};
};

/// Per-vertex skin binding: up to 4 influencing joints (indices into FigureData::bones) with
/// normalized weights. Unused slots carry weight 0. One entry per *base* mesh vertex.
struct VertexSkin {
    glm::ivec4 joints{0};
    glm::vec4  weights{0.0f};
};

/// One imported figure: its per-zone meshes plus (Phase 3) the skeleton and per-base-vertex skin
/// weights. `vertexSkins` is index-aligned with the geometry's base vertices; both are empty for an
/// unrigged import. Morph channels and richer material data attach in later phases.
struct FigureData {
    std::vector<FigureMesh>     meshes;
    std::vector<FigureBone>     bones;
    std::vector<VertexSkin>     vertexSkins;
    std::vector<PoseCorrective> correctives;      ///< Pose-driven corrective morphs; base-vertex space.
    float                       figureScale = 1.0f; ///< Overall uniform scale (character height), from the driver formulas.
};

} // namespace pose

#endif // FIGUREDATA_H
