/**
 * @file postprocess.h
 * @brief The post-processing chain over the HDR scene target: screen-space subsurface
 *        scattering, bloom, and the fullscreen composite that tonemaps into the swapchain.
 *
 * Six fullscreen passes share one fullscreen-triangle vertex shader (fullscreen.vert) and one
 * three-sampler descriptor layout: the SSS blur (two full-resolution masked passes, H into a
 * scratch target and V back INTO the HDR resolve, so everything downstream sees diffused skin —
 * the V pass adds the scene's separately-rendered specular back over the blur), the classic
 * three-pass half-resolution bloom (bright-extract with a soft threshold, then a separable
 * Gaussian: H into a second target, V back into the first), and the composite, which adds the
 * bloom over the HDR image, applies ACES (PBR mode with the tonemap dial on; every other mode
 * passes through untouched, since the stylized modes still author display-ready values) and
 * paints the selection outline. The composite draws through the swapchain's single-sample
 * present pass; the other five share one single-sample RGBA16F "bloom" render pass that is
 * size-agnostic. Resolution-dependent images/descriptors rebuild on resize(); the render pass +
 * pipelines are format-only and stay. Pure Vulkan + std, no Qt.
 */

#ifndef POSTPROCESS_H
#define POSTPROCESS_H

#include "attachmentimage.h"
#include "selectionstyle.h" // the outline's default color (kSelectionAccentLinear)
#include "vulkanhandles.h"

#include <vulkan/vulkan.h>

#include <memory>
#include <vector>

namespace pose {

class VulkanContext;
class VulkanPipeline;

class PostProcess {
public:
    /// @param swapchainRenderPass The pass the composite draws through (the swapchain's
    ///                            single-sample present pass).
    /// @param hdrResolve          The HDR target's resolved-texture descriptor (re-fed on resize).
    /// @param hdrSpecResolve      The HDR target's resolved SPECULAR descriptor — sampled by the
    ///                            SSS V-pass, which adds it back after diffusing (the blur must
    ///                            never smear glints; smeared specular reads as wet skin).
    /// @param hdrPhotoResolve     The HDR target's resolved PHOTO LAYER — the Gaussian splats,
    ///                            premultiplied color + coverage in the trainers' own color
    ///                            space, which the composite lays over the tonemapped scene.
    /// @param outlineMask         The selection-outline coverage mask (OutlineMask) the composite
    ///                            dilates into the outline (re-fed on resize).
    PostProcess(VulkanContext& context, VkRenderPass swapchainRenderPass,
                const VkDescriptorImageInfo& hdrResolve,
                const VkDescriptorImageInfo& hdrSpecResolve,
                const VkDescriptorImageInfo& hdrPhotoResolve,
                const VkDescriptorImageInfo& outlineMask, VkExtent2D extent,
                const std::vector<char>& fullscreenVertSpirv,
                const std::vector<char>& brightFragSpirv, const std::vector<char>& blurFragSpirv,
                const std::vector<char>& compositeFragSpirv,
                const std::vector<char>& sssBlurFragSpirv);
    ~PostProcess(); // out of line: VulkanPipeline is incomplete here

    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    /// Rebuilds the half-res bloom targets + all descriptor sets for a new size / recreated HDR
    /// target and outline mask. Caller must have made the device idle.
    void resize(VkExtent2D extent, const VkDescriptorImageInfo& hdrResolve,
                const VkDescriptorImageInfo& hdrSpecResolve,
                const VkDescriptorImageInfo& hdrPhotoResolve,
                const VkDescriptorImageInfo& outlineMask);

    /// The selection outline's color (LINEAR rgb) and opacity (0 = no outline, 1 = the color
    /// itself over the frame): Preferences → Selection, through VulkanRenderer::setSelectionStyle.
    /// Read at the next composite; nothing is rebuilt.
    void setOutlineStyle(const glm::vec3& colorLinear, float opacity);

    /// Records the screen-space subsurface scattering blur: two full-res masked passes, HDR
    /// resolve -> scratch -> back into the HDR resolve (so everything downstream — bloom, the
    /// composite — just sees the diffused image). Call right after the HDR scene pass, PBR only.
    void recordSss(VkCommandBuffer cmd);

    /// Records the bloom chain (bright -> blur H -> blur V), three small render passes at half
    /// resolution. Call between the HDR scene pass and the swapchain pass, PBR mode only.
    void recordBloom(VkCommandBuffer cmd);

    /// The composite's optional inputs (VR Mode — see VulkanRenderer::drawVrFrame): draw from the
    /// MIRROR source (setMirrorSource) instead of this chain's own targets, through a crop of its
    /// UV space (scale + offset — the desktop viewport shows a VR eye cropped to its own aspect),
    /// and encode sRGB in the shader when the target format cannot (a UNORM headset swapchain).
    struct CompositeOptions {
        bool  fromMirror = false;
        float uvScale[2] = {1.0f, 1.0f};
        float uvOffset[2] = {0.0f, 0.0f};
        bool  encodeSrgb = false;
        /// The share of the photo layer's UV space its current scale draws into
        /// (HdrTarget::photoUvScale): the layer is sampled at uv × this.
        float photoUvScale[2] = {1.0f, 1.0f};
        /// The outline mask holds a splat scene's accumulated opacity (Scene::selectedIsSplat):
        /// the outline follows its 50% contour rather than reading it as coverage.
        bool  outlineFromSplats = false;
    };

    /// Records the fullscreen composite draw. The caller has begun the swapchain render pass and
    /// set the full-extent viewport/scissor. @p tonemap applies ACES (PBR + dial on); @p bloom
    /// adds the bloom target (PBR only — must match whether recordBloom ran this frame);
    /// @p outlineWidthPx > 0 paints the selection outline that wide (physical pixels) around
    /// the coverage in the outline mask — pass 0 whenever the mask pass didn't run this frame
    /// (the mask's contents are then stale).
    void recordComposite(VkCommandBuffer cmd, bool tonemap, bool bloom, float outlineWidthPx,
                         const CompositeOptions& options = {});

    /// Points the MIRROR composite (CompositeOptions::fromMirror) at another chain's outputs: its
    /// HDR resolve, its bloom target (bloomInfo()), its outline mask and its photo layer — the
    /// desktop viewport's window onto what the headset sees. Kept across resize();
    /// clearMirrorSource() drops it (call before those images are destroyed).
    void setMirrorSource(const VkDescriptorImageInfo& hdrResolve, const VkDescriptorImageInfo& bloom,
                         const VkDescriptorImageInfo& outlineMask, const VkDescriptorImageInfo& photo);
    void clearMirrorSource();
    /// The final bloom target (sampled, SHADER_READ_ONLY after recordBloom) — a mirror source.
    VkDescriptorImageInfo bloomInfo() const;

    /// VR Mode's DEPTH RESOLVE (XR_KHR_composition_layer_depth): the eye target's multisampled
    /// depth (@p msaaDepth = HdrTarget::depthInfo of a storeDepth target, @p sampleCount samples)
    /// resolved — the nearest sample — into the headset's single-sample depth image through
    /// @p depthRenderPass (the session's depth-only pass, cleared to 1.0 here). Build once after
    /// construction; recordDepthResolve then runs per eye.
    void enableDepthResolve(VkRenderPass depthRenderPass, const VkDescriptorImageInfo& msaaDepth,
                            uint32_t sampleCount, const std::vector<char>& fullscreenVertSpirv,
                            const std::vector<char>& depthResolveFragSpirv);
    bool hasDepthResolve() const { return m_depthResolvePipeline != nullptr; }
    void recordDepthResolve(VkCommandBuffer cmd, VkFramebuffer framebuffer, VkExtent2D extent);

private:
    /// One single-sample RGBA16F render target + the framebuffer that draws into it.
    struct Target {
        AttachmentImage   image;
        UniqueFramebuffer framebuffer;
    };

    void createTargets();
    void destroyTargets();
    void updateDescriptors(const VkDescriptorImageInfo& hdrResolve,
                           const VkDescriptorImageInfo& hdrSpecResolve,
                   const VkDescriptorImageInfo& hdrPhotoResolve,
                           const VkDescriptorImageInfo& outlineMask);
    void runFullscreenPass(VkCommandBuffer cmd, VkFramebuffer framebuffer, VkExtent2D extent,
                           const VulkanPipeline& pipeline, VkDescriptorSet set,
                           const float params[4]);

    // Declaration order = reverse destruction order: the pipelines and sets go first, the
    // targets/framebuffers before the pass they were built for.
    VulkanContext& m_context;
    VkExtent2D     m_extent{};      // full resolution; bloom runs at half
    VkExtent2D     m_bloomExtent{}; // max(extent/2, 1)
    VkImageView    m_hdrResolveView = VK_NULL_HANDLE; // for the SSSSS V pass's framebuffer

    UniqueRenderPass m_bloomPass; // single RGBA16F attachment -> SHADER_READ_ONLY
    VkSampler        m_sampler = VK_NULL_HANDLE; // clamp+linear, borrowed from the SamplerCache
    Target           m_bloomA; // bright output, then final (post blur-V) bloom
    Target           m_bloomB; // blur-H intermediate

    // SSSSS: a full-res scratch target for the H pass, and a framebuffer ON the HDR resolve image
    // so the V pass writes the diffused result back in place (the bloom render pass is reused —
    // same format/structure, size-agnostic).
    Target            m_sssScratch;   // full resolution
    UniqueFramebuffer m_sssResolveFb; // targets the HDR resolve view

    // One three-sampler set layout serves every pass (a pass's unused bindings = its binding-0
    // image again, so every declared binding is valid).
    UniqueDescriptorSetLayout m_setLayout;
    UniqueDescriptorPool      m_pool;
    VkDescriptorSet           m_brightSet = VK_NULL_HANDLE;    // 0 = HDR resolve
    VkDescriptorSet           m_blurHSet = VK_NULL_HANDLE;     // 0 = bloom A
    VkDescriptorSet           m_blurVSet = VK_NULL_HANDLE;     // 0 = bloom B
    VkDescriptorSet           m_compositeSet = VK_NULL_HANDLE; // 0 = HDR resolve, 1 = bloom A, 2 = outline mask
    VkDescriptorSet           m_sssHSet = VK_NULL_HANDLE;      // 0 = HDR resolve
    VkDescriptorSet           m_sssVSet = VK_NULL_HANDLE;      // 0 = SSS scratch, 1 = spec resolve
    VkDescriptorSet           m_mirrorSet = VK_NULL_HANDLE;    // the composite over setMirrorSource's images
    bool                      m_hasMirror = false;
    VkDescriptorImageInfo     m_mirrorInfos[4]{};              // hdr, bloom, outline, photo (re-written on resize)
    VkDescriptorSet           m_depthResolveSet = VK_NULL_HANDLE; // 0 = the MSAA depth (enableDepthResolve)
    VkRenderPass              m_depthResolvePass = VK_NULL_HANDLE; // borrowed (the session's)
    uint32_t                  m_depthSamples = 1;

    // Selection-outline color (linear rgb) and opacity, pushed to the composite: the app's
    // accent blue (kSelectionAccentLinear) in full until setOutlineStyle says otherwise — the
    // composite paints it after tonemapping and the sRGB swapchain re-encodes it exactly.
    float m_outlineColor[3] = {kSelectionAccentLinear[0], kSelectionAccentLinear[1],
                               kSelectionAccentLinear[2]};
    float m_outlineOpacity = 1.0f;

    std::unique_ptr<VulkanPipeline> m_brightPipeline; // bloom pass
    std::unique_ptr<VulkanPipeline> m_blurPipeline;   // bloom pass (direction via push constant)
    std::unique_ptr<VulkanPipeline> m_sssPipeline;    // SSSSS masked blur (bloom pass, full res)
    std::unique_ptr<VulkanPipeline> m_compositePipeline; // swapchain pass
    std::unique_ptr<VulkanPipeline> m_depthResolvePipeline; // the session's depth pass (VR, optional)
};

} // namespace pose

#endif // POSTPROCESS_H
