#version 450

// Fragment half of the infinite grid (see grid.vert). Intersects the per-pixel eye ray with
// the ground plane y=0, draws anti-aliased lines using screen-space derivatives, writes a
// correct depth so scene geometry interacts with the grid, and fades the lines out with
// distance so the floor dissolves toward the horizon instead of ending in a hard edge.

layout(push_constant) uniform PC {
    mat4 viewProj;     // also used here to compute correct fragment depth
    vec4 lightRow0;    // key light's ortho view-projection, math rows 0..2 (the bottom row of an
    vec4 lightRow1;    // ortho·rigid transform is always (0,0,0,1), so it's dropped to make room
    vec4 lightRow2;    // for the shadow dials within the 128-byte push budget)
    vec4 shadowParams; // x = intensity (0..1), y = softness (0..1), z = reach (world), w unused
} pc;

// The scene's shadow map (set 3 bindings 2/3 in the mesh path; the grid's pipeline binds that
// same set at index 0 and declares only these). Binding 2: comparison sampler (1 = lit).
// Binding 3: the SAME depth image through a non-comparison sampler — raw depths, so the PCSS
// blocker search can measure how far above the floor the caster sits.
layout(set = 0, binding = 2) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 3) uniform sampler2D uShadowDepth;

layout(location = 0) in vec3 vNearPoint;
layout(location = 1) in vec3 vFarPoint;
layout(location = 0) out vec4 outColor;

// --- Tunables. Colours are LINEAR: the swapchain is sRGB, so the GPU encodes on store. ---
const vec3  kLineColor    = vec3(0.16);  // soft grey, a touch lighter than the #3E4042 floor
const float kMinorSpacing = 1.0;         // world units between fine lines
const float kMajorEvery   = 10.0;        // every Nth line is a brighter "major" line
const float kMajorBoost    = 1.7;        // how much brighter major lines are
const float kFadeDistance  = 60.0;       // grid fully faded out by this distance from camera
const vec3  kShadowColor   = vec3(0.012); // the contact shadow's tint (near-black, linear)
// PCSS ground shadow (percentage-closer soft shadow with contact hardening): the penumbra widens
// and the shadow lightens with the caster's height above the floor — dark and crisp where the feet
// touch, dissolving with distance, exactly how an area light's real shadow behaves.
// The dial-driven parts (opacity, penumbra growth, dissolve reach) come from pc.shadowParams —
// the Environment panel's Shadow group. The constants below are the fixed calibration those dials
// scale (LightingSettings holds the shipped default dial values).
const float kShadowOpacity  = 0.65; // opacity at contact at intensity 1 (the darkest the floor gets)
const float kSearchWorld    = 0.3;  // blocker-search radius (world units ≈ m): how far the search reaches
const float kSpreadPerSoft  = 0.35; // penumbra growth per world unit of caster height at softness 1
                                    // (tan of the key's area size; softness 0.35 ≈ a 60 cm softbox at 5 m)
const float kBaseSoftWorld  = 0.005; // minimum penumbra so even the contact edge isn't hard-pixel aliased
const float kBaseSoftPerSoft = 0.03; // extra base softness with the dial (softens the contact edge too)

// 16-tap Poisson disk (unit radius) — shared by the blocker search (first 9) and the PCF.
const vec2 kPoisson[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790));

// Anti-aliased line coverage for a lattice with the given world-space spacing. Uses fwidth so
// lines stay ~1px wide at any distance/zoom (and merge gracefully into a tone far away).
float gridCoverage(vec2 worldXZ, float spacing) {
    vec2 coord = worldXZ / spacing;
    vec2 derivative = fwidth(coord);
    vec2 g = abs(fract(coord - 0.5) - 0.5) / derivative;
    return 1.0 - min(min(g.x, g.y), 1.0);
}

// PCSS ground-shadow occlusion of @p world (0 = lit floor, 1 = fully shadowed at contact).
// Three steps: (1) a blocker search over raw depths finds the caster's average height above this
// floor point; (2) the PCF kernel radius grows with that height (an area light's penumbra widens
// with caster distance) — contact-hardening: crisp at the feet, soft far from them; (3) the
// shadow's intensity also fades with that height, so it trails off instead of staying a flat
// blob. The sampler's white border + the z-range check make anything outside the fitted frustum
// read unshadowed.
float groundShadow(vec3 world) {
    if (pc.shadowParams.x <= 0.001) {
        return 0.0; // intensity dial at zero — skip all the sampling
    }
    vec4 w4 = vec4(world, 1.0);
    // The light transform arrives as its three math rows (ortho·rigid: w is always 1).
    vec3 p = vec3(dot(pc.lightRow0, w4), dot(pc.lightRow1, w4), dot(pc.lightRow2, w4));
    if (p.z <= 0.0 || p.z >= 1.0) {
        return 0.0;
    }
    vec2 uv = p.xy * 0.5 + 0.5;

    // World-space scales straight from the ortho matrix (view is rigid, so row lengths are the
    // clip-axis scales): row 0 maps world → clip x over the frustum's 2·radius width, row 2 maps
    // world → depth over the near/far range. No extra uniforms needed.
    float uvPerWorld = 0.5 * length(pc.lightRow0.xyz);
    float worldPerDepth = 1.0 / max(length(pc.lightRow2.xyz), 1e-6);

    // Per-pixel kernel rotation: turns the sparse Poisson taps' banding into unstructured noise.
    float angle = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy,
                                                           vec2(0.06711056, 0.00583715))));
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);

    // 1. Blocker search: average raw depth of the map texels nearer the light than this floor
    // point, within the search radius. The radius shrinks with the Softness dial (a small light's
    // penumbra never reaches far, and a tight ring wastes fewer taps), and the CENTER is always
    // sampled — without it, a pixel squarely under a small caster (a foot) can have every sparse
    // ring tap miss, which showed as salt-noise speckles inside crisp contact shadows. No
    // blockers => open floor, no shadow.
    float searchUV = kSearchWorld * (0.25 + 0.75 * pc.shadowParams.y) * uvPerWorld;
    float blockerSum = 0.0;
    int blockerCount = 0;
    float dc = texture(uShadowDepth, uv).r;
    if (dc < p.z) {
        blockerSum += dc;
        ++blockerCount;
    }
    for (int i = 0; i < 16; ++i) {
        float d = texture(uShadowDepth, uv + rot * kPoisson[i] * searchUV).r;
        if (d < p.z) {
            blockerSum += d;
            ++blockerCount;
        }
    }
    if (blockerCount == 0) {
        return 0.0;
    }
    float casterHeight = (p.z - blockerSum / float(blockerCount)) * worldPerDepth;

    // 2. Contact-hardening PCF: penumbra ∝ caster height above the floor, scaled by the panel's
    // Softness dial.
    float softness = pc.shadowParams.y;
    float radiusUV = (kBaseSoftWorld + kBaseSoftPerSoft * softness +
                      kSpreadPerSoft * softness * casterHeight) * uvPerWorld;
    float lit = 0.0;
    for (int i = 0; i < 16; ++i) {
        lit += texture(uShadowMap, vec3(uv + rot * kPoisson[i] * radiusUV, p.z));
    }
    lit /= 16.0;

    // 3. Trail off: the higher the caster, the weaker its shadow (the area light wraps around it).
    // The panel's Reach dial sets where the trail fully dissolves.
    float fade = clamp(1.0 - casterHeight / max(pc.shadowParams.z, 0.1), 0.0, 1.0);
    return (1.0 - lit) * fade * pc.shadowParams.x;
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

    // Ground/contact shadow: where the key light is occluded, the (otherwise invisible) floor
    // shows the PCSS soft shadow — dark and crisp at the figure's contact points, widening and
    // dissolving with caster height — which is what visually grounds the figure on the plane.
    // Composite the grid lines OVER the shadow patch; both share the distance fade.
    float lineA = clamp(coverage, 0.0, 1.0) * fade;
    float shadowA = groundShadow(fragPos) * kShadowOpacity * fade;
    float outA = lineA + shadowA * (1.0 - lineA);
    if (outA < 0.003) {
        discard; // fully empty floor pixel — skip the blend entirely
    }
    vec3 outCol = (kLineColor * lineA + kShadowColor * shadowA * (1.0 - lineA)) / outA;
    outColor = vec4(outCol, outA);
}
