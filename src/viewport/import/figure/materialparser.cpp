/**
 * @file materialparser.cpp
 * @brief Implementation of parseMaterials. See materialparser.h.
 */

#include "materialparser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>

namespace pose {

namespace {

// Reads a float_color channel's colour (the tint multiplier) into an RGB vector, if present.
// "current_value" is the *dialed* colour and must win over "value", which is only the channel's
// default — the same convention channelScalar() follows. Reading "value" alone rendered every
// zone with the channel default (a 0.75 grey): textures came out ~25% too dark, and a pupil
// whose material dials its diffuse to black (common across figure generations) drew as a bright
// grey/white dot in the eye instead.
bool readColorValue(const nlohmann::json& channel, glm::vec3& out) {
    for (const char* key : {"current_value", "value"}) {
        const auto it = channel.find(key);
        if (it != channel.end() && it->is_array() && it->size() >= 3) {
            out = glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
            return true;
        }
    }
    return false;
}

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
                return (*m)[0].value("url", std::string());
            }
        }
    }
    return std::string();
}

// A channel's scalar value ("current_value", else "value"), defaulting to @p fallback.
float channelScalar(const nlohmann::json& channel, float fallback) {
    for (const char* key : {"current_value", "value"}) {
        const auto it = channel.find(key);
        if (it != channel.end() && it->is_number()) {
            return it->get<float>();
        }
    }
    return fallback;
}

// A channel's boolean value ("current_value", else "value"; numbers tolerated), else @p fallback.
bool channelBool(const nlohmann::json& channel, bool fallback) {
    for (const char* key : {"current_value", "value"}) {
        const auto it = channel.find(key);
        if (it != channel.end()) {
            if (it->is_boolean()) {
                return it->get<bool>();
            }
            if (it->is_number()) {
                return it->get<float>() != 0.0f;
            }
        }
    }
    return fallback;
}

// Finds the channel with @p id in a material's "extra" studio_material_channels blocks.
const nlohmann::json* findChannelIn(const nlohmann::json& mat, const std::string& id) {
    const auto ex = mat.find("extra");
    if (ex == mat.end() || !ex->is_array()) {
        return nullptr;
    }
    for (const auto& block : *ex) {
        if (block.value("type", std::string()) != "studio_material_channels" ||
            !block.contains("channels")) {
            continue;
        }
        for (const auto& wrapper : block["channels"]) {
            const auto ch = wrapper.find("channel");
            if (ch != wrapper.end() && ch->value("id", std::string()) == id) {
                return &(*ch);
            }
        }
    }
    return nullptr;
}

// The effective channel for @p id: the scene material's override if it has one, else the base's.
const nlohmann::json* effectiveChannel(const nlohmann::json& scene, const nlohmann::json* base,
                                       const std::string& id) {
    if (const nlohmann::json* c = findChannelIn(scene, id)) {
        return c;
    }
    return base ? findChannelIn(*base, id) : nullptr;
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

} // namespace

std::unordered_map<std::string, MaterialRefs> parseMaterials(const nlohmann::json& materials,
                                                             const nlohmann::json& materialLibrary,
                                                             const nlohmann::json& imageLibrary) {
    std::unordered_map<std::string, MaterialRefs> out;
    if (!materials.is_array()) {
        return out;
    }

    // Index the base materials by id so a scene material's "#id" url can find its base.
    std::unordered_map<std::string, const nlohmann::json*> library;
    if (materialLibrary.is_array()) {
        for (const auto& base : materialLibrary) {
            library.emplace(base.value("id", std::string()), &base);
        }
    }

    for (const auto& mat : materials) {
        // Resolve the base material this one extends. Same-file "#id" references resolve here
        // directly; CROSS-FILE bases (newer skin materials' "/data/...#PBRSkin") are pre-resolved
        // by the importer (applySceneMaterials), which appends the referenced base into
        // @p materialLibrary and rewrites the url to this same-file form.
        const nlohmann::json* base = nullptr;
        if (const std::string url = mat.value("url", std::string()); url.size() > 1 && url[0] == '#') {
            const auto it = library.find(url.substr(1));
            if (it != library.end()) {
                base = it->second;
            }
        }

        MaterialRefs ref;

        if (const nlohmann::json* diffuse = diffuseChannel(mat, base)) {
            readColorValue(*diffuse, ref.baseColor);
            ref.diffuseImageUri = channelImageUri(*diffuse, imageLibrary);
        }
        // --- Specular: interpret the material's LOBES, never a single channel. ------------------
        // The source shaders express specular as weighted lobes, and skin materials typically
        // DISABLE the glossy lobe (Glossy Layered Weight = 0, its roughness left at 0) and author
        // the real response in Dual Lobe Specular: broad lobe roughnesses (~0.6–1.0) blended by a
        // ratio, and a reflectivity ~0.3 — which IS real skin's F0 (the source convention:
        // F0 = 0.08 × reflectivity, so 0.5 = the standard 4% dielectric). The old single
        // "Glossy Roughness" read took the disabled lobe's 0 and rendered skin mirror-smooth —
        // wet plastic under image-based specular. Ladder: dual lobe when enabled + weighted, else
        // the glossy lobe, else the struct defaults (the no-channels case: minimal/legacy
        // materials). Channels carried as maps use the dialed scalar (maps aren't sampled here).
        const auto scalarChannel = [&](const char* id, float fallback, bool* present = nullptr) {
            const nlohmann::json* c = effectiveChannel(mat, base, id);
            if (present != nullptr) {
                *present = (c != nullptr);
            }
            return c ? channelScalar(*c, fallback) : fallback;
        };

        // Whether a channel carries a texture map (whose texels multiply the dialed scalar).
        const auto channelHasMap = [&](const char* id) {
            const nlohmann::json* c = effectiveChannel(mat, base, id);
            return c != nullptr && !channelImageUri(*c, imageLibrary).empty();
        };
        // Folds a lobe's weight × reflectivity into the final F0 multiplier. Two corrections keep
        // it skin-honest: (1) weight/reflectivity channels on skin are usually a dark MASK map
        // with the scalar as its multiplier (often dialed >1) — the maps aren't sampled at
        // import, so map-carried channels apply an assumed mask average, else the raw multiplier
        // lands far above the map-scaled truth and skin turns wet; (2) the result is capped at 1
        // (= the standard 4% dielectric F0) — dial values above that are map headroom, not
        // physics, and a dielectric brighter than 4% reads lacquered.
        constexpr float kSpecMaskAssumedAverage = 0.6f;
        const auto foldSpecWeight = [&](float weight, float reflectivity, bool mapped) {
            float w = weight * (reflectivity / 0.5f);
            if (mapped) {
                w *= kSpecMaskAssumedAverage;
            }
            return std::clamp(w, 0.0f, 1.0f);
        };
        // The undiscounted fold, carried alongside for when the mask map itself is resolved and
        // decoded (the sampled texels then do the real scaling; the shader clamps scalar × texel
        // to the same ≤1 ceiling). Clamped loosely — a >1 scalar is legitimate map headroom here.
        const auto foldSpecWeightRaw = [&](float weight, float reflectivity) {
            return std::clamp(weight * (reflectivity / 0.5f), 0.0f, 4.0f);
        };
        // The active lobe's map URI for a channel (used for the roughness + mask maps).
        const auto channelMapUri = [&](const char* id) {
            const nlohmann::json* c = effectiveChannel(mat, base, id);
            return c ? channelImageUri(*c, imageLibrary) : std::string();
        };

        bool dualPresent = false;
        const float dualWeight =
            std::clamp(scalarChannel("Dual Lobe Specular Weight", 0.0f, &dualPresent), 0.0f, 1.0f);
        const nlohmann::json* dualEnable = effectiveChannel(mat, base, "Dual Lobe Specular Enable");
        if (dualPresent && dualWeight > 0.0f && (!dualEnable || channelBool(*dualEnable, true))) {
            const float lobe1 = scalarChannel("Specular Lobe 1 Roughness", 0.7f);
            bool lobe2Present = false;
            float lobe2 = scalarChannel("Specular Lobe 2 Roughness", 0.0f, &lobe2Present);
            if (!lobe2Present) {
                // The newer skin shader expresses lobe 2 as a multiplier on lobe 1's roughness.
                lobe2 = lobe1 * scalarChannel("Specular Lobe 2 Roughness Mult", 1.0f);
            }
            const float ratio =
                std::clamp(scalarChannel("Dual Lobe Specular Ratio", 0.85f), 0.0f, 1.0f);
            ref.roughness = std::clamp(ratio * lobe1 + (1.0f - ratio) * lobe2, 0.0f, 1.0f);
            // Keep the two lobes for the PBR mode to render as real dual-lobe specular (the
            // blended ref.roughness above still feeds the single-lobe shade modes + env mip).
            ref.lobe1Roughness = std::clamp(lobe1, 0.0f, 1.0f);
            ref.lobe2Roughness = std::clamp(lobe2, 0.0f, 1.0f);
            ref.lobeRatio = ratio;
            const float reflectivity = scalarChannel("Dual Lobe Specular Reflectivity", 0.5f);
            ref.specularWeight =
                foldSpecWeight(dualWeight, reflectivity,
                               channelHasMap("Dual Lobe Specular Weight") ||
                                   channelHasMap("Dual Lobe Specular Reflectivity"));
            ref.specularWeightWithMap = foldSpecWeightRaw(dualWeight, reflectivity);
            // The maps themselves, for the decode layer to sample per-texel: lobe 1's roughness
            // map (approximating both lobes) and whichever mask the lobe weight carries.
            ref.roughnessImageUri = channelMapUri("Specular Lobe 1 Roughness");
            ref.specWeightImageUri = channelMapUri("Dual Lobe Specular Weight");
            if (ref.specWeightImageUri.empty()) {
                ref.specWeightImageUri = channelMapUri("Dual Lobe Specular Reflectivity");
            }
        } else {
            bool glossyWeightPresent = false;
            const float glossyWeight = std::clamp(
                scalarChannel("Glossy Layered Weight", 1.0f, &glossyWeightPresent), 0.0f, 1.0f);
            if (glossyWeight > 0.0f) {
                bool haveRough = false, haveGloss = false;
                const float gRough = scalarChannel("Glossy Roughness", 0.0f, &haveRough);
                const float gGloss = scalarChannel("Glossiness", 1.0f, &haveGloss);
                if (haveRough || haveGloss) {
                    // The pair are two dials for one quantity (artists set one, leave the other at
                    // its no-op default); take whichever expresses more roughness. An eye cornea
                    // (both at their defaults) stays mirror-smooth — the catchlight survives.
                    ref.roughness = std::clamp(std::max(gRough, 1.0f - gGloss), 0.0f, 1.0f);
                }
                const float reflectivity = scalarChannel("Glossy Reflectivity", 0.5f);
                ref.specularWeight =
                    foldSpecWeight(glossyWeight, reflectivity,
                                   channelHasMap("Glossy Layered Weight") ||
                                       channelHasMap("Glossy Reflectivity"));
                ref.specularWeightWithMap = foldSpecWeightRaw(glossyWeight, reflectivity);
                // The roughness map pairs with the "Glossy Roughness" channel — only meaningful
                // when that channel (not the Glossiness dial) is what ref.roughness came from.
                if (gRough >= 1.0f - gGloss) {
                    ref.roughnessImageUri = channelMapUri("Glossy Roughness");
                }
                ref.specWeightImageUri = channelMapUri("Glossy Layered Weight");
                if (ref.specWeightImageUri.empty()) {
                    ref.specWeightImageUri = channelMapUri("Glossy Reflectivity");
                }
            } else if (glossyWeightPresent) {
                ref.specularWeight = 0.0f; // both lobes authored OFF — a matte / SSS-only surface
                ref.specularWeightWithMap = 0.0f;
            }
        }
        if (const nlohmann::json* c = effectiveChannel(mat, base, "Metallic Weight")) {
            ref.metallic = std::clamp(channelScalar(*c, 0.0f), 0.0f, 1.0f);
        }

        // --- Top Coat: the thin clear/oily layer skins author over the base lobes. ---------------
        // Gated by the newer shader's explicit enable when present; reflectivity folds into the
        // weight (0.5 = the standard 4% coat F0, same convention as the base lobes).
        bool tcPresent = false;
        const float tcWeightRaw = scalarChannel("Top Coat Weight", 0.0f, &tcPresent);
        const nlohmann::json* tcEnable = effectiveChannel(mat, base, "Top Coat Enable");
        if (tcPresent && tcWeightRaw > 0.0f && (!tcEnable || channelBool(*tcEnable, true))) {
            const float tcReflectivity =
                std::clamp(scalarChannel("Top Coat Reflectivity", 0.5f), 0.0f, 1.0f);
            // The coat's COLOUR scales it: a dark-grey Top Coat Color is how 2017-era skins tame a
            // "top coat as the main specular" setup (weight dialed far above 1 riding a custom
            // Fresnel curve) — ignoring it rendered those skins as a full-strength white glaze,
            // i.e. wet plastic. The >1 weight itself is the curve trick, not a stronger coat: cap
            // at the physical 0..1. A map on the weight is discounted by the same assumed
            // dark-mask average as the spec-mask fold (maps aren't sampled at import).
            glm::vec3 tcColor(1.0f);
            if (const nlohmann::json* c = effectiveChannel(mat, base, "Top Coat Color")) {
                readColorValue(*c, tcColor);
            }
            const float tcColorLum = glm::dot(tcColor, glm::vec3(0.2126f, 0.7152f, 0.0722f));
            float tcW = std::min(tcWeightRaw, 1.0f);
            if (channelHasMap("Top Coat Weight")) {
                tcW *= 0.5f;
            }
            ref.topCoatWeight =
                std::clamp(tcW * (tcReflectivity / 0.5f) * tcColorLum, 0.0f, 1.0f);
            bool tcRoughPresent = false;
            float tcRough = scalarChannel("Top Coat Roughness", 0.0f, &tcRoughPresent);
            if (!tcRoughPresent || tcRough <= 0.0f) {
                // The pair convention again: roughness or glossiness, whichever was dialed.
                tcRough = 1.0f - scalarChannel("Top Coat Glossiness", 0.35f);
            }
            ref.topCoatRoughness = std::clamp(tcRough, 0.0f, 1.0f);
        }

        // --- Translucency: per-region subsurface strength + the transmitted (flesh-red) tint. ----
        // The color map carries the tint skin authors for transmitted light; the grayscale weight
        // map localizes strength (ears/nose high, over bone low). Materials without the channel
        // keep the 0.5 default (the old uniform-dial behaviour).
        if (const nlohmann::json* c = effectiveChannel(mat, base, "Translucency Weight")) {
            ref.translucencyWeight = std::clamp(channelScalar(*c, 0.5f), 0.0f, 1.0f);
        }
        ref.translucencyImageUri = channelMapUri("Translucency Color");
        if (ref.translucencyImageUri.empty()) {
            ref.translucencyImageUri = channelMapUri("Translucency Weight");
        }

        // --- Micro-detail normal: the tiled pore-grain layer (newer skin shader). ----------------
        ref.detailNormalImageUri = channelMapUri("Detail Normal Map");
        if (!ref.detailNormalImageUri.empty()) {
            ref.detailWeight =
                std::clamp(scalarChannel("Detail Weight", 0.5f), 0.0f, 1.0f);
            bool tilesPresent = false;
            ref.detailTiles = scalarChannel("Detail Horizontal Tiles", 32.0f, &tilesPresent);
            if (!tilesPresent) {
                ref.detailTiles = scalarChannel("Detail Vertical Tiles", 32.0f);
            }
            ref.detailTiles = std::clamp(ref.detailTiles, 1.0f, 512.0f);
        }

        // Both detail-map channels carry a dialed STRENGTH alongside the map ("Normal Map" 2.5,
        // "Bump Strength" 4 are typical for heavily-detailed skins) — the map's texels are scaled
        // by it at render. Ignoring it flattened strongly-authored skins (the surface stayed near
        // mirror-coherent, so the broad specular sheen read as wet plastic). Clamped to the
        // push-constant packing range (mesh.record packs mode + strength/8 into one float).
        if (const nlohmann::json* c = effectiveChannel(mat, base, "Normal Map")) {
            ref.normalImageUri = channelImageUri(*c, imageLibrary);
            ref.normalStrength = std::clamp(channelScalar(*c, 1.0f), 0.0f, 7.9f);
        }
        if (const nlohmann::json* c = effectiveChannel(mat, base, "Bump Strength")) {
            ref.bumpImageUri = channelImageUri(*c, imageLibrary);
            ref.bumpStrength = std::clamp(channelScalar(*c, 1.0f), 0.0f, 7.9f);
        }

        // Transparency: straight cutout opacity, plus two clear-surface cases that the eye's moisture
        // and cornea rely on — an explicit refraction weight, or a mirror-smooth glossy shell with no
        // diffuse map (a clear coat over the iris). Both read as "see the iris behind it".
        // The scalar can be masked by an image (lash/brow cutout cards, older generations' eye
        // shells); the mask URI is carried out for the decode layer to bake into the diffuse alpha.
        float cutout = 1.0f;
        float refraction = 0.0f;
        if (const nlohmann::json* c = effectiveChannel(mat, base, "Cutout Opacity")) {
            cutout = channelScalar(*c, 1.0f);
            ref.opacityImageUri = channelImageUri(*c, imageLibrary);
        } else if (const nlohmann::json* legacy = topLevelChannel(mat, base, "transparency")) {
            // Older-generation (pre-PBR-era) materials have no "Cutout Opacity" shader channel; their
            // opacity is the format's CORE "transparency" property (UI name "Opacity Strength" —
            // the value IS the opacity: 0 = invisible). This is how those figures' eye shells go
            // clear — without reading it, the cornea/moisture render as opaque white balls over
            // the eyes.
            cutout = channelScalar(*legacy, 1.0f);
            ref.opacityImageUri = channelImageUri(*legacy, imageLibrary);
        }
        if (const nlohmann::json* c = effectiveChannel(mat, base, "Refraction Weight")) {
            refraction = channelScalar(*c, 0.0f);
        }
        ref.opacity = cutout;
        if (refraction > 0.5f) {
            ref.opacity = std::min(ref.opacity, 0.05f); // clear refractive shell (cornea / eye moisture)
        }
        // NB: do NOT treat "no diffuse map + low roughness" as transparent — that over-broad
        // heuristic wrongly turned some figures' untextured skin zones invisible. Refraction is the
        // reliable clear-shell signal.

        if (const auto g = mat.find("groups"); g != mat.end() && g->is_array()) {
            for (const auto& zone : *g) {
                if (zone.is_string()) {
                    out[zone.get<std::string>()] = ref;
                }
            }
        }
    }

    return out;
}

} // namespace pose
