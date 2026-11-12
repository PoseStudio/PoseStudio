/**
 * @file check_composite.cpp
 * @brief Checks of a render's Gaussian splat composite (render/splatcomposite.h) on synthetic
 *        layers and synthetic renders — no engine, no device, no Qt.
 *
 * A standalone guard for what the composite DOES to a picture: the display transform and its
 * inverse; the capture coming out of a render exactly as it went in; the backdrop behind a
 * capture that does not cover the pixel (flat, and a pixel's own); the render's coverage laying
 * the figure over the capture; the floor's shadow darkening the capture in LINEAR light; the
 * front of the capture over the figure; a transparent picture keeping its coverage; the linear
 * picture an OpenEXR file is saved from. Registered with CTest (`ctest -R splat-composite-check`).
 *
 * Exit code = number of failed checks (0 = green).
 */

#include "splatcomposite.h"

#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

using pose::RenderSplatLayers;
using pose::SplatComposite;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const char* name) {
    ++checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}

bool nearly(float a, float b, float tolerance) {
    return std::abs(a - b) <= tolerance;
}

/// A picture of one pixel: the layers' values, the render's, and what came out.
struct Pixel {
    float all[4] = {0, 0, 0, 0};
    float front[4] = {0, 0, 0, 0};
    float render[4] = {0, 0, 0, 0};
    std::vector<float> backdrop; // empty, or 3 values

    unsigned char out8[4] = {0, 0, 0, 0};
    float         linear[4] = {0, 0, 0, 0};

    void compose(const SplatComposite::View& view) {
        RenderSplatLayers layers;
        layers.width = 1;
        layers.height = 1;
        layers.all.assign(all, all + 4);
        layers.front.assign(front, front + 4);
        layers.backdrop = backdrop;
        const SplatComposite composite(std::move(layers), view);
        composite.compose(render, out8, 4, linear);
    }
};

unsigned char byteOf(float display) {
    return static_cast<unsigned char>(std::lround(std::fmin(std::fmax(display, 0.0f), 1.0f) * 255.0f));
}

void checkTransform() {
    bool roundTrip = true;
    bool monotone = true;
    for (int tonemap = 0; tonemap < 2; ++tonemap) {
        float last = -1.0f;
        for (int i = 0; i <= 100; ++i) {
            const float display = static_cast<float>(i) / 100.0f;
            const float linear = pose::displayDecode(display, tonemap != 0);
            roundTrip = roundTrip && nearly(pose::displayEncode(linear, tonemap != 0), display, 2.0e-4f);
            monotone = monotone && linear >= last;
            last = linear;
        }
    }
    check(roundTrip, "display(inverse(c)) = c over the whole range, with the tonemap and without");
    check(monotone, "the inverse rises with the display value");
    check(nearly(pose::displayEncode(0.0f, true), 0.0f, 1.0e-6f) && nearly(pose::displayDecode(0.0f, true), 0.0f, 1.0e-6f),
          "black stays black both ways");
    check(nearly(pose::displayEncode(0.18f, false), 0.4614f, 1.0e-3f), "without the tonemap the transform is the sRGB curve");
    check(pose::displayEncode(100.0f, true) <= 1.0f && pose::displayEncode(-1.0f, true) == 0.0f &&
              pose::displayEncode(std::nanf(""), false) == 0.0f,
          "the transform clamps what a display cannot show");
}

void checkPlate() {
    SplatComposite::View view; // tonemapped, exposure 1, a black backdrop
    // The capture covers the pixel, nothing of the scene does: it comes out as it went in.
    {
        Pixel p;
        const float capture[3] = {0.80f, 0.35f, 0.10f};
        for (int c = 0; c < 3; ++c) {
            p.all[c] = capture[c];
            p.front[c] = 0.0f;
        }
        p.all[3] = 1.0f;
        p.compose(view);
        // (To a level: the trip through linear light and back is exact to four decimals.)
        const auto same = [](unsigned char got, float display) {
            return std::abs(static_cast<int>(got) - static_cast<int>(byteOf(display))) <= 1;
        };
        check(same(p.out8[0], 0.80f) && same(p.out8[1], 0.35f) && same(p.out8[2], 0.10f) && p.out8[3] == 255,
              "a capture that covers the pixel comes out of the render as it went in");
        check(nearly(pose::displayEncode(p.linear[0], true), 0.80f, 1.0e-3f) && nearly(p.linear[3], 1.0f, 1.0e-6f),
              "… and its linear value shows as that color");
    }
    // No capture at the pixel: the flat backdrop, through the exposure and the display transform.
    {
        Pixel p;
        SplatComposite::View lit = view;
        lit.exposure = 0.5f;
        lit.backdropColor[0] = 0.4f;
        lit.backdropColor[1] = 0.2f;
        lit.backdropColor[2] = 0.1f;
        p.compose(lit);
        check(p.out8[0] == byteOf(pose::displayEncode(0.2f, true)) && p.out8[1] == byteOf(pose::displayEncode(0.1f, true)) &&
                  p.out8[2] == byteOf(pose::displayEncode(0.05f, true)),
              "with no capture at the pixel the flat backdrop shows, exposed and tonemapped");
        // … and a pixel's own backdrop (an environment) in its place.
        Pixel q;
        q.backdrop = {1.0f, 0.5f, 0.0f};
        q.compose(lit);
        check(q.out8[0] == byteOf(pose::displayEncode(0.5f, true)) && q.out8[1] == byteOf(pose::displayEncode(0.25f, true)) &&
                  q.out8[2] == 0,
              "a per-pixel backdrop replaces the flat one");
    }
    // Half covered: the capture over the backdrop, mixed as display values (the viewport's rule).
    {
        Pixel p;
        SplatComposite::View lit = view;
        lit.tonemap = false;
        lit.backdropColor[0] = lit.backdropColor[1] = lit.backdropColor[2] = pose::displayDecode(0.2f, false);
        p.all[0] = 0.5f * 0.9f; // premultiplied
        p.all[1] = 0.5f * 0.9f;
        p.all[2] = 0.5f * 0.9f;
        p.all[3] = 0.5f;
        p.compose(lit);
        check(std::abs(static_cast<int>(p.out8[0]) - static_cast<int>(byteOf(0.45f + 0.5f * 0.2f))) <= 1,
              "a half-covered pixel is the capture over the backdrop");
    }
}

void checkRenderOverPlate() {
    SplatComposite::View view;
    view.tonemap = false;
    const float captureDisplay = 0.6f;
    // The scene covers the pixel: the render's own color, whatever the capture behind it.
    {
        Pixel p;
        p.all[0] = p.all[1] = p.all[2] = captureDisplay;
        p.all[3] = 1.0f;
        p.render[0] = 0.30f;
        p.render[1] = 0.20f;
        p.render[2] = 0.10f;
        p.render[3] = 1.0f;
        p.compose(view);
        check(p.out8[0] == byteOf(pose::displayEncode(0.30f, false)) && p.out8[2] == byteOf(pose::displayEncode(0.10f, false)),
              "where the scene covers the pixel the render shows, not the capture behind it");
        check(nearly(p.linear[0], 0.30f, 1.0e-6f) && nearly(p.linear[1], 0.20f, 1.0e-6f),
              "… and the linear picture keeps the render's own values there");
    }
    // An edge: half the scene, half the capture — mixed in LINEAR light.
    {
        Pixel p;
        p.all[0] = p.all[1] = p.all[2] = captureDisplay;
        p.all[3] = 1.0f;
        p.render[0] = p.render[1] = p.render[2] = 0.5f * 0.8f; // premultiplied: a 0.8 surface at half coverage
        p.render[3] = 0.5f;
        p.compose(view);
        const float expect = pose::displayEncode(0.4f + 0.5f * pose::displayDecode(captureDisplay, false), false);
        check(std::abs(static_cast<int>(p.out8[0]) - static_cast<int>(byteOf(expect))) <= 1,
              "a figure's edge is antialiased against the capture, in linear light");
    }
    // A SHADOW: no color, coverage 0.5 — the capture under it at half its LIGHT, which is more
    // than half its display value.
    {
        Pixel p;
        p.all[0] = p.all[1] = p.all[2] = captureDisplay;
        p.all[3] = 1.0f;
        p.render[3] = 0.5f;
        p.compose(view);
        const float expect = pose::displayEncode(0.5f * pose::displayDecode(captureDisplay, false), false);
        check(std::abs(static_cast<int>(p.out8[0]) - static_cast<int>(byteOf(expect))) <= 1 && p.out8[0] > byteOf(0.5f * captureDisplay) + 10,
              "the floor's shadow halves the capture's light (not its display value)");
        check(p.out8[0] < byteOf(captureDisplay) - 20, "… and the capture is visibly darker in it");
    }
}

void checkFront() {
    SplatComposite::View view;
    view.tonemap = false;
    // The front of the capture covers the scene.
    {
        Pixel p;
        p.all[0] = p.front[0] = 0.9f;
        p.all[1] = p.front[1] = 0.1f;
        p.all[2] = p.front[2] = 0.1f;
        p.all[3] = p.front[3] = 1.0f;
        p.render[0] = p.render[1] = p.render[2] = 0.7f;
        p.render[3] = 1.0f;
        p.compose(view);
        check(p.out8[0] == byteOf(0.9f) && p.out8[1] == byteOf(0.1f), "a splat in front of the scene covers it");
        check(nearly(pose::displayEncode(p.linear[0], false), 0.9f, 2.0e-3f), "… in the linear picture too");
    }
    // Half in front: the front layer over the shown render, as display values.
    {
        Pixel p;
        p.front[0] = p.front[1] = p.front[2] = 0.5f * 1.0f; // white at half coverage
        p.front[3] = 0.5f;
        p.all[0] = p.all[1] = p.all[2] = 0.5f;
        p.all[3] = 0.5f;
        p.render[0] = p.render[1] = p.render[2] = pose::displayDecode(0.2f, false);
        p.render[3] = 1.0f;
        p.compose(view);
        check(std::abs(static_cast<int>(p.out8[0]) - static_cast<int>(byteOf(0.5f + 0.5f * 0.2f))) <= 1,
              "a half-covering splat in front is laid over the shown render");
    }
}

void checkTransparent() {
    SplatComposite::View view;
    view.tonemap = false;
    view.transparent = true;
    // Nothing anywhere: a clear pixel.
    {
        Pixel p;
        p.compose(view);
        check(p.out8[3] == 0 && nearly(p.linear[3], 0.0f, 1.0e-6f), "a transparent picture is clear where nothing is");
    }
    // The capture alone: its color at its coverage.
    {
        Pixel p;
        p.all[0] = 0.6f * 0.8f;
        p.all[1] = 0.6f * 0.4f;
        p.all[2] = 0.6f * 0.2f;
        p.all[3] = 0.6f;
        p.compose(view);
        check(std::abs(static_cast<int>(p.out8[3]) - static_cast<int>(byteOf(0.6f))) <= 1 &&
                  std::abs(static_cast<int>(p.out8[0]) - static_cast<int>(byteOf(0.8f))) <= 1 &&
                  std::abs(static_cast<int>(p.out8[2]) - static_cast<int>(byteOf(0.2f))) <= 1,
              "… keeps a capture's color (straight) at the capture's coverage");
        check(nearly(p.linear[3], 0.6f, 1.0e-4f) && nearly(p.linear[0], pose::displayDecode(0.8f, false) * 0.6f, 1.0e-3f),
              "… and its linear picture is premultiplied by that coverage");
    }
    // The scene alone at half coverage.
    {
        Pixel p;
        p.render[0] = 0.5f * 0.4f;
        p.render[1] = 0.5f * 0.4f;
        p.render[2] = 0.5f * 0.4f;
        p.render[3] = 0.5f;
        p.compose(view);
        check(std::abs(static_cast<int>(p.out8[3]) - 128) <= 1 &&
                  std::abs(static_cast<int>(p.out8[0]) - static_cast<int>(byteOf(pose::displayEncode(0.4f, false)))) <= 1,
              "… and the scene's own coverage where the capture is not");
    }
    // An opaque view never writes a coverage under one.
    {
        Pixel p;
        SplatComposite::View opaque;
        p.compose(opaque);
        check(p.out8[3] == 255 && nearly(p.linear[3], 1.0f, 1.0e-6f), "an opaque picture is opaque everywhere");
    }
}

void checkPicture() {
    // A whole picture, not one pixel: every pixel is its own (rows and columns are not mixed up).
    const int         w = 5;
    const int         h = 3;
    RenderSplatLayers layers;
    layers.width = w;
    layers.height = h;
    layers.all.assign(static_cast<std::size_t>(w * h * 4), 0.0f);
    layers.front.assign(static_cast<std::size_t>(w * h * 4), 0.0f);
    std::vector<float> render(static_cast<std::size_t>(w * h * 4), 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y * w + x) * 4u;
            const float       value = static_cast<float>(y * w + x + 1) / static_cast<float>(w * h + 1);
            layers.all[i] = layers.all[i + 1] = layers.all[i + 2] = value;
            layers.all[i + 3] = 1.0f;
        }
    }
    SplatComposite::View view;
    view.tonemap = false;
    const SplatComposite       composite(std::move(layers), view);
    const std::size_t          stride = static_cast<std::size_t>(w) * 4u + 8u; // (rows padded, as an image's may be)
    std::vector<unsigned char> out(stride * static_cast<std::size_t>(h), 0);
    composite.compose(render.data(), out.data(), stride, render.data()); // (in place, as the session does)
    bool ok = composite.width() == w && composite.height() == h;
    for (int y = 0; y < h && ok; ++y) {
        for (int x = 0; x < w && ok; ++x) {
            const float value = static_cast<float>(y * w + x + 1) / static_cast<float>(w * h + 1);
            ok = out[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4u] == byteOf(value);
        }
    }
    check(ok, "a whole picture: every pixel its own, rows of any stride, composited in place");
}

} // namespace

int main() {
    checkTransform();
    checkPlate();
    checkRenderOverPlate();
    checkFront();
    checkTransparent();
    checkPicture();
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures;
}
