/**
 * @file materialoverlay.h
 * @brief A zone's OVERLAYS (2026-11-11): images layered over a surface's own maps — a tattoo, a
 *        bruise, freckles, veins, stretch marks, a scar, makeup — each painted on the surface's UV
 *        layout, with the controls that fit it to the skin under it.
 *
 * One overlay is a small GROUP of elements that belong together:
 *
 *   Base Color    a color and, optionally, an image with alpha (the picture: ink, blood under
 *                 the skin, lipstick) that the color multiplies — blended over the base color
 *                 map by a BLEND MODE at an OPACITY, after its own bitmap adjustments: gamma,
 *                 brightness, contrast, the color, hue and saturation. Multiply is the default
 *                 blend: ink and bruising darken the skin that is there, so the skin's own tone and
 *                 grain show through and the same overlay sits right on any skin. WITHOUT an image
 *                 the layer is the flat color — over the opacity map's area, or the whole map.
 *   Opacity       how much of the color shows, and a MAP that scales it per texel — the overlay's
 *                 MASK (white, or opaque: it shows; black, or transparent: it does not), as the
 *                 material's own Opacity carries its cutout. A Base Color and an opacity map
 *                 alone are a lipstick, an eye shadow: one mask, any color, in any blend.
 *   Gloss         how much glossier (or more matte) the surface is where the color is — lip
 *                 gloss, a wet wound, powder — through the overlay's shape: the picture's alpha
 *                 and the opacity map.
 *   Metallic      how much more METALLIC (or less) the surface is where the overlay is — metallic
 *                 ink, gold leaf, glitter; paint over a metal — through the same shape, and
 *                 through an image of its own that limits it (the metallic map).
 *   Bump          a grayscale height image: relief (mid grey = no change, lighter rises). The
 *                 viewport draws it through the normal map, as the slope of the height; an
 *                 export adds it to the bump map.
 *   Normal        a tangent-space normal image blended over the normal map.
 *   Displacement  a grayscale height image for a renderer's displacement (stored; the viewport
 *                 does not displace).
 *
 * The Opacity VALUE is the color's alone: the gloss, the metallic and the relief answer to their
 * own settings (an overlay at opacity 0 is its gloss, metal and relief and no color). Its MAP
 * is the overlay's shape for the color, the gloss and the metal; the RELIEF answers to its own
 * images and strengths alone.
 *
 * An overlay is painted in place on the UV layout, and can be MOVED, TURNED and SCALED from
 * there: its scale and its rotation are about the group's own centre, its offset shifts it across
 * the map — the whole group, color, gloss and relief together.
 *
 * A zone carries any number of overlays, composited in order (the first is the lowest). They are
 * part of the zone's material (MaterialParams::overlays): undoable, saved with the project. The
 * compositing itself is overlaycompositor.h's. Qt-free plain data, like materialparams.h (whose
 * materialparams.cpp defines this header's name tables and field setters too).
 */

#ifndef MATERIALOVERLAY_H
#define MATERIALOVERLAY_H

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace pose {

/// How an overlay's color meets the color under it (per channel, in the maps' own display
/// space — what an image editor's layer modes do).
enum class OverlayBlend {
    Multiply,  ///< darkens: ink in skin, a bruise, freckles, veins (white changes nothing)
    Normal,    ///< covers: opaque makeup, a sticker, body paint
    SoftLight, ///< a gentle tint that keeps the skin's light and dark: blush, a faint mark
    Overlay,   ///< a stronger tint with more contrast
    Screen,    ///< lightens: scars, stretch marks, highlighter (black changes nothing)
    Darken,    ///< the darker of the two
    Lighten,   ///< the lighter of the two
    Count
};

/// The blend's name in a script and a project file.
const char* overlayBlendName(OverlayBlend blend);
bool overlayBlendByName(const std::string& name, OverlayBlend& out);

/// The images an overlay can carry, one per element.
enum class OverlayElement { Color, Opacity, Metallic, Bump, Normal, Displacement, Count };

/// The element's name in a script (`overlay <n> map <element> <image>`), and its inverse.
const char* overlayElementName(OverlayElement element);
bool overlayElementByName(const std::string& name, OverlayElement& out);

/// The range of an overlay's scale: a tenth of its painted size to five times it.
inline constexpr float kOverlayScaleMin = 0.1f;
inline constexpr float kOverlayScaleMax = 5.0f;

/// One overlay: its images (file paths; "" = the element is absent) and their controls.
struct MaterialOverlay {
    std::string  name;             ///< What the tab calls it: "Overlay 1" as added, or what the user renamed it. Paints nothing.
    bool         enabled = true;   ///< Off: the overlay contributes nothing, every value kept.
    /// The overlay's PLACE: how far it is moved across the map from where it was painted, as a
    /// share of the map's width (x, + to the right) and of its height (y, + UP the map) — the
    /// whole overlay, its gloss and relief with its color. What runs off the map's edge is cut
    /// (it does not wrap round to the other side).
    glm::vec2    offset{0.0f};
    /// The overlay TURNED about its own centre, in degrees: positive is clockwise as the map is
    /// seen (image rows running down). The centre is that of the overlay's ANCHOR image — its
    /// first image, see overlayAnchorSource — where that image has content, so every element of
    /// the group turns about one point and stays registered.
    float        rotation = 0.0f;
    /// The overlay's SIZE about the same centre: 1 is as painted, 2 twice as wide and as tall.
    /// (Turned or scaled the images are resampled; at 0 degrees and 1 they are painted texel for
    /// texel.)
    float        scale = 1.0f;
    // --- Base Color (the tab's row: the swatch is `tint`, the picture beside it `color`) ---
    std::string  color;            ///< The color image (RGBA; its alpha is its shape). "" = a flat layer of the color below.
    OverlayBlend blend = OverlayBlend::Multiply;
    float        opacity = 1.0f;   ///< 0 … 1 over the overlay's shape — the COLOR's alone.
    /// The opacity's MAP — the overlay's MASK: where it applies (read as OverlayImageKind::Mask:
    /// the image's alpha where it has any transparency, else its brightness). It scales the
    /// color's opacity per texel and is the SHAPE of the gloss and the metal with the picture's
    /// alpha; the relief does not follow it. "" = wherever the picture is (or the whole map).
    std::string  opacityMap;
    glm::vec3    tint{1.0f};       ///< The Base Color: multiplies the image's color, or IS the color without one. DISPLAY (sRGB) values — the space the layers meet in. White = the image as painted.
    float        brightness = 0.0f; ///< -1 … 1, added.
    float        contrast = 0.0f;  ///< -1 (flat grey) … 0 (as painted) … 1 (hard).
    float        gamma = 1.0f;     ///< Midtones: above 1 lightens them, below darkens (0.2 … 5).
    float        hue = 0.0f;       ///< Degrees around the color wheel (-180 … 180).
    float        saturation = 0.0f; ///< -1 (grey) … 0 … 1 (doubled).
    // --- Gloss (through the color image's alpha) ---
    float        gloss = 0.0f;     ///< -1 as matte as the material allows … 0 unchanged … 1 mirror-smooth.
    // --- Metallic (through the same shape, and its own map) ---
    float        metallic = 0.0f;  ///< -1 not metallic at all … 0 unchanged … 1 fully metallic.
    std::string  metallicMap;      ///< Where the metallic applies within the overlay (read as a mask: OverlayImageKind::Mask). "" = all of it.
    // --- Relief ---
    std::string  bump;             ///< A grayscale height image (mid grey = no change; alpha = where).
    float        bumpStrength = 1.0f; ///< Negative inverts it (a groove instead of a ridge).
    std::string  normal;           ///< A tangent-space normal image (alpha = where).
    float        normalStrength = 1.0f;
    std::string  displacement;     ///< A grayscale height image for a renderer's displacement (stored).
    float        displacementStrength = 1.0f;

    bool operator==(const MaterialOverlay& o) const {
        return name == o.name && enabled == o.enabled && offset == o.offset && rotation == o.rotation && scale == o.scale && color == o.color && opacityMap == o.opacityMap &&
               blend == o.blend && opacity == o.opacity && tint == o.tint &&
               brightness == o.brightness && contrast == o.contrast && gamma == o.gamma && hue == o.hue &&
               saturation == o.saturation && gloss == o.gloss && metallic == o.metallic && metallicMap == o.metallicMap && bump == o.bump &&
               bumpStrength == o.bumpStrength &&
               normal == o.normal && normalStrength == o.normalStrength && displacement == o.displacement &&
               displacementStrength == o.displacementStrength;
    }
    bool operator!=(const MaterialOverlay& o) const { return !(*this == o); }
};

/// The overlay's ANCHOR image: the first it has, in the elements' order — the picture, else the
/// opacity map, the metallic map, the bump, the normal, the displacement. Its content's centre is
/// what the overlay turns and scales about.
inline const std::string& overlayAnchorSource(const MaterialOverlay& o) {
    return !o.color.empty()         ? o.color
           : !o.opacityMap.empty()  ? o.opacityMap
           : !o.metallicMap.empty() ? o.metallicMap
           : !o.bump.empty()        ? o.bump
           : !o.normal.empty()      ? o.normal
                                    : o.displacement;
}
/// Whether the overlay has a COLOR layer at all: a picture, an opacity map to lay its flat
/// color through, or a Base Color other than white (a flat tint over the whole map). With none
/// of them it is relief alone — or empty, as it is when it has just been added.
inline bool overlayHasColor(const MaterialOverlay& o) {
    return !o.color.empty() || !o.opacityMap.empty() || o.tint != glm::vec3(1.0f);
}
/// Whether the overlay is turned or scaled — placed by more than its offset, so resampled about
/// its anchor's centre.
inline bool overlayTransformed(const MaterialOverlay& o) {
    return o.rotation != 0.0f || o.scale != 1.0f;
}

inline std::string& overlayElementSource(MaterialOverlay& o, OverlayElement element) {
    switch (element) {
    case OverlayElement::Opacity: return o.opacityMap;
    case OverlayElement::Metallic: return o.metallicMap;
    case OverlayElement::Bump: return o.bump;
    case OverlayElement::Normal: return o.normal;
    case OverlayElement::Displacement: return o.displacement;
    default: return o.color;
    }
}
inline const std::string& overlayElementSource(const MaterialOverlay& o, OverlayElement element) {
    return overlayElementSource(const_cast<MaterialOverlay&>(o), element);
}

/// What an overlay is CALLED in the tab: its name ("" = none given — the tab then shows its place
/// in the stack).
inline const std::string& overlayTitle(const MaterialOverlay& o) {
    return o.name;
}

/// The number a NEW overlay's default name takes: its PLACE in the stack — the third overlay is
/// "Overlay 3" whatever the two before it have been renamed to — or the next number after that
/// which no overlay of the stack already carries (one was removed from the middle).
int nextOverlayNumber(const std::vector<MaterialOverlay>& overlays, const std::string& prefix);

/// The overlay's SCALAR controls as fields with their DISPLAY units (percent, degrees, a plain
/// gamma), so the tab and the scripted test read and set the same numbers through one definition
/// — MaterialField's treatment. Enabled is a 0 / 1 field; Blend the mode's index.
enum class OverlayField {
    Enabled,
    Blend,
    Opacity,
    Brightness,
    Contrast,
    Gamma,
    Hue,
    Saturation,
    Gloss,
    BumpStrength,
    NormalStrength,
    DisplacementStrength,
    OffsetX, ///< percent of the map's width, + to the right
    OffsetY, ///< percent of the map's height, + up
    Rotation, ///< degrees about the overlay's own centre, + clockwise on the map
    Scale,    ///< percent of the painted size, about the same centre
    Metallic, ///< percent: above 0 more metallic, below 0 less
    Count
};

/// A field's display value off @p o, and the change a display value makes to it (clamped to
/// the field's range).
float overlayFieldValue(const MaterialOverlay& o, OverlayField field);

void setOverlayField(MaterialOverlay& o, OverlayField field, float v);

/// Holds every control of @p o to its field's range (setOverlayField's; the Base Color to 0..1).
/// An overlay is made on the tab alone, so those ranges are everything one can be: a FILE's
/// value outside them — an offset of 1e30, which the compositor turns into a count of texels —
/// is held to them. A value inside its range is left bit for bit.
void clampMaterialOverlay(MaterialOverlay& o);

/// The field's name in the scripted test (`overlay <n> <name> <value>`, `overlay.<n>.<name>`).
const char* overlayFieldName(OverlayField field);
bool overlayFieldByName(const std::string& name, OverlayField& out);

} // namespace pose

#endif // MATERIALOVERLAY_H
