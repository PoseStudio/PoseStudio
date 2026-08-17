/**
 * @file shadowmap.cpp
 * @brief Implementation of ShadowMap. See shadowmap.h.
 */

#include "shadowmap.h"

#include "vulkancommands.h"
#include "vulkancommon.h"
#include "vulkancontext.h"

namespace pose {

namespace {
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
} // namespace

ShadowMap::ShadowMap(VulkanContext& context, uint32_t size) : m_context(context), m_size(size) {
    VkDevice device = m_context.device();

    // --- Depth image (sampled after the pass) ---
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kDepthFormat;
    imageInfo.extent = {m_size, m_size, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; // shadow pass is single-sample (PCF does the softening)
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT; // TRANSFER_DST: the one-time clear below
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(m_context.allocator(), &imageInfo, &allocInfo, &m_image, &m_allocation,
                            nullptr));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kDepthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &m_view));

    // Comparison sampler: linear filter + compare = hardware PCF (each tap returns the fraction of
    // the bilinear footprint that passes ref <= texel, i.e. 1 = lit). Border is opaque WHITE so
    // receivers outside the fitted light frustum read fully lit rather than shadow-striped.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &m_sampler));

    // --- Depth-only render pass: clear -> raster depth -> leave in shader-read layout ---
    VkAttachmentDescription depth{};
    depth.format = kDepthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; // contents are cleared; prior layout irrelevant
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    // Explicit dependencies: the previous frame's fragment reads must finish before this pass
    // overwrites the map, and the map's depth writes must finish before this frame samples it.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &depth;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 2;
    rpInfo.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(device, &rpInfo, nullptr, &m_renderPass));

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &m_view;
    fbInfo.width = m_size;
    fbInfo.height = m_size;
    fbInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(device, &fbInfo, nullptr, &m_framebuffer));

    // One-time init: clear to 1.0 ("everything lit") and move to the read layout, so the map is
    // well-defined for any frame that samples it before the first shadow pass runs (or while the
    // pass is skipped because the scene has no casters).
    submitImmediate(m_context, [this](VkCommandBuffer cmd) {
        VkImageMemoryBarrier toClear{};
        toClear.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toClear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toClear.image = m_image;
        toClear.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        toClear.subresourceRange.levelCount = 1;
        toClear.subresourceRange.layerCount = 1;
        toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toClear);

        VkClearDepthStencilValue clear{1.0f, 0};
        VkImageSubresourceRange range = toClear.subresourceRange;
        vkCmdClearDepthStencilImage(cmd, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                    &range);

        VkImageMemoryBarrier toRead = toClear;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toRead);
    });
}

ShadowMap::~ShadowMap() {
    VkDevice device = m_context.device();
    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, m_framebuffer, nullptr);
    }
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, m_renderPass, nullptr);
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_sampler, nullptr);
    }
    if (m_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_view, nullptr);
    }
    if (m_image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_context.allocator(), m_image, m_allocation);
    }
}

VkDescriptorImageInfo ShadowMap::descriptorInfo() const {
    VkDescriptorImageInfo info{};
    info.sampler = m_sampler;
    info.imageView = m_view;
    info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    return info;
}

} // namespace pose
