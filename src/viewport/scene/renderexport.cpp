/**
 * @file renderexport.cpp
 * @brief The scene's vocabulary translated for a render engine. See renderexport.h.
 */

#include "renderexport.h"

#include "camera.h"
#include "cameraubo.h" // keyLightDirection
#include "lightingsettings.h"
#include "materialparams.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>

namespace pose {

namespace {

void set3(float out[3], const glm::vec3& v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

// The viewport's background, #3E4042, as linear values (VulkanRenderer's scene-pass clear).
const glm::vec3 kViewportGrey(0.04816f, 0.05125f, 0.05447f);
// The key light's color (fillCameraUbo's lightColor).
const glm::vec3 kKeyColor(1.0f, 0.96f, 0.9f);
// The distances light diffuses under skin, relative to red's, used where a material names none:
// red light travels furthest, blue hardly at all.
const glm::vec3 kSkinProfile(1.0f, 0.2f, 0.1f);
// Red's diffusion distance under skin where a material says nothing of its volume, metres.
constexpr float kDefaultScatterRadius = 0.005f;
// A scattering radius is kept within these, metres: under the first a channel does not scatter
// at all to a renderer, and over the second a body is wax.
constexpr float kMinScatterRadius = 0.0001f;
constexpr float kMaxScatterRadius = 0.02f;

/// How far light DIFFUSES under the surface before it comes back out, per channel, in metres —
/// what a renderer's skin-type subsurface takes for its radius.
///
/// A native figure's skin describes a VOLUME: how far light goes between two scatterings (the
/// scattering distance, with its direction) and what a given depth of the medium absorbs (the
/// transmission color at its depth: red hardly at all, green and blue most of it). The distance
/// light then diffuses is the diffusion length of that medium, 1 / sqrt(3 σa (σa + σs')) with
/// σs' = (1 − g) / distance and σa = −ln(color) / depth — which is where skin's red bleed comes
/// from: red is scattered as often as the others and absorbed a fortieth as much.
/// Elsewhere the scattering distance IS the radius, per channel by the scattering color (skin's
/// profile where it names none).
glm::vec3 scatterRadius(const MaterialParams& m, bool volumeAuthored) {
    const float     distance = std::max(m.subsurfaceRadius, 0.0f) * 0.01f; // cm -> m
    const glm::vec3 color = glm::clamp(m.subsurfaceRadiusColor, glm::vec3(0.0f), glm::vec3(1.0f));
    const float     peak = std::max(color.r, std::max(color.g, color.b));
    const bool      colored = peak > 0.02f && color != glm::vec3(1.0f); // (black and white both say "none named")
    glm::vec3       radius = kSkinProfile * kDefaultScatterRadius;
    if (distance > 0.0f && volumeAuthored && m.transmissionDepth > 0.0f) {
        const float     g = std::clamp(m.subsurfaceAnisotropy, -0.9f, 0.9f);
        const glm::vec3 scatter = glm::vec3((1.0f - g) / distance) * (colored ? glm::max(color, glm::vec3(0.05f)) : glm::vec3(1.0f));
        const glm::vec3 absorb = -glm::log(glm::clamp(m.transmissionColor, glm::vec3(1.0e-3f), glm::vec3(0.999f))) / m.transmissionDepth;
        radius = 1.0f / glm::sqrt(3.0f * absorb * (absorb + scatter));
    } else if (distance > 0.0f && !volumeAuthored) {
        radius = (colored ? glm::max(color, glm::vec3(0.01f)) : kSkinProfile) * distance;
    }
    return glm::clamp(radius, glm::vec3(kMinScatterRadius), glm::vec3(kMaxScatterRadius));
}

} // namespace

renderapi::Material renderMaterial(const MaterialParams& m, const MaterialParams& imported, const char* name,
                                   const RenderMaterialContext& context) {
    renderapi::Material out;
    out.name = name;

    // --- Base and specular ---
    set3(out.baseColor, m.baseEnabled ? m.baseColor : glm::vec3(0.0f));
    out.metallic = m.baseEnabled ? std::clamp(m.metalness, 0.0f, 1.0f) : 0.0f;
    out.diffuseRoughness = std::clamp(m.diffuseRoughness, 0.0f, 1.0f);
    out.specular = m.specularEnabled ? std::max(m.specular, 0.0f) : 0.0f;
    set3(out.specularColor, m.specularColor);
    out.roughness = std::clamp(m.roughness, 0.0f, 1.0f);
    out.ior = std::max(m.ior, 1.0f);
    out.anisotropy = std::clamp(m.anisotropy, 0.0f, 1.0f);
    out.anisotropyRotation = glm::radians(m.anisotropyRotation);

    // --- Transmission. The viewport draws a clear shell at a low opacity because it cannot
    // transmit through alpha alone; a renderer transmits, so a transmissive zone is solid to it
    // by as much as it transmits. ---
    const float transmission = m.transmissionEnabled ? std::clamp(m.transmission, 0.0f, 1.0f) : 0.0f;
    out.transmission = transmission;
    set3(out.transmissionColor, m.transmissionColor);
    out.transmissionDepth = std::max(m.transmissionDepth, 0.0f);
    out.transmissionRoughness = m.transmissionRoughness >= 0.0f ? std::min(m.transmissionRoughness, 1.0f) : -1.0f;
    if (m.thinWalled) {
        out.flags |= renderapi::kMaterialThinWalled;
    }
    out.opacity = std::clamp(m.opacity + (1.0f - m.opacity) * transmission, 0.0f, 1.0f);

    // --- Subsurface: the zone's translucency weight under the Subsurface dial (0.5, its
    // default, leaves the weight as authored). See RenderMaterialContext for which zones scatter.
    const bool scatters = m.subsurfaceEnabled && transmission < 0.999f &&
                          (context.translucencyAuthored || m.translucency != imported.translucency);
    if (scatters) {
        out.subsurface = std::clamp(m.translucency * context.subsurfaceDial * 2.0f, 0.0f, 1.0f);
        set3(out.subsurfaceRadius, scatterRadius(m, context.translucencyAuthored));
        out.subsurfaceAnisotropy = std::clamp(m.subsurfaceAnisotropy, -1.0f, 1.0f);
    }

    // --- Sheen, coat, thin film, emission ---
    if (m.sheenEnabled) {
        out.sheen = std::clamp(m.sheen, 0.0f, 1.0f);
        set3(out.sheenColor, m.sheenColor);
        out.sheenRoughness = std::clamp(m.sheenRoughness, 0.0f, 1.0f);
    }
    if (m.topCoatEnabled) {
        out.coat = std::clamp(m.topCoat, 0.0f, 1.0f);
        out.coatRoughness = std::clamp(m.topCoatRoughness, 0.0f, 1.0f);
        out.coatIor = std::max(m.coatIor, 1.0f);
        set3(out.coatColor, m.coatColor);
    }
    if (m.thinFilmEnabled && m.thinFilm > 0.0f) {
        out.thinFilmThickness = std::max(m.thinFilmThickness, 0.0f) * std::clamp(m.thinFilm, 0.0f, 1.0f);
        out.thinFilmIor = std::max(m.thinFilmIor, 1.0f);
    }
    if (m.emissionEnabled && m.emission > 0.0f) {
        set3(out.emissionColor, m.emissionColor);
        out.emissionStrength = m.emission;
    }

    // --- The surface maps' strengths and the UV transform ---
    out.normalStrength = m.geometryEnabled ? std::max(m.normalStrength, 0.0f) : 0.0f;
    out.bumpStrength = m.geometryEnabled ? m.bumpStrength : 0.0f;
    if (m.detailEnabled && m.detailWeight > 0.0f && m.detailTiles >= 1.0f) {
        out.detailWeight = std::clamp(m.detailWeight, 0.0f, 1.0f);
        out.detailTiling[0] = m.detailTiles;
        out.detailTiling[1] = m.detailTilesV > 0.0f ? m.detailTilesV : m.detailTiles;
        out.detailOffset[0] = m.detailOffset.x;
        out.detailOffset[1] = m.detailOffset.y;
    }
    if (m.tilingEnabled) {
        out.uvTiling[0] = m.uvTiling.x;
        out.uvTiling[1] = m.uvTiling.y;
        out.uvOffset[0] = m.uvOffset.x;
        out.uvOffset[1] = m.uvOffset.y;
        out.uvRotation = glm::radians(m.uvRotation);
    }
    return out;
}

renderapi::Camera renderCamera(const Camera& camera) {
    renderapi::Camera out;
    const glm::mat4   cameraToWorld = glm::inverse(camera.view());
    std::copy(glm::value_ptr(cameraToWorld), glm::value_ptr(cameraToWorld) + 16, out.cameraToWorld);
    out.fovY = camera.fovY();
    out.orthoHeight = camera.orthographic() ? camera.orthographicHeight() : 0.0f;
    out.nearClip = camera.nearPlane();
    out.farClip = camera.farPlane();
    return out;
}

renderapi::SunLight renderKeyLight(const LightingSettings& lighting) {
    renderapi::SunLight sun;
    set3(sun.direction, glm::normalize(keyLightDirection(lighting, /*pbrFollowsRotation=*/true)));
    set3(sun.color, kKeyColor);
    sun.strength = std::max(lighting.keyIntensity, 0.0f);
    // The Softness dial is the light's size: a crisp sun at 0 (one degree), a broad softbox's
    // eleven at 1.
    sun.angle = glm::radians(1.0f + 10.0f * std::clamp(lighting.shadowSoftness, 0.0f, 1.0f));
    return sun;
}

void renderEnvironment(const LightingSettings& lighting, float luminanceScale, renderapi::Environment& environment) {
    const float diffuse = std::max(lighting.diffuseIntensity, 0.0f);
    environment.strength = diffuse * luminanceScale;
    environment.ambient = std::max(lighting.ambientFill, 0.0f);
    environment.rotation = glm::radians(lighting.environmentRotationDeg);
    if (lighting.backdropMode != 0) {
        // The viewport draws the backdrop at its Brightness, whatever the Diffuse dial says.
        environment.backdrop = 1;
        environment.backdropStrength = diffuse > 1.0e-4f ? lighting.backdropBrightness / diffuse : 0.0f;
    } else {
        // No backdrop: the viewport's own grey, which its composite puts through the tonemap
        // like the rest of the frame — so it is given as the radiance that IS that grey under
        // the exposure the image will have, and the picture's display transform does the rest.
        environment.backdrop = 0;
        const float exposure = std::max(lighting.exposure, 1.0e-4f);
        set3(environment.backdropColor, kViewportGrey / exposure);
    }
}

float tonemapAces(float x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    x = std::max(x, 0.0f);
    return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
}

} // namespace pose
