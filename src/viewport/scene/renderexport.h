/**
 * @file renderexport.h
 * @brief The scene's own vocabulary translated for a render engine (render/renderengineapi.h):
 *        a zone's canonical material, the orbit camera, the lighting dials.
 *
 * THE RULES OF THE TRANSLATION live here, in one Qt-free place both the application's snapshot
 * (the window's captureRenderSnapshot) and a test can call: what a layer switch means to a
 * renderer, how the viewport's subsurface dial becomes a scattering weight, which sun stands for
 * the key light, what color the camera sees with the backdrop off. The TEXTURES are not here —
 * they are the window's (it owns the decoded images and paints the overlays into them).
 */

#ifndef RENDEREXPORT_H
#define RENDEREXPORT_H

#include "renderengineapi.h"

namespace pose {

class Camera;
struct LightingSettings;
struct MaterialParams;

/// What of a model's import says how its materials are to be read (see renderMaterial).
struct RenderMaterialContext {
    /// The Environment tab's Subsurface dial (LightingSettings::subsurface).
    float subsurfaceDial = 0.5f;
    /// Whether a zone's translucency weight is an AUTHORED subsurface weight. A native figure's
    /// is (its skin says 0.5 – 0.8, its cloth 0); the other formats carry none, and their zones
    /// arrive at the importers' default — a soft wrap in the viewport, which read as subsurface
    /// scattering would turn every prop to wax. For those a zone scatters only once its weight
    /// has been moved off the imported value.
    bool translucencyAuthored = false;
};

/// @p material as a renderer reads it: the VALUES (the maps are left empty). The layer switches
/// are applied — a layer that is off arrives with its weight at 0.
renderapi::Material renderMaterial(const MaterialParams& material, const MaterialParams& imported, const char* name,
                                   const RenderMaterialContext& context);

/// The viewport camera @p camera as a renderer's (its framing; the image's aspect is the
/// render's own).
renderapi::Camera renderCamera(const Camera& camera);

/// The key light as a sun: the direction the viewport's PBR mode lights from (it follows the
/// environment's rotation), at the viewport's own irradiance. Strength 0 with the key dialed out.
renderapi::SunLight renderKeyLight(const LightingSettings& lighting);

/// The lighting dials of @p environment that are not the panorama itself: its strength (the
/// Diffuse dial × @p luminanceScale, the factor the viewport normalized the panorama by — 1 for
/// an image handed over already normalized), the ambient fill, the rotation, and what the camera
/// sees — the panorama with the backdrop on, else the viewport's own background grey, given as
/// the radiance that is that grey under the exposure (the viewport tonemaps its background with
/// the rest of the frame, and so does a render's display transform).
void renderEnvironment(const LightingSettings& lighting, float luminanceScale, renderapi::Environment& environment);

/// The display transform the viewport's PBR mode ends with — the ACES fit of color.glsl.
float tonemapAces(float linear);

} // namespace pose

#endif // RENDEREXPORT_H
