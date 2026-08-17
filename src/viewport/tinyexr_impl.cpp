/**
 * @file tinyexr_impl.cpp
 * @brief The single translation unit that compiles tinyexr's implementation.
 *
 * tinyexr is header-only: exactly one TU must define TINYEXR_IMPLEMENTATION (the same one-TU
 * pattern as stb_impl.cpp for stb_image and vma_impl.cpp for VMA). Everyone else includes
 * <tinyexr.h> for declarations only. Used by environmentsource.cpp to decode `.exr` panoramas.
 *
 * TINYEXR_USE_MINIZ (tinyexr's default) routes its ZIP inflate through <miniz.h> — satisfied by
 * the same miniz sources the figure importer already compiles into the target (see the
 * target_sources block in CMakeLists.txt), so EXR support adds no zlib dependency.
 */

#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>
