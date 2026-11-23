// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
#version 450

// Separable 9-tap Gaussian blur for the bloom chain — run twice, horizontal then vertical
// (direction in the push constant).
//
// Pass:    the bloom render pass at half resolution, twice: H from bloom A into B, V from B
//          back into A (which the composite then samples).
// Inputs:  set 0.0 the previous stage's target; push: texel size + blur direction.
// Outputs: location 0 -> the pass's single RGBA16F attachment.

layout(set = 0, binding = 0) uniform sampler2D uSrc;

layout(push_constant) uniform PC {
    vec4 params; // xy = texel size, zw = blur direction (1,0) or (0,1)
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

const float kWeights[5] = float[](0.227027, 0.194594, 0.121621, 0.054054, 0.016216);

void main() {
    vec2 step = pc.params.xy * pc.params.zw;
    vec3 sum = texture(uSrc, vUv).rgb * kWeights[0];
    for (int i = 1; i < 5; ++i) {
        vec2 off = step * float(i) * 1.5; // slightly stretched taps: wider glow for the same cost
        sum += texture(uSrc, vUv + off).rgb * kWeights[i];
        sum += texture(uSrc, vUv - off).rgb * kWeights[i];
    }
    outColor = vec4(sum, 1.0);
}
