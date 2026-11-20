/**
 * @file vulkanwindow_materialfile.cpp
 * @brief VulkanWindow's side of a SAVED MATERIAL (scene/materialfile.h): the selected zone's
 *        material written to a `.psm` file, the picture of it on a ball that becomes the file's
 *        icon, and a saved material put on the selected zone.
 *        (A TU of VulkanWindow — see vulkanwindow.h for the class and its other TUs.)
 *
 * SAVING (saveMaterialFile) writes the zone's values and, for every slot that holds a map, the
 * map's file — by THE STANDARD of assets/assetstore.h: an asset's textures lie in a `.textures`
 * folder beside it and are named by paths relative to it, so the material's folder is all the
 * material needs. A map that already lies beside the material (or in its folder's store) is
 * named where it is; any other file is COPIED into `.textures/<material>/`, unless the same
 * bytes are in the store already; an image that has no file of its own — one that lives inside
 * the model's file (a GLB's), or ONE CHANNEL of a packed map (a glTF roughness in the green of
 * its metallic-roughness image: a chosen file is read from its red) — is WRITTEN there as a
 * PNG. Told not to copy (Preferences > Assets), the save names each file where it is — relative
 * if in or under the material's folder, else absolute — and leaves an image without a file out.
 * Overlays are an object's, not a surface's, and are never saved.
 *
 * THE ICON (renderMaterialIcon) is the material on a ball under a FIXED light — the ICON STUDIO
 * of scene/materialball.h, a room made for what a ball should show, never the scene's light nor
 * a panorama of the user's library — so every icon of a library is lit alike. Where there is a
 * render engine the ball is path traced through the same capture a render of the scene uses
 * (captureZoneMaterial: the same values and maps), with nothing behind it; where there is none,
 * materialball.h's built-in shader draws a plainer one. Either way the result is a square
 * picture with a transparent background.
 *
 * APPLYING (applyMaterialFile) is one undoable material edit on the zone the Materials tab
 * shows: every slot becomes what the file says (a slot the file does not list is emptied — the
 * file is the whole surface), then the values. The zone's overlays stay. A map that is not
 * where the file says is LOOKED FOR (AssetStore::TextureResolver): by its name beside the
 * material, in its store, and through the library the material lies in. The slots change as
 * one batch (applySlotSources), so a packed binding is made once for all of its slots.
 */

#include "vulkanwindow.h"

#include "assetstore.h"
#include "environment.h"
#include "environmentsource.h" // loadEnvironmentImage (the POSESTUDIO_MATERIAL_ICON_HDRI lever)
#include "ikscript.h"
#include "importformats.h"
#include "librarypaths.h"
#include "lightingsettings.h"
#include "materialball.h"
#include "materialfile.h"
#include "materialparams.h"
#include "materialxfile.h"
#include "shaderpresetfile.h"
#include "uriresolver.h" // a shader preset's maps: content-root-relative URIs
#include "figureutils.h" // urlDecode
#include "colorcurves.h"
#include "assetlibraries.h"
#include "preferencesmanager.h"
#include "constants.h"
#include "model.h"
#include "renderexport.h"
#include "rendersession.h"
#include "rendersnapshot.h"
#include "scene.h"
#include "vulkanrenderer.h"

#include <QBuffer>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSaveFile>
#include <QTimer>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pose {

namespace {

/// The ball: half a metre across at the origin, seen through a long lens (a short one bulges a
/// ball's reflections toward its rim).
constexpr float kBallRadius = 0.25f;
constexpr float kBallFovDeg = 18.0f;
/// The engine renders the icon at this many times its size and it is scaled down: cleaner
/// edges than the samples alone give, and an even size for the engine whatever the icon's.
constexpr int kIconSupersample = 2;
constexpr int kIconSamples = 192;
constexpr bool kIconDenoise = true;
/// A render that has not ended by now is given up on (the built-in shader then draws the icon).
constexpr int kIconTimeoutMs = 120000;

bool samePath(const QString& a, const QString& b) {
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

/// A slot's decoded image as the bytes of a PNG file, for a map that has no file of its own to
/// be saved with a material: the VALUE channel alone, as gray, where the slot reads one channel
/// of a packed image (a file chosen for a slot is read from its red); else the image as it is —
/// without its alpha where that is opaque throughout.
QByteArray slotImageAsPng(const DecodedImage& image, int channel) {
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    if (width <= 0 || height <= 0 || image.pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u) {
        return {};
    }
    QImage out;
    if (channel > 0 && channel < 4) {
        out = QImage(width, height, QImage::Format_Grayscale8);
        for (int y = 0; y < height; ++y) {
            const uint8_t* source = &image.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u];
            uchar*         row = out.scanLine(y);
            for (int x = 0; x < width; ++x) {
                row[x] = source[static_cast<std::size_t>(x) * 4u + static_cast<std::size_t>(channel)];
            }
        }
    } else {
        bool opaque = true;
        for (std::size_t i = 3; i < image.pixels.size() && opaque; i += 4) {
            opaque = image.pixels[i] == 255;
        }
        const QImage rgba(image.pixels.data(), width, height, width * 4, QImage::Format_RGBA8888);
        out = opaque ? rgba.convertToFormat(QImage::Format_RGB888) : rgba.copy();
    }
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    return out.save(&buffer, "PNG") ? bytes : QByteArray();
}

} // namespace

VulkanWindow::IconEnvironment VulkanWindow::iconEnvironment() {
    IconEnvironment light;
    EnvironmentImage studio;
    // (The lever the studio was designed with: POSESTUDIO_MATERIAL_ICON_HDRI=<panorama> lights
    // the icons of a run with that file INSTEAD, as it is — its own radiances, not normalized —
    // under the same dials. A studio drawn by a script is looked at on real balls that way
    // before its numbers go into makeIconStudio; and a photographed room shows what it does to
    // a mirror ball.)
    static const QString lever = qEnvironmentVariable("POSESTUDIO_MATERIAL_ICON_HDRI");
    if (!lever.isEmpty()) {
        if (std::optional<EnvironmentImage> loaded = loadEnvironmentImage(lever.toStdString());
            loaded && loaded->width > 0 && loaded->height > 0) {
            studio = std::move(*loaded);
        } else {
            qWarning().noquote() << "[materials] POSESTUDIO_MATERIAL_ICON_HDRI could not be read:" << lever;
        }
    }
    if (studio.pixels.empty()) {
        studio = makeIconStudio();
    }
    light.width = studio.width;
    light.height = studio.height;
    light.pixels = std::make_shared<const std::vector<glm::vec3>>(std::move(studio.pixels));
    return light;
}

bool VulkanWindow::saveMaterialFile(const QString& path, bool copyTextures, QString* error, MaterialSaveReport* report) {
    const auto fail = [error](const QString& why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    MaterialSaveReport counts;
    if (report != nullptr) {
        *report = counts;
    }
    if (!m_renderer) {
        return fail(tr("The viewport is not ready yet."));
    }
    const MaterialView view = materialView();
    if (!view.valid || view.zone < 0) {
        return fail(tr("No object is selected. Click an object in the viewport first: the material of the zone the Materials tab shows is what is saved."));
    }

    const QFileInfo  file(path);
    const QDir       folder = file.absoluteDir();
    MaterialDocument doc;
    doc.name = file.completeBaseName().toStdString();
    doc.params = view.params;
    // The file as it is named when nothing is collected: by its relative path where it lies in
    // or under the material's own folder (the pair can be moved together), else where it is.
    const auto named = [&folder](const QString& onDisk) {
        const QString absolute = QFileInfo(onDisk).absoluteFilePath();
        const QString relative = folder.relativeFilePath(absolute);
        const bool    inside = !relative.startsWith(QLatin1String("..")) && QDir::isRelativePath(relative);
        return inside ? relative : absolute;
    };
    AssetStore::TextureCollector collector(folder.absolutePath(), file.completeBaseName());
    for (int i = 0; i < kMaterialMapCount; ++i) {
        const std::size_t slot = static_cast<std::size_t>(i);
        if (view.params.maps[slot].source.empty()) {
            continue; // no map in this slot
        }
        const MaterialMap map = static_cast<MaterialMap>(i);
        const QString     onDisk = QString::fromStdString(view.mapPaths[slot]);
        const bool        hasFile = !onDisk.isEmpty() && QFileInfo(onDisk).isFile();
        QString           written;
        if (!copyTextures) {
            if (!hasFile) {
                ++counts.leftOut; // an image that lives inside the model's file: nothing on disk to name
                continue;
            }
            written = named(onDisk);
            ++counts.linked;
        } else {
            // The slot's decoded image and the channel its value is read from: a file can be
            // taken as it is only where the slot reads it as a chosen file would be read.
            int                                       channel = 0;
            const std::shared_ptr<const DecodedImage> plain = materialSlotImage(map, &channel);
            int                                       named3 = 0; // the channel the slot's SOURCE names (a cutout from an alpha)
            bool                                      turnedOver = false; // … and whether it reads the file inverted
            textureSourcePath(view.params.maps[slot].source, &named3, &turnedOver);
            AssetStore::Placed                        how = AssetStore::Placed::Failed;
            if (hasFile && channel == 0 && named3 == 0 && !turnedOver) {
                written = collector.addFile(onDisk, &how);
                switch (how) {
                    case AssetStore::Placed::Linked: ++counts.linked; break;
                    case AssetStore::Placed::Reused: ++counts.reused; break;
                    case AssetStore::Placed::Copied: ++counts.copied; break;
                    case AssetStore::Placed::Failed:
                        // (It could not be copied: named where it is, so the material is whole
                        // on this machine at least — and said.)
                        written = named(onDisk);
                        ++counts.linked;
                        ++counts.notCopied;
                        break;
                }
            } else {
                const QByteArray png = plain ? slotImageAsPng(*plain, channel) : QByteArray();
                if (!png.isEmpty()) {
                    written = collector.addBytes(png, file.completeBaseName() + QLatin1Char('_') + QString::fromLatin1(materialMapName(map)) +
                                                          QStringLiteral(".png"), &how);
                }
                if (how == AssetStore::Placed::Reused) {
                    ++counts.reused;
                } else if (how == AssetStore::Placed::Copied) {
                    ++counts.written;
                } else if (hasFile) {
                    written = named(onDisk); // (the packed file itself: better than nothing)
                    ++counts.linked;
                    ++counts.notCopied;
                } else {
                    ++counts.leftOut;
                    continue;
                }
            }
        }
        doc.textures.push_back({materialMapName(map), written.toStdString()});
    }
    if (report != nullptr) {
        *report = counts;
    }

    const std::string text = materialFileText(doc);
    QSaveFile         out(path); // (written whole or not at all: a failed save leaves the old file)
    if (!out.open(QIODevice::WriteOnly) || out.write(text.data(), static_cast<qint64>(text.size())) != static_cast<qint64>(text.size()) ||
        !out.commit()) {
        return fail(tr("The file could not be written:\n%1").arg(out.errorString()));
    }
    // NOTHING IS TIDIED: a material saved over itself with other maps leaves the files its old
    // version named where they are. (Until 2026-11-14 a save moved them to the trash when no
    // material of the folder named them any more — and took maps the user still wanted: a map
    // out of a slot is not a map out of use. The files are the user's to manage.)
    return true;
}

namespace {

/// A MaterialX document read from disk, its includes found relative to it.
bool loadMaterialXFile(const QString& path, MaterialXDocument& document, QString* error) {
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QObject::tr("The file could not be read:\n%1").arg(in.errorString());
        }
        return false;
    }
    const QByteArray bytes = in.readAll();
    const QDir       folder = QFileInfo(path).absoluteDir();
    std::string      why;
    const bool       ok = readMaterialXText(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())), document, why,
                                            [&folder](const std::string& href, std::string& text) {
                                                QFile included(folder.absoluteFilePath(QString::fromStdString(href)));
                                                if (!included.open(QIODevice::ReadOnly)) {
                                                    return false;
                                                }
                                                const QByteArray data = included.readAll();
                                                text.assign(data.constData(), static_cast<std::size_t>(data.size()));
                                                return true;
                                            });
    if (!ok && error != nullptr) {
        *error = QString::fromStdString(why);
    }
    return ok;
}

} // namespace

namespace {

/// Whether @p path is a content library's preset file by its name (`.duf`): which KIND of
/// preset is the reader's to say (shaderpresetfile.h).
bool isPresetFile(const QString& path) {
    return path.endsWith(QStringLiteral(".duf"), Qt::CaseInsensitive);
}

/// The content roots a shader preset's maps are looked for under ("/Runtime/Textures/…" is
/// relative to one): the library the preset lies in — the nearest folder above it that holds a
/// "Runtime" or a "data" folder — then the configured figure content folders and the
/// registered asset libraries.
std::vector<std::string> shaderPresetRoots(const QString& path) {
    std::vector<std::string> roots;
    const auto               add = [&roots](const QString& folder) {
        if (folder.trimmed().isEmpty()) {
            return;
        }
        const std::string root = QDir::cleanPath(folder.trimmed()).toStdString();
        if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
            roots.push_back(root);
        }
    };
    QDir dir = QFileInfo(path).absoluteDir();
    for (int level = 0; level < 16; ++level) {
        if (dir.exists(QStringLiteral("Runtime")) || dir.exists(QStringLiteral("runtime")) || dir.exists(QStringLiteral("data"))) {
            add(dir.absolutePath());
            break;
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    const QString configured = PreferencesManager::instance().getValue(Constants::PREF_FIGURE_CONTENT_ROOTS).toString();
    for (const QString& root : configured.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        add(root);
    }
    for (const QString& root : AssetLibraries::enabledPaths()) {
        add(root);
    }
    return roots;
}

/// The mean color of the image at @p path, LINEAR, from a small decode of it (a JPEG is scaled
/// in its decode: milliseconds for a 4K map). False when it cannot be read.
bool imageMeanLinear(const QString& path, glm::vec3& mean) {
    QImageReader reader(path);
    reader.setAutoTransform(false);
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > 64 || size.height() > 64)) {
        reader.setScaledSize(size.scaled(64, 64, Qt::KeepAspectRatio));
    }
    const QImage image = reader.read().convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return false;
    }
    double sum[3] = {0.0, 0.0, 0.0};
    for (int y = 0; y < image.height(); ++y) {
        const uchar* row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            for (int c = 0; c < 3; ++c) {
                sum[c] += colorcurves::srgbToLinear(row[x * 4 + c] / 255.0f);
            }
        }
    }
    const double count = static_cast<double>(image.width()) * image.height();
    mean = glm::vec3(static_cast<float>(sum[0] / count), static_cast<float>(sum[1] / count), static_cast<float>(sum[2] / count));
    return true;
}

} // namespace

QStringList VulkanWindow::materialNamesIn(const QString& path, QString* error) {
    QStringList names;
    if (pose::ImportFormats::isMaterialXFile(path)) {
        MaterialXDocument document;
        if (loadMaterialXFile(path, document, error)) {
            for (const MaterialXMaterial& material : document.materials) {
                names.append(QString::fromStdString(material.name));
            }
        }
    } else {
        names.append(QFileInfo(path).completeBaseName());
    }
    return names;
}

bool VulkanWindow::applyMaterialFile(const QString& path, QString* error, int* missingMaps, int* relocatedMaps, const QString& material,
                                     QStringList* notes) {
    const auto fail = [error](const QString& why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    if (missingMaps != nullptr) {
        *missingMaps = 0;
    }
    if (relocatedMaps != nullptr) {
        *relocatedMaps = 0;
    }
    if (!m_renderer) {
        return fail(tr("The viewport is not ready yet."));
    }
    MaterialDocument                    doc;
    bool                                partial = false; // a shader preset that changes PART of the surface: the other slots stay
    std::array<bool, kMaterialMapCount> named{};         // … the slots it names
    if (isPresetFile(path)) {
        // A content library's SHADER PRESET: read as a material (shaderpresetfile.h) — onto the
        // zone's own material where the preset names only part of a surface — its maps found
        // under the content roots, a glossiness map carried as an inverted roughness map.
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly)) {
            return fail(tr("The file could not be read:\n%1").arg(in.errorString()));
        }
        const QByteArray                raw = in.readAll();
        const std::vector<std::uint8_t> bytes(raw.constBegin(), raw.constEnd());
        const MaterialView              standing = materialView();
        const std::vector<std::string>  roots = shaderPresetRoots(path);
        const UriResolver               uris(roots);
        std::unordered_map<std::string, std::pair<bool, glm::vec3>> means; // (a map's mean is asked for once)
        const auto mapMean = [&uris, &means](const std::string& uri, glm::vec3& mean) {
            auto it = means.find(uri);
            if (it == means.end()) {
                glm::vec3         value(1.0f);
                const ResolvedUri file = uris.resolve(uri);
                const bool        ok = file.resolved() && imageMeanLinear(QString::fromStdString(file.path), value);
                it = means.emplace(uri, std::make_pair(ok, value)).first;
            }
            mean = it->second.second;
            return it->second.first;
        };
        ShaderPresetMaterial preset;
        std::string          why;
        if (!readShaderPreset(bytes, standing.valid && standing.zone >= 0 ? standing.params : MaterialParams(), preset, why, mapMean)) {
            return fail(QString::fromStdString(why));
        }
        doc.name = QFileInfo(path).completeBaseName().toStdString();
        doc.params = preset.params;
        partial = preset.partial;
        for (const ShaderPresetTexture& texture : preset.textures) {
            named[static_cast<std::size_t>(texture.slot)] = true;
            if (texture.uri.empty()) {
                continue; // (a partial preset empties the slot)
            }
            // Where the map is; a map found under no root keeps the place it should have been
            // at, so that it is looked for by its name and, failing that, counted as missing.
            std::string file = uris.resolve(texture.uri).path;
            if (file.empty()) {
                const std::string base = roots.empty() ? QFileInfo(path).absolutePath().toStdString() : roots.front();
                file = base + urlDecode(texture.uri);
            }
            doc.textures.push_back({materialMapName(texture.slot), textureSourceWithChannel(file, 0, texture.inverted)});
        }
        qInfo().noquote() << "[materials]" << QFileInfo(path).fileName() << ": a shader preset read as"
                          << QString::fromStdString(preset.reading) << "(" << preset.settings << "settings," << preset.textures.size() << "maps )";
        for (const std::string& said : preset.notes) {
            qInfo().noquote() << "[materials]" << QFileInfo(path).fileName() << ":" << QString::fromStdString(said);
            if (notes != nullptr) {
                notes->append(QString::fromStdString(said));
            }
        }
    } else if (pose::ImportFormats::isMaterialXFile(path)) {
        // A MATERIALX document: one of its materials, as the values and maps of a saved one (a
        // map read from another channel than the red carries the channel in its source).
        MaterialXDocument document;
        QString           why;
        if (!loadMaterialXFile(path, document, &why)) {
            return fail(why);
        }
        const MaterialXMaterial* chosen = &document.materials.front();
        if (!material.isEmpty()) {
            chosen = nullptr;
            for (const MaterialXMaterial& candidate : document.materials) {
                if (QString::fromStdString(candidate.name).compare(material, Qt::CaseInsensitive) == 0) {
                    chosen = &candidate;
                    break;
                }
            }
            if (chosen == nullptr) {
                return fail(tr("The file has no material named \"%1\".").arg(material));
            }
        }
        doc.name = chosen->name;
        doc.params = chosen->params;
        for (const MaterialXTexture& texture : chosen->textures) {
            doc.textures.push_back({materialMapName(texture.slot), textureSourceWithChannel(texture.file, texture.channel)});
        }
        for (const std::string& said : chosen->notes) {
            qInfo().noquote() << "[materials]" << QFileInfo(path).fileName() << "/" << QString::fromStdString(chosen->name) << ":"
                              << QString::fromStdString(said);
            if (notes != nullptr) {
                notes->append(QString::fromStdString(said));
            }
        }
    } else {
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly)) {
            return fail(tr("The file could not be read:\n%1").arg(in.errorString()));
        }
        const QByteArray bytes = in.readAll();
        std::string      why;
        if (!readMaterialFileText(std::string(bytes.constData(), static_cast<std::size_t>(bytes.size())), doc, why)) {
            return fail(QString::fromStdString(why));
        }
    }
    const MaterialView view = materialView();
    if (!view.valid || view.zone < 0) {
        return fail(tr("No object is selected. Click an object in the viewport first: a material is put on the zone the Materials tab shows."));
    }

    // What each slot is to hold: the file's map, or nothing — the file is the whole surface, so a
    // slot it does not list is emptied. A map is where the file says (relative to the material's
    // folder), or is LOOKED FOR: through the material's library, and by its name (assetstore.h).
    const QString                          folder = QFileInfo(path).absolutePath();
    const QString                          library = LibraryPaths::libraryRootOf(folder);
    AssetStore::TextureResolver            resolver(folder, library); // (one walk of the library serves all of the material's maps)
    std::array<QString, kMaterialMapCount> wanted;
    for (const ProjectTexture& texture : doc.textures) {
        MaterialMap map = MaterialMap::BaseColor;
        if (!materialMapByName(texture.slot, map) || texture.source.empty()) {
            continue;
        }
        int           channel = 0;      // (a MaterialX map may be one channel of a packed image)
        bool          inverted = false; // (a shader preset's glossiness map is a roughness map read inverted)
        const QString source = QString::fromStdString(textureSourcePath(texture.source, &channel, &inverted));
        bool          viaName = false;
        const QString found = resolver.resolve(source, &viaName);
        if (found.isEmpty()) {
            qWarning().noquote() << "[materials] a map of" << QFileInfo(path).fileName() << "is missing:" << QDir::toNativeSeparators(source);
            if (missingMaps != nullptr) {
                ++*missingMaps;
            }
            continue;
        }
        if (viaName) {
            qInfo().noquote() << "[materials] a map of" << QFileInfo(path).fileName() << "was not at" << QDir::toNativeSeparators(source)
                              << "and was found at" << QDir::toNativeSeparators(found);
            if (relocatedMaps != nullptr) {
                ++*relocatedMaps;
            }
        }
        wanted[static_cast<std::size_t>(map)] = QString::fromStdString(textureSourceWithChannel(found.toStdString(), channel, inverted));
    }

    // ONE undoable edit (its own bracket, or the one already open).
    const bool opened = !m_materialEdit;
    if (opened && !beginMaterialEdit()) {
        return fail(tr("The material could not be applied while another edit is in progress."));
    }
    // Every slot that changes, as ONE batch: the slots that share a packed binding — the
    // parameter map's four, the layer map's four, the base color and its cutout — then make and
    // upload it once, not once a slot (applySlotSources).
    std::vector<std::pair<MaterialMap, std::string>> changes;
    for (int i = 0; i < kMaterialMapCount; ++i) {
        const std::size_t slot = static_cast<std::size_t>(i);
        const MaterialMap map = static_cast<MaterialMap>(i);
        const bool        has = !view.params.maps[slot].source.empty();
        const QString     want = wanted[slot];
        if (partial && !named[slot]) {
            continue; // (part of a surface: a slot the preset does not name keeps its map)
        }
        if (want.isEmpty()) {
            if (has) {
                changes.emplace_back(map, std::string());
            }
        } else if (!has || !samePath(QString::fromStdString(view.params.maps[slot].source), want)) {
            changes.emplace_back(map, want.toStdString());
        }
    }
    // (Decoded for the material the file brings — its Tiling decides the maps' gutter fill —
    // since the values are set after the slots.)
    OverlayBake                    bake;
    const std::vector<MaterialMap> unread =
        applySlotSources(scene().selectedModelIndex(), scene().selectedZoneIndex(), changes, bake, &doc.params);
    // A map that was FOUND and could not be READ — a truncated file, a format the decoder does
    // not have (an .exr) — is a missing map like one that was not found: its slot is emptied and
    // it is counted. (Left as it was, the slot kept the PREVIOUS material's map under the new
    // material's values, and the message after applying said nothing.)
    if (!unread.empty()) {
        std::vector<std::pair<MaterialMap, std::string>> emptied;
        for (const MaterialMap map : unread) {
            qWarning().noquote() << "[materials] a map of" << QFileInfo(path).fileName() << "could not be read: the" << materialMapName(map)
                                 << "slot is left empty";
            emptied.emplace_back(map, std::string());
            if (missingMaps != nullptr) {
                ++*missingMaps;
            }
        }
        applySlotSources(scene().selectedModelIndex(), scene().selectedZoneIndex(), emptied, bake, &doc.params);
    }
    // The values last: they are the file's, all of them (the detail layer's too, which a detail
    // map chosen on the tab would have brought defaults for).
    // (All but THE SMOOTHING, which is the object's: the zone keeps its own.)
    MaterialParams values = doc.params;
    const MaterialParams own = materialView().params;
    values.smoothByAngle = own.smoothByAngle;
    values.smoothAngle = own.smoothAngle;
    setSelectedMaterial(values, /*linked=*/false);
    if (opened) {
        endMaterialEdit();
    }
    requestUpdate();
    return true;
}

QImage VulkanWindow::renderMaterialIcon(int size, QString* renderer) {
    if (renderer != nullptr) {
        renderer->clear();
    }
    if (!m_renderer || size < 8) {
        return QImage();
    }
    const int model = scene().selectedModelIndex();
    const int mesh = scene().selectedZoneIndex();
    Model*    owner = model >= 0 ? scene().modelAt(static_cast<std::size_t>(model)) : nullptr;
    if (owner == nullptr || mesh < 0 || static_cast<std::size_t>(mesh) >= owner->meshCount()) {
        return QImage();
    }
    const LightingSettings lighting = iconStudioLighting();
    const IconEnvironment  light = iconEnvironment();

    // What the built-in shader needs, read NOW: the engine's render below waits in a nested event
    // loop, in which the renderer can go (a device lost by a frame drawn meanwhile) — the
    // fallback after it must touch no scene state.
    const MaterialParams ballMaterial = owner->mesh(static_cast<std::size_t>(mesh)).material();
    BallMaps             maps;
    const auto           slot = [&](MaterialMap map, int* channel) {
        return scene().meshSlotImage(model, mesh, map, /*imported=*/false, channel);
    };
    maps.baseColor = slot(MaterialMap::BaseColor, nullptr);
    maps.roughness = slot(MaterialMap::Roughness, &maps.roughnessChannel);
    maps.metalness = slot(MaterialMap::Metalness, &maps.metalnessChannel);
    maps.specular = slot(MaterialMap::SpecularMask, &maps.specularChannel);
    maps.emission = slot(MaterialMap::Emission, nullptr);

    // --- With a render engine: the ball path traced --------------------------------------------
    static const bool builtInOnly = qEnvironmentVariableIsSet("POSESTUDIO_MATERIAL_ICON_BUILTIN");
    QString           engineError;
    if (!builtInOnly && RenderSession::engine(&engineError) != nullptr) {
        auto        snapshot = std::make_shared<RenderSnapshot>();
        OverlayBake bake;
        snapshot->names.emplace_back("material ball");
        const char* name = snapshot->names.back().c_str();

        RenderMaterialContext context;
        context.subsurfaceDial = lighting.subsurface;
        context.translucencyAuthored = ImportFormats::isNativeFigureFile(QString::fromStdString(owner->sourcePath()));
        const renderapi::Material material = captureZoneMaterial(*snapshot, bake, model, mesh, name, context, /*withOverlays=*/false);

        BallMesh ball = makeBallMesh(glm::vec3(0.0f), kBallRadius);
        snapshot->geometry.emplace_back();
        Model::PosedMesh& geometry = snapshot->geometry.back();
        geometry.positions = std::move(ball.positions);
        geometry.normals = std::move(ball.normals);
        geometry.uvs = std::move(ball.uvs);
        geometry.tangents = std::move(ball.tangents);
        geometry.indices = std::move(ball.indices);
        renderapi::Mesh out;
        out.name = name;
        out.material = 0;
        snapshot->materials.push_back(material);
        snapshot->meshes.push_back(out);

        // The icon studio, as it is (strength 1: its radiances are the light); nothing behind
        // the ball and no floor under it.
        snapshot->environmentPixels = light.pixels;
        renderapi::Image studio;
        studio.id = RenderSnapshot::kIconStudioImageId;
        studio.pixels = snapshot->environmentPixels->data();
        studio.width = light.width;
        studio.height = light.height;
        studio.channels = 3;
        studio.format = renderapi::PixelFormat::Float32;
        studio.colorSpace = renderapi::ColorSpace::Linear;
        studio.name = "icon studio";
        renderapi::Environment& environment = snapshot->description.environment;
        environment.image = static_cast<int32_t>(snapshot->images.size());
        snapshot->images.push_back(studio);
        renderEnvironment(lighting, 1.0f, environment);
        snapshot->description.floorShadows = 0;

        // The camera: on +Z, level with the ball, far enough that the ball fills the picture.
        const float     halfFov = glm::radians(kBallFovDeg) * 0.5f;
        const float     distance = kBallRadius / std::sin(std::atan(kMaterialBallShare * std::tan(halfFov)));
        const glm::mat4 cameraToWorld = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, distance));
        renderapi::Camera& camera = snapshot->description.camera;
        std::copy(glm::value_ptr(cameraToWorld), glm::value_ptr(cameraToWorld) + 16, camera.cameraToWorld);
        camera.fovY = glm::radians(kBallFovDeg);
        camera.orthoHeight = 0.0f;
        camera.nearClip = 0.05f;
        camera.farClip = 100.0f;

        snapshot->viewSettings.exposure = lighting.exposure;
        snapshot->viewSettings.tonemap = lighting.tonemap;
        // (Levers for telling the renderer's part in an icon from the light's:
        // POSESTUDIO_MATERIAL_ICON_SUPERSAMPLE, _SAMPLES and _DENOISE=0|1 override the three
        // settings below for a run. They are how "the chrome ball looks fuzzy" was found to be
        // the room it mirrored and not the engine: none of them changed the picture.)
        const int  supersample = qBound(1, qEnvironmentVariableIntValue("POSESTUDIO_MATERIAL_ICON_SUPERSAMPLE") > 0
                                              ? qEnvironmentVariableIntValue("POSESTUDIO_MATERIAL_ICON_SUPERSAMPLE")
                                              : kIconSupersample, 8);
        const int  samples = qEnvironmentVariableIntValue("POSESTUDIO_MATERIAL_ICON_SAMPLES") > 0
                                 ? qEnvironmentVariableIntValue("POSESTUDIO_MATERIAL_ICON_SAMPLES")
                                 : kIconSamples;
        const bool denoise = qEnvironmentVariableIsSet("POSESTUDIO_MATERIAL_ICON_DENOISE")
                                 ? qEnvironmentVariableIntValue("POSESTUDIO_MATERIAL_ICON_DENOISE") != 0
                                 : kIconDenoise;
        snapshot->viewSettings.width = size * supersample;
        snapshot->viewSettings.height = size * supersample;
        snapshot->link();

        RenderOptions options;
        options.width = size * supersample;
        options.height = size * supersample;
        options.samples = samples;
        options.denoise = denoise;
        options.transparent = true;
        options.deviceId = preferredRenderDevice(); // the Render window's, if it is still there

        RenderSession session;
        QEventLoop    loop;
        QTimer        giveUp;
        giveUp.setSingleShot(true);
        connect(&session, &RenderSession::ended, &loop, &QEventLoop::quit);
        connect(&giveUp, &QTimer::timeout, &session, &RenderSession::stop);
        connect(&giveUp, &QTimer::timeout, &loop, &QEventLoop::quit);
        QString error;
        if (session.start(snapshot, options, &error)) {
            giveUp.start(kIconTimeoutMs);
            loop.exec();
            if (session.status().state == renderapi::State::Finished && session.hasImage()) {
                // Scaled down premultiplied (a straight-alpha average would bleed the transparent
                // pixels' color into the ball's edge), saved straight.
                const QImage full = session.image().convertToFormat(QImage::Format_ARGB32_Premultiplied);
                const QImage icon = full.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                        .convertToFormat(QImage::Format_ARGB32);
                if (renderer != nullptr) {
                    *renderer = RenderSession::engineName();
                }
                return icon;
            }
            error = QString::fromUtf8(session.status().error);
        }
        qWarning().noquote() << "[materials] the render engine did not make the material's icon (" << error
                             << "): the built-in ball shader draws it";
    }

    // --- Without one: the built-in ball shader (from what was read before the render) ------------
    EnvironmentImage studio;
    studio.width = light.width;
    studio.height = light.height;
    studio.pixels = *light.pixels;
    const std::vector<std::uint8_t> pixels = shadeMaterialBall(ballMaterial, maps, studio, lighting, size);
    QImage                          icon(size, size, QImage::Format_RGBA8888);
    for (int y = 0; y < size; ++y) {
        std::copy_n(&pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) * 4u], static_cast<std::size_t>(size) * 4u, icon.scanLine(y));
    }
    if (renderer != nullptr) {
        *renderer = tr("the built-in ball shader");
    }
    return icon.convertToFormat(QImage::Format_ARGB32);
}

// The scripted in-app test's `savematerial` and `applymaterial` (vulkanwindow_script.cpp;
// tools/ikscripts/README.md): the Materials tab's Save — the selected zone's material written
// and its icon rendered — and a library double-click, with the run's folder for the dialogs.
void VulkanWindow::scriptSaveMaterial(const QStringList& words) {
    IkScript& s = *m_script;
    s.matFileOk = 0.0;
    s.matFileEmbedded = 0.0;
    s.matFileCopied = s.matFileReused = s.matFileWritten = s.matFileLinked = 0.0;
    s.matIconOk = 0.0;
    s.matIconEngine = 0.0;
    s.matIcon = QImage();
    // savematerial <name.psm> [noicon] [link] [<size>]: into the run's folder (the name may
    // carry folders, which are made).
    const QString path = QDir(s.outDir).filePath(words[1]);
    QDir().mkpath(QFileInfo(path).absolutePath());
    bool          icon = true;
    bool          copy = true;
    int           size = 255;
    for (int i = 2; i < words.size(); ++i) {
        bool      number = false;
        const int value = words[i].toInt(&number);
        if (number) {
            size = value;
        } else if (words[i].compare(QLatin1String("noicon"), Qt::CaseInsensitive) == 0) {
            icon = false;
        } else if (words[i].compare(QLatin1String("link"), Qt::CaseInsensitive) == 0) {
            copy = false;
        }
    }
    QString            error;
    MaterialSaveReport report;
    const bool         saved = saveMaterialFile(path, copy, &error, &report);
    s.matFileOk = saved ? 1.0 : 0.0;
    s.matFileEmbedded = report.leftOut;
    s.matFileCopied = report.copied;
    s.matFileReused = report.reused;
    s.matFileWritten = report.written;
    s.matFileLinked = report.linked;
    std::fprintf(stderr, "[ikscript]   material %s: %s%s\n", saved ? "saved" : "NOT saved", qPrintable(path),
                 saved ? "" : qPrintable(QStringLiteral(" (") + error + QLatin1Char(')')));
    if (saved) {
        std::fprintf(stderr, "[ikscript]   textures: %d copied, %d reused, %d written, %d linked, %d left out\n", report.copied,
                     report.reused, report.written, report.linked, report.leftOut);
    }
    if (saved && icon) {
        QString       how;
        QElapsedTimer clock;
        clock.start();
        const QImage picture = renderMaterialIcon(size, &how);
        const double seconds = static_cast<double>(clock.elapsed()) / 1000.0;
        if (!picture.isNull()) {
            const QFileInfo file(path);
            const QString   iconPath = file.absoluteDir().filePath(file.completeBaseName() + QStringLiteral(".png"));
            s.matIconOk = picture.save(iconPath) ? 1.0 : 0.0;
            s.matIconEngine = how == RenderSession::engineName() && !how.isEmpty() ? 1.0 : 0.0;
            s.matIcon = picture;
            std::fprintf(stderr, "[ikscript]   icon %d x %d by %s in %.2f s -> %s\n", picture.width(), picture.height(),
                         qPrintable(how), seconds, qPrintable(iconPath));
        } else {
            std::fprintf(stderr, "[ikscript]   icon NOT rendered\n");
        }
    }
    std::fflush(stderr);
}

void VulkanWindow::scriptApplyMaterial(const QStringList& words) {
    IkScript& s = *m_script;
    s.matFileOk = 0.0;
    s.matFileMissing = 0.0;
    s.matFileRelocated = 0.0;
    // applymaterial <name.psm>: from the run's folder (a file `savematerial` wrote), else a test asset.
    // A script's line is split on spaces, and a content library's files have spaces in their
    // names: the file is the LONGEST run of the line's words that names one (so
    // `applymaterial Metal - Gold.duf` and a path of folders with spaces both work), and what
    // follows it is the material's name (a MaterialX document: `applymaterial <file.mtlx>
    // [<material name>]`). With no such file the first word is the path, as before.
    const QString assets = qEnvironmentVariable("POSESTUDIO_IK_SCRIPT_ASSETS");
    const auto    located = [&](const QString& name) {
        const QString inRun = QDir(s.outDir).filePath(name);
        if (QFileInfo(inRun).isFile() || assets.isEmpty()) {
            return inRun;
        }
        return QDir(assets).filePath(name);
    };
    QString path = located(words[1]);
    int     pathWords = 1;
    for (int count = static_cast<int>(words.size()) - 1; count > 1; --count) {
        const QString candidate = located(words.mid(1, count).join(QLatin1Char(' ')));
        if (QFileInfo(candidate).isFile()) {
            path = candidate;
            pathWords = count;
            break;
        }
    }
    const QString material = words.mid(1 + pathWords).join(QLatin1Char(' '));
    s.matFileMaterials = static_cast<double>(materialNamesIn(path).size());
    QString     error;
    int         missing = 0;
    int         relocated = 0;
    QStringList notes;
    const bool  applied = applyMaterialFile(path, &error, &missing, &relocated, material, &notes);
    s.matFileOk = applied ? 1.0 : 0.0;
    s.matFileMissing = missing;
    s.matFileRelocated = relocated;
    s.matFileNotes = static_cast<double>(notes.size());
    std::fprintf(stderr, "[ikscript]   material %s: %s%s\n", applied ? "applied" : "NOT applied", qPrintable(path),
                 applied ? "" : qPrintable(QStringLiteral(" (") + error + QLatin1Char(')')));
    std::fflush(stderr);
}

} // namespace pose
