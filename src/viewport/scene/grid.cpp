/**
 * @file grid.cpp
 * @brief Implementation of the viewport floor grid. See grid.h.
 */

#include "grid.h"

#include "camera.h"
#include "vulkancontext.h"
#include "vulkanpipeline.h"

#include <cstring>

namespace pose {

Grid::Grid(VulkanContext& context, VkRenderPass renderPass,
           const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv) {
    PipelineConfig config;
    config.pushConstantSize = sizeof(GridPushConstants);
    // The fragment shader also reads the matrix (to write correct depth), so both stages.
    config.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    config.blendEnable = true;        // lines blend over the floor via their coverage alpha
    config.depthTestEnable = true;    // scene geometry in front occludes the grid
    config.depthWriteEnable = false;  // but the transparent ground plane must not occlude
    m_pipeline = std::make_unique<VulkanPipeline>(context, renderPass, vertSpirv, fragSpirv, config);
}

Grid::~Grid() = default;

void Grid::record(VkCommandBuffer cmd, const Camera& camera) {
    GridPushConstants push{};
    const glm::mat4 viewProj = camera.viewProjection();
    std::memcpy(push.viewProj, &viewProj[0][0], sizeof(push.viewProj));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());
    vkCmdPushConstants(cmd, m_pipeline->layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    vkCmdDraw(cmd, 6, 1, 0, 0); // full-screen quad; the grid is reconstructed in the shaders
}

} // namespace pose
