/**
 * @file shadowpass.cpp
 * @brief The key-light shadow pass. See shadowpass.h.
 */

#include "shadowpass.h"

#include "mesh.h" // ShadowPushConstants
#include "model.h"
#include "shadowmap.h"
#include "vertex.h"
#include "vulkanpipeline.h"

#include <glm/gtc/matrix_transform.hpp> // lookAt / ortho for the shadow frustum

#include <cmath>
#include <limits>

namespace pose {

ShadowPass::ShadowPass(VulkanContext& context, VkDescriptorSetLayout poseSetLayout,
                       const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv)
    : m_shadowMap(std::make_unique<ShadowMap>(context)) {
    // Depth-only shadow pipeline: same skinned vertex layout as the mesh pass, rendered into the
    // shadow map's own single-sample pass with a static depth bias (acne) and no color
    // attachment. Its only descriptor set is the pose layout (bound at index 0 — see
    // Model::recordShadow).
    PipelineConfig shadowConfig;
    shadowConfig.pushConstantSize = sizeof(ShadowPushConstants);
    shadowConfig.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT;
    shadowConfig.singleSample = true;
    shadowConfig.hasColorAttachment = false;
    shadowConfig.depthBiasConstant = 1.25f;
    shadowConfig.depthBiasSlope = 1.9f;
    // No culling: imported winding is inconsistent (same as the main pass) — PipelineConfig's
    // default.
    const VkVertexInputBindingDescription binding = Vertex::bindingDescription();
    const auto attrs = Vertex::attributeDescriptions();
    shadowConfig.vertexBindings.assign(1, binding);
    shadowConfig.vertexAttributes.assign(attrs.begin(), attrs.end());
    shadowConfig.descriptorSetLayouts = {poseSetLayout};
    m_pipeline = std::make_unique<VulkanPipeline>(context, m_shadowMap->renderPass(), vertSpirv,
                                                  fragSpirv, shadowConfig);
}

ShadowPass::~ShadowPass() = default;

bool ShadowPass::fit(const std::vector<std::unique_ptr<Model>>& models, const glm::vec3& keyDir,
                     bool enabled) {
    // Shadows off: skip the whole pass (its cost included) and park the light matrix on the same
    // degenerate projection the no-casters case uses — every receiver (figure + floor) reads
    // fully lit through it.
    if (!enabled) {
        m_lightViewProj = glm::mat4(0.0f);
        m_lightViewProj[3][3] = 1.0f;
        return false;
    }

    // Fit the light's ortho frustum around every caster AND its shadow's landing spot on the floor
    // (casting each AABB corner along the light onto y=0) — the ground shadow is only correct where
    // the RECEIVER is inside the frustum, so the floor patch must be covered too.
    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(std::numeric_limits<float>::lowest());
    bool any = false;
    const glm::vec3 dir = keyDir; // TO the light; a shadow ray travels along -dir
    for (const std::unique_ptr<Model>& model : models) {
        glm::vec3 a;
        glm::vec3 b;
        if (!model->worldBounds(a, b)) {
            continue;
        }
        any = true;
        mn = glm::min(mn, a);
        mx = glm::max(mx, b);
        if (dir.y > 0.05f) {
            for (int i = 0; i < 8; ++i) {
                const glm::vec3 c((i & 1) ? b.x : a.x, (i & 2) ? b.y : a.y, (i & 4) ? b.z : a.z);
                const glm::vec3 onFloor = c - dir * (c.y / dir.y); // where c's shadow lands on y=0
                mn = glm::min(mn, onFloor);
                mx = glm::max(mx, onFloor);
            }
        }
    }
    if (!any) {
        // No casters: skip the pass (the map was cleared fully-lit at creation) and park the light
        // matrix on a projection that maps every receiver to depth 0 — the LESS_OR_EQUAL compare
        // then always passes, i.e. everything reads unshadowed.
        m_lightViewProj = glm::mat4(0.0f);
        m_lightViewProj[3][3] = 1.0f;
        return false;
    }

    // Ortho frustum around the fitted bounding sphere (padded ~10%: bind-pose bounds understate a
    // posed figure's reach a little).
    const glm::vec3 center = 0.5f * (mn + mx);
    const float radius = 0.55f * glm::length(mx - mn) + 0.05f;
    const glm::vec3 eye = center + dir * (radius + 0.25f);
    const glm::vec3 up =
        (std::abs(dir.y) > 0.95f) ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 view = glm::lookAt(eye, center, up);
    // GLM_FORCE_DEPTH_ZERO_TO_ONE puts z in [0,1]. No Vulkan Y-negate here: the map is rendered
    // and sampled through this same matrix, so the flip would cancel anyway.
    const glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius, 0.01f, 2.0f * radius + 0.5f);
    m_lightViewProj = proj * view;
    return true;
}

void ShadowPass::record(VkCommandBuffer cmd, const std::vector<std::unique_ptr<Model>>& models,
                        uint32_t frameIndex) {
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_shadowMap->renderPass();
    rp.framebuffer = m_shadowMap->framebuffer();
    rp.renderArea.extent = {m_shadowMap->size(), m_shadowMap->size()};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(m_shadowMap->size());
    viewport.height = static_cast<float>(m_shadowMap->size());
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {m_shadowMap->size(), m_shadowMap->size()};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());
    for (const std::unique_ptr<Model>& model : models) {
        model->recordShadow(cmd, m_pipeline->layout(), m_lightViewProj, frameIndex);
    }
    vkCmdEndRenderPass(cmd);
}

} // namespace pose
