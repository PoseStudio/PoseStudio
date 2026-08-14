/**
 * @file texturegutters.h
 * @brief Fills the unused background of a mesh's decoded textures with colours dilated out from
 *        its UV islands, so minified mips never bleed the atlas background through UV seams.
 *
 * Character/atlas textures leave the space between UV islands a flat colour (commonly white) with
 * no padding. The GPU mip chain is built by successive down-filtering, so every coarser mip
 * averages island-edge texels with that background — and at viewing distances where those mips are
 * sampled, the seams show up as bright lines that fade back out as you zoom in (mip 0 is clean).
 * The durable fix is at the source: after decoding, rasterize which texels the mesh's own UV
 * triangles actually cover, then flood every uncovered texel with colour pulled from the nearest
 * covered ones (a pull-push fill). Covered texels are untouched; mips then average island colour
 * into island colour, never background. Pure std + GLM — no Qt, no Vulkan.
 */

#ifndef TEXTUREGUTTERS_H
#define TEXTUREGUTTERS_H

namespace pose {

struct MeshData;

/// Gutter-fills the mesh's decoded diffuse and detail (normal/bump) images in place, using the
/// mesh's own UV triangles as the coverage source. No-op for untextured meshes, meshes without
/// geometry, or textures the UVs fully cover (e.g. tiled/wrapping materials).
void fillTextureGutters(MeshData& mesh);

} // namespace pose

#endif // TEXTUREGUTTERS_H
