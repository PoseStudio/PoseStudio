// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file shadowmap.cpp
 * @brief Implementation of ShadowMap. See shadowmap.h.
 */

#include "shadowmap.h"

#include "renderpassbuilder.h"
#include "samplercache.h"
#include "vulkancommands.h"
#include "vulkancommon.h"
#include "vulkancontext.h"

namespace pose {

namespace {
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT; // mandatory depth-attachment support
} // namespace

ShadowMap::ShadowMap(VulkanContext& context, uint32_t size) : m_context(context), m_size(size) {
    // --- Depth-only render pass: clear -> raster depth -> leave in shader-read layout. The
    // previous frame's fragment reads must finish before this pass overwrites the map, and the
    // map's depth writes must finish before this frame samples it (the depth-target contract). ---
    m_renderPass = RenderPassBuilder()
                       .depth(kDepthFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                              VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)
                       .dependencies(sampledDepthTargetDependencies())
                       .build(m_context.device());

    // --- Depth image (sampled after the pass). Single-sample: PCF does the softening.
    // TRANSFER_DST: the one-time clear below. ---
    m_image = AttachmentImage(m_context, kDepthFormat, {m_size, m_size}, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT);

    // Comparison sampler: linear filter + compare = hardware PCF (each tap returns the fraction of
    // the bilinear footprint that passes ref <= texel, i.e. 1 = lit). Border is opaque WHITE so
    // receivers outside the fitted light frustum read fully lit rather than shadow-striped.
    m_sampler = m_context.samplers().get(SamplerDesc::shadowCompare());
    // Non-comparison twin (same border, no compare): raw depth values for the ground shadow's
    // PCSS blocker search. NEAREST filter — averaging depths across a caster's silhouette edge
    // would invent phantom blocker distances between the caster and the background.
    m_rawSampler = m_context.samplers().get(SamplerDesc::shadowRaw());

    const VkImageView view = m_image.view();
    m_framebuffer = createFramebuffer(m_context.device(), m_renderPass.get(), &view, 1, {m_size, m_size});

    // One-time init: clear to 1.0 ("everything lit") and move to the read layout, so the map is
    // well-defined for any frame that samples it before the first shadow pass runs.
    const VkImageSubresourceRange range = wholeImageRange(VK_IMAGE_ASPECT_DEPTH_BIT);
    submitImmediate(m_context, [&](VkCommandBuffer cmd) {
        transitionImage(cmd, m_image.image(), range, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearDepthStencilValue clear{1.0f, 0};
        vkCmdClearDepthStencilImage(cmd, m_image.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear,
                                    1, &range);
        transitionImage(cmd, m_image.image(), range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_SHADER_READ_BIT);
    });
}

VkDescriptorImageInfo ShadowMap::descriptorInfo() const {
    VkDescriptorImageInfo info{};
    info.sampler = m_sampler;
    info.imageView = m_image.view();
    info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    return info;
}

VkDescriptorImageInfo ShadowMap::rawDescriptorInfo() const {
    VkDescriptorImageInfo info = descriptorInfo();
    info.sampler = m_rawSampler;
    return info;
}

} // namespace pose
