/**
 * @file memoryimage.cpp
 * @brief The in-memory image loader. See memoryimage.h.
 */

#include "memoryimage.h"

#include "util/image_metadata.h"

#include <cstring>

namespace posecycles {

using pose::renderapi::PixelFormat;

MemoryImageLoader::MemoryImageLoader(const pose::renderapi::Image& image, bool flipRows)
    : m_image(image), m_name(image.name != nullptr ? image.name : "image"), m_flipRows(flipRows) {
    m_image.name = nullptr; // (the copy in m_name is the one that lives)
}

bool MemoryImageLoader::load_metadata(ccl::ImageMetaData& metadata, const ccl::ImageLoaderParams& /*params*/,
                                      ccl::Progress& /*progress*/) {
    if (m_image.pixels == nullptr || m_image.width <= 0 || m_image.height <= 0 || m_image.channels < 1 ||
        m_image.channels > 4) {
        return false;
    }
    metadata.width = m_image.width;
    metadata.height = m_image.height;
    metadata.channels = m_image.channels;
    const bool single = (m_image.channels == 1);
    if (m_image.format == PixelFormat::Float32) {
        metadata.type = single ? ccl::IMAGE_DATA_TYPE_FLOAT : ccl::IMAGE_DATA_TYPE_FLOAT4;
    } else {
        metadata.type = single ? ccl::IMAGE_DATA_TYPE_BYTE : ccl::IMAGE_DATA_TYPE_BYTE4;
    }
    // (The color space is the ImageParams' the handle was added with: the image manager sets it
    // on the metadata before this call and finalizes it after.)
    return true;
}

bool MemoryImageLoader::load_pixels(const ccl::ImageMetaData& metadata, void* pixels) {
    if (m_image.pixels == nullptr) {
        return false;
    }
    // The buffer is sized for the CONFORMED image (one or four channels); the source is written
    // at its own channel count, tightly packed, and conform_pixels() expands it in place — the
    // layout Cycles's own file loader produces.
    const std::size_t texel = (m_image.format == PixelFormat::Float32 ? sizeof(float) : 1u) *
                              static_cast<std::size_t>(m_image.channels);
    const std::size_t rowBytes = texel * static_cast<std::size_t>(m_image.width);
    const auto*       src = static_cast<const unsigned char*>(m_image.pixels);
    auto*             dst = static_cast<unsigned char*>(pixels);
    if (!m_flipRows) {
        std::memcpy(dst, src, rowBytes * static_cast<std::size_t>(m_image.height));
    } else {
        for (int y = 0; y < m_image.height; ++y) {
            std::memcpy(dst + rowBytes * static_cast<std::size_t>(y),
                        src + rowBytes * static_cast<std::size_t>(m_image.height - 1 - y), rowBytes);
        }
    }
    metadata.conform_pixels(pixels);
    return true;
}

ccl::string MemoryImageLoader::name() const {
    return m_name;
}

bool MemoryImageLoader::equals(const ccl::ImageLoader& other) const {
    // (ImageLoader::equals(a, b) has already matched the dynamic types.)
    const auto& o = static_cast<const MemoryImageLoader&>(other);
    return m_image.id == o.m_image.id && m_image.pixels == o.m_image.pixels && m_flipRows == o.m_flipRows;
}

} // namespace posecycles
