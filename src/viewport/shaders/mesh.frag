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
    mat4 lightViewProj; // key light's ortho view-projection (shadow-map space)
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
    vec4 params4; // x/y = backdrop dials (unused here), z = shadow intensity (0..1), w reserved
} cam;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 baseColor; // rgb tint, a = opacity
    vec4 material;  // x = roughness (ratio-blended), y = normalMode (0 none / 1 normal / 2 bump),
                    // z = metalness, w = specular (F0) weight (1 = 4% dielectric; skin ~0.5)
    vec4 material2; // x = lobe1 rough, y = lobe2 rough, z = lobe ratio (1 = single-lobe),
                    // w = translucency weight
    vec4 material3; // x = top-coat weight (0 = none), y = top-coat roughness, z/w reserved
} pc;

layout(set = 1, binding = 0) uniform sampler2D uDiffuse;   // sRGB colour map (white 1x1 fallback)
layout(set = 1, binding = 1) uniform sampler2D uDetail;    // linear normal/bump map (flat fallback)
layout(set = 1, binding = 2) uniform sampler2D uRoughMap;  // linear roughness ×-map (white fallback)
layout(set = 1, binding = 3) uniform sampler2D uSpecMask;  // linear spec-weight mask (white fallback)
layout(set = 1, binding = 4) uniform sampler2D uTransMap;    // sRGB translucency tint/weight map (white)
layout(set = 1, binding = 5) uniform sampler2D uMicroNormal; // linear tiled pore-normal map (flat)

// IBL maps (set 3): the specular half of split-sum image-based lighting. The diffuse half is the SH
// in the camera UBO. Prefiltered cube: mip = roughness. BRDF LUT: (NdotV, roughness) -> (scale, bias).
layout(set = 3, binding = 0) uniform samplerCube uPrefilteredSpec;
layout(set = 3, binding = 1) uniform sampler2D  uBrdfLut;
// The key light's shadow map (comparison sampler: each tap returns 1 = lit). Rendered by the
// depth-only shadow pass each frame; the border is opaque white so outside-frustum reads are lit.
layout(set = 3, binding = 2) uniform sampler2DShadow uShadowMap;

layout(location = 0) in vec3 vWorldNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in float vAo; // baked ambient occlusion (1 = open; import-time hemisphere bake)
layout(location = 4) in vec4 vTangent; // baked UV tangent + handedness (w = 0 -> none)

layout(location = 0) out vec4 outColor;
// The HDR pass's second colour attachment: the PBR opaque pass's SPECULAR, kept out of the
// screen-space SSS blur (blurred glints smear into a wet-looking film) and added back by the
// blur's V pass. Only the opaque mesh pipeline writes this attachment (others mask it off).
layout(location = 1) out vec4 outSpec;

const float PI = 3.14159265359;

// Builds a tangent basis from screen-space derivatives of position + uv (Schüler's cotangent frame).
// The fallback for geometry without baked tangents — see tangentFrame() below.
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

// The surface tangent frame: the import-baked UV tangents when present (cleaner + stable across
// UDIM seams — see tangentgen.h), else the screen-space cotangent fallback.
mat3 tangentFrame(vec3 n, vec3 p, vec2 uv) {
    if (abs(vTangent.w) > 0.5) {
        vec3 t = normalize(vTangent.xyz - n * dot(n, vTangent.xyz));
        vec3 b = cross(n, t) * vTangent.w;
        return mat3(t, b, n);
    }
    return cotangentFrame(n, p, uv);
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

// Key-light visibility from the shadow map: 3x3 PCF through the hardware comparison sampler
// (1 = fully lit). Depth bias is applied at shadow-raster time (the pipeline's static bias);
// z outside [0,1] or uv outside the map (white border) simply read lit.
// @p geomN / @p ndlGeom: the GEOMETRIC surface normal and its N·L against the key — the sample
// point is pushed off the surface along it (normal-offset shadows) before projecting. The static
// raster-time depth bias alone under-compensates at grazing key angles (a low raking key across a
// vertical thigh), where self-shadowing acne draws soft streak lines down every figure — the
// offset grows as N·L falls, exactly where acne lives, while contact shadows stay put.
float keyShadow(vec3 worldPos, vec3 geomN, float ndlGeom) {
    // One shadow texel in world units, from the fitted ortho matrix's row scale (row0 length =
    // 1 / frustum half-width; the map is square).
    vec3  r0 = vec3(cam.lightViewProj[0][0], cam.lightViewProj[1][0], cam.lightViewProj[2][0]);
    float mapSize = float(textureSize(uShadowMap, 0).x);
    float texelWorld = 2.0 / (max(length(r0), 1e-6) * mapSize);
    vec3  offsetPos = worldPos + geomN * texelWorld * (1.0 + 2.5 * (1.0 - ndlGeom));

    vec4 lp = cam.lightViewProj * vec4(offsetPos, 1.0);
    vec3 p = lp.xyz / lp.w;
    if (p.z <= 0.0 || p.z >= 1.0) {
        return 1.0;
    }
    vec2 uv = p.xy * 0.5 + 0.5;
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += texture(uShadowMap, vec3(uv + vec2(x, y) * texel, p.z));
        }
    }
    return sum / 9.0;
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
    outSpec = vec4(0.0); // only the PBR opaque path routes specular here
    int mode = int(cam.params.x + 0.5);

    vec3 n = normalize(vWorldNormal);
    if (!gl_FrontFacing) {
        n = -n; // light back faces as if they faced us (no back-face culling; winding is inconsistent)
    }

    // Detail-map normal perturbation (uniform branch on the per-draw normalMode). material.y packs
    // mode + authored strength (floor = mode, fract×8 = strength): heavily-detailed skins dial
    // e.g. Normal Map 2.5 / Bump 4, and applying their maps at a fixed 1x left the surface so
    // coherent that the broad specular sheen read as wet plastic on smooth body regions.
    int nm = int(floor(pc.material.y));
    float detailStrength = fract(pc.material.y) * 8.0;
    if (nm == 1) {
        vec3 mapN = texture(uDetail, vUv).xyz * 2.0 - 1.0;
        mapN.xy *= detailStrength; // authored strength steepens/flattens the mapped slope
        n = normalize(tangentFrame(n, vWorldPos, vUv) * mapN);
    } else if (nm == 2) {
        vec2 texel = 1.0 / vec2(textureSize(uDetail, 0));
        float hL = texture(uDetail, vUv - vec2(texel.x, 0.0)).r;
        float hR = texture(uDetail, vUv + vec2(texel.x, 0.0)).r;
        float hD = texture(uDetail, vUv - vec2(0.0, texel.y)).r;
        float hU = texture(uDetail, vUv + vec2(0.0, texel.y)).r;
        vec2 dFine = vec2(hR - hL, hU - hD);
        // HIGH-PASS the bump: a second central difference with a 4-texel arm captures the broad,
        // low-frequency slope (veins, stretch bands, large forms), which is subtracted out — for
        // a feature wider than the arm the two gradients cancel exactly. The bump layer must read
        // as pore-scale GRAIN only: the source renderer's tiny physical bump scale never shows
        // its maps' vein content, but our slope-space bump did, drawing "weird stretch-mark
        // lines" down every figure's thighs at any overall factor.
        float bL = texture(uDetail, vUv - vec2(4.0 * texel.x, 0.0)).r;
        float bR = texture(uDetail, vUv + vec2(4.0 * texel.x, 0.0)).r;
        float bD = texture(uDetail, vUv - vec2(0.0, 4.0 * texel.y)).r;
        float bU = texture(uDetail, vUv + vec2(0.0, 4.0 * texel.y)).r;
        vec2 dBroad = vec2(bR - bL, bU - bD) * 0.25; // normalized to the fine gradient's units
        // 1.5 = height→slope base for the remaining grain, scaled by the authored strength.
        vec2 dH = (dFine - dBroad) * 1.5 * detailStrength;
        n = normalize(tangentFrame(n, vWorldPos, vUv) * vec3(-dH.x, -dH.y, 1.0));
    }

    // Micro-detail (pore) normal: the newer skin shader's tiled grain layer, applied over the base
    // perturbation. material3.w packs it (integer part = UV tiling, fraction = weight; 0 = none —
    // the push block is at its 128-byte limit, so the pair shares one float).
    float detailPacked = pc.material3.w;
    if (detailPacked >= 1.0) {
        float detailTiles = floor(detailPacked);
        float detailWeight = fract(detailPacked);
        vec3 dn = texture(uMicroNormal, vUv * detailTiles).xyz * 2.0 - 1.0;
        dn.xy *= detailWeight;
        n = normalize(tangentFrame(n, vWorldPos, vUv) * normalize(dn));
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
    // Scalar roughness × its per-texel map (white fallback = scalar-only): the source materials
    // author per-region variation — T-zone oil, matte body, glossy lips — in these maps.
    float roughTexel = texture(uRoughMap, vUv).r;
    // Geometric specular anti-aliasing (Kaplanyan/Tokuyoshi-style): where the SHADED normal turns
    // fast across the screen — the ridges of a strong detail map (stretch marks, pores), silhouette
    // curvature — a pixel really covers a whole fan of normals, and evaluating a narrow lobe at
    // just one of them draws hot white streaks (the classic normal-map specular aliasing; Sahel's
    // 2.5x stretch-mark ridges lit them up under the environment). Widen the lobes by the normal's
    // screen-space variance so ridge highlights broaden and dim into a sheen instead.
    vec3  dndx = dFdx(n);
    vec3  dndy = dFdy(n);
    // Screen-space variance fades as the camera closes in (the pixel footprint shrinks), so add a
    // ZOOM-INDEPENDENT term: how far the detail map bent this pixel's normal off the geometric
    // one. A strongly-authored ridge (stretch marks at 2.5x) is a steep micro-slope whose real
    // reflection is spread by the ridge's own curvature — evaluating a narrow lobe on it draws a
    // hot white streak at any viewing distance.
    vec3  geomN = normalize(vWorldNormal) * (gl_FrontFacing ? 1.0 : -1.0);
    float slopeDev = 1.0 - clamp(dot(n, geomN), 0.0, 1.0);
    float normalVariance =
        clamp(0.5 * (dot(dndx, dndx) + dot(dndy, dndy)) + 0.8 * slopeDev, 0.0, 0.35);
    float rough = clamp(pc.material.x * roughTexel + normalVariance, 0.04, 1.0);

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
        // Dielectric F0 = 4% × the material's specular weight — the import layer derives the weight
        // from the source material's active specular lobe (its reflectivity: skin authors ~0.3,
        // i.e. ~2% F0 — the real thing) × the per-texel spec-mask map (white fallback = scalar
        // only; when the real mask decoded, material.w carries the UNdiscounted fold and the mask
        // texels do the regional scaling). Clamped to 1 = the dielectric ceiling either way, then
        // ramping to metal = albedo tint.
        float specWeight = min(pc.material.w * texture(uSpecMask, vUv).r, 1.0);
        vec3  f0 = mix(vec3(0.04) * specWeight, albedo, metallic);
        vec3  diffuseAlbedo = albedo * (1.0 - metallic); // metals have no diffuse
        float a = rough * rough;

        // Fresnel energy split shared by the key + environment diffuse (kS + kD = 1).
        vec3  kS = fresnelSchlick(ndv, f0);
        vec3  kD = (1.0 - kS) * (1.0 - metallic);

        vec3 color = vec3(0.0);   // DIFFUSE light — diffuses through skin (the SSS blur's input)
        vec3 specSum = vec3(0.0); // SPECULAR — surface reflection, must stay crisp (own attachment)

        // Key light — kept modest (the environment already lights the form; a strong key double-lights
        // the facing side and blows the skin out). Its diffuse is a per-channel *wrapped* Lambert, the
        // classic cheap skin-translucency model: red wraps farthest past the terminator, then green,
        // then blue — producing the warm terminator band of real skin that decays to NEUTRAL darkness
        // deep in shadow (the previous additive band saturated across the whole far side, which is what
        // tinted the figure's back red). The (1+w)² denominator keeps each channel energy-conserving,
        // which also slightly relaxes the lit side instead of stacking onto it. Subsurface = 0 reduces
        // to plain Lambert.
        // Shadow the key light (diffuse + specular + top coat alike): self-shadowing under the
        // chin/nose, arm-on-torso, etc. The environment terms stay unshadowed (that's AO's job).
        // The panel's shadow-intensity dial blends the occlusion toward lit (0 = shadowing off).
        float ndlGeom = clamp(dot(geomN, l), 0.0, 1.0);
        float shadow = mix(1.0, keyShadow(vWorldPos, geomN, ndlGeom), cam.params4.z);

        // Dual-lobe specular: skin authors a broad + a tighter GGX lobe blended by a ratio; a
        // ratio of 1 marks a single-lobe material (OBJ, glossy-lobe surfaces) which uses the
        // ratio-blended `rough` exactly as before. Both lobes share the roughness map.
        float lobeRatio = clamp(pc.material2.z, 0.0, 1.0);
        // Both lobes get the same specular-AA variance widening as the single-lobe path — the
        // TIGHT lobe is exactly where ridge aliasing shows.
        float rough1 = clamp(pc.material2.x * roughTexel + normalVariance, 0.04, 1.0);
        float rough2 = clamp(pc.material2.y * roughTexel + normalVariance, 0.04, 1.0);
        bool  dualLobe = lobeRatio < 0.999;

        // Per-region translucency: authored weight × the map (a flesh-red colour map carries the
        // transmitted tint; a grayscale weight map reads through its luminance; white fallback =
        // uniform). Scales the skin wrap AND the backlit transmission — cloth/props authored at
        // 0 stop wrapping like skin.
        vec3  transTexel = texture(uTransMap, vUv).rgb;
        float transAmount = clamp(pc.material2.w, 0.0, 1.0) *
                            clamp(dot(transTexel, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        // 2×: the neutral default (weight 0.5 × white map) reproduces the plain dial behaviour.
        float sssLocal = clamp(sssAmount * 2.0 * transAmount, 0.0, 1.0);

        {
            vec3 wrapW   = vec3(0.40, 0.16, 0.08) * sssLocal;
            vec3 onePlus = vec3(1.0) + wrapW;
            vec3 wrapped = clamp((vec3(rawNdl) + wrapW) / (onePlus * onePlus), 0.0, 1.0);
            color += kD * diffuseAlbedo * (1.0 / PI) * wrapped * cam.lightColor.rgb * keyInt * shadow;
            if (rawNdl > 0.0 && shadow > 0.0) {
                // GGX at full strength (materials tame themselves via real roughness + F0); the
                // dual-lobe path blends the two lobes' distributions.
                float D;
                float G;
                if (dualLobe) {
                    float a1 = rough1 * rough1;
                    float a2 = rough2 * rough2;
                    D = mix(distGGX(ndh, a2), distGGX(ndh, a1), lobeRatio);
                    G = mix(geomSmith(ndv, rawNdl, a2), geomSmith(ndv, rawNdl, a1), lobeRatio);
                } else {
                    D = distGGX(ndh, a);
                    G = geomSmith(ndv, rawNdl, a);
                }
                vec3  F = fresnelSchlick(vdh, f0);
                vec3  keySpec = (D * G) * F / max(4.0 * ndv * rawNdl, 1e-4);
                specSum += keySpec * cam.lightColor.rgb * keyInt * rawNdl * shadow;
            }
            // Backlit transmission: key light entering from BEHIND glows through thin translucent
            // regions (ears, nostrils, finger webbing). The through-vector is the light bent by
            // the surface; blood makes transmitted light red, so the tint is the translucency
            // map's colour × a warm absorption profile over the albedo. Deliberately NOT gated by
            // the shadow map — this is exactly the light that passes through the occluder.
            if (transAmount > 0.0) {
                vec3  through = normalize(-l + n * 0.35);
                float transDot = pow(clamp(dot(v, -through), 0.0, 1.0), 4.0);
                vec3  transTint = transTexel * albedo * vec3(1.0, 0.42, 0.25);
                color += transTint * transDot * transAmount * sssAmount * 1.2 *
                         cam.lightColor.rgb * keyInt;
            }
        }

        // Baked ambient occlusion gates the ENVIRONMENT terms only (the key light has real shadow
        // mapping): cavities — eye sockets, nostrils, under the chin, between fingers — stop
        // receiving full sky light. Specular occlusion uses the tighter ao² curve (reflections die
        // in cavities faster than diffuse skylight does).
        float ao = clamp(vAo, 0.0, 1.0);
        float specAo = ao * ao;

        // Environment diffuse: per-normal irradiance from the SH bake (sampled through the environment
        // rotation), Fresnel-split so grazing angles hand energy to the specular.
        vec3 irradiance = shIrradiance(rotateY(n, envRot));
        color += kD * diffuseAlbedo * irradiance * (1.0 / PI) * diffuseInt * ao;

        // Ambient fill: a small flat lift on the diffuse albedo so the shadow side of a directional
        // HDRI (the figure's back/far side) doesn't crush to near-black — reduces front-to-back contrast.
        color += diffuseAlbedo * ambientFill * ao;

        // Environment specular (split-sum): prefiltered environment radiance sampled along the (rotated)
        // reflection at a mip chosen by roughness, scaled by the environment-BRDF LUT and the panel's
        // Specular dial. The grazing (F90) bias term also scales with the material's specular weight,
        // so a matte-authored surface stays matte at glancing angles instead of picking up a wet sheen.
        vec3 R = reflect(-v, n);
        float maxMip = float(textureQueryLevels(uPrefilteredSpec) - 1);
        // Dominant-direction correction (Frostbite): a rough lobe is wide, but the split-sum
        // prefilter is sampled along a single direction — strictly along R, a bumpy normal aiming
        // R at a bright ceiling softbox draws a hot white streak even at near-max roughness (the
        // "wet streak marks" on strongly normal-mapped skin). Bend the sample direction toward the
        // normal as the lobe widens (α = rough²), so a broad lobe averages the hemisphere around
        // the SURFACE rather than mirroring whatever R hits.
        vec3 prefiltered;
        vec2 brdf;
        if (dualLobe) {
            // Two prefiltered samples + two LUT fetches, blended by the lobe ratio — the broad
            // lobe's soft sheen and the tight lobe's sharper reflection coexist like real skin.
            vec3 dir1 = rotateY(normalize(mix(R, n, rough1 * rough1)), envRot);
            vec3 dir2 = rotateY(normalize(mix(R, n, rough2 * rough2)), envRot);
            prefiltered = mix(textureLod(uPrefilteredSpec, dir2, rough2 * maxMip).rgb,
                              textureLod(uPrefilteredSpec, dir1, rough1 * maxMip).rgb, lobeRatio);
            brdf = mix(texture(uBrdfLut, vec2(ndv, rough2)).rg,
                       texture(uBrdfLut, vec2(ndv, rough1)).rg, lobeRatio);
        } else {
            vec3 envDir = rotateY(normalize(mix(R, n, a)), envRot);
            prefiltered = textureLod(uPrefilteredSpec, envDir, rough * maxMip).rgb;
            brdf = texture(uBrdfLut, vec2(ndv, rough)).rg;
        }
        // Horizon fade: a detail-map-perturbed normal can point the reflection vector below the
        // real surface horizon — environment light from *under* the skin, which reads as an oily
        // film sitting on every bump. Fade the environment specular as R sinks under the vertex
        // (geometric) normal's hemisphere.
        vec3 vertexN = geomN; // the geometric normal computed for the specular-AA slope term
        float horizon = clamp(1.0 + dot(R, vertexN), 0.0, 1.0);
        horizon *= horizon;
        // Grazing (F90) bias: the split-sum's white B-term models the coherent grazing sheen of a
        // SMOOTH dielectric interface. A micro-rough surface (broad-lobe skin) loses that
        // coherence to micro self-shadowing and multiple scattering — left undamped, the white
        // term over a bright studio environment lays a milky "wet" veil across the environment-lit
        // side of the figure (worst exactly on the roughness map's roughest blotches, and most
        // visible from behind where no key light competes). Damp it by (1 − roughness): the
        // smooth cornea keeps its grazing glint, rough skin dries out. specWeight already carries
        // the per-texel spec mask (≤ 1 by construction).
        float grazing = brdf.y * specWeight * (1.0 - rough);
        // A rough lobe's gathered environment light is the same light the diffuse term already
        // integrates — the split-sum overshoots it, and what remains reads as a milky veil pinned
        // to the roughness map's brightest (roughest) blotches, worst on the environment-lit back
        // where no key light competes. Fade the environment specular out DECISIVELY as roughness
        // rises: gone by rough ≈ 0.95, at half strength ≈ 0.7 — broad-rough skin keeps only a
        // whisper of sheen, while the smooth cornea/lips (rough ≲ 0.45) are untouched.
        float envSpecFade = 1.0 - smoothstep(0.45, 0.95, rough);
        specSum +=
            prefiltered * (f0 * brdf.x + grazing) * specScale * horizon * specAo * envSpecFade;

        // Top Coat: the thin clear/oily layer skins author over the base lobes (weight ~0.35-0.45,
        // roughness ~0.6-0.7) — a second dielectric specular layer at the standard 4% F0, on both
        // the key light and the environment. The subtle extra sheen that reads as living skin
        // rather than dry clay. (Base-layer attenuation under the coat is skipped — at these
        // weights it's below visibility.)
        float tcWeight = clamp(pc.material3.x, 0.0, 1.0);
        if (tcWeight > 0.0) {
            float tcRough = clamp(pc.material3.y, 0.04, 1.0);
            vec3  f0c = vec3(0.04);
            if (rawNdl > 0.0 && shadow > 0.0) {
                float aC = tcRough * tcRough;
                float Dc = distGGX(ndh, aC);
                float Gc = geomSmith(ndv, rawNdl, aC);
                vec3  Fc = fresnelSchlick(vdh, f0c);
                specSum += (Dc * Gc) * Fc / max(4.0 * ndv * rawNdl, 1e-4) * cam.lightColor.rgb *
                           keyInt * rawNdl * shadow * tcWeight;
            }
            vec3 envDirC = rotateY(normalize(mix(R, n, tcRough * tcRough)), envRot);
            vec3 preC = textureLod(uPrefilteredSpec, envDirC, tcRough * maxMip).rgb;
            vec2 brdfC = texture(uBrdfLut, vec2(ndv, tcRough)).rg;
            // Same anti-veil treatment as the base lobes: damp the white grazing bias by the
            // coat's roughness and fade the term as it saturates (an undamped 0.65-rough coat's
            // grazing energy alone re-glazed figures whose coat IS their main specular).
            float grazingC = brdfC.y * (1.0 - tcRough);
            float coatFade = 1.0 - 0.75 * smoothstep(0.6, 1.0, tcRough);
            specSum += preC * (f0c * brdfC.x + grazingC) * specScale * horizon * specAo *
                       coatFade * tcWeight;
        }

        // Photographic back-rim: a cool edge where the surface grazes the view while facing the fixed
        // back light — the portrait rim that lifts the dark side off the background (what a studio
        // photographer adds precisely because the unlit side of a subject otherwise goes flat/dark).
        float rimEdge = pow(1.0 - ndv, 3.0) * max(dot(n, normalize(cam.rimDir.xyz)), 0.0);
        specSum += cam.rimColor.rgb * rimEdge * rimInt * ao; // no rim glow inside cavities

        color *= exposure;
        specSum *= exposure;
        // LINEAR HDR out — the scene renders offscreen now and the composite pass applies ACES
        // (post-bloom, so bloom thresholds see real radiance). ALPHA SEMANTICS: in the opaque pass
        // alpha carries the per-pixel translucency as the SSS MASK (the screen-space subsurface
        // blur's kernel scale — skin ~0.9, cloth/props 0, eyes ~0, backdrop 0); the transparent
        // pass keeps real blend opacity (its pipeline doesn't write alpha, but the blend factors
        // read it). SPECULAR routing: the opaque pass writes it to attachment 1 so the SSS blur
        // can't smear it (the blur's V pass adds it back); the transparent pass can't write
        // attachment 1 (masked) — its specular stays in-line, fine because its pixels aren't
        // blurred (SSS mask ≈ 0 under the clear shells).
        if (pc.material3.z > 0.5) {
            outColor = vec4(color + specSum, alpha);
        } else {
            outColor = vec4(color, sssLocal);
            outSpec = vec4(specSum, 0.0);
        }
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
