// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file materialparams.h
 * @brief THE CANONICAL MATERIAL (2026-11-09): one zone's surface as a complete, renderer-neutral
 *        parameter set — what the Materials tab edits by slider, what every importer fills, what
 *        a project file carries, and what a rendering engine will consume — plus which maps the
 *        zone carries and what the tab shows for the selected object.
 *
 * The model is OpenPBR Surface's: base (color, metalness, diffuse roughness), specular (weight,
 * color, roughness, index of refraction, anisotropy), transmission (weight, color, depth,
 * dispersion, thin-walled), subsurface (weight, color, radius), sheen (fuzz), coat (weight,
 * color, roughness, ior), thin film, emission, and geometry (opacity, detail-map strengths, UV
 * tiling) — with PoseStudio's skin extension, the dual specular lobe and the tiled pore-detail
 * normal. The VIEWPORT renders an approximation of it (mesh.frag reads what it can from the
 * packed MaterialGpu — Mesh::packGpu); a production renderer gets the whole set. Nothing here
 * is a node: a surface is a point in this parameter space, and the tab is sliders over it.
 *
 * A plain-data contract between the scene and the properties panel, like JointTransform: the
 * scene fills a MaterialView from the selected model (Scene::materialView), the viewport hands
 * it out, the panel mirrors it into its rows and sends MaterialParams back. Qt-free.
 *
 * This header is the DATA and what is trivial about it; the name tables, the fields' display
 * values and setters and the comparisons are defined in materialparams.cpp (most of the viewport
 * includes this header, and a table edited here rebuilt all of it).
 */

#ifndef MATERIALPARAMS_H
#define MATERIALPARAMS_H

#include "materialoverlay.h" // MaterialOverlay (the images layered over a zone's maps)

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace pose {

/// The texture slots a zone has — one per channel the shaders sample or a renderer would. Each
/// is a file (or an embedded image) the material was imported with, a file the user chose on the
/// tab, or none.
enum class MaterialMap {
    BaseColor,    ///< sRGB color (binding 0; the cutout mask baked into its alpha)
    Normal,       ///< tangent-space normal map (linear)
    Bump,         ///< grayscale height map (linear)
    Roughness,    ///< the roughness map (its channel → the parameter map's red)
    Metalness,    ///< the metalness map (its channel → the parameter map's green)
    SpecularMask, ///< the specular weight's per-texel multiplier
    Translucency, ///< the Translucency COLOR map (sRGB): the tint light takes under the surface, its luminance the weight
    Emission,     ///< sRGB emission map
    PoreDetail,   ///< the DETAIL NORMAL MAP: the tiled micro-detail normal (linear; the script name stays `pore`)
    Opacity,      ///< the cutout mask (grayscale; baked into the base color's alpha)
    // --- The slots after this line are STORED-ONLY (2026-11-10) — maps a renderer reads and the
    // viewport does not sample: kept with the material, shown in the tab, saved with the project,
    // no set-1 binding — EXCEPT the seven materialMapIsRendered names (the coat weight and
    // roughness, the sheen weight and color, the thin-film thickness, the ambient occlusion and
    // the transmission weight), packed into two bindings the same day. Add a slot here (and its
    // name in materialMapName, its MeshData field, the importers that carry it, a tab row).
    DiffuseRoughness, ///< grayscale: multiplies Diffuse Roughness per texel
    CoatWeight,       ///< grayscale: multiplies the top coat's weight per texel
    CoatColor,        ///< sRGB: the top coat's tint map
    CoatRoughness,    ///< grayscale: multiplies the top coat's roughness per texel
    CoatReflectivity, ///< grayscale: scales the top coat's reflectance per texel (the IOR row's map)
    CoatBump,         ///< the top coat's own bump (height) or normal map
    TranslucencyWeight, ///< grayscale: the per-region translucency (subsurface) weight map
    SpecularColor,      ///< sRGB: the dielectric reflection's tint map
    Anisotropy,         ///< grayscale: scales Anisotropy per texel (glTF's direction map stored as it is)
    AnisotropyRotation, ///< grayscale: the grain's direction per texel
    Lobe2Roughness,     ///< grayscale: multiplies the broad lobe's roughness per texel (skin)
    LobeRatio,          ///< grayscale: the lobe ratio per texel (skin)
    DetailWeight,       ///< grayscale: where the detail layer applies (its weight per texel)
    DetailRoughness,    ///< grayscale: the detail layer's roughness multiplier per texel, tiled with it
    Displacement,       ///< grayscale height: the displacement map (mid level = no displacement)
    AmbientOcclusion,   ///< grayscale: a baked ambient-occlusion map (the viewport bakes its own per vertex)
    TransmissionColor,  ///< sRGB: the transmission tint map (stained glass)
    TransmissionWeight, ///< grayscale: the transmission weight per texel
    CoatAnisotropy,     ///< grayscale: the top coat's anisotropy per texel
    CoatAnisotropyRotation, ///< grayscale: the top coat's grain direction per texel
    SheenWeight,        ///< grayscale: the sheen's weight per texel
    SheenColor,         ///< sRGB: the sheen's color map
    SheenRoughness,     ///< grayscale: the sheen's roughness per texel
    ThinFilmWeight,     ///< grayscale: the thin film's weight per texel
    ThinFilmThickness,  ///< grayscale: the thin film's thickness per texel (black = 0, white = the Thickness row)
    Count
};
constexpr int kMaterialMapCount = static_cast<int>(MaterialMap::Count);

/// Where a slot's texture comes from: "" = none, kImportedTexture = the texture the file was
/// imported with (whatever and wherever it was — an embedded image has no path), else the path
/// of a file the user chose on the tab. The mesh keeps the imported texture, so "@imported" is
/// always restorable; a chosen file is decoded again when a project or an undo asks for it.
constexpr const char* kImportedTexture = "@imported";

/// A CHOSEN FILE READ FROM ONE CHANNEL. A slot's value may ride in the green, blue or alpha of
/// an image that packs several maps (a MaterialX or glTF occlusion / roughness / metalness
/// image, a cutout in a color image's alpha). The source then carries the channel after its
/// path — "D:/maps/orm.png?channel=g" — so that whatever reads the source again (an undo, a
/// project) reads the same channel. ('?' cannot be part of a Windows file name.) A source
/// without the mark is read from its red.
constexpr const char* kTextureChannelMark = "?channel=";

/// A CHOSEN FILE READ INVERTED. A map may hold the opposite of what its slot holds — a
/// GLOSSINESS map where the material has a roughness slot (a content library's shader preset:
/// shaderpresetfile.h). The source then ends in this mark — "D:/maps/gloss.jpg?inverted", after
/// a channel mark where it has both — and the image is decoded with its color channels turned
/// over (255 less each), so everything downstream holds a plain roughness map. Like a channel
/// mark it lives in a slot's source alone: saved into a material file, the map is written out
/// as a file of its own, already inverted.
constexpr const char* kTextureInvertMark = "?inverted";

/// @p path as a slot's source read from @p channel (0 red … 3 alpha) and, with @p inverted,
/// turned over: the path itself for channel 0 as it is.
std::string textureSourceWithChannel(const std::string& path, int channel, bool inverted = false);

/// The file a slot's source names, without its marks; @p channel (when given) is the channel
/// it names, 0 where it names none, and @p inverted whether it is to be read inverted.
std::string textureSourcePath(const std::string& source, int* channel = nullptr, bool* inverted = nullptr);

struct MaterialTextureRef {
    std::string source; ///< "", kImportedTexture, or a file path (which may end in a channel mark: textureSourcePath)
    bool operator==(const MaterialTextureRef& o) const { return source == o.source; }
    bool operator!=(const MaterialTextureRef& o) const { return source != o.source; }
};

/// The script name of a slot (`texture <slot> <path>`, `matmap.<slot>`): basecolor, normal, bump,
/// roughness, metalness, specmask, translucency, emission, pore, cutout, diffuseroughness, coatweight,
/// coatcolor, coatroughness, coatreflectivity, coatbump, translucencyweight, specularcolor, anisotropy,
/// anisotropyrotation, lobe2, loberatio, detailweight, detailroughness, displacement, ao, transmissioncolor,
/// transmissionweight, coatanisotropy, coatanisotropyrotation, sheen, sheencolor, sheenroughness, thinfilm,
/// thinfilmthickness.
const char* materialMapName(MaterialMap map);
/// Whether a slot's map is ONE VALUE a texel — a weight, a roughness, a height, a mask: read
/// through one channel of its image, which may be an image that packs several such maps — or a
/// color or a normal, read whole. (What the enum's comments call "grayscale".) The Materials tab
/// shows a scalar slot's picture as that channel in grey: the map the zone uses, not the packed
/// image it rides in.
inline bool materialMapIsScalar(MaterialMap map) {
    switch (map) {
    case MaterialMap::BaseColor:
    case MaterialMap::Normal:
    case MaterialMap::Translucency:
    case MaterialMap::Emission:
    case MaterialMap::PoreDetail:
    case MaterialMap::CoatColor:
    case MaterialMap::CoatBump:
    case MaterialMap::SpecularColor:
    case MaterialMap::TransmissionColor:
    case MaterialMap::SheenColor:
        return false;
    default:
        return true;
    }
}

/// Whether the viewport SAMPLES a slot (it has a set-1 binding and an uploaded texture) or only
/// stores it for a renderer. A stored-only slot is decoded for its thumbnail and nothing else.
inline bool materialMapIsRendered(MaterialMap map) {
    switch (map) {
    // The ten slots with a binding of their own (or a share of one), and since 2026-11-10 the
    // six PACKED into two more: the ambient occlusion and transmission weight maps ride the
    // parameter map's blue and alpha, the coat weight, coat roughness, sheen weight and
    // thin-film thickness maps the LAYER map's four channels, and the sheen color map has
    // its own binding (mesh.h's MaterialBinding).
    case MaterialMap::AmbientOcclusion:
    case MaterialMap::TransmissionWeight:
    case MaterialMap::CoatWeight:
    case MaterialMap::CoatRoughness:
    case MaterialMap::SheenWeight:
    case MaterialMap::SheenColor:
    case MaterialMap::ThinFilmThickness:
        return true;
    default:
        return static_cast<int>(map) < static_cast<int>(MaterialMap::DiffuseRoughness); // the stored-only slots follow it
    }
}
/// The inverse of materialMapName; false for an unknown name.
bool materialMapByName(const std::string& name, MaterialMap& out);

/// The editable material values of one zone. Colors are LINEAR; weights are 0..1 unless said.
struct MaterialParams {
    // --- Base ---
    glm::vec3 baseColor{1.0f};      ///< The color itself untextured, a tint on the map.
    float     metalness = 0.0f;     ///< 0 dielectric … 1 metal (F0 = the base color).
    float     diffuseRoughness = 0.0f; ///< Oren-Nayar roughness of the diffuse lobe (rendered on the key light).
    // --- Specular ---
    float     specular = 1.0f;      ///< The F0 weight: 1 = the dielectric's own reflectance at `ior` (4 % at 1.5), 0 = none.
    glm::vec3 specularColor{1.0f};  ///< The dielectric reflection's tint.
    float     roughness = 0.7f;     ///< 0 mirror-smooth … 1 matte (× the roughness map).
    float     ior = 1.5f;           ///< Index of refraction (1.5 = glass / the standard dielectric).
    float     anisotropy = 0.0f;    ///< 0 isotropic … 1 fully stretched highlights (rendered: the stretched GGX lobe).
    float     anisotropyRotation = 0.0f; ///< Degrees about the normal (rendered).
    // --- Transmission ---
    float     transmission = 0.0f;  ///< 0 opaque … 1 fully transmissive (glass, water). Rendered: the frame shows through, tinted, under the reflection.
    glm::vec3 transmissionColor{1.0f}; ///< The tint light takes on passing through (rendered; deeper toward a solid's edges).
    float     transmissionDepth = 0.0f; ///< Metres over which the tint is reached (0 = at the surface).
    /// How rough the surface is to what passes THROUGH it, where that is not its Roughness (a
    /// negative value = the Roughness): an eye's clear shells reflect softly and transmit sharply.
    /// Stored for the rendering engines (the viewport's transmission is unblurred either way).
    float     transmissionRoughness = -1.0f;
    float     dispersion = 0.0f;    ///< 0 none … 1 strong chromatic dispersion (stored; not rendered).
    bool      thinWalled = false;   ///< A single sheet (a window pane, a leaf) rather than a solid.
    // --- Subsurface (the weight is `translucency`, the native skin shaders' name) ---
    float     translucency = 0.5f;  ///< The subsurface / transmission weight (× the translucency map).
    glm::vec3 subsurfaceColor{1.0f}; ///< The Translucency Color: the tint light takes under the surface (stored; the viewport's profile is fixed, its map's tint rendered).
    glm::vec3 subsurfaceRadiusColor{1.0f}; ///< The SCATTERING COLOR: the per-channel scattering distance tint (the source app's SSS Color; stored).
    float     subsurfaceRadius = 1.0f; ///< The SCATTERING DISTANCE in centimetres (the source app's Scattering Measurement Distance; stored).
    float     subsurfaceAnisotropy = 0.0f; ///< The scattering direction: -1 back, 0 even, +1 forward (the source app's SSS Direction; stored).
    // --- Sheen (fuzz: cloth, velvet; rendered) ---
    float     sheen = 0.0f;
    glm::vec3 sheenColor{1.0f};
    float     sheenRoughness = 0.5f;
    // --- Coat (the native shaders' Top Coat) ---
    float     topCoat = 0.0f;       ///< The thin clear layer's weight (0 = none).
    float     topCoatRoughness = 0.65f;
    glm::vec3 coatColor{1.0f};      ///< The coat's tint: what passes through it takes this color (rendered).
    float     coatIor = 1.5f;       ///< The coat's index: 1.5 = the standard 4% coat; a lacquer 1.5 – 1.6 (rendered).
    float     coatBumpStrength = 1.0f; ///< The coat's own bump map's strength (stored; nothing without a map).
    float     coatAnisotropy = 0.0f;   ///< The coat's highlight stretch (rendered).
    float     coatAnisotropyRotation = 0.0f; ///< … and its grain's direction, degrees (rendered).
    // --- Thin film (iridescence; rendered: the film's interference colors in the reflection) ---
    float     thinFilm = 0.0f;
    float     thinFilmThickness = 500.0f; ///< Nanometres.
    float     thinFilmIor = 1.4f;
    // --- Emission ---
    glm::vec3 emissionColor{1.0f};
    float     emission = 0.0f;      ///< Luminance factor: 0 = none, 1 = the color at full radiance.
    // --- Geometry ---
    float     opacity = 1.0f;       ///< < 1 draws the zone in the alpha-blended pass.
    float     normalStrength = 1.0f; ///< The normal map's strength; nothing without a map.
    /// FLIP NORMAL — the normal map's CONVENTION. A tangent-space normal map's green channel says
    /// how the surface leans up or down the image, and there are two ways round: OpenGL's (green
    /// = up the image: what glTF, USD and MaterialX specify, what the figure format's own content
    /// is made in, and what a material library's "GL" download is) and DirectX's (green = down).
    /// false: the map is OpenGL's, the default for every map. true: it is DirectX's.
    /// In the VIEWPORT the mesh's bitangent runs DOWN the image (tangentgen: dP/dv with v down),
    /// so mesh.frag negates an OpenGL map's green (Mesh::packGpu's material3.z = -1) and reads a
    /// DirectX one as it is; the render engine is told a DirectX map by a flag
    /// (renderapi::kMaterialFlipNormalY) and reads an unflagged one as OpenGL's. (Until
    /// 2026-11-14 the viewport read every map the DirectX way — an OpenGL map's relief lit from
    /// the wrong side top to bottom, and unlike the render of the same scene.) Nothing without a map.
    bool      flipNormal = false;
    float     bumpStrength = 1.0f;  ///< The bump map's strength; nothing without a map.
    // --- The DETAIL layer: a tiled micro-detail normal (pores, weave, grain) over the surface ---
    float     detailWeight = 0.0f;  ///< Its strength (0 = none; < 1). Rendered.
    float     detailTiles = 0.0f;   ///< Its horizontal UV tiling (at least 1 with the layer on). Rendered.
    float     detailTilesV = 0.0f;  ///< Its vertical tiling (0 = the horizontal count). Rendered.
    glm::vec2 detailOffset{0.0f};   ///< Its UV offset in repeats. Rendered.
    float     detailRoughness = 1.0f; ///< A roughness multiplier by the layer's weight (rendered; nothing without the layer).
    // --- Displacement (stored; a renderer's tessellation): the map's white less its black in cm,
    // the map value that is no displacement, and how finely to subdivide before displacing ---
    float     displacementStrength = 0.2f; ///< Centimetres from the map's black to its white (the source app's 0.2 cm default range).
    float     displacementMidLevel = 0.5f; ///< The map value at zero displacement (0.5: black sinks, white rises).
    float     displacementSubdiv = 0.0f;   ///< Subdivision levels applied before displacing (0..5).
    float     aoWeight = 1.0f;             ///< How much a baked ambient-occlusion map darkens the environment's light (rendered; nothing without the map).
    bool      doubleSided = false;         ///< Render the back faces too (a leaf, a cloth card); the viewport always does (stored).
    // --- THE SMOOTHING (import/smoothnormals.h): the zone's shading normals computed here, by
    // angle, instead of taken from its file. Off = as imported (which may already be smooth). An
    // OBJECT's state shown with its material: a value for undo, linked zones, Reset and a
    // project — sameValues counts it — but not part of a surface: nearlyEqual (the linked zones'
    // "same material") does not, and a saved material file neither carries nor changes it.
    bool      smoothByAngle = false;
    float     smoothAngle = 60.0f;         ///< Degrees: edges sharper than this stay crisp (0 faceted, 180 smooth everywhere).
    glm::vec2 uvTiling{1.0f};       ///< The material's UV repeat (a tiled floor).
    glm::vec2 uvOffset{0.0f};
    float     uvRotation = 0.0f;    ///< The material's UV rotation, degrees, about the UV origin after the tiling (rendered).
    // --- The skin extension: the dual specular lobe (lobeRatio 1 = a single lobe, where the
    // shader uses `roughness` alone). Kept in proportion to `roughness` by setRoughness.
    float lobe1Roughness = 0.7f;
    float lobe2Roughness = 0.7f;
    float lobeRatio = 1.0f;
    // --- THE LAYER SWITCHES (2026-11-10): a layered section on or off with its values kept — the
    // A/B a weight of 0 cannot give back. Off = the layer contributes nothing (the viewport zeroes
    // the coat, the translucency, the emission and the detail; a renderer export writes weight 0).
    bool      topCoatEnabled = true;
    bool      sheenEnabled = true;
    bool      thinFilmEnabled = true;
    bool      transmissionEnabled = true;
    bool      subsurfaceEnabled = true;
    bool      emissionEnabled = true;
    bool      displacementEnabled = true;
    bool      detailEnabled = true;
    // … and the surface's own (2026-11-10, the user: "Shouldn't we have one there too?"): Base off = no
    // diffuse and no metal (a black dielectric: glass, a coat alone), Specular off = no dielectric
    // reflection (matte; the coat still reflects), Geometry off = the surface maps at rest (normal,
    // bump, occlusion; opacity and double-sidedness are the shape, not an effect), Tiling off =
    // the maps as painted.
    bool      baseEnabled = true;
    bool      specularEnabled = true;
    bool      geometryEnabled = true;
    bool      tilingEnabled = true;
    // --- Textures ---
    std::array<MaterialTextureRef, kMaterialMapCount> maps{};
    // --- Overlays (materialoverlay.h): images layered over the zone's own maps, lowest first.
    // Like the maps they are the MESH's own — a value edit (a linked zone's, an undo snapshot's,
    // Reset Zone) never moves them; they change through the overlay calls alone.
    std::vector<MaterialOverlay> overlays;

    /// Sets the roughness the way the tab's slider does: the two lobes of a dual-lobe material
    /// scale with it, so a skin material stays a skin material (its broad and its tight lobe keep
    /// their ratio to the blended value) rather than collapsing to one lobe.
    void setRoughness(float r);

    /// The single-lobe roughness as the blend of the two lobes by their ratio (the import's own
    /// rule), after a lobe or the ratio is edited directly. A dual-lobe material only.
    void blendLobes();

    /// Whether the material has two specular lobes (the skin extension) — the ONE test of it:
    /// mesh.frag's own threshold, the tab's lobe rows, and what setRoughness and blendLobes go by.
    bool dualLobe() const { return lobeRatio < 0.999f; }

    /// Exact equality of every VALUE (the maps excluded): "has the material been edited".
    bool sameValues(const MaterialParams& o) const;
    bool operator==(const MaterialParams& o) const { return sameValues(o) && maps == o.maps && overlays == o.overlays; }
    bool operator!=(const MaterialParams& o) const { return !(*this == o); }

    /// Equal to within @p eps on every VALUE — "the same material" for the tab's linked zones (a
    /// figure's skin zones carry one preset's values with different maps per zone, its eyes and
    /// mouth other values). The maps are not compared.
    bool nearlyEqual(const MaterialParams& o, float eps = 1.0e-4f) const;
};

/// Holds every value of @p p to the range it can take. For a material that comes from a FILE,
/// whose numbers no row of the tab has clamped: each value is held to what its row allows
/// (setMaterialField), widened to what an importer or the struct's default legitimately holds
/// beyond that — a detail layer's tile count of 0 (none), a normal strength of 8, a mirrored
/// tiling, a transmission roughness of -1 (the surface's own). A value inside its range is left
/// bit for bit; one that is not a number becomes its range's low end. The colors are held to
/// 0 and up; the switches, the maps and the overlays are not touched (clampMaterialOverlay).
void clampMaterialParams(MaterialParams& p);

/// A FILE'S MATERIAL OVER A ZONE'S OWN. A material read from a file is the values the file
/// NAMES; a value it does not name — the file was written by a build that did not know the value
/// yet — is not the struct's default but whatever stood before: for a project's zone, the
/// re-imported zone's own. This is @p base with the values of @p values put over it for the
/// keys in @p named (the material's JSON keys: "roughness", "ior", "uvTiling", …; a key that
/// is no value of a material is passed over), the maps and the overlays @p base's.
/// (Defined in materialjson.cpp: it goes through the material's one codec, so which values a
/// material HAS is listed nowhere else. A caller links that file.)
MaterialParams materialWithNamedValues(const MaterialParams& base, const MaterialParams& values, const std::vector<std::string>& named);

/// Whether overlay @p o changes the image the viewport binds for slot @p map — its color the
/// base color map, its gloss the roughness map, its bump and normal images theirs. (The
/// displacement image is a renderer's: Displacement answers for an export, nothing is bound.)
bool overlayAffects(const MaterialOverlay& o, MaterialMap map);

/// The slots an overlay stack is composited into FOR THE VIEWPORT, in the order they are rebound:
/// the color map, the roughness map (the gloss), the metalness map (the metal — it shares the
/// roughness map's BINDING, the parameter map) and the normal map — which carries the overlays'
/// BUMP images too, as slopes (the viewport's own bump path keeps pore-scale grain alone, by
/// design: a scar's ridge would cancel out of it). The bump map itself is composited for an
/// export, not bound.
constexpr std::array<MaterialMap, 4> kOverlaySlots = {MaterialMap::BaseColor, MaterialMap::Roughness, MaterialMap::Metalness, MaterialMap::Normal};

/// Everything about @p overlays that decides slot @p map's composite, as one string — "" when no
/// overlay touches the slot. Two stacks with the same signature paint the same image over the
/// same base, so a scrub of one control re-composites only the slot it changes (a hue scrub the
/// color map, never the bump), and the zones of one map share one composite. @p bumpAsNormal
/// (the viewport's Normal slot): the overlays' bump images count toward the normal map's.
std::string overlaySignature(const std::vector<MaterialOverlay>& overlays, MaterialMap map, bool bumpAsNormal = false);

/// The layered sections a header switch turns on or off (the switch is a material value: saved,
/// undoable, linked across matching zones, restorable to the imported state).
enum class MaterialLayer { TopCoat, Sheen, ThinFilm, Transmission, Subsurface, Emission, Displacement, Detail, Base, Specular, Geometry, Tiling, Count };

inline bool& materialLayerEnabled(MaterialParams& p, MaterialLayer layer) {
    switch (layer) {
    case MaterialLayer::Sheen: return p.sheenEnabled;
    case MaterialLayer::ThinFilm: return p.thinFilmEnabled;
    case MaterialLayer::Transmission: return p.transmissionEnabled;
    case MaterialLayer::Subsurface: return p.subsurfaceEnabled;
    case MaterialLayer::Emission: return p.emissionEnabled;
    case MaterialLayer::Displacement: return p.displacementEnabled;
    case MaterialLayer::Detail: return p.detailEnabled;
    case MaterialLayer::Base: return p.baseEnabled;
    case MaterialLayer::Specular: return p.specularEnabled;
    case MaterialLayer::Geometry: return p.geometryEnabled;
    case MaterialLayer::Tiling: return p.tilingEnabled;
    default: return p.topCoatEnabled;
    }
}
inline bool materialLayerIsEnabled(const MaterialParams& p, MaterialLayer layer) {
    return materialLayerEnabled(const_cast<MaterialParams&>(p), layer);
}

/// The tab's SCALAR rows as fields, with their DISPLAY units — percent for a weight, degrees,
/// nanometres, centimetres, a plain value — so the panel and the scripted test read and set the
/// same numbers through one definition. Roughness goes through MaterialParams::setRoughness
/// (the dual-lobe split follows it).
enum class MaterialField {
    Roughness,
    Metallic,
    Specular,
    NormalStrength,
    PoreDetail,
    PoreTiles,
    Opacity,
    Subsurface,
    TopCoat,
    TopCoatRoughness,
    // (the canonical model's further rows, 2026-11-09)
    Ior,
    Anisotropy,
    AnisotropyRotation,
    DiffuseRoughness,
    Transmission,
    TransmissionDepth,
    Dispersion,
    SubsurfaceRadius,
    Sheen,
    SheenRoughness,
    CoatIor,
    ThinFilm,
    ThinFilmThickness,
    ThinFilmIor,
    Emission,
    BumpStrength,
    TilingX,
    TilingY,
    OffsetX,
    OffsetY,
    CoatBumpStrength,
    // The skin extension's dual lobe (2026-11-10): shown for a dual-lobe material only.
    Lobe1Roughness,
    Lobe2Roughness,
    LobeRatio,
    // The detail layer's further rows (2026-11-10; PoreDetail / PoreTiles are its weight and horizontal tiles).
    DetailRoughness,
    DetailTilesV,
    DetailOffsetU,
    DetailOffsetV,
    // Displacement (2026-11-10; stored for renderers).
    DisplacementStrength,
    DisplacementMidLevel,
    DisplacementSubdiv,
    AmbientOcclusion, ///< the baked AO map's weight
    SubsurfaceAnisotropy, ///< the scattering direction
    CoatAnisotropy,
    CoatAnisotropyRotation,
    TilingRotation,
    // The layer switches, as 0 / 1 fields for the scripts.
    TopCoatEnabled,
    SheenEnabled,
    ThinFilmEnabled,
    TransmissionEnabled,
    SubsurfaceEnabled,
    EmissionEnabled,
    DisplacementEnabled,
    DetailEnabled,
    BaseEnabled,
    SpecularEnabled,
    GeometryEnabled,
    TilingEnabled,
    ThinWalled, ///< 0 / 1: the Transmission section's Thin Walled box (a sheet, not a solid)
    FlipNormal, ///< 0 / 1: the Geometry section's Flip Normal box (a DirectX-style normal map)
    SmoothByAngle, ///< 0 / 1: the Geometry section's Smooth by Angle box (THE SMOOTHING)
    SmoothAngle,   ///< … and its angle, degrees
    Count
};

/// A field's display value off @p p.
float materialFieldValue(const MaterialParams& p, MaterialField field);

/// The change a display value makes to @p p (clamped to what makes sense).
void setMaterialField(MaterialParams& p, MaterialField field, float v);

/// The field's name in the scripted test (`material <name> <value>`, `mat.<name>`).
const char* materialFieldName(MaterialField field);

/// The inverse of materialFieldName; false for an unknown name.
bool materialFieldByName(const std::string& name, MaterialField& out);

/// The tab's COLOR rows (swatches), with the same one-definition treatment.
enum class MaterialColor { Base, Specular, Transmission, Subsurface, Sheen, Coat, Emission, ScatterColor, Count };

inline glm::vec3& materialColorRef(MaterialParams& p, MaterialColor which) {
    switch (which) {
    case MaterialColor::Specular: return p.specularColor;
    case MaterialColor::Transmission: return p.transmissionColor;
    case MaterialColor::Subsurface: return p.subsurfaceColor;
    case MaterialColor::Sheen: return p.sheenColor;
    case MaterialColor::Coat: return p.coatColor;
    case MaterialColor::Emission: return p.emissionColor;
    case MaterialColor::ScatterColor: return p.subsurfaceRadiusColor;
    default: return p.baseColor;
    }
}
inline glm::vec3 materialColorValue(const MaterialParams& p, MaterialColor which) {
    return materialColorRef(const_cast<MaterialParams&>(p), which);
}
/// The color's name in the scripted test (`material <name> r g b`, `mat.<name>.<r|g|b>`), and its inverse.
const char* materialColorName(MaterialColor which);
bool materialColorByName(const std::string& name, MaterialColor& out);

/// Which maps a zone carries RIGHT NOW (bound on the GPU) — facts the tab shows beside the
/// sliders. Indexed by MaterialMap.
struct MaterialMaps {
    std::array<bool, kMaterialMapCount> present{};
    bool has(MaterialMap m) const { return present[static_cast<std::size_t>(m)]; }
    bool operator==(const MaterialMaps& o) const { return present == o.present; }
};

/// What saving a material did with the zone's texture maps (VulkanWindow::saveMaterialFile; the
/// standard is assets/assetstore.h: an asset's textures lie in a `.textures` folder beside it).
struct MaterialSaveReport {
    int copied = 0;    ///< Files copied into the material's texture folder.
    int reused = 0;    ///< … not copied: a file with the same bytes was in the folder's store already.
    int written = 0;   ///< Images that had no file of their own (inside a model's file, or one channel of a packed map), written out.
    int linked = 0;    ///< Maps named where they are (beside the material already; or the save was told not to copy).
    int notCopied = 0; ///< … of those, files that SHOULD have been copied and could not be.
    int leftOut = 0;   ///< Maps the saved material does not include (an image without a file, when nothing may be written).
};

/// What the Materials tab shows for the SELECTED object: its zones, the selected zone's values —
/// as they stand and as imported — and its maps. Invalid with no object selected (a splat scene
/// has no materials).
struct MaterialView {
    bool                     valid = false;
    int                      model = -1;      ///< The selected model's index (Scene::selectedModelIndex).
    std::string              modelName;       ///< The source file's name (for the heading).
    std::vector<std::string> zones;           ///< Every zone's name, in mesh order ("Zone N" where unnamed).
    int                      zone = -1;       ///< The selected zone (the one under the last click, else the first).
    MaterialParams           params;          ///< The selected zone's values as they stand.
    MaterialParams           imported;        ///< … and as the file had them (the restore target).
    MaterialMaps             maps;            ///< The maps bound now.
    std::array<std::string, kMaterialMapCount> mapNames{}; ///< What each bound map is: a file name, or "embedded".
    std::array<std::string, kMaterialMapCount> mapPaths{}; ///< … and its file on disk ("" for an embedded image).
    int                      linkedZones = 0; ///< The OTHER zones whose imported material equals this one's (edited together when linked).
    /// The zones an OVERLAY on the selected zone is painted on: the zone itself and every other
    /// zone that was imported with the same base-color map (a face map serves the face, the lips
    /// and the ears: a makeup image reaches all of them). In mesh order; the zone alone when it
    /// shares its map with none, or has none.
    std::vector<int>         overlayZones;
    std::uint64_t            revision = 0;    ///< Bumped by every material edit of the model (the frame notify's cheap key).

    /// The cheap "has what the tab shows changed" test the frame notify runs: not the values
    /// (the revision stands for them) but the selection and the zone list.
    bool sameView(const MaterialView& o) const {
        return valid == o.valid && model == o.model && zone == o.zone && revision == o.revision && zones.size() == o.zones.size();
    }
};

/// What decides whether a MaterialView has CHANGED — the selection, the zone, the zone count and
/// the object's material revision (which stands for every value) — without the view itself
/// (Scene::materialViewKey). The window asks at every rendered frame whether to tell the Materials
/// tab; a view is two whole materials, thirty-five map names and paths and a comparison across
/// every zone, and is built only when the tab re-reads.
struct MaterialViewKey {
    bool          valid = false;
    int           model = -1;
    int           zone = -1;
    std::size_t   zoneCount = 0;
    std::uint64_t revision = 0;

    bool operator==(const MaterialViewKey& o) const {
        return valid == o.valid && model == o.model && zone == o.zone && zoneCount == o.zoneCount && revision == o.revision;
    }
    bool operator!=(const MaterialViewKey& o) const { return !(*this == o); }
};

} // namespace pose

#endif // MATERIALPARAMS_H
