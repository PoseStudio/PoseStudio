/**
 * @file model.h
 * @brief One imported model (OBJ mesh or rigged figure): its meshes under a shared transform,
 *        the armature that poses it, and the per-frame GPU pose data the shaders skin with.
 *
 * A Model groups the meshes of one imported file (one Mesh per material group — mesh.h), owns
 * the descriptor pool their set-1 descriptors and its own per-frame set-2 (pose) descriptors
 * come from, and carries an Armature (armature.h) — the runtime skeleton, pose, pins, and
 * full-body IK of a skinned figure — whose skinning dual quaternions it uploads per frame in
 * flight, next to the pose correctives' weights (CorrectiveSet) and the ground samples the
 * ground button scans (GroundSampler). Every posing call here is a thin forwarder into the
 * armature (the ones that change the pose also refresh this model's correctives, which the
 * armature can't know about); new posing behaviour belongs in the armature, never here — the
 * split is what lets the IK harness drive the real drag loop without Vulkan. Qt-free.
 */

#ifndef MODEL_H
#define MODEL_H

#include "armature.h"      // the skeleton + pose + IK a Model poses through
#include "correctiveset.h" // the GPU-blended pose correctives
#include "groundsampler.h" // the ground button's posed lowest-point scan
#include "mesh.h"
#include "vulkanbuffer.h"
#include "vulkancommon.h"  // kMaxFramesInFlight (per-frame pose buffers)
#include "vulkanhandles.h" // UniqueDescriptorPool

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <limits>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pose {

class VulkanContext;
class VulkanTexture;
struct DecodedImage;
struct ModelData;
struct Ray;
class SplatCloud;
struct SplatData;

class Model {
public:
    /// Uploads @p data's meshes (one ImmediateBatch for the whole model — one GPU round-trip),
    /// resolves its pose correctives, builds its armature, and allocates its descriptors:
    /// set 1 per mesh from @p materialSetLayout, and one set 2 per frame in flight from
    /// @p poseSetLayout. Meshes without a given map bind the shared 1x1 fallbacks.
    Model(VulkanContext& context, const ModelData& data, VkDescriptorSetLayout materialSetLayout,
          VkDescriptorSetLayout poseSetLayout, const VulkanTexture& fallbackDiffuse,
          const VulkanTexture& fallbackNormal);
    /// A GAUSSIAN SPLAT scene (splatcloud.h): a static model with no meshes and no skeleton,
    /// drawn by the Scene's splat pass; its bounds are the splats' centres'.
    Model(VulkanContext& context, const SplatData& data, VkDescriptorSetLayout splatSetLayout);
    ~Model();

    /// The splat cloud of a splat-scene model, else null.
    SplatCloud*       splats() { return m_splats.get(); }
    const SplatCloud* splats() const { return m_splats.get(); }
    /// Whether the model has any mesh to draw (a splat model has none).
    bool hasMeshes() const { return !m_meshes.empty(); }

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    // --- Drawing. Every path first uploads this frame slot's pose data if stale and binds the
    // pose set (bindPose); whichever runs first in a frame does the copies, the rest no-op. ---

    /// Uploads frame slot @p frameIndex's pose data (skin dual quaternions + corrective weights)
    /// if that slot hasn't seen the current pose, and binds the slot's set-2 descriptor at
    /// @p setIndex (2 in the mesh passes, 0 in the position-only shadow/outline passes — set
    /// compatibility is by layout, not index). The Scene's cross-model transparent pass calls
    /// this whenever the model it is drawing changes. No-op for a model with no meshes (which
    /// never created its descriptors).
    void bindPose(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t setIndex,
                  uint32_t frameIndex);

    /// Records the OPAQUE meshes at the current transform (the caller has bound the opaque
    /// pipeline). The transparent meshes are drawn by the Scene, sorted back-to-front across
    /// every model (see transparentMeshes / posedCentroid).
    void recordOpaque(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex);

    /// Records EVERY mesh as wireframe edges (the caller has bound the wire pipeline): the
    /// wireframe shade modes' edge overlay, or their whole drawing when there is no surface.
    void recordWire(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex);

    /// Records the OPAQUE meshes as a depth-only surface fill (the caller has bound the
    /// hidden-line pipeline, whose color writes are masked): the surface occludes what lies
    /// behind it — the grid, the far side's wires — while showing the viewport's clear color.
    /// Transparent meshes are left out so an eye's iris wires stay visible through its shells.
    void recordDepthFill(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex);
    /// Records the SEE-THROUGH meshes — cards, shells, panes — as occluders of the Gaussian
    /// splat layer (the caller has bound ScenePipelines::depthCutout): depth where each has
    /// enough coverage, from a half or, @p fine, from a twelfth (depthcutout.frag says why).
    void recordDepthCutout(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex, bool fine);

    /// Records the model's opaque meshes into the depth-only shadow pass (the caller has bound the
    /// shadow pipeline and begun its render pass). Transparent meshes (clear eye shells, cutout
    /// lash/brow cards) are skipped — the pass has no alpha, so they'd cast solid-card shadows.
    void recordShadow(VkCommandBuffer cmd, VkPipelineLayout layout,
                      const glm::mat4& lightViewProj, uint32_t frameIndex);

    /// Records the model's silhouette into the selection-outline mask pass (the caller has
    /// bound the mask pipeline and begun its render pass): every mesh, skinned and projected by
    /// the camera's @p viewProj, except opacity-masked cards (lashes/brows), whose solid card
    /// geometry would put rectangular bumps on the outline — the shape the user sees exists only
    /// in a texture this material-less pass doesn't sample. Translucent shells (a cornea) ARE
    /// drawn: a selected translucent object still has a silhouette.
    void recordSilhouette(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& viewProj,
                          uint32_t frameIndex);

    /// The meshes that must be drawn in the alpha-blended pass (Mesh::isTransparent), partitioned
    /// at construction and again when a material edit changes a mesh's transparency.
    const std::vector<const Mesh*>& transparentMeshes() const { return m_transparentMeshes; }

    // --- THE MATERIALS TAB (2026-11-09): the zones' editable materials (materialparams.h) ---
    std::size_t meshCount() const { return m_meshes.size(); }
    const Mesh& mesh(std::size_t i) const { return m_meshes[i]; }
    /// The key a project's material override addresses mesh @p i by: its zone name, or "#i" for
    /// an unnamed zone (an MTL without names).
    std::string zoneKey(std::size_t i) const;
    /// Replaces mesh @p i's material values (Mesh::setMaterial), re-partitioning the meshes if its
    /// transparency changed. True if anything changed; bumps materialRevision().
    bool setMeshMaterial(std::size_t i, const MaterialParams& params);
    /// Every mesh's material values in mesh order (an undo snapshot), and their re-application.
    std::vector<MaterialParams> captureMaterials() const;
    void applyMaterials(const std::vector<MaterialParams>& materials);
    /// One zone's departure from the import (the .pss document's `materials` block): its key, its
    /// values, and the texture slots whose source differs from the import's — (slot name, source;
    /// "" = cleared). materialOverrides() lists every zone that differs; applyMaterialOverride is
    /// the VALUES' inverse — every zone with key @p zone takes @p params (true if one did); the
    /// slots come back through the window's applySlotSource.
    struct MaterialOverride {
        std::string                                      zone;
        MaterialParams                                   params;
        std::vector<std::pair<std::string, std::string>> textures;
    };
    std::vector<MaterialOverride> materialOverrides() const;
    /// (@p keepSmoothing: the zone keeps its own smoothing whatever @p params carry — a project
    /// saved before THE SMOOTHING existed says nothing of it.)
    bool applyMaterialOverride(const std::string& zone, const MaterialParams& params, bool keepSmoothing = false);
    /// THE TEXTURE SLOTS: replaces slot @p map of mesh @p i (Mesh::setSlotImage — the caller has
    /// derived @p gpuImage and waited for the device), re-partitioning the meshes if the cutout
    /// changed the mesh's transparency; bumps materialRevision(). False for a bad index.
    bool setMeshTexture(std::size_t i, MaterialMap map, const std::shared_ptr<const DecodedImage>& plain, int channel,
                        const std::shared_ptr<const DecodedImage>& gpuImage, const std::string& source);
    /// THE OVERLAYS (materialoverlay.h): replaces mesh @p i's stack (the values; the caller
    /// rebinds the slots the stack is painted into); bumps materialRevision(). True if it changed.
    bool setMeshOverlays(std::size_t i, const std::vector<MaterialOverlay>& overlays);
    /// Mesh @p i's metalness as BAKED into its bound parameter map for the Metallic value
    /// @p scalar (Mesh::setMetalBaked; negative = not baked); bumps materialRevision().
    void setMeshMetalBaked(std::size_t i, float scalar);
    /// The zones an overlay on mesh @p i is painted on: @p i and every other mesh imported with
    /// the same base-color map, in mesh order (MaterialView::overlayZones).
    std::vector<int> overlayZones(std::size_t i) const;
    /// Bumped by every material change — the cheap "has anything changed" the frame notify reads.
    std::uint64_t materialRevision() const { return m_materialRevision; }

    /// THE SMOOTHING (import/smoothnormals.h): true while a zone's material asks for another
    /// smoothing than its geometry has (a value set through setMeshMaterial — the tab, a linked
    /// zone, undo, Reset, a project).
    bool smoothingPending() const;
    /// Brings the geometry in line: the zones that are to be smooth by angle are computed again
    /// from their FILE geometry — together, a surface being smoothed across its zones, so a zone
    /// whose own setting did not move may still come out different — and uploaded where they
    /// changed; a zone that is to be as imported gets its file geometry back. New device buffers:
    /// THE CALLER HAS WAITED FOR THE DEVICE. True if any geometry changed (materialRevision()
    /// is bumped: a render engine's scene is another one).
    bool applySmoothing();

    /// The world-space point a transparent @p mesh (one of this model's) sorts by: its bind
    /// centroid carried by its weight-dominant joint's CURRENT skin transform, through the model
    /// transform. A bind centroid alone mis-orders posed shells — a figure lying on its back
    /// after a hip drag has its eye shells' bind centroids nowhere near their posed order — and
    /// the dominant joint's rigid motion is exact for the near-single-joint shells that layer.
    glm::vec3 posedCentroid(const Mesh& mesh) const;

    /// The model's world-space AABB (local bounds through the model transform), unioned with a
    /// posed figure's live joint positions plus a flesh margin. Returns false when the model has
    /// no geometry bounds. Used to fit the key light's shadow frustum and for framing.
    bool worldBounds(glm::vec3& outMin, glm::vec3& outMax) const;

    /// The model matrix (owned by the armature — IK, picking, and grounding all need it).
    const glm::mat4& transform() const { return m_armature.transform(); }

    /// The file this model was imported from (the .pss document references its figures by this
    /// path). Empty for a model added without one.
    const std::string& sourcePath() const { return m_sourcePath; }
    void setSourcePath(std::string path) { m_sourcePath = std::move(path); }

    /// Poses the joint @p boneName by an Euler rotation (degrees) applied in its local frame, then
    /// recomputes the skin data and refreshes the correctives. No-op if the model has no such
    /// bone. This is the primitive a posing UI drives; @p eulerDegrees of 0 restores the rest pose.
    void setBoneRotation(const std::string& boneName, const glm::vec3& eulerDegrees);

    /// Whether this model has a skeleton (i.e. is a posable figure rather than a static mesh).
    bool hasSkeleton() const { return m_armature.hasSkeleton(); }

    // --- Posing-UI support (world-space skeleton for overlay/picking + interactive rotation) ---
    std::size_t      boneCount() const { return m_armature.boneCount(); }
    /// Current world-space position of each joint (updated whenever the pose changes).
    const glm::vec3& boneWorldPosition(std::size_t i) const { return m_armature.boneWorldPosition(i); }
    int              boneParent(std::size_t i) const { return m_armature.boneParent(i); }

    int  selectedBone() const { return m_armature.selectedBone(); }
    /// The joint the selection highlight shows (Armature::highlightBone: the selected joint, the
    /// user's through a landing bounce).
    int  highlightBone() const { return m_armature.highlightBone(); }
    /// The runtime skeleton, read-only (diagnostics: the scripted posing test reads joint
    /// positions, channels and the rig's step count from it).
    const Armature& armature() const { return m_armature; }
    void setSelectedBone(int index) { m_armature.setSelectedBone(index); }
    // THE GROUP (Armature::toggleGroupBone): several joints dragged together.
    bool toggleGroupBone(int index) { return m_armature.toggleGroupBone(index); }
    bool makeGroupPrimary(int index) { return m_armature.makeGroupPrimary(index); }
    bool removeFromGroup(int index) { return m_armature.removeFromGroup(index); }
    bool inGroup(int index) const { return m_armature.inGroup(index); }
    const std::vector<int>& groupBones() const { return m_armature.groupBones(); }
    void restoreSelection(int primary, const std::vector<int>& secondaries) { m_armature.restoreSelection(primary, secondaries); }
    /// What bone @p index is to the posing UI (Armature::boneClass: a joint of the body, a bone of
    /// the face rig — which selects the head — or a finger's or a toe's joint).
    BoneClass boneClass(int index) { return m_armature.boneClass(index); }
    /// The selected joint's highlight twin (a bend bone's twist child and vice versa; see
    /// Armature), or -1 — the second joint the selected-part highlight tints.
    int selectedHighlightTwin() const { return m_armature.selectedHighlightTwin(); }
    /// Selects the bone named @p name (diagnostics / the IK benchmark); its index, or -1.
    int selectBoneByName(const std::string& name) { return m_armature.selectBoneByName(name); }
    /// Adds @p deltaEulerDegrees to the selected bone's accumulated rotation and re-poses it.
    void nudgeSelectedBone(const glm::vec3& deltaEulerDegrees) {
        m_armature.nudgeSelectedBone(deltaEulerDegrees);
    }
    /// Turns one of a joint's transform dials (Armature::setJointDial; the dial itself is read off
    /// armature()). The correctives follow at the next recorded frame, like any pose change.
    bool setJointDial(const JointDial& dial, float value) { return m_armature.setJointDial(dial, value); }

    // --- Full-body IK (scene/ik/, driven through the armature): drag a joint, the body follows ---
    bool beginIkDrag(IkScope scope = IkScope::Body) { return m_armature.beginIkDrag(scope); }
    void setIkGrabPoint(int bone, const glm::vec3& worldPoint) { m_armature.setIkGrabPoint(bone, worldPoint); }
    glm::vec3 ikGrabPointWorld() const { return m_armature.ikGrabPointWorld(); }
    void setIkGrabOnSegment(int bone, float share) { m_armature.setIkGrabOnSegment(bone, share); }
    bool dragIkTo(const glm::vec3& targetWorld) { return m_armature.dragIkTo(targetWorld); }
    bool settleIkTick() { return m_armature.settleIkTick(); }
    /// Ends the FBIK drag (the solved pose stays; the caller refreshes the correctives).
    void endIkDrag() { m_armature.endIkDrag(); }
    // --- The landing bounce (see Armature): the Ground button's fall, landed on her feet ---
    bool beginLandingBounce(float impactSpeed) { return m_armature.beginLandingBounce(impactSpeed); }
    bool landingBounceTick(float seconds) { return m_armature.landingBounceTick(seconds); }
    // THE HANGING FEET COME DOWN FLAT (Armature::beginHangFeetLanding): the Ground button's fall.
    bool beginHangFeetLanding() { return m_armature.beginHangFeetLanding(); }
    bool poseHangFeet(float hang) { return m_armature.poseHangFeet(hang, -1.0e9f); } // (no floor: the window measures the drop itself)
    bool poseHangFeetForFall(float remaining, float total) { return m_armature.poseHangFeetForFall(remaining, total); }
    void endHangFeetLanding() { m_armature.endHangFeetLanding(); }
    // THE SOFT BREASTS (Armature::tickSoftBody): the fall's and the settings' hooks.
    bool  tickSoftBody(float dt) { return m_armature.tickSoftBody(dt); }
    void  settleSoftBody() { m_armature.settleSoftBody(); }
    bool  softBodyMoving() const { return m_armature.softBodyMoving(); }
    void  applySoftBodySettings() { m_armature.applySoftBodySettings(); }
    float breastSwingDeg(int side) const { return m_armature.breastSwingDeg(side); }
    float breastHangMm(int side) const { return m_armature.breastHangMm(side); }
    float breastSideMm(int side) const { return m_armature.breastSideMm(side); }
    float breastUpMm(int side) const { return m_armature.breastUpMm(side); }
    float buttockMoveMm(int side) const { return m_armature.buttockMoveMm(side); }
    // THE FIGURE'S PLACE AND HEADING (the Transform tab's Figure group): pose edits like a dial's.
    void setFigurePosition(const glm::vec3& world) { m_armature.setFigurePosition(world); refreshCorrectives(); }
    void setFigureRotationDeg(const glm::vec3& turnTipTilt) { m_armature.setFigureRotationDeg(turnTipTilt); refreshCorrectives(); }
    /// Ends the bounce: the pose she landed in, exactly, and its correctives.
    void endLandingBounce() {
        m_armature.endLandingBounce();
        refreshCorrectives();
    }
    // --- User joint pins (see Armature) ---
    bool togglePinSelectedBone() { return m_armature.togglePinSelectedBone(); }
    bool isBonePinned(std::size_t index) const { return m_armature.isBonePinned(index); }
    bool selectedBonePinned() const { return m_armature.selectedBonePinned(); }
    bool hasPinnedBones() const { return m_armature.hasPinnedBones(); }
    void unpinAllBones() { m_armature.unpinAllBones(); }
    // --- Pose utilities (see Armature): reset / mirror. The correctives follow at the next
    // recorded frame like any pose change; the caller's finalizePose is the settled-pose hook.
    bool resetSelectedBone(bool subtree) {
        return m_armature.resetBone(m_armature.selectedBone(), subtree);
    }
    void resetPose() { m_armature.resetPose(); }
    void mirrorPose() { m_armature.mirrorPose(); }
    bool mirrorSelectedLimb() {
        return m_armature.mirrorSubtreeToOpposite(m_armature.selectedBone());
    }
    /// The rig's CONTACT pins (ground-detected, not user pins) while an IK drag is active — for
    /// the overlay's "which feet are planted" markers. Empty outside a drag.
    std::vector<int> activeContactPins() const { return m_armature.activeContactPins(); }

    /// Captures the current pose as (bone name, Euler degrees) for each non-rest joint, plus the
    /// @trans:/@pin: rows (see Armature::capturePose) — the undo/pose-file snapshot.
    std::vector<std::pair<std::string, glm::vec3>> capturePose() const {
        return m_armature.capturePose();
    }
    /// Resets to bind pose, then applies @p pose (bone name -> Euler degrees; unknown bones
    /// ignored) and re-evaluates the corrective weights for the restored pose (the GPU blends
    /// them at the next recorded frame).
    void applyPose(const std::vector<std::pair<std::string, glm::vec3>>& pose);

    /// Sets the model's transform matrix (the armature owns it — IK, picking, and grounding all
    /// read it) and recomputes the skin data. The .pss load path restores a saved TRS through it.
    void setTransform(const glm::mat4& transform);
    /// A STATIC model's (turn, tip, tilt) as the Transform tab last showed or set them: the
    /// branch its orientation is read back on (Scene's decomposeStaticTransform), so a Tip carried
    /// past 90 reads on instead of flipping the other two by 180.
    glm::vec3 staticRotationShown() const { return m_staticRotationShown; }
    /// (A reader's bookkeeping, not a change of the model: const, on a mutable member.)
    void setStaticRotationShown(const glm::vec3& turnTipTilt) const { m_staticRotationShown = turnTipTilt; }
    /// Re-evaluates the pose correctives' weights against the current pose now and, with
    /// POSESTUDIO_DUMP_CORRECTIVES set, prints the active ones (CorrectiveSet::refresh). The
    /// explicit "pose settled" hook (Scene::finalizePose, setBoneRotation, applyPose); every
    /// recorded frame re-evaluates them from the live pose anyway, so a drag tracks without it.
    void refreshCorrectives() { m_correctives.refresh(m_armature); }

    /// Tests @p ray (world space) against the model's box: a static model's local bind AABB
    /// through the model matrix, a POSED figure's world-space box of its live joints plus a flesh
    /// margin. On a hit, writes the entry distance (a world-space ray parameter, so values are
    /// comparable across models) to @p tOut and returns true. Box-level precision — enough to
    /// pick which imported object the cursor is over.
    bool intersectRay(const Ray& ray, float& tOut) const;
    /// The FIRST point of the CURRENT pose's SKIN along @p ray (world) and the joint that weighs
    /// most on the skin there — what a click is ON (GroundSampler::pickSurface; the opaque meshes
    /// only) — and, in @p mesh when given, the mesh (material zone) the hit is on. A STATIC model's
    /// surface is cast through its transform (its samples are kept up to a triangle cap). False
    /// for a ray that misses or a model without samples.
    bool pickSurface(const Ray& ray, float& tOut, glm::vec3& hitWorld, int& bone, int* mesh = nullptr) const;
    /// What a click's ray meets of a model (pickExact). Distances are the world's, along the
    /// ray's unit direction: comparable across models.
    struct ModelHit {
        /// The first of the model's surface along the ray: a triangle of any kind (a see-through
        /// one included: hair cards, lashes, glass), or where a splat scene's opacity gathers.
        float t = std::numeric_limits<float>::max();
        /// The first of it that HIDES what is behind it — an opaque triangle; a splat scene where
        /// its opacity along the ray reaches a half — or the largest float: nothing of it does.
        float tSolid = std::numeric_limits<float>::max();
        /// A splat scene fainter than a half under the cursor: on it, but only just — any other
        /// surface under the cursor is what the click means.
        bool faint = false;
    };
    /// WHETHER THE CURSOR IS ON THIS MODEL ITSELF — its surface, not its box — and where along
    /// @p ray (world): a figure's or a prop's triangles (GroundSampler::pickAnySurface), a
    /// Gaussian splat scene's opacity under the cursor (SplatCloud::pick). False for a ray that
    /// misses the surface, for a splat scene the eye stands INSIDE (an environment is not what
    /// a click in it means: intersectRay's rule), and for a model that keeps no surface to cast
    /// against (a static mesh past the triangle cap): that one is picked by its box alone, as
    /// ever (Scene::pickModel).
    bool pickExact(const Ray& ray, ModelHit& hit) const;

    /// One mesh of the model AS POSED, in world space — what the vertex stage makes of it
    /// (mesh.vert: the pose correctives and soft-body fields blended into the bind mesh, then the
    /// dual-quaternion or linear skin, then the model transform), computed on the CPU for a
    /// RENDER ENGINE. Per vertex: position, unit normal, UV and the UV tangent with its
    /// handedness (w = 0 where none was baked); the indices are the mesh's own.
    struct PosedMesh {
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> normals;
        std::vector<glm::vec2> uvs;
        std::vector<glm::vec4> tangents;
        std::vector<uint32_t>  indices;
    };
    /// Fills @p out with mesh @p i as the pose stands (the corrective weights are re-evaluated
    /// first). False for a bad index or a model without meshes.
    bool bakePosedMesh(std::size_t i, PosedMesh& out);

    /// The world height of the CURRENT pose's lowest point (GroundSampler::lowestY): positive =
    /// hovering that far above the floor, negative = sunk into it. False when there is no
    /// geometry to measure. The viewport animates the ground button's drop from this and
    /// applies it through translateY.
    bool groundGap(float& lowestY) const;
    /// Moves the model by @p dy along world Y (the animated ground drop applies its per-frame fall
    /// increments through this): a FIGURE in her pose — the root's pose translation, so the drop is
    /// a pose edit (Armature::shiftPoseY) — a static model by its transform.
    void translateY(float dy) { m_armature.shiftPoseY(dy); }

private:
    mutable glm::vec3 m_staticRotationShown{0.0f};

    /// Which of the model's meshes a record path draws.
    enum class MeshFilter { Opaque, All, NotMasked };

    /// Rebuilds m_opaqueMeshes / m_transparentMeshes from Mesh::isTransparent (at construction,
    /// and after a material edit moved a mesh across).
    void repartitionMeshes();

    /// The shaded record paths (recordOpaque / recordWire / recordDepthFill): binds the pose set
    /// at index 2 and draws the filtered meshes as @p kind through Mesh::record.
    void recordShaded(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex,
                      MeshDrawKind kind, MeshFilter filter);
    /// The position-only record paths (recordShadow / recordSilhouette): binds the pose set at
    /// index 0, pushes (@p viewProj, model) once for every mesh, and draws the filtered meshes'
    /// geometry alone (Mesh::recordDepth).
    void recordPositionOnly(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& viewProj,
                            uint32_t frameIndex, MeshFilter filter);

    /// Uploads the armature's skin data into frame slot @p frameIndex's joint buffer if that slot
    /// hasn't seen the current Armature::skinVersion yet, and likewise the pose-corrective
    /// weights (CorrectiveSet::uploadIfDirty).
    void uploadJointsIfDirty(uint32_t frameIndex);
    /// Copies every mesh's packed material (Mesh::gpu) into frame slot @p frameIndex's material
    /// buffer if that slot hasn't seen the current materialRevision() yet (set 2 binding 3).
    void uploadMaterialsIfDirty(uint32_t frameIndex);

    // Declared before the meshes and pose sets whose descriptors it owns, so it is destroyed
    // after them; RAII so a throwing constructor step (a mesh upload, a descriptor allocation)
    // can't leak it. Sized to one set-1 per mesh + one set-2 per frame slot.
    UniqueDescriptorPool     m_descriptorPool;
    std::vector<Mesh>        m_meshes;
    // The meshes partitioned once by Mesh::isTransparent (pointers into m_meshes, which never
    // changes after construction), so the record paths don't re-test every mesh every frame.
    std::vector<const Mesh*> m_opaqueMeshes;
    std::vector<const Mesh*> m_transparentMeshes;
    glm::vec3                m_boundsMin{0.0f}; // local-space AABB over all mesh vertices
    glm::vec3                m_boundsMax{0.0f};
    bool                     m_hasBounds = false;

    // The skeleton + pose + pins + IK (and the model transform) — see armature.h.
    Armature                 m_armature;

    std::string              m_sourcePath; // the import source file (the .pss document's reference)
    std::uint64_t            m_materialRevision = 0; // see materialRevision()
    SlotUploads              m_slotUploads;          // the texture slots' shared uploads (weak: see mesh.h)
    std::unique_ptr<SplatCloud> m_splats;    // a Gaussian splat scene's cloud (splatcloud.h); null for a mesh model

    // Skinning upload: the armature's dual quaternions (see Armature::skinDualQuats) reach the
    // GPU through a storage buffer (set 2) bound per draw — ONE PER FRAME IN FLIGHT: interactive
    // posing rewrites the data at 60 Hz, and a single shared buffer was overwritten while a
    // still-executing frame READ it — vertices skinned by half-updated matrices streaked into
    // momentary exploded-geometry artifacts (and the same torn frame corrupted the shadow
    // pass). At record time this frame slot's fence has been waited on, so writing ITS buffer
    // can never race the GPU.
    std::array<VulkanBuffer, kMaxFramesInFlight>    m_jointBuffers;
    std::array<VkDescriptorSet, kMaxFramesInFlight> m_jointSets{}; // set 2; from m_descriptorPool
    std::array<std::uint64_t, kMaxFramesInFlight>   m_jointUploaded{}; // last skin version per slot
    // THE MATERIAL BUFFER: every mesh's MaterialGpu per frame slot (binding 3 of the pose set),
    // rewritten when a slot is behind m_materialRevision (uploadMaterialsIfDirty).
    std::array<VulkanBuffer, kMaxFramesInFlight>    m_materialBuffers;
    std::array<std::uint64_t, kMaxFramesInFlight>   m_materialUploaded{}; // materialRevision + 1 last written per slot
    // The joint buffer's SECOND HALF: per joint, whether its flesh is lit WITH the selection
    // (Armature::highlightedWithSelection — the face rig under a selected head, the group's
    // members), as the .x of one more entry per joint; written per slot when the selection
    // changes (the key is the selected bone + 2 under the group's version, so that 0 — the
    // array's initial value — is "never written").
    std::array<std::uint64_t, kMaxFramesInFlight>   m_highlightUploaded{};
    uint32_t                                        m_jointCount = 1;
    bool                                            m_linearSkinning = false; ///< MeshData::linearSkinning: the joint buffer's second half, entry 0's .y.

    // Pose correctives, blended on the GPU (set 2 bindings 1 + 2 — see correctiveset.h).
    CorrectiveSet            m_correctives;
    // The ground button's samples (skinned models only — see groundsampler.h).
    GroundSampler            m_ground;
};

} // namespace pose

#endif // MODEL_H
