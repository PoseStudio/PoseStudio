/**
 * @file tracedview.cpp
 * @brief The Rendered viewport mode's picture as a texture and its one draw. See tracedview.h.
 */

#include "tracedview.h"

#include "descriptorutil.h"
#include "samplercache.h"
#include "vulkancommon.h" // kMaxFramesInFlight, VK_CHECK
#include "vulkancontext.h"
#include "vulkanpipeline.h"

#include <cstring>

namespace pose {

namespace {

constexpr VkFormat kTracedFormat = VK_FORMAT_R32G32B32A32_SFLOAT; // what the engine hands over, as it is

} // namespace

TracedView::TracedView(VulkanContext& context, VkRenderPass sceneRenderPass,
                       const std::vector<char>& fullscreenVertSpirv, const std::vector<char>& tracedFragSpirv)
    : m_context(context) {
    VkDevice device = m_context.device();
    m_setLayout = createDescriptorSetLayout(
        device, uniformBindings(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT));
    m_pool = createDescriptorPool(device, {poolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1)}, 1);
    m_set = allocateDescriptorSet(device, m_pool.get(), m_setLayout.get());

    // Texel for pixel: nearest (a 32-bit float format's linear filtering is optional, and there is
    // nothing to filter).
    SamplerDesc nearest = SamplerDesc::linearClamp();
    nearest.filter = VK_FILTER_NEAREST;
    m_sampler = m_context.samplers().get(nearest);

    // A fullscreen triangle in the HDR scene pass, as the HDRI backdrop is: no depth, no blend,
    // the specular attachment present but masked off. It writes alpha 0 — the HDR target's alpha
    // is the subsurface mask, and a path-traced picture has done its own scattering.
    PipelineConfig config;
    config.depthTestEnable = false;
    config.depthWriteEnable = false;
    config.blendEnable = false;
    config.colorAttachmentCount = 2;
    config.descriptorSetLayouts = {m_setLayout.get()};
    m_pipeline = std::make_unique<VulkanPipeline>(m_context, sceneRenderPass, fullscreenVertSpirv, tracedFragSpirv, config);

    // A texel of nothing, so the set is valid before the first picture.
    createImage(1, 1);
}

TracedView::~TracedView() = default;

void TracedView::createImage(uint32_t width, uint32_t height) {
    m_staging.clear();
    m_image.reset();
    m_extent = {width, height};
    m_image = AttachmentImage(m_context, kTracedFormat, m_extent, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    // (Sampled only once a picture has been copied into it: record() draws nothing before. The
    // set still names a valid image in a known layout.)
    initialiseToShaderRead(m_context, {m_image.image()});
    m_imageWritten = false;

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u * sizeof(float);
    m_staging.reserve(kMaxFramesInFlight);
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        m_staging.emplace_back(m_context, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }
    const VkDescriptorImageInfo info = sampledImageInfo(m_image.view(), m_sampler);
    writeCombinedImageSamplers(m_context.device(), m_set, 0, &info, 1);
}

void TracedView::setImage(const float* rgba, uint32_t width, uint32_t height) {
    if (rgba == nullptr || width == 0 || height == 0) {
        return;
    }
    const std::size_t count = static_cast<std::size_t>(width) * height * 4u;
    m_pending.resize(count);
    std::memcpy(m_pending.data(), rgba, count * sizeof(float));
    m_pendingWidth = width;
    m_pendingHeight = height;
    m_dirty = true;
    m_hasImage = true;
}

void TracedView::clear() {
    m_hasImage = false;
    m_dirty = false;
}

void TracedView::prepare(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!m_dirty) {
        return;
    }
    m_dirty = false;
    if (m_pendingWidth != m_extent.width || m_pendingHeight != m_extent.height) {
        // Another size: the image, its staging buffers and the set that names it are replaced —
        // none of which may happen under a frame still in flight.
        vkDeviceWaitIdle(m_context.device());
        createImage(m_pendingWidth, m_pendingHeight);
    }
    VulkanBuffer& staging = m_staging[frameIndex % m_staging.size()];
    std::memcpy(staging.mappedData(), m_pending.data(), m_pending.size() * sizeof(float));

    const VkImageSubresourceRange range = wholeImageRange(VK_IMAGE_ASPECT_COLOR_BIT);
    transitionImage(cmd, m_image.image(), range, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {m_extent.width, m_extent.height, 1};
    vkCmdCopyBufferToImage(cmd, staging.handle(), m_image.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transitionImage(cmd, m_image.image(), range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    m_imageWritten = true;
}

void TracedView::record(VkCommandBuffer cmd) {
    if (!m_hasImage || !m_imageWritten) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->layout(), 0, 1, &m_set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace pose
