/**
 * @file vulkanpipeline.cpp
 * @brief Implementation of the graphics pipeline. See vulkanpipeline.h.
 */

#include "vulkanpipeline.h"

#include "vulkancontext.h"

#include <algorithm>
#include <array>

namespace pose {

VulkanPipeline::VulkanPipeline(VulkanContext& context, VkRenderPass renderPass,
                               const std::vector<char>& vertSpirv,
                               const std::vector<char>& fragSpirv,
                               const PipelineConfig& config)
    : m_context(context) {
    VkDevice device = m_context.device();

    // Every handle created here is an RAII owner: a throwing constructor never runs
    // ~VulkanPipeline, but the owners' own destructors still release whatever was created
    // before a mid-sequence VK_CHECK throw (otherwise it leaks until device destruction and
    // trips validation's object-leak check). The shader modules are locals — they can be
    // destroyed as soon as the pipeline is built, whatever the outcome.
    const UniqueShaderModule vertModule = createShaderModule(vertSpirv);
    const UniqueShaderModule fragModule = createShaderModule(fragSpirv);

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule.get();
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule.get();
    stages[1].pName = "main";

    // Vertex input: empty config => no vertex buffers (grid generates verts from gl_VertexIndex);
    // otherwise the supplied interleaved binding + attributes (the mesh pipeline).
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount =
        static_cast<uint32_t>(config.vertexBindings.size());
    vertexInput.pVertexBindingDescriptions =
        config.vertexBindings.empty() ? nullptr : config.vertexBindings.data();
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(config.vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions =
        config.vertexAttributes.empty() ? nullptr : config.vertexAttributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = config.topology;

    // Viewport/scissor are dynamic so the pipeline survives window resizes untouched —
    // the renderer sets them per frame from the swapchain extent.
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = config.polygonMode; // FILL, or LINE for the wireframe shade modes
    raster.cullMode = config.cullMode; // default NONE; mesh/grid both opt out of culling
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f; // any other width needs the wideLines feature, which isn't enabled
    // Static depth bias (the shadow pass): pushes stored depth away from the light to kill acne.
    raster.depthBiasEnable =
        (config.depthBiasConstant != 0.0f || config.depthBiasSlope != 0.0f) ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = config.depthBiasConstant;
    raster.depthBiasSlopeFactor = config.depthBiasSlope;

    // Must match the target render pass's sample count: the context's MSAA count for the HDR
    // scene pass and the outline mask, single-sample for the depth-only shadow pass, the post
    // chain and the swapchain composite.
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples =
        config.singleSample ? VK_SAMPLE_COUNT_1_BIT : m_context.sampleCount();

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = config.depthTestEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = config.depthWriteEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        (config.colorWriteRgb
             ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT)
             : 0u) |
        (config.colorWriteAlpha ? VK_COLOR_COMPONENT_A_BIT : 0u);
    blendAttachment.blendEnable = config.blendEnable ? VK_TRUE : VK_FALSE;
    // Straight-alpha blending by default: out = src.rgb*src.a + dst.rgb*(1-src.a) — the config's
    // factors, which the transmissive mesh variants set to multiply or add instead.
    blendAttachment.srcColorBlendFactor = config.srcColorBlendFactor;
    blendAttachment.dstColorBlendFactor = config.dstColorBlendFactor;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    // Attachment 1 (the HDR pass's specular target — see PipelineConfig): plainly written by the
    // opaque mesh pipeline, fully masked off for every other pipeline in that pass.
    constexpr VkColorComponentFlags kRgba =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState blendAttachments[2] = {blendAttachment, {}};
    blendAttachments[1].colorWriteMask = config.writeColorAttachment1 ? kRgba : 0u;
    blendAttachments[1].blendEnable = VK_FALSE;
    if (config.colorAttachmentCount > 1 && !m_context.supportsIndependentBlend()) {
        // Without the independentBlend feature every element of pAttachments must be IDENTICAL
        // (VUID-VkPipelineColorBlendStateCreateInfo-pAttachments-00605), so the specular
        // attachment inherits attachment 0's blend + write mask. On such a device the specular
        // target then receives whatever each pipeline writes to location 1 — the opaque mesh
        // pass's real specular, but also the (undeclared, hence undefined) location-1 output of
        // the overlays and the transparent pass — and the SSS V-pass adds that back over the
        // diffused image: a visual degradation of the skin's specular, not a crash. Every desktop
        // GPU and MoltenVK has the feature; this keeps an exotic device conformant.
        blendAttachments[1] = blendAttachments[0];
    }

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    // Clamped to the blendAttachments array's 2 entries — a future config asking for more must
    // grow that array, not silently read past it. 0 = depth-only pass.
    colorBlend.attachmentCount =
        config.hasColorAttachment ? std::min(config.colorAttachmentCount, 2u) : 0;
    colorBlend.pAttachments = config.hasColorAttachment ? blendAttachments : nullptr;

    const std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // A single push-constant block (if any), visible to the stages the config names.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = config.pushConstantStages;
    pushRange.offset = 0;
    pushRange.size = config.pushConstantSize;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(config.descriptorSetLayouts.size());
    layoutInfo.pSetLayouts =
        config.descriptorSetLayouts.empty() ? nullptr : config.descriptorSetLayouts.data();
    layoutInfo.pushConstantRangeCount = (config.pushConstantSize > 0) ? 1 : 0;
    layoutInfo.pPushConstantRanges = (config.pushConstantSize > 0) ? &pushRange : nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout));
    m_layout.reset(device, layout);

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_layout.get();
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    m_pipeline.reset(device, pipeline);
}

UniqueShaderModule VulkanPipeline::createShaderModule(const std::vector<char>& spirv) const {
    if (spirv.empty() || (spirv.size() % 4) != 0) {
        throw VulkanError("SPIR-V bytecode is empty or not a multiple of 4 bytes.");
    }
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size();
    // The data() pointer must be 4-byte aligned; std::vector's allocator guarantees that.
    ci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());

    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(m_context.device(), &ci, nullptr, &module));
    return UniqueShaderModule(m_context.device(), module);
}

} // namespace pose
