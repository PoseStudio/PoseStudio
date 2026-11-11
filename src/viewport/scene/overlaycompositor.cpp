/**
 * @file overlaycompositor.cpp
 * @brief Implementation of the overlay compositor. See overlaycompositor.h.
 */

#include "overlaycompositor.h"

#include "parallelfor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>

namespace pose {

namespace {

constexpr float kInv255 = 1.0f / 255.0f;
constexpr float kPi = 3.14159265358979323846f;
/// Under this many texels a layer is painted on the calling thread: starting the worker threads
/// costs more than the work (a mole, a small scar).
constexpr uint64_t kParallelArea = 1u << 17;
/// A texel whose effective alpha is under this contributes nothing visible in 8 bits.
constexpr float kMinAlpha = 0.5f / 255.0f;

uint8_t toByte(float v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// --- Hue / saturation / lightness (the adjustment's working space) ----------------------------

void rgbToHsl(const glm::vec3& c, float& h, float& s, float& l) {
    const float mx = std::max(c.x, std::max(c.y, c.z));
    const float mn = std::min(c.x, std::min(c.y, c.z));
    l = 0.5f * (mx + mn);
    const float d = mx - mn;
    if (d < 1.0e-6f) {
        h = 0.0f;
        s = 0.0f;
        return;
    }
    s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == c.x) {
        h = (c.y - c.z) / d + (c.y < c.z ? 6.0f : 0.0f);
    } else if (mx == c.y) {
        h = (c.z - c.x) / d + 2.0f;
    } else {
        h = (c.x - c.y) / d + 4.0f;
    }
    h /= 6.0f;
}

float hueToChannel(float p, float q, float t) {
    if (t < 0.0f) {
        t += 1.0f;
    }
    if (t > 1.0f) {
        t -= 1.0f;
    }
    if (t < 1.0f / 6.0f) {
        return p + (q - p) * 6.0f * t;
    }
    if (t < 0.5f) {
        return q;
    }
    if (t < 2.0f / 3.0f) {
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    }
    return p;
}

glm::vec3 hslToRgb(float h, float s, float l) {
    if (s <= 0.0f) {
        return glm::vec3(l);
    }
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    return glm::vec3(hueToChannel(p, q, h + 1.0f / 3.0f), hueToChannel(p, q, h), hueToChannel(p, q, h - 1.0f / 3.0f));
}

/// An overlay's bitmap adjustments, prepared once per layer: the tone first (gamma, brightness,
/// contrast — what the picture's values are), then the tint (what color they are: a white
/// lipstick mask takes the tint whole), then hue and saturation (a turn of the result).
struct ColorAdjust {
    bool      useGamma = false;
    float     invGamma = 1.0f;
    bool      useTone = false;
    float     brightness = 0.0f;
    float     contrast = 1.0f; ///< the slope about mid grey
    bool      useTint = false;
    glm::vec3 tint{1.0f};
    bool      useHsl = false;
    float     hueTurns = 0.0f;
    float     saturation = 1.0f;

    explicit ColorAdjust(const MaterialOverlay& o) {
        const float gamma = std::clamp(o.gamma, 0.2f, 5.0f);
        useGamma = std::abs(gamma - 1.0f) > 1.0e-4f;
        invGamma = 1.0f / gamma;
        brightness = std::clamp(o.brightness, -1.0f, 1.0f);
        // -1 is flat grey (slope 0), 0 the picture as painted (slope 1), toward 1 a hard threshold.
        const float c = std::clamp(o.contrast, -1.0f, 0.98f);
        contrast = std::tan((c + 1.0f) * kPi * 0.25f);
        useTone = brightness != 0.0f || std::abs(contrast - 1.0f) > 1.0e-5f;
        tint = glm::clamp(o.tint, glm::vec3(0.0f), glm::vec3(1.0f));
        useTint = tint != glm::vec3(1.0f);
        hueTurns = o.hue / 360.0f;
        saturation = 1.0f + std::clamp(o.saturation, -1.0f, 1.0f);
        useHsl = o.hue != 0.0f || o.saturation != 0.0f;
    }

    glm::vec3 apply(glm::vec3 c) const {
        if (useGamma) {
            c = glm::vec3(std::pow(std::max(c.x, 0.0f), invGamma), std::pow(std::max(c.y, 0.0f), invGamma),
                          std::pow(std::max(c.z, 0.0f), invGamma));
        }
        if (useTone) {
            c = (c + brightness - 0.5f) * contrast + 0.5f;
            c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
        }
        if (useTint) {
            c *= tint;
        }
        if (useHsl) {
            float h = 0.0f;
            float s = 0.0f;
            float l = 0.0f;
            rgbToHsl(glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f)), h, s, l);
            h += hueTurns;
            h -= std::floor(h);
            s = std::clamp(s * saturation, 0.0f, 1.0f);
            c = hslToRgb(h, s, l);
        }
        return c;
    }
};

/// Where a layer is put on the map: the overlay scaled and turned about its pivot (its own
/// centre, see placementOf), then moved by its offset.
struct Placement {
    glm::vec2 offset{0.0f};       ///< a share of the map: x to the right, y UP
    float     rotationDeg = 0.0f; ///< clockwise as the map is seen
    float     scale = 1.0f;
    glm::vec2 pivot{0.5f};        ///< a share of the map: x to the right, y DOWN the image
    bool      transformed() const { return rotationDeg != 0.0f || scale != 1.0f; }
};

/// The side of the canvas an overlay of flat layers alone is painted on where the zone has no map
/// of its own (its color is the same at every texel).
constexpr uint32_t kFlatCanvas = 4;

/// The most sub-samples a side a target texel is read with where the layer is scaled down (an
/// overlay at a tenth of its size still reads every texel of its image, near enough).
constexpr int kMaxTaps = 8;

/// Reads a layer at the TARGET's texels: the kept rectangle mapped onto the target's size (the
/// overlay and the map it is painted on may differ in resolution), texel for texel where the
/// sizes match and bilinear — on premultiplied color, so a transparent texel's color never
/// bleeds into an edge — where they do not. A layer that is TURNED or SCALED is read through
/// the inverse of that placement (samplePlaced).
class LayerSampler {
public:
    /// @p place is where the overlay is put (see Placement). Its OFFSET is a share of the target's
    /// width to the right and of its height UP, applied as a whole number of target texels — a
    /// move of less than a texel shows nothing, and a whole one keeps the texel-for-texel path
    /// exact — and what it carries past the target's edge is cut. Its ROTATION turns the layer
    /// about the pivot, clockwise as the map is seen, and its SCALE sizes it about the same
    /// point; the pivot is taken to a whole target texel, so a quarter or a half turn at the
    /// painted size lands texel on texel, and anything else is resampled.
    LayerSampler(const OverlayImage& image, uint32_t targetWidth, uint32_t targetHeight, const Placement& place) : m_image(image) {
        const glm::vec2& offset = place.offset;
        m_same = image.fullWidth == targetWidth && image.fullHeight == targetHeight;
        m_scaleX = static_cast<float>(image.fullWidth) / static_cast<float>(std::max<uint32_t>(targetWidth, 1));
        m_scaleY = static_cast<float>(image.fullHeight) / static_cast<float>(std::max<uint32_t>(targetHeight, 1));
        m_shiftX = static_cast<int64_t>(std::llround(static_cast<double>(offset.x) * targetWidth));
        m_shiftY = -static_cast<int64_t>(std::llround(static_cast<double>(offset.y) * targetHeight)); // (rows run DOWN the map)
        if (place.transformed()) {
            // TURNED or SCALED: the general path.
            m_placed = true;
            const double quarter = static_cast<double>(place.rotationDeg) / 90.0;
            if (quarter == std::floor(quarter)) {
                // (A whole number of quarter turns: exact, so its texels land on texels.)
                static constexpr double kCos[4] = {1.0, 0.0, -1.0, 0.0};
                static constexpr double kSin[4] = {0.0, 1.0, 0.0, -1.0};
                const int q = static_cast<int>(((static_cast<long long>(quarter) % 4) + 4) % 4);
                m_cos = kCos[q];
                m_sin = kSin[q];
            } else {
                const double radians = static_cast<double>(place.rotationDeg) * 3.14159265358979323846 / 180.0;
                m_cos = std::cos(radians);
                m_sin = std::sin(radians);
            }
            const double zoom = std::clamp(static_cast<double>(place.scale), 0.01, 100.0);
            m_invZoom = 1.0 / zoom;
            m_pivotX = std::round(static_cast<double>(place.pivot.x) * targetWidth);
            m_pivotY = std::round(static_cast<double>(place.pivot.y) * targetHeight);
            // Scaled down, a target texel covers several of the image's: it is read with that
            // many sub-samples a side, so thin lines thin out instead of breaking up.
            const double perTexel = std::max(static_cast<double>(m_scaleX), static_cast<double>(m_scaleY)) * m_invZoom;
            m_taps = std::clamp(static_cast<int>(std::ceil(perTexel - 1.0e-3)), 1, kMaxTaps);
            // What is walked: the kept rectangle and one image texel around it (the edge is
            // filtered over that texel), its corners scaled and turned about the pivot and moved.
            const double l = (static_cast<double>(image.x) - 1.0) / m_scaleX;
            const double t = (static_cast<double>(image.y) - 1.0) / m_scaleY;
            const double r = (static_cast<double>(image.x) + image.width + 1.0) / m_scaleX;
            const double b = (static_cast<double>(image.y) + image.height + 1.0) / m_scaleY;
            const double corners[4][2] = {{l, t}, {r, t}, {l, b}, {r, b}};
            double       minX = 0.0;
            double       minY = 0.0;
            double       maxX = 0.0;
            double       maxY = 0.0;
            for (int i = 0; i < 4; ++i) {
                const double dx = (corners[i][0] - m_pivotX) * zoom;
                const double dy = (corners[i][1] - m_pivotY) * zoom;
                const double x = m_pivotX + m_cos * dx - m_sin * dy + static_cast<double>(m_shiftX);
                const double y = m_pivotY + m_sin * dx + m_cos * dy + static_cast<double>(m_shiftY);
                minX = i == 0 ? x : std::min(minX, x);
                maxX = i == 0 ? x : std::max(maxX, x);
                minY = i == 0 ? y : std::min(minY, y);
                maxY = i == 0 ? y : std::max(maxY, y);
            }
            const auto bound = [](double v, uint32_t size) { return static_cast<uint32_t>(std::clamp(v, 0.0, static_cast<double>(size))); };
            x0 = bound(std::floor(minX), targetWidth);
            y0 = bound(std::floor(minY), targetHeight);
            x1 = std::max(x0, bound(std::ceil(maxX), targetWidth));
            y1 = std::max(y0, bound(std::ceil(maxY), targetHeight));
            return;
        }
        int64_t left = 0;
        int64_t top = 0;
        int64_t right = 0;
        int64_t bottom = 0;
        if (m_same) {
            left = image.x;
            top = image.y;
            right = static_cast<int64_t>(image.x) + image.width;
            bottom = static_cast<int64_t>(image.y) + image.height;
        } else {
            left = static_cast<int64_t>(std::floor(static_cast<float>(image.x) / m_scaleX));
            top = static_cast<int64_t>(std::floor(static_cast<float>(image.y) / m_scaleY));
            right = static_cast<int64_t>(std::ceil(static_cast<float>(image.x + image.width) / m_scaleX));
            bottom = static_cast<int64_t>(std::ceil(static_cast<float>(image.y + image.height) / m_scaleY));
        }
        const auto within = [](int64_t v, uint32_t size) { return static_cast<uint32_t>(std::clamp<int64_t>(v, 0, size)); };
        x0 = within(left + m_shiftX, targetWidth);
        y0 = within(top + m_shiftY, targetHeight);
        x1 = std::max(x0, within(right + m_shiftX, targetWidth));
        y1 = std::max(y0, within(bottom + m_shiftY, targetHeight));
    }

    /// The target rectangle the layer covers ([x0, x1) × [y0, y1)).
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t x1 = 0;
    uint32_t y1 = 0;

    uint64_t area() const { return static_cast<uint64_t>(x1 - x0) * (y1 - y0); }

    /// The layer at target texel (@p tx, @p ty) — inside the rectangle above — as 0..1 RGBA.
    /// False where the texel falls outside the kept rectangle after the size mapping.
    bool sample(uint32_t tx, uint32_t ty, float out[4]) const {
        if (m_placed) {
            return samplePlaced(tx, ty, out);
        }
        const OverlayImage& img = m_image;
        // The target texel in the overlay's own, un-moved coordinates.
        const int64_t sx = static_cast<int64_t>(tx) - m_shiftX;
        const int64_t sy = static_cast<int64_t>(ty) - m_shiftY;
        if (m_same) {
            const uint8_t* p = &img.rgba[(static_cast<std::size_t>(sy - img.y) * img.width + static_cast<std::size_t>(sx - img.x)) * 4];
            out[0] = p[0] * kInv255;
            out[1] = p[1] * kInv255;
            out[2] = p[2] * kInv255;
            out[3] = p[3] * kInv255;
            return true;
        }
        float fx = (static_cast<float>(sx) + 0.5f) * m_scaleX - 0.5f - static_cast<float>(img.x);
        float fy = (static_cast<float>(sy) + 0.5f) * m_scaleY - 0.5f - static_cast<float>(img.y);
        const float maxX = static_cast<float>(img.width - 1);
        const float maxY = static_cast<float>(img.height - 1);
        if (fx < -0.5f || fy < -0.5f || fx > maxX + 0.5f || fy > maxY + 0.5f) {
            return false;
        }
        fx = std::clamp(fx, 0.0f, maxX);
        fy = std::clamp(fy, 0.0f, maxY);
        const uint32_t ix = static_cast<uint32_t>(fx);
        const uint32_t iy = static_cast<uint32_t>(fy);
        const uint32_t jx = std::min(ix + 1, img.width - 1);
        const uint32_t jy = std::min(iy + 1, img.height - 1);
        const float    wx = fx - static_cast<float>(ix);
        const float    wy = fy - static_cast<float>(iy);
        const float    weights[4] = {(1.0f - wx) * (1.0f - wy), wx * (1.0f - wy), (1.0f - wx) * wy, wx * wy};
        const uint8_t* texels[4] = {&img.rgba[(static_cast<std::size_t>(iy) * img.width + ix) * 4],
                                    &img.rgba[(static_cast<std::size_t>(iy) * img.width + jx) * 4],
                                    &img.rgba[(static_cast<std::size_t>(jy) * img.width + ix) * 4],
                                    &img.rgba[(static_cast<std::size_t>(jy) * img.width + jx) * 4]};
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const float wa = weights[k] * texels[k][3] * kInv255;
            r += texels[k][0] * kInv255 * wa;
            g += texels[k][1] * kInv255 * wa;
            b += texels[k][2] * kInv255 * wa;
            a += wa;
        }
        if (a > 1.0e-6f) {
            out[0] = r / a;
            out[1] = g / a;
            out[2] = b / a;
        } else {
            out[0] = out[1] = out[2] = 0.0f;
        }
        out[3] = a;
        return true;
    }

    /// Whether the layer is turned — its vectors (a normal image's slopes) must turn with it.
    bool turned() const { return m_sin != 0.0 || m_cos != 1.0; }
    /// A vector of the layer (x to the right, y down the image: a normal map's slope) turned as
    /// the layer is.
    void turn(float& x, float& y) const {
        const float tx = static_cast<float>(m_cos) * x - static_cast<float>(m_sin) * y;
        y = static_cast<float>(m_sin) * x + static_cast<float>(m_cos) * y;
        x = tx;
    }

private:
    /// The turned or scaled layer at a target texel: the texel taken back through the offset,
    /// the rotation and the scale into the image, and read there bilinearly on premultiplied
    /// color — m_taps × m_taps times across the texel where the layer is scaled down. What lies
    /// outside the kept rectangle counts as transparent, which it is (the rectangle is where the
    /// image has content), so the layer's edge is filtered rather than stepped.
    bool samplePlaced(uint32_t tx, uint32_t ty, float out[4]) const {
        const OverlayImage& img = m_image;
        const float         maxX = static_cast<float>(img.width);
        const float         maxY = static_cast<float>(img.height);
        const double        step = 1.0 / m_taps;
        float               r = 0.0f;
        float               g = 0.0f;
        float               b = 0.0f;
        float               a = 0.0f;
        for (int j = 0; j < m_taps; ++j) {
            const double dy = static_cast<double>(ty) + (j + 0.5) * step - static_cast<double>(m_shiftY) - m_pivotY;
            for (int i = 0; i < m_taps; ++i) {
                const double dx = static_cast<double>(tx) + (i + 0.5) * step - static_cast<double>(m_shiftX) - m_pivotX;
                const double ux = m_pivotX + (m_cos * dx + m_sin * dy) * m_invZoom; // (the inverse turn and scale)
                const double uy = m_pivotY + (m_cos * dy - m_sin * dx) * m_invZoom;
                const float  fx = static_cast<float>(ux * m_scaleX - 0.5 - static_cast<double>(img.x));
                const float  fy = static_cast<float>(uy * m_scaleY - 0.5 - static_cast<double>(img.y));
                if (fx <= -1.0f || fy <= -1.0f || fx >= maxX || fy >= maxY) {
                    continue;
                }
                const float   flx = std::floor(fx);
                const float   fly = std::floor(fy);
                const int64_t ix = static_cast<int64_t>(flx);
                const int64_t iy = static_cast<int64_t>(fly);
                const float   wx = fx - flx;
                const float   wy = fy - fly;
                const float   weights[4] = {(1.0f - wx) * (1.0f - wy), wx * (1.0f - wy), (1.0f - wx) * wy, wx * wy};
                const int64_t taps[4][2] = {{ix, iy}, {ix + 1, iy}, {ix, iy + 1}, {ix + 1, iy + 1}};
                for (int k = 0; k < 4; ++k) {
                    if (weights[k] <= 0.0f || taps[k][0] < 0 || taps[k][1] < 0 || taps[k][0] >= static_cast<int64_t>(img.width) ||
                        taps[k][1] >= static_cast<int64_t>(img.height)) {
                        continue;
                    }
                    const uint8_t* p = &img.rgba[(static_cast<std::size_t>(taps[k][1]) * img.width + static_cast<std::size_t>(taps[k][0])) * 4];
                    const float    wa = weights[k] * p[3] * kInv255;
                    r += p[0] * kInv255 * wa;
                    g += p[1] * kInv255 * wa;
                    b += p[2] * kInv255 * wa;
                    a += wa;
                }
            }
        }
        if (a <= 1.0e-6f) {
            return false;
        }
        out[0] = r / a;
        out[1] = g / a;
        out[2] = b / a;
        out[3] = std::min(a / static_cast<float>(m_taps * m_taps), 1.0f);
        return true;
    }

    const OverlayImage& m_image;
    bool                m_same = true;
    float               m_scaleX = 1.0f;  ///< the image's texels per texel of the target
    float               m_scaleY = 1.0f;
    int64_t             m_shiftX = 0; ///< the overlay's offset in target texels, + right
    int64_t             m_shiftY = 0; ///< … and + DOWN (the offset's y is up)
    bool                m_placed = false; ///< turned or scaled: samplePlaced's path
    double              m_cos = 1.0;      ///< of the rotation (clockwise on the map, rows down)
    double              m_sin = 0.0;
    double              m_invZoom = 1.0;  ///< 1 / the overlay's scale
    double              m_pivotX = 0.0;   ///< what it turns and scales about, in target texels (whole ones)
    double              m_pivotY = 0.0;
    int                 m_taps = 1;       ///< sub-samples a side per target texel (scaled down: more)
};

/// Runs @p paintRow(y) for every target row the layer covers — across the cores for a layer worth
/// it, in bands (a row is too small a unit to claim one at a time).
template <typename Fn>
void forEachRow(uint32_t y0, uint32_t y1, uint64_t area, Fn&& paintRow) {
    if (y1 <= y0) {
        return;
    }
    const uint32_t rows = y1 - y0;
    if (area < kParallelArea) {
        for (uint32_t y = y0; y < y1; ++y) {
            paintRow(y);
        }
        return;
    }
    const int bands = static_cast<int>(std::min<uint32_t>(rows, 64));
    parallelFor(bands, [&](int band) {
        const uint32_t from = y0 + static_cast<uint32_t>((static_cast<uint64_t>(rows) * static_cast<uint64_t>(band)) / static_cast<uint64_t>(bands));
        const uint32_t to = y0 + static_cast<uint32_t>((static_cast<uint64_t>(rows) * static_cast<uint64_t>(band + 1)) / static_cast<uint64_t>(bands));
        for (uint32_t y = from; y < to; ++y) {
            paintRow(y);
        }
    });
}
template <typename Fn>
void forEachRow(const LayerSampler& sampler, Fn&& paintRow) {
    if (sampler.x1 > sampler.x0) {
        forEachRow(sampler.y0, sampler.y1, sampler.area(), paintRow);
    }
}

/// A LIMITING MAP at the target's texels, 0 .. 1 — the overlay's shape (its opacity map: where
/// its color, gloss and metal go) or a channel's own map within it (the metallic map) — read
/// through the overlay's placement like its other images. Without one there is no limit.
class LimitSampler {
public:
    LimitSampler(const OverlayImage* limit, uint32_t targetWidth, uint32_t targetHeight, const Placement& place) {
        if (limit != nullptr) {
            m_sampler.emplace(*limit, targetWidth, targetHeight, place);
        }
    }

    /// Narrows the target rectangle [x0, x1) × [y0, y1) to where the map can be anything but 0.
    void clip(uint32_t& x0, uint32_t& y0, uint32_t& x1, uint32_t& y1) const {
        if (!m_sampler) {
            return;
        }
        x0 = std::max(x0, m_sampler->x0);
        y0 = std::max(y0, m_sampler->y0);
        x1 = std::max(x0, std::min(x1, m_sampler->x1));
        y1 = std::max(y0, std::min(y1, m_sampler->y1));
    }

    float at(uint32_t x, uint32_t y) const {
        if (!m_sampler) {
            return 1.0f;
        }
        if (x < m_sampler->x0 || y < m_sampler->y0 || x >= m_sampler->x1 || y >= m_sampler->y1) {
            return 0.0f;
        }
        float src[4];
        return m_sampler->sample(x, y, src) ? src[3] : 0.0f;
    }

private:
    std::optional<LayerSampler> m_sampler;
};

/// The rectangle of the target a color, gloss or metal layer is painted over: its image's, or — a
/// FLAT layer, with no image — the whole target; narrowed to the overlay's shape (its opacity map).
struct LayerRect {
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t x1 = 0;
    uint32_t y1 = 0;

    LayerRect(const std::optional<LayerSampler>& sampler, const LimitSampler& shaped, const DecodedImage& target) {
        x1 = target.width;
        y1 = target.height;
        if (sampler) {
            x0 = sampler->x0;
            y0 = sampler->y0;
            x1 = sampler->x1;
            y1 = sampler->y1;
        }
        shaped.clip(x0, y0, x1, y1);
    }
    bool     empty() const { return x1 <= x0 || y1 <= y0; }
    uint64_t area() const { return static_cast<uint64_t>(x1 - x0) * (y1 - y0); }
};

/// The overlay's color over the base color: its picture (@p image) — or, without one, a FLAT
/// layer of its Base Color — adjusted, blended by the overlay's mode, and mixed in by the image's
/// alpha × the opacity × the opacity's map (@p shape: the overlay's mask). A flat layer covers
/// what the map does and, without a map, the whole target. The target's own alpha (a cutout
/// baked later) is left alone.
void paintColor(DecodedImage& target, const OverlayImage* image, const MaterialOverlay& overlay, const Placement& place, const OverlayImage* shape) {
    std::optional<LayerSampler> sampler;
    if (image != nullptr) {
        sampler.emplace(*image, target.width, target.height, place);
    }
    const LimitSampler shaped(shape, target.width, target.height, place);
    const LayerRect    rect(sampler, shaped, target);
    if (rect.empty()) {
        return;
    }
    const ColorAdjust  adjust(overlay);
    const glm::vec3    flat = adjust.apply(glm::vec3(1.0f)); // (no image: white through the adjustments — the Base Color itself)
    const float        opacity = std::clamp(overlay.opacity, 0.0f, 1.0f);
    const OverlayBlend blend = overlay.blend;
    forEachRow(rect.y0, rect.y1, rect.area(), [&](uint32_t y) {
        uint8_t* row = target.pixels.data() + static_cast<std::size_t>(y) * target.width * 4;
        float    src[4];
        for (uint32_t x = rect.x0; x < rect.x1; ++x) {
            float a = opacity;
            if (sampler) {
                if (!sampler->sample(x, y, src)) {
                    continue;
                }
                a = src[3] * opacity;
            }
            if (a < kMinAlpha) {
                continue;
            }
            a *= shaped.at(x, y);
            if (a < kMinAlpha) {
                continue;
            }
            uint8_t*        d = row + static_cast<std::size_t>(x) * 4;
            const glm::vec3 dest(d[0] * kInv255, d[1] * kInv255, d[2] * kInv255);
            const glm::vec3 blended = blendOverlayColor(blend, dest, sampler ? adjust.apply(glm::vec3(src[0], src[1], src[2])) : flat);
            const glm::vec3 out = dest + (blended - dest) * a;
            d[0] = toByte(out.x);
            d[1] = toByte(out.y);
            d[2] = toByte(out.z);
        }
    });
}

/// An AMOUNT painted into one channel of a packed map, where the overlay is — through the
/// picture's alpha (everywhere, for a flat layer), the opacity's map (@p shape: the overlay's
/// mask) and @p own (the channel's own map: where ITS effect goes within the overlay); never the
/// opacity VALUE. A positive @p amount takes the channel's value toward 0 by that share, a
/// negative one toward 1:
///   the GLOSS into the roughness — amount = the gloss. The map is the material's roughness per
///   texel as a share of the Roughness value, so glossier is a smaller share and more matte a
///   share nearer the whole; sheer lipstick is as glossy as opaque.
///   the METAL into the metalness — amount = MINUS the overlay's Metallic (more metallic is a
///   value nearer 1), on the zone's absolute metalness (see compositeOverlays).
void paintAmount(DecodedImage& target, int channel, const OverlayImage* image, float amount, const Placement& place, const OverlayImage* shape,
                 const OverlayImage* own) {
    std::optional<LayerSampler> sampler;
    if (image != nullptr) {
        sampler.emplace(*image, target.width, target.height, place);
    }
    const LimitSampler shaped(shape, target.width, target.height, place);
    const LimitSampler owned(own, target.width, target.height, place);
    LayerRect          rect(sampler, shaped, target);
    owned.clip(rect.x0, rect.y0, rect.x1, rect.y1);
    if (rect.empty()) {
        return;
    }
    const float       gloss = std::clamp(amount, -1.0f, 1.0f);
    const std::size_t c = static_cast<std::size_t>(std::clamp(channel, 0, 3));
    forEachRow(rect.y0, rect.y1, rect.area(), [&](uint32_t y) {
        uint8_t* row = target.pixels.data() + static_cast<std::size_t>(y) * target.width * 4;
        float    src[4];
        for (uint32_t x = rect.x0; x < rect.x1; ++x) {
            float cover = 1.0f;
            if (sampler) {
                if (!sampler->sample(x, y, src)) {
                    continue;
                }
                cover = src[3];
            }
            if (cover < kMinAlpha) {
                continue;
            }
            cover *= shaped.at(x, y);
            if (cover < kMinAlpha) {
                continue;
            }
            cover *= owned.at(x, y);
            if (cover < kMinAlpha) {
                continue;
            }
            uint8_t&    d = row[static_cast<std::size_t>(x) * 4 + c];
            const float v = d * kInv255;
            const float k = cover * std::abs(gloss);
            d = toByte(gloss > 0.0f ? v * (1.0f - k) : v + (1.0f - v) * k);
        }
    });
}

/// A layer's value as a signed height about mid grey: 127 and 128 are both "no change" (an
/// 8-bit image has no exact middle), so an opaque height image leaves what it does not paint.
float signedHeight(float value01) {
    const float d = value01 * 255.0f - 127.5f;
    return std::abs(d) <= 0.5f ? 0.0f : d * kInv255;
}

/// A grayscale height image ADDED to the height map about mid grey, by @p strength through the
/// image's own alpha: the map's grain (the skin's pores) stays under a scar's ridge. (The relief
/// answers to its own image and strength alone — not the overlay's opacity, nor the opacity's map.)
void paintHeight(DecodedImage& target, const OverlayImage& image, float strength, const Placement& place) {
    const LayerSampler sampler(image, target.width, target.height, place);
    forEachRow(sampler, [&](uint32_t y) {
        uint8_t* row = target.pixels.data() + static_cast<std::size_t>(y) * target.width * 4;
        float    src[4];
        for (uint32_t x = sampler.x0; x < sampler.x1; ++x) {
            if (!sampler.sample(x, y, src) || src[3] < kMinAlpha) {
                continue;
            }
            const float delta = signedHeight(src[0]) * strength * src[3];
            if (delta == 0.0f) {
                continue;
            }
            uint8_t*      d = row + static_cast<std::size_t>(x) * 4;
            const uint8_t v = toByte(d[0] * kInv255 + delta);
            d[0] = v;
            d[1] = v;
            d[2] = v;
        }
    });
}

/// A normal's x or y from a byte, flat where the byte is the 8-bit middle (127 or 128).
float signedNormal(float value01) {
    const float d = value01 * 255.0f - 127.5f;
    return std::abs(d) <= 0.5f ? 0.0f : d / 127.5f;
}

/// A tangent-space normal image over the normal map: the overlay's slope added to the map's (the
/// "UDN" blend — the map's own detail kept, the overlay's relief on top), by @p strength through
/// the image's alpha. A TURNED overlay's slopes are turned with it: a normal's x and y are a
/// vector across the map (x to the right, y down the image, as the mesh shader reads them), and
/// a ridge that ran across the map and now runs down it must lean the new way. A SCALED one's
/// are left as they are: the relief grows with the picture, as a decal's does.
void paintNormal(DecodedImage& target, const OverlayImage& image, float strength, const Placement& place) {
    const LayerSampler sampler(image, target.width, target.height, place);
    forEachRow(sampler, [&](uint32_t y) {
        uint8_t* row = target.pixels.data() + static_cast<std::size_t>(y) * target.width * 4;
        float    src[4];
        for (uint32_t x = sampler.x0; x < sampler.x1; ++x) {
            if (!sampler.sample(x, y, src) || src[3] < kMinAlpha) {
                continue;
            }
            const float k = strength * src[3];
            float       ox = signedNormal(src[0]) * k;
            float       oy = signedNormal(src[1]) * k;
            if (ox == 0.0f && oy == 0.0f) {
                continue;
            }
            if (sampler.turned()) {
                sampler.turn(ox, oy);
            }
            uint8_t*  d = row + static_cast<std::size_t>(x) * 4;
            glm::vec3 n(d[0] * kInv255 * 2.0f - 1.0f + ox, d[1] * kInv255 * 2.0f - 1.0f + oy,
                        std::max(d[2] * kInv255 * 2.0f - 1.0f, 0.01f));
            n = glm::normalize(n);
            d[0] = toByte(n.x * 0.5f + 0.5f);
            d[1] = toByte(n.y * 0.5f + 0.5f);
            d[2] = toByte(n.z * 0.5f + 0.5f);
        }
    });
}

/// A grayscale height image painted into the NORMAL map as the slope of its height — the
/// viewport's way of drawing an overlay's bump (see compositeOverlays). The height the overlay
/// adds at a texel is its value about mid grey × its alpha (so it falls to nothing at the
/// image's edge); its central difference across the map, per UV unit, × kOverlayBumpSlope ×
/// @p strength is the slope, added to the map's normal with the mesh shader's own bump
/// convention: the normal leans (-dh/du, -dh/dv), v running down the image.
void paintHeightAsNormal(DecodedImage& target, const OverlayImage& image, float strength, const Placement& place) {
    const LayerSampler sampler(image, target.width, target.height, place);
    if (sampler.x1 <= sampler.x0 || sampler.y1 <= sampler.y0) {
        return;
    }
    // One texel beyond the layer each way: the height steps to nothing there, which is a slope.
    const uint32_t x0 = sampler.x0 > 0 ? sampler.x0 - 1 : 0;
    const uint32_t y0 = sampler.y0 > 0 ? sampler.y0 - 1 : 0;
    const uint32_t x1 = std::min(sampler.x1 + 1, target.width);
    const uint32_t y1 = std::min(sampler.y1 + 1, target.height);
    const float    scaleX = strength * kOverlayBumpSlope * 0.5f * static_cast<float>(target.width);
    const float    scaleY = strength * kOverlayBumpSlope * 0.5f * static_cast<float>(target.height);
    const auto     heightAt = [&sampler](int64_t x, int64_t y) {
        if (x < static_cast<int64_t>(sampler.x0) || y < static_cast<int64_t>(sampler.y0) || x >= static_cast<int64_t>(sampler.x1) ||
            y >= static_cast<int64_t>(sampler.y1)) {
            return 0.0f;
        }
        float src[4];
        if (!sampler.sample(static_cast<uint32_t>(x), static_cast<uint32_t>(y), src)) {
            return 0.0f;
        }
        return signedHeight(src[0]) * src[3];
    };
    forEachRow(y0, y1, static_cast<uint64_t>(x1 - x0) * (y1 - y0), [&](uint32_t y) {
        uint8_t* row = target.pixels.data() + static_cast<std::size_t>(y) * target.width * 4;
        for (uint32_t x = x0; x < x1; ++x) {
            const float ox = -(heightAt(static_cast<int64_t>(x) + 1, y) - heightAt(static_cast<int64_t>(x) - 1, y)) * scaleX;
            const float oy = -(heightAt(x, static_cast<int64_t>(y) + 1) - heightAt(x, static_cast<int64_t>(y) - 1)) * scaleY;
            if (ox == 0.0f && oy == 0.0f) {
                continue;
            }
            uint8_t*  d = row + static_cast<std::size_t>(x) * 4;
            glm::vec3 n(d[0] * kInv255 * 2.0f - 1.0f + ox, d[1] * kInv255 * 2.0f - 1.0f + oy,
                        std::max(d[2] * kInv255 * 2.0f - 1.0f, 0.01f));
            n = glm::normalize(n);
            d[0] = toByte(n.x * 0.5f + 0.5f);
            d[1] = toByte(n.y * 0.5f + 0.5f);
            d[2] = toByte(n.z * 0.5f + 0.5f);
        }
    });
}

/// Which of an overlay's images slot @p map is painted from, and what it is read as.
const std::string& sourceFor(const MaterialOverlay& o, MaterialMap map) {
    switch (map) {
    case MaterialMap::Bump: return o.bump;
    case MaterialMap::Normal: return o.normal;
    case MaterialMap::Displacement: return o.displacement;
    default: return o.color; // the color, and the gloss through its alpha
    }
}
OverlayImageKind kindFor(MaterialMap map) {
    switch (map) {
    case MaterialMap::Bump:
    case MaterialMap::Displacement: return OverlayImageKind::Height;
    case MaterialMap::Normal: return OverlayImageKind::Normal;
    default: return OverlayImageKind::Color; // the picture: the color, and the gloss and the metal through its alpha
    }
}

/// Where an overlay is put: its offset, rotation and scale, and — turned or scaled — what it
/// does so about: the centre of its ANCHOR image's content (the picture's, else the opacity
/// map's, else the first image it has that can be had), as a share of that image. One point for the whole group, so the
/// color, the gloss and the relief stay registered whichever of them a slot shows.
Placement placementOf(const MaterialOverlay& o, const OverlayImageSource& images) {
    Placement place;
    place.offset = o.offset;
    place.rotationDeg = o.rotation;
    place.scale = o.scale;
    if (!place.transformed() || !images) {
        return place; // (as painted, or only moved: no centre is needed, and no image fetched for one)
    }
    const std::pair<const std::string*, OverlayImageKind> candidates[6] = {{&o.color, OverlayImageKind::Color},
                                                                           {&o.opacityMap, OverlayImageKind::Mask},
                                                                           {&o.metallicMap, OverlayImageKind::Mask},
                                                                           {&o.bump, OverlayImageKind::Height},
                                                                           {&o.normal, OverlayImageKind::Normal},
                                                                           {&o.displacement, OverlayImageKind::Height}};
    for (const auto& candidate : candidates) {
        if (candidate.first->empty()) {
            continue;
        }
        const std::shared_ptr<const OverlayImage> anchor = images(*candidate.first, candidate.second);
        if (anchor && !anchor->empty() && anchor->fullWidth > 0 && anchor->fullHeight > 0) {
            place.pivot = glm::vec2((static_cast<float>(anchor->x) + 0.5f * static_cast<float>(anchor->width)) / static_cast<float>(anchor->fullWidth),
                                    (static_cast<float>(anchor->y) + 0.5f * static_cast<float>(anchor->height)) / static_cast<float>(anchor->fullHeight));
            break;
        }
    }
    return place;
}

} // namespace

glm::vec3 adjustOverlayColor(const MaterialOverlay& overlay, glm::vec3 rgb) {
    return ColorAdjust(overlay).apply(rgb);
}

glm::vec3 blendOverlayColor(OverlayBlend blend, const glm::vec3& dest, const glm::vec3& source) {
    const auto each = [&](float (*fn)(float, float)) { return glm::vec3(fn(dest.x, source.x), fn(dest.y, source.y), fn(dest.z, source.z)); };
    switch (blend) {
    case OverlayBlend::Multiply: return dest * source;
    case OverlayBlend::Screen: return glm::vec3(1.0f) - (glm::vec3(1.0f) - dest) * (glm::vec3(1.0f) - source);
    case OverlayBlend::Overlay:
        return each([](float d, float s) { return d < 0.5f ? 2.0f * d * s : 1.0f - 2.0f * (1.0f - d) * (1.0f - s); });
    case OverlayBlend::SoftLight:
        // (The W3C compositing formula: continuous at the mid point, unlike the older editors'.)
        return each([](float d, float s) {
            if (s <= 0.5f) {
                return d - (1.0f - 2.0f * s) * d * (1.0f - d);
            }
            const float g = d <= 0.25f ? ((16.0f * d - 12.0f) * d + 4.0f) * d : std::sqrt(d);
            return d + (2.0f * s - 1.0f) * (g - d);
        });
    case OverlayBlend::Darken: return glm::min(dest, source);
    case OverlayBlend::Lighten: return glm::max(dest, source);
    default: return source; // Normal
    }
}

std::shared_ptr<OverlayImage> makeOverlayImage(const DecodedImage& image, OverlayImageKind kind) {
    auto out = std::make_shared<OverlayImage>();
    out->fullWidth = image.width;
    out->fullHeight = image.height;
    const uint32_t w = image.width;
    const uint32_t h = image.height;
    if (w == 0 || h == 0 || image.pixels.size() < static_cast<std::size_t>(w) * h * 4) {
        return out;
    }
    // Two rectangles in one pass: where the alpha is above 0, and — for an image with no
    // transparency at all — where a height is off mid grey or a normal off flat.
    struct Box {
        uint32_t x0, y0, x1, y1; // [x0, x1] × [y0, y1], empty while x1 < x0
        void     add(uint32_t x, uint32_t y) {
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
        bool empty() const { return x1 < x0 || y1 < y0; }
    };
    Box  alphaBox{w, h, 0, 0};
    Box  valueBox{w, h, 0, 0};
    bool transparent = false;
    bool alphaAny = false;
    bool valueAny = false;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = image.pixels.data() + static_cast<std::size_t>(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = row + static_cast<std::size_t>(x) * 4;
            if (p[3] != 255) {
                transparent = true;
            }
            if (p[3] != 0) {
                alphaBox.add(x, y);
                alphaAny = true;
            }
            const bool off = kind == OverlayImageKind::Height   ? (p[0] < 127 || p[0] > 128)
                             : kind == OverlayImageKind::Normal ? (p[0] < 126 || p[0] > 129 || p[1] < 126 || p[1] > 129)
                             : kind == OverlayImageKind::Mask   ? ((p[0] | p[1] | p[2]) != 0)
                                                                : true;
            if (off) {
                valueBox.add(x, y);
                valueAny = true;
            }
        }
    }
    out->hasAlpha = transparent;
    const Box& box = transparent ? alphaBox : valueBox;
    if (!(transparent ? alphaAny : valueAny) || box.empty()) {
        return out; // nothing anywhere
    }
    out->x = box.x0;
    out->y = box.y0;
    out->width = box.x1 - box.x0 + 1;
    out->height = box.y1 - box.y0 + 1;
    out->rgba.resize(static_cast<std::size_t>(out->width) * out->height * 4);
    for (uint32_t y = 0; y < out->height; ++y) {
        std::memcpy(out->rgba.data() + static_cast<std::size_t>(y) * out->width * 4,
                    image.pixels.data() + (static_cast<std::size_t>(out->y + y) * w + out->x) * 4, static_cast<std::size_t>(out->width) * 4);
    }
    if (kind == OverlayImageKind::Mask) {
        // A mask is kept as ONE value a texel, in the alpha of a white image: the source's alpha
        // where it has transparency (its opaque part is the mask, whatever its color), else its
        // brightness (white applies, black does not).
        const std::size_t texels = static_cast<std::size_t>(out->width) * out->height;
        for (std::size_t i = 0; i < texels; ++i) {
            uint8_t*      p = &out->rgba[i * 4];
            const uint8_t value = transparent ? p[3] : static_cast<uint8_t>((77u * p[0] + 150u * p[1] + 29u * p[2] + 128u) >> 8);
            p[0] = p[1] = p[2] = 255;
            p[3] = value;
        }
        out->hasAlpha = true;
    }
    return out;
}

std::shared_ptr<DecodedImage> compositeOverlays(MaterialMap map, const std::shared_ptr<const DecodedImage>& base, int channel,
                                                const std::vector<MaterialOverlay>& overlays, const OverlayImageSource& images,
                                                bool bumpAsNormal, float baseScale) {
    // The layers that touch this slot and whose image can be had, in stack order. (For the
    // viewport's normal map an overlay may be two: its bump image as a slope, then its normal.
    // The color, the gloss and the metal go through the overlay's PICTURE and its SHAPE — the
    // opacity's map — and with no picture they are a flat layer, over the map's area or the whole
    // target. The relief goes through its own images alone.)
    struct Layer {
        const MaterialOverlay*              overlay = nullptr;
        std::shared_ptr<const OverlayImage> image; ///< null: a flat layer (the color, the gloss, the metal)
        bool                                heightAsNormal = false;
        Placement                           place;
        std::shared_ptr<const OverlayImage> shape; ///< the opacity map (the color's, the gloss's, the metal's); null: none
        std::shared_ptr<const OverlayImage> own;   ///< the slot's own limiting map (the metallic map); null: none
    };
    std::vector<Layer> layers;
    for (const MaterialOverlay& o : overlays) {
        if (!images) {
            break;
        }
        const std::size_t first = layers.size();
        const bool        shaped = map == MaterialMap::BaseColor || map == MaterialMap::Roughness || map == MaterialMap::Metalness;
        if (bumpAsNormal && map == MaterialMap::Normal && overlayAffects(o, MaterialMap::Bump)) {
            std::shared_ptr<const OverlayImage> image = images(o.bump, OverlayImageKind::Height);
            if (image && !image->empty()) {
                layers.push_back({&o, std::move(image), true, {}, nullptr, nullptr});
            }
        }
        if (overlayAffects(o, map)) {
            const std::string& source = sourceFor(o, map);
            if (source.empty()) {
                // (Only the color, the gloss and the metal go without an image of their own.)
                if (shaped) {
                    layers.push_back({&o, nullptr, false, {}, nullptr, nullptr});
                }
            } else {
                // (A picture that cannot be had is no layer: it is never painted flat instead.)
                std::shared_ptr<const OverlayImage> image = images(source, kindFor(map));
                if (image && !image->empty()) {
                    layers.push_back({&o, std::move(image), false, {}, nullptr, nullptr});
                }
            }
        }
        if (layers.size() == first) {
            continue;
        }
        // THE OPACITY'S MAP is the overlay's shape for its color, gloss and metal, and the
        // METALLIC MAP limits the metal within it. One that cannot be had, or that lets nothing
        // through, takes the layer out: without it a flat color — or the metal — would flood.
        std::shared_ptr<const OverlayImage> shape;
        std::shared_ptr<const OverlayImage> own;
        if (shaped && !o.opacityMap.empty()) {
            shape = images(o.opacityMap, OverlayImageKind::Mask);
            if (!shape || shape->empty()) {
                layers.resize(first);
                continue;
            }
        }
        if (map == MaterialMap::Metalness && !o.metallicMap.empty()) {
            own = images(o.metallicMap, OverlayImageKind::Mask);
            if (!own || own->empty()) {
                layers.resize(first);
                continue;
            }
        }
        // One placement for the overlay's layers: whatever of it this slot shows is moved,
        // turned and scaled as the whole group is.
        const Placement place = placementOf(o, images);
        for (std::size_t i = first; i < layers.size(); ++i) {
            layers[i].place = place;
            layers[i].shape = shape;
            layers[i].own = own;
        }
    }
    if (layers.empty()) {
        return nullptr;
    }

    // The canvas: the slot's own image, or — the zone having none — a neutral one the size of the
    // largest layer (white: the color is the material's Base Color, the roughness its Roughness;
    // mid grey: a level height; flat: an unperturbed normal).
    auto       out = std::make_shared<DecodedImage>();
    const bool hasBase = base && base->width > 0 && base->height > 0 &&
                         base->pixels.size() >= static_cast<std::size_t>(base->width) * base->height * 4;
    if (hasBase) {
        *out = *base;
    } else {
        for (const Layer& layer : layers) {
            for (const OverlayImage* image : {layer.image.get(), layer.shape.get(), layer.own.get()}) {
                if (image != nullptr) {
                    out->width = std::max(out->width, image->fullWidth);
                    out->height = std::max(out->height, image->fullHeight);
                }
            }
        }
        if (out->width == 0 || out->height == 0) {
            out->width = out->height = kFlatCanvas; // (flat layers alone: the color is the same everywhere)
        }
        const std::size_t texels = static_cast<std::size_t>(out->width) * out->height;
        out->pixels.resize(texels * 4);
        uint8_t fill[4] = {255, 255, 255, 255};
        if (map == MaterialMap::Bump || map == MaterialMap::Displacement) {
            fill[0] = fill[1] = fill[2] = 128;
        } else if (map == MaterialMap::Normal) {
            fill[0] = fill[1] = 128;
        }
        for (std::size_t i = 0; i < texels; ++i) {
            std::memcpy(out->pixels.data() + i * 4, fill, 4);
        }
    }

    if (map == MaterialMap::Metalness && baseScale != 1.0f) {
        // The zone's Metallic VALUE into the map: what is painted on is the absolute metalness.
        const std::size_t c = static_cast<std::size_t>(std::clamp(hasBase ? channel : 0, 0, 3));
        const float       scale = std::clamp(baseScale, 0.0f, 1.0f);
        forEachRow(0, out->height, static_cast<uint64_t>(out->width) * out->height, [&](uint32_t y) {
            uint8_t* row = out->pixels.data() + static_cast<std::size_t>(y) * out->width * 4;
            for (uint32_t x = 0; x < out->width; ++x) {
                uint8_t& d = row[static_cast<std::size_t>(x) * 4 + c];
                d = toByte(d * kInv255 * scale);
            }
        });
    }

    for (const Layer& layer : layers) {
        switch (map) {
        case MaterialMap::BaseColor: paintColor(*out, layer.image.get(), *layer.overlay, layer.place, layer.shape.get()); break;
        case MaterialMap::Roughness:
            paintAmount(*out, hasBase ? channel : 0, layer.image.get(), layer.overlay->gloss, layer.place, layer.shape.get(), nullptr);
            break;
        case MaterialMap::Metalness:
            paintAmount(*out, hasBase ? channel : 0, layer.image.get(), -layer.overlay->metallic, layer.place, layer.shape.get(), layer.own.get());
            break;
        case MaterialMap::Bump: paintHeight(*out, *layer.image, layer.overlay->bumpStrength, layer.place); break;
        case MaterialMap::Displacement: paintHeight(*out, *layer.image, layer.overlay->displacementStrength, layer.place); break;
        case MaterialMap::Normal:
            if (layer.heightAsNormal) {
                paintHeightAsNormal(*out, *layer.image, layer.overlay->bumpStrength, layer.place);
            } else {
                paintNormal(*out, *layer.image, layer.overlay->normalStrength, layer.place);
            }
            break;
        default: break;
        }
    }
    return out;
}

} // namespace pose
