#version 450

// Lit mesh vertex shader with linear-blend skinning. Each vertex is transformed by a weighted blend
// of its (up to 4) joint skin matrices (set 2 storage buffer), then by the per-draw model matrix and
// the per-frame camera view-projection. Static meshes bind a single identity joint and default to
// weight (1,0,0,0), so this same path leaves them untransformed. Passes a world-space normal, UV,
// and world position (the last feeds the specular view vector) to the fragment.

// Shared set-0 camera/lighting UBO. The fragment stage declares the full block (view + lighting rig +
// SH + params — see mesh.frag / scene.cpp's CameraUbo); the vertex stage only needs the view-projection,
// so it declares just that first member (std140 offset 0, so no other fields need to be spelled out).
layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
} cam;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColor; // rgb tint, a = opacity
    vec4 material;  // x = roughness, y = normalMode
} pc;

// Per-model skin matrices (poseGlobal * inverseBind per joint). Read-only in the vertex stage.
layout(std430, set = 2, binding = 0) readonly buffer Joints {
    mat4 skin[];
} joints;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;
layout(location = 5) in float inAo; // baked per-vertex ambient occlusion (1 = open)
layout(location = 6) in vec4 inTangent; // UV tangent + handedness (w = 0 -> none baked)

layout(location = 0) out vec3 vWorldNormal;
layout(location = 1) out vec2 vUv;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out float vAo;
layout(location = 4) out vec4 vTangent; // world-space tangent, w = handedness (0 = absent)

void main() {
    // Blend the influencing joints' skin matrices by weight. Weights sum to 1, so at bind pose (all
    // skin matrices identity) this is exactly the identity — the mesh renders as its rest geometry.
    mat4 skinMat = inWeights.x * joints.skin[inJoints.x] +
                   inWeights.y * joints.skin[inJoints.y] +
                   inWeights.z * joints.skin[inJoints.z] +
                   inWeights.w * joints.skin[inJoints.w];

    vec4 skinnedPos = skinMat * vec4(inPos, 1.0);
    vec4 worldPos = pc.model * skinnedPos;
    // mat3 of (model * skin) is correct for the rigid/uniform-scale transforms in play; swap to a
    // proper normal matrix if non-uniform scale ever appears.
    vWorldNormal = mat3(pc.model) * mat3(skinMat) * inNormal;
    vUv = inUv;
    vWorldPos = worldPos.xyz;
    vAo = inAo;
    vTangent = vec4(mat3(pc.model) * mat3(skinMat) * inTangent.xyz, inTangent.w);
    gl_Position = cam.viewProj * worldPos;
}
