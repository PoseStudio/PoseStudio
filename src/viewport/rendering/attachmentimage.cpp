/**
 * @file attachmentimage.cpp
 * @brief Implementation of AttachmentImage and the barrier helpers. See attachmentimage.h.
 */

#include "attachmentimage.h"

#include "vulkancommands.h"
#include "vulkancommon.h"
#include "vulkancontext.h"

#include <utility>

namespace pose {

AttachmentImage::AttachmentImage(VulkanContext& context, VkFormat format, VkExtent2D extent,
                                 VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                                 VkImageAspectFlags aspect)
    : m_allocator(context.allocator()) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) {
        // A transient attachment is never stored to memory (storeOp DONT_CARE, resolved or
        // discarded within its pass). Tile-based GPUs can then keep it entirely on-chip — but
        // only if the allocation asks for LAZILY_ALLOCATED memory; without the preference a
        // 4x RGBA16F MSAA target costs its full size in VRAM there. Desktop drivers expose no
        // such heap, so VMA silently falls back to device-local memory: a no-op on Windows/Linux.
        allocInfo.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    }
    VK_CHECK(vmaCreateImage(m_allocator, &info, &allocInfo, &m_image, &m_allocation, nullptr));

    // The view is created second; if it throws, the destructor of this half-built object never
    // runs — so free the image ourselves before propagating.
    try {
        m_view = createImageView(context.device(), m_image, VK_IMAGE_VIEW_TYPE_2D, format, aspect);
    } catch (...) {
        reset();
        throw;
    }
}

AttachmentImage::~AttachmentImage() { reset(); }

void AttachmentImage::reset() {
    m_view.reset();
    if (m_image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_image, m_allocation);
    }
    m_image = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
}

AttachmentImage::AttachmentImage(AttachmentImage&& other) noexcept
    : m_allocator(other.m_allocator), m_image(other.m_image), m_allocation(other.m_allocation),
      m_view(std::move(other.m_view)) {
    other.m_image = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
}

AttachmentImage& AttachmentImage::operator=(AttachmentImage&& other) noexcept {
    if (this != &other) {
        reset();
        m_allocator = other.m_allocator;
        m_image = other.m_image;
        m_allocation = other.m_allocation;
        m_view = std::move(other.m_view);
        other.m_image = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE;
    }
    return *this;
}

UniqueImageView createImageView(VkDevice device, VkImage image, VkImageViewType viewType,
                                VkFormat format, VkImageAspectFlags aspect, uint32_t mipLevels,
                                uint32_t layers) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = viewType;
    info.format = format;
    info.subresourceRange = wholeImageRange(aspect, mipLevels, layers);
    VkImageView view = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(device, &info, nullptr, &view));
    return UniqueImageView(device, view);
}

VkImageMemoryBarrier imageBarrier(VkImage image, const VkImageSubresourceRange& range,
                                  VkImageLayout oldLayout, VkImageLayout newLayout,
                                  VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    return barrier;
}

void transitionImage(VkCommandBuffer cmd, VkImage image, const VkImageSubresourceRange& range,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                     VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    const VkImageMemoryBarrier barrier =
        imageBarrier(image, range, oldLayout, newLayout, srcAccess, dstAccess);
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void initialiseToShaderReadCleared(VulkanContext& context, std::initializer_list<VkImage> images) {
    submitImmediate(context, [&](VkCommandBuffer cmd) {
        const VkImageSubresourceRange range = wholeImageRange(VK_IMAGE_ASPECT_COLOR_BIT);
        const VkClearColorValue       zero{};
        for (VkImage image : images) {
            transitionImage(cmd, image, range, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            transitionImage(cmd, image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_SHADER_READ_BIT);
        }
    });
}

void initialiseDepthReadOnly(VulkanContext& context, VkImage image) {
    submitImmediate(context, [&](VkCommandBuffer cmd) {
        transitionImage(cmd, image, wholeImageRange(VK_IMAGE_ASPECT_DEPTH_BIT), VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    });
}

void initialiseToShaderRead(VulkanContext& context, std::initializer_list<VkImage> images) {
    submitImmediate(context, [&](VkCommandBuffer cmd) {
        for (VkImage image : images) {
            transitionImage(cmd, image, wholeImageRange(VK_IMAGE_ASPECT_COLOR_BIT),
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
    });
}

} // namespace pose
