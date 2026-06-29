/**
 * @file objloader.cpp
 * @brief OBJ/MTL parsing via tinyobjloader. See objloader.h.
 *
 * This is the project's single tinyobjloader translation unit: it defines
 * TINYOBJLOADER_IMPLEMENTATION before the header (the same pattern vma_impl.cpp uses for VMA).
 */

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include "objloader.h"

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

} // namespace

ModelData loadObj(const std::string& path) {
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
            mesh.baseColor = glm::vec3(mat.diffuse[0], mat.diffuse[1], mat.diffuse[2]);
            mesh.diffuseTexturePath = resolveTexturePath(config.mtl_search_path, mat.diffuse_texname);
        }
        result.meshes.push_back(std::move(mesh));
        vertexCaches.emplace_back();
        return meshIndex;
    };

    for (const tinyobj::shape_t& shape : shapes) {
        std::size_t indexOffset = 0;
        for (std::size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            const int faceVertices = shape.mesh.num_face_vertices[f]; // 3 after triangulation
            const int materialId = shape.mesh.material_ids.empty() ? -1 : shape.mesh.material_ids[f];
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
                if (fileHasNormals && idx.normal_index >= 0) {
                    vertex.normal = glm::vec3(attrib.normals[3 * idx.normal_index + 0],
                                              attrib.normals[3 * idx.normal_index + 1],
                                              attrib.normals[3 * idx.normal_index + 2]);
                }
                if (idx.texcoord_index >= 0) {
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
    }

    return result;
}

} // namespace pose
