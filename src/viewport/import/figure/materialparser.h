/**
 * @file materialparser.h
 * @brief Parses a figure preset's per-zone physically-based materials.
 *
 * A figure preset assigns one material per polygon_material_group ("Torso", "Face", "Irises", …).
 * Each is a set of shader channels — values and the texture maps that scale them — that only
 * OVERRIDE a base material it extends. This reads each merged material into a FigureMaterial
 * (figuredata.h: every value the canonical material has a place for, drawn in the viewport or
 * stored for the Materials tab and the render engine), keyed by zone name. A map is left as the
 * URI the file wrote; resolving it to a path is the caller's (scenematerials), which holds the
 * content roots. Pure std + GLM + nlohmann — no Qt.
 */

#ifndef MATERIALPARSER_H
#define MATERIALPARSER_H

#include "figuredata.h" // FigureMaterial

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pose {

/// Cross-file material bases the importer pre-resolved for parseMaterials, which itself only knows
/// same-file "#id" references: `extraBases` are base materials lifted from OTHER documents, each
/// under a key that names its file and its id together (the importer's to choose; it must tell two
/// files' bases of one id apart); `crossFileBase` maps a scene material (by address within
/// `sceneMaterials`) whose url names a base in another document to that base's key. Such a
/// material's base is looked for in `extraBases` ALONE — never in the document's own library,
/// whose entry of the same id is another material — and has none where the key is not there (the
/// other document does not define it). Both are views — the pointed-at JSON must outlive the
/// parse. Replaces an older scheme that deep-copied the scene materials and the whole material
/// library to append/rewrite in place.
/// `skip` lists scene materials (by address) the parse must pass over: those a scene of several
/// figures lists for ANOTHER figure's geometry (see FigureScope). `baseIdsAsWritten` compares a
/// same-file base reference with the library ids as the reference spells it, percent-encoding and
/// all — so a base whose id has a space in it is NOT found; see applySceneMaterials for the one
/// caller that asks for that, and why.
struct MaterialBaseOverrides {
    std::unordered_map<std::string, const nlohmann::json*> extraBases;
    std::unordered_map<const nlohmann::json*, std::string> crossFileBase;
    std::unordered_set<const nlohmann::json*>              skip;
    bool                                                   baseIdsAsWritten = false;
};

/// Parses a preset's `scene.materials` into a zone-name -> FigureMaterial map whose map members
/// hold the URIs AS WRITTEN (still root-relative, e.g. "/Runtime/Textures/…"; see FigureMaterial).
/// Each scene material only *overrides* channels on a base material it references by `url`
/// (`#id`); the full channel set (roughness, transparency, …) lives in @p materialLibrary. This
/// merges the two (scene wins), which is essential — e.g. the eye's clear shells get their
/// transparency-defining channels only from the base. A material listing several groups is
/// registered under each. Pass an empty array for @p materialLibrary when there is none (a base
/// `.dsf` imported directly carries no materials). @p overrides (optional) supplies the bases
/// that live in other documents — see MaterialBaseOverrides. A material whose channels cannot be read
/// is left out — its zones keep what they have — and named in @p skipped (optional) with the
/// reason; nothing is thrown for it.
std::unordered_map<std::string, FigureMaterial> parseMaterials(const nlohmann::json& sceneMaterials,
                                                               const nlohmann::json& materialLibrary,
                                                               const nlohmann::json& imageLibrary,
                                                               const MaterialBaseOverrides* overrides = nullptr,
                                                               std::vector<std::string>* skipped = nullptr);

} // namespace pose

#endif // MATERIALPARSER_H
