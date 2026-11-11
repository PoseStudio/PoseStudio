/**
 * @file scenepipelines.h
 * @brief The scene's shared GPU plumbing: the four descriptor set layouts, the per-frame camera
 *        UBO sets, the scene-wide IBL/shadow set, the 1x1 fallback textures, and the main-pass
 *        mesh pipelines (opaque, transparent, the transmission pair, wire, hidden-line fill, backdrop).
 *
 * Everything here is created once when the Scene is built and shared by every model and pass:
 * the layouts define the descriptor contract of the mesh shaders (set 0 = camera, 1 = material
 * textures, 2 = per-model pose data, 3 = IBL maps + shadow map), the pipelines are the variants
 * of that one shader pair the shade modes select between, and the fallbacks let one pipeline
 * serve textured and untextured meshes alike. Every Vulkan handle is held by an RAII owner
 * (vulkanhandles.h / the buffer, texture and pipeline classes), so a constructor step that throws
 * — a missing shader, a failed allocation — can't leak what was created before it; members are
 * torn down in reverse declaration order and the Scene needs no hand-written destroy sequence.
 * Qt-free (Vulkan + std).
 */

#ifndef SCENEPIPELINES_H
#define SCENEPIPELINES_H

#include "vulkanbuffer.h"
#include "vulkanhandles.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace pose {

class IblMaps;
class ShadowMap;
class VulkanContext;
class VulkanPipeline;
class VulkanTexture;

class ScenePipelines {
public:
    /// Creates the layouts, pools, camera UBOs + sets, the IBL set (its bindings are written
    /// later by writeShadowMap / writeEnvironment), the fallback textures, and the main-pass
    /// pipelines against @p renderPass from the mesh (@p vertSpirv/@p fragSpirv) and backdrop
    /// shaders. The wire pipeline is only built on a device with fillModeNonSolid (wire() is
    /// null otherwise).
    /// @p photoRenderPass is the HDR target's photo pass (single-sample), which the Gaussian
    /// splat pipeline alone builds against.
    ScenePipelines(VulkanContext& context, VkRenderPass renderPass, VkRenderPass photoRenderPass,
                   const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv,
                   const std::vector<char>& backgroundVertSpirv,
                   const std::vector<char>& backgroundFragSpirv,
                   const std::vector<char>& splatVertSpirv, const std::vector<char>& splatFragSpirv);
    ~ScenePipelines();

    ScenePipelines(const ScenePipelines&) = delete;
    ScenePipelines& operator=(const ScenePipelines&) = delete;

    // --- Descriptor set layouts (the mesh shaders' contract) ---
    VkDescriptorSetLayout cameraSetLayout() const { return m_cameraSetLayout.get(); }
    VkDescriptorSetLayout materialSetLayout() const { return m_materialSetLayout.get(); }
    VkDescriptorSetLayout poseSetLayout() const { return m_poseSetLayout.get(); }
    VkDescriptorSetLayout iblSetLayout() const { return m_iblSetLayout.get(); }
    VkDescriptorSetLayout splatSetLayout() const { return m_splatSetLayout.get(); } ///< The splat pipeline's set 1 (splatcloud.h).

    // --- Per-frame camera/lighting UBO (set 0, binding 0): one buffer + set per frame in flight ---
    VkDescriptorSet cameraSet(uint32_t frameIndex) const { return m_cameraSets[frameIndex]; }
    /// The persistently mapped UBO memory of frame slot @p frameIndex (write a CameraUbo into it).
    void* cameraUboData(uint32_t frameIndex) const { return m_cameraBuffers[frameIndex].mappedData(); }

    // --- The scene-wide set 3: IBL maps + the shadow map ---
    VkDescriptorSet iblSet() const { return m_iblSet; }
    /// Writes bindings 2 + 3 (the shadow map through its comparison and raw-depth samplers).
    /// Called once, when the shadow pass exists — the image is stable, its contents re-render.
    void writeShadowMap(const ShadowMap& shadowMap);
    /// Writes bindings 0 + 1 (the prefiltered specular cubemap + BRDF LUT) — every environment
    /// swap. The caller has made the device idle (the old maps may still be sampled).
    void writeEnvironment(const IblMaps& maps);

    // --- The 1x1 fallbacks a mesh binds where it has no map ---
    const VulkanTexture& fallbackDiffuse() const { return *m_fallbackDiffuse; } // opaque white
    const VulkanTexture& fallbackNormal() const { return *m_fallbackNormal; }   // flat normal, linear

    // --- Main-pass pipelines (all built against the one mesh shader pair + layout set, except
    // the backdrop) ---
    const VulkanPipeline& opaque() const { return *m_opaque; }          // depth write on
    const VulkanPipeline& transparent() const { return *m_transparent; } // alpha-blended, depth write off
    // TRANSMISSION (2026-11-10): a transmissive zone is drawn twice in the transparent pass —
    // the frame behind it MULTIPLIED by what it lets through, then its own light ADDED.
    const VulkanPipeline& transmitMultiply() const { return *m_transmitMultiply; }
    const VulkanPipeline& transmitAdd() const { return *m_transmitAdd; }
    const VulkanPipeline* wire() const { return m_wire.get(); }          // triangle EDGES; null: device can't
    const VulkanPipeline& hiddenLine() const { return *m_hiddenLine; }   // depth-only surface fill
    const VulkanPipeline& background() const { return *m_background; }   // HDRI backdrop (PBR mode)
    const VulkanPipeline& splat() const { return *m_splat; }             // Gaussian splats, far-first alpha-blended (splatcloud.h)

private:
    VkDevice m_device = VK_NULL_HANDLE;

    // Declaration order = reverse destruction order: layouts outlive the pools and pipelines
    // built against them, pools outlive the buffers their sets describe.
    UniqueDescriptorSetLayout m_cameraSetLayout;
    // Set 1: six combined image samplers read in the fragment stage — binding 0 = diffuse,
    // 1 = detail (normal/bump) map, 2 = roughness map, 3 = spec-mask map, 4 = translucency map,
    // 5 = micro-detail (pore) normal map (2/3 are per-texel multipliers on the material scalars,
    // 4 the transmitted tint/strength, 5 a tiled linear normal; absent maps bind the fallbacks).
    // The layout is shared; each Model owns the pool/sets for its meshes.
    UniqueDescriptorSetLayout m_materialSetLayout;
    // Set 2: the per-model POSE buffers, all read in the vertex stage — binding 0 = the skinning
    // dual quaternions, 1 = this frame's pose-corrective weights, 2 = the model's per-vertex
    // corrective delta entries (static). Bindings 1/2 let the vertex shaders blend the
    // correctives live; a model without correctives binds tiny zero buffers. Each Model owns its
    // buffers + one set per frame in flight.
    UniqueDescriptorSetLayout m_poseSetLayout;
    // Set 3: the scene-wide maps — binding 0 = prefiltered specular cubemap, 1 = BRDF LUT (both
    // refilled per environment), 2 = the key light's shadow map (comparison sampler), 3 = the
    // SAME shadow map through the non-comparison sampler (raw depths for the ground shadow's
    // PCSS blocker search). All fragment-stage; one static set for the whole scene.
    UniqueDescriptorSetLayout m_iblSetLayout;
    // The Gaussian splat pipeline's set 1: the splats, their harmonics and this frame's draw order
    // — three storage buffers read in the vertex stage; each SplatCloud owns its sets.
    UniqueDescriptorSetLayout m_splatSetLayout;
    UniqueDescriptorPool      m_cameraPool;
    UniqueDescriptorPool      m_iblPool;
    std::vector<VulkanBuffer>    m_cameraBuffers;
    std::vector<VkDescriptorSet> m_cameraSets; // freed with m_cameraPool
    VkDescriptorSet              m_iblSet = VK_NULL_HANDLE; // freed with m_iblPool

    std::unique_ptr<VulkanTexture> m_fallbackDiffuse; // 1x1 opaque white
    std::unique_ptr<VulkanTexture> m_fallbackNormal;  // 1x1 flat normal (128,128,255), linear

    std::unique_ptr<VulkanPipeline> m_opaque;
    std::unique_ptr<VulkanPipeline> m_transparent;
    std::unique_ptr<VulkanPipeline> m_transmitMultiply;
    std::unique_ptr<VulkanPipeline> m_transmitAdd;
    std::unique_ptr<VulkanPipeline> m_wire;
    std::unique_ptr<VulkanPipeline> m_hiddenLine;
    std::unique_ptr<VulkanPipeline> m_splat;
    std::unique_ptr<VulkanPipeline> m_background;
};

} // namespace pose

#endif // SCENEPIPELINES_H
