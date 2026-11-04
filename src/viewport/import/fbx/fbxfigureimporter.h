/**
 * @file fbxfigureimporter.h
 * @brief The FBX figure reader: a rigged (or static) character in an FBX file, read through ufbx
 *        into the renderer's ModelData — the skinned meshes with their materials and textures, and
 *        the RAW skeleton (names, hierarchy, bind positions; identity frames, no limits).
 *
 * What it settles, so that everything downstream can take it for granted as it does for a native
 * figure: metres, Y up, the figure FACING +z with her left at +x (the yaw read off the feet and
 * baked into the geometry and the skeleton), centred over the origin, standing on the floor (the
 * lowest vertex at y = 0) — the last three through the step every rigged-character reader shares
 * (import/rigged/riggedimport.h). What it leaves to the rig standard's conformer
 * (scene/ik/rigconformer.h): every bone's orientation frame, rotation order and limits — an FBX
 * rig authors none of the limits the posing engine lives by, and its local frames are its
 * authoring tool's.
 *
 * Skinning: a mesh with a skin deformer takes up to four influences per vertex from the clusters,
 * its vertices placed at their BIND positions (the clusters' bind matrices applied to the mesh's
 * geometry, blended by weight — exact at bind), so a file saved mid-pose still imports at rest. A
 * mesh without one rides the bone it is parented to. Textures: embedded content comes as bytes,
 * a file as its path — the file's own path, the path relative to the FBX, or a file of that name
 * anywhere under the FBX's folder (exports put their textures in a folder tree beside the file).
 * Qt-free (std + GLM + ufbx).
 */

#ifndef FBXFIGUREIMPORTER_H
#define FBXFIGUREIMPORTER_H

#include "modeldata.h"
#include "riggedimport.h"

#include <string>

namespace pose {

/**
 * @class FbxFigureImporter
 * @brief Reads one FBX file into a ModelData (see the file comment). Throws std::runtime_error on
 *        a file that cannot be read or holds no mesh.
 */
class FbxFigureImporter {
public:
    ModelData load(const std::string& path, RiggedImportInfo* info = nullptr) const;
};

} // namespace pose

#endif // FBXFIGUREIMPORTER_H
