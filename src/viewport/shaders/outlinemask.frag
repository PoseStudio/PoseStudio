// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
#version 450

// Selection-outline mask: every fragment of the selected object writes full coverage into the
// single-channel mask (see OutlineMask). The MSAA resolve turns the silhouette's partially
// covered edge pixels into fractional values, which the composite (composite.frag) dilates by
// the outline width and blends against — so the outline's inner edge matches the object's own
// anti-aliased fringe. Skinned + camera-projected by shadow.vert.
//
// Pass:    the outline-mask pass (OutlineMask; the context's MSAA count, no depth).
// Inputs:  none of its own (shadow.vert consumes the joint/corrective set + push block).
// Outputs: location 0 -> the R8 coverage attachment (resolved for the composite).

layout(location = 0) out float outCoverage;

void main() {
    outCoverage = 1.0;
}
