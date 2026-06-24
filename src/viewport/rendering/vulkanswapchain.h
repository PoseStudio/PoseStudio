/**
 * @file vulkanswapchain.h
 * @brief Everything tied to the window's current resolution: the swapchain images,
 *        the shared depth buffer, the render pass, and the per-image framebuffers.
 *
 * This is the layer that gets thrown away and rebuilt on every resize (and whenever
 * the surface goes out of date). VulkanContext stays alive across that; VulkanSwapchain
 * does not. The render pass technically only depends on the colour/depth *formats*
 * (which don't change on resize), so recreate() keeps it and only rebuilds the
 * resolution-dependent images/framebuffers.
 */

#ifndef VULKANSWAPCHAIN_H
#define VULKANSWAPCHAIN_H

#include "vulkancommon.h"

#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

namespace pose {

class VulkanContext;

/**
 * @class VulkanSwapchain
 * @brief Owns the swapchain + its render targets and the render pass they feed.
 */
class VulkanSwapchain {
public:
    /// Builds the swapchain at @p extentHint (used only when the surface lets us choose
    /// the size, i.e. on some compositors). @p extentHint is in physical pixels.
    VulkanSwapchain(VulkanContext& context, VkExtent2D extentHint);
    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    /// Tears down the resolution-dependent objects and rebuilds them at the new size.
    /// Safe to call repeatedly; the caller must vkDeviceWaitIdle first.
    void recreate(VkExtent2D extentHint);

    VkSwapchainKHR handle()        const { return m_swapchain; }
    VkRenderPass   renderPass()    const { return m_renderPass; }
    VkExtent2D     extent()        const { return m_extent; }
    VkFormat       colorFormat()   const { return m_colorFormat; }
    uint32_t       imageCount()    const { return static_cast<uint32_t>(m_images.size()); }
    VkFramebuffer  framebuffer(uint32_t imageIndex) const { return m_framebuffers[imageIndex]; }

private:
    void createSwapchain(VkExtent2D extentHint);
    void createImageViews();
    void createDepthResources();
    void createRenderPass();
    void createFramebuffers();
    void cleanupResolutionResources(); // everything except the render pass

    // Chooses surface format / present mode / extent from what the surface supports.
    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) const;
    VkPresentModeKHR   choosePresentMode(const std::vector<VkPresentModeKHR>& available) const;
    VkExtent2D         chooseExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D extentHint) const;

    VulkanContext& m_context;

    VkSwapchainKHR             m_swapchain   = VK_NULL_HANDLE;
    VkRenderPass              m_renderPass  = VK_NULL_HANDLE;
    VkFormat                  m_colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat                  m_depthFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D                m_extent      = {0, 0};

    std::vector<VkImage>      m_images;       // owned by the swapchain, not destroyed individually
    std::vector<VkImageView>  m_imageViews;
    std::vector<VkFramebuffer> m_framebuffers;

    // Single shared depth target (we don't render frames-in-flight into the same depth
    // buffer concurrently because acquire/present serialises per image).
    VkImage       m_depthImage      = VK_NULL_HANDLE;
    VmaAllocation m_depthAllocation = VK_NULL_HANDLE;
    VkImageView   m_depthView       = VK_NULL_HANDLE;
};

} // namespace pose

#endif // VULKANSWAPCHAIN_H
