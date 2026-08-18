#version 450

// Screen-space subsurface scattering: one axis of a masked, per-channel-width Gaussian over the
// HDR scene (run twice, H then V). The source's ALPHA is the SSS mask the opaque mesh pass wrote
// (per-pixel translucency: skin ~0.9, cloth/eyes/backdrop 0) — it scales the kernel and weights
// every tap, so light diffuses within skin and never bleeds across the silhouette onto the
// background. Red diffuses farthest, then green, then blue — the diffusion profile that makes
// skin read as flesh (soft red shadow edges) instead of painted plastic.

layout(set = 0, binding = 0) uniform sampler2D uSrc;
layout(set = 0, binding = 1) uniform sampler2D uSpec; // scene SPECULAR (V pass only; H binds uSrc)

layout(push_constant) uniform PC {
    vec4 params; // xy = texel size, zw = blur direction (1,0) or (0,1)
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

const float kRadiusPx = 6.0;                     // kernel half-width at full mask, in pixels
const vec3  kFalloff  = vec3(2.0, 6.0, 10.0);    // per-channel Gaussian falloff (R widest)

void main() {
    // Only DIFFUSE light diffuses through skin — the mesh pass routes specular to its own target,
    // and the V (final) pass adds it back here, over the blurred result. Blurring specular smeared
    // every glint into a wet-looking film, which no amount of material tuning could dry out.
    vec3 specAdd = (pc.params.w > 0.5) ? texture(uSpec, vUv).rgb : vec3(0.0);
    vec4 center = texture(uSrc, vUv);
    if (center.a < 0.03) {
        outColor = vec4(center.rgb + specAdd, center.a); // not skin — untouched (plus its specular)
        return;
    }
    vec2 stepUv = pc.params.xy * pc.params.zw * kRadiusPx * center.a;
    vec3 sum = center.rgb;
    vec3 wsum = vec3(1.0);
    for (int i = 1; i <= 4; ++i) {
        float t = float(i) * 0.25; // tap distance fraction: 0.25 .. 1.0
        vec3 w = exp(-t * t * kFalloff);
        vec4 s1 = texture(uSrc, vUv + stepUv * t);
        vec4 s2 = texture(uSrc, vUv - stepUv * t);
        vec3 w1 = w * s1.a; // taps weighted by THEIR mask: no pulling from non-skin
        vec3 w2 = w * s2.a;
        sum += s1.rgb * w1 + s2.rgb * w2;
        wsum += w1 + w2;
    }
    outColor = vec4(sum / wsum + specAdd, center.a); // mask rides through for the second axis
}
