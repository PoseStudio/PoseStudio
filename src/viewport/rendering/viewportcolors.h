/**
 * @file viewportcolors.h
 * @brief The viewport's own colors that more than one layer must agree on. Header-only, no
 *        dependencies (std-free, GLM-free, Vulkan-free), so the engine core and the render
 *        export can both read it.
 *
 * (The selection's colors are a user preference and live in selectionstyle.h.)
 */

#ifndef VIEWPORTCOLORS_H
#define VIEWPORTCOLORS_H

namespace pose {

/// The viewport's flat background, #3E4042 = (62, 64, 66), as LINEAR values (each channel
/// through the sRGB decode). The scene pass clears to it — the HDR target is a linear format and
/// the composite's sRGB swapchain store does the final encode, so the displayed pixel comes back
/// out as #3E4042 where nothing tonemaps it — and a render's flat backdrop is derived from the
/// same three numbers (scene/renderexport.cpp), which is what makes a render's backdrop the
/// viewport's.
inline constexpr float kViewportGreyLinear[3] = {0.04816f, 0.05125f, 0.05447f};

} // namespace pose

#endif // VIEWPORTCOLORS_H
