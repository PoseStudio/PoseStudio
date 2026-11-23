// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file smoothnormals.h
 * @brief SMOOTHING BY ANGLE: a mesh's shading normals computed here, from its faces, instead of
 *        taken from its file.
 *
 * PoseStudio shades a surface with the normals its file gives it. A file may give none that say
 * anything: a modelling package's default export writes ONE NORMAL PER FACE, and the surface then
 * renders faceted — in the viewport and in a render alike, since both read the same normals. This
 * computes them instead, the way every modelling package's "smooth by angle" does:
 *
 *   - the faces round a point are gathered across the whole MODEL (a surface does not stop at a
 *     material zone's border; points are the same point when their positions are bit-equal, which
 *     is what a de-indexed file gives);
 *   - two faces that share an EDGE belong to one smooth group when they meet at LESS than the
 *     angle (the angle between their normals; the lesser of the two zones' angles). An edge AT
 *     the angle or sharper stays crisp — so with 60 degrees a sphere is round, a cube keeps its
 *     edges and a hexagonal bolt its six sides;
 *   - a corner's normal is the mean of its group's face normals, each weighted by the angle the
 *     face has at that point (so the result does not depend on how a quad was triangulated).
 *
 * 0 degrees is faceted and 180 smooth everywhere. A hard edge needs a point's vertices apart and
 * a smooth one may bring a file's per-face vertices together, so the result is NEW vertices and
 * indices (the triangles in the file's order): a vertex per (point's attributes, smooth group).
 *
 * Positions, UVs, skin weights and corrective ranges are carried over; the baked occlusion is
 * averaged over each group (occlusion baked per face would band a smooth surface); tangents are
 * left zero — the caller computes them for the new normals (tangentgen.h).
 *
 * Pure std + GLM: no Qt, no device.
 */

#ifndef SMOOTHNORMALS_H
#define SMOOTHNORMALS_H

#include "vertex.h"

#include <cstdint>
#include <vector>

namespace pose {

struct ModelData;

/// The angle a file without smoothing of its own is smoothed by at import (and the Materials
/// tab's default). 60 is what importers settle on: round things come out round, right angles and
/// a hexagon's sides stay crisp, and a 45-degree chamfer blends — which a faceted file's author
/// could not have asked for either way.
constexpr float kDefaultSmoothAngle = 60.0f;

/// One mesh (material zone) of a model, as the smoothing reads it.
struct SmoothMesh {
    const std::vector<Vertex>*   vertices = nullptr; ///< its geometry AS THE FILE HAS IT
    const std::vector<uint32_t>* indices = nullptr;
    bool  smooth = false;                             ///< smooth it by angle (false: untouched, and its faces join no group)
    float angleDeg = kDefaultSmoothAngle;
};

/// A smoothed mesh.
struct SmoothedMesh {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;  ///< as many as the input's, the triangles in the input's order
    std::vector<uint32_t> origin;   ///< per vertex: the input vertex its attributes were taken from
};

/// Smooths every mesh of @p meshes that asks for it; the result is parallel to @p meshes (empty
/// for a mesh that did not ask).
std::vector<SmoothedMesh> smoothByAngle(const std::vector<SmoothMesh>& meshes);

/// Whether the mesh's normals say nothing about smoothing — a faceted export, ONE NORMAL PER
/// FACE: every triangle (with an area) carries one and the same normal at its three corners, and
/// that normal is its face's (to 30 degrees, either side, for 95 % of them: a polygon that is not
/// quite flat is cut into triangles that each stand a little off the polygon's normal, and a
/// sliver's own normal is noise). One triangle whose corners differ and the file has smoothed
/// something: its normals are its author's. A mesh with no real triangle is not faceted.
bool hasOnlyFaceNormals(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

/// The share of the mesh's triangles (those with an area) that are faceted in that sense — the
/// three corners one normal, the face's: 1 = faceted throughout, 0 = no faceted triangle (0 for
/// an empty mesh).
float facetedShare(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

/// THE IMPORT'S DEFAULT: each mesh whose file brings no smoothing of its own — only face
/// normals, or none at all (MeshData::normalsGenerated) — is smoothed by kDefaultSmoothAngle:
/// its `vertices` / `indices` become the smoothed geometry, the file's are kept beside them
/// (`fileVertices` / `fileIndices`: what "as imported" puts back) and `smoothByAngle` is set. A
/// mesh with authored normals is left exactly as it is. Call BEFORE the occlusion bake and the
/// tangents. Returns how many meshes were smoothed.
int applyImportSmoothing(ModelData& data);

} // namespace pose

#endif // SMOOTHNORMALS_H
