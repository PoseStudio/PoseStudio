/**
 * @file hdrtarget.h
 * @brief The offscreen HDR scene target: MSAA RGBA16F colour + depth, resolved to a sampleable
 *        single-sample HDR texture.
 *
 * The scene (backdrop, meshes, grid, overlays) renders here instead of straight into the
 * swapchain, in LINEAR HDR — the PBR shader no longer tonemaps inline. The resolved texture then
 * feeds the post-processing chain (bloom bright-extract, and the fullscreen composite that
 * tonemaps into the swapchain). The render pass mirrors the swapchain's MSAA structure
 * (colour(0)/depth(1)/resolve(2), same sample count) so every scene pipeline builds against this
 * pass unchanged; it depends only on formats, so resize() rebuilds images + framebuffer but keeps
 * the pass — scene pipelines never rebuild. Pure Vulkan + std, no Qt.
 */

#ifndef HDRTARGET_H
#define HDRTARGET_H

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace pose {

class VulkanContext;

class HdrTarget {
public:
    HdrTarget(VulkanContext& context, VkExtent2D extent);
    ~HdrTarget();

    HdrTarget(const HdrTarget&) = delete;
    HdrTarget& operator=(const HdrTarget&) = delete;

    /// Recreates the images/views/framebuffer at @p extent (the render pass is kept). The caller
    /// must have made the device idle (the renderer's resize path already does).
    void resize(VkExtent2D extent);

    VkRenderPass  renderPass() const { return m_renderPass; }
    VkFramebuffer framebuffer() const { return m_framebuffer; }
    VkExtent2D    extent() const { return m_extent; }

    /// The resolved (single-sample) HDR texture, in SHADER_READ_ONLY layout after the pass.
    VkDescriptorImageInfo resolveInfo() const;

    static constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

private:
    void createImages();
    void destroyImages();

    VulkanContext& m_context;
    VkExtent2D     m_extent{};
    VkRenderPass   m_renderPass = VK_NULL_HANDLE;
    VkSampler      m_sampler = VK_NULL_HANDLE;

    // MSAA colour (transient) + MSAA depth + single-sample resolve target.
    VkImage       m_colorImage = VK_NULL_HANDLE;
    VmaAllocation m_colorAlloc = VK_NULL_HANDLE;
    VkImageView   m_colorView = VK_NULL_HANDLE;
    VkImage       m_depthImage = VK_NULL_HANDLE;
    VmaAllocation m_depthAlloc = VK_NULL_HANDLE;
    VkImageView   m_depthView = VK_NULL_HANDLE;
    VkImage       m_resolveImage = VK_NULL_HANDLE;
    VmaAllocation m_resolveAlloc = VK_NULL_HANDLE;
    VkImageView   m_resolveView = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
};

} // namespace pose

#endif // HDRTARGET_H
