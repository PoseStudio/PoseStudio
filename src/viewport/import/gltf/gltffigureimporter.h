/**
 * @file gltffigureimporter.h
 * @brief The glTF figure reader: a rigged (or static) character in a glTF 2.0 file — binary
 *        (`.glb`) or JSON (`.gltf` with its `.bin` and images beside it, or embedded as data
 *        URIs) — read through cgltf into the renderer's ModelData: the skinned meshes with their
 *        materials and textures, and the RAW skeleton (names, hierarchy, bind positions;
 *        identity frames, no limits) that the rig standard's conformer completes.
 *
 * glTF already is metres, right-handed and Y up, so the reader converts nothing but the facing,
 * the centring and the floor, which every rigged-character reader shares
 * (import/rigged/riggedimport.h). Skinning: a skinned mesh's vertices are its BIND positions by
 * the format's own definition (the skin's inverse bind matrices map them into the joints), so a
 * file saved mid-animation imports at rest; a joint's bind frame is the inverse of its inverse
 * bind matrix; up to four influences per vertex (the first joint and weight sets), normalized.
 * A mesh without a skin rides the nearest bone above its node. Materials: the metallic-roughness
 * model (base color factor and map, the metallic-roughness map's green channel as the roughness
 * map, the normal map with its scale, the alpha mode as the opacity and the base map's alpha as
 * the cutout) with the older specular-glossiness extension as the fallback. Skinless leaf joints
 * (an exporter's end markers) are left out of the skeleton and kept as their parents' tips.
 * Not read: morph targets, vertex colors, animations, texture transforms, the second UV set;
 * a file that REQUIRES Draco or meshopt compression, or Basis Universal textures, is refused
 * with a message that says so. Qt-free (std + GLM + cgltf).
 */

#ifndef GLTFFIGUREIMPORTER_H
#define GLTFFIGUREIMPORTER_H

#include "modeldata.h"
#include "riggedimport.h"

#include <string>

namespace pose {

/**
 * @class GltfFigureImporter
 * @brief Reads one `.glb` / `.gltf` file into a ModelData (see the file comment). Throws
 *        std::runtime_error on a file that cannot be read or holds no mesh.
 */
class GltfFigureImporter {
public:
    ModelData load(const std::string& path, RiggedImportInfo* info = nullptr) const;
};

} // namespace pose

#endif // GLTFFIGUREIMPORTER_H
