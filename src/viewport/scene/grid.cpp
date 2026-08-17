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
           const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv,
           VkDescriptorSetLayout sceneSet3Layout) {
    PipelineConfig config;
    config.pushConstantSize = sizeof(GridPushConstants);
    // The fragment shader also reads the matrices (depth + ground shadow), so both stages.
    config.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    config.blendEnable = true;        // lines blend over the floor via their coverage alpha
    config.depthTestEnable = true;    // scene geometry in front occludes the grid
    config.depthWriteEnable = false;  // but the transparent ground plane must not occlude
    config.colorWriteAlpha = false;   // the HDR target's alpha is the SSS mask — leave it alone
    // Set 0 = the scene's set-3 layout: the fragment shader samples only its binding 2 (the key
    // light's shadow map) for the ground/contact shadow under the figure.
    config.descriptorSetLayouts = {sceneSet3Layout};
    m_pipeline = std::make_unique<VulkanPipeline>(context, renderPass, vertSpirv, fragSpirv, config);
}

Grid::~Grid() = default;

void Grid::record(VkCommandBuffer cmd, const Camera& camera, VkDescriptorSet sceneSet3,
                  const glm::mat4& lightViewProj) {
    GridPushConstants push{};
    const glm::mat4 viewProj = camera.viewProjection();
    std::memcpy(push.viewProj, &viewProj[0][0], sizeof(push.viewProj));
    std::memcpy(push.lightViewProj, &lightViewProj[0][0], sizeof(push.lightViewProj));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->layout(), 0, 1,
                            &sceneSet3, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipeline->layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    vkCmdDraw(cmd, 6, 1, 0, 0); // full-screen quad; the grid is reconstructed in the shaders
}

} // namespace pose
