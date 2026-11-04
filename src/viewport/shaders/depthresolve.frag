#version 450

// VR Mode's DEPTH RESOLVE: the eye's multisampled scene depth into the headset's single-sample
// depth swapchain image, so the runtime can reproject the frame against real geometry when a
// frame is late (XR_KHR_composition_layer_depth). The NEAREST sample wins — at a silhouette the
// closer surface is what the reprojection should move with.
//
// Pass:    the session's depth-only pass over its depth swapchain image (cleared to 1.0, depth
//          test LESS + write: a far-plane fragment leaves the clear's 1.0, every nearer one lands).
// Inputs:  set 0.0 the HDR target's MSAA depth (sampler2DMS; HdrTarget built with storeDepth);
//          push: params.x = the sample count.
// Outputs: gl_FragDepth only (no colour attachment).

layout(set = 0, binding = 0) uniform sampler2DMS uDepth;

layout(push_constant) uniform PC {
    vec4 params; // x = sample count
} pc;

layout(location = 0) in vec2 vUv;

void main() {
    const ivec2 texel = ivec2(gl_FragCoord.xy);
    const int   samples = int(pc.params.x);
    float depth = 1.0;
    for (int i = 0; i < samples; ++i) {
        depth = min(depth, texelFetch(uDepth, texel, i).r);
    }
    gl_FragDepth = depth;
}
