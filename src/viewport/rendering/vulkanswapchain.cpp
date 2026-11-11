/**
 * @file vulkanswapchain.cpp
 * @brief Implementation of the resolution-dependent presentation layer. See vulkanswapchain.h.
 */

#include "vulkanswapchain.h"

#include "attachmentimage.h"
#include "renderpassbuilder.h"
#include "vulkancontext.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace pose {

namespace {

/// VK_CHECK for the two-call enumeration pattern: VK_INCOMPLETE (the count changed between the
/// calls) is a success code, not a failure.
void checkQuery(VkResult result, const char* what) {
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        throw VulkanError(std::string(what) + " failed: " + vkResultString(result));
    }
}

} // namespace

VulkanSwapchain::VulkanSwapchain(VulkanContext& context, VkExtent2D extentHint)
    : m_context(context) {
    createSwapchain(extentHint);
    createImageViews();
    createRenderPass();
    createFramebuffers();
}

void VulkanSwapchain::recreate(VkExtent2D extentHint) {
    cleanupResolutionResources();
    // Render pass survives: it depends only on the color format, which doesn't change on resize.
    createSwapchain(extentHint);
    createImageViews();
    createFramebuffers();
}

void VulkanSwapchain::cleanupResolutionResources() {
    m_framebuffers.clear();
    m_imageViews.clear();
    m_images.clear(); // images are owned by the swapchain object itself
    m_swapchain.reset();
}

VkSurfaceFormatKHR VulkanSwapchain::chooseSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& available) const {
    // An sRGB 8-bit format: the whole engine relies on the swapchain's sRGB STORE doing the final
    // gamma encode (the composite writes linear values; the grid, outline and clear colors are
    // authored linear). BGRA is the common Windows/Linux default; RGBA the usual alternative.
    for (VkFormat wanted : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}) {
        for (const auto& format : available) {
            if (format.format == wanted && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
    }
    // No sRGB format at all: fall back to whatever the surface offers first. A UNORM swapchain
    // stores linear values unencoded, so the image will look too dark/flat — worth a loud note,
    // since every "the sRGB store encodes" assumption in the shaders is now false. (Plain stderr:
    // the rendering/ core is Qt-free.)
    std::fprintf(stderr,
                 "[Vulkan] No sRGB swapchain format offered; using format %d (color space %d) — "
                 "the viewport will not be gamma-encoded correctly.\n",
                 static_cast<int>(available.front().format),
                 static_cast<int>(available.front().colorSpace));
    return available.front();
}

VkPresentModeKHR VulkanSwapchain::choosePresentMode(
    const std::vector<VkPresentModeKHR>& available) const {
    // MAILBOX when available: tear-free like FIFO, but presenting never BLOCKS on the vertical
    // blank — a just-rendered frame replaces the queued one and shows at the next scan-out
    // instead of waiting a whole refresh behind it. That is up to a frame (16ms at 60Hz) less
    // input-to-photon latency on every interactive drag, which the user feels directly as
    // responsiveness (the IK drag's damped dynamics already sit on a 60Hz tick, so pipeline
    // latency is a large share of what is left). The usual power objection to mailbox
    // (rendering as fast as possible) doesn't apply here: rendering is EVENT-DRIVEN — no
    // frames are produced while nothing changes, and a drag produces at most one per tick.
    // FIFO (always supported) is the fallback.
    for (VkPresentModeKHR mode : available) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return mode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanSwapchain::chooseExtent(const VkSurfaceCapabilitiesKHR& caps,
                                         VkExtent2D extentHint) const {
    // If the surface dictates the extent (currentExtent != UINT32_MAX), we must use it.
    // Otherwise clamp our window-derived hint into the allowed range.
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return caps.currentExtent;
    }
    VkExtent2D extent = extentHint;
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return extent;
}

VkCompositeAlphaFlagBitsKHR VulkanSwapchain::chooseCompositeAlpha(
    const VkSurfaceCapabilitiesKHR& caps) const {
    // The viewport is opaque, so OPAQUE is what we want — but a surface may not offer it (some
    // Wayland compositors expose only INHERIT or PRE_MULTIPLIED), and requesting an unsupported
    // bit is a spec violation (VUID-VkSwapchainCreateInfoKHR-compositeAlpha-01280). The composite
    // writes alpha 1 everywhere, so every mode below presents the same opaque image. The spec
    // guarantees at least one bit is set.
    for (VkCompositeAlphaFlagBitsKHR bit :
         {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if (caps.supportedCompositeAlpha & bit) {
            return bit;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

void VulkanSwapchain::createSwapchain(VkExtent2D extentHint) {
    VkPhysicalDevice gpu = m_context.physicalDevice();
    VkSurfaceKHR surface = m_context.surface();

    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));

    // The enumerations are checked: an unchecked failure leaves the lists empty and
    // chooseSurfaceFormat's available.front() would then be undefined behaviour.
    uint32_t formatCount = 0;
    checkQuery(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, nullptr),
               "vkGetPhysicalDeviceSurfaceFormatsKHR");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    checkQuery(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, formats.data()),
               "vkGetPhysicalDeviceSurfaceFormatsKHR");
    formats.resize(formatCount);
    if (formats.empty()) {
        throw VulkanError("The surface reports no supported swapchain formats.");
    }

    uint32_t presentModeCount = 0;
    checkQuery(vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &presentModeCount, nullptr),
               "vkGetPhysicalDeviceSurfacePresentModesKHR");
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    checkQuery(vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &presentModeCount,
                                                         presentModes.data()),
               "vkGetPhysicalDeviceSurfacePresentModesKHR");
    presentModes.resize(presentModeCount);
    if (presentModes.empty()) {
        throw VulkanError("The surface reports no supported present modes.");
    }

    const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(formats);
    const VkPresentModeKHR presentMode = choosePresentMode(presentModes);
    m_extent = chooseExtent(caps, extentHint);
    m_colorFormat = surfaceFormat.format;

    // One more than the minimum lets the GPU keep working while we hold one image.
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = surfaceFormat.format;
    ci.imageColorSpace = surfaceFormat.colorSpace;
    ci.imageExtent = m_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // (... and a copy SOURCE where the surface allows it — every desktop driver does: the frame
    // capture of the scripted test runner reads the composited image back, VulkanRenderer::
    // requestCapture. The flag costs nothing while no capture is asked for.)
    m_readable = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (m_readable) {
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    // Single graphics+present family (see VulkanContext::findQueueFamilies), so the
    // images are used exclusively by one family — no concurrent sharing needed.
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = chooseCompositeAlpha(caps);
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE; // we fully tear down before recreating

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSwapchainKHR(m_context.device(), &ci, nullptr, &swapchain));
    m_swapchain.reset(m_context.device(), swapchain);

    uint32_t actualCount = 0;
    checkQuery(vkGetSwapchainImagesKHR(m_context.device(), swapchain, &actualCount, nullptr),
               "vkGetSwapchainImagesKHR");
    m_images.resize(actualCount);
    checkQuery(vkGetSwapchainImagesKHR(m_context.device(), swapchain, &actualCount, m_images.data()),
               "vkGetSwapchainImagesKHR");
    m_images.resize(actualCount);
    if (m_images.empty()) {
        throw VulkanError("The swapchain reports no images.");
    }
}

void VulkanSwapchain::createImageViews() {
    m_imageViews.clear();
    m_imageViews.reserve(m_images.size());
    for (VkImage image : m_images) {
        m_imageViews.push_back(createImageView(m_context.device(), image, VK_IMAGE_VIEW_TYPE_2D,
                                               m_colorFormat, VK_IMAGE_ASPECT_COLOR_BIT));
    }
}

void VulkanSwapchain::createRenderPass() {
    // One single-sample color attachment: the swapchain image itself. loadOp DONT_CARE — the
    // composite's fullscreen triangle overwrites every pixel, so no clear is needed (and the
    // render-pass begin passes no clear values); stored and handed to presentation.
    //
    // The one dependency orders this pass's color writes after the image acquire: the submit
    // waits on the acquire semaphore at COLOR_ATTACHMENT_OUTPUT, and the external->0 dependency
    // at that same stage (no source access — the semaphore wait already made everything
    // available) is where the UNDEFINED->COLOR_ATTACHMENT layout transition is placed.
    VkSubpassDependency acquire{};
    acquire.srcSubpass = VK_SUBPASS_EXTERNAL;
    acquire.dstSubpass = 0;
    acquire.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    acquire.srcAccessMask = 0;
    acquire.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    acquire.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    m_renderPass = RenderPassBuilder()
                       .color(m_colorFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                              VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                       .dependency(acquire)
                       .build(m_context.device());
}

void VulkanSwapchain::createFramebuffers() {
    m_framebuffers.clear();
    m_framebuffers.reserve(m_imageViews.size());
    for (const UniqueImageView& view : m_imageViews) {
        const VkImageView attachment = view.get();
        m_framebuffers.push_back(
            createFramebuffer(m_context.device(), m_renderPass.get(), &attachment, 1, m_extent));
    }
}

} // namespace pose
