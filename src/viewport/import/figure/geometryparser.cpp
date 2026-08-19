/**
 * @file geometryparser.cpp
 * @brief Implementation of parseGeometry + assembleFigureMeshes. See geometryparser.h.
 */

#include "geometryparser.h"

#include "uvparser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>

namespace pose {

namespace {
// Some figure-format arrays are {"count":N,"values":[...]}, others are a bare array. Return elements.
const nlohmann::json& valuesArray(const nlohmann::json& node) {
    if (node.is_object()) {
        const auto it = node.find("values");
        if (it != node.end()) {
            return *it;
        }
    }
    return node;
}
} // namespace

GeometryData parseGeometry(const nlohmann::json& g) {
    GeometryData out;

    const auto vertsIt = g.find("vertices");
    const auto polyIt = g.find("polylist");
    if (vertsIt == g.end() || polyIt == g.end()) {
        throw std::runtime_error("figure geometry: missing 'vertices' or 'polylist'");
    }

    const nlohmann::json& verts = valuesArray(*vertsIt);
    out.positions.reserve(verts.size());
    for (const auto& v : verts) {
        // Validate before indexing: nlohmann's const operator[] past the end (or on a non-array)
        // is undefined behavior in release builds — a corrupt file must fail cleanly, not crash.
        if (!v.is_array() || v.size() < 3) {
            throw std::runtime_error("figure geometry: malformed vertex entry");
        }
        out.positions.emplace_back(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
    }

    if (const auto zonesIt = g.find("polygon_material_groups"); zonesIt != g.end()) {
        for (const auto& s : valuesArray(*zonesIt)) {
            out.materialZones.push_back(s.get<std::string>());
        }
    }

    const nlohmann::json& polys = valuesArray(*polyIt);
    const auto vertCount = static_cast<uint32_t>(out.positions.size());
    out.faces.reserve(polys.size());
    for (const auto& p : polys) {
        // p = [groupIndex, materialIndex, v0, v1, v2, (v3)] — 5 entries for a tri, 6 for a quad.
        const int n = p.is_array() ? static_cast<int>(p.size()) - 2 : 0;
        if (n < 3 || n > 4) {
            continue; // skip degenerate/unsupported polygons
        }
        GeometryData::Face f{};
        f.material = p[1].get<int>();
        f.count = n;
        bool valid = true;
        for (int i = 0; i < n; ++i) {
            f.v[i] = p[2 + i].get<uint32_t>();
            // Reject out-of-range vertex indices HERE, not downstream: the subdivision cage
            // builder indexes per-vertex adjacency vectors with these values, so a hostile or
            // corrupt file would otherwise write out of bounds (the assembler merely clamps).
            if (f.v[i] >= vertCount) valid = false;
        }
        if (!valid) continue;
        out.faces.push_back(f);
    }

    if (const auto uvIt = g.find("default_uv_set"); uvIt != g.end()) {
        // Usually a string; tolerate an array form by taking its first element.
        if (uvIt->is_string()) {
            out.defaultUvSetUri = uvIt->get<std::string>();
        } else if (uvIt->is_array() && !uvIt->empty() && uvIt->front().is_string()) {
            out.defaultUvSetUri = uvIt->front().get<std::string>();
        }
    }

    return out;
}

std::vector<FigureMesh> assembleFigureMeshes(const GeometryData& geo, const UvSet& uv,
                                             const std::vector<VertexSkin>& skins) {
    const std::size_t vertCount = geo.positions.size();

    // 1. Smooth normals over the WHOLE surface (all zones), so shading is seamless across zone
    //    boundaries: a vertex shared by two zones accumulates faces from both before splitting.
    std::vector<glm::vec3> normals(vertCount, glm::vec3(0.0f));
    auto accumulateTri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (a >= vertCount || b >= vertCount || c >= vertCount) {
            return;
        }
        const glm::vec3 fn =
            glm::cross(geo.positions[b] - geo.positions[a], geo.positions[c] - geo.positions[a]);
        normals[a] += fn;
        normals[b] += fn;
        normals[c] += fn;
    };
    for (const GeometryData::Face& f : geo.faces) {
        accumulateTri(f.v[0], f.v[1], f.v[2]);
        if (f.count == 4) {
            accumulateTri(f.v[0], f.v[2], f.v[3]);
        }
    }
    for (glm::vec3& n : normals) {
        const float len = glm::length(n);
        n = (len > 1e-8f) ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }

    // 2. One mesh per material zone (at least one, so untagged geometry has a home). Empty zones are
    //    kept so a mesh's index lines up with the material-zone / material list.
    const std::size_t zoneCount = std::max<std::size_t>(geo.materialZones.size(), 1);
    std::vector<FigureMesh> meshes(zoneCount);
    for (std::size_t z = 0; z < zoneCount; ++z) {
        meshes[z].materialIndex = static_cast<int>(z);
        meshes[z].materialZone = (z < geo.materialZones.size()) ? geo.materialZones[z] : std::string();
    }
    // Per-zone de-index cache: (baseVertex, uvIndex) -> local vertex index within that zone's mesh.
    std::vector<std::unordered_map<uint64_t, uint32_t>> caches(zoneCount);

    auto emit = [&](std::size_t zone, uint32_t polygonIndex, uint32_t vIdx) -> uint32_t {
        if (vIdx >= vertCount) {
            vIdx = 0; // guard against malformed indices
        }
        const uint32_t uvIdx = uv.empty() ? vIdx : uv.uvIndexFor(polygonIndex, vIdx);
        const uint64_t key = (static_cast<uint64_t>(vIdx) << 32) | uvIdx;

        std::unordered_map<uint64_t, uint32_t>& cache = caches[zone];
        FigureMesh& mesh = meshes[zone];
        if (const auto it = cache.find(key); it != cache.end()) {
            return it->second;
        }

        Vertex vert{};
        vert.pos = geo.positions[vIdx];
        vert.normal = normals[vIdx];
        if (!uv.empty() && uvIdx < uv.uvs.size()) {
            const glm::vec2 t = uv.uvs[uvIdx];
            vert.uv = glm::vec2(t.x, 1.0f - t.y); // flip V for Vulkan's top-left UV origin
        } else {
            vert.uv = glm::vec2(0.0f);
        }
        if (vIdx < skins.size()) {
            vert.joints = glm::uvec4(skins[vIdx].joints); // ivec4 -> uvec4 (indices are non-negative)
            vert.weights = skins[vIdx].weights;
        }

        const uint32_t local = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(vert);
        mesh.baseVertex.push_back(vIdx); // remember the source base vertex (for pose correctives)
        cache.emplace(key, local);
        return local;
    };

    for (std::size_t p = 0; p < geo.faces.size(); ++p) {
        const GeometryData::Face& f = geo.faces[p];
        const std::size_t zone =
            (f.material >= 0 && f.material < static_cast<int>(zoneCount)) ? static_cast<std::size_t>(f.material) : 0;
        FigureMesh& mesh = meshes[zone];
        const auto poly = static_cast<uint32_t>(p);

        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
            mesh.indices.push_back(emit(zone, poly, a));
            mesh.indices.push_back(emit(zone, poly, b));
            mesh.indices.push_back(emit(zone, poly, c));
        };
        tri(f.v[0], f.v[1], f.v[2]);
        if (f.count == 4) {
            tri(f.v[0], f.v[2], f.v[3]);
        }
    }

    return meshes;
}

} // namespace pose
