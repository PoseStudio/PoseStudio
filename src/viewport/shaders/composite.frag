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
// Inputs:  set 0.0 the (SSS-diffused) HDR resolve, 0.1 the bloom target, 0.2 the outline mask,
//          0.3 the PHOTO layer (the Gaussian splats: premultiplied colour + coverage, sRGB-encoded);
//          push: params (tonemap, bloom strength, bloom on, outline width px), the outline colour
//          (+ the sRGB-encode flag in .w), the UV crop (scale.xy, offset.xy).
// Outputs: location 0 -> the swapchain image (the sRGB store gamma-encodes, unless outline.w).

#extension GL_GOOGLE_include_directive : enable
#include "colour.glsl" // tonemapACES

layout(set = 0, binding = 0) uniform sampler2D uHdr;
layout(set = 0, binding = 1) uniform sampler2D uBloom;
layout(set = 0, binding = 2) uniform sampler2D uOutline;
layout(set = 0, binding = 3) uniform sampler2D uPhoto;

layout(push_constant) uniform PC {
    vec4 params;  // x = tonemap (0/1), y = bloom strength, z = bloom on (0/1),
                  // w = selection-outline width in pixels (0 = no outline this frame)
    vec4 outline; // rgb = the outline colour, LINEAR (the swapchain's sRGB store encodes it);
                  // w = encode sRGB here (0/1) — for a target whose format cannot
    vec4 uvRect;  // xy = UV scale, zw = UV offset (1,1,0,0 = the whole source)
    vec4 photo;   // xy = the share of the photo layer drawn at its current scale (1,1 = all of it);
                  // z = 1 when the outline mask holds a SPLAT SCENE's accumulated opacity, which
                  // the outline reads through its 50% contour (a mesh's mask is coverage as is)
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

// The outline weight in [0,1] for this pixel: how much of the dilated selection mask exceeds
// the pixel's own coverage. Two rings of taps (16 at the full width, 8 at half) approximate the
// disc dilation; at the widths in use (2-3px) the 16-gon deviates from the disc by well under a
// tenth of a pixel.
// One tap of the selection mask. A mesh's mask is its coverage (0 or 1, fractional on the
// MSAA-resolved fringe) and reads as is; a splat scene's is its accumulated opacity, soft across
// the fuzzy edge and faint about the floaters, and reads through a steep step about 50% — so the
// outline is one line around the opacity's half contour, not a band around every hole and island
// of a soft edge.
float maskTap(vec2 uv) {
    float m = texture(uOutline, uv).r;
    return pc.photo.z > 0.5 ? smoothstep(0.4, 0.6, m) : m;
}

float outlineWeight(vec2 uv, float widthPx) {
    float center = maskTap(uv);
    if (center >= 0.999) {
        return 0.0; // fully inside the object: never outlined
    }
    vec2 texel = 1.0 / vec2(textureSize(uOutline, 0));
    float dilated = center;
    const float kTwoPi = 6.28318530718;
    for (int i = 0; i < 16; ++i) {
        float a = kTwoPi * (float(i) + 0.5) / 16.0;
        vec2 dir = vec2(cos(a), sin(a));
        dilated = max(dilated, maskTap(uv + dir * texel * widthPx));
        if ((i & 1) == 0) {
            dilated = max(dilated, maskTap(uv + dir * texel * widthPx * 0.5));
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

// Its inverse: an sRGB-encoded value back to linear.
vec3 decodeSrgb(vec3 c) {
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(lo, hi, step(vec3(0.04045), c));
}

// The photo layer's UV for a composite UV: the drawn share of the layer, held off its edge.
vec2 photoUvOf(vec2 uv) {
    return min(uv * pc.photo.xy, pc.photo.xy - 0.5 / vec2(textureSize(uPhoto, 0)));
}

// Catmull-Rom over the layer at its drawn scale: the 4x4 kernel as nine bilinear taps (the
// middle two weights of each axis merged into one tap between them), every tap held inside
// the drawn rectangle. Premultiplied colour + coverage interpolate as one; the kernel's slight
// overshoot is clamped so no colour exceeds its coverage.
vec4 samplePhotoCatmullRom(vec2 uv) {
    const vec2 size = vec2(textureSize(uPhoto, 0));
    const vec2 lo = vec2(0.5) / size;
    const vec2 hi = pc.photo.xy - lo;
    const vec2 p = uv * pc.photo.xy * size - 0.5; // in texels, the sample between texel centres
    const vec2 i = floor(p);
    const vec2 f = p - i;
    const vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const vec2 w3 = f * f * (-0.5 + 0.5 * f);
    const vec2 w12 = w1 + w2;
    const vec2 t0 = clamp((i - 0.5) / size, lo, hi);
    const vec2 t12 = clamp((i + 0.5 + w2 / w12) / size, lo, hi);
    const vec2 t3 = clamp((i + 2.5) / size, lo, hi);
    vec4 c = texture(uPhoto, vec2(t0.x, t0.y)) * (w0.x * w0.y) + texture(uPhoto, vec2(t12.x, t0.y)) * (w12.x * w0.y) +
             texture(uPhoto, vec2(t3.x, t0.y)) * (w3.x * w0.y) + texture(uPhoto, vec2(t0.x, t12.y)) * (w0.x * w12.y) +
             texture(uPhoto, vec2(t12.x, t12.y)) * (w12.x * w12.y) + texture(uPhoto, vec2(t3.x, t12.y)) * (w3.x * w12.y) +
             texture(uPhoto, vec2(t0.x, t3.y)) * (w0.x * w3.y) + texture(uPhoto, vec2(t12.x, t3.y)) * (w12.x * w3.y) +
             texture(uPhoto, vec2(t3.x, t3.y)) * (w3.x * w3.y);
    c.a = clamp(c.a, 0.0, 1.0);
    c.rgb = clamp(c.rgb, vec3(0.0), vec3(c.a));
    return c;
}

void main() {
    vec2 uv = vUv * pc.uvRect.xy + pc.uvRect.zw;
    vec3 color = texture(uHdr, uv).rgb;
    color += texture(uBloom, uv).rgb * pc.params.y * pc.params.z;
    color = pc.params.x > 0.5 ? tonemapACES(color) : clamp(color, vec3(0.0), vec3(1.0));
    // THE PHOTO LAYER: the Gaussian splats, accumulated in the HDR pass as premultiplied colour +
    // coverage in the trainers' own display-referred space, laid OVER the tonemapped scene in
    // that space — so a capture's photographed colours come out as they went into training,
    // whatever the lighting dials, and a figure inside it is covered exactly as the splats
    // cover her.
    // The layer is drawn into the sub-rectangle its scale picks (hdrtarget.h): sampled at
    // uv × that share, and never past the last texel drawn, whose bilinear neighbour outside
    // the rectangle holds whatever an earlier, larger scale left there. A layer drawn smaller
    // than the eye is enlarged here, and bilinear enlargement is soft: Catmull-Rom (nine
    // bilinear taps standing in for sixteen) keeps edges where they are at a fraction of the
    // sharpness lost. At the eye's own size the pixels and texels coincide and one tap is exact.
    vec4 photo = pc.photo.x < 0.98 ? samplePhotoCatmullRom(uv) : texture(uPhoto, photoUvOf(uv));
    if (photo.a > 0.0) {
        color = decodeSrgb(photo.rgb + (1.0 - photo.a) * encodeSrgb(clamp(color, vec3(0.0), vec3(1.0))));
    }
    if (pc.params.w > 0.0) {
        color = mix(color, pc.outline.rgb, outlineWeight(uv, pc.params.w));
    }
    if (pc.outline.w > 0.5) {
        color = encodeSrgb(clamp(color, vec3(0.0), vec3(1.0)));
    }
    outColor = vec4(color, 1.0);
}
