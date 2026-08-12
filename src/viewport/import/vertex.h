/**
 * @file vertex.h
 * @brief The interleaved vertex layout shared by every model importer, the GPU mesh, and the mesh
 *        pipeline's vertex-input state.
 *
 * This is the geometry contract importers produce: position + normal + UV, the minimum that
 * supports lit, textured meshes. It lives in import/ (alongside ModelData) because it defines what
 * an importer hands back; scene/ consumes it when uploading to the GPU. UVs are always carried (even
 * by importers that ignore textures) so adding texturing is additive and never re-churns the vertex
 * format. Pure GLM + Vulkan, no Qt.
 */

#ifndef VERTEX_H
#define VERTEX_H

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>

namespace pose {

struct Vertex {
    glm::vec3  pos;
    glm::vec3  normal;
    glm::vec2  uv;
    // Skinning: up to 4 influencing joints (indices into the model's joint-matrix buffer) and their
    // weights. The defaults bind the vertex fully to joint 0, so meshes that don't skin (OBJ) — and
    // the model's single identity joint — leave the vertex untransformed. Figure importers overwrite
    // these from the parsed skin weights.
    glm::uvec4 joints{0, 0, 0, 0};
    glm::vec4  weights{1.0f, 0.0f, 0.0f, 0.0f};

    /// Single interleaved binding at slot 0.
    static VkVertexInputBindingDescription bindingDescription() {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    /// loc0 = pos (vec3), loc1 = normal (vec3), loc2 = uv (vec2), loc3 = joints (uvec4),
    /// loc4 = weights (vec4) — matches mesh.vert.
    static std::array<VkVertexInputAttributeDescription, 5> attributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 5> attrs{};
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
        attrs[3].location = 3;
        attrs[3].binding = 0;
        attrs[3].format = VK_FORMAT_R32G32B32A32_UINT;
        attrs[3].offset = offsetof(Vertex, joints);
        attrs[4].location = 4;
        attrs[4].binding = 0;
        attrs[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[4].offset = offsetof(Vertex, weights);
        return attrs;
    }
};

} // namespace pose

#endif // VERTEX_H
