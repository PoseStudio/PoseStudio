/**
 * @file aobaker.cpp
 * @brief Implementation of bakeVertexAO. See aobaker.h.
 */

#include "aobaker.h"

#include "modeldata.h"
#include "parallelfor.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pose {

namespace {

constexpr int   kSamples = 32;          // hemisphere rays per vertex (cosine-weighted Fibonacci set)
constexpr float kMaxDistance = 0.6f;    // occlusion range (world units ≈ m): local cavity AO only
constexpr float kRayEpsilon = 0.003f;   // origin offset along the normal (self-intersection guard)
constexpr std::size_t kMaxOccluderTriangles = 600000; // beyond this, skip the bake (import time)
constexpr int   kBvhLeafSize = 4;

struct Triangle {
    glm::vec3 a, b, c;
};

// Flat median-split BVH over triangle centroids; stack traversal, closest-hit within tMax.
struct BvhNode {
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    int32_t   left = -1;   // internal: child node indices; leaf: -1
    int32_t   right = -1;
    uint32_t  firstTri = 0; // leaf: range into the (reordered) triangle list
    uint32_t  triCount = 0;
};

class Bvh {
public:
    explicit Bvh(std::vector<Triangle> tris) : m_tris(std::move(tris)) {
        if (m_tris.empty()) {
            return;
        }
        m_nodes.reserve(m_tris.size() * 2);
        build(0, static_cast<uint32_t>(m_tris.size()));
    }

    bool empty() const { return m_tris.empty(); }

    /// Closest hit distance in (0, tMax) along the ray, or -1 if none.
    float raycast(const glm::vec3& origin, const glm::vec3& dir, float tMax) const {
        if (m_nodes.empty()) {
            return -1.0f;
        }
        const glm::vec3 invDir = 1.0f / dir; // IEEE inf on zero components is fine for slab tests
        float best = tMax;
        bool hit = false;
        int stack[64];
        int top = 0;
        stack[top++] = 0;
        while (top > 0) {
            const BvhNode& node = m_nodes[static_cast<std::size_t>(stack[--top])];
            if (!slabTest(node, origin, invDir, best)) {
                continue;
            }
            if (node.left < 0) { // leaf
                for (uint32_t i = 0; i < node.triCount; ++i) {
                    const float t = intersect(m_tris[node.firstTri + i], origin, dir, best);
                    if (t > 0.0f) {
                        best = t;
                        hit = true;
                    }
                }
            } else if (top + 2 <= static_cast<int>(sizeof(stack) / sizeof(stack[0]))) {
                stack[top++] = node.left;
                stack[top++] = node.right;
            }
        }
        return hit ? best : -1.0f;
    }

private:
    // Builds the node for triangle range [first, first+count) and returns its index.
    int32_t build(uint32_t first, uint32_t count) {
        const int32_t nodeIndex = static_cast<int32_t>(m_nodes.size());
        m_nodes.emplace_back();
        glm::vec3 mn(1e30f);
        glm::vec3 mx(-1e30f);
        for (uint32_t i = 0; i < count; ++i) {
            const Triangle& t = m_tris[first + i];
            mn = glm::min(mn, glm::min(t.a, glm::min(t.b, t.c)));
            mx = glm::max(mx, glm::max(t.a, glm::max(t.b, t.c)));
        }
        m_nodes[static_cast<std::size_t>(nodeIndex)].boundsMin = mn;
        m_nodes[static_cast<std::size_t>(nodeIndex)].boundsMax = mx;
        if (count <= static_cast<uint32_t>(kBvhLeafSize)) {
            m_nodes[static_cast<std::size_t>(nodeIndex)].firstTri = first;
            m_nodes[static_cast<std::size_t>(nodeIndex)].triCount = count;
            return nodeIndex;
        }
        // Split at the median centroid along the widest axis.
        const glm::vec3 extent = mx - mn;
        const int axis = (extent.x > extent.y && extent.x > extent.z) ? 0
                         : (extent.y > extent.z)                      ? 1
                                                                      : 2;
        const uint32_t mid = count / 2;
        std::nth_element(m_tris.begin() + first, m_tris.begin() + first + mid,
                         m_tris.begin() + first + count, [axis](const Triangle& x, const Triangle& y) {
                             return (x.a[axis] + x.b[axis] + x.c[axis]) <
                                    (y.a[axis] + y.b[axis] + y.c[axis]);
                         });
        const int32_t left = build(first, mid);
        const int32_t right = build(first + mid, count - mid);
        m_nodes[static_cast<std::size_t>(nodeIndex)].left = left;
        m_nodes[static_cast<std::size_t>(nodeIndex)].right = right;
        return nodeIndex;
    }

    static bool slabTest(const BvhNode& node, const glm::vec3& origin, const glm::vec3& invDir,
                         float tMax) {
        const glm::vec3 t0 = (node.boundsMin - origin) * invDir;
        const glm::vec3 t1 = (node.boundsMax - origin) * invDir;
        const glm::vec3 tsmall = glm::min(t0, t1);
        const glm::vec3 tbig = glm::max(t0, t1);
        const float tEnter = std::max(std::max(tsmall.x, tsmall.y), tsmall.z);
        const float tExit = std::min(std::min(tbig.x, tbig.y), std::min(tbig.z, tMax));
        return tEnter <= tExit && tExit > 0.0f;
    }

    // Möller–Trumbore, two-sided (imported winding is inconsistent). Returns t in (0, tMax) or -1.
    static float intersect(const Triangle& tri, const glm::vec3& origin, const glm::vec3& dir,
                           float tMax) {
        const glm::vec3 e1 = tri.b - tri.a;
        const glm::vec3 e2 = tri.c - tri.a;
        const glm::vec3 p = glm::cross(dir, e2);
        const float det = glm::dot(e1, p);
        if (std::abs(det) < 1e-9f) {
            return -1.0f;
        }
        const float invDet = 1.0f / det;
        const glm::vec3 s = origin - tri.a;
        const float u = glm::dot(s, p) * invDet;
        if (u < 0.0f || u > 1.0f) {
            return -1.0f;
        }
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(dir, q) * invDet;
        if (v < 0.0f || u + v > 1.0f) {
            return -1.0f;
        }
        const float t = glm::dot(e2, q) * invDet;
        return (t > 0.0f && t < tMax) ? t : -1.0f;
    }

    std::vector<Triangle> m_tris;
    std::vector<BvhNode>  m_nodes;
};

// Deterministic cosine-weighted hemisphere directions about +Z (Fibonacci spiral): sampling with
// pdf ∝ cosθ means visibility just averages — no per-sample weighting needed.
std::vector<glm::vec3> cosineHemisphere(int count) {
    std::vector<glm::vec3> dirs;
    dirs.reserve(static_cast<std::size_t>(count));
    const float golden = 2.39996323f; // golden angle (radians)
    for (int i = 0; i < count; ++i) {
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
        const float cosTheta = std::sqrt(1.0f - u); // cosine-weighted in elevation
        const float sinTheta = std::sqrt(u);
        const float phi = golden * static_cast<float>(i);
        dirs.emplace_back(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
    }
    return dirs;
}

} // namespace

void bakeVertexAO(ModelData& data) {
    // Occluders: every OPAQUE triangle across the model (transparent shells/cards receive AO but
    // must never cast it — a clear cornea darkening the iris would be wrong).
    std::vector<Triangle> tris;
    for (const MeshData& mesh : data.meshes) {
        if (mesh.opacity < 0.999f || mesh.hasOpacityMask) {
            continue;
        }
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            tris.push_back({mesh.vertices[mesh.indices[i]].pos,
                            mesh.vertices[mesh.indices[i + 1]].pos,
                            mesh.vertices[mesh.indices[i + 2]].pos});
        }
        if (tris.size() > kMaxOccluderTriangles) {
            return; // huge model: leave every vertex fully lit rather than stall the import
        }
    }
    if (tris.empty()) {
        return;
    }
    const Bvh bvh(std::move(tris));
    const std::vector<glm::vec3> hemisphere = cosineHemisphere(kSamples);

    for (MeshData& mesh : data.meshes) {
        parallelFor(static_cast<int>(mesh.vertices.size()), [&](int vi) {
            Vertex& v = mesh.vertices[static_cast<std::size_t>(vi)];
            const glm::vec3 n = glm::normalize(v.normal);
            // Tangent frame about the vertex normal for the hemisphere directions.
            const glm::vec3 helper =
                (std::abs(n.y) < 0.99f) ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 tangent = glm::normalize(glm::cross(helper, n));
            const glm::vec3 bitangent = glm::cross(n, tangent);
            const glm::vec3 origin = v.pos + n * kRayEpsilon;

            float occlusion = 0.0f;
            for (const glm::vec3& h : hemisphere) {
                const glm::vec3 dir = tangent * h.x + bitangent * h.y + n * h.z;
                const float t = bvh.raycast(origin, dir, kMaxDistance);
                if (t > 0.0f) {
                    occlusion += 1.0f - t / kMaxDistance; // nearer blockers occlude harder
                }
            }
            v.ao = glm::clamp(1.0f - occlusion / static_cast<float>(kSamples), 0.0f, 1.0f);
        });
    }
}

} // namespace pose
