// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file shaderlibrary.cpp
 * @brief Implementation of ShaderLibrary. See shaderlibrary.h.
 */

#include "shaderlibrary.h"

#include "vulkancommon.h"

#include <fstream>
#include <utility>

namespace pose {

ShaderLibrary::ShaderLibrary(std::string dir) : m_dir(std::move(dir)) {}

const std::vector<char>& ShaderLibrary::get(const std::string& name) {
    auto it = m_blobs.find(name);
    if (it != m_blobs.end()) {
        return it->second;
    }
    const std::string path = m_dir + "/" + name + ".spv";
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw VulkanError("Could not open SPIR-V file: " + path +
                          " (was the shader-compile build step run?)");
    }
    const std::streamsize size = file.tellg();
    std::vector<char> buffer(static_cast<size_t>(size > 0 ? size : 0));
    file.seekg(0);
    file.read(buffer.data(), size);
    if (buffer.empty() || (buffer.size() % 4) != 0) {
        throw VulkanError("SPIR-V file is empty or not a multiple of 4 bytes: " + path);
    }
    return m_blobs.emplace(name, std::move(buffer)).first->second;
}

void ShaderLibrary::preload() {
    for (const char* name : kShaderNames) {
        get(name);
    }
}

} // namespace pose
