#version 450

// The fullscreen composite: HDR scene + bloom, then ACES into the sRGB swapchain, then the
// selection outline over the top. In the PBR mode the scene passes arrive LINEAR (mesh/background
// no longer tonemap inline) — tonemapping lives here so bloom thresholds/blurs operate on real
// radiance. The stylized modes still author display-ready values, so they pass through untouched
// (tonemap + bloom both off).
//
// The selection outline is a UI element, not scene light: it is painted AFTER tonemapping so it
// keeps its exact theme colour whatever the exposure/tonemap dials do. uOutline is the selected
// object's coverage mask (OutlineMask, MSAA-resolved so its silhouette edge is fractional). The
// outline lives OUTSIDE the object: the mask is dilated by the outline width (the max over a ring
// of taps) and the outline weight is the excess of that dilation over the pixel's own coverage —
// zero inside the object, (1 − coverage) on its anti-aliased fringe (so outline and object blend
// exactly like the object blends with the background), full within the width, and softened by
// the mask's bilinear filtering along the outer edge.
//
// VR Mode draws through the same shader twice over: once per eye into the headset's swapchain
// (whose format may be UNORM — outline.w then asks for the sRGB encode the store cannot do), and
// once for the desktop MIRROR, which composites an eye's targets through a UV crop (uvRect) so
// the eye's near-square image fills the viewport's wider aspect without stretching.
//
// Pass:    the swapchain present pass (single-sample), the frame's only draw there.
// Inputs:  set 0.0 the (SSS-diffused) HDR resolve, 0.1 the bloom target, 0.2 the outline mask;
//          push: params (tonemap, bloom strength, bloom on, outline width px), the outline colour
//          (+ the sRGB-encode flag in .w), the UV crop (scale.xy, offset.xy).
// Outputs: location 0 -> the swapchain image (the sRGB store gamma-encodes, unless outline.w).

#extension GL_GOOGLE_include_directive : enable
#include "colour.glsl" // tonemapACES

layout(set = 0, binding = 0) uniform sampler2D uHdr;
layout(set = 0, binding = 1) uniform sampler2D uBloom;
layout(set = 0, binding = 2) uniform sampler2D uOutline;

layout(push_constant) uniform PC {
    vec4 params;  // x = tonemap (0/1), y = bloom strength, z = bloom on (0/1),
                  // w = selection-outline width in pixels (0 = no outline this frame)
    vec4 outline; // rgb = the outline colour, LINEAR (the swapchain's sRGB store encodes it);
                  // w = encode sRGB here (0/1) — for a target whose format cannot
    vec4 uvRect;  // xy = UV scale, zw = UV offset (1,1,0,0 = the whole source)
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

// The outline weight in [0,1] for this pixel: how much of the dilated selection mask exceeds
// the pixel's own coverage. Two rings of taps (16 at the full width, 8 at half) approximate the
// disc dilation; at the widths in use (2-3px) the 16-gon deviates from the disc by well under a
// tenth of a pixel.
float outlineWeight(vec2 uv, float widthPx) {
    float center = texture(uOutline, uv).r;
    if (center >= 0.999) {
        return 0.0; // fully inside the object: never outlined
    }
    vec2 texel = 1.0 / vec2(textureSize(uOutline, 0));
    float dilated = center;
    const float kTwoPi = 6.28318530718;
    for (int i = 0; i < 16; ++i) {
        float a = kTwoPi * (float(i) + 0.5) / 16.0;
        vec2 dir = vec2(cos(a), sin(a));
        dilated = max(dilated, texture(uOutline, uv + dir * texel * widthPx).r);
        if ((i & 1) == 0) {
            dilated = max(dilated, texture(uOutline, uv + dir * texel * widthPx * 0.5).r);
        }
    }
    return clamp(dilated - center, 0.0, 1.0);
}

// The sRGB transfer function (IEC 61966-2-1), for a UNORM target: what an _SRGB store does.
vec3 encodeSrgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, step(vec3(0.0031308), c));
}

void main() {
    vec2 uv = vUv * pc.uvRect.xy + pc.uvRect.zw;
    vec3 color = texture(uHdr, uv).rgb;
    color += texture(uBloom, uv).rgb * pc.params.y * pc.params.z;
    color = pc.params.x > 0.5 ? tonemapACES(color) : clamp(color, vec3(0.0), vec3(1.0));
    if (pc.params.w > 0.0) {
        color = mix(color, pc.outline.rgb, outlineWeight(uv, pc.params.w));
    }
    if (pc.outline.w > 0.5) {
        color = encodeSrgb(clamp(color, vec3(0.0), vec3(1.0)));
    }
    outColor = vec4(color, 1.0);
}
