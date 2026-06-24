#version 450

// Fragment half of the infinite grid (see grid.vert). Intersects the per-pixel eye ray with
// the ground plane y=0, draws anti-aliased lines using screen-space derivatives, writes a
// correct depth so scene geometry interacts with the grid, and fades the lines out with
// distance so the floor dissolves toward the horizon instead of ending in a hard edge.

layout(push_constant) uniform PC {
    mat4 viewProj; // also used here to compute correct fragment depth
} pc;

layout(location = 0) in vec3 vNearPoint;
layout(location = 1) in vec3 vFarPoint;
layout(location = 0) out vec4 outColor;

// --- Tunables. Colours are LINEAR: the swapchain is sRGB, so the GPU encodes on store. ---
const vec3  kLineColor    = vec3(0.16);  // soft grey, a touch lighter than the #3E4042 floor
const float kMinorSpacing = 1.0;         // world units between fine lines
const float kMajorEvery   = 10.0;        // every Nth line is a brighter "major" line
const float kMajorBoost    = 1.7;        // how much brighter major lines are
const float kFadeDistance  = 60.0;       // grid fully faded out by this distance from camera

// Anti-aliased line coverage for a lattice with the given world-space spacing. Uses fwidth so
// lines stay ~1px wide at any distance/zoom (and merge gracefully into a tone far away).
float gridCoverage(vec2 worldXZ, float spacing) {
    vec2 coord = worldXZ / spacing;
    vec2 derivative = fwidth(coord);
    vec2 g = abs(fract(coord - 0.5) - 0.5) / derivative;
    return 1.0 - min(min(g.x, g.y), 1.0);
}

void main() {
    // Ray/plane intersection with y=0. t<=0 means the plane is behind the camera here.
    float t = -vNearPoint.y / (vFarPoint.y - vNearPoint.y);
    if (t <= 0.0) {
        discard;
    }
    vec3 fragPos = vNearPoint + t * (vFarPoint - vNearPoint);

    // Write true depth so the grid is occluded by / occludes scene geometry correctly.
    // (Vulkan clip-space z is already in [0,1], the range gl_FragDepth expects.)
    vec4 clip = pc.viewProj * vec4(fragPos, 1.0);
    gl_FragDepth = clip.z / clip.w;

    float minor = gridCoverage(fragPos.xz, kMinorSpacing);
    float major = gridCoverage(fragPos.xz, kMinorSpacing * kMajorEvery);
    float coverage = max(minor, major * kMajorBoost);

    // Fade by distance from the camera (the near-plane point is a good stand-in for the eye).
    float dist = length(fragPos - vNearPoint);
    float fade = clamp(1.0 - dist / kFadeDistance, 0.0, 1.0);

    outColor = vec4(kLineColor, clamp(coverage, 0.0, 1.0) * fade);
}
