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
/// screen rays; the fragment shader uses it to write correct depth, and the light rows + params
/// to draw the PCSS ground shadow. The light matrix travels as its first three ROWS only — an
/// ortho·rigid transform's bottom row is always (0,0,0,1), and dropping it frees exactly one vec4
/// for the shadow dials within the 128-byte guaranteed push minimum. Must stay byte-identical to
/// the `push_constant` block in grid.vert / grid.frag (mat4 + 4 × vec4 = 128 bytes).
struct GridPushConstants {
    float viewProj[16];    // column-major, as produced by glm::mat4
    float lightRow0[4];    // lightViewProj math-row 0: clip.x = row0 · (world, 1)
    float lightRow1[4];    // math-row 1
    float lightRow2[4];    // math-row 2 (depth)
    float shadowParams[4]; // x = intensity (0..1), y = softness (0..1), z = reach (world), w unused
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
    /// @p sceneSet3 is the scene's live set 3 (shadow map at bindings 2/3) and @p lightViewProj
    /// the matching light matrix; @p shadowParams carries the Environment panel's shadow dials
    /// (x = intensity, y = softness, z = reach) — together they draw the PCSS ground shadow.
    void record(VkCommandBuffer cmd, const Camera& camera, VkDescriptorSet sceneSet3,
                const glm::mat4& lightViewProj, const glm::vec4& shadowParams);

private:
    std::unique_ptr<VulkanPipeline> m_pipeline;
};

} // namespace pose

#endif // GRID_H
