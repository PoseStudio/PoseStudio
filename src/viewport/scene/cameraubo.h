/**
 * @file cameraubo.h
 * @brief The per-frame camera + lighting uniform block (descriptor set 0) and the code that
 *        fills it from the camera, the Environment panel's dials, and the baked environment.
 *
 * The block is the one contract between the CPU and every scene shader that reads set 0
 * (mesh.vert/frag, background.frag, skeleton.vert): its std140 layout must match the `CameraUbo`
 * declaration in those shaders field for field. Keeping the struct and its fill routine in one
 * Qt-free file means a new dial is added in exactly two places — here and the shader — and the
 * Scene just memcpy's the result into this frame's mapped buffer. The analytic 3-point rig
 * (key/fill/rim) still lives here for the non-IBL shade modes; the PBR mode reads only the key,
 * the SH irradiance, and the dials.
 */

#ifndef CAMERAUBO_H
#define CAMERAUBO_H

#include <glm/glm.hpp>

#include <vector>

namespace pose {

class Camera;
struct EnvironmentSH;
struct LightingSettings;
struct SceneLight;
struct SelectionStyle;
struct ShadeMode;

/// One LIGHT OBJECT as the shaders read it (scenelight.h; camera_ubo.glsl's SceneLightUbo).
struct LightUbo {
    glm::vec4 position;  // xyz = where it is (world); w = its kind (0 point, 1 spot, 2 area, 3 sun)
    glm::vec4 direction; // xyz = the way it shines (unit); w = the least squared distance it is
                         //       evaluated at (its radius squared: no singularity on its surface)
    glm::vec4 color;     // rgb = its color × intensity (linear: the irradiance a metre away);
                         //       w = a spot's cos(half cone)
    glm::vec4 params;    // x = a spot's edge: 1 / ((1 - cos half cone) × blend); yzw spare
};
/// How many of them the block carries (= kMaxViewportLights, scenelight.h).
inline constexpr int kCameraUboLights = 8;

/// std140-compatible camera + lighting block. vec3 fields are padded to vec4. Must match the
/// `CameraUbo` block declared in mesh.vert / mesh.frag. Layout (std140) must match both shaders.
struct CameraUbo {
    glm::mat4 viewProj;
    glm::mat4 view;          // world -> view (fragment modes derive view-space normals for matcaps)
    glm::mat4 lightViewProj; // key light's ortho view-projection (shadow-map space; see ShadowPass)
    glm::vec4 cameraPos;     // xyz world-space eye
    glm::vec4 lightDir;    // xyz = normalized direction TO the key light
    glm::vec4 lightColor;  // rgb
    glm::vec4 fillDir;     // fill light (dir TO light)
    glm::vec4 fillColor;   // rgb (intensity baked in)
    glm::vec4 rimDir;      // rim / back light (dir TO light)
    glm::vec4 rimColor;    // rgb (intensity baked in)
    glm::vec4 ambient;     // rgb ambient/fill term
    glm::vec4 params;      // x = shade mode, y = exposure, z = specularIntensity, w = ambientFill
    glm::vec4 sh[9];       // environment diffuse irradiance: 9 SH coefficients (rgb in .xyz)
    glm::vec4 params2;     // x = diffuseIntensity, y = keyIntensity, z = envRotation(rad),
                           // w = the joint highlight color's luminance (jointTintLuminance: what
                           // mesh.frag divides the tint by to match the surface's own)
    glm::vec4 params3;     // x = subsurface, y = rimIntensity, z = backdropMode, w = backdropBlur
    glm::vec4 params4;     // x = backdropBrightness, y = domeRadius, z = shadowIntensity,
                           // w = the wireframe overlay's linear grey (ShadeMode::wireLevel)
    glm::vec4 selection;   // the selected-joint highlight (SelectionStyle): rgb = its color, linear;
                           // a = its opacity at a full skin weight
    glm::vec4 lightInfo;   // x = how many of `lights` are in use; yzw spare
    LightUbo  lights[kCameraUboLights]; // the scene's light objects (the first enabled ones)
};

/// The normalized world-space direction TO the key light from the Environment panel's
/// azimuth/elevation dials (world-fixed relative to the subject; the defaults reproduce the old
/// hardcoded front-right ~30° direction). With @p pbrFollowsRotation the direction additionally
/// follows the panel's environment Rotation dial: in the image-based (PBR) mode the key stands in
/// for the environment's dominant light — the HDRI bake auto-aims the dials at it — so it must
/// turn with the visible environment. The shader samples the environment at
/// rotateY(worldDir, +rot) (mesh.frag), so the world direction matching a fixed environment-space
/// direction is rotateY(dir, -rot) with the same rotation convention, replicated here. The
/// analytic modes keep the dial as a plain world-space direction (their rig ignores the
/// environment).
glm::vec3 keyLightDirection(const LightingSettings& lighting, bool pbrFollowsRotation);

/// Fills @p ubo for one frame: the camera matrices, the fitted light matrix, the key direction
/// (@p keyDir, from keyLightDirection), the fixed fill/rim/ambient rig, the shade mode's fragment
/// mode + wire level, every live dial, the environment's SH irradiance, and the selected-joint
/// highlight's color and opacity (@p selection — the outline half of it is the composite's).
///
/// The default (PBR) mode is image-based-lit — its diffuse comes from the environment's SH
/// irradiance and its specular from the prefiltered-environment cubemap (set 3). The analytic
/// lights feed only the *non-IBL* shade modes: only Rendered (mode 0) uses the full key/fill/rim
/// rig; the other analytic modes use just the key + ambient, and PBR uses only the key (scaled by
/// the panel's keyIntensity). The rig is world-space (fixed relative to the subject, so lighting
/// stays consistent as the camera orbits); dirs point TO each light — Fill = opposite the key,
/// lower + dimmer (opens the shadow side); Back/rim = behind + above (rims the shoulders so the
/// figure pops off the grid).
void fillCameraUbo(CameraUbo& ubo, const Camera& camera, const LightingSettings& lighting,
                   const ShadeMode& spec, const EnvironmentSH& sh, const glm::mat4& lightViewProj,
                   const glm::vec3& keyDir, const SelectionStyle& selection);

/// The scene's LIGHT OBJECTS into @p ubo: the first kCameraUboLights enabled ones of @p lights,
/// each as the PBR mode shades with it (mesh.frag) — unshadowed; a render traces them all.
void fillCameraUboLights(CameraUbo& ubo, const std::vector<SceneLight>& lights);

} // namespace pose

#endif // CAMERAUBO_H
