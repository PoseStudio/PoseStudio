#version 450

// Lit mesh vertex shader. Transforms by the per-frame camera view-projection (set 0 UBO) and the
// per-draw model matrix (push constant), and passes a world-space normal + UV to the fragment.

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
} cam;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColor;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 vWorldNormal;
layout(location = 1) out vec2 vUv;

void main() {
    vec4 worldPos = pc.model * vec4(inPos, 1.0);
    // mat3(model) is correct for the rigid/uniform-scale transforms the importer uses; swap to a
    // proper normal matrix (transpose(inverse)) if non-uniform scale ever appears.
    vWorldNormal = mat3(pc.model) * inNormal;
    vUv = inUv;
    gl_Position = cam.viewProj * worldPos;
}
