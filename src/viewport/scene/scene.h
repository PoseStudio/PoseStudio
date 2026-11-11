/**
 * @file scene.h
 * @brief Owns the scene's shared GPU plumbing (ScenePipelines), its passes (shadow, outline,
 *        line overlay), the lighting environment, and the list of imported models. The mesh
 *        analogue of Grid.
 *
 * Kept self-contained so VulkanRenderer doesn't accumulate per-feature pipeline/descriptor code:
 * the renderer just constructs a Scene, forwards imports to addModel(), and calls the three
 * record entry points in order each frame — recordShadowPass() and recordOutlinePass() before
 * the main render pass, record() inside it (before the transparent grid). The posing API is a
 * set of thin forwarders to the ACTIVE figure (sceneposing.cpp); the heavy lifting lives in
 * Model/Armature. Qt-free (Vulkan + std + GLM).
 */

#ifndef SCENE_H
#define SCENE_H

#include "environment.h"
#include "lightingsettings.h"
#include "shademode.h"
#include "armature.h" // IkScope
#include "jointtransform.h"
#include "lineoverlay.h" // PointerLine (setVrPointerLines)
#include "materialparams.h" // MaterialView / MaterialParams (the Materials tab)

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pose {

class Camera;
class IblMaps;
class LineOverlay;
class Mesh;
class Armature;
class Model;
class OutlineMask;
class OutlinePass;
class ScenePipelines;
class ShadowPass;
class VulkanContext;
struct DecodedImage;
struct ModelData;
struct Ray;
struct SplatData;

/**
 * @class Scene
 * @brief The collection of imported meshes plus the machinery to draw them lit.
 */
class Scene {
public:
    /// @param outlineMaskPass The selection-outline mask's render pass (OutlineMask; the renderer
    ///                        owns the screen-sized target) — the silhouette pipeline builds
    ///                        against it, reusing shadow.vert with @p outlineMaskFragSpirv.
    /// @param photoPass       The HDR target's PHOTO pass (hdrtarget.h), single-sample — the
    ///                        Gaussian splat pipeline builds against it; recordSplats runs in it.
    Scene(VulkanContext& context, VkRenderPass renderPass, VkRenderPass outlineMaskPass, VkRenderPass photoPass,
          const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv,
          const std::vector<char>& skeletonVertSpirv, const std::vector<char>& skeletonFragSpirv,
          const std::vector<char>& shadowVertSpirv, const std::vector<char>& shadowFragSpirv,
          const std::vector<char>& backgroundVertSpirv,
          const std::vector<char>& backgroundFragSpirv,
          const std::vector<char>& outlineMaskFragSpirv,
          const std::vector<char>& splatVertSpirv, const std::vector<char>& splatFragSpirv,
          const std::vector<char>& splatMaskFragSpirv);
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    /// Uploads CPU geometry to the GPU and adds it to the scene, and SELECTS it (a freshly
    /// imported object is the one the user is about to work with, and the outline shows the
    /// import landed). Call from the GUI thread between frames (the upload blocks briefly on its
    /// own one-time submit; it touches no in-flight state).
    void addModel(const ModelData& data);
    /// Adds a Gaussian splat scene (splatcloud.h) as a static model at @p transform (the import
    /// service turns the capture's y-down frame over); it becomes the selection.
    void addSplatModel(const SplatData& data, const glm::mat4& transform);

    // --- Object selection (the outlined model) ---
    /// The selected model's index, or -1 when nothing is selected. The selection is what the
    /// viewport outlines; clicking a figure's joint selects that figure (setActiveFigure), a
    /// plain click on a model's box selects it, a click on empty space clears it.
    int selectedModelIndex() const;
    /// Selects model @p index (-1 or out of range = clear). Selecting a figure also makes it the
    /// active (posing-target) figure; the previously selected figure's joint selection is
    /// cleared, so nothing posing-related lingers on an unselected model.
    void setSelectedModel(int index);

    /// Uploads a CPU-baked environment (SH irradiance + prefiltered specular; see bakeEnvironment) and
    /// points the IBL descriptor set at it. Waits for the GPU to idle first, since it swaps the maps the
    /// mesh pipeline samples. The heavy CPU bake is done by the caller (off the render thread for
    /// interactive HDRI switches); this does only the fast GPU work.
    void applyBakedEnvironment(const BakedEnvironment& baked);

    /// Live lighting/exposure controls (the Environment panel drives these). All shader-side, so this
    /// is just a cached value read into the next frame's UBO — no re-bake.
    void                   setLightingSettings(const LightingSettings& settings) { m_lighting = settings; }
    const LightingSettings& lightingSettings() const { return m_lighting; }

    /// Returns the index of the nearest model whose bounding box @p ray hits, or -1 if none.
    int pickModel(const Ray& ray) const;

    /// The world AABB to frame for "frame selected": the selected model's, else the union of
    /// every model's (a posed figure's bounds follow its joints — see Model::worldBounds).
    /// Returns false with nothing to frame.
    bool framingBounds(glm::vec3& outMin, glm::vec3& outMax) const;

    /// Removes the model at @p index (no-op if out of range). The caller MUST have ensured the GPU
    /// is idle first (the model's buffers/descriptors may be referenced by in-flight frames) —
    /// VulkanRenderer::deleteModel() does this.
    void removeModel(std::size_t index);

    /// Removes EVERY model, resetting the selection and the active figure — File → New and Open
    /// start from this (the renderer's own delete pass runs first, as for removeModel).
    void clearModels();

    /// The number of imported models, in scene order (the Nth import is index N-1).
    std::size_t modelCount() const;

    /// Model @p index in scene order — the .pss document addresses its figures by this position
    /// (out of range: nullptr).
    Model*       modelAt(std::size_t index);
    const Model* modelAt(std::size_t index) const;

    /// Records the key light's depth-only shadow pass (its own render pass on the shadow map).
    /// Call BEFORE the main render pass each frame — it is also where this frame's key-light
    /// direction is computed, and it (re)fits the light's ortho frustum around the scene + its
    /// floor projections, which record() then feeds to the shaders via the UBO. Skipped while
    /// shadows are off or the scene has no casters: the map then keeps its previous contents,
    /// harmless because the parked (degenerate) light matrix makes every lookup read lit
    /// without consulting it (see ShadowPass::fit).
    void recordShadowPass(VkCommandBuffer cmd, uint32_t frameIndex);

    /// Records the selection-outline mask pass — the selected model's silhouette, skinned and
    /// camera-projected, into @p mask (its own render pass, begun and ended here). Call before
    /// the main render pass, like recordShadowPass. Returns false, recording nothing, when no
    /// model is selected (the composite then draws no outline). A selected splat scene's
    /// silhouette is its quads (outlinepass.h): @p asyncSort as recordSplats takes it.
    bool recordOutlinePass(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex,
                           const OutlineMask& mask, bool asyncSort = false);

    /// Updates this frame's camera UBO, then records in order: the HDRI backdrop (PBR mode),
    /// the surface passes (opaque, then every model's transparent meshes sorted back-to-front
    /// together, or the hidden-line depth fill), the wireframe overlay, and the line overlay
    /// (skeleton / pins / the orthographic floor line — always, even with no models). The caller
    /// has begun the render pass and set the dynamic viewport/scissor.
    void record(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex);
    /// The GAUSSIAN SPLAT pass (splatcloud.h): every splat model's cloud, sorted far-first for
    /// this view and alpha-blended, depth-tested against the opaque geometry. Recorded by the
    /// renderer AFTER the grid, so the floor's lines lie under a capture's floor instead of
    /// drawing across its legs (a splat writes no depth). No-op without a splat model.
    void recordSplats(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex, bool asyncSort = false);
    /// The ORDER buffer's slot of a splat scene for this frame: the desktop frames alternate theirs
    /// with the frame in flight; VR Mode's two eyes (one VR frame in flight, waited for before the
    /// next records, the same far-first order for both) share slot 0, so the order is copied once a
    /// frame. recordSplats and recordOutlinePass must agree on it: one definition.
    static uint32_t splatOrderSlot(uint32_t frameIndex, bool asyncSort) { return asyncSort ? 0u : frameIndex; }
    /// Whether any model is a Gaussian splat cloud (the renderer records the photo pass only then).
    bool hasSplats() const;
    /// Whether the selected model is a Gaussian splat cloud (the composite then reads the outline
    /// mask as accumulated opacity — see outlinepass.h).
    bool selectedIsSplat() const;
    /// Whether any model has meshes — the opaque pass drew something, so the SSS blur and the
    /// specular it adds back have work (a scene of splats alone, or empty, skips them).
    bool hasMeshes() const;

    // The grid's ground shadow samples the scene's shadow map through the scene-wide set 3 (the
    // IBL set — binding 2 is the shadow map). Grid's pipeline is built against this layout and
    // binds this set at record time, with the matching light matrix from lightViewProj().
    VkDescriptorSetLayout iblSetLayout() const;
    VkDescriptorSet       iblSet() const;
    const glm::mat4&      lightViewProj() const;

    // --- Posing UI ---
    /// Whether there is a figure to pose: an ACTIVE figure that is also the selection (or
    /// nothing else is selected) — see figureModel().
    bool hasPosableFigure() const;
    /// The ACTIVE figure — the one every posing call (selection, IK, FK, pins, pose
    /// snapshot, ground) addresses: the figure whose joint was last clicked (selectBoneAt
    /// searches every figure's joints), else the first figure in the scene. -1 without one.
    int  activeFigureIndex() const;
    /// Makes model @p index the active figure (no-op unless it is a figure) — and the SELECTED
    /// model, so the outline follows the posing target. Undo/redo restores the figure a pose
    /// snapshot was taken from through this before applying it.
    void setActiveFigure(int index);
    /// Show/hide the skeleton overlay (drawn over the figure so the joints — always grabbable,
    /// drawn or not — can be seen).
    void setShowSkeleton(bool on);
    bool showSkeleton() const;
    /// The VR controllers' pointer rays, drawn by the line overlay from now on (VR Mode sets
    /// them every frame; empty = none, the state outside VR Mode).
    void setVrPointerLines(std::vector<PointerLine> lines);

    // --- Shading ---
    /// Selects the viewport shade mode: an index into the picker's table (scene/shademode.h),
    /// whose row says which mesh.frag mode shades the surface (written into the per-frame camera
    /// UBO — no pipeline swap for that) and whether record() draws the surface at all, a
    /// hidden-line depth fill, and/or a wireframe overlay. Clamped into the table's range.
    void setShadeMode(int mode) {
        m_shadeMode = mode < 0 ? 0 : (mode >= kShadeModeCount ? kShadeModeCount - 1 : mode);
    }
    int  shadeMode() const { return m_shadeMode; }
    /// The current mode's table row, and whether it is in the PBR family (PBR Shaded and its
    /// specular-only view) — the modes that render linear HDR through the image-based path, so
    /// the HDR post chain (SSS, bloom, ACES) and the HDRI backdrop run only for them.
    const ShadeMode& shadeModeSpec() const { return shadeModeAt(m_shadeMode); }
    bool             isPbr() const { return fragModeIsHdr(shadeModeSpec().fragMode); }
    /// Selects the figure joint nearest the pixel (@p px, @p py) in a @p vpW × @p vpH viewport (only
    /// if within a small radius; see bonepicker.h). Returns the selected bone index, or -1 if
    /// none is selected.
    int selectBoneAt(float px, float py, float vpW, float vpH, const Camera& camera);
    /// Selects the figure joint nearest the world point @p point within @p radius (metres) — a VR
    /// controller's HAND GRAB: the joint is taken where the hand is, its grab point the joint
    /// itself. Every figure's joints compete, the nearest of any figure wins and its figure
    /// becomes the active one. Returns the selected bone, or -1 with none within reach.
    int selectNearestBone(const glm::vec3& point, float radius);
    /// The pick selectBoneAt and shiftPressBoneAt share: the figure, the bone and the grab point
    /// under the pixel, with no selection made. False on a miss.
    bool pickBoneAt(float px, float py, float vpW, float vpH, const Camera& camera, int& outModel, int& outBone,
                    glm::vec3& outGrab);
    /// A SHIFT press at the pixel (THE GROUP, 2026-10-27): the same pick, but the selection is
    /// EXTENDED rather than replaced — the picked joint becomes the PRIMARY (Armature::makeGroupPrimary),
    /// the previous selection joining the group, and its grab point is set; @p wasMember says whether
    /// it was a member already (a CLICK then removes it: removeGroupBone). A pick on another figure
    /// than the active one, or of a finger or a toe, or with nothing selected, is a plain selection.
    /// Returns the primary, or -1 on a miss.
    int shiftPressBoneAt(float px, float py, float vpW, float vpH, const Camera& camera, bool& wasMember);
    /// Removes @p bone from the active figure's group (Armature::removeFromGroup).
    bool removeGroupBone(int bone);
    /// The active figure's group secondaries (empty without a figure).
    std::vector<int> groupBones() const;
    /// Diagnostics (the scripted test): puts the active figure's selection and group back
    /// (Armature::restoreSelection) after a by-name lookup replaced them.
    void restoreSelection(int primary, const std::vector<int>& secondaries);
    /// A/B probe (the scripts' `pick2d on|off`): the 2D projection pick alone, as before the
    /// skin cast of 2026-09-26 — which parts a click may reach through others.
    void setPick2D(bool on) { m_pick2D = on; }
    /// True if a joint is currently selected on the figure.
    bool hasSelectedBone() const;
    /// Selects the posable figure's bone named @p name (diagnostics / the IK benchmark).
    int selectBoneByName(const std::string& name);
    /// Rotates the selected joint by @p deltaEulerDegrees (accumulated), re-posing the figure.
    void nudgeSelectedBone(const glm::vec3& deltaEulerDegrees);
    /// The selected joint's TRANSFORM DIALS (the Transform tab; Armature::jointDial): its name and
    /// the three dials' values and ranges as the pose stands — invalid without a posable figure or
    /// a selected joint. The joint is the one the selection HIGHLIGHTS (the user's, through a
    /// landing bounce).
    JointTransform selectedJointTransform() const;
    /// Turns dial @p dial (a JointDialKind) of the selected joint to @p value. True if the pose
    /// changed — the pose may rest short of the value (the limits, the FK collision stop). The
    /// other joints of a Shift+click selection turn with it: by the same CHANGE on their own
    /// 0-100 scales — the change in what is ASKED, from @p previousAsk (the value last asked of
    /// this dial in the open edit) when given, else from the primary's value as it stands — or,
    /// @p absolute (the restore buttons), to @p value themselves.
    bool setSelectedJointDial(int dial, float value, bool absolute = false, const float* previousAsk = nullptr);
    /// How turning dial @p dial of the selected joint moves its limb, as the pose stands
    /// (Armature::jointDialSweep) — what the viewport's mouse mode follows. False without a
    /// selected joint or for a motion the joint does not have.
    bool selectedJointDialSweep(int dial, JointDialSweep& sweep) const;
    /// The "pose settled" hook after an interactive pose edit: re-evaluates the figure's pose
    /// corrective weights now (they blend live on the GPU during a drag anyway) and prints the
    /// active ones under POSESTUDIO_DUMP_CORRECTIVES — see Model::refreshCorrectives.
    void finalizePose();

    // --- Full-body IK (a plain drag on a joint; Ctrl+drag is the single-joint FK rotate) ---
    /// Begins an FBIK drag of the figure's selected joint: ground contacts are detected and become
    /// the anchor/pins, and the balance support polygon is captured. Returns false without a
    /// figure or selection.
    bool beginBoneIkDrag(IkScope scope = IkScope::Body);
    /// One FBIK drag update: solves the whole body so the selected joint reaches toward
    /// @p targetWorld (feet stay planted, CoM auto-balanced), then re-poses the figure. Returns
    /// true if the pose actually changed (false: the solve left the pose where it was — no redraw needed).
    bool dragBoneIkTo(const glm::vec3& targetWorld);
    /// One animated release-settle step (see Armature::settleIkTick): call at the drag tick rate
    /// after release until it returns false, then endBoneIkDrag().
    bool settleBoneIkTick();
    /// Ends the FBIK drag (the pose stays; call finalizePose() to settle correctives, as with any
    /// pose drag).
    void endBoneIkDrag();
    /// World-space position of the figure's selected joint (the IK drag plane's anchor point).
    /// Returns false when no joint is selected.
    bool selectedBoneWorldPosition(glm::vec3& out) const;
    /// The point of the selected bone the user took hold of (Armature::ikGrabPointWorld): the
    /// IK drag's plane passes through it and its targets are where IT should go.
    bool ikGrabPointWorld(glm::vec3& out) const;
    /// The posable figure's runtime skeleton, read-only, or null (diagnostics — see Model::armature).
    const Armature* figureArmature() const;
    /// Diagnostics (the IK benchmark): puts the grab point @p share of the way along the rigid
    /// limb segment the selected bone belongs to (Armature::setIkGrabOnSegment).
    void setIkGrabOnSegment(float share);
    // --- User joint pins (forwarded to the figure; see Armature::togglePinSelectedBone) ---
    bool togglePinSelectedBone();
    bool selectedBonePinned() const;
    bool hasPinnedBones() const;
    void unpinAllBones();
    // --- Pose utilities on the ACTIVE figure (see Armature::resetBone / mirrorPose) ---
    bool resetSelectedJoint(bool subtree);
    void resetPose();
    void mirrorPose();
    bool mirrorSelectedLimb();
    /// The posable figure's current lowest world height (see Model::groundGap); false without one.
    /// The viewport's ground button animates the drop from this through translateModelY.
    bool figureGroundGap(float& lowestY) const;
    /// Translates model @p index along world Y — the animated ground drop addresses the model it
    /// STARTED on by index, since undo can switch the active figure mid-fall. No-op for a bad index.
    void translateModelY(int index, float dy);
    /// THE LANDING BOUNCE of model @p index (Armature::beginLandingBounce: a figure dropped onto
    /// her feet absorbs the landing, deeper the faster she came down) — addressed by index like
    /// the drop it follows. begin: true when a bounce began (false: she did not land on her feet,
    /// or the model is no figure); tick: false once it is over; end: the pose she landed in.
    bool beginLandingBounce(int index, float impactSpeed);
    bool landingBounceTick(int index, float seconds);
    void endLandingBounce(int index);
    /// THE SOFT BREASTS (Armature::tickSoftBody) on model @p index: the fall's tick, its tail's
    /// test and rest; the settings applied to every figure; the active figure's swing (scripts).
    bool  tickSoftBody(int index, float dt);
    bool  softBodyMoving(int index) const;
    void  settleSoftBody(int index);
    void  applySoftBodySettings();
    float breastSwingDeg(int side) const;
    float breastHangMm(int side) const;
    float breastSideMm(int side) const;
    float breastUpMm(int side) const;
    float buttockMoveMm(int side) const; ///< THE SOFT BUTTOCKS: how far the left (0) / right (1) cheek has moved (mm).
    /// THE FIGURE'S PLACE AND HEADING on the active figure (the Transform tab's Figure group):
    /// position in world centimetres; the rotation as turn / tip / tilt (`axis` 0/1/2) in degrees.
    /// True when something moved.
    bool  setFigurePosition(int axis, float cm);
    bool  setFigureRotationDeg(int axis, float degrees);
    /// A STATIC model's uniform scale in percent of its imported size (the Transform tab's Scale
    /// row); false for a figure or with nothing static selected.
    bool setFigureScalePct(float pct);

    // --- THE MATERIALS TAB (materialparams.h; scenematerials.cpp): the selected object's zones ---
    /// What the tab shows: the selected model's zones and the selected zone's values, as they
    /// stand and as imported. Invalid without a selected model that has meshes.
    MaterialView materialView() const;
    /// Selects zone @p index of the selected model (the tab's picker). A click on a model's
    /// surface selects the zone it lands on (selectZoneAt, and the joint pick). Out of range or
    /// -1 = the first zone.
    void setSelectedZone(int index);
    int  selectedZone() const { return m_selectedZone; }
    /// Casts @p ray against the SELECTED model's surface and selects the zone it hits (a plain
    /// click on a model: the joint pick does the same for a figure's skin). False on a miss.
    bool selectZoneAt(const Ray& ray);
    /// Sets the selected zone's material — and, @p linked, every other zone of the model whose
    /// IMPORTED material equals its (the tab's linked zones: a figure's skin zones together).
    /// True if anything changed.
    bool setSelectedZoneMaterial(const MaterialParams& params, bool linked);
    /// Every zone material of model @p model in mesh order (an undo snapshot), and its
    /// re-application. Empty / no-op for a bad index.
    std::vector<MaterialParams> captureMaterials(int model) const;
    void applyMaterials(int model, const std::vector<MaterialParams>& materials);
    /// The selected zone's material back to its imported values — its linked zones' too,
    /// @p linked — or, @p allZones, every zone of the selected model's. True if anything changed.
    bool resetZoneMaterials(bool allZones, bool linked);
    /// THE TEXTURE SLOTS (the tab's Textures group; the Qt layer decodes and derives, this applies):
    /// the plain image mesh @p mesh of model @p model carries in slot @p map now or as @p imported
    /// (null = none; @p channel its packed channel), and the replacement — @p plain + @p channel +
    /// @p gpuImage (the derived upload, see Mesh::setSlotImage) with @p source recorded — after a
    /// device wait (the mesh's descriptor set is rewritten). A slot is the zone's own: never
    /// linked across zones. The selected zone's index is selectedZoneIndex() (-1 without one).
    std::shared_ptr<const DecodedImage> meshSlotImage(int model, int mesh, MaterialMap map, bool imported, int* channel = nullptr) const;
    bool setMeshTexture(int model, int mesh, MaterialMap map, const std::shared_ptr<const DecodedImage>& plain, int channel,
                        const std::shared_ptr<const DecodedImage>& gpuImage, const std::string& source);
    int  selectedZoneIndex() const;

    // --- Pose snapshot (for undo/redo) ---
    std::vector<std::pair<std::string, glm::vec3>> capturePose() const;
    void applyPose(const std::vector<std::pair<std::string, glm::vec3>>& pose);

    /// Saves / loads the figure's pose snapshot (rotations + the @trans:/@pin: rows) to/from a
    /// plain-text file (posefile.h). Returns false if there's no figure, the file can't be
    /// opened, or (load) the file is malformed — in which case the pose is left untouched.
    bool savePose(const std::string& path) const;
    bool loadPose(const std::string& path);

private:
    /// The figure the posing API addresses: the ACTIVE figure (activeFigureIndex — the last
    /// clicked, else the first), but ONLY while it is the selection or nothing is selected.
    /// With a different model selected (an OBJ next to a figure) it is null, so every pose
    /// utility no-ops and hasPosableFigure() greys the actions — they must never edit a figure
    /// that isn't the outlined selection.
    Model*       figureModel();
    const Model* figureModel() const;
    /// The selected model when it is STATIC (no skeleton: an OBJ, a splat scene) and no figure is
    /// active — what the Transform tab's Figure group places then; else null.
    Model*       selectedStaticModel();
    const Model* selectedStaticModel() const;

    VulkanContext& m_context;

    // Member order = reverse destruction order (after ~Scene's explicit model teardown): the
    // shared layouts/pipelines outlive the passes built against them, which outlive the maps
    // and models that bind through them.
    // The layouts, camera sets, IBL/shadow set, fallbacks, and mesh pipelines (scenepipelines.h).
    std::unique_ptr<ScenePipelines> m_pipelines;
    // The scene's other passes/overlays (each owns its pipeline + targets/buffers): the key-light
    // shadow pass (also the fitted light matrix + the shadow map bound as set 3 bindings 2/3),
    // the selection-outline mask pass, and the line overlay (skeleton, pins, floor line).
    std::unique_ptr<ShadowPass>     m_shadowPass;
    std::unique_ptr<OutlinePass>    m_outlinePass;
    std::unique_ptr<LineOverlay>    m_lineOverlay;
    // Image-based lighting: the prefiltered specular cubemap + BRDF LUT the scene-wide set 3
    // samples, (re)uploaded by applyBakedEnvironment() when the environment changes.
    std::unique_ptr<IblMaps>        m_iblMaps;

    std::vector<std::unique_ptr<Model>> m_models;
    int m_activeFigure = -1;
    bool m_pick2D = false; ///< See setPick2D(). ///< See activeFigureIndex(): the posing target among the figures.
    int m_selectedModel = -1; ///< See selectedModelIndex(): the outlined model (-1 = none).
    int m_selectedZone = -1;  ///< See selectedZone(): the selected model's zone the Materials tab shows (-1 = the first).
    int m_pickedZone = -1;    ///< The zone the last skin cast of pickBoneAt landed on (selectBoneAt reads it).

    int m_shadeMode = kDefaultShadeMode; // picker-table index (shademode.h)
    // The key light's world direction this frame (keyLightDirection), computed once by
    // recordShadowPass — which always precedes record() in a frame — and reused by the UBO fill.
    glm::vec3 m_keyLightDir{0.0f, 1.0f, 0.0f};

    // Lighting environment: the baked diffuse-irradiance SH (feeds the per-frame camera UBO so the PBR
    // mode is image-based-lit) and the environment-independent split-sum BRDF LUT (integrated once,
    // reused on every re-bake). The prefiltered specular cubemap lives in m_iblMaps. A CPU-baked
    // BakedEnvironment (SH + specular) is applied via applyBakedEnvironment(); no source image is kept.
    EnvironmentSH    m_environmentSH;
    BrdfLut          m_brdfLut;
    LightingSettings m_lighting; // live exposure/diffuse/specular/fill/key/rotation/tonemap dials (Environment panel)

    // The transparent pass's per-frame sort list (every model's transparent meshes together,
    // farthest first), kept as a member so the per-frame sort allocates nothing.
    struct TransparentDraw {
        float       distSq;
        Model*      model;
        const Mesh* mesh;
    };
    std::vector<TransparentDraw> m_transparentScratch;
};

} // namespace pose

#endif // SCENE_H
