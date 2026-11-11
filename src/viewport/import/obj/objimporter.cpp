/**
 * @file objimporter.cpp
 * @brief OBJ/MTL parsing via tinyobjloader. See objimporter.h.
 *
 * This is the project's single tinyobjloader translation unit: it defines
 * TINYOBJLOADER_IMPLEMENTATION before the header (the same pattern vma_impl.cpp uses for VMA).
 */

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>
#include "specgloss.h"
#include <algorithm>
#include <cmath>

#include "objimporter.h"

#include <glm/glm.hpp>

#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace pose {

namespace {

// Identity of a wavefront face-vertex: indices into the position / normal / texcoord pools.
// Two face-vertices with the same triple share one de-indexed Vertex.
struct IndexKey {
    int v;
    int n;
    int t;
    bool operator==(const IndexKey& o) const { return v == o.v && n == o.n && t == o.t; }
};

struct IndexKeyHash {
    std::size_t operator()(const IndexKey& k) const {
        std::size_t h = std::hash<int>()(k.v);
        h ^= std::hash<int>()(k.n) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(k.t) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// Resolves a (possibly relative) MTL texture name against the .obj's directory. Returns an empty
// string for an empty name. Absolute paths (drive-letter or leading slash) are kept as-is.
std::string resolveTexturePath(const std::string& baseDir, const std::string& texName) {
    // Trim surrounding whitespace and a single pair of quotes. Some exporters quote map_Kd paths
    // that contain spaces, and tinyobjloader passes the quotes through verbatim.
    std::string name = texName;
    const std::size_t first = name.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return std::string();
    }
    name = name.substr(first, name.find_last_not_of(" \t") - first + 1);
    if (name.size() >= 2 && ((name.front() == '"' && name.back() == '"') ||
                             (name.front() == '\'' && name.back() == '\''))) {
        name = name.substr(1, name.size() - 2);
    }
    if (name.empty()) {
        return std::string();
    }

    const bool absolute = (name.size() > 1 && name[1] == ':') || // C:\... or C:/...
                          name[0] == '/' || name[0] == '\\';
    if (absolute || baseDir.empty()) {
        return name;
    }
    return baseDir + "/" + name;
}

// Recomputes smooth per-vertex normals by accumulating triangle face normals (used when the OBJ
// carries no normals of its own). Leaves degenerate triangles out of the average.
void generateSmoothNormals(MeshData& mesh) {
    for (Vertex& vert : mesh.vertices) {
        vert.normal = glm::vec3(0.0f);
    }
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t i0 = mesh.indices[i];
        const uint32_t i1 = mesh.indices[i + 1];
        const uint32_t i2 = mesh.indices[i + 2];
        const glm::vec3& p0 = mesh.vertices[i0].pos;
        const glm::vec3& p1 = mesh.vertices[i1].pos;
        const glm::vec3& p2 = mesh.vertices[i2].pos;
        const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
        mesh.vertices[i0].normal += faceNormal;
        mesh.vertices[i1].normal += faceNormal;
        mesh.vertices[i2].normal += faceNormal;
    }
    for (Vertex& vert : mesh.vertices) {
        const float len = glm::length(vert.normal);
        vert.normal = (len > 1e-8f) ? vert.normal / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

// Fills normals ONLY for vertices that didn't get one from the file, keeping authored normals
// untouched. A legal OBJ can mix `f v//n` and plain `f v` faces (common when exports are
// concatenated); the plain faces' vertices would otherwise stay zero-initialized, and
// normalize(0) is NaN in the shader.
void fillMissingNormals(MeshData& mesh) {
    std::vector<bool> missing(mesh.vertices.size());
    bool any = false;
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        missing[i] = glm::dot(mesh.vertices[i].normal, mesh.vertices[i].normal) < 1e-12f;
        any = any || missing[i];
    }
    if (!any) {
        return;
    }
    std::vector<glm::vec3> accum(mesh.vertices.size(), glm::vec3(0.0f));
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t i0 = mesh.indices[i];
        const uint32_t i1 = mesh.indices[i + 1];
        const uint32_t i2 = mesh.indices[i + 2];
        const glm::vec3 faceNormal = glm::cross(mesh.vertices[i1].pos - mesh.vertices[i0].pos,
                                                mesh.vertices[i2].pos - mesh.vertices[i0].pos);
        accum[i0] += faceNormal;
        accum[i1] += faceNormal;
        accum[i2] += faceNormal;
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (!missing[i]) {
            continue;
        }
        const float len = glm::length(accum[i]);
        mesh.vertices[i].normal = (len > 1e-8f) ? accum[i] / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

} // namespace

ModelData ObjImporter::load(const std::string& path) const {
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    // Resolve the .mtl (and its relative texture paths) next to the .obj.
    const std::size_t slash = path.find_last_of("/\\");
    config.mtl_search_path = (slash == std::string::npos) ? std::string() : path.substr(0, slash);

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, config)) {
        std::string msg = "Failed to load OBJ: " + path;
        if (!reader.Error().empty()) {
            msg += "\n" + reader.Error();
        }
        throw std::runtime_error(msg);
    }

    const tinyobj::attrib_t& attrib = reader.GetAttrib();
    const std::vector<tinyobj::shape_t>& shapes = reader.GetShapes();
    const std::vector<tinyobj::material_t>& materials = reader.GetMaterials();
    const bool fileHasNormals = !attrib.normals.empty();

    // Group faces by material id across all shapes, de-indexing as we go. A material id of -1
    // (faces with no material) collapses into one default group.
    std::unordered_map<int, std::size_t> materialToMesh; // material id -> index into result.meshes
    std::vector<std::unordered_map<IndexKey, uint32_t, IndexKeyHash>> vertexCaches;
    ModelData result;

    auto meshForMaterial = [&](int materialId) -> std::size_t {
        auto it = materialToMesh.find(materialId);
        if (it != materialToMesh.end()) {
            return it->second;
        }
        const std::size_t meshIndex = result.meshes.size();
        materialToMesh.emplace(materialId, meshIndex);
        MeshData mesh;
        if (materialId >= 0 && materialId < static_cast<int>(materials.size())) {
            const tinyobj::material_t& mat = materials[materialId];
            mesh.name = mat.name; // the Materials tab's zone name
            mesh.baseColor = glm::vec3(mat.diffuse[0], mat.diffuse[1], mat.diffuse[2]);
            mesh.diffuse.path = resolveTexturePath(config.mtl_search_path, mat.diffuse_texname);
            // THE PBR EXTENSION of the MTL format (Pr / Pm / map_Pr / map_Pm / norm — what Blender
            // and most current exporters write), and the classic channels read into the one
            // material model (specgloss.h): Ks is a PHONG specular, whose grey level is the
            // specular weight (Blender's default 0.5 = the standard dielectric; black = matte; an
            // absent Ks reads black in the parser and keeps the default), map_Ks the specular mask,
            // Ns the glossiness in Blender's own convention (Ns = (1 − roughness)² × 1000, the
            // most common source of an MTL today; 3ds Max's default 96 lands at 0.69, the old
            // default) where no Pr is given, d / map_d the opacity, map_bump / bump a height map
            // with its -bm multiplier.
            const bool hasPr = mat.roughness > 0.0f || !mat.roughness_texname.empty();
            if (hasPr) {
                mesh.roughness = std::clamp(mat.roughness > 0.0f ? mat.roughness : 1.0f, 0.05f, 1.0f);
                mesh.roughnessMap.path = resolveTexturePath(config.mtl_search_path, mat.roughness_texname);
            } else if (mat.shininess > 1.0f) { // (the parser's default for an absent Ns is 1: no authored value reads as the old default roughness)
                mesh.roughness = std::clamp(1.0f - std::sqrt(std::clamp(mat.shininess / 1000.0f, 0.0f, 1.0f)), 0.05f, 1.0f);
            }
            mesh.lobe1Roughness = mesh.roughness;
            mesh.lobe2Roughness = mesh.roughness;
            if (mat.metallic > 0.0f || !mat.metallic_texname.empty()) {
                mesh.metalness = std::clamp(mat.metallic > 0.0f ? mat.metallic : 1.0f, 0.0f, 1.0f);
                mesh.metalnessMap.path = resolveTexturePath(config.mtl_search_path, mat.metallic_texname);
            }
            const glm::vec3 ks(mat.specular[0], mat.specular[1], mat.specular[2]);
            if (ks.r + ks.g + ks.b > 0.0f) {
                mesh.specularWeight = phongSpecularWeight(ks);
                mesh.specularWeightWithMap = mesh.specularWeight;
            }
            mesh.specMask.path = resolveTexturePath(config.mtl_search_path, mat.specular_texname);
            if (!mat.normal_texname.empty()) {
                mesh.normal.path = resolveTexturePath(config.mtl_search_path, mat.normal_texname);
                mesh.normalMode = mesh.normal.path.empty() ? 0 : 1;
            } else if (!mat.bump_texname.empty()) {
                mesh.normal.path = resolveTexturePath(config.mtl_search_path, mat.bump_texname);
            }
            if (!mat.displacement_texname.empty()) {
                mesh.displacementMap.path = resolveTexturePath(config.mtl_search_path, mat.displacement_texname); // disp (stored-only)
                mesh.normalMode = mesh.normal.path.empty() ? 0 : 2;
                mesh.normalStrength = std::clamp(mat.bump_texopt.bump_multiplier, 0.1f, 8.0f);
            }
            if (mat.dissolve > 0.0f && mat.dissolve < 1.0f) {
                mesh.opacity = mat.dissolve;
            }
            mesh.opacityMask.path = resolveTexturePath(config.mtl_search_path, mat.alpha_texname);
            // THE CANONICAL MATERIAL's further channels: Ke / map_Ke the emission, Ni the index of
            // refraction, Pc / Pcr the clearcoat, Ps the sheen, aniso / anisor the anisotropy, Tf the
            // transmission filter (a tint; the MTL has no weight to go with it).
            {
                const glm::vec3 ke(mat.emission[0], mat.emission[1], mat.emission[2]);
                const float     peak = std::max(ke.r, std::max(ke.g, ke.b));
                if (peak > 0.0f || !mat.emissive_texname.empty()) {
                    mesh.emissionColor = peak > 0.0f ? ke / peak : glm::vec3(1.0f);
                    mesh.emission = peak > 0.0f ? peak : 1.0f;
                    mesh.emissionMap.path = resolveTexturePath(config.mtl_search_path, mat.emissive_texname);
                }
            }
            if (mat.ior > 1.0f) {
                mesh.ior = std::clamp(mat.ior, 1.0f, 3.0f);
            }
            if (mat.clearcoat_thickness > 0.0f) {
                mesh.topCoatWeight = std::clamp(mat.clearcoat_thickness, 0.0f, 1.0f);
                mesh.topCoatRoughness = std::clamp(mat.clearcoat_roughness, 0.0f, 1.0f);
            }
            if (mat.sheen > 0.0f) {
                mesh.sheen = std::clamp(mat.sheen, 0.0f, 1.0f);
            }
            if (mat.anisotropy > 0.0f) {
                mesh.anisotropy = std::clamp(mat.anisotropy, 0.0f, 1.0f);
                mesh.anisotropyRotation = std::clamp(mat.anisotropy_rotation, 0.0f, 1.0f) * 360.0f; // (a turn)
            }
            {
                const glm::vec3 tf(mat.transmittance[0], mat.transmittance[1], mat.transmittance[2]);
                if (tf != glm::vec3(1.0f) && tf != glm::vec3(0.0f)) {
                    mesh.transmissionColor = tf;
                }
            }
        }
        result.meshes.push_back(std::move(mesh));
        vertexCaches.emplace_back();
        return meshIndex;
    };

    // Pool sizes for index validation: tinyobjloader reports out-of-range indices only as a
    // *warning* (ParseFromFile still succeeds), so `f 999999 ...` over an 8-vertex pool would
    // otherwise read far out of bounds below. Faces with a bad position index are skipped;
    // bad normal/texcoord indices degrade to "absent".
    const int vertexPool = static_cast<int>(attrib.vertices.size() / 3);
    const int normalPool = static_cast<int>(attrib.normals.size() / 3);
    const int texcoordPool = static_cast<int>(attrib.texcoords.size() / 2);

    for (const tinyobj::shape_t& shape : shapes) {
        std::size_t indexOffset = 0;
        for (std::size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            const int faceVertices = shape.mesh.num_face_vertices[f]; // 3 after triangulation
            const int materialId = shape.mesh.material_ids.empty() ? -1 : shape.mesh.material_ids[f];

            bool faceValid = true;
            for (int v = 0; v < faceVertices; ++v) {
                const int vi = shape.mesh.indices[indexOffset + v].vertex_index;
                if (vi < 0 || vi >= vertexPool) {
                    faceValid = false;
                    break;
                }
            }
            if (!faceValid) {
                indexOffset += faceVertices;
                continue;
            }

            const std::size_t meshIndex = meshForMaterial(materialId);
            MeshData& mesh = result.meshes[meshIndex];
            auto& cache = vertexCaches[meshIndex];

            for (int v = 0; v < faceVertices; ++v) {
                const tinyobj::index_t idx = shape.mesh.indices[indexOffset + v];
                const IndexKey key{idx.vertex_index, idx.normal_index, idx.texcoord_index};

                auto cached = cache.find(key);
                if (cached != cache.end()) {
                    mesh.indices.push_back(cached->second);
                    continue;
                }

                Vertex vertex{};
                vertex.pos = glm::vec3(attrib.vertices[3 * idx.vertex_index + 0],
                                       attrib.vertices[3 * idx.vertex_index + 1],
                                       attrib.vertices[3 * idx.vertex_index + 2]);
                if (idx.normal_index >= 0 && idx.normal_index < normalPool) {
                    vertex.normal = glm::vec3(attrib.normals[3 * idx.normal_index + 0],
                                              attrib.normals[3 * idx.normal_index + 1],
                                              attrib.normals[3 * idx.normal_index + 2]);
                }
                if (idx.texcoord_index >= 0 && idx.texcoord_index < texcoordPool) {
                    // OBJ's V origin is bottom-left; flip to Vulkan's top-left UV convention.
                    vertex.uv = glm::vec2(attrib.texcoords[2 * idx.texcoord_index + 0],
                                          1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]);
                }

                const uint32_t newIndex = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(vertex);
                mesh.indices.push_back(newIndex);
                cache.emplace(key, newIndex);
            }
            indexOffset += faceVertices;
        }
    }

    if (!fileHasNormals) {
        for (MeshData& mesh : result.meshes) {
            generateSmoothNormals(mesh);
        }
    } else {
        // The file has SOME normals, but individual faces may still omit them (mixed `v//n` and
        // plain `v` faces are legal) — fill in just the vertices that got none.
        for (MeshData& mesh : result.meshes) {
            fillMissingNormals(mesh);
        }
    }

    return result;
}

} // namespace pose
