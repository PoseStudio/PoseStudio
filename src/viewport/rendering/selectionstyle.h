/**
 * @file selectionstyle.h
 * @brief How the viewport marks what is selected: the color and the opacity of the outline
 *        around the selected object, and of the highlight on the flesh a selected joint drives.
 *
 * Two marks, set apart (Preferences → Selection):
 *
 *   THE OUTLINE  the silhouette line the composite paints around the selected model after
 *                tonemapping (composite.frag, pushed by PostProcess): the color itself, mixed
 *                over the frame by the opacity.
 *   THE JOINT    the tint mesh.frag lays over the skin a selected joint (and what lights with
 *   HIGHLIGHT    it) weighs on. It is LUMINANCE-MATCHED: the surface keeps its own light and
 *                shadow and takes the color's hue and saturation, so the color's own luminance
 *                divides out (jointTintLuminance) — a dark blue and a bright blue tint alike,
 *                and only a color near black darkens the part instead (the floor).
 *
 * The defaults are the constants the shaders were written with — the app's accent blue #5b87cc
 * decoded to linear and rounded, its luminance rounded to 0.239, the highlight at 0.75 — and are
 * handed over AS those constants, never re-derived from the hex color: an installation that
 * changes nothing renders bit for bit as it did before the colors could be set.
 *
 * Qt-free and Vulkan-free (std + GLM): PostProcess, the Scene's camera block and the Qt layer
 * that reads the preference all include it.
 */

#ifndef SELECTIONSTYLE_H
#define SELECTIONSTYLE_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace pose {

/// The viewport's selection accent: the app's QSS accent blue #5b87cc (hover borders, the
/// DragNumberBox glyphs), sRGB-decoded to LINEAR — the swapchain's sRGB store re-encodes it
/// exactly. The default for both marks. The darker selection FILL blue (#314D7A) is deliberately
/// not used: against the viewport grey it doesn't read as a line.
inline constexpr float kSelectionAccentLinear[3] = {0.1046f, 0.2423f, 0.6038f};
/// The same color as the display bytes it is chosen by.
inline constexpr int kSelectionAccentSrgb8[3] = {0x5b, 0x87, 0xcc};
/// Its Rec.709 luminance, as mesh.frag's highlight was written with it.
inline constexpr float kSelectionAccentLuminance = 0.239f;
/// The joint highlight's default opacity: the tint at a full skin weight.
inline constexpr float kJointHighlightDefaultOpacity = 0.75f;
/// Below this luminance a highlight color is no longer normalized up to the surface's: it
/// darkens the part instead, down to black for black (a continuous rule — without the floor a
/// color a hair from black would tint as brightly as a grey).
inline constexpr float kJointTintLuminanceFloor = 0.05f;

/// The two marks' colors (LINEAR rgb) and opacities (0 = not drawn, 1 = the color in full).
struct SelectionStyle {
    glm::vec3 outlineColor{kSelectionAccentLinear[0], kSelectionAccentLinear[1], kSelectionAccentLinear[2]};
    float     outlineOpacity = 1.0f;
    glm::vec3 jointColor{kSelectionAccentLinear[0], kSelectionAccentLinear[1], kSelectionAccentLinear[2]};
    float     jointOpacity = kJointHighlightDefaultOpacity;
};

/// A display color (sRGB bytes) as the linear color the shaders take. The default accent is
/// returned as the constant itself (see the file comment).
inline glm::vec3 selectionColorFromSrgb8(int r, int g, int b) {
    if (r == kSelectionAccentSrgb8[0] && g == kSelectionAccentSrgb8[1] && b == kSelectionAccentSrgb8[2]) {
        return {kSelectionAccentLinear[0], kSelectionAccentLinear[1], kSelectionAccentLinear[2]};
    }
    const auto decode = [](int byte) {
        const float c = static_cast<float>(std::clamp(byte, 0, 255)) / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return {decode(r), decode(g), decode(b)};
}

/// What the joint highlight divides its color by, so that the tint carries the SURFACE's
/// luminance: the color's own Rec.709 luminance, never under the floor (and the rounded
/// constant for the default accent).
inline float jointTintLuminance(const glm::vec3& colorLinear) {
    if (colorLinear.x == kSelectionAccentLinear[0] && colorLinear.y == kSelectionAccentLinear[1] &&
        colorLinear.z == kSelectionAccentLinear[2]) {
        return kSelectionAccentLuminance;
    }
    return std::max(glm::dot(colorLinear, glm::vec3(0.2126f, 0.7152f, 0.0722f)), kJointTintLuminanceFloor);
}

} // namespace pose

#endif // SELECTIONSTYLE_H
