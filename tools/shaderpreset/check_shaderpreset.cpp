// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file check_shaderpreset.cpp
 * @brief The shader-preset reader on presets of its own (CTest `shader-preset-check`), and a
 *        report of what REAL presets read as.
 *
 * src/viewport/scene/shaderpresetfile.h turns a content library's shader preset (a `.duf`
 * file of asset type preset_shader, gzip-compressed or plain) into a PoseStudio material.
 * Checked here on presets written in this file, in the format's own layout — a shader
 * definition in the material library, the preset's values as tracks on "the selection's
 * materials": the three base mixing modes; colors taken from display to linear; the top coat
 * as a clear coat and merged; metallic flakes; refraction, translucency, sheen, the thin film,
 * emission and its units; the relief and the tiling; a glossiness map read inverted; a map's
 * mean deciding metal from dielectric; partial presets read onto a standing material; and
 * what is NOT a shader preset this reader reads.
 *
 *     PoseStudioShaderPresetCheck                          the checks; exit code = failures
 *     PoseStudioShaderPresetCheck --report <file|folder>   what each preset reads as, one line
 *                                 [--root <content root>]  each, and a tally (with a content
 *                                                          root, maps are decoded for their
 *                                                          mean color, as the application does)
 *
 * No Qt, no device.
 */

#include "shaderpresetfile.h"

#include "colorcurves.h"
#include "figuredocument.h" // readFileBytes
#include "figureutils.h"    // urlDecode

#include <miniz.h>
#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using nlohmann::json;
using pose::MaterialMap;
using pose::MaterialParams;
using pose::ShaderPresetMaterial;

int failures = 0;

void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}

bool about(float a, float b, float tolerance = 2e-3f) {
    return std::fabs(a - b) <= tolerance;
}

bool about(const glm::vec3& a, float r, float g, float b, float tolerance = 2e-3f) {
    return about(a.x, r, tolerance) && about(a.y, g, tolerance) && about(a.z, b, tolerance);
}

float lin(float display) {
    return pose::colorcurves::srgbToLinear(display);
}

// --- A preset written here ---------------------------------------------------------------------

/// The standard shader's settings and their defaults, as a preset's definition carries them
/// (the ones the reader asks for; a real definition has about a hundred).
json defaults() {
    return json{{"Base Mixing", 0},
                {"Metallic Weight", 0.0},
                {"Diffuse Weight", 1.0},
                {"Diffuse Roughness", 0.0},
                {"Glossy Weight", 0.0},
                {"Glossy Layered Weight", 1.0},
                {"Glossy Color", {1.0, 1.0, 1.0}},
                {"Glossy Specular", {0.2313726, 0.2313726, 0.2313726}},
                {"Glossy Reflectivity", 0.5},
                {"Glossy Roughness", 0.5},
                {"Glossiness", 1.0},
                {"Glossy Anisotropy", 0.0},
                {"Glossy Anisotropy Rotations", 0.0},
                {"Roughness Squared", true},
                {"Share Glossy Inputs", true},
                {"Refraction Weight", 0.0},
                {"Refraction Index", 1.5},
                {"Refraction Color", {1.0, 1.0, 1.0}},
                {"Refraction Roughness", 0.0},
                {"Refraction Glossiness", 1.0},
                {"Abbe", 0.0},
                {"Thin Walled", false},
                {"Transmitted Color", {1.0, 1.0, 1.0}},
                {"Transmitted Measurement Distance", 0.1},
                {"Translucency Weight", 0.0},
                {"Translucency Color", {1.0, 1.0, 1.0}},
                {"SSS Direction", 0.0},
                {"Scattering Measurement Distance", 0.1},
                {"Backscattering Weight", 0.0},
                {"Backscattering Color", {1.0, 1.0, 1.0}},
                {"Backscattering Roughness", 0.5},
                {"Backscattering Glossiness", 0.5},
                {"Metallic Flakes Weight", 0.0},
                {"Metallic Flakes Color", {1.0, 1.0, 1.0}},
                {"Metallic Flakes Roughness", 0.0},
                {"Metallic Flakes Glossiness", 1.0},
                {"Metallic Flakes Size", 0.001},
                {"Metallic Flakes Density", 1.0},
                {"Top Coat Weight", 0.0},
                {"Top Coat Color", {1.0, 1.0, 1.0}},
                {"Top Coat Roughness", 0.0},
                {"Top Coat Glossiness", 1.0},
                {"Top Coat Layering Mode", 0},
                {"Reflectivity", 0.5},
                {"Top Coat IOR", 1.5},
                {"Top Coat Curve Normal", 0.04},
                {"Top Coat Anisotropy", 0.0},
                {"Top Coat Rotations", 0.0},
                {"Top Coat Bump", 1.0},
                {"Top Coat Thin Film", 0.0},
                {"Top Coat Thin Film IOR", 1.5},
                {"Base Thin Film", 0.0},
                {"Base Thin Film IOR", 1.5},
                {"Bump Strength", 1.0},
                {"Normal Map", 1.0},
                {"Displacement Strength", 1.0},
                {"Minimum Displacement", -0.1},
                {"Maximum Displacement", 0.1},
                {"SubD Displacement Level", 0},
                {"Cutout Opacity", 1.0},
                {"Emission Color", {0.0, 0.0, 0.0}},
                {"Emission Temperature", 6500.0},
                {"Luminance", 1500.0},
                {"Luminance Units", 0},
                {"Luminous Efficacy", 15.0},
                {"Horizontal Tiles", 1.0},
                {"Vertical Tiles", 1.0},
                {"Horizontal Offset", 0.0},
                {"Vertical Offset", 0.0},
                {"Tags", ""},
                {"Render Priority", 3}};
}

std::string encoded(const std::string& name) {
    std::string out;
    for (const char c : name) {
        if (c == ' ') {
            out += "%20";
        } else if (c == '/') {
            out += "%2F";
        } else {
            out += c;
        }
    }
    return out;
}

/// One track of a preset: a setting's value or its map.
json track(const std::string& setting, const char* leaf, const json& value) {
    const std::string path = setting == "diffuse" ? std::string("diffuse") : "extra/studio_material_channels/channels/" + encoded(setting);
    return json{{"url", "name://@selection#materials:?" + path + "/" + leaf}, {"keys", json::array({json::array({0, value})})}};
}

struct Spec {
    json                               values = json::object(); ///< Settings the preset dials (over the defaults).
    std::map<std::string, json>        images;                  ///< Setting -> image_file (a string or null), or an image-library index.
    bool                               full = true;             ///< Name every setting (a surface) or only `values` / `images` (a part).
    bool                               definition = true;       ///< Carry the shader's definition in the material library.
    std::string                        type = "preset_shader";
    bool                               standardShader = true;   ///< false: a definition with no base mixing mode (another shader).
    json                               imageLibrary = json::array();
};

std::string presetText(const Spec& spec) {
    json all = defaults();
    if (!spec.standardShader) {
        all.erase("Base Mixing");
    }
    json root;
    root["file_version"] = "0.6.0.0";
    root["asset_info"] = {{"id", "/Shader%20Presets/Test/Test.duf"}, {"type", spec.type}};
    if (!spec.imageLibrary.empty()) {
        root["image_library"] = spec.imageLibrary;
    }
    if (spec.definition) {
        json channels = json::array();
        for (auto it = all.begin(); it != all.end(); ++it) {
            channels.push_back({{"channel", {{"id", it.key()}, {"type", "float"}, {"value", it.value()}}}});
        }
        json material;
        material["id"] = "sphere";
        material["diffuse"] = {{"channel", {{"id", "diffuse"}, {"type", "float_color"}, {"value", {0.7529412, 0.7529412, 0.7529412}}}}};
        material["extra"] = json::array({json{{"type", "studio_material_channels"}, {"channels", channels}}});
        root["material_library"] = json::array({material});
    }
    json tracks = json::array();
    if (spec.full) {
        json dialed = all;
        dialed["diffuse"] = {0.7529412, 0.7529412, 0.7529412};
        for (auto it = spec.values.begin(); it != spec.values.end(); ++it) {
            dialed[it.key()] = it.value();
        }
        for (auto it = dialed.begin(); it != dialed.end(); ++it) {
            tracks.push_back(track(it.key(), "value", it.value()));
            const auto image = spec.images.find(it.key());
            // (A file goes out as image_file; an entry of the image library — by index or "#id" — as image.)
            const bool library = image != spec.images.end() &&
                                 (image->second.is_number() || (image->second.is_string() && image->second.get<std::string>().rfind("#", 0) == 0));
            tracks.push_back(track(it.key(), "image_file", image != spec.images.end() && !library ? image->second : json()));
            if (library) {
                tracks.push_back(track(it.key(), "image", image->second));
            }
        }
    } else {
        for (auto it = spec.values.begin(); it != spec.values.end(); ++it) {
            tracks.push_back(track(it.key(), "value", it.value()));
        }
        for (const auto& [setting, image] : spec.images) {
            tracks.push_back(track(setting, image.is_number() ? "image" : "image_file", image));
        }
    }
    root["scene"] = {{"materials", json::array({json{{"url", "#sphere"}}})}, {"animations", tracks}};
    return root.dump(1);
}

std::vector<std::uint8_t> bytesOf(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

/// @p text as a gzip stream (a preset is usually stored compressed).
std::vector<std::uint8_t> gzipped(const std::string& text) {
    std::size_t deflatedSize = 0;
    void*       deflated = tdefl_compress_mem_to_heap(text.data(), text.size(), &deflatedSize, 6);
    std::vector<std::uint8_t> out = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
    out.insert(out.end(), static_cast<std::uint8_t*>(deflated), static_cast<std::uint8_t*>(deflated) + deflatedSize);
    mz_free(deflated);
    const std::uint32_t crc = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(text.data()), text.size()));
    const std::uint32_t size = static_cast<std::uint32_t>(text.size());
    for (const std::uint32_t word : {crc, size}) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>((word >> (8 * i)) & 0xff));
        }
    }
    return out;
}

struct Read {
    bool                 ok = false;
    ShaderPresetMaterial material;
    std::string          error;
};

Read read(const Spec& spec, const MaterialParams& standing = MaterialParams(), const pose::ShaderPresetMapMean& mean = {}) {
    Read r;
    r.ok = pose::readShaderPreset(bytesOf(presetText(spec)), standing, r.material, r.error, mean);
    return r;
}

const pose::ShaderPresetTexture* map(const ShaderPresetMaterial& m, MaterialMap slot) {
    for (const pose::ShaderPresetTexture& t : m.textures) {
        if (t.slot == slot) {
            return &t;
        }
    }
    return nullptr;
}

/// Whether the surface was read by merging its reflective layers into base color + metalness.
bool merged(const ShaderPresetMaterial& m) {
    return m.reading.rfind("merged", 0) == 0;
}

bool said(const ShaderPresetMaterial& m, const char* word) {
    for (const std::string& note : m.notes) {
        if (note.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// --- The checks --------------------------------------------------------------------------------

void checkDocument() {
    Spec        spec;
    const std::string text = presetText(spec);
    check(pose::isShaderPreset(bytesOf(text)), "a shader preset is recognised");
    check(pose::isShaderPreset(gzipped(text)), "… and a gzip-compressed one");
    Spec other;
    other.type = "character";
    check(!pose::isShaderPreset(bytesOf(presetText(other))), "another kind of preset is not");
    check(!pose::isShaderPreset(bytesOf("not a document at all")), "nor is a file that is no document");

    ShaderPresetMaterial plain, packed;
    std::string          error;
    const bool           a = pose::readShaderPreset(bytesOf(text), MaterialParams(), plain, error);
    const bool           b = pose::readShaderPreset(gzipped(text), MaterialParams(), packed, error);
    check(a && b && plain.params.sameValues(packed.params) && plain.settings == packed.settings, "a compressed preset reads as the plain one does");
    check(a && !plain.partial && plain.settings >= 40, "a preset that names the whole shader is a full surface");
    check(a && plain.settings == static_cast<int>(defaults().size()) + 1 - 2, "bookkeeping settings are not counted");

    Read r = read(other);
    check(!r.ok && r.error.find("not a shader preset") != std::string::npos, "another kind of preset is refused, and said to be one");
    Spec zones;
    zones.type = "preset_material";
    r = read(zones);
    check(!r.ok && r.error.find("named surfaces") != std::string::npos, "a material preset of one object's surfaces is refused as that");
    ShaderPresetMaterial out;
    check(!pose::readShaderPreset(bytesOf("{ not json"), MaterialParams(), out, error) && !error.empty(), "broken text is refused with a reason");
    Spec foreign;
    foreign.standardShader = false;
    r = read(foreign);
    check(!r.ok && r.error.find("shader PoseStudio does not read") != std::string::npos, "a preset of another shader is refused as that");
    Spec empty;
    empty.full = false;
    r = read(empty);
    check(!r.ok, "a preset that sets nothing is refused");
}

void checkMetallicRoughness() {
    Spec spec;
    spec.values = {{"diffuse", {0.7529412, 0.2, 0.2}}, {"Metallic Weight", 0.25}, {"Glossy Roughness", 0.4}};
    Read r = read(spec);
    const MaterialParams& m = r.material.params;
    check(r.ok && r.material.reading == "metallic/roughness", "metallicity / roughness is read as the material's own");
    check(about(m.baseColor, lin(0.7529412f), lin(0.2f), lin(0.2f)), "the base color comes out linear");
    check(about(m.metalness, 0.25f) && about(m.roughness, 0.4f), "metallic weight and roughness carry over");
    check(about(m.specular, 1.0f) && about(m.ior, 1.5f), "a reflectivity of a half is the standard dielectric");
    check(about(m.translucency, 0.0f) && about(m.topCoat, 0.0f) && about(m.transmission, 0.0f) && about(m.emission, 0.0f),
          "nothing the preset leaves off is switched on");
    check(about(m.lobeRatio, 1.0f), "the surface has one specular lobe");

    spec.values = {{"Glossy Reflectivity", 0.25}, {"Glossy Layered Weight", 0.5}, {"Glossy Color", {1.0, 0.5, 0.5}}};
    r = read(spec);
    check(r.ok && about(r.material.params.specular, 0.25f) && about(r.material.params.specularColor, 1.0f, lin(0.5f), lin(0.5f)),
          "a weaker glossy layer is a lower specular weight, its color the tint");
    spec.values = {{"Glossy Reflectivity", 1.0}};
    r = read(spec);
    check(r.ok && about(r.material.params.specular, 1.0f) && about(r.material.params.ior, 1.789f, 0.01f),
          "a reflectivity above a half raises the index");
    spec.values = {{"Glossy Layered Weight", 0.0}};
    r = read(spec);
    check(r.ok && about(r.material.params.specular, 0.0f), "a glossy layer switched off is a matte surface");
    spec.values = {{"Roughness Squared", false}, {"Glossy Roughness", 0.25}};
    r = read(spec);
    check(r.ok && about(r.material.params.roughness, 0.5f), "a roughness the shader does not square is the squared value");

    spec.values = {{"Normal Map", 2.5}, {"Bump Strength", 3.0}, {"Horizontal Tiles", 4.0}, {"Vertical Tiles", 2.0}, {"Horizontal Offset", 0.25},
                   {"Cutout Opacity", 0.8}, {"Glossy Roughness", 1.0}};
    spec.images = {{"diffuse", "/Runtime/Textures/T/color.jpg"},  {"Normal Map", "/Runtime/Textures/T/normal.jpg"},
                   {"Bump Strength", "/Runtime/Textures/T/bump.jpg"}, {"Glossy Roughness", "/Runtime/Textures/T/rough.jpg"},
                   {"Metallic Weight", "/Runtime/Textures/T/metal.jpg"}, {"Cutout Opacity", "/Runtime/Textures/T/cut.jpg"},
                   {"Glossy Layered Weight", "/Runtime/Textures/T/mask.jpg"}};
    r = read(spec);
    const ShaderPresetMaterial& t = r.material;
    check(r.ok && map(t, MaterialMap::BaseColor) && map(t, MaterialMap::BaseColor)->uri == "/Runtime/Textures/T/color.jpg", "the color map goes to Base Color");
    check(r.ok && map(t, MaterialMap::Normal) && about(t.params.normalStrength, 2.5f) && map(t, MaterialMap::Bump) && about(t.params.bumpStrength, 3.0f),
          "the normal and bump maps carry their strengths");
    check(r.ok && map(t, MaterialMap::Roughness) && !map(t, MaterialMap::Roughness)->inverted && map(t, MaterialMap::Metalness) &&
              map(t, MaterialMap::Opacity) && map(t, MaterialMap::SpecularMask),
          "the roughness, metallic, cutout and glossy-weight maps find their slots");
    check(r.ok && about(t.params.uvTiling.x, 4.0f) && about(t.params.uvTiling.y, 2.0f) && about(t.params.uvOffset.x, 0.25f) && about(t.params.opacity, 0.8f),
          "tiling, offset and opacity carry over");
    check(r.ok && t.notes.empty(), "a plain surface loses nothing");
}

void checkSpecularGlossiness() {
    Spec spec;
    spec.values = {{"Base Mixing", 1}, {"Glossiness", 0.7}};
    Read r = read(spec);
    check(r.ok && r.material.reading == "specular/glossiness" && about(r.material.params.metalness, 0.0f) && about(r.material.params.roughness, 0.3f),
          "specular / glossiness with the default specular is a dielectric, its roughness one less the glossiness");
    check(r.ok && about(r.material.params.ior, 1.525f, 0.02f) && about(r.material.params.specular, 1.0f), "… reflecting what the default specular says");

    spec.values = {{"Base Mixing", 1}, {"diffuse", {0.0, 0.0, 0.0}}, {"Glossy Specular", {1.0, 0.78, 0.34}}, {"Glossiness", 0.9}};
    r = read(spec);
    check(r.ok && merged(r.material) && r.material.params.metalness > 0.9f, "a bright specular color over black is a metal");
    check(r.ok && about(r.material.params.baseColor, 1.0f, lin(0.78f), lin(0.34f), 0.03f), "… in that color");
    check(r.ok && about(r.material.params.roughness, 0.1f) && said(r.material, "merged"), "… at the glossiness's roughness, and the merge is said");

    spec.values = {{"Base Mixing", 1}};
    spec.images = {{"Glossiness", "/Runtime/Textures/T/gloss.jpg"}};
    r = read(spec);
    check(r.ok && map(r.material, MaterialMap::Roughness) && map(r.material, MaterialMap::Roughness)->inverted && about(r.material.params.roughness, 1.0f),
          "a glossiness map is the roughness map read inverted");

    // "White × a map": only the map's mean says whether it is a metal.
    spec.values = {{"Base Mixing", 1}, {"diffuse", {0.0, 0.0, 0.0}}, {"Glossy Specular", {1.0, 1.0, 1.0}}};
    spec.images = {{"Glossy Specular", "/Runtime/Textures/T/spec.jpg"}};
    const auto dark = [](const std::string&, glm::vec3& mean) {
        mean = glm::vec3(0.04f);
        return true;
    };
    const auto bright = [](const std::string&, glm::vec3& mean) {
        mean = glm::vec3(0.9f, 0.7f, 0.3f);
        return true;
    };
    r = read(spec, MaterialParams(), dark);
    check(r.ok && about(r.material.params.metalness, 0.0f) && map(r.material, MaterialMap::SpecularColor), "a dark specular map is a dielectric's");
    r = read(spec, MaterialParams(), bright);
    check(r.ok && r.material.params.metalness > 0.9f && map(r.material, MaterialMap::BaseColor) &&
              map(r.material, MaterialMap::BaseColor)->uri == "/Runtime/Textures/T/spec.jpg" && about(r.material.params.baseColor, 1.0f, 1.0f, 1.0f, 0.05f),
          "a bright one is a metal's, and becomes its color map");
}

void checkWeighted() {
    Spec spec;
    spec.values = {{"Base Mixing", 2}, {"Diffuse Weight", 1.0}, {"Glossy Weight", 0.0}, {"diffuse", {0.5, 0.5, 0.5}}};
    Read r = read(spec);
    check(r.ok && about(r.material.params.metalness, 0.0f) && about(r.material.params.specular, 0.0f) && about(r.material.params.baseColor.x, lin(0.5f)),
          "weighted with no glossy weight is the diffuse alone");
    spec.values = {{"Base Mixing", 2}, {"Diffuse Weight", 0.0}, {"Glossy Weight", 1.0}, {"Glossy Color", {1.0, 0.78, 0.34}}, {"Glossy Roughness", 0.2}};
    r = read(spec);
    check(r.ok && r.material.params.metalness > 0.9f && about(r.material.params.baseColor, 1.0f, lin(0.78f), lin(0.34f), 0.03f) &&
              about(r.material.params.roughness, 0.2f),
          "weighted with the glossy layer alone is a metal in its color");
    spec.values = {{"Base Mixing", 2}, {"Diffuse Weight", 1.0}, {"Glossy Weight", 0.25}, {"Glossy Color", {0.5, 0.5, 0.5}}, {"diffuse", {0.8, 0.2, 0.2}}};
    spec.images = {{"diffuse", "/Runtime/Textures/T/color.jpg"}};
    r = read(spec);
    check(r.ok && about(r.material.params.metalness, 0.0f) && map(r.material, MaterialMap::BaseColor) &&
              map(r.material, MaterialMap::BaseColor)->uri == "/Runtime/Textures/T/color.jpg" && !merged(r.material) && r.material.notes.empty(),
          "a light glossy weight over a colored diffuse stays a dielectric with its color map, and nothing is said");
    check(r.ok && about(r.material.params.specular, 1.0f) && about(r.material.params.ior, 1.523f, 0.02f) &&
              about(r.material.params.baseColor.x, lin(0.8f) * 0.8f, 0.01f),
          "... reflecting what that weight reflects, the diffuse keeping its share");
    spec.images.clear();
    spec.values = {{"Base Mixing", 2}, {"Diffuse Weight", 1.0}, {"Glossy Weight", 0.05}, {"Glossy Color", {0.5, 0.5, 0.5}}};
    r = read(spec);
    check(r.ok && about(r.material.params.metalness, 0.0f) && r.material.params.specular < 0.3f && about(r.material.params.ior, 1.5f),
          "a faint glossy weight is a faint specular, not the standard one");
    spec.values = {{"Base Mixing", 2}, {"Diffuse Weight", 0.0}, {"Glossy Weight", 1.0}, {"Refraction Weight", 1.0}, {"Refraction Index", 1.46},
                   {"Glossy Roughness", 0.0}, {"Transmitted Color", {0.0, 0.0, 0.0}}};
    r = read(spec);
    check(r.ok && about(r.material.params.metalness, 0.0f) && about(r.material.params.transmission, 1.0f) && about(r.material.params.ior, 1.46f) &&
              about(r.material.params.specular, 1.0f) && about(r.material.params.roughness, 0.0f),
          "a refracting surface is glass in every mode: its glossy layer is the glass's own reflection");
    check(r.ok && about(r.material.params.transmissionColor, 1.0f, 1.0f, 1.0f) && about(r.material.params.transmissionDepth, 0.0f),
          "... and a black transmitted color absorbs nothing");
}

void checkCoatAndFlakes() {
    Spec spec;
    spec.values = {{"Top Coat Weight", 1.0}, {"Top Coat Layering Mode", 2}, {"Top Coat IOR", 1.6}, {"Top Coat Glossiness", 0.9}};
    Read r = read(spec);
    check(r.ok && about(r.material.params.topCoat, 1.0f) && about(r.material.params.coatIor, 1.6f, 0.01f) && about(r.material.params.topCoatRoughness, 0.1f) &&
              r.material.reading == "metallic/roughness",
          "a top coat with an index is a clear coat at that index");
    spec.values = {{"Top Coat Weight", 0.5}, {"Top Coat Layering Mode", 0}, {"Reflectivity", 0.5}, {"Top Coat Roughness", 0.3}};
    r = read(spec);
    check(r.ok && about(r.material.params.topCoat, 0.5f) && about(r.material.params.coatIor, 1.5f) && about(r.material.params.topCoatRoughness, 0.3f),
          "a top coat by reflectivity is the standard coat at its weight");
    spec.values = {{"Top Coat Weight", 0.2}, {"Top Coat Layering Mode", 1}};
    r = read(spec);
    check(r.ok && about(r.material.params.topCoat, 1.0f) && about(r.material.params.coatIor, 2.618f, 0.02f), "a lightly weighted coat becomes a coat of that reflectance");
    spec.values = {{"Top Coat Weight", 1.0}, {"Top Coat Layering Mode", 2}, {"Top Coat IOR", 300.0}, {"Top Coat Color", {1.0, 0.78, 0.34}},
                   {"diffuse", {0.0, 0.0, 0.0}}, {"Glossy Layered Weight", 0.0}, {"Top Coat Glossiness", 0.8}};
    r = read(spec);
    check(r.ok && merged(r.material) && r.material.params.metalness > 0.9f && about(r.material.params.topCoat, 0.0f) &&
              about(r.material.params.roughness, 0.2f),
          "a coat that reflects nearly everything, in a color, is merged as a metal at the coat's roughness");
    spec.values = {{"Top Coat Weight", 0.0}};
    r = read(spec);
    check(r.ok && about(r.material.params.topCoat, 0.0f), "no top coat is no coat");

    spec.values = {{"diffuse", {0.0, 0.0, 0.0}}, {"Glossy Layered Weight", 0.0}, {"Metallic Flakes Weight", 0.8}, {"Metallic Flakes Color", {0.9, 0.9, 0.9}}};
    r = read(spec);
    check(r.ok && r.material.params.metalness > 0.8f && r.material.params.roughness >= 0.3f && said(r.material, "flakes"),
          "metallic flakes become a metallic layer, never mirror-smooth, and the loss is said");
    spec.values = {{"Metallic Flakes Weight", 1.0}, {"Metallic Flakes Color", {0.03, 0.03, 0.03}}, {"Metallic Flakes Size", 0.0}};
    r = read(spec);
    check(r.ok && r.material.reading == "metallic/roughness" && about(r.material.params.baseColor.x, lin(0.7529412f)) && r.material.notes.empty(),
          "flakes of no size are no flakes");
}

void checkVolumeAndLight() {
    Spec spec;
    spec.values = {{"Refraction Weight", 1.0}, {"Refraction Index", 1.33}, {"Thin Walled", true}, {"Refraction Color", {0.5, 1.0, 0.5}}};
    Read r = read(spec);
    const MaterialParams* m = &r.material.params;
    check(r.ok && about(m->transmission, 1.0f) && about(m->ior, 1.33f) && m->thinWalled && about(m->transmissionColor, lin(0.5f), 1.0f, lin(0.5f)) &&
              about(m->transmissionDepth, 0.0f) && about(m->opacity, 1.0f),
          "a thin refracting surface transmits, tinted at the surface, and stays opaque to the cutout");
    spec.values = {{"Refraction Weight", 1.0}, {"Transmitted Color", {0.5, 0.8, 1.0}}, {"Transmitted Measurement Distance", 5.0}, {"Abbe", 40.0},
                   {"Share Glossy Inputs", false}, {"Refraction Roughness", 0.2}};
    r = read(spec);
    m = &r.material.params;
    check(r.ok && about(m->transmissionColor, lin(0.5f), lin(0.8f), 1.0f) && about(m->transmissionDepth, 0.05f) && about(m->dispersion, 0.5f) &&
              about(m->transmissionRoughness, 0.2f),
          "a solid tints by depth, disperses by its Abbe number, and keeps a transmission roughness of its own");

    spec.values = {{"Refraction Weight", 1.0}, {"SSS Amount", 1.0}, {"Transmitted Color", {0.6, 1.0, 0.7}}, {"Transmitted Measurement Distance", 20.0},
                   {"Scattering Measurement Distance", 20.0}, {"SSS Direction", 0.3}, {"Glossy Roughness", 0.2}};
    r = read(spec);
    m = &r.material.params;
    check(r.ok && about(m->transmission, 0.0f) && about(m->translucency, 1.0f) && about(m->baseColor, lin(0.6f), 1.0f, lin(0.7f)) &&
              about(m->subsurfaceRadius, 20.0f) && about(m->subsurfaceAnisotropy, 0.3f) && about(m->roughness, 0.2f),
          "a solid that scatters what it refracts is a subsurface solid in the volume's color, not glass");

    spec.values = {{"Translucency Weight", 0.4}, {"Translucency Color", {1.0, 0.5, 0.5}}, {"Backscattering Weight", 0.6}, {"Backscattering Roughness", 0.7},
                   {"Base Thin Film", 300.0}, {"Base Thin Film IOR", 1.4}};
    r = read(spec);
    m = &r.material.params;
    check(r.ok && about(m->translucency, 0.4f) && about(m->subsurfaceColor, 1.0f, lin(0.5f), lin(0.5f)), "translucency carries its weight and color");
    check(r.ok && about(m->sheen, 0.6f * 0.15f) && about(m->sheenRoughness, 0.7f), "backscattering is the sheen, at the share of it that looks alike");
    check(r.ok && about(m->thinFilm, 1.0f) && about(m->thinFilmThickness, 300.0f) && about(m->thinFilmIor, 1.4f), "a thin film's thickness switches it on");

    spec.values = {{"Emission Color", {1.0, 1.0, 1.0}}, {"Luminance", 5000.0}};
    r = read(spec);
    check(r.ok && about(r.material.params.emission, 5.0f, 0.02f) && about(r.material.params.emissionColor, 1.0f, 1.0f, 1.0f, 0.02f),
          "emission in candelas per square metre: a thousand is the engine's one");
    spec.values = {{"Emission Color", {1.0, 1.0, 1.0}}, {"Luminance", 5.0}, {"Luminance Units", 1}, {"Emission Temperature", 2700.0}};
    r = read(spec);
    check(r.ok && about(r.material.params.emission, 5.0f, 0.05f) &&
              r.material.params.emissionColor.x > r.material.params.emissionColor.z + 0.3f,
          "thousands of candelas are read as such, and a low temperature warms the color");
    spec.values = {{"Emission Color", {1.0, 1.0, 1.0}}, {"Luminance", 1600.0}, {"Luminance Units", 4}};
    r = read(spec);
    check(r.ok && r.material.params.emission > 1.0f && said(r.material, "lumens"), "a total in lumens is brought in for an assumed area, and said");
    spec.values = {{"Emission Color", {1.0, 1.0, 1.0}}, {"Luminance", 1500.0}, {"Luminance Units", 1}};
    r = read(spec);
    check(r.ok && about(r.material.params.emission, 100.0f) && said(r.material, "light source"), "a light source's emission stops at the top of the range, and that is said");
    spec.values = json::object();
    r = read(spec);
    check(r.ok && about(r.material.params.emission, 0.0f), "a black emission color is no emission");

    spec.values = {{"Displacement Strength", 2.0}, {"Minimum Displacement", -0.5}, {"Maximum Displacement", 1.5}, {"SubD Displacement Level", 3}};
    spec.images = {{"Displacement Strength", "/Runtime/Textures/T/height.jpg"}};
    r = read(spec);
    check(r.ok && about(r.material.params.displacementStrength, 4.0f) && about(r.material.params.displacementMidLevel, 0.25f) &&
              about(r.material.params.displacementSubdiv, 3.0f) && map(r.material, MaterialMap::Displacement),
          "displacement's lowest and highest become a strength and a mid level");

    Spec layered;
    layered.imageLibrary = json::array({json{{"name", "stack"}, {"map", json::array({json{{"url", "/Runtime/Textures/T/first.jpg"}}, json{{"url", "/Runtime/Textures/T/second.jpg"}}})}}});
    layered.images = {{"diffuse", 0}};
    r = read(layered);
    check(r.ok && map(r.material, MaterialMap::BaseColor) && map(r.material, MaterialMap::BaseColor)->uri == "/Runtime/Textures/T/first.jpg" && said(r.material, "layered"),
          "a map of layered images is its first image, and that is said");
    Spec byId;
    byId.imageLibrary = json::array({json{{"id", "other"}, {"map", json::array({json{{"url", "/Runtime/Textures/T/other.jpg"}}})}},
                                     json{{"id", "Knit 4B 4"}, {"map", json::array({json{{"label", "COLOR"}, {"color", {0.4, 0.1, 0.1}}},
                                                                                    json{{"url", "/Runtime/Textures/T/knit.jpg"}}})}}});
    byId.images = {{"diffuse", "#Knit%204B%204"}};
    r = read(byId);
    check(r.ok && map(r.material, MaterialMap::BaseColor) && map(r.material, MaterialMap::BaseColor)->uri == "/Runtime/Textures/T/knit.jpg",
          "an image named by its id in the image library is found, past a layer that is a flat color");
    byId.images = {{"diffuse", "#no%20such"}};
    r = read(byId);
    check(r.ok && map(r.material, MaterialMap::BaseColor) == nullptr, "an id the library does not have is no map");
}

void checkPartial() {
    MaterialParams standing;
    standing.roughness = 0.123f;
    standing.metalness = 0.75f;
    standing.baseColor = glm::vec3(0.1f, 0.2f, 0.3f);

    Spec spec;
    spec.full = false;
    spec.values = {{"Horizontal Tiles", 6.0}, {"Vertical Tiles", 3.0}, {"Horizontal Offset", 0.0}, {"Vertical Offset", 0.5}, {"Tags", "tiling"}};
    Read r = read(spec, standing);
    check(r.ok && r.material.partial && r.material.settings == 4, "a preset that names a few settings is a partial one");
    check(r.ok && about(r.material.params.uvTiling.x, 6.0f) && about(r.material.params.uvTiling.y, 3.0f) && about(r.material.params.uvOffset.y, 0.5f),
          "it sets what it names");
    check(r.ok && about(r.material.params.roughness, 0.123f) && about(r.material.params.metalness, 0.75f) && about(r.material.params.baseColor, 0.1f, 0.2f, 0.3f) &&
              r.material.textures.empty(),
          "… and leaves the rest of the standing material, and every map, alone");

    spec.values = {{"Normal Map", 1.5}};
    spec.images = {{"Normal Map", "/Runtime/Textures/T/weave.jpg"}};
    r = read(spec, standing);
    check(r.ok && r.material.partial && r.material.textures.size() == 1 && map(r.material, MaterialMap::Normal) &&
              map(r.material, MaterialMap::Normal)->uri == "/Runtime/Textures/T/weave.jpg" && about(r.material.params.normalStrength, 1.5f),
          "a partial preset of one map names that slot alone");
    spec.values = json::object();
    spec.images = {{"Bump Strength", json()}};
    r = read(spec, standing);
    check(r.ok && r.material.textures.size() == 1 && map(r.material, MaterialMap::Bump) && map(r.material, MaterialMap::Bump)->uri.empty(),
          "a partial preset that says \"no map\" empties the slot");
    spec.images.clear();
    spec.values = {{"Glossiness", 0.8}};
    r = read(spec, standing);
    check(r.ok && about(r.material.params.roughness, 0.2f), "a partial glossiness is a roughness");
    spec.values = {{"Metallic Flakes Size", 0.01}, {"Metallic Flakes Density", 0.5}};
    r = read(spec, standing);
    check(!r.ok && r.error.find("Metallic Flakes Size") != std::string::npos, "a partial preset of settings with no equal is refused, naming them");
    spec.values = {{"Base Mixing", 0}, {"Glossy Roughness", 0.3}, {"Metallic Weight", 1.0}, {"diffuse", {1.0, 0.78, 0.34}}};
    r = read(spec, standing);
    check(r.ok && !r.material.partial && about(r.material.params.metalness, 1.0f) && about(r.material.params.roughness, 0.3f) &&
              about(r.material.params.topCoat, 0.0f),
          "a short preset that sets the base mixing mode is a whole surface, not a part of the standing one");
    spec.definition = false;
    spec.values = {{"Horizontal Tiles", 2.0}, {"Vertical Tiles", 2.0}};
    r = read(spec, standing);
    check(r.ok && r.material.partial && about(r.material.params.uvTiling.x, 2.0f), "a partial preset needs no shader definition");
}

// --- The report --------------------------------------------------------------------------------

/// Mean colors of the maps under a content root, decoded once each.
struct MapMeans {
    std::string                                 root;
    std::unordered_map<std::string, glm::vec3>  cache;
    std::unordered_map<std::string, bool>       known;

    bool operator()(const std::string& uri, glm::vec3& mean) {
        if (root.empty()) {
            return false;
        }
        if (const auto it = known.find(uri); it != known.end()) {
            mean = cache[uri];
            return it->second;
        }
        const std::string path = root + pose::urlDecode(uri);
        int               w = 0, h = 0, n = 0;
        unsigned char*    pixels = stbi_load(path.c_str(), &w, &h, &n, 3);
        bool              ok = false;
        if (pixels != nullptr && w > 0 && h > 0) {
            // (Every 8th texel each way is plenty for a mean.)
            double      sum[3] = {0.0, 0.0, 0.0};
            std::size_t count = 0;
            for (int y = 0; y < h; y += 8) {
                for (int x = 0; x < w; x += 8) {
                    const unsigned char* px = pixels + (static_cast<std::size_t>(y) * w + x) * 3;
                    for (int c = 0; c < 3; ++c) {
                        sum[c] += lin(px[c] / 255.0f);
                    }
                    ++count;
                }
            }
            mean = glm::vec3(sum[0] / count, sum[1] / count, sum[2] / count);
            ok = true;
        }
        stbi_image_free(pixels);
        cache[uri] = mean;
        known[uri] = ok;
        return ok;
    }
};

int report(const std::string& target, const std::string& root) {
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    std::error_code       ec;
    if (fs::is_directory(fs::u8path(target), ec)) {
        for (fs::recursive_directory_iterator it(fs::u8path(target), fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (it->is_regular_file(ec)) {
                std::string ext = it->path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".duf") {
                    files.push_back(it->path());
                }
            }
        }
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(fs::u8path(target));
    }
    MapMeans                   means{root, {}, {}};
    std::map<std::string, int> tally;
    for (const fs::path& file : files) {
        const std::string name = fs::relative(file, fs::is_directory(fs::u8path(target), ec) ? fs::u8path(target) : file.parent_path(), ec).generic_u8string();
        std::vector<std::uint8_t> bytes;
        try {
            bytes = pose::readFileBytes(file.u8string());
        } catch (const std::exception& e) {
            std::printf("UNREADABLE | %s | %s\n", name.c_str(), e.what());
            ++tally["unreadable"];
            continue;
        }
        if (!pose::isShaderPreset(bytes)) {
            ++tally["not a shader preset"];
            continue;
        }
        ShaderPresetMaterial m;
        std::string          error;
        const bool           ok = pose::readShaderPreset(bytes, MaterialParams(), m, error, [&means](const std::string& uri, glm::vec3& mean) { return means(uri, mean); });
        if (!ok) {
            std::printf("REFUSED | %s | %.90s\n", name.c_str(), error.c_str());
            ++tally["refused: " + error.substr(0, 60)];
            continue;
        }
        const MaterialParams& p = m.params;
        std::string           maps;
        for (const pose::ShaderPresetTexture& t : m.textures) {
            maps += std::string(" ") + pose::materialMapName(t.slot) + (t.inverted ? "(inv)" : "") + (t.uri.empty() ? "=none" : "");
        }
        std::printf("%s | %s | %s n=%d | base %.2f %.2f %.2f metal %.2f rough %.2f spec %.2f ior %.2f | coat %.2f r%.2f ior %.2f | trans %.2f transl %.2f sheen %.2f emit "
                    "%.2f op %.2f tile %.1fx%.1f |%s | notes %zu\n",
                    m.partial ? "PARTIAL" : "FULL", name.c_str(), m.reading.c_str(), m.settings, p.baseColor.x, p.baseColor.y, p.baseColor.z, p.metalness,
                    p.roughness, p.specular, p.ior, p.topCoat, p.topCoatRoughness, p.coatIor, p.transmission, p.translucency, p.sheen, p.emission, p.opacity,
                    p.uvTiling.x, p.uvTiling.y, maps.c_str(), m.notes.size());
        ++tally[std::string(m.partial ? "partial" : "full: ") + (m.partial ? "" : m.reading)];
        for (const std::string& note : m.notes) {
            ++tally["note: " + note.substr(0, 50)];
        }
    }
    std::printf("\n== %zu files\n", files.size());
    for (const auto& [what, count] : tally) {
        std::printf("%6d  %s\n", count, what.c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--report") {
        std::string root;
        for (int i = 3; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--root") {
                root = argv[i + 1];
            }
        }
        return report(argv[2], root);
    }
    checkDocument();
    checkMetallicRoughness();
    checkSpecularGlossiness();
    checkWeighted();
    checkCoatAndFlakes();
    checkVolumeAndLight();
    checkPartial();
    std::printf("\n%d failure(s)\n", failures);
    return failures;
}
