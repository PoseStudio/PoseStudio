/**
 * @file figureimporter.h
 * @brief Imports a native figure (a character preset .duf, or a base figure .dsf) into a FigureData.
 *
 * This is the top-level orchestrator of the import/figure/ subsystem. Given a file and the content-
 * library roots, it: loads the preset, locates and loads the referenced base figure geometry, bakes
 * the dialed character morphs (with joint-center adjustment), parses the skeleton + per-vertex skin
 * weights, discovers the pose correctives, resolves and parses the UV set, subdivides the cage,
 * assembles the per-zone meshes, attaches each zone's material, and merges any post-load follower
 * addons. Unlike the static MeshImporter (which returns ModelData and needs no external context), a
 * figure spans many files and needs the content roots to resolve its cross-file URIs — so it is its
 * own class, invoked directly by the Qt import service rather than through ImporterRegistry.
 * Pure std + GLM + nlohmann — no Qt, no Vulkan.
 */

#ifndef FIGUREIMPORTER_H
#define FIGUREIMPORTER_H

#include "figuredata.h"

#include <string>
#include <vector>

namespace pose {

/**
 * @class FigureImporter
 * @brief Assembles a FigureData from a native figure file and the content roots.
 */
class FigureImporter {
public:
    /// Imports @p path (a `.duf` preset or a base `.dsf`). @p contentRoots are the library roots the
    /// figure's `/data/...` and `/Runtime/...` URIs resolve against (each directly containing the
    /// `data/` folder). Throws std::runtime_error on IO/parse failure or if the base geometry can't
    /// be located/resolved. Texture paths that don't resolve are left empty (the zone renders with
    /// its base color), so a missing map degrades gracefully rather than failing the import.
    FigureData load(const std::string& path, const std::vector<std::string>& contentRoots) const;
};

} // namespace pose

#endif // FIGUREIMPORTER_H
