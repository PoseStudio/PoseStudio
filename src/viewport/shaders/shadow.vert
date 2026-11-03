#version 450

// Position-only vertex shader for the projected passes that need no materials: the key light's
// shadow map (paired with shadow.frag, transformed by the light's ortho view-projection) and
// the selection-outline mask (paired with outlinemask.frag, transformed by the CAMERA's
// view-projection). Same DUAL-QUATERNION skinning as mesh.vert — literally the same code, via
// skinning.glsl (so a posed figure casts its posed shadow and outlines its posed silhouette —
// the blends MUST match or the figure self-shadows against a differently-deformed caster). In
// both pipelines the joint storage buffer is bound at SET 0 (the pipeline's only set — same set
// object the main pass binds at set 2; the layouts match). Normal/UV attributes exist in the
// vertex layout but are simply not consumed. The POSE-CORRECTIVE blend (set bindings 1 + 2,
// attribute 7) is applied before the skinning exactly as in mesh.vert, through correctives.glsl
// — the shadow and the outline must trace the corrected surface.
//
// Pass:    the shadow-map pass (depth only) and the outline-mask pass (coverage only).
// Inputs:  set 0.0-2 the per-model joint DQs + corrective weights/deltas; push: the pass's
//          projection + the model matrix (ShadowPushConstants); vertex attributes 0, 3, 4, 7.
// Outputs: gl_Position only.

#extension GL_GOOGLE_include_directive : enable
#define SKIN_SET 0 // the joint + corrective buffers are this pipeline's only set
#include "skinning.glsl"
#include "correctives.glsl"

layout(push_constant) uniform Push {
    mat4 viewProj; // the pass's projection: the fitted light matrix (shadow) or the camera's (outline)
    mat4 model;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;
layout(location = 7) in uint inCorrectiveRange; // (first delta entry << 8) | count; 0 = none

void main() {
    vec3 bindPos = applyCorrectives(inPos, inCorrectiveRange);
    vec3 skinnedPos;
    if (linearSkinning()) { // (the model's rig was authored for it: see skinning.glsl)
        skinnedPos = linearSkinPoint(inJoints, inWeights, bindPos);
    } else {
        vec4 rAcc;
        vec4 dAcc;
        blendDualQuat(inJoints, inWeights, rAcc, dAcc);
        skinnedPos = dualQuatTransform(rAcc, dAcc, bindPos);
    }
    gl_Position = pc.viewProj * pc.model * vec4(skinnedPos, 1.0);
}
