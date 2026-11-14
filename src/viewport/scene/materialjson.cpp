/**
 * @file materialjson.cpp
 * @brief A material as JSON: the one reader and the one writer. See materialjson.h.
 *
 * The two bodies below were moved here from projectfile.cpp as they stood (2026-11-13), when
 * the `.psm` material file needed the same codec: a project's zone and a saved material are
 * the same object in JSON, so a value added to MaterialParams is added HERE, once, for both.
 */

#include "materialjson.h"

#include <nlohmann/json.hpp>

#include <cmath>

namespace pose {

using nlohmann::json;

/// Reads @p j's [x, y, z] into @p out. False (leaving @p out untouched) if it isn't a three-number
/// array or any value is non-finite.
bool readJsonVec3(const json& j, glm::vec3& out) {
    if (!j.is_array() || j.size() != 3) {
        return false;
    }
    float v[3];
    for (int i = 0; i < 3; ++i) {
        if (!j[static_cast<std::size_t>(i)].is_number()) {
            return false;
        }
        v[i] = j[static_cast<std::size_t>(i)].get<float>();
    }
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) {
        return false;
    }
    out = glm::vec3(v[0], v[1], v[2]);
    return true;
}

/// Reads the boolean @p field of @p j into @p out. Absent leaves @p out as it is (true);
/// present but not a JSON boolean is false.
bool readJsonBool(const json& j, const char* field, bool& out) {
    const auto it = j.find(field);
    if (it == j.end()) {
        return true;
    }
    if (!it->is_boolean()) {
        return false;
    }
    out = it->get<bool>();
    return true;
}

bool materialFromJson(const json& entry, MaterialParams& params, std::vector<ProjectTexture>& textures) {
    if (!entry.is_object()) {
        return false;
    }
    const auto number = [&entry](const char* key, float& out) -> bool {
        const auto it = entry.find(key);
        if (it == entry.end()) {
            return true; // absent: the default stands
        }
        if (!it->is_number()) {
            return false;
        }
        const double v = it->get<double>();
        if (!std::isfinite(v)) {
            return false;
        }
        out = static_cast<float>(v);
        return true;
    };
    const auto colorIt = entry.find("baseColor");
    if (colorIt != entry.end() && !readJsonVec3(*colorIt, params.baseColor)) {
        return false;
    }
    if (!number("opacity", params.opacity) || !number("roughness", params.roughness) ||
        !number("metalness", params.metalness) || !number("specular", params.specular) ||
        !number("normalStrength", params.normalStrength) || !number("translucency", params.translucency) ||
        !number("topCoat", params.topCoat) || !number("topCoatRoughness", params.topCoatRoughness) ||
        !number("detailWeight", params.detailWeight) || !number("detailTiles", params.detailTiles) ||
        !number("detailTilesV", params.detailTilesV) || !number("detailOffsetU", params.detailOffset.x) ||
        !number("detailOffsetV", params.detailOffset.y) || !number("detailRoughness", params.detailRoughness) ||
        !number("displacementStrength", params.displacementStrength) || !number("displacementMidLevel", params.displacementMidLevel) ||
        !number("displacementSubdiv", params.displacementSubdiv) || !number("aoWeight", params.aoWeight) ||
        !number("subsurfaceAnisotropy", params.subsurfaceAnisotropy) || !number("coatAnisotropy", params.coatAnisotropy) ||
        !number("coatAnisotropyRotation", params.coatAnisotropyRotation) || !number("uvRotation", params.uvRotation) ||
        !number("lobe1Roughness", params.lobe1Roughness) || !number("lobe2Roughness", params.lobe2Roughness) ||
        !number("lobeRatio", params.lobeRatio)) {
        return false;
    }
    // The canonical material's further channels (absent = the default, as above).
    const auto flag = [&entry](const char* key, bool& out) -> bool {
        const auto it = entry.find(key);
        if (it == entry.end()) {
            return true;
        }
        if (!it->is_boolean()) {
            return false;
        }
        out = it->get<bool>();
        return true;
    };
    const auto color = [&entry](const char* key, glm::vec3& out) -> bool {
        const auto it = entry.find(key);
        return it == entry.end() || readJsonVec3(*it, out);
    };
    const auto pair2 = [&entry](const char* key, glm::vec2& out) -> bool {
        const auto it = entry.find(key);
        if (it == entry.end()) {
            return true;
        }
        if (!it->is_array() || it->size() != 2) {
            return false;
        }
        for (std::size_t k = 0; k < 2; ++k) {
            const json& v = (*it)[k];
            if (!v.is_number() || !std::isfinite(v.get<double>())) {
                return false;
            }
            out[static_cast<glm::length_t>(k)] = static_cast<float>(v.get<double>());
        }
        return true;
    };
    if (!number("ior", params.ior) || !number("anisotropy", params.anisotropy) ||
        !number("anisotropyRotation", params.anisotropyRotation) || !number("diffuseRoughness", params.diffuseRoughness) ||
        !number("transmission", params.transmission) || !number("transmissionDepth", params.transmissionDepth) ||
        !number("transmissionRoughness", params.transmissionRoughness) ||
        !number("dispersion", params.dispersion) || !number("subsurfaceRadius", params.subsurfaceRadius) ||
        !number("sheen", params.sheen) || !number("sheenRoughness", params.sheenRoughness) ||
        !number("coatIor", params.coatIor) || !number("coatBumpStrength", params.coatBumpStrength) ||
        !number("thinFilm", params.thinFilm) ||
        !number("thinFilmThickness", params.thinFilmThickness) || !number("thinFilmIor", params.thinFilmIor) ||
        !number("emission", params.emission) || !number("bumpStrength", params.bumpStrength)) {
        return false;
    }
    if (!color("specularColor", params.specularColor) || !color("transmissionColor", params.transmissionColor) ||
        !color("subsurfaceColor", params.subsurfaceColor) || !color("subsurfaceRadiusColor", params.subsurfaceRadiusColor) ||
        !color("sheenColor", params.sheenColor) ||
        !color("coatColor", params.coatColor) || !color("emissionColor", params.emissionColor)) {
        return false;
    }
    // (THE SMOOTHING: an object's state. A project's zone carries it; a material file never does.)
    if (!flag("smoothByAngle", params.smoothByAngle) || !number("smoothAngle", params.smoothAngle)) {
        return false;
    }
    if (!flag("thinWalled", params.thinWalled) || !flag("doubleSided", params.doubleSided) || !pair2("uvTiling", params.uvTiling) ||
        !pair2("uvOffset", params.uvOffset) ||
        !flag("topCoatEnabled", params.topCoatEnabled) ||
        !flag("sheenEnabled", params.sheenEnabled) ||
        !flag("thinFilmEnabled", params.thinFilmEnabled) ||
        !flag("transmissionEnabled", params.transmissionEnabled) ||
        !flag("subsurfaceEnabled", params.subsurfaceEnabled) ||
        !flag("emissionEnabled", params.emissionEnabled) ||
        !flag("displacementEnabled", params.displacementEnabled) ||
        !flag("detailEnabled", params.detailEnabled) ||
        !flag("baseEnabled", params.baseEnabled) ||
        !flag("specularEnabled", params.specularEnabled) ||
        !flag("geometryEnabled", params.geometryEnabled) ||
        !flag("tilingEnabled", params.tilingEnabled)) {
        return false;
    }
    // The texture slots the tab changed: a slot this version does not know is left out
    // (forward compatible, like an unknown field); a malformed entry rejects the file.
    const auto texturesIt = entry.find("textures");
    if (texturesIt != entry.end()) {
        if (!texturesIt->is_array()) {
            return false;
        }
        for (const json& t : *texturesIt) {
            if (!t.is_object()) {
                return false;
            }
            const auto slotIt = t.find("slot");
            const auto sourceIt = t.find("source");
            if (slotIt == t.end() || !slotIt->is_string() || sourceIt == t.end() || !sourceIt->is_string()) {
                return false;
            }
            MaterialMap map = MaterialMap::BaseColor;
            if (!materialMapByName(slotIt->get<std::string>(), map)) {
                continue;
            }
            textures.push_back({slotIt->get<std::string>(), sourceIt->get<std::string>()});
        }
    }
    // The zone's overlays, lowest first: each an object of image paths and controls (an
    // absent value keeps its default; a blend this version does not know keeps Multiply;
    // a malformed entry rejects the file).
    const auto overlaysIt = entry.find("overlays");
    if (overlaysIt != entry.end()) {
        if (!overlaysIt->is_array()) {
            return false;
        }
        for (const json& oj : *overlaysIt) {
            if (!oj.is_object()) {
                return false;
            }
            MaterialOverlay o;
            const auto      value = [&oj](const char* key, float& out) -> bool {
                const auto it = oj.find(key);
                if (it == oj.end()) {
                    return true;
                }
                if (!it->is_number() || !std::isfinite(it->get<double>())) {
                    return false;
                }
                out = it->get<float>();
                return true;
            };
            const auto text = [&oj](const char* key, std::string& out) -> bool {
                const auto it = oj.find(key);
                if (it == oj.end()) {
                    return true;
                }
                if (!it->is_string()) {
                    return false;
                }
                out = it->get<std::string>();
                return true;
            };
            std::string blend;
            const auto  tintIt = oj.find("tint");
            if (!value("offsetX", o.offset.x) || !value("offsetY", o.offset.y) || !value("rotation", o.rotation) ||
                !value("scale", o.scale) ||
                !text("name", o.name) || !readJsonBool(oj, "enabled", o.enabled) || !text("color", o.color) || !text("opacityMap", o.opacityMap) ||
                !text("blend", blend) ||
                !value("opacity", o.opacity) || (tintIt != oj.end() && !readJsonVec3(*tintIt, o.tint)) ||
                !value("brightness", o.brightness) || !value("contrast", o.contrast) || !value("gamma", o.gamma) ||
                !value("hue", o.hue) || !value("saturation", o.saturation) || !value("gloss", o.gloss) ||
                !value("metallic", o.metallic) || !text("metallicMap", o.metallicMap) ||
                !text("bump", o.bump) || !value("bumpStrength", o.bumpStrength) || !text("normal", o.normal) ||
                !value("normalStrength", o.normalStrength) || !text("displacement", o.displacement) ||
                !value("displacementStrength", o.displacementStrength)) {
                return false;
            }
            overlayBlendByName(blend, o.blend);
            params.overlays.push_back(std::move(o));
        }
    }
    return true;
}

json materialToJson(const MaterialParams& p, const std::vector<ProjectTexture>& textures) {
        const auto vec3 = [](const glm::vec3& v) { return json::array({v.x, v.y, v.z}); };
    json mat = {
            {"baseColor", vec3(p.baseColor)},
        {"opacity", p.opacity},
        {"roughness", p.roughness},
        {"metalness", p.metalness},
        {"specular", p.specular},
        {"normalStrength", p.normalStrength},
        {"translucency", p.translucency},
        {"topCoat", p.topCoat},
        {"topCoatRoughness", p.topCoatRoughness},
        {"detailWeight", p.detailWeight},
        {"detailTiles", p.detailTiles},
        {"detailTilesV", p.detailTilesV},
        {"detailOffsetU", p.detailOffset.x},
        {"detailOffsetV", p.detailOffset.y},
        {"detailRoughness", p.detailRoughness},
        {"displacementStrength", p.displacementStrength},
        {"displacementMidLevel", p.displacementMidLevel},
        {"displacementSubdiv", p.displacementSubdiv},
        {"aoWeight", p.aoWeight},
        {"lobe1Roughness", p.lobe1Roughness},
        {"lobe2Roughness", p.lobe2Roughness},
        {"lobeRatio", p.lobeRatio},
        // The canonical material's further channels (2026-11-09).
        {"ior", p.ior},
        {"anisotropy", p.anisotropy},
        {"anisotropyRotation", p.anisotropyRotation},
        {"diffuseRoughness", p.diffuseRoughness},
        {"specularColor", vec3(p.specularColor)},
        {"transmission", p.transmission},
        {"transmissionColor", vec3(p.transmissionColor)},
        {"transmissionDepth", p.transmissionDepth},
        {"transmissionRoughness", p.transmissionRoughness},
        {"dispersion", p.dispersion},
        {"thinWalled", p.thinWalled},
        {"doubleSided", p.doubleSided},
        {"smoothByAngle", p.smoothByAngle},
        {"smoothAngle", p.smoothAngle},
        {"topCoatEnabled", p.topCoatEnabled},
        {"sheenEnabled", p.sheenEnabled},
        {"thinFilmEnabled", p.thinFilmEnabled},
        {"transmissionEnabled", p.transmissionEnabled},
        {"subsurfaceEnabled", p.subsurfaceEnabled},
        {"emissionEnabled", p.emissionEnabled},
        {"displacementEnabled", p.displacementEnabled},
        {"detailEnabled", p.detailEnabled},
        {"baseEnabled", p.baseEnabled},
        {"specularEnabled", p.specularEnabled},
        {"geometryEnabled", p.geometryEnabled},
        {"tilingEnabled", p.tilingEnabled},
        {"uvRotation", p.uvRotation},
        {"coatAnisotropy", p.coatAnisotropy},
        {"coatAnisotropyRotation", p.coatAnisotropyRotation},
        {"subsurfaceColor", vec3(p.subsurfaceColor)},
        {"subsurfaceRadius", p.subsurfaceRadius},
        {"subsurfaceRadiusColor", vec3(p.subsurfaceRadiusColor)},
        {"subsurfaceAnisotropy", p.subsurfaceAnisotropy},
        {"sheen", p.sheen},
        {"sheenColor", vec3(p.sheenColor)},
        {"sheenRoughness", p.sheenRoughness},
        {"coatColor", vec3(p.coatColor)},
        {"coatIor", p.coatIor},
        {"coatBumpStrength", p.coatBumpStrength},
        {"thinFilm", p.thinFilm},
        {"thinFilmThickness", p.thinFilmThickness},
        {"thinFilmIor", p.thinFilmIor},
        {"emissionColor", vec3(p.emissionColor)},
        {"emission", p.emission},
        {"bumpStrength", p.bumpStrength},
        {"uvTiling", json::array({p.uvTiling.x, p.uvTiling.y})},
        {"uvOffset", json::array({p.uvOffset.x, p.uvOffset.y})},
    };
    if (!textures.empty()) {
        json slots = json::array();
        for (const ProjectTexture& t : textures) {
            slots.push_back({{"slot", t.slot}, {"source", t.source}});
        }
        mat["textures"] = std::move(slots);
    }
    if (!p.overlays.empty()) {
        json overlays = json::array();
        for (const MaterialOverlay& o : p.overlays) {
            overlays.push_back({
                {"name", o.name},
                {"enabled", o.enabled},
                {"offsetX", o.offset.x},
                {"offsetY", o.offset.y},
                {"rotation", o.rotation},
                {"scale", o.scale},
                {"color", o.color},
                {"opacityMap", o.opacityMap},
                {"blend", overlayBlendName(o.blend)},
                {"opacity", o.opacity},
                {"tint", vec3(o.tint)},
                {"brightness", o.brightness},
                {"contrast", o.contrast},
                {"gamma", o.gamma},
                {"hue", o.hue},
                {"saturation", o.saturation},
                {"gloss", o.gloss},
                {"metallic", o.metallic},
                {"metallicMap", o.metallicMap},
                {"bump", o.bump},
                {"bumpStrength", o.bumpStrength},
                {"normal", o.normal},
                {"normalStrength", o.normalStrength},
                {"displacement", o.displacement},
                {"displacementStrength", o.displacementStrength},
            });
        }
        mat["overlays"] = std::move(overlays);
    }
    return mat;
}

} // namespace pose
