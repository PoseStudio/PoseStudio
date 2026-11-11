/**
 * @file usdfigureimporter.h
 * @brief The USD figure reader: a rigged (or static) character in a Universal Scene Description
 *        file — ASCII (`.usda`), binary crate (`.usdc` / `.usd`) or a USDZ package (`.usdz`, the
 *        textures inside it) — read through LightUSD (the library formerly TinyUSDZ) and its
 *        render-scene layer into the renderer's ModelData: the skinned meshes with their
 *        materials and textures, and the RAW skeleton (names, hierarchy, bind positions; identity
 *        frames, no limits) that the rig standard's conformer completes.
 *
 * What it settles: the stage's `metersPerUnit` (the format's own fallback, centimetres, when the
 * file authors none) and `upAxis` (a Z-up stage is turned Y-up) into metres, Y up; then the
 * facing, the centring and the floor that every rigged-character reader shares
 * (import/rigged/riggedimport.h). Skinning: UsdSkel — the meshes' joint indices and weights (up
 * to four per vertex, normalized), their vertices placed at their BIND positions through the
 * geometry bind transform and the skeleton's own transform, the joints' bind frames the
 * Skeleton's world-space `bindTransforms`; one skeleton per file (the one with the most joints
 * when a file has several; the others' meshes come in rigid). A mesh without a binding is rigid,
 * on the figure's root bone. Materials: UsdPreviewSurface — diffuse color and map, roughness
 * (value or map, whichever channel the map's output names), metalness, the normal map, opacity
 * (the base map's alpha when that is what the opacity reads). A USDZ's textures are read out of
 * the package; a plain file's are found beside it. Skinless leaf joints are left out of the
 * skeleton and kept as their parents' tips. Not read: blend shapes, animation, point instancers,
 * MaterialX materials (a mesh with one renders untextured). Qt-free (std + GLM + LightUSD).
 */

#ifndef USDFIGUREIMPORTER_H
#define USDFIGUREIMPORTER_H

#include "modeldata.h"
#include "riggedimport.h"

#include <string>

namespace pose {

/**
 * @class UsdFigureImporter
 * @brief Reads one `.usd` / `.usda` / `.usdc` / `.usdz` file into a ModelData (see the file
 *        comment). Throws std::runtime_error on a file that cannot be read or holds no mesh.
 */
class UsdFigureImporter {
public:
    ModelData load(const std::string& path, RiggedImportInfo* info = nullptr) const;
};

} // namespace pose

#endif // USDFIGUREIMPORTER_H
