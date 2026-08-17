#version 450

// Bloom bright-extract (half resolution): keeps only what's brighter than the threshold, with a
// soft knee so highlights ease into blooming instead of popping. Samples the resolved HDR scene.

layout(set = 0, binding = 0) uniform sampler2D uHdr;

layout(push_constant) uniform PC {
    vec4 params; // x = luminance threshold (post-exposure)
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 c = texture(uHdr, vUv).rgb;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Soft knee: nothing below threshold, full contribution one stop above it.
    float k = smoothstep(pc.params.x, pc.params.x + 1.0, lum);
    outColor = vec4(c * k, 1.0);
}
