/**
 * @file hdrtarget.cpp
 * @brief Implementation of HdrTarget. See hdrtarget.h.
 */

#include "hdrtarget.h"

#include "renderpassbuilder.h"
#include "samplercache.h"
#include "vulkancommon.h"
#include "vulkancontext.h"

namespace pose {

namespace {
// D32 is mandatory optimal-tiling depth-attachment support in every Vulkan implementation, so
// no format probing is needed for an offscreen depth target.
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
} // namespace

HdrTarget::HdrTarget(VulkanContext& context, VkExtent2D extent, bool storeDepth)
    : m_context(context), m_extent(extent), m_storeDepth(storeDepth) {
    const VkSampleCountFlagBits samples = m_context.sampleCount();
    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;

    // Render pass: TWO colour attachments in RGBA16F, both ending SHADER_READ_ONLY for the post
    // passes. Colour 0 = the scene (diffuse for the PBR mode — the SSS blur diffuses it in
    // place); colour 1 = the PBR mode's SPECULAR, which the blur must never smear (smeared glints
    // read as a wet film on skin) — the SSS V-pass adds it back after diffusing. Under MSAA each
    // colour attachment is a transient multisampled target resolved into its own single-sample
    // image; single-sample, the colour images are stored and sampled directly. Attachment order
    // (see attachmentCount()): colourMS(0)/depth(1)/resolve(2)/specMS(3)/specResolve(4), or
    // colour(0)/depth(1)/spec(2).
    const VkAttachmentStoreOp colorStore = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                               : VK_ATTACHMENT_STORE_OP_STORE;
    const VkImageLayout colorFinal = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    RenderPassBuilder pass;
    pass.color(kColorFormat, samples, VK_ATTACHMENT_LOAD_OP_CLEAR, colorStore, colorFinal);
    // The depth is discarded after the pass — unless it is kept for VR Mode's depth resolve
    // (storeDepth): stored, and left readable for the resolve's sampler.
    pass.depth(kDepthFormat, samples, VK_ATTACHMENT_LOAD_OP_CLEAR,
               m_storeDepth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
               m_storeDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                            : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    if (msaa) {
        pass.resolve(kColorFormat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    pass.color(kColorFormat, samples, VK_ATTACHMENT_LOAD_OP_CLEAR, colorStore, colorFinal); // spec
    if (msaa) {
        pass.resolve(kColorFormat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    // The resolved images are sampled by the post passes, and the depth image is re-cleared
    // every frame: the sampled-target contract WITH its depth half (see renderpassbuilder.h).
    pass.dependencies(sampledTargetDependencies(/*withDepth=*/true));
    m_renderPass = pass.build(m_context.device());
    m_attachmentCount = pass.attachmentCount();

    m_sampler = m_context.samplers().get(SamplerDesc::linearClamp());
    if (m_storeDepth) {
        // The depth is fetched sample by sample (texelFetch): a nearest, non-comparison sampler.
        m_depthSampler = m_context.samplers().get(SamplerDesc::shadowRaw());
    }
    createImages();
}

void HdrTarget::resize(VkExtent2D extent) {
    m_extent = extent;
    destroyImages();
    createImages();
}

void HdrTarget::createImages() {
    const VkSampleCountFlagBits samples = m_context.sampleCount();
    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;

    // MSAA colour: transient (tile-local on many GPUs, memoryless where the driver allows); when
    // single-sample it IS the sampleable result. The specular target mirrors the colour target.
    const VkImageUsageFlags colorUsage =
        msaa ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
             : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    constexpr VkImageUsageFlags kResolveUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    m_color = AttachmentImage(m_context, kColorFormat, m_extent, samples, colorUsage,
                              VK_IMAGE_ASPECT_COLOR_BIT);
    const VkImageUsageFlags depthUsage =
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | (m_storeDepth ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u);
    m_depth = AttachmentImage(m_context, kDepthFormat, m_extent, samples, depthUsage, VK_IMAGE_ASPECT_DEPTH_BIT);
    m_spec = AttachmentImage(m_context, kColorFormat, m_extent, samples, colorUsage,
                             VK_IMAGE_ASPECT_COLOR_BIT);
    if (msaa) {
        m_resolve = AttachmentImage(m_context, kColorFormat, m_extent, VK_SAMPLE_COUNT_1_BIT,
                                    kResolveUsage, VK_IMAGE_ASPECT_COLOR_BIT);
        m_specResolve = AttachmentImage(m_context, kColorFormat, m_extent, VK_SAMPLE_COUNT_1_BIT,
                                        kResolveUsage, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    // Attachment order matches the render pass: MSAA = colourMS/depth/resolve/specMS/specResolve;
    // single-sample = colour/depth/spec.
    const VkImageView views[5] = {m_color.view(), m_depth.view(),
                                  msaa ? m_resolve.view() : m_spec.view(), m_spec.view(),
                                  m_specResolve.view()};
    m_framebuffer = createFramebuffer(m_context.device(), m_renderPass.get(), views,
                                      m_attachmentCount, m_extent);
}

void HdrTarget::destroyImages() {
    m_framebuffer.reset();
    m_specResolve.reset();
    m_spec.reset();
    m_resolve.reset();
    m_depth.reset();
    m_color.reset();
}

VkDescriptorImageInfo HdrTarget::depthInfo() const {
    VkDescriptorImageInfo info{};
    info.sampler = m_depthSampler;
    info.imageView = m_depth.view();
    info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    return info;
}

VkDescriptorImageInfo HdrTarget::resolveInfo() const {
    VkDescriptorImageInfo info{};
    info.sampler = m_sampler;
    // Single-sample fallback: there is no resolve image — the colour attachment itself is
    // stored (storeOp STORE, final layout SHADER_READ_ONLY) and is the sampleable result, so the
    // "resolve" descriptor aliases the colour view. Callers never need to know which case holds.
    info.imageView = m_resolve ? m_resolve.view() : m_color.view();
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return info;
}

VkDescriptorImageInfo HdrTarget::specResolveInfo() const {
    VkDescriptorImageInfo info{};
    info.sampler = m_sampler;
    info.imageView = m_specResolve ? m_specResolve.view() : m_spec.view(); // same aliasing as above
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return info;
}

} // namespace pose
