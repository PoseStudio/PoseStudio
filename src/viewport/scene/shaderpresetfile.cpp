/**
 * @file shaderpresetfile.cpp
 * @brief Implementation of the shader-preset reader. See shaderpresetfile.h.
 *
 * THREE STEPS. (1) The document is flattened into one table of SETTINGS (a value and a map
 * each): the shader's definition from `material_library`, the scene material's own channels
 * over it, and the preset's tracks over both — a track is what makes a setting NAMED by the
 * preset. (2) The table decides what the preset is: one that names most of the shader is a
 * FULL surface; one that names a few settings is PARTIAL. (3) A full preset is read layer by
 * layer into a material (readFull); a partial one setting by setting onto the standing
 * material (readPartial).
 *
 * THE SHADER IS RECOGNISED BY WHAT IT HAS, not by a name: the standard physically-based
 * surface shader is the one with a "Base Mixing" setting.
 *
 * HOW A FULL SURFACE IS READ. PoseStudio's material is a metallic / roughness base under one
 * clear dielectric coat. The shader has more ways to reflect than that, so every layer of it
 * is first put as what it reflects head on, and sorted into one of two kinds:
 *   - a FRESNEL layer — a dielectric's few percent head on, rising toward the edges: the base's
 *     glossy layer, a clear top coat. These map onto the material's specular weight / index
 *     and its coat;
 *   - a CONSTANT reflector — the same share of light at every angle, in a color: the weighted
 *     mode's glossy layer, the metallic flakes, a metal, a top coat too strong or too colored
 *     to be a clear coat. These are MERGED: summed into one specular color over what is left of
 *     the diffuse, and that pair is converted to base color + metalness (specgloss.h), which
 *     is what a constant colored reflection is to a metallic / roughness material.
 * With nothing to merge the base maps over directly.
 *
 * The thresholds below are choices, not physics; each says what it separates.
 */

#include "shaderpresetfile.h"

#include "colorcurves.h"    // srgbToLinear: the file's colors are display values
#include "figuredocument.h" // isGzip / gunzip: a preset is gzip-compressed or plain; readFileBytes
#include "figureutils.h"    // urlDecode, stringOf
#include "specgloss.h"      // convertSpecGloss: a diffuse + specular pair as base color + metalness

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <unordered_map>

namespace pose {

namespace {

// --- Thresholds --------------------------------------------------------------------------------

/// A preset that names at least this many shader settings describes a whole surface; fewer, a
/// part of one. (The two kinds lie far apart: a utility preset names one to two dozen settings,
/// a surface about a hundred.)
constexpr int kFullPresetSettings = 40;

/// A top coat reflecting more than this head on is no clear coat (the material's coat is a
/// dielectric: an index of 3 reflects a quarter, and a little past that still reads as a coat)
/// — it is merged into the base as a constant reflector.
constexpr float kCoatMergeAbove = 0.4f;
/// … and so is a coat that reflects in a COLOR (this saturated, and at least this strong): the
/// material's coat reflects white.
constexpr float kCoatTintSaturation = 0.25f;
constexpr float kCoatTintFloor = 0.08f;
/// The most a dielectric reflects head on before it is read as a constant reflector (an index
/// of 3: the material's range).
constexpr float kDielectricCeiling = 0.25f;
/// The standard dielectric's reflectance head on (an index of 1.5): the material's specular
/// weight and coat weight are shares of it.
constexpr float kStandardF0 = 0.04f;
/// Metallic flakes lie at many angles: brought in as a plain layer they are never smoother
/// than this.
constexpr float kFlakeRoughness = 0.3f;
/// The share of the shader's backscattering weight that is the material's sheen weight. The
/// two are not one scale: the shader's own fabric presets run the backscattering at full weight
/// in white over a colored cloth, and the material's sheen at full weight in white is a coat of
/// white fuzz that hides the cloth. Chosen by LOOKING at those presets (a red velvet, a blue
/// silk) rendered at 100 / 50 / 30 / 15 / 0 %: at 15 % the cloth keeps its color under a soft
/// pale bloom.
constexpr float kBackscatterSheen = 0.15f;
/// The most a preset's emission is brought in at: the top of the Materials tab's Emission. A
/// preset made to LIGHT a scene (a bulb, a fire) is dialed for a camera's exposure, a thousand
/// times and more above anything a surface shows.
constexpr float kEmissionCeiling = 100.0f;
/// The area a surface is taken to shine from where a preset gives its emission as a total
/// (lumens, watts) and not per area: a tenth of a square metre.
constexpr float kEmitterArea = 0.1f;

// --- Small math --------------------------------------------------------------------------------

float luminance(const glm::vec3& c) {
    return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

float peakOf(const glm::vec3& c) {
    return std::max(c.x, std::max(c.y, c.z));
}

float saturationOf(const glm::vec3& c) {
    const float hi = peakOf(c);
    return hi > 1.0e-4f ? (hi - std::min(c.x, std::min(c.y, c.z))) / hi : 0.0f;
}

float clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

/// A dielectric's reflectance head on, from its index — and the index from the reflectance.
float f0OfIor(float ior) {
    const float r = (ior - 1.0f) / (ior + 1.0f);
    return r * r;
}

float iorOfF0(float f0) {
    const float s = std::sqrt(std::clamp(f0, 0.0f, 0.98f));
    return (1.0f + s) / (1.0f - s);
}

/// The tint of a black body at @p kelvin, LINEAR, as a share of daylight's (6500 K is white):
/// what the shader's emission temperature multiplies its emission color by. (The widely used
/// curve fit of the Planckian locus in display space, taken to linear light.)
glm::vec3 temperatureTint(float kelvin) {
    const auto display = [](float k) {
        const float t = std::clamp(k, 1000.0f, 40000.0f) / 100.0f;
        glm::vec3   c;
        c.r = t <= 66.0f ? 255.0f : 329.698727446f * std::pow(t - 60.0f, -0.1332047592f);
        c.g = t <= 66.0f ? 99.4708025861f * std::log(t) - 161.1195681661f : 288.1221695283f * std::pow(t - 60.0f, -0.0755148492f);
        c.b = t >= 66.0f ? 255.0f : (t <= 19.0f ? 0.0f : 138.5177312231f * std::log(t - 10.0f) - 305.0447927307f);
        c = glm::clamp(c / 255.0f, 0.0f, 1.0f);
        return glm::vec3(colorcurves::srgbToLinear(c.r), colorcurves::srgbToLinear(c.g), colorcurves::srgbToLinear(c.b));
    };
    const glm::vec3 tint = display(kelvin) / glm::max(display(6500.0f), glm::vec3(1.0e-4f));
    return tint / std::max(peakOf(tint), 1.0e-4f);
}

// --- The settings table ------------------------------------------------------------------------

/// One setting of the shader as the preset leaves it.
struct Setting {
    nlohmann::json value;              ///< The dialed value: a number, a switch, a color. Null where none.
    std::string    image;              ///< The map's URI as the file wrote it; empty = no map.
    int            layers = 0;         ///< How many images the map layers (the first is the one read).
    bool           named = false;      ///< The PRESET sets the value (a track), not just the shader's definition.
    bool           imageNamed = false; ///< … or says the map (also "no map").
};

using Settings = std::unordered_map<std::string, Setting>;

/// The URI of an entry of the document's image library — its first layer that IS an image (a
/// layer may be a flat color, which names no file) — and how many layers the entry has. Empty
/// where the entry names no image.
std::string libraryEntryImage(const nlohmann::json& entry, int& layers) {
    layers = 0;
    const auto map = entry.find("map");
    if (map == entry.end() || !map->is_array() || map->empty()) {
        return std::string();
    }
    layers = static_cast<int>(map->size());
    for (const nlohmann::json& layer : *map) {
        if (std::string url = stringOf(layer, "url"); !url.empty()) {
            return url;
        }
    }
    return std::string();
}

/// … of the entry at @p index; empty for no such entry.
std::string libraryImage(const nlohmann::json& images, int index, int& layers) {
    layers = 0;
    if (index < 0 || !images.is_array() || index >= static_cast<int>(images.size())) {
        return std::string();
    }
    return libraryEntryImage(images[static_cast<std::size_t>(index)], layers);
}

/// … of the entry a "#id" reference names (the id percent-encoded in the reference, plain in
/// the library); empty for no such entry.
std::string libraryImage(const nlohmann::json& images, const std::string& reference, int& layers) {
    layers = 0;
    const std::string id = urlDecode(reference.substr(1));
    if (images.is_array()) {
        for (const nlohmann::json& entry : images) {
            if (stringOf(entry, "id") == id || stringOf(entry, "name") == id) {
                return libraryEntryImage(entry, layers);
            }
        }
    }
    return std::string();
}

/// A setting's map as a track or a channel gives it: a path, an entry of the image library by
/// its index or by "#id", or null / -1 for none.
void setImage(Setting& setting, const nlohmann::json& image, const nlohmann::json& images) {
    setting.image.clear();
    setting.layers = 0;
    if (image.is_string()) {
        const std::string text = image.get<std::string>();
        if (!text.empty() && text[0] == '#') {
            setting.image = libraryImage(images, text, setting.layers);
        } else {
            setting.image = text;
            setting.layers = text.empty() ? 0 : 1;
        }
    } else if (image.is_number_integer()) {
        setting.image = libraryImage(images, image.get<int>(), setting.layers);
    }
}

/// One channel of a material definition into the table (the dialed value wins over the default).
void absorbChannel(const std::string& id, const nlohmann::json& channel, const nlohmann::json& images, Settings& settings) {
    if (id.empty() || !channel.is_object()) {
        return;
    }
    Setting& setting = settings[id];
    if (const auto current = channel.find("current_value"); current != channel.end() && !current->is_null()) {
        setting.value = *current;
    } else if (const auto value = channel.find("value"); value != channel.end() && !value->is_null()) {
        setting.value = *value;
    }
    if (const auto file = channel.find("image_file"); file != channel.end()) {
        setImage(setting, *file, images);
    } else if (const auto image = channel.find("image"); image != channel.end()) {
        setImage(setting, *image, images);
    }
}

/// A material object's channels into the table: the format's core properties ("diffuse",
/// "transparency", …: an object with a "channel" each, keyed by the property) and the shader's
/// own (the "extra" blocks' channel lists, keyed by each channel's id).
void absorbMaterial(const nlohmann::json& material, const nlohmann::json& images, Settings& settings) {
    if (!material.is_object()) {
        return;
    }
    for (auto it = material.begin(); it != material.end(); ++it) {
        if (it->is_object()) {
            if (const auto channel = it->find("channel"); channel != it->end()) {
                absorbChannel(it.key(), *channel, images, settings);
            }
        }
    }
    const auto extra = material.find("extra");
    if (extra == material.end() || !extra->is_array()) {
        return;
    }
    for (const nlohmann::json& block : *extra) {
        const auto channels = block.find("channels");
        if (channels == block.end() || !channels->is_array()) {
            continue;
        }
        for (const nlohmann::json& wrapper : *channels) {
            if (const auto channel = wrapper.find("channel"); channel != wrapper.end()) {
                absorbChannel(stringOf(*channel, "id"), *channel, images, settings);
            }
        }
    }
}

/// The preset's tracks into the table. A track addresses "the selection's materials" and one
/// leaf of one setting: `…#materials:?<setting>/value`, or the shader's own channels
/// `…?extra/<block>/channels/<setting>/value`, with `image_file` and `image` for the map. Its
/// one key carries the value. Tracks of anything else (a node's transform) are passed over.
void absorbTracks(const nlohmann::json& animations, const nlohmann::json& images, Settings& settings) {
    if (!animations.is_array()) {
        return;
    }
    for (const nlohmann::json& track : animations) {
        const std::string url = stringOf(track, "url");
        const std::size_t query = url.find('?');
        if (query == std::string::npos || url.substr(0, query).find("#materials") == std::string::npos) {
            continue;
        }
        // The path's segments, each percent-decoded on its own (a setting's name may hold an
        // encoded slash).
        std::vector<std::string> segments;
        for (std::size_t at = query + 1; at <= url.size();) {
            const std::size_t slash = std::min(url.find('/', at), url.size());
            segments.push_back(urlDecode(url.substr(at, slash - at)));
            at = slash + 1;
        }
        if (segments.size() < 2) {
            continue;
        }
        const std::string& leaf = segments.back();
        const std::string& id = segments[segments.size() - 2];
        const auto         keys = track.find("keys");
        if (id.empty() || keys == track.end() || !keys->is_array() || keys->empty() || !(*keys)[0].is_array() || (*keys)[0].size() < 2) {
            continue;
        }
        const nlohmann::json& value = (*keys)[0][1];
        if (leaf == "value") {
            Setting& setting = settings[id];
            setting.value = value;
            setting.named = true;
        } else if (leaf == "image_file" || leaf == "image") {
            Setting& setting = settings[id];
            setImage(setting, value, images);
            setting.imageNamed = true;
        }
    }
}

/// Settings every preset carries that say nothing of the surface: bookkeeping of the source
/// application, and the object's own smoothing (which PoseStudio keeps with the object).
bool isBookkeeping(const std::string& id) {
    static const char* const kNames[] = {"Tags",          "OutlineColorID",       "Render Priority",               "Propagate Priority",
                                         "Smooth On",     "Smooth Angle",         "Round Corners Radius",          "Round Corners Across Materials",
                                         "Round Corners Roundness", "uv_set",     "Line Start Width",              "Line End Width"};
    for (const char* name : kNames) {
        if (id == name) {
            return true;
        }
    }
    return false;
}

// --- Reading the table -------------------------------------------------------------------------

/// The settings as the readers ask for them, with the notes and the textures they collect.
struct Preset {
    Settings                   settings;
    const ShaderPresetMapMean* mapMean = nullptr;
    ShaderPresetMaterial*      out = nullptr;

    const Setting* find(const char* id) const {
        const auto it = settings.find(id);
        return it != settings.end() ? &it->second : nullptr;
    }
    /// Whether the setting carries a value at all (the shader has it).
    bool has(const char* id) const {
        const Setting* s = find(id);
        return s != nullptr && !s->value.is_null();
    }
    float num(const char* id, float fallback) const {
        const Setting* s = find(id);
        if (s == nullptr) {
            return fallback;
        }
        if (s->value.is_number()) {
            const float v = s->value.get<float>();
            return std::isfinite(v) ? v : fallback;
        }
        return s->value.is_boolean() ? (s->value.get<bool>() ? 1.0f : 0.0f) : fallback;
    }
    bool flag(const char* id, bool fallback) const {
        const Setting* s = find(id);
        if (s == nullptr) {
            return fallback;
        }
        if (s->value.is_boolean()) {
            return s->value.get<bool>();
        }
        return s->value.is_number() ? s->value.get<float>() != 0.0f : fallback;
    }
    /// The setting's color, LINEAR (the file holds display values); @p fallback (linear) without one.
    glm::vec3 color(const char* id, const glm::vec3& fallback) const {
        const Setting* s = find(id);
        if (s == nullptr || !s->value.is_array() || s->value.size() < 3) {
            return fallback;
        }
        glm::vec3 c(0.0f);
        for (int i = 0; i < 3; ++i) {
            const nlohmann::json& component = s->value[static_cast<std::size_t>(i)];
            if (!component.is_number()) {
                return fallback;
            }
            c[i] = colorcurves::srgbToLinear(std::clamp(component.get<float>(), 0.0f, 1.0f));
        }
        return c;
    }
    const std::string& image(const char* id) const {
        static const std::string kNone;
        const Setting*           s = find(id);
        return s != nullptr ? s->image : kNone;
    }
    bool hasImage(const char* id) const { return !image(id).empty(); }
    /// The mean color of the setting's map, linear; white without a map or where the caller
    /// cannot say.
    glm::vec3 imageMean(const char* id) const {
        const std::string& uri = image(id);
        glm::vec3          mean(1.0f);
        if (!uri.empty() && mapMean != nullptr && *mapMean && (*mapMean)(uri, mean)) {
            return glm::clamp(mean, 0.0f, 1.0f);
        }
        return glm::vec3(1.0f);
    }
    /// The setting's color as the surface shows it on average: the color × its map's mean.
    glm::vec3 tinted(const char* id, const glm::vec3& fallback) const { return color(id, fallback) * imageMean(id); }

    void note(const std::string& text) const {
        if (std::find(out->notes.begin(), out->notes.end(), text) == out->notes.end()) {
            out->notes.push_back(text);
        }
    }
    /// Puts the setting's map into @p slot (the first claim of a slot stands).
    void put(MaterialMap slot, const char* id, bool inverted = false) const {
        const Setting* s = find(id);
        if (s == nullptr || s->image.empty()) {
            return;
        }
        for (const ShaderPresetTexture& texture : out->textures) {
            if (texture.slot == slot) {
                return;
            }
        }
        out->textures.push_back({slot, s->image, inverted});
        if (s->layers > 1) {
            note("Some of its maps are several images layered over one another; the first image of each is used.");
        }
    }
};

/// A roughness the shader holds, as the material's: the shader squares its roughness itself
/// when "Roughness Squared" is on (the usual convention, and the material's); with it off the
/// number is the squared value already.
float roughnessOf(const Preset& p, float value) {
    const float v = clamp01(value);
    return p.flag("Roughness Squared", true) ? v : std::sqrt(v);
}

/// How rough a layer is, and the map that says so: its roughness setting, or — the pair being
/// two dials for one quantity — one less its glossiness, whose map is then read inverted.
struct Gloss {
    float       roughness = 0.5f;
    const char* map = nullptr; ///< The setting whose image is the roughness map; null for none.
    bool        inverted = false;
};

Gloss glossOf(const Preset& p, const char* roughnessId, const char* glossinessId, bool glossinessFirst) {
    Gloss g;
    const bool byRoughness = !glossinessFirst && p.has(roughnessId) && (p.num(roughnessId, 0.0f) > 0.0f || !p.has(glossinessId) || p.hasImage(roughnessId));
    if (byRoughness) {
        g.roughness = roughnessOf(p, p.num(roughnessId, 0.5f));
        g.map = p.hasImage(roughnessId) ? roughnessId : nullptr;
        return g;
    }
    if (p.hasImage(glossinessId)) {
        // The map carries the glossiness; inverted it is the roughness, at full strength.
        g.roughness = 1.0f;
        g.map = glossinessId;
        g.inverted = true;
        return g;
    }
    g.roughness = clamp01(1.0f - p.num(glossinessId, 1.0f));
    return g;
}

/// A layer that reflects the same at every angle (see the file comment), as the merge takes it.
struct Reflector {
    glm::vec3   reflectance{0.0f};
    Gloss       gloss;
    const char* colorMap = nullptr; ///< The setting whose image colors it; null for none.
};

/// A dielectric's reflectance head on (@p f0, a color) as the material's specular weight,
/// tint and — for an opaque surface that reflects more than the standard dielectric — index.
void setDielectric(MaterialParams& m, const glm::vec3& f0, bool transmissive) {
    const float peak = peakOf(f0);
    if (peak <= 1.0e-5f) {
        m.specular = 0.0f;
        return;
    }
    m.specularColor = glm::clamp(f0 / peak, 0.0f, 1.0f);
    if (transmissive) {
        // The index is the refraction's; the weight is the share of that index's reflectance.
        m.specular = clamp01(peak / std::max(f0OfIor(m.ior), 1.0e-4f));
    } else if (peak <= kStandardF0) {
        m.ior = 1.5f;
        m.specular = peak / kStandardF0;
    } else {
        m.ior = std::clamp(iorOfF0(std::min(peak, kDielectricCeiling)), 1.5f, 3.0f);
        m.specular = 1.0f;
    }
}

// --- A full surface ----------------------------------------------------------------------------

void readRelief(const Preset& p, MaterialParams& m) {
    if (p.hasImage("Normal Map")) {
        p.put(MaterialMap::Normal, "Normal Map");
        m.normalStrength = std::clamp(p.num("Normal Map", 1.0f), 0.0f, 7.9f);
    }
    if (p.hasImage("Bump Strength")) {
        p.put(MaterialMap::Bump, "Bump Strength");
        m.bumpStrength = std::clamp(p.num("Bump Strength", 1.0f), 0.0f, 7.9f);
    }
    if (p.hasImage("Displacement Strength")) {
        // The shader's lowest / highest displacement in centimetres as the material's strength
        // (black to white) and mid level (the value at no displacement).
        p.put(MaterialMap::Displacement, "Displacement Strength");
        const float strength = p.num("Displacement Strength", 1.0f);
        const float lowest = p.num("Minimum Displacement", -0.1f);
        const float highest = p.num("Maximum Displacement", 0.1f);
        const float range = highest - lowest;
        m.displacementStrength = std::clamp(range * strength, -100.0f, 100.0f);
        m.displacementMidLevel = std::abs(range) > 1.0e-6f ? clamp01(-lowest / range) : 0.5f;
        m.displacementSubdiv = std::clamp(std::round(p.num("SubD Displacement Level", 0.0f)), 0.0f, 5.0f);
    }
    m.uvTiling = glm::vec2(p.num("Horizontal Tiles", 1.0f), p.num("Vertical Tiles", 1.0f));
    m.uvOffset = glm::vec2(p.num("Horizontal Offset", 0.0f), p.num("Vertical Offset", 0.0f));
    m.opacity = clamp01(p.num("Cutout Opacity", 1.0f));
    p.put(MaterialMap::Opacity, "Cutout Opacity");
}

void readEmission(const Preset& p, MaterialParams& m) {
    glm::vec3 color = p.color("Emission Color", glm::vec3(0.0f));
    if (peakOf(color) <= 0.0f) {
        return;
    }
    if (const float kelvin = p.num("Emission Temperature", 0.0f); kelvin > 0.0f) {
        color *= temperatureTint(kelvin);
    }
    const float peak = peakOf(color);
    if (peak <= 0.0f) {
        return;
    }
    // The luminance in candelas per square metre, whatever unit the preset dialed it in.
    float       luminance = std::max(0.0f, p.num("Luminance", 1500.0f));
    const int   units = static_cast<int>(std::lround(p.num("Luminance Units", 0.0f)));
    const float kPi = 3.14159265f;
    switch (units) {
    case 1: luminance *= 1000.0f; break;        // thousands of candelas per square metre
    case 2: luminance *= 10.7639f; break;       // candelas per square foot
    case 3: luminance *= 10000.0f; break;       // candelas per square centimetre
    case 4:                                     // lumens: a total, spread over the assumed area
    case 5: {                                   // watts: lumens through the efficacy
        const float lumens = units == 5 ? luminance * std::max(0.0f, p.num("Luminous Efficacy", 15.0f)) : luminance;
        luminance = lumens / (kPi * kEmitterArea);
        p.note("Its light is given as a total (lumens or watts), which depends on the size of the object it is put on: "
               "it was brought in for a surface a tenth of a square metre large. Adjust Emission to taste.");
        break;
    }
    default: break;                             // candelas per square metre
    }
    m.emissionColor = color / peak;
    m.emission = luminance * peak / 1000.0f; // (1000 cd/m² = the engine's radiance 1)
    if (m.emission > kEmissionCeiling) {
        m.emission = kEmissionCeiling;
        p.note("It is a light source, dialed far brighter than a surface shows: its Emission was brought in at the top of the range. "
               "Lower it to taste.");
    }
    p.put(MaterialMap::Emission, "Emission Color");
}

void readVolume(const Preset& p, MaterialParams& m, float refraction) {
    // (Called with the base read: a scattering solid takes its color from the volume.)
    // Translucency: light under the surface.
    m.translucency = clamp01(p.num("Translucency Weight", 0.0f));
    if (m.translucency > 0.0f) {
        m.subsurfaceColor = p.color("Translucency Color", glm::vec3(1.0f));
        m.subsurfaceRadiusColor = p.color("SSS Color", glm::vec3(1.0f));
        m.subsurfaceRadius = std::clamp(p.num("Scattering Measurement Distance", 1.0f), 0.0f, 100.0f);
        m.subsurfaceAnisotropy = std::clamp(p.num("SSS Direction", 0.0f), -1.0f, 1.0f);
        p.put(MaterialMap::Translucency, "Translucency Color");
        p.put(MaterialMap::TranslucencyWeight, "Translucency Weight");
    }
    // Refraction: light through it.
    m.thinWalled = p.flag("Thin Walled", false);
    if (refraction <= 0.0f) {
        return;
    }
    const glm::vec3 volume = p.color("Transmitted Color", glm::vec3(1.0f));
    const float     distance = std::max(0.0f, p.num("Transmitted Measurement Distance", 0.0f));
    // A solid that SCATTERS the light it lets in (wax, jade, milk) is no glass: by the share it
    // scatters it is a subsurface solid in the volume's color, and only the rest passes through.
    const float scatter = m.thinWalled ? 0.0f : clamp01(p.num("SSS Amount", 0.0f));
    if (scatter > 0.0f) {
        const float share = refraction * scatter;
        const bool  tinted = peakOf(volume) > 0.004f;
        m.translucency = std::max(m.translucency, share);
        m.subsurfaceColor = tinted ? volume : glm::vec3(1.0f);
        m.subsurfaceRadius = std::clamp(p.num("Scattering Measurement Distance", 1.0f), 0.0f, 100.0f);
        m.subsurfaceAnisotropy = std::clamp(p.num("SSS Direction", 0.0f), -1.0f, 1.0f);
        if (tinted) {
            m.baseColor = glm::mix(m.baseColor, volume, share);
        }
        refraction *= 1.0f - scatter;
        if (refraction <= 0.004f) {
            return;
        }
    }
    m.transmission = refraction;
    p.put(MaterialMap::TransmissionWeight, "Refraction Weight");
    // (A BLACK transmitted color is the shader's "none": the volume then absorbs nothing.)
    const bool      absorbs = !m.thinWalled && distance > 0.0f && peakOf(volume) > 0.004f && peakOf(glm::vec3(1.0f) - volume) > 0.004f;
    if (absorbs) {
        // A solid that tints light by how far it travels: the color reached at that distance.
        m.transmissionColor = volume;
        m.transmissionDepth = distance * 0.01f; // cm → m
        p.put(MaterialMap::TransmissionColor, "Transmitted Color");
    } else {
        m.transmissionColor = p.color("Refraction Color", glm::vec3(1.0f));
        m.transmissionDepth = 0.0f;
        p.put(MaterialMap::TransmissionColor, "Refraction Color");
    }
    if (const float abbe = p.num("Abbe", 0.0f); abbe > 0.0f) {
        m.dispersion = clamp01(20.0f / abbe); // (the Abbe number: low = strong dispersion)
    }
    if (!p.flag("Share Glossy Inputs", true)) {
        // What passes through has a roughness of its own.
        m.transmissionRoughness = glossOf(p, "Refraction Roughness", "Refraction Glossiness", false).roughness;
    }
}

void readSheenAndFilm(const Preset& p, MaterialParams& m) {
    // Backscattering is the shader's sheen: the retro-reflective fuzz of cloth.
    m.sheen = clamp01(p.num("Backscattering Weight", 0.0f)) * kBackscatterSheen;
    if (m.sheen > 0.0f || p.hasImage("Backscattering Weight")) {
        m.sheenColor = p.color("Backscattering Color", glm::vec3(1.0f));
        m.sheenRoughness = glossOf(p, "Backscattering Roughness", "Backscattering Glossiness", false).roughness;
        p.put(MaterialMap::SheenWeight, "Backscattering Weight");
        p.put(MaterialMap::SheenColor, "Backscattering Color");
        p.put(MaterialMap::SheenRoughness, "Backscattering Roughness");
    }
    // The thin film: a thickness above 0 switches it on (the base's, else the coat's).
    const float base = p.num("Base Thin Film", 0.0f);
    const float coat = p.num("Top Coat Thin Film", 0.0f);
    if (const float thickness = base > 0.0f ? base : coat; thickness > 0.0f) {
        m.thinFilm = 1.0f;
        m.thinFilmThickness = std::clamp(thickness, 1.0f, 2000.0f);
        m.thinFilmIor = std::clamp(p.num(base > 0.0f ? "Base Thin Film IOR" : "Top Coat Thin Film IOR", 1.5f), 1.0f, 3.0f);
    }
}

void readFull(const Preset& p, ShaderPresetMaterial& result) {
    MaterialParams m;      // the material's own defaults …
    m.translucency = 0.0f; // … but for the skin-like wrap: a surface says its own
    const int       mode = std::clamp(static_cast<int>(std::lround(p.num("Base Mixing", 0.0f))), 0, 2);
    const glm::vec3 diffuseColor = p.color("diffuse", glm::vec3(1.0f));
    const glm::vec3 diffuseMean = p.imageMean("diffuse");
    const float     refraction = clamp01(p.num("Refraction Weight", 0.0f)) * luminance(p.imageMean("Refraction Weight"));
    if (refraction > 0.0f) {
        m.ior = std::clamp(p.num("Refraction Index", 1.5f), 1.0f, 3.0f);
    }

    std::string            trace;         // which layers went where (the report's)
    std::vector<Reflector> merged;        // the constant reflectors, topmost first
    glm::vec3              through(1.0f); // the share of light that gets below them

    // --- The top coat ---------------------------------------------------------------------------
    const float coatWeight = clamp01(p.num("Top Coat Weight", 0.0f));
    bool        clearCoat = false;
    if (coatWeight * luminance(p.imageMean("Top Coat Weight")) > 1.0e-4f) {
        // What the coat reflects head on, by its layering mode: a reflectivity (as a share of
        // 8 %), a plain weight (no Fresnel: all of it), an index, or a curve's own value.
        const int   layering = static_cast<int>(std::lround(p.num("Top Coat Layering Mode", 0.0f)));
        const float f0 = layering == 1   ? 1.0f
                         : layering == 2 ? f0OfIor(std::max(1.0f, p.num("Top Coat IOR", 1.5f)))
                         : layering == 3 ? clamp01(p.num("Top Coat Curve Normal", kStandardF0))
                                         : 0.08f * clamp01(p.num("Reflectivity", 0.5f));
        const glm::vec3 color = p.tinted("Top Coat Color", glm::vec3(1.0f));
        const float     weight = coatWeight * luminance(p.imageMean("Top Coat Weight"));
        const glm::vec3 reflectance = glm::clamp(weight * f0 * color, 0.0f, 1.0f);
        const Gloss     gloss = glossOf(p, "Top Coat Roughness", "Top Coat Glossiness", false);
        const bool      strong = luminance(reflectance) > kCoatMergeAbove;
        const bool      colored = saturationOf(color) > kCoatTintSaturation && luminance(reflectance) > kCoatTintFloor;
        if (strong || colored) {
            merged.push_back({reflectance, gloss, p.hasImage("Top Coat Color") ? "Top Coat Color" : nullptr});
            through *= glm::vec3(1.0f) - reflectance;
            trace += strong ? " coat(strong)" : " coat(colored)";
        } else if (luminance(reflectance) > 1.0e-4f) {
            // A clear coat: the material's own, its index set from what it reflects (a plain
            // weight has no index: the whole reflection becomes one).
            clearCoat = true;
            float coatF0 = layering == 1 ? luminance(reflectance) : f0 * luminance(color);
            m.topCoat = layering == 1 ? 1.0f : coatWeight;
            if (coatF0 >= kStandardF0) {
                m.coatIor = std::clamp(iorOfF0(std::min(coatF0, kDielectricCeiling)), 1.5f, 3.0f);
            } else {
                m.coatIor = 1.5f;
                m.topCoat *= coatF0 / kStandardF0;
            }
            m.topCoatRoughness = gloss.roughness;
            if (layering != 1) {
                p.put(MaterialMap::CoatWeight, "Top Coat Weight");
            }
            if (gloss.map != nullptr) {
                p.put(MaterialMap::CoatRoughness, gloss.map, gloss.inverted);
            }
            if (p.hasImage("Top Coat Bump")) {
                p.put(MaterialMap::CoatBump, "Top Coat Bump");
                m.coatBumpStrength = std::clamp(p.num("Top Coat Bump", 1.0f), 0.0f, 7.9f);
            }
            m.coatAnisotropy = clamp01(p.num("Top Coat Anisotropy", 0.0f));
            m.coatAnisotropyRotation = clamp01(p.num("Top Coat Rotations", 0.0f)) * 360.0f;
            if (saturationOf(color) > kCoatTintSaturation) {
                p.note("Its top coat reflects in a color; PoseStudio's coat reflects white.");
            }
        }
    }

    // --- The metallic flakes --------------------------------------------------------------------
    // (Flakes of no size are no flakes: a "reset" preset leaves the weight up and the size at 0.)
    if (const float flakes = p.num("Metallic Flakes Size", 0.001f) > 0.0f
                                 ? clamp01(p.num("Metallic Flakes Weight", 0.0f)) * luminance(p.imageMean("Metallic Flakes Weight"))
                                 : 0.0f;
        flakes > 0.01f) {
        Gloss gloss = glossOf(p, "Metallic Flakes Roughness", "Metallic Flakes Glossiness", false);
        gloss.roughness = std::max(gloss.roughness, kFlakeRoughness);
        gloss.map = nullptr;
        merged.push_back({through * flakes * p.tinted("Metallic Flakes Color", glm::vec3(1.0f)), gloss,
                          p.hasImage("Metallic Flakes Color") ? "Metallic Flakes Color" : nullptr});
        through *= 1.0f - flakes;
        trace += " flakes";
        p.note("Its metallic flakes are brought in as a plain metallic layer: the sparkle of single flakes is not carried.");
    }

    // --- The base, by its mixing mode -----------------------------------------------------------
    const Gloss gloss = glossOf(p, "Glossy Roughness", "Glossiness", mode == 1);
    glm::vec3   diffuseShare = through; // what the diffuse keeps of the light
    glm::vec3   fresnel(0.0f);          // the glossy layer's reflectance head on, where it is a dielectric's
    const char* fresnelMap = nullptr;   // … and the map that colors it
    float       metal = 0.0f;
    if (refraction >= 0.5f) {
        // A REFRACTING surface reflects as its index says, in every mode: its glossy layer is
        // the glass's own reflection (tinted by the glossy color), never a mirror over it.
        fresnel = f0OfIor(m.ior) * p.color("Glossy Color", glm::vec3(1.0f));
        fresnelMap = p.hasImage("Glossy Color") ? "Glossy Color" : nullptr;
    } else if (mode == 0) {
        // Metallicity / roughness: a metal share, and a dielectric glossy layer over the rest.
        metal = clamp01(p.num("Metallic Weight", 0.0f));
        fresnel = 0.08f * clamp01(p.num("Glossy Reflectivity", 0.5f)) * clamp01(p.num("Glossy Layered Weight", 1.0f)) *
                  p.color("Glossy Color", glm::vec3(1.0f));
        fresnelMap = p.hasImage("Glossy Color") ? "Glossy Color" : nullptr;
    } else if (mode == 1) {
        // Specular / glossiness: the glossy layer reflects its specular color head on.
        fresnel = clamp01(p.num("Glossy Layered Weight", 1.0f)) * luminance(p.imageMean("Glossy Layered Weight")) *
                  p.tinted("Glossy Specular", glm::vec3(kStandardF0)) * p.tinted("Glossy Color", glm::vec3(1.0f));
        fresnelMap = p.hasImage("Glossy Specular") ? "Glossy Specular" : p.hasImage("Glossy Color") ? "Glossy Color" : nullptr;
    } else {
        // Weighted: a diffuse and a glossy layer mixed by two weights (brought to a sum of one
        // where they exceed it); the glossy one reflects its color at every angle.
        float diffuseWeight = clamp01(p.num("Diffuse Weight", 1.0f));
        float glossyWeight = clamp01(p.num("Glossy Weight", 0.0f)) * luminance(p.imageMean("Glossy Weight"));
        if (const float sum = diffuseWeight + glossyWeight; sum > 1.0f) {
            diffuseWeight /= sum;
            glossyWeight /= sum;
        }
        if (glossyWeight > 1.0e-3f) {
            merged.push_back({through * glossyWeight * p.tinted("Glossy Color", glm::vec3(1.0f)), gloss,
                              p.hasImage("Glossy Color") ? "Glossy Color" : nullptr});
            trace += " glossy(weighted)";
        }
        diffuseShare *= diffuseWeight;
    }
    if (peakOf(fresnel) > kDielectricCeiling) {
        // More than a dielectric reflects: a metal said as a specular color.
        merged.push_back({through * glm::clamp(fresnel, 0.0f, 1.0f), gloss, fresnelMap});
        diffuseShare *= glm::vec3(1.0f) - glm::clamp(fresnel, 0.0f, 1.0f);
        fresnel = glm::vec3(0.0f);
        trace += " specular(metal)";
    }
    if (metal > 0.0f && !merged.empty()) {
        // A metal share under merged layers joins them (the diffuse color is the metal's).
        merged.push_back({through * metal * diffuseColor * diffuseMean, gloss, p.hasImage("diffuse") ? "diffuse" : nullptr});
        diffuseShare *= 1.0f - metal;
        metal = 0.0f;
        trace += " metal";
    }

    if (merged.empty()) {
        // --- Nothing to merge: the base maps over as it is. --------------------------------------
        m.baseColor = glm::clamp(diffuseColor * diffuseShare, 0.0f, 1.0f);
        m.metalness = metal;
        m.roughness = gloss.roughness;
        setDielectric(m, fresnel, refraction > 0.0f);
        p.put(MaterialMap::BaseColor, "diffuse");
        p.put(MaterialMap::Metalness, "Metallic Weight");
        if (gloss.map != nullptr) {
            p.put(MaterialMap::Roughness, gloss.map, gloss.inverted);
        }
        p.put(MaterialMap::SpecularMask, p.hasImage("Glossy Layered Weight") ? "Glossy Layered Weight" : "Glossy Reflectivity");
        if (fresnelMap != nullptr) {
            p.put(MaterialMap::SpecularColor, fresnelMap);
        }
        result.reading = mode == 0 ? "metallic/roughness" : mode == 1 ? "specular/glossiness" : "weighted, diffuse alone";
    } else {
        // --- The merge: one specular color over what is left of the diffuse. ---------------------
        glm::vec3        specular(0.0f);
        float            weightSum = 0.0f, roughnessSum = 0.0f;
        const Reflector* dominant = &merged.front();
        for (const Reflector& layer : merged) {
            specular += layer.reflectance;
            const float weight = luminance(layer.reflectance);
            weightSum += weight;
            roughnessSum += weight * layer.gloss.roughness;
            if (weight > luminance(dominant->reflectance)) {
                dominant = &layer;
            }
        }
        if (const float weight = luminance(through * fresnel); weight > 0.0f) {
            specular += through * fresnel;
            weightSum += weight;
            roughnessSum += weight * gloss.roughness;
        }
        specular = glm::clamp(specular, 0.0f, 1.0f);
        const float     roughness = weightSum > 1.0e-5f ? roughnessSum / weightSum : gloss.roughness;
        const glm::vec3 albedo = glm::clamp(diffuseShare * diffuseColor * diffuseMean, 0.0f, 1.0f);
        m.roughness = roughness;
        if (dominant->gloss.map != nullptr) {
            p.put(MaterialMap::Roughness, dominant->gloss.map, dominant->gloss.inverted);
        }
        if (peakOf(specular) <= kDielectricCeiling && saturationOf(specular) < kCoatTintSaturation) {
            // All of it together reflects no more than a dielectric can, and without a color: the
            // surface stays a dielectric - its reflection white, as the layers' was - with the
            // index (or the weight) that reflects that much.
            m.baseColor = glm::clamp(diffuseShare * diffuseColor, 0.0f, 1.0f);
            m.metalness = 0.0f;
            setDielectric(m, specular, refraction > 0.0f);
            p.put(MaterialMap::BaseColor, "diffuse");
            result.reading = "layers as a dielectric";
        } else {
            // A strong or colored reflection: base color + metalness is what holds one.
            const MetallicRoughness solved = convertSpecGloss(albedo, specular, 1.0f - roughness);
            m.metalness = solved.metalness;
            m.specular = 1.0f;
            if (refraction <= 0.0f) {
                m.ior = 1.5f;
            }
            if (solved.metalness >= 0.5f) {
                // Mostly metal: the reflection's color is the surface's, and so is its map.
                if (dominant->colorMap != nullptr) {
                    m.baseColor = glm::clamp(solved.baseColor / glm::max(p.imageMean(dominant->colorMap), glm::vec3(1.0e-3f)), 0.0f, 1.0f);
                    p.put(MaterialMap::BaseColor, dominant->colorMap);
                } else {
                    m.baseColor = solved.baseColor;
                    if (p.hasImage("diffuse") && peakOf(albedo) > 0.02f) {
                        p.note("Its color map lies under a reflective layer that covers most of it; the map was left out.");
                    }
                }
            } else {
                m.baseColor = glm::clamp(solved.baseColor / glm::max(diffuseMean, glm::vec3(1.0e-3f)), 0.0f, 1.0f);
                p.put(MaterialMap::BaseColor, "diffuse");
            }
            result.reading = "merged";
            p.note("It reflects through layers PoseStudio's material has no equal for (a colored or strong top coat, metallic flakes, "
                   "or a weighted glossy layer); they were merged into the base color, Metallic and Roughness.");
        }
    }
    result.reading += trace;

    // --- The rest of the surface ----------------------------------------------------------------
    m.diffuseRoughness = clamp01(p.num("Diffuse Roughness", 0.0f));
    p.put(MaterialMap::DiffuseRoughness, "Diffuse Roughness");
    m.anisotropy = clamp01(p.num("Glossy Anisotropy", 0.0f));
    m.anisotropyRotation = clamp01(p.num("Glossy Anisotropy Rotations", 0.0f)) * 360.0f;
    p.put(MaterialMap::Anisotropy, "Glossy Anisotropy");
    p.put(MaterialMap::AnisotropyRotation, "Glossy Anisotropy Rotations");
    readVolume(p, m, refraction);
    readSheenAndFilm(p, m);
    readEmission(p, m);
    readRelief(p, m);
    if (!clearCoat) {
        m.topCoat = 0.0f;
    }
    if (p.num("Dual Lobe Specular Weight", 0.0f) > 0.0f) {
        p.note("Its second specular lobe is not carried.");
    }
    if (p.num("Diffuse Overlay Weight", 0.0f) > 0.0f) {
        p.note("Its diffuse overlay layer is not carried.");
    }
    // The dual lobe follows the single roughness (a preset is no skin).
    m.lobe1Roughness = m.roughness;
    m.lobe2Roughness = m.roughness;
    m.lobeRatio = 1.0f;
    result.params = m;
}

// --- A partial preset --------------------------------------------------------------------------

/// Applies ONE named setting of a partial preset onto @p m. False where the setting is none
/// the material holds as one value or one map.
bool applyPartial(const Preset& p, const std::string& id, const Setting& setting, MaterialParams& m, ShaderPresetMaterial& result) {
    const char* const key = id.c_str();
    const bool        valued = setting.named;
    // The setting's map into a slot, as the preset says it: another image, or none.
    const auto slot = [&](MaterialMap map, bool inverted = false) {
        if (setting.imageNamed) {
            result.textures.push_back({map, setting.image, inverted && !setting.image.empty()});
        }
    };
    const auto is = [&id](const char* name) { return id == name; };

    if (is("Horizontal Tiles")) { if (valued) { m.uvTiling.x = p.num(key, 1.0f); } return true; }
    if (is("Vertical Tiles")) { if (valued) { m.uvTiling.y = p.num(key, 1.0f); } return true; }
    if (is("Horizontal Offset")) { if (valued) { m.uvOffset.x = p.num(key, 0.0f); } return true; }
    if (is("Vertical Offset")) { if (valued) { m.uvOffset.y = p.num(key, 0.0f); } return true; }
    if (is("diffuse")) { if (valued) { m.baseColor = p.color(key, m.baseColor); } slot(MaterialMap::BaseColor); return true; }
    if (is("Normal Map")) { if (valued) { m.normalStrength = std::clamp(p.num(key, 1.0f), 0.0f, 7.9f); } slot(MaterialMap::Normal); return true; }
    if (is("Bump Strength")) { if (valued) { m.bumpStrength = std::clamp(p.num(key, 1.0f), 0.0f, 7.9f); } slot(MaterialMap::Bump); return true; }
    if (is("Cutout Opacity")) { if (valued) { m.opacity = clamp01(p.num(key, 1.0f)); } slot(MaterialMap::Opacity); return true; }
    if (is("Metallic Weight")) { if (valued) { m.metalness = clamp01(p.num(key, 0.0f)); } slot(MaterialMap::Metalness); return true; }
    if (is("Glossy Roughness")) {
        if (valued) { m.setRoughness(roughnessOf(p, p.num(key, 0.5f))); }
        slot(MaterialMap::Roughness);
        return true;
    }
    if (is("Glossiness")) {
        if (valued) { m.setRoughness(setting.image.empty() ? clamp01(1.0f - p.num(key, 1.0f)) : 1.0f); }
        slot(MaterialMap::Roughness, /*inverted=*/true);
        return true;
    }
    if (is("Glossy Color")) { if (valued) { m.specularColor = p.color(key, m.specularColor); } slot(MaterialMap::SpecularColor); return true; }
    if (is("Glossy Anisotropy")) { if (valued) { m.anisotropy = clamp01(p.num(key, 0.0f)); } slot(MaterialMap::Anisotropy); return true; }
    if (is("Glossy Anisotropy Rotations")) {
        if (valued) { m.anisotropyRotation = clamp01(p.num(key, 0.0f)) * 360.0f; }
        slot(MaterialMap::AnisotropyRotation);
        return true;
    }
    if (is("Diffuse Roughness")) { if (valued) { m.diffuseRoughness = clamp01(p.num(key, 0.0f)); } slot(MaterialMap::DiffuseRoughness); return true; }
    if (is("Translucency Weight")) { if (valued) { m.translucency = clamp01(p.num(key, 0.0f)); } slot(MaterialMap::TranslucencyWeight); return true; }
    if (is("Translucency Color")) { if (valued) { m.subsurfaceColor = p.color(key, m.subsurfaceColor); } slot(MaterialMap::Translucency); return true; }
    if (is("Top Coat Weight")) { if (valued) { m.topCoat = clamp01(p.num(key, 0.0f)); } slot(MaterialMap::CoatWeight); return true; }
    if (is("Top Coat Roughness")) { if (valued) { m.topCoatRoughness = roughnessOf(p, p.num(key, 0.0f)); } slot(MaterialMap::CoatRoughness); return true; }
    if (is("Top Coat Glossiness")) {
        if (valued) { m.topCoatRoughness = setting.image.empty() ? clamp01(1.0f - p.num(key, 1.0f)) : 1.0f; }
        slot(MaterialMap::CoatRoughness, /*inverted=*/true);
        return true;
    }
    if (is("Top Coat Anisotropy")) { if (valued) { m.coatAnisotropy = clamp01(p.num(key, 0.0f)); } slot(MaterialMap::CoatAnisotropy); return true; }
    if (is("Top Coat Rotations")) {
        if (valued) { m.coatAnisotropyRotation = clamp01(p.num(key, 0.0f)) * 360.0f; }
        slot(MaterialMap::CoatAnisotropyRotation);
        return true;
    }
    if (is("Top Coat Bump")) { if (valued) { m.coatBumpStrength = std::clamp(p.num(key, 1.0f), 0.0f, 7.9f); } slot(MaterialMap::CoatBump); return true; }
    if (is("Top Coat Color")) {
        p.note("Its top coat color is not carried: PoseStudio's coat reflects white.");
        return true;
    }
    if (is("Refraction Weight")) { if (valued) { m.transmission = clamp01(p.num(key, 0.0f)); } slot(MaterialMap::TransmissionWeight); return true; }
    if (is("Refraction Index")) { if (valued) { m.ior = std::clamp(p.num(key, 1.5f), 1.0f, 3.0f); } return true; }
    if (is("Abbe")) { if (valued) { m.dispersion = p.num(key, 0.0f) > 0.0f ? clamp01(20.0f / p.num(key, 0.0f)) : 0.0f; } return true; }
    if (is("Thin Walled")) { if (valued) { m.thinWalled = p.flag(key, false); } return true; }
    if (is("Refraction Color") || is("Transmitted Color")) {
        if (valued) { m.transmissionColor = p.color(key, m.transmissionColor); }
        slot(MaterialMap::TransmissionColor);
        return true;
    }
    if (is("Backscattering Weight")) { if (valued) { m.sheen = clamp01(p.num(key, 0.0f)) * kBackscatterSheen; } slot(MaterialMap::SheenWeight); return true; }
    if (is("Backscattering Color")) { if (valued) { m.sheenColor = p.color(key, m.sheenColor); } slot(MaterialMap::SheenColor); return true; }
    if (is("Backscattering Roughness")) { if (valued) { m.sheenRoughness = roughnessOf(p, p.num(key, 0.5f)); } slot(MaterialMap::SheenRoughness); return true; }
    if (is("Displacement Strength") || is("Minimum Displacement") || is("Maximum Displacement") || is("SubD Displacement Level")) {
        // The four say one displacement together; the ones the preset leaves out are the shader's own.
        const float range = p.num("Maximum Displacement", 0.1f) - p.num("Minimum Displacement", -0.1f);
        m.displacementStrength = std::clamp(range * p.num("Displacement Strength", 1.0f), -100.0f, 100.0f);
        m.displacementMidLevel = std::abs(range) > 1.0e-6f ? clamp01(-p.num("Minimum Displacement", -0.1f) / range) : 0.5f;
        m.displacementSubdiv = std::clamp(std::round(p.num("SubD Displacement Level", 0.0f)), 0.0f, 5.0f);
        if (is("Displacement Strength")) {
            slot(MaterialMap::Displacement);
        }
        return true;
    }
    return false;
}

bool readPartial(const Preset& p, const MaterialParams& standing, ShaderPresetMaterial& result, std::string& error) {
    MaterialParams           m = standing;
    std::vector<std::string> unknown;
    int                      applied = 0;
    // (In the settings' name order, so two runs read — and report — alike.)
    std::vector<const std::string*> names;
    for (const auto& [id, setting] : p.settings) {
        if ((setting.named || setting.imageNamed) && !isBookkeeping(id)) {
            names.push_back(&id);
        }
    }
    std::sort(names.begin(), names.end(), [](const std::string* a, const std::string* b) { return *a < *b; });
    for (const std::string* id : names) {
        if (applyPartial(p, *id, p.settings.at(*id), m, result)) {
            ++applied;
        } else {
            unknown.push_back(*id);
        }
    }
    if (!unknown.empty()) {
        std::string list;
        for (std::size_t i = 0; i < unknown.size() && i < 4; ++i) {
            list += (i ? ", " : "") + unknown[i];
        }
        if (unknown.size() > 4) {
            list += ", and " + std::to_string(unknown.size() - 4) + " more";
        }
        error = "This preset changes part of a surface, and some of what it changes has no equal in a PoseStudio material (" + list +
                "). It can only be used with the shader it was made for.";
        return false;
    }
    if (applied == 0) {
        error = "This preset changes nothing of a surface PoseStudio's material holds.";
        return false;
    }
    result.partial = true;
    result.params = m;
    result.reading = "partial";
    return true;
}

/// The document's bytes as text: inflated when gzip-compressed.
std::string textOf(const std::vector<std::uint8_t>& bytes) {
    if (isGzip(bytes)) {
        const std::vector<std::uint8_t> plain = gunzip(bytes.data(), bytes.size());
        return std::string(plain.begin(), plain.end());
    }
    return std::string(bytes.begin(), bytes.end());
}

} // namespace

bool isShaderPreset(const std::vector<std::uint8_t>& bytes) {
    try {
        // The asset's type is said in the document's first lines.
        const std::string text = textOf(bytes);
        return text.substr(0, 4096).find("\"preset_shader\"") != std::string::npos;
    } catch (const std::exception&) {
        return false;
    }
}

bool isShaderPresetFile(const std::string& path) {
    try {
        return isShaderPreset(readFileBytes(path));
    } catch (const std::exception&) {
        return false;
    }
}

bool readShaderPreset(const std::vector<std::uint8_t>& bytes, const MaterialParams& standing, ShaderPresetMaterial& out, std::string& error,
                      const ShaderPresetMapMean& mapMean) {
    out = ShaderPresetMaterial();
    error.clear();
    nlohmann::json root;
    try {
        std::string text = textOf(bytes);
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF) {
            text.erase(0, 3); // a byte-order mark
        }
        root = nlohmann::json::parse(text);
    } catch (const std::exception& e) {
        error = std::string("The file is not a preset this version can read: ") + e.what();
        return false;
    }
    if (!root.is_object()) {
        error = "The file is not a preset this version can read.";
        return false;
    }
    const std::string type = root.contains("asset_info") ? stringOf(root["asset_info"], "type") : std::string();
    if (type != "preset_shader") {
        if (type == "preset_material" || type == "preset_hierarchical_material") {
            error = "This is a material preset made for one object's own named surfaces, not a shader preset. Only a shader preset - "
                    "one surface, for whatever it is applied to - can be put on a zone.";
        } else {
            error = "This file is not a shader preset" + (type.empty() ? std::string(".") : " (it is a \"" + type + "\" file).");
        }
        return false;
    }

    // The settings: the shader's definition, the scene material's own channels, the tracks.
    const nlohmann::json  none = nlohmann::json::array();
    const nlohmann::json& images = root.contains("image_library") ? root["image_library"] : none;
    const nlohmann::json& library = root.contains("material_library") ? root["material_library"] : none;
    const nlohmann::json& scene = root.contains("scene") ? root["scene"] : none;
    const nlohmann::json& materials = scene.is_object() && scene.contains("materials") ? scene["materials"] : none;
    Preset                preset;
    preset.mapMean = &mapMean;
    preset.out = &out;
    try {
        const nlohmann::json* sceneMaterial = materials.is_array() && !materials.empty() ? &materials[0] : nullptr;
        // The definition the scene material extends ("#id"), else the library's first.
        const nlohmann::json* definition = nullptr;
        if (library.is_array() && !library.empty()) {
            definition = &library[0];
            const std::string wanted = sceneMaterial != nullptr ? referencedId(stringOf(*sceneMaterial, "url")) : std::string();
            for (const nlohmann::json& candidate : library) {
                if (!wanted.empty() && stringOf(candidate, "id") == wanted) {
                    definition = &candidate;
                    break;
                }
            }
        }
        if (definition != nullptr) {
            absorbMaterial(*definition, images, preset.settings);
        }
        if (sceneMaterial != nullptr) {
            absorbMaterial(*sceneMaterial, images, preset.settings);
        }
        absorbTracks(scene.is_object() && scene.contains("animations") ? scene["animations"] : none, images, preset.settings);
    } catch (const std::exception& e) {
        error = std::string("The preset's settings could not be read: ") + e.what();
        return false;
    }

    for (const auto& [id, setting] : preset.settings) {
        if ((setting.named || setting.imageNamed) && !isBookkeeping(id)) {
            ++out.settings;
        }
    }
    if (out.settings == 0) {
        error = "This preset sets nothing of a surface.";
        return false;
    }
    try {
        // (The standard shader is the one with a base mixing mode.)
        const char* const otherShader =
            "This preset is made for a shader PoseStudio does not read - a custom shader, or an older renderer's surface. "
            "Its look is in that shader's own code, not in values that could be carried over. Presets of the standard "
            "physically-based surface shader are the ones that can be applied.";
        const Setting* mixing = preset.find("Base Mixing");
        const bool     rebases = mixing != nullptr && mixing->named; // (it says what the base settings MEAN: a whole surface, however short)
        if (out.settings < kFullPresetSettings && !rebases) {
            if (!readPartial(preset, standing, out, error)) {
                out = ShaderPresetMaterial();
                return false;
            }
            return true;
        }
        if (!preset.has("Base Mixing")) {
            error = otherShader;
            out = ShaderPresetMaterial();
            return false;
        }
        readFull(preset, out);
    } catch (const std::exception& e) {
        error = std::string("The preset's settings could not be read: ") + e.what();
        out = ShaderPresetMaterial();
        return false;
    }
    return true;
}

} // namespace pose
