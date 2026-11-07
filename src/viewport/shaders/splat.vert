#version 450

// GAUSSIAN SPLATS — the vertex stage. A four-vertex triangle strip instanced once a splat,
// gl_InstanceIndex the slot in the sorted (far-first) order the CPU wrote this frame — four
// vertex invocations a splat where six plain vertices were six (the strip shares its two
// middle vertices): the splat's 3D covariance (local space, from its scales and rotation at
// import) is carried through the model and view matrices and projected to a 2D covariance by the
// perspective's Jacobian at the splat's view-space depth (the reference's EWA splatting, with the
// 0.3 px low-pass dilation), whose eigenvectors and extents — 3σ, or less where this splat's own
// Gaussian falls under 1/255 sooner: a faint splat's tail is invisible well inside 3σ, and the
// alpha-blended fill is the dearest part of a capture of half a million splats, in a headset
// above all — place the quad's corner in pixels; the fragment then reads the Gaussian off the
// corner's local (σ-unit) coordinates. The
// colour is the splat's spherical harmonics evaluated along the eye→splat direction IN THE
// SPLAT'S OWN FRAME (the model matrix never touches the harmonics), handed on AS TRAINED — the
// trainers' display-referred values — to the pass's photo layer, where the splats blend in that
// space and the composite lays them over the tonemapped scene (see splat.frag).
//
// Pass:    the PHOTO PASS (hdrtarget.h), after the scene pass: single-sample, depth-tested
//          against the resolved scene depth, no depth write, into the photo layer.
// Inputs:  set 0 the camera UBO; set 1 the splat buffers (0 = position+opacity / covariance,
//          1 = the harmonics as packed halves, 2 = this frame's draw order); push constants.
// Outputs: vLocal (the corner in σ units), vColor (the colour as trained + the splat's opacity).

#extension GL_GOOGLE_include_directive : enable
#include "camera_ubo.glsl"

layout(set = 1, binding = 0) readonly buffer Splats { vec4 data[]; } splats;       // 3 vec4 a splat
layout(set = 1, binding = 1) readonly buffer Harmonics { uint data[]; } harmonics; // halves, packed in pairs
layout(set = 1, binding = 2) readonly buffer Order { uint index[]; } order;        // far first

layout(push_constant) uniform PC {
    mat4 model;
    vec4 focal;  // x = fx, y = fy (pixels per unit of NDC·w), z = viewport width, w = height
    vec4 params; // x = SH degree, y = harmonics uints per splat, z = 1 shrinks every quad to a
                 // pixel (POSESTUDIO_SPLAT_TINY: the vertex stage's cost alone), w = orthographic (0/1)
    vec4 camPos; // xyz = the eye in the splat's own (model) space; w = 1 for the opacity compensation (0 = the A/B off)
} pc;

layout(location = 0) out vec2 vLocal;
layout(location = 1) out vec4 vColor;

const vec2 kCorners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0)); // strip order
const float SH_C0 = 0.28209479177387814;
const float SH_C1 = 0.4886025119029199;
const float SH_C2[5] = float[5](1.0925484305920792, -1.0925484305920792, 0.31539156525252005,
                                -1.0925484305920792, 0.5462742152960396);
const float SH_C3[7] = float[7](-0.5900435899266435, 2.890611442640554, -0.4570457994644658,
                                0.3731763325901154, -0.4570457994644658, 1.445305721320277,
                                -0.5900435899266435);

// Coefficient k's (r, g, b): three consecutive halves from element k*3 of the splat's run.
vec3 shCoeff(uint base, uint k) {
    vec3 c;
    for (int i = 0; i < 3; ++i) {
        uint e = k * 3u + uint(i);
        vec2 pair = unpackHalf2x16(harmonics.data[base + e / 2u]);
        c[i] = (e & 1u) == 0u ? pair.x : pair.y;
    }
    return c;
}



void cull() {
    gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // outside the clip box: no fragments
    vLocal = vec2(0.0);
    vColor = vec4(0.0);
}

void main() {
    uint splat = order.index[gl_InstanceIndex];
    uint corner = uint(gl_VertexIndex); // 0..3
    vec4 p0 = splats.data[splat * 3u];
    vec4 c0 = splats.data[splat * 3u + 1u];
    vec4 c1 = splats.data[splat * 3u + 2u];
    vec4 posW = pc.model * vec4(p0.xyz, 1.0);
    vec4 posV = cam.view * posW;
    bool ortho = pc.params.w > 0.5;
    if (!ortho && posV.z > -0.05) {
        cull(); // behind the eye
        return;
    }
    vec4 clip = cam.viewProj * posW;
    if (abs(clip.x) > 1.3 * clip.w || abs(clip.y) > 1.3 * clip.w || clip.z < 0.0 || clip.z > clip.w) {
        cull(); // the centre well outside the view (a splat's extent is a few pixels to a few hundred)
        return;
    }

    // The 3D covariance through model and view, then the projection's Jacobian: a 2D covariance
    // in pixels, dilated by the low-pass filter.
    mat3 cov3 = mat3(c0.x, c0.y, c0.z,
                     c0.y, c0.w, c1.x,
                     c0.z, c1.x, c1.y);
    mat3 T = mat3(cam.view) * mat3(pc.model);
    mat3 covV = T * cov3 * transpose(T);
    float fx = pc.focal.x;
    float fy = pc.focal.y;
    vec3 j0;
    vec3 j1;
    if (ortho) {
        j0 = vec3(fx, 0.0, 0.0);
        j1 = vec3(0.0, fy, 0.0);
    } else {
        float tz = posV.z;
        float z2 = tz * tz;
        j0 = vec3(fx / tz, 0.0, -fx * posV.x / z2);
        j1 = vec3(0.0, fy / tz, -fy * posV.y / z2);
    }
    float a0 = dot(j0, covV * j0);
    float b = dot(j0, covV * j1);
    float d0 = dot(j1, covV * j1);
    float a = a0 + 0.3;
    float d = d0 + 0.3;
    // The opacity scaled by what the dilation did to the footprint — sqrt(det before / det
    // after), the antialiasing (mip-splatting) formulation's compensation — so a sub-pixel splat
    // does not come out heavier than its size earns. Applied to every scene (see
    // SplatCloud::record): without it a capture seen from a distance breaks into dark speckles.
    float aaScale = pc.camPos.w > 0.5 ? sqrt(max((a0 * d0 - b * b) / max(a * d - b * b, 1e-12), 0.0)) : 1.0;
    float alpha = p0.w * aaScale;
    if (alpha < 1.0 / 255.0) {
        cull(); // under the fragment's own threshold at its very centre: no pixel would show
        return;
    }
    // The footprint's radius in σ: 3 at most (the reference's), and no more than where this
    // splat's Gaussian — its opacity times exp(-r²/2) — falls under 1/255, which for a faint
    // splat is well inside 3σ (opacity 0.1: 2.5σ, a third less area; 0.03: 2σ, half).
    float rad = min(3.0, sqrt(2.0 * log(255.0 * alpha)));
    float mid = 0.5 * (a + d);
    float disc = sqrt(max(mid * mid - (a * d - b * b), 0.0));
    float l1 = max(mid + disc, 1e-4);
    float l2 = max(mid - disc, 1e-4);
    vec2 v1 = abs(b) < 1e-6 ? (a >= d ? vec2(1.0, 0.0) : vec2(0.0, 1.0)) : normalize(vec2(b, l1 - a));
    vec2 v2 = vec2(-v1.y, v1.x);
    float r1 = rad * sqrt(l1);
    float r2 = rad * sqrt(l2);
    vec2 local = kCorners[corner];
    vec2 offsetPx = local.x * r1 * v1 + local.y * r2 * v2;
    if (pc.params.z > 0.5) {
        offsetPx = local * 0.5; // the tiny-quad probe: a pixel a splat, the vertex work as ever
    }
    // View space is y-up; the projection flips y into Vulkan's y-down NDC (Camera negates
    // [1][1]), so a pixel offset's y goes the other way in NDC.
    vec2 offsetNdc = vec2(offsetPx.x * 2.0 / pc.focal.z, -offsetPx.y * 2.0 / pc.focal.w);
    gl_Position = vec4(clip.xy + offsetNdc * clip.w, clip.z, clip.w);
    vLocal = local * rad;

    // The colour: the harmonics along the eye→splat direction in the splat's own frame.
    uint degree = uint(pc.params.x + 0.5);
    uint base = splat * uint(pc.params.y + 0.5);
    vec3 dir = normalize(p0.xyz - pc.camPos.xyz);
    float x = dir.x, y = dir.y, z = dir.z;
    vec3 col = SH_C0 * shCoeff(base, 0u);
    if (degree >= 1u) {
        col += -SH_C1 * y * shCoeff(base, 1u) + SH_C1 * z * shCoeff(base, 2u) - SH_C1 * x * shCoeff(base, 3u);
        if (degree >= 2u) {
            float xx = x * x, yy = y * y, zz = z * z, xy = x * y, yz = y * z, xz = x * z;
            col += SH_C2[0] * xy * shCoeff(base, 4u) + SH_C2[1] * yz * shCoeff(base, 5u) +
                   SH_C2[2] * (2.0 * zz - xx - yy) * shCoeff(base, 6u) + SH_C2[3] * xz * shCoeff(base, 7u) +
                   SH_C2[4] * (xx - yy) * shCoeff(base, 8u);
            if (degree >= 3u) {
                col += SH_C3[0] * y * (3.0 * xx - yy) * shCoeff(base, 9u) + SH_C3[1] * xy * z * shCoeff(base, 10u) +
                       SH_C3[2] * y * (4.0 * zz - xx - yy) * shCoeff(base, 11u) +
                       SH_C3[3] * z * (2.0 * zz - 3.0 * xx - 3.0 * yy) * shCoeff(base, 12u) +
                       SH_C3[4] * x * (4.0 * zz - xx - yy) * shCoeff(base, 13u) +
                       SH_C3[5] * z * (xx - yy) * shCoeff(base, 14u) + SH_C3[6] * x * (xx - 3.0 * yy) * shCoeff(base, 15u);
            }
        }
    }
    // The colour AS TRAINED — the trainers' display-referred 0..1 values, sRGB-encoded as their
    // photographs were — goes to the photo layer untouched: the splats must blend in that space
    // to come out as the training did (decoded, or put through the inverse tonemap, before the
    // blend, every bright splat weighed more than it was trained to: speckles, a sheen).
    col = clamp(col + 0.5, 0.0, 1.0);
    vColor = vec4(col, alpha);
}
