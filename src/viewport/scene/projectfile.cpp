// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file projectfile.cpp
 * @brief The `.pss` project file reader/writer. See projectfile.h.
 */

#include "projectfile.h"

#include "materialjson.h" // the material's own JSON: shared with the .psm material file

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

/// Reads @p j's [x, y, z] into @p out, wrapping each channel into [-360, 360].
bool readVec3Degrees(const json& j, glm::vec3& out) {
    if (!readJsonVec3(j, out)) {
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

/// Reads one lighting dial by field name. Absent keeps the struct's default (a file from an
/// older build simply lacks the new dial); present but non-numeric, non-finite or past a
/// float's range rejects the file.
bool readSetting(const json& j, const char* field, float& out) { return readJsonFloat(j, field, out); }

/// … and a dial that is a whole number (the backdrop's mode): one that an int does not hold
/// rejects the file too.
bool readSetting(const json& j, const char* field, int& out) {
    const auto it = j.find(field);
    if (it == j.end()) {
        return true;
    }
    if (!it->is_number()) {
        return false;
    }
    const double v = it->get<double>();
    if (!std::isfinite(v) || v < -2147483648.0 || v > 2147483647.0) {
        return false;
    }
    out = static_cast<int>(v);
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
           readJsonBool(j, "tonemap",               s.tonemap) &&
           readSetting(j, "backdropMode",           s.backdropMode) &&
           readSetting(j, "backdropBlur",           s.backdropBlur) &&
           readSetting(j, "backdropBrightness",     s.backdropBrightness) &&
           readSetting(j, "domeRadius",             s.domeRadius) &&
           readJsonBool(j, "shadowsEnabled",        s.shadowsEnabled) &&
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

    // The Materials tab's per-zone edits: each entry names its zone and carries the zone's
    // values (a non-numeric or non-finite one rejects the file, as everywhere). Which of them
    // the entry NAMES is kept beside them: an absent value reads as the struct's default here,
    // and the loader gives the zone its own imported value for it instead (ProjectMaterial::named).
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
            if (!materialFromJson(entry, m.params, m.textures)) {
                return false;
            }
            for (const auto& item : entry.items()) {
                m.named.push_back(item.key());
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
    if (transIt != j.end() && !readJsonVec3(*transIt, fig.rootTranslation)) {
        return false;
    }

    const auto transformIt = j.find("transform");
    if (transformIt != j.end()) {
        if (!transformIt->is_object()) {
            return false;
        }
        const json& t = *transformIt;
        const auto tr = t.find("translation");
        if (tr != t.end() && !readJsonVec3(*tr, fig.translation)) {
            return false;
        }
        const auto ro = t.find("rotation");
        if (ro != t.end() && !readQuat(*ro, fig.rotation)) {
            return false;
        }
        const auto sc = t.find("scale");
        if (sc != t.end() && !readJsonVec3(*sc, fig.scale)) {
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

/// One light object (scenelight.h): every field by name, absent = the struct's default; a kind
/// this build does not know keeps Point. False on a value of the wrong type or a non-finite one.
bool readLight(const json& j, SceneLight& light) {
    if (!j.is_object()) {
        return false;
    }
    if (const auto it = j.find("name"); it != j.end()) {
        if (!it->is_string()) {
            return false;
        }
        light.name = it->get<std::string>();
    }
    if (const auto it = j.find("kind"); it != j.end()) {
        if (!it->is_string()) {
            return false;
        }
        light.kind = LightKind::Point;
        lightKindFromKey(it->get<std::string>(), light.kind); // (scenelight.h's keys: "point", "spot", "area", "sun")
    }
    if (const auto it = j.find("position"); it != j.end() && !readJsonVec3(*it, light.position)) {
        return false;
    }
    if (const auto it = j.find("color"); it != j.end() && !readJsonVec3(*it, light.color)) {
        return false;
    }
    return readJsonBool(j, "enabled", light.enabled) && readSetting(j, "turnDeg", light.turnDeg) &&
           readSetting(j, "tiltDeg", light.tiltDeg) && readSetting(j, "intensity", light.intensity) &&
           readSetting(j, "radius", light.radius) && readSetting(j, "coneDeg", light.coneDeg) &&
           readSetting(j, "blend", light.blend) && readSetting(j, "width", light.width) &&
           readSetting(j, "height", light.height) && readSetting(j, "sunAngleDeg", light.sunAngleDeg) &&
           readJsonBool(j, "castShadow", light.castShadow);
}

/// The `camera` object: target + yaw/pitch/distance/ortho.
bool readCamera(const json& j, ProjectCamera& cam) {
    if (!j.is_object()) {
        return false;
    }
    const auto targetIt = j.find("target");
    if (targetIt != j.end() && !readJsonVec3(*targetIt, cam.target)) {
        return false;
    }
    // (Absent keeps the struct's default; non-numeric or no finite float rejects the file.)
    return readJsonFloat(j, "yaw", cam.yaw) &&
           readJsonFloat(j, "pitch", cam.pitch) &&
           readJsonFloat(j, "distance", cam.distance) &&
           readJsonBool(j, "ortho", cam.ortho);
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
                json mat = materialToJson(m.params, m.textures);
                mat["zone"] = m.zone;
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

    // The light objects (scenelight.h): every field by name.
    json lights = json::array();
    for (const SceneLight& light : doc.lights) {
        lights.push_back({
            {"name", light.name},
            {"kind", lightKindKey(light.kind)},
            {"enabled", light.enabled},
            {"position", json::array({light.position.x, light.position.y, light.position.z})},
            {"turnDeg", light.turnDeg},
            {"tiltDeg", light.tiltDeg},
            {"color", json::array({light.color.x, light.color.y, light.color.z})},
            {"intensity", light.intensity},
            {"radius", light.radius},
            {"coneDeg", light.coneDeg},
            {"blend", light.blend},
            {"width", light.width},
            {"height", light.height},
            {"sunAngleDeg", light.sunAngleDeg},
            {"castShadow", light.castShadow},
        });
    }
    root["lights"] = std::move(lights);

    const ProjectCamera& cam = doc.camera;
    root["camera"] = {
        {"target",   json::array({cam.target.x, cam.target.y, cam.target.z})},
        {"yaw",      cam.yaw},
        {"pitch",    cam.pitch},
        {"distance", cam.distance},
        {"ortho",    cam.ortho},
    };

    // The text is made BEFORE the file is opened: opening truncates, and a document that cannot
    // be put into words must not cost the project that was there. A name that is not valid UTF-8
    // (a zone or a bone named by a model file in another encoding) is written with the
    // replacement character in the bad bytes' place — the strict default THROWS on one, which
    // left an empty file behind and took the application down with it.
    std::string text;
    try {
        text = root.dump(2, ' ', false, json::error_handler_t::replace);
    } catch (const json::exception&) {
        return false;
    }
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << text << '\n';
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

    // (Read into a document of its own and handed over whole: a failure half way leaves @p doc
    // empty, as the header says, never holding the figures that came before the bad one.)
    ProjectDocument read;
    const auto      figuresIt = root.find("figures");
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
            read.figures.push_back(std::move(fig));
        }
    }

    const auto environmentIt = root.find("environment");
    if (environmentIt != root.end() && !readEnvironment(*environmentIt, read.environment)) {
        error = "malformed environment entry";
        return false;
    }
    const auto lightsIt = root.find("lights");
    if (lightsIt != root.end()) {
        if (!lightsIt->is_array()) {
            error = "malformed document: \"lights\" is not an array";
            return false;
        }
        for (const json& entry : *lightsIt) {
            SceneLight light;
            if (!readLight(entry, light)) {
                error = "malformed light entry";
                return false;
            }
            read.lights.push_back(std::move(light));
        }
    }
    const auto cameraIt = root.find("camera");
    if (cameraIt != root.end() && !readCamera(*cameraIt, read.camera)) {
        error = "malformed camera entry";
        return false;
    }

    doc = std::move(read);
    return true;
}

} // namespace pose
