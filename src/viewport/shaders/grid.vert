#version 450

// Infinite ground-plane grid (Blender-style floor). Rather than drawing line geometry, we
// draw a single full-screen quad and reconstruct the world-space point on the y=0 plane in
// the fragment shader — so the grid is mathematically infinite and perfectly anti-aliased.
//
// Technique: unproject the quad's near- and far-plane points per pixel; the fragment shader
// then intersects that eye ray with the ground plane. See grid.frag.

layout(push_constant) uniform PC {
    mat4 viewProj; // camera view-projection; inverted here to unproject
} pc;

layout(location = 0) out vec3 vNearPoint; // world-space point on the near plane for this pixel
layout(location = 1) out vec3 vFarPoint;  // world-space point on the far plane for this pixel

// Full-screen quad in clip space (two triangles, CCW).
const vec3 kQuad[6] = vec3[](
    vec3(-1.0, -1.0, 0.0), vec3( 1.0, -1.0, 0.0), vec3( 1.0,  1.0, 0.0),
    vec3(-1.0, -1.0, 0.0), vec3( 1.0,  1.0, 0.0), vec3(-1.0,  1.0, 0.0)
);

vec3 unproject(vec2 ndcXY, float ndcZ, mat4 invViewProj) {
    vec4 world = invViewProj * vec4(ndcXY, ndcZ, 1.0);
    return world.xyz / world.w;
}

void main() {
    vec3 p = kQuad[gl_VertexIndex];
    mat4 invViewProj = inverse(pc.viewProj);
    // Vulkan clip-space depth is [0,1]: z=0 is the near plane, z=1 the far plane.
    vNearPoint = unproject(p.xy, 0.0, invViewProj);
    vFarPoint  = unproject(p.xy, 1.0, invViewProj);
    gl_Position = vec4(p, 1.0);
}
