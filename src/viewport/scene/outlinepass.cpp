/**
 * @file outlinepass.cpp
 * @brief Implementation of OutlinePass. See outlinepass.h.
 */

#include "outlinepass.h"

#include "camera.h"
#include "mesh.h" // ShadowPushConstants
#include "model.h"
#include "outlinemask.h"
#include "splatcloud.h"
#include "vertex.h"
#include "vulkanpipeline.h"

#include <array>

namespace pose {

OutlinePass::OutlinePass(VulkanContext& context, VkRenderPass maskPass,
                         VkDescriptorSetLayout poseSetLayout, const std::vector<char>& vertSpirv,
                         const std::vector<char>& fragSpirv, VkDescriptorSetLayout cameraSetLayout,
                         VkDescriptorSetLayout splatSetLayout, const std::vector<char>& splatVertSpirv,
                         const std::vector<char>& splatMaskFragSpirv) {
    // Selection-outline mask: the shadow pass's skinned position-only vertex shader again, now
    // projected by the camera into the OutlineMask's coverage pass (its MSAA count matches the
    // context's, so the silhouette edge resolves to fractional coverage), writing constant 1.0.
    // No depth attachment in that pass — the mask is the object's whole projected silhouette.
    PipelineConfig outlineConfig;
    outlineConfig.pushConstantSize = sizeof(ShadowPushConstants);
    outlineConfig.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT;
    outlineConfig.depthTestEnable = false;
    outlineConfig.depthWriteEnable = false;
    outlineConfig.blendEnable = false;
    const VkVertexInputBindingDescription binding = Vertex::bindingDescription();
    const auto attrs = Vertex::attributeDescriptions();
    outlineConfig.vertexBindings.assign(1, binding);
    outlineConfig.vertexAttributes.assign(attrs.begin(), attrs.end());
    outlineConfig.descriptorSetLayouts = {poseSetLayout};
    m_pipeline = std::make_unique<VulkanPipeline>(context, maskPass, vertSpirv, fragSpirv,
                                                  outlineConfig);

    // A splat scene's silhouette: the splat pipeline's vertex stage over its own two sets, the
    // quads blended into the mask by their opacity (splatmask.frag: the mask ends as the
    // capture's total opacity, whose 50% contour the composite outlines) — no depth, the mask's
    // MSAA.
    PipelineConfig splatConfig;
    splatConfig.pushConstantSize = sizeof(SplatPushConstants);
    splatConfig.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT;
    splatConfig.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    splatConfig.depthTestEnable = false;
    splatConfig.depthWriteEnable = false;
    splatConfig.blendEnable = true;
    splatConfig.descriptorSetLayouts = {cameraSetLayout, splatSetLayout};
    m_splatPipeline = std::make_unique<VulkanPipeline>(context, maskPass, splatVertSpirv, splatMaskFragSpirv,
                                                       splatConfig);
}

OutlinePass::~OutlinePass() = default;

bool OutlinePass::record(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex,
                         const OutlineMask& mask, Model& selected, VkDescriptorSet cameraSet,
                         uint32_t orderSlot, bool asyncSort) {
    // Clear to 0 ("not the selected object"); with MSAA the second slot is the resolve target,
    // whose load is DONT_CARE (the clear value is simply unused).
    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clears[1].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    const VkExtent2D extent = mask.extent();
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = mask.renderPass();
    rp.framebuffer = mask.framebuffer();
    rp.renderArea.extent = extent;
    rp.clearValueCount = mask.attachmentCount();
    rp.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (SplatCloud* cloud = selected.splats()) {
        // The cloud's quads through the camera UBO (set 0; this frame's values — the scene pass
        // writes the slot before the GPU executes either pass) in this view's far-first order.
        // The order is the photo pass's own (the same slot, sorted here if stale, so the pass
        // after finds it done); the mask needs no order, but the buffer must hold one.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_splatPipeline->handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_splatPipeline->layout(), 0, 1, &cameraSet, 0,
                                nullptr);
        cloud->prepareFrame(camera, selected.transform(), orderSlot, asyncSort);
        cloud->record(cmd, m_splatPipeline->layout(), orderSlot, camera, selected.transform());
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());
        selected.recordSilhouette(cmd, m_pipeline->layout(), camera.viewProjection(), frameIndex);
    }
    vkCmdEndRenderPass(cmd);
    return true;
}

} // namespace pose
