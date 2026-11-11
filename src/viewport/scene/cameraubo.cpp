/**
 * @file cameraubo.cpp
 * @brief The camera UBO fill and the key-light direction. See cameraubo.h.
 */

#include "cameraubo.h"

#include "camera.h"
#include "environment.h"
#include "lightingsettings.h"
#include "selectionstyle.h"
#include "shademode.h"

#include <algorithm>
#include <cmath>

namespace pose {

glm::vec3 keyLightDirection(const LightingSettings& lighting, bool pbrFollowsRotation) {
    const float az = glm::radians(lighting.keyAzimuthDeg);
    const float el = glm::radians(lighting.keyElevationDeg);
    glm::vec3 dir(std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az));
    if (pbrFollowsRotation) {
        const float a = -glm::radians(lighting.environmentRotationDeg);
        const float c = std::cos(a);
        const float s = std::sin(a);
        dir = glm::vec3(c * dir.x + s * dir.z, dir.y, -s * dir.x + c * dir.z);
    }
    return dir;
}

void fillCameraUbo(CameraUbo& ubo, const Camera& camera, const LightingSettings& lighting,
                   const ShadeMode& spec, const EnvironmentSH& sh, const glm::mat4& lightViewProj,
                   const glm::vec3& keyDir, const SelectionStyle& selection) {
    // The fixed halves of the analytic rig never change, so normalize them once rather than
    // once per frame.
    static const glm::vec4 kFillDir(glm::normalize(glm::vec3(-0.6f, 0.28f, 0.5f)), 0.0f);  // fill: opposite, lower
    static const glm::vec4 kRimDir(glm::normalize(glm::vec3(-0.15f, 0.55f, -0.9f)), 0.0f); // back: behind + above

    ubo.viewProj = camera.viewProjection();
    ubo.view = camera.view();
    ubo.lightViewProj = lightViewProj; // fitted by the shadow pass earlier this frame
    ubo.cameraPos = glm::vec4(camera.position(), 1.0f);
    ubo.lightDir = glm::vec4(keyDir, 0.0f);
    ubo.lightColor = glm::vec4(1.0f, 0.96f, 0.9f, 0.0f);
    ubo.fillDir = kFillDir;
    ubo.fillColor = glm::vec4(0.26f, 0.31f, 0.4f, 0.0f); // dim + cool (~1/4 key)
    ubo.rimDir = kRimDir;
    ubo.rimColor = glm::vec4(1.2f, 1.28f, 1.45f, 0.0f);  // cool, localised to edges
    ubo.ambient = glm::vec4(0.10f, 0.11f, 0.13f, 0.0f);
    // params.x is mesh.frag's mode for the SURFACE (the picker row's fragMode — the picker index
    // itself never reaches the shader); a row without a shaded surface pushes 0, unread.
    ubo.params = glm::vec4(static_cast<float>(std::max(spec.fragMode, 0)), lighting.exposure,
                           lighting.specularIntensity, lighting.ambientFill);
    // params2.w: the joint highlight color's own luminance — the tint is the color over this,
    // times the surface's luminance (mesh.frag), so the part keeps its light and shadow.
    ubo.params2 = glm::vec4(lighting.diffuseIntensity, lighting.keyIntensity,
                            glm::radians(lighting.environmentRotationDeg),
                            jointTintLuminance(selection.jointColor));
    ubo.selection = glm::vec4(selection.jointColor, std::clamp(selection.jointOpacity, 0.0f, 1.0f));
    ubo.params3 = glm::vec4(lighting.subsurface, lighting.rimIntensity,
                            static_cast<float>(lighting.backdropMode), lighting.backdropBlur);
    ubo.params4 = glm::vec4(lighting.backdropBrightness, lighting.domeRadius,
                            lighting.shadowIntensity, spec.wireLevel);
    // Environment diffuse irradiance (SH). The PBR mode reconstructs per-normal ambient from these
    // instead of the flat `ambient` constant, so shadow sides pick up the environment's color.
    for (int i = 0; i < 9; ++i) {
        ubo.sh[i] = glm::vec4(sh.c[i], 0.0f);
    }
}

} // namespace pose
