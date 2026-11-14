/**
 * @file tangentgen.h
 * @brief Computes per-vertex UV-aligned tangents for an imported model at import time.
 *
 * The mesh shaders previously derived a tangent frame from screen-space derivatives (Schüler's
 * cotangent frame) — workable, but noisier than real geometry tangents and unstable across UDIM
 * seams. This fills Vertex::tangent (xyz + bitangent handedness in w) with the classic Lengyel
 * accumulation: per-triangle tangents from the UV parameterization, summed per vertex, then
 * Gram-Schmidt orthonormalized against the vertex normal. Runs on the FINAL render meshes
 * (post-morph, post-subdivision). Meshes without UVs keep w = 0 — the shader's marker to fall
 * back to the cotangent frame. Pure std + GLM — no Qt, no Vulkan.
 */

#ifndef TANGENTGEN_H
#define TANGENTGEN_H

#include <cstdint>
#include <vector>

namespace pose {

struct ModelData;
struct Vertex;

/// Fills every mesh vertex's `tangent` in place.
void computeTangents(ModelData& data);

/// One mesh's: the vertices' `tangent` from their positions, normals and UVs (a mesh whose
/// normals were recomputed after import — the smoothing by angle — gets its tangents again).
void computeTangents(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

} // namespace pose

#endif // TANGENTGEN_H
