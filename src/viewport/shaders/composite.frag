#version 450

// The fullscreen composite: HDR scene + bloom, then ACES into the sRGB swapchain, then the
// selection outline over the top. In the PBR mode the scene passes arrive LINEAR (mesh/background
// no longer tonemap inline) — tonemapping lives here so bloom thresholds/blurs operate on real
// radiance. The stylized modes still author display-ready values, so they pass through untouched
// (tonemap + bloom both off).
//
// The selection outline is a UI element, not scene light: it is painted AFTER tonemapping so it
// keeps its exact theme color whatever the exposure/tonemap dials do. uOutline is the selected
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
// THE EDGE (2026-11-12) — a scene silhouette in front of a Gaussian splat scene. The scene is
// drawn multisampled and RESOLVED before this shader sees it: a pixel the figure half covers
// holds half her color and half the BACKDROP's. The splats are a single-sample layer tested
// against the NEAREST of the pixel's depth samples, so every splat behind her is kept out of
// that whole pixel. Laid together as they stood, the pixel showed her edge blended with the
// viewport's grey between her skin and the capture: a dark, stair-stepped line around her
// wherever a capture stood behind her. With the scene's COVERAGE of the pixel (uCover,
// photoresolve.frag: the share of its samples a surface covers) the pixel is put together as
// its samples would have been:
//     her own color      = (the resolve − (1 − cover) · the backdrop) / cover, in linear light,
//     the covered share  = that color, tonemapped,
//     the open share     = the capture (over the backdrop where it is see-through),
// with the backdrop and the capture read off the nearest OPEN pixels (coverage 0: one ring out,
// two if none) — what lies behind her edge is what shows beside it, a pixel away. Where no
// capture is behind the edge (its coverage there is 0) the pixel is the resolve as ever, and a
// silhouette nowhere near a capture is untouched to the bit. The splats in FRONT of her are
// this pixel's own photo texel, laid over the result as always.
//
// WITH THE LAYER SCALED DOWN (VR Mode's lever, edge.x = 2) the same defect is a band: a texel
// of the layer spans a block of scene pixels and is tested against the nearest depth of the
// whole block, so a texel the silhouette merely TOUCHES holds no splat from behind it — the
// backdrop showed around her for the width of a block, in steps. There uCover says which
// texels were touched (photoresolve.frag) and uCoverFull is the scene's coverage pixel for
// pixel. Where a pixel's 4x4 texel footprint holds both kinds, the layer is read as TWO: the
// touched texels' mean is what stands in FRONT of the scene here, the untouched texels' mean
// is the capture as it would be seen past her; an open pixel takes the second, a covered one
// the first over the scene, and a pixel she covers in part is put together as above. (Cubic
// B-spline weights within the band — all positive, so each kind's mean is well defined —
// where the rest of the layer is enlarged by Catmull-Rom.)
//
// Pass:    the swapchain present pass (single-sample), the frame's only draw there.
// Inputs:  set 0.0 the (SSS-diffused) HDR resolve, 0.1 the bloom target, 0.2 the outline mask,
//          0.3 the PHOTO layer (the Gaussian splats: premultiplied color + coverage, sRGB-encoded),
//          0.4 the scene's COVERAGE per texel of the photo layer and 0.5 per pixel (read only
//          with edge.x set: photoresolve.frag has the two readings);
//          push: params (tonemap, bloom strength, bloom on, outline width px), the outline color
//          (+ the sRGB-encode flag in .w), the UV crop (scale.xy, offset.xy), the photo layer's
//          share + the outline's two, the edge mode.
// Outputs: location 0 -> the swapchain image (the sRGB store gamma-encodes, unless outline.w).

#extension GL_GOOGLE_include_directive : enable
#include "color.glsl" // tonemapACES

layout(set = 0, binding = 0) uniform sampler2D uHdr;
layout(set = 0, binding = 1) uniform sampler2D uBloom;
layout(set = 0, binding = 2) uniform sampler2D uOutline;
layout(set = 0, binding = 3) uniform sampler2D uPhoto;
layout(set = 0, binding = 4) uniform sampler2D uCover;
layout(set = 0, binding = 5) uniform sampler2D uCoverFull;

layout(push_constant) uniform PC {
    vec4 params;  // x = tonemap (0/1), y = bloom strength, z = bloom on (0/1),
                  // w = selection-outline width in pixels (0 = no outline this frame)
    vec4 outline; // rgb = the outline color, LINEAR (the swapchain's sRGB store encodes it);
                  // w = encode sRGB here (0/1) — for a target whose format cannot
    vec4 uvRect;  // xy = UV scale, zw = UV offset (1,1,0,0 = the whole source)
    vec4 photo;   // xy = the share of the photo layer drawn at its current scale (1,1 = all of it);
                  // z = 1 when the outline mask holds a SPLAT SCENE's accumulated opacity, which
                  // the outline reads through its 50% contour (a mesh's mask is coverage as is);
                  // w = the outline's opacity (1 = the color itself over the frame)
    vec4 edge;    // x = THE EDGE's mode: 0 off (no coverage this frame; the mirror); 1 = uCover is
                  // the scene's coverage, texel for texel with uHdr and uPhoto; 2 = the photo
                  // layer is scaled down: uCover marks its touched texels, uCoverFull is the
                  // scene's coverage
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
// the drawn rectangle. Premultiplied color + coverage interpolate as one; the kernel's slight
// overshoot is clamped so no color exceeds its coverage.
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

// The scene's linear value to the display's linear one: ACES for the PBR family, the stylized
// modes as they are.
vec3 toneOf(vec3 c) {
    return pc.params.x > 0.5 ? tonemapACES(c) : clamp(c, vec3(0.0), vec3(1.0));
}

// A pixel counts as OPEN under this coverage and as wholly covered over one minus it (the
// coverage comes in steps of the sample count: a quarter at 4x).
const float kOpen = 0.1;

// The eight neighbours of a pixel, the four nearest first.
const vec2 kRing[8] = vec2[8](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0),
                              vec2(1.0, 1.0), vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(-1.0, -1.0));

// The scene's coverage of a pixel (by the mode: with the layer scaled, uCover is per block).
// (textureLod throughout THE EDGE: its taps sit under non-uniform control flow, where an
// implicit level of detail is undefined.)
float sceneCover(vec2 uv) {
    return pc.edge.x > 1.5 ? textureLod(uCoverFull, uv, 0.0).r : textureLod(uCover, uv, 0.0).r;
}

// The scene (linear) — and, where wanted, the photo layer — at the OPEN pixels nearest a pixel:
// the mean over the ring one out, else two. False where none is open within two.
bool openAround(vec2 uv, bool withCapture, out vec3 behind, out vec4 capture) {
    const vec2 texel = 1.0 / vec2(textureSize(uHdr, 0));
    behind = vec3(0.0);
    capture = vec4(0.0);
    float open = 0.0;
    for (int ring = 1; ring <= 2 && open == 0.0; ++ring) {
        for (int i = 0; i < 8; ++i) {
            const vec2 at = uv + kRing[i] * texel * float(ring);
            if (sceneCover(at) < kOpen) {
                behind += textureLod(uHdr, at, 0.0).rgb;
                if (withCapture) {
                    capture += textureLod(uPhoto, photoUvOf(at), 0.0);
                }
                open += 1.0;
            }
        }
    }
    if (open == 0.0) {
        return false;
    }
    behind /= open;
    capture /= open;
    return true;
}

// The display-space scene for a pixel the scene covers by `cover`, with the capture `capture`
// (premultiplied color + coverage) behind her and the scene `behind` (linear) at the open
// pixels beside her: her own color is the resolve less the backdrop's share of it, over her
// share (at least a sample of hers is in it, so `cover` is a quarter or more: nothing is
// amplified far). By the capture's own coverage: none = the resolve as it stands (`here`);
// whole = her share of her color and the open share of the capture's.
vec3 edgeBlend(float cover, vec3 hdr, vec3 bloom, vec3 here, vec3 behind, vec4 capture) {
    const vec3 own = max((hdr - (1.0 - cover) * behind) / cover, vec3(0.0));
    const vec3 ownShown = encodeSrgb(toneOf(own + bloom));
    return (1.0 - capture.a) * here + capture.a * cover * ownShown + (1.0 - cover) * capture.rgb;
}

// THE EDGE with the layer at the scene's size (see the file comment): the display-space scene
// for a pixel the scene covers by `cover`. False — `shown` unset — where no open pixel is
// within two, or no capture is behind: the pixel then stays the resolve.
bool edgeScene(vec2 uv, float cover, vec3 hdr, vec3 bloom, vec3 toned, out vec3 shown) {
    vec3 behind;
    vec4 capture;
    if (!openAround(uv, true, behind, capture) || capture.a <= 0.0) {
        return false;
    }
    capture.a = min(capture.a, 1.0);
    shown = edgeBlend(cover, hdr, bloom, encodeSrgb(clamp(toned, vec3(0.0), vec3(1.0))), behind, capture);
    return true;
}

// THE EDGE with the layer SCALED DOWN: the whole pixel, display-space, the splats laid in.
// False — `laid` unset — outside the band (the pixel's 4x4 texel footprint is all touched or
// all untouched: the layer is whole there and is enlarged as ever).
bool edgeScaled(vec2 uv, vec3 hdr, vec3 bloom, vec3 toned, out vec3 laid) {
    const vec2  size = vec2(textureSize(uPhoto, 0));
    const ivec2 last = ivec2(pc.photo.xy * size + 0.5) - ivec2(1); // the last texel drawn
    const vec2  lo = vec2(0.5) / size;
    const vec2  hi = pc.photo.xy - lo;
    const vec2  p = uv * pc.photo.xy * size - 0.5; // in texels, the sample between texel centres
    const vec2  i = floor(p);
    const vec2  f = p - i;
    // The band: four bilinear taps, each between four texels, together the 4x4 footprint.
    float touched = 0.0;
    for (int y = 0; y <= 2; y += 2) {
        for (int x = 0; x <= 2; x += 2) {
            touched += textureLod(uCover, clamp((i + vec2(x, y)) / size, lo, hi), 0.0).r;
        }
    }
    touched *= 0.25;
    if (touched < 0.01 || touched > 0.99) {
        return false;
    }
    // The layer read as two: the touched texels (what is in FRONT of the scene) and the open
    // ones (the capture as seen past it), each a cubic B-spline mean over its own texels.
    const vec2 f2 = f * f;
    const vec2 f3 = f2 * f;
    vec2 w[4];
    w[0] = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    w[1] = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    w[2] = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    w[3] = f3 / 6.0;
    vec4  front = vec4(0.0);
    vec4  past = vec4(0.0);
    float frontWeight = 0.0;
    float pastWeight = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const ivec2 at = clamp(ivec2(i) + ivec2(x - 1, y - 1), ivec2(0), last);
            const float weight = w[x].x * w[y].y;
            const vec4  texel = texelFetch(uPhoto, at, 0);
            if (texelFetch(uCover, at, 0).r > 0.5) {
                front += weight * texel;
                frontWeight += weight;
            } else {
                past += weight * texel;
                pastWeight += weight;
            }
        }
    }
    if (pastWeight < 1e-4) {
        return false; // (no open texel carries weight here: nothing to go by)
    }
    past /= pastWeight;
    past.a = min(past.a, 1.0);
    front = frontWeight > 1e-4 ? front / frontWeight : past;
    front.a = min(front.a, 1.0);
    const vec3  here = encodeSrgb(clamp(toned, vec3(0.0), vec3(1.0)));
    const float cover = textureLod(uCoverFull, uv, 0.0).r;
    if (cover < kOpen) {
        laid = past.rgb + (1.0 - past.a) * here; // an open pixel: the capture as it is past her
        return true;
    }
    vec3 under = here; // the scene under what stands in front of it
    if (cover < 1.0 - kOpen && past.a > 0.0) {
        vec3 behind;
        vec4 unused;
        if (openAround(uv, false, behind, unused)) {
            under = edgeBlend(cover, hdr, bloom, here, behind, past);
        }
    }
    laid = front.rgb + (1.0 - front.a) * under;
    return true;
}

void main() {
    vec2 uv = vUv * pc.uvRect.xy + pc.uvRect.zw;
    const vec3 hdr = texture(uHdr, uv).rgb;
    const vec3 bloom = texture(uBloom, uv).rgb * pc.params.y * pc.params.z;
    vec3 color = toneOf(hdr + bloom);
    // THE PHOTO LAYER: the Gaussian splats, accumulated in the HDR pass as premultiplied color +
    // coverage in the trainers' own display-referred space, laid OVER the tonemapped scene in
    // that space — so a capture's photographed colors come out as they went into training,
    // whatever the lighting dials, and a figure inside it is covered exactly as the splats
    // cover her.
    // The layer is drawn into the sub-rectangle its scale picks (hdrtarget.h): sampled at
    // uv × that share, and never past the last texel drawn, whose bilinear neighbour outside
    // the rectangle holds whatever an earlier, larger scale left there. A layer drawn smaller
    // than the eye is enlarged here, and bilinear enlargement is soft: Catmull-Rom (nine
    // bilinear taps standing in for sixteen) keeps edges where they are at a fraction of the
    // sharpness lost. At the eye's own size the pixels and texels coincide and one tap is exact.
    vec4 photo = pc.photo.x < 0.98 ? samplePhotoCatmullRom(uv) : texture(uPhoto, photoUvOf(uv));
    // THE EDGE: a pixel the scene covers in part, with a capture behind it — or, with the
    // layer scaled down, any pixel of the band along her silhouette.
    bool edged = false;
    vec3 shown = vec3(0.0);
    if (pc.edge.x > 1.5) {
        if (edgeScaled(uv, hdr, bloom, color, shown)) {
            edged = true;
            photo = vec4(0.0); // (the splats are laid into `shown` already)
        }
    } else if (pc.edge.x > 0.5) {
        const float cover = textureLod(uCover, uv, 0.0).r;
        if (cover > kOpen && cover < 1.0 - kOpen) {
            edged = edgeScene(uv, cover, hdr, bloom, color, shown);
        }
    }
    if (edged) {
        color = decodeSrgb(photo.rgb + (1.0 - photo.a) * shown);
    } else if (photo.a > 0.0) {
        color = decodeSrgb(photo.rgb + (1.0 - photo.a) * encodeSrgb(clamp(color, vec3(0.0), vec3(1.0))));
    }
    if (pc.params.w > 0.0) {
        color = mix(color, pc.outline.rgb, outlineWeight(uv, pc.params.w) * pc.photo.w);
    }
    if (pc.outline.w > 0.5) {
        color = encodeSrgb(clamp(color, vec3(0.0), vec3(1.0)));
    }
    outColor = vec4(color, 1.0);
}
