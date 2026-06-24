/**
 * @file vulkancontext.h
 * @brief The "device layer": owns the long-lived Vulkan objects that outlive any
 *        single swapchain or frame.
 *
 * One VulkanContext is created per window once a VkSurfaceKHR exists. It picks a
 * physical device with a queue family that can both render and present to that
 * surface, creates the logical device + queues, and stands up a VMA allocator for
 * the rest of the subsystem to allocate buffers/images through.
 *
 * Everything here is created once and torn down only when the viewport is destroyed.
 * Per-resolution objects (swapchain, depth buffer, framebuffers) live in
 * VulkanSwapchain; per-frame objects (command buffers, sync) live in VulkanRenderer.
 */

#ifndef VULKANCONTEXT_H
#define VULKANCONTEXT_H

#include "vulkancommon.h"

#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

namespace pose {

/**
 * @class VulkanContext
 * @brief Logical device, queues, and memory allocator for one rendering surface.
 *
 * Non-copyable, non-movable: it owns raw Vulkan handles whose lifetime is tied to
 * this object. Construct it on the heap and hold it via unique_ptr in VulkanWindow.
 */
class VulkanContext {
public:
    /**
     * @brief Selects a device and brings up queues + the VMA allocator.
     * @param instance       The VkInstance owned by the app's QVulkanInstance.
     * @param surface        The window surface to ensure the queue family can present to.
     * @param apiVersion     Vulkan API version the instance was created with (e.g. VK_API_VERSION_1_2).
     * @throws VulkanError if no suitable device is found or device creation fails.
     */
    VulkanContext(VkInstance instance, VkSurfaceKHR surface, uint32_t apiVersion);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // --- Accessors (handles are owned by this object; callers must not destroy them) ---
    VkInstance       instance()        const { return m_instance; }
    VkSurfaceKHR     surface()         const { return m_surface; }
    VkPhysicalDevice physicalDevice()  const { return m_physicalDevice; }
    VkDevice         device()          const { return m_device; }
    VkQueue          graphicsQueue()   const { return m_graphicsQueue; }
    VkQueue          presentQueue()    const { return m_presentQueue; }
    uint32_t         graphicsFamily()  const { return m_graphicsFamily; }
    uint32_t         presentFamily()   const { return m_presentFamily; }
    VmaAllocator     allocator()       const { return m_allocator; }

    /// Picks the first format the device supports for the depth/stencil attachment,
    /// preferring a pure 32-bit depth format. Throws if none are available.
    VkFormat findDepthFormat() const;

private:
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createAllocator(uint32_t apiVersion);

    /// Returns true and fills the out-params if @p device has a single queue family
    /// supporting both graphics and presentation to our surface.
    bool findQueueFamilies(VkPhysicalDevice device, uint32_t& graphicsFamily, uint32_t& presentFamily) const;

    VkInstance       m_instance       = VK_NULL_HANDLE; // borrowed (owned by QVulkanInstance)
    VkSurfaceKHR     m_surface        = VK_NULL_HANDLE; // borrowed (owned by QVulkanInstance/window)
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE; // not destroyed (implicitly owned by instance)
    VkDevice         m_device         = VK_NULL_HANDLE;
    VkQueue          m_graphicsQueue  = VK_NULL_HANDLE;
    VkQueue          m_presentQueue   = VK_NULL_HANDLE;
    uint32_t         m_graphicsFamily = 0;
    uint32_t         m_presentFamily  = 0;
    VmaAllocator     m_allocator      = VK_NULL_HANDLE;
};

} // namespace pose

#endif // VULKANCONTEXT_H
