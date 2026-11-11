/**
 * @file vulkanwindow_materials.cpp
 * @brief VulkanWindow's TEXTURE SLOTS — the Materials tab's Textures group (2026-11-09).
 *
 * A zone's map is replaced by an image file, cleared, or put back to the one the file imported
 * with. The Qt layer owns the decode (the import's own: modelimportservice.h — the same size cap,
 * the same cutout bake and parameter-map build), derives the image the slot's BINDING needs from
 * the slot and its partner (the base colour with the cutout baked into its alpha; the roughness /
 * metalness parameter map), and hands the Scene the plain image, the channel, the bound image and
 * the recorded source. Every change is a material edit: one undo entry through the tab's bracket
 * (beginMaterialEdit .. endMaterialEdit), and the project file carries the sources. Undo and a
 * project opened come back through applyMaterialTextures, which re-derives every slot whose
 * recorded source differs from the mesh's.
 */

#include "vulkanwindow.h"

#include "modeldata.h"
#include "modelimportservice.h"
#include "scene.h"

#include <QDebug>
#include <QString>

#include <algorithm>

namespace pose {

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

bool VulkanWindow::applySlotSource(int model, int mesh, MaterialMap map, const std::string& source) {
    // The slot's new PLAIN image: the imported one back, a file decoded, or none.
    std::shared_ptr<const DecodedImage> plain;
    int                                 channel = 0;
    if (source == kImportedTexture) {
        plain = scene().meshSlotImage(model, mesh, map, /*imported=*/true, &channel);
    } else if (!source.empty()) {
        // A cutout mask is read as gray (the import's decodeMask: a single-channel or RGB mask
        // alike); every other slot as RGBA. A roughness or metalness file read here is a plain
        // grayscale map whose RED channel is the value.
        plain = map == MaterialMap::Opacity ? decodeMaskFile(source) : decodeTextureFile(source);
        channel = 0;
        if (!plain) {
            qWarning().noquote() << "[materials] could not read the image" << QString::fromStdString(source);
            return false;
        }
    }
    // The image the slot's BINDING samples, derived with its partner slots' current plain images:
    // BaseColor + Opacity share the baked diffuse, Roughness + Metalness + AmbientOcclusion +
    // TransmissionWeight the parameter map, CoatWeight + CoatRoughness + SheenWeight +
    // ThinFilmThickness the layer map; the rest bind their plain image as it is.
    std::shared_ptr<const DecodedImage> bound;
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
    switch (map) {
    case MaterialMap::BaseColor: {
        const auto mask = scene().meshSlotImage(model, mesh, MaterialMap::Opacity, /*imported=*/false);
        bound = bakeCutoutIntoBaseColor(plain, mask);
        break;
    }
    case MaterialMap::Opacity: {
        const auto base = scene().meshSlotImage(model, mesh, MaterialMap::BaseColor, /*imported=*/false);
        bound = bakeCutoutIntoBaseColor(base, plain);
        break;
    }
    case MaterialMap::Roughness:
    case MaterialMap::Metalness:
    case MaterialMap::AmbientOcclusion:
    case MaterialMap::TransmissionWeight: {
        int        roughChannel = 0;
        int        metalChannel = 0;
        const auto rough = partner(MaterialMap::Roughness, &roughChannel);
        const auto metal = partner(MaterialMap::Metalness, &metalChannel);
        bound = buildParameterMapImage(rough, roughChannel, metal, metalChannel, partner(MaterialMap::AmbientOcclusion, nullptr),
                                       partner(MaterialMap::TransmissionWeight, nullptr));
        break;
    }
    case MaterialMap::CoatWeight:
    case MaterialMap::CoatRoughness:
    case MaterialMap::SheenWeight:
    case MaterialMap::ThinFilmThickness:
        bound = buildLayerMapImage(partner(MaterialMap::CoatWeight, nullptr), partner(MaterialMap::CoatRoughness, nullptr),
                                   partner(MaterialMap::SheenWeight, nullptr), partner(MaterialMap::ThinFilmThickness, nullptr));
        break;
    default:
        bound = materialMapIsRendered(map) ? plain : nullptr; // (a stored-only slot binds nothing)
        break;
    }
    // (An import without a map in this slot has nothing to put back: the Mesh records a null
    // plain image as "none", whatever source was asked.)
    return scene().setMeshTexture(model, mesh, map, plain, channel, bound, source);
}

void VulkanWindow::applyMaterialTextures(int model, const std::vector<MaterialParams>& materials) {
    if (!m_renderer || model < 0) {
        return;
    }
    // Every slot of every zone whose recorded source differs from what the mesh carries now —
    // the undo swap's and the project load's texture half, run BEFORE the values are applied.
    const std::vector<MaterialParams> now = scene().captureMaterials(model);
    const std::size_t                 count = std::min(now.size(), materials.size());
    for (std::size_t mesh = 0; mesh < count; ++mesh) {
        for (int m = 0; m < kMaterialMapCount; ++m) {
            const std::string& wanted = materials[mesh].maps[static_cast<std::size_t>(m)].source;
            if (wanted == now[mesh].maps[static_cast<std::size_t>(m)].source) {
                continue;
            }
            if (!applySlotSource(model, static_cast<int>(mesh), static_cast<MaterialMap>(m), wanted)) {
                // A file that has gone: the slot keeps what it has (the warning above says which).
                continue;
            }
        }
    }
}

} // namespace pose
