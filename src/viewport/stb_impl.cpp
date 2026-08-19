/**
 * @file stb_impl.cpp
 * @brief The single translation unit that compiles stb_image's implementation.
 *
 * stb_image is header-only: exactly one TU must define STB_IMAGE_IMPLEMENTATION (the same one-TU
 * pattern as vma_impl.cpp for VMA and objimporter.cpp for tinyobjloader). Everyone else includes
 * <stb_image.h> for declarations only. Used by environmentsource.cpp to decode `.hdr` panoramas.
 */

// On Windows, make stbi's file open treat narrow paths as UTF-8 (via _wfopen): the callers hand
// it QString::toStdString() output, which is UTF-8 — without this, an .hdr in a folder with
// non-ASCII characters fails to open on systems whose ANSI code page isn't UTF-8.
#define STBI_WINDOWS_UTF8
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
