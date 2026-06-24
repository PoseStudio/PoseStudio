/**
 * @file vulkanimage.h
 * @brief A sampled RGBA8 texture: device-local image + view + sampler, with a full mip chain.
 *
 * Takes already-decoded, tightly-packed RGBA8 pixels (the Qt layer decodes image files via
 * QImage and hands the raw bytes down — keeping rendering/ free of any image-codec dependency).
 * Move-only RAII. Pixels are treated as sRGB albedo (the view uses an _SRGB format so the
 * hardware linearises on sample). Qt-free.
 */

#ifndef VULKANIMAGE_H
#define VULKANIMAGE_H

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstdint>

namespace pose {

class VulkanContext;

/**
 * @class VulkanTexture
 * @brief Owns one sampled image (image + allocation + view + sampler). Move-only.
 */
class VulkanTexture {
public:
    VulkanTexture() = default;
    /// @param pixels Tightly packed RGBA8, exactly width*height*4 bytes.
    VulkanTexture(VulkanContext& context, const uint8_t* pixels, uint32_t width, uint32_t height);
    ~VulkanTexture();

    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;
    VulkanTexture(VulkanTexture&& other) noexcept;
    VulkanTexture& operator=(VulkanTexture&& other) noexcept;

    VkImageView imageView() const { return m_view; }
    VkSampler   sampler()   const { return m_sampler; }

private:
    void destroy();

    VulkanContext* m_context    = nullptr;
    VmaAllocator   m_allocator  = VK_NULL_HANDLE; // borrowed from the context
    VkImage        m_image      = VK_NULL_HANDLE;
    VmaAllocation  m_allocation = VK_NULL_HANDLE;
    VkImageView    m_view       = VK_NULL_HANDLE;
    VkSampler      m_sampler    = VK_NULL_HANDLE;
    uint32_t       m_mipLevels  = 1;
};

} // namespace pose

#endif // VULKANIMAGE_H
