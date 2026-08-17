/**
 * @file hdrtarget.cpp
 * @brief Implementation of HdrTarget. See hdrtarget.h.
 */

#include "hdrtarget.h"

#include "vulkancommon.h"
#include "vulkancontext.h"

#include <array>

namespace pose {

namespace {
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
} // namespace

HdrTarget::HdrTarget(VulkanContext& context, VkExtent2D extent)
    : m_context(context), m_extent(extent) {
    VkDevice device = m_context.device();
    const VkSampleCountFlagBits samples = m_context.sampleCount();
    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;

    // Render pass: mirrors the swapchain's (colour(0)/depth(1)/resolve(2) when MSAA; clean
    // single-sample fallback without the resolve), but in RGBA16F and ending SHADER_READ_ONLY so
    // the post-processing passes can sample the result.
    std::array<VkAttachmentDescription, 3> attachments{};
    attachments[0].format = kColorFormat;
    attachments[0].samples = samples;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                      : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[1].format = kDepthFormat;
    attachments[1].samples = samples;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[2].format = kColorFormat; // resolve (single-sample), only used when MSAA
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    subpass.pResolveAttachments = msaa ? &resolveRef : nullptr;

    // The resolved image is sampled by the post passes: order this pass's writes before later
    // fragment reads (and after any earlier ones — the previous frame's composite).
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = msaa ? 3u : 2u;
    rpInfo.pAttachments = attachments.data();
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
    rpInfo.pDependencies = deps.data();
    VK_CHECK(vkCreateRenderPass(device, &rpInfo, nullptr, &m_renderPass));

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &m_sampler));

    createImages();
}

HdrTarget::~HdrTarget() {
    destroyImages();
    VkDevice device = m_context.device();
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_sampler, nullptr);
    }
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, m_renderPass, nullptr);
    }
}

void HdrTarget::resize(VkExtent2D extent) {
    m_extent = extent;
    destroyImages();
    createImages();
}

void HdrTarget::createImages() {
    VkDevice device = m_context.device();
    const VkSampleCountFlagBits samples = m_context.sampleCount();
    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;

    const auto makeImage = [&](VkFormat format, VkSampleCountFlagBits sampleCount,
                               VkImageUsageFlags usage, VkImage& image, VmaAllocation& alloc,
                               VkImageView& view, VkImageAspectFlags aspect) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {m_extent.width, m_extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = sampleCount;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VK_CHECK(vmaCreateImage(m_context.allocator(), &info, &allocInfo, &image, &alloc, nullptr));

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = aspect;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &view));
    };

    // MSAA colour: transient when possible (tile-local on many GPUs); when single-sample it IS the
    // sampleable result.
    makeImage(kColorFormat, samples,
              msaa ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
                   : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
              m_colorImage, m_colorAlloc, m_colorView, VK_IMAGE_ASPECT_COLOR_BIT);
    makeImage(kDepthFormat, samples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, m_depthImage,
              m_depthAlloc, m_depthView, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (msaa) {
        makeImage(kColorFormat, VK_SAMPLE_COUNT_1_BIT,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_resolveImage,
                  m_resolveAlloc, m_resolveView, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    const VkImageView views[3] = {m_colorView, m_depthView, m_resolveView};
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_renderPass;
    fbInfo.attachmentCount = msaa ? 3u : 2u;
    fbInfo.pAttachments = views;
    fbInfo.width = m_extent.width;
    fbInfo.height = m_extent.height;
    fbInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(device, &fbInfo, nullptr, &m_framebuffer));
}

void HdrTarget::destroyImages() {
    VkDevice device = m_context.device();
    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
    const auto destroy = [&](VkImage& image, VmaAllocation& alloc, VkImageView& view) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
        if (image != VK_NULL_HANDLE) {
            vmaDestroyImage(m_context.allocator(), image, alloc);
            image = VK_NULL_HANDLE;
            alloc = VK_NULL_HANDLE;
        }
    };
    destroy(m_resolveImage, m_resolveAlloc, m_resolveView);
    destroy(m_depthImage, m_depthAlloc, m_depthView);
    destroy(m_colorImage, m_colorAlloc, m_colorView);
}

VkDescriptorImageInfo HdrTarget::resolveInfo() const {
    VkDescriptorImageInfo info{};
    info.sampler = m_sampler;
    // Single-sample fallback: the colour image itself is the sampleable result.
    info.imageView = (m_resolveView != VK_NULL_HANDLE) ? m_resolveView : m_colorView;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return info;
}

} // namespace pose
