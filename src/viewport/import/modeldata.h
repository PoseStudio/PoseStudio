/**
 * @file modeldata.h
 * @brief The format-neutral CPU-side geometry every model importer produces and the renderer
 *        consumes — a Model made of per-material Meshes.
 *
 * This is the contract between the import/ subsystem and scene/: importers (import/meshimporter.h,
 * import/obj/…) fill a ModelData; VulkanWindow's import service decodes its textures; Scene turns it
 * into device buffers (scene/mesh.h). Pure data + std + GLM — no Vulkan, no Qt — so importers stay
 * trivially testable and the type carries no format identity of its own.
 */

#ifndef MODELDATA_H
#define MODELDATA_H

#include "vertex.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pose {

/// Where a mesh's diffuse image comes from. An importer sets exactly one of these (or neither, for
/// an untextured mesh): an external file that lives on disk (Wavefront .mtl map_Kd, resolved to an
/// absolute path), or encoded image bytes embedded inside the model file itself (e.g. a texture
/// packed into a .glb). The Qt layer decodes whichever is present via QImage, so the Qt-free
/// renderer never needs an image codec regardless of how the source arrived.
struct TextureSource {
    std::string          path;    ///< Resolved external file; empty if embedded/none.
    std::vector<uint8_t> encoded; ///< Embedded encoded image bytes (PNG/JPEG/…); empty if path/none.

    bool empty() const { return path.empty() && encoded.empty(); }
};

/// One material group of an imported model: a self-contained triangle mesh, its diffuse base color,
/// and (optionally) its diffuse texture.
struct MeshData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    // Material diffuse color when present, else this default for geometry with no material/texture:
    // a light neutral grey, hex #D1D1D1. Stored LINEAR (sRGB->linear of 209/209/209) because the lit
    // fragment output is hardware-encoded to the sRGB swapchain — so a fully-lit surface reads as
    // exactly #D1D1D1 on screen. Same convention as the renderer's clear-colour note.
    glm::vec3             baseColor{0.6376f, 0.6376f, 0.6376f};

    // Diffuse texture source (an external path or embedded bytes; see TextureSource). The Qt layer
    // (ModelImportService) decodes it into diffusePixels (tightly-packed RGBA8) so the Qt-free
    // renderer never needs an image codec. Empty source / zero size => untextured.
    TextureSource         diffuse;
    std::vector<uint8_t>  diffusePixels;
    uint32_t              diffuseWidth  = 0;
    uint32_t              diffuseHeight = 0;

    // Detail (surface) map: a tangent-space normal map or a grayscale bump/height map, selected by
    // normalMode. Decoded like the diffuse map, but uploaded as a LINEAR texture (it's data, not
    // colour). Empty => flat shading. normalMode: 0 none, 1 tangent-space normal, 2 grayscale bump.
    TextureSource         normal;
    std::vector<uint8_t>  normalPixels;
    uint32_t              normalWidth  = 0;
    uint32_t              normalHeight = 0;
    int                   normalMode   = 0;

    // Opacity (cutout) mask: a grayscale image whose texels modulate this mesh's opacity (lash and
    // brow cards, older generations' eye shells). Not uploaded as its own texture: the decode layer
    // bakes it into the diffuse map's ALPHA channel (the shader already samples the diffuse, so the
    // mask rides along for free) and sets hasOpacityMask, which routes the mesh to the alpha-blended
    // transparent pass even when the scalar opacity is 1 (a lash card's scalar is 1 — its shape
    // exists only in the mask).
    TextureSource         opacityMask;
    bool                  hasOpacityMask = false; // set by the decode layer once the bake succeeds

    // Specular parameter maps (both LINEAR, sampled .r): the roughness map multiplies the scalar
    // roughness per texel; the spec-mask map multiplies specularWeight per texel (the decode layer
    // switches specularWeight to the material's undiscounted fold when the mask decodes — see
    // MaterialRefs::specularWeightWithMap). Empty => 1x1 white fallback (scalar behaviour).
    TextureSource         roughnessMap;
    std::vector<uint8_t>  roughnessPixels;
    uint32_t              roughnessWidth  = 0;
    uint32_t              roughnessHeight = 0;
    TextureSource         specMask;
    std::vector<uint8_t>  specMaskPixels;
    uint32_t              specMaskWidth  = 0;
    uint32_t              specMaskHeight = 0;

    // Translucency map (sRGB colour: the flesh-red transmitted tint, or a grayscale weight map —
    // luminance = per-region strength). White fallback = uniform.
    TextureSource         translucencyMap;
    std::vector<uint8_t>  translucencyPixels;
    uint32_t              translucencyWidth  = 0;
    uint32_t              translucencyHeight = 0;

    // Micro-detail (pore) normal map: LINEAR tangent-space normals tiled detailTiles× across the
    // UVs at detailWeight strength. No gutter fill (it wraps, and tiled UVs leave no gutters).
    TextureSource         detailNormalMap;
    std::vector<uint8_t>  detailNormalPixels;
    uint32_t              detailNormalWidth  = 0;
    uint32_t              detailNormalHeight = 0;
    float                 detailWeight = 0.0f;
    float                 detailTiles  = 0.0f;

    float                 roughness = 0.7f; // 0 = mirror-smooth, 1 = fully rough (matte).
    float                 specularWeight = 1.0f; // F0 multiplier (1 = 4% dielectric; skin ~0.5).
    float                 specularWeightWithMap = 1.0f; // see the map comment above
    float                 metalness = 0.0f; // 0 dielectric … 1 metal (F0 = albedo).
    float                 lobe1Roughness = 0.7f; // dual-lobe specular; lobeRatio 1 = single lobe
    float                 lobe2Roughness = 0.7f; //   (the shader then uses `roughness` as before)
    float                 lobeRatio      = 1.0f; // lobe 1's fraction of the specular
    float                 topCoatWeight    = 0.0f;  // thin clear-coat layer (0 = none)
    float                 topCoatRoughness = 0.65f;
    float                 translucencyWeight = 0.5f; // × the translucency map's luminance
    float                 opacity   = 1.0f; // < 1 => drawn in the transparent pass, alpha-blended.

    // For pose correctives: the source *base* (pre-de-index) vertex index each render vertex
    // came from — the key that maps a corrective's base-vertex deltas onto these render vertices.
    // Parallel to `vertices`; empty for static/OBJ meshes (they have no correctives).
    std::vector<uint32_t> baseVertex;
};

// --- Pose correctives (joint-driven corrective morphs) ----------------------------------------
// A rigged figure ships corrective shapes that fire as a function of joint rotation to fix the mesh
// crumpling at bent elbows/shoulders/hips/knees ("candy-wrapper"). Each is a sparse morph whose blend
// weight is driven by one or more joint-rotation angles through a tiny driver formula. The engine keeps
// the base mesh on the CPU and re-morphs it (position += weight · delta) whenever the pose changes.

/// One knot of a corrective's driver spline: at driver angle `x` (degrees) the weight is `y`. The
/// source authors these as TCB knots with tension/continuity/bias ~0, so the evaluator interpolates
/// them as a Catmull-Rom Hermite spline and clamps outside the knot range.
struct CorrectiveKnot {
    float x = 0.0f;
    float y = 0.0f;
};

/// One operation of a corrective formula — a tiny stack program evaluated at pose time. The native
/// base correctives use only this vocabulary: push a driver angle / a constant, a driver spline, and
/// multiply/add.
struct CorrectiveOp {
    enum class Kind {
        PushRotation, ///< Push driver joint `bone`'s current Euler angle (degrees) about `axis`.
        PushConst,    ///< Push the constant `value`.
        Spline,       ///< Pop a driver value, push the spline through `knots` evaluated at it.
        Mult,         ///< Pop b, a; push a*b.
        Add           ///< Pop b, a; push a+b.
    };
    Kind                        kind  = Kind::PushConst;
    std::string                 bone;          ///< PushRotation: driver joint name.
    int                         axis  = 0;     ///< PushRotation: 0=x, 1=y, 2=z (rotation/{x,y,z}).
    float                       value = 0.0f;  ///< PushConst: the constant.
    std::vector<CorrectiveKnot> knots;         ///< Spline: driver→weight knots (ascending in x).
};

/// One driver formula: a stack program yielding a scalar contribution.
struct CorrectiveFormula {
    std::vector<CorrectiveOp> ops;
};

/// A pose-driven corrective morph. weight = (Σ over sumFormulas) × gateScale, where gateScale folds
/// in the character/enable gates resolved to constants at import (a corrective whose gate resolved to
/// 0 — e.g. its character isn't dialed in — is dropped at import and never reaches the engine).
struct PoseCorrective {
    std::string                                 id;
    std::vector<CorrectiveFormula>              sumFormulas; ///< Summed; joint-rotation driven.
    float                                       gateScale = 1.0f;
    std::vector<std::pair<uint32_t, glm::vec3>> deltas;      ///< (base-vertex index, displacement).
};

/// One skeleton joint for GPU skinning: its parent (index into ModelData::bones, -1 for the root)
/// and its rest (bind) world transform. The renderer derives inverse-bind and the per-frame skin
/// matrices from these; the mesh vertices' joint indices index into this list.
struct ModelBone {
    std::string name;              ///< Joint name (e.g. "lForearmBend") — lets callers pose by name.
    int         parent = -1;
    glm::mat4   bindGlobal{1.0f};  ///< Rest world transform (translation-only; orientation cancels).
    glm::vec3   orientation{0.0f}; ///< Rest orientation (Euler degrees) — the frame pose rotations act in.
    std::string rotationOrder = "XYZ"; ///< Order the pose Euler angles compose in.
    // Per-axis pose-rotation limits (degrees): the joint's anatomical range of motion. Only axes
    // flagged in `rotationLimited` are enforced; others rotate freely. Defaults leave every axis free,
    // so a static model / an OBJ (no skeleton) is unaffected.
    glm::vec3   rotationMin{0.0f};
    glm::vec3   rotationMax{0.0f};
    glm::bvec3  rotationLimited{false, false, false};
};

/// One imported model: its material groups, plus an optional skeleton. An empty `bones` list means a
/// static model — the renderer binds a single identity joint, which its vertices default to.
struct ModelData {
    std::vector<MeshData>       meshes;
    std::vector<ModelBone>      bones;
    std::vector<PoseCorrective> correctives; ///< Pose-driven corrective morphs; empty for static models.
};

} // namespace pose

#endif // MODELDATA_H
