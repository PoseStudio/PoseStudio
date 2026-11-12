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
 */

#ifndef MATERIALPARAMS_H
#define MATERIALPARAMS_H

#include "materialoverlay.h" // MaterialOverlay (the images layered over a zone's maps)

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
    // name below, its MeshData field, the importers that carry it, a tab row).
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

struct MaterialTextureRef {
    std::string source; ///< "", kImportedTexture, or a file path
    bool operator==(const MaterialTextureRef& o) const { return source == o.source; }
    bool operator!=(const MaterialTextureRef& o) const { return source != o.source; }
};

/// The script name of a slot (`texture <slot> <path>`, `matmap.<slot>`): basecolor, normal, bump,
/// roughness, metalness, specmask, translucency, emission, pore, cutout, diffuseroughness, coatweight,
/// coatcolor, coatroughness, coatreflectivity, coatbump, translucencyweight, specularcolor, anisotropy,
/// anisotropyrotation, lobe2, loberatio, detailweight, detailroughness, displacement, ao, transmissioncolor,
/// transmissionweight, coatanisotropy, coatanisotropyrotation, sheen, sheencolor, sheenroughness, thinfilm,
/// thinfilmthickness.
inline const char* materialMapName(MaterialMap map) {
    switch (map) {
    case MaterialMap::BaseColor: return "basecolor";
    case MaterialMap::Normal: return "normal";
    case MaterialMap::Bump: return "bump";
    case MaterialMap::Roughness: return "roughness";
    case MaterialMap::Metalness: return "metalness";
    case MaterialMap::SpecularMask: return "specmask";
    case MaterialMap::Translucency: return "translucency";
    case MaterialMap::Emission: return "emission";
    case MaterialMap::PoreDetail: return "pore";
    case MaterialMap::Opacity: return "cutout";
    case MaterialMap::DiffuseRoughness: return "diffuseroughness";
    case MaterialMap::CoatWeight: return "coatweight";
    case MaterialMap::CoatColor: return "coatcolor";
    case MaterialMap::CoatRoughness: return "coatroughness";
    case MaterialMap::CoatReflectivity: return "coatreflectivity";
    case MaterialMap::CoatBump: return "coatbump";
    case MaterialMap::TranslucencyWeight: return "translucencyweight";
    case MaterialMap::SpecularColor: return "specularcolor";
    case MaterialMap::Anisotropy: return "anisotropy";
    case MaterialMap::AnisotropyRotation: return "anisotropyrotation";
    case MaterialMap::Lobe2Roughness: return "lobe2";
    case MaterialMap::LobeRatio: return "loberatio";
    case MaterialMap::DetailWeight: return "detailweight";
    case MaterialMap::DetailRoughness: return "detailroughness";
    case MaterialMap::Displacement: return "displacement";
    case MaterialMap::AmbientOcclusion: return "ao";
    case MaterialMap::TransmissionColor: return "transmissioncolor";
    case MaterialMap::TransmissionWeight: return "transmissionweight";
    case MaterialMap::CoatAnisotropy: return "coatanisotropy";
    case MaterialMap::CoatAnisotropyRotation: return "coatanisotropyrotation";
    case MaterialMap::SheenWeight: return "sheen";
    case MaterialMap::SheenColor: return "sheencolor";
    case MaterialMap::SheenRoughness: return "sheenroughness";
    case MaterialMap::ThinFilmWeight: return "thinfilm";
    case MaterialMap::ThinFilmThickness: return "thinfilmthickness";
    default: return "";
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
inline bool materialMapByName(const std::string& name, MaterialMap& out) {
    for (int i = 0; i < kMaterialMapCount; ++i) {
        if (name == materialMapName(static_cast<MaterialMap>(i))) {
            out = static_cast<MaterialMap>(i);
            return true;
        }
    }
    return false;
}

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
    void setRoughness(float r) {
        r = std::clamp(r, 0.0f, 1.0f);
        if (lobeRatio < 1.0f && roughness > 1.0e-3f) {
            const float k = r / roughness;
            lobe1Roughness = std::clamp(lobe1Roughness * k, 0.0f, 1.0f);
            lobe2Roughness = std::clamp(lobe2Roughness * k, 0.0f, 1.0f);
        } else {
            lobe1Roughness = r;
            lobe2Roughness = r;
        }
        roughness = r;
    }

    /// The single-lobe roughness as the blend of the two lobes by their ratio (the import's own
    /// rule), after a lobe or the ratio is edited directly. A dual-lobe material only.
    void blendLobes() {
        if (lobeRatio < 1.0f) {
            roughness = std::clamp(lobeRatio * lobe1Roughness + (1.0f - lobeRatio) * lobe2Roughness, 0.0f, 1.0f);
        } else {
            roughness = lobe1Roughness;
        }
    }

    /// Whether the material has two specular lobes (the skin extension): the tab's lobe rows.
    bool dualLobe() const { return lobeRatio < 0.999f; }

    /// Exact equality of every VALUE (the maps excluded): "has the material been edited".
    bool sameValues(const MaterialParams& o) const {
        return baseColor == o.baseColor && metalness == o.metalness && diffuseRoughness == o.diffuseRoughness &&
               specular == o.specular && specularColor == o.specularColor && roughness == o.roughness && ior == o.ior &&
               anisotropy == o.anisotropy && anisotropyRotation == o.anisotropyRotation && transmission == o.transmission &&
               transmissionColor == o.transmissionColor && transmissionDepth == o.transmissionDepth && dispersion == o.dispersion &&
               transmissionRoughness == o.transmissionRoughness && thinWalled == o.thinWalled && translucency == o.translucency && subsurfaceColor == o.subsurfaceColor &&
               subsurfaceRadius == o.subsurfaceRadius && subsurfaceRadiusColor == o.subsurfaceRadiusColor &&
               subsurfaceAnisotropy == o.subsurfaceAnisotropy && sheen == o.sheen && sheenColor == o.sheenColor &&
               sheenRoughness == o.sheenRoughness && topCoat == o.topCoat && topCoatRoughness == o.topCoatRoughness &&
               coatColor == o.coatColor && coatIor == o.coatIor && coatBumpStrength == o.coatBumpStrength && thinFilm == o.thinFilm &&
               coatAnisotropy == o.coatAnisotropy && coatAnisotropyRotation == o.coatAnisotropyRotation && uvRotation == o.uvRotation &&
               doubleSided == o.doubleSided && topCoatEnabled == o.topCoatEnabled && sheenEnabled == o.sheenEnabled && thinFilmEnabled == o.thinFilmEnabled && transmissionEnabled == o.transmissionEnabled && subsurfaceEnabled == o.subsurfaceEnabled && emissionEnabled == o.emissionEnabled && displacementEnabled == o.displacementEnabled && detailEnabled == o.detailEnabled && baseEnabled == o.baseEnabled && specularEnabled == o.specularEnabled && geometryEnabled == o.geometryEnabled && tilingEnabled == o.tilingEnabled &&
               thinFilmThickness == o.thinFilmThickness && thinFilmIor == o.thinFilmIor && emissionColor == o.emissionColor &&
               emission == o.emission && opacity == o.opacity && normalStrength == o.normalStrength &&
               bumpStrength == o.bumpStrength && detailWeight == o.detailWeight && detailTiles == o.detailTiles &&
               detailTilesV == o.detailTilesV && detailOffset == o.detailOffset && detailRoughness == o.detailRoughness &&
               displacementStrength == o.displacementStrength && displacementMidLevel == o.displacementMidLevel &&
               displacementSubdiv == o.displacementSubdiv && aoWeight == o.aoWeight &&
               uvTiling == o.uvTiling && uvOffset == o.uvOffset && lobe1Roughness == o.lobe1Roughness &&
               lobe2Roughness == o.lobe2Roughness && lobeRatio == o.lobeRatio;
    }
    bool operator==(const MaterialParams& o) const { return sameValues(o) && maps == o.maps && overlays == o.overlays; }
    bool operator!=(const MaterialParams& o) const { return !(*this == o); }

    /// Equal to within @p eps on every VALUE — "the same material" for the tab's linked zones (a
    /// figure's skin zones carry one preset's values with different maps per zone, its eyes and
    /// mouth other values). The maps are not compared.
    bool nearlyEqual(const MaterialParams& o, float eps = 1.0e-4f) const {
        const auto close = [eps](float a, float b) { return std::abs(a - b) <= eps; };
        const auto close3 = [&](const glm::vec3& a, const glm::vec3& b) {
            return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z);
        };
        return close3(baseColor, o.baseColor) && close(metalness, o.metalness) && close(diffuseRoughness, o.diffuseRoughness) &&
               close(specular, o.specular) && close3(specularColor, o.specularColor) && close(roughness, o.roughness) &&
               close(ior, o.ior) && close(anisotropy, o.anisotropy) && close(anisotropyRotation, o.anisotropyRotation) &&
               close(transmission, o.transmission) && close3(transmissionColor, o.transmissionColor) &&
               close(transmissionDepth, o.transmissionDepth) && close(dispersion, o.dispersion) &&
               close(transmissionRoughness, o.transmissionRoughness) && thinWalled == o.thinWalled &&
               close(translucency, o.translucency) && close3(subsurfaceColor, o.subsurfaceColor) &&
               close(subsurfaceRadius, o.subsurfaceRadius) && close3(subsurfaceRadiusColor, o.subsurfaceRadiusColor) &&
               close(subsurfaceAnisotropy, o.subsurfaceAnisotropy) && close(sheen, o.sheen) && close3(sheenColor, o.sheenColor) &&
               close(sheenRoughness, o.sheenRoughness) && close(topCoat, o.topCoat) && close(topCoatRoughness, o.topCoatRoughness) &&
               close3(coatColor, o.coatColor) && close(coatIor, o.coatIor) && close(coatBumpStrength, o.coatBumpStrength) &&
               close(coatAnisotropy, o.coatAnisotropy) && close(coatAnisotropyRotation, o.coatAnisotropyRotation) &&
               close(uvRotation, o.uvRotation) && doubleSided == o.doubleSided && topCoatEnabled == o.topCoatEnabled && sheenEnabled == o.sheenEnabled && thinFilmEnabled == o.thinFilmEnabled && transmissionEnabled == o.transmissionEnabled && subsurfaceEnabled == o.subsurfaceEnabled && emissionEnabled == o.emissionEnabled && displacementEnabled == o.displacementEnabled && detailEnabled == o.detailEnabled && baseEnabled == o.baseEnabled && specularEnabled == o.specularEnabled && geometryEnabled == o.geometryEnabled && tilingEnabled == o.tilingEnabled &&
               close(thinFilm, o.thinFilm) &&
               close(thinFilmThickness, o.thinFilmThickness) && close(thinFilmIor, o.thinFilmIor) &&
               close3(emissionColor, o.emissionColor) && close(emission, o.emission) && close(opacity, o.opacity) &&
               close(normalStrength, o.normalStrength) && close(bumpStrength, o.bumpStrength) && close(detailWeight, o.detailWeight) &&
               close(detailTiles, o.detailTiles) && close(detailTilesV, o.detailTilesV) && close(detailOffset.x, o.detailOffset.x) &&
               close(detailOffset.y, o.detailOffset.y) && close(detailRoughness, o.detailRoughness) &&
               close(displacementStrength, o.displacementStrength) && close(displacementMidLevel, o.displacementMidLevel) &&
               close(displacementSubdiv, o.displacementSubdiv) && close(aoWeight, o.aoWeight) &&
               close(uvTiling.x, o.uvTiling.x) && close(uvTiling.y, o.uvTiling.y) &&
               close(uvOffset.x, o.uvOffset.x) && close(uvOffset.y, o.uvOffset.y) && close(lobe1Roughness, o.lobe1Roughness) &&
               close(lobe2Roughness, o.lobe2Roughness) && close(lobeRatio, o.lobeRatio);
    }
};

/// Whether overlay @p o changes the image the viewport binds for slot @p map — its color the
/// base color map, its gloss the roughness map, its bump and normal images theirs. (The
/// displacement image is a renderer's: Displacement answers for an export, nothing is bound.)
inline bool overlayAffects(const MaterialOverlay& o, MaterialMap map) {
    if (!o.enabled) {
        return false;
    }
    switch (map) {
    // (The color is a picture, or a flat Base Color — through an opacity map, or over the whole
    // map when it is not white; the gloss and the metal go wherever the overlay does.)
    case MaterialMap::BaseColor: return overlayHasColor(o) && o.opacity > 0.0f;
    case MaterialMap::Roughness: return o.gloss != 0.0f;
    case MaterialMap::Metalness: return o.metallic != 0.0f;
    case MaterialMap::Bump: return !o.bump.empty() && o.bumpStrength != 0.0f;
    case MaterialMap::Normal: return !o.normal.empty() && o.normalStrength != 0.0f;
    case MaterialMap::Displacement: return !o.displacement.empty() && o.displacementStrength != 0.0f;
    default: return false;
    }
}

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
inline std::string overlaySignature(const std::vector<MaterialOverlay>& overlays, MaterialMap map, bool bumpAsNormal = false) {
    std::string out;
    char        buffer[256];
    char        number[160];
    for (const MaterialOverlay& o : overlays) {
        // The overlay's PLACE moves every one of its images: its offset, its rotation, its scale
        // — and, turned or scaled, the image it does so about (another anchor is another centre).
        std::snprintf(number, sizeof(number), "@%a,%a,%a,%a", static_cast<double>(o.offset.x), static_cast<double>(o.offset.y),
                      static_cast<double>(o.rotation), static_cast<double>(o.scale));
        std::string place = number;
        if (overlayTransformed(o)) {
            place += '^';
            place += overlayAnchorSource(o);
        }
        if (bumpAsNormal && map == MaterialMap::Normal && overlayAffects(o, MaterialMap::Bump)) {
            std::snprintf(buffer, sizeof(buffer), "|bump %a|", static_cast<double>(o.bumpStrength));
            out += o.bump;
            out += buffer;
            out += place;
        }
        if (!overlayAffects(o, map)) {
            continue;
        }
        switch (map) {
        case MaterialMap::BaseColor:
            std::snprintf(buffer, sizeof(buffer), "|%d;%a;%a;%a;%a;%a;%a;%a;%a;%a|", static_cast<int>(o.blend),
                          static_cast<double>(o.opacity), static_cast<double>(o.tint.x), static_cast<double>(o.tint.y),
                          static_cast<double>(o.tint.z), static_cast<double>(o.brightness), static_cast<double>(o.contrast),
                          static_cast<double>(o.gamma), static_cast<double>(o.hue), static_cast<double>(o.saturation));
            out += o.color;
            out += '~'; // (the opacity map: the shape of the color, the gloss and the metal)
            out += o.opacityMap;
            break;
        case MaterialMap::Roughness:
            std::snprintf(buffer, sizeof(buffer), "|%a|", static_cast<double>(o.gloss));
            out += o.color;
            out += '~';
            out += o.opacityMap;
            break;
        case MaterialMap::Metalness:
            std::snprintf(buffer, sizeof(buffer), "|%a|", static_cast<double>(o.metallic));
            out += o.color;
            out += '~';
            out += o.opacityMap;
            out += '&';
            out += o.metallicMap;
            break;
        case MaterialMap::Bump:
            std::snprintf(buffer, sizeof(buffer), "|%a|", static_cast<double>(o.bumpStrength));
            out += o.bump;
            break;
        case MaterialMap::Normal:
            std::snprintf(buffer, sizeof(buffer), "|%a|", static_cast<double>(o.normalStrength));
            out += o.normal;
            break;
        default:
            std::snprintf(buffer, sizeof(buffer), "|%a|", static_cast<double>(o.displacementStrength));
            out += o.displacement;
            break;
        }
        out += buffer;
        out += place;
    }
    return out;
}

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
    Count
};

/// A field's display value off @p p.
inline float materialFieldValue(const MaterialParams& p, MaterialField field) {
    switch (field) {
    case MaterialField::Roughness: return p.roughness * 100.0f;
    case MaterialField::Metallic: return p.metalness * 100.0f;
    case MaterialField::Specular: return p.specular * 100.0f;
    case MaterialField::NormalStrength: return p.normalStrength * 100.0f;
    case MaterialField::PoreDetail: return p.detailWeight * 100.0f;
    case MaterialField::PoreTiles: return p.detailTiles;
    case MaterialField::Opacity: return p.opacity * 100.0f;
    case MaterialField::Subsurface: return p.translucency * 100.0f;
    case MaterialField::TopCoat: return p.topCoat * 100.0f;
    case MaterialField::TopCoatRoughness: return p.topCoatRoughness * 100.0f;
    case MaterialField::Ior: return p.ior;
    case MaterialField::Anisotropy: return p.anisotropy * 100.0f;
    case MaterialField::AnisotropyRotation: return p.anisotropyRotation;
    case MaterialField::DiffuseRoughness: return p.diffuseRoughness * 100.0f;
    case MaterialField::Transmission: return p.transmission * 100.0f;
    case MaterialField::TransmissionDepth: return p.transmissionDepth * 100.0f; // cm
    case MaterialField::Dispersion: return p.dispersion * 100.0f;
    case MaterialField::SubsurfaceRadius: return p.subsurfaceRadius; // cm
    case MaterialField::Sheen: return p.sheen * 100.0f;
    case MaterialField::SheenRoughness: return p.sheenRoughness * 100.0f;
    case MaterialField::CoatIor: return p.coatIor;
    case MaterialField::ThinFilm: return p.thinFilm * 100.0f;
    case MaterialField::ThinFilmThickness: return p.thinFilmThickness; // nm
    case MaterialField::ThinFilmIor: return p.thinFilmIor;
    case MaterialField::Emission: return p.emission;
    case MaterialField::BumpStrength: return p.bumpStrength * 100.0f;
    case MaterialField::TilingX: return p.uvTiling.x;
    case MaterialField::TilingY: return p.uvTiling.y;
    case MaterialField::OffsetX: return p.uvOffset.x;
    case MaterialField::OffsetY: return p.uvOffset.y;
    case MaterialField::CoatBumpStrength: return p.coatBumpStrength * 100.0f;
    case MaterialField::Lobe1Roughness: return p.lobe1Roughness * 100.0f;
    case MaterialField::Lobe2Roughness: return p.lobe2Roughness * 100.0f;
    case MaterialField::LobeRatio: return p.lobeRatio * 100.0f;
    case MaterialField::DetailRoughness: return p.detailRoughness * 100.0f;
    case MaterialField::DetailTilesV: return p.detailTilesV;
    case MaterialField::DetailOffsetU: return p.detailOffset.x;
    case MaterialField::DetailOffsetV: return p.detailOffset.y;
    case MaterialField::DisplacementStrength: return p.displacementStrength; // cm
    case MaterialField::DisplacementMidLevel: return p.displacementMidLevel * 100.0f;
    case MaterialField::DisplacementSubdiv: return p.displacementSubdiv;
    case MaterialField::AmbientOcclusion: return p.aoWeight * 100.0f;
    case MaterialField::SubsurfaceAnisotropy: return p.subsurfaceAnisotropy * 100.0f;
    case MaterialField::CoatAnisotropy: return p.coatAnisotropy * 100.0f;
    case MaterialField::CoatAnisotropyRotation: return p.coatAnisotropyRotation;
    case MaterialField::TilingRotation: return p.uvRotation;
    case MaterialField::TopCoatEnabled: return p.topCoatEnabled ? 1.0f : 0.0f;
    case MaterialField::SheenEnabled: return p.sheenEnabled ? 1.0f : 0.0f;
    case MaterialField::ThinFilmEnabled: return p.thinFilmEnabled ? 1.0f : 0.0f;
    case MaterialField::TransmissionEnabled: return p.transmissionEnabled ? 1.0f : 0.0f;
    case MaterialField::SubsurfaceEnabled: return p.subsurfaceEnabled ? 1.0f : 0.0f;
    case MaterialField::EmissionEnabled: return p.emissionEnabled ? 1.0f : 0.0f;
    case MaterialField::DisplacementEnabled: return p.displacementEnabled ? 1.0f : 0.0f;
    case MaterialField::DetailEnabled: return p.detailEnabled ? 1.0f : 0.0f;
    case MaterialField::BaseEnabled: return p.baseEnabled ? 1.0f : 0.0f;
    case MaterialField::SpecularEnabled: return p.specularEnabled ? 1.0f : 0.0f;
    case MaterialField::GeometryEnabled: return p.geometryEnabled ? 1.0f : 0.0f;
    case MaterialField::TilingEnabled: return p.tilingEnabled ? 1.0f : 0.0f;
    case MaterialField::ThinWalled: return p.thinWalled ? 1.0f : 0.0f;
    default: return 0.0f;
    }
}

/// The change a display value makes to @p p (clamped to what makes sense).
inline void setMaterialField(MaterialParams& p, MaterialField field, float v) {
    switch (field) {
    case MaterialField::Roughness: p.setRoughness(v / 100.0f); break;
    case MaterialField::Metallic: p.metalness = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::Specular: p.specular = std::max(0.0f, v / 100.0f); break;
    case MaterialField::NormalStrength: p.normalStrength = std::clamp(v / 100.0f, 0.0f, 7.9f); break;
    case MaterialField::PoreDetail: p.detailWeight = std::clamp(v / 100.0f, 0.0f, 0.99f); break;
    case MaterialField::PoreTiles: p.detailTiles = std::clamp(std::round(v), 1.0f, 64.0f); break;
    case MaterialField::Opacity: p.opacity = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::Subsurface: p.translucency = std::max(0.0f, v / 100.0f); break;
    case MaterialField::TopCoat: p.topCoat = std::max(0.0f, v / 100.0f); break;
    case MaterialField::TopCoatRoughness: p.topCoatRoughness = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::Ior: p.ior = std::clamp(v, 1.0f, 3.0f); break;
    case MaterialField::Anisotropy: p.anisotropy = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::AnisotropyRotation: p.anisotropyRotation = std::clamp(v, 0.0f, 360.0f); break;
    case MaterialField::DiffuseRoughness: p.diffuseRoughness = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::Transmission: p.transmission = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::TransmissionDepth: p.transmissionDepth = std::max(0.0f, v / 100.0f); break;
    case MaterialField::Dispersion: p.dispersion = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::SubsurfaceRadius: p.subsurfaceRadius = std::clamp(v, 0.0f, 100.0f); break;
    case MaterialField::Sheen: p.sheen = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::SheenRoughness: p.sheenRoughness = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::CoatIor: p.coatIor = std::clamp(v, 1.0f, 3.0f); break;
    case MaterialField::ThinFilm: p.thinFilm = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::ThinFilmThickness: p.thinFilmThickness = std::clamp(v, 0.0f, 2000.0f); break;
    case MaterialField::ThinFilmIor: p.thinFilmIor = std::clamp(v, 1.0f, 3.0f); break;
    case MaterialField::Emission: p.emission = std::max(0.0f, v); break;
    case MaterialField::BumpStrength: p.bumpStrength = std::clamp(v / 100.0f, 0.0f, 7.9f); break;
    case MaterialField::TilingX: p.uvTiling.x = std::clamp(v, 0.01f, 1000.0f); break;
    case MaterialField::TilingY: p.uvTiling.y = std::clamp(v, 0.01f, 1000.0f); break;
    case MaterialField::OffsetX: p.uvOffset.x = std::clamp(v, -1000.0f, 1000.0f); break;
    case MaterialField::OffsetY: p.uvOffset.y = std::clamp(v, -1000.0f, 1000.0f); break;
    case MaterialField::CoatBumpStrength: p.coatBumpStrength = std::clamp(v / 100.0f, 0.0f, 7.9f); break;
    // A lobe edited directly re-blends the single-lobe roughness the way the import does
    // (ratio x lobe 1 + (1 - ratio) x lobe 2): the analytic modes, the env mip and the Roughness
    // Map view read that blend, the PBR mode the lobes themselves.
    case MaterialField::Lobe1Roughness: p.lobe1Roughness = std::clamp(v / 100.0f, 0.0f, 1.0f); p.blendLobes(); break;
    case MaterialField::Lobe2Roughness: p.lobe2Roughness = std::clamp(v / 100.0f, 0.0f, 1.0f); p.blendLobes(); break;
    case MaterialField::LobeRatio: p.lobeRatio = std::clamp(v / 100.0f, 0.0f, 1.0f); p.blendLobes(); break;
    case MaterialField::DetailRoughness: p.detailRoughness = std::clamp(v / 100.0f, 0.0f, 4.0f); break;
    case MaterialField::DetailTilesV: p.detailTilesV = std::clamp(std::round(v), 1.0f, 512.0f); break;
    case MaterialField::DetailOffsetU: p.detailOffset.x = std::clamp(v, -1000.0f, 1000.0f); break;
    case MaterialField::DetailOffsetV: p.detailOffset.y = std::clamp(v, -1000.0f, 1000.0f); break;
    case MaterialField::DisplacementStrength: p.displacementStrength = std::clamp(v, -100.0f, 100.0f); break;
    case MaterialField::DisplacementMidLevel: p.displacementMidLevel = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::DisplacementSubdiv: p.displacementSubdiv = std::clamp(std::round(v), 0.0f, 5.0f); break;
    case MaterialField::AmbientOcclusion: p.aoWeight = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::SubsurfaceAnisotropy: p.subsurfaceAnisotropy = std::clamp(v / 100.0f, -1.0f, 1.0f); break;
    case MaterialField::CoatAnisotropy: p.coatAnisotropy = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::CoatAnisotropyRotation: p.coatAnisotropyRotation = std::clamp(v, 0.0f, 360.0f); break;
    case MaterialField::TilingRotation: p.uvRotation = std::clamp(v, -360.0f, 360.0f); break;
    case MaterialField::TopCoatEnabled: p.topCoatEnabled = v >= 0.5f; break;
    case MaterialField::SheenEnabled: p.sheenEnabled = v >= 0.5f; break;
    case MaterialField::ThinFilmEnabled: p.thinFilmEnabled = v >= 0.5f; break;
    case MaterialField::TransmissionEnabled: p.transmissionEnabled = v >= 0.5f; break;
    case MaterialField::SubsurfaceEnabled: p.subsurfaceEnabled = v >= 0.5f; break;
    case MaterialField::EmissionEnabled: p.emissionEnabled = v >= 0.5f; break;
    case MaterialField::DisplacementEnabled: p.displacementEnabled = v >= 0.5f; break;
    case MaterialField::DetailEnabled: p.detailEnabled = v >= 0.5f; break;
    case MaterialField::BaseEnabled: p.baseEnabled = v >= 0.5f; break;
    case MaterialField::SpecularEnabled: p.specularEnabled = v >= 0.5f; break;
    case MaterialField::GeometryEnabled: p.geometryEnabled = v >= 0.5f; break;
    case MaterialField::TilingEnabled: p.tilingEnabled = v >= 0.5f; break;
    case MaterialField::ThinWalled: p.thinWalled = v >= 0.5f; break;
    default: break;
    }
}

/// The field's name in the scripted test (`material <name> <value>`, `mat.<name>`).
inline const char* materialFieldName(MaterialField field) {
    switch (field) {
    case MaterialField::Roughness: return "roughness";
    case MaterialField::Metallic: return "metallic";
    case MaterialField::Specular: return "specular";
    case MaterialField::NormalStrength: return "normal";
    case MaterialField::PoreDetail: return "pore";
    case MaterialField::PoreTiles: return "tiles";
    case MaterialField::Opacity: return "opacity";
    case MaterialField::Subsurface: return "subsurface";
    case MaterialField::TopCoat: return "topcoat";
    case MaterialField::TopCoatRoughness: return "topcoatrough";
    case MaterialField::Ior: return "ior";
    case MaterialField::Anisotropy: return "anisotropy";
    case MaterialField::AnisotropyRotation: return "anisotropyrotation";
    case MaterialField::DiffuseRoughness: return "diffuserough";
    case MaterialField::Transmission: return "transmission";
    case MaterialField::TransmissionDepth: return "transmissiondepth";
    case MaterialField::Dispersion: return "dispersion";
    case MaterialField::SubsurfaceRadius: return "subsurfaceradius";
    case MaterialField::Sheen: return "sheen";
    case MaterialField::SheenRoughness: return "sheenrough";
    case MaterialField::CoatIor: return "coatior";
    case MaterialField::ThinFilm: return "thinfilm";
    case MaterialField::ThinFilmThickness: return "thinfilmthickness";
    case MaterialField::ThinFilmIor: return "thinfilmior";
    case MaterialField::Emission: return "emission";
    case MaterialField::BumpStrength: return "bump";
    case MaterialField::TilingX: return "tilingx";
    case MaterialField::TilingY: return "tilingy";
    case MaterialField::OffsetX: return "offsetx";
    case MaterialField::OffsetY: return "offsety";
    case MaterialField::CoatBumpStrength: return "coatbump";
    case MaterialField::Lobe1Roughness: return "lobe1";
    case MaterialField::Lobe2Roughness: return "lobe2";
    case MaterialField::LobeRatio: return "loberatio";
    case MaterialField::DetailRoughness: return "detailroughness";
    case MaterialField::DetailTilesV: return "detailtilesv";
    case MaterialField::DetailOffsetU: return "detailoffsetu";
    case MaterialField::DetailOffsetV: return "detailoffsetv";
    case MaterialField::DisplacementStrength: return "displacement";
    case MaterialField::DisplacementMidLevel: return "displacementmid";
    case MaterialField::DisplacementSubdiv: return "displacementsubd";
    case MaterialField::AmbientOcclusion: return "aoweight";
    case MaterialField::SubsurfaceAnisotropy: return "sssdirection";
    case MaterialField::CoatAnisotropy: return "coatanisotropy";
    case MaterialField::CoatAnisotropyRotation: return "coatanisotropyrotation";
    case MaterialField::TilingRotation: return "tilingrotation";
    case MaterialField::TopCoatEnabled: return "topcoatenabled";
    case MaterialField::SheenEnabled: return "sheenenabled";
    case MaterialField::ThinFilmEnabled: return "thinfilmenabled";
    case MaterialField::TransmissionEnabled: return "transmissionenabled";
    case MaterialField::SubsurfaceEnabled: return "subsurfaceenabled";
    case MaterialField::EmissionEnabled: return "emissionenabled";
    case MaterialField::DisplacementEnabled: return "displacementenabled";
    case MaterialField::DetailEnabled: return "detailenabled";
    case MaterialField::BaseEnabled: return "baseenabled";
    case MaterialField::SpecularEnabled: return "specularenabled";
    case MaterialField::GeometryEnabled: return "geometryenabled";
    case MaterialField::TilingEnabled: return "tilingenabled";
    case MaterialField::ThinWalled: return "thinwalled";
    default: return "";
    }
}

/// The inverse of materialFieldName; false for an unknown name.
inline bool materialFieldByName(const std::string& name, MaterialField& out) {
    for (int i = 0; i < static_cast<int>(MaterialField::Count); ++i) {
        if (name == materialFieldName(static_cast<MaterialField>(i))) {
            out = static_cast<MaterialField>(i);
            return true;
        }
    }
    return false;
}

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
inline const char* materialColorName(MaterialColor which) {
    switch (which) {
    case MaterialColor::Base: return "color";
    case MaterialColor::Specular: return "specularcolor";
    case MaterialColor::Transmission: return "transmissioncolor";
    case MaterialColor::Subsurface: return "subsurfacecolor";
    case MaterialColor::Sheen: return "sheencolor";
    case MaterialColor::Coat: return "coatcolor";
    case MaterialColor::Emission: return "emissioncolor";
    case MaterialColor::ScatterColor: return "scattercolor";
    default: return "";
    }
}
inline bool materialColorByName(const std::string& name, MaterialColor& out) {
    for (int i = 0; i < static_cast<int>(MaterialColor::Count); ++i) {
        if (name == materialColorName(static_cast<MaterialColor>(i))) {
            out = static_cast<MaterialColor>(i);
            return true;
        }
    }
    return false;
}

/// Which maps a zone carries RIGHT NOW (bound on the GPU) — facts the tab shows beside the
/// sliders. Indexed by MaterialMap.
struct MaterialMaps {
    std::array<bool, kMaterialMapCount> present{};
    bool has(MaterialMap m) const { return present[static_cast<std::size_t>(m)]; }
    bool operator==(const MaterialMaps& o) const { return present == o.present; }
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

} // namespace pose

#endif // MATERIALPARAMS_H
