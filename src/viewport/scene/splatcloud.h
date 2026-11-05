/**
 * @file splatcloud.h
 * @brief GAUSSIAN SPLATS on the GPU: a scene's splats as storage buffers, the per-view far-first
 *        draw order sorted on the CPU, and the one instanced-quad draw the splat pipeline makes
 *        of them (shaders/splat.vert + splat.frag; the pipeline and its set layout are
 *        ScenePipelines'). A Model holds one (Model::splats) in place of meshes — a splat scene
 *        is static, unlit and never a figure — and the Scene draws every model's cloud after the
 *        opaque meshes, before the transparent ones.
 *
 * THE DATA: per splat three vec4 (set 1 binding 0) — the centre with its opacity, then the 3D
 * covariance Σ = R S Sᵀ Rᵀ from its rotation and scales (xx xy xz yy | yz zz) in the splat's own
 * frame, carried through the model matrix in the shader — and its spherical harmonics as halves
 * packed in pairs (binding 1; one coefficient-major run of 3, 12, 27 or 48 halves a splat). THE
 * ORDER (binding 2, one host-mapped buffer per frame in flight): a translucent splat is blended
 * over what lies behind it, so the splats are drawn FAR FIRST along the view direction — a
 * counting sort on a 16-bit quantized depth, parallel over chunks (parallelfor.h), re-done only
 * when the view or the model transform moved (an event-driven frame that only posed the figure
 * reuses the last order); 4 million splats sort in a few milliseconds on eight cores. Vertex
 * buffers there are none: gl_VertexIndex / 6 is the sorted slot, % 6 the quad corner.
 *
 * Qt-free (Vulkan + std + GLM).
 */

#ifndef SPLATCLOUD_H
#define SPLATCLOUD_H

#include "vulkanbuffer.h"
#include "vulkancommon.h"
#include "vulkanhandles.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pose {

class Camera;
class VulkanContext;
struct SplatData;

/// The splat pipeline's push block — must stay byte-identical to splat.vert's `PC`.
struct SplatPushConstants {
    glm::mat4 model;
    glm::vec4 focal;  ///< x = fx, y = fy (pixels), z = viewport width, w = height
    glm::vec4 params; ///< x = SH degree, y = harmonics uints per splat, z = inverse-tonemap (0/1), w = orthographic (0/1)
    glm::vec4 camPos; ///< xyz = the eye in the splat's own (model) frame; w = 1 when the scene was trained antialiased (the opacity compensation on)
};
static_assert(sizeof(SplatPushConstants) == 112, "must match splat.vert's push block");

class SplatCloud {
public:
    /// Uploads @p data (one blocking transfer per buffer) and allocates the per-frame order
    /// buffers and descriptor sets from @p setLayout (ScenePipelines::splatSetLayout).
    SplatCloud(VulkanContext& context, const SplatData& data, VkDescriptorSetLayout setLayout);
    ~SplatCloud();

    SplatCloud(const SplatCloud&) = delete;
    SplatCloud& operator=(const SplatCloud&) = delete;

    std::size_t      count() const { return m_count; }
    int              shDegree() const { return m_shDegree; }
    const glm::vec3& boundsMin() const { return m_boundsMin; } ///< The centres' local AABB.
    const glm::vec3& boundsMax() const { return m_boundsMax; }

    /// Sorts the splats far-first for @p camera's view of the cloud at @p model (when either
    /// moved since the last sort) and writes the order into frame slot @p frameIndex's buffer if
    /// that slot is behind. Call before recording the frame (GUI thread; CPU work).
    void prepareFrame(const Camera& camera, const glm::mat4& model, uint32_t frameIndex);

    /// Records the draw: binds this frame slot's set at index 1 (the caller has bound the splat
    /// pipeline and the camera set at 0), pushes the constants (the focal lengths off the
    /// camera's projection and viewport, the eye in the splat frame) and draws six vertices a
    /// splat (into the pass's photo layer: the colours as trained, the composite lays them over).
    void record(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frameIndex, const Camera& camera,
                const glm::mat4& model) const;

private:
    void sortForView(const glm::mat4& view, const glm::mat4& model);

    VulkanContext& m_context;
    std::size_t    m_count = 0;
    int            m_shDegree = 0;
    uint32_t       m_shStride = 2; ///< Harmonics uints per splat.
    bool           m_antialiased = false; ///< Trained with the antialiasing formulation (an .spz flag): the shader compensates the dilation.
    glm::vec3      m_boundsMin{0.0f};
    glm::vec3      m_boundsMax{0.0f};

    // The CPU side of the sort.
    std::vector<glm::vec3> m_positions; ///< Local centres.
    std::vector<glm::vec3> m_world;     ///< ... through the model matrix they were last sorted at.
    glm::mat4              m_worldFor{0.0f};
    std::vector<float>     m_depth;
    std::vector<uint16_t>  m_keys;
    std::vector<uint32_t>  m_order;
    std::vector<uint32_t>  m_hist;
    std::vector<float>     m_chunkMin, m_chunkMax;
    glm::vec4              m_lastRow{0.0f};
    glm::mat4              m_lastModel{0.0f};
    bool                   m_sorted = false;
    std::uint64_t          m_sortVersion = 0;
    std::array<std::uint64_t, kMaxFramesInFlight> m_uploaded{};

    // The GPU side. Declaration order = reverse destruction: the pool after the buffers its sets
    // describe.
    UniqueDescriptorPool                           m_pool;
    VulkanBuffer                                   m_splatBuffer; ///< 3 vec4 a splat, device-local.
    VulkanBuffer                                   m_shBuffer;    ///< The packed halves, device-local.
    std::array<VulkanBuffer, kMaxFramesInFlight>   m_orderBuffers; ///< One host-mapped order a frame slot.
    std::array<VkDescriptorSet, kMaxFramesInFlight> m_sets{};
};

} // namespace pose

#endif // SPLATCLOUD_H
