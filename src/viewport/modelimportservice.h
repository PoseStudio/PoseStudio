// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file modelimportservice.h
 * @brief The Qt-facing orchestration that turns a file path into a model in the scene.
 *
 * This is the seam between the Qt-free import/ subsystem and the Qt-free renderer core. It:
 *   1. picks the right importer for the file (ImporterRegistry),
 *   2. parses it into a format-neutral ModelData (MeshImporter::load),
 *   3. decodes each mesh's texture maps via QImage — from external paths OR embedded bytes — into
 *      tightly-packed shared RGBA8 images (every texture slot), baking opacity masks into the
 *      diffuse alpha and gutter-filling UV seams, so the renderer never needs an image codec
 *      (texturedecode.h),
 *   4. smooths by angle a mesh whose file brings no smoothing of its own (import/smoothnormals.h),
 *   5. bakes per-vertex ambient occlusion + tangents on the final mesh, and
 *   6. uploads it (VulkanRenderer::addModel),
 * optionally driving a modal staged progress dialog through those phases (importprogress.h).
 *
 * Steps 3-6 — everything after the format-specific parse — are the SAME for every source, so
 * they live here once (uploadModelData) and the figure and rigged-character paths
 * (FigureImportService, RiggedImportService) call them after their own parse: any new import
 * path must go through them too, or the decode-once texture sharing and the seam-killing gutter
 * fill are lost.
 *
 * All the Qt of an import (the QImage decode, the progress UI) lives in this layer so both the
 * importers and the renderer stay Qt-free. VulkanWindow just forwards to it. Import runs
 * synchronously on the GUI thread.
 */

#ifndef MODELIMPORTSERVICE_H
#define MODELIMPORTSERVICE_H

#include "importprogress.h" // ImportProgress (every import service shows one)

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

    /// The post-parse half of every import: decodes @p data's textures (decodeModelTextures),
    /// smooths a faceted mesh by angle (applyImportSmoothing; POSESTUDIO_NO_IMPORT_SMOOTHING is
    /// the A/B), bakes per-vertex AO + tangents on the final mesh, uploads it to @p renderer —
    /// throwing when the scene takes none of it (a file with no face) — and logs the
    /// per-phase timing as "[viewport] imported <kindPrefix><path> in ... (parse, decode,
    /// upload)<logSuffix>" — @p kindPrefix is "" for a model, "figure " for a figure; @p logSuffix
    /// trails the line (the figure's zone/corrective counts). @p progress must have had parsed()
    /// called. Throws on Vulkan failure too (the caller's catch reports it).
    static void uploadModelData(VulkanRenderer& renderer, ModelData& data, ImportProgress& progress,
                                const char* kindPrefix, const QString& path,
                                const QString& logSuffix = QString());
};

} // namespace pose

#endif // MODELIMPORTSERVICE_H
