/**
 * @file mesh.cpp
 * @brief Implementation of Mesh and Model. See mesh.h.
 */

#include "mesh.h"

#include "objloader.h"
#include "vertex.h"
#include "vulkancommon.h"
#include "vulkancontext.h"

namespace pose {

Mesh::Mesh(VulkanContext& context, const MeshData& data, VkDescriptorSetLayout materialSetLayout,
           VkDescriptorPool materialPool, const VulkanTexture& fallback)
    : m_indexCount(static_cast<uint32_t>(data.indices.size())), m_baseColor(data.baseColor) {
    const VkDeviceSize vertexBytes = sizeof(Vertex) * data.vertices.size();
    const VkDeviceSize indexBytes = sizeof(uint32_t) * data.indices.size();
    m_vertexBuffer = createDeviceLocalBuffer(context, data.vertices.data(), vertexBytes,
                                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m_indexBuffer = createDeviceLocalBuffer(context, data.indices.data(), indexBytes,
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    // Upload the diffuse texture if the Qt layer decoded one for this mesh; else use the fallback.
    if (data.diffuseWidth > 0 && data.diffuseHeight > 0 && !data.diffusePixels.empty()) {
        m_texture = std::make_unique<VulkanTexture>(context, data.diffusePixels.data(),
                                                    data.diffuseWidth, data.diffuseHeight);
    }
    const VulkanTexture& texture = m_texture ? *m_texture : fallback;

    // Allocate this mesh's set-1 descriptor from the owning Model's pool and point it at the texture.
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = materialPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &materialSetLayout;
    VK_CHECK(vkAllocateDescriptorSets(context.device(), &allocInfo, &m_materialSet));

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = texture.imageView();
    imageInfo.sampler = texture.sampler();

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_materialSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(context.device(), 1, &write, 0, nullptr);
}

void Mesh::record(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& model) const {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &m_materialSet, 0,
                            nullptr); // set 1 = this mesh's diffuse texture

    MeshPushConstants push{};
    push.model = model;
    push.baseColor = glm::vec4(m_baseColor, 1.0f);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);

    const VkBuffer vertexBuffer = m_vertexBuffer.handle();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, m_indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m_indexCount, 1, 0, 0, 0);
}

Model::Model(VulkanContext& context, const ModelData& data, VkDescriptorSetLayout materialSetLayout,
             const VulkanTexture& fallback)
    : m_context(context) {
    uint32_t meshCount = 0;
    for (const MeshData& meshData : data.meshes) {
        if (!meshData.indices.empty()) {
            ++meshCount;
        }
    }
    if (meshCount == 0) {
        return; // nothing to draw; no pool/sets needed
    }

    // One combined-image-sampler descriptor (set 1) per mesh, from this model's own pool.
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = meshCount;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = meshCount;
    VK_CHECK(vkCreateDescriptorPool(m_context.device(), &poolInfo, nullptr, &m_materialPool));

    m_meshes.reserve(meshCount);
    for (const MeshData& meshData : data.meshes) {
        if (meshData.indices.empty()) {
            continue;
        }
        m_meshes.emplace_back(m_context, meshData, materialSetLayout, m_materialPool, fallback);
    }
}

Model::~Model() {
    // Destroying the pool frees all of this model's set-1 descriptors; the meshes (textures +
    // buffers) are freed afterwards as members. The device is idle by the time a Model dies
    // (Scene waits before tearing models down), so the freed sets are never referenced again.
    if (m_materialPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_context.device(), m_materialPool, nullptr);
    }
}

void Model::record(VkCommandBuffer cmd, VkPipelineLayout layout) const {
    for (const Mesh& mesh : m_meshes) {
        mesh.record(cmd, layout, m_transform);
    }
}

} // namespace pose
