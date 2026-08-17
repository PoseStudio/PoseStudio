#version 450

// The fullscreen composite: HDR scene + bloom, then ACES into the sRGB swapchain. In the PBR
// mode the scene passes arrive LINEAR (mesh/background no longer tonemap inline) — tonemapping
// lives here so bloom thresholds/blurs operate on real radiance. The stylized modes still author
// display-ready values, so they pass through untouched (tonemap + bloom both off).

layout(set = 0, binding = 0) uniform sampler2D uHdr;
layout(set = 0, binding = 1) uniform sampler2D uBloom;

layout(push_constant) uniform PC {
    vec4 params; // x = tonemap (0/1), y = bloom strength, z = bloom on (0/1)
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

vec3 tonemapACES(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 color = texture(uHdr, vUv).rgb;
    color += texture(uBloom, vUv).rgb * pc.params.y * pc.params.z;
    outColor = vec4(pc.params.x > 0.5 ? tonemapACES(color) : clamp(color, vec3(0.0), vec3(1.0)),
                    1.0);
}
