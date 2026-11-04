/**
 * @file riggedimportservice.h
 * @brief The Qt-facing orchestration that imports a character from an interchange file — FBX,
 *        glTF / GLB or USD — into the scene.
 *
 * The analogue of FigureImportService for the formats other tools write: picks the Qt-free
 * reader by the file's extension (import/fbx/, import/gltf/, import/usd/ — each returns the
 * skinned meshes, the materials and textures, the raw skeleton, in metres, facing +z, on the
 * floor), then runs the rig standard's CONFORMER (scene/ik/rigconformer.h), which gives every
 * bone the frame, rotation order and limits of its role so the posing engine poses it as it
 * poses the reference figures, then the shared post-parse pipeline
 * (ModelImportService::uploadModelData: texture decode, AO + tangents, GPU upload) under a modal
 * progress dialog. A file whose skeleton the standard cannot place in full still imports: the
 * roles it found are conformed, the rest keep free, identity-framed channels (posable joint by
 * joint), and the log says what was missing. A file with no skeleton imports as a static model.
 * POSESTUDIO_RIG_NO_CONFORM=1 imports the raw skeleton (the A/B; POSESTUDIO_FBX_NO_CONFORM is
 * honoured too, its older name).
 */

#ifndef RIGGEDIMPORTSERVICE_H
#define RIGGEDIMPORTSERVICE_H

#include <QString>

namespace pose {

class VulkanRenderer;

class RiggedImportService {
public:
    /// Loads @p path (an `.fbx`, `.glb` / `.gltf`, or `.usd` / `.usda` / `.usdc` / `.usdz` file)
    /// into @p renderer's scene. Logs and returns false on failure. @p showProgress drives a modal
    /// staged dialog (false for the startup drain).
    static bool importInto(VulkanRenderer& renderer, const QString& path, bool showProgress);
};

} // namespace pose

#endif // RIGGEDIMPORTSERVICE_H
