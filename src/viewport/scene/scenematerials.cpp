/**
 * @file scenematerials.cpp
 * @brief THE MATERIALS TAB's side of the Scene (2026-11-09): the selected object's zones and the
 *        selected zone's material values — what the tab shows and what it sets. See
 *        materialparams.h for the contract and mesh.h / model.h for where the values live.
 */

#include "scene.h"

#include "mesh.h"
#include "model.h"
#include "modeldata.h" // DecodedImage
#include "ray.h"
#include "vulkancontext.h"

#include <algorithm>
#include <string>

namespace pose {

namespace {

/// The file's name without its folders (either separator), for the tab's heading.
std::string baseName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

MaterialView Scene::materialView() const {
    MaterialView view;
    const int    sel = selectedModelIndex();
    const Model* model = sel >= 0 ? modelAt(static_cast<std::size_t>(sel)) : nullptr;
    if (model == nullptr || model->meshCount() == 0) {
        return view; // nothing selected, or a splat scene (no materials to edit)
    }
    view.valid = true;
    view.model = sel;
    view.modelName = baseName(model->sourcePath());
    const std::size_t count = model->meshCount();
    view.zones.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::string& name = model->mesh(i).name();
        view.zones.push_back(name.empty() ? "Zone " + std::to_string(i + 1) : name);
    }
    const int zone = (m_selectedZone >= 0 && static_cast<std::size_t>(m_selectedZone) < count) ? m_selectedZone : 0;
    view.zone = zone;
    const Mesh& mesh = model->mesh(static_cast<std::size_t>(zone));
    view.params = mesh.material();
    view.imported = mesh.importedMaterial();
    view.maps = mesh.materialMaps();
    for (int m = 0; m < kMaterialMapCount; ++m) {
        view.mapNames[static_cast<std::size_t>(m)] = mesh.slotLabel(static_cast<MaterialMap>(m));
        view.mapPaths[static_cast<std::size_t>(m)] = mesh.slotPath(static_cast<MaterialMap>(m));
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (static_cast<int>(i) != zone && model->mesh(i).importedMaterial().nearlyEqual(mesh.importedMaterial())) {
            ++view.linkedZones;
        }
    }
    view.overlayZones = model->overlayZones(static_cast<std::size_t>(zone));
    view.revision = model->materialRevision();
    return view;
}

void Scene::setSelectedZone(int index) {
    const int    sel = selectedModelIndex();
    const Model* model = sel >= 0 ? modelAt(static_cast<std::size_t>(sel)) : nullptr;
    if (model == nullptr || index < 0 || static_cast<std::size_t>(index) >= model->meshCount()) {
        m_selectedZone = -1;
        return;
    }
    m_selectedZone = index;
}

bool Scene::selectZoneAt(const Ray& ray) {
    const int    sel = selectedModelIndex();
    const Model* model = sel >= 0 ? modelAt(static_cast<std::size_t>(sel)) : nullptr;
    if (model == nullptr || model->meshCount() == 0) {
        return false;
    }
    float     t = 0.0f;
    glm::vec3 point(0.0f);
    int       bone = -1;
    int       zone = -1;
    if (!model->pickSurface(ray, t, point, bone, &zone) || zone < 0) {
        return false;
    }
    m_selectedZone = zone;
    return true;
}

bool Scene::setSelectedZoneMaterial(const MaterialParams& params, bool linked) {
    const int sel = selectedModelIndex();
    Model*    model = sel >= 0 ? modelAt(static_cast<std::size_t>(sel)) : nullptr;
    if (model == nullptr || model->meshCount() == 0) {
        return false;
    }
    const std::size_t count = model->meshCount();
    const std::size_t zone = (m_selectedZone >= 0 && static_cast<std::size_t>(m_selectedZone) < count)
                                 ? static_cast<std::size_t>(m_selectedZone)
                                 : 0;
    // The linked zones are read off the IMPORTED materials (a figure's skin zones all carry one
    // preset's values; its eyes and mouth others), so a zone edited apart earlier still follows
    // the group when the group is edited again — the imported material is the group's identity.
    const MaterialParams imported = model->mesh(zone).importedMaterial();
    bool                 changed = model->setMeshMaterial(zone, params);
    if (linked) {
        for (std::size_t i = 0; i < count; ++i) {
            if (i != zone && model->mesh(i).importedMaterial().nearlyEqual(imported)) {
                changed = model->setMeshMaterial(i, params) || changed;
            }
        }
    }
    return changed;
}

std::vector<MaterialParams> Scene::captureMaterials(int model) const {
    const Model* m = model >= 0 ? modelAt(static_cast<std::size_t>(model)) : nullptr;
    return m != nullptr ? m->captureMaterials() : std::vector<MaterialParams>();
}

void Scene::applyMaterials(int model, const std::vector<MaterialParams>& materials) {
    if (Model* m = model >= 0 ? modelAt(static_cast<std::size_t>(model)) : nullptr) {
        m->applyMaterials(materials);
    }
}

int Scene::selectedZoneIndex() const {
    const int    sel = selectedModelIndex();
    const Model* model = sel >= 0 ? modelAt(static_cast<std::size_t>(sel)) : nullptr;
    if (model == nullptr || model->meshCount() == 0) {
        return -1;
    }
    return (m_selectedZone >= 0 && static_cast<std::size_t>(m_selectedZone) < model->meshCount()) ? m_selectedZone : 0;
}

std::shared_ptr<const DecodedImage> Scene::meshSlotImage(int model, int mesh, MaterialMap map, bool imported, int* channel) const {
    const Model* m = model >= 0 ? modelAt(static_cast<std::size_t>(model)) : nullptr;
    if (m == nullptr || mesh < 0 || static_cast<std::size_t>(mesh) >= m->meshCount()) {
        return nullptr;
    }
    const Mesh& zone = m->mesh(static_cast<std::size_t>(mesh));
    if (channel != nullptr) {
        *channel = zone.slotChannel(map, imported);
    }
    return zone.slotImage(map, imported);
}

bool Scene::setMeshTexture(int model, int mesh, MaterialMap map, const std::shared_ptr<const DecodedImage>& plain, int channel,
                           const std::shared_ptr<const DecodedImage>& gpuImage, const std::string& source) {
    Model* m = model >= 0 ? modelAt(static_cast<std::size_t>(model)) : nullptr;
    if (m == nullptr || mesh < 0 || static_cast<std::size_t>(mesh) >= m->meshCount()) {
        return false;
    }
    // The mesh's descriptor set may be referenced by a frame in flight: wait it out, as the
    // environment swap and the delete do.
    vkDeviceWaitIdle(m_context.device());
    return m->setMeshTexture(static_cast<std::size_t>(mesh), map, plain, channel, gpuImage, source);
}

std::vector<MaterialOverlay> Scene::meshOverlays(int model, int mesh) const {
    const Model* m = model >= 0 ? modelAt(static_cast<std::size_t>(model)) : nullptr;
    if (m == nullptr || mesh < 0 || static_cast<std::size_t>(mesh) >= m->meshCount()) {
        return {};
    }
    return m->mesh(static_cast<std::size_t>(mesh)).material().overlays;
}

float Scene::meshMetalness(int model, int mesh) const {
    const Model* m = model >= 0 ? modelAt(static_cast<std::size_t>(model)) : nullptr;
    if (m == nullptr || mesh < 0 || static_cast<std::size_t>(mesh) >= m->meshCount()) {
        return 0.0f;
    }
    return m->mesh(static_cast<std::size_t>(mesh)).material().metalness;
}

float Scene::meshMetalBaked(int model, int mesh) const {
    const Model* m = model >= 0 ? modelAt(static_cast<std::size_t>(model)) : nullptr;
    if (m == nullptr || mesh < 0 || static_cast<std::size_t>(mesh) >= m->meshCount()) {
        return -1.0f;
    }
    return m->mesh(static_cast<std::size_t>(mesh)).metalBaked();
}

void Scene::setMeshMetalBaked(int model, int mesh, float scalar) {
    Model* m = model >= 0 ? modelAt(static_cast<std::size_t>(model)) : nullptr;
    if (m != nullptr && mesh >= 0) {
        m->setMeshMetalBaked(static_cast<std::size_t>(mesh), scalar);
    }
}

bool Scene::setMeshOverlays(int model, int mesh, const std::vector<MaterialOverlay>& overlays) {
    Model* m = model >= 0 ? modelAt(static_cast<std::size_t>(model)) : nullptr;
    return m != nullptr && mesh >= 0 && m->setMeshOverlays(static_cast<std::size_t>(mesh), overlays);
}

std::string Scene::meshSlotSource(int model, int mesh, MaterialMap map) const {
    const Model* m = model >= 0 ? modelAt(static_cast<std::size_t>(model)) : nullptr;
    if (m == nullptr || mesh < 0 || static_cast<std::size_t>(mesh) >= m->meshCount()) {
        return {};
    }
    return m->mesh(static_cast<std::size_t>(mesh)).material().maps[static_cast<std::size_t>(map)].source;
}

std::vector<int> Scene::overlayZones(int model, int mesh) const {
    const Model* m = model >= 0 ? modelAt(static_cast<std::size_t>(model)) : nullptr;
    if (m == nullptr || mesh < 0) {
        return {};
    }
    return m->overlayZones(static_cast<std::size_t>(mesh));
}

bool Scene::resetZoneMaterials(bool allZones, bool linked) {
    const int sel = selectedModelIndex();
    Model*    model = sel >= 0 ? modelAt(static_cast<std::size_t>(sel)) : nullptr;
    if (model == nullptr || model->meshCount() == 0) {
        return false;
    }
    const std::size_t count = model->meshCount();
    bool              changed = false;
    if (allZones) {
        for (std::size_t i = 0; i < count; ++i) {
            changed = model->setMeshMaterial(i, model->mesh(i).importedMaterial()) || changed;
        }
        return changed;
    }
    const std::size_t zone = (m_selectedZone >= 0 && static_cast<std::size_t>(m_selectedZone) < count)
                                 ? static_cast<std::size_t>(m_selectedZone)
                                 : 0;
    const MaterialParams imported = model->mesh(zone).importedMaterial();
    changed = model->setMeshMaterial(zone, imported);
    if (linked) {
        for (std::size_t i = 0; i < count; ++i) {
            if (i != zone && model->mesh(i).importedMaterial().nearlyEqual(imported)) {
                changed = model->setMeshMaterial(i, model->mesh(i).importedMaterial()) || changed;
            }
        }
    }
    return changed;
}

} // namespace pose
