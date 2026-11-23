// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file vulkanimage.cpp
 * @brief Implementation of VulkanTexture. See vulkanimage.h.
 */

#include "vulkanimage.h"

#include "attachmentimage.h"
#include "samplercache.h"
#include "vulkanbuffer.h"
#include "vulkancommands.h"
#include "vulkancommon.h"
#include "vulkancontext.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace pose {

VulkanTexture::VulkanTexture(VulkanContext& context, const uint8_t* pixels, uint32_t width,
                             uint32_t height, bool srgb)
    : m_context(&context), m_allocator(context.allocator()),
      m_format(srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM) {
    // One-off upload: record into a single-use batch and flush it immediately.
    ImmediateBatch batch(context);
    recordUpload(batch, pixels, width, height);
    batch.submitAndWait();
}

VulkanTexture::VulkanTexture(VulkanContext& context, const uint8_t* pixels, uint32_t width,
                             uint32_t height, ImmediateBatch& batch, bool srgb)
    : m_context(&context), m_allocator(context.allocator()),
      m_format(srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM) {
    // Batched upload: the caller flushes @p batch once for the whole import.
    recordUpload(batch, pixels, width, height);
}

void VulkanTexture::recordUpload(ImmediateBatch& batch, const uint8_t* pixels, uint32_t width,
                                 uint32_t height) {
    VulkanContext& context = *m_context;
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

    // Mipmaps need linear blit support for this format; fall back to a single level if absent.
    // (The format properties are cached per format on the context — a figure uploads dozens of
    // maps of the same two formats.)
    const VkFormatProperties& formatProps = context.formatProperties(m_format);
    const bool canMip = (formatProps.optimalTilingFeatures &
                         VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0 &&
                        (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0 &&
                        (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
    m_mipLevels = canMip
                      ? static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1
                      : 1;
    m_sampler = context.samplers().get(SamplerDesc::linearRepeatMipmapped());

    // Staging buffer with the pixel bytes. Handed to the batch right away (the GPU reads it
    // until the batch submits); the handle is kept for the copy command below.
    VulkanBuffer staging(context, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT);
    std::memcpy(staging.mappedData(), pixels, static_cast<size_t>(imageSize));
    const VkBuffer stagingHandle = staging.handle();
    batch.retain(std::move(staging));

    // Device-local image. TRANSFER_SRC is needed because mip generation blits from each level.
    // The image and its view are created BEFORE any command references the image, so a failure
    // (caught below, which frees the image) can never leave the batch pointing at a dead image.
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = m_format;
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
    try {
        VK_CHECK(vmaCreateImage(m_allocator, &imageInfo, &allocInfo, &m_image, &m_allocation, nullptr));
        m_view = createImageView(context.device(), m_image, VK_IMAGE_VIEW_TYPE_2D, m_format,
                                 VK_IMAGE_ASPECT_COLOR_BIT, m_mipLevels);
    } catch (...) {
        destroy();
        throw;
    }

    // --- Record the upload + mip chain. Barrier count is mipLevels + 1 (one all-mips transition
    // in, one DST->SRC step per generated level, one combined transition out) — the earlier three
    // barriers per level put ~1500 vkCmdPipelineBarriers into a single figure's import batch.
    VkCommandBuffer cmd = batch.commandBuffer();
    const VkImageSubresourceRange allMips = wholeImageRange(VK_IMAGE_ASPECT_COLOR_BIT, m_mipLevels);
    const auto mipRange = [](uint32_t first, uint32_t count) {
        return VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, first, count, 0, 1};
    };

    // Every level: UNDEFINED -> TRANSFER_DST (level 0 receives the pixels, the others the blits).
    transitionImage(cmd, m_image, allMips, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = 0;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, stagingHandle, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);

    // Generate the mip chain by successively halving + blitting from the previous level.
    int32_t mipW = static_cast<int32_t>(width);
    int32_t mipH = static_cast<int32_t>(height);
    for (uint32_t i = 1; i < m_mipLevels; ++i) {
        // Previous level: its writes (the copy or the previous blit) must land before it becomes
        // the blit SOURCE — TRANSFER_DST -> TRANSFER_SRC.
        transitionImage(cmd, m_image, mipRange(i - 1, 1), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);

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

        mipW = std::max(mipW / 2, 1);
        mipH = std::max(mipH / 2, 1);
    }

    // Out: levels 0..n-2 are TRANSFER_SRC (they served as blit sources), the last level is still
    // TRANSFER_DST — both to SHADER_READ_ONLY in one barrier call.
    VkImageMemoryBarrier finals[2];
    uint32_t finalCount = 0;
    if (m_mipLevels > 1) {
        finals[finalCount++] = imageBarrier(m_image, mipRange(0, m_mipLevels - 1),
                                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    finals[finalCount++] = imageBarrier(m_image, mipRange(m_mipLevels - 1, 1),
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, finalCount, finals);
}

VulkanTexture::~VulkanTexture() { destroy(); }

void VulkanTexture::destroy() {
    if (m_context == nullptr) {
        return;
    }
    m_view.reset();
    if (m_image != VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, m_image, m_allocation);
    }
    m_context = nullptr;
    m_allocator = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
}

VulkanTexture::VulkanTexture(VulkanTexture&& other) noexcept
    : m_context(other.m_context), m_allocator(other.m_allocator), m_image(other.m_image),
      m_allocation(other.m_allocation), m_view(std::move(other.m_view)), m_sampler(other.m_sampler),
      m_mipLevels(other.m_mipLevels), m_format(other.m_format) {
    other.m_context = nullptr;
    other.m_allocator = VK_NULL_HANDLE;
    other.m_image = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
    other.m_sampler = VK_NULL_HANDLE;
}

VulkanTexture& VulkanTexture::operator=(VulkanTexture&& other) noexcept {
    if (this != &other) {
        destroy();
        m_context = other.m_context;
        m_allocator = other.m_allocator;
        m_image = other.m_image;
        m_allocation = other.m_allocation;
        m_view = std::move(other.m_view);
        m_sampler = other.m_sampler;
        m_mipLevels = other.m_mipLevels;
        m_format = other.m_format; // without this a moved linear (_UNORM) texture reports sRGB
        other.m_context = nullptr;
        other.m_allocator = VK_NULL_HANDLE;
        other.m_image = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE;
        other.m_sampler = VK_NULL_HANDLE;
    }
    return *this;
}

} // namespace pose
