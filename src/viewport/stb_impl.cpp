/**
 * @file stb_impl.cpp
 * @brief The single translation unit that compiles stb_image's implementation.
 *
 * stb_image is header-only: exactly one TU must define STB_IMAGE_IMPLEMENTATION (the same one-TU
 * pattern as vma_impl.cpp for VMA and objimporter.cpp for tinyobjloader). Everyone else includes
 * <stb_image.h> for declarations only. Used by environmentsource.cpp to decode `.hdr` panoramas.
 */

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
