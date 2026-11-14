/**
 * @file projectfile.h
 * @brief The `.pss` project file: the whole working scene — figures with their poses, model
 *        transforms, the lighting environment and the camera framing — written out and read back.
 *
 * A project file is a small (kilobyte-scale) versioned JSON document that REFERENCES imported
 * figures by source path (`.duf`/`.dsf`/`.obj`) instead of duplicating their geometry or
 * textures: loading re-imports each figure from its source and restores the saved state on top.
 * The document carries an explicit format marker ("posestudio.project") and version; unknown
 * FIELDS are ignored (forward compatibility), a higher/unknown VERSION is rejected (we don't
 * guess). This file owns the codec only — it never touches a figure — and is Qt-free (<fstream> +
 * nlohmann alone), so the Scene's save/load are thin forwarders and the format has exactly one
 * reader and one writer, mirroring posefile.h's discipline: a bad file is rejected WHOLE, never
 * half-applied.
 */

#ifndef PROJECTFILE_H
#define PROJECTFILE_H

#include "posefile.h"          // PoseRows (the figure's pose snapshot rows)
#include "lightingsettings.h"  // LightingSettings (the environment dials)
#include "materialjson.h"      // ProjectTexture, and the material's JSON codec (shared with materialfile.h)
#include "materialparams.h"    // MaterialParams (the Materials tab's per-zone values)
#include "scenelight.h"        // SceneLight (the light objects)

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace pose {

/// One zone's material as the Materials tab left it (2026-11-09): the zone's key (its name, or
/// "#i" for an unnamed zone — Model::zoneKey), its whole value set, and the texture slots the
/// tab changed (ProjectTexture, materialjson.h): each slot's name and what it holds now — a file
/// path, or "" for a cleared slot. A slot not listed is the import's own.
struct ProjectMaterial {
    std::string                 zone;
    MaterialParams              params;
    std::vector<ProjectTexture> textures; ///< The slots that differ from the import (paths are absolute).
    /// Whether the file says how the zone is smoothed (params.smoothByAngle / smoothAngle). A
    /// project saved before the smoothing existed does not: the zone then keeps what the import
    /// gives it.
    bool                        hasSmoothing = false;
};

/// One saved figure: its source file plus the state to restore onto the re-imported model.
struct ProjectFigure {
    std::string          source;   ///< Absolute path of the imported file (`.duf`/`.dsf`/`.obj`).
    PoseRows             pose;     ///< The pose snapshot's rotation rows: (bone name, Euler degrees).
    std::vector<std::string> pins; ///< The user's pinned bone names (the snapshot's @pin: rows).
    std::string          rootBone; ///< The bone carrying the saved pose translation ("" = none).
    glm::vec3            rootTranslation{0.0f}; ///< The skeleton root's pose translation (@trans: row).
    glm::vec3            translation{0.0f};     ///< The model transform's translation part.
    glm::vec4            rotation{0.0f, 0.0f, 0.0f, 1.0f}; ///< ...rotation part (a unit quaternion x,y,z,w).
    glm::vec3            scale{1.0f};           ///< ...scale part.
    /// The Materials tab's edits: every zone whose material differs from the imported one (the
    /// unchanged zones are not written — the re-imported file supplies them).
    std::vector<ProjectMaterial> materials;
};

/// The saved environment: the HDRI reference path plus all the live lighting dials.
struct ProjectEnvironment {
    std::string       hdri;  ///< The chosen panorama's path (empty = the procedural studio).
    LightingSettings  settings;
};

/// The saved camera framing (scene/camera.h's orbit state).
struct ProjectCamera {
    glm::vec3 target{0.0f, 0.9f, 0.0f}; ///< The orbit's aim point.
    float     yaw = 0.0f;               ///< Around the world up axis (radians).
    float     pitch = 0.0f;             ///< Above/below the horizon (radians).
    float     distance = 2.6f;          ///< Eye-to-target distance.
    bool      ortho = false;            ///< Orthographic projection (an axis view) or perspective.
};

/// The whole `.pss` document: every figure in scene order, the environment, the light objects,
/// the camera.
struct ProjectDocument {
    std::vector<ProjectFigure>  figures; ///< In scene order — the Nth entry becomes the Nth figure.
    ProjectEnvironment          environment;
    std::vector<SceneLight>     lights;  ///< The light objects (scenelight.h), in the order they were added.
    ProjectCamera               camera;
};

/// Writes @p doc to @p path as versioned JSON (`"format": "posestudio.project"`, `"version": 1`).
/// Returns false if the file can't be opened or written.
bool writeProjectFile(const std::string& path, const ProjectDocument& doc);

/// Reads @p path into @p doc (leaving it empty on failure). Returns false — with a human-readable
/// @p error — if the file can't be opened, is not valid JSON, carries a different format marker,
/// a higher/unknown version, or a non-finite value; unknown fields are ignored. Rotation rows are
/// wrapped into [-360, 360] as in posefile.h.
bool readProjectFile(const std::string& path, ProjectDocument& doc, std::string& error);

} // namespace pose

#endif // PROJECTFILE_H
