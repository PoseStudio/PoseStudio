/**
 * @file renderpassbuilder.h
 * @brief Builds the engine's single-subpass render passes (color / resolve / depth attachments +
 *        external dependencies) and the framebuffers that go with them.
 *
 * Every pass in this engine is one subpass over a short attachment list — the HDR scene target,
 * the outline mask, the bloom/SSS targets, the swapchain composite, the depth-only shadow map —
 * and four of them were typed out as the same 60 lines of VkAttachmentDescription/Reference/
 * SubpassDependency filling. The builder expresses a pass as its attachment list in order
 * (color, an optional resolve for the color just added, an optional depth) plus the
 * dependencies, so the attachment order the framebuffer and the clear-value array must follow
 * is visible in one place per target.
 *
 * The dependency helpers encode the engine's two synchronisation contracts, so a fix is a
 * one-place change: sampledTargetDependencies() for a target rendered every frame and sampled
 * by a later pass (with the depth attachment's own writes included when the pass has one — the
 * omission of those was a cross-frame write-after-write hazard on the HDR target's depth), and
 * sampledDepthTargetDependencies() for a depth-only pass whose map the next passes sample.
 *
 * Pure Vulkan + std, no Qt.
 */

#ifndef RENDERPASSBUILDER_H
#define RENDERPASSBUILDER_H

#include "vulkanhandles.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

namespace pose {

/**
 * @class RenderPassBuilder
 * @brief Accumulates attachments + dependencies for one graphics subpass, then creates the pass.
 *
 * Attachment indices are assigned in call order; the framebuffer's view list and the render-pass
 * begin's clear values must follow that same order (attachmentCount() tells the caller how many).
 */
class RenderPassBuilder {
public:
    /// Appends a color attachment. Stencil ops are DONT_CARE; initialLayout is UNDEFINED
    /// (every pass either clears or fully overwrites its targets, so prior contents never matter
    /// and no layout transition is owed on entry).
    RenderPassBuilder& color(VkFormat format, VkSampleCountFlagBits samples, VkAttachmentLoadOp loadOp,
                             VkAttachmentStoreOp storeOp, VkImageLayout finalLayout);

    /// Appends a single-sample resolve target for the color attachment most recently added
    /// (loadOp DONT_CARE — the resolve overwrites it — storeOp STORE).
    RenderPassBuilder& resolve(VkFormat format, VkImageLayout finalLayout);

    /// Appends the depth attachment (at most one).
    RenderPassBuilder& depth(VkFormat format, VkSampleCountFlagBits samples, VkAttachmentLoadOp loadOp,
                             VkAttachmentStoreOp storeOp, VkImageLayout finalLayout);

    /// Appends a depth attachment this pass only READS — the depth test against a depth another
    /// pass produced (the photo pass over the resolved scene depth): loaded, kept, and both
    /// entered and left in @p layout, a read-only depth layout. The one attachment a pass takes
    /// with its contents defined, so initialLayout is @p layout, not UNDEFINED.
    RenderPassBuilder& depthReadOnly(VkFormat format, VkSampleCountFlagBits samples, VkImageLayout layout);

    /// Appends an external dependency.
    RenderPassBuilder& dependency(const VkSubpassDependency& dep);
    /// Appends several (the helpers below return pairs).
    RenderPassBuilder& dependencies(const std::array<VkSubpassDependency, 2>& deps);

    /// Creates the pass. Throws VulkanError on failure.
    UniqueRenderPass build(VkDevice device) const;

    /// How many attachments the pass declares (= the framebuffer view count and the clear count).
    uint32_t attachmentCount() const { return static_cast<uint32_t>(m_attachments.size()); }

private:
    std::vector<VkAttachmentDescription> m_attachments;
    std::vector<VkAttachmentReference>   m_colorRefs;
    std::vector<VkAttachmentReference>   m_resolveRefs; // parallel to m_colorRefs; UNUSED where none
    VkAttachmentReference                m_depthRef{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
    bool                                 m_hasDepth = false;
    bool                                 m_hasResolve = false;
    std::vector<VkSubpassDependency>     m_dependencies;
};

/// The dependency pair for a target that a later pass SAMPLES in the fragment stage and that is
/// re-rendered every frame: (0) the previous frame's fragment reads — and, with @p withDepth,
/// the previous frame's depth writes — must complete before this pass clears/writes the
/// attachments; (1) this pass's writes must be visible to the fragment reads that follow.
/// The depth half matters even though nothing samples the depth image: with two frames in
/// flight, frame N+1's loadOp CLEAR is a write to the same depth image frame N wrote, and without
/// the fragment-test stages + depth-write access on the source side nothing makes those writes
/// available (a write-after-write hazard reported by synchronisation validation).
std::array<VkSubpassDependency, 2> sampledTargetDependencies(bool withDepth);

/// The depth-only variant (the shadow map): previous fragment reads before this pass's depth
/// writes; this pass's depth writes before the fragment reads that follow.
std::array<VkSubpassDependency, 2> sampledDepthTargetDependencies();

/// Creates a framebuffer of @p extent over @p views (which must follow the pass's attachment order).
UniqueFramebuffer createFramebuffer(VkDevice device, VkRenderPass renderPass, const VkImageView* views,
                                    uint32_t viewCount, VkExtent2D extent);

} // namespace pose

#endif // RENDERPASSBUILDER_H
