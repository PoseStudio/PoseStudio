// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file environmentsource.cpp
 * @brief Implementation of loadEnvironmentImage. See environmentsource.h.
 */

#include "environmentsource.h"

#include "parallelfor.h"

#include <stb_image.h>
#include <tinyexr.h> // declarations only; the implementation TU is tinyexr_impl.cpp

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace pose {

namespace {

// EnvironmentImage stores glm::vec3 pixels; the decoders hand back a packed float array. The
// copies below rely on a vec3 being exactly three contiguous floats.
static_assert(sizeof(glm::vec3) == 3 * sizeof(float), "glm::vec3 must be three packed floats");

// Radiance .hdr via stb_image (RGBE-decoded to linear float radiance).
std::optional<EnvironmentImage> loadWithStb(const std::string& path) {
    int width = 0, height = 0, channels = 0;
    // Force 3 channels (RGB); stbi_loadf returns linear float radiance for .hdr.
    float* data = stbi_loadf(path.c_str(), &width, &height, &channels, 3);
    if (data == nullptr || width <= 0 || height <= 0) {
        if (data != nullptr) {
            stbi_image_free(data);
        }
        return std::nullopt;
    }

    EnvironmentImage env;
    env.width = width;
    env.height = height;
    env.pixels.resize(static_cast<std::size_t>(width) * height);
    // Packed RGB floats in, packed vec3s out: one copy.
    std::memcpy(env.pixels.data(), data, env.pixels.size() * sizeof(glm::vec3));
    stbi_image_free(data);
    return env;
}

// OpenEXR .exr via tinyexr's one-shot loader: first (single) image part, half/uint channels
// converted to float, always RGBA out — alpha is dropped, the IBL bake wants radiance only.
std::optional<EnvironmentImage> loadWithTinyExr(const std::string& path) {
    float* rgba = nullptr;
    int width = 0, height = 0;
    const char* err = nullptr;
    const int ret = LoadEXR(&rgba, &width, &height, path.c_str(), &err);
    if (err != nullptr) {
        FreeEXRErrorMessage(err); // only pass/fail matters here (the caller logs the path)
    }
    if (ret != TINYEXR_SUCCESS || rgba == nullptr || width <= 0 || height <= 0) {
        std::free(rgba); // null-safe; LoadEXR allocates with malloc
        return std::nullopt;
    }

    EnvironmentImage env;
    env.width = width;
    env.height = height;
    env.pixels.resize(static_cast<std::size_t>(width) * height);
    // RGBA -> RGB is a stride change, so it can't be one memcpy; a panorama is millions of pixels,
    // so the rows are split across cores.
    const std::size_t w = static_cast<std::size_t>(width);
    parallelFor(height, [&](int y) {
        const float* src = rgba + static_cast<std::size_t>(y) * w * 4;
        glm::vec3*   dst = env.pixels.data() + static_cast<std::size_t>(y) * w;
        for (std::size_t x = 0; x < w; ++x) {
            dst[x] = glm::vec3(src[x * 4 + 0], src[x * 4 + 1], src[x * 4 + 2]);
        }
    });
    std::free(rgba);
    return env;
}

// Lower-cased extension (without the dot) of @p path; "" when there is none. find_last_of('.')
// can land on a dot in a DIRECTORY name for an extension-less file ("~/my.hdris/studio") and
// return that tail — harmless: nothing matches "exr", so the file falls to stb, which rejects
// it on its own if it isn't a Radiance file.
std::string lowerExtension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

std::optional<EnvironmentImage> loadEnvironmentImage(const std::string& path) {
    if (lowerExtension(path) == "exr") {
        return loadWithTinyExr(path);
    }
    return loadWithStb(path); // .hdr (and anything else stb_image can float-decode)
}

} // namespace pose
