#version 450

// Posing overlay: draws the skeleton as coloured line segments (joint -> parent), transformed by the
// per-frame camera view-projection (the same set-0 UBO the mesh pipeline uses). Per-vertex colour
// lets the selected joint be highlighted.

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
} cam;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 vColor;

void main() {
    vColor = inColor;
    gl_Position = cam.viewProj * vec4(inPos, 1.0);
}
