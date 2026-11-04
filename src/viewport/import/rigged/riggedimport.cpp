/**
 * @file riggedimport.cpp
 * @brief What the rigged-character readers share (see riggedimport.h). The facing / centre /
 *        floor step and the texture finder were the FBX reader's and moved here unchanged when
 *        the glTF and USD readers came (2026-11-04): the FBX reader's output is bit-identical.
 */

#include "riggedimport.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>

namespace pose {

namespace fs = std::filesystem;

bool isImageFile(const fs::path& p) {
    static const char* const kExt[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".tif", ".tiff", ".webp", ".exr", ".hdr", ".dds", ".psd", ".gif"};
    const std::string ext = lowerCopy(p.extension().string());
    for (const char* e : kExt) {
        if (ext == e) {
            return true;
        }
    }
    return false;
}

std::string lowerCopy(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

float srgbToLinear(float c) {
    c = std::max(0.0f, std::min(1.0f, c));
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

void fillMissingNormals(MeshData& mesh) {
    std::vector<char> missing(mesh.vertices.size(), 0);
    bool              any = false;
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        missing[i] = glm::dot(mesh.vertices[i].normal, mesh.vertices[i].normal) < 1e-12f ? 1 : 0;
        any = any || missing[i];
    }
    if (!any) {
        return;
    }
    std::vector<glm::vec3> accum(mesh.vertices.size(), glm::vec3(0.0f));
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t  i0 = mesh.indices[i];
        const uint32_t  i1 = mesh.indices[i + 1];
        const uint32_t  i2 = mesh.indices[i + 2];
        const glm::vec3 faceNormal = glm::cross(mesh.vertices[i1].pos - mesh.vertices[i0].pos, mesh.vertices[i2].pos - mesh.vertices[i0].pos);
        accum[i0] += faceNormal;
        accum[i1] += faceNormal;
        accum[i2] += faceNormal;
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (!missing[i]) {
            continue;
        }
        const float len = glm::length(accum[i]);
        mesh.vertices[i].normal = len > 1e-8f ? accum[i] / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

// ---- TextureFinder ----------------------------------------------------------------------------

TextureSource TextureFinder::resolve(const std::vector<std::string>& candidates, RiggedImportInfo* info, const std::string& shown) {
    TextureSource   out;
    std::error_code ec;
    for (const std::string& c : candidates) {
        if (c.empty()) {
            continue;
        }
        const fs::path p(c);
        if (p.is_absolute() && fs::is_regular_file(p, ec)) {
            return found(p, info);
        }
        const fs::path rel = m_dir / p;
        if (fs::is_regular_file(rel, ec)) {
            return found(rel, info);
        }
        // (... or against the folder ABOVE the file's: exports keep "textures/" beside a
        // "formats/" folder, and write the path relative to the project.)
        const fs::path above = m_dir.parent_path() / p;
        if (!m_dir.parent_path().empty() && fs::is_regular_file(above, ec)) {
            return found(above, info);
        }
    }
    for (const std::string& c : candidates) {
        if (c.empty()) {
            continue;
        }
        const std::string name = lowerCopy(fs::path(c).filename().string());
        if (name.empty()) {
            continue;
        }
        index();
        const auto it = m_byName.find(name);
        if (it != m_byName.end()) {
            return found(it->second, info);
        }
    }
    if (info != nullptr) {
        ++info->missingTextures;
        if (m_missingNoted.insert(shown).second && m_missingNoted.size() <= 8) {
            info->notes.push_back("texture not found: " + shown);
        }
    }
    return out;
}

TextureSource TextureFinder::found(const fs::path& p, RiggedImportInfo* info) {
    TextureSource out;
    out.path = p.string();
    if (info != nullptr) {
        ++info->externalTextures;
    }
    return out;
}

void TextureFinder::index() {
    if (m_indexed) {
        return;
    }
    m_indexed = true;
    std::error_code ec;
    // The file's own folder, deep (an export's texture tree is a few folders deep), then the
    // folder above it, shallower (a "textures/" beside a "formats/"; a library root is not what
    // sits beside a character — the walk is capped either way).
    const fs::path above = m_dir.parent_path();
    const struct { fs::path dir; int depth; } roots[] = {{m_dir, 8}, {above, 3}};
    std::size_t files = 0;
    for (const auto& r : roots) {
        if (r.dir.empty() || !fs::is_directory(r.dir, ec)) {
            continue;
        }
        fs::recursive_directory_iterator       it(r.dir, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; it != end && files < 20000; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (it.depth() > r.depth) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec) || !isImageFile(it->path())) {
                continue;
            }
            ++files;
            m_byName.emplace(lowerCopy(it->path().filename().string()), it->path()); // (the first found wins: the file's own folder)
        }
    }
}

// ---- finishRiggedModel ------------------------------------------------------------------------

void finishRiggedModel(ModelData& model, std::vector<RawSkeletonBone>& bones, RiggedImportInfo* info) {
    const int boneCount = static_cast<int>(bones.size());

    // ---- FACING, CENTRE AND FLOOR. The figure faces +z with her left at +x, stands over the origin,
    // and her lowest point rests on y = 0 — what every reader of the bind pose takes for granted.
    float yawDeg = 0.0f;
    if (boneCount >= 3) {
        float     minY = std::numeric_limits<float>::max();
        float     maxY = -std::numeric_limits<float>::max();
        glm::vec3 mean(0.0f);
        for (const RawSkeletonBone& b : bones) {
            minY = std::min(minY, b.position.y);
            maxY = std::max(maxY, b.position.y);
            mean += b.position;
        }
        mean /= static_cast<float>(boneCount);
        const float height = maxY - minY;
        // THE LATERAL AXIS is the skeleton's widest horizontal spread (a standing figure is wider
        // than deep, arms out or down), read off the bone positions' covariance in the floor plane
        // — exact to a fraction of a degree where the toes alone (splayed, few) are not; the toes
        // give the SIGN: they point forward.
        float sxx = 0.0f, sxz = 0.0f, szz = 0.0f;
        for (const RawSkeletonBone& b : bones) {
            const glm::vec3& p = b.position;
            sxx += (p.x - mean.x) * (p.x - mean.x);
            sxz += (p.x - mean.x) * (p.z - mean.z);
            szz += (p.z - mean.z) * (p.z - mean.z);
        }
        const float lateralAngle = 0.5f * std::atan2(2.0f * sxz, sxx - szz); // the major axis, from +x toward +z
        glm::vec3   lateral(std::cos(lateralAngle), 0.0f, std::sin(lateralAngle));
        glm::vec3   forward = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), lateral); // one of the two perpendiculars
        glm::vec3   toes(0.0f);
        if (height > 0.2f) {
            const float lowY = minY + 0.12f * height;
            for (int i = 0; i < boneCount; ++i) {
                const int p = bones[static_cast<std::size_t>(i)].parent;
                if (p < 0) {
                    continue;
                }
                const glm::vec3& a = bones[static_cast<std::size_t>(p)].position;
                const glm::vec3& b = bones[static_cast<std::size_t>(i)].position;
                if (a.y < lowY && b.y < lowY) {
                    toes += glm::vec3(b.x - a.x, 0.0f, b.z - a.z);
                }
            }
        }
        const float spread = std::abs(sxx - szz) + std::abs(sxz);
        if (spread > 1.0e-4f && glm::length(toes) > 0.01f * std::max(height, 0.2f)) {
            if (glm::dot(forward, toes) < 0.0f) {
                forward = -forward;
            }
            yawDeg = glm::degrees(std::atan2(forward.x, forward.z));
        } else if (glm::length(toes) > 0.01f * std::max(height, 0.2f)) {
            yawDeg = glm::degrees(std::atan2(toes.x, toes.z)); // (no clear spread: the toes alone)
        } else if (info != nullptr) {
            info->notes.push_back("the figure's facing could not be read off the skeleton: assumed to face +z");
        }
    }
    if (std::abs(yawDeg) > 0.5f) {
        const glm::mat4 turn = glm::rotate(glm::mat4(1.0f), glm::radians(-yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat3 turn3(turn);
        for (RawSkeletonBone& b : bones) {
            b.position = turn3 * b.position;
        }
        for (RawSkeletonBone& b : bones) {
            b.tip = turn3 * b.tip;
        }
        for (MeshData& mesh : model.meshes) {
            for (Vertex& v : mesh.vertices) {
                v.pos = turn3 * v.pos;
                v.normal = turn3 * v.normal;
            }
        }
    }
    glm::vec3 centre(0.0f);
    if (boneCount > 0) {
        // Over the HIPS: the first bone (parents first) with two children or more — the body's
        // midline runs through it, and the engine measures the sides from it.
        std::vector<int> childCount(bones.size(), 0);
        for (int i = 0; i < boneCount; ++i) {
            const int p = bones[static_cast<std::size_t>(i)].parent;
            if (p >= 0) {
                ++childCount[static_cast<std::size_t>(p)];
            }
        }
        int hips = -1;
        for (int i = 0; i < boneCount && hips < 0; ++i) {
            if (childCount[static_cast<std::size_t>(i)] >= 2) {
                hips = i;
            }
        }
        if (hips >= 0) {
            centre = bones[static_cast<std::size_t>(hips)].position;
        } else {
            for (const RawSkeletonBone& b : bones) {
                centre += b.position;
            }
            centre /= static_cast<float>(boneCount);
        }
    } else {
        std::size_t count = 0;
        for (const MeshData& mesh : model.meshes) {
            for (const Vertex& v : mesh.vertices) {
                centre += v.pos;
                ++count;
            }
        }
        centre /= static_cast<float>(std::max<std::size_t>(1, count));
    }
    float lowest = std::numeric_limits<float>::max();
    float highest = -std::numeric_limits<float>::max();
    for (const MeshData& mesh : model.meshes) {
        for (const Vertex& v : mesh.vertices) {
            lowest = std::min(lowest, v.pos.y);
            highest = std::max(highest, v.pos.y);
        }
    }
    const glm::vec3 shift(-centre.x, -lowest, -centre.z);
    for (MeshData& mesh : model.meshes) {
        for (Vertex& v : mesh.vertices) {
            v.pos += shift;
        }
    }
    for (RawSkeletonBone& b : bones) {
        b.position += shift;
    }
    for (RawSkeletonBone& b : bones) {
        b.tip += shift;
    }

    // ---- THE BONES, raw: names, hierarchy, bind positions. Frames and limits are the conformer's.
    model.bones.clear();
    model.bones.reserve(bones.size());
    std::unordered_set<std::string> usedNames;
    for (int i = 0; i < boneCount; ++i) {
        const RawSkeletonBone& raw = bones[static_cast<std::size_t>(i)];
        ModelBone              bone;
        bone.name = raw.name;
        if (bone.name.empty()) {
            bone.name = "bone_" + std::to_string(i);
        }
        for (int suffix = 2; !usedNames.insert(bone.name).second; ++suffix) {
            bone.name = raw.name + "_" + std::to_string(suffix);
        }
        bone.parent = raw.parent;
        bone.bindGlobal = glm::translate(glm::mat4(1.0f), raw.position);
        bone.rotationOrder = "XYZ";
        bone.tip = raw.tip;
        bone.hasTip = raw.hasTip;
        bone.roleHint = raw.roleHint;
        model.bones.push_back(std::move(bone));
    }

    if (info != nullptr) {
        info->bones = boneCount;
        info->heightMetres = highest - lowest;
        info->yawDegrees = yawDeg;
        info->groundedMm = -lowest * 1000.0f;
        if (info->heightMetres < 1.2f || info->heightMetres > 2.4f) {
            char line[160];
            std::snprintf(line, sizeof(line), "the figure stands %.2f m tall after the unit conversion: an unusual height for a human",
                          static_cast<double>(info->heightMetres));
            info->notes.push_back(line);
        }
    }
}

} // namespace pose
