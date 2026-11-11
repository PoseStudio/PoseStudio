/**
 * @file renderpassbuilder.cpp
 * @brief Implementation of RenderPassBuilder and the dependency helpers. See renderpassbuilder.h.
 */

#include "renderpassbuilder.h"

#include "vulkancommon.h"

namespace pose {

namespace {

VkAttachmentDescription describe(VkFormat format, VkSampleCountFlagBits samples,
                                 VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp,
                                 VkImageLayout finalLayout) {
    VkAttachmentDescription a{};
    a.format = format;
    a.samples = samples;
    a.loadOp = loadOp;
    a.storeOp = storeOp;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = finalLayout;
    return a;
}

} // namespace

RenderPassBuilder& RenderPassBuilder::color(VkFormat format, VkSampleCountFlagBits samples,
                                            VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp,
                                            VkImageLayout finalLayout) {
    const uint32_t index = attachmentCount();
    m_attachments.push_back(describe(format, samples, loadOp, storeOp, finalLayout));
    m_colorRefs.push_back({index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    m_resolveRefs.push_back({VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    return *this;
}

RenderPassBuilder& RenderPassBuilder::resolve(VkFormat format, VkImageLayout finalLayout) {
    if (m_colorRefs.empty()) {
        throw VulkanError("RenderPassBuilder::resolve() needs a preceding color attachment.");
    }
    const uint32_t index = attachmentCount();
    m_attachments.push_back(describe(format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                     VK_ATTACHMENT_STORE_OP_STORE, finalLayout));
    m_resolveRefs.back() = {index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    m_hasResolve = true;
    return *this;
}

RenderPassBuilder& RenderPassBuilder::depth(VkFormat format, VkSampleCountFlagBits samples,
                                            VkAttachmentLoadOp loadOp, VkAttachmentStoreOp storeOp,
                                            VkImageLayout finalLayout) {
    if (m_hasDepth) {
        throw VulkanError("RenderPassBuilder::depth() called twice.");
    }
    const uint32_t index = attachmentCount();
    m_attachments.push_back(describe(format, samples, loadOp, storeOp, finalLayout));
    m_depthRef = {index, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    m_hasDepth = true;
    return *this;
}

RenderPassBuilder& RenderPassBuilder::depthReadOnly(VkFormat format, VkSampleCountFlagBits samples,
                                                    VkImageLayout layout) {
    if (m_hasDepth) {
        throw VulkanError("RenderPassBuilder::depth() called twice.");
    }
    const uint32_t index = attachmentCount();
    VkAttachmentDescription a = describe(format, samples, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, layout);
    a.initialLayout = layout;
    m_attachments.push_back(a);
    m_depthRef = {index, layout};
    m_hasDepth = true;
    return *this;
}

RenderPassBuilder& RenderPassBuilder::dependency(const VkSubpassDependency& dep) {
    m_dependencies.push_back(dep);
    return *this;
}

RenderPassBuilder& RenderPassBuilder::dependencies(const std::array<VkSubpassDependency, 2>& deps) {
    m_dependencies.push_back(deps[0]);
    m_dependencies.push_back(deps[1]);
    return *this;
}

UniqueRenderPass RenderPassBuilder::build(VkDevice device) const {
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(m_colorRefs.size());
    subpass.pColorAttachments = m_colorRefs.empty() ? nullptr : m_colorRefs.data();
    // pResolveAttachments must be either null or one entry per color attachment (UNUSED where a
    // color attachment has no resolve).
    subpass.pResolveAttachments = m_hasResolve ? m_resolveRefs.data() : nullptr;
    subpass.pDepthStencilAttachment = m_hasDepth ? &m_depthRef : nullptr;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = attachmentCount();
    info.pAttachments = m_attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<uint32_t>(m_dependencies.size());
    info.pDependencies = m_dependencies.empty() ? nullptr : m_dependencies.data();

    VkRenderPass pass = VK_NULL_HANDLE;
    VK_CHECK(vkCreateRenderPass(device, &info, nullptr, &pass));
    return UniqueRenderPass(device, pass);
}

std::array<VkSubpassDependency, 2> sampledTargetDependencies(bool withDepth) {
    const VkPipelineStageFlags depthStages = withDepth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
                                                       : 0u;
    const VkAccessFlags depthWrite = withDepth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0u;
    const VkAccessFlags depthReadWrite =
        withDepth ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
                  : 0u;

    std::array<VkSubpassDependency, 2> deps{};
    // (0) Before this pass: the previous frame's fragment-shader reads of the target (and its
    // depth writes) must be done and available before the clear/first write.
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | depthStages;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | depthWrite;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | depthStages;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | depthReadWrite;
    // (1) After this pass: its color (and depth) writes must be visible to the fragment reads of
    // the passes that sample it.
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | depthStages;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | depthWrite;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    return deps;
}

std::array<VkSubpassDependency, 2> sampledDepthTargetDependencies() {
    constexpr VkPipelineStageFlags kDepthStages =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = kDepthStages;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = kDepthStages;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    return deps;
}

UniqueFramebuffer createFramebuffer(VkDevice device, VkRenderPass renderPass, const VkImageView* views,
                                    uint32_t viewCount, VkExtent2D extent) {
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = renderPass;
    info.attachmentCount = viewCount;
    info.pAttachments = views;
    info.width = extent.width;
    info.height = extent.height;
    info.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFramebuffer(device, &info, nullptr, &fb));
    return UniqueFramebuffer(device, fb);
}

} // namespace pose
