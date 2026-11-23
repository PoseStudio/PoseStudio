// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file splatimportservice.h
 * @brief The Qt-facing import of a Gaussian splat scene (`.ply` / `.splat` / `.spz`): the
 *        reader (import/splat/splatimporter.h), the progress dialog and the error prompt, and
 *        the upload (VulkanRenderer::addSplatModel). The twin of ModelImportService's OBJ path
 *        for a scene that is neither a mesh nor a figure — static, unlit, drawn by its own pass
 *        (scene/splatcloud.h). Imported the capture's way up: a trained `.ply` / `.splat` scene
 *        is y-DOWN (the camera convention its images were posed in), so its DATA is turned over
 *        (180° about x) and the model transform starts at the identity — the Transform tab's
 *        Figure group reads 0 for the imported orientation and turns, tips and scales it from
 *        there. (An `.spz` is y-up as written.)
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
