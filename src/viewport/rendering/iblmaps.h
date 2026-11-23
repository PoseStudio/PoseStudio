// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file iblmaps.h
 * @brief The GPU-resident image-based-lighting maps: a prefiltered specular cubemap and the
 *        environment-BRDF LUT (the two textures the split-sum specular term samples).
 *
 * The CPU bakes (scene/environment.h) produce the pixel data as the PODs in ibldata.h; this owns
 * the Vulkan images/views and hands out descriptor infos for the mesh pipeline's IBL set (set 3).
 * The diffuse half of the IBL is carried as SH coefficients in the camera UBO, so no irradiance
 * cubemap is needed here. The BRDF LUT is environment-independent (baked once); the specular
 * cubemap is rebuilt when the environment changes. Samplers are borrowed from the context's
 * SamplerCache. Non-copyable — Scene owns one via unique_ptr. Pure Vulkan + std, no Qt.
 */

#ifndef IBLMAPS_H
#define IBLMAPS_H

#include "vulkanhandles.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace pose {

class VulkanContext;
struct PrefilteredSpecular;
struct BrdfLut;

class IblMaps {
public:
    /// Uploads @p specular as an RGBA16F cubemap (mip = roughness) and @p lut as an RGBA16F 2D
    /// texture (red/green the split-sum scale and bias, blue the sheen lobe's directional albedo).
    IblMaps(VulkanContext& context, const PrefilteredSpecular& specular, const BrdfLut& lut);
    ~IblMaps();

    IblMaps(const IblMaps&) = delete;
    IblMaps& operator=(const IblMaps&) = delete;

    /// Combined-image-sampler descriptor infos for set 3 (binding 0 = specular cube, 1 = BRDF LUT).
    VkDescriptorImageInfo specularInfo() const;
    VkDescriptorImageInfo brdfInfo() const;

private:
    /// Frees both images (and their views). Idempotent — also the constructor's throw path.
    void destroyImages();

    VulkanContext&  m_context;
    VkImage         m_specImage   = VK_NULL_HANDLE;
    VmaAllocation   m_specAlloc   = VK_NULL_HANDLE;
    UniqueImageView m_specView;
    VkSampler       m_specSampler = VK_NULL_HANDLE; // borrowed (SamplerCache)
    VkImage         m_lutImage    = VK_NULL_HANDLE;
    VmaAllocation   m_lutAlloc    = VK_NULL_HANDLE;
    UniqueImageView m_lutView;
    VkSampler       m_lutSampler  = VK_NULL_HANDLE; // borrowed (SamplerCache)
};

} // namespace pose

#endif // IBLMAPS_H
