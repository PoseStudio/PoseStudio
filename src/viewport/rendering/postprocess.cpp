/**
 * @file postprocess.cpp
 * @brief Implementation of PostProcess. See postprocess.h.
 */

#include "postprocess.h"

#include "hdrtarget.h"
#include "vulkancommands.h"
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
//   composite: x = tonemap (0/1), y = bloom strength, z = bloom on (0/1)
struct PostPush {
    float params[4];
};

} // namespace

PostProcess::PostProcess(VulkanContext& context, VkRenderPass swapchainRenderPass,
                         const VkDescriptorImageInfo& hdrResolve,
                         const VkDescriptorImageInfo& hdrSpecResolve, VkExtent2D extent,
                         const std::vector<char>& fullscreenVertSpirv,
                         const std::vector<char>& brightFragSpirv,
                         const std::vector<char>& blurFragSpirv,
                         const std::vector<char>& compositeFragSpirv,
                         const std::vector<char>& sssBlurFragSpirv)
    : m_context(context), m_extent(extent), m_hdrResolveView(hdrResolve.imageView) {
    VkDevice device = m_context.device();

    // --- Bloom render pass: one single-sample RGBA16F attachment, left sampleable. -------------
    VkAttachmentDescription color{};
    color.format = HdrTarget::kColorFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // fullscreen draw overwrites every pixel
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    // The chain ping-pongs A -> B -> A with each pass sampling the previous one's output:
    // order reads before overwrites and writes before reads, both directions.
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &color;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
    rpInfo.pDependencies = deps.data();
    VK_CHECK(vkCreateRenderPass(device, &rpInfo, nullptr, &m_bloomPass));

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device, &samplerInfo, nullptr, &m_sampler));

    // --- Descriptors: one 2-sampler layout, six sets (bright/blurH/blurV/sssH/sssV/composite). --
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_setLayout));

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 6;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_pool));

    const std::array<VkDescriptorSetLayout, 6> layouts{m_setLayout, m_setLayout, m_setLayout,
                                                       m_setLayout, m_setLayout, m_setLayout};
    std::array<VkDescriptorSet, 6> sets{};
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = m_pool;
    alloc.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    alloc.pSetLayouts = layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(device, &alloc, sets.data()));
    m_brightSet = sets[0];
    m_blurHSet = sets[1];
    m_blurVSet = sets[2];
    m_compositeSet = sets[3];
    m_sssHSet = sets[4];
    m_sssVSet = sets[5];

    // --- Pipelines. Bright/blur target the bloom pass (single-sample); the composite targets the
    // swapchain's MSAA pass (a fully-covered fullscreen triangle — MSAA is a no-op on it). -------
    PipelineConfig postConfig;
    postConfig.pushConstantSize = sizeof(PostPush);
    postConfig.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT;
    postConfig.depthTestEnable = false;
    postConfig.depthWriteEnable = false;
    postConfig.blendEnable = false;
    postConfig.descriptorSetLayouts = {m_setLayout};
    postConfig.singleSample = true;
    m_brightPipeline = std::make_unique<VulkanPipeline>(m_context, m_bloomPass, fullscreenVertSpirv,
                                                        brightFragSpirv, postConfig);
    m_blurPipeline = std::make_unique<VulkanPipeline>(m_context, m_bloomPass, fullscreenVertSpirv,
                                                      blurFragSpirv, postConfig);
    m_sssPipeline = std::make_unique<VulkanPipeline>(m_context, m_bloomPass, fullscreenVertSpirv,
                                                     sssBlurFragSpirv, postConfig);
    postConfig.singleSample = false; // must match the swapchain pass's MSAA count
    m_compositePipeline = std::make_unique<VulkanPipeline>(
        m_context, swapchainRenderPass, fullscreenVertSpirv, compositeFragSpirv, postConfig);

    createTargets();
    updateDescriptors(hdrResolve, hdrSpecResolve);
}

PostProcess::~PostProcess() {
    destroyTargets();
    VkDevice device = m_context.device();
    m_brightPipeline.reset();
    m_blurPipeline.reset();
    m_sssPipeline.reset();
    m_compositePipeline.reset();
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_pool, nullptr);
    }
    if (m_setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, m_sampler, nullptr);
    }
    if (m_bloomPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, m_bloomPass, nullptr);
    }
}

void PostProcess::resize(VkExtent2D extent, const VkDescriptorImageInfo& hdrResolve,
                         const VkDescriptorImageInfo& hdrSpecResolve) {
    m_extent = extent;
    m_hdrResolveView = hdrResolve.imageView;
    destroyTargets();
    createTargets();
    updateDescriptors(hdrResolve, hdrSpecResolve);
}

void PostProcess::createTargets() {
    VkDevice device = m_context.device();
    m_bloomExtent = {std::max(m_extent.width / 2, 1u), std::max(m_extent.height / 2, 1u)};

    const auto makeTarget = [&](BloomTarget& target, VkExtent2D extent) {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = HdrTarget::kColorFormat;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VK_CHECK(vmaCreateImage(m_context.allocator(), &info, &allocInfo, &target.image,
                                &target.alloc, nullptr));

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = target.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = HdrTarget::kColorFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &target.view));

        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = m_bloomPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &target.view;
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device, &fbInfo, nullptr, &target.framebuffer));
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
    submitImmediate(m_context, [&](VkCommandBuffer cmd) {
        const auto toReadOnly = [&](VkImage image) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &barrier);
        };
        toReadOnly(m_bloomA.image);
        toReadOnly(m_bloomB.image);
        toReadOnly(m_sssScratch.image);
    });

    // The SSSSS V pass writes straight back into the HDR resolve image (same format/usage — the
    // bloom pass object is compatible and size-agnostic).
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = m_bloomPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &m_hdrResolveView;
    fbInfo.width = m_extent.width;
    fbInfo.height = m_extent.height;
    fbInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(device, &fbInfo, nullptr, &m_sssResolveFb));
}

void PostProcess::destroyTargets() {
    VkDevice device = m_context.device();
    const auto destroy = [&](BloomTarget& target) {
        if (target.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, target.framebuffer, nullptr);
            target.framebuffer = VK_NULL_HANDLE;
        }
        if (target.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, target.view, nullptr);
            target.view = VK_NULL_HANDLE;
        }
        if (target.image != VK_NULL_HANDLE) {
            vmaDestroyImage(m_context.allocator(), target.image, target.alloc);
            target.image = VK_NULL_HANDLE;
            target.alloc = VK_NULL_HANDLE;
        }
    };
    destroy(m_bloomA);
    destroy(m_bloomB);
    destroy(m_sssScratch);
    if (m_sssResolveFb != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, m_sssResolveFb, nullptr);
        m_sssResolveFb = VK_NULL_HANDLE;
    }
}

void PostProcess::updateDescriptors(const VkDescriptorImageInfo& hdrResolve,
                                    const VkDescriptorImageInfo& hdrSpecResolve) {
    const auto imageInfo = [&](const BloomTarget& target) {
        VkDescriptorImageInfo info{};
        info.sampler = m_sampler;
        info.imageView = target.view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return info;
    };
    const VkDescriptorImageInfo aInfo = imageInfo(m_bloomA);
    const VkDescriptorImageInfo bInfo = imageInfo(m_bloomB);
    const VkDescriptorImageInfo sssInfo = imageInfo(m_sssScratch);

    // (set, binding, image): bright reads HDR; blurH reads A; blurV reads B; composite reads
    // HDR + A; sssH reads HDR; sssV reads the SSS scratch + the SPECULAR resolve (added back
    // after the blur — the blur must never smear glints). Unused second bindings get a duplicate
    // write so every declared binding is valid.
    struct Entry {
        VkDescriptorSet              set;
        uint32_t                     binding;
        const VkDescriptorImageInfo* info;
    };
    const std::array<Entry, 12> entries{{{m_brightSet, 0, &hdrResolve},
                                         {m_brightSet, 1, &hdrResolve},
                                         {m_blurHSet, 0, &aInfo},
                                         {m_blurHSet, 1, &aInfo},
                                         {m_blurVSet, 0, &bInfo},
                                         {m_blurVSet, 1, &bInfo},
                                         {m_compositeSet, 0, &hdrResolve},
                                         {m_compositeSet, 1, &aInfo},
                                         {m_sssHSet, 0, &hdrResolve},
                                         {m_sssHSet, 1, &hdrResolve},
                                         {m_sssVSet, 0, &sssInfo},
                                         {m_sssVSet, 1, &hdrSpecResolve}}};
    std::array<VkWriteDescriptorSet, 12> writes{};
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
    rp.renderPass = m_bloomPass;
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

void PostProcess::recordSss(VkCommandBuffer cmd) {
    // Full-res masked blur, H into the scratch target, V back into the HDR resolve image — the
    // rest of the chain (bloom, composite) just sees the diffused image.
    const float texelX = 1.0f / static_cast<float>(m_extent.width);
    const float texelY = 1.0f / static_cast<float>(m_extent.height);
    const float pushH[4] = {texelX, texelY, 1.0f, 0.0f};
    const float pushV[4] = {texelX, texelY, 0.0f, 1.0f};
    runFullscreenPass(cmd, m_sssScratch.framebuffer, m_extent, *m_sssPipeline, m_sssHSet, pushH);
    runFullscreenPass(cmd, m_sssResolveFb, m_extent, *m_sssPipeline, m_sssVSet, pushV);
}

void PostProcess::recordBloom(VkCommandBuffer cmd) {
    const float texelX = 1.0f / static_cast<float>(m_bloomExtent.width);
    const float texelY = 1.0f / static_cast<float>(m_bloomExtent.height);
    constexpr float kBloomThreshold = 1.0f; // post-exposure luminance where bloom starts

    const float pushBright[4] = {kBloomThreshold, 0.0f, 0.0f, 0.0f};
    const float pushH[4] = {texelX, texelY, 1.0f, 0.0f};
    const float pushV[4] = {texelX, texelY, 0.0f, 1.0f};
    runFullscreenPass(cmd, m_bloomA.framebuffer, m_bloomExtent, *m_brightPipeline, m_brightSet,
                      pushBright);
    runFullscreenPass(cmd, m_bloomB.framebuffer, m_bloomExtent, *m_blurPipeline, m_blurHSet, pushH);
    runFullscreenPass(cmd, m_bloomA.framebuffer, m_bloomExtent, *m_blurPipeline, m_blurVSet, pushV);
}

void PostProcess::recordComposite(VkCommandBuffer cmd, bool tonemap, bool bloom) {
    constexpr float kBloomStrength = 0.35f;
    const PostPush push{{tonemap ? 1.0f : 0.0f, kBloomStrength, bloom ? 1.0f : 0.0f, 0.0f}};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline->handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_compositePipeline->layout(), 0,
                            1, &m_compositeSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_compositePipeline->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace pose
