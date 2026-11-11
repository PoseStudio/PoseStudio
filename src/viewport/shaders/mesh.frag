#version 450

// Mesh fragment shader with a selectable *shade mode* (the viewport's shader picker). One über-shader
// branches on cam.params.x — a per-frame uniform, so every fragment in a draw takes the same branch
// (coherent, cheap). Modes (the picker's rows map onto these through scene/shademode.h — the
// picker's own order is NOT this numbering; 3 and 4 are UNUSED numbers, kept vacant so the
// reachable modes stay stable — the skin/metal matcaps that lived there never had a picker row):
//   0 Rendered (the picker's "Texture Shaded")    1 PBR         2 Matcap (Studio)   3-4 unused
//   5 Toon           6 Clay            7 Lighting Only
//   8 Flat Shaded    9 Normals         10 Albedo (unlit) 11 UV Checker
//   12 Ambient Occlusion (the baked vAo)   13 Silhouette (flat fill)
//   14 Specular only (the PBR path's material specular — key GGX + environment + top coat —
//      without diffuse or rim: the roughness and spec maps at work; HDR like PBR)
//   15 Roughness map (the material's scalar roughness × its map, as grey: white = matte)
// Shared across the shaded modes: a detail map (tangent-space normal = mode 1, grayscale bump = mode 2)
// perturbs the normal through the import-baked UV tangent frame (vTangent; a screen-space
// cotangent frame is the fallback for geometry without baked tangents — see tangentFrame());
// lighting is two-sided (the normal AND the tangent frame's handedness flip to face the viewer
// regardless of winding); and alpha = the per-draw opacity, so the same shader serves the opaque
// and the alpha-blended transparent pass. Two further DRAW KINDS bypass the modes entirely
// (pc.meta.y): the wireframe overlay's edges (a flat grey) and the hidden-line surface fill
// (depth only; its pipeline masks the colour writes).
//
// Pass:    the HDR scene pass (HdrTarget), opaque + transparent + wire + hidden-line variants.
// Inputs:  set 0.0 the camera/lighting UBO (camera_ubo.glsl); set 1.0-5 the material maps; set 3
//          the IBL maps + shadow map; push: the 128-byte MeshPushConstants block (mesh.h).
// Outputs: location 0 -> colour attachment 0 (scene; alpha = SSS mask in the opaque pass),
//          location 1 -> colour attachment 1 (the PBR opaque pass's specular; masked elsewhere).

#extension GL_GOOGLE_include_directive : enable
#include "camera_ubo.glsl" // the set-0 camera/lighting block (cam) + rotateY
#include "colour.glsl"     // tonemapACES (the matcap mode) + kSelectionAccent (the highlight)

#define MATERIAL_SET 2 // the per-model material buffer (binding 3 of the pose set)
#include "material.glsl"

// The push block (MeshPushConstants in mesh.h): the model matrix and meta — x the mesh's index
// into the material buffer, y the draw kind (MeshDrawKind: 0 opaque (alpha = SSS mask),
// 1 transparent (alpha = blend opacity), 2 wireframe edges, 3 hidden-line depth fill, 4 and 5 a
// transmissive zone's MULTIPLY and ADD draws — see the PBR branch's end) packed with
// the selected joint + its twist twin (read by the vertex stage). The MATERIAL itself — base
// colour, roughness, metalness, the lobes, the coat, the detail layers — is the buffer entry
// main() fetches into `surf` before shading (it rode in the push block until 2026-11-09, at the
// 128-byte limit).
layout(push_constant) uniform Push {
    mat4 model;
    vec4 meta;
} pc;

MaterialGpu surf; // this draw's material (set in main)
bool gTransmitShaded = false; // a transmission draw (kinds 4 / 5) answered by the PBR branch (see main)

layout(set = 1, binding = 0) uniform sampler2D uDiffuse;   // sRGB colour map (white 1x1 fallback)
layout(set = 1, binding = 1) uniform sampler2D uDetail;    // linear normal/bump map (flat fallback)
layout(set = 1, binding = 2) uniform sampler2D uRoughMap;  // linear PARAMETER map: roughness (R), metalness (G), ambient occlusion (B), transmission weight (A); white fallback
layout(set = 1, binding = 3) uniform sampler2D uSpecMask;  // linear spec-weight mask (white fallback)
layout(set = 1, binding = 4) uniform sampler2D uTransMap;    // sRGB translucency tint/weight map (white)
layout(set = 1, binding = 5) uniform sampler2D uMicroNormal; // linear tiled pore-normal map (flat)
layout(set = 1, binding = 6) uniform sampler2D uBump;        // linear bump / height map beside a normal map (flat fallback)
layout(set = 1, binding = 7) uniform sampler2D uEmission;    // sRGB emission map (white fallback)
layout(set = 1, binding = 8) uniform sampler2D uLayerMap;    // linear LAYER map: coat weight (R), coat roughness (G), sheen weight (B), thin-film thickness (A); white fallback
layout(set = 1, binding = 9) uniform sampler2D uSheenColor;  // sRGB sheen colour map (white fallback)

// The slope a grayscale bump (height) map gives the normal at uv — tangent space, applied as
// (-dH.x, -dH.y, 1): a mip-stable texture-space central difference, HIGH-PASSED — a second
// central difference with a 4-texel arm captures the broad, low-frequency slope (veins, stretch
// bands, large forms), which is subtracted out; for a feature wider than the arm the two
// gradients cancel exactly. The bump layer must read as pore-scale GRAIN only: the source
// renderer's tiny physical bump scale never shows its maps' vein content, but our slope-space
// bump did, drawing "weird stretch-mark lines" down every figure's thighs at any overall factor.
// 1.5 = the height→slope base for the remaining grain, scaled by the authored strength.
vec2 bumpSlope(sampler2D map, vec2 uv, float strength) {
    vec2 texel = 1.0 / vec2(textureSize(map, 0));
    float hL = texture(map, uv - vec2(texel.x, 0.0)).r;
    float hR = texture(map, uv + vec2(texel.x, 0.0)).r;
    float hD = texture(map, uv - vec2(0.0, texel.y)).r;
    float hU = texture(map, uv + vec2(0.0, texel.y)).r;
    vec2 dFine = vec2(hR - hL, hU - hD);
    float bL = texture(map, uv - vec2(4.0 * texel.x, 0.0)).r;
    float bR = texture(map, uv + vec2(4.0 * texel.x, 0.0)).r;
    float bD = texture(map, uv - vec2(0.0, 4.0 * texel.y)).r;
    float bU = texture(map, uv + vec2(0.0, 4.0 * texel.y)).r;
    vec2 dBroad = vec2(bR - bL, bU - bD) * 0.25; // normalized to the fine gradient's units
    return (dFine - dBroad) * 1.5 * strength;
}

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
layout(location = 5) in float vSelect; // skin weight on the SELECTED joint (+ twin): the highlight amount

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
    float m = max(dot(T, T), dot(B, B));
    // Degenerate UVs (constant across the triangle) leave no tangent direction — inversesqrt(0)
    // is inf, and one non-finite normal spreads through the SSS/bloom blurs as a blotch. Fall
    // back to an arbitrary orthonormal frame; detail perturbation degrades to (finite) noise.
    if (m < 1e-16) {
        vec3 t = normalize(abs(N.y) < 0.99 ? cross(vec3(0.0, 1.0, 0.0), N)
                                           : cross(vec3(1.0, 0.0, 0.0), N));
        return mat3(t, cross(N, t), N);
    }
    float invmax = inversesqrt(m);
    return mat3(T * invmax, B * invmax, N);
}

// The surface tangent frame: the import-baked UV tangents when present (cleaner + stable across
// UDIM seams — see tangentgen.h), else the screen-space cotangent fallback. @p tangent is the
// interpolated vTangent with its handedness (w) already flipped for a back face (see shade()).
mat3 tangentFrame(vec3 n, vec3 p, vec2 uv, vec4 tangent) {
    if (abs(tangent.w) > 0.5) {
        // Gram-Schmidt the interpolated tangent against the shaded normal — guarding the
        // degenerate case where interpolation across a UV-seam split lands the tangent (near-)
        // parallel to n: normalize(≈0) is NaN, and one NaN pixel spreads through the blurs.
        vec3 tRaw = tangent.xyz - n * dot(n, tangent.xyz);
        float len2 = dot(tRaw, tRaw);
        if (len2 > 1e-12) {
            vec3 t = tRaw * inversesqrt(len2);
            vec3 b = cross(n, t) * tangent.w;
            return mat3(t, b, n);
        }
    }
    return cotangentFrame(n, p, uv);
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

// --- Anisotropic GGX (Burley's stretched lobe with the height-correlated Smith visibility, as
// Filament): the highlight drawn out along the surface's tangent frame — brushed metal, hair, a
// record's grooves. at / ab are the roughness α along the two tangent axes; at = ab is the
// isotropic lobe exactly. The visibility carries the 1 / (4 N·V N·L) already.
float distGGXAniso(float ndh, float tdh, float bdh, float at, float ab) {
    float a2 = at * ab;
    vec3  d = vec3(ab * tdh, at * bdh, a2 * ndh);
    float d2 = dot(d, d);
    float w2 = a2 / max(d2, 1e-12);
    return a2 * w2 * w2 / PI;
}
float visSmithGGXAniso(float ndv, float ndl, float tdv, float bdv, float tdl, float bdl, float at, float ab) {
    float lambdaV = ndl * length(vec3(at * tdv, ab * bdv, ndv));
    float lambdaL = ndv * length(vec3(at * tdl, ab * bdl, ndl));
    return 0.5 / max(lambdaV + lambdaL, 1e-5);
}
// One stretched lobe's D·V for a roughness α and an anisotropy in 0..1 (0 = round): α along the
// grain grows by (1 + anisotropy), across it shrinks by (1 − anisotropy).
float ggxAnisoDV(float a, float aniso, float ndh, float ndv, float ndl, float tdh, float bdh, float tdv,
                 float bdv, float tdl, float bdl) {
    float at = max(a * (1.0 + aniso), 1e-3);
    float ab = max(a * (1.0 - aniso), 1e-3);
    return distGGXAniso(ndh, tdh, bdh, at, ab) * visSmithGGXAniso(ndv, ndl, tdv, bdv, tdl, bdl, at, ab);
}
// The environment's reflection under a stretched lobe: the normal bent toward the grain's plane
// (Filament's bent normal), so the prefiltered environment smears along the stretch too.
vec3 anisoReflect(vec3 v, vec3 n, vec3 bitangent, float aniso) {
    vec3 anisoT = cross(bitangent, v);
    vec3 anisoN = cross(anisoT, bitangent);
    return reflect(-v, normalize(mix(n, anisoN, aniso)));
}

// --- Sheen: Estevez & Kulla's "Charlie" distribution with Neubelt's visibility — the back-scatter
// of fibres at grazing angles (velvet, felt, peach fuzz). @p a is the sheen roughness squared.
float distCharlie(float ndh, float a) {
    float invA = 1.0 / max(a, 1e-3);
    float sin2h = max(1.0 - ndh * ndh, 0.0078125); // 2^-7
    return (2.0 + invA) * pow(sin2h, invA * 0.5) / (2.0 * PI);
}
float visNeubelt(float ndv, float ndl) {
    return 1.0 / max(4.0 * (ndl + ndv - ndl * ndv), 1e-5);
}

// --- Thin-film interference (Belcour & Barla 2017, in the form the glTF iridescence model takes):
// the reflectance of a film of index filmIor and thickness thicknessNm over a base of reflectance
// baseF0, seen at cosTheta — the colours of a soap bubble, oil on water, a beetle's shell. Three
// Gaussians stand in for the eye's spectral sensitivity (in XYZ, converted to linear Rec.709).
vec3 filmSensitivity(float opd, vec3 shift) {
    float phase = 2.0 * PI * opd * 1.0e-9;
    vec3  val = vec3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    vec3  pos = vec3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    vec3  var = vec3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    vec3  xyz = val * sqrt(2.0 * PI * var) * cos(pos * phase + shift) * exp(-(phase * phase) * var);
    xyz.x += 9.7470e-14 * sqrt(2.0 * PI * 4.5282e+09) * cos(2.2399e+06 * phase + shift.x) *
             exp(-4.5282e+09 * phase * phase);
    xyz /= 1.0685e-7;
    const mat3 xyzToRec709 = mat3( 3.2404542, -0.9692660,  0.0556434,
                                  -1.5371385,  1.8760108, -0.2040259,
                                  -0.4985314,  0.0415560,  1.0572252);
    return xyzToRec709 * xyz;
}
vec3 fresnelFromIor(vec3 iorT, float iorI) {
    vec3 r = (iorT - vec3(iorI)) / (iorT + vec3(iorI));
    return r * r;
}
vec3 iorFromFresnel(vec3 f0) {
    vec3 s = sqrt(f0);
    return (vec3(1.0) + s) / (vec3(1.0) - s);
}
vec3 thinFilmFresnel(float filmIor, float cosTheta1, float thicknessNm, vec3 baseF0) {
    const float outsideIor = 1.0;
    // A vanishing film is no film: its index goes to the air's with its thickness.
    float filmIorEff = mix(outsideIor, filmIor, smoothstep(0.0, 0.03, thicknessNm));
    float eta = outsideIor / filmIorEff;
    float sinTheta2Sq = eta * eta * (1.0 - cosTheta1 * cosTheta1);
    float cosTheta2Sq = 1.0 - sinTheta2Sq;
    if (cosTheta2Sq < 0.0) {
        return vec3(1.0); // total internal reflection
    }
    float cosTheta2 = sqrt(cosTheta2Sq);
    // The air-film interface.
    float r0 = (filmIorEff - outsideIor) / (filmIorEff + outsideIor);
    r0 *= r0;
    float r12 = r0 + (1.0 - r0) * pow(1.0 - cosTheta1, 5.0);
    float t121 = 1.0 - r12;
    float phi12 = filmIorEff < outsideIor ? PI : 0.0;
    float phi21 = PI - phi12;
    // The film-base interface (the base's index read back from its reflectance).
    vec3  baseIor = iorFromFresnel(clamp(baseF0, vec3(0.0), vec3(0.9999)));
    vec3  r1 = fresnelFromIor(baseIor, filmIorEff);
    vec3  r23 = r1 + (vec3(1.0) - r1) * pow(1.0 - cosTheta2, 5.0);
    vec3  phi23 = vec3(baseIor.x < filmIorEff ? PI : 0.0, baseIor.y < filmIorEff ? PI : 0.0,
                       baseIor.z < filmIorEff ? PI : 0.0);
    // The optical path difference and the phase shifts, then the Airy sum's first terms.
    float opd = 2.0 * filmIorEff * thicknessNm * cosTheta2;
    vec3  phi = vec3(phi21) + phi23;
    vec3  r123 = clamp(r12 * r23, vec3(1e-5), vec3(0.9999));
    vec3  rs = t121 * t121 * r23 / (vec3(1.0) - r123);
    vec3  I = r12 + rs;
    vec3  cm = rs - vec3(t121);
    vec3  r123sqrt = sqrt(r123);
    for (int m = 1; m <= 2; ++m) {
        cm *= r123sqrt;
        I += cm * 2.0 * filmSensitivity(float(m) * opd, float(m) * phi);
    }
    return max(I, vec3(0.0));
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

// --- Procedural matcap: shade a "material sphere" from the view-space normal (vn.z points at eye) ---
vec3 matcapStudio(vec3 vn) {
    float t = smoothstep(-0.85, 1.0, vn.y);
    vec3 c = mix(vec3(0.06, 0.07, 0.09), vec3(0.82, 0.85, 0.90), t);
    c += vec3(1.0) * pow(max(dot(vn, normalize(vec3(-0.5, 0.55, 0.65))), 0.0), 28.0) * 0.9; // key
    c += vec3(0.18, 0.20, 0.26) * pow(max(dot(vn, normalize(vec3(0.6, 0.1, 0.6))), 0.0), 4.0); // fill
    c += vec3(0.25, 0.30, 0.42) * pow(1.0 - max(vn.z, 0.0), 3.0); // rim
    return c;
}

// The shading proper (main() below wraps it with the selection highlight). Every mode returns
// from here with outColor (and, in the PBR opaque case, outSpec) written.
void shade() {
    outSpec = vec4(0.0); // only the PBR opaque path routes specular here
    // pc.meta.y packs the DRAW KIND (its low 3 bits — the rest is the selected joint, read by
    // the vertex stage). Kinds beyond the lit passes: the hidden-line DEPTH FILL (3) has every
    // colour write masked in its pipeline — nothing to shade, so return before any texture
    // fetch — and the WIREFRAME edges (2) are a flat grey from the UBO's wire level (the wire
    // pipeline rasterizes each triangle as its edges; alpha 0 keeps the SSS mask untouched).
    // (The transmission's kinds 4 and 5 are LIT draws of the transparent pass: see the PBR
    // branch's end, and main() for the other modes.)
    float drawKind = mod(pc.meta.y, 8.0);
    if (drawKind > 2.5 && drawKind < 3.5) {
        outColor = vec4(0.0);
        return;
    }
    if (drawKind > 1.5 && drawKind < 2.5) {
        outColor = vec4(vec3(cam.params4.w), 0.0);
        return;
    }
    int mode = int(cam.params.x + 0.5);

    vec3 n = normalize(vWorldNormal);
    vec4 tangent = vTangent;
    if (!gl_FrontFacing) {
        // Light back faces as if they faced us (no back-face culling; winding is inconsistent).
        // The baked handedness flips WITH the normal: the frame's bitangent is cross(n, t) * w,
        // so negating n alone mirrored the frame and a detail map on a back face perturbed toward
        // the wrong side of its tangent — the flipped-winding "front" faces some imports produce
        // shaded their normal maps inverted.
        n = -n;
        tangent.w = -tangent.w;
    }

    // Detail-map normal perturbation (uniform branch on the per-draw normalMode). material.y packs
    // mode + authored strength (floor = mode, fract×8 = strength): heavily-detailed skins dial
    // e.g. Normal Map 2.5 / Bump 4, and applying their maps at a fixed 1x left the surface so
    // coherent that the broad specular sheen read as wet plastic on smooth body regions.
    int nm = int(floor(surf.material.y));
    float detailStrength = fract(surf.material.y) * 8.0;
    if (nm == 1) {
        vec3 mapN = texture(uDetail, vUv).xyz * 2.0 - 1.0;
        mapN.xy *= detailStrength; // authored strength steepens/flattens the mapped slope
        n = normalize(tangentFrame(n, vWorldPos, vUv, tangent) * mapN);
        // A bump map CARRIED BESIDE the normal map (extra.w = 1 when one is bound, emission.w its
        // strength — the Materials tab's Bump Strength): its grain over the mapped normal.
        if (surf.extra.w > 0.5) {
            vec2 dH = bumpSlope(uBump, vUv, surf.emission.w);
            n = normalize(tangentFrame(n, vWorldPos, vUv, tangent) * vec3(-dH.x, -dH.y, 1.0));
        }
    } else if (nm == 2) {
        vec2 dH = bumpSlope(uDetail, vUv, detailStrength); // (the high-passed bump: see bumpSlope)
        n = normalize(tangentFrame(n, vWorldPos, vUv, tangent) * vec3(-dH.x, -dH.y, 1.0));
    }

    // Micro-detail (pore) normal: the newer skin shader's tiled grain layer, applied over the base
    // perturbation. material3.w packs it (integer part = UV tiling, fraction = weight; 0 = none —
    // the push block is at its 128-byte limit, so the pair shares one float).
    float detailPacked = surf.material3.w;
    if (detailPacked >= 1.0) {
        // Its placement rides material.detail (xy tiling — x the count the packing floors — zw
        // offset): a square layer at no offset samples exactly as the packed count did.
        vec2  detailTiles = surf.detail.xy;
        float detailWeight = fract(detailPacked);
        vec3 dn = texture(uMicroNormal, vUv * detailTiles + surf.detail.zw).xyz * 2.0 - 1.0;
        dn.xy *= detailWeight;
        n = normalize(tangentFrame(n, vWorldPos, vUv, tangent) * normalize(dn));
    }

    // Alpha = per-draw opacity × the diffuse texel's alpha. The decode layer bakes any opacity
    // (cutout) mask into the diffuse map's alpha channel (lash/brow cards, legacy eye shells), so
    // one sample covers both; unmasked textures carry alpha 1 and the multiply is a no-op. Only the
    // transparent pass blends, so opaque-pass meshes are unaffected either way.
    vec4  diffuseTexel = texture(uDiffuse, vUv);
    vec3  albedo = surf.baseColor.rgb * diffuseTexel.rgb;
    float alpha  = surf.baseColor.a * diffuseTexel.a;

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
    // (The parameter map's other channels: G the metalness map, B an ambient occlusion map, A a
    // transmission weight map — 1 where the material has none.)
    vec4  paramTexel = texture(uRoughMap, vUv);
    // Geometric specular anti-aliasing (Kaplanyan/Tokuyoshi-style): where the SHADED normal turns
    // fast across the screen — the ridges of a strong detail map (stretch marks, pores), silhouette
    // curvature — a pixel really covers a whole fan of normals, and evaluating a narrow lobe at
    // just one of them draws hot white streaks (the classic normal-map specular aliasing; one test
    // character's 2.5x-strength stretch-mark ridges lit them up under the environment). Widen the
    // lobes by the normal's screen-space variance so ridge highlights broaden into a sheen instead.
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
    float rough = clamp(surf.material.x * roughTexel + normalVariance, 0.04, 1.0);
    // The detail layer's roughness multiplier (extra2.x, 1 = none) by the layer's weight: the
    // pores read a little rougher than the skin between them. A uniform branch — at 1 every
    // lobe is as it was.
    float detailRoughMul = 1.0;
    if (detailPacked >= 1.0 && surf.extra2.x != 1.0) {
        detailRoughMul = mix(1.0, max(surf.extra2.x, 0.0), fract(detailPacked));
        rough = clamp(rough * detailRoughMul, 0.04, 1.0);
    }

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
    if (mode == 12) {             // Ambient Occlusion: the import-time per-vertex bake, as grey
        outColor = vec4(vec3(clamp(vAo, 0.0, 1.0)), alpha);
        return;
    }
    if (mode == 13) {             // Silhouette: one flat light fill — pose readability, nothing else
        outColor = vec4(vec3(0.8), alpha);
        return;
    }
    if (mode == 15) {             // Roughness Map: the material's roughness as authored (scalar × map),
                                  // WITHOUT the specular-AA widening the lit modes add — the data itself
        outColor = vec4(vec3(clamp(surf.material.x * roughTexel, 0.0, 1.0)), alpha);
        return;
    }

    // --- Matcap (view-space normal): the procedural studio sphere. Its HDR range is tonemapped
    // inline — unlike the PBR family it writes a display-ready value (the composite passes it through).
    if (mode == 2) {
        vec3 vn = normalize(mat3(cam.view) * n);
        outColor = vec4(tonemapACES(matcapStudio(vn)), alpha);
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
    // Mode 14 (the Roughness / Specular Map view) runs this same path and outputs only the
    // material specular it accumulates (see the end of the branch).
    if (mode == 1 || mode == 14) {
        // Live dials from the Environment panel (via the UBO).
        float exposure    = cam.params.y;
        float specScale   = cam.params.z;
        float ambientFill = cam.params.w;
        float diffuseInt  = cam.params2.x;
        float keyInt      = cam.params2.y;
        float envRot      = cam.params2.z;   // environment Y-rotation (radians)
        float sssAmount   = cam.params3.x;   // 0 = opaque Lambert, 1 = strongly translucent skin
        float rimInt      = cam.params3.y;   // photographic back-rim intensity (0 = off)

        // The metalness: the scalar × the parameter map's GREEN channel — the metalness map where
        // the material has one (glTF's metallic-roughness texture's blue, an FBX / USD / MTL map,
        // read into this channel by the decode layer), 1 where it has none.
        float metallic = clamp(surf.material.z * texture(uRoughMap, vUv).g, 0.0, 1.0);
        // Dielectric F0 = 4% × the material's specular weight — the import layer derives the weight
        // from the source material's active specular lobe (its reflectivity: skin authors ~0.3,
        // i.e. ~2% F0 — the real thing) × the per-texel spec-mask map (white fallback = scalar
        // only; when the real mask decoded, material.w carries the UNdiscounted fold and the mask
        // texels do the regional scaling). Clamped to 1 = the dielectric ceiling either way, then
        // ramping to metal = albedo tint.
        float specWeight = min(surf.material.w * texture(uSpecMask, vUv).r, 1.0);
        // The dielectric's own reflectance is its INDEX OF REFRACTION's, ((ior-1)/(ior+1))²: 4% at
        // the standard 1.5 (the literal, so every material at the default is bit-identical to
        // before), 2% at water's 1.33, 17% at a diamond's 2.4 — × the weight, tinted by the
        // material's specular colour (white for nearly every real material).
        float ior = surf.transmission.w;
        float f0Dielectric = ior == 1.5 ? 0.04 : pow((ior - 1.0) / (ior + 1.0), 2.0);
        vec3  f0 = mix(vec3(f0Dielectric) * specWeight * surf.specularColor.rgb, albedo, metallic);
        vec3  diffuseAlbedo = albedo * (1.0 - metallic); // metals have no diffuse
        float a = rough * rough;

        // Fresnel energy split shared by the key + environment diffuse (kS + kD = 1).
        vec3  kS = fresnelSchlick(ndv, f0);
        vec3  kD = (1.0 - kS) * (1.0 - metallic);

        // THIN FILM (extra.y weight, sheenColor.w thickness in nm, coatColor.w the film's index):
        // the film's interference colours replace the base's plain Fresnel by the film's weight
        // — at the key light's half vector below, and at the view for the environment here.
        // (The LAYER map: the coat weight (R), the coat roughness (G), the sheen weight (B) and the
        // thin-film thickness (A) per texel — 1 where the material has no such map.)
        vec4  layerTexel = texture(uLayerMap, vUv);
        float filmW = clamp(surf.extra.y, 0.0, 1.0);
        float filmThickness = surf.sheenColor.w * layerTexel.a;
        vec3  f0Env = f0;
        if (filmW > 0.0) {
            f0Env = mix(f0, thinFilmFresnel(surf.coatColor.w, ndv, filmThickness, f0), filmW);
        }

        // ANISOTROPY (specularColor.w its strength, sheen.z the grain's direction about the
        // normal; the coat's own in extra2.y / .z): the tangent frame, turned to the grain, built
        // once for whichever lobe asks.
        float aniso = clamp(surf.specularColor.w, 0.0, 1.0);
        float anisoCoat = clamp(surf.extra2.y, 0.0, 1.0);
        bool  anisotropic = aniso > 0.001;
        bool  anisotropicCoat = anisoCoat > 0.001;
        vec3  T = vec3(0.0);
        vec3  B = vec3(0.0);
        vec3  Tc = vec3(0.0);
        vec3  Bc = vec3(0.0);
        if (anisotropic || anisotropicCoat) {
            mat3 frame = tangentFrame(n, vWorldPos, vUv, tangent);
            if (anisotropic) {
                float cr = cos(surf.sheen.z);
                float sr = sin(surf.sheen.z);
                T = frame[0] * cr + frame[1] * sr;
                B = frame[1] * cr - frame[0] * sr;
            }
            if (anisotropicCoat) {
                float cr = cos(surf.extra2.z);
                float sr = sin(surf.extra2.z);
                Tc = frame[0] * cr + frame[1] * sr;
                Bc = frame[1] * cr - frame[0] * sr;
            }
        }

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
        float shadow = 1.0;
        if (cam.params4.z > 0.0) { // dial at 0 = shadowing off: skip the 9-tap PCF entirely
            shadow = mix(1.0, keyShadow(vWorldPos, geomN, ndlGeom), cam.params4.z);
        }

        // Dual-lobe specular: skin authors a broad + a tighter GGX lobe blended by a ratio; a
        // ratio of 1 marks a single-lobe material (OBJ, glossy-lobe surfaces) which uses the
        // ratio-blended `rough` exactly as before. Both lobes share the roughness map.
        float lobeRatio = clamp(surf.material2.z, 0.0, 1.0);
        // Both lobes get the same specular-AA variance widening as the single-lobe path — the
        // TIGHT lobe is exactly where ridge aliasing shows.
        float rough1 = clamp(surf.material2.x * roughTexel + normalVariance, 0.04, 1.0);
        float rough2 = clamp(surf.material2.y * roughTexel + normalVariance, 0.04, 1.0);
        if (detailRoughMul != 1.0) {
            rough1 = clamp(rough1 * detailRoughMul, 0.04, 1.0);
            rough2 = clamp(rough2 * detailRoughMul, 0.04, 1.0);
        }
        bool  dualLobe = lobeRatio < 0.999;

        // Per-region translucency: authored weight × the map (a flesh-red colour map carries the
        // transmitted tint; a grayscale weight map reads through its luminance; white fallback =
        // uniform). Scales the skin wrap AND the backlit transmission — cloth/props authored at
        // 0 stop wrapping like skin.
        vec3  transTexel = texture(uTransMap, vUv).rgb;
        float transAmount = clamp(surf.material2.w, 0.0, 1.0) *
                            clamp(dot(transTexel, vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        // 2×: the neutral default (weight 0.5 × white map) reproduces the plain dial behaviour.
        float sssLocal = clamp(sssAmount * 2.0 * transAmount, 0.0, 1.0);

        {
            vec3 wrapW   = vec3(0.40, 0.16, 0.08) * sssLocal;
            vec3 onePlus = vec3(1.0) + wrapW;
            vec3 wrapped = clamp((vec3(rawNdl) + wrapW) / (onePlus * onePlus), 0.0, 1.0);
            vec3 keyDiffuse = kD * diffuseAlbedo * (1.0 / PI) * wrapped * cam.lightColor.rgb * keyInt * shadow;
            // DIFFUSE ROUGHNESS (extra.x): Oren-Nayar's retro-reflection over Lambert — cloth,
            // chalk, the moon brighten toward the light's side and flatten (Fujii's form; 0 is
            // Lambert exactly). On the key light; the environment's irradiance is its own.
            float sigma = clamp(surf.extra.x, 0.0, 1.0);
            if (sigma > 0.0) {
                float s = dot(l, v) - rawNdl * ndv;
                float t = s <= 0.0 ? 1.0 : max(ndl, ndv);
                keyDiffuse *= (1.0 + sigma * s / max(t, 1e-4)) / (1.0 + (0.5 - 2.0 / (3.0 * PI)) * sigma);
            }
            color += keyDiffuse;
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
                if (filmW > 0.0) {
                    F = mix(F, thinFilmFresnel(surf.coatColor.w, vdh, filmThickness, f0), filmW);
                }
                vec3  keySpec;
                if (anisotropic) {
                    float tdh = dot(T, hv);
                    float bdh = dot(B, hv);
                    float tdv = dot(T, v);
                    float bdv = dot(B, v);
                    float tdl = dot(T, l);
                    float bdl = dot(B, l);
                    float DV;
                    if (dualLobe) {
                        DV = mix(ggxAnisoDV(rough2 * rough2, aniso, ndh, ndv, rawNdl, tdh, bdh, tdv, bdv, tdl, bdl),
                                 ggxAnisoDV(rough1 * rough1, aniso, ndh, ndv, rawNdl, tdh, bdh, tdv, bdv, tdl, bdl),
                                 lobeRatio);
                    } else {
                        DV = ggxAnisoDV(a, aniso, ndh, ndv, rawNdl, tdh, bdh, tdv, bdv, tdl, bdl);
                    }
                    keySpec = DV * F;
                } else {
                    keySpec = (D * G) * F / max(4.0 * ndv * rawNdl, 1e-4);
                }
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
        // An AMBIENT OCCLUSION MAP (the parameter map's blue) by its weight (extra.z) over the
        // baked per-vertex term: a prop's authored occlusion where the vertex bake is coarse.
        if (surf.extra.z > 0.0) {
            ao *= mix(1.0, paramTexel.b, surf.extra.z);
        }
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
        vec3 Rb = anisotropic ? anisoReflect(v, n, B, aniso) : R; // the base lobes' (R keeps the horizon)
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
            vec3 dir1 = rotateY(normalize(mix(Rb, n, rough1 * rough1)), envRot);
            vec3 dir2 = rotateY(normalize(mix(Rb, n, rough2 * rough2)), envRot);
            prefiltered = mix(textureLod(uPrefilteredSpec, dir2, rough2 * maxMip).rgb,
                              textureLod(uPrefilteredSpec, dir1, rough1 * maxMip).rgb, lobeRatio);
            brdf = mix(texture(uBrdfLut, vec2(ndv, rough2)).rg,
                       texture(uBrdfLut, vec2(ndv, rough1)).rg, lobeRatio);
        } else {
            vec3 envDir = rotateY(normalize(mix(Rb, n, a)), envRot);
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
            prefiltered * (f0Env * brdf.x + grazing) * specScale * horizon * specAo * envSpecFade;

        // Top Coat: the thin clear/oily layer skins author over the base lobes (weight ~0.35-0.45,
        // roughness ~0.6-0.7) — a second dielectric specular layer at the standard 4% F0, on both
        // the key light and the environment. The subtle extra sheen that reads as living skin
        // rather than dry clay. (Base-layer attenuation under the coat is skipped — at these
        // weights it's below visibility.)
        float tcWeight = clamp(surf.material3.x * layerTexel.r, 0.0, 1.0);
        if (tcWeight > 0.0) {
            float tcRough = clamp(surf.material3.y * layerTexel.g, 0.04, 1.0);
            // The coat's reflectance from ITS index (sheen.w): 4% at the standard 1.5 (the literal,
            // so every skin's coat is bit-identical), 5.3% at a 1.6 lacquer, 2% at water's 1.33.
            float coatIor = surf.sheen.w;
            vec3  f0c = vec3(coatIor == 1.5 ? 0.04 : pow((coatIor - 1.0) / (coatIor + 1.0), 2.0));
            // COAT COLOUR (coatColor.rgb): what passes through the coat to the surface takes its
            // tint — the base, diffuse and specular, under a coloured lacquer (candy-apple red
            // over metal flakes). White leaves it as it was.
            if (any(lessThan(surf.coatColor.rgb, vec3(0.999)))) {
                vec3 coatTint = mix(vec3(1.0), clamp(surf.coatColor.rgb, 0.0, 1.0), tcWeight);
                color *= coatTint;
                specSum *= coatTint;
            }
            if (rawNdl > 0.0 && shadow > 0.0) {
                float aC = tcRough * tcRough;
                vec3  Fc = fresnelSchlick(vdh, f0c);
                if (anisotropicCoat) {
                    float DVc = ggxAnisoDV(aC, anisoCoat, ndh, ndv, rawNdl, dot(Tc, hv), dot(Bc, hv), dot(Tc, v),
                                           dot(Bc, v), dot(Tc, l), dot(Bc, l));
                    specSum += DVc * Fc * cam.lightColor.rgb * keyInt * rawNdl * shadow * tcWeight;
                } else {
                    float Dc = distGGX(ndh, aC);
                    float Gc = geomSmith(ndv, rawNdl, aC);
                    specSum += (Dc * Gc) * Fc / max(4.0 * ndv * rawNdl, 1e-4) * cam.lightColor.rgb *
                               keyInt * rawNdl * shadow * tcWeight;
                }
            }
            vec3 Rc = anisotropicCoat ? anisoReflect(v, n, Bc, anisoCoat) : R;
            vec3 envDirC = rotateY(normalize(mix(Rc, n, tcRough * tcRough)), envRot);
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

        // SHEEN (sheen.x weight, sheen.y roughness, sheenColor.rgb): the back-scatter of fibres at
        // grazing angles — velvet, felt, peach fuzz. The Charlie lobe on the key light; for the
        // environment its directional albedo (the LUT's blue channel, baked beside the GGX
        // terms) over the environment's IRRADIANCE — the lobe is broad enough that the SH
        // irradiance stands for it, where the GGX-prefiltered cube's 128 importance samples
        // showed as rings of dots under a strong sheen at mid roughness. The fuzz is the
        // outermost layer: everything beneath it loses what the fuzz reflects.
        float shW = clamp(surf.sheen.x * layerTexel.b, 0.0, 1.0);
        if (shW > 0.0) {
            float shRough = clamp(surf.sheen.y, 0.07, 1.0);
            float shA = shRough * shRough;
            vec3  shColor = clamp(surf.sheenColor.rgb * texture(uSheenColor, vUv).rgb, 0.0, 1.0) * shW;
            float shE = texture(uBrdfLut, vec2(ndv, shRough)).b;
            float shScale = 1.0 - max(max(shColor.r, shColor.g), shColor.b) * shE;
            color *= shScale;
            specSum *= shScale;
            if (rawNdl > 0.0 && shadow > 0.0) {
                specSum += shColor * distCharlie(ndh, shA) * visNeubelt(ndv, rawNdl) * cam.lightColor.rgb *
                           keyInt * rawNdl * shadow;
            }
            specSum += irradiance * (1.0 / PI) * shColor * shE * specScale * ao;
        }

        // The Roughness / Specular Map view: everything the MATERIAL reflects — the key light's
        // GGX lobes, the environment's split-sum specular, the top coat — and nothing else (no
        // diffuse, and no rim, which is a light rather than a material property). Highlight
        // breadth shows the roughness map, highlight strength the specular weight and its mask,
        // exactly as the PBR mode will render them. Linear HDR like PBR, so the composite's
        // ACES + bloom apply; alpha 0 = not skin for the SSS blur.
        if (mode == 14) {
            if (drawKind > 3.5) { // a transmissive zone: its reflection added over an untouched frame
                outColor = drawKind > 4.5 ? vec4(specSum * exposure, 0.0) : vec4(1.0);
                gTransmitShaded = true;
                return;
            }
            outColor = vec4(specSum * exposure, 0.0);
            return;
        }

        // Photographic back-rim: a cool edge where the surface grazes the view while facing the fixed
        // back light — the portrait rim that lifts the dark side off the background (what a studio
        // photographer adds precisely because the unlit side of a subject otherwise goes flat/dark).
        float rimEdge = pow(1.0 - ndv, 3.0) * max(dot(n, normalize(cam.rimDir.xyz)), 0.0);
        specSum += cam.rimColor.rgb * rimEdge * rimInt * ao; // no rim glow inside cavities

        // TRANSMISSION (transmission.x weight, .w the index, .z thin-walled, transmissionColor the
        // tint): a transmissive zone is drawn twice in the transparent pass — first MULTIPLYING the
        // frame behind it by what it lets through (kind 4), then ADDING its own reflection and
        // what is left of its diffuse (kind 5) — so the scene shows through tinted glass with the
        // environment's reflection on it. What passes is the transmission's share of the light
        // the surface does not reflect (1 − Fresnel), tinted by the transmission colour; through
        // a SOLID the path lengthens as the view grazes it (Snell), so the tint deepens toward the
        // edges, while a thin-walled sheet is its colour throughout. The view is not bent and a
        // rough glass does not blur what is behind it (that needs the opaque frame as a texture).
        float Tw = 0.0;
        vec3  transmittance = vec3(1.0);
        if (drawKind > 3.5) {
            Tw = clamp(surf.transmission.x * paramTexel.a, 0.0, 1.0) * (1.0 - metallic);
            vec3 tint = clamp(surf.transmissionColor.rgb, 0.0, 1.0);
            if (surf.transmission.z < 0.5) {
                float eta = 1.0 / max(surf.transmission.w, 1.0);
                float cosT = sqrt(max(1.0 - eta * eta * (1.0 - ndv * ndv), 0.0));
                tint = pow(max(tint, vec3(1e-4)), vec3(1.0 / max(cosT, 0.25)));
            }
            transmittance = vec3(1.0 - alpha) + alpha * Tw * (vec3(1.0) - kS) * tint;
            color *= 1.0 - Tw; // the diffuse the transmission replaces
            gTransmitShaded = true;
        }
        color *= exposure;
        specSum *= exposure;
        // EMISSION: the surface as a light — the material's colour × luminance (emission.rgb, 0 = none)
        // × its map — added as radiance, which the composite's bloom then spreads for a lamp or a
        // screen. (A uniform branch: an unlit material's shading is untouched, sample and all.)
        if (surf.emission.x + surf.emission.y + surf.emission.z > 0.0) {
            color += surf.emission.rgb * texture(uEmission, vUv).rgb * exposure;
        }
        // LINEAR HDR out — the scene renders offscreen now and the composite pass applies ACES
        // (post-bloom, so bloom thresholds see real radiance). ALPHA SEMANTICS: in the opaque pass
        // alpha carries the per-pixel translucency as the SSS MASK (the screen-space subsurface
        // blur's kernel scale — skin ~0.9, cloth/props 0, eyes ~0, backdrop 0); the transparent
        // pass keeps real blend opacity (its pipeline doesn't write alpha, but the blend factors
        // read it). SPECULAR routing: the opaque pass writes it to attachment 1 so the SSS blur
        // can't smear it (the blur's V pass adds it back); the transparent pass can't write
        // attachment 1 (masked) — its specular stays in-line, fine because its pixels aren't
        // blurred (SSS mask ≈ 0 under the clear shells).
        if (drawKind > 4.5) { // the transmission's ADD draw: the zone's own light over the frame
            outColor = vec4((color + specSum) * alpha, 0.0);
            return;
        }
        if (drawKind > 3.5) { // … and its MULTIPLY draw: what it lets through
            outColor = vec4(transmittance, 1.0);
            return;
        }
        if (drawKind > 0.5) { // draw kind 1 = the transparent pass
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

// The SELECTED-JOINT highlight, over whatever shade() produced, in every shade mode (the wire
// modes tint their wires): vSelect is the vertex's skin weight on the selected joint and its
// twist twin, so exactly the flesh that joint drives tints toward the selection blue, fading
// where the weights blend into the neighbouring bones — the "you grabbed THIS part" cue, with
// no bones drawn. Luminance-matched: the surface keeps its shading (form, highlights) and only
// its hue moves, with a floor so dark cavities still read as selected. Alpha is untouched (it
// is the SSS mask / blend opacity), and the specular attachment stays clean.
void main() {
    surf = materials.m[int(pc.meta.x + 0.5)]; // this draw's material (a uniform read per draw)
    float drawKind = mod(pc.meta.y, 8.0);
    shade();
    if (drawKind > 3.5 && !gTransmitShaded) {
        // A transmissive zone in a mode other than PBR (which answers its own draws above): the
        // frame behind it shows through by the transmission weight, tinted (the MULTIPLY draw),
        // under what is left of the mode's shaded surface (the ADD draw); at a weight of 0 the
        // two draws together are the plain alpha blend.
        float Tw = clamp(surf.transmission.x, 0.0, 1.0);
        float a = clamp(outColor.a, 0.0, 1.0);
        if (drawKind > 4.5) {
            outColor = vec4(outColor.rgb * a * (1.0 - Tw), 0.0);
        } else {
            outColor = vec4(vec3(1.0 - a) + a * Tw * clamp(surf.transmissionColor.rgb, 0.0, 1.0), 1.0);
        }
    }
    if (vSelect > 0.001 && !(drawKind > 3.5 && drawKind < 4.5)) { // (a multiply draw tints nothing)
        // kSelectionAccent (colour.glsl): linear #5b87cc, the same value PostProcess pushes for the outline.
        const float kAccentLum = 0.239;  // its Rec.709 luminance
        const float kStrength = 0.75;    // tint at full weight
        float lum = max(dot(outColor.rgb, vec3(0.2126, 0.7152, 0.0722)), 0.08);
        vec3 target = kSelectionAccent * (lum / kAccentLum) * 1.3;
        outColor.rgb = mix(outColor.rgb, target, clamp(vSelect, 0.0, 1.0) * kStrength);
    }
}
