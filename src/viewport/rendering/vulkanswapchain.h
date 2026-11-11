/**
 * @file vulkanswapchain.h
 * @brief Everything tied to the window's current resolution: the swapchain images, their
 *        views, the present render pass, and the per-image framebuffers.
 *
 * This is the layer that gets thrown away and rebuilt on every resize (and whenever the
 * surface goes out of date). VulkanContext stays alive across that; VulkanSwapchain does not.
 * The render pass depends only on the surface's color format (which doesn't change on
 * resize), so recreate() keeps it and only rebuilds the resolution-dependent images/framebuffers.
 *
 * The pass is deliberately minimal: ONE single-sample color attachment, loaded DONT_CARE and
 * stored for presentation, no depth. Since the HDR/post rework the scene renders offscreen
 * (HdrTarget, at the context's MSAA count) and the only draw in this pass is the fullscreen
 * composite, which overwrites every pixel with depth testing off — a multisampled color target
 * + depth buffer here would be cleared and resolved every frame (~236 MB at 5120x1440 4x) for
 * nothing. Pipelines built against this pass (the composite) are therefore single-sample.
 */

#ifndef VULKANSWAPCHAIN_H
#define VULKANSWAPCHAIN_H

#include "vulkancommon.h"
#include "vulkanhandles.h"

#include <cstdint>
#include <vector>

namespace pose {

class VulkanContext;

/**
 * @class VulkanSwapchain
 * @brief Owns the swapchain + its image views/framebuffers and the present render pass.
 */
class VulkanSwapchain {
public:
    /// Builds the swapchain at @p extentHint (used only when the surface lets us choose
    /// the size, i.e. on some compositors). @p extentHint is in physical pixels.
    VulkanSwapchain(VulkanContext& context, VkExtent2D extentHint);
    ~VulkanSwapchain() = default; // members clean up in reverse declaration order

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    /// Tears down the resolution-dependent objects and rebuilds them at the new size.
    /// Safe to call repeatedly; the caller must vkDeviceWaitIdle first.
    void recreate(VkExtent2D extentHint);

    VkSwapchainKHR handle()        const { return m_swapchain.get(); }
    VkRenderPass   renderPass()    const { return m_renderPass.get(); }
    VkExtent2D     extent()        const { return m_extent; }
    uint32_t       imageCount()    const { return static_cast<uint32_t>(m_images.size()); }
    /// The swapchain images can be copied FROM (frame capture); their handle and format.
    bool           readable()      const { return m_readable; }
    VkImage        image(uint32_t imageIndex) const { return m_images[imageIndex]; }
    VkFormat       colorFormat()   const { return m_colorFormat; }
    VkFramebuffer  framebuffer(uint32_t imageIndex) const { return m_framebuffers[imageIndex].get(); }

private:
    void createSwapchain(VkExtent2D extentHint);
    void createImageViews();
    void createRenderPass();
    void createFramebuffers();
    void cleanupResolutionResources(); // everything except the render pass

    // Chooses surface format / present mode / extent / composite alpha from what the surface supports.
    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& available) const;
    VkPresentModeKHR   choosePresentMode(const std::vector<VkPresentModeKHR>& available) const;
    VkExtent2D         chooseExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D extentHint) const;
    VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& caps) const;

    VulkanContext& m_context;

    // Declaration order = reverse destruction order: framebuffers and views go before the
    // swapchain whose images they wrap, and the pass they were built for goes last.
    UniqueRenderPass               m_renderPass;
    UniqueSwapchain                m_swapchain;
    VkFormat                       m_colorFormat = VK_FORMAT_UNDEFINED;
    bool                           m_readable = false; ///< See readable().
    VkExtent2D                     m_extent      = {0, 0};
    std::vector<VkImage>           m_images;      // owned by the swapchain, not destroyed individually
    std::vector<UniqueImageView>   m_imageViews;
    std::vector<UniqueFramebuffer> m_framebuffers;
};

} // namespace pose

#endif // VULKANSWAPCHAIN_H
