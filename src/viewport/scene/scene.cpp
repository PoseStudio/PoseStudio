/**
 * @file scene.cpp
 * @brief Scene construction/teardown, the environment upload, model management + selection, the
 *        per-frame record sequence, and the active-figure logic. The posing forwarders live in
 *        sceneposing.cpp. See scene.h.
 */

#include "scene.h"

#include "ikmath.h"

#include "bonepicker.h"
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace pose {

Scene::Scene(VulkanContext& context, VkRenderPass renderPass, VkRenderPass outlineMaskPass,
             const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv,
             const std::vector<char>& skeletonVertSpirv, const std::vector<char>& skeletonFragSpirv,
             const std::vector<char>& shadowVertSpirv, const std::vector<char>& shadowFragSpirv,
             const std::vector<char>& backgroundVertSpirv,
             const std::vector<char>& backgroundFragSpirv,
             const std::vector<char>& outlineMaskFragSpirv,
             const std::vector<char>& splatVertSpirv, const std::vector<char>& splatFragSpirv)
    : m_context(context) {
    // The shared plumbing first: every pass below builds against its layouts.
    m_pipelines = std::make_unique<ScenePipelines>(m_context, renderPass, vertSpirv, fragSpirv,
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
                                                  outlineMaskFragSpirv);
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
    m_environmentSH = baked.sh;
    // The BRDF LUT is environment-independent (roughness + NdotV only), so integrate it once and reuse
    // — re-running the ~8M-sample integration on every HDRI swap would be pure waste.
    if (m_brdfLut.data.empty()) {
        m_brdfLut = integrateBrdfLut();
    }
    m_iblMaps = std::make_unique<IblMaps>(m_context, baked.specular, m_brdfLut);
    m_pipelines->writeEnvironment(*m_iblMaps);
}

VkDescriptorSetLayout Scene::iblSetLayout() const { return m_pipelines->iblSetLayout(); }

VkDescriptorSet Scene::iblSet() const { return m_pipelines->iblSet(); }

const glm::mat4& Scene::lightViewProj() const { return m_shadowPass->lightViewProj(); }

void Scene::setShowSkeleton(bool on) { m_lineOverlay->setShowSkeleton(on); }

bool Scene::showSkeleton() const { return m_lineOverlay->showSkeleton(); }

void Scene::setVrPointerLines(std::vector<PointerLine> lines) { m_lineOverlay->setPointerLines(std::move(lines)); }

void Scene::addModel(const ModelData& data) {
    m_models.push_back(std::make_unique<Model>(m_context, data, m_pipelines->materialSetLayout(),
                                               m_pipelines->poseSetLayout(),
                                               m_pipelines->fallbackDiffuse(),
                                               m_pipelines->fallbackNormal()));
    setSelectedModel(static_cast<int>(m_models.size()) - 1); // the new arrival is the selection
}

void Scene::addSplatModel(const SplatData& data, const glm::mat4& transform) {
    m_models.push_back(std::make_unique<Model>(m_context, data, m_pipelines->splatSetLayout()));
    m_models.back()->setTransform(transform);
    setSelectedModel(static_cast<int>(m_models.size()) - 1);
}

void Scene::recordSplats(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex) {
    bool any = false;
    for (const std::unique_ptr<Model>& model : m_models) {
        if (model->splats() != nullptr) {
            any = true;
            break;
        }
    }
    if (!any) {
        return;
    }
    const VulkanPipeline& pipeline = m_pipelines->splat();
    const VkDescriptorSet cameraSet = m_pipelines->cameraSet(frameIndex);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1, &cameraSet, 0, nullptr);
    // The composite will ACES-tonemap the HDR target in the PBR mode (with the dial on): the
    // splats' display-referred colours go in through the inverse curve and come out as trained.
    const bool inverseTonemap = isPbr() && m_lighting.tonemap;
    for (const std::unique_ptr<Model>& model : m_models) {
        SplatCloud* cloud = model->splats();
        if (cloud == nullptr) {
            continue;
        }
        cloud->prepareFrame(camera, model->transform(), frameIndex);
        cloud->record(cmd, pipeline.layout(), frameIndex, camera, model->transform(), inverseTonemap);
    }
}

int Scene::selectedModelIndex() const {
    return (m_selectedModel >= 0 && static_cast<std::size_t>(m_selectedModel) < m_models.size())
               ? m_selectedModel
               : -1;
}

void Scene::setSelectedModel(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_models.size()) {
        index = -1;
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
    if (index >= 0 && m_models[static_cast<std::size_t>(index)]->hasSkeleton()) {
        m_activeFigure = index; // the selected figure is the posing target
    }
}

int Scene::pickModel(const Ray& ray) const {
    int best = -1;
    float bestT = std::numeric_limits<float>::max();
    for (std::size_t i = 0; i < m_models.size(); ++i) {
        float t = 0.0f;
        if (m_models[i]->intersectRay(ray, t) && t < bestT) {
            bestT = t;
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool Scene::framingBounds(glm::vec3& outMin, glm::vec3& outMax) const {
    const int selected = selectedModelIndex();
    if (selected >= 0) {
        return m_models[static_cast<std::size_t>(selected)]->worldBounds(outMin, outMax);
    }
    bool any = false;
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (const std::unique_ptr<Model>& model : m_models) {
        glm::vec3 a;
        glm::vec3 b;
        if (model->worldBounds(a, b)) {
            outMin = glm::min(outMin, a);
            outMax = glm::max(outMax, b);
            any = true;
        }
    }
    return any;
}

void Scene::removeModel(std::size_t index) {
    if (index < m_models.size()) {
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
        } else if (m_selectedModel > static_cast<int>(index)) {
            --m_selectedModel;
        }
    }
}

void Scene::clearModels() {
    m_models.clear(); // destructors free buffers/descriptors, as the last removeModel would
    setSelectedModel(-1); // clear the selection (and its joint selection)
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
                              const OutlineMask& mask) {
    const int selected = selectedModelIndex();
    if (selected < 0) {
        return false; // nothing selected: skip the pass; the composite draws no outline
    }
    return m_outlinePass->record(cmd, camera, frameIndex, mask,
                                 *m_models[static_cast<std::size_t>(selected)]);
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

    // Update this frame's camera + lighting UBO (see fillCameraUbo for what feeds which mode).
    CameraUbo ubo{};
    fillCameraUbo(ubo, camera, m_lighting, spec, m_environmentSH, m_shadowPass->lightViewProj(),
                  m_keyLightDir);
    std::memcpy(m_pipelines->cameraUboData(frameIndex), &ubo, sizeof(ubo));
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
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, transparent.handle());
            const VkPipelineLayout layout = transparent.layout();
            Model* bound = nullptr; // whose pose set (2) is currently bound
            for (const TransparentDraw& draw : m_transparentScratch) {
                if (draw.model != bound) {
                    draw.model->bindPose(cmd, layout, 2, frameIndex);
                    bound = draw.model;
                }
                draw.mesh->record(cmd, layout, draw.model->transform(), MeshDrawKind::Transparent,
                                  draw.model->highlightBone(), draw.model->selectedHighlightTwin());
            }
        } else if (spec.fill == FillKind::HiddenLine) {
            // Depth only (colour writes masked): the surface hides what's behind it and shows the
            // viewport's clear colour; the wire pass then draws only the edges facing the camera.
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
                          frameIndex);
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

bool Scene::pickBoneAt(float px, float py, float vpW, float vpH, const Camera& camera, int& outModel, int& outBone,
                       glm::vec3& outGrab) {
    // A CLICK PICKS WHAT THE CURSOR IS ON (2026-09-26): the click is cast against every figure's
    // posed SKIN (Model::pickSurface) and the FIRST surface along the ray wins — its bone the joint
    // that weighs most on the skin at the hit, its point the grab point (Armature::setIkGrabPoint).
    // Until then the pick was a 2D one alone — every joint and bone body projected to the screen,
    // the nearest within 32px taken (pickBone) — and it saw THROUGH the body: a click on the chest
    // found the shoulder or the arm behind it as often as the chest (the user: "no body part
    // should ever get selected by clicking through another body part"). A joint of the hit bone's
    // OWN rigid segment (Armature::segmentJoints) that projects within kJointSnapPx of the click
    // is still snapped to — a click on the knee grabs the knee, not a point of the shin's skin
    // beside it — and cannot be hidden, being the segment the cursor is on. The 2D pick remains
    // for a click that MISSES the skin (just outside the silhouette), and only when nothing stands
    // between the camera and the point it picked (a second cast toward that point; a bone's own
    // flesh, kFallbackFleshDepth deep, is not "something").
    // Every figure competes: the nearest surface (or, in the fallback, the nearest joint) of ANY
    // figure wins, and its figure becomes the active one (the posing target). On a miss return -1
    // (so the caller orbits / click-selects) but keep the current selection — the figure can be
    // orbited while a joint is selected.
    constexpr float kJointSnapPx = 14.0f;          // (pickBone's: right on a joint origin)
    constexpr float kFallbackFleshDepth = 0.10f;   // metres: a picked joint sits inside its own flesh
    const Ray ray = camera.screenPointToRay(px, py, vpW, vpH);
    const glm::mat4 viewProj = camera.viewProjection();
    const auto screenOf = [&](const glm::vec3& world, glm::vec2& out) {
        const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
        if (clip.w <= 1.0e-4f) {
            return false;
        }
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        out = glm::vec2((ndc.x * 0.5f + 0.5f) * vpW, (ndc.y * 0.5f + 0.5f) * vpH);
        return true;
    };
    int       pickedModel = -1;
    int       pickedBone = -1;
    glm::vec3 grab(0.0f);
    static const bool kPick2DEnv = std::getenv("POSESTUDIO_PICK_2D") != nullptr; // A/B probe: the 2D pick alone, as before 2026-09-26
    const bool kPick2D = kPick2DEnv || m_pick2D;
    {
        float hitT = std::numeric_limits<float>::max();
        for (std::size_t m = 0; m < m_models.size() && !kPick2D; ++m) {
            const Model* fig = m_models[m].get();
            float     t = 0.0f;
            glm::vec3 point(0.0f);
            int       bone = -1;
            if (fig->hasSkeleton() && fig->pickSurface(ray, t, point, bone) && t < hitT) {
                hitT = t;
                pickedModel = static_cast<int>(m);
                pickedBone = bone;
                grab = point;
            }
        }
    }
    // WHAT the hit bone is to the posing UI (Armature::boneClass, 2026-09-28). A bone of the FACE RIG
    // is for expressions and is not posed: the click is on the HEAD (the selection below maps it),
    // at the point it hit. A finger's or a toe's joint is dragged by the point the cursor is ON —
    // its drag moves the digit alone, and a point AT a joint has no lever on the bone it belongs
    // to. Neither snaps to a joint, and no joint of either is a snap candidate for a hit beside it
    // (a click on the back of the hand is the hand's, not the knuckle's 10px away). A CARPAL — the
    // hand's or the foot's own flesh below the wrist or the ankle (2026-10-28) — is the HAND's or
    // the FOOT's click at the point it hit, like the face's the head's: it never snaps and is never
    // a snap candidate; the selection below maps it.
    const auto classOf = [&](int model, int bone) {
        return m_models[static_cast<std::size_t>(model)]->boneClass(bone);
    };
    if (pickedBone >= 0 && classOf(pickedModel, pickedBone) == BoneClass::Body) {
        // The joint snap: a joint of the hit bone's OWN body that projects within kJointSnapPx of
        // the click — the near and far joints of its rigid segment (or its own joint, for a leaf),
        // the joints its CHILDREN hang from (where its flesh ends) and its PARENT's joint. The
        // parent matters for the root: it has no skin of its own (the pelvic flesh is the pelvis
        // bone's), so a click right on the hip's pixel lands on the pelvis bone, and with the hit
        // bone's own joint the only candidate it took the pelvis joint 10px away over the hip at
        // 0px — the drag's first tick then asked the hips 2cm back, to put the pelvis joint where
        // the hip's pixel is, which on all fours popped a foot 165mm (2026-09-26). The CHILDREN's
        // joints must lie no deeper along the ray than the hit bone's own joint (plus
        // kSnapDepthSlack): a trunk bone's children come in lateral PAIRS — the two collars, the
        // two thigh sockets — and from the side the far twin projects onto the near one's pixel.
        // The parent's joint is not depth-checked: it is where the hit bone hangs from, inside the
        // body the cursor is on however deep — the chest joint sits on the spine, 10cm behind the
        // breast's skin, and a click at the chest joint's pixel that lands on a pectoral must
        // still take the chest (depth-checked, it kept the pectoral, and the Home view's chest
        // drag changed) — and the hit bone's own joints are trusted as they are, being the
        // segment the cursor is on. The nearest to the click wins; two at the same pixel go to
        // the nearer to the camera. POSESTUDIO_PICK_TRACE prints every candidate.
        static const bool kSnapTrace = std::getenv("POSESTUDIO_PICK_TRACE") != nullptr;
        constexpr float kSnapDepthSlack = 0.03f; // metres
        const Model&    fig = *m_models[static_cast<std::size_t>(pickedModel)];
        const Armature& arm = fig.armature();
        const int       hitBone = pickedBone;
        struct Candidate {
            int  joint;
            bool depthChecked;
        };
        std::vector<Candidate> candidates;
        const auto             offer = [&](int joint, bool depthChecked) { // (once each: a pectoral's segment near
            for (const Candidate& c : candidates) {                          // joint and its parent are both the chest)
                if (c.joint == joint) {
                    return;
                }
            }
            candidates.push_back({joint, depthChecked});
        };
        int nearJoint = -1;
        int farJoint = -1;
        if (arm.segmentJoints(hitBone, nearJoint, farJoint)) {
            offer(nearJoint, false);
            offer(farJoint, false);
        } else {
            offer(hitBone, false);
        }
        if (const int parent = arm.boneParent(static_cast<std::size_t>(hitBone)); parent >= 0) {
            offer(parent, false);
        }
        for (std::size_t c = 0; c < arm.boneCount(); ++c) {
            if (arm.boneParent(c) == hitBone) {
                offer(static_cast<int>(c), true);
            }
        }
        const auto depthOf = [&](int j) { return glm::dot(fig.boneWorldPosition(static_cast<std::size_t>(j)) - ray.origin, ray.direction); };
        const float     ownDepth = depthOf(hitBone);
        const glm::vec2 click(px, py);
        float           bestPx = kJointSnapPx;
        float           bestDepth = std::numeric_limits<float>::max();
        bool            snapped = false;
        for (const Candidate& c : candidates) {
            glm::vec2 at(0.0f);
            if (c.joint < 0 || static_cast<std::size_t>(c.joint) >= fig.boneCount() ||
                classOf(pickedModel, c.joint) != BoneClass::Body || // (a face-rig bone's or a digit's joint: see above)
                !screenOf(fig.boneWorldPosition(static_cast<std::size_t>(c.joint)), at)) {
                continue;
            }
            const float dPx = glm::length(at - click);
            if (dPx > kJointSnapPx) {
                continue;
            }
            const float depth = depthOf(c.joint);
            const bool  tooDeep = c.depthChecked && depth > ownDepth + kSnapDepthSlack; // the far twin, behind the body the cursor is on
            const bool  nearer = !tooDeep && (!snapped || dPx < bestPx - 0.5f || (dPx <= bestPx + 0.5f && depth < bestDepth));
            if (kSnapTrace) {
                std::fprintf(stderr, "[pick] hit %s: snap candidate %s (%d): %.1f px, depth %+.3f m against the hit bone's joint%s%s\n",
                             arm.boneName(static_cast<std::size_t>(hitBone)).c_str(), arm.boneName(static_cast<std::size_t>(c.joint)).c_str(),
                             c.joint, dPx, depth - ownDepth,
                             tooDeep ? " (too deep)" : "", nearer ? " -> taken" : "");
            }
            if (nearer) {
                snapped = true;
                bestPx = dPx;
                bestDepth = depth;
                pickedBone = c.joint;
                grab = fig.boneWorldPosition(static_cast<std::size_t>(c.joint));
            }
        }
    } else if (pickedBone < 0) {
        const BonePick pick = pickBone(m_models, px, py, vpW, vpH, camera);
        if (pick.bone < 0) {
            return false;
        }
        const Model& fig = *m_models[static_cast<std::size_t>(pick.model)];
        // The picked point: the joint itself for a pick of the joint, the clicked point of the
        // body otherwise (the point of the picked segment nearest the cursor's ray; the pick's own
        // parameter is a screen-space one, good enough when the ray runs along the bone).
        grab = fig.boneWorldPosition(static_cast<std::size_t>(pick.bone));
        if (pick.child >= 0 && pick.child < static_cast<int>(fig.boneCount())) {
            const glm::vec3& far = fig.boneWorldPosition(static_cast<std::size_t>(pick.child));
            float onRay = 0.0f;
            float onBone = pick.along;
            const glm::vec3 reach = ray.origin + ray.direction * (2.0f * glm::length(far - ray.origin) + 1.0f);
            if (glm::length(far - grab) > 1.0e-5f) {
                closestSegmentPoints(ray.origin, reach, grab, far, onRay, onBone);
            }
            grab = glm::mix(grab, far, glm::clamp(onBone, 0.0f, 1.0f));
        }
        // ... and nothing may stand between the camera and it: cast toward the point through every
        // figure, and a surface met more than the point's own flesh short of it hides it.
        const glm::vec3 toPoint = grab - ray.origin;
        const float     dist = glm::length(toPoint);
        if (dist > 1.0e-4f && !kPick2D) {
            const Ray toward{ray.origin, toPoint / dist};
            for (const std::unique_ptr<Model>& other : m_models) {
                float     t = 0.0f;
                glm::vec3 point(0.0f);
                int       bone = -1;
                if (other->hasSkeleton() && other->pickSurface(toward, t, point, bone) && t < dist - kFallbackFleshDepth) {
                    return false; // hidden behind another part: not what the cursor is on
                }
            }
        }
        pickedModel = pick.model;
        pickedBone = pick.bone;
    }
    outModel = pickedModel;
    outBone = pickedBone;
    outGrab = grab;
    return true;
}

int Scene::selectBoneAt(float px, float py, float vpW, float vpH, const Camera& camera) {
    int       pickedModel = -1;
    int       pickedBone = -1;
    glm::vec3 grab(0.0f);
    if (!pickBoneAt(px, py, vpW, vpH, camera, pickedModel, pickedBone, grab)) {
        return -1;
    }
    if (pickedModel != activeFigureIndex()) {
        // Switching figures: the previous one's selection goes away.
        if (Model* previous = figureModel()) {
            previous->setSelectedBone(-1);
        }
    }
    setActiveFigure(pickedModel);
    Model& figure = *m_models[static_cast<std::size_t>(pickedModel)];
    figure.setSelectedBone(pickedBone); // (a bone of the face rig selects the HEAD: Armature::posingBone)
    figure.setIkGrabPoint(pickedBone, grab);
    return figure.selectedBone();
}

int Scene::selectNearestBone(const glm::vec3& point, float radius) {
    int   pickedModel = -1;
    int   pickedBone = -1;
    float best = radius;
    for (std::size_t m = 0; m < m_models.size(); ++m) {
        const Model* fig = m_models[m].get();
        if (!fig->hasSkeleton()) {
            continue;
        }
        const Armature& arm = fig->armature();
        for (std::size_t i = 0; i < fig->boneCount(); ++i) {
            const float distance = glm::length(arm.boneWorldPosition(i) - point);
            if (distance < best) {
                best = distance;
                pickedModel = static_cast<int>(m);
                pickedBone = static_cast<int>(i);
            }
        }
    }
    if (pickedBone < 0) {
        return -1;
    }
    if (pickedModel != activeFigureIndex()) {
        if (Model* previous = figureModel()) {
            previous->setSelectedBone(-1);
        }
    }
    setActiveFigure(pickedModel);
    Model& figure = *m_models[static_cast<std::size_t>(pickedModel)];
    figure.setSelectedBone(pickedBone); // (a face-rig bone selects the head, as for a click)
    figure.setIkGrabPoint(figure.selectedBone(), figure.armature().boneWorldPosition(static_cast<std::size_t>(pickedBone)));
    return figure.selectedBone();
}

int Scene::shiftPressBoneAt(float px, float py, float vpW, float vpH, const Camera& camera, bool& wasMember) {
    wasMember = false;
    int       pickedModel = -1;
    int       pickedBone = -1;
    glm::vec3 grab(0.0f);
    if (!pickBoneAt(px, py, vpW, vpH, camera, pickedModel, pickedBone, grab)) {
        return -1;
    }
    Model& figure = *m_models[static_cast<std::size_t>(pickedModel)];
    // Another figure than the active one, a digit (posed alone), or nothing selected yet: a plain
    // selection, as without Shift.
    if (pickedModel != activeFigureIndex() || figure.selectedBone() < 0 ||
        figure.boneClass(pickedBone) == BoneClass::Digit) {
        return selectBoneAt(px, py, vpW, vpH, camera);
    }
    const int primary = figure.armature().knownPosingBone(pickedBone); // (a face-rig bone: the head)
    wasMember = primary == figure.selectedBone() || figure.inGroup(primary);
    static const bool kShiftTrace = std::getenv("POSESTUDIO_PICK_TRACE") != nullptr;
    if (kShiftTrace) {
        std::fprintf(stderr, "[pick] shift press: picked %s (model %d), selected before %d, group %zu, member %d\n",
                     figure.armature().boneName(static_cast<std::size_t>(pickedBone)).c_str(), pickedModel, figure.selectedBone(),
                     figure.groupBones().size(), wasMember ? 1 : 0);
    }
    if (!figure.makeGroupPrimary(pickedBone)) {
        return selectBoneAt(px, py, vpW, vpH, camera);
    }
    figure.setIkGrabPoint(pickedBone, grab);
    return figure.selectedBone();
}

bool Scene::removeGroupBone(int bone) {
    Model* fig = figureModel();
    return fig != nullptr && fig->removeFromGroup(bone);
}

std::vector<int> Scene::groupBones() const {
    const Model* fig = figureModel();
    return fig != nullptr ? fig->groupBones() : std::vector<int>();
}

void Scene::restoreSelection(int primary, const std::vector<int>& secondaries) {
    if (Model* fig = figureModel()) {
        fig->restoreSelection(primary, secondaries);
    }
}

} // namespace pose
