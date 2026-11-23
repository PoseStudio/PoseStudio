// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file vulkanbuffer.h
 * @brief A thin RAII wrapper over a VMA-allocated buffer, plus the creation helpers the scene
 *        needs (a staged device-local buffer — blocking or batched — a persistently-mapped
 *        uniform buffer, and a zeroed host-mapped storage buffer).
 *
 * Like the rest of rendering/, this is Qt-free (plain Vulkan + VMA + std). VulkanBuffer owns its
 * VkBuffer + VmaAllocation and frees both on destruction; it is move-only so it can live in a
 * std::vector (per-frame UBOs) or inside a Mesh.
 */

#ifndef VULKANBUFFER_H
#define VULKANBUFFER_H

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace pose {

class VulkanContext;
class ImmediateBatch;

/**
 * @class VulkanBuffer
 * @brief Owns one VMA buffer allocation. Move-only; default-constructs to an empty handle.
 */
class VulkanBuffer {
public:
    VulkanBuffer() = default;
    VulkanBuffer(VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
                 VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocFlags = 0);
    ~VulkanBuffer();

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;
    VulkanBuffer(VulkanBuffer&& other) noexcept;
    VulkanBuffer& operator=(VulkanBuffer&& other) noexcept;

    VkBuffer     handle() const { return m_buffer; }

    /// Non-null only when the buffer was created with a HOST_ACCESS + MAPPED flag (e.g. a UBO).
    /// Host-access buffers are always HOST_COHERENT (see the constructor), so a write needs no
    /// flush and a read-back no invalidate.
    void* mappedData() const { return m_mappedData; }

private:
    void destroy();

    VmaAllocator  m_allocator  = VK_NULL_HANDLE; // borrowed from VulkanContext
    VkBuffer      m_buffer     = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    void*         m_mappedData = nullptr;
};

/// Creates a device-local buffer of @p usage and fills it with @p data via a host staging buffer
/// and a one-time graphics-queue copy (blocks until the copy completes). For vertex/index data.
VulkanBuffer createDeviceLocalBuffer(VulkanContext& context, const void* data, VkDeviceSize size,
                                     VkBufferUsageFlags usage);

/// Batched variant: records the staging->device copy into @p batch (so many buffers share one
/// submit) and hands the staging buffer to the batch to keep alive until it completes. Does not
/// block; the caller flushes @p batch once all uploads are recorded.
VulkanBuffer createDeviceLocalBuffer(VulkanContext& context, const void* data, VkDeviceSize size,
                                     VkBufferUsageFlags usage, ImmediateBatch& batch);

/// Creates a host-visible, persistently-mapped uniform buffer (write via mappedData() each frame).
VulkanBuffer createMappedUniformBuffer(VulkanContext& context, VkDeviceSize size);

/// A host-mapped, zero-filled STORAGE buffer of @p bytes (rewritten in place via mappedData()):
/// the per-frame pose-corrective weight buffers, and the placeholder bound where a model has no
/// correctives so its descriptor set stays complete.
VulkanBuffer makeZeroedHostStorageBuffer(VulkanContext& context, VkDeviceSize bytes);

} // namespace pose

#endif // VULKANBUFFER_H
