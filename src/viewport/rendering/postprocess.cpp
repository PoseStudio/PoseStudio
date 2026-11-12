/**
 * @file postprocess.cpp
 * @brief Implementation of PostProcess. See postprocess.h.
 */

#include "postprocess.h"

#include "descriptorutil.h"
#include "hdrtarget.h"
#include "renderpassbuilder.h"
#include "samplercache.h"
#include "vulkancommon.h"
#include "vulkancontext.h"
#include "vulkanpipeline.h"

#include <algorithm>
#include <array>

namespace pose {

namespace {

// Push-constant block shared by all post shaders (see fullscreen.vert consumers):
//   bright:    x = threshold
//   blur:      xy = texel size, zw = blur direction
//   sss:       xy = texel size, zw = blur direction (H then V; V adds the spec attachment back)
//   composite: x = tonemap (0/1), y = bloom strength, z = bloom on (0/1),
//              w = selection-outline width in pixels (0 = none); outline.rgb = its linear color,
//              outline.w = encode sRGB in the shader (0/1); uvRect = the UV crop (scale.xy,
//              offset.xy — identity for the desktop's own composite, see CompositeOptions)
// The bright/blur/sss shaders declare only the first vec4 — a shader block may be a prefix of
// the pipeline's push range.
struct PostPush {
    float params[4];
    float outline[4];
    float uvRect[4];
    float photo[4]; // xy = the photo layer's UV scale (CompositeOptions::photoUvScale); z = the
                    // outline mask is a splat scene's opacity; w = the outline's opacity
    float edge[4];  // x = the edge blend's mode (CompositeOptions::edgeMode: 0 off, 1, 2); yzw spare
};

/// Bindings of the one post set layout (composite.frag reads all six; every other pass a prefix).
constexpr uint32_t kPostBindings = 6;

} // namespace

PostProcess::PostProcess(VulkanContext& context, VkRenderPass swapchainRenderPass,
                         const VkDescriptorImageInfo& hdrResolve,
                         const VkDescriptorImageInfo& hdrSpecResolve,
                         const VkDescriptorImageInfo& hdrPhotoResolve,
                         const VkDescriptorImageInfo& hdrCoverage,
                         const VkDescriptorImageInfo& hdrCoverageFull,
                         const VkDescriptorImageInfo& outlineMask, VkExtent2D extent,
                         const std::vector<char>& fullscreenVertSpirv,
                         const std::vector<char>& brightFragSpirv,
                         const std::vector<char>& blurFragSpirv,
                         const std::vector<char>& compositeFragSpirv,
                         const std::vector<char>& sssBlurFragSpirv)
    : m_context(context), m_extent(extent), m_hdrResolveView(hdrResolve.imageView) {
    VkDevice device = m_context.device();

    // --- Bloom render pass: one single-sample RGBA16F attachment, loaded DONT_CARE (the
    // fullscreen draw overwrites every pixel) and left sampleable. The chain ping-pongs
    // A -> B -> A with each pass sampling the previous one's output: the sampled-target contract
    // orders reads before overwrites and writes before reads, both directions. ------------------
    m_bloomPass = RenderPassBuilder()
                      .color(HdrTarget::kColorFormat, VK_SAMPLE_COUNT_1_BIT,
                             VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                      .dependencies(sampledTargetDependencies(/*withDepth=*/false))
                      .build(device);

    m_sampler = m_context.samplers().get(SamplerDesc::linearClamp());

    // --- Descriptors: one 6-sampler layout, seven sets (bright/blurH/blurV/composite/sssH/sssV
    // + the mirror composite over another chain's images, see setMirrorSource). ----------------
    m_setLayout = createDescriptorSetLayout(
        device,
        uniformBindings(kPostBindings, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT));
    // (+ an eighth for VR Mode's depth resolve over the eye target's MSAA depth.)
    m_pool = createDescriptorPool(device, {poolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * kPostBindings)},
                                  8);
    std::array<VkDescriptorSet, 8> sets{};
    allocateDescriptorSets(device, m_pool.get(), m_setLayout.get(), static_cast<uint32_t>(sets.size()),
                           sets.data());
    m_brightSet = sets[0];
    m_blurHSet = sets[1];
    m_blurVSet = sets[2];
    m_compositeSet = sets[3];
    m_sssHSet = sets[4];
    m_sssVSet = sets[5];
    m_mirrorSet = sets[6];
    m_depthResolveSet = sets[7];

    // --- Pipelines. All single-sample: bright/blur/SSS target the bloom pass, the composite the
    // swapchain's single-sample present pass (the scene's MSAA lives in the HDR target). --------
    PipelineConfig postConfig;
    postConfig.pushConstantSize = sizeof(PostPush);
    postConfig.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT;
    postConfig.depthTestEnable = false;
    postConfig.depthWriteEnable = false;
    postConfig.blendEnable = false;
    postConfig.descriptorSetLayouts = {m_setLayout.get()};
    postConfig.singleSample = true;
    m_brightPipeline = std::make_unique<VulkanPipeline>(m_context, m_bloomPass.get(), fullscreenVertSpirv,
                                                        brightFragSpirv, postConfig);
    m_blurPipeline = std::make_unique<VulkanPipeline>(m_context, m_bloomPass.get(), fullscreenVertSpirv,
                                                      blurFragSpirv, postConfig);
    m_sssPipeline = std::make_unique<VulkanPipeline>(m_context, m_bloomPass.get(), fullscreenVertSpirv,
                                                     sssBlurFragSpirv, postConfig);
    m_compositePipeline = std::make_unique<VulkanPipeline>(
        m_context, swapchainRenderPass, fullscreenVertSpirv, compositeFragSpirv, postConfig);

    createTargets();
    updateDescriptors(hdrResolve, hdrSpecResolve, hdrPhotoResolve, hdrCoverage, hdrCoverageFull, outlineMask);
}

PostProcess::~PostProcess() = default; // members clean up in reverse declaration order

void PostProcess::resize(VkExtent2D extent, const VkDescriptorImageInfo& hdrResolve,
                         const VkDescriptorImageInfo& hdrSpecResolve,
                         const VkDescriptorImageInfo& hdrPhotoResolve,
                         const VkDescriptorImageInfo& hdrCoverage,
                         const VkDescriptorImageInfo& hdrCoverageFull,
                         const VkDescriptorImageInfo& outlineMask) {
    m_extent = extent;
    m_hdrResolveView = hdrResolve.imageView;
    destroyTargets();
    createTargets();
    updateDescriptors(hdrResolve, hdrSpecResolve, hdrPhotoResolve, hdrCoverage, hdrCoverageFull, outlineMask);
}

void PostProcess::createTargets() {
    VkDevice device = m_context.device();
    m_bloomExtent = {std::max(m_extent.width / 2, 1u), std::max(m_extent.height / 2, 1u)};

    const auto makeTarget = [&](Target& target, VkExtent2D extent) {
        target.image = AttachmentImage(m_context, HdrTarget::kColorFormat, extent, VK_SAMPLE_COUNT_1_BIT,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
        const VkImageView view = target.image.view();
        target.framebuffer = createFramebuffer(device, m_bloomPass.get(), &view, 1, extent);
    };
    makeTarget(m_bloomA, m_bloomExtent);
    makeTarget(m_bloomB, m_bloomExtent);
    makeTarget(m_sssScratch, m_extent); // SSSSS works at full resolution

    // Initialise every target to SHADER_READ_ONLY once. The composite pass samples the bloom
    // chain unconditionally (its contribution is merely weighted to zero outside PBR mode), so a
    // freshly created target that the skipped bloom/SSS passes never rendered would otherwise be
    // sampled while still in UNDEFINED layout — a validation violation, and UB on hardware.
    // Repro without this: resize the window while in a stylized shade mode. (Runs only at
    // construction/resize, when the device is idle, so the blocking submit is fine.)
    initialiseToShaderRead(m_context,
                           {m_bloomA.image.image(), m_bloomB.image.image(), m_sssScratch.image.image()});

    // The SSSSS V pass writes straight back into the HDR resolve image (same format/usage — the
    // bloom pass object is compatible and size-agnostic).
    m_sssResolveFb = createFramebuffer(device, m_bloomPass.get(), &m_hdrResolveView, 1, m_extent);
}

void PostProcess::destroyTargets() {
    m_sssResolveFb.reset();
    for (Target* target : {&m_bloomA, &m_bloomB, &m_sssScratch}) {
        target->framebuffer.reset();
        target->image.reset();
    }
}

void PostProcess::updateDescriptors(const VkDescriptorImageInfo& hdrResolve,
                                    const VkDescriptorImageInfo& hdrSpecResolve,
                                    const VkDescriptorImageInfo& hdrPhotoResolve,
                                    const VkDescriptorImageInfo& hdrCoverage,
                                    const VkDescriptorImageInfo& hdrCoverageFull,
                                    const VkDescriptorImageInfo& outlineMask) {
    const VkDescriptorImageInfo aInfo = sampledImageInfo(m_bloomA.image.view(), m_sampler);
    const VkDescriptorImageInfo bInfo = sampledImageInfo(m_bloomB.image.view(), m_sampler);
    const VkDescriptorImageInfo sssInfo = sampledImageInfo(m_sssScratch.image.view(), m_sampler);

    // (set, binding, image): bright reads HDR; blurH reads A; blurV reads B; composite reads
    // HDR + A + the selection-outline mask + the PHOTO layer (the splats, laid over at the end)
    // + the scene's COVERAGE, per layer texel and per pixel (the edge blend); sssH reads HDR; sssV reads the SSS scratch + the SPECULAR resolve (added back after the
    // blur — the blur must never smear glints).
    //
    // The DUPLICATE-WRITE trick: every set uses the same six-binding layout, but most shaders
    // declare only binding 0 (or 0 and 1). Vulkan requires every binding a pipeline's layout
    // declares to hold a valid descriptor when the set is bound (VUID-vkCmdDraw-None-02699 and
    // kin), whether or not the shader reads it — so a pass's unused bindings are written with its
    // binding-0 image again rather than left uninitialised. One layout + one pool for six passes
    // in exchange for a few redundant writes.
    struct Entry {
        VkDescriptorSet              set;
        uint32_t                     binding;
        const VkDescriptorImageInfo* info;
    };
    // The mirror set follows the images it was pointed at (setMirrorSource) and, with none, this
    // chain's own composite inputs — valid either way, so a stray bind can never fault.
    const VkDescriptorImageInfo* mirrorHdr = m_hasMirror ? &m_mirrorInfos[0] : &hdrResolve;
    const VkDescriptorImageInfo* mirrorBloom = m_hasMirror ? &m_mirrorInfos[1] : &aInfo;
    const VkDescriptorImageInfo* mirrorOutline = m_hasMirror ? &m_mirrorInfos[2] : &outlineMask;
    const VkDescriptorImageInfo* mirrorPhoto = m_hasMirror ? &m_mirrorInfos[3] : &hdrPhotoResolve;
    const VkDescriptorImageInfo* mirrorCoverage = m_hasMirror ? &m_mirrorInfos[4] : &hdrCoverage;
    const VkDescriptorImageInfo* mirrorCoverageFull = m_hasMirror ? &m_mirrorInfos[5] : &hdrCoverageFull;
    // (The depth-resolve set is written by enableDepthResolve, from a target that never resizes;
    // until then it holds this chain's HDR image so a stray bind is valid.)
    const VkDescriptorImageInfo* depthSource = m_depthResolvePipeline ? nullptr : &hdrResolve;
    if (depthSource != nullptr) {
        const VkDescriptorImageInfo infos[kPostBindings] = {*depthSource, *depthSource, *depthSource,
                                                            *depthSource, *depthSource, *depthSource};
        writeCombinedImageSamplers(m_context.device(), m_depthResolveSet, 0, infos, kPostBindings);
    }
    const std::array<Entry, 7 * kPostBindings> entries{{{m_brightSet, 0, &hdrResolve},
                                                        {m_brightSet, 1, &hdrResolve},
                                                        {m_brightSet, 2, &hdrResolve},
                                                        {m_brightSet, 3, &hdrResolve},
                                                        {m_brightSet, 4, &hdrResolve},
                                                        {m_brightSet, 5, &hdrResolve},
                                                        {m_blurHSet, 0, &aInfo},
                                                        {m_blurHSet, 1, &aInfo},
                                                        {m_blurHSet, 2, &aInfo},
                                                        {m_blurHSet, 3, &aInfo},
                                                        {m_blurHSet, 4, &aInfo},
                                                        {m_blurHSet, 5, &aInfo},
                                                        {m_blurVSet, 0, &bInfo},
                                                        {m_blurVSet, 1, &bInfo},
                                                        {m_blurVSet, 2, &bInfo},
                                                        {m_blurVSet, 3, &bInfo},
                                                        {m_blurVSet, 4, &bInfo},
                                                        {m_blurVSet, 5, &bInfo},
                                                        {m_compositeSet, 0, &hdrResolve},
                                                        {m_compositeSet, 1, &aInfo},
                                                        {m_compositeSet, 2, &outlineMask},
                                                        {m_compositeSet, 3, &hdrPhotoResolve},
                                                        {m_compositeSet, 4, &hdrCoverage},
                                                        {m_compositeSet, 5, &hdrCoverageFull},
                                                        {m_sssHSet, 0, &hdrResolve},
                                                        {m_sssHSet, 1, &hdrResolve},
                                                        {m_sssHSet, 2, &hdrResolve},
                                                        {m_sssHSet, 3, &hdrResolve},
                                                        {m_sssHSet, 4, &hdrResolve},
                                                        {m_sssHSet, 5, &hdrResolve},
                                                        {m_sssVSet, 0, &sssInfo},
                                                        {m_sssVSet, 1, &hdrSpecResolve},
                                                        {m_sssVSet, 2, &sssInfo},
                                                        {m_sssVSet, 3, &sssInfo},
                                                        {m_sssVSet, 4, &sssInfo},
                                                        {m_sssVSet, 5, &sssInfo},
                                                        {m_mirrorSet, 0, mirrorHdr},
                                                        {m_mirrorSet, 1, mirrorBloom},
                                                        {m_mirrorSet, 2, mirrorOutline},
                                                        {m_mirrorSet, 3, mirrorPhoto},
                                                        {m_mirrorSet, 4, mirrorCoverage},
                                                        {m_mirrorSet, 5, mirrorCoverageFull}}};
    std::array<VkWriteDescriptorSet, 7 * kPostBindings> writes{};
    for (std::size_t i = 0; i < entries.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = entries[i].set;
        writes[i].dstBinding = entries[i].binding;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = entries[i].info;
    }
    vkUpdateDescriptorSets(m_context.device(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

void PostProcess::runFullscreenPass(VkCommandBuffer cmd, VkFramebuffer framebuffer,
                                    VkExtent2D extent, const VulkanPipeline& pipeline,
                                    VkDescriptorSet set, const float params[4]) {
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_bloomPass.get();
    rp.framebuffer = framebuffer;
    rp.renderArea.extent = extent;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1, &set, 0,
                            nullptr);
    PostPush push{};
    for (int i = 0; i < 4; ++i) {
        push.params[i] = params[i];
    }
    vkCmdPushConstants(cmd, pipeline.layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void PostProcess::setOutlineStyle(const glm::vec3& colorLinear, float opacity) {
    m_outlineColor[0] = colorLinear.x;
    m_outlineColor[1] = colorLinear.y;
    m_outlineColor[2] = colorLinear.z;
    m_outlineOpacity = std::clamp(opacity, 0.0f, 1.0f);
}

void PostProcess::recordSss(VkCommandBuffer cmd) {
    // Full-res masked blur, H into the scratch target, V back into the HDR resolve image — the
    // rest of the chain (bloom, composite) just sees the diffused image.
    const float texelX = 1.0f / static_cast<float>(m_extent.width);
    const float texelY = 1.0f / static_cast<float>(m_extent.height);
    const float pushH[4] = {texelX, texelY, 1.0f, 0.0f};
    const float pushV[4] = {texelX, texelY, 0.0f, 1.0f};
    runFullscreenPass(cmd, m_sssScratch.framebuffer.get(), m_extent, *m_sssPipeline, m_sssHSet, pushH);
    runFullscreenPass(cmd, m_sssResolveFb.get(), m_extent, *m_sssPipeline, m_sssVSet, pushV);
}

void PostProcess::recordBloom(VkCommandBuffer cmd) {
    const float texelX = 1.0f / static_cast<float>(m_bloomExtent.width);
    const float texelY = 1.0f / static_cast<float>(m_bloomExtent.height);
    constexpr float kBloomThreshold = 1.0f; // post-exposure luminance where bloom starts

    const float pushBright[4] = {kBloomThreshold, 0.0f, 0.0f, 0.0f};
    const float pushH[4] = {texelX, texelY, 1.0f, 0.0f};
    const float pushV[4] = {texelX, texelY, 0.0f, 1.0f};
    runFullscreenPass(cmd, m_bloomA.framebuffer.get(), m_bloomExtent, *m_brightPipeline, m_brightSet,
                      pushBright);
    runFullscreenPass(cmd, m_bloomB.framebuffer.get(), m_bloomExtent, *m_blurPipeline, m_blurHSet, pushH);
    runFullscreenPass(cmd, m_bloomA.framebuffer.get(), m_bloomExtent, *m_blurPipeline, m_blurVSet, pushV);
}

void PostProcess::recordComposite(VkCommandBuffer cmd, bool tonemap, bool bloom,
                                  float outlineWidthPx, const CompositeOptions& options) {
    constexpr float kBloomStrength = 0.35f;
    const PostPush push{{tonemap ? 1.0f : 0.0f, kBloomStrength, bloom ? 1.0f : 0.0f, outlineWidthPx},
                        {m_outlineColor[0], m_outlineColor[1], m_outlineColor[2], options.encodeSrgb ? 1.0f : 0.0f},
                        {options.uvScale[0], options.uvScale[1], options.uvOffset[0], options.uvOffset[1]},
                        {options.photoUvScale[0], options.photoUvScale[1], options.outlineFromSplats ? 1.0f : 0.0f,
                         m_outlineOpacity},
                        {options.fromMirror ? 0.0f : static_cast<float>(options.edgeMode), 0.0f, 0.0f, 0.0f}};
    const VkDescriptorSet set = (options.fromMirror && m_hasMirror) ? m_mirrorSet : m_compositeSet;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline->handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline->layout(), 0,
                            1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, m_compositePipeline->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void PostProcess::setMirrorSource(const VkDescriptorImageInfo& hdrResolve, const VkDescriptorImageInfo& bloom,
                                  const VkDescriptorImageInfo& outlineMask, const VkDescriptorImageInfo& photo,
                                  const VkDescriptorImageInfo& coverage,
                                  const VkDescriptorImageInfo& coverageFull) {
    m_mirrorInfos[0] = hdrResolve;
    m_mirrorInfos[1] = bloom;
    m_mirrorInfos[2] = outlineMask;
    m_mirrorInfos[3] = photo;
    m_mirrorInfos[4] = coverage;
    m_mirrorInfos[5] = coverageFull;
    m_hasMirror = true;
    writeCombinedImageSamplers(m_context.device(), m_mirrorSet, 0, m_mirrorInfos, kPostBindings);
}

void PostProcess::clearMirrorSource() {
    if (!m_hasMirror) {
        return;
    }
    m_hasMirror = false;
    // Point the set back at this chain's own images (they outlive the mirror's) so it stays valid.
    const VkDescriptorImageInfo aInfo = sampledImageInfo(m_bloomA.image.view(), m_sampler);
    const VkDescriptorImageInfo infos[kPostBindings] = {aInfo, aInfo, aInfo, aInfo, aInfo, aInfo};
    writeCombinedImageSamplers(m_context.device(), m_mirrorSet, 0, infos, kPostBindings);
}

VkDescriptorImageInfo PostProcess::bloomInfo() const {
    return sampledImageInfo(m_bloomA.image.view(), m_sampler);
}

void PostProcess::enableDepthResolve(VkRenderPass depthRenderPass, const VkDescriptorImageInfo& msaaDepth,
                                     uint32_t sampleCount, const std::vector<char>& fullscreenVertSpirv,
                                     const std::vector<char>& depthResolveFragSpirv) {
    m_depthResolvePass = depthRenderPass;
    m_depthSamples = sampleCount;
    const VkDescriptorImageInfo infos[kPostBindings] = {msaaDepth, msaaDepth, msaaDepth, msaaDepth,
                                                        msaaDepth, msaaDepth}; // (binding 0 read; the rest valid)
    writeCombinedImageSamplers(m_context.device(), m_depthResolveSet, 0, infos, kPostBindings);

    // A depth-only single-sample pipeline: the fullscreen triangle writes gl_FragDepth and the
    // depth test (LESS against the pass's clear of 1.0) lets every nearer value land.
    PipelineConfig config;
    config.pushConstantSize = sizeof(PostPush);
    config.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT;
    config.depthTestEnable = true;
    config.depthWriteEnable = true;
    config.blendEnable = false;
    config.descriptorSetLayouts = {m_setLayout.get()};
    config.singleSample = true;
    config.hasColorAttachment = false;
    m_depthResolvePipeline = std::make_unique<VulkanPipeline>(m_context, depthRenderPass, fullscreenVertSpirv,
                                                              depthResolveFragSpirv, config);
}

void PostProcess::recordDepthResolve(VkCommandBuffer cmd, VkFramebuffer framebuffer, VkExtent2D extent) {
    if (!m_depthResolvePipeline) {
        return;
    }
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = m_depthResolvePass;
    rp.framebuffer = framebuffer;
    rp.renderArea.extent = extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_depthResolvePipeline->handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_depthResolvePipeline->layout(), 0, 1,
                            &m_depthResolveSet, 0, nullptr);
    const PostPush push{{static_cast<float>(m_depthSamples), 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f},
                        {1.0f, 1.0f, 0.0f, 0.0f}};
    vkCmdPushConstants(cmd, m_depthResolvePipeline->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

} // namespace pose
