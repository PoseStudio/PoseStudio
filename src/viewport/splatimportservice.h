/**
 * @file splatimportservice.h
 * @brief The Qt-facing import of a Gaussian splat scene (`.ply` / `.splat`): the reader
 *        (import/splat/splatimporter.h), the progress dialog and the error prompt, and the
 *        upload (VulkanRenderer::addSplatModel). The twin of ModelImportService's OBJ path for
 *        a scene that is neither a mesh nor a figure — static, unlit, drawn by its own pass
 *        (scene/splatcloud.h). Imported the capture's way up: a trained scene is almost always
 *        y-DOWN (the camera convention its images were posed in), so the model transform starts
 *        turned over (180° about x); the Transform tab's Figure group turns, tips and scales it
 *        from there.
 */

#ifndef SPLATIMPORTSERVICE_H
#define SPLATIMPORTSERVICE_H

#include <QString>

namespace pose {

class VulkanRenderer;

class SplatImportService {
public:
    /// Reads @p path and adds the splat scene to @p renderer's scene; with @p showProgress a
    /// modal progress dialog and an error prompt (the interactive path). True if a model was
    /// added. GUI thread only.
    static bool importInto(VulkanRenderer& renderer, const QString& path, bool showProgress);
};

} // namespace pose

#endif // SPLATIMPORTSERVICE_H
