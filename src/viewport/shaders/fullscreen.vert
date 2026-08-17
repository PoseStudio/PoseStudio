#version 450

// Shared fullscreen-triangle vertex stage for every post-processing pass (bloom bright/blur, the
// composite). One oversized triangle covers the screen; vUv is the [0,1] screen UV.

layout(location = 0) out vec2 vUv;

const vec2 kTri[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));

void main() {
    vec2 p = kTri[gl_VertexIndex];
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
