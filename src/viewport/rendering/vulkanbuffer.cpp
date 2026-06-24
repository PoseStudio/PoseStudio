/**
 * @file vulkanbuffer.cpp
 * @brief Implementation of VulkanBuffer and its creation helpers. See vulkanbuffer.h.
 */

#include "vulkanbuffer.h"

#include "vulkancommands.h"
#include "vulkancommon.h"
#include "vulkancontext.h"

#include <cstring>

namespace pose {

VulkanBuffer::VulkanBuffer(VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
                           VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocFlags)
    : m_allocator(context.allocator()), m_size(size) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = memoryUsage;
    allocInfo.flags = allocFlags;

    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &m_buffer, &m_allocation, &info));
    m_mappedData = info.pMappedData; // non-null only when a MAPPED flag was requested
}

VulkanBuffer::~VulkanBuffer() { destroy(); }

void VulkanBuffer::destroy() {
    if (m_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
    }
    m_buffer = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
    m_size = 0;
    m_mappedData = nullptr;
}

VulkanBuffer::VulkanBuffer(VulkanBuffer&& other) noexcept
    : m_allocator(other.m_allocator), m_buffer(other.m_buffer), m_allocation(other.m_allocation),
      m_size(other.m_size), m_mappedData(other.m_mappedData) {
    other.m_buffer = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
    other.m_size = 0;
    other.m_mappedData = nullptr;
}

VulkanBuffer& VulkanBuffer::operator=(VulkanBuffer&& other) noexcept {
    if (this != &other) {
        destroy();
        m_allocator = other.m_allocator;
        m_buffer = other.m_buffer;
        m_allocation = other.m_allocation;
        m_size = other.m_size;
        m_mappedData = other.m_mappedData;
        other.m_buffer = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE;
        other.m_size = 0;
        other.m_mappedData = nullptr;
    }
    return *this;
}

VulkanBuffer createDeviceLocalBuffer(VulkanContext& context, const void* data, VkDeviceSize size,
                                     VkBufferUsageFlags usage) {
    // Host-visible staging buffer we can memcpy into directly.
    VulkanBuffer staging(context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                             VMA_ALLOCATION_CREATE_MAPPED_BIT);
    std::memcpy(staging.mappedData(), data, static_cast<size_t>(size));

    // Device-local destination, then copy staging -> device on the graphics queue.
    VulkanBuffer buffer(context, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_AUTO, 0);
    submitImmediate(context, [&](VkCommandBuffer cmd) {
        VkBufferCopy region{};
        region.size = size;
        vkCmdCopyBuffer(cmd, staging.handle(), buffer.handle(), 1, &region);
    });
    return buffer; // staging frees itself on scope exit
}

VulkanBuffer createMappedUniformBuffer(VulkanContext& context, VkDeviceSize size) {
    return VulkanBuffer(context, size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

} // namespace pose
