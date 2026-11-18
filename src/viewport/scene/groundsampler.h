/**
 * @file groundsampler.h
 * @brief A model's SURFACE ON THE CPU — a compact copy of its vertices and triangles — for the
 *        three questions asked of the posed surface: the CURRENT pose's lowest point (the ground
 *        button / the animated ground drop: lowestY), what a click is ON (pickSurface: the
 *        opaque meshes' triangles, with the joint and the material zone at the hit), and whether
 *        the cursor is on the model at all (pickAnySurface: the see-through meshes too).
 *
 * (The name is its first job's; it has been the model's CPU surface for all three since the
 * picks came.) The GPU vertex buffers are device-local and unreadable, and a posed figure's
 * lowest point is not its bind box's: a knee bend lifts the feet, so grounding by the bind AABB
 * would sink or float the figure. So every render vertex's skinning inputs (position + joints +
 * weights, ~44 bytes/vertex, ~10 MB for a figure) are kept here and CPU-skinned on demand with
 * the armature's live pose. A STATIC model keeps its samples too, up to a triangle cap
 * (Model's constructor) — for the two picks, cast through its transform alone; grounding needs
 * none of them (its bind AABB through the transform is exact, and lowestY() handles that case
 * from the bounds). Qt-free, Vulkan-free.
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

    /// The FIRST point of the CURRENT pose's skin along @p ray (world space) — what a click is ON.
    /// CPU-skins every sample as lowestY() does (once per pose: posedSamples), tests the pickable
    /// triangles (two-sided, as the mesh is drawn) and takes the nearest hit: @p tOut its distance along the ray's direction
    /// (normalized), @p hitWorld the point, @p bone the joint that weighs most on the skin there
    /// (the three corners' weights blended at the hit), @p meshOut (optional) the model's mesh
    /// index the hit triangle belongs to — its material zone. False without samples or when the
    /// ray misses every triangle. A few milliseconds for a figure: a click's worth, never a
    /// frame's. A STATIC model's samples (no skeleton) are cast through the transform alone, and
    /// its hit names no joint: @p bone comes back -1 with the distance, the point and the zone.
    bool pickSurface(const Armature& armature, const Ray& ray, float& tOut, glm::vec3& hitWorld,
                     int& bone, int* meshOut = nullptr) const;

    /// The first point of the CURRENT pose's surface along @p ray counting EVERY mesh — the
    /// pickable skin and the see-through meshes pickSurface leaves out (hair cards, lashes, the
    /// eye's shells, glass) — for the question "is the cursor on this MODEL" (Model::pickExact):
    /// a click on a character's hair is a click on her, whatever stands behind it. @p tOut is
    /// the distance along the ray's direction (normalized) to the first triangle of either
    /// kind; @p tOpaqueOut to the first OPAQUE one — the first of the model that hides what is
    /// behind it — or the largest float where the ray meets see-through triangles alone. False
    /// without samples or triangles, or when the ray misses them all. (A card's clear texels
    /// count as the card: the cast knows triangles, not textures.)
    /// @p seeThrough says, per mesh of the model (the index add() was given), whether that mesh
    /// is see-through AS ITS MATERIAL STANDS — a zone made translucent on the Materials tab
    /// since the import counts as it is now; a mesh beyond the vector's end counts as it was
    /// imported.
    bool pickAnySurface(const Armature& armature, const Ray& ray, const std::vector<char>& seeThrough, float& tOut,
                        float& tOpaqueOut) const;

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
    std::vector<PickTriangle> m_softTriangles; ///< The see-through meshes' triangles (pickAnySurface alone).

    /// Every sample as the current pose (or, for a static model, the transform alone) has it, in
    /// world space — or null when @p dir from @p origin misses their box: most of a click's rays
    /// miss the model altogether. The positions are computed once per pose and kept (m_world):
    /// the two to four casts of one click, and every click until the pose moves, share one skin
    /// pass. Valid until the next call after the pose or the transform has moved.
    const std::vector<glm::vec3>* posedSamples(const Armature& armature, const glm::vec3& origin,
                                               const glm::vec3& dir) const;
    // The posed samples and their box, for the pose (Armature::skinVersion) and the model
    // transform they were skinned at. Mutable: a cache the const casts fill; the casts run on
    // the GUI thread alone. Costs 12 bytes a sample once a model has been cast against.
    mutable std::vector<glm::vec3> m_world;
    mutable glm::vec3              m_worldLo{0.0f};
    mutable glm::vec3              m_worldHi{0.0f};
    mutable std::uint64_t          m_worldSkinVersion = 0;
    mutable glm::mat4              m_worldTransform{1.0f};
    mutable bool                   m_worldValid = false;
    /// The nearest triangle of @p triangles along the ray (Moller-Trumbore, two-sided), nearer
    /// than @p best on entry: @p best, @p bestTri (an index into @p triangles) and the hit's
    /// barycentric @p bestU / @p bestV are updated on a nearer hit, else left as they were.
    /// With @p seeThrough (pickAnySurface's per-mesh flags) only the triangles of meshes that
    /// are — @p wantSeeThrough — or are not see-through are cast; @p listIsSoft is what a mesh
    /// beyond the flags' end counts as (the list it was imported into).
    static void nearestTriangle(const std::vector<PickTriangle>& triangles, const std::vector<glm::vec3>& world,
                                const glm::vec3& origin, const glm::vec3& dir, float& best, int& bestTri,
                                float& bestU, float& bestV, const std::vector<char>* seeThrough = nullptr,
                                bool listIsSoft = false, bool wantSeeThrough = false);
};

} // namespace pose

#endif // GROUNDSAMPLER_H
