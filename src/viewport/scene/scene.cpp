/**
 * @file scene.cpp
 * @brief Scene construction/teardown, the environment upload, the models, the light objects and
 *        what is selected, the per-frame record sequence (shadow, outline, scene, overlays,
 *        splats), and the active-figure logic. See scene.h.
 *
 * The first of Scene's four translation units, and the only one that records commands: the CPU
 * picking is scenepicking.cpp's, the posing forwarders sceneposing.cpp's, the Materials tab's
 * side scenematerials.cpp's.
 */

#include "scene.h"

#include "camera.h"
#include "cameraubo.h"
#include "iblmaps.h"
#include "lineoverlay.h"
#include "model.h"
#include "modeldata.h"
#include "outlinepass.h"
#include "scenepipelines.h"
#include "shadowpass.h"
#include "splatcloud.h"
#include "vulkancontext.h"
#include "vulkanpipeline.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace pose {

Scene::Scene(VulkanContext& context, VkRenderPass renderPass, VkRenderPass outlineMaskPass, VkRenderPass photoPass,
             const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv,
             const std::vector<char>& skeletonVertSpirv, const std::vector<char>& skeletonFragSpirv,
             const std::vector<char>& shadowVertSpirv, const std::vector<char>& shadowFragSpirv,
             const std::vector<char>& backgroundVertSpirv,
             const std::vector<char>& backgroundFragSpirv,
             const std::vector<char>& outlineMaskFragSpirv,
             const std::vector<char>& splatVertSpirv, const std::vector<char>& splatFragSpirv,
             const std::vector<char>& splatMaskFragSpirv)
    : m_context(context) {
    // The shared plumbing first: every pass below builds against its layouts.
    m_pipelines = std::make_unique<ScenePipelines>(m_context, renderPass, photoPass, vertSpirv, fragSpirv,
                                                   backgroundVertSpirv, backgroundFragSpirv,
                                                   splatVertSpirv, splatFragSpirv);

    // The shadow pass (map + pipeline) needs the pose set layout; the scene-wide set 3 needs the
    // map — so the pass is built right after the layouts and its map written into set 3 next,
    // all before the first frame can sample it.
    m_shadowPass = std::make_unique<ShadowPass>(m_context, m_pipelines->poseSetLayout(),
                                                shadowVertSpirv, shadowFragSpirv);
    m_pipelines->writeShadowMap(m_shadowPass->shadowMap());

    // The selection-outline mask pass (shadow.vert again, projected by the camera) and the line
    // overlay (skeleton, pin markers, the orthographic floor line).
    m_outlinePass = std::make_unique<OutlinePass>(m_context, outlineMaskPass,
                                                  m_pipelines->poseSetLayout(), shadowVertSpirv,
                                                  outlineMaskFragSpirv, m_pipelines->cameraSetLayout(),
                                                  m_pipelines->splatSetLayout(), splatVertSpirv,
                                                  splatMaskFragSpirv);
    m_lineOverlay = std::make_unique<LineOverlay>(m_context, renderPass,
                                                  m_pipelines->cameraSetLayout(),
                                                  skeletonVertSpirv, skeletonFragSpirv);

    // Bake the default procedural studio environment synchronously so descriptor set 3 is valid before
    // the first frame. A real HDRI is baked off the render thread (VulkanWindow) and swapped in when
    // ready via applyBakedEnvironment().
    applyBakedEnvironment(bakeEnvironment(generateStudioEnvironment()));
}

Scene::~Scene() {
    // Nothing in flight may reference the pipeline/descriptors when we tear them down. The
    // models go first (their sets come from their own pools but were written against the
    // shared layouts); every other member is an RAII owner released in reverse declaration
    // order.
    vkDeviceWaitIdle(m_context.device());
    m_models.clear();
}

void Scene::applyBakedEnvironment(const BakedEnvironment& baked) {
    // The mesh pipeline samples the specular cubemap + LUT this is about to replace; ensure no in-flight
    // frame is still reading the old IblMaps before it's destroyed. (The expensive CPU bake — SH +
    // prefiltered specular — already happened in bakeEnvironment(), off the render thread for switches.)
    vkDeviceWaitIdle(m_context.device());
    // The BRDF LUT is environment-independent (roughness + NdotV only), so integrate it once and reuse
    // — re-running the ~8M-sample integration on every HDRI swap would be pure waste.
    if (m_brdfLut.data.empty()) {
        m_brdfLut = integrateBrdfLut();
    }
    // The new maps BEFORE anything of the old environment is replaced: their upload can throw
    // (the window catches it and keeps the environment it had), and the diffuse SH must not
    // then be the new environment's beside the old one's specular cube.
    std::unique_ptr<IblMaps> maps = std::make_unique<IblMaps>(m_context, baked.specular, m_brdfLut);
    m_environmentSH = baked.sh;
    m_iblMaps = std::move(maps);
    m_pipelines->writeEnvironment(*m_iblMaps);
}

VkDescriptorSetLayout Scene::iblSetLayout() const { return m_pipelines->iblSetLayout(); }

VkDescriptorSet Scene::iblSet() const { return m_pipelines->iblSet(); }

const glm::mat4& Scene::lightViewProj() const { return m_shadowPass->lightViewProj(); }

void Scene::setShowSkeleton(bool on) { m_lineOverlay->setShowSkeleton(on); }

bool Scene::showSkeleton() const { return m_lineOverlay->showSkeleton(); }

void Scene::setVrPointerLines(std::vector<PointerLine> lines) { m_lineOverlay->setPointerLines(std::move(lines)); }

bool Scene::addModel(const ModelData& data) {
    std::unique_ptr<Model> model = std::make_unique<Model>(m_context, data, m_pipelines->materialSetLayout(),
                                                           m_pipelines->poseSetLayout(),
                                                           m_pipelines->fallbackDiffuse(),
                                                           m_pipelines->fallbackNormal());
    // A model with no mesh to draw (a file of points or lines alone: no mesh with a triangle) is
    // NOT kept. It has no bounds and no surface, so nothing could pick, select or delete it —
    // yet it counted as a model, was written into a project by its source path and came back
    // as a phantom at every Open.
    if (!model->hasMeshes()) {
        return false;
    }
    ++m_modelsRevision;
    m_models.push_back(std::move(model));
    // The new arrival is NOT selected (2026-11-06; the user: "when an object is brought into a
    // scene it should not be selected by default" — until then every import arrived outlined).
    // The selection stays as it was; a figure imported into an empty scene is the posing target
    // all the same, activeFigureIndex() falling back to the first figure until a joint is clicked.
    return true;
}

void Scene::addSplatModel(const SplatData& data, const glm::mat4& transform) {
    ++m_modelsRevision;
    m_models.push_back(std::make_unique<Model>(m_context, data, m_pipelines->splatSetLayout()));
    m_models.back()->setTransform(transform);
    // Not selected either (above).
}

bool Scene::hasMeshes() const {
    for (const std::unique_ptr<Model>& model : m_models) {
        if (model->hasMeshes()) {
            return true;
        }
    }
    return false;
}

bool Scene::selectedIsSplat() const {
    const int selected = selectedModelIndex();
    return selected >= 0 && m_models[static_cast<std::size_t>(selected)]->splats() != nullptr;
}

bool Scene::hasSplats() const {
    for (const std::unique_ptr<Model>& model : m_models) {
        if (model->splats() != nullptr) {
            return true;
        }
    }
    return false;
}

void Scene::buildSplatOccluderPipeline(VkRenderPass renderPass, const std::vector<char>& vertSpirv,
                                       const std::vector<char>& fragSpirv) {
    m_pipelines->buildDepthCutout(m_context, renderPass, vertSpirv, fragSpirv);
}

void Scene::writeCameraUbo(const Camera& camera, uint32_t frameIndex) {
    writeCameraUbo(camera, frameIndex, shadeModeSpec());
}

void Scene::writeCameraUbo(const Camera& camera, uint32_t frameIndex, const ShadeMode& spec) {
    // This frame's camera + lighting block (see fillCameraUbo for what feeds which mode).
    CameraUbo ubo{};
    fillCameraUbo(ubo, camera, m_lighting, spec, m_environmentSH, m_shadowPass->lightViewProj(), m_keyLightDir,
                  m_selectionStyle);
    fillCameraUboLights(ubo, m_lights);
    std::memcpy(m_pipelines->cameraUboData(frameIndex), &ubo, sizeof(ubo));
}

void Scene::recordSplatOccluders(VkCommandBuffer cmd, uint32_t frameIndex, bool opaque, bool fine) {
    if (m_models.empty()) {
        return;
    }
    const VulkanPipeline& hiddenLine = m_pipelines->hiddenLine();
    const VkDescriptorSet cameraSet = m_pipelines->cameraSet(frameIndex);
    const VkDescriptorSet iblSet = m_pipelines->iblSet();
    // (Every mesh pipeline shares one layout: the camera set and the scene-wide set are bound
    // once, as Scene::record binds them.)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hiddenLine.layout(), 0, 1, &cameraSet, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hiddenLine.layout(), 3, 1, &iblSet, 0, nullptr);
    if (opaque) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hiddenLine.handle());
        for (const std::unique_ptr<Model>& model : m_models) {
            model->recordDepthFill(cmd, hiddenLine.layout(), frameIndex);
        }
    }
    // POSESTUDIO_SPLAT_NO_CUTOUT=1: the see-through meshes write no depth, as before 2026-11-12
    // (the A/B: a capture behind a head of hair cards is then drawn over the hair).
    static const bool kNoCutout = std::getenv("POSESTUDIO_SPLAT_NO_CUTOUT") != nullptr;
    if (const VulkanPipeline* cutout = kNoCutout ? nullptr : m_pipelines->depthCutout()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cutout->handle());
        for (const std::unique_ptr<Model>& model : m_models) {
            model->recordDepthCutout(cmd, cutout->layout(), frameIndex, fine);
        }
    }
}

void Scene::recordSplats(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex, bool asyncSort,
                         const glm::vec4& cut) {
    if (!hasSplats()) {
        return;
    }
    const VulkanPipeline& pipeline = m_pipelines->splat();
    const VkDescriptorSet cameraSet = m_pipelines->cameraSet(frameIndex);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1, &cameraSet, 0, nullptr);
    for (const std::unique_ptr<Model>& model : m_models) {
        SplatCloud* cloud = model->splats();
        if (cloud == nullptr) {
            continue;
        }
        // The ORDER buffer's slot: the desktop frames alternate theirs with the frame in flight;
        // VR Mode's two eyes — one VR frame in flight, waited for before the next records, and
        // the same far-first order for both (it is the view direction's alone) — share slot 0,
        // so the order is copied once a frame, not once an eye.
        const uint32_t orderSlot = splatOrderSlot(frameIndex, asyncSort);
        cloud->prepareFrame(camera, model->transform(), orderSlot, asyncSort);
        cloud->record(cmd, pipeline.layout(), orderSlot, camera, model->transform(), cut);
    }
}

int Scene::selectedModelIndex() const {
    return (m_selectedModel >= 0 && static_cast<std::size_t>(m_selectedModel) < m_models.size())
               ? m_selectedModel
               : -1;
}

void Scene::setLights(std::vector<SceneLight> lights) {
    m_lights = std::move(lights);
    ++m_lightsRevision;
    if (m_selectedLight >= static_cast<int>(m_lights.size())) {
        m_selectedLight = -1;
    }
}

int Scene::selectedLight() const {
    return (m_selectedLight >= 0 && static_cast<std::size_t>(m_selectedLight) < m_lights.size()) ? m_selectedLight : -1;
}

void Scene::setSelectedLight(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_lights.size()) {
        index = -1;
    }
    if (index >= 0) {
        setSelectedModel(-1); // a light and a model are never selected together
    }
    if (index != m_selectedLight) {
        m_selectedLight = index;
        ++m_lightsRevision; // (the Lights tab shows the selection)
    }
}

bool Scene::subjectBounds(glm::vec3& outMin, glm::vec3& outMax) const {
    const int figure = activeFigureIndex();
    // (Where she IS — Model::posedBounds — not her bind box where she was imported.)
    if (figure >= 0 && m_models[static_cast<std::size_t>(figure)]->posedBounds(outMin, outMax)) {
        return true;
    }
    return unionBounds(outMin, outMax);
}

bool Scene::unionBounds(glm::vec3& outMin, glm::vec3& outMax) const {
    bool any = false;
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (const std::unique_ptr<Model>& model : m_models) {
        glm::vec3 a;
        glm::vec3 b;
        if (model->posedBounds(a, b)) {
            outMin = glm::min(outMin, a);
            outMax = glm::max(outMax, b);
            any = true;
        }
    }
    return any;
}

void Scene::setSelectedModel(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_models.size()) {
        index = -1;
    }
    if (index >= 0 && m_selectedLight >= 0) {
        m_selectedLight = -1; // a light and a model are never selected together
        ++m_lightsRevision;
    }
    const int previous = selectedModelIndex();
    if (index == previous) {
        return;
    }
    // The outgoing selection's joint selection goes away — a joint
    // selection on a model that isn't the selection would contradict the outline.
    if (previous >= 0 && m_models[static_cast<std::size_t>(previous)]->hasSkeleton()) {
        m_models[static_cast<std::size_t>(previous)]->setSelectedBone(-1);
    }
    m_selectedModel = index;
    m_selectedZone = -1; // another object: the Materials tab shows its first zone until a click or a pick
    if (index >= 0 && m_models[static_cast<std::size_t>(index)]->hasSkeleton()) {
        m_activeFigure = index; // the selected figure is the posing target
    }
}

bool Scene::framingBounds(glm::vec3& outMin, glm::vec3& outMax) const {
    const int selected = selectedModelIndex();
    if (selected >= 0) {
        return m_models[static_cast<std::size_t>(selected)]->posedBounds(outMin, outMax);
    }
    return unionBounds(outMin, outMax);
}

void Scene::removeModel(std::size_t index) {
    if (index < m_models.size()) {
        ++m_modelsRevision;
        m_models.erase(m_models.begin() + static_cast<std::ptrdiff_t>(index));
        // Keep the active figure pointing at the same model (indices above shift down); the
        // deleted figure itself falls back to the first remaining one. The selection likewise
        // follows its model, and a deleted selection leaves nothing selected.
        if (m_activeFigure == static_cast<int>(index)) {
            m_activeFigure = -1;
        } else if (m_activeFigure > static_cast<int>(index)) {
            --m_activeFigure;
        }
        if (m_selectedModel == static_cast<int>(index)) {
            m_selectedModel = -1;
            m_selectedZone = -1;
        } else if (m_selectedModel > static_cast<int>(index)) {
            --m_selectedModel;
        }
    }
}

void Scene::clearModels() {
    ++m_modelsRevision;
    m_models.clear();     // destructors free buffers/descriptors, as the last removeModel would
    m_selectedZone = -1;
    m_selectedModel = -1; // the selection with them — DIRECTLY: setSelectedModel(-1) reads the
                          // selection through the bounds check, which an empty list already fails,
                          // and returned early with the old index kept; the next model imported
                          // into that slot then arrived selected (File → New / Open, 2026-11-07)
    m_activeFigure = -1;  // no posing target
}

std::size_t Scene::modelCount() const {
    return m_models.size();
}

Model* Scene::modelAt(std::size_t index) {
    return index < m_models.size() ? m_models[index].get() : nullptr;
}

const Model* Scene::modelAt(std::size_t index) const {
    return index < m_models.size() ? m_models[index].get() : nullptr;
}
void Scene::recordShadowPass(VkCommandBuffer cmd, uint32_t frameIndex) {
    // This frame's key direction, shared with record()'s UBO fill (computed once per frame here,
    // since this pass always runs first).
    m_keyLightDir = keyLightDirection(m_lighting, isPbr());
    if (m_shadowPass->fit(m_models, m_keyLightDir, m_lighting.shadowsEnabled)) {
        m_shadowPass->record(cmd, m_models, frameIndex);
    }
}

bool Scene::recordOutlinePass(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex,
                              const OutlineMask& mask, bool asyncSort) {
    const int selected = selectedModelIndex();
    if (selected < 0) {
        return false; // nothing selected: skip the pass; the composite draws no outline
    }
    // (A splat scene's order slot as recordSplats picks it: VR Mode's eyes share slot 0.)
    const uint32_t orderSlot = splatOrderSlot(frameIndex, asyncSort);
    return m_outlinePass->record(cmd, camera, frameIndex, mask, *m_models[static_cast<std::size_t>(selected)],
                                 m_pipelines->cameraSet(frameIndex), orderSlot, asyncSort);
}

void Scene::recordOverlays(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex) {
    writeCameraUbo(camera, frameIndex);
    const int overlayFigure = activeFigureIndex();
    m_lineOverlay->record(cmd, camera, m_pipelines->cameraSet(frameIndex), m_models,
                          overlayFigure >= 0 ? m_models[static_cast<std::size_t>(overlayFigure)].get() : nullptr, m_lights,
                          selectedLight(), frameIndex);
}

void Scene::record(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex) {
    const VulkanPipeline* wire = m_pipelines->wire();

    // The mode's row, with one substitution: on a device without fillModeNonSolid there is no
    // wire pipeline, and a row whose drawing IS the wires (Wireframe, Hidden Line) would leave
    // the figure invisible — draw its surface as Clay instead so the model stays on screen.
    ShadeMode spec = shadeModeSpec();
    if (spec.wireframe && wire == nullptr) {
        spec.wireframe = false;
        if (spec.fill != FillKind::Shaded) {
            spec.fill = FillKind::Shaded;
            spec.fragMode = kFragModeClay;
        }
    }

    // Update this frame's camera + lighting UBO, for the substituted row.
    writeCameraUbo(camera, frameIndex, spec);
    const VkDescriptorSet cameraSet = m_pipelines->cameraSet(frameIndex);
    const VkDescriptorSet iblSet = m_pipelines->iblSet();

    // HDRI backdrop first (PBR mode only — the stylized modes keep the flat viewport clear): the
    // environment that lights the figure, visible behind it. Depth test/write are off in its
    // pipeline, so everything after simply draws over it. Runs even with no models — an empty
    // PBR viewport still shows the environment. Backdrop mode 0 ("Off", an Environment-panel
    // dial) skips it, leaving the flat viewport grey.
    if (isPbr() && m_lighting.backdropMode != 0) {
        const VulkanPipeline& background = m_pipelines->background();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, background.handle());
        const VkDescriptorSet bgSets[2] = {cameraSet, iblSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, background.layout(), 0, 2,
                                bgSets, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    if (!m_models.empty()) {
        const VulkanPipeline& opaque = m_pipelines->opaque();
        // Every mesh pipeline shares one layout: the camera set (0) and the scene-global IBL/shadow
        // set (3) are bound once here and stay bound through the surface and wire passes below;
        // set 1 (material) is bound per mesh and set 2 (pose) per model.
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque.layout(), 0, 1,
                                &cameraSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque.layout(), 3, 1,
                                &iblSet, 0, nullptr);

        // --- The surface: the lit passes, a hidden-line depth fill, or nothing (see ShadeMode). ---
        if (spec.fill == FillKind::Shaded) {
            // Opaque pass first.
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaque.handle());
            for (const std::unique_ptr<Model>& model : m_models) {
                model->recordOpaque(cmd, opaque.layout(), frameIndex);
            }
            // ... then every model's see-through meshes, sorted together.
            recordTransparent(cmd, camera, frameIndex);
        } else if (spec.fill == FillKind::HiddenLine) {
            // Depth only (color writes masked): the surface hides what's behind it and shows the
            // viewport's clear color; the wire pass then draws only the edges facing the camera.
            const VulkanPipeline& hiddenLine = m_pipelines->hiddenLine();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hiddenLine.handle());
            for (const std::unique_ptr<Model>& model : m_models) {
                model->recordDepthFill(cmd, hiddenLine.layout(), frameIndex);
            }
        }

        // --- Wireframe: every mesh's triangle edges, over the surface or on their own. ---
        if (spec.wireframe && wire != nullptr) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wire->handle());
            for (const std::unique_ptr<Model>& model : m_models) {
                model->recordWire(cmd, wire->layout(), frameIndex);
            }
        }
    }

    // --- Line overlays (skeleton, pins, the orthographic floor line), models or not. ---
    // The skeleton follows the ACTIVE figure (the posing target), not figureModel(): Show
    // Skeleton is a view toggle, so selecting a plain model beside a figure must not blank the
    // figure's bones — only the posing utilities go quiet in that state (see figureModel()).
    const int overlayFigure = activeFigureIndex();
    m_lineOverlay->record(cmd, camera, cameraSet, m_models,
                          overlayFigure >= 0 ? m_models[static_cast<std::size_t>(overlayFigure)].get()
                                             : nullptr,
                          m_lights, selectedLight(), frameIndex);
}

void Scene::recordTransparent(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex) {
    // Transparent pass: alpha-blended, depth-write off, drawn after all opaque geometry so
    // it blends over what's behind it (e.g. the eye's moisture/cornea over the iris).
    // EVERY model's transparent meshes are sorted back-to-front TOGETHER, by their posed
    // centroids (Model::posedCentroid) — a per-model sort in import order drew a
    // translucent object imported first before a figure's eye behind it, and a bind-pose
    // key mis-ordered the shells of a figure lying on its back.
    const glm::vec3 camPos = camera.position();
    m_transparentScratch.clear();
    for (const std::unique_ptr<Model>& model : m_models) {
        for (const Mesh* mesh : model->transparentMeshes()) {
            const glm::vec3 d = model->posedCentroid(*mesh) - camPos;
            m_transparentScratch.push_back({glm::dot(d, d), model.get(), mesh});
        }
    }
    std::sort(m_transparentScratch.begin(), m_transparentScratch.end(),
              [](const TransparentDraw& a, const TransparentDraw& b) {
                  return a.distSq > b.distSq; // farthest first
              });
    const VulkanPipeline& transparent = m_pipelines->transparent();
    const VulkanPipeline& transmitMultiply = m_pipelines->transmitMultiply();
    const VulkanPipeline& transmitAdd = m_pipelines->transmitAdd();
    const VkPipelineLayout layout = transparent.layout(); // (every mesh variant shares it)
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    const auto bindPipeline = [&](const VulkanPipeline& pipeline) {
        if (pipeline.handle() != boundPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
            boundPipeline = pipeline.handle();
        }
    };
    Model* bound = nullptr; // whose pose set (2) is currently bound
    for (const TransparentDraw& draw : m_transparentScratch) {
        if (draw.model != bound) {
            draw.model->bindPose(cmd, layout, 2, frameIndex);
            bound = draw.model;
        }
        if (draw.mesh->isTransmissive()) {
            // TRANSMISSION: two draws — the frame behind the zone multiplied by what it
            // lets through (a per-channel tint), then its own reflection and what is
            // left of its diffuse added (ScenePipelines::transmitMultiply / transmitAdd).
            bindPipeline(transmitMultiply);
            draw.mesh->record(cmd, layout, draw.model->transform(), MeshDrawKind::TransmitMultiply,
                              draw.model->highlightBone(), draw.model->selectedHighlightTwin());
            bindPipeline(transmitAdd);
            draw.mesh->record(cmd, layout, draw.model->transform(), MeshDrawKind::TransmitAdd,
                              draw.model->highlightBone(), draw.model->selectedHighlightTwin());
            continue;
        }
        bindPipeline(transparent);
        draw.mesh->record(cmd, layout, draw.model->transform(), MeshDrawKind::Transparent,
                          draw.model->highlightBone(), draw.model->selectedHighlightTwin());
    }
}

const Model* Scene::figureModel() const {
    const int active = activeFigureIndex();
    if (active < 0) {
        return nullptr;
    }
    const int selected = selectedModelIndex();
    if (selected >= 0 && selected != active) {
        return nullptr; // a different model is the selection: nothing to pose
    }
    return m_models[static_cast<std::size_t>(active)].get();
}

Model* Scene::figureModel() {
    return const_cast<Model*>(static_cast<const Scene*>(this)->figureModel());
}

int Scene::activeFigureIndex() const {
    if (m_activeFigure >= 0 && static_cast<std::size_t>(m_activeFigure) < m_models.size() &&
        m_models[static_cast<std::size_t>(m_activeFigure)]->hasSkeleton()) {
        return m_activeFigure;
    }
    for (std::size_t i = 0; i < m_models.size(); ++i) {
        if (m_models[i]->hasSkeleton()) {
            return static_cast<int>(i); // the first figure until one is clicked
        }
    }
    return -1;
}

void Scene::setActiveFigure(int index) {
    if (index >= 0 && static_cast<std::size_t>(index) < m_models.size() &&
        m_models[static_cast<std::size_t>(index)]->hasSkeleton()) {
        m_activeFigure = index;
        setSelectedModel(index); // the posing target is the selection (and the outline)
    }
}

bool Scene::hasPosableFigure() const { return figureModel() != nullptr; }

bool Scene::hasSelectedBone() const {
    const Model* fig = figureModel();
    return fig != nullptr && fig->selectedBone() >= 0;
}

int Scene::selectBoneByName(const std::string& name) {
    Model* fig = figureModel();
    return fig ? fig->selectBoneByName(name) : -1;
}

} // namespace pose
