#version 450

// The DEPTH RESOLVE: the multisampled scene depth into a single-sample depth image of the same
// size — VR Mode's per-eye depth for the headset's reprojection
// (XR_KHR_composition_layer_depth). The NEAREST sample wins: at a silhouette the closer surface
// is what a reprojection should move with. (The photo pass's depth, which the Gaussian splats
// are tested against, is resolved by photoresolve.frag: the same rule over a block of texels,
// with the scene's coverage written beside it.)
//
// Pass:    a depth-only single-sample pass over the destination (cleared to 1.0, depth test
//          LESS + write: a far-plane fragment leaves the clear's 1.0, every nearer one lands).
// Inputs:  set 0.0 the HDR target's MSAA depth (sampler2DMS); push: params.x = the sample count.
// Outputs: gl_FragDepth only (no color attachment).

layout(set = 0, binding = 0) uniform sampler2DMS uDepth;

layout(push_constant) uniform PC {
    vec4 params; // x = sample count
} pc;

layout(location = 0) in vec2 vUv;

void main() {
    // Source and destination are the same size: the fragment's own texel (gl_FragCoord is its
    // centre, index + 0.5).
    const ivec2 texel = min(ivec2(gl_FragCoord.xy), textureSize(uDepth) - ivec2(1));
    const int   samples = int(pc.params.x);
    float depth = 1.0;
    for (int i = 0; i < samples; ++i) {
        depth = min(depth, texelFetch(uDepth, texel, i).r);
    }
    gl_FragDepth = depth;
}
