// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file viewpreset.h
 * @brief The named camera views the viewport's View picker lists and reports.
 *
 * Its own tiny header so the viewport strip (viewportstrip.h) can name the enum without pulling
 * in the Vulkan window's header — the input, IK and undo machinery the strip has no business
 * seeing. The strip and the window agree on exactly this one vocabulary: the window reports the
 * view it is in through VulkanWindow::viewPresetChanged, the strip asks for one through
 * ViewportStrip::viewSelected, and ViewportWidget wires the two.
 */

#ifndef VIEWPRESET_H
#define VIEWPRESET_H

namespace pose {

/// The named views the viewport's View picker lists and reports: the six axis views, Home (the
/// default perspective framing), and Free — the user orbited away from any named view (shown as
/// "Perspective"). Flip swaps Front/Back and Left/Right; frame-selected and zoom keep the view.
enum class ViewPreset { Free, Top, Bottom, Front, Back, Left, Right, Home };

} // namespace pose

#endif // VIEWPRESET_H
