/**
 * @file iblmaps.cpp
 * @brief Implementation of IblMaps. See iblmaps.h.
 */

#include "iblmaps.h"

#include "environment.h"
#include "vulkanbuffer.h"
#include "vulkancommands.h"
#include "vulkancommon.h"
#include "vulkancontext.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pose {

namespace {

constexpr VkFormat kSpecFormat = VK_FORMAT_R16G16B16A16_SFLOAT; // linear-filterable HDR (widely supported)
constexpr VkFormat kLutFormat  = VK_FORMAT_R16G16_SFLOAT;

// Packs an RGBA float texel into two 16F-pair uints (RG, BA) — the RGBA16F memory layout.
void packRgba16f(const glm::vec4& c, uint32_t& lo, uint32_t& hi) {
    lo = glm::packHalf2x16(glm::vec2(c.r, c.g));
    hi = glm::packHalf2x16(glm::vec2(c.b, c.a));
}

// Full-subresource-range image barrier (all mips, all layers) between two layouts.
void transitionAll(VkCommandBuffer cmd, VkImage image, uint32_t mips, uint32_t layers,
                   VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags src,
                   VkAccessFlags dst, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

IblMaps::IblMaps(VulkanContext& context, const PrefilteredSpecular& specular, const BrdfLut& lut)
    : m_context(context) {
    const VmaAllocator allocator = context.allocator();
    const uint32_t mips = static_cast<uint32_t>(std::max(1, specular.mipCount));
    const uint32_t base = static_cast<uint32_t>(std::max(1, specular.baseSize));

    // --- Pack the cubemap face/mip data into one staging buffer, recording a copy region per face+mip.
    std::vector<uint32_t> cubeStaging; // two uints per RGBA16F texel
    std::vector<VkBufferImageCopy> cubeCopies;
    for (int face = 0; face < 6; ++face) {
        for (uint32_t mip = 0; mip < mips; ++mip) {
            const uint32_t size = std::max(1u, base >> mip);
            const std::vector<glm::vec4>& src = specular.faces[face][mip];
            VkBufferImageCopy copy{};
            copy.bufferOffset = static_cast<VkDeviceSize>(cubeStaging.size()) * sizeof(uint32_t);
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, static_cast<uint32_t>(face), 1};
            copy.imageExtent = {size, size, 1};
            cubeCopies.push_back(copy);
            for (const glm::vec4& texel : src) {
                uint32_t lo, hi;
                packRgba16f(texel, lo, hi);
                cubeStaging.push_back(lo);
                cubeStaging.push_back(hi);
            }
        }
    }

    VkImageCreateInfo cubeInfo{};
    cubeInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    cubeInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    cubeInfo.imageType = VK_IMAGE_TYPE_2D;
    cubeInfo.format = kSpecFormat;
    cubeInfo.extent = {base, base, 1};
    cubeInfo.mipLevels = mips;
    cubeInfo.arrayLayers = 6;
    cubeInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    cubeInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    cubeInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    cubeInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    cubeInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(allocator, &cubeInfo, &alloc, &m_specImage, &m_specAlloc, nullptr));

    // --- Pack the LUT (RG16F: one uint per texel).
    std::vector<uint32_t> lutStaging;
    lutStaging.reserve(lut.data.size());
    for (const glm::vec2& t : lut.data) {
        lutStaging.push_back(glm::packHalf2x16(t));
    }
    const uint32_t lutSize = static_cast<uint32_t>(std::max(1, lut.size));

    VkImageCreateInfo lutInfo{};
    lutInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    lutInfo.imageType = VK_IMAGE_TYPE_2D;
    lutInfo.format = kLutFormat;
    lutInfo.extent = {lutSize, lutSize, 1};
    lutInfo.mipLevels = 1;
    lutInfo.arrayLayers = 1;
    lutInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    lutInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    lutInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    lutInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    lutInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vmaCreateImage(allocator, &lutInfo, &alloc, &m_lutImage, &m_lutAlloc, nullptr));

    // --- One batch: stage both, copy, and transition to shader-read.
    VulkanBuffer cubeBuf(context, cubeStaging.size() * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VMA_MEMORY_USAGE_AUTO,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT);
    std::memcpy(cubeBuf.mappedData(), cubeStaging.data(), cubeStaging.size() * sizeof(uint32_t));
    VulkanBuffer lutBuf(context, lutStaging.size() * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VMA_MEMORY_USAGE_AUTO,
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT);
    std::memcpy(lutBuf.mappedData(), lutStaging.data(), lutStaging.size() * sizeof(uint32_t));

    ImmediateBatch batch(context);
    VkCommandBuffer cmd = batch.commandBuffer();
    transitionAll(cmd, m_specImage, mips, 6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(cmd, cubeBuf.handle(), m_specImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(cubeCopies.size()), cubeCopies.data());
    transitionAll(cmd, m_specImage, mips, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    transitionAll(cmd, m_lutImage, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy lutCopy{};
    lutCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    lutCopy.imageExtent = {lutSize, lutSize, 1};
    vkCmdCopyBufferToImage(cmd, lutBuf.handle(), m_lutImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &lutCopy);
    transitionAll(cmd, m_lutImage, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    batch.retain(std::move(cubeBuf));
    batch.retain(std::move(lutBuf));
    batch.submitAndWait();

    // --- Views + samplers.
    VkImageViewCreateInfo cubeView{};
    cubeView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    cubeView.image = m_specImage;
    cubeView.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    cubeView.format = kSpecFormat;
    cubeView.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 6};
    VK_CHECK(vkCreateImageView(context.device(), &cubeView, nullptr, &m_specView));

    VkImageViewCreateInfo lutView{};
    lutView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    lutView.image = m_lutImage;
    lutView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    lutView.format = kLutFormat;
    lutView.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(context.device(), &lutView, nullptr, &m_lutView));

    VkSamplerCreateInfo samp{};
    samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp.magFilter = VK_FILTER_LINEAR;
    samp.minFilter = VK_FILTER_LINEAR;
    samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.minLod = 0.0f;
    samp.maxLod = static_cast<float>(mips);
    VK_CHECK(vkCreateSampler(context.device(), &samp, nullptr, &m_specSampler));
    samp.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(context.device(), &samp, nullptr, &m_lutSampler));
}

IblMaps::~IblMaps() {
    VkDevice device = m_context.device();
    const VmaAllocator allocator = m_context.allocator();
    if (m_specSampler) vkDestroySampler(device, m_specSampler, nullptr);
    if (m_lutSampler) vkDestroySampler(device, m_lutSampler, nullptr);
    if (m_specView) vkDestroyImageView(device, m_specView, nullptr);
    if (m_lutView) vkDestroyImageView(device, m_lutView, nullptr);
    if (m_specImage) vmaDestroyImage(allocator, m_specImage, m_specAlloc);
    if (m_lutImage) vmaDestroyImage(allocator, m_lutImage, m_lutAlloc);
}

VkDescriptorImageInfo IblMaps::specularInfo() const {
    return {m_specSampler, m_specView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

VkDescriptorImageInfo IblMaps::brdfInfo() const {
    return {m_lutSampler, m_lutView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

} // namespace pose
