/**
 * @file memoryimage.h
 * @brief A Cycles image loader that reads an application image straight out of memory.
 *
 * PoseStudio hands the engine its textures as decoded pixels (renderapi::Image): the maps a
 * figure was imported with, capped and gutter-filled, and the composites its overlays are
 * painted into — images that exist nowhere on disk. Cycles loads images lazily, on its own
 * threads, when the scene is first synchronized: this loader keeps the application's pointer
 * (the render API guarantees it for the length of the render) and copies the rows into the
 * buffer Cycles gives it.
 *
 * Rows are copied as they are — row 0 of the application's image, its TOP, becomes Cycles's row
 * 0, which is v = 0 — so the application's UVs (v down from the top) index the same texels in
 * both. An ENVIRONMENT image is the exception: Cycles's equirectangular mapping reads v = 1 at
 * the zenith, so its rows are flipped (@p flipRows).
 */

#ifndef POSESTUDIO_CYCLES_MEMORYIMAGE_H
#define POSESTUDIO_CYCLES_MEMORYIMAGE_H

#include "renderengineapi.h"

#include "scene/image_loader.h"

namespace posecycles {

class MemoryImageLoader : public ccl::ImageLoader {
public:
    MemoryImageLoader(const pose::renderapi::Image& image, bool flipRows);

    bool load_metadata(ccl::ImageMetaData& metadata, const ccl::ImageLoaderParams& params,
                       ccl::Progress& progress) override;
    bool load_pixels(const ccl::ImageMetaData& metadata, void* pixels) override;
    ccl::string name() const override;
    bool equals(const ccl::ImageLoader& other) const override;

private:
    pose::renderapi::Image m_image; // (the struct is copied; the pixels stay the application's)
    ccl::string            m_name;
    bool                   m_flipRows = false;
};

} // namespace posecycles

#endif // POSESTUDIO_CYCLES_MEMORYIMAGE_H
