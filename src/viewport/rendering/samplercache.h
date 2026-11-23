// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file samplercache.h
 * @brief Shared, device-lifetime VkSamplers keyed by their creation parameters.
 *
 * A sampler describes HOW to filter, not WHAT — yet every VulkanTexture used to create its own
 * identical repeat/linear/mipmapped sampler (a figure imports ~40 unique maps, and every further
 * model adds more, against drivers whose maxSamplerAllocationCount is 4000), and four more
 * identical clamp/linear samplers lived in the HDR target, the outline mask, the post chain and
 * the IBL maps. The cache hands out one VkSampler per distinct SamplerDesc, owned by the
 * VulkanContext for the device's lifetime; textures and targets borrow the handle and own only
 * their image + view. Handles returned by get() must never be destroyed by the caller.
 *
 * Mipmapped presets use VK_LOD_CLAMP_NONE rather than the image's own level count: the sampled
 * level is clamped to the VIEW's mip range after the sampler's LOD clamp (Vulkan spec, image
 * level selection), so one sampler serves every mip count with bit-identical results.
 *
 * Pure Vulkan + std, no Qt. Not thread-safe (GUI-thread use only, like the rest of the engine).
 */

#ifndef SAMPLERCACHE_H
#define SAMPLERCACHE_H

#include "vulkanhandles.h"

#include <vulkan/vulkan.h>

#include <utility>
#include <vector>

namespace pose {

/// The sampler parameters the engine varies. Everything else (anisotropy off — the feature isn't
/// enabled on the device — unnormalized coordinates off, zero LOD bias) is fixed.
struct SamplerDesc {
    VkFilter             filter = VK_FILTER_LINEAR;                      // mag + min
    VkSamplerMipmapMode  mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;   // irrelevant at maxLod 0
    VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // all three axes
    VkBorderColor        borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    bool                 compare = false;
    VkCompareOp          compareOp = VK_COMPARE_OP_NEVER;
    float                maxLod = 0.0f;

    bool operator==(const SamplerDesc& o) const {
        return filter == o.filter && mipmapMode == o.mipmapMode && addressMode == o.addressMode &&
               borderColor == o.borderColor && compare == o.compare && compareOp == o.compareOp &&
               maxLod == o.maxLod;
    }

    // --- The engine's presets -----------------------------------------------------------------
    /// Linear, clamp-to-edge, no mips: the screen-sized targets the post passes sample (HDR
    /// resolve, outline mask, bloom/SSS) and the single-level BRDF LUT.
    static SamplerDesc linearClamp();
    /// Linear + trilinear mips, REPEAT: model textures (UV-wrapping material maps).
    static SamplerDesc linearRepeatMipmapped();
    /// Linear + trilinear mips, clamp-to-edge: the prefiltered specular cubemap (mip = roughness).
    static SamplerDesc linearClampMipmapped();
    /// The shadow map's comparison sampler: linear (hardware PCF), clamp-to-border OPAQUE WHITE so
    /// receivers outside the fitted light frustum read fully lit, compare LESS_OR_EQUAL.
    static SamplerDesc shadowCompare();
    /// The shadow map's raw-depth twin: NEAREST (averaging depths across a caster's silhouette
    /// would invent phantom blocker distances), same white border, no compare.
    static SamplerDesc shadowRaw();
};

/**
 * @class SamplerCache
 * @brief Creates each distinct sampler once and keeps it until the cache is destroyed.
 */
class SamplerCache {
public:
    explicit SamplerCache(VkDevice device) : m_device(device) {}

    SamplerCache(const SamplerCache&) = delete;
    SamplerCache& operator=(const SamplerCache&) = delete;

    /// The shared sampler for @p desc (created on first request). Borrowed — never destroy it.
    VkSampler get(const SamplerDesc& desc);

private:
    VkDevice                                          m_device;
    std::vector<std::pair<SamplerDesc, UniqueSampler>> m_samplers; // a handful of entries: linear scan
};

} // namespace pose

#endif // SAMPLERCACHE_H
