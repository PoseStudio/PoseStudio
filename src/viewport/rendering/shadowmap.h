/**
 * @file shadowmap.h
 * @brief The key light's shadow map: a depth-only offscreen target + the comparison sampler
 *        the lit shaders test against.
 *
 * Owns the D32 depth image, its single-sample depth-only render pass (independent of the MSAA
 * swapchain pass), the framebuffer, and a comparison (PCF) sampler. Scene records the depth-only
 * pass into it each frame (recordShadowPass) before the main pass samples it — the mesh shader
 * through set 3 binding 2 (shadowing the key light on the figure), the grid shader through the
 * same set (the ground/contact shadow). The sampler clamps to an opaque-white border, so any
 * receiver outside the fitted light frustum simply reads "unshadowed". At creation the image is
 * cleared to 1.0 and moved to the read layout, so frames before the first shadow pass (or with
 * no casters, when the pass is skipped) sample a defined, fully-lit map.
 *
 * Like the rest of rendering/, pure Vulkan + std — no Qt.
 */

#ifndef SHADOWMAP_H
#define SHADOWMAP_H

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace pose {

class VulkanContext;

class ShadowMap {
public:
    explicit ShadowMap(VulkanContext& context, uint32_t size = 2048);
    ~ShadowMap();

    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;

    VkRenderPass  renderPass() const { return m_renderPass; }
    VkFramebuffer framebuffer() const { return m_framebuffer; }
    uint32_t      size() const { return m_size; }

    /// Combined-image-sampler descriptor info (read layout + the comparison sampler) for the
    /// shaders' `sampler2DShadow` binding (set 3 binding 2 in the mesh path; the grid shares it).
    VkDescriptorImageInfo descriptorInfo() const;

    /// Same image through the NON-comparison sampler (set 3 binding 3): raw depth values, for the
    /// ground shadow's PCSS blocker search (a comparison sampler can only answer lit/unlit, not
    /// "how far above the floor is the caster"). Border reads 1.0 = empty/far, i.e. no blocker.
    VkDescriptorImageInfo rawDescriptorInfo() const;

private:
    VulkanContext& m_context;
    uint32_t       m_size = 0;
    VkImage        m_image = VK_NULL_HANDLE;
    VmaAllocation  m_allocation = VK_NULL_HANDLE;
    VkImageView    m_view = VK_NULL_HANDLE;
    VkSampler      m_sampler = VK_NULL_HANDLE;
    VkSampler      m_rawSampler = VK_NULL_HANDLE; // no compare: raw depths for the blocker search
    VkRenderPass   m_renderPass = VK_NULL_HANDLE;
    VkFramebuffer  m_framebuffer = VK_NULL_HANDLE;
};

} // namespace pose

#endif // SHADOWMAP_H
