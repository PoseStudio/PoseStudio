/**
 * @file environment.cpp
 * @brief Implementation of the procedural studio environment and the SH irradiance projection.
 *        See environment.h.
 */

#include "environment.h"

#include <algorithm>
#include <cmath>

namespace pose {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Maps a pixel's (theta, phi) to a world-space direction. theta in [0,pi] measured from +Y (the top
// row), phi in [0,2pi) the azimuth. Kept identical to sample()'s inverse mapping so the procedurally
// generated pixels and the SH projection agree on which direction each texel represents.
glm::vec3 dirFromAngles(float theta, float phi) {
    const float sinT = std::sin(theta);
    return glm::vec3(sinT * std::sin(phi), std::cos(theta), -sinT * std::cos(phi));
}

// The 9 real spherical-harmonic basis values (bands 0..2) along a unit direction, in the order the
// shader's irradiance reconstruction expects: [Y00, Y1-1, Y10, Y11, Y2-2, Y2-1, Y20, Y21, Y22].
void shBasis(const glm::vec3& d, float b[9]) {
    b[0] = 0.282095f;
    b[1] = 0.488603f * d.y;
    b[2] = 0.488603f * d.z;
    b[3] = 0.488603f * d.x;
    b[4] = 1.092548f * d.x * d.y;
    b[5] = 1.092548f * d.y * d.z;
    b[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
    b[7] = 1.092548f * d.x * d.z;
    b[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

} // namespace

glm::vec3 EnvironmentImage::sample(const glm::vec3& d) const {
    if (pixels.empty()) {
        return glm::vec3(0.0f);
    }
    const glm::vec3 dir = glm::normalize(d);
    const float theta = std::acos(std::clamp(dir.y, -1.0f, 1.0f)); // 0 = up (+Y)
    float phi = std::atan2(dir.x, -dir.z);
    if (phi < 0.0f) {
        phi += 2.0f * kPi;
    }
    const float fx = (phi / (2.0f * kPi)) * width - 0.5f;
    const float fy = (theta / kPi) * height - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0;
    const float ty = fy - y0;
    const auto wrapX = [&](int x) { x %= width; return x < 0 ? x + width : x; };
    const auto clampY = [&](int y) { return std::clamp(y, 0, height - 1); };
    const glm::vec3 c00 = texel(wrapX(x0), clampY(y0));
    const glm::vec3 c10 = texel(wrapX(x0 + 1), clampY(y0));
    const glm::vec3 c01 = texel(wrapX(x0), clampY(y0 + 1));
    const glm::vec3 c11 = texel(wrapX(x0 + 1), clampY(y0 + 1));
    return glm::mix(glm::mix(c00, c10, tx), glm::mix(c01, c11, tx), ty);
}

EnvironmentImage generateStudioEnvironment(int width, int height) {
    EnvironmentImage env;
    env.width = width;
    env.height = height;
    env.pixels.resize(static_cast<std::size_t>(width) * height);

    // A neutral photographic studio: a cool, softly graded surround (brighter overhead, mid at the
    // horizon), a broad bright overhead softbox that is the dominant source, a gentle warm key from
    // the front, and a dim warm floor bounce. Tuned so the *diffuse* result reads clean and slightly
    // cool with soft shadowing, like a product/portrait studio — not a flat grey ambient.
    const glm::vec3 zenith(0.55f, 0.60f, 0.70f);   // cool sky overhead
    const glm::vec3 horizon(0.40f, 0.42f, 0.46f);  // neutral mid at the horizon
    const glm::vec3 floorCol(0.14f, 0.12f, 0.10f); // dim warm floor
    const glm::vec3 softbox(1.00f, 0.99f, 0.96f);  // broad overhead source (slightly warm-white)
    const glm::vec3 keyCol(1.00f, 0.95f, 0.88f);   // warm key
    const float softboxIntensity = 2.6f;
    const float keyIntensity = 1.8f;
    const glm::vec3 keyDir = glm::normalize(glm::vec3(0.45f, 0.55f, 0.70f)); // front-upper-right

    for (int y = 0; y < height; ++y) {
        const float theta = (y + 0.5f) / height * kPi;
        for (int x = 0; x < width; ++x) {
            const float phi = (x + 0.5f) / width * 2.0f * kPi;
            const glm::vec3 dir = dirFromAngles(theta, phi);
            const float up = dir.y;

            glm::vec3 col = (up >= 0.0f)
                                ? glm::mix(horizon, zenith, std::pow(std::clamp(up, 0.0f, 1.0f), 0.6f))
                                : glm::mix(horizon, floorCol, std::pow(std::clamp(-up, 0.0f, 1.0f), 0.5f));

            // Broad overhead softbox: a smooth bright cap near the top, the dominant light.
            const float box = glm::smoothstep(0.30f, 0.95f, up);
            col += softbox * (box * softboxIntensity);

            // Soft warm key from the front-upper-right for a little directionality.
            const float k = std::max(glm::dot(dir, keyDir), 0.0f);
            col += keyCol * (std::pow(k, 6.0f) * keyIntensity);

            env.pixels[static_cast<std::size_t>(y) * width + x] = col;
        }
    }
    return env;
}

void normalizeEnvironmentLuminance(EnvironmentImage& env, float targetAverageLuminance) {
    if (env.pixels.empty() || targetAverageLuminance <= 0.0f) {
        return;
    }
    // Solid-angle weighted average (equirect over-samples the poles, so weight rows by sin(theta)).
    double weighted = 0.0;
    double weight = 0.0;
    for (int y = 0; y < env.height; ++y) {
        const double sinT = std::sin((y + 0.5) / env.height * kPi);
        for (int x = 0; x < env.width; ++x) {
            const glm::vec3& c = env.texel(x, y);
            const double lum = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
            weighted += lum * sinT;
            weight += sinT;
        }
    }
    const double avg = (weight > 0.0) ? weighted / weight : 0.0;
    if (avg <= 1e-6) {
        return;
    }
    const float scale = static_cast<float>(targetAverageLuminance / avg);
    for (glm::vec3& p : env.pixels) {
        p *= scale;
    }
}

EnvironmentSH projectIrradianceSH(const EnvironmentImage& env) {
    EnvironmentSH sh;
    if (env.pixels.empty()) {
        return sh;
    }
    // Solid-angle-weighted projection: each texel covers dφ·dθ·sinθ steradians. Accumulate in double
    // to keep precision over the ~130k-texel sum, then store as float coefficients.
    glm::dvec3 acc[9] = {};
    const double dPhi = 2.0 * kPi / env.width;
    const double dTheta = kPi / env.height;
    for (int y = 0; y < env.height; ++y) {
        const float theta = (y + 0.5f) / env.height * kPi;
        const double dOmega = dPhi * dTheta * std::sin(theta);
        for (int x = 0; x < env.width; ++x) {
            const float phi = (x + 0.5f) / env.width * 2.0f * kPi;
            const glm::vec3 dir = dirFromAngles(theta, phi);
            const glm::vec3 radiance = env.texel(x, y);
            float b[9];
            shBasis(dir, b);
            for (int i = 0; i < 9; ++i) {
                acc[i] += glm::dvec3(radiance) * (static_cast<double>(b[i]) * dOmega);
            }
        }
    }
    for (int i = 0; i < 9; ++i) {
        sh.c[i] = glm::vec3(acc[i]);
    }
    return sh;
}

namespace {

// Low-discrepancy Hammersley point i of N (van der Corput radical inverse in y).
glm::vec2 hammersley(uint32_t i, uint32_t n) {
    uint32_t bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return glm::vec2(static_cast<float>(i) / n, static_cast<float>(bits) * 2.3283064365386963e-10f);
}

// GGX importance-sampled half-vector around normal @p n for the given roughness.
glm::vec3 importanceSampleGGX(const glm::vec2& xi, const glm::vec3& n, float roughness) {
    const float a = roughness * roughness;
    const float phi = 2.0f * kPi * xi.x;
    const float cosT = std::sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    const float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
    const glm::vec3 h(sinT * std::cos(phi), sinT * std::sin(phi), cosT); // tangent space
    const glm::vec3 up = std::abs(n.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
    const glm::vec3 t = glm::normalize(glm::cross(up, n));
    const glm::vec3 b = glm::cross(n, t);
    return glm::normalize(t * h.x + b * h.y + n * h.z);
}

// World-space direction for cube face @p face at texel (@p u,@p v) in [-1,1] (standard cube layout).
glm::vec3 cubeDir(int face, float u, float v) {
    switch (face) {
        case 0: return glm::normalize(glm::vec3(1.0f, -v, -u));  // +X
        case 1: return glm::normalize(glm::vec3(-1.0f, -v, u));  // -X
        case 2: return glm::normalize(glm::vec3(u, 1.0f, v));    // +Y
        case 3: return glm::normalize(glm::vec3(u, -1.0f, -v));  // -Y
        case 4: return glm::normalize(glm::vec3(u, -v, 1.0f));   // +Z
        default: return glm::normalize(glm::vec3(-u, -v, -1.0f)); // -Z
    }
}

} // namespace

PrefilteredSpecular prefilterSpecular(const EnvironmentImage& env, int baseSize, int mipCount, int samples) {
    PrefilteredSpecular out;
    out.baseSize = baseSize;
    out.mipCount = mipCount;
    out.faces.assign(6, {});
    for (int face = 0; face < 6; ++face) {
        out.faces[face].resize(mipCount);
        for (int mip = 0; mip < mipCount; ++mip) {
            const int size = std::max(1, baseSize >> mip);
            const float roughness = (mipCount > 1) ? static_cast<float>(mip) / (mipCount - 1) : 0.0f;
            std::vector<glm::vec4>& dst = out.faces[face][mip];
            dst.resize(static_cast<std::size_t>(size) * size);
            for (int y = 0; y < size; ++y) {
                const float v = 2.0f * (y + 0.5f) / size - 1.0f;
                for (int x = 0; x < size; ++x) {
                    const float u = 2.0f * (x + 0.5f) / size - 1.0f;
                    const glm::vec3 n = cubeDir(face, u, v);
                    glm::vec3 color(0.0f);
                    if (roughness <= 0.0f) {
                        color = env.sample(n); // mirror reflection: sample the environment directly
                    } else {
                        // Split-sum prefilter: importance-sample GGX around N (== V == R), accumulate
                        // env radiance weighted by N·L (Karis 2013).
                        const glm::vec3 vv = n;
                        float weight = 0.0f;
                        for (int s = 0; s < samples; ++s) {
                            const glm::vec3 h = importanceSampleGGX(hammersley(s, samples), n, roughness);
                            const glm::vec3 l = glm::normalize(2.0f * glm::dot(vv, h) * h - vv);
                            const float ndl = glm::dot(n, l);
                            if (ndl > 0.0f) {
                                color += env.sample(l) * ndl;
                                weight += ndl;
                            }
                        }
                        color = (weight > 0.0f) ? color / weight : env.sample(n);
                    }
                    dst[static_cast<std::size_t>(y) * size + x] = glm::vec4(color, 1.0f);
                }
            }
        }
    }
    return out;
}

BrdfLut integrateBrdfLut(int size, int samples) {
    BrdfLut lut;
    lut.size = size;
    lut.data.resize(static_cast<std::size_t>(size) * size);
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    for (int y = 0; y < size; ++y) {
        const float roughness = (y + 0.5f) / size;
        for (int x = 0; x < size; ++x) {
            const float ndv = std::max((x + 0.5f) / size, 1e-3f);
            const glm::vec3 vv(std::sqrt(1.0f - ndv * ndv), 0.0f, ndv);
            float a = 0.0f, b = 0.0f;
            // Smith geometry, IBL variant: k = roughness^2 / 2.
            const float k = (roughness * roughness) * 0.5f;
            for (int s = 0; s < samples; ++s) {
                const glm::vec3 h = importanceSampleGGX(hammersley(s, samples), n, roughness);
                const glm::vec3 l = glm::normalize(2.0f * glm::dot(vv, h) * h - vv);
                const float ndl = std::max(l.z, 0.0f);
                const float ndh = std::max(h.z, 0.0f);
                const float vdh = std::max(glm::dot(vv, h), 0.0f);
                if (ndl > 0.0f) {
                    const float gv = ndv / (ndv * (1.0f - k) + k);
                    const float gl = ndl / (ndl * (1.0f - k) + k);
                    const float gVis = (gv * gl) * vdh / std::max(ndh * ndv, 1e-4f);
                    const float fc = std::pow(1.0f - vdh, 5.0f);
                    a += (1.0f - fc) * gVis;
                    b += fc * gVis;
                }
            }
            lut.data[static_cast<std::size_t>(y) * size + x] = glm::vec2(a, b) / static_cast<float>(samples);
        }
    }
    return lut;
}

BakedEnvironment bakeEnvironment(EnvironmentImage env) {
    // Normalize to a fixed average luminance so a bright outdoor HDRI and a dim interior light the
    // subject at the same exposure (the shader's exposure dial then sets the final level).
    constexpr float kTargetAverageLuminance = 0.5f;
    normalizeEnvironmentLuminance(env, kTargetAverageLuminance);
    BakedEnvironment baked;
    baked.sh = projectIrradianceSH(env);
    baked.specular = prefilterSpecular(env);
    return baked;
}

} // namespace pose
