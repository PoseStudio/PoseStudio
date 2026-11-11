/**
 * @file check_overlay.cpp
 * @brief Checks of the overlay compositor (scene/overlaycompositor.h) on synthetic images.
 *
 * A standalone guard for what an overlay DOES to a map: the blend modes and the bitmap
 * adjustments on known colors, the crop of an overlay image to where it does something, and
 * whole composites — a color image over a skin-colored map (by blend, opacity and alpha), the
 * gloss into the roughness map's channel, a height added about mid grey, a normal blended over a
 * flat one, a stack's order, an overlay of another resolution than the map, a zone that has
 * no map at all, and an overlay PLACED otherwise than it was painted: MOVED (its offset: every
 * element with it, whole texels, cut at the map's edge), TURNED (its rotation: clockwise on the
 * map about the group's own centre, a quarter turn exact, a normal image's slopes turned with
 * it) and SCALED (about the same centre, filtered where it shrinks) — and an overlay's MASK and
 * flat BASE COLOR: the mask as a crop, and as the OPACITY'S MAP — the shape of the color, the
 * gloss and the metal, never of the relief; a color and an opacity map with no picture (makeup);
 * an overlay as it is added, empty; the opacity value reaching the color alone; an overlay's
 * NAME — and its METAL: painted on the zone's absolute metalness (the Metallic value
 * × the map), through the overlay's shape, its mask and its own metallic map. It links scene/overlaycompositor.cpp alone — no Qt, no device — and is
 * registered with CTest (`ctest -R overlay-check`).
 *
 * Exit code = number of failed checks (0 = green).
 */

#include "overlaycompositor.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>

using pose::DecodedImage;
using pose::MaterialMap;
using pose::MaterialOverlay;
using pose::OverlayBlend;
using pose::OverlayImage;
using pose::OverlayImageKind;

namespace {

int failures = 0;

void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}

bool approx(float a, float b, float eps = 0.004f) {
    return std::abs(a - b) <= eps;
}
bool approx3(const glm::vec3& a, const glm::vec3& b, float eps = 0.004f) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
}

/// A w × h image filled with one RGBA value.
std::shared_ptr<DecodedImage> solid(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    auto image = std::make_shared<DecodedImage>();
    image->width = w;
    image->height = h;
    image->pixels.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        image->pixels[i * 4] = r;
        image->pixels[i * 4 + 1] = g;
        image->pixels[i * 4 + 2] = b;
        image->pixels[i * 4 + 3] = a;
    }
    return image;
}

/// Fills the block [x0, x1) × [y0, y1) of @p image.
void block(DecodedImage& image, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
            uint8_t* p = &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4];
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = a;
        }
    }
}

const uint8_t* at(const DecodedImage& image, uint32_t x, uint32_t y) {
    return &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4];
}
bool is(const DecodedImage& image, uint32_t x, uint32_t y, int r, int g, int b, int tolerance = 1) {
    const uint8_t* p = at(image, x, y);
    return std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance;
}

/// The "files" a test's overlays name: decoded images by path, cropped on demand as the app's
/// cache does.
struct Library {
    std::map<std::string, std::shared_ptr<DecodedImage>> files;

    pose::OverlayImageSource source() const {
        return [this](const std::string& path, OverlayImageKind kind) -> std::shared_ptr<const OverlayImage> {
            const auto it = files.find(path);
            return it == files.end() ? nullptr : pose::makeOverlayImage(*it->second, kind);
        };
    }
};

void checkBlends() {
    const glm::vec3 d(0.5f, 0.25f, 0.8f);
    const glm::vec3 s(0.5f, 0.5f, 0.25f);
    check(approx3(pose::blendOverlayColor(OverlayBlend::Normal, d, s), s), "blend: normal is the source");
    check(approx3(pose::blendOverlayColor(OverlayBlend::Multiply, d, s), d * s), "blend: multiply");
    check(approx3(pose::blendOverlayColor(OverlayBlend::Multiply, d, glm::vec3(1.0f)), d), "blend: multiply by white changes nothing");
    check(approx3(pose::blendOverlayColor(OverlayBlend::Screen, d, glm::vec3(0.0f)), d), "blend: screen with black changes nothing");
    check(approx3(pose::blendOverlayColor(OverlayBlend::Screen, d, s), glm::vec3(0.75f, 0.625f, 0.85f)), "blend: screen");
    check(approx3(pose::blendOverlayColor(OverlayBlend::Overlay, d, s), glm::vec3(0.5f, 0.25f, 0.7f)), "blend: overlay (multiply below mid, screen above)");
    check(approx3(pose::blendOverlayColor(OverlayBlend::SoftLight, d, glm::vec3(0.5f)), d), "blend: soft light with mid grey changes nothing");
    const glm::vec3 soft = pose::blendOverlayColor(OverlayBlend::SoftLight, d, glm::vec3(0.0f));
    check(soft.x < d.x && soft.y < d.y && soft.z < d.z && soft.x > 0.0f, "blend: soft light with black darkens, gently");
    check(approx3(pose::blendOverlayColor(OverlayBlend::Darken, d, s), glm::vec3(0.5f, 0.25f, 0.25f)), "blend: darken");
    check(approx3(pose::blendOverlayColor(OverlayBlend::Lighten, d, s), glm::vec3(0.5f, 0.5f, 0.8f)), "blend: lighten");
}

void checkAdjustments() {
    MaterialOverlay o;
    const glm::vec3 c(0.8f, 0.4f, 0.2f);
    check(approx3(pose::adjustOverlayColor(o, c), c, 1.0e-6f), "adjust: the defaults change nothing");
    o.tint = glm::vec3(0.7f, 0.1f, 0.3f);
    check(approx3(pose::adjustOverlayColor(o, glm::vec3(1.0f)), o.tint), "adjust: a white mask takes the tint whole (lipstick)");
    o = MaterialOverlay();
    o.brightness = 0.25f;
    check(approx3(pose::adjustOverlayColor(o, glm::vec3(0.25f)), glm::vec3(0.5f)), "adjust: brightness adds");
    o = MaterialOverlay();
    o.contrast = -1.0f;
    check(approx3(pose::adjustOverlayColor(o, c), glm::vec3(0.5f)), "adjust: contrast -100 is flat grey");
    o.contrast = 0.5f;
    const glm::vec3 hard = pose::adjustOverlayColor(o, glm::vec3(0.6f, 0.4f, 0.5f));
    check(hard.x > 0.6f && hard.y < 0.4f && approx(hard.z, 0.5f), "adjust: contrast spreads values about mid grey");
    o = MaterialOverlay();
    o.gamma = 2.0f;
    check(approx3(pose::adjustOverlayColor(o, glm::vec3(0.25f)), glm::vec3(0.5f)), "adjust: gamma 2 lifts the midtones (0.25 -> 0.5)");
    o = MaterialOverlay();
    o.hue = 120.0f;
    check(approx3(pose::adjustOverlayColor(o, glm::vec3(1.0f, 0.0f, 0.0f)), glm::vec3(0.0f, 1.0f, 0.0f)), "adjust: hue +120 turns red to green");
    o = MaterialOverlay();
    o.saturation = -1.0f;
    const glm::vec3 grey = pose::adjustOverlayColor(o, glm::vec3(1.0f, 0.0f, 0.0f));
    check(approx(grey.x, grey.y) && approx(grey.y, grey.z), "adjust: saturation -100 is grey");
    // The order: tone, THEN tint — black ink brightened and tinted is a dark ink of that color.
    o = MaterialOverlay();
    o.brightness = 0.4f;
    o.tint = glm::vec3(0.0f, 0.0f, 1.0f);
    check(approx3(pose::adjustOverlayColor(o, glm::vec3(0.0f)), glm::vec3(0.0f, 0.0f, 0.4f)), "adjust: tone before tint (black ink made blue)");
}

void checkCrop() {
    auto image = solid(64, 48, 0, 0, 0, 0);
    block(*image, 20, 30, 30, 36, 255, 0, 0, 255);
    const auto crop = pose::makeOverlayImage(*image, OverlayImageKind::Color);
    check(crop->fullWidth == 64 && crop->fullHeight == 48 && crop->x == 20 && crop->y == 30 && crop->width == 10 && crop->height == 6 &&
              crop->hasAlpha && crop->rgba.size() == 10u * 6u * 4u && crop->rgba[0] == 255 && crop->rgba[3] == 255,
          "crop: a transparent image is kept as its alpha's rectangle");
    check(pose::makeOverlayImage(*solid(16, 16, 9, 9, 9, 0), OverlayImageKind::Color)->empty(), "crop: a fully transparent image is empty");
    const auto whole = pose::makeOverlayImage(*solid(16, 8, 9, 9, 9, 255), OverlayImageKind::Color);
    check(whole->width == 16 && whole->height == 8 && !whole->hasAlpha, "crop: an opaque color image is kept whole");
    auto height = solid(32, 32, 128, 128, 128, 255);
    block(*height, 4, 5, 9, 11, 200, 200, 200, 255);
    const auto ridge = pose::makeOverlayImage(*height, OverlayImageKind::Height);
    check(ridge->x == 4 && ridge->y == 5 && ridge->width == 5 && ridge->height == 6, "crop: an opaque height is kept where it leaves mid grey");
    check(pose::makeOverlayImage(*solid(16, 16, 127, 127, 127, 255), OverlayImageKind::Height)->empty(), "crop: a level height is empty");
    auto normal = solid(32, 32, 128, 128, 255, 255);
    block(*normal, 10, 12, 14, 13, 190, 128, 230, 255);
    const auto slope = pose::makeOverlayImage(*normal, OverlayImageKind::Normal);
    check(slope->x == 10 && slope->y == 12 && slope->width == 4 && slope->height == 1, "crop: an opaque normal is kept where it is not flat");
}

void checkColorComposite() {
    Library library;
    auto    ink = solid(64, 64, 0, 0, 0, 0);
    block(*ink, 16, 16, 32, 32, 0, 0, 0, 255);     // black, opaque
    block(*ink, 32, 16, 48, 32, 255, 255, 255, 255); // white, opaque
    block(*ink, 16, 32, 32, 48, 255, 0, 0, 128);   // red, half transparent
    library.files["ink.png"] = ink;
    const std::shared_ptr<const DecodedImage> skin = solid(64, 64, 200, 150, 120, 77);

    MaterialOverlay tattoo;
    tattoo.color = "ink.png";
    auto out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {tattoo}, library.source());
    check(out && out->width == 64 && out->height == 64, "color: a composite the map's size");
    if (!out) {
        return;
    }
    check(is(*out, 20, 20, 0, 0, 0), "color: black ink multiplies the skin to black");
    check(is(*out, 40, 20, 200, 150, 120), "color: white multiplies to the skin itself (a tattoo's highlights are skin)");
    check(is(*out, 20, 40, 200, 75, 60, 2), "color: half-transparent red, multiplied, half way");
    check(is(*out, 5, 5, 200, 150, 120, 0) && is(*out, 60, 60, 200, 150, 120, 0), "color: untouched outside the image's alpha");
    check(at(*out, 20, 20)[3] == 77 && at(*out, 5, 5)[3] == 77, "color: the map's own alpha is left alone");
    check(is(*skin, 20, 20, 200, 150, 120, 0), "color: the map itself is never written");

    tattoo.opacity = 0.5f;
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {tattoo}, library.source());
    check(out && is(*out, 20, 20, 100, 75, 60), "color: opacity 50 mixes half way");
    tattoo.opacity = 1.0f;
    tattoo.blend = OverlayBlend::Normal;
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {tattoo}, library.source());
    check(out && is(*out, 40, 20, 255, 255, 255) && is(*out, 20, 20, 0, 0, 0), "color: normal covers (white stays white)");
    tattoo.tint = glm::vec3(0.0f, 0.5f, 1.0f);
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {tattoo}, library.source());
    check(out && is(*out, 40, 20, 0, 128, 255), "color: the tint colors the image");

    // The stack: the later overlay is on top.
    auto red = solid(64, 64, 255, 0, 0, 255);
    auto blue = solid(64, 64, 0, 0, 255, 255);
    library.files["red.png"] = red;
    library.files["blue.png"] = blue;
    MaterialOverlay a;
    a.color = "red.png";
    a.blend = OverlayBlend::Normal;
    MaterialOverlay b = a;
    b.color = "blue.png";
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {a, b}, library.source());
    check(out && is(*out, 10, 10, 0, 0, 255), "stack: the last overlay is on top");
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {b, a}, library.source());
    check(out && is(*out, 10, 10, 255, 0, 0), "stack: ... and the order decides");
    b.enabled = false;
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {a, b}, library.source());
    check(out && is(*out, 10, 10, 255, 0, 0), "stack: a disabled overlay paints nothing");
    a.enabled = false;
    check(!pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {a, b}, library.source()), "stack: nothing enabled, no composite");
    MaterialOverlay missing;
    missing.color = "gone.png";
    check(!pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {missing}, library.source()), "a missing image is left out (no composite)");
    check(!pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {}, library.source()), "an empty stack makes no composite");

    // A zone without a base-color map: a white canvas the overlay's size.
    tattoo = MaterialOverlay();
    tattoo.color = "ink.png";
    out = pose::compositeOverlays(MaterialMap::BaseColor, nullptr, 0, {tattoo}, library.source());
    check(out && out->width == 64 && out->height == 64 && is(*out, 5, 5, 255, 255, 255, 0) && is(*out, 20, 20, 0, 0, 0) && at(*out, 5, 5)[3] == 255,
          "no map: the overlay is painted on a white canvas of its own size");

    // An overlay of another resolution than the map: it lands on the same part of the surface.
    auto big = solid(128, 128, 0, 0, 0, 0);
    block(*big, 64, 0, 128, 64, 0, 0, 0, 255); // the top right quarter, black
    library.files["big.png"] = big;
    MaterialOverlay quarter;
    quarter.color = "big.png";
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {quarter}, library.source());
    check(out && out->width == 64 && is(*out, 48, 16, 0, 0, 0) && is(*out, 16, 16, 200, 150, 120, 0) && is(*out, 48, 48, 200, 150, 120, 0),
          "size: a larger overlay is mapped onto the map's texels");
}

void checkGloss() {
    Library library;
    auto    lips = solid(32, 32, 255, 255, 255, 0);
    block(*lips, 8, 8, 24, 24, 255, 255, 255, 255);
    library.files["lips.png"] = lips;
    // A packed map: the roughness rides in GREEN here; red and blue are other things.
    const std::shared_ptr<const DecodedImage> rough = solid(32, 32, 10, 200, 30, 255);
    MaterialOverlay gloss;
    gloss.color = "lips.png";
    gloss.opacity = 0.2f; // (sheer color: the gloss does not follow the opacity)
    check(!pose::compositeOverlays(MaterialMap::Roughness, rough, 1, {gloss}, library.source()), "gloss: 0 leaves the roughness map alone");
    gloss.gloss = 0.5f;
    auto out = pose::compositeOverlays(MaterialMap::Roughness, rough, 1, {gloss}, library.source());
    check(out && at(*out, 16, 16)[1] == 100 && at(*out, 2, 2)[1] == 200, "gloss: +50 halves the roughness under the image, nowhere else");
    check(out && at(*out, 16, 16)[0] == 10 && at(*out, 16, 16)[2] == 30, "gloss: the map's other channels are left alone");
    gloss.gloss = -1.0f;
    out = pose::compositeOverlays(MaterialMap::Roughness, rough, 1, {gloss}, library.source());
    check(out && at(*out, 16, 16)[1] == 255, "gloss: -100 is as matte as the material allows");
    gloss.gloss = 1.0f;
    out = pose::compositeOverlays(MaterialMap::Roughness, nullptr, 3, {gloss}, library.source());
    check(out && at(*out, 16, 16)[0] == 0 && at(*out, 2, 2)[0] == 255, "gloss: without a roughness map, a white canvas with the gloss in red");
}

void checkRelief() {
    Library library;
    auto    ridge = solid(32, 32, 0, 0, 0, 0);
    block(*ridge, 8, 8, 16, 16, 255, 255, 255, 255); // a rise
    block(*ridge, 16, 8, 24, 16, 128, 128, 128, 255); // level
    library.files["ridge.png"] = ridge;
    const std::shared_ptr<const DecodedImage> bump = solid(32, 32, 100, 100, 100, 255);
    MaterialOverlay scar;
    scar.bump = "ridge.png";
    auto out = pose::compositeOverlays(MaterialMap::Bump, bump, 0, {scar}, library.source());
    check(out && is(*out, 10, 10, 228, 228, 228, 1), "bump: white adds half the range to the height");
    check(out && is(*out, 20, 10, 100, 100, 100, 0) && is(*out, 2, 2, 100, 100, 100, 0), "bump: mid grey and the outside change nothing");
    scar.bumpStrength = -1.0f;
    out = pose::compositeOverlays(MaterialMap::Bump, bump, 0, {scar}, library.source());
    check(out && is(*out, 10, 10, 0, 0, 0, 0), "bump: a negative strength sinks instead (clamped at black)");
    scar.bumpStrength = 0.25f;
    out = pose::compositeOverlays(MaterialMap::Bump, nullptr, 0, {scar}, library.source());
    check(out && is(*out, 2, 2, 128, 128, 128, 0) && is(*out, 10, 10, 160, 160, 160, 1), "bump: without a bump map, a mid-grey canvas");
    check(!pose::compositeOverlays(MaterialMap::BaseColor, bump, 0, {scar}, library.source()), "bump: a relief-only overlay paints no color");
    scar.displacement = "ridge.png";
    out = pose::compositeOverlays(MaterialMap::Displacement, bump, 0, {scar}, library.source());
    check(out && is(*out, 10, 10, 228, 228, 228, 1), "displacement: composited like a height, for an export");

    auto slope = solid(32, 32, 128, 128, 255, 0);
    block(*slope, 8, 8, 16, 16, 218, 128, 218, 255); // tilted toward +x
    library.files["slope.png"] = slope;
    const std::shared_ptr<const DecodedImage> flat = solid(32, 32, 128, 128, 255, 255);
    MaterialOverlay groove;
    groove.normal = "slope.png";
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {groove}, library.source());
    bool unit = false;
    if (out) {
        const uint8_t* p = at(*out, 10, 10);
        const glm::vec3 n(p[0] / 255.0f * 2.0f - 1.0f, p[1] / 255.0f * 2.0f - 1.0f, p[2] / 255.0f * 2.0f - 1.0f);
        unit = std::abs(glm::length(n) - 1.0f) < 0.02f && n.x > 0.5f && std::abs(n.y) < 0.02f && n.z > 0.5f;
    }
    check(out && unit, "normal: the overlay's slope over a flat map, a unit normal");
    check(out && is(*out, 2, 2, 128, 128, 255, 0), "normal: flat outside the image");
    out = pose::compositeOverlays(MaterialMap::Normal, nullptr, 0, {groove}, library.source());
    check(out && is(*out, 2, 2, 128, 128, 255, 0), "normal: without a normal map, a flat canvas");

    // THE VIEWPORT'S normal map carries a bump image as the slope of its height: where the height
    // rises going right the normal leans left, where it falls it leans right, flat on the level
    // top and outside; an export (the flag off) paints no normal from a bump.
    MaterialOverlay raised;
    raised.bump = "ridge.png";
    check(!pose::compositeOverlays(MaterialMap::Normal, flat, 0, {raised}, library.source()), "bump: no part of an export's normal map");
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {raised}, library.source(), /*bumpAsNormal=*/true);
    check(out && at(*out, 8, 12)[0] < 126 && at(*out, 16, 12)[0] > 130, "bump as normal: the slope leans away from the rise, on both sides");
    check(out && is(*out, 12, 12, 128, 128, 255, 1) && is(*out, 2, 2, 128, 128, 255, 0) && is(*out, 28, 28, 128, 128, 255, 0),
          "bump as normal: flat on the level top and outside");
    check(out && at(*out, 12, 8)[1] < 126 && at(*out, 12, 16)[1] > 130, "bump as normal: ... and along the image's rows (v runs down)");
    raised.bumpStrength = -1.0f;
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {raised}, library.source(), true);
    check(out && at(*out, 8, 12)[0] > 130 && at(*out, 16, 12)[0] < 126, "bump as normal: a negative strength is a groove");
    raised.bumpStrength = 1.0f;
    raised.normal = "slope.png";
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {raised}, library.source(), true);
    check(out && at(*out, 10, 10)[0] > 190, "bump as normal: an overlay's own normal image is painted with it");
}

void checkOffset() {
    Library library;
    auto    ink = solid(64, 64, 0, 0, 0, 0);
    block(*ink, 16, 16, 32, 32, 0, 0, 0, 255); // a black square, painted at (16..32, 16..32)
    library.files["ink.png"] = ink;
    const std::shared_ptr<const DecodedImage> skin = solid(64, 64, 200, 150, 120, 255);
    const auto black = [](const DecodedImage& image, uint32_t x, uint32_t y) { return is(image, x, y, 0, 0, 0, 0); };
    const auto bare = [](const DecodedImage& image, uint32_t x, uint32_t y) { return is(image, x, y, 200, 150, 120, 0); };

    MaterialOverlay tattoo;
    tattoo.color = "ink.png";
    tattoo.offset = glm::vec2(0.25f, 0.0f); // 16 texels to the right
    auto out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {tattoo}, library.source());
    check(out && black(*out, 32, 16) && black(*out, 47, 31) && bare(*out, 31, 20) && bare(*out, 48, 20) && bare(*out, 20, 20),
          "offset: +x moves the overlay to the right, and it is gone from where it was painted");
    tattoo.offset = glm::vec2(0.0f, 0.25f); // 16 texels UP the map (toward row 0)
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {tattoo}, library.source());
    check(out && black(*out, 16, 0) && black(*out, 31, 15) && bare(*out, 20, 16) && bare(*out, 20, 20), "offset: +y moves it UP the map");
    tattoo.offset = glm::vec2(-0.125f, -0.25f);
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {tattoo}, library.source());
    check(out && black(*out, 8, 32) && black(*out, 23, 47) && bare(*out, 7, 40) && bare(*out, 24, 40) && bare(*out, 16, 31) && bare(*out, 16, 48),
          "offset: negative moves it left and down, both at once");

    // What runs off the map is cut: it does not come back in on the other side.
    tattoo.offset = glm::vec2(0.625f, 0.0f); // the square at 56..72: half of it is off the map
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {tattoo}, library.source());
    check(out && black(*out, 56, 20) && black(*out, 63, 20) && bare(*out, 55, 20) && bare(*out, 0, 20) && bare(*out, 7, 20),
          "offset: cut at the map's right edge, no wrap round to the left");
    tattoo.offset = glm::vec2(-0.375f, 0.375f); // at -8..8, -8..8: the corner quarter stays
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {tattoo}, library.source());
    check(out && black(*out, 0, 0) && black(*out, 7, 7) && bare(*out, 8, 4) && bare(*out, 4, 8) && bare(*out, 63, 63) && bare(*out, 60, 4),
          "offset: cut at the left and top edges");
    tattoo.offset = glm::vec2(1.0f, 0.0f);
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {tattoo}, library.source());
    bool untouched = out != nullptr;
    for (uint32_t y = 0; out && y < 64; y += 3) {
        for (uint32_t x = 0; x < 64; x += 3) {
            untouched = untouched && bare(*out, x, y);
        }
    }
    check(untouched, "offset: moved a whole map away, nothing of it is left");

    // Whole texels: under half a texel is no move, over half is one.
    tattoo.offset = glm::vec2(0.005f, 0.0f); // 0.32 of a texel
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {tattoo}, library.source());
    check(out && black(*out, 16, 20) && bare(*out, 32, 20), "offset: less than half a texel shows nothing");
    tattoo.offset = glm::vec2(0.01f, 0.0f); // 0.64 of a texel
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {tattoo}, library.source());
    check(out && bare(*out, 16, 20) && black(*out, 17, 20) && black(*out, 32, 20) && bare(*out, 33, 20), "offset: ... and more than half moves one texel");

    // An overlay of another resolution: the offset is a share of the MAP, so it lands the same.
    auto big = solid(128, 128, 0, 0, 0, 0);
    block(*big, 64, 0, 128, 64, 0, 0, 0, 255); // the top right quarter
    library.files["big.png"] = big;
    MaterialOverlay quarter;
    quarter.color = "big.png";
    quarter.offset = glm::vec2(-0.5f, -0.5f); // to the bottom left quarter
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {quarter}, library.source());
    check(out && black(*out, 16, 48) && black(*out, 4, 36) && black(*out, 28, 60) && bare(*out, 48, 16) && bare(*out, 16, 16) && bare(*out, 48, 48),
          "offset: a larger overlay moves by the same share of the map");
    // ... and on a canvas (a zone with no map): the canvas is the overlay's size, the move its share.
    out = pose::compositeOverlays(MaterialMap::BaseColor, nullptr, 0, {quarter}, library.source());
    check(out && out->width == 128 && is(*out, 32, 96, 0, 0, 0, 0) && is(*out, 96, 32, 255, 255, 255, 0), "offset: on a canvas of the overlay's own size");

    // The whole group moves: the gloss, the height, the normal, the bump drawn as a normal.
    auto lips = solid(64, 64, 255, 255, 255, 0);
    block(*lips, 16, 16, 32, 32, 255, 255, 255, 255);
    library.files["lips.png"] = lips;
    const std::shared_ptr<const DecodedImage> rough = solid(64, 64, 200, 200, 200, 255);
    MaterialOverlay gloss;
    gloss.color = "lips.png";
    gloss.gloss = 0.5f;
    gloss.offset = glm::vec2(0.25f, -0.25f);
    out = pose::compositeOverlays(MaterialMap::Roughness, rough, 0, {gloss}, library.source());
    check(out && at(*out, 40, 40)[0] == 100 && at(*out, 20, 20)[0] == 200 && at(*out, 40, 20)[0] == 200, "offset: the gloss moves with the color");

    auto ridge = solid(64, 64, 0, 0, 0, 0);
    block(*ridge, 16, 16, 32, 32, 255, 255, 255, 255);
    library.files["ridge.png"] = ridge;
    const std::shared_ptr<const DecodedImage> level = solid(64, 64, 100, 100, 100, 255);
    MaterialOverlay scar;
    scar.bump = "ridge.png";
    scar.displacement = "ridge.png";
    scar.offset = glm::vec2(0.25f, -0.25f);
    out = pose::compositeOverlays(MaterialMap::Bump, level, 0, {scar}, library.source());
    check(out && is(*out, 40, 40, 228, 228, 228, 1) && is(*out, 20, 20, 100, 100, 100, 0), "offset: the bump moves");
    out = pose::compositeOverlays(MaterialMap::Displacement, level, 0, {scar}, library.source());
    check(out && is(*out, 40, 40, 228, 228, 228, 1) && is(*out, 20, 20, 100, 100, 100, 0), "offset: the displacement moves");
    const std::shared_ptr<const DecodedImage> flat = solid(64, 64, 128, 128, 255, 255);
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {scar}, library.source(), /*bumpAsNormal=*/true);
    check(out && at(*out, 32, 40)[0] < 126 && at(*out, 48, 40)[0] > 130 && is(*out, 40, 40, 128, 128, 255, 1) && is(*out, 16, 24, 128, 128, 255, 0) &&
              is(*out, 32, 24, 128, 128, 255, 0),
          "offset: the bump drawn as a normal has its slopes at the moved edges");
    auto slope = solid(64, 64, 128, 128, 255, 0);
    block(*slope, 16, 16, 32, 32, 218, 128, 218, 255);
    library.files["slope.png"] = slope;
    MaterialOverlay groove;
    groove.normal = "slope.png";
    groove.offset = glm::vec2(0.25f, -0.25f);
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {groove}, library.source());
    check(out && at(*out, 40, 40)[0] > 190 && is(*out, 20, 20, 128, 128, 255, 0), "offset: the normal moves");

    // The field, in the tab's units: percent of the map, a whole map either way at most.
    MaterialOverlay o;
    pose::setOverlayField(o, pose::OverlayField::OffsetX, 12.5f);
    pose::setOverlayField(o, pose::OverlayField::OffsetY, -250.0f);
    pose::OverlayField byName = pose::OverlayField::Count;
    check(approx(o.offset.x, 0.125f, 1.0e-6f) && approx(o.offset.y, -1.0f, 1.0e-6f) &&
              approx(pose::overlayFieldValue(o, pose::OverlayField::OffsetX), 12.5f, 1.0e-4f) &&
              pose::overlayFieldByName("offsetx", byName) && byName == pose::OverlayField::OffsetX &&
              pose::overlayFieldByName("offsety", byName) && byName == pose::OverlayField::OffsetY && !(o == MaterialOverlay()),
          "offset: the fields read percent of the map, clamped at a whole map");
}

/// Four marks at the middles of the sides of the square 16..48 of a @p size square image (64, or
/// a multiple): red on top, green on the right, blue below, yellow on the left. Their bounding box
/// is that square, so the image turns and scales about its middle.
std::shared_ptr<DecodedImage> fourMarks(uint32_t size = 64) {
    const uint32_t k = size / 64;
    auto           image = solid(size, size, 0, 0, 0, 0);
    block(*image, 30 * k, 16 * k, 34 * k, 20 * k, 255, 0, 0, 255);
    block(*image, 44 * k, 30 * k, 48 * k, 34 * k, 0, 255, 0, 255);
    block(*image, 30 * k, 44 * k, 34 * k, 48 * k, 0, 0, 255, 255);
    block(*image, 16 * k, 30 * k, 20 * k, 34 * k, 255, 255, 0, 255);
    return image;
}

/// How much of @p image is painted black over the test skin, in texels (its red channel's fall).
float inked(const DecodedImage& image) {
    float sum = 0.0f;
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            sum += (200.0f - static_cast<float>(at(image, x, y)[0])) / 200.0f;
        }
    }
    return sum;
}

void checkTurn() {
    Library library;
    library.files["marks.png"] = fourMarks();
    const std::shared_ptr<const DecodedImage> skin = solid(64, 64, 200, 150, 120, 255);
    const auto bare = [](const DecodedImage& image, uint32_t x, uint32_t y) { return is(image, x, y, 200, 150, 120, 0); };
    const auto red = [](const DecodedImage& image, uint32_t x, uint32_t y) { return is(image, x, y, 255, 0, 0, 0); };
    const auto green = [](const DecodedImage& image, uint32_t x, uint32_t y) { return is(image, x, y, 0, 255, 0, 0); };
    const auto blue = [](const DecodedImage& image, uint32_t x, uint32_t y) { return is(image, x, y, 0, 0, 255, 0); };
    const auto yellow = [](const DecodedImage& image, uint32_t x, uint32_t y) { return is(image, x, y, 255, 255, 0, 0); };

    MaterialOverlay marks;
    marks.color = "marks.png";
    marks.blend = OverlayBlend::Normal;
    marks.rotation = 90.0f;
    auto out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {marks}, library.source());
    check(out && red(*out, 46, 32) && green(*out, 32, 46) && blue(*out, 18, 32) && yellow(*out, 32, 18),
          "turn: +90 is clockwise on the map (the top mark goes to the right)");
    check(out && red(*out, 44, 30) && red(*out, 47, 33) && bare(*out, 43, 32) && bare(*out, 48, 32) && bare(*out, 46, 29) && bare(*out, 46, 34) &&
              bare(*out, 32, 32),
          "turn: a quarter turn lands texel on texel (no blur, no spill)");
    marks.rotation = -90.0f;
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {marks}, library.source());
    check(out && red(*out, 18, 32) && green(*out, 32, 18) && blue(*out, 46, 32) && yellow(*out, 32, 46), "turn: -90 is counter-clockwise");
    marks.rotation = 180.0f;
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {marks}, library.source());
    check(out && red(*out, 32, 46) && green(*out, 18, 32) && blue(*out, 32, 18) && yellow(*out, 46, 32) && red(*out, 30, 44) && red(*out, 33, 47),
          "turn: 180 is upside down, exactly");

    // Turned about its own centre, THEN moved.
    marks.rotation = 90.0f;
    marks.offset = glm::vec2(0.125f, -0.125f); // 8 texels right, 8 down
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {marks}, library.source());
    check(out && red(*out, 54, 40) && green(*out, 40, 54) && blue(*out, 26, 40) && yellow(*out, 40, 26) && bare(*out, 46, 32),
          "turn: the offset moves the turned overlay");

    // An overlay of twice the map's resolution: the same quarter turn, each texel the mean of four.
    library.files["marks2.png"] = fourMarks(128);
    MaterialOverlay large;
    large.color = "marks2.png";
    large.blend = OverlayBlend::Normal;
    large.rotation = 90.0f;
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {large}, library.source());
    check(out && out->width == 64 && red(*out, 46, 32) && red(*out, 44, 30) && green(*out, 32, 46) && bare(*out, 43, 32) && bare(*out, 48, 32),
          "turn: a larger overlay turned onto the map's texels");

    // Any other angle is resampled: the same area, a filtered edge, the corners gone.
    auto square = solid(64, 64, 0, 0, 0, 0);
    block(*square, 24, 24, 40, 40, 0, 0, 0, 255);
    library.files["square.png"] = square;
    MaterialOverlay diamond;
    diamond.color = "square.png";
    diamond.blend = OverlayBlend::Normal;
    diamond.rotation = 45.0f;
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {diamond}, library.source());
    int partial = 0;
    for (uint32_t y = 16; out && y < 48; ++y) {
        for (uint32_t x = 16; x < 48; ++x) {
            const int r = at(*out, x, y)[0];
            partial += (r > 10 && r < 190) ? 1 : 0;
        }
    }
    check(out && is(*out, 32, 32, 0, 0, 0, 0) && is(*out, 32, 22, 0, 0, 0, 1) && is(*out, 41, 32, 0, 0, 0, 1) && bare(*out, 25, 25) &&
              bare(*out, 38, 38) && bare(*out, 32, 19),
          "turn: 45 degrees makes the square a diamond (points out, corners gone)");
    check(out && partial > 8 && std::abs(inked(*out) - 256.0f) < 6.0f, "turn: ... of the same area, its edge filtered");

    // ONE centre for the group: a ridge painted to the right of the color's centre swings below it.
    auto patch = solid(64, 64, 0, 0, 0, 0);
    block(*patch, 16, 16, 32, 32, 0, 0, 0, 255); // the color: centred on (24, 24)
    auto ridge = solid(64, 64, 0, 0, 0, 0);
    block(*ridge, 40, 20, 48, 28, 255, 255, 255, 255); // a rise centred on (44, 24)
    library.files["patch.png"] = patch;
    library.files["ridge.png"] = ridge;
    const std::shared_ptr<const DecodedImage> level = solid(64, 64, 100, 100, 100, 255);
    MaterialOverlay scar;
    scar.color = "patch.png";
    scar.bump = "ridge.png";
    scar.rotation = 90.0f;
    out = pose::compositeOverlays(MaterialMap::Bump, level, 0, {scar}, library.source());
    check(out && is(*out, 24, 44, 228, 228, 228, 1) && is(*out, 20, 40, 228, 228, 228, 1) && is(*out, 27, 47, 228, 228, 228, 1) &&
              is(*out, 44, 24, 100, 100, 100, 0) && is(*out, 28, 44, 100, 100, 100, 0),
          "turn: the relief turns about the COLOR's centre (the group stays registered)");
    scar.color.clear(); // relief only: its own image is the anchor
    out = pose::compositeOverlays(MaterialMap::Bump, level, 0, {scar}, library.source());
    check(out && is(*out, 44, 24, 228, 228, 228, 1) && is(*out, 24, 44, 100, 100, 100, 0), "turn: a relief-only overlay turns about its own centre");

    // A NORMAL image's slopes turn with it: leaning right, then (a quarter turn clockwise) down.
    auto slope = solid(64, 64, 128, 128, 255, 0);
    block(*slope, 16, 16, 32, 32, 218, 128, 218, 255);
    library.files["slope.png"] = slope;
    const std::shared_ptr<const DecodedImage> flat = solid(64, 64, 128, 128, 255, 255);
    MaterialOverlay groove;
    groove.normal = "slope.png";
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {groove}, library.source());
    check(out && at(*out, 24, 24)[0] > 190 && std::abs(at(*out, 24, 24)[1] - 128) <= 1, "turn: (as painted, the normal leans right)");
    groove.rotation = 90.0f;
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {groove}, library.source());
    check(out && at(*out, 24, 24)[1] > 190 && std::abs(at(*out, 24, 24)[0] - 128) <= 1, "turn: a quarter turn clockwise, it leans down the map");
    groove.rotation = 180.0f;
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {groove}, library.source());
    check(out && at(*out, 24, 24)[0] < 66 && std::abs(at(*out, 24, 24)[1] - 128) <= 1, "turn: half a turn, it leans left");

    // The two reliefs agree on which way a turn goes: a height rising to the right (its normal
    // leaning LEFT) and a normal image leaning left, both turned 30 degrees, lean the same way.
    auto ramp = solid(64, 64, 128, 128, 128, 255);
    for (uint32_t x = 16; x < 48; ++x) {
        const uint8_t v = static_cast<uint8_t>(130 + (x - 16) * 4);
        block(*ramp, x, 16, x + 1, 48, v, v, v, 255);
    }
    auto lean = solid(64, 64, 128, 128, 255, 255);
    block(*lean, 16, 16, 48, 48, 60, 128, 236, 255);
    library.files["ramp.png"] = ramp;
    library.files["lean.png"] = lean;
    MaterialOverlay rise;
    rise.bump = "ramp.png";
    rise.bumpStrength = 40.0f;
    rise.rotation = 30.0f;
    MaterialOverlay leaning;
    leaning.normal = "lean.png";
    leaning.rotation = 30.0f;
    const auto direction = [](const DecodedImage& image) {
        const uint8_t* p = at(image, 32, 32);
        return glm::normalize(glm::vec2(static_cast<float>(p[0]) - 127.5f, static_cast<float>(p[1]) - 127.5f));
    };
    const glm::vec2 expected(-0.8660f, -0.5f); // (-1, 0) turned 30 degrees clockwise, rows down
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {rise}, library.source(), /*bumpAsNormal=*/true);
    const bool bumpTurned = out && glm::length(direction(*out) - expected) < 0.12f;
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {leaning}, library.source());
    const bool normalTurned = out && glm::length(direction(*out) - expected) < 0.12f;
    check(bumpTurned && normalTurned, "turn: a bump image and a normal image turn the same way");

    // The fields, and what a turn re-paints.
    MaterialOverlay o;
    pose::setOverlayField(o, pose::OverlayField::Rotation, 37.5f);
    pose::OverlayField byName = pose::OverlayField::Count;
    const bool set = o.rotation == 37.5f && pose::overlayFieldByName("rotation", byName) && byName == pose::OverlayField::Rotation;
    pose::setOverlayField(o, pose::OverlayField::Rotation, -500.0f);
    check(set && o.rotation == -180.0f && !(o == MaterialOverlay()), "turn: the field reads degrees, half a turn either way at most");
}

void checkScale() {
    Library library;
    const std::shared_ptr<const DecodedImage> skin = solid(64, 64, 200, 150, 120, 255);
    const auto bare = [](const DecodedImage& image, uint32_t x, uint32_t y) { return is(image, x, y, 200, 150, 120, 0); };
    const auto black = [](const DecodedImage& image, uint32_t x, uint32_t y) { return is(image, x, y, 0, 0, 0, 0); };

    auto square = solid(64, 64, 0, 0, 0, 0);
    block(*square, 24, 24, 40, 40, 0, 0, 0, 255); // 16 texels a side, centred on (32, 32)
    library.files["square.png"] = square;
    MaterialOverlay o;
    o.color = "square.png";
    o.blend = OverlayBlend::Normal;
    o.scale = 2.0f;
    auto out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {o}, library.source());
    check(out && black(*out, 17, 17) && black(*out, 46, 46) && black(*out, 32, 32) && bare(*out, 14, 32) && bare(*out, 49, 32) && bare(*out, 32, 14) &&
              std::abs(inked(*out) - 1024.0f) < 12.0f,
          "scale: 200 doubles the overlay about its own centre (four times the area)");
    o.scale = 0.5f;
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {o}, library.source());
    check(out && black(*out, 28, 28) && black(*out, 35, 35) && bare(*out, 27, 32) && bare(*out, 36, 32) && bare(*out, 32, 27) && bare(*out, 32, 36) &&
              std::abs(inked(*out) - 64.0f) < 0.5f,
          "scale: 50 halves it, exactly (each texel the mean of four)");

    // Scaled DOWN the image is filtered, not skipped through: hairlines a texel wide, four apart,
    // become an even quarter grey at a quarter of the size.
    auto lines = solid(64, 64, 0, 0, 0, 0);
    for (uint32_t x = 16; x < 48; x += 4) {
        block(*lines, x, 16, x + 1, 48, 0, 0, 0, 255);
    }
    library.files["lines.png"] = lines;
    MaterialOverlay hatch;
    hatch.color = "lines.png";
    hatch.blend = OverlayBlend::Normal;
    hatch.scale = 0.25f;
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {hatch}, library.source());
    bool even = out != nullptr;
    for (uint32_t y = 29; out && y < 35; ++y) {
        for (uint32_t x = 29; x < 34; ++x) {
            even = even && is(*out, x, y, 150, 113, 90, 3);
        }
    }
    check(even, "scale: scaled down, thin lines thin out evenly (filtered, none dropped)");

    // Scaled, turned and moved at once: the top mark at half the distance, a quarter turn on, 8 texels over.
    library.files["marks.png"] = fourMarks();
    MaterialOverlay marks;
    marks.color = "marks.png";
    marks.blend = OverlayBlend::Normal;
    marks.scale = 0.5f;
    marks.rotation = 90.0f;
    marks.offset = glm::vec2(0.125f, 0.0f);
    out = pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {marks}, library.source());
    check(out && is(*out, 46, 31, 255, 0, 0, 0) && is(*out, 47, 32, 255, 0, 0, 0) && is(*out, 39, 38, 0, 255, 0, 0) && is(*out, 32, 31, 0, 0, 255, 0) &&
              is(*out, 40, 24, 255, 255, 0, 0) && bare(*out, 40, 32) && bare(*out, 54, 32),
          "scale: scaled, turned and moved together");

    // The whole group: the gloss and the relief grow with the color; a normal's slopes are kept.
    auto lips = solid(64, 64, 255, 255, 255, 0);
    block(*lips, 24, 24, 40, 40, 255, 255, 255, 255);
    library.files["lips.png"] = lips;
    const std::shared_ptr<const DecodedImage> rough = solid(64, 64, 200, 200, 200, 255);
    MaterialOverlay gloss;
    gloss.color = "lips.png";
    gloss.gloss = 0.5f;
    gloss.scale = 2.0f;
    out = pose::compositeOverlays(MaterialMap::Roughness, rough, 0, {gloss}, library.source());
    check(out && at(*out, 18, 18)[0] == 100 && at(*out, 45, 45)[0] == 100 && at(*out, 12, 32)[0] == 200, "scale: the gloss grows with the color");
    auto ridge = solid(64, 64, 0, 0, 0, 0);
    block(*ridge, 24, 24, 40, 40, 255, 255, 255, 255);
    library.files["ridge.png"] = ridge;
    const std::shared_ptr<const DecodedImage> level = solid(64, 64, 100, 100, 100, 255);
    MaterialOverlay scar;
    scar.bump = "ridge.png";
    scar.scale = 2.0f;
    out = pose::compositeOverlays(MaterialMap::Bump, level, 0, {scar}, library.source());
    check(out && is(*out, 18, 18, 228, 228, 228, 1) && is(*out, 45, 45, 228, 228, 228, 1) && is(*out, 12, 32, 100, 100, 100, 0),
          "scale: the bump grows, as high as it was");
    const std::shared_ptr<const DecodedImage> flat = solid(64, 64, 128, 128, 255, 255);
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {scar}, library.source(), /*bumpAsNormal=*/true);
    check(out && at(*out, 16, 32)[0] < 126 && at(*out, 47, 32)[0] > 130 && is(*out, 32, 32, 128, 128, 255, 1) && is(*out, 24, 32, 128, 128, 255, 1),
          "scale: drawn as a normal, its slopes are at the grown edges");
    auto slope = solid(64, 64, 128, 128, 255, 0);
    block(*slope, 24, 24, 40, 40, 218, 128, 218, 255);
    library.files["slope.png"] = slope;
    MaterialOverlay groove;
    groove.normal = "slope.png";
    groove.scale = 2.0f;
    out = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {groove}, library.source());
    const int asPainted = [&]() {
        MaterialOverlay plain = groove;
        plain.scale = 1.0f;
        const auto image = pose::compositeOverlays(MaterialMap::Normal, flat, 0, {plain}, library.source());
        return image ? static_cast<int>(at(*image, 32, 32)[0]) : -1;
    }();
    check(out && asPainted > 190 && std::abs(at(*out, 18, 18)[0] - asPainted) <= 1 && std::abs(at(*out, 45, 45)[0] - asPainted) <= 1 &&
              is(*out, 12, 32, 128, 128, 255, 0),
          "scale: a normal image grows with its slopes as painted");

    // The field: percent of the painted size, a tenth to five times.
    MaterialOverlay f;
    pose::setOverlayField(f, pose::OverlayField::Scale, 150.0f);
    pose::OverlayField byName = pose::OverlayField::Count;
    const bool set = f.scale == 1.5f && pose::overlayFieldValue(f, pose::OverlayField::Scale) == 150.0f &&
                     pose::overlayFieldByName("scale", byName) && byName == pose::OverlayField::Scale && !(f == MaterialOverlay());
    pose::setOverlayField(f, pose::OverlayField::Scale, 1.0f);
    const bool low = approx(f.scale, 0.1f, 1.0e-6f);
    pose::setOverlayField(f, pose::OverlayField::Scale, 9000.0f);
    check(set && low && approx(f.scale, 5.0f, 1.0e-6f), "scale: the field reads percent, a tenth of the size to five times it");
}

void checkMask() {
    Library library;
    const std::shared_ptr<const DecodedImage> skin = solid(64, 64, 200, 150, 120, 255);
    const auto bare = [](const DecodedImage& image, uint32_t x, uint32_t y) { return is(image, x, y, 200, 150, 120, 0); };
    const auto source = library.source();
    const auto color = [&](const MaterialOverlay& o) { return pose::compositeOverlays(MaterialMap::BaseColor, skin, 0, {o}, source); };

    // THE MASK IMAGE: one value a texel — a grayscale image's brightness, or, where the image has
    // transparency, its alpha whatever its color — kept where it is above 0.
    auto grey = solid(64, 48, 0, 0, 0, 255);
    block(*grey, 20, 30, 30, 36, 255, 255, 255, 255);
    block(*grey, 30, 30, 34, 36, 128, 128, 128, 255);
    const auto fromGrey = pose::makeOverlayImage(*grey, OverlayImageKind::Mask);
    check(fromGrey->x == 20 && fromGrey->y == 30 && fromGrey->width == 14 && fromGrey->height == 6 && fromGrey->hasAlpha &&
              fromGrey->rgba[0] == 255 && fromGrey->rgba[3] == 255 && fromGrey->rgba[(10 * 4) + 3] == 128 && fromGrey->rgba[(10 * 4)] == 255,
          "mask image: a grayscale mask is its brightness, cropped to where it is above black");
    auto shape = solid(64, 48, 0, 0, 0, 0);
    block(*shape, 8, 8, 16, 12, 0, 0, 0, 255);  // BLACK, opaque: a tattoo's silhouette
    block(*shape, 16, 8, 20, 12, 90, 20, 20, 64); // a quarter there
    const auto fromAlpha = pose::makeOverlayImage(*shape, OverlayImageKind::Mask);
    check(fromAlpha->x == 8 && fromAlpha->y == 8 && fromAlpha->width == 12 && fromAlpha->height == 4 && fromAlpha->rgba[3] == 255 &&
              fromAlpha->rgba[(8 * 4) + 3] == 64 && fromAlpha->rgba[0] == 255,
          "mask image: an image with transparency masks by its alpha, whatever its color");
    check(pose::makeOverlayImage(*solid(16, 16, 0, 0, 0, 255), OverlayImageKind::Mask)->empty() &&
              pose::makeOverlayImage(*solid(16, 16, 200, 200, 200, 0), OverlayImageKind::Mask)->empty(),
          "mask image: all black, or all transparent, lets nothing through");

    // A PICTURE and an OPACITY MAP: the picture only where the map lets it, by as much as it lets it.
    auto ink = solid(64, 64, 0, 0, 0, 0);
    block(*ink, 16, 16, 48, 48, 0, 0, 0, 255);
    auto half = solid(64, 64, 0, 0, 0, 255); // the right half shows, a strip of it by half
    block(*half, 32, 0, 64, 64, 255, 255, 255, 255);
    block(*half, 32, 0, 36, 64, 128, 128, 128, 255);
    library.files["ink.png"] = ink;
    library.files["half.png"] = half;
    MaterialOverlay tattoo;
    tattoo.color = "ink.png";
    tattoo.blend = OverlayBlend::Normal;
    tattoo.opacityMap = "half.png";
    auto out = color(tattoo);
    check(out && is(*out, 40, 20, 0, 0, 0, 0) && bare(*out, 20, 20) && bare(*out, 31, 40) && bare(*out, 50, 50) && is(*out, 33, 20, 100, 75, 60, 2),
          "opacity map: a picture shows only where the map lets it (and by half where the map is half)");
    tattoo.opacity = 0.5f;
    out = color(tattoo);
    check(out && is(*out, 40, 20, 100, 75, 60, 2) && is(*out, 33, 20, 150, 113, 90, 2) && bare(*out, 20, 20),
          "opacity map: the Opacity value scales the map (50 of white is half, of half a quarter)");

    // A COLOR and an opacity map, no picture: makeup.
    auto lips = solid(64, 64, 255, 255, 255, 0);
    block(*lips, 16, 16, 32, 32, 255, 255, 255, 255);
    library.files["lips.png"] = lips;
    MaterialOverlay lipstick;
    lipstick.opacityMap = "lips.png";
    lipstick.tint = glm::vec3(0.8f, 0.1f, 0.2f);
    out = color(lipstick);
    check(out && is(*out, 20, 20, 160, 15, 24, 1) && bare(*out, 10, 10) && bare(*out, 32, 20) && bare(*out, 20, 33),
          "makeup: a Base Color and an opacity map alone — the color, multiplied into the skin, inside the map");
    lipstick.blend = OverlayBlend::Normal;
    out = color(lipstick);
    check(out && is(*out, 20, 20, 204, 26, 51, 1) && bare(*out, 40, 40), "makeup: ... or covering it (Normal): any blend");
    lipstick.opacity = 0.5f;
    out = color(lipstick);
    check(out && is(*out, 20, 20, 202, 88, 86, 1), "makeup: its opacity mixes it half way");
    lipstick.opacity = 1.0f;
    lipstick.hue = 120.0f; // (the adjustments work on a flat color as on a picture)
    out = color(lipstick);
    check(out && at(*out, 20, 20)[1] > at(*out, 20, 20)[0] && at(*out, 20, 20)[1] > at(*out, 20, 20)[2], "makeup: hue turns the flat color");
    lipstick.hue = 0.0f;
    // (The same file as a PICTURE with the color over it is the same lipstick: a white shape.)
    MaterialOverlay painted;
    painted.color = "lips.png";
    painted.tint = lipstick.tint;
    painted.blend = OverlayBlend::Normal;
    const auto asPicture = color(painted);
    out = color(lipstick);
    check(out && asPicture && out->pixels == asPicture->pixels, "makeup: a white shape as the opacity map, or as the picture, is the same lipstick");
    // A GRAYSCALE mask works as the map (as a picture it would paint its black).
    MaterialOverlay band;
    band.opacityMap = "half.png";
    band.tint = glm::vec3(0.8f, 0.1f, 0.2f);
    band.blend = OverlayBlend::Normal;
    out = color(band);
    check(out && is(*out, 40, 20, 204, 26, 51, 1) && bare(*out, 20, 20) && is(*out, 33, 20, 202, 88, 86, 2), "makeup: a grayscale mask is read by its brightness");

    // The map is one of the group's images: it is moved, turned and scaled with it — and, with no
    // picture, it is what the overlay turns about.
    lipstick.offset = glm::vec2(0.25f, 0.0f);
    out = color(lipstick);
    check(out && is(*out, 36, 20, 204, 26, 51, 1) && is(*out, 47, 31, 204, 26, 51, 1) && bare(*out, 20, 20) && bare(*out, 48, 20),
          "makeup: the offset moves the map, the color with it");
    lipstick.offset = glm::vec2(0.0f);
    auto bar = solid(64, 64, 0, 0, 0, 0);
    block(*bar, 16, 20, 32, 28, 255, 255, 255, 255); // 16 wide, 8 tall, centred on (24, 24)
    library.files["bar.png"] = bar;
    MaterialOverlay liner;
    liner.opacityMap = "bar.png";
    liner.tint = glm::vec3(0.0f);
    liner.blend = OverlayBlend::Normal;
    liner.rotation = 90.0f;
    out = color(liner);
    check(out && is(*out, 20, 16, 0, 0, 0, 0) && is(*out, 27, 31, 0, 0, 0, 0) && bare(*out, 17, 24) && bare(*out, 30, 24) && bare(*out, 24, 33),
          "makeup: turned, the map turns about its own centre");
    liner.rotation = 0.0f;
    liner.scale = 2.0f;
    out = color(liner);
    check(out && is(*out, 10, 18, 0, 0, 0, 0) && is(*out, 38, 30, 0, 0, 0, 0) && bare(*out, 24, 12) && bare(*out, 44, 24), "makeup: ... and scales about it");
    check(pose::overlayAnchorSource(liner) == "bar.png", "makeup: the opacity map is the overlay's anchor where it has no picture");

    // No picture and no map: a Base Color other than white tints the WHOLE map; white is nothing.
    MaterialOverlay tan;
    tan.tint = glm::vec3(0.5f, 0.5f, 0.5f);
    out = color(tan);
    check(out && is(*out, 0, 0, 100, 75, 60, 1) && is(*out, 63, 63, 100, 75, 60, 1) && is(*out, 31, 17, 100, 75, 60, 1),
          "flat: a Base Color with no picture and no map covers the whole map");
    out = pose::compositeOverlays(MaterialMap::BaseColor, nullptr, 0, {tan}, source);
    check(out && out->width > 0 && out->width <= 8 && is(*out, 0, 0, 128, 128, 128, 1), "flat: ... on a small canvas where the zone has no map");
    out = pose::compositeOverlays(MaterialMap::BaseColor, nullptr, 0, {lipstick}, source);
    check(out && out->width == 64 && is(*out, 20, 20, 204, 26, 51, 1) && is(*out, 5, 5, 255, 255, 255, 0),
          "flat: with an opacity map the canvas is the map's size");

    // An overlay AS IT IS ADDED — nothing chosen yet — paints nothing, anywhere.
    const MaterialOverlay empty;
    bool                  nothing = !pose::overlayHasColor(empty);
    for (const MaterialMap map : {MaterialMap::BaseColor, MaterialMap::Roughness, MaterialMap::Metalness, MaterialMap::Bump, MaterialMap::Normal,
                                  MaterialMap::Displacement}) {
        nothing = nothing && !pose::overlayAffects(empty, map) && pose::overlaySignature({empty}, map, true).empty() &&
                  !pose::compositeOverlays(map, skin, 0, {empty}, source, true);
    }
    check(nothing, "empty: an overlay as it is added paints nothing");
    MaterialOverlay switched = empty;
    switched.blend = OverlayBlend::Normal; // (even set to cover: there is no color to cover with)
    check(!color(switched), "empty: ... whatever its blend");

    // The GLOSS takes its shape from the map — and nothing from the Opacity VALUE.
    const std::shared_ptr<const DecodedImage> rough = solid(64, 64, 200, 200, 200, 255);
    MaterialOverlay glossy = lipstick;
    glossy.gloss = 0.5f;
    glossy.opacity = 0.0f;
    out = pose::compositeOverlays(MaterialMap::Roughness, rough, 0, {glossy}, source);
    check(out && at(*out, 20, 20)[0] == 100 && at(*out, 40, 40)[0] == 200 && at(*out, 10, 20)[0] == 200,
          "opacity map: the gloss is in the map's shape, whatever the Opacity value");
    glossy.opacityMap.clear();
    out = pose::compositeOverlays(MaterialMap::Roughness, rough, 0, {glossy}, source);
    check(out && at(*out, 20, 20)[0] == 100 && at(*out, 60, 3)[0] == 100, "flat: a gloss with no picture and no map is the whole map's");

    // THE RELIEF IS ITS OWN: its image and strength alone — not the Opacity value, not its map.
    // A ridge across the whole width is raised on BOTH sides of a map that shows only one.
    auto ridge = solid(64, 64, 0, 0, 0, 0);
    block(*ridge, 8, 8, 56, 24, 255, 255, 255, 255);
    auto slope = solid(64, 64, 128, 128, 255, 0);
    block(*slope, 8, 8, 56, 24, 218, 128, 218, 255);
    library.files["ridge.png"] = ridge;
    library.files["slope.png"] = slope;
    const std::shared_ptr<const DecodedImage> level = solid(64, 64, 100, 100, 100, 255);
    const std::shared_ptr<const DecodedImage> flat = solid(64, 64, 128, 128, 255, 255);
    MaterialOverlay wound;
    wound.color = "ink.png";
    wound.bump = "ridge.png";
    wound.normal = "slope.png";
    wound.displacement = "ridge.png";
    MaterialOverlay masked = wound;
    masked.opacityMap = "half.png";
    masked.opacity = 0.0f;
    out = pose::compositeOverlays(MaterialMap::Bump, level, 0, {masked}, source);
    check(out && is(*out, 12, 12, 228, 228, 228, 1) && is(*out, 50, 20, 228, 228, 228, 1) && is(*out, 12, 40, 100, 100, 100, 0),
          "relief: the bump is where ITS image is — on both sides of an opacity map that shows one");
    const auto same = [](const std::shared_ptr<DecodedImage>& a, const std::shared_ptr<DecodedImage>& b) { return a && b && a->pixels == b->pixels; };
    check(same(pose::compositeOverlays(MaterialMap::Bump, level, 0, {wound}, source), pose::compositeOverlays(MaterialMap::Bump, level, 0, {masked}, source)) &&
              same(pose::compositeOverlays(MaterialMap::Normal, flat, 0, {wound}, source, true),
                   pose::compositeOverlays(MaterialMap::Normal, flat, 0, {masked}, source, true)) &&
              same(pose::compositeOverlays(MaterialMap::Displacement, level, 0, {wound}, source),
                   pose::compositeOverlays(MaterialMap::Displacement, level, 0, {masked}, source)),
          "relief: the bump, the normal and the displacement follow neither the Opacity nor its map");
    check(!color(masked) && pose::overlaySignature({wound}, MaterialMap::Bump) == pose::overlaySignature({masked}, MaterialMap::Bump) &&
              pose::overlaySignature({wound}, MaterialMap::Normal, true) == pose::overlaySignature({masked}, MaterialMap::Normal, true),
          "opacity: at 0 the color is gone, and nothing of the relief is re-painted");

    // A map that cannot be had — or lets nothing through — takes the COLOR out (unmasked, a flat
    // color would flood the map); the relief, which never asked it, stays.
    MaterialOverlay lost = lipstick;
    lost.opacityMap = "gone.png";
    lost.bump = "ridge.png";
    MaterialOverlay shut = lipstick;
    library.files["black.png"] = solid(64, 64, 0, 0, 0, 255);
    shut.opacityMap = "black.png";
    check(!color(lost) && !color(shut) && pose::compositeOverlays(MaterialMap::Bump, level, 0, {lost}, source),
          "opacity map: one that is missing, or all black, leaves the color out (and the relief in)");

    // What the map re-paints: the color, the gloss and the metal — not the relief.
    MaterialOverlay o;
    o.color = "ink.png";
    o.bump = "ridge.png";
    o.gloss = 0.3f;
    o.metallic = 0.2f;
    const std::string before[4] = {pose::overlaySignature({o}, MaterialMap::BaseColor), pose::overlaySignature({o}, MaterialMap::Roughness),
                                   pose::overlaySignature({o}, MaterialMap::Metalness), pose::overlaySignature({o}, MaterialMap::Bump)};
    o.opacityMap = "half.png";
    check(pose::overlaySignature({o}, MaterialMap::BaseColor) != before[0] && pose::overlaySignature({o}, MaterialMap::Roughness) != before[1] &&
              pose::overlaySignature({o}, MaterialMap::Metalness) != before[2] && pose::overlaySignature({o}, MaterialMap::Bump) == before[3] &&
              !(o == MaterialOverlay()),
          "signature: an opacity map re-paints the color, the gloss and the metal, not the relief");
    pose::OverlayElement element = pose::OverlayElement::Color;
    check(pose::overlayElementByName("opacity", element) && element == pose::OverlayElement::Opacity && pose::overlayElementSource(o, element) == "half.png",
          "opacity map: an element of the overlay, by name");
}

void checkNames() {
    // A new overlay is "Overlay N": one past the highest such name in the stack, so a name is not
    // handed out twice and an overlay keeps its own when the stack is reordered.
    std::vector<MaterialOverlay> stack;
    const bool                   first = pose::nextOverlayNumber(stack, "Overlay") == 1;
    for (const char* name : {"Overlay 1", "Overlay 3", "Lipstick", "Overlay x", "Overlay 12abc", "Overlays 9", "Overlay", ""}) {
        MaterialOverlay o;
        o.name = name;
        stack.push_back(o);
    }
    check(first && pose::nextOverlayNumber(stack, "Overlay") == 4, "name: a new overlay takes the next number after the highest in the stack");
    stack.erase(stack.begin() + 1); // "Overlay 3" removed: 2 is free again
    check(pose::nextOverlayNumber(stack, "Overlay") == 2, "name: ... counting only the overlays that are there");

    // The name is the overlay's title — not its picture's file — an edit of its own (it undoes),
    // and no part of any map: a rename re-paints nothing.
    MaterialOverlay o;
    o.color = "dragon.png";
    o.gloss = 0.5f;
    o.bump = "ridge.png";
    MaterialOverlay renamed = o;
    renamed.name = "Dragon, left thigh";
    bool sameMaps = true;
    for (const MaterialMap map : {MaterialMap::BaseColor, MaterialMap::Roughness, MaterialMap::Metalness, MaterialMap::Bump, MaterialMap::Normal}) {
        sameMaps = sameMaps && pose::overlaySignature({o}, map, true) == pose::overlaySignature({renamed}, map, true);
    }
    check(pose::overlayTitle(o).empty() && pose::overlayTitle(renamed) == "Dragon, left thigh" && !(o == renamed) && sameMaps,
          "name: the overlay's title, an edit of its own, and no part of what is painted");
}

void checkMetal() {
    Library library;
    auto    ink = solid(64, 64, 0, 0, 0, 0);
    block(*ink, 16, 16, 48, 48, 200, 160, 40, 255); // gold ink
    auto half = solid(64, 64, 0, 0, 0, 255);        // the right half
    block(*half, 32, 0, 64, 64, 255, 255, 255, 255);
    auto bar = solid(64, 64, 0, 0, 0, 0);
    block(*bar, 16, 20, 32, 28, 255, 255, 255, 255); // 16 wide, 8 tall, centred on (24, 24)
    library.files["ink.png"] = ink;
    library.files["half.png"] = half;
    library.files["bar.png"] = bar;
    const auto source = library.source();
    const auto metal = [&](const std::shared_ptr<const DecodedImage>& base, int channel, const MaterialOverlay& o, float value) {
        return pose::compositeOverlays(MaterialMap::Metalness, base, channel, {o}, source, false, value);
    };

    // A zone with no metalness map and a Metallic value of 0 — skin: the composite is its ABSOLUTE
    // metalness, 0, with the overlay's metal on it.
    MaterialOverlay gold;
    gold.color = "ink.png";
    gold.metallic = 1.0f;
    auto out = metal(nullptr, 0, gold, 0.0f);
    check(out && out->width == 64 && at(*out, 20, 20)[0] == 255 && at(*out, 47, 47)[0] == 255 && at(*out, 5, 5)[0] == 0 && at(*out, 48, 20)[0] == 0,
          "metal: on skin (no map, Metallic 0) the overlay is metal where its picture is, and only there");
    gold.metallic = 0.5f;
    out = metal(nullptr, 0, gold, 0.0f);
    check(out && std::abs(at(*out, 20, 20)[0] - 128) <= 1 && at(*out, 5, 5)[0] == 0, "metal: 50 is half way to metal");

    // A metalness map (in green here) and a Metallic value: their product, the overlay's metal over it.
    const std::shared_ptr<const DecodedImage> map = solid(64, 64, 7, 128, 9, 255);
    out = metal(map, 1, gold, 0.5f);
    check(out && at(*out, 5, 5)[1] == 64 && std::abs(at(*out, 20, 20)[1] - 160) <= 1 && at(*out, 20, 20)[0] == 7 && at(*out, 20, 20)[2] == 9,
          "metal: the zone's Metallic value x its map, the overlay's metal on top (the other channels left alone)");
    check(is(*map, 20, 20, 7, 128, 9, 0), "metal: the zone's own map is never written");

    // Below 0: paint over a metal.
    const std::shared_ptr<const DecodedImage> steel = solid(64, 64, 255, 255, 255, 255);
    gold.metallic = -1.0f;
    out = metal(steel, 0, gold, 1.0f);
    check(out && at(*out, 20, 20)[0] == 0 && at(*out, 5, 5)[0] == 255, "metal: -100 is paint over a metal surface");
    gold.metallic = -0.5f;
    out = metal(steel, 0, gold, 1.0f);
    check(out && std::abs(at(*out, 20, 20)[0] - 128) <= 1, "metal: ... by halves");

    // Its own map limits it WITHIN the overlay; the mask limits the whole overlay.
    gold.metallic = 1.0f;
    gold.metallicMap = "half.png";
    out = metal(nullptr, 0, gold, 0.0f);
    check(out && at(*out, 40, 20)[0] == 255 && at(*out, 20, 20)[0] == 0 && at(*out, 60, 20)[0] == 0,
          "metal: the metallic map limits it within the picture");
    auto color = pose::compositeOverlays(MaterialMap::BaseColor, solid(64, 64, 10, 10, 10, 255), 0, {gold}, source);
    gold.blend = OverlayBlend::Normal;
    color = pose::compositeOverlays(MaterialMap::BaseColor, solid(64, 64, 10, 10, 10, 255), 0, {gold}, source);
    check(color && is(*color, 20, 20, 200, 160, 40, 0) && is(*color, 40, 20, 200, 160, 40, 0), "metal: ... and leaves the color whole");
    gold.metallicMap.clear();
    gold.opacityMap = "half.png";
    out = metal(nullptr, 0, gold, 0.0f);
    check(out && at(*out, 40, 20)[0] == 255 && at(*out, 20, 20)[0] == 0, "metal: the opacity map shapes it too, as it shapes the color");
    gold.opacityMap.clear();

    // No picture: the metallic map is the shape — and what the overlay turns about.
    MaterialOverlay leaf;
    leaf.metallic = 1.0f;
    leaf.metallicMap = "bar.png";
    out = metal(nullptr, 0, leaf, 0.0f);
    check(out && at(*out, 20, 24)[0] == 255 && at(*out, 40, 24)[0] == 0 && at(*out, 24, 30)[0] == 0, "metal: with no picture, the metallic map is its shape");
    leaf.rotation = 90.0f;
    out = metal(nullptr, 0, leaf, 0.0f);
    check(out && at(*out, 22, 18)[0] == 255 && at(*out, 27, 31)[0] == 255 && at(*out, 18, 24)[0] == 0 && pose::overlayAnchorSource(leaf) == "bar.png",
          "metal: ... and its anchor (a quarter turn about its own centre)");

    // Nothing at all: the whole zone is metal.
    MaterialOverlay chrome;
    chrome.metallic = 1.0f;
    out = metal(map, 1, chrome, 0.0f);
    check(out && at(*out, 0, 0)[1] == 255 && at(*out, 63, 63)[1] == 255 && at(*out, 31, 9)[1] == 255, "metal: with no picture, mask or map, the whole zone");

    // The opacity is the color's: the metal is what it was. A metallic map that cannot be had
    // takes the metal out; an overlay that is not metallic makes no composite, whatever the value.
    MaterialOverlay faded = gold;
    faded.opacity = 0.0f;
    const auto a = metal(nullptr, 0, gold, 0.0f);
    const auto b = metal(nullptr, 0, faded, 0.0f);
    check(a && b && a->pixels == b->pixels, "metal: it does not follow the opacity");
    MaterialOverlay lost = gold;
    lost.metallicMap = "gone.png";
    MaterialOverlay plain;
    plain.color = "ink.png";
    check(!metal(nullptr, 0, lost, 0.0f) && !metal(map, 1, plain, 0.5f), "metal: a missing metallic map leaves it out; no metallic, no composite");

    // What it re-paints: the metalness, alone.
    MaterialOverlay o;
    o.color = "ink.png";
    o.gloss = 0.3f;
    const std::string colorBefore = pose::overlaySignature({o}, MaterialMap::BaseColor);
    const std::string glossBefore = pose::overlaySignature({o}, MaterialMap::Roughness);
    const bool        none = pose::overlaySignature({o}, MaterialMap::Metalness).empty() && !pose::overlayAffects(o, MaterialMap::Metalness);
    o.metallic = 0.4f;
    const std::string metalOn = pose::overlaySignature({o}, MaterialMap::Metalness);
    o.metallicMap = "half.png";
    const std::string metalMapped = pose::overlaySignature({o}, MaterialMap::Metalness);
    o.offset.x = 0.1f;
    check(none && !metalOn.empty() && metalMapped != metalOn && pose::overlaySignature({o}, MaterialMap::Metalness) != metalMapped,
          "signature: the metalness follows the metallic, its map and the overlay's place");
    o.offset.x = 0.0f;
    check(pose::overlaySignature({o}, MaterialMap::BaseColor) == colorBefore && pose::overlaySignature({o}, MaterialMap::Roughness) == glossBefore,
          "signature: ... and a metallic edit re-paints neither the color nor the gloss");

    // The field and the element.
    MaterialOverlay f;
    pose::setOverlayField(f, pose::OverlayField::Metallic, 60.0f);
    pose::OverlayField   field = pose::OverlayField::Count;
    pose::OverlayElement element = pose::OverlayElement::Color;
    const bool           set = approx(f.metallic, 0.6f, 1.0e-6f) && pose::overlayFieldByName("metallic", field) && field == pose::OverlayField::Metallic;
    pose::setOverlayField(f, pose::OverlayField::Metallic, -900.0f);
    f.metallicMap = "m.png";
    check(set && f.metallic == -1.0f && pose::overlayElementByName("metallic", element) && element == pose::OverlayElement::Metallic &&
              pose::overlayElementSource(f, element) == "m.png" && !(f == MaterialOverlay()),
          "metal: the field reads percent either way, and its map is an element of the overlay");
}

void checkSignatures() {
    MaterialOverlay o;
    o.color = "a.png";
    o.bump = "b.png";
    std::vector<MaterialOverlay> stack = {o};
    const std::string color = pose::overlaySignature(stack, MaterialMap::BaseColor);
    const std::string bump = pose::overlaySignature(stack, MaterialMap::Bump);
    check(!color.empty() && !bump.empty() && pose::overlaySignature(stack, MaterialMap::Roughness).empty() &&
              pose::overlaySignature(stack, MaterialMap::Normal).empty(),
          "signature: only the slots the overlay touches have one");
    stack[0].hue = 30.0f;
    check(pose::overlaySignature(stack, MaterialMap::BaseColor) != color && pose::overlaySignature(stack, MaterialMap::Bump) == bump,
          "signature: a hue change re-paints the color map, not the bump");
    const std::string relief = pose::overlaySignature(stack, MaterialMap::Normal, /*bumpAsNormal=*/true);
    stack[0].bumpStrength = 2.0f;
    check(!relief.empty() && pose::overlaySignature(stack, MaterialMap::Normal, true) != relief,
          "signature: the viewport's normal map follows the bump image and its strength");
    stack[0].gloss = 0.3f;
    check(!pose::overlaySignature(stack, MaterialMap::Roughness).empty(), "signature: a gloss brings the roughness map in");
    // A move re-paints EVERY map the overlay touches, and none it does not.
    const std::string before[4] = {pose::overlaySignature(stack, MaterialMap::BaseColor), pose::overlaySignature(stack, MaterialMap::Bump),
                                   pose::overlaySignature(stack, MaterialMap::Roughness), pose::overlaySignature(stack, MaterialMap::Normal, true)};
    stack[0].offset.y = 0.01f;
    check(pose::overlaySignature(stack, MaterialMap::BaseColor) != before[0] && pose::overlaySignature(stack, MaterialMap::Bump) != before[1] &&
              pose::overlaySignature(stack, MaterialMap::Roughness) != before[2] &&
              pose::overlaySignature(stack, MaterialMap::Normal, true) != before[3] && pose::overlaySignature(stack, MaterialMap::Normal).empty(),
          "signature: a move re-paints every map the overlay touches");
    // So does a turn, and a scale. And turned or scaled, the overlay does so about its ANCHOR
    // image's centre: another color image re-paints the bump too — which, as painted, it does not.
    const auto all = [&stack]() {
        return pose::overlaySignature(stack, MaterialMap::BaseColor) + "#" + pose::overlaySignature(stack, MaterialMap::Bump) + "#" +
               pose::overlaySignature(stack, MaterialMap::Roughness) + "#" + pose::overlaySignature(stack, MaterialMap::Normal, true);
    };
    const auto differs = [&stack](const std::string before[4]) {
        return pose::overlaySignature(stack, MaterialMap::BaseColor) != before[0] && pose::overlaySignature(stack, MaterialMap::Bump) != before[1] &&
               pose::overlaySignature(stack, MaterialMap::Roughness) != before[2] && pose::overlaySignature(stack, MaterialMap::Normal, true) != before[3];
    };
    const auto snapshot = [&stack](std::string out[4]) {
        out[0] = pose::overlaySignature(stack, MaterialMap::BaseColor);
        out[1] = pose::overlaySignature(stack, MaterialMap::Bump);
        out[2] = pose::overlaySignature(stack, MaterialMap::Roughness);
        out[3] = pose::overlaySignature(stack, MaterialMap::Normal, true);
    };
    std::string was[4];
    snapshot(was);
    stack[0].color = "other.png";
    const bool bumpKept = pose::overlaySignature(stack, MaterialMap::Bump) == was[1];
    stack[0].color = "a.png";
    stack[0].rotation = 15.0f;
    const bool turnRepaints = differs(was);
    snapshot(was);
    stack[0].color = "other.png";
    const bool bumpFollowsAnchor = pose::overlaySignature(stack, MaterialMap::Bump) != was[1];
    stack[0].color = "a.png";
    stack[0].rotation = 0.0f;
    snapshot(was);
    stack[0].scale = 1.25f;
    const bool scaleRepaints = differs(was);
    snapshot(was);
    stack[0].color = "other.png";
    const bool bumpFollowsAnchorScaled = pose::overlaySignature(stack, MaterialMap::Bump) != was[1];
    stack[0].color = "a.png";
    check(turnRepaints && scaleRepaints && !all().empty(), "signature: a turn and a scale re-paint every map the overlay touches");
    check(bumpKept && bumpFollowsAnchor && bumpFollowsAnchorScaled,
          "signature: turned or scaled, the relief follows the color image it turns about");
    stack[0].enabled = false;
    check(pose::overlaySignature(stack, MaterialMap::BaseColor).empty() && pose::overlaySignature(stack, MaterialMap::Bump).empty(),
          "signature: a disabled overlay touches nothing");
}

} // namespace

int main() {
    checkBlends();
    checkAdjustments();
    checkCrop();
    checkColorComposite();
    checkGloss();
    checkRelief();
    checkOffset();
    checkTurn();
    checkScale();
    checkMask();
    checkNames();
    checkMetal();
    checkSignatures();
    std::printf("\n%d failure(s)\n", failures);
    return failures;
}
