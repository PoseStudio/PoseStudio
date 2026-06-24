/**
 * @file vulkanbuffer.h
 * @brief A thin RAII wrapper over a VMA-allocated buffer, plus the two creation helpers the
 *        mesh path needs (a staged device-local buffer and a persistently-mapped uniform buffer).
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
    VkDeviceSize size()   const { return m_size; }

    /// Non-null only when the buffer was created with a HOST_ACCESS + MAPPED flag (e.g. a UBO).
    void* mappedData() const { return m_mappedData; }

private:
    void destroy();

    VmaAllocator  m_allocator  = VK_NULL_HANDLE; // borrowed from VulkanContext
    VkBuffer      m_buffer     = VK_NULL_HANDLE;
    VmaAllocation m_allocation = VK_NULL_HANDLE;
    VkDeviceSize  m_size       = 0;
    void*         m_mappedData = nullptr;
};

/// Creates a device-local buffer of @p usage and fills it with @p data via a host staging buffer
/// and a one-time graphics-queue copy (blocks until the copy completes). For vertex/index data.
VulkanBuffer createDeviceLocalBuffer(VulkanContext& context, const void* data, VkDeviceSize size,
                                     VkBufferUsageFlags usage);

/// Creates a host-visible, persistently-mapped uniform buffer (write via mappedData() each frame).
VulkanBuffer createMappedUniformBuffer(VulkanContext& context, VkDeviceSize size);

} // namespace pose

#endif // VULKANBUFFER_H
