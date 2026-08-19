/**
 * @file modelimportservice.cpp
 * @brief Implementation of ModelImportService. See modelimportservice.h.
 */

#include "modelimportservice.h"

#include "aobaker.h"
#include "tangentgen.h"

#include "import/importerregistry.h"
#include "import/meshimporter.h"
#include "import/modeldata.h"
#include "import/texturegutters.h"
#include "parallelfor.h"
#include "rendering/vulkanrenderer.h"

#include <QApplication>
#include <QBuffer>
#include <QDebug>
#include <QElapsedTimer>
#include <QImage>
#include <QImageReader>
#include <QProgressDialog>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pose {

namespace {

// Identity key for a texture source: external files share by path — the point of the whole decode
// cache, since figure zones commonly reference the same atlas files (face/lips/ears share the face
// maps, torso/arms/legs the body maps). Embedded bytes are keyed by their address, i.e. unique to
// their own mesh. Empty key = no source.
std::string sourceKey(const TextureSource& src) {
    if (!src.path.empty()) {
        return src.path;
    }
    if (!src.encoded.empty()) {
        return "embedded:" + std::to_string(reinterpret_cast<std::uintptr_t>(&src));
    }
    return {};
}

// Viewport texture-resolution cap. Characters commonly ship 8192² bump/roughness maps; at 8K each
// costs 4× a 4K map at EVERY stage — decode, gutter fill, staging copy, GPU mip build, and ~341 MB
// of VRAM per map — for pore-level detail the viewport camera can't resolve at working distances.
// Downscaling at decode keeps import fast and VRAM sane (mainstream DCC viewports cap/compress the
// same way); JPEG sources even decode directly at the reduced size (libjpeg DCT scaling). Promote
// to a Preference if a full-res close-up use case appears.
constexpr int kMaxTextureSize = 4096;

// Reads a TextureSource (external path OR embedded bytes) into a QImage, downscaling anything over
// kMaxTextureSize as part of the decode. Returns a null QImage on failure (the caller logs).
// Safe to call from worker threads — QImageReader and the built-in plugins are reentrant.
QImage readCapped(const TextureSource& src) {
    QBuffer buffer;
    QImageReader reader;
    if (!src.encoded.empty()) {
        // qsizetype, not int — an int cast would truncate embedded buffers over 2 GB.
        buffer.setData(reinterpret_cast<const char*>(src.encoded.data()),
                       static_cast<qsizetype>(src.encoded.size()));
        buffer.open(QIODevice::ReadOnly);
        reader.setDevice(&buffer);
    } else {
        reader.setFileName(QString::fromStdString(src.path));
    }
    // size() is a header-only peek for PNG/JPEG; setScaledSize folds the downscale into the decode
    // (a fast DCT-scaled decode for JPEG, decode+smooth-scale for the rest).
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > kMaxTextureSize || size.height() > kMaxTextureSize)) {
        reader.setScaledSize(size.scaled(kMaxTextureSize, kMaxTextureSize, Qt::KeepAspectRatio));
    }
    QImage image = reader.read();
    // Formats whose header size wasn't available still get capped, just less efficiently.
    if (!image.isNull() &&
        (image.width() > kMaxTextureSize || image.height() > kMaxTextureSize)) {
        image = image.scaled(kMaxTextureSize, kMaxTextureSize, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    return image;
}

// Decodes a TextureSource into a tightly-packed RGBA8 image (capped — see kMaxTextureSize).
// Returns null (logged) for an undecodable image.
std::shared_ptr<DecodedImage> decodeSource(const TextureSource& src) {
    QImage image = readCapped(src);
    if (image.isNull()) {
        const QString name = src.path.empty() ? QStringLiteral("<embedded texture>")
                                              : QString::fromStdString(src.path);
        qWarning() << "[viewport] Texture not found/decodable:" << name;
        return nullptr;
    }
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) {
        // convertToFormat can itself fail (allocation failure on a 4K map under memory pressure)
        // — a 0x0 DecodedImage with empty pixels must not reach the GPU upload path.
        qWarning() << "[viewport] Texture conversion failed (out of memory?):"
                   << QString::fromStdString(src.path);
        return nullptr;
    }
    auto out = std::make_shared<DecodedImage>();
    out->width = static_cast<uint32_t>(image.width());
    out->height = static_cast<uint32_t>(image.height());
    const uchar* bits = image.constBits();
    out->pixels.assign(bits, bits + image.sizeInBytes());
    return out;
}

// Decodes an opacity (cutout) mask to grayscale (capped like every other map — it's rescaled to
// the diffuse dimensions at bake anyway). Returns a null QImage (logged) on failure — the mesh
// then keeps its scalar opacity.
QImage decodeMask(const TextureSource& src) {
    QImage mask = readCapped(src);
    if (mask.isNull()) {
        qWarning() << "[viewport] Opacity mask not found/decodable:"
                   << QString::fromStdString(src.path);
        return mask;
    }
    return mask.convertToFormat(QImage::Format_Grayscale8);
}

} // namespace

void ModelImportService::decodeModelTextures(ModelData& data) {
    constexpr std::size_t kNoCombo = static_cast<std::size_t>(-1);

    // ---- 1. Collect every unique image source (and opacity mask) across the model's meshes. ----
    struct SourceJob {
        std::string          key;
        const TextureSource* src = nullptr;
    };
    std::unordered_map<std::string, std::shared_ptr<DecodedImage>> images;
    std::vector<SourceJob>                                         imageJobs;
    auto addImageSource = [&](const TextureSource& src) -> std::string {
        std::string key = sourceKey(src);
        if (!key.empty() && images.emplace(key, nullptr).second) {
            imageJobs.push_back({key, &src});
        }
        return key;
    };
    std::unordered_map<std::string, QImage> masks;
    std::vector<SourceJob>                  maskJobs;
    auto addMaskSource = [&](const TextureSource& src) -> std::string {
        std::string key = sourceKey(src);
        if (!key.empty() && masks.emplace(key, QImage()).second) {
            maskJobs.push_back({key, &src});
        }
        return key;
    };

    struct MeshKeys {
        std::string diffuse, mask, normal, roughness, specMask, translucency, detailNormal;
    };
    std::vector<MeshKeys> keys(data.meshes.size());
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        MeshData& mesh = data.meshes[m];
        keys[m].diffuse = addImageSource(mesh.diffuse);
        keys[m].mask = addMaskSource(mesh.opacityMask);
        keys[m].normal = addImageSource(mesh.normal);
        keys[m].roughness = addImageSource(mesh.roughnessMap);
        keys[m].specMask = addImageSource(mesh.specMask);
        keys[m].translucency = addImageSource(mesh.translucencyMap);
        keys[m].detailNormal = addImageSource(mesh.detailNormalMap);
    }

    // ---- 2. Decode each unique source exactly once, across cores. The serial per-mesh decode of
    // shared 4K atlas maps used to dominate figure import time.
    {
        std::vector<std::shared_ptr<DecodedImage>> imageResults(imageJobs.size());
        std::vector<QImage>                        maskResults(maskJobs.size());
        const int imageCount = static_cast<int>(imageJobs.size());
        parallelFor(imageCount + static_cast<int>(maskJobs.size()), [&](int i) {
            if (i < imageCount) {
                imageResults[static_cast<std::size_t>(i)] =
                    decodeSource(*imageJobs[static_cast<std::size_t>(i)].src);
            } else {
                const std::size_t mi = static_cast<std::size_t>(i - imageCount);
                maskResults[mi] = decodeMask(*maskJobs[mi].src);
            }
        });
        for (std::size_t i = 0; i < imageJobs.size(); ++i) {
            images[imageJobs[i].key] = std::move(imageResults[i]);
        }
        for (std::size_t i = 0; i < maskJobs.size(); ++i) {
            masks[maskJobs[i].key] = std::move(maskResults[i]);
        }
    }

    // ---- 3. Bake each unique (diffuse, mask) pair into its own shared image: the mask (a lash/
    // brow card's shape, an older figure's eye-shell fade) rides in the diffuse map's ALPHA channel,
    // so the shader needs no extra texture/binding. The bake COPIES the diffuse first — the plain
    // shared decode must stay pristine for meshes using it unmasked. Runs before the gutter fill so
    // the baked alpha gets the same seam treatment as the colours.
    struct Combo {
        std::string                   diffuseKey, maskKey;
        std::shared_ptr<DecodedImage> image;
        bool                          masked = false;
    };
    std::vector<Combo>                           combos;
    std::unordered_map<std::string, std::size_t> comboIndex; // "diffuse\x1fmask" -> index
    std::vector<std::size_t>                     comboOfMesh(data.meshes.size(), kNoCombo);
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        if (keys[m].mask.empty()) {
            continue; // no opacity mask — the plain shared diffuse serves as-is
        }
        const std::string ck = keys[m].diffuse + '\x1f' + keys[m].mask;
        const auto [it, inserted] = comboIndex.emplace(ck, combos.size());
        if (inserted) {
            combos.push_back({keys[m].diffuse, keys[m].mask, nullptr, false});
        }
        comboOfMesh[m] = it->second;
    }
    parallelFor(static_cast<int>(combos.size()), [&](int ci) {
        Combo&        combo = combos[static_cast<std::size_t>(ci)];
        const QImage& mask = masks.find(combo.maskKey)->second;
        const std::shared_ptr<DecodedImage> base =
            combo.diffuseKey.empty() ? nullptr : images.find(combo.diffuseKey)->second;
        if (mask.isNull()) {
            combo.image = base; // undecodable mask: the mesh keeps its scalar opacity
            return;
        }
        auto img = std::make_shared<DecodedImage>();
        if (base) {
            *img = *base;
        } else {
            // Untextured mesh: a white RGBA canvas at the mask's own resolution.
            img->width = static_cast<uint32_t>(mask.width());
            img->height = static_cast<uint32_t>(mask.height());
            img->pixels.assign(static_cast<std::size_t>(img->width) * img->height * 4, 255);
        }
        // Rescaled to the diffuse dimensions, so mismatched map sizes are fine. The re-convert
        // after scaling is load-bearing: QImage::scaled() on a Grayscale8 source silently returns
        // RGB32 whenever the size actually changes (Qt's smooth-scale passthrough list excludes
        // no-alpha 8-bit formats), and reading 4-byte BGRX rows as 1-byte gray bakes garbage alpha.
        const QImage scaled =
            mask.scaled(static_cast<int>(img->width), static_cast<int>(img->height),
                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_Grayscale8);
        if (scaled.isNull()) {
            combo.image = base;
            return;
        }
        for (uint32_t y = 0; y < img->height; ++y) {
            const uchar* row = scaled.constScanLine(static_cast<int>(y));
            uint8_t* out = img->pixels.data() + static_cast<std::size_t>(y) * img->width * 4;
            for (uint32_t x = 0; x < img->width; ++x) {
                out[x * 4 + 3] = row[x];
            }
        }
        combo.image = std::move(img);
        combo.masked = true;
    });

    // ---- 4. Assign the shared images + per-slot flags to each mesh. ----------------------------
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        MeshData&       mesh = data.meshes[m];
        const MeshKeys& k = keys[m];
        if (comboOfMesh[m] != kNoCombo) {
            const Combo& combo = combos[comboOfMesh[m]];
            mesh.diffuseImage = combo.image;
            mesh.hasOpacityMask = combo.masked;
        } else if (!k.diffuse.empty()) {
            mesh.diffuseImage = images[k.diffuse];
        }
        if (!k.normal.empty()) {
            mesh.normalImage = images[k.normal];
        }
        if (!mesh.normalImage) {
            mesh.normalMode = 0; // undecodable/absent detail map: drop to flat shading
        }
        if (!k.roughness.empty()) {
            mesh.roughnessImage = images[k.roughness];
        }
        if (!k.specMask.empty()) {
            mesh.specMaskImage = images[k.specMask];
        }
        if (mesh.specMaskImage) {
            // The sampled mask texels now do the scaling the parser's assumed-average discount only
            // guessed at — switch to the material's UNdiscounted specular weight.
            mesh.specularWeight = mesh.specularWeightWithMap;
        }
        if (!k.translucency.empty()) {
            mesh.translucencyImage = images[k.translucency];
        }
        if (!k.detailNormal.empty()) {
            mesh.detailNormalImage = images[k.detailNormal];
        }
        if (!mesh.detailNormalImage) {
            mesh.detailWeight = 0.0f; // undecodable map: no detail layer
        }
    }

    // ---- 5. Gutter-fill each unique image ONCE, against the union of its users' UV islands, so
    // the GPU-built mip chain never averages the atlas background through a seam (see
    // texturegutters.h). A shared atlas must keep every user's islands intact, which is exactly
    // what the union coverage provides. The micro-detail (pore) normal is excluded — it tiles/
    // wraps, leaving no gutters. Serial over images; the fill itself is parallel inside.
    {
        std::unordered_map<DecodedImage*, std::vector<UvMeshRef>> usersOf;
        auto noteUser = [&](const std::shared_ptr<DecodedImage>& img, const MeshData& mesh) {
            if (img && mesh.indices.size() >= 3 && !mesh.vertices.empty()) {
                usersOf[img.get()].push_back({&mesh.vertices, &mesh.indices});
            }
        };
        for (std::size_t m = 0; m < data.meshes.size(); ++m) {
            const MeshData& mesh = data.meshes[m];
            const MeshKeys& k = keys[m];
            if (comboOfMesh[m] != kNoCombo) {
                noteUser(combos[comboOfMesh[m]].image, mesh);
            } else if (!k.diffuse.empty()) {
                noteUser(images[k.diffuse], mesh);
            }
            if (!k.normal.empty()) {
                noteUser(images[k.normal], mesh);
            }
            if (!k.roughness.empty()) {
                noteUser(images[k.roughness], mesh);
            }
            if (!k.specMask.empty()) {
                noteUser(images[k.specMask], mesh);
            }
            if (!k.translucency.empty()) {
                noteUser(images[k.translucency], mesh);
            }
        }
        for (auto& [image, users] : usersOf) {
            fillImageGutters(image->pixels, image->width, image->height, users);
        }
    }
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

        const int total = 5; // parse, decode, bake, upload, done
        int step = 0;
        if (progress) {
            progress->setRange(0, total); // switch the busy indicator to a determinate bar
            progress->setValue(++step);   // parse complete
        }

        // Decode every unique texture once — shared across the meshes that sample it — with the
        // decodes running across cores (see decodeModelTextures). Qt layer, so the importers and
        // the renderer stay codec-free.
        if (progress) {
            progress->setLabelText(QStringLiteral("Decoding textures…"));
            progress->setValue(++step);
        }
        decodeModelTextures(data);

        const qint64 msDecode = timer.elapsed();

        // Bake per-vertex ambient occlusion (skipped internally for very large meshes) + tangents.
        if (progress) {
            progress->setLabelText(QStringLiteral("Baking ambient occlusion…"));
            progress->setValue(++step);
        }
        bakeVertexAO(data);
        computeTangents(data);

        // Show "Uploading…" below the max so the dialog stays up during the blocking GPU upload,
        // then jump to the max afterwards to auto-close it.
        if (progress) {
            progress->setLabelText(QStringLiteral("Uploading to GPU…"));
            progress->setValue(++step);
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
