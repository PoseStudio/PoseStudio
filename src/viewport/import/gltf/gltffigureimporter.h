// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
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
 * (import/rigged/riggedimport.h). Skinning: THE BIND IS THE NODES AS THE FILE STANDS — glTF
 * keeps no bind pose apart from the skins' inverse bind matrices, and exporters define those
 * against different spaces, so a joint's bind position is its node's world position and a
 * skinned vertex is placed by its joints' world transforms times their inverse bind matrices,
 * blended by weight: what every viewer shows at the default pose (the reasoning is at the
 * skeleton block in the .cpp). A file whose nodes are saved posed imports in that pose; up to
 * four influences per vertex (the first joint and weight sets), normalized. A mesh without a
 * skin rides the nearest bone above its node. Materials: the metallic-roughness model (base color
 * factor and map, the metallic-roughness map's green and blue channels as the roughness and
 * metalness maps, the normal map with its scale, the occlusion and emission maps, the alpha mode
 * as the opacity and the base map's alpha as the cutout), the older specular-glossiness
 * extension converted to it (import/specgloss.h), the factors of the KHR material extensions the
 * canonical material has channels for, and a texture transform on the base map as the material's
 * tiling. A VRM avatar (`.vrm`, a GLB with a humanoid map) has its mapped bones named by the rig
 * standard. Skinless leaf joints (an exporter's end markers) are left out of the skeleton and
 * kept as their parents' tips. Not read: morph targets, vertex colors, animations, influences
 * past the first four, a UV set other than the one the base color map samples; a file that
 * REQUIRES Draco or meshopt compression, or Basis Universal textures, is refused with a message
 * that says so, as is one whose data reaches past its buffers. Qt-free (std + GLM + cgltf).
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
