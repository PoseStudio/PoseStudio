/**
 * @file specgloss.h
 * @brief The specular / glossiness → metallic / roughness conversion every importer applies to a
 *        file authored in the older convention (2026-11-09). Qt-free (GLM + std).
 *
 * PoseStudio has ONE material model — metallic / roughness with a specular weight (mesh.frag:
 * F0 = mix(0.04 × specular, albedo, metalness)) — and the Materials tab is sliders over it. A
 * source that gives a surface a DIFFUSE colour, a SPECULAR colour (its reflectance at normal
 * incidence, F0) and a GLOSSINESS instead — glTF's KHR_materials_pbrSpecularGlossiness, a USD
 * preview surface with useSpecularWorkflow on, a 3ds Max "PBR spec/gloss" material — is converted
 * at import by the standard solve the Khronos glTF tools use: roughness = 1 − glossiness; the
 * metalness is the solution of the quadratic that makes a metallic-roughness material's diffuse
 * and specular come out as the source's, with 4 % as the dielectric baseline; the base colour is
 * the diffuse un-attenuated for a dielectric, the specular colour for a metal, blended by the
 * metalness squared. It is exact for a physically plausible material (a dielectric's specular is
 * a grey near 4 %; a metal's diffuse is black and its specular its colour). What it cannot
 * represent — a tinted specular on a non-metal, a dielectric far brighter than 4 % — is
 * non-physical, and such a material comes out as the nearest metallic-roughness one.
 *
 * convertSpecGloss does the scalar case (the readers call it for an untextured material); the
 * decode layer runs the same solve per texel when the source carries maps (a diffuse map, a
 * specular colour map, a glossiness map), baking a base-colour map and a metalness map —
 * ModelImportService::decodeModelTextures, which has the decoded pixels.
 *
 * A PHONG material's specular colour is NOT a reflectance (Blender exports Ks 0.5 for every
 * default material, Maya 0.5 grey) and is not solved: the readers take only its grey level as
 * the specular weight (phongSpecularWeight) — a black Ks is a matte surface, the default grey the
 * standard dielectric — and leave the metalness at 0.
 */

#ifndef SPECGLOSS_H
#define SPECGLOSS_H

#include <glm/glm.hpp>

namespace pose {

/// A specular / glossiness material's metallic / roughness equivalent.
struct MetallicRoughness {
    glm::vec3 baseColor{1.0f}; ///< Linear.
    float     metalness = 0.0f;
    float     roughness = 0.5f;
};

/// The Khronos solve on LINEAR colours: @p diffuse and @p specular (F0) in 0..1, @p glossiness
/// 0 (rough) .. 1 (mirror).
MetallicRoughness convertSpecGloss(const glm::vec3& diffuse, const glm::vec3& specular, float glossiness);

/// The metalness alone (the per-texel bake's inner loop): the perceived brightness of the linear
/// diffuse and specular colours, and 1 − max(specular).
float solveMetallic(float diffuseBrightness, float specularBrightness, float oneMinusSpecularStrength);

/// Perceived brightness of a linear colour (the Khronos sample's: sqrt of the weighted squares).
float perceivedBrightness(const glm::vec3& linear);

/// A PHONG specular colour × factor read as a specular WEIGHT (1 = the standard 4 % dielectric,
/// the cap): its grey level over 0.5, the grey every modelling package exports for a default
/// material. Black = matte (0).
float phongSpecularWeight(const glm::vec3& specularColor, float factor = 1.0f);

} // namespace pose

#endif // SPECGLOSS_H
