/**
 * @file figureimportservice.h
 * @brief The Qt-facing orchestration that imports a native figure into the scene.
 *
 * The figure analogue of ModelImportService. It locates the content root for the file,
 * runs the Qt-free FigureImporter (import/figure/) to assemble a FigureData, converts it to the
 * renderer's ModelData (applying the cm->world unit scale), decodes its textures via QImage (reusing
 * ModelImportService::decodeModelTextures), and uploads it — driving a modal progress dialog. As with
 * the model path, all Qt lives here so the importer core stays Qt-free.
 *
 * Phase 1c renders the figure through the existing lit mesh pipeline (diffuse + base color). The
 * richer PBR/skinning path arrives in later phases; this service grows with them.
 */

#ifndef FIGUREIMPORTSERVICE_H
#define FIGUREIMPORTSERVICE_H

#include <QString>

namespace pose {

class VulkanRenderer;

/**
 * @class FigureImportService
 * @brief Stateless helper: load a native figure file into a renderer's scene. See file header.
 */
class FigureImportService {
public:
    /// Loads @p path (a figure preset `.duf` or base `.dsf`) into @p renderer's scene. The content
    /// roots are auto-detected by walking up to the folder that contains `data/`. Logs and returns
    /// false on failure. @p showProgress drives a modal staged dialog (false for the startup drain).
    static bool importInto(VulkanRenderer& renderer, const QString& path, bool showProgress);
};

} // namespace pose

#endif // FIGUREIMPORTSERVICE_H
