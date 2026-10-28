/**
 * @file subdivision.h
 * @brief Catmull-Clark subdivision of a figure cage into the assembled per-zone render meshes.
 *
 * The base figure is authored as a low-resolution quad *cage* meant to be subdivided at render time;
 * drawn directly it looks faceted on curved areas (limbs, face). This subdivides the cage @p levels
 * times: positions follow the smooth Catmull-Clark rules, while UVs (per polygon corner, so seams are
 * preserved) and skin weights are refined linearly. It then assembles the result into per-zone render
 * meshes — triangulated, de-indexed at UV seams, smooth-normalled over the whole surface so zone
 * boundaries stay seamless. Level 0 runs the same assembly on the raw cage, so this is THE assembler
 * for every level and the engine, skinning, and pose correctives see one vertex layout.
 *
 * The key property this leans on: Catmull-Clark is a *linear* operator on per-vertex values, so the
 * same stencils that smooth the rest positions also carry any per-base-vertex displacement field onto
 * the subdivided cage. carryCorrectivesToSubdivided() uses the retained per-level topology to rewrite
 * each pose corrective's sparse cage deltas onto the subdivided cage once at import, so the GPU's
 * live corrective blend (weight · delta summed before skinning, see mesh.vert) drives the dense mesh
 * directly.
 *
 * Pure std + GLM (+ the figure IR) — no Qt, no Vulkan.
 */

#ifndef SUBDIVISION_H
#define SUBDIVISION_H

#include "figuredata.h" // FigureMesh
#include "modeldata.h"  // PoseCorrective
#include "uvparser.h"   // UvSet

#include <cstddef>
#include <memory>
#include <vector>

namespace pose {

struct GeometryData;

/// The retained per-level connectivity of a subdivision (opaque; defined in subdivision.cpp).
struct SubdivisionTopology;

/// The result of subdividing a figure cage.
struct SubdivisionResult {
    std::vector<FigureMesh> meshes;         ///< Subdivided, assembled per-zone render meshes.
    std::size_t             vertexCount = 0; ///< Subdivided-cage vertex count (the baseVertex space).
    /// The per-level stencils, kept so carryCorrectivesToSubdivided can replay them on delta fields.
    std::shared_ptr<const SubdivisionTopology> topology;
};

/// Subdivides the cage (@p geo positions + faces) and per-base-vertex skin weights @p skins
/// @p levels times with Catmull-Clark, then assembles per-zone render meshes. @p zoneUvs is the UV
/// set each material zone's corners are read through, indexed like GeometryData::materialZones
/// (a zone past its end, or a null or empty entry, gets (0,0) UVs, as an unmapped cage does); the
/// sets may be shared between zones and must outlive the call. @p levels == 0 assembles the cage
/// as-is (no smoothing). @p skins may be empty (an unrigged mesh).
SubdivisionResult subdivideFigure(const GeometryData& geo, const std::vector<const UvSet*>& zoneUvs,
                                  const std::vector<VertexSkin>& skins, int levels);

/// Rewrites every corrective's sparse BASE-cage deltas (indices < @p cageVertexCount; out-of-range
/// entries are ignored, duplicates accumulate) onto the SUBDIVIDED cage of @p sub, in place: the
/// same stencils that smoothed the positions, applied to each delta field, then re-sparsified
/// (entries whose squared length exceeds 1e-12, ascending by vertex index). Correctives are
/// independent, so they run across cores.
void carryCorrectivesToSubdivided(const SubdivisionResult& sub, std::size_t cageVertexCount,
                                  std::vector<PoseCorrective>& correctives);

} // namespace pose

#endif // SUBDIVISION_H
