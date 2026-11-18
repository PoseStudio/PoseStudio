/**
 * @file meshdisplace.h
 * @brief DISPLACEMENT: a zone's posed mesh subdivided and moved by its displacement map, on the
 *        CPU, on its way to a render engine. Plain std + GLM: no Vulkan, no Qt.
 *
 * A displacement map is a height: where it is brighter than the material's mid level the
 * surface stands out, where it is darker it sinks in — real geometry, with a silhouette and
 * shadows of its own, where a bump or a normal map only shades. The Materials tab's three
 * values say how: the Strength (centimetres from the map's black to its white), the Mid Level
 * (the map value that moves nothing) and the Subdivision (how many times each triangle is cut
 * in four first: a displacement can only move vertices, so a coarse mesh needs more of them).
 *
 * WHERE IT HAPPENS. In the capture of the scene for a render (VulkanWindow::captureRenderScene),
 * after the vertex stage has posed the mesh and before the engine sees it — so a final render
 * and the Rendered viewport mode show it, and the engine's interface knows nothing of it. The
 * rasterized viewport modes do not displace.
 *
 * WHAT displaceMesh DOES, in order:
 *   1. SUBDIVIDES, linearly: each triangle into four at its edges' midpoints, `levels` times
 *      (fewer where the triangle budget says so). Positions, normals, UVs and tangents are
 *      interpolated; the shape is not rounded — the surface keeps its silhouette's polygon and
 *      gains only vertices to move.
 *   2. WELDS BY POSITION: vertices at the same place (to a micrometre) — the two sides of a UV
 *      seam, the corners of a hard edge — are one point of the surface, and move together, so
 *      no seam opens.
 *   3. SAMPLES the height at every vertex (the material's UV tiling, rotation and offset
 *      applied as the vertex shader applies them), from a copy of the map reduced until a
 *      texel is about as large as the mesh's triangles: a map finer than the mesh is averaged,
 *      not point-sampled into noise.
 *   4. MOVES each point: along its normal by (height - mid level) × scale. Where one point has
 *      several normals — a box's edge, between two faces that each displace along their own —
 *      it goes where the displaced faces MEET (the least offset that moves it by each face's
 *      amount along that face's normal), so a displaced box keeps its edges closed and crisp.
 *   5. PINS THE BORDERS IT SHARES: an open edge whose two ends are also vertices of another
 *      mesh of the object (the next material zone) does not move, and neither do the vertices
 *      subdivision puts on it — the neighbour is not subdivided the same way, and a moved
 *      border would tear. A free border (a plane's rim) moves.
 *   6. TURNS THE NORMALS AND TANGENTS by what the displacement did to the surface around each
 *      vertex: the rotation from the subdivided surface's own normal before to after, applied
 *      to the vertex's shading normal — so an undisplaced mesh comes out shaded exactly as it
 *      went in (a coarse smooth sphere stays smooth), and relief is lit as relief.
 */

#ifndef MESHDISPLACE_H
#define MESHDISPLACE_H

#include "posedmesh.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace pose {

/// The most subdivision levels a displacement applies (each multiplies the triangles by four).
constexpr int kDisplaceMaxLevels = 5;
/// The most triangles a displaced mesh may have: the levels asked for are reduced until the
/// mesh fits (a figure's torso at five levels would be twenty million).
constexpr std::size_t kDisplaceMaxTriangles = 5000000;

/// A height map: one channel of a tightly packed RGBA8 image (row 0 the top), wrapping.
struct HeightMap {
    const uint8_t* pixels = nullptr;
    int            width = 0;
    int            height = 0;
    int            channel = 0; ///< 0..3: the channel the height is read from
};

struct DisplaceSettings {
    float       scale = 0.0f;     ///< World units from the map's black to its white; negative inverts.
    float       midLevel = 0.5f;  ///< The map value (0..1) that moves nothing.
    int         levels = 0;       ///< Subdivision levels asked for (0..kDisplaceMaxLevels).
    glm::vec2   uvTiling{1.0f};   ///< The material's UV transform, as mesh.vert applies it:
    glm::vec2   uvOffset{0.0f};   ///<   uv' = R(rotation) · (uv × tiling) + offset
    float       uvRotation = 0.0f; ///<  (radians; 0 = none)
    std::size_t maxTriangles = kDisplaceMaxTriangles;
};

struct DisplaceReport {
    bool        displaced = false; ///< false: nothing to do (no map, no strength, no triangles) — the mesh is untouched
    int         levels = 0;        ///< the subdivision levels applied (fewer than asked where the budget said so)
    std::size_t triangles = 0;
    std::size_t vertices = 0;
    std::size_t points = 0;        ///< the vertices welded by position
    std::size_t pinned = 0;        ///< points held on a border shared with another mesh
    int         mipLevel = 0;      ///< how many times the map was halved before it was sampled
    float       maxOffset = 0.0f;  ///< the farthest a point moved, world units
};

/// The places the meshes of ONE object have vertices at (to a micrometre) — which is how a
/// displaced mesh knows a border it shares with another (step 5 above). Fill it with every mesh
/// of the object BEFORE any of them is displaced.
class SharedPositions {
public:
    SharedPositions();
    ~SharedPositions();
    SharedPositions(const SharedPositions&) = delete;
    SharedPositions& operator=(const SharedPositions&) = delete;

    /// One mesh's vertex positions (call once per mesh).
    void addMesh(const std::vector<glm::vec3>& positions);
    /// True where at least two of the meshes added have a vertex at @p position.
    bool shared(const glm::vec3& position) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Subdivides and displaces @p mesh in place (the file comment's six steps). @p neighbours: the
/// object's shared positions, or null for a mesh that stands alone. Returns what was done; a
/// report with `displaced` false means the mesh was left exactly as it came.
DisplaceReport displaceMesh(PosedMesh& mesh, const HeightMap& map, const DisplaceSettings& settings,
                            const SharedPositions* neighbours = nullptr);

} // namespace pose

#endif // MESHDISPLACE_H
