/**
 * @file vulkanpipeline.h
 * @brief A single graphics pipeline (+ its layout), built from SPIR-V bytecode.
 *
 * Intentionally takes raw SPIR-V *bytes*, not file paths: locating and reading the
 * compiled .spv files is an application concern (it depends on where the build dropped
 * them), so that lives in ShaderLibrary / VulkanRenderer. This class stays a pure Vulkan
 * object and could be reused outside the app.
 *
 * The fixed-function state every pipeline shares (dynamic viewport/scissor — so pipelines
 * survive resizes untouched) lives in the class; everything that differs between pipelines —
 * push constants, blending, depth, culling, topology, vertex input, descriptor set layouts,
 * sample count, colour-attachment count and write masks — arrives via PipelineConfig. Add a
 * knob there only when a real use case needs it, or introduce a small PipelineBuilder if the
 * permutations grow.
 */

#ifndef VULKANPIPELINE_H
#define VULKANPIPELINE_H

#include "vulkancommon.h"
#include "vulkanhandles.h"

#include <cstdint>
#include <vector>

namespace pose {

class VulkanContext;

/**
 * @struct PipelineConfig
 * @brief The fixed-function knobs that differ between the engine's pipelines.
 *
 * Defaults describe an opaque, depth-tested+written, vertex-buffer-less, single-colour-attachment
 * pipeline at the context's MSAA count with a vertex-only push constant. The engine's pipelines
 * are all variations on that: the grid flips on blending, off depth-write and alpha-write, binds
 * the scene's IBL/shadow set, and declares the HDR pass's second colour attachment (masked); the
 * mesh pipelines supply the interleaved vertex layout, four descriptor set layouts and a
 * vertex+fragment push block, and the opaque one writes the second (specular) attachment; the
 * skeleton overlay is a LINE_LIST; the shadow pass is single-sample and depth-only with a static
 * depth bias; the outline mask and the post passes are depth-free; the post passes and the
 * composite are single-sample. The rest (dynamic viewport/scissor, front face, blend factors) is
 * shared and lives in VulkanPipeline itself.
 */
struct PipelineConfig {
    uint32_t           pushConstantSize   = 0;                          // 0 = no push constants
    VkShaderStageFlags pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT; // stages that read them
    bool               blendEnable        = false;                      // standard alpha blending
    bool               depthTestEnable    = true;
    bool               depthWriteEnable   = true;
    VkCullModeFlags    cullMode           = VK_CULL_MODE_NONE;          // back/front/none (every
                                                                        // scene pipeline: NONE —
                                                                        // imported winding is
                                                                        // inconsistent)
    VkPrimitiveTopology topology          = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // e.g. LINE_LIST for overlays

    // Sample count + colour presence. `singleSample` false = the context's MSAA count (the HDR
    // scene pass and the outline mask, which resolve); true = single-sample (the depth-only
    // shadow pass, the post chain, and the swapchain composite — the present pass has no MSAA).
    // `hasColorAttachment` false = a depth-only render pass (no blend state at all), used with a
    // static rasterizer depth bias (enabled when either factor is non-zero) to keep shadow acne down.
    bool  singleSample       = false;
    bool  hasColorAttachment = true;
    float depthBiasConstant  = 0.0f;
    float depthBiasSlope     = 0.0f;

    // The HDR target's ALPHA channel carries the SSS mask (written by the opaque mesh pass).
    // Overlays that draw after it (grid, skeleton, the transparent mesh variant) set this false so
    // their blending can't contaminate the mask — blend factors still use the shader's src alpha,
    // only the framebuffer alpha WRITE is masked off.
    bool  colorWriteAlpha    = true;
    // RGB writes off = a depth-only draw through a colour pass: the hidden-line shade mode's
    // surface fill lays down depth (so the surface occludes the grid and the wires behind it)
    // while the framebuffer keeps the viewport's clear colour.
    bool  colorWriteRgb      = true;

    // Wireframe: rasterize each triangle as its three edges instead of filling it. Needs the
    // fillModeNonSolid device feature (VulkanContext enables it when the device has it — check
    // VulkanContext::supportsWireframe() before building a LINE pipeline). Lines are always one
    // pixel wide (wider lines would need the wideLines feature).
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;

    // Colour attachments in the target render pass. The HDR scene pass has TWO: attachment 0 is
    // the (SSS-blurred) diffuse scene, attachment 1 the SPECULAR the blur must never smear
    // (smeared glints read as a wet film on skin). Attachment 0 keeps this config's blend state;
    // attachment 1 is written plainly by the opaque mesh pipeline (writeColorAttachment1) and
    // masked off entirely for every other scene pipeline, whose shaders don't even declare it.
    // (The Gaussian splats' PHOTO LAYER is a single-attachment pass of its own — hdrtarget.h —
    // where the splat pipeline blends with this config's standard "over" state: premultiplied
    // colour + coverage, src.a / 1 − src.a for the colour and 1 / 1 − src.a for the alpha.)
    // (On a device without independentBlend the attachments must share one state — see
    // VulkanContext::supportsIndependentBlend() and the note in vulkanpipeline.cpp.)
    uint32_t colorAttachmentCount   = 1;
    bool     writeColorAttachment1  = false;

    // Vertex input. Empty (the default) means "no vertex buffers" — vertices come from
    // gl_VertexIndex (the grid, the fullscreen passes). The mesh and skeleton pipelines supply an
    // interleaved binding + attributes.
    std::vector<VkVertexInputBindingDescription>   vertexBindings;
    std::vector<VkVertexInputAttributeDescription> vertexAttributes;

    // Descriptor set layouts bound to the pipeline, in set order (the mesh path binds four:
    // camera, material, pose, IBL; the grid one; the post passes one). Empty means
    // push-constants-only. Handles are owned by the caller and must outlive the pipeline.
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
    ~VulkanPipeline() = default; // the owners below clean up (pipeline before layout)

    VulkanPipeline(const VulkanPipeline&) = delete;
    VulkanPipeline& operator=(const VulkanPipeline&) = delete;

    VkPipeline       handle() const { return m_pipeline.get(); }
    VkPipelineLayout layout() const { return m_layout.get(); }

private:
    UniqueShaderModule createShaderModule(const std::vector<char>& spirv) const;

    VulkanContext&       m_context;
    UniquePipelineLayout m_layout;
    UniquePipeline       m_pipeline; // declared after the layout: destroyed first
};

} // namespace pose

#endif // VULKANPIPELINE_H
