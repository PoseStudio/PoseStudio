#version 450

// The DEPTH RESOLVE: the multisampled scene depth into a single-sample depth image — VR Mode's
// per-eye depth for the headset's reprojection (XR_KHR_composition_layer_depth). The NEAREST
// sample wins: at a silhouette the closer surface is what a reprojection should move with.
// (The photo pass's depth, which the Gaussian splats are tested against, is resolved the same
// way by photoresolve.frag, which writes the scene's coverage beside it.)
//
// Pass:    a depth-only single-sample pass over the destination (cleared to 1.0, depth test
//          LESS + write: a far-plane fragment leaves the clear's 1.0, every nearer one lands).
// Inputs:  set 0.0 the HDR target's MSAA depth (sampler2DMS); push: params.x = the sample
//          count, params.y = the source texels per destination texel (a photo layer at a
//          reduced scale: the nearest over the whole block that maps to the texel; 0 or 1 = one).
// Outputs: gl_FragDepth only (no color attachment).

layout(set = 0, binding = 0) uniform sampler2DMS uDepth;

layout(push_constant) uniform PC {
    vec4 params; // x = sample count, y = source texels per destination texel (0 / 1 = one)
} pc;

layout(location = 0) in vec2 vUv;

void main() {
    const float ratio = pc.params.y > 0.0 ? pc.params.y : 1.0;
    const int   block = max(int(ceil(ratio - 1e-4)), 1);
    const ivec2 last = textureSize(uDepth) - ivec2(1);
    // The block destination texel p covers starts at source texel p · ratio — read off the texel's
    // INDEX (gl_FragCoord is its centre, p + 0.5: scaled as it stands, at ratio 2 the block began a
    // source texel late and the splats were depth-tested a scene pixel off, 2026-11-07). At ratio 1
    // the two agree.
    const ivec2 base = min(ivec2(floor(gl_FragCoord.xy - 0.5) * ratio), last);
    const int   samples = int(pc.params.x);
    float depth = 1.0;
    for (int y = 0; y < block; ++y) {
        for (int x = 0; x < block; ++x) {
            const ivec2 texel = min(base + ivec2(x, y), last);
            for (int i = 0; i < samples; ++i) {
                depth = min(depth, texelFetch(uDepth, texel, i).r);
            }
        }
    }
    gl_FragDepth = depth;
}
