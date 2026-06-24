/**
 * @file mesh.h
 * @brief GPU-resident geometry: a Mesh (one material group) and a Model (one imported file).
 *
 * A Mesh owns device-local vertex + index buffers, its material base color, and its diffuse
 * texture (or none, in which case it points at the scene's shared white fallback), plus the
 * descriptor set (set 1) that binds that texture. A Model groups the meshes of one OBJ under a
 * shared transform and owns the descriptor pool those sets are allocated from. Qt-free.
 */

#ifndef MESH_H
#define MESH_H

#include "vulkanbuffer.h"
#include "vulkanimage.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace pose {

class VulkanContext;
struct MeshData;
struct ModelData;

/// Per-draw push-constant block for the mesh shaders. Must stay byte-identical to the
/// `push_constant` block in mesh.vert / mesh.frag (mat4 + vec4 = 80 bytes).
struct MeshPushConstants {
    glm::mat4 model;
    glm::vec4 baseColor; // rgb used; a reserved for later (e.g. opacity)
};

/// One material group of a model: device-local vertex/index buffers, base color, and a diffuse
/// texture bound through its own set-1 descriptor.
class Mesh {
public:
    /// @param materialSetLayout  The shared set-1 layout (Scene owns it).
    /// @param materialPool       The owning Model's pool to allocate this mesh's set from.
    /// @param fallback           Shared 1x1 white texture, used when the mesh has no texture.
    Mesh(VulkanContext& context, const MeshData& data, VkDescriptorSetLayout materialSetLayout,
         VkDescriptorPool materialPool, const VulkanTexture& fallback);

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) noexcept = default;
    Mesh& operator=(Mesh&&) noexcept = default;

    /// Binds set 1 (this mesh's texture), pushes (model, baseColor), binds buffers, and draws.
    /// The caller has already bound the mesh pipeline and the per-frame camera set (set 0).
    void record(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& model) const;

private:
    VulkanBuffer                   m_vertexBuffer;
    VulkanBuffer                   m_indexBuffer;
    uint32_t                       m_indexCount = 0;
    glm::vec3                      m_baseColor{0.8f};
    std::unique_ptr<VulkanTexture> m_texture;                     // null => uses the fallback
    VkDescriptorSet                m_materialSet = VK_NULL_HANDLE; // set 1; owned by the Model's pool
};

/// One imported OBJ: its meshes (each a material group) plus a shared model transform. Owns the
/// descriptor pool the meshes' set-1 descriptors are allocated from.
class Model {
public:
    Model(VulkanContext& context, const ModelData& data, VkDescriptorSetLayout materialSetLayout,
          const VulkanTexture& fallback);
    ~Model();

    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    /// Records every sub-mesh at the model's current transform.
    void record(VkCommandBuffer cmd, VkPipelineLayout layout) const;

    void             setTransform(const glm::mat4& transform) { m_transform = transform; }
    const glm::mat4& transform() const { return m_transform; }

private:
    VulkanContext&    m_context;
    VkDescriptorPool  m_materialPool = VK_NULL_HANDLE;
    std::vector<Mesh> m_meshes;
    glm::mat4         m_transform{1.0f};
};

} // namespace pose

#endif // MESH_H
