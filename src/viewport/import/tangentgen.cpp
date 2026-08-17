/**
 * @file tangentgen.cpp
 * @brief Implementation of computeTangents. See tangentgen.h.
 */

#include "tangentgen.h"

#include "modeldata.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <vector>

namespace pose {

void computeTangents(ModelData& data) {
    for (MeshData& mesh : data.meshes) {
        if (mesh.indices.size() < 3 || mesh.vertices.empty()) {
            continue;
        }
        std::vector<glm::vec3> tan(mesh.vertices.size(), glm::vec3(0.0f));
        std::vector<glm::vec3> bitan(mesh.vertices.size(), glm::vec3(0.0f));

        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const uint32_t i0 = mesh.indices[i];
            const uint32_t i1 = mesh.indices[i + 1];
            const uint32_t i2 = mesh.indices[i + 2];
            const Vertex& v0 = mesh.vertices[i0];
            const Vertex& v1 = mesh.vertices[i1];
            const Vertex& v2 = mesh.vertices[i2];

            const glm::vec3 e1 = v1.pos - v0.pos;
            const glm::vec3 e2 = v2.pos - v0.pos;
            const glm::vec2 duv1 = v1.uv - v0.uv;
            const glm::vec2 duv2 = v2.uv - v0.uv;

            const float det = duv1.x * duv2.y - duv2.x * duv1.y;
            if (std::abs(det) < 1e-12f) {
                continue; // degenerate UVs — contribute nothing
            }
            const float r = 1.0f / det;
            const glm::vec3 t = (e1 * duv2.y - e2 * duv1.y) * r;
            const glm::vec3 b = (e2 * duv1.x - e1 * duv2.x) * r;
            for (const uint32_t idx : {i0, i1, i2}) {
                tan[idx] += t;
                bitan[idx] += b;
            }
        }

        for (std::size_t vi = 0; vi < mesh.vertices.size(); ++vi) {
            Vertex& v = mesh.vertices[vi];
            const glm::vec3 n = glm::normalize(v.normal);
            const glm::vec3 t = tan[vi];
            if (glm::dot(t, t) < 1e-16f) {
                v.tangent = glm::vec4(0.0f); // no UV signal — shader falls back to cotangent frame
                continue;
            }
            // Gram-Schmidt orthonormalize against the normal; w = bitangent handedness.
            const glm::vec3 ortho = glm::normalize(t - n * glm::dot(n, t));
            const float handed = (glm::dot(glm::cross(n, ortho), bitan[vi]) < 0.0f) ? -1.0f : 1.0f;
            v.tangent = glm::vec4(ortho, handed);
        }
    }
}

} // namespace pose
