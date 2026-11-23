// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file meshrepair.h
 * @brief What a reader does to a mesh its file left incomplete or damaged, whatever the format:
 *        normals for the vertices that came without one, and the vertices whose position is not
 *        a number taken out. Qt-free (std + GLM).
 *
 * These are not about any one format — a glTF primitive, a USD mesh, an FBX part and an OBJ
 * material group all end as a MeshData — so they live beside the IR (modeldata.h) and not in a
 * reader's folder.
 */

#ifndef MESHREPAIR_H
#define MESHREPAIR_H

#include <cstddef>

namespace pose {

struct MeshData;
struct ModelData;

/// Gives every vertex without a normal (a zero vector, or one that is not a number) the smooth
/// normal of the triangles around it; authored normals are left alone. Sets
/// MeshData::normalsGenerated when EVERY vertex lacked one — a file with no normals at all, which
/// the import then smooths by angle (smoothnormals.h).
void fillMissingNormals(MeshData& mesh);

/// Takes out every vertex whose position is not finite — a NaN or an infinity, which a binary
/// format can carry and a text one can spell — together with the triangles that use one, and
/// renumbers the rest. Returns how many vertices went (0: the mesh is untouched). One such vertex
/// would otherwise spoil everything measured over the model — the floor it is stood on (a vertex
/// at minus infinity lifts every other one to plus infinity), its centre, the occlusion bake's
/// tree — while its triangles could never be drawn anyway.
std::size_t removeNonFiniteVertices(MeshData& mesh);

/// The same over a whole model, with the meshes that are left without a triangle taken out too.
/// Returns how many vertices went.
std::size_t removeNonFiniteVertices(ModelData& model);

} // namespace pose

#endif // MESHREPAIR_H
