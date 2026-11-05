/**
 * @file shaderlibrary.h
 * @brief Loads the compiled SPIR-V shaders by name from the build's shader directory.
 *
 * The renderer's constructor used to open each *.spv file inline, so the set of shaders the
 * engine needs was spread over a dozen loadSpirv() calls and mirrored by SHADER_SOURCES in
 * CMakeLists.txt. The library owns the reading (a shader is loaded once and cached; a missing
 * file is a clear VulkanError naming the path and the likely cause) and kShaderNames is the one
 * in-code list of what the build must produce — keep it in step with SHADER_SOURCES.
 *
 * Takes a plain std::string directory, not a Qt path: locating the shader folder is the app's
 * concern (VulkanWindow resolves <applicationDirPath>/shaders), the engine core stays Qt-free.
 */

#ifndef SHADERLIBRARY_H
#define SHADERLIBRARY_H

#include <string>
#include <unordered_map>
#include <vector>

namespace pose {

/// Every shader the engine loads, by source name (the .spv suffix is implied). Mirrors the
/// SHADER_SOURCES list in CMakeLists.txt; ShaderLibrary::preload() reads them all.
inline constexpr const char* kShaderNames[] = {
    "grid.vert",        "grid.frag",       "mesh.vert",       "mesh.frag",
    "skeleton.vert",    "skeleton.frag",   "shadow.vert",     "shadow.frag",
    "background.vert",  "background.frag", "fullscreen.vert", "bloombright.frag",
    "bloomblur.frag",   "composite.frag",  "sssblur.frag",    "outlinemask.frag",
    "depthresolve.frag", "splat.vert",      "splat.frag",
};

/**
 * @class ShaderLibrary
 * @brief Lazily loads + caches SPIR-V blobs from one directory.
 */
class ShaderLibrary {
public:
    /// @param dir Directory holding the compiled <name>.spv files (see CMake's shader step).
    explicit ShaderLibrary(std::string dir);

    /// The SPIR-V for shader @p name (e.g. "mesh.frag"), read from <dir>/<name>.spv on first
    /// use. Throws VulkanError if the file is missing or empty. The reference stays valid for the
    /// library's lifetime.
    const std::vector<char>& get(const std::string& name);

    /// Reads every shader in kShaderNames now, so a missing file fails at startup with one clear
    /// message rather than partway through pipeline construction.
    void preload();

private:
    std::string                                         m_dir;
    std::unordered_map<std::string, std::vector<char>>  m_blobs;
};

} // namespace pose

#endif // SHADERLIBRARY_H
