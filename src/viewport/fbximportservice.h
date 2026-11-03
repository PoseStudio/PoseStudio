/**
 * @file fbximportservice.h
 * @brief The Qt-facing orchestration that imports a character from an FBX file into the scene.
 *
 * The FBX analogue of FigureImportService: runs the Qt-free FbxFigureImporter (import/fbx/) — the
 * skinned meshes, the materials and textures, the raw skeleton, in metres, facing +z, on the floor
 * — then the rig standard's CONFORMER (scene/ik/rigconformer.h), which gives every bone the frame,
 * rotation order and limits of its role so the posing engine poses it as it poses the reference
 * figures, then the shared post-parse pipeline (ModelImportService::uploadModelData: texture
 * decode, AO + tangents, GPU upload) under a modal progress dialog. A file whose skeleton the
 * standard cannot place in full still imports: the roles it found are conformed, the rest keep
 * free, identity-framed channels (posable joint by joint), and the log says what was missing.
 * POSESTUDIO_FBX_NO_CONFORM=1 imports the raw skeleton (the A/B).
 */

#ifndef FBXIMPORTSERVICE_H
#define FBXIMPORTSERVICE_H

#include <QString>

namespace pose {

class VulkanRenderer;

class FbxImportService {
public:
    /// Loads @p path (an `.fbx` file) into @p renderer's scene. Logs and returns false on failure.
    /// @p showProgress drives a modal staged dialog (false for the startup drain).
    static bool importInto(VulkanRenderer& renderer, const QString& path, bool showProgress);
};

} // namespace pose

#endif // FBXIMPORTSERVICE_H
