/**
 * @file vulkanpipeline.h
 * @brief A single graphics pipeline (+ its layout), built from SPIR-V bytecode.
 *
 * Intentionally takes raw SPIR-V *bytes*, not file paths: locating and reading the
 * compiled .spv files is an application concern (it depends on where the build dropped
 * them), so that lives in VulkanRenderer. This class stays a pure Vulkan object and
 * could be reused outside the app.
 *
 * Today's pipelines are deliberately minimal — no vertex buffers (vertices are produced
 * in the shader from gl_VertexIndex) and at most a single push-constant block. As real
 * geometry arrives, add vertex input bindings/attributes and descriptor set layouts here,
 * or introduce a small PipelineBuilder if the permutations grow.
 */

#ifndef VULKANPIPELINE_H
#define VULKANPIPELINE_H

#include "vulkancommon.h"

#include <cstdint>
#include <vector>

namespace pose {

class VulkanContext;

/**
 * @struct PipelineConfig
 * @brief The handful of fixed-function knobs that differ between pipelines.
 *
 * Defaults describe an opaque, depth-tested+written, vertex-buffer-less pipeline with a
 * vertex-only push constant (the grid: it also flips on alpha blending, off depth-write,
 * and makes the push constant fragment-visible). The mesh pipeline supplies vertex
 * bindings/attributes and a descriptor set layout. The rest (dynamic viewport/scissor,
 * triangle-list topology) is shared and lives in VulkanPipeline itself — add a knob here
 * only when a real use case needs it, rather than exposing all of Vulkan's state up front.
 */
struct PipelineConfig {
    uint32_t           pushConstantSize   = 0;                          // 0 = no push constants
    VkShaderStageFlags pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT; // stages that read them
    bool               blendEnable        = false;                      // standard alpha blending
    bool               depthTestEnable    = true;
    bool               depthWriteEnable   = true;
    VkCullModeFlags    cullMode           = VK_CULL_MODE_NONE;          // back/front/none

    // Vertex input. Empty (the default) means "no vertex buffers" — vertices come from
    // gl_VertexIndex (the grid). The mesh pipeline supplies an interleaved binding + attributes.
    std::vector<VkVertexInputBindingDescription>   vertexBindings;
    std::vector<VkVertexInputAttributeDescription> vertexAttributes;

    // Descriptor set layouts bound to the pipeline (e.g. the mesh path's per-frame camera UBO).
    // Empty means push-constants-only. Handles are owned by the caller and must outlive the pipeline.
    std::vector<VkDescriptorSetLayout> descriptorSetLayouts;
};

/**
 * @class VulkanPipeline
 * @brief Owns one VkPipeline and its VkPipelineLayout.
 */
class VulkanPipeline {
public:
    VulkanPipeline(VulkanContext& context, VkRenderPass renderPass,
                   const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv,
                   const PipelineConfig& config);
    ~VulkanPipeline();

    VulkanPipeline(const VulkanPipeline&) = delete;
    VulkanPipeline& operator=(const VulkanPipeline&) = delete;

    VkPipeline       handle() const { return m_pipeline; }
    VkPipelineLayout layout() const { return m_layout; }

private:
    VkShaderModule createShaderModule(const std::vector<char>& spirv) const;

    VulkanContext&   m_context;
    VkPipelineLayout m_layout   = VK_NULL_HANDLE;
    VkPipeline       m_pipeline = VK_NULL_HANDLE;
};

} // namespace pose

#endif // VULKANPIPELINE_H
