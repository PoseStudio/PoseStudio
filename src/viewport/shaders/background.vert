// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
#version 450

// HDRI backdrop vertex stage: one fullscreen triangle whose fragments each get a world-space eye
// ray (near/far unprojection — the grid's technique), which the fragment stage points into the
// environment cubemap. Drawn FIRST in the main pass (depth test/write off) so everything else
// renders over it; PBR mode only (Scene::record gates it).
//
// Pass:    the HDR scene pass, first draw.
// Inputs:  set 0.0 the camera UBO (only viewProj is read; the shared block declares it all).
// Outputs: the per-pixel near/far world points for background.frag.

#extension GL_GOOGLE_include_directive : enable
#include "camera_ubo.glsl"
#include "fullscreen.glsl"

layout(location = 0) out vec3 vNearPoint;
layout(location = 1) out vec3 vFarPoint;

void main() {
    vec2 p = kTri[gl_VertexIndex];
    mat4 invViewProj = inverse(cam.viewProj);
    vec4 nearPt = invViewProj * vec4(p, 0.0, 1.0); // Vulkan clip z: 0 = near
    vec4 farPt  = invViewProj * vec4(p, 1.0, 1.0); // 1 = far
    vNearPoint = nearPt.xyz / nearPt.w;
    vFarPoint  = farPt.xyz / farPt.w;
    gl_Position = vec4(p, 0.0, 1.0);
}
