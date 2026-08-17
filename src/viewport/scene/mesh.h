/**
 * @file mesh.h
 * @brief GPU-resident geometry: a Mesh (one material group) and a Model (one imported file).
 *
 * A Mesh owns device-local vertex + index buffers, its material parameters (base color, roughness,
 * opacity), and its two textures — a diffuse map and a detail (normal/bump) map, each falling back to
 * a shared 1x1 texture when absent — bound through its own descriptor set (set 1, two samplers). A
 * Model groups the meshes of one imported file under a shared transform, owns the descriptor pool
 * those sets are allocated from, and carries the runtime skeleton for skinned figures. Qt-free.
 */

#ifndef MESH_H
#define MESH_H

#include "modeldata.h" // CorrectiveFormula (runtime corrective evaluation)
#include "vulkanbuffer.h"
#include "vulkanimage.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pose {

class VulkanContext;
class ImmediateBatch;
struct Ray;

/// Per-draw push-constant block for the mesh shaders. Must stay byte-identical to the
/// `push_constant` block in mesh.frag (mat4 + 4×vec4 = 128 bytes — exactly the guaranteed
/// push-constant minimum, so no further vec4 fits; mesh.vert declares the shorter prefix).
struct MeshPushConstants {
    glm::mat4 model;
    glm::vec4 baseColor; // rgb tint; a = opacity (1 = opaque)
    glm::vec4 material;  // x = roughness (ratio-blended), y = normalMode (0/1/2), z = metalness,
                         // w = specular (F0) weight (1 = 4% dielectric)
    glm::vec4 material2; // x = lobe1 rough, y = lobe2 rough, z = lobe ratio (1 = single-lobe),
                         // w = translucency weight
    glm::vec4 material3; // x = top-coat weight (0 = none), y = top-coat roughness,
                         // z = transparent-pass flag (0 = opaque: PBR writes the SSS mask into
                         // alpha; 1 = transparent: alpha stays the blend opacity),
                         // w = micro-detail packed as floor(tiles) + fract(weight) — the block is
                         //     at the 128-byte push limit, so the pair shares one float (0 = none)
};

/// Push-constant block for the depth-only shadow pass. Must stay byte-identical to shadow.vert's
/// `push_constant` block (two mat4s = 128 bytes, the guaranteed push-constant minimum).
struct ShadowPushConstants {
    glm::mat4 lightViewProj;
    glm::mat4 model;
};

/// One material group of a model: device-local vertex/index buffers, material params (base color,
/// roughness, opacity), and its diffuse + detail (normal/bump) textures bound through its own set-1
/// descriptor (two samplers, each with a shared 1x1 fallback when the map is absent).
class Mesh {
public:
    /// @param materialSetLayout  The shared set-1 layout (Scene owns it).
    /// @param materialPool       The owning Model's pool to allocate this mesh's set from.
    /// @param fallbackDiffuse    Shared 1x1 white texture, bound when the mesh has no diffuse map.
    /// @param fallbackNormal     Shared 1x1 flat-normal texture, bound when the mesh has no detail map.
    /// @param batch              Upload batch the mesh's buffers/textures record into (the Model
    ///                           flushes it once for all its meshes — one submit per import).
    Mesh(VulkanContext& context, const MeshData& data, VkDescriptorSetLayout materialSetLayout,
         VkDescriptorPool materialPool, const VulkanTexture& fallbackDiffuse,
         const VulkanTexture& fallbackNormal, ImmediateBatch& batch);

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) noexcept = default;
    Mesh& operator=(Mesh&&) noexcept = default;

    /// Binds set 1 (this mesh's texture), pushes (model, baseColor, material), binds buffers, and
    /// draws. The caller has already bound the mesh pipeline and the per-frame camera set (set 0).
    /// @p transparentPass tells the shader which alpha semantics apply (blend alpha vs SSS mask).
    void record(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& model,
                bool transparentPass) const;

    /// Depth-only draw for the shadow pass: just buffers + drawIndexed — no material set, no push
    /// (the Model pushes the light matrices once for all its meshes).
    void recordDepth(VkCommandBuffer cmd) const;

    /// Replaces the vertex buffer contents (same vertex count) — used to push a re-morphed mesh after
    /// pose correctives change. Records into @p batch; the caller submits it and must have made the
    /// GPU idle first (the old buffer may still be referenced by an in-flight frame).
    void reuploadVertices(VulkanContext& context, const std::vector<Vertex>& vertices,
                          ImmediateBatch& batch);

    /// True if this mesh is (partly) see-through and must be drawn in the transparent pass. An
    /// opacity mask baked into the diffuse alpha makes a mesh transparent even at scalar opacity 1
    /// (a lash/brow card's shape exists only in the mask).
    bool isTransparent() const { return m_opacity < 0.999f || m_hasOpacityMask; }

    /// Bind-pose centroid (local space) — the transparent pass's back-to-front sort key.
    const glm::vec3& centroid() const { return m_centroid; }

private:
    VulkanBuffer                   m_vertexBuffer;
    VulkanBuffer                   m_indexBuffer;
    uint32_t                       m_indexCount = 0;
    glm::vec3                      m_centroid{0.0f}; // bind-pose local centroid (transparency sort)
    glm::vec3                      m_baseColor{0.8f};
    float                          m_roughness = 0.7f;
    float                          m_specularWeight = 1.0f; // F0 multiplier (1 = 4% dielectric)
    float                          m_metalness = 0.0f;
    float                          m_lobe1Roughness = 0.7f; // dual-lobe spec (ratio 1 = single lobe)
    float                          m_lobe2Roughness = 0.7f;
    float                          m_lobeRatio = 1.0f;
    float                          m_topCoatWeight = 0.0f;
    float                          m_topCoatRoughness = 0.65f;
    float                          m_translucencyWeight = 0.5f;
    float                          m_detailWeight = 0.0f; // micro-detail normal strength (0 = none)
    float                          m_detailTiles = 0.0f;  // micro-detail UV tiling
    float                          m_opacity = 1.0f;
    bool                           m_hasOpacityMask = false;
    int                            m_normalMode = 0;              // 0 none / 1 normal map / 2 bump map
    std::unique_ptr<VulkanTexture> m_texture;                     // diffuse; null => white fallback
    std::unique_ptr<VulkanTexture> m_normalTexture;               // detail; null => flat-normal fallback
    std::unique_ptr<VulkanTexture> m_roughnessTexture;            // linear ×-multiplier map; null => white
    std::unique_ptr<VulkanTexture> m_specMaskTexture;             // linear spec mask; null => white
    std::unique_ptr<VulkanTexture> m_translucencyTexture;         // sRGB translucency map; null => white
    std::unique_ptr<VulkanTexture> m_microNormalTexture;          // linear tiled pore normals; null => flat
    VkDescriptorSet                m_materialSet = VK_NULL_HANDLE; // set 1; owned by the Model's pool
};

/// One imported OBJ: its meshes (each a material group) plus a shared model transform. Owns the
/// descriptor pool the meshes' set-1 descriptors are allocated from.
class Model {
public:
    Model(VulkanContext& context, const ModelData& data, VkDescriptorSetLayout materialSetLayout,
          VkDescriptorSetLayout jointSetLayout, const VulkanTexture& fallbackDiffuse,
          const VulkanTexture& fallbackNormal);
    ~Model();

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    /// Records the model's sub-meshes at its current transform. @p transparentPass selects which
    /// meshes are drawn: false = only opaque meshes, true = only transparent ones (sorted
    /// back-to-front by centroid distance to @p cameraPos, so layered shells — lashes over cornea
    /// over sclera — blend correctly from any angle). The caller draws the whole scene's opaque
    /// pass first, then binds the blend pipeline for the transparent pass.
    void record(VkCommandBuffer cmd, VkPipelineLayout layout, bool transparentPass,
                const glm::vec3& cameraPos) const;

    /// Records the model's opaque meshes into the depth-only shadow pass (the caller has bound the
    /// shadow pipeline and begun its render pass). Transparent meshes (clear eye shells, cutout
    /// lash/brow cards) are skipped — the pass has no alpha, so they'd cast solid-card shadows.
    void recordShadow(VkCommandBuffer cmd, VkPipelineLayout layout,
                      const glm::mat4& lightViewProj) const;

    /// The model's world-space AABB (local bounds through the model transform). Returns false when
    /// the model has no geometry bounds. Used to fit the key light's shadow frustum.
    bool worldBounds(glm::vec3& outMin, glm::vec3& outMax) const;

    void             setTransform(const glm::mat4& transform) { m_transform = transform; }
    const glm::mat4& transform() const { return m_transform; }

    /// Poses the joint @p boneName by an Euler rotation (degrees) applied in its local frame, then
    /// recomputes the skin matrices. No-op if the model has no such bone. This is the primitive a
    /// posing UI drives; @p eulerDegrees of 0 restores the bone's rest pose.
    void setBoneRotation(const std::string& boneName, const glm::vec3& eulerDegrees);

    /// Whether this model has a skeleton (i.e. is a posable figure rather than a static mesh).
    bool hasSkeleton() const { return !m_bones.empty(); }

    // --- Posing-UI support (world-space skeleton for overlay/picking + interactive rotation) ---
    std::size_t      boneCount() const { return m_bones.size(); }
    /// Current world-space position of each joint (updated whenever the pose changes).
    const glm::vec3& boneWorldPosition(std::size_t i) const { return m_boneWorldPos[i]; }
    int              boneParent(std::size_t i) const { return m_bones[i].parent; }
    const std::string& boneName(std::size_t i) const { return m_boneNames[i]; }

    int  selectedBone() const { return m_selectedBone; }
    void setSelectedBone(int index) { m_selectedBone = index; }
    /// Adds @p deltaEulerDegrees to the selected bone's accumulated rotation and re-poses it.
    void nudgeSelectedBone(const glm::vec3& deltaEulerDegrees);

    /// World-space origin + rotation frame of the selected joint, for the rotate gizmo. Returns false
    /// if no joint is selected. @p axes receives the joint's three local rotation-channel axes (the
    /// axes its Euler pose rotates about, X/Y/Z as columns), normalized.
    bool selectedBoneFrame(glm::vec3& center, glm::mat3& axes) const;

    /// Captures the current pose as (bone name, Euler degrees) for each non-rest joint (for saving).
    std::vector<std::pair<std::string, glm::vec3>> capturePose() const;
    /// Resets to bind pose, then applies @p pose (bone name -> Euler degrees); unknown bones ignored.
    void applyPose(const std::vector<std::pair<std::string, glm::vec3>>& pose);

    /// Whether this model carries pose correctives (joint-driven corrective morphs).
    bool hasCorrectives() const { return !m_correctives.empty(); }
    /// Re-evaluates the pose correctives against the current pose and, if any changed, re-morphs and
    /// re-uploads the affected meshes. No-op without correctives. Call after a pose edit settles (the
    /// interactive drag defers to this on release; setBoneRotation/applyPose call it immediately).
    void refreshCorrectives();

    /// Tests @p ray (world space) against the model's axis-aligned bounding box (transformed by the
    /// model matrix). On a hit, writes the entry distance (a world-space ray parameter, so values
    /// are comparable across models) to @p tOut and returns true. Box-level precision — enough to
    /// pick which imported object the cursor is over.
    bool intersectRay(const Ray& ray, float& tOut) const;

private:
    /// Recomputes each bone's skin matrix (poseGlobal · inverseBind) from the current pose, updates
    /// the world-space bone positions, and writes the matrices to the joint storage buffer. Assumes
    /// bones are ordered parent-before-child (figure skeletons are). No skeleton => one identity.
    void computeSkinMatrices();

    /// Re-poses bone @p index from its accumulated Euler (m_boneEuler) in its oriented frame, then
    /// recomputes the skin matrices. Shared by setBoneRotation() and nudgeSelectedBone().
    void applyBoneEuler(int index);

    /// Clamps m_boneEuler[index] in place to the bone's per-axis rotation limits (a no-op on axes the
    /// figure leaves unconstrained). The single enforcement point every posing path funnels through.
    void clampBoneEuler(int index);

    /// Evaluates one corrective's blend weight from the current pose (Σ sumFormulas × gateScale).
    float evalCorrectiveWeight(std::size_t correctiveIndex) const;

    /// Maps each corrective's base-vertex deltas onto the render meshes (via @p perMeshBaseIndex),
    /// records which meshes are affected, and keeps their base vertices for re-morphing. Called once
    /// at construction when the model has correctives.
    void buildRuntimeCorrectives(const ModelData& data,
                                 const std::vector<std::vector<uint32_t>>& perMeshBaseIndex);

    // One pose corrective resolved for this model: its driver formulas (in base-vertex-independent
    // joint-angle terms) plus, per affected render mesh, the local vertices its deltas land on. Built
    // once at construction by mapping the base-vertex deltas through each mesh's baseVertex array.
    struct RuntimeCorrective {
        std::vector<CorrectiveFormula> sumFormulas;
        float                          gateScale = 1.0f;
        struct MeshDelta {
            uint32_t               mesh = 0;      ///< Index into m_meshes / m_baseVertices.
            std::vector<uint32_t>  localVertex;   ///< Render-vertex indices within that mesh.
            std::vector<glm::vec3> delta;         ///< Matching displacement (world units).
        };
        std::vector<MeshDelta> meshDeltas;
    };

    // One runtime skeleton joint. localBind is its rest transform relative to its parent; poseLocal
    // folds in the current animated rotation (== localBind at bind pose). `orient`/`invOrient` frame
    // that rotation in the joint's true orientation so bends are anatomically correct.
    struct GpuBone {
        int         parent = -1;
        glm::mat4   inverseBind{1.0f};
        glm::mat4   localBind{1.0f};
        glm::mat4   poseLocal{1.0f};
        glm::mat4   orient{1.0f};
        glm::mat4   invOrient{1.0f};
        std::string rotationOrder = "XYZ";
        // Per-axis pose-rotation limits (degrees) enforced by clampBoneEuler(); only axes flagged in
        // rotLimited are constrained (the figure's anatomical range of motion). Default: unconstrained.
        glm::vec3   rotMin{0.0f};
        glm::vec3   rotMax{0.0f};
        glm::bvec3  rotLimited{false, false, false};
    };

    VulkanContext&       m_context;
    VkDescriptorPool     m_materialPool = VK_NULL_HANDLE;
    std::vector<Mesh>    m_meshes;
    glm::mat4            m_transform{1.0f};
    glm::vec3            m_boundsMin{0.0f}; // local-space AABB over all mesh vertices
    glm::vec3            m_boundsMax{0.0f};
    bool                 m_hasBounds = false;

    // Skinning: the runtime skeleton + a storage buffer of skin matrices (set 2) bound per draw.
    std::vector<GpuBone>                 m_bones;
    std::unordered_map<std::string, int> m_boneIndex; // bone name -> index into m_bones
    std::vector<std::string>             m_boneNames;  // parallel to m_bones (for the posing UI)
    std::vector<glm::vec3>               m_boneWorldPos; // current world position per bone (overlay/pick)
    std::vector<glm::mat4>               m_boneGizmoFrame; // per bone: parentGlobal·localBind·orient (gizmo axes)
    std::vector<glm::mat4>               m_poseGlobal;  // scratch for computeSkinMatrices (it runs per drag-move; no per-call allocation)
    std::vector<glm::vec3>               m_boneEuler;  // accumulated pose rotation per bone (degrees)
    int                                  m_selectedBone = -1;
    VulkanBuffer                         m_jointBuffer;
    VkDescriptorSet                      m_jointSet = VK_NULL_HANDLE; // set 2; from m_materialPool
    uint32_t                             m_jointCount = 1;

    // Pose correctives: the base (uncorrected) vertices per render mesh, the resolved
    // correctives, and each corrective's last-applied weight. Empty for a model without correctives.
    // The base mesh is re-morphed on the CPU (base + Σ weight·delta) and re-uploaded when the pose
    // changes enough to move a corrective's weight.
    std::vector<std::vector<Vertex>>  m_baseVertices;     // parallel to m_meshes; only affected meshes filled
    std::vector<RuntimeCorrective>    m_correctives;
    std::vector<float>                m_correctiveWeight;  // last-applied weight per corrective
    std::vector<uint32_t>             m_correctiveMeshes;  // affected render-mesh indices (sorted, unique)
};

} // namespace pose

#endif // MESH_H
