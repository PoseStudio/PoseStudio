/**
 * @file hdrtarget.h
 * @brief The offscreen HDR scene target: MSAA RGBA16F color + depth, resolved to a sampleable
 *        single-sample HDR texture — and the PHOTO PASS beside it, where the Gaussian splats go.
 *
 * The scene (backdrop, meshes, grid, overlays) renders here instead of straight into the
 * swapchain, in LINEAR HDR — the PBR shader no longer tonemaps inline. The resolved texture then
 * feeds the post-processing chain (the screen-space SSS blur, bloom bright-extract, and the
 * fullscreen composite that tonemaps into the swapchain). The pass carries TWO color
 * attachments — the scene (diffuse in the PBR mode, whose alpha is the SSS mask) and the PBR
 * opaque pass's SPECULAR, kept apart so the SSS blur can never smear glints into a wet film —
 * each with its own single-sample resolve, plus a depth attachment, all at the context's MSAA
 * count; every scene pipeline builds against this pass. It depends only on formats, so
 * resize() rebuilds images + framebuffer but keeps the pass — scene pipelines never rebuild.
 *
 * THE PHOTO PASS (recordPhotoPass): the Gaussian splats — accumulated display-referred in the
 * trainers' own color space as premultiplied color + coverage, which the composite lays over
 * the tonemapped scene — are drawn in a pass of their own, SINGLE-SAMPLE, after the scene pass.
 * A translucent splat is a blend per covered sample, and drawn inside the scene pass at its 4×
 * MSAA (as it was until 2026-11-05) every pixel of every quad of a capture of half a million
 * splats cost four blends for an edge the soft Gaussian never has: the splats were the whole of
 * a frame's GPU time, and a headset frame, two eyes of several megapixels each, could not be
 * had at all. The pass depth-tests the splats against the scene's depth, resolved first — the
 * nearest sample (photoresolve.frag) — into a single-sample depth image of the layer's size; on
 * a single-sample device the scene's depth image is the attachment itself and nothing is
 * resolved. The same resolve writes the scene's COVERAGE of each texel (coverageInfo): at a
 * silhouette the resolved scene is part figure, part backdrop, and the nearest-sample test
 * keeps every splat behind her out of the whole texel — the composite needs the coverage to
 * put the capture, not the backdrop, behind her antialiased edge (composite.frag, THE EDGE).
 * With the layer at a reduced scale that coverage is per BLOCK (which texels of the layer the
 * scene touched), and a second, coverage-only pass writes the scene's own, pixel for pixel
 * (coverageFullInfo): the composite needs both there — see coverageMode(). The layer may be SMALLER than the scene
 * (photoScale, VR Mode's lever: the fill is what a headset frame cannot afford, and a capture
 * upsampled a little is softer where a dropped frame is worse); the composite samples it
 * bilinearly whatever its size. Without a splat in the scene the pass is skipped and the layer
 * holds coverage 0 — cleared at creation, and once more after the last splat model goes.
 * Pure Vulkan + std, no Qt.
 */

#ifndef HDRTARGET_H
#define HDRTARGET_H

#include "attachmentimage.h"
#include "vulkanhandles.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pose {

class VulkanContext;
class VulkanPipeline;

class HdrTarget {
public:
    /// @param fullscreenVertSpirv, photoResolveFragSpirv  The photo pass's resolve
    ///        (photoresolve.frag: the nearest of the scene depth's samples, and the share of them
    ///        the scene covers), built here against the resolve pass; unused on a single-sample
    ///        device.
    /// @param photoScale  The photo layer's size as a share of the scene's, 0.25 .. 1 (1 on a
    ///        single-sample device, whose scene depth image is the photo pass's own attachment
    ///        and so must be its size) — the initial value of setPhotoScale.
    HdrTarget(VulkanContext& context, VkExtent2D extent, const std::vector<char>& fullscreenVertSpirv,
              const std::vector<char>& photoResolveFragSpirv, float photoScale = 1.0f);
    ~HdrTarget(); // out of line: VulkanPipeline is incomplete here

    HdrTarget(const HdrTarget&) = delete;
    HdrTarget& operator=(const HdrTarget&) = delete;

    /// Recreates the images/views/framebuffers at @p extent (the render passes are kept). The
    /// caller must have made the device idle (the renderer's resize path already does).
    void resize(VkExtent2D extent);

    VkRenderPass  renderPass() const { return m_renderPass.get(); }
    VkFramebuffer framebuffer() const { return m_framebuffer.get(); }

    /// The resolved (single-sample) HDR texture, in SHADER_READ_ONLY layout after the pass.
    VkDescriptorImageInfo resolveInfo() const;

    /// The resolved SPECULAR texture (color attachment 1) — the opaque mesh pass routes its
    /// specular here so the screen-space SSS blur (which runs on the main resolve) can never
    /// smear glints into a wet-looking film; the blur's V pass adds it back after diffusing.
    VkDescriptorImageInfo specResolveInfo() const;

    /// The depth attachment (at the context's MSAA count) through a NEAREST non-comparison
    /// sampler, in DEPTH_STENCIL_READ_ONLY layout after the pass — kept every frame: the photo
    /// pass's resolve reads it, and VR Mode's depth layer resolves it again for the headset.
    VkDescriptorImageInfo depthInfo() const;

    /// Number of attachments in the pass — the render-pass-begin clear array must cover them.
    /// Attachment order (the clear array follows it): MSAA = colorMS(0) / depth(1) /
    /// resolve(2) / specMS(3) / specResolve(4); single-sample = color(0) / depth(1) / spec(2).
    uint32_t attachmentCount() const { return m_attachmentCount; }

    // --- The photo pass (see the file comment) ---------------------------------------------
    /// The pass the splat pipeline builds against: one single-sample RGBA16F color attachment
    /// (the layer, cleared to coverage 0) and a read-only single-sample depth (the scene's).
    VkRenderPass photoRenderPass() const { return m_photoPass.get(); }
    /// The layer's size (the scene's × photoScale): the viewport recordPhotoPass sets, and what
    /// the splats' pixel footprints are sized against.
    VkExtent2D photoExtent() const { return m_photoExtent; }
    /// Changes the scale between frames at no cost: the layer and its depth are allocated at the
    /// scene's size, and the scale picks the sub-rectangle — from the origin — that the passes
    /// render into and the composite samples (photoUvScale). VR Mode's dynamic scale moves it
    /// every frame; a single-sample device keeps 1.
    void setPhotoScale(float scale);
    /// The share of the layer's UV space the current scale draws into (photoExtent / the
    /// scene's extent), for the composite: it samples uv × this.
    void photoUvScale(float out[2]) const;
    /// Records the photo pass: the scene depth resolved into the layer's own depth (a
    /// multisampled device), then the pass — the layer cleared, the viewport set — with @p draw
    /// recording the splats inside it. An EMPTY @p draw means no splats this frame: the pass then
    /// runs only if the layer still holds a drawing (the one clear after the last splat model
    /// goes), else nothing is recorded. Call after the scene pass, before the layer is composited.
    void recordPhotoPass(VkCommandBuffer cmd, const std::function<void()>& draw);
    /// The photo layer — premultiplied color + coverage — in SHADER_READ_ONLY layout whenever
    /// no pass is writing it (from creation on: it is cleared and moved there before any frame).
    VkDescriptorImageInfo photoInfo() const;
    /// The photo layer's image itself (in SHADER_READ_ONLY_OPTIMAL after its pass) — for a
    /// readback of the layer (VulkanRenderer::renderSplatLayers).
    VkImage photoImage() const { return m_photo.image(); }
    /// THE SCENE'S COVERAGE of each texel of the LAYER (R8), written by the photo pass's resolve
    /// beside its depth. With the layer at the scene's size: the share of the texel's depth
    /// samples a depth-writing surface covers (0 open .. 1 covered, fractional only along
    /// silhouettes). With the layer scaled down: 1 where ANY sample of the texel's block is
    /// covered — the texels the scene touched, which hold no splat from behind it. In
    /// SHADER_READ_ONLY layout whenever no pass is writing it. A single-sample device has no
    /// resolve and no fractional coverage: the descriptor then names the photo layer (valid,
    /// never read — coverageMode() is 0).
    VkDescriptorImageInfo coverageInfo() const;
    /// The scene's coverage PIXEL FOR PIXEL (the share, as above) — written only while the
    /// layer is scaled down (coverageMode() 2), by a coverage-only pass after the resolve.
    VkDescriptorImageInfo coverageFullInfo() const;
    /// What the coverage images hold for the frame being recorded: 0 = nothing to go by (no
    /// splats drawn this frame, or a single-sample device); 1 = coverageInfo() is the scene's
    /// coverage, texel for texel (the layer at the scene's size); 2 = the layer is scaled:
    /// coverageInfo() says which of its texels the scene touched, coverageFullInfo() is the
    /// scene's coverage. The composite's edge blend follows it (CompositeOptions::edgeMode).
    int coverageMode() const;

    static constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

private:
    void createImages();
    void destroyImages();

    VulkanContext&   m_context;
    VkExtent2D       m_extent{};
    UniqueRenderPass m_renderPass;
    VkSampler        m_sampler = VK_NULL_HANDLE;      // borrowed from the context's SamplerCache
    VkSampler        m_depthSampler = VK_NULL_HANDLE; // nearest, borrowed
    uint32_t         m_attachmentCount = 0;

    // MSAA color + specular (transient) + MSAA depth + their single-sample resolve targets.
    // Single-sample fallback: the color/spec images ARE the sampleable results (no resolves).
    AttachmentImage   m_color;
    AttachmentImage   m_depth;
    AttachmentImage   m_resolve;
    AttachmentImage   m_spec;
    AttachmentImage   m_specResolve;
    UniqueFramebuffer m_framebuffer; // references the views above

    // The photo pass: the layer, the pass over it, and — on a multisampled device — the resolve
    // of the scene depth into a depth image of the layer's size (its pass, its pipeline, the
    // set that samples the scene depth). Declaration order = reverse destruction: the pipeline
    // goes before the passes and the layout it was built against, the framebuffers before the
    // images they reference.
    float                           m_photoScale = 1.0f;
    VkExtent2D                      m_photoExtent{};
    bool                            m_photoDrawn = false; // the layer holds a drawing: a clear is owed once the splats go
    UniqueRenderPass                m_photoPass;
    UniqueRenderPass                m_resolvePass;
    UniqueRenderPass                m_coveragePass; // the coverage-only pass (a scaled layer's full-size coverage)
    UniqueDescriptorSetLayout       m_resolveSetLayout;
    UniqueDescriptorPool            m_resolvePool;
    VkDescriptorSet                 m_resolveSet = VK_NULL_HANDLE; // freed with the pool
    std::unique_ptr<VulkanPipeline> m_resolvePipeline;
    std::unique_ptr<VulkanPipeline> m_coveragePipeline; // the resolve's shader against the coverage-only pass
    AttachmentImage                 m_photo;      // single-sample RGBA16F, sampled by the composite
    AttachmentImage                 m_photoDepth; // single-sample D32 (multisampled devices only)
    AttachmentImage                 m_coverage;   // single-sample R8, the scene's coverage (likewise)
    AttachmentImage                 m_coverageFull; // single-sample R8, the same at the scene's size (a scaled layer)
    UniqueFramebuffer               m_resolveFramebuffer;
    UniqueFramebuffer               m_coverageFramebuffer;
    UniqueFramebuffer               m_photoFramebuffer;
};

} // namespace pose

#endif // HDRTARGET_H
