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
    float exposure              = 0.75f; ///< Overall brightness multiplier applied before tonemapping.
    float diffuseIntensity      = 0.5f;  ///< Scales the environment (SH) diffuse contribution.
    float specularIntensity     = 1.0f;  ///< Scales the prefiltered-environment specular reflection.
    float ambientFill           = 0.15f; ///< Flat diffuse lift so a directional HDRI's shadow side isn't black.
    float keyIntensity          = 1.25f; ///< Intensity of the single analytic key light layered over the IBL.
    float environmentRotationDeg = 30.0f; ///< Spins the environment about the vertical axis (degrees).
    // Key-light direction (the environment already lights the form; this is the movable accent).
    // Defaults: front-on (camera home side), high — a photographic butterfly-style key.
    float keyAzimuthDeg         = 0.0f;  ///< Around the vertical axis: 0 = +Z (camera home side).
    float keyElevationDeg       = 50.0f; ///< Above the horizon.
    // PBR-mode accents. Subsurface drives the per-channel wrapped diffuse (the warm terminator of
    // translucent skin); rim is the photographic back light lifting the dark side off the background.
    float subsurface            = 0.5f;  ///< 0 = opaque Lambert skin, 1 = strong translucent wrap.
    float rimIntensity          = 0.25f; ///< 0 = off.
    bool  tonemap               = true;  ///< ACES filmic tonemap on/off (off = clamp).
    // Backdrop (the visible environment behind the figure; PBR mode only — all shader-side dials).
    int   backdropMode          = 0;     ///< 0 = off (flat viewport grey — the default: the HDRI
                                         ///< lights the figure without being drawn behind it),
                                         ///< 1 = environment (infinite sphere), 2 = ground-projected
                                         ///< dome (the figure stands on the environment's floor
                                         ///< instead of a horizon at infinity).
    float backdropBlur          = 1.0f;  ///< Defocus in prefiltered-mip steps (0 = sharp).
    float backdropBrightness    = 1.0f;  ///< Backdrop-only exposure multiplier (the photographer's
                                         ///< "background a stop down" — subject lighting unchanged).
    float domeRadius            = 12.0f; ///< Dome mode: dome radius in world units (~metres).

    /// Field-wise equality, so callers can tell whether an edit actually changed anything (the
    /// Environment panel skips the undo entry for a no-op gesture). Exact float compares are right
    /// here: both sides come from the same widget values, never from computation.
    bool operator==(const LightingSettings& o) const {
        return exposure == o.exposure && diffuseIntensity == o.diffuseIntensity &&
               specularIntensity == o.specularIntensity && ambientFill == o.ambientFill &&
               keyIntensity == o.keyIntensity &&
               environmentRotationDeg == o.environmentRotationDeg &&
               keyAzimuthDeg == o.keyAzimuthDeg && keyElevationDeg == o.keyElevationDeg &&
               subsurface == o.subsurface && rimIntensity == o.rimIntensity &&
               tonemap == o.tonemap && backdropMode == o.backdropMode &&
               backdropBlur == o.backdropBlur && backdropBrightness == o.backdropBrightness &&
               domeRadius == o.domeRadius;
    }
    bool operator!=(const LightingSettings& o) const { return !(*this == o); }
};

} // namespace pose

#endif // LIGHTINGSETTINGS_H
