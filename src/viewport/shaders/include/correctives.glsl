// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
// correctives.glsl — the per-model pose-corrective buffers and their per-vertex blend.
//
// Pose correctives are blended BEFORE skinning: each vertex carries a packed range into the
// model's corrective-delta buffer (binding 2 — entries sorted per vertex), and the per-frame
// corrective weights (binding 1) scale those deltas. The corrected bind mesh is then skinned,
// exactly what the CPU re-morph used to produce — but a weight change is now a tiny per-frame
// buffer write instead of a device-idle + vertex-buffer re-upload, which is what lets the
// correctives track a drag live rather than popping in on release.
//
// Shared by mesh.vert and shadow.vert (the shadow and the outline must trace the corrected
// surface) — both bind the buffers at SKIN_SET, defined by the including shader (see skinning.glsl).

#ifndef SKIN_SET
#error "Define SKIN_SET (the descriptor set index of the corrective buffers) before including correctives.glsl"
#endif

layout(std430, set = SKIN_SET, binding = 1) readonly buffer CorrectiveWeights {
    float w[];
} jcmWeights;
struct CorrectiveDelta {
    uint  corrective; // index into jcmWeights.w
    float dx;         // bind-space displacement at weight 1 (model units)
    float dy;
    float dz;
};
layout(std430, set = SKIN_SET, binding = 2) readonly buffer CorrectiveDeltas {
    CorrectiveDelta d[];
} jcmDeltas;

// The bind position plus this vertex's weighted corrective deltas. @p range is the vertex's
// packed (first delta entry << 8) | count — empty for untouched vertices and every static-mesh
// vertex, so the loop is free there.
vec3 applyCorrectives(vec3 inPos, uint range) {
    vec3 bindPos = inPos;
    uint jcmCount = range & 0xFFu;
    uint jcmFirst = range >> 8;
    for (uint k = 0u; k < jcmCount; ++k) {
        CorrectiveDelta cd = jcmDeltas.d[jcmFirst + k];
        bindPos += jcmWeights.w[cd.corrective] * vec3(cd.dx, cd.dy, cd.dz);
    }
    return bindPos;
}
