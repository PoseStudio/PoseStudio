/**
 * @file ibldata.h
 * @brief The CPU-baked image-based-lighting products the GPU layer uploads: the prefiltered
 *        specular cubemap and the environment-BRDF LUT.
 *
 * These two PODs are the data contract between the scene-side bakes (scene/environment.h, which
 * computes them) and rendering/iblmaps.h (which uploads them into the mesh pipeline's IBL set).
 * They live here, in rendering/, so the GPU layer never has to include a scene header: rendering/
 * sits BELOW scene/ in the engine's layering (only VulkanRenderer, the frame orchestrator, looks
 * upward). Pure GLM + std, no Vulkan, no Qt.
 */

#ifndef IBLDATA_H
#define IBLDATA_H

#include <glm/glm.hpp>

#include <vector>

namespace pose {

/// A GGX-prefiltered specular cubemap, baked on the CPU: `faces[face][mip]` holds the RGBA texels of
/// one cube face at one mip. Mip 0 is a sharp reflection (roughness 0); each successive mip is
/// prefiltered for a higher roughness (mip/(mipCount-1)), so the shader samples mip = roughness ·
/// (mipCount-1) to get a roughness-appropriate environment reflection (the specular half of split-sum).
struct PrefilteredSpecular {
    int                                              baseSize = 0;
    int                                              mipCount = 0;
    std::vector<std::vector<std::vector<glm::vec4>>> faces; // [6][mip][texel], row-major per face
};

/// The environment BRDF integration LUT (the other half of split-sum): at (NdotV, roughness) it stores
/// the GGX scale (.x) and bias (.y) to apply to F0, and (.z) the SHEEN lobe's directional albedo —
/// how much a fuzz of that roughness reflects at that angle (the Charlie distribution with Neubelt's
/// visibility), the mesh shader's environment sheen term and the energy the base loses under it;
/// .w is unused. Environment-independent, so it's baked once. `data` is row-major, size×size,
/// y = roughness, x = NdotV.
struct BrdfLut {
    int                    size = 0;
    std::vector<glm::vec4> data;
};

} // namespace pose

#endif // IBLDATA_H
