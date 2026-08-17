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
    vec4 params;  // x = shade mode, y = exposure, z = specularIntensity, w = ambientFill
    vec4 sh[9];   // environment diffuse irradiance: 9 SH coefficients (rgb in .xyz)
    vec4 params2; // x = diffuseIntensity, y = keyIntensity, z = envRotation(rad), w = tonemap(0/1)
    vec4 params3; // x = subsurface, y = rimIntensity, z/w reserved
} cam;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColor; // rgb tint, a = opacity
    vec4 material;  // x = roughness, y = normalMode (0 none / 1 normal / 2 bump), z = metalness
} pc;

layout(set = 1, binding = 0) uniform sampler2D uDiffuse; // sRGB colour map (white 1x1 fallback)
layout(set = 1, binding = 1) uniform sampler2D uDetail;  // linear normal/bump map (flat fallback)

// IBL maps (set 3): the specular half of split-sum image-based lighting. The diffuse half is the SH
// in the camera UBO. Prefiltered cube: mip = roughness. BRDF LUT: (NdotV, roughness) -> (scale, bias).
layout(set = 3, binding = 0) uniform samplerCube uPrefilteredSpec;
layout(set = 3, binding = 1) uniform sampler2D  uBrdfLut;

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

// Rotates a direction about the vertical (Y) axis — used to spin the lighting environment (its SH
// diffuse and prefiltered specular) around the subject without re-baking, from the panel's rotation dial.
vec3 rotateY(vec3 v, float angle) {
    float c = cos(angle), s = sin(angle);
    return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

// Reconstructs the environment's diffuse irradiance E(n) from the 9 SH coefficients in the camera
// UBO (Ramamoorthi & Hanrahan's irradiance-environment-map form; the cosine-lobe convolution is
// folded into the c1..c5 constants). This is the real image-based ambient — soft, directional, and
// tinted by the environment — that replaces the old flat `ambient` constant for the PBR mode.
vec3 shIrradiance(vec3 n) {
    const float c1 = 0.429043, c2 = 0.511664, c3 = 0.743125, c4 = 0.886227, c5 = 0.247708;
    vec3 L00 = cam.sh[0].rgb;
    vec3 L1m1 = cam.sh[1].rgb, L10 = cam.sh[2].rgb, L11 = cam.sh[3].rgb;
    vec3 L2m2 = cam.sh[4].rgb, L2m1 = cam.sh[5].rgb, L20 = cam.sh[6].rgb;
    vec3 L21 = cam.sh[7].rgb, L22 = cam.sh[8].rgb;
    float x = n.x, y = n.y, z = n.z;
    vec3 E = c1 * L22 * (x * x - y * y) + c3 * L20 * (z * z) + c4 * L00 - c5 * L20
           + 2.0 * c1 * (L2m2 * x * y + L21 * x * z + L2m1 * y * z)
           + 2.0 * c2 * (L11 * x + L1m1 * y + L10 * z);
    return max(E, vec3(0.0));
}

// --- Cook-Torrance microfacet BRDF terms (GGX / Smith / Schlick) ------------------------------------
float distGGX(float ndh, float a) {
    float a2 = a * a;
    float d = (ndh * ndh) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-6);
}
float geomSmith(float ndv, float ndl, float a) {
    // @p a is roughness²; the Schlick-GGX direct-light remap is k = roughness²/2 (i.e. a/2).
    float k = a * 0.5;
    float gv = ndv / (ndv * (1.0 - k) + k);
    float gl = ndl / (ndl * (1.0 - k) + k);
    return gv * gl;
}
vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
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

    // Alpha = per-draw opacity × the diffuse texel's alpha. The decode layer bakes any opacity
    // (cutout) mask into the diffuse map's alpha channel (lash/brow cards, legacy eye shells), so
    // one sample covers both; unmasked textures carry alpha 1 and the multiply is a no-op. Only the
    // transparent pass blends, so opaque-pass meshes are unaffected either way.
    vec4  diffuseTexel = texture(uDiffuse, vUv);
    vec3  albedo = pc.baseColor.rgb * diffuseTexel.rgb;
    float alpha  = pc.baseColor.a * diffuseTexel.a;

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

    // --- PBR: Cook-Torrance GGX with image-based lighting (the default photoreal mode) ---
    // The environment does the heavy lifting: SH-reconstructed diffuse irradiance gives the soft,
    // directional, colour-bleeding ambient a flat term never could, and one analytic key light adds
    // crisp form + a highlight on top. No fill/rim lights — the environment supplies wrap + rim.
    if (mode == 1) {
        // Live dials from the Environment panel (via the UBO).
        float exposure    = cam.params.y;
        float specScale   = cam.params.z;
        float ambientFill = cam.params.w;
        float diffuseInt  = cam.params2.x;
        float keyInt      = cam.params2.y;
        float envRot      = cam.params2.z;   // environment Y-rotation (radians)
        float tonemapOn   = cam.params2.w;
        float sssAmount   = cam.params3.x;   // 0 = opaque Lambert, 1 = strongly translucent skin
        float rimInt      = cam.params3.y;   // photographic back-rim intensity (0 = off)

        float metallic = clamp(pc.material.z, 0.0, 1.0);
        vec3  f0 = mix(vec3(0.04), albedo, metallic);   // dielectric 4% ramping to metal = albedo tint
        vec3  diffuseAlbedo = albedo * (1.0 - metallic); // metals have no diffuse
        float a = rough * rough;

        // Fresnel energy split shared by the key + environment diffuse (kS + kD = 1).
        vec3  kS = fresnelSchlick(ndv, f0);
        vec3  kD = (1.0 - kS) * (1.0 - metallic);

        vec3 color = vec3(0.0);

        // Key light — kept modest (the environment already lights the form; a strong key double-lights
        // the facing side and blows the skin out). Its diffuse is a per-channel *wrapped* Lambert, the
        // classic cheap skin-translucency model: red wraps farthest past the terminator, then green,
        // then blue — producing the warm terminator band of real skin that decays to NEUTRAL darkness
        // deep in shadow (the previous additive band saturated across the whole far side, which is what
        // tinted the figure's back red). The (1+w)² denominator keeps each channel energy-conserving,
        // which also slightly relaxes the lit side instead of stacking onto it. Subsurface = 0 reduces
        // to plain Lambert.
        {
            vec3 wrapW   = vec3(0.40, 0.16, 0.08) * sssAmount;
            vec3 onePlus = vec3(1.0) + wrapW;
            vec3 wrapped = clamp((vec3(rawNdl) + wrapW) / (onePlus * onePlus), 0.0, 1.0);
            color += kD * diffuseAlbedo * (1.0 / PI) * wrapped * cam.lightColor.rgb * keyInt;
            if (rawNdl > 0.0) {
                // GGX specular, dialed to ~25%: full dielectric spec reads oily/plastic on skin and
                // blows the eye catchlight into a white blob (skin scatters much of it subsurface).
                float D = distGGX(ndh, a);
                float G = geomSmith(ndv, rawNdl, a);
                vec3  F = fresnelSchlick(vdh, f0);
                vec3  spec = (D * G) * F / max(4.0 * ndv * rawNdl, 1e-4);
                color += spec * 0.25 * cam.lightColor.rgb * keyInt * rawNdl;
            }
        }

        // Environment diffuse: per-normal irradiance from the SH bake (sampled through the environment
        // rotation), Fresnel-split so grazing angles hand energy to the specular.
        vec3 irradiance = shIrradiance(rotateY(n, envRot));
        color += kD * diffuseAlbedo * irradiance * (1.0 / PI) * diffuseInt;

        // Ambient fill: a small flat lift on the diffuse albedo so the shadow side of a directional
        // HDRI (the figure's back/far side) doesn't crush to near-black — reduces front-to-back contrast.
        color += diffuseAlbedo * ambientFill;

        // Environment specular (split-sum): prefiltered environment radiance sampled along the (rotated)
        // reflection at a mip chosen by roughness, scaled by the environment-BRDF LUT and by specScale —
        // dialed well below physical for skin, a subsurface dielectric that reads as wet plastic at full
        // dielectric specular.
        vec3 R = reflect(-v, n);
        float specMip = rough * float(textureQueryLevels(uPrefilteredSpec) - 1);
        vec3 prefiltered = textureLod(uPrefilteredSpec, rotateY(R, envRot), specMip).rgb;
        vec2 brdf = texture(uBrdfLut, vec2(ndv, rough)).rg;
        color += prefiltered * (f0 * brdf.x + brdf.y) * specScale;

        // Photographic back-rim: a cool edge where the surface grazes the view while facing the fixed
        // back light — the portrait rim that lifts the dark side off the background (what a studio
        // photographer adds precisely because the unlit side of a subject otherwise goes flat/dark).
        float rimEdge = pow(1.0 - ndv, 3.0) * max(dot(n, normalize(cam.rimDir.xyz)), 0.0);
        color += cam.rimColor.rgb * rimEdge * rimInt;

        color *= exposure;
        outColor = vec4(tonemapOn > 0.5 ? tonemapACES(color) : clamp(color, 0.0, 1.0), alpha);
        return;
    }

    // --- Rendered (mode 0): skin-friendly Blinn-Phong + subsurface, lit by the analytic 3-point rig ---
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
    outColor = vec4(color, alpha);
}
