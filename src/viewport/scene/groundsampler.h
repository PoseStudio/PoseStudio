/**
 * @file groundsampler.h
 * @brief A compact CPU copy of a skinned model's vertices, for finding the CURRENT pose's lowest
 *        point (the ground button / the animated ground drop) — and, since 2026-09-26, the
 *        SURFACE a click is on (pickSurface: the opaque meshes' triangles ride along).
 *
 * The GPU vertex buffers are device-local and unreadable, and a posed figure's lowest point is
 * not its bind box's: a knee bend lifts the feet, so grounding by the bind AABB would sink or
 * float the figure. So every render vertex's skinning inputs (position + joints + weights,
 * ~44 bytes/vertex, ~10 MB for a figure) are kept here and CPU-skinned on demand with the
 * armature's live pose. Static models need no samples: their bind AABB through the transform is
 * exact, and lowestY() handles that case from the bounds alone. Qt-free, Vulkan-free.
 */

#ifndef GROUNDSAMPLER_H
#define GROUNDSAMPLER_H

#include "ray.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pose {

class Armature;
struct Vertex;

class GroundSampler {
public:
    /// Reserves room for @p vertexCount samples. Reserve ONCE, summed across meshes: an
    /// exact-fit reserve per mesh would reallocate-and-copy the whole store every mesh (exact-fit
    /// defeats geometric growth), turning a figure's ~10 MB of samples into O(zones × total)
    /// redundant copying.
    void reserve(std::size_t vertexCount, std::size_t triangleCount);
    /// Appends every vertex of @p vertices (its position, joints, and weights) — and, when
    /// @p pickable, its triangles (@p indices, triples into @p vertices) join the SURFACE that
    /// pickSurface() casts against. Transparent shells and masked cards are not pickable: a click
    /// on a cornea is a click on the eye behind it, one on a lash card's clear part a click on
    /// the lid.
    /// @p mesh is the model's mesh index the triangles belong to — what a click's zone is
    /// (pickSurface's meshOut; the Materials tab shows the zone under the last click).
    void add(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, bool pickable, int mesh = 0);
    bool empty() const { return m_samples.empty(); }

    /// The FIRST point of the CURRENT pose's skin along @p ray (world space) — what a click is ON.
    /// CPU-skins every sample as lowestY() does, tests the pickable triangles (two-sided, as the
    /// mesh is drawn) and takes the nearest hit: @p tOut its distance along the ray's direction
    /// (normalized), @p hitWorld the point, @p bone the joint that weighs most on the skin there
    /// (the three corners' weights blended at the hit), @p meshOut (optional) the model's mesh
    /// index the hit triangle belongs to — its material zone. False without samples or when the
    /// ray misses every triangle. A few milliseconds for a figure: a click's worth, never a
    /// frame's. A STATIC model's samples (no skeleton) are cast through the transform alone.
    bool pickSurface(const Armature& armature, const Ray& ray, float& tOut, glm::vec3& hitWorld,
                     int& bone, int* meshOut = nullptr) const;

    /// The world height of the CURRENT pose's lowest point: positive = hovering that far above
    /// the floor, negative = sunk into it. With samples, CPU-skins every sample with
    /// @p armature's live pose; without (a static model), uses the bind AABB (@p boundsMin /
    /// @p boundsMax, valid when @p hasBounds) through the transform, which is exact for a model
    /// that can't pose. False when there is nothing to measure.
    bool lowestY(const Armature& armature, const glm::vec3& boundsMin, const glm::vec3& boundsMax,
                 bool hasBounds, float& out) const;

private:
    struct GroundSample {
        glm::vec3  pos;
        glm::uvec4 joints;
        glm::vec4  weights;
    };
    std::vector<GroundSample> m_samples;
    struct PickTriangle {
        uint32_t a, b, c; // indices into m_samples
        int      mesh;    // the model's mesh (material zone) the triangle belongs to
    };
    std::vector<PickTriangle> m_triangles; ///< The pickable surface (pickSurface).
};

} // namespace pose

#endif // GROUNDSAMPLER_H
