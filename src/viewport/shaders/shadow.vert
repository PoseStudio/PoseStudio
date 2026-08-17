#version 450

// Depth-only vertex shader for the key light's shadow map. Same linear-blend skinning as
// mesh.vert (so a posed figure casts its posed shadow), but transformed by the light's ortho
// view-projection instead of the camera's. In the shadow pipeline the joint storage buffer is
// bound at SET 0 (the pipeline's only set — same set object the main pass binds at set 2; the
// layouts match). Normal/UV attributes exist in the vertex layout but are simply not consumed.

layout(push_constant) uniform Push {
    mat4 lightViewProj; // key light's ortho view-projection (fitted by Scene::recordShadowPass)
    mat4 model;
} pc;

layout(std430, set = 0, binding = 0) readonly buffer Joints {
    mat4 skin[];
} joints;

layout(location = 0) in vec3 inPos;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;

void main() {
    mat4 skinMat = inWeights.x * joints.skin[inJoints.x] +
                   inWeights.y * joints.skin[inJoints.y] +
                   inWeights.z * joints.skin[inJoints.z] +
                   inWeights.w * joints.skin[inJoints.w];
    gl_Position = pc.lightViewProj * pc.model * (skinMat * vec4(inPos, 1.0));
}
