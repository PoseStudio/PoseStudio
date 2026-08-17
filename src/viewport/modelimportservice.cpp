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

// Bakes a mesh's opacity (cutout) mask into its diffuse map's ALPHA channel — the shader samples
// the diffuse anyway, so the mask rides along without its own texture/binding. An untextured mesh
// gets a white RGBA canvas at the mask's own resolution first. The mask is grayscale-converted and
// rescaled to the diffuse dimensions, so mismatched map sizes are fine. On success the mesh is
// flagged so it renders in the alpha-blended transparent pass (a lash card's scalar opacity is 1 —
// its shape exists only in the mask). Runs BEFORE the gutter fill, whose pull-push dilation covers
// all four channels, so the baked alpha gets the same seam treatment as the colours.
void bakeOpacityMask(MeshData& mesh) {
    QImage mask;
    if (!mesh.opacityMask.encoded.empty()) {
        mask.loadFromData(mesh.opacityMask.encoded.data(),
                          static_cast<int>(mesh.opacityMask.encoded.size()));
    } else {
        mask.load(QString::fromStdString(mesh.opacityMask.path));
    }
    if (mask.isNull()) {
        qWarning() << "[viewport] Opacity mask not found/decodable:"
                   << QString::fromStdString(mesh.opacityMask.path);
        return; // mesh keeps its scalar opacity
    }
    if (mesh.diffusePixels.empty()) {
        mesh.diffuseWidth = static_cast<uint32_t>(mask.width());
        mesh.diffuseHeight = static_cast<uint32_t>(mask.height());
        mesh.diffusePixels.assign(
            static_cast<std::size_t>(mesh.diffuseWidth) * mesh.diffuseHeight * 4, 255);
    }
    const QImage scaled =
        mask.convertToFormat(QImage::Format_Grayscale8)
            .scaled(static_cast<int>(mesh.diffuseWidth), static_cast<int>(mesh.diffuseHeight),
                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (scaled.isNull()) {
        return;
    }
    for (uint32_t y = 0; y < mesh.diffuseHeight; ++y) {
        const uchar* row = scaled.constScanLine(static_cast<int>(y));
        uint8_t* out = mesh.diffusePixels.data() + static_cast<std::size_t>(y) * mesh.diffuseWidth * 4;
        for (uint32_t x = 0; x < mesh.diffuseWidth; ++x) {
            out[x * 4 + 3] = row[x];
        }
    }
    mesh.hasOpacityMask = true;
}

} // namespace

void ModelImportService::decodeMeshTexture(MeshData& mesh) {
    // Diffuse: an undecodable/absent map just leaves the mesh on its base color (white fallback).
    decodeTexture(mesh.diffuse, mesh.diffusePixels, mesh.diffuseWidth, mesh.diffuseHeight);
    // Detail (normal/bump): if it can't be decoded, drop to flat shading (mode 0).
    if (!decodeTexture(mesh.normal, mesh.normalPixels, mesh.normalWidth, mesh.normalHeight)) {
        mesh.normalMode = 0;
    }
    // Opacity (cutout) mask -> the diffuse map's alpha channel (see bakeOpacityMask). Before the
    // gutter fill so the baked alpha shares the seam treatment.
    if (!mesh.opacityMask.empty()) {
        bakeOpacityMask(mesh);
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
