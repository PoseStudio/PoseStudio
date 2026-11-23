// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
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

/// One material zone's material, translated from the source's physically-based shader into the
/// canonical material's terms (scene/materialparams.h): the values the viewport draws and the ones
/// that are stored for the Materials tab and the render engine, and one map per slot.
///
/// A MAP MEMBER holds a path in two stages. materialparser (parseMaterials) fills it with the URI
/// the file wrote — root-relative and percent-encoded, "/Runtime/Textures/…" — and scenematerials
/// resolves it, in place, to the absolute on-disk path a FigureData carries (empty: no map, or one
/// that is not under any content root). ONE struct for both stages on purpose: as two structs
/// with the same sixty-odd members and a hand-written copy between them, a channel added to the
/// one and not the other would be dropped in silence.
///
/// Each member names the shader channel it is read from; where and how is materialparser.cpp's.
struct FigureMaterial {
    glm::vec3   baseColor{0.75f};  ///< Diffuse tint multiplier (× the diffuse map, if any).
    std::string diffuseMapPath;    ///< Base-color / diffuse map, or empty.
    std::string normalMapPath;     ///< "Normal Map" channel (tangent-space RGB), or empty.
    std::string bumpMapPath;       ///< "Bump Strength" channel (grayscale height), or empty.
    std::string opacityMapPath;    ///< Cutout/opacity mask (lash & brow cards, legacy eye shells), or empty.
    std::string roughnessMapPath;  ///< The active lobe's roughness MAP (multiplies `roughness`), or empty.
    std::string specMaskMapPath;   ///< The active lobe's weight/reflectivity MASK map, or empty.
    /// The Translucency COLOR map (the weight map where the color has none: the viewport's one slot).
    std::string translucencyMapPath;
    std::string translucencyWeightMapPath; ///< The Translucency Weight map itself (stored-only), or empty.
    std::string detailNormalMapPath; ///< "Detail Normal Map": the tiled micro-detail (pore) normal map, or empty.
    float       roughness = 0.7f;  ///< Scalar roughness (× the roughness map when present).
    float       normalStrength = 1.0f; ///< The "Normal Map" channel's dialed strength (scales its texels).
    float       bumpStrength   = 1.0f; ///< The "Bump Strength" channel's dialed strength (scales the height gradient).
    float       specularWeight = 1.0f; ///< F0 multiplier (1 = the standard 4% dielectric — also the
                                       ///< cap): the active specular lobe's weight × reflectivity/0.5,
                                       ///< discounted when map-carried (see foldSpecWeight). Skin
                                       ///< lands ~0.3-0.8 → 1-3% F0 — real skin, not wet plastic.
    float       specularWeightWithMap = 1.0f; ///< The UNdiscounted fold, for when specMaskMapPath
                                              ///< actually decodes: the sampled mask texels then do the
                                              ///< scaling the discount only guessed at (shader clamps
                                              ///< scalar × texel to the same ≤1 dielectric ceiling).
    float       metallic  = 0.0f;  ///< "Metallic Weight" (0 dielectric … 1 metal; jewelry/hardware).

    // Dual-lobe specular, kept as TWO lobes for the renderer (`roughness` stays the ratio-blend for
    // the single-lobe shade modes). lobeRatio = lobe 1's fraction; 1 = single-lobe material (the
    // shader then uses the blended roughness exactly as before).
    float       lobe1Roughness = 0.7f;
    float       lobe2Roughness = 0.7f;
    float       lobeRatio      = 1.0f;

    // Top Coat: the thin oily/clear layer skin authors at weight ~0.35-0.45 (0 = none). The weight
    // has the coat's reflectivity folded in (refl/0.5, like the base lobes).
    float       topCoatWeight    = 0.0f;
    bool        topCoatEnabled   = true;  ///< "Top Coat Enable" (the weight is read either way).
    float       topCoatRoughness = 0.65f;

    // Translucency: per-region subsurface strength + the transmitted tint. The weight scales the
    // skin wrap and backlit transmission; the map (translucencyMapPath) localizes it. Default 0.5 =
    // the old uniform dial behaviour for materials that don't author translucency channels.
    float       translucencyWeight = 0.5f;

    // Micro-detail normal: the newer skin shader's tiled pore-grain map (detailNormalMapPath,
    // repeated detailTiles× across the UVs at detailWeight strength). 0 = none; the values are
    // read only where the layer has its map.
    float       detailWeight = 0.0f;
    float       detailTiles  = 0.0f;
    float       detailTilesV = 0.0f;         ///< "Detail Vertical Tiles" (the horizontal count where absent).
    glm::vec2   detailOffset{0.0f};          ///< "Detail Horizontal / Vertical Offset".
    float       detailRoughness = 1.0f;      ///< "Detail Specular Roughness Mult" …
    std::string detailWeightMapPath;         ///< … the weight's map and
    std::string detailRoughnessMapPath;      ///< … the multiplier's map (stored-only slots).
    std::string displacementMapPath;         ///< "Displacement Strength"'s map; the values converted to
    float       displacementStrength = 0.2f; ///< centimetres black-to-white ((max - min) x strength),
    float       displacementMidLevel = 0.5f; ///< the zero level (-min / (max - min)) and
    float       displacementSubdiv = 0.0f;   ///< "SubD Displacement Level".
    float       opacity   = 1.0f;  ///< From Cutout Opacity (or the legacy "transparency" channel),
                                   ///< lowered for refractive (glass) surfaces: < 1 => transparent.

    // THE CANONICAL MATERIAL's further channels (2026-11-09; materialparams.h), read where the
    // shader authors them and neutral where it does not.
    glm::vec3   emissionColor{1.0f};     ///< "Emission Color" (black = none) …
    float       emission = 0.0f;         ///< … × "Luminance" / 1000 (cd/m² → the engine's radiance factor).
    std::string emissionMapPath;         ///< The emission color's map, or empty.
    float       ior = 1.5f;              ///< "Refraction Index" (of a refracting surface: see readTransparency).
    float       transmission = 0.0f;     ///< "Refraction Weight" (the clear shells' signal, kept for the renderer).
    glm::vec3   transmissionColor{1.0f}; ///< "Transmitted Color".
    float       transmissionDepth = 0.0f; ///< "Transmitted Measurement Distance" (cm → m).
    /// The refraction's own roughness where the shader does not share its glossy inputs with it
    /// ("Share Glossy Inputs" off: "Refraction Roughness"); negative = the surface's roughness.
    float       transmissionRoughness = -1.0f;
    bool        thinWalled = false;      ///< "Thin Walled".
    glm::vec3   subsurfaceColor{1.0f};   ///< "Translucency Color" (the tint skins author under the surface).
    glm::vec3   subsurfaceRadiusColor{1.0f}; ///< "SSS Color": the volume's scattering color.
    float       subsurfaceRadius = 1.0f;     ///< "Scattering Measurement Distance" (cm).
    float       subsurfaceAnisotropy = 0.0f; ///< "SSS Direction" (-1..1).
    std::string transmissionColorMapPath;    ///< "Transmitted Color"'s map (stored-only slot).
    std::string transmissionWeightMapPath;   ///< "Refraction Weight"'s map.
    float       coatAnisotropy = 0.0f;       ///< "Top Coat Anisotropy" (+ map) and "Top Coat Rotations" x 360 (+ map).
    float       coatAnisotropyRotation = 0.0f;
    std::string coatAnisotropyMapPath, coatAnisotropyRotationMapPath;
    float       sheen = 0.0f;                ///< "Backscattering Weight" / Color / Roughness — the shader's sheen (+ maps).
    glm::vec3   sheenColor{1.0f};
    float       sheenRoughness = 0.5f;
    std::string sheenWeightMapPath, sheenColorMapPath, sheenRoughnessMapPath;
    float       thinFilm = 0.0f;             ///< 1 where "Thin Film Thickness" > 0 …
    float       thinFilmThickness = 500.0f;  ///< … the thickness (nm, + map) and
    float       thinFilmIor = 1.4f;          ///< "Thin Film IOR".
    std::string thinFilmThicknessMapPath;
    float       anisotropy = 0.0f;       ///< "Glossy Anisotropy".
    float       anisotropyRotation = 0.0f; ///< "Glossy Anisotropy Rotations" (turns → degrees).
    float       diffuseRoughness = 0.0f; ///< "Diffuse Roughness".
    std::string diffuseRoughnessMapPath; ///< … and its map (a grayscale strength map), or empty (stored-only).
    glm::vec3   coatColor{1.0f};         ///< "Top Coat Color", its HUE alone (its luminance is folded into the coat's weight).
    float       coatIor = 1.5f;          ///< "Top Coat IOR" in the Fresnel layering mode; 1.5 in the others.
    /// The top coat's maps (stored-only): "Top Coat Weight", Color, Roughness (or Glossiness),
    /// Reflectivity (or IOR), and "Top Coat Bump" — the coat's own height / normal map …
    std::string coatWeightMapPath, coatColorMapPath, coatRoughnessMapPath, coatReflectivityMapPath, coatBumpMapPath;
    float       coatBumpStrength = 1.0f; ///< … with its strength.
    /// The Specular section's maps (stored-only): "Glossy Color", "Glossy Anisotropy" and its
    /// Rotations, "Specular Lobe 2 Roughness Mult" (or "… Roughness") and "Dual Lobe Specular Ratio".
    std::string specularColorMapPath, anisotropyMapPath, anisotropyRotationMapPath, lobe2RoughnessMapPath, lobeRatioMapPath;
    glm::vec2   uvTiling{1.0f};          ///< "Horizontal Tiles" / "Vertical Tiles".
    glm::vec2   uvOffset{0.0f};          ///< "Horizontal Offset" / "Vertical Offset".
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
/// normalized weights. Unused slots carry weight 0. One entry per *base* mesh vertex — the form
/// the skin parser returns and subdivision refines; a FigureData carries the result per render
/// vertex instead.
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

/// One imported figure: its per-zone meshes plus the skeleton and the pose correctives. The skin
/// weights ride each render vertex (Vertex::joints / weights, indices into `bones`); `bones` is
/// empty for an unrigged import. Character-identity morphs don't appear here — they are baked into
/// the mesh vertices at import.
struct FigureData {
    std::vector<FigureMesh>     meshes;
    std::vector<FigureBone>     bones;
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
    /// OPTIONAL parts of the files that were malformed and left out, the figure imported without
    /// them — "<what>: <why>" (a material whose channels could not be read: its zones keep their
    /// defaults). Reported, never consumed, like the shape: the import service logs each line and
    /// tools/figurecheck prints it. A merged follower's lines are carried over.
    std::vector<std::string>    skipped;
};

} // namespace pose

#endif // FIGUREDATA_H
