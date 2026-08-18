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
    config.colorAttachmentCount = 2;  // the pass's spec target is present but masked off for the grid
    // Set 0 = the scene's set-3 layout: the fragment shader samples only its binding 2 (the key
    // light's shadow map) for the ground/contact shadow under the figure.
    config.descriptorSetLayouts = {sceneSet3Layout};
    m_pipeline = std::make_unique<VulkanPipeline>(context, renderPass, vertSpirv, fragSpirv, config);
}

Grid::~Grid() = default;

void Grid::record(VkCommandBuffer cmd, const Camera& camera, VkDescriptorSet sceneSet3,
                  const glm::mat4& lightViewProj, const glm::vec4& shadowParams) {
    GridPushConstants push{};
    const glm::mat4 viewProj = camera.viewProjection();
    std::memcpy(push.viewProj, &viewProj[0][0], sizeof(push.viewProj));
    // The light matrix travels as its first three math rows (see GridPushConstants): glm stores
    // column-major, so math-row i is (m[0][i], m[1][i], m[2][i], m[3][i]).
    for (int c = 0; c < 4; ++c) {
        push.lightRow0[c] = lightViewProj[c][0];
        push.lightRow1[c] = lightViewProj[c][1];
        push.lightRow2[c] = lightViewProj[c][2];
    }
    std::memcpy(push.shadowParams, &shadowParams[0], sizeof(push.shadowParams));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->layout(), 0, 1,
                            &sceneSet3, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipeline->layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    vkCmdDraw(cmd, 6, 1, 0, 0); // full-screen quad; the grid is reconstructed in the shaders
}

} // namespace pose
