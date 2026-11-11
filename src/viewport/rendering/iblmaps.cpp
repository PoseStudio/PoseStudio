/**
 * @file iblmaps.cpp
 * @brief Implementation of IblMaps. See iblmaps.h.
 */

#include "iblmaps.h"

#include "attachmentimage.h"
#include "ibldata.h"
#include "samplercache.h"
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
constexpr VkFormat kLutFormat  = VK_FORMAT_R16G16B16A16_SFLOAT; // GGX scale + bias, the sheen albedo

// Packs an RGBA float texel into two 16F-pair uints (RG, BA) — the RGBA16F memory layout.
void packRgba16f(const glm::vec4& c, uint32_t& lo, uint32_t& hi) {
    lo = glm::packHalf2x16(glm::vec2(c.r, c.g));
    hi = glm::packHalf2x16(glm::vec2(c.b, c.a));
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
    std::size_t totalTexels = 0;
    for (uint32_t mip = 0; mip < mips; ++mip) {
        const uint32_t size = std::max(1u, base >> mip);
        totalTexels += static_cast<std::size_t>(size) * size;
    }
    cubeStaging.reserve(totalTexels * 6 * 2);
    cubeCopies.reserve(6 * mips);
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

    // --- Pack the LUT (RGBA16F: two uints per texel).
    std::vector<uint32_t> lutStaging;
    lutStaging.reserve(lut.data.size() * 2);
    for (const glm::vec4& t : lut.data) {
        uint32_t lo, hi;
        packRgba16f(t, lo, hi);
        lutStaging.push_back(lo);
        lutStaging.push_back(hi);
    }
    const uint32_t lutSize = static_cast<uint32_t>(std::max(1, lut.size));

    // A throw anywhere below leaves a half-built object whose destructor never runs, so the
    // images allocated so far are freed here before the exception propagates (the views and the
    // staging buffers are RAII and free themselves).
    try {
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

        // --- Views (created before any command references the images, so a failure here leaves
        // nothing recorded against them).
        m_specView = createImageView(context.device(), m_specImage, VK_IMAGE_VIEW_TYPE_CUBE, kSpecFormat,
                                     VK_IMAGE_ASPECT_COLOR_BIT, mips, 6);
        m_lutView = createImageView(context.device(), m_lutImage, VK_IMAGE_VIEW_TYPE_2D, kLutFormat,
                                    VK_IMAGE_ASPECT_COLOR_BIT);

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
        const VkImageSubresourceRange cubeRange = wholeImageRange(VK_IMAGE_ASPECT_COLOR_BIT, mips, 6);
        transitionImage(cmd, m_specImage, cubeRange, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(cmd, cubeBuf.handle(), m_specImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(cubeCopies.size()), cubeCopies.data());
        transitionImage(cmd, m_specImage, cubeRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT);

        const VkImageSubresourceRange lutRange = wholeImageRange(VK_IMAGE_ASPECT_COLOR_BIT);
        transitionImage(cmd, m_lutImage, lutRange, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy lutCopy{};
        lutCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        lutCopy.imageExtent = {lutSize, lutSize, 1};
        vkCmdCopyBufferToImage(cmd, lutBuf.handle(), m_lutImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &lutCopy);
        transitionImage(cmd, m_lutImage, lutRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT);

        batch.retain(std::move(cubeBuf));
        batch.retain(std::move(lutBuf));
        batch.submitAndWait();
    } catch (...) {
        destroyImages();
        throw;
    }

    // --- Samplers (shared): trilinear clamp for the cube (mip = roughness; the cache's
    // VK_LOD_CLAMP_NONE limit is clamped to the view's mip range by the hardware, so it is
    // equivalent to maxLod = mips). The single-level LUT needs no mip filtering at all — with one
    // level every mipmap mode selects level 0, so the plain linear-clamp sampler is exact.
    m_specSampler = context.samplers().get(SamplerDesc::linearClampMipmapped());
    m_lutSampler = context.samplers().get(SamplerDesc::linearClamp());
}

IblMaps::~IblMaps() { destroyImages(); }

void IblMaps::destroyImages() {
    const VmaAllocator allocator = m_context.allocator();
    m_specView.reset();
    m_lutView.reset();
    if (m_specImage) {
        vmaDestroyImage(allocator, m_specImage, m_specAlloc);
        m_specImage = VK_NULL_HANDLE;
        m_specAlloc = VK_NULL_HANDLE;
    }
    if (m_lutImage) {
        vmaDestroyImage(allocator, m_lutImage, m_lutAlloc);
        m_lutImage = VK_NULL_HANDLE;
        m_lutAlloc = VK_NULL_HANDLE;
    }
}

VkDescriptorImageInfo IblMaps::specularInfo() const {
    return {m_specSampler, m_specView.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

VkDescriptorImageInfo IblMaps::brdfInfo() const {
    return {m_lutSampler, m_lutView.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

} // namespace pose
