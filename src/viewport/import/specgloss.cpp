// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file specgloss.cpp
 * @brief The specular / glossiness → metallic / roughness solve. See specgloss.h.
 */

#include "specgloss.h"

#include <algorithm>
#include <cmath>

namespace pose {

namespace {
constexpr float kDielectricSpecular = 0.04f; // a dielectric's reflectance at normal incidence
constexpr float kEpsilon = 1.0e-4f;

/// Perceived brightness of a linear color (the Khronos sample's: sqrt of the weighted squares).
float perceivedBrightness(const glm::vec3& c) {
    return std::sqrt(0.299f * c.r * c.r + 0.587f * c.g * c.g + 0.114f * c.b * c.b);
}

/// The metalness alone: from the perceived brightness of the linear diffuse and specular colors,
/// and 1 − max(specular).
float solveMetallic(float diffuseBrightness, float specularBrightness, float oneMinusSpecularStrength) {
    if (specularBrightness < kDielectricSpecular) {
        return 0.0f;
    }
    // A metallic-roughness material's specular is mix(0.04, base, m) and its diffuse base × (1 − m):
    // with the source's diffuse and specular given, m solves a quadratic in which the base color
    // has been eliminated (the Khronos glTF sample's derivation).
    const float a = kDielectricSpecular;
    const float b = diffuseBrightness * oneMinusSpecularStrength / (1.0f - kDielectricSpecular) + specularBrightness -
                    2.0f * kDielectricSpecular;
    const float c = kDielectricSpecular - specularBrightness;
    const float d = std::max(b * b - 4.0f * a * c, 0.0f);
    return std::clamp((-b + std::sqrt(d)) / (2.0f * a), 0.0f, 1.0f);
}

} // namespace

MetallicRoughness convertSpecGloss(const glm::vec3& diffuse, const glm::vec3& specular, float glossiness) {
    MetallicRoughness out;
    const glm::vec3 d = glm::clamp(diffuse, 0.0f, 1.0f);
    const glm::vec3 s = glm::clamp(specular, 0.0f, 1.0f);
    const float     oneMinusSpecularStrength = 1.0f - std::max(s.r, std::max(s.g, s.b));
    const float     metallic = solveMetallic(perceivedBrightness(d), perceivedBrightness(s), oneMinusSpecularStrength);
    const glm::vec3 baseFromDiffuse =
        d * (oneMinusSpecularStrength / (1.0f - kDielectricSpecular) / std::max(1.0f - metallic, kEpsilon));
    const glm::vec3 baseFromSpecular = (s - glm::vec3(kDielectricSpecular) * (1.0f - metallic)) * (1.0f / std::max(metallic, kEpsilon));
    out.baseColor = glm::clamp(glm::mix(baseFromDiffuse, baseFromSpecular, metallic * metallic), 0.0f, 1.0f);
    out.metalness = metallic;
    out.roughness = std::clamp(1.0f - glossiness, 0.0f, 1.0f);
    return out;
}

float phongSpecularWeight(const glm::vec3& specularColor, float factor) {
    const float grey = perceivedBrightness(glm::clamp(specularColor, 0.0f, 1.0f)) * std::max(factor, 0.0f);
    return std::clamp(grey / 0.5f, 0.0f, 1.0f);
}

} // namespace pose
