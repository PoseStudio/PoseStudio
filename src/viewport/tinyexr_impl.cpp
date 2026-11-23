// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file tinyexr_impl.cpp
 * @brief The single translation unit that compiles tinyexr's implementation.
 *
 * tinyexr is header-only: exactly one TU must define TINYEXR_IMPLEMENTATION (the same one-TU
 * pattern as stb_impl.cpp for stb_image and vma_impl.cpp for VMA). Everyone else includes
 * <tinyexr.h> for declarations only. Used by environmentsource.cpp to decode `.exr` panoramas.
 *
 * TINYEXR_USE_MINIZ (tinyexr's default) routes its ZIP inflate through <miniz.h> — satisfied by
 * the same miniz objects the figure importer inflates its files with (the build's
 * posestudio_miniz library, in CMakeLists.txt), so EXR support adds no zlib dependency.
 */

#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>
