#version 450

// depthcutout.frag — a SEE-THROUGH mesh drawn as an OCCLUDER of the Gaussian splat layer: depth
// only (every color write is masked in its pipeline), and only where the surface has enough
// coverage to count as being there.
//
// The splats are depth-tested against the scene's depth, and the scene's depth is the OPAQUE
// meshes' — a transparent mesh (a hair card, an eyelash, a glass pane) is blended without
// writing any. So a splat BEHIND a strand of hair that has nothing opaque behind it passed the
// test and was drawn over the hair: in front of a capture, a head of hair cards lost its outline.
// Drawn through this shader after the scene's own passes, those meshes write the depth they
// have where they are opaque enough:
//
//   - the COARSE cutout (MeshDrawKind::DepthCutout, the viewport): from half coverage. A thin
//     fringe stays the splats' — it would otherwise show the backdrop it was blended over, a
//     pale halo around the hair;
//   - the FINE cutout (MeshDrawKind::DepthCutoutFine, a render's FRONT layer): from a twelfth.
//     There the capture behind the hair is the render's own plate, blended under each strand by
//     its coverage, and only the splats truly in front of the hair may be laid over it.
//
// mesh.vert's partner (its varyings by location; the one this stage needs is the UV), the mesh
// pipelines' layout: the material buffer for the zone's opacity, the base color map for its
// cutout. A separate small shader rather than a branch of mesh.frag: a `discard` in the lit
// shader would cost every opaque draw its early depth write.

#define MATERIAL_SET 2
#include "material.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    vec4 meta; // x = the mesh's material index, y = the draw kind (+ the highlight packing)
} pc;

layout(set = 1, binding = 0) uniform sampler2D uDiffuse; // the cutout mask rides in its alpha

layout(location = 1) in vec2 vUv;

layout(location = 0) out vec4 outColor; // masked: the pass writes depth alone
layout(location = 1) out vec4 outSpec;

const float kCoarseCoverage = 0.5;
const float kFineCoverage = 0.08;

void main() {
    float cover = materials.m[int(pc.meta.x + 0.5)].baseColor.a * texture(uDiffuse, vUv).a;
    float kind = mod(pc.meta.y, 8.0);
    if (cover < (kind > 6.5 ? kFineCoverage : kCoarseCoverage)) {
        discard;
    }
    outColor = vec4(0.0);
    outSpec = vec4(0.0);
}
