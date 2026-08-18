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

#include <cstdint>
#include <vector>

namespace pose {

struct Vertex;

/// One mesh's UV geometry contributing coverage to a (possibly shared) texture. The pointed-at
/// containers must outlive the fill call.
struct UvMeshRef {
    const std::vector<Vertex>*   vertices = nullptr;
    const std::vector<uint32_t>* indices  = nullptr;
};

/// Gutter-fills one decoded RGBA8 image in place, using the UNION of every user's UV triangles as
/// the coverage source — an atlas shared by several meshes must keep all their islands intact, and
/// filling once per image (instead of once per mesh) is what lets shared decodes stay shared.
/// No-op when the coverage is empty (nothing to pull colour from) or complete (nothing to fill —
/// e.g. tiled/wrapping materials).
void fillImageGutters(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
                      const std::vector<UvMeshRef>& users);

} // namespace pose

#endif // TEXTUREGUTTERS_H
