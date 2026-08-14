/**
 * @file modelimportservice.cpp
 * @brief Implementation of ModelImportService. See modelimportservice.h.
 */

#include "modelimportservice.h"

#include "import/importerregistry.h"
#include "import/meshimporter.h"
#include "import/modeldata.h"
#include "import/texturegutters.h"
#include "rendering/vulkanrenderer.h"

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QImage>
#include <QProgressDialog>

#include <exception>
#include <memory>

namespace pose {

namespace {

// Decodes a TextureSource (external path OR embedded bytes) into tightly-packed RGBA8 via QImage.
// Returns false (leaving the outputs untouched) for an empty source or an undecodable image.
bool decodeTexture(const TextureSource& src, std::vector<uint8_t>& outPixels, uint32_t& outWidth,
                   uint32_t& outHeight) {
    if (src.empty()) {
        return false;
    }
    QImage image;
    if (!src.encoded.empty()) {
        image.loadFromData(src.encoded.data(), static_cast<int>(src.encoded.size()));
    } else {
        image.load(QString::fromStdString(src.path));
    }
    if (image.isNull()) {
        const QString name = src.path.empty() ? QStringLiteral("<embedded texture>")
                                              : QString::fromStdString(src.path);
        qWarning() << "[viewport] Texture not found/decodable:" << name;
        return false;
    }
    image = image.convertToFormat(QImage::Format_RGBA8888);
    outWidth = static_cast<uint32_t>(image.width());
    outHeight = static_cast<uint32_t>(image.height());
    const uchar* bits = image.constBits();
    outPixels.assign(bits, bits + image.sizeInBytes());
    return true;
}

} // namespace

void ModelImportService::decodeMeshTexture(MeshData& mesh) {
    // Diffuse: an undecodable/absent map just leaves the mesh on its base color (white fallback).
    decodeTexture(mesh.diffuse, mesh.diffusePixels, mesh.diffuseWidth, mesh.diffuseHeight);
    // Detail (normal/bump): if it can't be decoded, drop to flat shading (mode 0).
    if (!decodeTexture(mesh.normal, mesh.normalPixels, mesh.normalWidth, mesh.normalHeight)) {
        mesh.normalMode = 0;
    }
    // Dilate each image's UV-island colours into its unused background so the GPU-built mip chain
    // never averages the atlas background through a seam (visible as bright seam lines when zoomed
    // out; see texturegutters.h). Runs after decode, before upload — covered texels are untouched.
    fillTextureGutters(mesh);
}

bool ModelImportService::importInto(VulkanRenderer& renderer, const QString& path,
                                    bool showProgress) {
    // Pick the importer for this file up front — if the format is unsupported, bail before we put
    // any progress UI on screen.
    const MeshImporter* importer = ImporterRegistry::instance().forPath(path.toStdString());
    if (!importer) {
        qWarning() << "[viewport] No importer for file (unsupported format):" << path;
        return false;
    }

    // Import runs synchronously on the GUI thread (parse -> texture decode -> GPU upload), so a
    // large model would otherwise freeze the UI with no feedback. Drive a modal staged progress
    // dialog through those phases. It lives entirely in this Qt-facing layer; the importers and the
    // renderer core stay Qt-free. No cancel button: each phase is a short, atomic, blocking operation
    // that can't be safely interrupted partway through. A modal QProgressDialog pumps the event loop
    // on setValue(), so the bar/label repaint between phases.
    std::unique_ptr<QProgressDialog> progress;
    if (showProgress) {
        progress = std::make_unique<QProgressDialog>(
            QStringLiteral("Reading model file…"), QString() /*no cancel button*/, 0, 0,
            QApplication::activeWindow());
        progress->setWindowTitle(QStringLiteral("Importing Model"));
        progress->setWindowModality(Qt::ApplicationModal);
        progress->setMinimumDuration(0); // don't wait for the default ~4s estimate before showing
        progress->setValue(0);           // range 0,0 => a busy indicator while the file parses
        // QProgressDialog's auto-show is driven by setValue() estimating the remaining time, which
        // can't see the upcoming blocking load() — so it would otherwise first appear mid-import.
        // Force it on-screen and paint it NOW, before the parse blocks the GUI thread.
        progress->show();
        QApplication::processEvents();
    }

    QElapsedTimer timer;
    timer.start();
    try {
        ModelData data = importer->load(path.toStdString()); // geometry + resolved texture sources
        const qint64 msParse = timer.elapsed();

        // Now the mesh count is known: 1 read tick + one decode tick per mesh + 1 upload tick.
        const int meshCount = static_cast<int>(data.meshes.size());
        const int total = meshCount + 2;
        int step = 0;
        if (progress) {
            progress->setRange(0, total); // switch the busy indicator to a determinate bar
            progress->setValue(++step);   // parse complete
        }

        // Decode each diffuse texture here (Qt layer) so the importers/renderer stay codec-free. The
        // source may be an external file path (OBJ map_Kd) or bytes embedded in the model file (e.g.
        // a texture packed into a .glb) — QImage handles both.
        for (MeshData& mesh : data.meshes) {
            if (progress) {
                progress->setLabelText(QStringLiteral("Decoding textures…"));
                progress->setValue(++step);
            }
            decodeMeshTexture(mesh);
        }

        const qint64 msDecode = timer.elapsed();

        // Show "Uploading…" at total-1 (still below the max) so the dialog stays up during the
        // blocking GPU upload, then jump to the max afterwards to auto-close it.
        if (progress) {
            progress->setLabelText(QStringLiteral("Uploading to GPU…"));
            progress->setValue(total - 1);
        }
        renderer.addModel(data);
        const qint64 msUpload = timer.elapsed();
        if (progress) {
            progress->setValue(total); // complete -> dialog closes
        }
        // Per-phase import timing — for high-res maps the texture decode/upload dominates.
        qDebug().nospace() << "[viewport] imported " << path << " in " << msUpload
                           << "ms (parse " << msParse << ", decode " << (msDecode - msParse)
                           << ", upload " << (msUpload - msDecode) << ")";
        return true;
    } catch (const std::exception& e) {
        qWarning() << "[viewport] Model import failed:" << path << "-" << e.what();
        return false;
    }
}

} // namespace pose
