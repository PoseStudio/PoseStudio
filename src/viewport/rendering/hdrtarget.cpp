/**
 * @file hdrtarget.cpp
 * @brief Implementation of HdrTarget. See hdrtarget.h.
 */

#include "hdrtarget.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "descriptorutil.h"
#include "renderpassbuilder.h"
#include "samplercache.h"
#include "vulkancommon.h"
#include "vulkancontext.h"
#include "vulkanpipeline.h"

namespace pose {

namespace {
// D32 is mandatory optimal-tiling depth-attachment support in every Vulkan implementation, so
// no format probing is needed for an offscreen depth target.
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

constexpr VkPipelineStageFlags kDepthStages =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

/// A pass whose depth attachment is READ by the depth test after another pass WROTE it (the
/// photo pass after the resolve — or, single-sample, after the scene pass): the writes must be
/// done and visible before the tests read.
VkSubpassDependency depthWrittenBeforeRead() {
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = kDepthStages;
    dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstStageMask = kDepthStages;
    dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    return dep;
}

/// The resolve pass's pair: (0) the previous frame's photo-pass reads and resolve writes of the
/// photo depth are done before this pass clears and writes it; (1) this pass's writes are
/// visible to the photo pass's depth test. (Its SAMPLED reads of the scene's MSAA depth are
/// ordered by the scene pass's own outgoing dependency, as the depth layer's resolve relies on.)
std::array<VkSubpassDependency, 2> resolveDependencies() {
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = kDepthStages;
    deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstStageMask = kDepthStages;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = kDepthStages;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = kDepthStages;
    deps[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    return deps;
}

void setViewport(VkCommandBuffer cmd, VkExtent2D extent) {
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

/// depthresolve.frag's push block: x = the sample count, y = the scene texels per photo texel
/// (the layer at a reduced scale takes the nearest sample over the block that maps to it).
struct ResolvePush {
    float samples;
    float ratio;
    float pad0;
    float pad1;
};

VkExtent2D scaledExtent(VkExtent2D extent, float scale) {
    const auto dim = [scale](uint32_t v) {
        return std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(v) * scale)));
    };
    return VkExtent2D{dim(extent.width), dim(extent.height)};
}
} // namespace

HdrTarget::HdrTarget(VulkanContext& context, VkExtent2D extent, const std::vector<char>& fullscreenVertSpirv,
                     const std::vector<char>& depthResolveFragSpirv, float photoScale)
    : m_context(context), m_extent(extent), m_photoScale(std::clamp(photoScale, 0.25f, 1.0f)) {
    const VkSampleCountFlagBits samples = m_context.sampleCount();
    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;
    if (!msaa) {
        m_photoScale = 1.0f; // the scene's own depth image is the photo pass's attachment: its size
    }

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
    // The depth is KEPT after the pass and left readable: the photo pass's resolve samples it
    // (or, single-sample, loads it as its own attachment), and VR Mode's depth layer resolves it
    // for the headset.
    pass.depth(kDepthFormat, samples, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
               VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
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
    // The depth is fetched sample by sample (texelFetch): a nearest, non-comparison sampler.
    m_depthSampler = m_context.samplers().get(SamplerDesc::shadowRaw());

    // THE PHOTO PASS: the layer (cleared to coverage 0, kept, sampled by the composite) over a
    // read-only single-sample depth — the resolved scene depth, or the scene's own image. Its
    // colour is the sampled-target contract; its depth was written by the pass before it.
    RenderPassBuilder photo;
    photo.color(kColorFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    photo.depthReadOnly(kDepthFormat, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    photo.dependencies(sampledTargetDependencies(/*withDepth=*/false));
    photo.dependency(depthWrittenBeforeRead());
    m_photoPass = photo.build(m_context.device());

    if (msaa) {
        // The resolve: a depth-only single-sample pass over the photo depth, cleared to 1.0, in
        // which the fullscreen triangle writes the nearest of the scene depth's samples per
        // texel (depth test LESS + write: a far-plane fragment leaves the clear, every nearer
        // one lands) — the depth layer's resolve, built against this pass.
        RenderPassBuilder resolve;
        resolve.depth(kDepthFormat, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        resolve.dependencies(resolveDependencies());
        m_resolvePass = resolve.build(m_context.device());

        const VkDevice device = m_context.device();
        m_resolveSetLayout = createDescriptorSetLayout(
            device, {descriptorBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)});
        m_resolvePool = createDescriptorPool(device, {poolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1)}, 1);
        m_resolveSet = allocateDescriptorSet(device, m_resolvePool.get(), m_resolveSetLayout.get());

        PipelineConfig config;
        config.pushConstantSize = sizeof(ResolvePush);
        config.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT;
        config.depthTestEnable = true;
        config.depthWriteEnable = true;
        config.blendEnable = false;
        config.singleSample = true;
        config.hasColorAttachment = false;
        config.descriptorSetLayouts = {m_resolveSetLayout.get()};
        m_resolvePipeline = std::make_unique<VulkanPipeline>(m_context, m_resolvePass.get(), fullscreenVertSpirv,
                                                             depthResolveFragSpirv, config);
    }
    createImages();
}

HdrTarget::~HdrTarget() = default;

void HdrTarget::resize(VkExtent2D extent) {
    m_extent = extent;
    destroyImages();
    createImages();
}

void HdrTarget::createImages() {
    const VkSampleCountFlagBits samples = m_context.sampleCount();
    const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;
    const VkDevice device = m_context.device();

    // MSAA colour: transient (tile-local on many GPUs, memoryless where the driver allows); when
    // single-sample it IS the sampleable result. The specular target mirrors the colour target.
    const VkImageUsageFlags colorUsage =
        msaa ? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
             : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    constexpr VkImageUsageFlags kResolveUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    m_color = AttachmentImage(m_context, kColorFormat, m_extent, samples, colorUsage,
                              VK_IMAGE_ASPECT_COLOR_BIT);
    // The depth is sampled by the resolves (the photo pass's, the depth layer's).
    m_depth = AttachmentImage(m_context, kDepthFormat, m_extent, samples,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT);
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
    std::array<VkImageView, 5> views{};
    if (msaa) {
        views = {m_color.view(), m_depth.view(), m_resolve.view(), m_spec.view(), m_specResolve.view()};
    } else {
        views = {m_color.view(), m_depth.view(), m_spec.view()};
    }
    m_framebuffer = createFramebuffer(device, m_renderPass.get(), views.data(), m_attachmentCount, m_extent);

    // The photo layer, at the SCENE's size whatever the scale (the scale picks the sub-rectangle
    // drawn and sampled, so it can move between frames: setPhotoScale): cleared to coverage 0
    // and moved to the read layout once — the composite samples it every frame, the pass runs
    // only with splats in the scene (a fresh image holds garbage, not zero, and an undrawn
    // layer must read as nothing over the scene).
    m_photoExtent = scaledExtent(m_extent, m_photoScale);
    m_photo = AttachmentImage(m_context, kColorFormat, m_extent, VK_SAMPLE_COUNT_1_BIT,
                              kResolveUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    initialiseToShaderReadCleared(m_context, {m_photo.image()});
    m_photoDrawn = false;
    if (msaa) {
        // The photo depth: written by the resolve, read by the photo pass, which LOADS it in the
        // read-only layout — so it starts there, before any resolve has run.
        m_photoDepth = AttachmentImage(m_context, kDepthFormat, m_extent, VK_SAMPLE_COUNT_1_BIT,
                                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        initialiseDepthReadOnly(m_context, m_photoDepth.image());
        const VkImageView resolveView = m_photoDepth.view();
        m_resolveFramebuffer = createFramebuffer(device, m_resolvePass.get(), &resolveView, 1, m_extent);
        const VkDescriptorImageInfo sceneDepth = depthInfo();
        writeCombinedImageSamplers(device, m_resolveSet, 0, &sceneDepth, 1);
    }
    const VkImageView photoViews[2] = {m_photo.view(), msaa ? m_photoDepth.view() : m_depth.view()};
    m_photoFramebuffer = createFramebuffer(device, m_photoPass.get(), photoViews, 2, m_extent);
}

void HdrTarget::setPhotoScale(float scale) {
    if (m_context.sampleCount() == VK_SAMPLE_COUNT_1_BIT) {
        return; // the scene's own depth image is the attachment: the layer stays its size
    }
    m_photoScale = std::clamp(scale, 0.25f, 1.0f);
    m_photoExtent = scaledExtent(m_extent, m_photoScale);
}

void HdrTarget::photoUvScale(float out[2]) const {
    out[0] = static_cast<float>(m_photoExtent.width) / static_cast<float>(m_extent.width);
    out[1] = static_cast<float>(m_photoExtent.height) / static_cast<float>(m_extent.height);
}

void HdrTarget::destroyImages() {
    m_photoFramebuffer.reset();
    m_resolveFramebuffer.reset();
    m_photoDepth.reset();
    m_photo.reset();
    m_framebuffer.reset();
    m_specResolve.reset();
    m_spec.reset();
    m_resolve.reset();
    m_depth.reset();
    m_color.reset();
}

void HdrTarget::recordPhotoPass(VkCommandBuffer cmd, const std::function<void()>& draw) {
    if (!draw && !m_photoDrawn) {
        return; // nothing to draw and nothing to clear
    }
    if (draw && m_resolvePipeline) {
        VkClearValue clear{};
        clear.depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = m_resolvePass.get();
        rp.framebuffer = m_resolveFramebuffer.get();
        rp.renderArea.extent = m_photoExtent;
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        setViewport(cmd, m_photoExtent);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_resolvePipeline->handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_resolvePipeline->layout(), 0, 1,
                                &m_resolveSet, 0, nullptr);
        const ResolvePush push{static_cast<float>(m_context.sampleCount()),
                               static_cast<float>(m_extent.width) / static_cast<float>(m_photoExtent.width), 0.0f,
                               0.0f};
        vkCmdPushConstants(cmd, m_resolvePipeline->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // The pass: the layer cleared to coverage 0 (the depth's clear value is owed by the array,
    // not used — the attachment loads), the splats drawn into it. The render area is the
    // scale's sub-rectangle of the full-size framebuffer: the clear and the draws stay inside
    // it, and the composite samples only that much of the layer.
    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_photoPass.get();
    rp.framebuffer = m_photoFramebuffer.get();
    rp.renderArea.extent = m_photoExtent;
    rp.clearValueCount = static_cast<uint32_t>(clears.size());
    rp.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    setViewport(cmd, m_photoExtent);
    if (draw) {
        draw();
    }
    vkCmdEndRenderPass(cmd);
    m_photoDrawn = static_cast<bool>(draw);
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

VkDescriptorImageInfo HdrTarget::photoInfo() const {
    VkDescriptorImageInfo info{};
    info.sampler = m_sampler;
    info.imageView = m_photo.view();
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
