/**
 * @file tracedview.h
 * @brief THE RENDERED VIEWPORT MODE's picture: a path-traced image of the scene, made by a render
 *        engine on its own threads (render/renderengineapi.h's interactive render), shown in the
 *        viewport in place of the rasterized scene.
 *
 * The renderer core knows nothing of render engines: it is handed PIXELS — linear RGBA floats,
 * row 0 the top row, the exposure applied, as the engine's interface hands them over — and draws
 * them as the HDR scene pass's whole content, so everything downstream is the frame it always
 * was: the composite tonemaps it (the Environment tab's tonemap dial), paints the selection
 * outline over it, and the line overlays (the light objects' markers, the pins, the skeleton)
 * are drawn on top in the same pass.
 *
 * setImage() copies the pixels and returns; the upload happens where the frame is recorded
 * (prepare(), before any render pass): through a host-mapped staging buffer PER FRAME IN FLIGHT
 * (a picture arriving while the last frame's copy still executes must not overwrite its source)
 * into one RGBA32F image, sampled with a nearest sampler — the picture is the window's size,
 * texel for pixel; while a resize is catching up it is stretched for a frame or two. A new size
 * re-creates the image and the buffers after a device wait.
 *
 * Pure Vulkan + std, no Qt.
 */

#ifndef TRACEDVIEW_H
#define TRACEDVIEW_H

#include "attachmentimage.h"
#include "vulkanbuffer.h"
#include "vulkanhandles.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace pose {

class VulkanContext;
class VulkanPipeline;

class TracedView {
public:
    /// @param sceneRenderPass The HDR scene pass the picture is drawn in (HdrTarget::renderPass).
    TracedView(VulkanContext& context, VkRenderPass sceneRenderPass, const std::vector<char>& fullscreenVertSpirv,
               const std::vector<char>& tracedFragSpirv);
    ~TracedView();

    TracedView(const TracedView&) = delete;
    TracedView& operator=(const TracedView&) = delete;

    /// The picture from now on: @p width × @p height linear RGBA floats, row 0 = top. Copied; it
    /// is on the GPU from the next recorded frame.
    void setImage(const float* rgba, uint32_t width, uint32_t height);
    /// Forgets the picture (hasImage() false until the next setImage).
    void clear();
    /// Whether there is a picture to draw.
    bool hasImage() const { return m_hasImage; }

    /// Uploads a picture handed over since the last frame. Call while recording frame slot
    /// @p frameIndex's command buffer, OUTSIDE any render pass.
    void prepare(VkCommandBuffer cmd, uint32_t frameIndex);
    /// Draws the picture over the whole target. Call inside the HDR scene pass, first, with the
    /// viewport and scissor set.
    void record(VkCommandBuffer cmd);

private:
    void createImage(uint32_t width, uint32_t height);

    VulkanContext& m_context;

    UniqueDescriptorSetLayout       m_setLayout;
    UniqueDescriptorPool            m_pool;
    VkDescriptorSet                 m_set = VK_NULL_HANDLE;
    VkSampler                       m_sampler = VK_NULL_HANDLE; // nearest + clamp, borrowed from the SamplerCache
    std::unique_ptr<VulkanPipeline> m_pipeline;

    AttachmentImage           m_image;          // RGBA32F, the picture's size
    VkExtent2D                m_extent{0, 0};
    bool                      m_imageWritten = false; // false while the image is still in its creation layout
    std::vector<VulkanBuffer> m_staging;        // one per frame in flight, the picture's size

    std::vector<float> m_pending; // the picture handed over, until it is uploaded
    uint32_t           m_pendingWidth = 0;
    uint32_t           m_pendingHeight = 0;
    bool               m_dirty = false;
    bool               m_hasImage = false;
};

} // namespace pose

#endif // TRACEDVIEW_H
