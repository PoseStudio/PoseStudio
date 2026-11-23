// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file textureuploadcache.cpp
 * @brief The shared texture upload. See textureuploadcache.h.
 */

#include "textureuploadcache.h"

#include "modeldata.h"
#include "vulkanimage.h"

namespace pose {

std::shared_ptr<VulkanTexture> uploadShared(VulkanContext& context, TextureUploadCache& cache,
                                            const std::shared_ptr<const DecodedImage>& image,
                                            bool srgbFormat, ImmediateBatch& batch) {
    if (!image || image->width == 0 || image->height == 0 || image->pixels.empty()) {
        return nullptr;
    }
    auto& map = srgbFormat ? cache.srgb : cache.linear;
    const auto it = map.find(image.get());
    if (it != map.end()) {
        return it->second;
    }
    auto texture = std::make_shared<VulkanTexture>(context, image->pixels.data(), image->width,
                                                   image->height, batch, srgbFormat);
    map.emplace(image.get(), texture);
    return texture;
}

} // namespace pose
