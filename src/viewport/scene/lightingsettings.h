/**
 * @file lightingsettings.h
 * @brief The live, user-tunable lighting/environment parameters — the plain-data contract between
 *        the Environment properties panel (Qt UI) and the renderer (Qt-free core).
 *
 * These are all *shader-side* dials (fed through the per-frame camera UBO), so changing them is a
 * cheap UBO write with no IBL re-bake — the panel can drive them live. The one setting that does
 * require a re-bake, the HDRI environment itself, is handled separately (setEnvironmentFile). The
 * defaults reproduce the tuned default look, so constructing one and applying it changes nothing.
 * Pure POD (float/bool) so it crosses the Qt↔core boundary without dragging either side's types.
 */

#ifndef LIGHTINGSETTINGS_H
#define LIGHTINGSETTINGS_H

namespace pose {

struct LightingSettings {
    float exposure              = 0.68f; ///< Overall brightness multiplier applied before tonemapping.
    float diffuseIntensity      = 1.0f;  ///< Scales the environment (SH) diffuse contribution.
    float specularIntensity     = 0.10f; ///< Scales the prefiltered-environment specular reflection.
    float ambientFill           = 0.16f; ///< Flat diffuse lift so a directional HDRI's shadow side isn't black.
    float keyIntensity          = 0.85f; ///< Intensity of the single analytic key light layered over the IBL.
    float environmentRotationDeg = 0.0f; ///< Spins the environment about the vertical axis (degrees).
    // Key-light direction (the environment already lights the form; this is the movable accent).
    // Defaults match the old fixed direction vec3(0.62, 0.5, 0.55): front-right, ~30° above horizon.
    float keyAzimuthDeg         = 48.0f; ///< Around the vertical axis: 0 = +Z (camera home side).
    float keyElevationDeg       = 30.0f; ///< Above the horizon.
    // PBR-mode accents. Subsurface drives the per-channel wrapped diffuse (the warm terminator of
    // translucent skin); rim is the photographic back light lifting the dark side off the background.
    float subsurface            = 0.6f;  ///< 0 = opaque Lambert skin, 1 = strong translucent wrap.
    float rimIntensity          = 0.35f; ///< 0 = off.
    bool  tonemap               = true;  ///< ACES filmic tonemap on/off (off = clamp).
};

} // namespace pose

#endif // LIGHTINGSETTINGS_H
