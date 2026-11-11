// material.glsl — the per-model MATERIAL buffer (set MATERIAL_SET, binding 3): one entry per
// mesh, packed by Mesh::packGpu (the C++ twin is MaterialGpu in mesh.h — the two must stay
// byte-identical), indexed by the push block's meta.x. Until 2026-11-09 the material rode in
// the push block itself, which was at the 128-byte limit; the buffer has room for the whole
// canonical material (materialparams.h), of which the shader reads what it renders.
//
// The reader defines MATERIAL_SET (2 in the main pass) before including this file.

#ifndef MATERIAL_SET
#error "Define MATERIAL_SET (the descriptor set index of the material buffer) before including material.glsl"
#endif

struct MaterialGpu {
    vec4 baseColor;         // rgb tint (linear), a = opacity
    vec4 material;          // x roughness (ratio-blended), y normalMode packed with the detail
                            //   strength (floor = mode, fract×8 = strength), z metalness,
                            //   w specular (F0) weight (1 = the standard 4% dielectric)
    vec4 material2;         // x lobe-1 roughness, y lobe-2 roughness, z lobe ratio (1 = one lobe),
                            //   w translucency (subsurface) weight
    vec4 material3;         // x top-coat weight, y top-coat roughness, z unused (the draw kind
                            //   rides the push block now), w micro-detail packed as
                            //   floor(tiles) + fract(weight) (0 = none)
    vec4 emission;          // rgb emission radiance factor (linear colour × luminance), w the
                            //   BUMP strength of a bump map carried beside a normal map
    vec4 uvTransform;       // xy tiling, zw offset (applied to the UVs in the vertex stage)
    vec4 specularColor;     // rgb specular tint (a dielectric's F0 colour), w anisotropy (0..1)
    vec4 transmission;      // x weight, y depth, z thin-walled (0/1), w index of refraction
    vec4 transmissionColor; // rgb tint, w dispersion
    vec4 subsurface;        // rgb colour, w radius scale
    vec4 sheen;             // x weight, y roughness, z the anisotropy's rotation (radians), w coat ior
    vec4 sheenColor;        // rgb, w thin-film thickness
    vec4 coatColor;         // rgb, w thin-film ior
    vec4 extra;             // x diffuse roughness, y thin-film weight, z unused, w = 1 when a bump map
                            //   is bound beside the normal map (its strength rides emission.w)
    vec4 detail;            // the DETAIL layer's placement: xy its tiling (x = the count material3.w
                            //   packs), zw its offset in repeats
    vec4 extra2;            // x the detail layer's roughness multiplier (1 = none), y the coat's
                            //   anisotropy, z its rotation (radians), w the UV rotation (radians)
};

layout(std430, set = MATERIAL_SET, binding = 3) readonly buffer Materials {
    MaterialGpu m[];
} materials;
