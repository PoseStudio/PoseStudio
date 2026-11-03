#version 450

// Lit mesh vertex shader with DUAL-QUATERNION skinning (skinning.glsl). Each vertex blends its
// (up to 4) joints' unit dual quaternions (set 2 storage buffer; real = rotation (xyz,w), dual =
// 0.5·(0,t)·real), normalizes, and transforms by the result, then by the per-draw model matrix
// and the per-frame camera view-projection. DQS, not linear matrix blending, because the figure
// format authors its skin weights AND its pose correctives against dual quaternions
// (skin_settings.general_map_mode = DualQuat): LBS collapses deep bends (a 155° knee folded into
// a shapeless blob) and the pose correctives — sculpted as corrections on top of DQS — made the collapse
// worse instead of fixing it. Static meshes bind a single identity joint and default to weight
// (1,0,0,0), so this same path leaves them untransformed. Passes a world-space normal, UV, and
// world position (the last feeds the specular view vector) to the fragment.
//
// POSE CORRECTIVES are blended here too, BEFORE skinning (correctives.glsl): each vertex
// carries a packed range into the model's corrective-delta buffer (set 2, binding 2 — sorted per
// vertex), and the per-frame corrective weights (set 2, binding 1) scale those deltas. The
// corrected bind mesh is then skinned, exactly what the CPU re-morph used to produce — but a
// weight change is now a tiny per-frame buffer write instead of a device-idle + vertex-buffer
// re-upload, which is what lets the correctives track a drag live rather than popping in on
// release. shadow.vert applies the IDENTICAL blend through the same includes (shadows/outlines
// must follow the corrected surface).
//
// Pass:    the HDR scene pass (every mesh variant: opaque, transparent, wire, hidden-line).
// Inputs:  set 0.0 the camera UBO (only viewProj is read; the shared block declares it all);
//          set 2.0-2 the per-model joint DQs + corrective weights/deltas; push: MeshPushConstants;
//          vertex attributes 0-7 (Vertex: pos, normal, uv, joints, weights, ao, tangent, range).
// Outputs: the varyings mesh.frag shades (world normal/position, uv, ao, tangent, selection).

#extension GL_GOOGLE_include_directive : enable
#include "camera_ubo.glsl"
#define SKIN_SET 2 // the joint + corrective buffers ride in the main pass's set 2
#include "skinning.glsl"
#include "correctives.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColor; // rgb tint, a = opacity
    vec4 material;  // x = roughness, y = normalMode
    vec4 material2; // (fragment-side material params)
    vec4 material3; // z packs the draw kind with the SELECTED joint + its twist twin (Mesh::record)
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;
layout(location = 3) in uvec4 inJoints;
layout(location = 4) in vec4 inWeights;
layout(location = 5) in float inAo; // baked per-vertex ambient occlusion (1 = open)
layout(location = 6) in vec4 inTangent; // UV tangent + handedness (w = 0 -> none baked)
layout(location = 7) in uint inCorrectiveRange; // (first delta entry << 8) | count; 0 = none

layout(location = 0) out vec3 vWorldNormal;
layout(location = 1) out vec2 vUv;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out float vAo;
layout(location = 4) out vec4 vTangent; // world-space tangent, w = handedness (0 = absent)
layout(location = 5) out float vSelect; // this vertex's skin weight on the SELECTED joint (+ twin)

void main() {
    // Pose correctives: the bind position plus this vertex's weighted corrective deltas.
    vec3 bindPos = applyCorrectives(inPos, inCorrectiveRange);

    // Blend the influencing joints' dual quaternions by weight (see skinning.glsl) — or linearly,
    // where the model's rig was authored for that.
    vec3 skinnedPos;
    vec3 skinnedNormal;
    vec3 skinnedTangent;
    if (linearSkinning()) {
        skinnedPos = linearSkinPoint(inJoints, inWeights, bindPos);
        skinnedNormal = normalize(linearSkinVector(inJoints, inWeights, inNormal));
        skinnedTangent = linearSkinVector(inJoints, inWeights, inTangent.xyz);
    } else {
        vec4 rAcc;
        vec4 dAcc;
        blendDualQuat(inJoints, inWeights, rAcc, dAcc);
        // Rigid transform of the normalized blend.
        skinnedPos = dualQuatTransform(rAcc, dAcc, bindPos);
        skinnedNormal = quatRotate(rAcc, inNormal);
        skinnedTangent = quatRotate(rAcc, inTangent.xyz);
    }
    vec4 worldPos = pc.model * vec4(skinnedPos, 1.0);
    // mat3(model) is correct for the rigid/uniform-scale model transforms in play; swap to a
    // proper normal matrix if non-uniform scale ever appears.
    vWorldNormal = mat3(pc.model) * skinnedNormal;
    vUv = inUv;
    vWorldPos = worldPos.xyz;
    vAo = inAo;
    vTangent = vec4(mat3(pc.model) * skinnedTangent, inTangent.w);

    // Selection highlight amount: the skin weight this vertex puts on the selected joint and its
    // twist twin — the flesh that joint moves, fading out where the weights blend into the
    // neighbouring bones. material3.z packs kind + 8·(joint+1) + 8192·(twin+1) (exact below
    // 2^24); -1 (no selection, or a static mesh, which has nothing to select) yields 0 everywhere.
    // ... and the joints lit WITH the selection: the FACE RIG under a selected head (the face is the
    // head's — its bones are not selectable — and the head bone's own weights cover the scalp
    // alone). The joint buffer's second half carries that per joint, in the .x of one more entry
    // each (Model::uploadJointsIfDirty); the joint count is half the buffer's length.
    int packed = int(pc.material3.z + 0.5);
    int selJoint = ((packed >> 3) & 1023) - 1;
    int selTwin = (packed >> 13) - 1;
    float sel = 0.0;
    if (selJoint >= 0) {
        ivec4 j = ivec4(inJoints);
        int   lit = joints.dq.length() / 2;
        sel += inWeights.x * ((j.x == selJoint || j.x == selTwin) ? 1.0 : joints.dq[lit + j.x].real.x);
        sel += inWeights.y * ((j.y == selJoint || j.y == selTwin) ? 1.0 : joints.dq[lit + j.y].real.x);
        sel += inWeights.z * ((j.z == selJoint || j.z == selTwin) ? 1.0 : joints.dq[lit + j.z].real.x);
        sel += inWeights.w * ((j.w == selJoint || j.w == selTwin) ? 1.0 : joints.dq[lit + j.w].real.x);
    }
    vSelect = sel;
    gl_Position = cam.viewProj * worldPos;
}
