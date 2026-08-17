/**
 * @file environmentsource.cpp
 * @brief Implementation of loadEnvironmentImage. See environmentsource.h.
 */

#include "environmentsource.h"

#include <stb_image.h>
#include <tinyexr.h> // declarations only; the implementation TU is tinyexr_impl.cpp

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>

namespace pose {

namespace {

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
    for (std::size_t i = 0; i < env.pixels.size(); ++i) {
        env.pixels[i] = glm::vec3(data[i * 3 + 0], data[i * 3 + 1], data[i * 3 + 2]);
    }
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
    for (std::size_t i = 0; i < env.pixels.size(); ++i) {
        env.pixels[i] = glm::vec3(rgba[i * 4 + 0], rgba[i * 4 + 1], rgba[i * 4 + 2]);
    }
    std::free(rgba);
    return env;
}

// Lower-cased extension (without the dot) of @p path; "" when there is none.
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
