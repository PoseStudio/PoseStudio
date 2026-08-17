/**
 * @file grid.h
 * @brief The infinite ground-plane grid drawn as a viewport overlay (a construction-plane floor).
 *
 * Self-contained so VulkanRenderer doesn't accumulate per-feature pipeline/push-constant
 * code: Grid owns its own graphics pipeline and knows how to record its draw. The pipeline
 * is alpha-blended and depth-tested but does NOT write depth — it's a transparent overlay,
 * so it must be occluded by scene geometry without itself occluding anything behind the
 * (mostly invisible) ground plane.
 *
 * Like the rest of rendering/scene, this is Qt-free (plain Vulkan + std + GLM).
 */

#ifndef GRID_H
#define GRID_H

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <memory>
#include <vector>

namespace pose {

class VulkanContext;
class VulkanPipeline;
class Camera;

/// Push-constant block for the grid shaders. The vertex shader inverts viewProj to unproject
/// screen rays; the fragment shader uses it to write correct depth, and lightViewProj to sample
/// the key light's shadow map for the ground/contact shadow. Must stay byte-identical to the
/// `push_constant` block in grid.vert / grid.frag (2 × mat4 = 128 bytes, the guaranteed minimum).
struct GridPushConstants {
    float viewProj[16];      // column-major, as produced by glm::mat4
    float lightViewProj[16]; // key light's ortho view-projection (Scene::lightViewProj)
};

/**
 * @class Grid
 * @brief Owns the grid pipeline and records a single full-screen draw that the shaders turn
 *        into an analytic, anti-aliased, distance-faded ground grid.
 */
class Grid {
public:
    /// @param sceneSet3Layout The scene-wide set-3 layout (IBL maps + shadow map at binding 2);
    ///                        the grid's ground shadow samples the shadow map through it.
    Grid(VulkanContext& context, VkRenderPass renderPass,
         const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv,
         VkDescriptorSetLayout sceneSet3Layout);
    ~Grid();

    Grid(const Grid&) = delete;
    Grid& operator=(const Grid&) = delete;

    /// Records the grid draw into @p cmd. Assumes the caller has already begun the render pass
    /// and set the dynamic viewport/scissor (the grid draws within them, after opaque geometry).
    /// @p sceneSet3 is the scene's live set 3 (shadow map at binding 2) and @p lightViewProj the
    /// matching light matrix — together they draw the ground/contact shadow.
    void record(VkCommandBuffer cmd, const Camera& camera, VkDescriptorSet sceneSet3,
                const glm::mat4& lightViewProj);

private:
    std::unique_ptr<VulkanPipeline> m_pipeline;
};

} // namespace pose

#endif // GRID_H
