/**
 * @file aobaker.h
 * @brief Bakes per-vertex ambient occlusion for an imported model at import time.
 *
 * For every render vertex, hemisphere rays (cosine-weighted, distance-attenuated) are cast against
 * a BVH of the model's own opaque triangles; the visible fraction lands in Vertex::ao (1 = fully
 * open, 0 = fully occluded). The PBR shader multiplies the ENVIRONMENT terms by it — cavities (eye
 * sockets, nostrils, under the chin, between fingers) stop receiving full sky light, which is what
 * separates "waxy" from grounded; the key light is left alone (it has real shadow mapping).
 *
 * Baked on the FINAL render mesh (post-morph, post-subdivision, all zones together — occlusion is
 * cross-zone: the eyelid occludes the eyeball). Transparent meshes (eye shells, lash/brow cutout
 * cards) receive AO but never occlude — a clear cornea must not darken the iris behind it. Bind
 * pose only: the bake is static, so a deep pose change won't update it (SSAO later supersedes
 * this dynamically). Pure std + GLM (parallelFor across vertices) — no Qt, no Vulkan.
 */

#ifndef AOBAKER_H
#define AOBAKER_H

namespace pose {

struct ModelData;

/// Fills every mesh vertex's `ao` in place. Skipped (all vertices stay 1.0) when the model has no
/// opaque geometry or exceeds the occluder-triangle cap (a huge OBJ shouldn't stall import).
void bakeVertexAO(ModelData& data);

} // namespace pose

#endif // AOBAKER_H
