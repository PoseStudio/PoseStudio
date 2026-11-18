/**
 * @file colorcurves.h
 * @brief The display curves, once: the sRGB transfer function both ways and the ACES tonemap
 *        fit the viewport's PBR mode ends with.
 *
 * Small pure functions that several places need and had each typed out — a color constant of a
 * MaterialX document made linear, a material ball's maps and its finished pixels, a render's
 * display picture. They are in a namespace of their own (pose::colorcurves) so that this
 * header can be included beside a file's older, local copy of one of them without a clash;
 * bring a name in with a using-declaration where it is used.
 *
 * std-only and Qt-free, like everything in util/.
 */

#ifndef COLORCURVES_H
#define COLORCURVES_H

#include <algorithm>
#include <cmath>

namespace pose {
namespace colorcurves {

/// A display (sRGB-encoded) value as linear light. Not clamped: a value outside 0..1 goes
/// through the curve as it is.
inline float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// Linear light as a display (sRGB-encoded) value, clamped to 0..1 first.
inline float linearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

/// The display transform the viewport's PBR mode ends with: the ACES fit of
/// shaders/include/color.glsl (tonemapACES), on one channel.
inline float tonemapAces(float x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    x = std::max(x, 0.0f);
    return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
}

} // namespace colorcurves
} // namespace pose

#endif // COLORCURVES_H
