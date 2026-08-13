/**
 * @file environment.h
 * @brief The lighting environment: an equirectangular HDR radiance image plus the baked
 *        image-based-lighting (IBL) data derived from it.
 *
 * PoseStudio lights the viewport with IBL rather than a hand-tuned analytic rig: a whole
 * environment illuminates the subject, giving soft wrap light, a natural rim, and (with the
 * prefiltered maps) real glossy reflections that stay consistent as the camera orbits. The
 * default environment is generated procedurally (a neutral studio) so no HDR asset needs to
 * ship; a real `.hdr` panorama can replace it. This file owns the *CPU* side — the source
 * radiance image and the bakes computed from it (spherical-harmonic diffuse irradiance, and
 * later the prefiltered specular cubemap + BRDF LUT). Pure std + GLM: no Qt, no Vulkan.
 */

#ifndef ENVIRONMENT_H
#define ENVIRONMENT_H

#include <glm/glm.hpp>

#include <array>
#include <vector>

namespace pose {

/// Equirectangular HDR radiance image (linear RGB; values may exceed 1 for lights). Row 0 is the
/// +Y pole, u wraps around the azimuth. All IBL bakes are computed from one of these.
struct EnvironmentImage {
    int                    width  = 0;
    int                    height = 0;
    std::vector<glm::vec3> pixels; ///< width*height, row-major.

    const glm::vec3& texel(int x, int y) const { return pixels[static_cast<std::size_t>(y) * width + x]; }
    /// Bilinear lookup along a world-space direction (need not be normalized). Wraps in u, clamps in v.
    glm::vec3 sample(const glm::vec3& dir) const;
};

/// Builds a neutral studio environment procedurally (soft overhead softbox + graded surround + a
/// dim warm floor bounce), so the default lighting needs no shipped HDR. Values are HDR (the
/// softbox and key exceed 1) so the environment actually drives the exposure, not just tints ambient.
EnvironmentImage generateStudioEnvironment(int width = 512, int height = 256);

/// Scales @p env so its solid-angle-weighted average luminance equals @p targetAverageLuminance.
/// Different HDRIs (a bright outdoor sky vs. a dim interior) otherwise light the subject at wildly
/// different exposures; normalizing here makes the *diffuse* result predictable and HDRI-independent,
/// so a single exposure control then dials the final brightness. No-op for an empty/black image.
void normalizeEnvironmentLuminance(EnvironmentImage& env, float targetAverageLuminance);

/// Nine RGB spherical-harmonic coefficients (the L_lm projections of the environment radiance,
/// solid-angle weighted). Evaluated in the shader to reconstruct per-normal diffuse irradiance —
/// exact for a Lambertian response and cheaper than sampling an irradiance cubemap.
struct EnvironmentSH {
    std::array<glm::vec3, 9> c{};
};

/// Projects @p env's radiance onto the first 9 real SH basis functions. The returned coefficients
/// are the raw L_lm; the shader folds in the cosine-lobe convolution constants when it reconstructs
/// irradiance E(n), so the CPU side stays a plain projection.
EnvironmentSH projectIrradianceSH(const EnvironmentImage& env);

/// A GGX-prefiltered specular cubemap, baked on the CPU: `faces[face][mip]` holds the RGBA texels of
/// one cube face at one mip. Mip 0 is a sharp reflection (roughness 0); each successive mip is
/// prefiltered for a higher roughness (mip/(mipCount-1)), so the shader samples mip = roughness ·
/// (mipCount-1) to get a roughness-appropriate environment reflection (the specular half of split-sum).
struct PrefilteredSpecular {
    int                                              baseSize = 0;
    int                                              mipCount = 0;
    std::vector<std::vector<std::vector<glm::vec4>>> faces; // [6][mip][texel], row-major per face
};

/// Prefilters @p env into a specular cubemap. @p baseSize is the mip-0 face resolution; @p mipCount
/// the roughness levels. @p samples is the GGX importance-sample count for the rough mips.
PrefilteredSpecular prefilterSpecular(const EnvironmentImage& env, int baseSize = 128, int mipCount = 5,
                                      int samples = 128);

/// The environment BRDF integration LUT (the other half of split-sum): at (NdotV, roughness) it stores
/// the scale (.x) and bias (.y) to apply to F0. Environment-independent, so it's baked once. `data` is
/// row-major, size×size, y = roughness, x = NdotV.
struct BrdfLut {
    int                    size = 0;
    std::vector<glm::vec2> data;
};

/// Numerically integrates the environment BRDF into a @p size × @p size LUT with @p samples per texel.
BrdfLut integrateBrdfLut(int size = 128, int samples = 512);

/// The CPU-baked, view-independent IBL products for one environment — SH diffuse irradiance and the
/// prefiltered specular cubemap — ready for GPU upload. Produced off the render thread by
/// bakeEnvironment() and consumed on the render thread by Scene::applyBakedEnvironment(); the
/// environment-independent BRDF LUT is baked (and cached) separately, so it isn't carried here.
struct BakedEnvironment {
    EnvironmentSH       sh;
    PrefilteredSpecular specular;
};

/// Normalizes @p env's brightness (so any HDRI lights at a consistent exposure) and bakes its SH
/// irradiance + prefiltered specular. Pure CPU (no Vulkan, no Qt) — safe to run on a worker thread,
/// which is how interactive HDRI switches stay off the UI thread. Takes @p env by value: it normalizes
/// its own copy, so the caller's image (and other threads) are untouched.
BakedEnvironment bakeEnvironment(EnvironmentImage env);

} // namespace pose

#endif // ENVIRONMENT_H
