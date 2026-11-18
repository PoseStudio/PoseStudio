/**
 * @file modelimportservice.cpp
 * @brief Implementation of ModelImportService. See modelimportservice.h.
 */

#include "modelimportservice.h"

#include "texturedecode.h"

#include "aobaker.h"
#include "smoothnormals.h"
#include "tangentgen.h"

#include "import/importerregistry.h"
#include "import/meshimporter.h"
#include "import/modeldata.h"
#include "rendering/vulkanrenderer.h"
#include "scene/model.h"
#include "scene/scene.h"

#include <QDebug>
#include <QObject>

#include <exception>
#include <stdexcept>
#include <string>

namespace pose {

// --- The shared post-parse pipeline -----------------------------------------------------------

void ModelImportService::uploadModelData(VulkanRenderer& renderer, ModelData& data,
                                         ImportProgress& progress, const char* kindPrefix,
                                         const QString& path, const QString& logSuffix) {
    // Decode every unique texture once — shared across the meshes that sample it — with the
    // decodes running across cores (see decodeModelTextures). Qt layer, so the importers and
    // the renderer stay codec-free.
    progress.phase(QObject::tr("Decoding textures…"));
    decodeModelTextures(data);
    const qint64 msDecode = progress.elapsedMs();

    // THE SMOOTHING's default (smoothnormals.h): a mesh whose file brings no smoothing of its own
    // — a normal per face, or none — is smoothed by angle, before the occlusion and the tangents
    // are made for it. (POSESTUDIO_NO_IMPORT_SMOOTHING: every file as it is, the A/B.)
    static const bool noImportSmoothing = qEnvironmentVariableIsSet("POSESTUDIO_NO_IMPORT_SMOOTHING");
    if (!noImportSmoothing) {
        applyImportSmoothing(data);
    }

    // Bake per-vertex ambient occlusion (skipped internally for very large meshes; parallel
    // across cores) + UV tangents on the FINAL render mesh — post-morph, post-subdivision, all
    // zones together. See aobaker.h / tangentgen.h.
    progress.phase(QObject::tr("Baking ambient occlusion…"));
    bakeVertexAO(data);
    computeTangents(data);

    // Show "Uploading…" below the max so the dialog stays up during the blocking GPU upload,
    // then jump to the max afterwards to auto-close it.
    progress.phase(QObject::tr("Uploading to GPU…"));
    if (!renderer.addModel(data)) {
        // Nothing was added (Scene::addModel): reported like any failed import — and the source
        // path below must not be stamped on the model that was imported before this one.
        throw std::runtime_error(
            QObject::tr("The file holds nothing that can be drawn: no mesh in it has a face.").toStdString());
    }
    // Remember where this model came from — the .pss project file references its figures by this
    // path (the importer is the only place that knows it).
    Scene& scene = renderer.scene();
    if (scene.modelCount() > 0) {
        scene.modelAt(scene.modelCount() - 1)->setSourcePath(path.toStdString());
    }
    const qint64 msUpload = progress.elapsedMs();
    progress.done();

    // Per-phase import timing — for high-res maps the texture decode/upload dominates.
    const qint64 msParse = progress.parseMs();
    qDebug().nospace() << "[viewport] imported " << kindPrefix << path << " in " << msUpload
                       << "ms (parse " << msParse << ", decode " << (msDecode - msParse)
                       << ", upload " << (msUpload - msDecode) << ")" << qUtf8Printable(logSuffix);
    for (const std::string& note : data.notes) {
        qInfo("[viewport]   %s", note.c_str()); // a reader's note: a conversion it made, a channel it could not read
    }
}

bool ModelImportService::importInto(VulkanRenderer& renderer, const QString& path,
                                    bool showProgress) {
    // Pick the importer for this file up front — if the format is unsupported, bail before we put
    // any progress UI on screen.
    const QString       failed = QObject::tr("Import Model"); // (the failure box's title)
    const MeshImporter* importer = ImporterRegistry::instance().forPath(path.toStdString());
    if (!importer) {
        qWarning() << "[viewport] No importer for file (unsupported format):" << path;
        reportImportFailure(showProgress, failed, path, QObject::tr("PoseStudio has no importer for this kind of file."));
        return false;
    }

    try {
        // (The progress dialog lives INSIDE the try: it is gone before a failure is reported.)
        ImportProgress progress(showProgress, QObject::tr("Importing Model"),
                                QObject::tr("Reading model file…"));
        ModelData data = importer->load(path.toStdString()); // geometry + resolved texture sources
        progress.parsed();
        uploadModelData(renderer, data, progress, "", path);
        return true;
    } catch (const std::exception& e) {
        // An interactive import SAYS that it failed and why (a file the reader refuses, a file
        // with nothing to draw): until this was reported, File → Import of such a file flashed
        // the progress dialog and then nothing, the reason in the log alone.
        qWarning() << "[viewport] Model import failed:" << path << "-" << e.what();
        reportImportFailure(showProgress, failed, path, QString::fromUtf8(e.what()));
        return false;
    }
}

} // namespace pose
