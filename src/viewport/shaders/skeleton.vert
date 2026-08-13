#version 450

// Posing overlay: draws the skeleton as coloured line segments (joint -> parent), transformed by the
// per-frame camera view-projection (the same set-0 UBO the mesh pipeline uses). Per-vertex colour
// lets the selected joint be highlighted.

// Shared set-0 camera/lighting UBO (the same buffer the mesh pipeline fills). The overlay only needs
// the view-projection, so it declares just the first member — declaring a partial-but-misordered
// subset (e.g. skipping `view`) would put later fields at the wrong std140 offsets.
layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
} cam;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 vColor;

void main() {
    vColor = inColor;
    gl_Position = cam.viewProj * vec4(inPos, 1.0);
}
