#version 450

// Mesh fragment shader with a selectable *shade mode* (the viewport's shader picker). One über-shader
// branches on cam.params.x — a per-frame uniform, so every fragment in a draw takes the same branch
// (coherent, cheap). Modes:
//   0 Rendered        1 PBR            2 Matcap Studio   3 Matcap Skin
//   4 Matcap Metal    5 Toon           6 Clay            7 Lighting Only
//   8 Flat Shaded     9 Normals        10 Albedo (unlit) 11 UV Checker
// Shared across the shaded modes: a detail map (tangent-space normal = mode 1, grayscale bump = mode 2)
// perturbs the normal via a screen-space cotangent frame (no vertex tangents); lighting is two-sided
// (the normal faces the viewer regardless of winding); and alpha = the per-draw opacity, so the same
// shader serves the opaque and the alpha-blended transparent pass.

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 view;
    vec4 cameraPos;
    vec4 lightDir;    // key light (direction TO the light)
    vec4 lightColor;
    vec4 fillDir;     // fill light (softer, opposite the key)
    vec4 fillColor;
    vec4 rimDir;      // rim / back light (behind the subject, pops the silhouette)
    vec4 rimColor;
    vec4 ambient;
    vec4 params; // x = shade mode
} cam;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColor; // rgb tint, a = opacity
    vec4 material;  // x = roughness, y = normalMode (0 none / 1 normal / 2 bump)
} pc;

layout(set = 1, binding = 0) uniform sampler2D uDiffuse; // sRGB colour map (white 1x1 fallback)
layout(set = 1, binding = 1) uniform sampler2D uDetail;  // linear normal/bump map (flat fallback)

layout(location = 0) in vec3 vWorldNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

// Builds a tangent basis from screen-space derivatives of position + uv (Schüler's cotangent frame).
mat3 cotangentFrame(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    return mat3(T * invmax, B * invmax, N);
}

// ACES filmic tonemap — keeps the HDR ranges of the PBR/matcap modes from clipping harshly before the
// sRGB framebuffer does its own gamma encode.
vec3 tonemapACES(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Cheap image-based-lighting stand-in: a two-colour hemisphere (cool sky above, warm-dark ground below).
// Deliberately dim — it's a fill-of-last-resort for the darkest areas, not a scene light.
vec3 hemiAmbient(vec3 dir) {
    const vec3 sky    = vec3(0.30, 0.34, 0.42);
    const vec3 ground = vec3(0.11, 0.10, 0.09);
    return mix(ground, sky, clamp(dir.y * 0.5 + 0.5, 0.0, 1.0));
}

// --- Cook-Torrance microfacet BRDF terms (GGX / Smith / Schlick) ------------------------------------
float distGGX(float ndh, float a) {
    float a2 = a * a;
    float d = (ndh * ndh) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}
float geomSmith(float ndv, float ndl, float a) {
    float k = (a * a) * 0.5;
    float gv = ndv / (ndv * (1.0 - k) + k);
    float gl = ndl / (ndl * (1.0 - k) + k);
    return gv * gl;
}
vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// One analytic light's Cook-Torrance contribution: Lambert diffuse (energy-conserving via kd = 1-F) +
// GGX specular, scaled by the light's radiance and N·L. @p a is roughness². Returns 0 for back-facing L.
vec3 pbrDirect(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float a, vec3 f0) {
    float ndl = dot(N, L);
    if (ndl <= 0.0) {
        return vec3(0.0);
    }
    vec3  H   = normalize(V + L);
    float ndv = max(dot(N, V), 1e-4);
    float ndh = max(dot(N, H), 0.0);
    float vdh = max(dot(V, H), 0.0);
    float D = distGGX(ndh, a);
    float G = geomSmith(ndv, ndl, a);
    vec3  F = fresnelSchlick(vdh, f0);
    vec3  spec = (D * G) * F / max(4.0 * ndv * ndl, 1e-4);
    vec3  kd = vec3(1.0) - F;
    // Specular dialed down: full-strength dielectric spec makes skin read oily/plastic and blows the
    // glossy eye catchlight into a white blob over the pupil. ~40% keeps a believable sheen + catchlight
    // without either artifact (skin isn't a true dielectric — subsurface scatters much of it away).
    return (kd * albedo / PI + spec * 0.4) * radiance * ndl;
}

// --- Procedural matcaps: shade a "material sphere" from the view-space normal (vn.z points at eye) ---
vec3 matcapStudio(vec3 vn) {
    float t = smoothstep(-0.85, 1.0, vn.y);
    vec3 c = mix(vec3(0.06, 0.07, 0.09), vec3(0.82, 0.85, 0.90), t);
    c += vec3(1.0) * pow(max(dot(vn, normalize(vec3(-0.5, 0.55, 0.65))), 0.0), 28.0) * 0.9; // key
    c += vec3(0.18, 0.20, 0.26) * pow(max(dot(vn, normalize(vec3(0.6, 0.1, 0.6))), 0.0), 4.0); // fill
    c += vec3(0.25, 0.30, 0.42) * pow(1.0 - max(vn.z, 0.0), 3.0); // rim
    return c;
}
vec3 matcapSkin(vec3 vn) {
    float t = smoothstep(-0.7, 1.0, vn.y);
    vec3 c = mix(vec3(0.30, 0.12, 0.10), vec3(0.96, 0.76, 0.66), t);
    c += vec3(1.0, 0.96, 0.90) * pow(max(dot(vn, normalize(vec3(-0.4, 0.5, 0.75))), 0.0), 20.0) * 0.30;
    c += vec3(0.70, 0.25, 0.18) * pow(1.0 - max(vn.z, 0.0), 2.5) * 0.55; // warm subsurface rim
    return c;
}
vec3 matcapMetal(vec3 vn) {
    float t = smoothstep(-1.0, 1.0, vn.y);
    vec3 c = mix(vec3(0.02, 0.03, 0.05), vec3(0.45, 0.50, 0.60), t);
    c += vec3(1.0) * pow(max(dot(vn, normalize(vec3(-0.35, 0.6, 0.7))), 0.0), 220.0); // sharp key
    c += vec3(0.25, 0.28, 0.34) * pow(max(dot(vn, normalize(vec3(0.5, -0.4, 0.6))), 0.0), 6.0); // bounce
    c += vec3(0.55, 0.60, 0.75) * pow(1.0 - max(vn.z, 0.0), 1.6) * 0.7; // bright rim
    return c;
}

void main() {
    int mode = int(cam.params.x + 0.5);

    vec3 n = normalize(vWorldNormal);
    if (!gl_FrontFacing) {
        n = -n; // light back faces as if they faced us (no back-face culling; winding is inconsistent)
    }

    // Detail-map normal perturbation (uniform branch on the per-draw normalMode).
    int nm = int(pc.material.y + 0.5);
    if (nm == 1) {
        vec3 mapN = texture(uDetail, vUv).xyz * 2.0 - 1.0;
        n = normalize(cotangentFrame(n, vWorldPos, vUv) * mapN);
    } else if (nm == 2) {
        vec2 texel = 1.0 / vec2(textureSize(uDetail, 0));
        float hL = texture(uDetail, vUv - vec2(texel.x, 0.0)).r;
        float hR = texture(uDetail, vUv + vec2(texel.x, 0.0)).r;
        float hD = texture(uDetail, vUv - vec2(0.0, texel.y)).r;
        float hU = texture(uDetail, vUv + vec2(0.0, texel.y)).r;
        vec2 dH = vec2(hR - hL, hU - hD) * 2.5;
        n = normalize(cotangentFrame(n, vWorldPos, vUv) * vec3(-dH.x, -dH.y, 1.0));
    }

    vec3  albedo = pc.baseColor.rgb * texture(uDiffuse, vUv).rgb;
    float alpha  = pc.baseColor.a;

    vec3  l  = normalize(cam.lightDir.xyz);
    vec3  v  = normalize(cam.cameraPos.xyz - vWorldPos);
    vec3  hv = normalize(l + v);
    float rawNdl = dot(n, l);
    float ndl = max(rawNdl, 0.0);
    float ndh = max(dot(n, hv), 0.0);
    float ndv = max(dot(n, v), 0.0);
    float vdh = max(dot(v, hv), 0.0);
    float rough = clamp(pc.material.x, 0.04, 1.0);

    // --- Data / unlit modes (return early; no lighting) ---
    if (mode == 9) {              // Normals: world-space normal as RGB
        outColor = vec4(n * 0.5 + 0.5, 1.0);
        return;
    }
    if (mode == 10) {             // Albedo: flat base color × diffuse map, unlit
        outColor = vec4(albedo, alpha);
        return;
    }
    if (mode == 11) {             // UV Checker: 16×16 checker, faintly tinted by uv for orientation
        vec2 g = floor(vUv * 16.0);
        float chk = mod(g.x + g.y, 2.0);
        vec3 c = mix(vec3(0.09, 0.09, 0.11), vec3(0.85, 0.85, 0.85), chk);
        c = mix(c, vec3(fract(vUv), 0.35), 0.12);
        outColor = vec4(c, 1.0);
        return;
    }

    // --- Matcaps (view-space normal) ---
    if (mode >= 2 && mode <= 4) {
        vec3 vn = normalize(mat3(cam.view) * n);
        vec3 c = (mode == 2) ? matcapStudio(vn) : (mode == 3) ? matcapSkin(vn) : matcapMetal(vn);
        outColor = vec4(tonemapACES(c), alpha);
        return;
    }

    // --- Flat Shaded: geometric (per-triangle) normal from position derivatives → faceted look ---
    if (mode == 8) {
        vec3 gN = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
        if (dot(gN, v) < 0.0) gN = -gN;
        float fndl = max(dot(gN, l), 0.0);
        outColor = vec4(albedo * (cam.ambient.rgb + cam.lightColor.rgb * fndl), alpha);
        return;
    }

    // --- Toon / Cel: quantized diffuse bands + a hard spec dot + a rim ---
    if (mode == 5) {
        float band = (ndl > 0.75) ? 1.0 : (ndl > 0.40) ? 0.70 : (ndl > 0.12) ? 0.42 : 0.22;
        float sdot = (ndh > 0.985) ? 1.0 : 0.0;
        float rim  = smoothstep(0.55, 1.0, pow(1.0 - ndv, 2.0));
        vec3 c = albedo * (cam.ambient.rgb + cam.lightColor.rgb * band) + vec3(sdot) * 0.35 + vec3(rim) * 0.15;
        outColor = vec4(c, alpha);
        return;
    }

    // --- Clay: matte neutral material, wrapped lambert — pure form study, ignores textures ---
    if (mode == 6) {
        const vec3 clay = vec3(0.72, 0.70, 0.67);
        float wrapd = max((rawNdl + 0.3) / 1.3, 0.0);
        outColor = vec4(clay * (cam.ambient.rgb + cam.lightColor.rgb * wrapd), alpha);
        return;
    }

    // --- Lighting Only: shading on a neutral white surface (checks the lighting, hides the texture) ---
    if (mode == 7) {
        float wrapd = max((rawNdl + 0.2) / 1.2, 0.0);
        float spec = pow(ndh, 48.0) * ndl;
        vec3 c = vec3(0.85) * (cam.ambient.rgb + cam.lightColor.rgb * wrapd) + cam.lightColor.rgb * spec * 0.3;
        outColor = vec4(c, alpha);
        return;
    }

    // --- PBR: Cook-Torrance GGX lit by a 3-point studio rig + hemisphere IBL (the photoreal mode) ---
    if (mode == 1) {
        const vec3 f0 = vec3(0.04); // dielectric (no metal maps in the source yet)
        const float a = rough * rough;

        // Three-point rig: a dominant warm key, a soft cool fill that only opens the shadows, and a
        // back/rim light that separates the silhouette from the background. Each is a full BRDF eval.
        vec3 color = vec3(0.0);
        color += pbrDirect(n, v, normalize(cam.lightDir.xyz), cam.lightColor.rgb * 2.42, albedo, a, f0);
        color += pbrDirect(n, v, normalize(cam.fillDir.xyz),  cam.fillColor.rgb * 1.1,   albedo, a, f0);
        color += pbrDirect(n, v, normalize(cam.rimDir.xyz),   cam.rimColor.rgb * 1.1,    albedo, a, f0);

        // Cheap subsurface scattering: a warm reddish band just past the key light's terminator, so
        // skin reads as translucent flesh rather than opaque plastic (a stand-in for true SSS).
        float keyNdl  = dot(n, normalize(cam.lightDir.xyz));
        float sssBand = pow(clamp(0.5 - keyNdl, 0.0, 1.0), 2.0);
        color += albedo * cam.lightColor.rgb * vec3(0.30, 0.085, 0.05) * sssBand * 0.9;

        // Hemisphere-IBL ambient: a soft diffuse wrap + a roughness-attenuated reflection, so surfaces
        // pick up environment colour and smoother materials show a faint sky reflection. Kept low so it
        // only lifts the darkest areas rather than flat-lighting the whole subject.
        vec3 r  = reflect(-v, n);
        vec3 Fr = fresnelSchlick(ndv, f0);
        vec3 diffuseIBL = hemiAmbient(n) * albedo;
        vec3 specIBL    = hemiAmbient(r) * mix(vec3(0.04), vec3(1.0), Fr) * (1.0 - rough);
        color += diffuseIBL * 0.35 + specIBL * 0.2;

        // Fresnel rim tinted by the back light — a thin edge on grazing angles that pops the silhouette
        // (gated by facing the rim light so it only lights the true back edge).
        float rimF = pow(1.0 - ndv, 4.0) * clamp(dot(n, normalize(cam.rimDir.xyz)) * 0.5 + 0.5, 0.0, 1.0);
        color += cam.rimColor.rgb * rimF * 0.12;

        outColor = vec4(tonemapACES(color), alpha);
        return;
    }

    // --- Rendered (default, mode 0): skin-friendly Blinn-Phong + subsurface, lit by the 3-point rig ---
    float shininess = 2.0 / (rough * rough) - 2.0;
    float spec = pow(ndh, max(shininess, 1.0)) * ndl;
    float fresnel = mix(0.03, 1.0, pow(1.0 - ndv, 5.0));
    vec3 specular = cam.lightColor.rgb * spec * mix(0.12, 0.4, fresnel);

    const float wrap = 0.35;
    float ndlWrap = max((rawNdl + wrap) / (1.0 + wrap), 0.0);
    float sssBand = pow(clamp(0.45 - rawNdl, 0.0, 1.0), 2.2);
    vec3 sss = vec3(0.35, 0.09, 0.05) * sssBand;

    // Fill light: a soft cool wash from the opposite side that opens up the shadow side; rim/back light:
    // a thin edge on grazing angles facing the back light, separating the figure from the background.
    float fillNdl = max(dot(n, normalize(cam.fillDir.xyz)), 0.0);
    float rimEdge = pow(1.0 - ndv, 3.0) * max(dot(n, normalize(cam.rimDir.xyz)), 0.0);

    vec3 lighting = cam.ambient.rgb + cam.lightColor.rgb * ndlWrap + cam.fillColor.rgb * fillNdl * 0.5;
    vec3 color = albedo * (lighting + cam.lightColor.rgb * sss) + specular
                 + cam.rimColor.rgb * rimEdge * 0.09;
    outColor = vec4(color, pc.baseColor.a);
}
