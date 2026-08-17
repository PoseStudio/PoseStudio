#version 450

// HDRI backdrop vertex stage: one fullscreen triangle whose fragments each get a world-space eye
// ray (near/far unprojection — the grid's technique), which the fragment stage points into the
// environment cubemap. Drawn FIRST in the main pass (depth test/write off) so everything else
// renders over it; PBR mode only (Scene::record gates it).

// Only the first member of the shared camera UBO is needed (std140 offset 0 — same partial-block
// pattern as mesh.vert).
layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
} cam;

layout(location = 0) out vec3 vNearPoint;
layout(location = 1) out vec3 vFarPoint;

// One triangle that covers the screen (no vertex buffers).
const vec2 kTri[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));

void main() {
    vec2 p = kTri[gl_VertexIndex];
    mat4 invViewProj = inverse(cam.viewProj);
    vec4 nearPt = invViewProj * vec4(p, 0.0, 1.0); // Vulkan clip z: 0 = near
    vec4 farPt  = invViewProj * vec4(p, 1.0, 1.0); // 1 = far
    vNearPoint = nearPt.xyz / nearPt.w;
    vFarPoint  = farPt.xyz / farPt.w;
    gl_Position = vec4(p, 0.0, 1.0);
}
