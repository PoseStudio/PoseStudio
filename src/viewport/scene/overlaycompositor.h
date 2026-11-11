/**
 * @file overlaycompositor.h
 * @brief Composites a zone's OVERLAYS (materialoverlay.h) into the images its texture slots bind:
 *        the color images over the base-color map, their gloss into the roughness map, their
 *        metal into the metalness map, the bump and normal images into theirs.
 *
 * The overlays are a layer stack painted on the CPU, not a shader feature: the zone's map with
 * its overlays IS a map — any number of layers costs the renderer nothing, the fixed sampler
 * budget of the mesh shader is untouched, and a rendering engine is handed the finished image.
 * A composite is a copy of the slot's own image (or a neutral canvas where the zone has none)
 * with every enabled overlay applied in order — its color (a picture, or a flat Base Color) by
 * its opacity and the opacity's MAP (the overlay's mask), its gloss and its metal in that shape,
 * its relief by its own images — each at its PLACE: where it was painted, scaled
 * and turned about its own centre (MaterialOverlay::scale / rotation — resampled, filtered where
 * it is scaled down, a normal image's slopes turned with it; at 1 and 0 degrees it is painted
 * texel for texel) and moved by its offset (MaterialOverlay::offset — whole texels of the map;
 * what leaves the map is cut).
 *
 * An overlay image is painted on the surface's whole UV square but covers a small part of it (a
 * tattoo is a few percent of a 4096-square map), so the decoded image is kept as an OverlayImage:
 * the rectangle where it does anything, cropped out. Sixteen megapixels become a few, in memory
 * and per composite — which is what lets a slider scrub re-composite live.
 *
 * The layers meet in the maps' own display space (the 8-bit sRGB values of the color maps), the
 * space image editors blend in and overlay images are painted for. Pure std + GLM: no Qt (the
 * decode is the caller's, handed in through OverlayImageSource), no Vulkan.
 */

#ifndef OVERLAYCOMPOSITOR_H
#define OVERLAYCOMPOSITOR_H

#include "materialparams.h" // MaterialMap, MaterialOverlay, overlayAffects
#include "modeldata.h"      // DecodedImage

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pose {

/// What an overlay image is read as — which decides where it "does nothing" and can be cropped.
enum class OverlayImageKind {
    Color,  ///< RGBA color: nothing where its alpha is 0.
    Height, ///< A grayscale height (bump, displacement): nothing where alpha is 0 — or, opaque, where it is mid grey.
    Normal, ///< A tangent-space normal: nothing where alpha is 0 — or, opaque, where it is flat.
    /// A MASK, one value a texel — an overlay's opacity map, its metallic map: the image's ALPHA
    /// where it has transparency (its opaque part is the mask, whatever its color — a white form
    /// on nothing, a tattoo's own silhouette), else its BRIGHTNESS (a grayscale mask: white
    /// applies, black does not). Kept as a white image with that value in its alpha; nothing
    /// where the value is 0.
    Mask
};

/// A decoded overlay image cropped to the rectangle where it does something.
struct OverlayImage {
    uint32_t             fullWidth = 0;  ///< The image's own size …
    uint32_t             fullHeight = 0;
    uint32_t             x = 0;          ///< … and the kept rectangle in it.
    uint32_t             y = 0;
    uint32_t             width = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> rgba;           ///< width × height × 4, tightly packed.
    bool                 hasAlpha = false; ///< The source had transparent texels (its alpha is the shape).

    bool empty() const { return width == 0 || height == 0 || rgba.empty(); }
};

/// Crops @p image to where it does something as a @p kind (see OverlayImageKind). Never null; an
/// image that does nothing anywhere comes back empty().
std::shared_ptr<OverlayImage> makeOverlayImage(const DecodedImage& image, OverlayImageKind kind);

/// Supplies an overlay's decoded image by file path (the Qt layer's decode + cache); null when
/// the file cannot be read — that layer is then left out.
using OverlayImageSource = std::function<std::shared_ptr<const OverlayImage>(const std::string& path, OverlayImageKind kind)>;

/// The image slot @p map binds once @p overlays are painted over @p base (the slot's own plain
/// image; null = the zone has none, and a neutral canvas the size of the largest overlay stands
/// in: white for the color and roughness maps, mid grey for a height, flat for a normal).
/// @p channel is the channel a packed map's value rides in (the roughness map's; 0 otherwise).
/// @p map is BaseColor, Roughness, Metalness, Bump, Normal — or Displacement, for an export.
/// For METALNESS the composite is the zone's ABSOLUTE metalness: @p baseScale is the zone's
/// Metallic value, multiplied into @p base's channel (or the canvas) before the overlays' metal
/// is painted — the map alone is only a share of that value, and no share of 0 is metal — so the
/// caller binds the result with the value taken as 1 (Mesh::setMetalBaked). Null when no
/// enabled overlay touches the slot or none of their images could be supplied: the caller keeps
/// @p base. @p base is never written. An overlay whose picture or opacity map cannot be supplied
/// has no color, gloss or metal (unmasked, a flat color would cover the map).
///
/// @p bumpAsNormal (with @p map Normal) is THE VIEWPORT'S normal map: each overlay's BUMP image is
/// painted into it too, as the slope of its height (kOverlayBumpSlope) — the viewport draws an
/// overlay's relief through the normal map, because its bump path keeps only pore-scale grain (a
/// high pass that removes anything wider than four texels: a scar's ridge would cancel). An
/// export leaves it false and composites the Bump map instead.
std::shared_ptr<DecodedImage> compositeOverlays(MaterialMap map, const std::shared_ptr<const DecodedImage>& base, int channel,
                                                const std::vector<MaterialOverlay>& overlays, const OverlayImageSource& images,
                                                bool bumpAsNormal = false, float baseScale = 1.0f);

/// The slope an overlay's bump image gives the viewport's normal at strength 1, per unit of height
/// change across one whole UV square: white-over-black is a relief of 1/160 of the map's width —
/// about half a centimetre on a body map, a scar's ridge reading clearly at 100.
inline constexpr float kOverlayBumpSlope = 1.0f / 160.0f;

/// One color through an overlay's bitmap adjustments, in their order — gamma, brightness,
/// contrast, tint, hue and saturation — on display (sRGB) values 0..1. What the compositor does
/// per texel; exposed for the tab's thumbnails and the tests.
glm::vec3 adjustOverlayColor(const MaterialOverlay& overlay, glm::vec3 rgb);

/// @p source blended over @p dest by @p blend, per channel, both 0..1 (before the alpha mix).
glm::vec3 blendOverlayColor(OverlayBlend blend, const glm::vec3& dest, const glm::vec3& source);

} // namespace pose

#endif // OVERLAYCOMPOSITOR_H
