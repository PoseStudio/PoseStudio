/**
 * @file vertex.h
 * @brief The interleaved vertex layout shared by the OBJ loader, the GPU mesh, and the mesh
 *        pipeline's vertex-input state.
 *
 * Position + normal + UV is the minimum that supports lit, eventually-textured meshes. UVs are
 * carried now (even though the first importer ignores textures) so adding texturing later is
 * additive and never re-churns the vertex format. Pure GLM + Vulkan, no Qt.
 */

#ifndef VERTEX_H
#define VERTEX_H

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>

namespace pose {

struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;

    /// Single interleaved binding at slot 0.
    static VkVertexInputBindingDescription bindingDescription() {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    /// loc0 = pos (vec3), loc1 = normal (vec3), loc2 = uv (vec2) — matches mesh.vert.
    static std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3> attrs{};
        attrs[0].location = 0;
        attrs[0].binding = 0;
        attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset = offsetof(Vertex, pos);
        attrs[1].location = 1;
        attrs[1].binding = 0;
        attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset = offsetof(Vertex, normal);
        attrs[2].location = 2;
        attrs[2].binding = 0;
        attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
        attrs[2].offset = offsetof(Vertex, uv);
        return attrs;
    }
};

} // namespace pose

#endif // VERTEX_H
