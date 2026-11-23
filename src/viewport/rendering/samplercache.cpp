// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file samplercache.cpp
 * @brief Implementation of SamplerCache and the SamplerDesc presets. See samplercache.h.
 */

#include "samplercache.h"

#include "vulkancommon.h"

namespace pose {

SamplerDesc SamplerDesc::linearClamp() {
    return SamplerDesc{};
}

SamplerDesc SamplerDesc::linearRepeatMipmapped() {
    SamplerDesc d;
    d.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    d.addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    d.maxLod = VK_LOD_CLAMP_NONE;
    return d;
}

SamplerDesc SamplerDesc::linearClampMipmapped() {
    SamplerDesc d;
    d.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    d.maxLod = VK_LOD_CLAMP_NONE;
    return d;
}

SamplerDesc SamplerDesc::shadowCompare() {
    SamplerDesc d;
    d.addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    d.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    d.compare = true;
    d.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    return d;
}

SamplerDesc SamplerDesc::shadowRaw() {
    SamplerDesc d;
    d.filter = VK_FILTER_NEAREST;
    d.addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    d.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    return d;
}

VkSampler SamplerCache::get(const SamplerDesc& desc) {
    for (const auto& entry : m_samplers) {
        if (entry.first == desc) {
            return entry.second.get();
        }
    }
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = desc.filter;
    info.minFilter = desc.filter;
    info.mipmapMode = desc.mipmapMode;
    info.addressModeU = desc.addressMode;
    info.addressModeV = desc.addressMode;
    info.addressModeW = desc.addressMode;
    info.anisotropyEnable = VK_FALSE; // the samplerAnisotropy feature isn't enabled on the device
    info.compareEnable = desc.compare ? VK_TRUE : VK_FALSE;
    info.compareOp = desc.compareOp;
    info.minLod = 0.0f;
    info.maxLod = desc.maxLod;
    info.borderColor = desc.borderColor;
    VkSampler sampler = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(m_device, &info, nullptr, &sampler));
    m_samplers.emplace_back(desc, UniqueSampler(m_device, sampler));
    return sampler;
}

} // namespace pose
