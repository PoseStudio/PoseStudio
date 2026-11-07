/**
 * @file attachmentimage.h
 * @brief RAII owner for a 2D render-target image (+ its view), plus the image-layout barrier
 *        helpers every offscreen target and texture upload needs.
 *
 * Five classes used to carry their own copy of "vmaCreateImage a 2D attachment, then
 * vkCreateImageView it" (the HDR target, the outline mask, the bloom targets, the swapchain's old
 * MSAA/depth targets, the shadow map) and five more copies of "fill a VkImageMemoryBarrier and
 * vkCmdPipelineBarrier it". Centralising them here does three things: the target classes shrink
 * to their render pass + policy; a throwing constructor can no longer leak an image whose view
 * failed (the owner frees both); and one policy line — lazily-allocated memory for TRANSIENT
 * attachments, which is what makes an MSAA colour target memoryless on tile-based GPUs (Apple
 * via MoltenVK) — applies everywhere instead of nowhere.
 *
 * Pure Vulkan + VMA + std, no Qt (the rendering/ rule).
 */

#ifndef ATTACHMENTIMAGE_H
#define ATTACHMENTIMAGE_H

#include "vulkanhandles.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <initializer_list>

namespace pose {

class VulkanContext;

/**
 * @class AttachmentImage
 * @brief One 2D, single-mip, single-layer image with a matching view. Move-only; empty by default.
 *
 * The memory policy is derived from the usage flags: a TRANSIENT_ATTACHMENT image asks for
 * LAZILY_ALLOCATED memory (a no-op on desktop drivers, memoryless on tilers); everything else
 * prefers device-local memory.
 */
class AttachmentImage {
public:
    AttachmentImage() = default;
    /// Creates the image in UNDEFINED layout and its view over the whole (single) subresource.
    AttachmentImage(VulkanContext& context, VkFormat format, VkExtent2D extent,
                    VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                    VkImageAspectFlags aspect);
    ~AttachmentImage();

    AttachmentImage(const AttachmentImage&) = delete;
    AttachmentImage& operator=(const AttachmentImage&) = delete;
    AttachmentImage(AttachmentImage&& other) noexcept;
    AttachmentImage& operator=(AttachmentImage&& other) noexcept;

    /// Frees the view and the image (idempotent; the object is empty afterwards).
    void reset();

    VkImage     image() const { return m_image; }
    VkImageView view() const { return m_view.get(); }
    explicit operator bool() const { return m_image != VK_NULL_HANDLE; }

private:
    VmaAllocator    m_allocator = VK_NULL_HANDLE; // borrowed from the context
    VkImage         m_image = VK_NULL_HANDLE;
    VmaAllocation   m_allocation = VK_NULL_HANDLE;
    UniqueImageView m_view; // declared last: destroyed before the image it views
};

/// Creates a view over @p image covering @p mipLevels x @p layers of @p aspect. The shape every
/// sampled image in the engine needs (2D textures, the IBL cubemap, swapchain images).
UniqueImageView createImageView(VkDevice device, VkImage image, VkImageViewType viewType,
                                VkFormat format, VkImageAspectFlags aspect,
                                uint32_t mipLevels = 1, uint32_t layers = 1);

/// Fills an image-memory barrier moving @p range of @p image from @p oldLayout to @p newLayout
/// with the given access masks — for callers that batch several barriers into one
/// vkCmdPipelineBarrier (the texture mip chain). The queue-family fields are IGNORED (the engine
/// has one graphics+present family — see VulkanContext).
VkImageMemoryBarrier imageBarrier(VkImage image, const VkImageSubresourceRange& range,
                                  VkImageLayout oldLayout, VkImageLayout newLayout,
                                  VkAccessFlags srcAccess, VkAccessFlags dstAccess);

/// Records one image-memory barrier (imageBarrier() + vkCmdPipelineBarrier) moving @p range of
/// @p image from @p oldLayout to @p newLayout between the given stages.
void transitionImage(VkCommandBuffer cmd, VkImage image, const VkImageSubresourceRange& range,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                     VkPipelineStageFlags dstStage, VkAccessFlags dstAccess);

/// The whole-image subresource range for @p aspect (all @p mipLevels, all @p layers).
inline VkImageSubresourceRange wholeImageRange(VkImageAspectFlags aspect, uint32_t mipLevels = 1,
                                               uint32_t layers = 1) {
    return {aspect, 0, mipLevels, 0, layers};
}

/// One blocking submit that moves each (single-mip, single-layer, colour) image from UNDEFINED
/// to SHADER_READ_ONLY_OPTIMAL. For targets a later pass samples unconditionally but whose own
/// pass runs only sometimes (the outline mask, the bloom/SSS targets outside the PBR mode): a
/// freshly created image would otherwise be bound in UNDEFINED layout — a validation violation
/// and UB on hardware. Only for construction/resize, when the device is idle anyway.
void initialiseToShaderRead(VulkanContext& context, std::initializer_list<VkImage> images);

/// As initialiseToShaderRead, CLEARING each (colour, TRANSFER_DST-capable) image to zero on the
/// way — for a target a later pass reads as "nothing drawn" while its own pass may never run
/// (the photo layer's coverage 0): a fresh image's contents are undefined, not zero.
void initialiseToShaderReadCleared(VulkanContext& context, std::initializer_list<VkImage> images);

/// One blocking submit moving a DEPTH image from UNDEFINED to DEPTH_STENCIL_READ_ONLY_OPTIMAL —
/// the layout a pass that LOADS it as a read-only attachment declares it enters in (the photo
/// pass's depth, before any resolve has written it).
void initialiseDepthReadOnly(VulkanContext& context, VkImage image);

} // namespace pose

#endif // ATTACHMENTIMAGE_H
