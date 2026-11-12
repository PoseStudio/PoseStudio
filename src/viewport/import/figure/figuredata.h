/**
 * @file figuredata.h
 * @brief The CPU-side result of importing a native figure — the rich analogue of ModelData.
 *
 * A figure is far more than static geometry: it is a set of per-material-zone meshes bound to a
 * skeleton by skin weights, shaped by morphs (baked at import), corrected by pose-driven morphs,
 * and shaded by multi-map materials. This struct is the format-neutral contract the figure
 * importer (import/figure/) produces and the engine consumes. Pure std + GLM — no Qt, no Vulkan.
 */

#ifndef FIGUREDATA_H
#define FIGUREDATA_H

#include "modeldata.h" // PoseCorrective (shared with the engine IR)
#include "vertex.h"

#include <glm/glm.hpp>

#include <cstddef>
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
    std::string opacityMapPath;    ///< Cutout/opacity mask (lash/brow cards, legacy eye shells), or empty.
    std::string roughnessMapPath;  ///< The active lobe's roughness map (× the scalar), or empty.
    std::string specMaskMapPath;   ///< The active lobe's weight/reflectivity mask map, or empty.
    std::string translucencyMapPath; ///< Translucency color/weight map (see materialparser.h), or empty.
    std::string translucencyWeightMapPath; ///< The Translucency Weight map itself (stored-only), or empty.
    std::string detailNormalMapPath; ///< Tiled micro-detail (pore) normal map, or empty.
    float       roughness = 0.7f;  ///< Scalar roughness (× the roughness map when present).
    float       normalStrength = 1.0f; ///< Dialed "Normal Map" strength (scales the map's texels).
    float       bumpStrength   = 1.0f; ///< Dialed "Bump Strength" (scales the height gradient).
    float       specularWeight = 1.0f; ///< F0 multiplier (1 = 4% dielectric; skin ~0.5 → ~2%).
    float       specularWeightWithMap = 1.0f; ///< Undiscounted fold — used when specMaskMapPath decodes.
    float       metallic  = 0.0f;  ///< Metallic Weight (0 dielectric … 1 metal).
    float       lobe1Roughness = 0.7f; ///< Dual-lobe specular (ratio 1 = single-lobe material).
    float       lobe2Roughness = 0.7f;
    float       lobeRatio      = 1.0f;
    float       topCoatWeight    = 0.0f; ///< Thin clear-coat layer (reflectivity folded in).
    bool        topCoatEnabled   = true;
    float       topCoatRoughness = 0.65f;
    float       translucencyWeight = 0.5f; ///< Per-region subsurface/transmission strength.
    float       detailWeight = 0.0f; ///< Micro-detail normal strength (0 = none).
    float       detailTiles  = 0.0f; ///< Micro-detail UV tiling (repeats across the mesh's UVs).
    float       detailTilesV = 0.0f; ///< … vertically (stored-only), its offsets, roughness multiplier and maps:
    glm::vec2   detailOffset{0.0f};
    float       detailRoughness = 1.0f;
    std::string detailWeightMapPath, detailRoughnessMapPath;
    std::string displacementMapPath; ///< Displacement (stored-only): the map, cm black-to-white, the zero level, the subdivision.
    float       displacementStrength = 0.2f;
    float       displacementMidLevel = 0.5f;
    float       displacementSubdiv = 0.0f;
    float       opacity   = 1.0f;  ///< < 1 => transparent (eye moisture/cornea, later hair/lashes).
    // THE CANONICAL MATERIAL's further channels (2026-11-09): see MaterialRefs (materialparser.h).
    glm::vec3   emissionColor{1.0f};
    float       emission = 0.0f;
    std::string emissionMapPath;
    float       ior = 1.5f;
    float       transmission = 0.0f;
    glm::vec3   transmissionColor{1.0f};
    float       transmissionDepth = 0.0f;
    float       transmissionRoughness = -1.0f; ///< < 0: the surface's own roughness
    bool        thinWalled = false;
    glm::vec3   subsurfaceColor{1.0f};
    glm::vec3   subsurfaceRadiusColor{1.0f}; ///< the scattering color (SSS Color), distance (cm) and direction
    float       subsurfaceRadius = 1.0f;
    float       subsurfaceAnisotropy = 0.0f;
    std::string transmissionColorMapPath; ///< the transmission tint's map (stored-only)
    std::string transmissionWeightMapPath;
    float       coatAnisotropy = 0.0f;
    float       coatAnisotropyRotation = 0.0f;
    std::string coatAnisotropyMapPath, coatAnisotropyRotationMapPath;
    float       sheen = 0.0f;
    glm::vec3   sheenColor{1.0f};
    float       sheenRoughness = 0.5f;
    std::string sheenWeightMapPath, sheenColorMapPath, sheenRoughnessMapPath;
    float       thinFilm = 0.0f;
    float       thinFilmThickness = 500.0f;
    float       thinFilmIor = 1.4f;
    std::string thinFilmThicknessMapPath;
    float       anisotropy = 0.0f;
    float       anisotropyRotation = 0.0f;
    float       diffuseRoughness = 0.0f;
    std::string diffuseRoughnessMapPath; ///< its grayscale map, or empty (stored-only)
    glm::vec3   coatColor{1.0f};
    float       coatIor = 1.5f;
    std::string coatWeightMapPath, coatColorMapPath, coatRoughnessMapPath, coatReflectivityMapPath, coatBumpMapPath; ///< stored-only
    float       coatBumpStrength = 1.0f;
    std::string specularColorMapPath, anisotropyMapPath, anisotropyRotationMapPath, lobe2RoughnessMapPath, lobeRatioMapPath; ///< stored-only
    glm::vec2   uvTiling{1.0f};
    glm::vec2   uvOffset{0.0f};
};

/// One material zone of a figure (e.g. "Torso", "Face", "Irises"): a triangulated, UV'd, de-indexed
/// submesh. Zones map 1:1 to the figure's polygon_material_groups, which lets each carry its own
/// material (skin vs eyes vs teeth) — the reason the mesh is split by zone rather than kept whole.
struct FigureMesh {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> baseVertex;         ///< Source base-vertex index per render vertex (for correctives).
    std::string           materialZone;      ///< polygon_material_groups name this mesh came from.
    FigureMaterial        material;
};

/// One skeleton joint. `parent` indexes into FigureData::bones (-1 for the figure root). `origin` is
/// the joint's absolute rest position (the node's center_point) and `orientation` its rest Euler
/// orientation in degrees — together they give the bind transform GPU (dual-quaternion) skinning
/// needs.
struct FigureBone {
    /// The node's `id` — the handle everything INSIDE a file refers to it by (parent links, skin
    /// joints, driver-formula urls), and the engine's name for the bone (pose files, picking).
    std::string name;
    /// The node's `name` field — the bone's identity ACROSS files. A file's ids only have to be
    /// unique within it, and one figure generation's base file renames thirty of them away from
    /// the bone's real name (the upper chest is id `chest_2`, the mid-forearm twist bone id
    /// `lWrist`, `lCarpal2` is another carpal's name entirely), while its followers use the
    /// names — so a follower's skeleton is matched to the figure's by THIS (mergeAddonFigure).
    /// Equal to `name` wherever a file keeps the two the same, which is nearly everywhere.
    std::string nodeName;
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

/// What became of the dials a preset sets on its figure — the character's shape. Reported, never
/// consumed: the import service logs it and tools/figurecheck prints it, so a character that
/// comes out as the base figure says why (which of its morph files is missing) instead of
/// leaving it to be guessed from the picture.
struct ShapeReport {
    struct Morph {
        std::string id;           ///< The morph's id ("Some Character Head").
        float       weight = 0.0f; ///< The weight it was applied at.
        std::size_t deltas = 0;    ///< How many vertices it moves.
    };
    std::vector<Morph>       applied;         ///< The shape morphs baked into the cage, in bake order.
    std::vector<std::string> missing;         ///< Dialed or driven morphs that could not be applied: "<id>: <why>".
    int                      jointsMoved = 0; ///< Bones whose rest origin the morphs moved.
};

/// One imported figure: its per-zone meshes plus the skeleton, per-base-vertex skin weights, and
/// pose correctives. `vertexSkins` is index-aligned with the geometry's base vertices; both it and
/// `bones` are empty for an unrigged import. Character-identity morphs don't appear here — they
/// are baked into the mesh vertices at import.
struct FigureData {
    std::vector<FigureMesh>     meshes;
    std::vector<FigureBone>     bones;
    std::vector<VertexSkin>     vertexSkins;
    std::vector<PoseCorrective> correctives;      ///< Pose-driven corrective morphs; base-vertex space.
    /// Size of the base-vertex space the meshes' `baseVertex` and the correctives' deltas index:
    /// the (subdivided) cage's vertex count — every cage vertex, referenced by a face or not. A
    /// merged follower's vertices are offset past it (see mergeAddonFigure), and it grows by the
    /// follower's own count so the next follower lands past both.
    std::size_t                 baseVertexCount = 0;
    float                       figureScale = 1.0f; ///< Overall uniform scale (character height), from the driver formulas.
    /// True when the geometry declares a POPULATED graft (non-empty hidden_polys/vertex_pairs) —
    /// a surface replacement authored in exact correspondence with a specific target shape. Used by
    /// the follower-addon path: populated-graft addons are vendor-fitted in place and must NOT be
    /// shape-follow projected (a generic follower's graft block is present but empty).
    bool                        graftPopulated = false;
    /// The scene node that carries the geometry — its `id` and its `name` — as the preset wrote
    /// them; empty for a bare base .dsf. A hierarchical-materials preset addresses a figure or a
    /// follower by that name ("<node>#materials/<zone>:?…"), which is what its tracks are matched
    /// against when the preset is applied across the followers (see applyAnimationMaterials).
    std::string                 figureNodeId;
    std::string                 figureNodeName;
    /// How many material zones read their UVs through a set OTHER than the geometry's default —
    /// the set their scene material names (see uvparser.h); 0 for a figure on one layout. Only
    /// reported (the import service logs it); a merged follower's count is not carried over.
    int                         zonesOnOwnUvSet = 0;
    /// The dialed shape as it was applied (see ShapeReport); a merged follower's is not carried over.
    ShapeReport                 shape;
};

} // namespace pose

#endif // FIGUREDATA_H
