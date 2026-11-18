/**
 * @file materialparams.cpp
 * @brief The canonical material's TABLES and non-trivial functions (materialparams.h) and the
 *        overlay's (materialoverlay.h): the slots', fields', colors' and blends' names, the
 *        fields' display values and their setters, the material's comparisons, and what of
 *        an overlay stack decides a slot's composite.
 *
 * They live here rather than in the two headers because those headers are plain-data contracts
 * that most of the viewport includes (through mesh.h and scene.h): a name table edited in a
 * header rebuilt all of it. Qt-free: std + glm.
 */

#include "materialparams.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pose {

namespace {

/// The bound of a value that has none of its own (a weight with no ceiling, an offset, an
/// angle): nothing the application reads or makes comes near it, and held to it the arithmetic
/// downstream — a product of a few such values, a float turned into a count — stays finite.
constexpr float kValueBound = 1.0e6f;

/// @p v held to [lo, hi]: itself, bit for bit, when it is inside; lo when it is no number.
void hold(float& v, float lo, float hi) {
    v = !(v >= lo) ? lo : v > hi ? hi : v;
}

} // namespace

// ============================================================================================
// THE TEXTURE SLOTS — a slot's source, and the slots' script names
// ============================================================================================

std::string textureSourceWithChannel(const std::string& path, int channel) {
    if (channel <= 0 || channel > 3 || path.empty() || path == kImportedTexture) {
        return path;
    }
    return path + kTextureChannelMark + "rgba"[channel];
}

std::string textureSourcePath(const std::string& source, int* channel) {
    if (channel != nullptr) {
        *channel = 0;
    }
    const std::string mark(kTextureChannelMark);
    const std::size_t at = source.rfind(mark);
    if (at == std::string::npos || at + mark.size() + 1 != source.size()) {
        return source;
    }
    const char letter = source.back();
    const int  index = letter == 'r' ? 0 : letter == 'g' ? 1 : letter == 'b' ? 2 : letter == 'a' ? 3 : -1;
    if (index < 0) {
        return source;
    }
    if (channel != nullptr) {
        *channel = index;
    }
    return source.substr(0, at);
}

const char* materialMapName(MaterialMap map) {
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

bool materialMapByName(const std::string& name, MaterialMap& out) {
    for (int i = 0; i < kMaterialMapCount; ++i) {
        if (name == materialMapName(static_cast<MaterialMap>(i))) {
            out = static_cast<MaterialMap>(i);
            return true;
        }
    }
    return false;
}


// ============================================================================================
// THE MATERIAL'S VALUES — the roughness with its two lobes, and the comparisons
// ============================================================================================

void MaterialParams::setRoughness(float r) {
    r = std::clamp(r, 0.0f, 1.0f);
    if (dualLobe() && roughness > 1.0e-3f) {
        const float k = r / roughness;
        lobe1Roughness = std::clamp(lobe1Roughness * k, 0.0f, 1.0f);
        lobe2Roughness = std::clamp(lobe2Roughness * k, 0.0f, 1.0f);
    } else {
        lobe1Roughness = r;
        lobe2Roughness = r;
    }
    roughness = r;
}

void MaterialParams::blendLobes() {
    if (dualLobe()) {
        roughness = std::clamp(lobeRatio * lobe1Roughness + (1.0f - lobeRatio) * lobe2Roughness, 0.0f, 1.0f);
    } else {
        roughness = lobe1Roughness;
    }
}

bool MaterialParams::sameValues(const MaterialParams& o) const {
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
           doubleSided == o.doubleSided && flipNormal == o.flipNormal && topCoatEnabled == o.topCoatEnabled && sheenEnabled == o.sheenEnabled && thinFilmEnabled == o.thinFilmEnabled && transmissionEnabled == o.transmissionEnabled && subsurfaceEnabled == o.subsurfaceEnabled && emissionEnabled == o.emissionEnabled && displacementEnabled == o.displacementEnabled && detailEnabled == o.detailEnabled && baseEnabled == o.baseEnabled && specularEnabled == o.specularEnabled && geometryEnabled == o.geometryEnabled && tilingEnabled == o.tilingEnabled &&
           smoothByAngle == o.smoothByAngle && smoothAngle == o.smoothAngle &&
           thinFilmThickness == o.thinFilmThickness && thinFilmIor == o.thinFilmIor && emissionColor == o.emissionColor &&
           emission == o.emission && opacity == o.opacity && normalStrength == o.normalStrength &&
           bumpStrength == o.bumpStrength && detailWeight == o.detailWeight && detailTiles == o.detailTiles &&
           detailTilesV == o.detailTilesV && detailOffset == o.detailOffset && detailRoughness == o.detailRoughness &&
           displacementStrength == o.displacementStrength && displacementMidLevel == o.displacementMidLevel &&
           displacementSubdiv == o.displacementSubdiv && aoWeight == o.aoWeight &&
           uvTiling == o.uvTiling && uvOffset == o.uvOffset && lobe1Roughness == o.lobe1Roughness &&
           lobe2Roughness == o.lobe2Roughness && lobeRatio == o.lobeRatio;
}

bool MaterialParams::nearlyEqual(const MaterialParams& o, float eps) const {
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
           close(uvRotation, o.uvRotation) && doubleSided == o.doubleSided && flipNormal == o.flipNormal && topCoatEnabled == o.topCoatEnabled && sheenEnabled == o.sheenEnabled && thinFilmEnabled == o.thinFilmEnabled && transmissionEnabled == o.transmissionEnabled && subsurfaceEnabled == o.subsurfaceEnabled && emissionEnabled == o.emissionEnabled && displacementEnabled == o.displacementEnabled && detailEnabled == o.detailEnabled && baseEnabled == o.baseEnabled && specularEnabled == o.specularEnabled && geometryEnabled == o.geometryEnabled && tilingEnabled == o.tilingEnabled &&
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


void clampMaterialParams(MaterialParams& p) {
    const auto share = [](float& v) { hold(v, 0.0f, 1.0f); };                    // a 0 .. 1 weight
    const auto amount = [](float& v) { hold(v, 0.0f, kValueBound); };            // 0 and up
    const auto anywhere = [](float& v) { hold(v, -kValueBound, kValueBound); }; // an offset, an angle
    const auto color = [&](glm::vec3& c) {
        amount(c.x);
        amount(c.y);
        amount(c.z);
    };
    // Base and specular (the two lobes with them).
    color(p.baseColor);
    share(p.metalness);
    share(p.diffuseRoughness);
    amount(p.specular);
    color(p.specularColor);
    share(p.roughness);
    hold(p.ior, 1.0f, 3.0f);
    share(p.anisotropy);
    anywhere(p.anisotropyRotation);
    share(p.lobe1Roughness);
    share(p.lobe2Roughness);
    share(p.lobeRatio);
    // Transmission (a negative roughness of its own = the surface's).
    share(p.transmission);
    color(p.transmissionColor);
    amount(p.transmissionDepth);
    hold(p.transmissionRoughness, -1.0f, 1.0f);
    share(p.dispersion);
    // Subsurface.
    amount(p.translucency);
    color(p.subsurfaceColor);
    color(p.subsurfaceRadiusColor);
    hold(p.subsurfaceRadius, 0.0f, 100.0f);
    hold(p.subsurfaceAnisotropy, -1.0f, 1.0f);
    // Sheen, coat, thin film, emission.
    share(p.sheen);
    color(p.sheenColor);
    share(p.sheenRoughness);
    amount(p.topCoat);
    share(p.topCoatRoughness);
    color(p.coatColor);
    hold(p.coatIor, 1.0f, 3.0f);
    hold(p.coatBumpStrength, 0.0f, 8.0f);
    share(p.coatAnisotropy);
    anywhere(p.coatAnisotropyRotation);
    share(p.thinFilm);
    amount(p.thinFilmThickness);
    hold(p.thinFilmIor, 1.0f, 3.0f);
    color(p.emissionColor);
    amount(p.emission);
    // Geometry. (A normal strength of 8 is what three importers clamp to, a little over the
    // row's 7.9; a bump strength below 0 is a MaterialX height read inverted.)
    share(p.opacity);
    hold(p.normalStrength, 0.0f, 8.0f);
    hold(p.bumpStrength, -8.0f, 8.0f);
    share(p.aoWeight);
    hold(p.smoothAngle, 0.0f, 180.0f);
    // The detail layer (a tile count of 0: no layer, or "as the horizontal count") and displacement.
    share(p.detailWeight);
    hold(p.detailTiles, 0.0f, 512.0f);
    hold(p.detailTilesV, 0.0f, 512.0f);
    anywhere(p.detailOffset.x);
    anywhere(p.detailOffset.y);
    hold(p.detailRoughness, 0.0f, 4.0f);
    hold(p.displacementStrength, -100.0f, 100.0f);
    share(p.displacementMidLevel);
    hold(p.displacementSubdiv, 0.0f, 5.0f);
    // The placement (a tiling below 0 is a mirrored map: glTF's texture transform gives one).
    anywhere(p.uvTiling.x);
    anywhere(p.uvTiling.y);
    anywhere(p.uvOffset.x);
    anywhere(p.uvOffset.y);
    anywhere(p.uvRotation);
}

// ============================================================================================
// THE TAB'S ROWS AS FIELDS — display values, setters and script names
// ============================================================================================

float materialFieldValue(const MaterialParams& p, MaterialField field) {
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
    case MaterialField::FlipNormal: return p.flipNormal ? 1.0f : 0.0f;
    case MaterialField::SmoothByAngle: return p.smoothByAngle ? 1.0f : 0.0f;
    case MaterialField::SmoothAngle: return p.smoothAngle;
    default: return 0.0f;
    }
}

void setMaterialField(MaterialParams& p, MaterialField field, float v) {
    switch (field) {
    case MaterialField::Roughness: p.setRoughness(v / 100.0f); break;
    case MaterialField::Metallic: p.metalness = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case MaterialField::Specular: p.specular = std::max(0.0f, v / 100.0f); break;
    case MaterialField::NormalStrength: p.normalStrength = std::clamp(v / 100.0f, 0.0f, 7.9f); break;
    case MaterialField::PoreDetail: p.detailWeight = std::clamp(v / 100.0f, 0.0f, 0.99f); break;
    // (The detail layer's two tile counts share one range — the tab's rows', the importer's:
    // the shader reads the count from MaterialGpu::detail, which holds any of them.)
    case MaterialField::PoreTiles: p.detailTiles = std::clamp(std::round(v), 1.0f, 512.0f); break;
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
    case MaterialField::FlipNormal: p.flipNormal = v >= 0.5f; break;
    case MaterialField::SmoothByAngle: p.smoothByAngle = v >= 0.5f; break;
    case MaterialField::SmoothAngle: p.smoothAngle = std::clamp(v, 0.0f, 180.0f); break;
    default: break;
    }
}

const char* materialFieldName(MaterialField field) {
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
    case MaterialField::FlipNormal: return "flipnormal";
    case MaterialField::SmoothByAngle: return "smooth";
    case MaterialField::SmoothAngle: return "smoothangle";
    default: return "";
    }
}

bool materialFieldByName(const std::string& name, MaterialField& out) {
    for (int i = 0; i < static_cast<int>(MaterialField::Count); ++i) {
        if (name == materialFieldName(static_cast<MaterialField>(i))) {
            out = static_cast<MaterialField>(i);
            return true;
        }
    }
    return false;
}

const char* materialColorName(MaterialColor which) {
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

bool materialColorByName(const std::string& name, MaterialColor& out) {
    for (int i = 0; i < static_cast<int>(MaterialColor::Count); ++i) {
        if (name == materialColorName(static_cast<MaterialColor>(i))) {
            out = static_cast<MaterialColor>(i);
            return true;
        }
    }
    return false;
}


// ============================================================================================
// THE OVERLAYS (materialoverlay.h) — names, fields, and what decides a slot's composite
// ============================================================================================

const char* overlayBlendName(OverlayBlend blend) {
    switch (blend) {
    case OverlayBlend::Multiply: return "multiply";
    case OverlayBlend::Normal: return "normal";
    case OverlayBlend::SoftLight: return "softlight";
    case OverlayBlend::Overlay: return "overlay";
    case OverlayBlend::Screen: return "screen";
    case OverlayBlend::Darken: return "darken";
    case OverlayBlend::Lighten: return "lighten";
    default: return "";
    }
}

bool overlayBlendByName(const std::string& name, OverlayBlend& out) {
    for (int i = 0; i < static_cast<int>(OverlayBlend::Count); ++i) {
        if (name == overlayBlendName(static_cast<OverlayBlend>(i))) {
            out = static_cast<OverlayBlend>(i);
            return true;
        }
    }
    return false;
}

const char* overlayElementName(OverlayElement element) {
    switch (element) {
    case OverlayElement::Color: return "color";
    case OverlayElement::Opacity: return "opacity";
    case OverlayElement::Metallic: return "metallic";
    case OverlayElement::Bump: return "bump";
    case OverlayElement::Normal: return "normal";
    case OverlayElement::Displacement: return "displacement";
    default: return "";
    }
}

bool overlayElementByName(const std::string& name, OverlayElement& out) {
    for (int i = 0; i < static_cast<int>(OverlayElement::Count); ++i) {
        if (name == overlayElementName(static_cast<OverlayElement>(i))) {
            out = static_cast<OverlayElement>(i);
            return true;
        }
    }
    return false;
}

int nextOverlayNumber(const std::vector<MaterialOverlay>& overlays, const std::string& prefix) {
    int number = static_cast<int>(overlays.size()) + 1;
    const auto taken = [&](int n) {
        const std::string name = prefix + " " + std::to_string(n);
        return std::any_of(overlays.begin(), overlays.end(), [&](const MaterialOverlay& o) { return o.name == name; });
    };
    while (taken(number)) {
        ++number;
    }
    return number;
}

float overlayFieldValue(const MaterialOverlay& o, OverlayField field) {
    switch (field) {
    case OverlayField::Enabled: return o.enabled ? 1.0f : 0.0f;
    case OverlayField::Blend: return static_cast<float>(o.blend);
    case OverlayField::Opacity: return o.opacity * 100.0f;
    case OverlayField::Brightness: return o.brightness * 100.0f;
    case OverlayField::Contrast: return o.contrast * 100.0f;
    case OverlayField::Gamma: return o.gamma;
    case OverlayField::Hue: return o.hue;
    case OverlayField::Saturation: return o.saturation * 100.0f;
    case OverlayField::Gloss: return o.gloss * 100.0f;
    case OverlayField::BumpStrength: return o.bumpStrength * 100.0f;
    case OverlayField::NormalStrength: return o.normalStrength * 100.0f;
    case OverlayField::DisplacementStrength: return o.displacementStrength * 100.0f;
    case OverlayField::OffsetX: return o.offset.x * 100.0f;
    case OverlayField::OffsetY: return o.offset.y * 100.0f;
    case OverlayField::Rotation: return o.rotation;
    case OverlayField::Scale: return o.scale * 100.0f;
    case OverlayField::Metallic: return o.metallic * 100.0f;
    default: return 0.0f;
    }
}

void setOverlayField(MaterialOverlay& o, OverlayField field, float v) {
    switch (field) {
    case OverlayField::Enabled: o.enabled = v >= 0.5f; break;
    case OverlayField::Blend:
        o.blend = static_cast<OverlayBlend>(std::clamp(static_cast<int>(std::lround(v)), 0, static_cast<int>(OverlayBlend::Count) - 1));
        break;
    case OverlayField::Opacity: o.opacity = std::clamp(v / 100.0f, 0.0f, 1.0f); break;
    case OverlayField::Brightness: o.brightness = std::clamp(v / 100.0f, -1.0f, 1.0f); break;
    case OverlayField::Contrast: o.contrast = std::clamp(v / 100.0f, -1.0f, 1.0f); break;
    case OverlayField::Gamma: o.gamma = std::clamp(v, 0.2f, 5.0f); break;
    case OverlayField::Hue: o.hue = std::clamp(v, -180.0f, 180.0f); break;
    case OverlayField::Saturation: o.saturation = std::clamp(v / 100.0f, -1.0f, 1.0f); break;
    case OverlayField::Gloss: o.gloss = std::clamp(v / 100.0f, -1.0f, 1.0f); break;
    case OverlayField::BumpStrength: o.bumpStrength = std::clamp(v / 100.0f, -4.0f, 4.0f); break;
    case OverlayField::NormalStrength: o.normalStrength = std::clamp(v / 100.0f, 0.0f, 4.0f); break;
    case OverlayField::DisplacementStrength: o.displacementStrength = std::clamp(v / 100.0f, -4.0f, 4.0f); break;
    // (A whole map either way: beyond it nothing of the overlay is left on the map.)
    case OverlayField::OffsetX: o.offset.x = std::clamp(v / 100.0f, -1.0f, 1.0f); break;
    case OverlayField::OffsetY: o.offset.y = std::clamp(v / 100.0f, -1.0f, 1.0f); break;
    case OverlayField::Rotation: o.rotation = std::clamp(v, -180.0f, 180.0f); break;
    case OverlayField::Scale: o.scale = std::clamp(v / 100.0f, kOverlayScaleMin, kOverlayScaleMax); break;
    case OverlayField::Metallic: o.metallic = std::clamp(v / 100.0f, -1.0f, 1.0f); break;
    default: break;
    }
}

void clampMaterialOverlay(MaterialOverlay& o) {
    // (setOverlayField's ranges, in the values' own units.)
    hold(o.offset.x, -1.0f, 1.0f);
    hold(o.offset.y, -1.0f, 1.0f);
    hold(o.rotation, -180.0f, 180.0f);
    hold(o.scale, kOverlayScaleMin, kOverlayScaleMax);
    hold(o.opacity, 0.0f, 1.0f);
    hold(o.tint.x, 0.0f, 1.0f);
    hold(o.tint.y, 0.0f, 1.0f);
    hold(o.tint.z, 0.0f, 1.0f);
    hold(o.brightness, -1.0f, 1.0f);
    hold(o.contrast, -1.0f, 1.0f);
    hold(o.gamma, 0.2f, 5.0f);
    hold(o.hue, -180.0f, 180.0f);
    hold(o.saturation, -1.0f, 1.0f);
    hold(o.gloss, -1.0f, 1.0f);
    hold(o.metallic, -1.0f, 1.0f);
    hold(o.bumpStrength, -4.0f, 4.0f);
    hold(o.normalStrength, 0.0f, 4.0f);
    hold(o.displacementStrength, -4.0f, 4.0f);
}

const char* overlayFieldName(OverlayField field) {
    switch (field) {
    case OverlayField::Enabled: return "enabled";
    case OverlayField::Blend: return "blend";
    case OverlayField::Opacity: return "opacity";
    case OverlayField::Brightness: return "brightness";
    case OverlayField::Contrast: return "contrast";
    case OverlayField::Gamma: return "gamma";
    case OverlayField::Hue: return "hue";
    case OverlayField::Saturation: return "saturation";
    case OverlayField::Gloss: return "gloss";
    case OverlayField::BumpStrength: return "bumpstrength";
    case OverlayField::NormalStrength: return "normalstrength";
    case OverlayField::DisplacementStrength: return "displacementstrength";
    case OverlayField::OffsetX: return "offsetx";
    case OverlayField::OffsetY: return "offsety";
    case OverlayField::Rotation: return "rotation";
    case OverlayField::Scale: return "scale";
    case OverlayField::Metallic: return "metallic";
    default: return "";
    }
}

bool overlayFieldByName(const std::string& name, OverlayField& out) {
    for (int i = 0; i < static_cast<int>(OverlayField::Count); ++i) {
        if (name == overlayFieldName(static_cast<OverlayField>(i))) {
            out = static_cast<OverlayField>(i);
            return true;
        }
    }
    return false;
}

bool overlayAffects(const MaterialOverlay& o, MaterialMap map) {
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

std::string overlaySignature(const std::vector<MaterialOverlay>& overlays, MaterialMap map, bool bumpAsNormal) {
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

} // namespace pose
