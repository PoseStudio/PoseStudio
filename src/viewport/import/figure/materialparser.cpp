/**
 * @file materialparser.cpp
 * @brief Implementation of parseMaterials. See materialparser.h.
 *
 * One material is read by readMaterial, section by section — each section a function named for
 * the part of the shader it reads (readSpecular, readTopCoat, …) — through a ChannelReader, which
 * is where "the scene material's channel, else its base's" and "the dialed value, else the
 * default" are said once. parseMaterials itself keeps the base lookup and the loop.
 */

#include "materialparser.h"

#include "figureutils.h" // channelScalar / channelBool / channelColor — the DIALED-value readers

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <initializer_list>
#include <string>
#include <unordered_map>

namespace pose {

namespace {

// Every channel value below is read with figureutils' "current_value wins" readers: a scene
// material's current_value is the DIALED state, "value" only the channel default. Reading "value"
// alone once rendered every zone with the 0.75-grey default (textures ~25% too dark) and drew
// black-dialed pupils as bright dots — the readers document the convention once.

// The texture URI a channel references. Two conventions across figure generations: a direct
// "image_file" path, or an "image" index into the document's image_library whose first map layer
// carries the url. Empty if the channel has no map (e.g. an "image" of null / -1).
std::string channelImageUri(const nlohmann::json& channel, const nlohmann::json& imageLibrary) {
    if (const auto it = channel.find("image_file"); it != channel.end() && it->is_string()) {
        return it->get<std::string>();
    }
    if (const auto it = channel.find("image"); it != channel.end() && it->is_number_integer()) {
        const int idx = it->get<int>();
        if (idx >= 0 && imageLibrary.is_array() && idx < static_cast<int>(imageLibrary.size())) {
            const nlohmann::json& img = imageLibrary[static_cast<std::size_t>(idx)];
            if (const auto m = img.find("map"); m != img.end() && m->is_array() && !m->empty()) {
                return stringOf((*m)[0], "url");
            }
        }
    }
    return std::string();
}

// The channels of a material's "extra" studio_material_channels blocks, indexed by id. A material
// is queried about a hundred times (several of them for the same id); walking the blocks per
// query was a linear scan each time, so each material builds this map once. The FIRST channel
// carrying an id wins, exactly as the walk's first match did.
using ChannelIndex = std::unordered_map<std::string, const nlohmann::json*>;
ChannelIndex indexChannels(const nlohmann::json& mat) {
    ChannelIndex index;
    const auto ex = mat.find("extra");
    if (ex == mat.end() || !ex->is_array()) {
        return index;
    }
    for (const auto& block : *ex) {
        if (stringOf(block, "type") != "studio_material_channels" ||
            !block.contains("channels")) {
            continue;
        }
        for (const auto& wrapper : block["channels"]) {
            const auto ch = wrapper.find("channel");
            if (ch != wrapper.end()) {
                index.emplace(stringOf(*ch, "id"), &(*ch));
            }
        }
    }
    return index;
}

// Finds the channel with @p id in a material's indexed channels.
const nlohmann::json* findChannelIn(const ChannelIndex& index, const std::string& id) {
    const auto it = index.find(id);
    return it != index.end() ? it->second : nullptr;
}

// A top-level material property's channel ("diffuse", "transparency", …; scene override wins
// over base). These are the format's CORE properties, predating the shader-specific channel
// blocks findChannelIn walks.
const nlohmann::json* topLevelChannel(const nlohmann::json& scene, const nlohmann::json* base,
                                      const char* property) {
    if (scene.contains(property) && scene[property].contains("channel")) {
        return &scene[property]["channel"];
    }
    if (base && base->contains(property) && (*base)[property].contains("channel")) {
        return &(*base)[property]["channel"];
    }
    return nullptr;
}

// The "diffuse" channel of a material (scene override wins over base).
const nlohmann::json* diffuseChannel(const nlohmann::json& scene, const nlohmann::json* base) {
    return topLevelChannel(scene, base, "diffuse");
}

// One material's shader channels as the parse asks for them: the scene material's own channel
// where it has one — it only OVERRIDES — else that of the base it extends; and of a channel its
// dialed value, or its map (through the document's image library).
struct ChannelReader {
    const ChannelIndex&   scene;
    const ChannelIndex*   base; ///< null: the material extends no base (that could be found)
    const nlohmann::json& images;

    /// The effective channel for @p id, or null where neither material carries it.
    const nlohmann::json* find(const char* id) const {
        if (const nlohmann::json* c = findChannelIn(scene, id)) {
            return c;
        }
        return base ? findChannelIn(*base, id) : nullptr;
    }
    /// The channel's dialed scalar; @p fallback where the channel is absent (@p present says
    /// which) or carries no number.
    float scalar(const char* id, float fallback, bool* present = nullptr) const {
        const nlohmann::json* c = find(id);
        if (present != nullptr) {
            *present = (c != nullptr);
        }
        return c ? channelScalar(*c, fallback) : fallback;
    }
    /// scalar(), held to [lo, hi].
    float clamped(const char* id, float fallback, float lo, float hi) const {
        return std::clamp(scalar(id, fallback), lo, hi);
    }
    /// The channel's dialed color into @p out; false, and @p out untouched, without one.
    bool color(const char* id, glm::vec3& out) const {
        const nlohmann::json* c = find(id);
        return c != nullptr && channelColor(*c, out);
    }
    /// The channel's dialed switch; @p fallback where the channel is absent or carries none.
    bool flag(const char* id, bool fallback) const {
        const nlohmann::json* c = find(id);
        return c ? channelBool(*c, fallback) : fallback;
    }
    /// The texture the channel carries (its texels multiply the dialed scalar), as the URI the
    /// file wrote; empty without one.
    std::string map(const char* id) const {
        const nlohmann::json* c = find(id);
        return c ? channelImageUri(*c, images) : std::string();
    }
    /// The first of @p ids whose channel carries a map: "this channel's, else that one's".
    std::string mapOf(std::initializer_list<const char*> ids) const {
        for (const char* id : ids) {
            std::string uri = map(id);
            if (!uri.empty()) {
                return uri;
            }
        }
        return std::string();
    }
    bool hasMap(const char* id) const { return !map(id).empty(); }
};

// Folds a lobe's weight × reflectivity into the final F0 multiplier. Two corrections keep it
// skin-honest: (1) weight/reflectivity channels on skin are usually a dark MASK map with the
// scalar as its multiplier (often dialed >1) — the maps aren't sampled at import, so map-carried
// channels apply an assumed mask average, else the raw multiplier lands far above the map-scaled
// truth and skin turns wet; (2) the result is capped at 1 (= the standard 4% dielectric F0) — dial
// values above that are map headroom, not physics, and a dielectric brighter than 4% reads
// lacquered.
float foldSpecWeight(float weight, float reflectivity, bool mapped) {
    constexpr float kSpecMaskAssumedAverage = 0.6f;
    float w = weight * (reflectivity / 0.5f);
    if (mapped) {
        w *= kSpecMaskAssumedAverage;
    }
    return std::clamp(w, 0.0f, 1.0f);
}

// The undiscounted fold, carried alongside for when the mask map itself is resolved and decoded
// (the sampled texels then do the real scaling; the shader clamps scalar × texel to the same ≤1
// ceiling). Clamped loosely — a >1 scalar is legitimate map headroom here.
float foldSpecWeightRaw(float weight, float reflectivity) {
    return std::clamp(weight * (reflectivity / 0.5f), 0.0f, 4.0f);
}

// --- Specular: interpret the material's LOBES, never a single channel. --------------------------
// The source shaders express specular as weighted lobes, and skin materials typically DISABLE the
// glossy lobe (Glossy Layered Weight = 0, its roughness left at 0) and author the real response in
// Dual Lobe Specular: broad lobe roughnesses (~0.6–1.0) blended by a ratio, and a reflectivity
// ~0.3 — which IS real skin's F0 (the source convention: F0 = 0.08 × reflectivity, so 0.5 = the
// standard 4% dielectric). The old single "Glossy Roughness" read took the disabled lobe's 0 and
// rendered skin mirror-smooth — wet plastic under image-based specular. Ladder: dual lobe when
// enabled + weighted, else the glossy lobe, else the struct defaults (the no-channels case:
// minimal/legacy materials). Channels carried as maps use the dialed scalar (maps aren't sampled
// here). The metallic weight, the anisotropy and the diffuse roughness are read with them.
void readSpecular(const ChannelReader& ch, FigureMaterial& out) {
    bool        dualPresent = false;
    const float dualWeight =
        std::clamp(ch.scalar("Dual Lobe Specular Weight", 0.0f, &dualPresent), 0.0f, 1.0f);
    if (dualPresent && dualWeight > 0.0f && ch.flag("Dual Lobe Specular Enable", true)) {
        const float lobe1 = ch.scalar("Specular Lobe 1 Roughness", 0.7f);
        bool        lobe2Present = false;
        float       lobe2 = ch.scalar("Specular Lobe 2 Roughness", 0.0f, &lobe2Present);
        if (!lobe2Present) {
            // The newer skin shader expresses lobe 2 as a multiplier on lobe 1's roughness.
            lobe2 = lobe1 * ch.scalar("Specular Lobe 2 Roughness Mult", 1.0f);
        }
        const float ratio = ch.clamped("Dual Lobe Specular Ratio", 0.85f, 0.0f, 1.0f);
        out.roughness = std::clamp(ratio * lobe1 + (1.0f - ratio) * lobe2, 0.0f, 1.0f);
        // Keep the two lobes for the PBR mode to render as real dual-lobe specular (the blended
        // roughness above still feeds the single-lobe shade modes + env mip).
        out.lobe1Roughness = std::clamp(lobe1, 0.0f, 1.0f);
        out.lobe2Roughness = std::clamp(lobe2, 0.0f, 1.0f);
        out.lobeRatio = ratio;
        const float reflectivity = ch.scalar("Dual Lobe Specular Reflectivity", 0.5f);
        out.specularWeight = foldSpecWeight(dualWeight, reflectivity,
                                            ch.hasMap("Dual Lobe Specular Weight") ||
                                                ch.hasMap("Dual Lobe Specular Reflectivity"));
        out.specularWeightWithMap = foldSpecWeightRaw(dualWeight, reflectivity);
        // The maps themselves, for the decode layer to sample per-texel: lobe 1's roughness map
        // (approximating both lobes) and whichever mask the lobe weight carries.
        out.roughnessMapPath = ch.map("Specular Lobe 1 Roughness");
        out.specMaskMapPath = ch.mapOf({"Dual Lobe Specular Weight", "Dual Lobe Specular Reflectivity"});
    } else {
        bool        glossyWeightPresent = false;
        const float glossyWeight =
            std::clamp(ch.scalar("Glossy Layered Weight", 1.0f, &glossyWeightPresent), 0.0f, 1.0f);
        if (glossyWeight > 0.0f) {
            bool        haveRough = false, haveGloss = false;
            const float gRough = ch.scalar("Glossy Roughness", 0.0f, &haveRough);
            const float gGloss = ch.scalar("Glossiness", 1.0f, &haveGloss);
            if (haveRough || haveGloss) {
                // The pair are two dials for one quantity (artists set one, leave the other at its
                // no-op default); take whichever expresses more roughness. An eye cornea (both at
                // their defaults) stays mirror-smooth — the catchlight survives.
                out.roughness = std::clamp(std::max(gRough, 1.0f - gGloss), 0.0f, 1.0f);
            }
            const float reflectivity = ch.scalar("Glossy Reflectivity", 0.5f);
            out.specularWeight = foldSpecWeight(glossyWeight, reflectivity,
                                                ch.hasMap("Glossy Layered Weight") ||
                                                    ch.hasMap("Glossy Reflectivity"));
            out.specularWeightWithMap = foldSpecWeightRaw(glossyWeight, reflectivity);
            // The roughness map pairs with the "Glossy Roughness" channel — only meaningful when
            // that channel (not the Glossiness dial) is what the roughness came from.
            if (gRough >= 1.0f - gGloss) {
                out.roughnessMapPath = ch.map("Glossy Roughness");
            }
            out.specMaskMapPath = ch.mapOf({"Glossy Layered Weight", "Glossy Reflectivity"});
        } else if (glossyWeightPresent) {
            out.specularWeight = 0.0f; // both lobes authored OFF — a matte / SSS-only surface
            out.specularWeightWithMap = 0.0f;
        }
    }
    out.metallic = ch.clamped("Metallic Weight", 0.0f, 0.0f, 1.0f);

    // The Specular section's further channels and maps (the canonical material's; the maps are
    // stored-only slots).
    out.anisotropy = ch.clamped("Glossy Anisotropy", 0.0f, 0.0f, 1.0f);
    out.anisotropyRotation = ch.clamped("Glossy Anisotropy Rotations", 0.0f, 0.0f, 1.0f) * 360.0f;
    out.specularColorMapPath = ch.map("Glossy Color");
    out.anisotropyMapPath = ch.map("Glossy Anisotropy");
    out.anisotropyRotationMapPath = ch.map("Glossy Anisotropy Rotations");
    out.lobe2RoughnessMapPath = ch.mapOf({"Specular Lobe 2 Roughness Mult", "Specular Lobe 2 Roughness"});
    out.lobeRatioMapPath = ch.map("Dual Lobe Specular Ratio");
    out.diffuseRoughness = ch.clamped("Diffuse Roughness", 0.0f, 0.0f, 1.0f);
    out.diffuseRoughnessMapPath = ch.map("Diffuse Roughness");
}

// --- Top Coat: the thin clear/oily layer skins author over the base lobes. -----------------------
// Its enable is a switch of its own (the weight is read either way); reflectivity folds into the
// weight (0.5 = the standard 4% coat F0, same convention as the base lobes).
void readTopCoat(const ChannelReader& ch, FigureMaterial& out) {
    // The shader's Top Coat Color, read once for its two uses below.
    glm::vec3  color(1.0f);
    const bool hasColor = ch.find("Top Coat Color") != nullptr;
    ch.color("Top Coat Color", color);
    const float colorLum = glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));

    bool        present = false;
    const float weightRaw = ch.scalar("Top Coat Weight", 0.0f, &present);
    out.topCoatEnabled = ch.flag("Top Coat Enable", true);
    if (present && weightRaw > 0.0f) {
        const float reflectivity = ch.clamped("Top Coat Reflectivity", 0.5f, 0.0f, 1.0f);
        // The coat's COLOR scales it: a dark-grey Top Coat Color is how 2017-era skins tame a "top
        // coat as the main specular" setup (weight dialed far above 1 riding a custom Fresnel
        // curve) — ignoring it rendered those skins as a full-strength white glaze, i.e. wet
        // plastic. The >1 weight itself is the curve trick, not a stronger coat: cap at the
        // physical 0..1. A map on the weight is discounted by the same assumed dark-mask average
        // as the spec-mask fold (maps aren't sampled at import).
        float weight = std::min(weightRaw, 1.0f);
        if (ch.hasMap("Top Coat Weight")) {
            weight *= 0.5f;
        }
        out.topCoatWeight = std::clamp(weight * (reflectivity / 0.5f) * colorLum, 0.0f, 1.0f);
        bool  roughPresent = false;
        float rough = ch.scalar("Top Coat Roughness", 0.0f, &roughPresent);
        if (!roughPresent || rough <= 0.0f) {
            // The pair convention again: roughness or glossiness, whichever was dialed.
            rough = 1.0f - ch.scalar("Top Coat Glossiness", 0.35f);
        }
        out.topCoatRoughness = std::clamp(rough, 0.0f, 1.0f);
    }

    if (hasColor) {
        // The shader's Top Coat Color tints the coat's REFLECTION, and its luminance is folded
        // into the weight above (a dark grey coat color is a weak coat). The canonical material's
        // coat color is the tint of what passes THROUGH the coat (OpenPBR's), which the viewport
        // draws, so only the HUE is kept: a grey coat color is white here, and a skin's coat
        // darkens nothing twice.
        out.coatColor = colorLum > 1e-4f ? glm::clamp(color / colorLum, 0.0f, 1.0f) : glm::vec3(1.0f);
    }
    // The coat's INDEX: the shader reads its Top Coat IOR channel in the Fresnel layering mode (2)
    // alone; in the Reflectivity / Weighted modes — the skins' — the coat's reflectance is its
    // Reflectivity, folded into the weight above at the standard 1.5 dielectric's 4%, and that is
    // the index the coat carries (read as the channel's 1.6 whatever the mode, every skin's coat
    // would brighten by a third the day the viewport drew the index, 2026-11-10).
    const int mode = static_cast<int>(ch.scalar("Top Coat Layering Mode", 0.0f) + 0.5f);
    out.coatIor = mode == 2 ? ch.clamped("Top Coat IOR", 1.5f, 1.0f, 3.0f) : 1.5f;
    out.coatAnisotropy = ch.clamped("Top Coat Anisotropy", 0.0f, 0.0f, 1.0f);
    out.coatAnisotropyRotation = ch.clamped("Top Coat Rotations", 0.0f, 0.0f, 1.0f) * 360.0f;
    out.coatAnisotropyMapPath = ch.map("Top Coat Anisotropy");
    out.coatAnisotropyRotationMapPath = ch.map("Top Coat Rotations");
    // The top coat's maps (stored-only slots): the weight, color, roughness and reflectivity (or
    // IOR) channels' images, and the coat's own bump map with its strength.
    out.coatWeightMapPath = ch.map("Top Coat Weight");
    out.coatColorMapPath = ch.map("Top Coat Color");
    out.coatRoughnessMapPath = ch.mapOf({"Top Coat Roughness", "Top Coat Glossiness"});
    out.coatReflectivityMapPath = ch.mapOf({"Top Coat Reflectivity", "Top Coat IOR"});
    out.coatBumpMapPath = ch.map("Top Coat Bump");
    if (!out.coatBumpMapPath.empty()) {
        out.coatBumpStrength = ch.clamped("Top Coat Bump", 1.0f, 0.0f, 7.9f);
    }
}

// --- Translucency, and the volume under the surface. ---------------------------------------------
// The Translucency color map carries the tint skin authors for transmitted light; the grayscale
// weight map localizes strength (ears/nose high, over bone low). Materials without the channel
// keep the 0.5 default (the old uniform-dial behaviour).
void readSubsurface(const ChannelReader& ch, FigureMaterial& out) {
    out.translucencyWeight = ch.clamped("Translucency Weight", 0.5f, 0.0f, 1.0f);
    out.translucencyWeightMapPath = ch.map("Translucency Weight");
    out.translucencyMapPath = ch.mapOf({"Translucency Color", "Translucency Weight"});
    // The Translucency Color is the tint skins author under the surface (the map's own tint); the
    // VOLUME's scattering is SSS Color (the per-channel scattering color), the Scattering
    // Measurement Distance (cm) and SSS Direction — OpenPBR's subsurface radius color, radius
    // scale and scatter anisotropy.
    ch.color("Translucency Color", out.subsurfaceColor);
    ch.color("SSS Color", out.subsurfaceRadiusColor);
    out.subsurfaceRadius = ch.clamped("Scattering Measurement Distance", 1.0f, 0.0f, 100.0f);
    out.subsurfaceAnisotropy = ch.clamped("SSS Direction", 0.0f, -1.0f, 1.0f);
}

// --- Micro-detail normal: the tiled pore-grain layer (newer skin shader). ------------------------
// Read only where the layer has its map; without it every detail value keeps its default.
void readDetail(const ChannelReader& ch, FigureMaterial& out) {
    out.detailNormalMapPath = ch.map("Detail Normal Map");
    if (out.detailNormalMapPath.empty()) {
        return;
    }
    out.detailWeight = ch.clamped("Detail Weight", 0.5f, 0.0f, 1.0f);
    bool tilesPresent = false;
    out.detailTiles = ch.scalar("Detail Horizontal Tiles", 32.0f, &tilesPresent);
    if (!tilesPresent) {
        out.detailTiles = ch.scalar("Detail Vertical Tiles", 32.0f);
    }
    out.detailTiles = std::clamp(out.detailTiles, 1.0f, 512.0f);
    // The layer's further channels (stored-only): the vertical count and the offsets, the
    // roughness multiplier tiled with it, and the weight's and the multiplier's maps.
    out.detailTilesV = ch.clamped("Detail Vertical Tiles", out.detailTiles, 1.0f, 512.0f);
    out.detailOffset = glm::vec2(ch.scalar("Detail Horizontal Offset", 0.0f), ch.scalar("Detail Vertical Offset", 0.0f));
    out.detailRoughness = ch.clamped("Detail Specular Roughness Mult", 1.0f, 0.0f, 4.0f);
    out.detailWeightMapPath = ch.map("Detail Weight");
    out.detailRoughnessMapPath = ch.map("Detail Specular Roughness Mult");
}

// --- The surface's relief: the normal map, the bump map, displacement. ---------------------------
// Both detail-map channels carry a dialed STRENGTH alongside the map ("Normal Map" 2.5, "Bump
// Strength" 4 are typical for heavily-detailed skins) — the map's texels are scaled by it at
// render. Ignoring it flattened strongly-authored skins (the surface stayed near mirror-coherent,
// so the broad specular sheen read as wet plastic). Held to the range the engine's material entry
// packs a strength in (the detail mode and strength / 8 share one float).
void readRelief(const ChannelReader& ch, FigureMaterial& out) {
    out.normalMapPath = ch.map("Normal Map");
    out.normalStrength = ch.clamped("Normal Map", 1.0f, 0.0f, 7.9f);
    out.bumpMapPath = ch.map("Bump Strength");
    out.bumpStrength = ch.clamped("Bump Strength", 1.0f, 0.0f, 7.9f);
    // Displacement (stored-only): the map, and the source app's min / max centimetres turned into
    // the renderers' strength (black to white) + mid level (the value at zero).
    out.displacementMapPath = ch.map("Displacement Strength");
    const float strength = ch.scalar("Displacement Strength", 1.0f);
    const float lowest = ch.scalar("Minimum Displacement", -0.1f);
    const float highest = ch.scalar("Maximum Displacement", 0.1f);
    const float range = highest - lowest;
    out.displacementStrength = std::clamp(range * strength, -100.0f, 100.0f);
    out.displacementMidLevel = std::abs(range) > 1.0e-6f ? std::clamp(-lowest / range, 0.0f, 1.0f) : 0.5f;
    out.displacementSubdiv = std::clamp(std::round(ch.scalar("SubD Displacement Level", 0.0f)), 0.0f, 5.0f);
}

// --- Transparency: the opacity, and what passes through. -----------------------------------------
// Straight cutout opacity, plus the clear-surface case the eye's moisture and cornea rely on — an
// explicit refraction weight ("see the iris behind it"). The scalar can be masked by an image
// (lash/brow cutout cards, older generations' eye shells); the mask's URI is carried out for the
// decode layer to bake into the diffuse alpha. @p mat / @p base are the materials themselves: the
// older generations' opacity is a core property of the material, not a shader channel.
void readTransparency(const ChannelReader& ch, const nlohmann::json& mat, const nlohmann::json* base,
                      FigureMaterial& out) {
    float cutout = 1.0f;
    if (const nlohmann::json* c = ch.find("Cutout Opacity")) {
        cutout = channelScalar(*c, 1.0f);
        out.opacityMapPath = channelImageUri(*c, ch.images);
    } else if (const nlohmann::json* legacy = topLevelChannel(mat, base, "transparency")) {
        // Older-generation (pre-PBR-era) materials have no "Cutout Opacity" shader channel; their
        // opacity is the format's CORE "transparency" property (UI name "Opacity Strength" — the
        // value IS the opacity: 0 = invisible). This is how those figures' eye shells go clear —
        // without reading it, the cornea/moisture render as opaque white balls over the eyes.
        cutout = channelScalar(*legacy, 1.0f);
        out.opacityMapPath = channelImageUri(*legacy, ch.images);
    }
    const float refraction = ch.scalar("Refraction Weight", 0.0f);
    out.opacity = cutout;
    if (refraction > 0.5f) {
        out.opacity = std::min(out.opacity, 0.05f); // clear refractive shell (cornea / eye moisture)
    }
    // NB: do NOT treat "no diffuse map + low roughness" as transparent — that over-broad heuristic
    // wrongly turned some figures' untextured skin zones invisible. Refraction is the reliable
    // clear-shell signal.

    out.transmission = std::clamp(refraction, 0.0f, 1.0f);
    out.transmissionWeightMapPath = ch.map("Refraction Weight");
    // The dielectric's reflectance is its REFLECTIVITY here (F0 = 0.08 × reflectivity, folded into
    // the specular weight with 1.5 as the baseline), never its Refraction Index — the shader's own
    // glossy layer reads it the same way — so the index is the TRANSMISSION's: read for a
    // refracting surface (the eye's clear shells, glass), left at the baseline for the rest (a
    // skin preset authors 1.33, and read into the viewport's F0 it halved every skin's specular on
    // top of the reflectivity that already states it).
    if (refraction > 0.0f) {
        out.ior = ch.clamped("Refraction Index", 1.5f, 1.0f, 3.0f);
    }
    ch.color("Transmitted Color", out.transmissionColor);
    out.transmissionColorMapPath = ch.map("Transmitted Color");
    out.transmissionDepth = std::max(0.0f, ch.scalar("Transmitted Measurement Distance", 0.0f)) * 0.01f; // cm → m
    // What passes through is as rough as what reflects unless the shader keeps the two apart
    // ("Share Glossy Inputs" off): an eye's clear shells author a soft glossy lobe over a
    // refraction of roughness 0, and read as one they are frosted glass over the iris.
    if (refraction > 0.0f && !ch.flag("Share Glossy Inputs", true)) {
        out.transmissionRoughness = std::clamp(std::max(ch.scalar("Refraction Roughness", 0.0f),
                                                        1.0f - ch.scalar("Refraction Glossiness", 1.0f)),
                                               0.0f, 1.0f);
    }
    out.thinWalled = ch.flag("Thin Walled", false);
}

// --- Emission, sheen, the thin film, and where the maps lie on the UVs. --------------------------
void readLayersAndPlacement(const ChannelReader& ch, FigureMaterial& out) {
    if (const nlohmann::json* c = ch.find("Emission Color")) {
        glm::vec3 e(0.0f);
        channelColor(*c, e);
        const float peak = std::max(e.x, std::max(e.y, e.z));
        if (peak > 0.0f) {
            out.emissionColor = e / peak;
            // "Luminance" in cd/m² (the shader's default unit): 1000 cd/m² = the engine's radiance 1.
            out.emission = std::max(0.0f, ch.scalar("Luminance", 1500.0f)) * peak / 1000.0f;
            out.emissionMapPath = ch.map("Emission Color");
        }
    }
    // Backscattering is the shader's sheen: the retro-reflective fuzz of cloth.
    out.sheen = ch.clamped("Backscattering Weight", 0.0f, 0.0f, 1.0f);
    ch.color("Backscattering Color", out.sheenColor);
    out.sheenRoughness = ch.clamped("Backscattering Roughness", 0.5f, 0.0f, 1.0f);
    out.sheenWeightMapPath = ch.map("Backscattering Weight");
    out.sheenColorMapPath = ch.map("Backscattering Color");
    out.sheenRoughnessMapPath = ch.map("Backscattering Roughness");
    // The thin film: a thickness above 0 switches it on (the shader has no weight).
    const float thickness = ch.clamped("Thin Film Thickness", 0.0f, 0.0f, 2000.0f);
    out.thinFilmThicknessMapPath = ch.map("Thin Film Thickness");
    if (thickness > 0.0f || !out.thinFilmThicknessMapPath.empty()) {
        out.thinFilm = 1.0f;
        out.thinFilmThickness = thickness > 0.0f ? thickness : 500.0f;
        out.thinFilmIor = ch.clamped("Thin Film IOR", 1.5f, 1.0f, 3.0f);
    }
    out.uvTiling = glm::vec2(ch.scalar("Horizontal Tiles", 1.0f), ch.scalar("Vertical Tiles", 1.0f));
    out.uvOffset = glm::vec2(ch.scalar("Horizontal Offset", 0.0f), ch.scalar("Vertical Offset", 0.0f));
}

// One scene material merged with the base it extends (@p base and its channel index: both null
// where it extends none that could be found).
FigureMaterial readMaterial(const nlohmann::json& mat, const nlohmann::json* base,
                            const ChannelIndex* baseIndex, const nlohmann::json& imageLibrary) {
    const ChannelIndex  sceneIndex = indexChannels(mat);
    const ChannelReader ch{sceneIndex, baseIndex, imageLibrary};

    FigureMaterial out;
    if (const nlohmann::json* diffuse = diffuseChannel(mat, base)) {
        channelColor(*diffuse, out.baseColor);
        out.diffuseMapPath = channelImageUri(*diffuse, imageLibrary);
    }
    readSpecular(ch, out);
    readTopCoat(ch, out);
    readSubsurface(ch, out);
    readDetail(ch, out);
    readRelief(ch, out);
    readTransparency(ch, mat, base, out);
    readLayersAndPlacement(ch, out);
    return out;
}

} // namespace

std::unordered_map<std::string, FigureMaterial> parseMaterials(const nlohmann::json& materials,
                                                               const nlohmann::json& materialLibrary,
                                                               const nlohmann::json& imageLibrary,
                                                               const MaterialBaseOverrides* overrides,
                                                               std::vector<std::string>* skipped) {
    std::unordered_map<std::string, FigureMaterial> out;
    if (!materials.is_array()) {
        return out;
    }

    // Index the base materials by id so a scene material's "#id" url can find its base.
    std::unordered_map<std::string, const nlohmann::json*> library;
    if (materialLibrary.is_array()) {
        for (const auto& base : materialLibrary) {
            library.emplace(stringOf(base, "id"), &base);
        }
    }
    // Base materials are shared by many scene materials (every skin zone extends the same base),
    // so their channel indexes are built once and reused.
    std::unordered_map<const nlohmann::json*, ChannelIndex> baseIndexes;

    for (const auto& mat : materials) {
        if (overrides && overrides->skip.count(&mat)) {
            continue; // another figure's material (see MaterialBaseOverrides::skip)
        }
        // Resolve the base material this one extends. A same-file "#id" reference names an entry
        // of this document's own library; a CROSS-FILE base (newer skin materials'
        // "/data/...#<SkinShaderBase>") was pre-resolved by the importer (applySceneMaterials),
        // which hands it in through @p overrides under the key it recorded for this material —
        // and that base is looked for THERE alone: the document's own library may hold an entry
        // of the same id, which is another material.
        const nlohmann::json* base = nullptr;
        const std::string*    crossFileKey = nullptr;
        if (overrides) {
            if (const auto cf = overrides->crossFileBase.find(&mat); cf != overrides->crossFileBase.end()) {
                crossFileKey = &cf->second;
            }
        }
        if (crossFileKey) {
            if (const auto ex = overrides->extraBases.find(*crossFileKey); ex != overrides->extraBases.end()) {
                base = ex->second;
            }
        } else if (const std::string url = stringOf(mat, "url"); url.size() > 1 && url[0] == '#') {
            // The reference spells the id percent-encoded ("#Layer%202"), the library plain
            // ("Layer 2"): compared as written, a base whose id has a space in it was never found
            // and its material lost every channel it leaves to the base (the roughness, what
            // makes a shell transparent).
            const bool asWritten = overrides && overrides->baseIdsAsWritten;
            const std::string fragment = asWritten ? url.substr(1) : referencedId(url);
            if (const auto it = library.find(fragment); it != library.end()) {
                base = it->second;
            }
        }

        // A material is an OPTIONAL part of a figure: one whose channels cannot be read leaves its
        // zones their defaults — and is said — where it used to fail the whole import.
        FigureMaterial material;
        try {
            const ChannelIndex* baseIndex = nullptr;
            if (base) {
                auto bi = baseIndexes.find(base);
                if (bi == baseIndexes.end()) {
                    bi = baseIndexes.emplace(base, indexChannels(*base)).first;
                }
                baseIndex = &bi->second;
            }
            material = readMaterial(mat, base, baseIndex, imageLibrary);
        } catch (const std::exception& e) {
            if (skipped) {
                skipped->push_back("material '" + stringOf(mat, "id") + "': " + e.what());
            }
            continue;
        }

        if (const auto g = mat.find("groups"); g != mat.end() && g->is_array()) {
            for (const auto& zone : *g) {
                if (zone.is_string()) {
                    out[zone.get<std::string>()] = material;
                }
            }
        }
    }

    return out;
}

} // namespace pose
