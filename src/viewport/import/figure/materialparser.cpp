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

// The "diffuse" channel of a material (scene override wins over base).
const nlohmann::json* diffuseChannel(const nlohmann::json& scene, const nlohmann::json* base) {
    if (scene.contains("diffuse") && scene["diffuse"].contains("channel")) {
        return &scene["diffuse"]["channel"];
    }
    if (base && base->contains("diffuse") && (*base)["diffuse"].contains("channel")) {
        return &(*base)["diffuse"]["channel"];
    }
    return nullptr;
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
        // Resolve the base material this one extends (same-file "#id" references only; a cross-file
        // base would need the resolver — not seen in the figures we target, so left for later).
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
        if (const nlohmann::json* c = effectiveChannel(mat, base, "Glossy Roughness")) {
            ref.roughness = channelScalar(*c, ref.roughness);
        }
        if (const nlohmann::json* c = effectiveChannel(mat, base, "Normal Map")) {
            ref.normalImageUri = channelImageUri(*c, imageLibrary);
        }
        if (const nlohmann::json* c = effectiveChannel(mat, base, "Bump Strength")) {
            ref.bumpImageUri = channelImageUri(*c, imageLibrary);
        }

        // Transparency: straight cutout opacity, plus two clear-surface cases that the eye's moisture
        // and cornea rely on — an explicit refraction weight, or a mirror-smooth glossy shell with no
        // diffuse map (a clear coat over the iris). Both read as "see the iris behind it".
        float cutout = 1.0f;
        float refraction = 0.0f;
        if (const nlohmann::json* c = effectiveChannel(mat, base, "Cutout Opacity")) {
            cutout = channelScalar(*c, 1.0f);
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
