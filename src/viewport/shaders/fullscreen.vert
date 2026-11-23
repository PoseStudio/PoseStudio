// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
#version 450

// Shared fullscreen-triangle vertex stage for every fullscreen pass: the SSS blur (H and V), the
// bloom bright-extract and blur (H and V), the composite, the Rendered mode's picture
// (traced.frag), the photo pass's depth + coverage resolves (photoresolve.frag) and the headset's
// depth resolve (depthresolve.frag). One oversized triangle covers the screen; vUv is the [0,1]
// screen UV.
//
// Pass:    every PostProcess pass, TracedView, and HdrTarget's resolves.
// Inputs:  none (gl_VertexIndex into the fullscreen triangle).
// Outputs: vUv, the screen UV for the fragment stage.

#extension GL_GOOGLE_include_directive : enable
#include "fullscreen.glsl"

layout(location = 0) out vec2 vUv;

void main() {
    vec2 p = kTri[gl_VertexIndex];
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
