/**
 * @file scene.cpp
 * @brief Implementation of Scene. See scene.h.
 */

#include "scene.h"

#include "camera.h"
#include "mesh.h"
#include "objloader.h"
#include "vertex.h"
#include "vulkancommon.h"
#include "vulkancontext.h"
#include "vulkanimage.h"
#include "vulkanpipeline.h"

#include <glm/glm.hpp>

#include <array>
#include <cstring>

namespace pose {

namespace {

// std140-compatible camera + lighting block. vec3 fields are padded to vec4. Must match the
// `CameraUbo` block declared in mesh.vert / mesh.frag.
struct CameraUbo {
    glm::mat4 viewProj;
    glm::vec4 cameraPos;   // xyz world-space eye
    glm::vec4 lightDir;    // xyz = normalized direction TO the key light
    glm::vec4 lightColor;  // rgb
    glm::vec4 ambient;     // rgb ambient/fill term
};

} // namespace

Scene::Scene(VulkanContext& context, VkRenderPass renderPass,
             const std::vector<char>& vertSpirv, const std::vector<char>& fragSpirv)
    : m_context(context) {
    createDescriptorResources();

    // 1x1 opaque-white fallback texture so untextured meshes can share the same pipeline/shader:
    // sampling white and multiplying by baseColor just yields baseColor.
    const uint8_t white[4] = {255, 255, 255, 255};
    m_fallbackTexture = std::make_unique<VulkanTexture>(m_context, white, 1, 1);

    PipelineConfig config;
    config.pushConstantSize = sizeof(MeshPushConstants);
    config.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    config.depthTestEnable = true;
    config.depthWriteEnable = true;
    config.blendEnable = false;
    // No back-face culling: imported OBJ winding is inconsistent in the wild, so we'd risk an
    // invisible/inside-out model. The fragment shader instead flips the normal per gl_FrontFacing
    // for correct two-sided lighting. Revisit once we normalize winding on import.
    config.cullMode = VK_CULL_MODE_NONE;

    const VkVertexInputBindingDescription binding = Vertex::bindingDescription();
    const std::array<VkVertexInputAttributeDescription, 3> attrs = Vertex::attributeDescriptions();
    config.vertexBindings.assign(1, binding);
    config.vertexAttributes.assign(attrs.begin(), attrs.end());
    config.descriptorSetLayouts = {m_setLayout, m_materialSetLayout}; // set 0 = camera, set 1 = material

    m_pipeline = std::make_unique<VulkanPipeline>(m_context, renderPass, vertSpirv, fragSpirv, config);
}

Scene::~Scene() {
    // Nothing in flight may reference the pipeline/descriptors when we tear them down.
    vkDeviceWaitIdle(m_context.device());

    m_models.clear();         // frees mesh buffers, textures, and per-model descriptor pools
    m_pipeline.reset();
    m_fallbackTexture.reset();
    m_cameraBuffers.clear();   // VulkanBuffers free themselves

    VkDevice device = m_context.device();
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, m_descriptorPool, nullptr); // frees the camera sets too
    }
    if (m_setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
    }
    if (m_materialSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, m_materialSetLayout, nullptr);
    }
}

void Scene::createDescriptorResources() {
    VkDevice device = m_context.device();

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_setLayout));

    // Set 1: a single combined image sampler (the diffuse texture), read in the fragment stage.
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo materialLayoutInfo{};
    materialLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    materialLayoutInfo.bindingCount = 1;
    materialLayoutInfo.pBindings = &samplerBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &materialLayoutInfo, nullptr, &m_materialSetLayout));

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = kMaxFramesInFlight;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = kMaxFramesInFlight;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_descriptorPool));

    const std::vector<VkDescriptorSetLayout> layouts(kMaxFramesInFlight, m_setLayout);
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = m_descriptorPool;
    setAlloc.descriptorSetCount = kMaxFramesInFlight;
    setAlloc.pSetLayouts = layouts.data();
    m_cameraSets.resize(kMaxFramesInFlight);
    VK_CHECK(vkAllocateDescriptorSets(device, &setAlloc, m_cameraSets.data()));

    m_cameraBuffers.reserve(kMaxFramesInFlight);
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        m_cameraBuffers.emplace_back(createMappedUniformBuffer(m_context, sizeof(CameraUbo)));

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_cameraBuffers[i].handle();
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(CameraUbo);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_cameraSets[i];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}

void Scene::addModel(const ModelData& data) {
    m_models.push_back(std::make_unique<Model>(m_context, data, m_materialSetLayout,
                                               *m_fallbackTexture));
}

void Scene::record(VkCommandBuffer cmd, const Camera& camera, uint32_t frameIndex) {
    if (m_models.empty()) {
        return; // nothing to draw; the grid still renders on its own
    }

    // Update this frame's camera + lighting UBO. The key light is a fixed studio-ish direction for
    // now (above/front), with a soft ambient fill; later this comes from real scene lighting.
    CameraUbo ubo{};
    ubo.viewProj = camera.viewProjection();
    ubo.cameraPos = glm::vec4(camera.position(), 1.0f);
    ubo.lightDir = glm::vec4(glm::normalize(glm::vec3(0.4f, 0.9f, 0.5f)), 0.0f);
    ubo.lightColor = glm::vec4(1.0f, 0.98f, 0.95f, 0.0f);
    ubo.ambient = glm::vec4(0.18f, 0.19f, 0.22f, 0.0f);
    std::memcpy(m_cameraBuffers[frameIndex].mappedData(), &ubo, sizeof(ubo));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->layout(), 0, 1,
                            &m_cameraSets[frameIndex], 0, nullptr);

    for (const std::unique_ptr<Model>& model : m_models) {
        model->record(cmd, m_pipeline->layout());
    }
}

} // namespace pose
