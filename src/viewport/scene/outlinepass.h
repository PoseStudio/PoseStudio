/**
 * @file outlinepass.h
 * @brief The selection-outline MASK pass: the selected model's silhouette, drawn into the
 *        OutlineMask for the composite to dilate into the blue outline.
 *
 * Two pipelines against the mask's render pass: the meshes' (shadow.vert's skinned position-only
 * vertex shader, projected by the camera through its push matrix, + outlinemask.frag writing
 * coverage 1) and, since 2026-11-05, the Gaussian splats' (splat.vert + splatmask.frag: the
 * selected cloud's quads as hard coverage over an opacity threshold), so a selected capture wears
 * the outline like any model. Pure Vulkan + std, no Qt.
 */

#ifndef OUTLINEPASS_H
#define OUTLINEPASS_H

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace pose {

class Camera;
class Model;
class OutlineMask;
class VulkanContext;
class VulkanPipeline;

class OutlinePass {
public:
    /// Builds the mask pipelines against @p maskPass (the OutlineMask's render pass): the
    /// meshes' from shadow.vert (@p vertSpirv — its push matrix is a generic viewProj: the
    /// light's in the shadow pass, the camera's here) + outlinemask.frag (@p fragSpirv), its only
    /// descriptor set the per-model POSE layout (@p poseSetLayout, at index 0); the splats' from
    /// splat.vert (@p splatVertSpirv) + splatmask.frag (@p splatMaskFragSpirv) over the splat
    /// pipeline's own two sets (@p cameraSetLayout at 0, @p splatSetLayout at 1).
    OutlinePass(VulkanContext& context, VkRenderPass maskPass, VkDescriptorSetLayout poseSetLayout,
                const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv,
                VkDescriptorSetLayout cameraSetLayout, VkDescriptorSetLayout splatSetLayout,
                const std::vector<char>& splatVertSpirv, const std::vector<char>& splatMaskFragSpirv);
    ~OutlinePass();

    OutlinePass(const OutlinePass&) = delete;
    OutlinePass& operator=(const OutlinePass&) = delete;

    /// Records the pass — begins @p mask's render pass, draws @p selected's silhouette through
    /// @p camera (a splat scene's through @p cameraSet and the order in slot @p orderSlot, sorted
    /// for this view first — with @p asyncSort on the worker thread, VR Mode's way), ends it.
    /// Returns true (the composite then draws the outline).
    bool record(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex, const OutlineMask& mask,
                Model& selected, VkDescriptorSet cameraSet, uint32_t orderSlot, bool asyncSort);

private:
    std::unique_ptr<VulkanPipeline> m_pipeline;      // the meshes'
    std::unique_ptr<VulkanPipeline> m_splatPipeline; // the Gaussian splats'
};

} // namespace pose

#endif // OUTLINEPASS_H
