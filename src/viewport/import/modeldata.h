/**
 * @file modeldata.h
 * @brief The format-neutral CPU-side geometry every model importer produces and the renderer
 *        consumes — a Model made of per-material Meshes.
 *
 * This is the contract between the import/ subsystem and scene/: importers (import/meshimporter.h,
 * import/obj/…, and the figure service's FigureData conversion) fill a ModelData; the Qt-facing
 * ModelImportService::decodeModelTextures decodes its textures; Scene turns it into device buffers
 * (scene/mesh.h). Pure data + std + GLM — no Vulkan, no Qt — so importers stay trivially testable
 * and the type carries no format identity of its own.
 */

#ifndef MODELDATA_H
#define MODELDATA_H

#include "vertex.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
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

/// A decoded, tightly-packed RGBA8 image, shared between the meshes that sample it. Zones of one
/// figure commonly reference the same atlas file (face/lips/ears share the face maps; torso/arms/
/// legs the body maps), so the decode layer produces ONE DecodedImage per unique source (+
/// processing combination — e.g. a baked opacity mask) and every mesh using it points at the same
/// object; the GPU layer keys its texture uploads on the object's identity, so a shared image is
/// also uploaded exactly once per model. Treat as immutable once assigned to a mesh.
struct DecodedImage {
    std::vector<uint8_t> pixels; ///< Tightly-packed RGBA8 (width × height × 4 bytes).
    uint32_t             width  = 0;
    uint32_t             height = 0;
};
using DecodedImagePtr = std::shared_ptr<const DecodedImage>;

/// One material group of an imported model: a self-contained triangle mesh, its diffuse base color,
/// and (optionally) its diffuse texture.
struct MeshData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    /// The material zone's name as the file has it ("Torso", a glTF material's name, an MTL
    /// material's): what the Materials tab lists and a project's material overrides are keyed
    /// by. Empty = unnamed (the tab shows "Zone N").
    std::string           name;
    // Material diffuse color when present, else this default for geometry with no material/texture:
    // a light neutral grey, hex #D1D1D1. Stored LINEAR (sRGB->linear of 209/209/209) because the lit
    // fragment output is hardware-encoded to the sRGB swapchain — so a fully-lit surface reads as
    // exactly #D1D1D1 on screen. Same convention as the renderer's clear-color note.
    glm::vec3             baseColor{0.6376f, 0.6376f, 0.6376f};

    // Diffuse texture source (an external path or embedded bytes; see TextureSource). The Qt layer
    // (ModelImportService::decodeModelTextures) decodes each unique source ONCE into a shared
    // DecodedImage so the Qt-free renderer never needs an image codec. Null image => untextured.
    // When hasOpacityMask is set, the image's alpha channel carries the baked opacity mask.
    TextureSource         diffuse;
    DecodedImagePtr       diffuseImage;
    /// THE TEXTURE SLOTS' plain images (2026-11-09; set by the decode layer, kept by the Mesh for
    /// the Materials tab's replacements): the base-color map's plain decode (no cutout baked),
    /// the cutout mask (its gray in every channel), the roughness and metalness maps' plain
    /// decodes (their channels are roughnessChannel / metalnessChannel). Null = none.
    DecodedImagePtr       diffusePlainImage;
    DecodedImagePtr       opacityMaskImage;
    DecodedImagePtr       roughnessPlainImage;
    DecodedImagePtr       metalnessPlainImage;
    /// A bump map CARRIED BESIDE a normal map (`normal` is then the normal map, normalMode 1), and
    /// an sRGB emission map — two more of the canonical material's slots (set 1 bindings 6 / 7).
    TextureSource         bumpMap;
    DecodedImagePtr       bumpImage;
    float                 bumpStrength = 1.0f;
    TextureSource         emissionMap;
    DecodedImagePtr       emissionImage;
    /// The STORED-ONLY maps (no binding; the Materials tab shows them, a renderer will read them):
    /// a grayscale map multiplying Diffuse Roughness.
    TextureSource         diffuseRoughnessMap;
    DecodedImagePtr       diffuseRoughnessImage;
    /// The top coat's maps (stored-only): weight, color (sRGB), roughness, reflectivity, and the
    /// coat's own bump / normal map with its strength.
    TextureSource         coatWeightMap;
    DecodedImagePtr       coatWeightImage;
    TextureSource         coatColorMap;
    DecodedImagePtr       coatColorImage;
    TextureSource         coatRoughnessMap;
    DecodedImagePtr       coatRoughnessImage;
    TextureSource         coatReflectivityMap;
    DecodedImagePtr       coatReflectivityImage;
    TextureSource         coatBumpMap;
    DecodedImagePtr       coatBumpImage;
    float                 coatBumpStrength = 1.0f;
    /// The translucency WEIGHT map (stored-only; the Translucency slot is the color map, whose
    /// luminance the viewport uses as the weight — a weight map alone is also read into it).
    TextureSource         translucencyWeightMap;
    DecodedImagePtr       translucencyWeightImage;
    /// The Specular section's maps (stored-only): the reflection's tint, the anisotropy and its
    /// rotation, the broad lobe's roughness and the lobe ratio.
    TextureSource         specularColorSlotMap;
    DecodedImagePtr       specularColorSlotImage;
    TextureSource         anisotropyMap;
    DecodedImagePtr       anisotropyImage;
    TextureSource         anisotropyRotationMap;
    DecodedImagePtr       anisotropyRotationImage;
    TextureSource         lobe2RoughnessMap;
    DecodedImagePtr       lobe2RoughnessImage;
    TextureSource         lobeRatioMap;
    DecodedImagePtr       lobeRatioImage;

    // Detail (surface) map: a tangent-space normal map or a grayscale bump/height map, selected by
    // normalMode. Decoded like the diffuse map, but uploaded as a LINEAR texture (it's data, not
    // color). Null => flat shading. normalMode: 0 none, 1 tangent-space normal, 2 grayscale bump.
    TextureSource         normal;
    DecodedImagePtr       normalImage;
    int                   normalMode   = 0;
    float                 normalStrength = 1.0f; ///< Authored detail-map strength (scales the perturbation).

    // Opacity (cutout) mask: a grayscale image whose texels modulate this mesh's opacity (lash and
    // brow cards, older generations' eye shells). Not uploaded as its own texture: the decode layer
    // bakes it into the diffuse map's ALPHA channel (the shader already samples the diffuse, so the
    // mask rides along for free) and sets hasOpacityMask, which routes the mesh to the alpha-blended
    // transparent pass even when the scalar opacity is 1 (a lash card's scalar is 1 — its shape
    // exists only in the mask).
    TextureSource         opacityMask;
    bool                  hasOpacityMask = false; // set by the decode layer once the bake succeeds
    /// Blend the skin LINEARLY (the rig was authored for linear blend skinning: every FBX export
    /// seen) instead of by dual quaternions (the native figures' authoring). A per-model choice,
    /// set on every mesh of the model by its importer; false for the native figures and OBJs.
    bool                  linearSkinning = false;

    // Specular parameter maps (both LINEAR, sampled .r): the roughness map multiplies the scalar
    // roughness per texel; the spec-mask map multiplies specularWeight per texel (the decode layer
    // switches specularWeight to the material's undiscounted fold when the mask decodes — see
    // MaterialRefs::specularWeightWithMap). Null => 1x1 white fallback (scalar behaviour).
    TextureSource         roughnessMap;
    DecodedImagePtr       roughnessImage;
    /// Which channel of the roughness map carries the roughness: 0 red (the native figures' and
    /// FBX's grayscale maps), 1 green (glTF's metallic-roughness map: roughness in G, metalness
    /// in B), 2 blue. The decode layer moves it into red, which is what the shader samples.
    int                   roughnessChannel = 0;
    /// The diffuse map's own ALPHA channel is the opacity (cutout) mask — a glTF material in
    /// MASK or BLEND alpha mode, a USD material whose opacity reads the base texture's alpha.
    /// The decode layer keeps the decoded alpha and sets hasOpacityMask without a bake.
    bool                  opacityFromDiffuseAlpha = false;
    TextureSource         specMask;
    DecodedImagePtr       specMaskImage;
    /// A METALNESS map (2026-11-09): glTF's metallic-roughness texture's blue channel, an FBX or
    /// USD metalness texture, an MTL's map_Pm. It rides the roughness texture's GREEN channel —
    /// the decode layer builds one parameter image per (roughness map, metalness map) pair with
    /// the roughness in red and the metalness in green (255 where a map is absent, so the
    /// shader's per-texel multiplies are no-ops) — and sets hasMetalnessMap. `metalness` is the
    /// factor the texel multiplies (1 with a map and no factor).
    TextureSource         metalnessMap;
    int                   metalnessChannel = 0;
    bool                  hasMetalnessMap = false; // set by the decode layer: the parameter map carries a metalness
    bool                  hasRoughnessMap = false; // set by the decode layer: the parameter map carries a roughness (a map, or a baked glossiness)
    /// A SPECULAR / GLOSSINESS source (import/specgloss.h), converted at import: the reader sets
    /// convertSpecGloss with the maps and factors of the older convention — the diffuse map is
    /// `diffuse` above with `diffuseFactor` on it, the specular color map (sRGB) and its factor,
    /// the glossiness map (a channel; glTF packs it in the specular map's alpha) and its factor —
    /// having already put the SCALAR solve into baseColor / metalness / roughness. With any of the
    /// maps present the decode layer bakes the solve per texel into a base-color map and a
    /// metalness map (and a roughness map from a glossiness map) in their place.
    bool                  convertSpecGloss = false;
    glm::vec4             diffuseFactor{1.0f};        // linear RGB × the diffuse map, alpha × its alpha
    TextureSource         specularColorMap;
    glm::vec3             specularColorFactor{1.0f};  // linear, × the specular map (the F0 itself untextured)
    TextureSource         glossinessMap;
    int                   glossinessChannel = 0;      // 0 red … 2 blue, 3 alpha
    float                 glossinessFactor = 1.0f;

    // Translucency map (sRGB color: the flesh-red transmitted tint, or a grayscale weight map —
    // luminance = per-region strength). White fallback = uniform.
    TextureSource         translucencyMap;
    DecodedImagePtr       translucencyImage;

    // Micro-detail (pore) normal map: LINEAR tangent-space normals tiled detailTiles× across the
    // UVs at detailWeight strength. No gutter fill (it wraps, and tiled UVs leave no gutters).
    TextureSource         detailNormalMap;
    DecodedImagePtr       detailNormalImage;
    float                 detailWeight = 0.0f;
    float                 detailTiles  = 0.0f;
    /// The detail layer's further values and maps (stored-only): the vertical tiling and the
    /// offsets, a roughness multiplier tiled with it, the weight's mask map and the multiplier's map.
    float                 detailTilesV = 0.0f;
    glm::vec2             detailOffset{0.0f};
    float                 detailRoughness = 1.0f;
    TextureSource         detailWeightMap;
    DecodedImagePtr       detailWeightImage;
    TextureSource         detailRoughnessMap;
    DecodedImagePtr       detailRoughnessImage;
    /// Displacement (stored-only; a renderer's): the height map, centimetres black-to-white, the
    /// mid level, the subdivision before displacing.
    TextureSource         displacementMap;
    DecodedImagePtr       displacementImage;
    float                 displacementStrength = 0.2f;
    float                 displacementMidLevel = 0.5f;
    float                 displacementSubdiv = 0.0f;
    /// A baked ambient-occlusion map and its weight (rendered since 2026-11-10 through the parameter
    /// map's blue, over the viewport's own per-vertex `ao`).
    TextureSource         aoMap;
    DecodedImagePtr       aoImage;
    float                 aoWeight = 1.0f;
    /// The transmission tint's map (stored-only).
    TextureSource         transmissionColorMap;
    DecodedImagePtr       transmissionColorImage;
    /// The completion batch's stored-only maps and values (2026-11-10): the transmission weight's map,
    /// the coat's anisotropy (+ rotation) with maps, the sheen's three maps, the thin film's weight
    /// and thickness maps, the UV rotation and the double-sided flag.
    TextureSource         transmissionWeightMap;
    DecodedImagePtr       transmissionWeightImage; ///< (rendered: the parameter map's alpha)
    /// THE LAYER MAP (the decode layer's buildPackedMap, 2026-11-10): the coat weight (R), the coat
    /// roughness (G), the sheen weight (B) and the thin-film thickness (A) maps packed into the one
    /// linear image the shader samples (set 1 binding 8); null without any of the four.
    DecodedImagePtr       layerMapImage;
    float                 coatAnisotropy = 0.0f;
    float                 coatAnisotropyRotation = 0.0f;
    TextureSource         coatAnisotropyMap;
    DecodedImagePtr       coatAnisotropyImage;
    TextureSource         coatAnisotropyRotationMap;
    DecodedImagePtr       coatAnisotropyRotationImage;
    TextureSource         sheenWeightMap;
    DecodedImagePtr       sheenWeightImage;
    TextureSource         sheenColorMap;
    DecodedImagePtr       sheenColorImage;
    TextureSource         sheenRoughnessMap;
    DecodedImagePtr       sheenRoughnessImage;
    TextureSource         thinFilmWeightMap;
    DecodedImagePtr       thinFilmWeightImage;
    TextureSource         thinFilmThicknessMap;
    DecodedImagePtr       thinFilmThicknessImage;
    float                 uvRotation = 0.0f;
    bool                  doubleSided = false;
    bool                  topCoatEnabled = true; ///< the native Top Coat Enable switch (the weight kept either way)

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
    // THE CANONICAL MATERIAL's further channels (materialparams.h, 2026-11-09): a reader fills what
    // its format carries; the rest stay at these neutral defaults.
    float                 diffuseRoughness = 0.0f;
    glm::vec3             specularColor{1.0f};
    float                 ior = 1.5f;
    float                 anisotropy = 0.0f;
    float                 anisotropyRotation = 0.0f;
    float                 transmission = 0.0f;
    glm::vec3             transmissionColor{1.0f};
    float                 transmissionDepth = 0.0f;
    float                 transmissionRoughness = -1.0f; ///< < 0: the surface's own roughness
    float                 dispersion = 0.0f;
    bool                  thinWalled = false;
    glm::vec3             subsurfaceColor{1.0f};
    float                 subsurfaceRadius = 1.0f; ///< the scattering distance, cm
    glm::vec3             subsurfaceRadiusColor{1.0f}; ///< the scattering color (SSS Color)
    float                 subsurfaceAnisotropy = 0.0f;   ///< the scattering direction, -1..1
    float                 sheen = 0.0f;
    glm::vec3             sheenColor{1.0f};
    float                 sheenRoughness = 0.5f;
    glm::vec3             coatColor{1.0f};
    float                 coatIor = 1.5f;
    float                 thinFilm = 0.0f;
    float                 thinFilmThickness = 500.0f; // nm
    float                 thinFilmIor = 1.4f;
    glm::vec3             emissionColor{1.0f};
    float                 emission = 0.0f;           // luminance factor (0 = none)
    glm::vec2             uvTiling{1.0f};
    glm::vec2             uvOffset{0.0f};

    // For pose correctives: the source *base* (pre-de-index) vertex index each render vertex
    // came from — the key that maps a corrective's base-vertex deltas onto these render vertices.
    // Parallel to `vertices`; empty for static/OBJ meshes (they have no correctives).
    std::vector<uint32_t> baseVertex;
};

// --- Pose correctives (joint-driven corrective morphs) ----------------------------------------
// A rigged figure ships corrective shapes that fire as a function of joint rotation to fix the mesh
// crumpling at bent elbows/shoulders/hips/knees ("candy-wrapper"). Each is a sparse morph whose blend
// weight is driven by one or more joint-rotation angles through a tiny driver formula. The engine
// blends them LIVE on the GPU: at upload the Model packs every corrective's deltas into one static
// delta table (each vertex's run referenced through Vertex::correctiveRange), re-evaluates the
// weights whenever the pose moves, and the vertex shaders sum weight · delta before skinning — so a
// pose change costs a few hundred bytes of weights per frame, never a geometry re-upload.

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
    // The morph's own value-channel range (`channel.min/max` with `clamped: true`): the driven
    // weight clamps to it. Load-bearing for LINEAR ramp drivers — a knee-EXTENSION flexion is
    // authored as `rotation/x × -1/11` and relies on the clamp to stay 0 through the 155° of
    // FLEXION (unclamped it reached -14 and cratered the thigh).
    bool                                        clamped = false;
    float                                       clampMin = 0.0f;
    float                                       clampMax = 1.0f;
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
    /// Where the bone's TIP lies (bind world space) when the file said so — an exporter's end
    /// marker, a skinless leaf the importer left out of the skeleton: the direction of a bone with
    /// no joint below it (a single-bone toe, a fingertip). Read by the rig standard's conformer;
    /// the native figure importer leaves it unset.
    glm::vec3   tip{0.0f};
    bool        hasTip = false;
    /// A DRIVEN bone: its pose rotation is driveFactor * the driver's, channel by channel (the
    /// two share a frame and a rotation order), re-derived after every pose change — a rig's
    /// helper bone whose driver expression the file could not carry (a knee's "share" bone that
    /// takes half the bend, a twist helper that takes its share of the segment's twist). Never
    /// posed by hand or by a solve. -1: not driven (every native figure bone).
    int         drivenBy = -1;
    glm::vec3   driveFactor{0.0f};
    /// What the FILE says this bone is, as the rig standard's name for the role (`upper_arm_l`,
    /// `spine2`, `thumb1_r`) — a VRM avatar's humanoid map. The rig's role inference takes it as
    /// GIVEN over its own structural reading (RigRoles::Input::givenNames), at conform time and
    /// at run time alike; empty for a bone the file says nothing about (every bone of every
    /// other format).
    std::string roleHint;
};

/// One imported model: its material groups, plus an optional skeleton. An empty `bones` list means a
/// static model — the renderer binds a single identity joint, which its vertices default to.
struct ModelData {
    std::vector<MeshData>       meshes;
    std::vector<ModelBone>      bones;
    std::vector<PoseCorrective> correctives; ///< Pose-driven corrective morphs; empty for static models.
    /// Anything the reader wants in the import log (a conversion it made, a channel it could not
    /// read) — the Qt layer prints them under the import's line, as the rigged readers' notes are.
    std::vector<std::string>    notes;
};

} // namespace pose

#endif // MODELDATA_H
