/**
 * @file textureuploadcache.h
 * @brief Per-model dedup of texture uploads: one VulkanTexture per unique decoded image.
 *
 * Figure zones commonly sample the same atlas files (face/lips/ears share the face maps; torso,
 * arms and legs the body maps), and the decode layer already hands every referencing mesh the
 * SAME shared DecodedImage. This cache keys the GPU upload on that image's identity so a shared
 * image is uploaded ONCE per model — one staging copy + mip chain instead of one per zone, which
 * multiplied import time and GPU memory by the sharing factor. It is keyed separately per color
 * space: the same image could in principle feed both an sRGB slot and a LINEAR slot, which need
 * distinct VkImages. The cache lives only for the duration of a Model's constructor; the meshes
 * keep the textures alive through their shared_ptrs afterwards. Qt-free.
 */

#ifndef TEXTUREUPLOADCACHE_H
#define TEXTUREUPLOADCACHE_H

#include <memory>
#include <unordered_map>

namespace pose {

class ImmediateBatch;
class VulkanContext;
class VulkanTexture;
struct DecodedImage;

struct TextureUploadCache {
    std::unordered_map<const DecodedImage*, std::shared_ptr<VulkanTexture>> srgb;
    std::unordered_map<const DecodedImage*, std::shared_ptr<VulkanTexture>> linear;
};

/// Returns the (shared) texture for @p image, uploading it into @p batch only on first use in
/// the requested color space. A null or empty image yields null (the caller binds the
/// appropriate 1x1 fallback).
std::shared_ptr<VulkanTexture> uploadShared(VulkanContext& context, TextureUploadCache& cache,
                                            const std::shared_ptr<const DecodedImage>& image,
                                            bool srgbFormat, ImmediateBatch& batch);

} // namespace pose

#endif // TEXTUREUPLOADCACHE_H
