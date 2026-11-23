// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file vulkanwindow_materials.cpp
 * @brief VulkanWindow's side of THE MATERIALS TAB: the view it shows, the edit bracket every
 *        change of a zone's material goes through, the TEXTURE SLOTS (the tab's maps) and the
 *        OVERLAYS (the images layered over them).
 *        (A TU of VulkanWindow — see vulkanwindow.h for the class and its other TUs.)
 *
 * THE EDIT BRACKET (beginMaterialEdit .. endMaterialEdit) makes one undo entry of a gesture,
 * however many values it passes through: the begin snapshots the selected object's zone
 * materials, the end pushes them onto the window's one undo stack (vulkanwindow_pose.cpp) if
 * anything differs. A value, a texture slot and an overlay all change inside it.
 *
 * THE TEXTURE SLOTS. A zone's map is replaced by an image file, cleared, or put back to the one
 * the file imported with. The Qt layer owns the decode (the import's own: texturedecode.h — the
 * same size cap, the same cutout bake and parameter-map build), derives the image the slot's
 * BINDING needs from the slot and its partners (the base color with the cutout baked into its
 * alpha; the roughness / metalness parameter map), and hands the Scene the plain image, the
 * channel, the bound image and the recorded source. Every change is a material edit — one undo
 * entry through the bracket — and the project file carries the sources. Undo and a project
 * opened come back through applyMaterialTextures, which re-derives every slot whose recorded
 * source — or whose overlays — differ from the mesh's.
 *
 * A CHOSEN MAP IS GUTTER-FILLED ONCE, AT ITS DECODE (decodeSlotSource), against its zone's UV
 * islands, as the import fills its own images — never at a rebind, so an overlay's scrub costs
 * what it did. Only while the zone's material leaves its UVs where they are: a material that
 * repeats, shifts or turns its maps draws them where the islands are not, and there the picture
 * is left as the file has it. syncChosenMapFills reads a map again when its zone crosses that
 * line (a map chosen first and tiled after).
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
 * the mesh; syncOverlayBakes re-bakes when the value moves (and binds the normal image again
 * when the zone's Flip Normal moves under overlays painted into it).
 */

#include "vulkanwindow.h"

#include "model.h" // Mesh::material / normalFlipBaked (the overlays under a flipped normal map)
#include "modeldata.h"
#include "texturedecode.h"
#include "texturegutters.h"
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

/// A file as it is on disk now, in one number: its time and size (-1 = no such file). An overlay
/// image is read again when this moves.
qint64 fileStamp(const QString& path) {
    const QFileInfo info(path);
    return info.exists() ? info.lastModified().toMSecsSinceEpoch() + info.size() : -1;
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

/// The slots whose CHOSEN image is gutter-filled at its decode (decodeSlotSource): the ones the
/// viewport samples through the zone's UVs. Not the detail normal, which repeats by a count of
/// its own and has no background (the import leaves it too), and not a slot nothing draws.
bool gutterFilledSlot(MaterialMap map) {
    return materialMapIsRendered(map) && map != MaterialMap::PoreDetail;
}

/// Whether a material MOVES its maps across the zone's UVs — the Tiling section: a repeat, an
/// offset, a turn. Such a zone samples its maps where its UV islands are NOT, so nothing of
/// them is background and a gutter fill would overwrite texels the zone draws (the import's
/// rule too: texturedecode.cpp, fillGutters).
bool movesMapsAcrossUvs(const MaterialParams& p) {
    return p.tilingEnabled && (p.uvTiling != glm::vec2(1.0f) || p.uvOffset != glm::vec2(0.0f) || p.uvRotation != 0.0f);
}

/// A normal image with its GREEN turned over: from OpenGL's convention (green = up the image)
/// to the mesh frame's own (the bitangent runs down it, which is DirectX's) and back.
std::shared_ptr<DecodedImage> greenFlipped(const DecodedImage& image) {
    auto out = std::make_shared<DecodedImage>(image);
    for (std::size_t i = 1; i < out->pixels.size(); i += 4) {
        out->pixels[i] = static_cast<uint8_t>(255 - out->pixels[i]);
    }
    return out;
}

} // namespace

// --- The Materials tab ------------------------------------------------------------------------

MaterialView VulkanWindow::materialView() const {
    return m_renderer ? scene().materialView() : MaterialView{};
}

void VulkanWindow::setMaterialZone(int zone) {
    if (!m_renderer) {
        return;
    }
    endMaterialEdit(); // an edit belongs to the zone it began on
    scene().setSelectedZone(zone);
    requestUpdate(); // (the frame's notify tells the tab)
}

bool VulkanWindow::beginMaterialEdit() {
    if (m_materialEdit) {
        return true; // already open (a set opened it, or a nested begin)
    }
    if (!m_renderer) {
        return false;
    }
    const int model = scene().selectedModelIndex();
    if (model < 0) {
        return false; // nothing selected: nothing to edit
    }
    m_materialEditModel = model;
    m_preEditMaterials = scene().captureMaterials(model);
    m_materialEdit = true;
    return true;
}

void VulkanWindow::setSelectedMaterial(const MaterialParams& params, bool linked) {
    if (!beginMaterialEdit()) {
        return;
    }
    if (scene().selectedModelIndex() != m_materialEditModel) {
        // The selection moved under an open bracket — by a script or a VR controller: the tab's
        // color dialogs are window-modal, so the mouse cannot reach the viewport under one — the
        // first object's entry is committed and the edit goes on with the new selection.
        endMaterialEdit();
        if (!beginMaterialEdit()) {
            return;
        }
    }
    if (scene().setSelectedZoneMaterial(params, linked)) {
        syncChosenMapFills(m_materialEditModel); // (a Tiling value moved under a chosen map's gutter fill)
        syncOverlayBakes(m_materialEditModel);   // (a Metallic value moved under an overlay's baked metal)
        requestUpdate(); // the values ride the model's material buffer: the next frame shows them
    }
}

void VulkanWindow::endMaterialEdit() {
    if (!m_materialEdit) {
        return;
    }
    m_materialEdit = false;
    if (m_renderer) {
        commitMaterialUndo(); // a no-op if nothing changed
        requestUpdate();
    }
    m_materialEditModel = -1;
}

void VulkanWindow::commitMaterialUndo() {
    if (!m_renderer || m_materialEditModel < 0) {
        return;
    }
    const std::vector<MaterialParams> now = scene().captureMaterials(m_materialEditModel);
    if (!now.empty() && now != m_preEditMaterials) {
        UndoEntry entry;
        entry.kind = UndoEntry::Kind::Material;
        entry.figure = m_materialEditModel;
        entry.materials = m_preEditMaterials;
        m_undoStack.push_back(std::move(entry));
        m_redoStack.clear();
        markProjectDirty(); // a committed material edit is a document change
    }
}

void VulkanWindow::resetSelectedMaterial(bool linked, bool allZones) {
    if (!m_renderer) {
        return;
    }
    endMaterialEdit();
    if (!beginMaterialEdit()) {
        return;
    }
    scene().resetZoneMaterials(allZones, linked);
    syncChosenMapFills(m_materialEditModel);
    syncOverlayBakes(m_materialEditModel);
    endMaterialEdit(); // registers no entry if nothing differed from the imported values
}

void VulkanWindow::notifyMaterialView() {
    const MaterialViewKey now = m_renderer ? scene().materialViewKey() : MaterialViewKey{};
    if (now != m_announcedMaterialKey) {
        m_announcedMaterialKey = now;
        emit materialViewChanged();
    }
}

// --- The texture slots ------------------------------------------------------------------------

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

const std::shared_ptr<const DecodedImage>* VulkanWindow::OverlayBake::find(const std::string& key) {
    const auto it = entries.find(key);
    if (it == entries.end()) {
        return nullptr;
    }
    it->second.used = true;
    return &it->second.image;
}

void VulkanWindow::OverlayBake::beginPass() {
    for (auto& [key, entry] : entries) {
        entry.used = false;
    }
}

void VulkanWindow::OverlayBake::dropUnused() {
    for (auto it = entries.begin(); it != entries.end();) {
        it = it->second.used ? std::next(it) : entries.erase(it);
    }
}

std::shared_ptr<const DecodedImage> VulkanWindow::OverlayBake::put(const std::string& key, std::shared_ptr<const DecodedImage> image,
                                                                   std::vector<std::shared_ptr<const DecodedImage>> sources) {
    Entry& entry = entries[key];
    entry.image = std::move(image);
    entry.sources = std::move(sources);
    entry.used = true;
    return entry.image;
}

bool VulkanWindow::decodeSlotSource(int model, int mesh, MaterialMap map, const std::string& source,
                                    std::shared_ptr<const DecodedImage>& plain, int& channel, const MaterialParams* standing) {
    // The slot's new PLAIN image: the imported one back, a file decoded, or none.
    plain.reset();
    channel = 0;
    if (source == kImportedTexture) {
        plain = scene().meshSlotImage(model, mesh, map, /*imported=*/true, &channel);
    } else if (!source.empty()) {
        // A cutout mask is read as gray (the import's decodeMask: a single-channel or RGB mask
        // alike); every other slot as RGBA. A roughness or metalness file read here is a plain
        // grayscale map whose RED channel is the value — unless the source names ANOTHER channel
        // (textureSourcePath: a packed image, a cutout in a color image's alpha) — and is read
        // INVERTED where the source says so (a glossiness map in a roughness slot).
        int                           wanted = 0;
        bool                          inverted = false;
        const std::string             path = textureSourcePath(source, &wanted, &inverted);
        std::shared_ptr<DecodedImage> decoded;
        if (map == MaterialMap::Opacity) {
            if (wanted != 0) {
                const std::shared_ptr<DecodedImage> image = decodeTextureFile(path);
                decoded = image ? channelAsMask(*image, wanted) : nullptr;
            } else {
                decoded = decodeMaskFile(path);
            }
            channel = 0;
        } else {
            decoded = decodeTextureFile(path);
            channel = wanted;
        }
        if (!decoded) {
            qWarning().noquote() << "[materials] could not read the image" << QString::fromStdString(path);
            return false;
        }
        if (inverted) {
            // The color channels turned over, the alpha left: from here on the image IS the map
            // its slot holds (what is packed, bound, rendered and saved is this picture).
            std::vector<uint8_t>& pixels = decoded->pixels;
            for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
                pixels[i] = static_cast<uint8_t>(255 - pixels[i]);
                pixels[i + 1] = static_cast<uint8_t>(255 - pixels[i + 1]);
                pixels[i + 2] = static_cast<uint8_t>(255 - pixels[i + 2]);
            }
        }
        // THE GUTTER FILL, once per chosen image and here: the atlas background between the
        // zone's UV islands is flooded with their colors, so the mip chain never averages it
        // through a seam (texturegutters.h) — what the import does for every image of its own,
        // and what a map chosen on the tab went without: a skin map replaced by an edited copy
        // showed its seams at a distance. Against THIS zone's islands (a slot is the zone's own),
        // before anything is derived from the image, so the packed and baked bindings and the
        // overlays' composites start from a filled picture and no edit step fills anything.
        const Model* owner = model >= 0 ? scene().modelAt(static_cast<std::size_t>(model)) : nullptr;
        if (gutterFilledSlot(map) && owner != nullptr && mesh >= 0 && static_cast<std::size_t>(mesh) < owner->meshCount()) {
            const Mesh& zone = owner->mesh(static_cast<std::size_t>(mesh));
            if (!movesMapsAcrossUvs(standing != nullptr ? *standing : zone.material())) {
                fillImageGutters(decoded->pixels, decoded->width, decoded->height, {{&zone.cpuVertices(), &zone.cpuIndices()}});
                for (auto it = m_gutterFilled.begin(); it != m_gutterFilled.end();) {
                    it = it->second.expired() ? m_gutterFilled.erase(it) : std::next(it);
                }
                m_gutterFilled[decoded.get()] = decoded;
            }
        }
        plain = std::move(decoded);
    }
    return true;
}

bool VulkanWindow::applySlotSource(int model, int mesh, MaterialMap map, const std::string& source, OverlayBake* bake) {
    std::shared_ptr<const DecodedImage> plain;
    int                                 channel = 0;
    if (!decodeSlotSource(model, mesh, map, source, plain, channel)) {
        return false;
    }
    OverlayBake local;
    return bindSlot(model, mesh, map, plain, channel, source, bake != nullptr ? *bake : local);
}

std::vector<MaterialMap> VulkanWindow::applySlotSources(int model, int mesh, const std::vector<std::pair<MaterialMap, std::string>>& changes,
                                                        OverlayBake& bake, const MaterialParams* standing) {
    // Decode first, bind after: bound one at a time, each slot of a packed binding would pack and
    // upload the map again with the partners it found (the parameter map up to four times). With
    // every new image known, each slot of a group derives the SAME bound image — made once (the
    // bake) and uploaded once (SlotUploads).
    std::vector<MaterialMap> unread;
    PendingSlots             pending;
    for (const auto& [map, source] : changes) {
        PendingSlots::Slot& slot = pending.bySlot[static_cast<std::size_t>(map)];
        if (decodeSlotSource(model, mesh, map, source, slot.plain, slot.channel, standing)) {
            slot.set = true;
        } else {
            unread.push_back(map);
        }
    }
    for (const auto& [map, source] : changes) {
        const PendingSlots::Slot& slot = pending.bySlot[static_cast<std::size_t>(map)];
        if (slot.set) {
            bindSlot(model, mesh, map, slot.plain, slot.channel, source, bake, &pending);
        }
    }
    return unread;
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
    // THE NORMAL MAP'S CONVENTION (MaterialParams::flipNormal): the overlays are painted in the
    // compositor's own convention (y down the image: DirectX's), while the zone's normal map —
    // unless it is marked DirectX-style — is OpenGL's, and is read as OpenGL's by the viewport's
    // shader and the render engine alike. So such a map is turned to the compositor's
    // convention, painted, and turned back: its readers then right the map and its overlays
    // together. (A zone with no normal map of its own too: its canvas is read the same way.)
    const Model* owner = model >= 0 ? scene().modelAt(static_cast<std::size_t>(model)) : nullptr;
    const bool   flipped = map == MaterialMap::Normal && owner != nullptr && mesh >= 0 && static_cast<std::size_t>(mesh) < owner->meshCount() &&
                         !owner->mesh(static_cast<std::size_t>(mesh)).material().flipNormal;
    std::string key = std::string(materialMapName(map)) + (forRenderer ? "@render#" : "#") + pointerKey(base.get()) + "#" +
                      std::to_string(channel) + "#" + scale + (flipped ? "#gl#" : "#") + signature;
    if (bake.kept) {
        // (A kept bake outlives the edit its entries were made in: a composite is the one asked
        // for only while the files it was painted from are the files on disk.)
        for (const MaterialOverlay& overlay : overlays) {
            for (int element = 0; element < static_cast<int>(OverlayElement::Count); ++element) {
                const std::string& file = overlayElementSource(overlay, static_cast<OverlayElement>(element));
                if (!file.empty()) {
                    key += "|" + std::to_string(fileStamp(QString::fromStdString(file)));
                }
            }
        }
    }
    if (const auto* made = bake.find(key)) {
        return *made;
    }
    const std::shared_ptr<const DecodedImage> canvas = flipped && base ? greenFlipped(*base) : base;
    std::shared_ptr<const DecodedImage> painted = compositeOverlays(
        map, canvas, channel, overlays, [this](const std::string& path, OverlayImageKind kind) { return overlayImage(QString::fromStdString(path), kind); },
        bumpAsNormal, baseScale);
    if (!painted) {
        painted = base; // (every image missing: the map as it is)
    } else if (flipped) {
        painted = greenFlipped(*painted); // (back to the map's own convention, OpenGL's)
    }
    return bake.put(key, painted, {base});
}

bool VulkanWindow::bindSlot(int model, int mesh, MaterialMap map, const std::shared_ptr<const DecodedImage>& plain, int channel,
                            const std::string& source, OverlayBake& bake, const PendingSlots* pending) {
    // The image the slot's BINDING samples, derived with its partner slots' plain images (as they
    // stand, or as the same edit is about to make them) and the zone's overlays: BaseColor +
    // Opacity share the painted diffuse with the cutout in
    // its alpha, Roughness + Metalness + AmbientOcclusion + TransmissionWeight the parameter map
    // (the overlays' gloss in its roughness), CoatWeight + CoatRoughness + SheenWeight +
    // ThinFilmThickness the layer map; Normal binds its image with the overlays' relief (their
    // normal images, and their bump images as slopes); the rest bind their plain image as it is.
    std::shared_ptr<const DecodedImage> bound;
    bool                                parameterMap = false; // the binding is the parameter map: its metal record follows
    bool                                normalSlot = false;   // the binding is the normal image: its Flip Normal record follows
    float                               metalBaked = -1.0f;   // the Metallic value the metalness was baked for (< 0: not baked)
    // A partner slot's plain image and channel: this one's new image where it IS the slot, the
    // image the same edit is about to give it where it is one of @p pending, else what it holds.
    const auto partner = [&](MaterialMap other, int* otherChannel) -> std::shared_ptr<const DecodedImage> {
        if (other == map) {
            if (otherChannel != nullptr) {
                *otherChannel = channel;
            }
            return plain;
        }
        if (pending != nullptr && pending->bySlot[static_cast<std::size_t>(other)].set) {
            const PendingSlots::Slot& slot = pending->bySlot[static_cast<std::size_t>(other)];
            if (otherChannel != nullptr) {
                *otherChannel = slot.plain ? slot.channel : 0; // (as meshSlotImage answers for an empty slot)
            }
            return slot.plain;
        }
        return scene().meshSlotImage(model, mesh, other, /*imported=*/false, otherChannel);
    };
    // A derived image by its key — which names @p sources by address — made once per edit (the
    // zones of one map, and the slots of one packed binding, share it).
    const auto shared = [&bake](const std::string& key, std::vector<std::shared_ptr<const DecodedImage>> sources,
                                const auto& make) -> std::shared_ptr<const DecodedImage> {
        if (const auto* made = bake.find(key)) {
            return *made;
        }
        return bake.put(key, make(), std::move(sources));
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
                      : shared("cutout#" + pointerKey(painted.get()) + "#" + pointerKey(mask.get()), {painted, mask},
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
                       {glossed, metalBound, occlusion, transmission},
                       [&]() -> std::shared_ptr<const DecodedImage> {
                           return buildParameterMap(glossed, roughChannel, metalBound, metalChannel, occlusion, transmission);
                       });
        break;
    }
    case MaterialMap::CoatWeight:
    case MaterialMap::CoatRoughness:
    case MaterialMap::SheenWeight:
    case MaterialMap::ThinFilmThickness: {
        const auto coatWeight = partner(MaterialMap::CoatWeight, nullptr);
        const auto coatRoughness = partner(MaterialMap::CoatRoughness, nullptr);
        const auto sheenWeight = partner(MaterialMap::SheenWeight, nullptr);
        const auto filmThickness = partner(MaterialMap::ThinFilmThickness, nullptr);
        bound = shared("layer#" + pointerKey(coatWeight.get()) + "#" + pointerKey(coatRoughness.get()) + "#" + pointerKey(sheenWeight.get()) +
                           "#" + pointerKey(filmThickness.get()),
                       {coatWeight, coatRoughness, sheenWeight, filmThickness},
                       [&]() -> std::shared_ptr<const DecodedImage> {
                           return buildLayerMap(coatWeight, coatRoughness, sheenWeight, filmThickness);
                       });
        break;
    }
    case MaterialMap::Normal:
        bound = overlaid(model, mesh, map, plain, 0, bake);
        normalSlot = true;
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
    if (normalSlot) {
        // (Overlays painted into it: painted for the zone's Flip Normal as it stands — see overlaid.)
        if (Model* owner = model >= 0 ? scene().modelAt(static_cast<std::size_t>(model)) : nullptr;
            owner != nullptr && mesh >= 0 && static_cast<std::size_t>(mesh) < owner->meshCount()) {
            const bool flip = owner->mesh(static_cast<std::size_t>(mesh)).material().flipNormal;
            owner->setMeshNormalFlipBaked(static_cast<std::size_t>(mesh), bound != plain ? (flip ? 1 : 0) : -1);
        }
    }
    return applied;
}

void VulkanWindow::syncChosenMapFills(int model) {
    if (!m_renderer || model < 0) {
        return;
    }
    const Model* owner = scene().modelAt(static_cast<std::size_t>(model));
    if (owner == nullptr) {
        return;
    }
    // (Like syncOverlayBakes this runs at every step of a material scrub, and reads each mesh's
    // material where it is. It finds nothing to do but at the one step that takes a zone's
    // Tiling off the identity or back onto it — a map chosen and THEN tiled must not keep a
    // fill made for the islands as they lay.)
    OverlayBake bake;
    for (std::size_t mesh = 0; mesh < owner->meshCount(); ++mesh) {
        const MaterialParams& material = owner->mesh(mesh).material();
        const bool            wanted = !movesMapsAcrossUvs(material);
        std::vector<std::pair<MaterialMap, std::string>> again;
        for (int m = 0; m < kMaterialMapCount; ++m) {
            const MaterialMap  map = static_cast<MaterialMap>(m);
            const std::string& source = material.maps[static_cast<std::size_t>(m)].source;
            if (!gutterFilledSlot(map) || source.empty() || source == kImportedTexture) {
                continue; // (an imported image is the import's: filled or not there, for good)
            }
            const auto plain = scene().meshSlotImage(model, static_cast<int>(mesh), map, /*imported=*/false);
            if (!plain) {
                continue;
            }
            const auto filledIt = m_gutterFilled.find(plain.get());
            const bool filled = filledIt != m_gutterFilled.end() && filledIt->second.lock() == plain;
            // (A file that has gone since keeps the picture it gave: asked for at every step, it
            // would be a warning a step.)
            if (filled != wanted && QFileInfo::exists(QString::fromStdString(textureSourcePath(source)))) {
                again.emplace_back(map, source);
            }
        }
        if (!again.empty()) {
            applySlotSources(model, static_cast<int>(mesh), again, bake);
        }
    }
}

void VulkanWindow::syncOverlayBakes(int model) {
    if (!m_renderer || model < 0) {
        return;
    }
    const Model* owner = scene().modelAt(static_cast<std::size_t>(model));
    if (owner == nullptr) {
        return;
    }
    // (This runs at every step of a material scrub: the two values are read off each mesh where
    // they are — not from a copy of every zone's whole material.)
    OverlayBake bake;
    for (std::size_t mesh = 0; mesh < owner->meshCount(); ++mesh) {
        // The overlays' METAL is baked for the zone's Metallic value …
        const float baked = owner->mesh(mesh).metalBaked();
        if (baked >= 0.0f && baked != owner->mesh(mesh).material().metalness) {
            rebindSlot(model, static_cast<int>(mesh), MaterialMap::Metalness, bake);
        }
        // … and their relief is painted for its Flip Normal.
        const int flipBaked = owner->mesh(mesh).normalFlipBaked();
        if (flipBaked >= 0 && (flipBaked == 1) != owner->mesh(mesh).material().flipNormal) {
            rebindSlot(model, static_cast<int>(mesh), MaterialMap::Normal, bake);
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
        // The slots whose source differs, as ONE batch (a packed binding is made once for all of
        // its slots that change) …
        std::vector<std::pair<MaterialMap, std::string>> changes;
        bool parametersBound = false; // (the roughness and the metalness share one binding: once is enough)
        for (int m = 0; m < kMaterialMapCount; ++m) {
            const MaterialMap  map = static_cast<MaterialMap>(m);
            const std::string& source = materials[mesh].maps[static_cast<std::size_t>(m)].source;
            if (source != now[mesh].maps[static_cast<std::size_t>(m)].source) {
                changes.emplace_back(map, source);
                parametersBound = parametersBound || map == MaterialMap::Roughness || map == MaterialMap::Metalness;
            }
        }
        // (A file that has gone: the slot keeps what it has — the warning says which. The maps
        // are decoded for the material the zone is about to carry: its Tiling decides the
        // gutter fill.)
        applySlotSources(model, static_cast<int>(mesh), changes, bake, &materials[mesh]);
        // … then the slots whose source stands and whose overlays differ.
        for (const MaterialMap map : kOverlaySlots) {
            const std::size_t m = static_cast<std::size_t>(map);
            const bool        parameters = map == MaterialMap::Roughness || map == MaterialMap::Metalness;
            if (materials[mesh].maps[m].source == now[mesh].maps[m].source && !(parameters && parametersBound) &&
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
    const std::string key = std::to_string(static_cast<int>(kind)) + "|" + path.toStdString();
    const qint64      modified = fileStamp(path);
    if (const auto it = m_overlayImages.find(key); it != m_overlayImages.end() && it->second.stamp == modified) {
        it->second.used = ++m_overlayUse;
        return it->second.image;
    }
    std::shared_ptr<const OverlayImage> image;
    if (modified >= 0) {
        if (std::shared_ptr<DecodedImage> decoded = decodeTextureFile(path.toStdString())) {
            if (kind == OverlayImageKind::Normal) {
                // (An overlay's normal image is OpenGL-style, as every normal map is taken to be:
                // turned here to the convention the compositor paints in.)
                decoded = greenFlipped(*decoded);
            }
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
                                 .arg(bake.entries.size())
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
