/**
 * @file vulkanwindow_materials.cpp
 * @brief VulkanWindow's TEXTURE SLOTS and OVERLAYS — the Materials tab's maps (2026-11-09) and
 *        the images layered over them (2026-11-11).
 *
 * A zone's map is replaced by an image file, cleared, or put back to the one the file imported
 * with. The Qt layer owns the decode (the import's own: modelimportservice.h — the same size cap,
 * the same cutout bake and parameter-map build), derives the image the slot's BINDING needs from
 * the slot and its partners (the base color with the cutout baked into its alpha; the roughness /
 * metalness parameter map), and hands the Scene the plain image, the channel, the bound image and
 * the recorded source. Every change is a material edit: one undo entry through the tab's bracket
 * (beginMaterialEdit .. endMaterialEdit), and the project file carries the sources. Undo and a
 * project opened come back through applyMaterialTextures, which re-derives every slot whose
 * recorded source — or whose overlays — differ from the mesh's.
 *
 * THE OVERLAYS (materialoverlay.h, overlaycompositor.h) are painted INTO those bound images: the
 * slot's plain image stays what the import or the user gave it, and what the slot binds is that
 * image with the zone's overlay stack composited over it (bindSlot → overlaid). The overlay files
 * are decoded once and kept cropped (m_overlayImages), so a slider scrub re-composites the one
 * slot its control touches and re-uploads it — live. An overlay belongs to a MAP: the zones that
 * share the selected zone's base-color map all carry the stack (Scene::overlayZones), and one
 * edit's composites are shared between them (OverlayBake), as the import shares their textures.
 *
 * THE OVERLAYS' METAL is the one composite that depends on a material VALUE: a metalness map is a
 * share of the zone's Metallic value, and no share of 0 is metal — so a zone whose overlays are
 * metallic binds its ABSOLUTE metalness (the value × the map, the overlays' metal painted on) and
 * the mesh takes the value as 1 (Mesh::setMetalBaked). The value it was baked for is recorded on
 * the mesh; syncOverlayMetal re-bakes when the value moves.
 */

#include "vulkanwindow.h"

#include "modeldata.h"
#include "modelimportservice.h"
#include "overlaycompositor.h"
#include "scene.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <cstdio>
#include <map>

namespace pose {

namespace {

/// The decoded overlay images kept at most (their cropped bytes): beyond it the least recently
/// used go. A tattoo's crop is a few megabytes; an opaque full-map image sixty-four.
constexpr qint64 kOverlayCacheBytes = 384ll * 1024 * 1024;

std::string pointerKey(const void* p) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%p", p);
    return buffer;
}

/// One channel of a decoded image as a cutout mask (its value in every channel, opaque): a
/// cutout that lives in the ALPHA of a color image (a leaf's), which the mask decode — gray —
/// would read as the leaf's brightness.
std::shared_ptr<DecodedImage> channelAsMask(const DecodedImage& image, int channel) {
    auto out = std::make_shared<DecodedImage>();
    out->width = image.width;
    out->height = image.height;
    out->pixels.resize(image.pixels.size());
    const std::size_t c = static_cast<std::size_t>(std::clamp(channel, 0, 3));
    for (std::size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        const uint8_t v = image.pixels[i + c];
        out->pixels[i] = out->pixels[i + 1] = out->pixels[i + 2] = v;
        out->pixels[i + 3] = 255;
    }
    return out;
}

} // namespace

bool VulkanWindow::setMaterialTexture(MaterialMap map, const QString& source) {
    if (!m_renderer) {
        return false;
    }
    const int model = scene().selectedModelIndex();
    const int mesh = scene().selectedZoneIndex();
    if (model < 0 || mesh < 0) {
        return false;
    }
    // ONE undo entry: the tab's choose / clear / restore is a click, so the bracket is this
    // call's own — unless an edit is already open (a script's, a panel's), which it then joins.
    const bool opened = !m_materialEdit;
    if (opened && !beginMaterialEdit()) {
        return false;
    }
    const bool ok = applySlotSource(model, mesh, map, source.toStdString());
    if (ok && map == MaterialMap::PoreDetail && !source.isEmpty()) {
        // A detail map put into an EMPTY layer brings the layer's defaults with it (the source
        // app's: weight 0.5, 32 tiles) — at weight 0 and no tiling the map would do nothing until
        // both were dialed, and the strength rows read 0 and 0.
        MaterialParams p = scene().materialView().params;
        bool           changed = false;
        if (p.detailWeight <= 0.0f) {
            p.detailWeight = 0.5f;
            changed = true;
        }
        if (p.detailTiles < 1.0f) {
            p.detailTiles = 32.0f;
            changed = true;
        }
        if (p.detailTilesV < 1.0f) {
            p.detailTilesV = p.detailTiles;
            changed = true;
        }
        if (changed) {
            scene().setSelectedZoneMaterial(p, /*linked=*/false);
        }
    }
    if (opened) {
        endMaterialEdit(); // (registers nothing if the slot ended as it began)
    }
    if (ok) {
        requestUpdate();
    }
    return ok;
}

std::shared_ptr<const DecodedImage> VulkanWindow::materialSlotImage(MaterialMap map, int* channel) const {
    if (!m_renderer) {
        return nullptr;
    }
    const int model = scene().selectedModelIndex();
    const int mesh = scene().selectedZoneIndex();
    if (model < 0 || mesh < 0) {
        return nullptr;
    }
    return scene().meshSlotImage(model, mesh, map, /*imported=*/false, channel);
}

bool VulkanWindow::applySlotSource(int model, int mesh, MaterialMap map, const std::string& source, OverlayBake* bake) {
    // The slot's new PLAIN image: the imported one back, a file decoded, or none.
    std::shared_ptr<const DecodedImage> plain;
    int                                 channel = 0;
    if (source == kImportedTexture) {
        plain = scene().meshSlotImage(model, mesh, map, /*imported=*/true, &channel);
    } else if (!source.empty()) {
        // A cutout mask is read as gray (the import's decodeMask: a single-channel or RGB mask
        // alike); every other slot as RGBA. A roughness or metalness file read here is a plain
        // grayscale map whose RED channel is the value — unless the source names ANOTHER channel
        // (textureSourcePath: a packed image, a cutout in a color image's alpha).
        int               wanted = 0;
        const std::string path = textureSourcePath(source, &wanted);
        if (map == MaterialMap::Opacity) {
            if (wanted != 0) {
                const std::shared_ptr<DecodedImage> image = decodeTextureFile(path);
                plain = image ? channelAsMask(*image, wanted) : nullptr;
            } else {
                plain = decodeMaskFile(path);
            }
            channel = 0;
        } else {
            plain = decodeTextureFile(path);
            channel = wanted;
        }
        if (!plain) {
            qWarning().noquote() << "[materials] could not read the image" << QString::fromStdString(path);
            return false;
        }
    }
    OverlayBake local;
    return bindSlot(model, mesh, map, plain, channel, source, bake != nullptr ? *bake : local);
}

void VulkanWindow::rebindSlot(int model, int mesh, MaterialMap map, OverlayBake& bake) {
    // The slot as it stands — its plain image, channel and recorded source — bound afresh: what
    // changed is the overlay stack painted over it.
    int        channel = 0;
    const auto plain = scene().meshSlotImage(model, mesh, map, /*imported=*/false, &channel);
    bindSlot(model, mesh, map, plain, channel, scene().meshSlotSource(model, mesh, map), bake);
}

std::shared_ptr<const DecodedImage> VulkanWindow::overlaid(int model, int mesh, MaterialMap map,
                                                           const std::shared_ptr<const DecodedImage>& base, int channel, OverlayBake& bake,
                                                           float baseScale, bool forRenderer) {
    // (The viewport's normal map carries the overlays' bump images too: see kOverlaySlots. A
    // render engine gets them where they belong, in the bump map.)
    const bool                         bumpAsNormal = map == MaterialMap::Normal && !forRenderer;
    const std::vector<MaterialOverlay> overlays = scene().meshOverlays(model, mesh);
    const std::string                  signature = overlaySignature(overlays, map, bumpAsNormal);
    if (signature.empty()) {
        return base; // no overlay touches this slot: the map as it is
    }
    char scale[40];
    std::snprintf(scale, sizeof(scale), "%a", static_cast<double>(baseScale));
    const std::string key = std::string(materialMapName(map)) + (forRenderer ? "@render#" : "#") + pointerKey(base.get()) + "#" +
                            std::to_string(channel) + "#" + scale + "#" + signature;
    if (const auto it = bake.images.find(key); it != bake.images.end()) {
        return it->second;
    }
    std::shared_ptr<const DecodedImage> painted = compositeOverlays(
        map, base, channel, overlays, [this](const std::string& path, OverlayImageKind kind) { return overlayImage(QString::fromStdString(path), kind); },
        bumpAsNormal, baseScale);
    if (!painted) {
        painted = base; // (every image missing: the map as it is)
    }
    bake.images.emplace(key, painted);
    return painted;
}

bool VulkanWindow::bindSlot(int model, int mesh, MaterialMap map, const std::shared_ptr<const DecodedImage>& plain, int channel,
                            const std::string& source, OverlayBake& bake) {
    // The image the slot's BINDING samples, derived with its partner slots' current plain images
    // and the zone's overlays: BaseColor + Opacity share the painted diffuse with the cutout in
    // its alpha, Roughness + Metalness + AmbientOcclusion + TransmissionWeight the parameter map
    // (the overlays' gloss in its roughness), CoatWeight + CoatRoughness + SheenWeight +
    // ThinFilmThickness the layer map; Normal binds its image with the overlays' relief (their
    // normal images, and their bump images as slopes); the rest bind their plain image as it is.
    std::shared_ptr<const DecodedImage> bound;
    bool                                parameterMap = false; // the binding is the parameter map: its metal record follows
    float                               metalBaked = -1.0f;   // the Metallic value the metalness was baked for (< 0: not baked)
    // A partner slot's current plain image and channel — or this one's new image where it IS the slot.
    const auto partner = [&](MaterialMap other, int* otherChannel) -> std::shared_ptr<const DecodedImage> {
        if (other == map) {
            if (otherChannel != nullptr) {
                *otherChannel = channel;
            }
            return plain;
        }
        return scene().meshSlotImage(model, mesh, other, /*imported=*/false, otherChannel);
    };
    // A derived image by its key, made once per edit (the zones of one map share it).
    const auto shared = [&bake](const std::string& key, const auto& make) -> std::shared_ptr<const DecodedImage> {
        if (const auto it = bake.images.find(key); it != bake.images.end()) {
            return it->second;
        }
        std::shared_ptr<const DecodedImage> made = make();
        bake.images.emplace(key, made);
        return made;
    };
    switch (map) {
    case MaterialMap::BaseColor:
    case MaterialMap::Opacity: {
        const auto base = partner(MaterialMap::BaseColor, nullptr);
        const auto mask = partner(MaterialMap::Opacity, nullptr);
        const auto painted = overlaid(model, mesh, MaterialMap::BaseColor, base, 0, bake);
        // No cutout: the painted color map IS the binding (the shared decode itself where no
        // overlay painted it — only ever read from here on).
        bound = !mask ? painted
                      : shared("cutout#" + pointerKey(painted.get()) + "#" + pointerKey(mask.get()),
                               [&]() -> std::shared_ptr<const DecodedImage> { return bakeCutoutIntoBaseColor(painted, mask); });
        break;
    }
    case MaterialMap::Roughness:
    case MaterialMap::Metalness:
    case MaterialMap::AmbientOcclusion:
    case MaterialMap::TransmissionWeight: {
        int        roughChannel = 0;
        int        metalChannel = 0;
        const auto rough = partner(MaterialMap::Roughness, &roughChannel);
        if (!rough) {
            roughChannel = 0; // (a canvas the overlays' gloss is painted on carries it in red)
        }
        const auto metal = partner(MaterialMap::Metalness, &metalChannel);
        const auto occlusion = partner(MaterialMap::AmbientOcclusion, nullptr);
        const auto transmission = partner(MaterialMap::TransmissionWeight, nullptr);
        const auto glossed = overlaid(model, mesh, MaterialMap::Roughness, rough, roughChannel, bake);
        // The overlays' METAL: painted on the zone's ABSOLUTE metalness — its Metallic value × its
        // map — which the mesh then reads with the value taken as 1 (see the file comment).
        std::shared_ptr<const DecodedImage> metalBound = metal;
        if (!metal) {
            metalChannel = 0; // (a canvas the overlays' metal is painted on carries it in red)
        }
        if (!overlaySignature(scene().meshOverlays(model, mesh), MaterialMap::Metalness).empty()) {
            const float value = scene().meshMetalness(model, mesh);
            const auto  painted = overlaid(model, mesh, MaterialMap::Metalness, metal, metalChannel, bake, value);
            if (painted != metal) {
                metalBound = painted;
                metalBaked = value;
            }
        }
        parameterMap = true;
        bound = shared("params#" + pointerKey(glossed.get()) + "#" + std::to_string(roughChannel) + "#" + pointerKey(metalBound.get()) + "#" +
                           std::to_string(metalChannel) + "#" + pointerKey(occlusion.get()) + "#" + pointerKey(transmission.get()),
                       [&]() -> std::shared_ptr<const DecodedImage> {
                           return buildParameterMapImage(glossed, roughChannel, metalBound, metalChannel, occlusion, transmission);
                       });
        break;
    }
    case MaterialMap::CoatWeight:
    case MaterialMap::CoatRoughness:
    case MaterialMap::SheenWeight:
    case MaterialMap::ThinFilmThickness:
        bound = buildLayerMapImage(partner(MaterialMap::CoatWeight, nullptr), partner(MaterialMap::CoatRoughness, nullptr),
                                   partner(MaterialMap::SheenWeight, nullptr), partner(MaterialMap::ThinFilmThickness, nullptr));
        break;
    case MaterialMap::Normal:
        bound = overlaid(model, mesh, map, plain, 0, bake);
        break;
    default:
        bound = materialMapIsRendered(map) ? plain : nullptr; // (a stored-only slot binds nothing)
        break;
    }
    // (An import without a map in this slot has nothing to put back: the Mesh records a null
    // plain image as "none", whatever source was asked.)
    const bool applied = scene().setMeshTexture(model, mesh, map, plain, channel, bound, source);
    if (parameterMap) {
        scene().setMeshMetalBaked(model, mesh, metalBaked);
    }
    return applied;
}

void VulkanWindow::syncOverlayMetal(int model) {
    if (!m_renderer || model < 0) {
        return;
    }
    const std::vector<MaterialParams> now = scene().captureMaterials(model);
    OverlayBake                       bake;
    for (std::size_t mesh = 0; mesh < now.size(); ++mesh) {
        const float baked = scene().meshMetalBaked(model, static_cast<int>(mesh));
        if (baked >= 0.0f && baked != now[mesh].metalness) {
            rebindSlot(model, static_cast<int>(mesh), MaterialMap::Metalness, bake);
        }
    }
}

void VulkanWindow::applyMaterialTextures(int model, const std::vector<MaterialParams>& materials) {
    if (!m_renderer || model < 0) {
        return;
    }
    // Every slot of every zone whose recorded source differs from what the mesh carries now, and
    // every slot whose OVERLAYS do — the undo swap's and the project load's texture half, run
    // BEFORE the values are applied. The overlay stack goes onto the mesh first, so a slot bound
    // here is bound with the stack it is to carry.
    const std::vector<MaterialParams> now = scene().captureMaterials(model);
    const std::size_t                 count = std::min(now.size(), materials.size());
    OverlayBake                       bake;
    for (std::size_t mesh = 0; mesh < count; ++mesh) {
        const std::vector<MaterialOverlay>& before = now[mesh].overlays;
        const std::vector<MaterialOverlay>& wanted = materials[mesh].overlays;
        scene().setMeshOverlays(model, static_cast<int>(mesh), wanted);
        bool parametersBound = false; // (the roughness and the metalness share one binding: once is enough)
        for (int m = 0; m < kMaterialMapCount; ++m) {
            const MaterialMap  map = static_cast<MaterialMap>(m);
            const std::string& source = materials[mesh].maps[static_cast<std::size_t>(m)].source;
            const bool         parameters = map == MaterialMap::Roughness || map == MaterialMap::Metalness;
            if (source != now[mesh].maps[static_cast<std::size_t>(m)].source) {
                // (A file that has gone: the slot keeps what it has — the warning says which.)
                applySlotSource(model, static_cast<int>(mesh), map, source, &bake);
                parametersBound = parametersBound || parameters;
            } else if (std::find(kOverlaySlots.begin(), kOverlaySlots.end(), map) != kOverlaySlots.end() && !(parameters && parametersBound) &&
                       overlaySignature(before, map, map == MaterialMap::Normal) != overlaySignature(wanted, map, map == MaterialMap::Normal)) {
                rebindSlot(model, static_cast<int>(mesh), map, bake);
                parametersBound = parametersBound || parameters;
            }
        }
    }
}

// --- THE OVERLAYS -------------------------------------------------------------------------------

std::shared_ptr<const OverlayImage> VulkanWindow::overlayImage(const QString& path, OverlayImageKind kind) {
    if (path.isEmpty()) {
        return nullptr;
    }
    // By file and kind (a height is cropped to where it leaves mid grey, a color to its alpha),
    // re-read when the file on disk changed — an image saved again from the paint program shows
    // at the next edit.
    const QFileInfo   info(path);
    const std::string key = std::to_string(static_cast<int>(kind)) + "|" + path.toStdString();
    const qint64      modified = info.exists() ? info.lastModified().toMSecsSinceEpoch() + info.size() : -1;
    if (const auto it = m_overlayImages.find(key); it != m_overlayImages.end() && it->second.stamp == modified) {
        it->second.used = ++m_overlayUse;
        return it->second.image;
    }
    std::shared_ptr<const OverlayImage> image;
    if (modified >= 0) {
        if (const std::shared_ptr<DecodedImage> decoded = decodeTextureFile(path.toStdString())) {
            image = makeOverlayImage(*decoded, kind);
        }
    }
    if (!image) {
        // (Remembered as unreadable until the file changes: a scrub must not retry — and warn —
        // at every step.)
        qWarning().noquote() << "[materials] could not read the overlay image" << path;
    }
    OverlayCacheEntry entry;
    entry.image = image;
    entry.stamp = modified;
    entry.bytes = image ? static_cast<qint64>(image->rgba.size()) : 0;
    entry.used = ++m_overlayUse;
    m_overlayImages[key] = std::move(entry);
    // Over the budget: the least recently used go (never the one just made).
    qint64 total = 0;
    for (const auto& [k, e] : m_overlayImages) {
        total += e.bytes;
    }
    while (total > kOverlayCacheBytes && m_overlayImages.size() > 1) {
        auto oldest = m_overlayImages.end();
        for (auto it = m_overlayImages.begin(); it != m_overlayImages.end(); ++it) {
            if (it->first != key && (oldest == m_overlayImages.end() || it->second.used < oldest->second.used)) {
                oldest = it;
            }
        }
        if (oldest == m_overlayImages.end()) {
            break;
        }
        total -= oldest->second.bytes;
        m_overlayImages.erase(oldest);
    }
    return image;
}

void VulkanWindow::applyZoneOverlays(int model, int mesh, const std::vector<MaterialOverlay>& overlays, OverlayBake& bake) {
    const std::vector<MaterialOverlay> before = scene().meshOverlays(model, mesh);
    if (!scene().setMeshOverlays(model, mesh, overlays)) {
        return; // the stack as it was (or a bad index)
    }
    // Only the slots whose composite the change touches: a hue scrub re-paints the color map
    // and leaves the bump, the roughness and the normal map alone. (The roughness and the
    // metalness are one binding — the parameter map: a change to both rebinds it once.)
    bool parametersBound = false;
    for (const MaterialMap map : kOverlaySlots) {
        const bool bumpAsNormal = map == MaterialMap::Normal;
        const bool parameters = map == MaterialMap::Roughness || map == MaterialMap::Metalness;
        if (parameters && parametersBound) {
            continue;
        }
        if (overlaySignature(before, map, bumpAsNormal) != overlaySignature(overlays, map, bumpAsNormal)) {
            rebindSlot(model, mesh, map, bake);
            parametersBound = parametersBound || parameters;
        }
    }
}

bool VulkanWindow::setMaterialOverlays(const std::vector<MaterialOverlay>& overlays) {
    if (!m_renderer) {
        return false;
    }
    const int model = scene().selectedModelIndex();
    const int mesh = scene().selectedZoneIndex();
    if (model < 0 || mesh < 0) {
        return false;
    }
    // One undo entry — this call's own bracket, or the open edit's (a scrub's begin .. end).
    const bool opened = !m_materialEdit;
    if (opened && !beginMaterialEdit()) {
        return false;
    }
    // The stack belongs to the MAP: every zone painted on the selected zone's color map takes
    // it, and one composite serves them all. (POSESTUDIO_OVERLAY_TRACE=1 prints what one edit
    // cost — the composites made and the time: the number a scrub's smoothness is.)
    static const bool trace = qEnvironmentVariableIsSet("POSESTUDIO_OVERLAY_TRACE");
    QElapsedTimer     clock;
    clock.start();
    OverlayBake            bake;
    const std::vector<int> zones = scene().overlayZones(model, mesh);
    for (const int zone : zones) {
        applyZoneOverlays(model, zone, overlays, bake);
    }
    if (trace) {
        qInfo().noquote() << QStringLiteral("[overlays] %1 overlay(s) on %2 zone(s): %3 image(s) derived in %4 ms")
                                 .arg(overlays.size())
                                 .arg(zones.size())
                                 .arg(bake.images.size())
                                 .arg(clock.elapsed());
    }
    if (opened) {
        endMaterialEdit(); // (registers nothing if the stack ended as it began)
    }
    requestUpdate();
    return true;
}

void VulkanWindow::setModelOverlays(int model, const std::vector<std::pair<int, std::vector<MaterialOverlay>>>& stacks) {
    if (!m_renderer || model < 0) {
        return;
    }
    OverlayBake bake;
    for (const auto& [mesh, overlays] : stacks) {
        applyZoneOverlays(model, mesh, overlays, bake);
    }
}

} // namespace pose
