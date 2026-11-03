/**
 * @file fbxfigureimporter.h
 * @brief The FBX figure reader: a rigged (or static) character in an FBX file, read through ufbx
 *        into the renderer's ModelData — the skinned meshes with their materials and textures, and
 *        the RAW skeleton (names, hierarchy, bind positions; identity frames, no limits).
 *
 * What it settles, so that everything downstream can take it for granted as it does for a native
 * figure: metres, Y up, the figure FACING +z with her left at +x (the yaw read off the feet and
 * baked into the geometry and the skeleton), centred over the origin, standing on the floor (the
 * lowest vertex at y = 0). What it leaves to the rig standard's conformer (scene/ik/rigconformer.h):
 * every bone's orientation frame, rotation order and limits — an FBX rig authors none of the
 * limits the posing engine lives by, and its local frames are its authoring tool's.
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

#include <string>
#include <vector>

namespace pose {

/// What the reader found and did — for the import log.
struct FbxImportInfo {
    std::string              sourceAxes;        ///< The file's own axes and unit, as text.
    std::string              skinningMethod;    ///< "linear" / "dual quaternion" / "rigid" / "none".
    float                    heightMetres = 0.0f; ///< The skeleton's (or the mesh's) height after conversion.
    float                    yawDegrees = 0.0f;   ///< The turn applied so the figure faces +z.
    float                    groundedMm = 0.0f;   ///< How far the figure was moved onto the floor.
    int                      bones = 0;
    int                      prunedLeaves = 0;      ///< Skinless leaf bones left out (an exporter's end markers).
    int                      skinnedMeshes = 0;
    int                      rigidMeshes = 0;
    int                      materials = 0;
    int                      embeddedTextures = 0;
    int                      externalTextures = 0;
    int                      missingTextures = 0;
    std::vector<std::string> notes;              ///< Anything worth a line in the log.
};

/**
 * @class FbxFigureImporter
 * @brief Reads one FBX file into a ModelData (see the file comment). Throws std::runtime_error on
 *        a file that cannot be read or holds no mesh.
 */
class FbxFigureImporter {
public:
    ModelData load(const std::string& path, FbxImportInfo* info = nullptr) const;
};

} // namespace pose

#endif // FBXFIGUREIMPORTER_H
