#version 450

// THE RENDERED VIEWPORT MODE's picture (rendering/tracedview.h): a path-traced image of the scene,
// made by the render engine, drawn as the HDR scene pass's whole content in place of the
// rasterized scene. Linear radiance with the exposure already applied — what the scene passes
// write in the PBR mode — so the composite tonemaps it like any frame and paints the selection
// outline over it. Alpha 0: the HDR target's alpha is the subsurface mask, and a path-traced
// picture has done its own scattering.
//
// Pass:    the HDR scene pass, first (depth test/write off; the line overlays draw over it).
// Inputs:  set 0.0 the picture (RGBA32F, nearest: it is the window's size, texel for pixel).
// Outputs: location 0 -> the HDR color (location 1, the specular attachment, is masked off).

layout(set = 0, binding = 0) uniform sampler2D uTraced;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(max(texture(uTraced, vUv).rgb, vec3(0.0)), 0.0);
}
