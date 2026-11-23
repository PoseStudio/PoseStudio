// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
#version 450

// Posing overlay: draws the skeleton as colored line segments (joint -> parent), transformed by the
// per-frame camera view-projection (the same set-0 UBO the mesh pipeline uses). Per-vertex color
// lets the selected joint be highlighted.
//
// Pass:    the HDR scene pass, last (the line overlays; depth test off).
// Inputs:  set 0.0 the camera UBO (only viewProj is read; the shared block declares it all);
//          vertex attributes: position + color (LineVertex).
// Outputs: vColor for skeleton.frag.

#extension GL_GOOGLE_include_directive : enable
#include "camera_ubo.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 vColor;

void main() {
    vColor = inColor;
    gl_Position = cam.viewProj * vec4(inPos, 1.0);
}
