// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file outlinemask.cpp
 * @brief Implementation of OutlineMask. See outlinemask.h.
 */

#include "outlinemask.h"

#include "renderpassbuilder.h"
#include "samplercache.h"
#include "vulkancommon.h"
#include "vulkancontext.h"

namespace pose {

OutlineMask::OutlineMask(VulkanContext& context, VkExtent2D extent)
    : m_context(context), m_extent(extent) {
    const VkSampleCountFlagBits samples = m_context.sampleCount();
    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;

    // Render pass: one coverage attachment at the context's MSAA count (cleared to 0 = "not the
    // selected object"), resolved into a single-sample image left SHADER_READ_ONLY for the
    // composite. No depth: the mask is the object's full projected silhouette. Single-sample
    // fallback: the one attachment is stored and sampled directly. The previous frame's
    // composite read must finish before this pass overwrites the mask, and this pass's writes
    // must land before this frame's composite samples it — the sampled-target contract.
    RenderPassBuilder pass;
    pass.color(kFormat, samples, VK_ATTACHMENT_LOAD_OP_CLEAR,
               msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
               msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (msaa) {
        pass.resolve(kFormat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    pass.dependencies(sampledTargetDependencies(/*withDepth=*/false));
    m_renderPass = pass.build(m_context.device());
    m_attachmentCount = pass.attachmentCount();

    // Linear filtering gives the composite's dilation taps a soft transition across the mask's
    // edge (the outer edge of the outline); clamp-to-edge means an object cut by the viewport
    // border gets no outline along the cut, the way it has no silhouette there.
    m_sampler = m_context.samplers().get(SamplerDesc::linearClamp());

    createImages();
}

void OutlineMask::resize(VkExtent2D extent) {
    m_extent = extent;
    destroyImages();
    createImages();
}

void OutlineMask::createImages() {
    const VkSampleCountFlagBits samples = m_context.sampleCount();
    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;

    // MSAA coverage: transient (tile-local on many GPUs) and resolved; single-sample: the
    // coverage image is the sampleable result itself.
    m_color = AttachmentImage(
        m_context, kFormat, m_extent, samples,
        msaa ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
             : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
        VK_IMAGE_ASPECT_COLOR_BIT);
    if (msaa) {
        m_resolve = AttachmentImage(m_context, kFormat, m_extent, VK_SAMPLE_COUNT_1_BIT,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
    }

    const VkImageView views[2] = {m_color.view(), m_resolve.view()};
    m_framebuffer = createFramebuffer(m_context.device(), m_renderPass.get(), views,
                                      m_attachmentCount, m_extent);

    // Move the sampled image to the read layout once: the composite binds it every frame, but
    // the mask pass only runs while something is selected — a fresh image that no pass has
    // rendered yet would otherwise be bound in UNDEFINED layout (a validation violation, and UB
    // on hardware). Construction/resize run on an idle device, so the blocking submit is fine.
    initialiseToShaderRead(m_context, {msaa ? m_resolve.image() : m_color.image()});
}

void OutlineMask::destroyImages() {
    m_framebuffer.reset();
    m_resolve.reset();
    m_color.reset();
}

VkDescriptorImageInfo OutlineMask::descriptorInfo() const {
    VkDescriptorImageInfo info{};
    info.sampler = m_sampler;
    info.imageView = m_resolve ? m_resolve.view() : m_color.view(); // single-sample: the coverage image
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return info;
}

} // namespace pose
