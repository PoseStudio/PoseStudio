/**
 * @file groundsampler.cpp
 * @brief The ground-sample store and the posed lowest-point scan. See groundsampler.h.
 */

#include "groundsampler.h"

#include "armature.h"
#include "vertex.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace pose {

void GroundSampler::reserve(std::size_t vertexCount, std::size_t triangleCount) {
    m_samples.reserve(vertexCount);
    m_triangles.reserve(triangleCount);
}

void GroundSampler::add(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, bool pickable, int mesh) {
    const uint32_t base = static_cast<uint32_t>(m_samples.size());
    for (const Vertex& v : vertices) {
        m_samples.push_back({v.pos, v.joints, v.weights});
    }
    if (!pickable) {
        return;
    }
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        if (indices[i] < vertices.size() && indices[i + 1] < vertices.size() && indices[i + 2] < vertices.size()) {
            m_triangles.push_back({base + indices[i], base + indices[i + 1], base + indices[i + 2], mesh});
        }
    }
}

bool GroundSampler::pickSurface(const Armature& armature, const Ray& ray, float& tOut, glm::vec3& hitWorld,
                                int& bone, int* meshOut) const {
    static const bool kTrace = std::getenv("POSESTUDIO_PICK_TRACE") != nullptr;
    if (kTrace) {
        std::fprintf(stderr, "[pick] skeleton=%d samples=%zu triangles=%zu origin(%.3f %.3f %.3f) dir(%.3f %.3f %.3f)\n",
                     armature.hasSkeleton() ? 1 : 0, m_samples.size(), m_triangles.size(), ray.origin.x, ray.origin.y, ray.origin.z,
                     ray.direction.x, ray.direction.y, ray.direction.z);
    }
    if (m_samples.empty() || m_triangles.empty()) {
        return false;
    }
    const float dirLen = glm::length(ray.direction);
    if (dirLen < 1.0e-9f) {
        return false;
    }
    const glm::vec3 dir = ray.direction / dirLen;
    // The skin as it stands: every sample CPU-skinned with the current pose (as lowestY: linear
    // blending, which agrees with the GPU's dual quaternions to millimetres — a pick's tolerance).
    // (A STATIC model — an OBJ, a prop — has no skeleton: its samples go through the transform
    // alone, so a click can find its material zone as it finds a figure's.)
    const bool        skinned = armature.hasSkeleton() && armature.boneCount() > 0;
    const std::size_t boneCount = skinned ? armature.boneCount() : 0;
    std::vector<glm::mat4> skin(boneCount);
    for (std::size_t i = 0; i < boneCount; ++i) {
        skin[i] = armature.transform() * armature.poseGlobal(i) * armature.inverseBind(i);
    }
    std::vector<glm::vec3> world(m_samples.size());
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (std::size_t i = 0; i < m_samples.size(); ++i) {
        const GroundSample& s = m_samples[i];
        const glm::vec4 p(s.pos, 1.0f);
        glm::vec3 posed(0.0f);
        if (!skinned) {
            posed = glm::vec3(armature.transform() * p);
        }
        for (int j = 0; j < 4 && skinned; ++j) {
            const float w = s.weights[j];
            if (w > 0.0f && s.joints[j] < skin.size()) {
                posed += w * glm::vec3(skin[s.joints[j]] * p);
            }
        }
        world[i] = posed;
        lo = glm::min(lo, posed);
        hi = glm::max(hi, posed);
    }
    // The posed box first: most of a click's rays miss the figure altogether.
    {
        float tMin = 0.0f;
        float tMax = std::numeric_limits<float>::max();
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(dir[axis]) < 1.0e-8f) {
                if (ray.origin[axis] < lo[axis] || ray.origin[axis] > hi[axis]) {
                    return false;
                }
                continue;
            }
            float t1 = (lo[axis] - ray.origin[axis]) / dir[axis];
            float t2 = (hi[axis] - ray.origin[axis]) / dir[axis];
            if (t1 > t2) {
                std::swap(t1, t2);
            }
            tMin = std::max(tMin, t1);
            tMax = std::min(tMax, t2);
            if (tMin > tMax) {
                if (kTrace) {
                    std::fprintf(stderr, "[pick] box miss: lo(%.3f %.3f %.3f) hi(%.3f %.3f %.3f)\n", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
                }
                return false;
            }
        }
    }
    // The nearest triangle along the ray (Moller-Trumbore, two-sided: the mesh is drawn without
    // back-face culling, and a click on a surface seen from inside is still a click on it).
    float best = std::numeric_limits<float>::max();
    int   bestTri = -1;
    float bestU = 0.0f, bestV = 0.0f;
    for (std::size_t k = 0; k < m_triangles.size(); ++k) {
        const PickTriangle& tri = m_triangles[k];
        const glm::vec3& a = world[tri.a];
        const glm::vec3 e1 = world[tri.b] - a;
        const glm::vec3 e2 = world[tri.c] - a;
        const glm::vec3 p = glm::cross(dir, e2);
        const float det = glm::dot(e1, p);
        if (std::abs(det) < 1.0e-12f) {
            continue;
        }
        const float inv = 1.0f / det;
        const glm::vec3 s = ray.origin - a;
        const float u = glm::dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f) {
            continue;
        }
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(dir, q) * inv;
        if (v < 0.0f || u + v > 1.0f) {
            continue;
        }
        const float t = glm::dot(e2, q) * inv;
        if (t <= 1.0e-4f || t >= best) {
            continue;
        }
        best = t;
        bestTri = static_cast<int>(k);
        bestU = u;
        bestV = v;
    }
    if (kTrace) {
        std::fprintf(stderr, "[pick] best t %.4f tri %d\n", best, bestTri);
    }
    if (bestTri < 0) {
        return false;
    }
    if (meshOut != nullptr) {
        *meshOut = m_triangles[static_cast<std::size_t>(bestTri)].mesh;
    }
    // The bone: the joint that weighs most on the skin at the hit — the corners' weights blended
    // by the hit's barycentric coordinates.
    const PickTriangle& tri = m_triangles[static_cast<std::size_t>(bestTri)];
    const uint32_t corners[3] = {tri.a, tri.b, tri.c};
    const float    bary[3] = {1.0f - bestU - bestV, bestU, bestV};
    unsigned       joints[12];
    float          weights[12];
    int            count = 0;
    for (int c = 0; c < 3; ++c) {
        const GroundSample& s = m_samples[corners[c]];
        for (int j = 0; j < 4; ++j) {
            const float w = s.weights[j] * bary[c];
            if (w <= 0.0f) {
                continue;
            }
            int found = -1;
            for (int k = 0; k < count; ++k) {
                if (joints[k] == s.joints[j]) {
                    found = k;
                    break;
                }
            }
            if (found >= 0) {
                weights[found] += w;
            } else {
                joints[count] = s.joints[j];
                weights[count] = w;
                ++count;
            }
        }
    }
    int   pick = -1;
    float most = -1.0f;
    for (int k = 0; k < count; ++k) {
        if (weights[k] > most && joints[k] < boneCount) {
            most = weights[k];
            pick = static_cast<int>(joints[k]);
        }
    }
    if (pick < 0) {
        return false;
    }
    tOut = best;
    hitWorld = ray.origin + dir * best;
    bone = pick;
    return true;
}

bool GroundSampler::lowestY(const Armature& armature, const glm::vec3& boundsMin,
                            const glm::vec3& boundsMax, bool hasBounds, float& out) const {
    float minY = std::numeric_limits<float>::max();
    if (armature.hasSkeleton() && !m_samples.empty()) {
        // Posed lowest point: CPU-skin every ground sample with the CURRENT pose's skin matrices
        // (poseGlobal · inverseBind — the armature's poseGlobal always holds the last pose
        // update) and track the world-space minimum. A one-shot ~few-ms walk; corrective deltas
        // are ignored (they move contact regions by millimetres at most). Linear matrix blending
        // is fine here even though the GPU skins with dual quaternions: contact regions (feet,
        // knees) are near-single-joint weighted, where LBS and DQS agree exactly.
        const std::size_t boneCount = armature.boneCount();
        std::vector<glm::mat4> skin(boneCount);
        for (std::size_t i = 0; i < boneCount; ++i) {
            skin[i] = armature.poseGlobal(i) * armature.inverseBind(i);
        }
        for (const GroundSample& s : m_samples) {
            const glm::vec4 p(s.pos, 1.0f);
            glm::vec3 posed(0.0f);
            for (int j = 0; j < 4; ++j) {
                const float w = s.weights[j];
                if (w > 0.0f && s.joints[j] < skin.size()) {
                    posed += w * glm::vec3(skin[s.joints[j]] * p);
                }
            }
            minY = std::min(minY, (armature.transform() * glm::vec4(posed, 1.0f)).y);
        }
    } else if (hasBounds) {
        // Static model: the bind AABB through the transform is exact (it can't pose).
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 c((i & 1) ? boundsMax.x : boundsMin.x, (i & 2) ? boundsMax.y : boundsMin.y,
                              (i & 4) ? boundsMax.z : boundsMin.z);
            minY = std::min(minY, (armature.transform() * glm::vec4(c, 1.0f)).y);
        }
    } else {
        return false; // no geometry to ground
    }
    if (!(minY < std::numeric_limits<float>::max())) {
        return false; // nothing sampled
    }
    out = minY;
    return true;
}

} // namespace pose
