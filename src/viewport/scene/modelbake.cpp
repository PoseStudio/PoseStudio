/**
 * @file modelbake.cpp
 * @brief Model::bakePosedMesh — a mesh as posed, on the CPU, for a render engine. See model.h.
 *
 * THE VERTEX STAGE, REPEATED (mesh.vert through skinning.glsl and correctives.glsl): the pose
 * correctives and the soft-body fields are blended into the bind position by the vertex's packed
 * range into the model's delta table, the result is skinned — the influencing joints' dual
 * quaternions blended, sign-aligned against the first joint's hemisphere, and normalized, or
 * (a rig authored for it) each joint's rigid transform blended linearly — and carried through
 * the model transform. The arithmetic is the shaders', in the shaders' order, so a render shows
 * the surface the viewport shows: change one and change the other.
 */

#include "model.h"

#include "parallelfor.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace pose {

namespace {

/// Rotates v by unit quaternion r ((xyz, w) layout) — skinning.glsl's quatRotate.
glm::vec3 quatRotate(const glm::vec4& r, const glm::vec3& v) {
    const glm::vec3 q(r);
    return v + 2.0f * glm::cross(q, glm::cross(q, v) + r.w * v);
}

/// The rigid transform of a (normalized) dual quaternion applied to a point — dualQuatTransform.
glm::vec3 dualQuatTransform(const glm::vec4& real, const glm::vec4& dual, const glm::vec3& p) {
    const glm::vec3 r(real);
    const glm::vec3 d(dual);
    return quatRotate(real, p) + 2.0f * (real.w * d - dual.w * r + glm::cross(r, d));
}

} // namespace

bool Model::bakePosedMesh(std::size_t i, PosedMesh& out) {
    if (i >= m_meshes.size()) {
        return false;
    }
    // The corrective weights for the pose as it stands (a frame recorded since the pose last
    // moved has evaluated them already; a bake straight after an edit has not).
    m_correctives.refresh(m_armature);

    const Mesh&                         mesh = m_meshes[i];
    const std::vector<Vertex>&          vertices = mesh.cpuVertices();
    const std::vector<CorrectiveEntry>& entries = m_correctives.entries();
    const std::vector<float>&           weights = m_correctives.weights();
    const std::vector<glm::vec4>&       dq = m_armature.skinDualQuats(); // (real, dual) per joint
    const std::size_t                   jointCount = dq.size() / 2;
    const glm::mat4&                    model = m_armature.transform();
    const glm::mat3                     linear(model); // (rigid or uniformly scaled: mesh.vert's rule)
    const bool                          linearSkinning = m_linearSkinning;

    const std::size_t count = vertices.size();
    out.positions.resize(count);
    out.normals.resize(count);
    out.uvs.resize(count);
    out.tangents.resize(count);
    out.indices = mesh.cpuIndices();
    if (count == 0 || jointCount == 0) {
        return count != 0;
    }

    const auto joint = [&](uint32_t index) { return std::min<std::size_t>(index, jointCount - 1); };
    constexpr std::size_t kChunk = 4096;
    const int             chunks = static_cast<int>((count + kChunk - 1) / kChunk);
    parallelFor(chunks, [&](int chunk) {
        const std::size_t begin = static_cast<std::size_t>(chunk) * kChunk;
        const std::size_t end = std::min(count, begin + kChunk);
        for (std::size_t v = begin; v < end; ++v) {
            const Vertex& in = vertices[v];

            // applyCorrectives: the bind position plus the vertex's weighted deltas.
            glm::vec3      bind = in.pos;
            const uint32_t deltaCount = in.correctiveRange & 0xFFu;
            const uint32_t deltaFirst = in.correctiveRange >> 8;
            for (uint32_t k = 0; k < deltaCount; ++k) {
                const std::size_t e = static_cast<std::size_t>(deltaFirst) + k;
                if (e < entries.size() && entries[e].corrective < weights.size()) {
                    bind += weights[entries[e].corrective] * entries[e].delta;
                }
            }

            glm::vec3 position;
            glm::vec3 normal;
            glm::vec3 tangent;
            if (linearSkinning) {
                position = glm::vec3(0.0f);
                normal = glm::vec3(0.0f);
                tangent = glm::vec3(0.0f);
                for (int k = 0; k < 4; ++k) {
                    const float       w = in.weights[k];
                    const std::size_t j = joint(in.joints[k]);
                    position += w * dualQuatTransform(dq[2 * j], dq[2 * j + 1], bind);
                    normal += w * quatRotate(dq[2 * j], in.normal);
                    tangent += w * quatRotate(dq[2 * j], glm::vec3(in.tangent));
                }
            } else {
                // blendDualQuat: sign-align against the first joint's hemisphere, then normalize.
                const glm::vec4 r0 = dq[2 * joint(in.joints[0])];
                glm::vec4       real = in.weights[0] * r0;
                glm::vec4       dual = in.weights[0] * dq[2 * joint(in.joints[0]) + 1];
                for (int k = 1; k < 4; ++k) {
                    const std::size_t j = joint(in.joints[k]);
                    const float       s = glm::dot(dq[2 * j], r0) < 0.0f ? -1.0f : 1.0f;
                    real += (in.weights[k] * s) * dq[2 * j];
                    dual += (in.weights[k] * s) * dq[2 * j + 1];
                }
                const float invLength = 1.0f / std::max(glm::length(real), 1.0e-6f);
                real *= invLength;
                dual *= invLength;
                position = dualQuatTransform(real, dual, bind);
                normal = quatRotate(real, in.normal);
                tangent = quatRotate(real, glm::vec3(in.tangent));
            }

            out.positions[v] = glm::vec3(model * glm::vec4(position, 1.0f));
            const glm::vec3 worldNormal = linear * normal;
            const float     normalLength = glm::length(worldNormal);
            out.normals[v] = normalLength > 1.0e-12f ? worldNormal / normalLength : glm::vec3(0.0f, 1.0f, 0.0f);
            out.tangents[v] = glm::vec4(linear * tangent, in.tangent.w);
            out.uvs[v] = in.uv;
        }
    });
    return true;
}

} // namespace pose
