/**
 * @file environmentsource.cpp
 * @brief Implementation of loadHdrEnvironment. See environmentsource.h.
 */

#include "environmentsource.h"

#include <stb_image.h>

#include <cstddef>

namespace pose {

std::optional<EnvironmentImage> loadHdrEnvironment(const std::string& path) {
    int width = 0, height = 0, channels = 0;
    // Force 3 channels (RGB); stbi_loadf returns linear float radiance for .hdr (RGBE-decoded).
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

} // namespace pose
