#version 450

// THE PHOTO PASS'S RESOLVE: the multisampled scene depth into the single-sample depth the
// Gaussian splats are tested against (hdrtarget.h) — the NEAREST sample, as depthresolve.frag
// takes it for the headset's depth layer: a splat behind the figure's edge must not show
// through her — and, beside it, the scene's COVERAGE of the texel: the share of its samples a
// depth-writing surface covers (nearer than the far plane's clear).
//
// The coverage is what the composite needs at a SILHOUETTE. The scene is resolved from its
// samples, so a texel the figure half covers holds half her color and half the BACKDROP's; and
// tested against the nearest sample, every splat behind her is rejected for the whole texel.
// The composite then showed that texel as it stood — her edge blended with the viewport's
// grey, a dark stair-stepped line between her and a capture behind her. With the coverage the
// composite takes the backdrop's share back out and puts the capture's in its place
// (composite.frag, THE EDGE).
//
// TWO READINGS, by the block: at the scene's own size (one source texel a texel) the output is
// the SHARE of the texel's samples covered — quarters at 4x — which is what the composite takes
// the backdrop out by. Over a block of several (the layer at a reduced scale) it is whether ANY
// sample of the block is covered, 0 or 1: such a texel of the layer was TOUCHED by the scene —
// its depth is a surface's, so every splat behind that surface is missing from it — and the
// composite reads the capture behind an edge off the untouched texels beside it. The same shader
// then runs once more at the scene's size, coverage only (hdrtarget.cpp), for the share.
//
// Pass:    the photo pass's resolve (hdrtarget.cpp): one R8 color attachment (cleared 0) over
//          the photo depth (cleared to 1.0, depth test LESS + write: a far-plane fragment
//          leaves the clear's 1.0, every nearer one lands).
// Inputs:  set 0.0 the HDR target's MSAA depth (sampler2DMS); push: params.x = the sample
//          count, params.y = the source texels per destination texel (a photo layer at a
//          reduced scale: the nearest, and the coverage, over the whole block that maps to the
//          texel; 0 or 1 = one).
// Outputs: gl_FragDepth (dropped by the coverage-only pass, which has no depth attachment);
//          location 0 -> the coverage, 0 (open) .. 1 (covered) — a share, or touched (above).

layout(set = 0, binding = 0) uniform sampler2DMS uDepth;

layout(push_constant) uniform PC {
    vec4 params; // x = sample count, y = source texels per destination texel (0 / 1 = one)
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out float outCoverage;

void main() {
    const float ratio = pc.params.y > 0.0 ? pc.params.y : 1.0;
    const int   block = max(int(ceil(ratio - 1e-4)), 1);
    const ivec2 last = textureSize(uDepth) - ivec2(1);
    // The block destination texel p covers starts at source texel p * ratio, read off the
    // texel's INDEX (see depthresolve.frag).
    const ivec2 base = min(ivec2(floor(gl_FragCoord.xy - 0.5) * ratio), last);
    const int   samples = int(pc.params.x);
    float depth = 1.0;
    float covered = 0.0;
    for (int y = 0; y < block; ++y) {
        for (int x = 0; x < block; ++x) {
            const ivec2 texel = min(base + ivec2(x, y), last);
            for (int i = 0; i < samples; ++i) {
                const float d = texelFetch(uDepth, texel, i).r;
                depth = min(depth, d);
                covered += d < 1.0 ? 1.0 : 0.0;
            }
        }
    }
    gl_FragDepth = depth;
    outCoverage = block > 1 ? (covered > 0.0 ? 1.0 : 0.0) : covered / float(samples);
}
