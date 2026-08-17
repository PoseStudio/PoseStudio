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

namespace pose {

/// Unresolved material references for one zone: the base-color tint plus raw image_file URIs (still
/// root-relative, e.g. "/Runtime/Textures/…"). The importer resolves the URIs against content roots.
struct MaterialRefs {
    glm::vec3   baseColor{0.75f};
    std::string diffuseImageUri;
    std::string normalImageUri;  ///< "Normal Map" channel (tangent-space RGB), or empty.
    std::string bumpImageUri;    ///< "Bump Strength" channel (grayscale height), or empty.
    std::string opacityImageUri; ///< Cutout/opacity mask (lash & brow cards, legacy eye shells), or empty.
    float       roughness = 0.7f;
    float       opacity   = 1.0f; ///< From Cutout Opacity (or the legacy "transparency" channel),
                                  ///< lowered for refractive (glass) surfaces.
};

/// Parses a preset's `scene.materials` into a zone-name -> MaterialRefs map. Each scene material only
/// *overrides* channels on a base material it references by `url` (`#id`); the full channel set
/// (roughness, transparency, …) lives in @p materialLibrary. This merges the two (scene wins), which
/// is essential — e.g. the eye's clear shells get their transparency-defining channels only from the
/// base. A material listing several groups is registered under each. Pass an empty array for
/// @p materialLibrary when there is none (a base `.dsf` imported directly carries no materials).
std::unordered_map<std::string, MaterialRefs> parseMaterials(const nlohmann::json& sceneMaterials,
                                                             const nlohmann::json& materialLibrary,
                                                             const nlohmann::json& imageLibrary);

} // namespace pose

#endif // MATERIALPARSER_H
