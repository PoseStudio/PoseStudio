/**
 * @file modelimportservice.h
 * @brief The Qt-facing orchestration that turns a file path into a model in the scene.
 *
 * This is the seam between the Qt-free import/ subsystem and the Qt-free renderer core. It:
 *   1. picks the right importer for the file (ImporterRegistry),
 *   2. parses it into a format-neutral ModelData (MeshImporter::load),
 *   3. decodes each mesh's diffuse texture via QImage — from an external path OR embedded bytes —
 *      into tightly-packed RGBA8, so the renderer never needs an image codec,
 *   4. uploads it (VulkanRenderer::addModel),
 * optionally driving a modal staged progress dialog through those phases.
 *
 * All the Qt (QImage decode, progress UI) lives here so both the importers and the renderer stay
 * Qt-free. VulkanWindow just forwards to it. Import runs synchronously on the GUI thread.
 */

#ifndef MODELIMPORTSERVICE_H
#define MODELIMPORTSERVICE_H

#include <QString>

namespace pose {

class VulkanRenderer;
struct ModelData;

/**
 * @class ModelImportService
 * @brief Stateless helper: load a model file into a renderer's scene. See file header.
 */
class ModelImportService {
public:
    /**
     * Loads @p path into @p renderer's scene. Logs and returns false on unsupported format or
     * parse/decode failure (a bad file drops only that import, never the app).
     * @param showProgress  When true (interactive imports), drives a modal staged QProgressDialog.
     *   Must be false for the startup/command-line drain called from inside the expose/init path,
     *   where the modal dialog's processEvents() could re-enter it.
     * @return true if a model was uploaded to the scene.
     */
    static bool importInto(VulkanRenderer& renderer, const QString& path, bool showProgress);

    /// Decodes every mesh's TextureSources (external paths OR embedded bytes) into shared
    /// DecodedImages via QImage, bakes opacity masks into the diffuse alpha, and gutter-fills each
    /// image against the union of its users' UVs. Each unique source is decoded ONCE (figure zones
    /// commonly share atlas files) and the decodes run across cores. Logs and leaves a mesh slot
    /// untextured if its image can't be decoded. Shared by the model and figure import paths so
    /// both decode textures identically — keep every import path behind this choke point, or the
    /// mip-bleed seams the gutter fill prevents come back.
    static void decodeModelTextures(ModelData& data);
};

} // namespace pose

#endif // MODELIMPORTSERVICE_H
