/**
 * @file vulkanimage.cpp
 * @brief Implementation of VulkanTexture. See vulkanimage.h.
 */

#include "vulkanimage.h"

#include "vulkanbuffer.h"
#include "vulkancommands.h"
#include "vulkancommon.h"
#include "vulkancontext.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pose {

namespace {

constexpr VkFormat kTextureFormat = VK_FORMAT_R8G8B8A8_SRGB;

// Inserts an image-memory barrier for a single mip level, moving it between layouts.
void transitionMip(VkCommandBuffer cmd, VkImage image, uint32_t mip, VkImageLayout oldLayout,
                   VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                   VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = mip;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

VulkanTexture::VulkanTexture(VulkanContext& context, const uint8_t* pixels, uint32_t width,
                             uint32_t height)
    : m_context(&context), m_allocator(context.allocator()) {
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

    // Mipmaps need linear blit support for this format; fall back to a single level if absent.
    VkFormatProperties formatProps{};
    vkGetPhysicalDeviceFormatProperties(context.physicalDevice(), kTextureFormat, &formatProps);
    const bool canMip = (formatProps.optimalTilingFeatures &
                         VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0 &&
                        (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0 &&
                        (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
    m_mipLevels = canMip
                      ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1
                      : 1;

    // Staging buffer with the pixel bytes.
    VulkanBuffer staging(context, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT);
    std::memcpy(staging.mappedData(), pixels, static_cast<size_t>(imageSize));

    // Device-local image. TRANSFER_SRC is needed because mip generation blits from each level.
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kTextureFormat;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = m_mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(m_allocator, &imageInfo, &allocInfo, &m_image, &m_allocation, nullptr));

    submitImmediate(context, [&](VkCommandBuffer cmd) {
        // mip 0: UNDEFINED -> TRANSFER_DST, copy pixels in.
        transitionMip(cmd, m_image, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = 0;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cmd, staging.handle(), m_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        // Generate the mip chain by successively halving + blitting from the previous level.
        int32_t mipW = static_cast<int32_t>(width);
        int32_t mipH = static_cast<int32_t>(height);
        for (uint32_t i = 1; i < m_mipLevels; ++i) {
            // Previous level: TRANSFER_DST -> TRANSFER_SRC (it becomes the blit source).
            transitionMip(cmd, m_image, i - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
            // This level was created as TRANSFER_DST (initial layout for all levels via the
            // first barrier? No — only level 0 was transitioned. Bring level i in now.)
            transitionMip(cmd, m_image, i, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = i - 1;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[1] = {mipW, mipH, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = i;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[1] = {std::max(mipW / 2, 1), std::max(mipH / 2, 1), 1};
            vkCmdBlitImage(cmd, m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

            // Previous level is finished: TRANSFER_SRC -> SHADER_READ_ONLY.
            transitionMip(cmd, m_image, i - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

            mipW = std::max(mipW / 2, 1);
            mipH = std::max(mipH / 2, 1);
        }

        // The last level is still TRANSFER_DST -> SHADER_READ_ONLY.
        transitionMip(cmd, m_image, m_mipLevels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kTextureFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = m_mipLevels;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(context.device(), &viewInfo, nullptr, &m_view));

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE; // samplerAnisotropy feature isn't enabled on the device
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(m_mipLevels);
    VK_CHECK(vkCreateSampler(context.device(), &samplerInfo, nullptr, &m_sampler));
}

VulkanTexture::~VulkanTexture() { destroy(); }

void VulkanTexture::destroy() {
    if (m_context == nullptr) {
        return;
    }
    VkDevice device = m_context->device();
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_sampler, nullptr);
    }
    if (m_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_view, nullptr);
    }
    if (m_image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_image, m_allocation);
    }
    m_context = nullptr;
    m_allocator = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_view = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
}

VulkanTexture::VulkanTexture(VulkanTexture&& other) noexcept
    : m_context(other.m_context), m_allocator(other.m_allocator), m_image(other.m_image),
      m_allocation(other.m_allocation), m_view(other.m_view), m_sampler(other.m_sampler),
      m_mipLevels(other.m_mipLevels) {
    other.m_context = nullptr;
    other.m_allocator = VK_NULL_HANDLE;
    other.m_image = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
    other.m_view = VK_NULL_HANDLE;
    other.m_sampler = VK_NULL_HANDLE;
}

VulkanTexture& VulkanTexture::operator=(VulkanTexture&& other) noexcept {
    if (this != &other) {
        destroy();
        m_context = other.m_context;
        m_allocator = other.m_allocator;
        m_image = other.m_image;
        m_allocation = other.m_allocation;
        m_view = other.m_view;
        m_sampler = other.m_sampler;
        m_mipLevels = other.m_mipLevels;
        other.m_context = nullptr;
        other.m_allocator = VK_NULL_HANDLE;
        other.m_image = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE;
        other.m_view = VK_NULL_HANDLE;
        other.m_sampler = VK_NULL_HANDLE;
    }
    return *this;
}

} // namespace pose
