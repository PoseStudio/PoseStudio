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
#include <unordered_set>

namespace pose {

/// Unresolved material references for one zone: the base-color tint plus raw image_file URIs (still
/// root-relative, e.g. "/Runtime/Textures/…"). The importer resolves the URIs against content roots.
struct MaterialRefs {
    glm::vec3   baseColor{0.75f};
    std::string diffuseImageUri;
    std::string normalImageUri;  ///< "Normal Map" channel (tangent-space RGB), or empty.
    std::string bumpImageUri;    ///< "Bump Strength" channel (grayscale height), or empty.
    float       normalStrength = 1.0f; ///< The "Normal Map" channel's dialed strength (scales its texels).
    float       bumpStrength   = 1.0f; ///< The "Bump Strength" channel's dialed strength.
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
    bool        topCoatEnabled   = true;  ///< "Top Coat Enable" (the weight is read either way).
    float       topCoatRoughness = 0.65f;

    // Translucency: per-region subsurface strength + the transmitted tint. The weight scales the
    // skin wrap and backlit transmission; the map (Translucency Color's flesh-red, else the
    // grayscale weight map) localizes it. Default 0.5 = the old uniform dial behaviour for
    // materials that don't author translucency channels.
    float       translucencyWeight = 0.5f;
    std::string translucencyImageUri;       ///< The Translucency COLOR map (the weight map where the color has none: the viewport's one slot).
    std::string translucencyWeightImageUri; ///< The Translucency Weight map itself (stored-only).

    // Micro-detail normal: the newer skin shader's tiled pore-grain map ("Detail Normal Map",
    // repeated detailTiles× across the UVs at detailWeight strength). Empty/0 = none.
    std::string detailNormalImageUri;
    float       detailWeight = 0.0f;
    float       detailTiles  = 0.0f;
    float       detailTilesV = 0.0f;         ///< "Detail Vertical Tiles" (the horizontal count where absent).
    glm::vec2   detailOffset{0.0f};          ///< "Detail Horizontal / Vertical Offset".
    float       detailRoughness = 1.0f;      ///< "Detail Specular Roughness Mult" …
    std::string detailWeightImageUri;        ///< … the weight's map and
    std::string detailRoughnessImageUri;     ///< … the multiplier's map (stored-only slots).
    std::string displacementImageUri;        ///< "Displacement Strength"'s map; the values converted to
    float       displacementStrength = 0.2f; ///< centimetres black-to-white ((max - min) x strength),
    float       displacementMidLevel = 0.5f; ///< the zero level (-min / (max - min)) and
    float       displacementSubdiv = 0.0f;   ///< "SubD Displacement Level".
    float       metallic  = 0.0f; ///< "Metallic Weight" (0 dielectric … 1 metal; jewelry/hardware).
    float       opacity   = 1.0f; ///< From Cutout Opacity (or the legacy "transparency" channel),
                                  ///< lowered for refractive (glass) surfaces.

    // THE CANONICAL MATERIAL's further channels (2026-11-09; materialparams.h), read where the
    // shader authors them and neutral where it does not — for the Materials tab and the rendering
    // engines to come.
    glm::vec3   emissionColor{1.0f};     ///< "Emission Color" (black = none) …
    float       emission = 0.0f;         ///< … × "Luminance" / 1000 (cd/m² → the engine's radiance factor).
    std::string emissionImageUri;        ///< The emission color's map, or empty.
    float       ior = 1.5f;              ///< "Refraction Index".
    float       transmission = 0.0f;     ///< "Refraction Weight" (the clear shells' signal, kept for the renderer).
    glm::vec3   transmissionColor{1.0f}; ///< "Transmitted Color".
    float       transmissionDepth = 0.0f; ///< "Transmitted Measurement Distance" (cm → m).
    bool        thinWalled = false;      ///< "Thin Walled".
    glm::vec3   subsurfaceColor{1.0f};   ///< "Translucency Color" (the tint skins author under the surface).
    glm::vec3   subsurfaceRadiusColor{1.0f}; ///< "SSS Color": the volume's scattering color.
    float       subsurfaceRadius = 1.0f;     ///< "Scattering Measurement Distance" (cm).
    float       subsurfaceAnisotropy = 0.0f; ///< "SSS Direction" (-1..1).
    std::string transmissionColorImageUri;   ///< "Transmitted Color"'s map (stored-only slot).
    std::string transmissionWeightImageUri;  ///< "Refraction Weight"'s map.
    float       coatAnisotropy = 0.0f;       ///< "Top Coat Anisotropy" (+ map) and "Top Coat Rotations" x 360 (+ map).
    float       coatAnisotropyRotation = 0.0f;
    std::string coatAnisotropyImageUri;
    std::string coatAnisotropyRotationImageUri;
    float       sheen = 0.0f;                ///< "Backscattering Weight" / Color / Roughness — the shader's sheen (+ maps).
    glm::vec3   sheenColor{1.0f};
    float       sheenRoughness = 0.5f;
    std::string sheenWeightImageUri;
    std::string sheenColorImageUri;
    std::string sheenRoughnessImageUri;
    float       thinFilm = 0.0f;             ///< 1 where "Thin Film Thickness" > 0 …
    float       thinFilmThickness = 500.0f;  ///< … the thickness (nm, + map) and
    float       thinFilmIor = 1.4f;          ///< "Thin Film IOR".
    std::string thinFilmThicknessImageUri;
    float       anisotropy = 0.0f;       ///< "Glossy Anisotropy".
    float       anisotropyRotation = 0.0f; ///< "Glossy Anisotropy Rotations" (turns → degrees).
    float       diffuseRoughness = 0.0f; ///< "Diffuse Roughness".
    std::string diffuseRoughnessImageUri; ///< … and its map (a grayscale strength map), or empty.
    glm::vec3   coatColor{1.0f};         ///< "Top Coat Color" (its luminance is also folded into the coat's weight above).
    float       coatIor = 1.6f;          ///< "Top Coat IOR".
    std::string coatWeightImageUri;      ///< The top coat's maps: "Top Coat Weight", …
    std::string coatColorImageUri;
    std::string coatRoughnessImageUri;
    std::string coatReflectivityImageUri;
    std::string coatBumpImageUri;        ///< "Top Coat Bump" (the coat's own height / normal map) …
    float       coatBumpStrength = 1.0f; ///< … and its strength.
    std::string specularColorImageUri;     ///< The Specular section's maps: "Glossy Color", …
    std::string anisotropyImageUri;
    std::string anisotropyRotationImageUri;
    std::string lobe2RoughnessImageUri;    ///< … "Specular Lobe 2 Roughness Mult" (or "… Roughness") …
    std::string lobeRatioImageUri;         ///< … and "Dual Lobe Specular Ratio".
    glm::vec2   uvTiling{1.0f};          ///< "Horizontal Tiles" / "Vertical Tiles".
    glm::vec2   uvOffset{0.0f};          ///< "Horizontal Offset" / "Vertical Offset".
};

/// Cross-file material bases the importer pre-resolved for parseMaterials, which itself only knows
/// same-file "#id" references: `extraBases` are base materials lifted from OTHER documents, keyed by
/// their fragment id and consulted after the same-file library; `urlRewrites` maps a scene material
/// (by address within `sceneMaterials`) to the same-file "#fragment" url it should be read with.
/// Both are views — the pointed-at JSON must outlive the parse. Replaces an older scheme that
/// deep-copied the scene materials and the whole material library to append/rewrite in place.
/// `skip` lists scene materials (by address) the parse must pass over: those a scene of several
/// figures lists for ANOTHER figure's geometry (see FigureScope). `baseIdsAsWritten` compares a
/// same-file base reference with the library ids as the reference spells it, percent-encoding and
/// all — so a base whose id has a space in it is NOT found; see applySceneMaterials for the one
/// caller that asks for that, and why.
struct MaterialBaseOverrides {
    std::unordered_map<std::string, const nlohmann::json*> extraBases;
    std::unordered_map<const nlohmann::json*, std::string> urlRewrites;
    std::unordered_set<const nlohmann::json*>              skip;
    bool                                                   baseIdsAsWritten = false;
};

/// Parses a preset's `scene.materials` into a zone-name -> MaterialRefs map. Each scene material only
/// *overrides* channels on a base material it references by `url` (`#id`); the full channel set
/// (roughness, transparency, …) lives in @p materialLibrary. This merges the two (scene wins), which
/// is essential — e.g. the eye's clear shells get their transparency-defining channels only from the
/// base. A material listing several groups is registered under each. Pass an empty array for
/// @p materialLibrary when there is none (a base `.dsf` imported directly carries no materials).
/// @p overrides (optional) supplies cross-file bases and url rewrites — see MaterialBaseOverrides.
std::unordered_map<std::string, MaterialRefs> parseMaterials(const nlohmann::json& sceneMaterials,
                                                             const nlohmann::json& materialLibrary,
                                                             const nlohmann::json& imageLibrary,
                                                             const MaterialBaseOverrides* overrides = nullptr);

} // namespace pose

#endif // MATERIALPARSER_H
