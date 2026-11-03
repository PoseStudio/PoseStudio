// skinning.glsl — the per-model joint buffer and the DUAL-QUATERNION skinning blend.
//
// Shared by mesh.vert (the lit pass) and shadow.vert (the shadow map + the selection-outline
// mask): the two blends MUST match, or a posed figure self-shadows against a differently-deformed
// caster and its outline traces the wrong silhouette. The joint buffer's descriptor SET differs
// between the pipelines — set 2 in the main pass, set 0 in the position-only pipelines, whose only
// set is the same set object — so the including shader defines SKIN_SET first.
//
// Why dual quaternions, not linear matrix blending: the figure format authors its skin weights
// AND its pose correctives against dual quaternions (skin_settings.general_map_mode = DualQuat).
// LBS collapses deep bends (a 155° knee folded into a shapeless blob) and the pose correctives — sculpted as
// corrections on top of DQS — made the collapse worse instead of fixing it. Static meshes bind a
// single identity joint and default to weight (1,0,0,0), so the same path leaves them untouched.

#ifndef SKIN_SET
#error "Define SKIN_SET (the descriptor set index of the joint buffer) before including skinning.glsl"
#endif

// Per-model skinning dual quaternions (from the rigid poseGlobal * inverseBind per joint).
struct DualQuat {
    vec4 real; // rotation quaternion, stored (x, y, z, w)
    vec4 dual; // 0.5 * (0, translation) * real
};
layout(std430, set = SKIN_SET, binding = 0) readonly buffer Joints {
    DualQuat dq[];
} joints;

// Rotates v by unit quaternion r ((xyz, w) layout).
vec3 quatRotate(vec4 r, vec3 v) {
    return v + 2.0 * cross(r.xyz, cross(r.xyz, v) + r.w * v);
}

// Blends the influencing joints' dual quaternions by weight, sign-aligning each against the
// first joint's hemisphere (q and -q encode the same rotation; blending across the seam cancels
// instead of averaging). Weights sum to 1, so at bind pose (identity DQs) the normalized blend is
// exactly the identity — the mesh renders as its rest geometry. Outputs the NORMALIZED blend.
void blendDualQuat(uvec4 inJoints, vec4 inWeights, out vec4 rAcc, out vec4 dAcc) {
    vec4 r0 = joints.dq[inJoints.x].real;
    rAcc = inWeights.x * r0;
    dAcc = inWeights.x * joints.dq[inJoints.x].dual;
    float s1 = (dot(joints.dq[inJoints.y].real, r0) < 0.0) ? -1.0 : 1.0;
    rAcc += (inWeights.y * s1) * joints.dq[inJoints.y].real;
    dAcc += (inWeights.y * s1) * joints.dq[inJoints.y].dual;
    float s2 = (dot(joints.dq[inJoints.z].real, r0) < 0.0) ? -1.0 : 1.0;
    rAcc += (inWeights.z * s2) * joints.dq[inJoints.z].real;
    dAcc += (inWeights.z * s2) * joints.dq[inJoints.z].dual;
    float s3 = (dot(joints.dq[inJoints.w].real, r0) < 0.0) ? -1.0 : 1.0;
    rAcc += (inWeights.w * s3) * joints.dq[inJoints.w].real;
    dAcc += (inWeights.w * s3) * joints.dq[inJoints.w].dual;
    float invLen = 1.0 / max(length(rAcc), 1e-6);
    rAcc *= invLen;
    dAcc *= invLen;
}

// Rigid transform of a normalized dual-quaternion blend applied to a point: rotate by the real
// part, translate by 2 * (dual * conjugate(real)).vector.
vec3 dualQuatTransform(vec4 rAcc, vec4 dAcc, vec3 p) {
    return quatRotate(rAcc, p) +
           2.0 * (rAcc.w * dAcc.xyz - dAcc.w * rAcc.xyz + cross(rAcc.xyz, dAcc.xyz));
}

// LINEAR blend skinning (2026-11-03): a rig authored for it — every FBX export seen — bulges at
// the outside of a bend under dual quaternions, which its weights were never painted for. Each
// joint's dual quaternion IS a rigid transform; the linear blend is the weighted sum of the
// transformed point (and of the rotated normal), from the same joint buffer. Chosen per model by
// the joint buffer's second half, entry 0's .y (Model::uploadJointsIfDirty); the native figures
// keep the dual-quaternion blend.
bool linearSkinning() {
    return joints.dq[joints.dq.length() / 2].real.y > 0.5;
}
vec3 linearSkinPoint(uvec4 inJoints, vec4 inWeights, vec3 p) {
    return inWeights.x * dualQuatTransform(joints.dq[inJoints.x].real, joints.dq[inJoints.x].dual, p) +
           inWeights.y * dualQuatTransform(joints.dq[inJoints.y].real, joints.dq[inJoints.y].dual, p) +
           inWeights.z * dualQuatTransform(joints.dq[inJoints.z].real, joints.dq[inJoints.z].dual, p) +
           inWeights.w * dualQuatTransform(joints.dq[inJoints.w].real, joints.dq[inJoints.w].dual, p);
}
vec3 linearSkinVector(uvec4 inJoints, vec4 inWeights, vec3 v) {
    return inWeights.x * quatRotate(joints.dq[inJoints.x].real, v) +
           inWeights.y * quatRotate(joints.dq[inJoints.y].real, v) +
           inWeights.z * quatRotate(joints.dq[inJoints.z].real, v) +
           inWeights.w * quatRotate(joints.dq[inJoints.w].real, v);
}
