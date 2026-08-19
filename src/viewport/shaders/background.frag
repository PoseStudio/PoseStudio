#version 450

// HDRI backdrop fragment stage: samples the lighting environment along the per-pixel eye ray, so
// the panorama that lights the figure is also VISIBLE behind it — reflections and shading read as
// coming from somewhere real, and the figure sits in a place instead of a void. Follows the same
// exposure / rotation / tonemap dials as the PBR shading (mesh.frag), so backdrop and subject
// always agree.
//
// The sampled image is the prefiltered specular cubemap's lower mips (set 3 binding 0 — bound
// here as set 1): mip 0 is the environment itself. The panel's Blur dial selects the LOD —
// photographic background defocus, which also hides the cube's finite resolution.

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 view;
    mat4 lightViewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 fillDir;
    vec4 fillColor;
    vec4 rimDir;
    vec4 rimColor;
    vec4 ambient;
    vec4 params;  // y = exposure
    vec4 sh[9];
    vec4 params2; // z = envRotation(rad)
    vec4 params3; // z = backdrop mode (1 = environment, 2 = ground-projected dome), w = blur (mips)
    vec4 params4; // x = backdrop brightness, y = dome radius (world units)
} cam;

layout(set = 1, binding = 0) uniform samplerCube uEnv;

layout(location = 0) in vec3 vNearPoint;
layout(location = 1) in vec3 vFarPoint;
layout(location = 0) out vec4 outColor;

// Dome mode: the height the environment was nominally captured from (a tripod) — the projection
// center the sample directions radiate from, so the panorama's floor lands on y=0 plausibly.
const float kTripodHeight = 1.6;

vec3 rotateY(vec3 v, float angle) {
    float c = cos(angle), s = sin(angle);
    return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

vec3 tonemapACES(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 dir = normalize(vFarPoint - vNearPoint);
    float exposure   = cam.params.y;
    float envRot     = cam.params2.z;
    int   mode       = int(cam.params3.z + 0.5);
    float blur       = max(cam.params3.w, 0.0);
    float brightness = cam.params4.x;
    float domeRadius = max(cam.params4.y, 2.0);

    vec3 sampleDir = dir; // mode 1: infinite environment — sample straight along the eye ray
    if (mode == 2) {
        // Ground-projected dome: intersect the eye ray with a finite dome (sphere of domeRadius
        // about the capture point) and, below the horizon, with the ground plane y=0 — then
        // sample the environment along the direction from the capture point to the hit. The
        // panorama's floor lands ON the viewport's ground plane, so the figure stands on the
        // environment's floor instead of a horizon floating at infinity.
        vec3 center = vec3(0.0, kTripodHeight, 0.0);
        vec3 ro = vNearPoint;
        // Sphere hit (far intersection — assumes the camera sits inside the dome). Dollied OUTSIDE
        // the dome, edge-of-screen rays can miss the sphere (disc < 0) or "hit" behind the camera
        // (ts <= 0); sampling those would mirror the panorama into warped streaks, so such pixels
        // fall back to the infinite-environment direction instead.
        vec3 oc = ro - center;
        float b = dot(oc, dir);
        float cc = dot(oc, oc) - domeRadius * domeRadius;
        float disc = b * b - cc;
        float ts = -b + sqrt(max(disc, 0.0));
        if (disc >= 0.0 && ts > 0.0) {
            vec3 hit = ro + dir * ts;
            // Ground hit wins when the ray strikes y=0 inside the dome footprint.
            if (dir.y < -1e-4) {
                float tg = -ro.y / dir.y;
                vec3 g = ro + dir * tg;
                if (tg > 0.0 && dot(g.xz, g.xz) < domeRadius * domeRadius) {
                    hit = g;
                }
            }
            sampleDir = normalize(hit - center);
        }
    }

    // LINEAR HDR out (like mesh.frag's PBR mode): the composite pass tonemaps — which also lets
    // bright environment sources bloom. Brightness scales the backdrop alone (the photographer's
    // "background a stop down"); the subject's lighting is untouched.
    float lod = min(blur, float(textureQueryLevels(uEnv) - 1));
    vec3 c = textureLod(uEnv, rotateY(sampleDir, envRot), lod).rgb * exposure * brightness;
    outColor = vec4(c, 0.0); // alpha 0: the backdrop is not skin (the HDR alpha is the SSS mask)
}
