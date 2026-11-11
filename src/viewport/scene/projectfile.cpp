/**
 * @file projectfile.cpp
 * @brief The `.pss` project file reader/writer. See projectfile.h.
 */

#include "projectfile.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>

namespace pose {

namespace {

using nlohmann::json;

/// The document's format marker and the highest version this build reads (a higher one is
/// rejected: we don't guess what a newer file means).
constexpr const char* kFormatMarker = "posestudio.project";
constexpr int         kVersion      = 1;

/// Wraps an angle into [-360, 360] (fmod keeps the sign) — the posefile.h convention, so a
/// hand-edited file can't feed an absurd angle to the limit clamp downstream.
float wrapDegrees(float v) { return std::fmod(v, 360.0f); }

/// Reads @p j's [x, y, z] into @p out. False (leaving @p out untouched) if it isn't a three-number
/// array or any value is non-finite.
bool readVec3(const json& j, glm::vec3& out) {
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

/// Reads @p j's [x, y, z] into @p out, wrapping each channel into [-360, 360].
bool readVec3Degrees(const json& j, glm::vec3& out) {
    if (!readVec3(j, out)) {
        return false;
    }
    out = glm::vec3(wrapDegrees(out.x), wrapDegrees(out.y), wrapDegrees(out.z));
    return true;
}

/// Reads @p j's [x, y, z, w] into a unit quaternion @p out. False if it isn't a four-number array,
/// any value is non-finite, or the length is zero (a degenerate rotation can't be normalized).
bool readQuat(const json& j, glm::vec4& out) {
    if (!j.is_array() || j.size() != 4) {
        return false;
    }
    float v[4];
    for (int i = 0; i < 4; ++i) {
        if (!j[static_cast<std::size_t>(i)].is_number()) {
            return false;
        }
        v[i] = j[static_cast<std::size_t>(i)].get<float>();
    }
    const glm::vec4 q(v[0], v[1], v[2], v[3]);
    const float len = glm::length(q);
    if (!std::isfinite(len) || len <= 0.0f) {
        return false;
    }
    out = q / len; // normalize: a hand-edited file may carry a slightly off length
    return true;
}

/// Reads one lighting dial by field name.
/// older build simply lacks the new dial); present but non-numeric or non-finite rejects the file.
template <class T>
bool readSetting(const json& j, const char* field, T& out) {
    const auto it = j.find(field);
    if (it == j.end()) {
        return true;
    }
    if (!it->is_number()) {
        return false;
    }
    const double v = it->get<double>();
    if (!std::isfinite(v)) {
        return false;
    }
    out = static_cast<T>(v);
    return true;
}

/// The bool analogue (a JSON boolean, not a number).
bool readBoolSetting(const json& j, const char* field, bool& out) {
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

/// The environment's `settings` object: every LightingSettings field by name (absent = default).
bool readLightingSettings(const json& j, LightingSettings& s) {
    if (!j.is_object()) {
        return false;
    }
    return readSetting(j, "exposure",               s.exposure) &&
           readSetting(j, "diffuseIntensity",       s.diffuseIntensity) &&
           readSetting(j, "specularIntensity",      s.specularIntensity) &&
           readSetting(j, "ambientFill",            s.ambientFill) &&
           readSetting(j, "keyIntensity",           s.keyIntensity) &&
           readSetting(j, "environmentRotationDeg", s.environmentRotationDeg) &&
           readSetting(j, "keyAzimuthDeg",          s.keyAzimuthDeg) &&
           readSetting(j, "keyElevationDeg",        s.keyElevationDeg) &&
           readSetting(j, "subsurface",             s.subsurface) &&
           readSetting(j, "rimIntensity",           s.rimIntensity) &&
           readBoolSetting(j, "tonemap",            s.tonemap) &&
           readSetting(j, "backdropMode",           s.backdropMode) &&
           readSetting(j, "backdropBlur",           s.backdropBlur) &&
           readSetting(j, "backdropBrightness",     s.backdropBrightness) &&
           readSetting(j, "domeRadius",             s.domeRadius) &&
           readBoolSetting(j, "shadowsEnabled",   s.shadowsEnabled) &&
           readSetting(j, "shadowIntensity",        s.shadowIntensity) &&
           readSetting(j, "shadowSoftness",         s.shadowSoftness) &&
           readSetting(j, "shadowReach",            s.shadowReach);
}

/// One figure entry: `source` (a non-empty string), `pose` rows of [name, rx, ry, rz], `pins`
/// names, `rootTranslation`, and the `transform` TRS. A malformed row rejects the WHOLE file.
bool readFigure(const json& j, ProjectFigure& fig) {
    if (!j.is_object()) {
        return false;
    }
    const auto sourceIt = j.find("source");
    if (sourceIt == j.end() || !sourceIt->is_string() || sourceIt->get<std::string>().empty()) {
        return false;
    }
    fig.source = sourceIt->get<std::string>();

    const auto poseIt = j.find("pose");
    if (poseIt != j.end()) {
        if (!poseIt->is_array()) {
            return false;
        }
        for (const json& row : *poseIt) {
            if (!row.is_array() || row.size() != 4 || !row[0].is_string()) {
                return false;
            }
            // The row is [name, rx, ry, rz] — the angle triple is the last three cells.
            const json angles = json::array({row[1], row[2], row[3]});
            glm::vec3 v;
            if (!readVec3Degrees(angles, v)) {
                return false;
            }
            fig.pose.emplace_back(row[0].get<std::string>(), v);
        }
    }

    const auto pinsIt = j.find("pins");
    if (pinsIt != j.end()) {
        if (!pinsIt->is_array()) {
            return false;
        }
        for (const json& name : *pinsIt) {
            if (!name.is_string()) {
                return false;
            }
            fig.pins.push_back(name.get<std::string>());
        }
    }

    // The Materials tab's per-zone edits: each entry names its zone and carries the values that
    // differ from the struct's defaults (an absent value keeps the default; a non-numeric or
    // non-finite one rejects the file, as everywhere).
    const auto materialsIt = j.find("materials");
    if (materialsIt != j.end()) {
        if (!materialsIt->is_array()) {
            return false;
        }
        for (const json& entry : *materialsIt) {
            if (!entry.is_object()) {
                return false;
            }
            const auto zoneIt = entry.find("zone");
            if (zoneIt == entry.end() || !zoneIt->is_string()) {
                return false;
            }
            ProjectMaterial m;
            m.zone = zoneIt->get<std::string>();
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
            if (colorIt != entry.end() && !readVec3(*colorIt, m.params.baseColor)) {
                return false;
            }
            if (!number("opacity", m.params.opacity) || !number("roughness", m.params.roughness) ||
                !number("metalness", m.params.metalness) || !number("specular", m.params.specular) ||
                !number("normalStrength", m.params.normalStrength) || !number("translucency", m.params.translucency) ||
                !number("topCoat", m.params.topCoat) || !number("topCoatRoughness", m.params.topCoatRoughness) ||
                !number("detailWeight", m.params.detailWeight) || !number("detailTiles", m.params.detailTiles) ||
                !number("detailTilesV", m.params.detailTilesV) || !number("detailOffsetU", m.params.detailOffset.x) ||
                !number("detailOffsetV", m.params.detailOffset.y) || !number("detailRoughness", m.params.detailRoughness) ||
                !number("displacementStrength", m.params.displacementStrength) || !number("displacementMidLevel", m.params.displacementMidLevel) ||
                !number("displacementSubdiv", m.params.displacementSubdiv) || !number("aoWeight", m.params.aoWeight) ||
                !number("subsurfaceAnisotropy", m.params.subsurfaceAnisotropy) || !number("coatAnisotropy", m.params.coatAnisotropy) ||
                !number("coatAnisotropyRotation", m.params.coatAnisotropyRotation) || !number("uvRotation", m.params.uvRotation) ||
                !number("lobe1Roughness", m.params.lobe1Roughness) || !number("lobe2Roughness", m.params.lobe2Roughness) ||
                !number("lobeRatio", m.params.lobeRatio)) {
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
                return it == entry.end() || readVec3(*it, out);
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
            if (!number("ior", m.params.ior) || !number("anisotropy", m.params.anisotropy) ||
                !number("anisotropyRotation", m.params.anisotropyRotation) || !number("diffuseRoughness", m.params.diffuseRoughness) ||
                !number("transmission", m.params.transmission) || !number("transmissionDepth", m.params.transmissionDepth) ||
                !number("dispersion", m.params.dispersion) || !number("subsurfaceRadius", m.params.subsurfaceRadius) ||
                !number("sheen", m.params.sheen) || !number("sheenRoughness", m.params.sheenRoughness) ||
                !number("coatIor", m.params.coatIor) || !number("coatBumpStrength", m.params.coatBumpStrength) ||
                !number("thinFilm", m.params.thinFilm) ||
                !number("thinFilmThickness", m.params.thinFilmThickness) || !number("thinFilmIor", m.params.thinFilmIor) ||
                !number("emission", m.params.emission) || !number("bumpStrength", m.params.bumpStrength)) {
                return false;
            }
            if (!color("specularColor", m.params.specularColor) || !color("transmissionColor", m.params.transmissionColor) ||
                !color("subsurfaceColor", m.params.subsurfaceColor) || !color("subsurfaceRadiusColor", m.params.subsurfaceRadiusColor) ||
                !color("sheenColor", m.params.sheenColor) ||
                !color("coatColor", m.params.coatColor) || !color("emissionColor", m.params.emissionColor)) {
                return false;
            }
            if (!flag("thinWalled", m.params.thinWalled) || !flag("doubleSided", m.params.doubleSided) || !pair2("uvTiling", m.params.uvTiling) ||
                !pair2("uvOffset", m.params.uvOffset) ||
                !flag("topCoatEnabled", m.params.topCoatEnabled) ||
                !flag("sheenEnabled", m.params.sheenEnabled) ||
                !flag("thinFilmEnabled", m.params.thinFilmEnabled) ||
                !flag("transmissionEnabled", m.params.transmissionEnabled) ||
                !flag("subsurfaceEnabled", m.params.subsurfaceEnabled) ||
                !flag("emissionEnabled", m.params.emissionEnabled) ||
                !flag("displacementEnabled", m.params.displacementEnabled) ||
                !flag("detailEnabled", m.params.detailEnabled) ||
                !flag("baseEnabled", m.params.baseEnabled) ||
                !flag("specularEnabled", m.params.specularEnabled) ||
                !flag("geometryEnabled", m.params.geometryEnabled) ||
                !flag("tilingEnabled", m.params.tilingEnabled)) {
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
                    m.textures.push_back({slotIt->get<std::string>(), sourceIt->get<std::string>()});
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
                        !text("name", o.name) || !readBoolSetting(oj, "enabled", o.enabled) || !text("color", o.color) || !text("opacityMap", o.opacityMap) ||
                        !text("blend", blend) ||
                        !value("opacity", o.opacity) || (tintIt != oj.end() && !readVec3(*tintIt, o.tint)) ||
                        !value("brightness", o.brightness) || !value("contrast", o.contrast) || !value("gamma", o.gamma) ||
                        !value("hue", o.hue) || !value("saturation", o.saturation) || !value("gloss", o.gloss) ||
                        !value("metallic", o.metallic) || !text("metallicMap", o.metallicMap) ||
                        !text("bump", o.bump) || !value("bumpStrength", o.bumpStrength) || !text("normal", o.normal) ||
                        !value("normalStrength", o.normalStrength) || !text("displacement", o.displacement) ||
                        !value("displacementStrength", o.displacementStrength)) {
                        return false;
                    }
                    overlayBlendByName(blend, o.blend);
                    m.params.overlays.push_back(std::move(o));
                }
            }
            fig.materials.push_back(std::move(m));
        }
    }

    const auto rootBoneIt = j.find("rootBone");
    if (rootBoneIt != j.end()) {
        if (!rootBoneIt->is_string()) {
            return false;
        }
        fig.rootBone = rootBoneIt->get<std::string>();
    }

    const auto transIt = j.find("rootTranslation");
    if (transIt != j.end() && !readVec3(*transIt, fig.rootTranslation)) {
        return false;
    }

    const auto transformIt = j.find("transform");
    if (transformIt != j.end()) {
        if (!transformIt->is_object()) {
            return false;
        }
        const json& t = *transformIt;
        const auto tr = t.find("translation");
        if (tr != t.end() && !readVec3(*tr, fig.translation)) {
            return false;
        }
        const auto ro = t.find("rotation");
        if (ro != t.end() && !readQuat(*ro, fig.rotation)) {
            return false;
        }
        const auto sc = t.find("scale");
        if (sc != t.end() && !readVec3(*sc, fig.scale)) {
            return false;
        }
    }

    return true;
}

/// The `environment` object: `hdri` path + the `settings` dials.
bool readEnvironment(const json& j, ProjectEnvironment& env) {
    if (!j.is_object()) {
        return false;
    }
    const auto hdriIt = j.find("hdri");
    if (hdriIt != j.end()) {
        if (!hdriIt->is_string()) {
            return false;
        }
        env.hdri = hdriIt->get<std::string>();
    }
    const auto settingsIt = j.find("settings");
    if (settingsIt != j.end() && !readLightingSettings(*settingsIt, env.settings)) {
        return false;
    }
    return true;
}

/// Reads one camera scalar by field name. Absent keeps the struct's default; present but
/// non-numeric or non-finite rejects the file.
bool readCameraScalar(const json& j, const char* field, float& out) {
    const auto it = j.find(field);
    if (it == j.end()) {
        return true;
    }
    if (!it->is_number() || !std::isfinite(it->get<double>())) {
        return false;
    }
    out = it->get<float>();
    return true;
}

/// The `camera` object: target + yaw/pitch/distance/ortho.
bool readCamera(const json& j, ProjectCamera& cam) {
    if (!j.is_object()) {
        return false;
    }
    const auto targetIt = j.find("target");
    if (targetIt != j.end() && !readVec3(*targetIt, cam.target)) {
        return false;
    }
    return readCameraScalar(j, "yaw", cam.yaw) &&
           readCameraScalar(j, "pitch", cam.pitch) &&
           readCameraScalar(j, "distance", cam.distance) &&
           readBoolSetting(j, "ortho", cam.ortho);
}

} // namespace

bool writeProjectFile(const std::string& path, const ProjectDocument& doc) {
    json root;
    root["format"]  = kFormatMarker;
    root["version"] = kVersion;

    json figures = json::array();
    for (const ProjectFigure& fig : doc.figures) {
        json entry;
        entry["source"] = fig.source;

        json pose = json::array();
        for (const auto& [name, value] : fig.pose) {
            pose.push_back(json::array({name, value.x, value.y, value.z}));
        }
        entry["pose"] = std::move(pose);

        json pins = json::array();
        for (const std::string& name : fig.pins) {
            pins.push_back(name);
        }
        entry["pins"] = std::move(pins);

        entry["rootBone"] = fig.rootBone;
        entry["rootTranslation"] = json::array({fig.rootTranslation.x, fig.rootTranslation.y,
                                                fig.rootTranslation.z});
        entry["transform"] = {
            {"translation", json::array({fig.translation.x, fig.translation.y, fig.translation.z})},
            {"rotation",    json::array({fig.rotation.x, fig.rotation.y, fig.rotation.z, fig.rotation.w})},
            {"scale",       json::array({fig.scale.x, fig.scale.y, fig.scale.z})},
        };
        if (!fig.materials.empty()) {
            json materials = json::array();
            for (const ProjectMaterial& m : fig.materials) {
                const MaterialParams& p = m.params;
                const auto vec3 = [](const glm::vec3& v) { return json::array({v.x, v.y, v.z}); };
                json mat = {
                    {"zone", m.zone},
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
                    {"dispersion", p.dispersion},
                    {"thinWalled", p.thinWalled},
                    {"doubleSided", p.doubleSided},
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
                if (!m.textures.empty()) {
                    json textures = json::array();
                    for (const ProjectTexture& t : m.textures) {
                        textures.push_back({{"slot", t.slot}, {"source", t.source}});
                    }
                    mat["textures"] = std::move(textures);
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
                materials.push_back(std::move(mat));
            }
            entry["materials"] = std::move(materials);
        }
        figures.push_back(std::move(entry));
    }
    root["figures"] = std::move(figures);

    const LightingSettings& s = doc.environment.settings;
    root["environment"] = {
        {"hdri", doc.environment.hdri},
        {"settings",
         {{"exposure",               s.exposure},
          {"diffuseIntensity",       s.diffuseIntensity},
          {"specularIntensity",      s.specularIntensity},
          {"ambientFill",            s.ambientFill},
          {"keyIntensity",           s.keyIntensity},
          {"environmentRotationDeg", s.environmentRotationDeg},
          {"keyAzimuthDeg",          s.keyAzimuthDeg},
          {"keyElevationDeg",        s.keyElevationDeg},
          {"subsurface",             s.subsurface},
          {"rimIntensity",           s.rimIntensity},
          {"tonemap",                s.tonemap},
          {"backdropMode",           s.backdropMode},
          {"backdropBlur",           s.backdropBlur},
          {"backdropBrightness",     s.backdropBrightness},
          {"domeRadius",             s.domeRadius},
          {"shadowsEnabled",         s.shadowsEnabled},
          {"shadowIntensity",        s.shadowIntensity},
          {"shadowSoftness",         s.shadowSoftness},
          {"shadowReach",            s.shadowReach}}},
    };

    const ProjectCamera& cam = doc.camera;
    root["camera"] = {
        {"target",   json::array({cam.target.x, cam.target.y, cam.target.z})},
        {"yaw",      cam.yaw},
        {"pitch",    cam.pitch},
        {"distance", cam.distance},
        {"ortho",    cam.ortho},
    };

    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << root.dump(2) << '\n';
    return static_cast<bool>(out);
}

bool readProjectFile(const std::string& path, ProjectDocument& doc, std::string& error) {
    doc = ProjectDocument{};

    std::ifstream in(path);
    if (!in) {
        error = "file not found: " + path;
        return false;
    }
    json root;
    try {
        in >> root;
    } catch (const json::exception& e) {
        error = std::string("not valid JSON: ") + e.what();
        return false;
    }
    if (!root.is_object()) {
        error = "not a project document (top level is not an object)";
        return false;
    }

    const auto formatIt = root.find("format");
    if (formatIt == root.end() || !formatIt->is_string() ||
        formatIt->get<std::string>() != kFormatMarker) {
        error = "not a PoseStudio project file (missing or wrong \"format\" marker)";
        return false;
    }
    const auto versionIt = root.find("version");
    if (versionIt == root.end() || !versionIt->is_number_integer() ||
        versionIt->get<int>() > kVersion) {
        error = "unsupported project file version (this build reads up to " +
                std::to_string(kVersion) + ")";
        return false;
    }

    const auto figuresIt = root.find("figures");
    if (figuresIt != root.end()) {
        if (!figuresIt->is_array()) {
            error = "malformed document: \"figures\" is not an array";
            return false;
        }
        for (const json& entry : *figuresIt) {
            ProjectFigure fig;
            if (!readFigure(entry, fig)) {
                error = "malformed figure entry";
                return false;
            }
            doc.figures.push_back(std::move(fig));
        }
    }

    const auto environmentIt = root.find("environment");
    if (environmentIt != root.end() && !readEnvironment(*environmentIt, doc.environment)) {
        error = "malformed document: \"environment\" is not an object";
        return false;
    }
    const auto cameraIt = root.find("camera");
    if (cameraIt != root.end() && !readCamera(*cameraIt, doc.camera)) {
        error = "malformed document: \"camera\" is not an object";
        return false;
    }

    return true;
}

} // namespace pose
