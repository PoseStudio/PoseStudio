/**
 * @file materialball.cpp
 * @brief The icon studio, the material ball's mesh and the built-in ball shader. See materialball.h.
 */

#include "materialball.h"

#include "parallelfor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace pose {

namespace {

constexpr float kPi = 3.14159265358979f;

/// Sub-samples per pixel, each way: the silhouette's antialiasing and the maps' filtering.
constexpr int kSubSamples = 4;

// --- The environment, blurred once per roughness -------------------------------------------
// A rough surface reflects a blurred environment. The viewport prefilters a cube map for that;
// here a handful of small equirectangular images do, each the environment convolved with the
// lobe of one roughness (a cosine power about the reflection direction) — and one more
// convolved with the cosine itself: the irradiance a diffuse surface receives.

/// The direction of texel (x, y) of a @p width × @p height equirectangular image — the inverse
/// of EnvironmentImage::sample's mapping.
glm::vec3 texelDirection(int x, int y, int width, int height) {
    const float phi = (static_cast<float>(x) + 0.5f) / static_cast<float>(width) * 2.0f * kPi;
    const float theta = (static_cast<float>(y) + 0.5f) / static_cast<float>(height) * kPi;
    const float s = std::sin(theta);
    return glm::vec3(s * std::sin(phi), std::cos(theta), -s * std::cos(phi));
}

/// @p environment averaged down to @p width × @p height (each texel the mean of the source
/// texels it covers): a detailed panorama's softboxes are a few texels wide, and sampled at a
/// point a small image of it would catch or miss them by chance.
EnvironmentImage boxDownsample(const EnvironmentImage& environment, int width, int height) {
    if (environment.width <= width && environment.height <= height) {
        return environment;
    }
    EnvironmentImage out;
    out.width = width;
    out.height = height;
    out.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), glm::vec3(0.0f));
    for (int y = 0; y < height; ++y) {
        const int y0 = y * environment.height / height;
        const int y1 = std::max((y + 1) * environment.height / height, y0 + 1);
        for (int x = 0; x < width; ++x) {
            const int x0 = x * environment.width / width;
            const int x1 = std::max((x + 1) * environment.width / width, x0 + 1);
            glm::vec3 sum(0.0f);
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    sum += environment.texel(sx, sy);
                }
            }
            out.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
                sum / static_cast<float>((x1 - x0) * (y1 - y0));
        }
    }
    return out;
}

struct Source {
    std::vector<glm::vec3> direction;
    std::vector<glm::vec3> radiance; // × the texel's solid angle
    std::vector<float>     solidAngle;
};

Source makeSource(const EnvironmentImage& full, int width, int height) {
    const EnvironmentImage environment = boxDownsample(full, width, height);
    Source source;
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    source.direction.resize(count);
    source.radiance.resize(count);
    source.solidAngle.resize(count);
    for (int y = 0; y < height; ++y) {
        const float theta = (static_cast<float>(y) + 0.5f) / static_cast<float>(height) * kPi;
        const float omega = std::sin(theta) * (kPi / static_cast<float>(height)) * (2.0f * kPi / static_cast<float>(width));
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            source.direction[i] = texelDirection(x, y, width, height);
            source.solidAngle[i] = omega;
            source.radiance[i] = environment.sample(source.direction[i]) * omega;
        }
    }
    return source;
}

/// The environment convolved with cos^power about each direction. @p normalized: divided by the
/// lobe's own integral (a reflection: the average radiance in the lobe); else the plain
/// integral (power 1: the irradiance).
EnvironmentImage convolve(const Source& source, int width, int height, float power, bool normalized) {
    EnvironmentImage out;
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    parallelFor(height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            const glm::vec3 axis = texelDirection(x, y, width, height);
            glm::vec3       sum(0.0f);
            float           weightSum = 0.0f;
            for (std::size_t i = 0; i < source.direction.size(); ++i) {
                const float c = glm::dot(axis, source.direction[i]);
                if (c <= 0.0f) {
                    continue;
                }
                const float w = std::pow(c, power);
                sum += source.radiance[i] * w;
                weightSum += source.solidAngle[i] * w;
            }
            out.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
                normalized ? (weightSum > 0.0f ? sum / weightSum : glm::vec3(0.0f)) : sum;
        }
    });
    return out;
}

/// The blurred environments: level 0 is the environment itself (a mirror), the others are the
/// roughnesses of kLevels.
constexpr std::array<float, 6> kLevels = {0.0f, 0.2f, 0.35f, 0.5f, 0.7f, 1.0f};

struct Lighting {
    EnvironmentImage                              mirror; // the environment at the size a mirror ball shows it
    const EnvironmentImage*                       sharp = nullptr;
    std::array<EnvironmentImage, kLevels.size()>  blurred; // [0] unused
    EnvironmentImage                              irradiance;

    glm::vec3 reflection(const glm::vec3& direction, float roughness) const {
        const float r = std::clamp(roughness, 0.0f, 1.0f);
        std::size_t k = 1;
        while (k + 1 < kLevels.size() && r > kLevels[k]) {
            ++k;
        }
        const float     t = (r - kLevels[k - 1]) / (kLevels[k] - kLevels[k - 1]);
        const glm::vec3 lower = k == 1 ? sharp->sample(direction) : blurred[k - 1].sample(direction);
        return glm::mix(lower, blurred[k].sample(direction), std::clamp(t, 0.0f, 1.0f));
    }
};

Lighting makeLighting(const EnvironmentImage& environment) {
    Lighting lighting;
    lighting.mirror = boxDownsample(environment, 1024, 512);
    const Source source = makeSource(lighting.mirror, 96, 48);
    for (std::size_t k = 1; k < kLevels.size(); ++k) {
        // A GGX lobe of roughness r is about as wide as a cosine power of 2 / r^4 - 2.
        const float alpha = kLevels[k] * kLevels[k];
        const float power = std::max(2.0f / (alpha * alpha) - 2.0f, 1.0f);
        lighting.blurred[k] = convolve(source, 64, 32, power, /*normalized=*/true);
    }
    lighting.irradiance = convolve(source, 32, 16, 1.0f, /*normalized=*/false);
    return lighting; // (sharp is pointed at the copy's own mirror by the caller: see shadeMaterialBall)
}

// --- The maps -------------------------------------------------------------------------------

float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

/// The texel of @p image at @p uv (wrapping), or null where there is no image.
const std::uint8_t* texel(const std::shared_ptr<const DecodedImage>& image, const glm::vec2& uv) {
    if (!image || image->width == 0 || image->height == 0 || image->pixels.empty()) {
        return nullptr;
    }
    const float u = uv.x - std::floor(uv.x);
    const float v = uv.y - std::floor(uv.y);
    const auto  x = std::min(static_cast<std::uint32_t>(u * static_cast<float>(image->width)), image->width - 1);
    const auto  y = std::min(static_cast<std::uint32_t>(v * static_cast<float>(image->height)), image->height - 1);
    return &image->pixels[(static_cast<std::size_t>(y) * image->width + x) * 4u];
}

glm::vec3 colorAt(const std::shared_ptr<const DecodedImage>& image, const glm::vec2& uv) {
    const std::uint8_t* p = texel(image, uv);
    if (p == nullptr) {
        return glm::vec3(1.0f);
    }
    return glm::vec3(srgbToLinear(p[0] / 255.0f), srgbToLinear(p[1] / 255.0f), srgbToLinear(p[2] / 255.0f));
}

float valueAt(const std::shared_ptr<const DecodedImage>& image, int channel, const glm::vec2& uv) {
    const std::uint8_t* p = texel(image, uv);
    return p == nullptr ? 1.0f : p[std::clamp(channel, 0, 3)] / 255.0f;
}

// --- The shading ----------------------------------------------------------------------------

/// The share of the environment a surface of reflectance @p f0 and @p roughness reflects toward
/// a viewer at cosine @p ndv: Karis's analytic fit of the split-sum's second half.
glm::vec3 environmentBrdf(const glm::vec3& f0, float roughness, float ndv) {
    const glm::vec4 c0(-1.0f, -0.0275f, -0.572f, 0.022f);
    const glm::vec4 c1(1.0f, 0.0425f, 1.04f, -0.04f);
    const glm::vec4 r = roughness * c0 + c1;
    const float     a004 = std::min(r.x * r.x, std::exp2(-9.28f * ndv)) * r.x + r.y;
    const glm::vec2 ab = glm::vec2(-1.04f, 1.04f) * a004 + glm::vec2(r.z, r.w);
    return f0 * ab.x + glm::vec3(ab.y);
}

float tonemap(float x) { // (the ACES fit the viewport's PBR mode ends with: color.glsl)
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    x = std::max(x, 0.0f);
    return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
}

// --- The icon studio ------------------------------------------------------------------------
// (What it is for and what is in it: materialball.h. Angles in degrees: an AZIMUTH is measured
// from +Z, the camera's side, toward +X, the picture's right; an ELEVATION up from the horizon.
// Every number below was set by LOOKING at chrome, gold, plastic, matte white, glass, car paint,
// brushed metal, velvet and skin balls rendered under it.)

constexpr float kStudioWall = 0.01f; ///< the tent's dark walls and ceiling

// The floor: its edge on the horizon behind the ball, this far BELOW it on the camera's side.
constexpr float kStudioFloor = 0.8f;
constexpr float kStudioFloorDip = 50.0f;
constexpr float kStudioFloorEdge = 1.5f;

// BEHIND THE BALL THE ROOM FALLS AWAY INTO DIMNESS. A ball's RIM mirrors what lies straight
// behind it — a point a share rho of the radius out from the middle mirrors the direction
// 2 acos(rho) from straight behind — and at that grazing angle every surface reflects without
// color. So what stands behind the ball is the color of its edge, whatever it is made of: a
// white backdrop there (1.8, the first studio) drew a pale, colorless rim round gold, plastic and
// skin alike, which on a library's dark page read as a halo. The floor therefore fades toward
// the far horizon — to this share of itself, over this angle from straight behind: the outer
// three tenths of a mirror ball's radius, a smooth fall toward its lower edge — and runs into a
// wall of that same dim grey: no line where they meet, and an edge that is darker than the ball,
// never lighter. (0.12 lost a chrome ball's outline on a black page and left glass dark; 0.32
// was a pale rim again.)
constexpr float kStudioFloorFar = 0.2f;
constexpr float kStudioFloorFade = 90.0f;

// The backdrop: that wall. Behind the ball, this far to either side of straight behind, from the
// floor up.
constexpr float kStudioBackdrop = kStudioFloor * kStudioFloorFar;
constexpr float kStudioBackdropHalfWidth = 65.0f;
constexpr float kStudioBackdropEdge = 10.0f;    ///< … its sides fading over this much more
constexpr float kStudioBackdropFullTo = 5.0f;   ///< whole up to this elevation …
constexpr float kStudioBackdropGoneAt = 30.0f;  ///< … and gone by this one

// AND STRAIGHT BEHIND THE BALL IT IS DARK — the black card a photographer stands behind a
// reflective thing. The dim wall alone still drew a ring: on a SATURATED metal the outer three
// or four pixels of the ball mirror the 30 degrees about straight behind at a grazing angle,
// where the reflectance of every channel runs to one, so 0.16 of grey came back as a grey,
// colorless line round a yellow, blue or red ball — "a little bit of a white halo", on a dark
// page. Within the first angle of straight behind the room is as dark as the walls, wall and
// floor alike, and whole again from the second: on a mirror ball the outer tenth of the radius
// (rho = cos(angle / 2): 0.996 .. 0.887), a fall into the page with the material's own color
// just inside it. (20 .. 60 and wider drew a BLACK outline round the ball on the library's
// dark grey and softened its foot; 0 .. 40 and narrower left the line.)
constexpr float kStudioBehindDark = 10.0f;
constexpr float kStudioBehindClear = 55.0f;

/// A rectangular light on the studio's sphere.
struct StudioLight {
    float azimuth, elevation;    ///< its centre
    float halfWidth, halfHeight; ///< its half extents, as angles at the ball
    float radiance;
    float edge;                  ///< how wide its edges fade
};
constexpr std::array<StudioLight, 2> kStudioLights = {{
    {-40.0f, 54.0f, 36.0f, 23.0f, 6.5f, 1.5f}, // the softbox: above, to the left, on the camera's side
    {62.0f, 38.0f, 3.0f, 26.0f, 6.0f, 0.8f},   // the strip: to the right, tall and thin
}};

/// 0 at @p from, 1 at @p to (either order), smooth between.
float ramp(float from, float to, float x) {
    const float t = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// A light's own tangent frame on the sphere: its centre, its width level, its height up.
struct StudioLightFrame {
    glm::vec3 centre, right, up;
};
const std::array<StudioLightFrame, kStudioLights.size()>& studioLightFrames() {
    static const std::array<StudioLightFrame, kStudioLights.size()> frames = [] {
        std::array<StudioLightFrame, kStudioLights.size()> made;
        for (std::size_t i = 0; i < kStudioLights.size(); ++i) {
            const float az = glm::radians(kStudioLights[i].azimuth), el = glm::radians(kStudioLights[i].elevation);
            made[i].centre = glm::vec3(std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az));
            made[i].right = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), made[i].centre));
            made[i].up = glm::cross(made[i].centre, made[i].right);
        }
        return made;
    }();
    return frames;
}

/// How much of @p light covers the unit direction @p d: 1 inside, 0 outside, its edges soft.
float lightCoverage(const StudioLight& light, const StudioLightFrame& frame, const glm::vec3& d) {
    // (More than 60 degrees from its centre no light of the studio reaches: the tangent-frame
    // angles below are then both past every half extent.)
    const float along = glm::dot(d, frame.centre);
    if (along <= 0.5f) {
        return 0.0f;
    }
    const float x = std::abs(glm::degrees(std::atan2(glm::dot(d, frame.right), along)));
    const float y = std::abs(glm::degrees(std::atan2(glm::dot(d, frame.up), along)));
    return (1.0f - ramp(light.halfWidth - light.edge, light.halfWidth, x)) *
           (1.0f - ramp(light.halfHeight - light.edge, light.halfHeight, y));
}

} // namespace

float iconStudioRadiance(const glm::vec3& direction) {
    const float length = glm::length(direction);
    if (!(length > 0.0f)) {
        return kStudioWall;
    }
    const glm::vec3 d = direction / length;
    const float     elevation = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
    const float     azimuth = glm::degrees(std::atan2(d.x, d.z));
    const float     behind = 0.5f - 0.5f * std::cos(glm::radians(azimuth)); // 0 on the camera's side .. 1 straight behind the ball

    float radiance = kStudioWall;
    const float backdrop = ramp(kStudioBackdropHalfWidth + kStudioBackdropEdge, kStudioBackdropHalfWidth, 180.0f - std::abs(azimuth)) *
                           ramp(kStudioBackdropGoneAt, kStudioBackdropFullTo, elevation);
    radiance = glm::mix(radiance, kStudioBackdrop, backdrop);
    const float horizon = -kStudioFloorDip * (1.0f - behind);
    const float fromBehind = glm::degrees(std::acos(std::clamp(-d.z, -1.0f, 1.0f))); // 0 straight behind the ball
    const float floor = kStudioFloor * glm::mix(kStudioFloorFar, 1.0f, ramp(0.0f, kStudioFloorFade, fromBehind));
    radiance = glm::mix(radiance, floor, ramp(horizon, horizon - kStudioFloorEdge, elevation));
    radiance = glm::mix(kStudioWall, radiance, ramp(kStudioBehindDark, kStudioBehindClear, fromBehind));
    const auto& frames = studioLightFrames();
    for (std::size_t i = 0; i < kStudioLights.size(); ++i) {
        radiance = glm::mix(radiance, kStudioLights[i].radiance, lightCoverage(kStudioLights[i], frames[i], d));
    }
    return radiance;
}

EnvironmentImage makeIconStudio(int width, int height) {
    width = std::max(width, 8);
    height = std::max(height, 4);
    EnvironmentImage studio;
    studio.width = width;
    studio.height = height;
    studio.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    constexpr int kSamples = 2; // each way: the lights' and the floor's edges antialiased
    parallelFor(height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            float sum = 0.0f;
            for (int sy = 0; sy < kSamples; ++sy) {
                const float theta = (static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / kSamples) / static_cast<float>(height) * kPi;
                for (int sx = 0; sx < kSamples; ++sx) {
                    // (EnvironmentImage::sample's mapping, inverted: texelDirection's.)
                    const float phi = (static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / kSamples) / static_cast<float>(width) * 2.0f * kPi;
                    const float s = std::sin(theta);
                    sum += iconStudioRadiance(glm::vec3(s * std::sin(phi), std::cos(theta), -s * std::cos(phi)));
                }
            }
            studio.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
                glm::vec3(sum / static_cast<float>(kSamples * kSamples));
        }
    });
    return studio;
}

LightingSettings iconStudioLighting() {
    LightingSettings lighting;
    lighting.exposure = 1.0f;
    lighting.diffuseIntensity = 1.0f; // (to a renderer: the environment's strength — the studio as it is)
    lighting.specularIntensity = 1.0f;
    lighting.ambientFill = 0.0f;
    lighting.keyIntensity = 0.0f;
    lighting.environmentRotationDeg = 0.0f;
    lighting.tonemap = true;
    lighting.backdropMode = 0;
    return lighting;
}

BallMesh makeBallMesh(const glm::vec3& center, float radius, int around, int down) {
    around = std::max(around, 8);
    down = std::max(down, 4);
    BallMesh    mesh;
    const auto  count = static_cast<std::size_t>(around + 1) * static_cast<std::size_t>(down + 1);
    mesh.positions.reserve(count);
    mesh.normals.reserve(count);
    mesh.uvs.reserve(count);
    mesh.tangents.reserve(count);
    for (int j = 0; j <= down; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(down);
        const float theta = v * kPi; // 0 at the top
        for (int i = 0; i <= around; ++i) {
            const float     turn = static_cast<float>(i) / static_cast<float>(around);
            // The turn starts at the ball's LEFT side (-X) and passes the front (+Z) a quarter
            // of the way round, growing toward +X: see the header for why.
            const float     phi = turn * 2.0f * kPi - 0.5f * kPi;
            const glm::vec3 n(std::sin(theta) * std::sin(phi), std::cos(theta), std::sin(theta) * std::cos(phi));
            mesh.positions.push_back(center + radius * n);
            mesh.normals.push_back(n);
            mesh.uvs.emplace_back(turn * 2.0f, v);
            // Along +u; the bitangent (down the ball, +v) is cross(n, t) reversed: handedness -1.
            mesh.tangents.emplace_back(std::cos(phi), 0.0f, -std::sin(phi), -1.0f);
        }
    }
    mesh.indices.reserve(static_cast<std::size_t>(around) * static_cast<std::size_t>(down) * 6u);
    const auto at = [around](int i, int j) { return static_cast<uint32_t>(j * (around + 1) + i); };
    for (int j = 0; j < down; ++j) {
        for (int i = 0; i < around; ++i) {
            // Seen from outside: u to the right, v down — top-left, bottom-left, bottom-right is
            // counter-clockwise. (The pole rows' degenerate triangles are left out.)
            if (j + 1 < down) {
                mesh.indices.insert(mesh.indices.end(), {at(i, j), at(i, j + 1), at(i + 1, j + 1)});
            }
            if (j > 0) {
                mesh.indices.insert(mesh.indices.end(), {at(i, j), at(i + 1, j + 1), at(i + 1, j)});
            }
        }
    }
    return mesh;
}

std::vector<std::uint8_t> shadeMaterialBall(const MaterialParams& m, const BallMaps& maps,
                                            const EnvironmentImage& environment, const LightingSettings& lighting,
                                            int size) {
    size = std::max(size, 1);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4u, 0);
    if (environment.pixels.empty()) {
        return out;
    }
    Lighting light = makeLighting(environment);
    light.sharp = &light.mirror;

    // The key light, as the viewport's PBR mode has it (cameraubo.cpp's direction, renderexport's color).
    const float     azimuth = glm::radians(lighting.keyAzimuthDeg);
    const float     elevation = glm::radians(lighting.keyElevationDeg);
    const glm::vec3 keyDirection(std::cos(elevation) * std::sin(azimuth), std::sin(elevation), std::cos(elevation) * std::cos(azimuth));
    const glm::vec3 keyColor = glm::vec3(1.0f, 0.96f, 0.9f) * std::max(lighting.keyIntensity, 0.0f);

    // The material's layers, each as its switch leaves it.
    const bool      tiles = m.tilingEnabled;
    const float     coat = m.topCoatEnabled ? std::clamp(m.topCoat, 0.0f, 1.0f) : 0.0f;
    const float     transmission = m.transmissionEnabled ? std::clamp(m.transmission, 0.0f, 1.0f) : 0.0f;
    const glm::vec3 emission = m.emissionEnabled ? m.emissionColor * std::max(m.emission, 0.0f) : glm::vec3(0.0f);
    const float     dielectric = ((m.ior - 1.0f) / (m.ior + 1.0f)) * ((m.ior - 1.0f) / (m.ior + 1.0f));
    const float     opacity = std::clamp(m.opacity, 0.0f, 1.0f);
    const glm::vec3 view(0.0f, 0.0f, 1.0f);

    const auto shade = [&](const glm::vec3& n) -> glm::vec3 {
        // The ball mesh's own UVs (makeBallMesh), then the material's tiling.
        float turn = (std::atan2(n.x, n.z) + 0.5f * kPi) / (2.0f * kPi);
        if (turn < 0.0f) {
            turn += 1.0f;
        }
        glm::vec2 uv(turn * 2.0f, std::acos(std::clamp(n.y, -1.0f, 1.0f)) / kPi);
        if (tiles) {
            uv = uv * m.uvTiling + m.uvOffset;
        }

        glm::vec3 albedo(0.0f);
        float     metal = 0.0f;
        if (m.baseEnabled) {
            albedo = m.baseColor * colorAt(maps.baseColor, uv);
            metal = std::clamp(m.metalness * valueAt(maps.metalness, maps.metalnessChannel, uv), 0.0f, 1.0f);
        }
        const float roughness = std::clamp(m.roughness * valueAt(maps.roughness, maps.roughnessChannel, uv), 0.03f, 1.0f);
        const float specular = m.specularEnabled ? m.specular * valueAt(maps.specular, maps.specularChannel, uv) : 0.0f;
        const glm::vec3 f0 = glm::mix(glm::min(dielectric * specular * m.specularColor, glm::vec3(1.0f)), albedo, metal);

        const float     ndv = std::max(n.z, 1.0e-3f);
        const glm::vec3 reflected = 2.0f * ndv * n - view;

        // Diffuse: the environment's irradiance and the fill, then the key light.
        const glm::vec3 diffuseColor = albedo * (1.0f - metal);
        glm::vec3       body = diffuseColor * (light.irradiance.sample(n) / kPi * lighting.diffuseIntensity + lighting.ambientFill);
        const float     ndl = glm::dot(n, keyDirection);
        if (ndl > 0.0f) {
            body += diffuseColor / kPi * ndl * keyColor;
        }
        // Transmission: what shows through the ball, bent once at its surface and tinted.
        if (transmission > 0.0f) {
            const glm::vec3 through = glm::refract(-view, n, 1.0f / std::max(m.ior, 1.0f));
            const float     blur = m.transmissionRoughness >= 0.0f ? m.transmissionRoughness : roughness;
            body = glm::mix(body, light.reflection(through, blur) * m.transmissionColor, transmission * (1.0f - metal));
        }

        // Specular: the blurred environment along the reflection, then the key light's lobe.
        glm::vec3 color = body + light.reflection(reflected, roughness) * environmentBrdf(f0, roughness, ndv) * lighting.specularIntensity;
        if (ndl > 0.0f) {
            const glm::vec3 h = glm::normalize(keyDirection + view);
            const float     ndh = std::max(glm::dot(n, h), 0.0f);
            const float     a = roughness * roughness;
            const float     d = a * a / (kPi * std::pow(ndh * ndh * (a * a - 1.0f) + 1.0f, 2.0f));
            const float     k = (roughness + 1.0f) * (roughness + 1.0f) / 8.0f;
            const float     g = (ndl / (ndl * (1.0f - k) + k)) * (ndv / (ndv * (1.0f - k) + k));
            const glm::vec3 f = f0 + (glm::vec3(1.0f) - f0) * std::pow(1.0f - std::max(glm::dot(h, view), 0.0f), 5.0f);
            color += f * (d * g / (4.0f * ndl * ndv)) * ndl * keyColor;
        }
        // The coat: a clear layer over all of it, with its own sharper reflection.
        if (coat > 0.0f) {
            const float coatRoughness = std::clamp(m.topCoatRoughness, 0.03f, 1.0f);
            const float reflects = environmentBrdf(glm::vec3(0.04f), coatRoughness, ndv).x * coat;
            color = color * (1.0f - reflects) + light.reflection(reflected, coatRoughness) * reflects * lighting.specularIntensity;
        }
        return color + emission;
    };

    const float pixelToBall = 2.0f / (static_cast<float>(size) * kMaterialBallShare);
    const float half = static_cast<float>(size) * 0.5f;
    parallelFor(size, [&](int y) {
        for (int x = 0; x < size; ++x) {
            glm::vec3 sum(0.0f); // display values, weighted by coverage
            int       covered = 0;
            for (int sy = 0; sy < kSubSamples; ++sy) {
                for (int sx = 0; sx < kSubSamples; ++sx) {
                    const float px = (static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / kSubSamples - half) * pixelToBall;
                    const float py = (half - (static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / kSubSamples)) * pixelToBall;
                    const float r2 = px * px + py * py;
                    if (r2 >= 1.0f) {
                        continue;
                    }
                    const glm::vec3 linear = shade(glm::vec3(px, py, std::sqrt(1.0f - r2))) * lighting.exposure;
                    for (int c = 0; c < 3; ++c) {
                        sum[c] += linearToSrgb(lighting.tonemap ? tonemap(linear[c]) : linear[c]);
                    }
                    ++covered;
                }
            }
            if (covered == 0) {
                continue;
            }
            std::uint8_t* p = &out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)) * 4u];
            for (int c = 0; c < 3; ++c) {
                p[c] = static_cast<std::uint8_t>(std::lround(std::clamp(sum[c] / static_cast<float>(covered), 0.0f, 1.0f) * 255.0f));
            }
            const float coverage = static_cast<float>(covered) / static_cast<float>(kSubSamples * kSubSamples);
            p[3] = static_cast<std::uint8_t>(std::lround(coverage * opacity * 255.0f));
        }
    });
    return out;
}

} // namespace pose
