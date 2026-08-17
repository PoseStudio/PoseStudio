/**
 * @file materialparser.h
 * @brief Parses a figure preset's per-zone physically-based materials into renderer-facing references.
 *
 * A figure preset assigns one material per polygon_material_group ("Torso", "Face", "Irises", …).
 * Each material carries a base-color tint plus texture references (image_file URIs) for diffuse,
 * normal/bump, gloss, etc. This lifts the subset the real-time renderer uses into MaterialRefs,
 * keyed by zone name, leaving URI-to-path resolution to the caller (which holds the content roots).
 * Pure std + GLM + nlohmann — no Qt.
 */

#ifndef MATERIALPARSER_H
#define MATERIALPARSER_H

#include <glm/glm.hpp>

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <unordered_map>

namespace pose {

/// Unresolved material references for one zone: the base-color tint plus raw image_file URIs (still
/// root-relative, e.g. "/Runtime/Textures/…"). The importer resolves the URIs against content roots.
struct MaterialRefs {
    glm::vec3   baseColor{0.75f};
    std::string diffuseImageUri;
    std::string normalImageUri;  ///< "Normal Map" channel (tangent-space RGB), or empty.
    std::string bumpImageUri;    ///< "Bump Strength" channel (grayscale height), or empty.
    std::string opacityImageUri; ///< Cutout/opacity mask (lash & brow cards, legacy eye shells), or empty.
    std::string roughnessImageUri;  ///< The active lobe's roughness MAP (multiplies `roughness`), or empty.
    std::string specWeightImageUri; ///< The active lobe's weight/reflectivity MASK map, or empty.
    float       roughness = 0.7f;
    float       specularWeight = 1.0f; ///< F0 multiplier (1 = the standard 4% dielectric — also the
                                       ///< cap): the active specular lobe's weight × reflectivity/0.5,
                                       ///< discounted when map-carried (see foldSpecWeight). Skin
                                       ///< lands ~0.3-0.8 → 1-3% F0 — real skin, not wet plastic.
    float       specularWeightWithMap = 1.0f; ///< The UNdiscounted fold, for when specWeightImageUri
                                              ///< actually decodes: the sampled mask texels then do the
                                              ///< scaling the discount only guessed at (shader clamps
                                              ///< scalar × texel to the same ≤1 dielectric ceiling).

    // Dual-lobe specular, kept as TWO lobes for the renderer (roughness above stays the ratio-blend
    // for the single-lobe shade modes). lobeRatio = lobe 1's fraction; 1 = single-lobe material
    // (the shader then uses the blended roughness exactly as before).
    float       lobe1Roughness = 0.7f;
    float       lobe2Roughness = 0.7f;
    float       lobeRatio      = 1.0f;

    // Top Coat: the thin oily/clear layer skin authors at weight ~0.35-0.45 (0 = none). Weight has
    // the coat's reflectivity folded in (refl/0.5, like the base lobes).
    float       topCoatWeight    = 0.0f;
    float       topCoatRoughness = 0.65f;

    // Translucency: per-region subsurface strength + the transmitted tint. The weight scales the
    // skin wrap and backlit transmission; the map (Translucency Color's flesh-red, else the
    // grayscale weight map) localizes it. Default 0.5 = the old uniform dial behaviour for
    // materials that don't author translucency channels.
    float       translucencyWeight = 0.5f;
    std::string translucencyImageUri;

    // Micro-detail normal: the newer skin shader's tiled pore-grain map ("Detail Normal Map",
    // repeated detailTiles× across the UVs at detailWeight strength). Empty/0 = none.
    std::string detailNormalImageUri;
    float       detailWeight = 0.0f;
    float       detailTiles  = 0.0f;
    float       metallic  = 0.0f; ///< "Metallic Weight" (0 dielectric … 1 metal; jewelry/hardware).
    float       opacity   = 1.0f; ///< From Cutout Opacity (or the legacy "transparency" channel),
                                  ///< lowered for refractive (glass) surfaces.
};

/// Parses a preset's `scene.materials` into a zone-name -> MaterialRefs map. Each scene material only
/// *overrides* channels on a base material it references by `url` (`#id`); the full channel set
/// (roughness, transparency, …) lives in @p materialLibrary. This merges the two (scene wins), which
/// is essential — e.g. the eye's clear shells get their transparency-defining channels only from the
/// base. A material listing several groups is registered under each. Pass an empty array for
/// @p materialLibrary when there is none (a base `.dsf` imported directly carries no materials).
std::unordered_map<std::string, MaterialRefs> parseMaterials(const nlohmann::json& sceneMaterials,
                                                             const nlohmann::json& materialLibrary,
                                                             const nlohmann::json& imageLibrary);

} // namespace pose

#endif // MATERIALPARSER_H
