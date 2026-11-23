// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
#version 450

// Flat-color fragment shader for the skeleton posing overlay.

layout(location = 0) in vec3 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vColor, 1.0);
}
