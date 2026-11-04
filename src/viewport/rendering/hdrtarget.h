/**
 * @file hdrtarget.h
 * @brief The offscreen HDR scene target: MSAA RGBA16F colour + depth, resolved to a sampleable
 *        single-sample HDR texture.
 *
 * The scene (backdrop, meshes, grid, overlays) renders here instead of straight into the
 * swapchain, in LINEAR HDR — the PBR shader no longer tonemaps inline. The resolved texture then
 * feeds the post-processing chain (the screen-space SSS blur, bloom bright-extract, and the
 * fullscreen composite that tonemaps into the swapchain). The pass carries TWO colour
 * attachments — the scene (diffuse in the PBR mode, whose alpha is the SSS mask) and the PBR
 * opaque pass's SPECULAR, kept apart so the SSS blur can never smear glints into a wet film —
 * each with its own single-sample resolve, plus a depth attachment, all at the context's MSAA
 * count; every scene pipeline builds against this pass. It depends only on formats, so
 * resize() rebuilds images + framebuffer but keeps the pass — scene pipelines never rebuild.
 * Pure Vulkan + std, no Qt.
 */

#ifndef HDRTARGET_H
#define HDRTARGET_H

#include "attachmentimage.h"
#include "vulkanhandles.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace pose {

class VulkanContext;

class HdrTarget {
public:
    /// @param storeDepth Keep the (multisampled) depth after the pass and make it sampleable —
    ///                   VR Mode's eye target, whose depth is resolved into the headset's depth
    ///                   swapchain for reprojection (depthInfo()). The desktop target discards it.
    HdrTarget(VulkanContext& context, VkExtent2D extent, bool storeDepth = false);
    ~HdrTarget() = default; // members clean up in reverse declaration order

    HdrTarget(const HdrTarget&) = delete;
    HdrTarget& operator=(const HdrTarget&) = delete;

    /// Recreates the images/views/framebuffer at @p extent (the render pass is kept). The caller
    /// must have made the device idle (the renderer's resize path already does).
    void resize(VkExtent2D extent);

    VkRenderPass  renderPass() const { return m_renderPass.get(); }
    VkFramebuffer framebuffer() const { return m_framebuffer.get(); }

    /// The resolved (single-sample) HDR texture, in SHADER_READ_ONLY layout after the pass.
    VkDescriptorImageInfo resolveInfo() const;

    /// The resolved SPECULAR texture (colour attachment 1) — the opaque mesh pass routes its
    /// specular here so the screen-space SSS blur (which runs on the main resolve) can never
    /// smear glints into a wet-looking film; the blur's V pass adds it back after diffusing.
    VkDescriptorImageInfo specResolveInfo() const;

    /// The depth attachment (at the context's MSAA count) through a NEAREST non-comparison
    /// sampler, in DEPTH_STENCIL_READ_ONLY layout after the pass — only with storeDepth.
    VkDescriptorImageInfo depthInfo() const;
    bool storesDepth() const { return m_storeDepth; }

    /// Number of attachments in the pass — the render-pass-begin clear array must cover them.
    /// Attachment order (the clear array follows it): MSAA = colourMS(0) / depth(1) /
    /// resolve(2) / specMS(3) / specResolve(4); single-sample = colour(0) / depth(1) / spec(2).
    uint32_t attachmentCount() const { return m_attachmentCount; }

    static constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

private:
    void createImages();
    void destroyImages();

    VulkanContext&   m_context;
    VkExtent2D       m_extent{};
    UniqueRenderPass m_renderPass;
    VkSampler        m_sampler = VK_NULL_HANDLE; // borrowed from the context's SamplerCache
    VkSampler        m_depthSampler = VK_NULL_HANDLE; // (storeDepth) nearest, borrowed
    bool             m_storeDepth = false;
    uint32_t         m_attachmentCount = 0;

    // MSAA colour + specular (transient) + MSAA depth + their single-sample resolve targets.
    // Single-sample fallback: the colour/spec images ARE the sampleable results (no resolves).
    AttachmentImage   m_color;
    AttachmentImage   m_depth;
    AttachmentImage   m_resolve;
    AttachmentImage   m_spec;
    AttachmentImage   m_specResolve;
    UniqueFramebuffer m_framebuffer; // last: it references the views above
};

} // namespace pose

#endif // HDRTARGET_H
