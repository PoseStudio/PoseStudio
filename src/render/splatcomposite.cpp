/**
 * @file splatcomposite.cpp
 * @brief See splatcomposite.h.
 */

#include "splatcomposite.h"

#include "parallelfor.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pose {

namespace {

// The ACES fit the viewport's composite and the render session share (color.glsl's tonemapACES).
constexpr float kA = 2.51f;
constexpr float kB = 0.03f;
constexpr float kC = 2.43f;
constexpr float kD = 0.59f;
constexpr float kE = 0.14f;

float srgbEncode(float x) {
    return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}
float srgbDecode(float x) {
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

unsigned char toByte(float x) {
    return static_cast<unsigned char>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f));
}

} // namespace

float displayEncode(float linear, bool tonemap) {
    float x = std::isfinite(linear) ? std::max(linear, 0.0f) : 0.0f;
    if (tonemap) {
        x = (x * (kA * x + kB)) / (x * (kC * x + kD) + kE);
    }
    return srgbEncode(std::clamp(x, 0.0f, 1.0f));
}

float displayDecode(float display, bool tonemap) {
    const float y = srgbDecode(std::clamp(std::isfinite(display) ? display : 0.0f, 0.0f, 1.0f));
    if (!tonemap) {
        return y;
    }
    // y = x (a x + b) / (x (c x + d) + e)  ->  (a - c y) x² + (b - d y) x - e y = 0, the root that
    // is not negative. (a - c y is at least 0.08 on 0..1: the fit tops out a little over one.)
    const float qa = kA - kC * y;
    const float qb = kB - kD * y;
    const float qc = -kE * y;
    return (-qb + std::sqrt(std::max(qb * qb - 4.0f * qa * qc, 0.0f))) / (2.0f * qa);
}

SplatComposite::SplatComposite(RenderSplatLayers&& layers, const View& view)
    : m_width(layers.width), m_height(layers.height), m_tonemap(view.tonemap), m_transparent(view.transparent),
      m_front(std::move(layers.front)) {
    const std::size_t pixels = static_cast<std::size_t>(std::max(m_width, 0)) * static_cast<std::size_t>(std::max(m_height, 0));
    m_front.resize(pixels * 4u, 0.0f);
    std::vector<float> all = std::move(layers.all);
    all.resize(pixels * 4u, 0.0f);
    const std::vector<float> backdrop = std::move(layers.backdrop); // RGB radiance a pixel, or empty: the flat color
    const bool               perPixel = backdrop.size() == pixels * 3u;
    const bool               tonemap = m_tonemap;
    const bool               transparent = m_transparent;
    const float              exposure = view.exposure;
    float                    flat[3];
    for (int c = 0; c < 3; ++c) {
        flat[c] = displayEncode(view.backdropColor[c] * exposure, tonemap);
    }

    // THE PLATE: the capture over the backdrop, as a display picture — then taken back to the
    // linear light the render is in. Over a backdrop it is opaque; with none it keeps the
    // capture's own coverage (and its color is un-premultiplied for the trip, as a color is).
    m_plate.resize(pixels * 4u);
    const int w = m_width;
    parallelFor(m_height, [&](int y) {
        const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (row + static_cast<std::size_t>(x)) * 4u;
            const float       cover = std::clamp(all[i + 3], 0.0f, 1.0f);
            float*            out = &m_plate[i];
            if (transparent) {
                const float inv = cover > 1.0e-5f ? 1.0f / cover : 0.0f;
                for (int c = 0; c < 3; ++c) {
                    out[c] = displayDecode(all[i + c] * inv, tonemap) * cover;
                }
                out[3] = cover;
            } else {
                for (int c = 0; c < 3; ++c) {
                    const float behind = perPixel ? displayEncode(backdrop[(row + static_cast<std::size_t>(x)) * 3u + c] * exposure, tonemap)
                                                  : flat[c];
                    out[c] = displayDecode(all[i + c] + (1.0f - cover) * behind, tonemap);
                }
                out[3] = 1.0f;
            }
        }
    });
}

void SplatComposite::compose(const float* render, unsigned char* out8, std::size_t stride8, float* linearOut) const {
    const int  w = m_width;
    const bool tonemap = m_tonemap;
    const bool transparent = m_transparent;
    parallelFor(m_height, [&](int y) {
        const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4u;
        unsigned char*    bytes = out8 + static_cast<std::size_t>(y) * stride8;
        for (int x = 0; x < w; ++x, bytes += 4) {
            const std::size_t i = row + static_cast<std::size_t>(x) * 4u;
            const float*      in = render + i;
            const float*      plate = &m_plate[i];
            const float*      front = &m_front[i];

            // The render over the plate, in linear light (both premultiplied).
            const float cover = std::clamp(in[3], 0.0f, 1.0f);
            const float through = 1.0f - cover;
            const float under[3] = {in[0] + through * plate[0], in[1] + through * plate[1], in[2] + through * plate[2]};
            const float underCover = cover + through * plate[3];

            // … shown, and the front of the capture over it (display values, premultiplied).
            const float inv = underCover > 1.0e-5f ? 1.0f / underCover : 0.0f;
            const float frontCover = std::clamp(front[3], 0.0f, 1.0f);
            const float behind = (1.0f - frontCover) * underCover;
            const float outCover = frontCover + behind;
            const float outInv = outCover > 1.0e-5f ? 1.0f / outCover : 0.0f;
            float       straight[3];
            for (int c = 0; c < 3; ++c) {
                const float shown = displayEncode(under[c] * inv, tonemap);
                straight[c] = (front[c] + behind * shown) * outInv;
                bytes[c] = toByte(straight[c]);
            }
            bytes[3] = transparent ? toByte(outCover) : 255;

            if (linearOut != nullptr) {
                // The same picture in linear light: the render's own values wherever the front of
                // the capture covers nothing (highlights above the display's white are kept), the
                // display picture taken back where it does.
                float* out = linearOut + i;
                if (frontCover > 1.0e-5f) {
                    for (int c = 0; c < 3; ++c) {
                        out[c] = displayDecode(straight[c], tonemap) * outCover;
                    }
                } else {
                    out[0] = under[0];
                    out[1] = under[1];
                    out[2] = under[2];
                }
                out[3] = transparent ? outCover : 1.0f;
            }
        }
    });
}

} // namespace pose
