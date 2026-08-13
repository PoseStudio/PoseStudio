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
    float ambientFill           = 0.10f; ///< Flat diffuse lift so a directional HDRI's shadow side isn't black.
    float keyIntensity          = 1.0f;  ///< Intensity of the single analytic key light layered over the IBL.
    float environmentRotationDeg = 0.0f; ///< Spins the environment about the vertical axis (degrees).
    bool  tonemap               = true;  ///< ACES filmic tonemap on/off (off = clamp).
};

} // namespace pose

#endif // LIGHTINGSETTINGS_H
