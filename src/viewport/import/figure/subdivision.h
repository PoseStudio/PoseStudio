/**
 * @file subdivision.h
 * @brief Catmull-Clark subdivision of a figure cage into a smoother render mesh.
 *
 * The base figure is authored as a low-resolution quad *cage* meant to be subdivided at render time;
 * drawn directly it looks faceted on curved areas (limbs, face). This subdivides the cage @p levels
 * times: positions follow the smooth Catmull-Clark rules, while UVs (per polygon corner, so seams are
 * preserved) and skin weights are refined linearly. It then assembles the result into per-zone render
 * meshes (triangulated, de-indexed, smooth-normalled) exactly like the non-subdivided path, so the
 * engine, skinning, and pose correctives are unchanged — they just run on more vertices.
 *
 * The key property this leans on: Catmull-Clark is a *linear* operator on per-vertex values, so the
 * same stencils that smooth the rest positions also carry any per-base-vertex displacement field onto
 * the subdivided cage. `subdivideDeltaField` exposes that so a pose corrective's sparse deltas can be
 * subdivided once at import and then drive the existing runtime re-morph on the dense mesh.
 *
 * Pure std + GLM (+ the figure IR) — no Qt, no Vulkan.
 */

#ifndef SUBDIVISION_H
#define SUBDIVISION_H

#include "figuredata.h" // FigureMesh
#include "uvparser.h"   // UvSet

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace pose {

struct GeometryData;

/// The result of subdividing a figure cage.
struct SubdivisionResult {
    std::vector<FigureMesh> meshes;         ///< Subdivided, assembled per-zone render meshes.
    std::size_t             vertexCount = 0; ///< Subdivided-cage vertex count (the baseVertex range).
    /// Carries a per-*base*-vertex displacement field (a corrective's deltas, densified to one vec3
    /// per base cage vertex) onto the subdivided cage — returns one vec3 per subdivided-cage vertex,
    /// indexed to match the assembled meshes' `baseVertex`.
    std::function<std::vector<glm::vec3>(const std::vector<glm::vec3>&)> subdivideDeltaField;
};

/// Subdivides the cage (@p geo positions + faces), UV set @p uv, and per-base-vertex skin weights
/// @p skins @p levels times with Catmull-Clark, then assembles per-zone render meshes. @p levels == 0
/// assembles the cage as-is (no smoothing). @p skins may be empty (an unrigged mesh).
SubdivisionResult subdivideFigure(const GeometryData& geo, const UvSet& uv,
                                  const std::vector<VertexSkin>& skins, int levels);

} // namespace pose

#endif // SUBDIVISION_H
