/**
 * @file subdivision.cpp
 * @brief Implementation of subdivideFigure (Catmull-Clark). See subdivision.h.
 */

#include "subdivision.h"

#include "geometryparser.h" // GeometryData

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace pose {

namespace {

// A subdividable cage. Positions + skin weights are per vertex (shared, so subdivided positions stay
// watertight); UVs live per polygon corner so seams split cleanly at assembly.
struct Corner {
    uint32_t  pos = 0; ///< index into CageState::positions / skins
    glm::vec2 uv{0.0f};
};
struct Face {
    int    material = 0;
    int    n = 4; ///< 3 or 4
    Corner c[4];
};
struct CageState {
    std::vector<glm::vec3>   positions;
    std::vector<VertexSkin>  skins; ///< empty for an unrigged cage
    std::vector<Face>        faces;
    std::vector<std::string> zones;
};

// Per-level connectivity: enough to (a) apply Catmull-Clark stencils to any per-vertex vec3 field and
// (b) build the next level's faces. Vertex points keep indices [0,inVerts); edge points are
// [inVerts, inVerts+numEdges); face points are the tail.
struct LevelTopo {
    uint32_t inVerts = 0;
    uint32_t numEdges = 0;
    uint32_t numFaces = 0;
    uint32_t outVerts = 0;

    std::vector<std::array<uint32_t, 4>> faceVerts; ///< input vertex indices per face
    std::vector<int>                     faceN;

    std::vector<std::array<uint32_t, 2>> edgeEnds;  ///< the two endpoints
    std::vector<std::array<int, 2>>      edgeFaces; ///< adjacent face indices (-1 = none)
    std::unordered_map<uint64_t, uint32_t> edgeMap; ///< (min<<32)|max -> edge index

    std::vector<std::vector<uint32_t>> vertFaces; ///< incident faces per vertex
    std::vector<std::vector<uint32_t>> vertEdges; ///< incident edges per vertex
    std::vector<uint8_t>               vertBoundary;
    std::vector<std::array<uint32_t, 2>> vertBoundaryNbr; ///< the two boundary-edge neighbours
};

constexpr uint32_t kNoVert = 0xFFFFFFFFu;

uint64_t edgeKey(uint32_t a, uint32_t b) {
    const uint32_t lo = std::min(a, b);
    const uint32_t hi = std::max(a, b);
    return (static_cast<uint64_t>(lo) << 32) | hi;
}

LevelTopo buildTopo(const CageState& cage) {
    LevelTopo t;
    t.inVerts = static_cast<uint32_t>(cage.positions.size());
    t.numFaces = static_cast<uint32_t>(cage.faces.size());
    t.faceVerts.resize(t.numFaces);
    t.faceN.resize(t.numFaces);

    for (uint32_t f = 0; f < t.numFaces; ++f) {
        const Face& F = cage.faces[f];
        t.faceN[f] = F.n;
        for (int i = 0; i < F.n; ++i) {
            t.faceVerts[f][i] = F.c[i].pos;
        }
        for (int i = 0; i < F.n; ++i) {
            const uint32_t a = F.c[i].pos;
            const uint32_t b = F.c[(i + 1) % F.n].pos;
            const uint64_t k = edgeKey(a, b);
            uint32_t ei;
            const auto it = t.edgeMap.find(k);
            if (it == t.edgeMap.end()) {
                ei = static_cast<uint32_t>(t.edgeEnds.size());
                t.edgeMap.emplace(k, ei);
                t.edgeEnds.push_back({a, b});
                t.edgeFaces.push_back({-1, -1});
            } else {
                ei = it->second;
            }
            if (t.edgeFaces[ei][0] < 0) {
                t.edgeFaces[ei][0] = static_cast<int>(f);
            } else if (t.edgeFaces[ei][1] < 0) {
                t.edgeFaces[ei][1] = static_cast<int>(f);
            }
        }
    }
    t.numEdges = static_cast<uint32_t>(t.edgeEnds.size());
    t.outVerts = t.inVerts + t.numEdges + t.numFaces;

    t.vertFaces.assign(t.inVerts, {});
    t.vertEdges.assign(t.inVerts, {});
    for (uint32_t f = 0; f < t.numFaces; ++f) {
        for (int i = 0; i < cage.faces[f].n; ++i) {
            t.vertFaces[cage.faces[f].c[i].pos].push_back(f);
        }
    }
    for (uint32_t e = 0; e < t.numEdges; ++e) {
        t.vertEdges[t.edgeEnds[e][0]].push_back(e);
        t.vertEdges[t.edgeEnds[e][1]].push_back(e);
    }

    t.vertBoundary.assign(t.inVerts, 0);
    t.vertBoundaryNbr.assign(t.inVerts, {kNoVert, kNoVert});
    for (uint32_t e = 0; e < t.numEdges; ++e) {
        if (t.edgeFaces[e][1] >= 0) {
            continue; // interior edge
        }
        const uint32_t a = t.edgeEnds[e][0];
        const uint32_t b = t.edgeEnds[e][1];
        auto addNbr = [&](uint32_t v, uint32_t nbr) {
            t.vertBoundary[v] = 1;
            if (t.vertBoundaryNbr[v][0] == kNoVert) {
                t.vertBoundaryNbr[v][0] = nbr;
            } else if (t.vertBoundaryNbr[v][1] == kNoVert) {
                t.vertBoundaryNbr[v][1] = nbr;
            }
        };
        addNbr(a, b);
        addNbr(b, a);
    }
    return t;
}

// Applies the Catmull-Clark smooth stencils to a per-vertex vec3 field (positions or a displacement
// field). Output layout matches the topology's vertex-point / edge-point / face-point ordering.
std::vector<glm::vec3> applyField(const LevelTopo& t, const std::vector<glm::vec3>& in) {
    std::vector<glm::vec3> facePoint(t.numFaces, glm::vec3(0.0f));
    std::vector<glm::vec3> out(t.outVerts, glm::vec3(0.0f));

    for (uint32_t f = 0; f < t.numFaces; ++f) {
        glm::vec3 s(0.0f);
        for (int i = 0; i < t.faceN[f]; ++i) {
            s += in[t.faceVerts[f][i]];
        }
        facePoint[f] = s / static_cast<float>(t.faceN[f]);
        out[t.inVerts + t.numEdges + f] = facePoint[f];
    }
    for (uint32_t e = 0; e < t.numEdges; ++e) {
        const glm::vec3 a = in[t.edgeEnds[e][0]];
        const glm::vec3 b = in[t.edgeEnds[e][1]];
        const int f1 = t.edgeFaces[e][0];
        const int f2 = t.edgeFaces[e][1];
        out[t.inVerts + e] = (f2 >= 0) ? (a + b + facePoint[f1] + facePoint[f2]) * 0.25f
                                       : (a + b) * 0.5f; // boundary edge -> midpoint
    }
    for (uint32_t v = 0; v < t.inVerts; ++v) {
        if (t.vertBoundary[v]) {
            const uint32_t n1 = t.vertBoundaryNbr[v][0];
            const uint32_t n2 = t.vertBoundaryNbr[v][1];
            const glm::vec3 a = (n1 != kNoVert) ? in[n1] : in[v];
            const glm::vec3 b = (n2 != kNoVert) ? in[n2] : in[v];
            out[v] = (6.0f * in[v] + a + b) / 8.0f; // cubic B-spline boundary rule
            continue;
        }
        const uint32_t n = static_cast<uint32_t>(t.vertEdges[v].size());
        if (n == 0) {
            out[v] = in[v];
            continue;
        }
        glm::vec3 fBar(0.0f);
        for (const uint32_t f : t.vertFaces[v]) {
            fBar += facePoint[f];
        }
        fBar /= static_cast<float>(std::max<std::size_t>(t.vertFaces[v].size(), 1));
        glm::vec3 rBar(0.0f);
        for (const uint32_t e : t.vertEdges[v]) {
            rBar += (in[t.edgeEnds[e][0]] + in[t.edgeEnds[e][1]]) * 0.5f;
        }
        rBar /= static_cast<float>(n);
        out[v] = (fBar + 2.0f * rBar + static_cast<float>(static_cast<int>(n) - 3) * in[v]) /
                 static_cast<float>(n);
    }
    return out;
}

// Blends skin-weight vectors: accumulate per joint, keep the top 4, renormalize.
VertexSkin blendSkins(const VertexSkin* items, int count) {
    std::unordered_map<int, float> acc;
    for (int i = 0; i < count; ++i) {
        for (int k = 0; k < 4; ++k) {
            const float w = items[i].weights[k];
            if (w > 0.0f) {
                acc[items[i].joints[k]] += w;
            }
        }
    }
    std::vector<std::pair<int, float>> sorted(acc.begin(), acc.end());
    const std::size_t keep = std::min<std::size_t>(4, sorted.size());
    std::partial_sort(sorted.begin(), sorted.begin() + keep, sorted.end(),
                      [](const auto& x, const auto& y) { return x.second > y.second; });
    VertexSkin out;
    out.joints = glm::ivec4(0);
    out.weights = glm::vec4(0.0f);
    float sum = 0.0f;
    for (std::size_t i = 0; i < keep; ++i) {
        out.joints[static_cast<int>(i)] = sorted[i].first;
        out.weights[static_cast<int>(i)] = sorted[i].second;
        sum += sorted[i].second;
    }
    if (sum > 0.0f) {
        out.weights /= sum;
    }
    return out;
}

std::vector<VertexSkin> subdivideWeights(const LevelTopo& t, const std::vector<VertexSkin>& in) {
    if (in.empty()) {
        return {}; // unrigged cage
    }
    std::vector<VertexSkin> out(t.outVerts);
    for (uint32_t f = 0; f < t.numFaces; ++f) {
        VertexSkin src[4];
        for (int i = 0; i < t.faceN[f]; ++i) {
            src[i] = in[t.faceVerts[f][i]];
        }
        out[t.inVerts + t.numEdges + f] = blendSkins(src, t.faceN[f]);
    }
    for (uint32_t e = 0; e < t.numEdges; ++e) {
        const VertexSkin ends[2] = {in[t.edgeEnds[e][0]], in[t.edgeEnds[e][1]]};
        out[t.inVerts + e] = blendSkins(ends, 2);
    }
    for (uint32_t v = 0; v < t.inVerts; ++v) {
        out[v] = in[v]; // vertex points keep their original weights
    }
    return out;
}

std::vector<Face> buildNewFaces(const CageState& cage, const LevelTopo& t) {
    std::vector<Face> out;
    out.reserve(t.numFaces * 4);
    for (uint32_t f = 0; f < t.numFaces; ++f) {
        const Face& F = cage.faces[f];
        glm::vec2 centerUv(0.0f);
        for (int i = 0; i < F.n; ++i) {
            centerUv += F.c[i].uv;
        }
        centerUv /= static_cast<float>(F.n);
        const uint32_t fp = t.inVerts + t.numEdges + f;
        for (int i = 0; i < F.n; ++i) {
            const int ip = (i + 1) % F.n;
            const int im = (i + F.n - 1) % F.n;
            const uint32_t epNext = t.inVerts + t.edgeMap.at(edgeKey(F.c[i].pos, F.c[ip].pos));
            const uint32_t epPrev = t.inVerts + t.edgeMap.at(edgeKey(F.c[im].pos, F.c[i].pos));
            Face q;
            q.material = F.material;
            q.n = 4;
            q.c[0] = {F.c[i].pos, F.c[i].uv};
            q.c[1] = {epNext, (F.c[i].uv + F.c[ip].uv) * 0.5f};
            q.c[2] = {fp, centerUv};
            q.c[3] = {epPrev, (F.c[im].uv + F.c[i].uv) * 0.5f};
            out.push_back(q);
        }
    }
    return out;
}

CageState subdivideCage(const CageState& cage, const LevelTopo& t) {
    CageState nc;
    nc.zones = cage.zones;
    nc.positions = applyField(t, cage.positions);
    nc.skins = subdivideWeights(t, cage.skins);
    nc.faces = buildNewFaces(cage, t);
    return nc;
}

// De-index key: a subdivided-cage vertex split per distinct (quantized) UV, mirroring the base
// assembler's (base vertex, uv index) split at seams.
struct PosUvKey {
    uint32_t pos;
    int32_t  ux;
    int32_t  uy;
    bool operator==(const PosUvKey& o) const { return pos == o.pos && ux == o.ux && uy == o.uy; }
};
struct PosUvHash {
    std::size_t operator()(const PosUvKey& k) const {
        std::size_t h = k.pos * 73856093u;
        h ^= static_cast<uint32_t>(k.ux) * 19349663u;
        h ^= static_cast<uint32_t>(k.uy) * 83492791u;
        return h;
    }
};

std::vector<FigureMesh> assembleCage(const CageState& cage) {
    const std::size_t vcount = cage.positions.size();

    // Smooth normals over the whole surface (all zones) so shading is seamless across zone boundaries.
    std::vector<glm::vec3> normals(vcount, glm::vec3(0.0f));
    auto accTri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (a >= vcount || b >= vcount || c >= vcount) {
            return;
        }
        const glm::vec3 fn =
            glm::cross(cage.positions[b] - cage.positions[a], cage.positions[c] - cage.positions[a]);
        normals[a] += fn;
        normals[b] += fn;
        normals[c] += fn;
    };
    for (const Face& f : cage.faces) {
        accTri(f.c[0].pos, f.c[1].pos, f.c[2].pos);
        if (f.n == 4) {
            accTri(f.c[0].pos, f.c[2].pos, f.c[3].pos);
        }
    }
    for (glm::vec3& n : normals) {
        const float len = glm::length(n);
        n = (len > 1e-8f) ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }

    const std::size_t zoneCount = std::max<std::size_t>(cage.zones.size(), 1);
    std::vector<FigureMesh> meshes(zoneCount);
    for (std::size_t z = 0; z < zoneCount; ++z) {
        meshes[z].materialIndex = static_cast<int>(z);
        meshes[z].materialZone = (z < cage.zones.size()) ? cage.zones[z] : std::string();
    }
    std::vector<std::unordered_map<PosUvKey, uint32_t, PosUvHash>> caches(zoneCount);

    auto emit = [&](std::size_t zone, const Corner& corner) -> uint32_t {
        constexpr float kUvQuant = 65536.0f;
        const PosUvKey key{corner.pos, static_cast<int32_t>(std::lround(corner.uv.x * kUvQuant)),
                           static_cast<int32_t>(std::lround(corner.uv.y * kUvQuant))};
        auto& cache = caches[zone];
        if (const auto it = cache.find(key); it != cache.end()) {
            return it->second;
        }
        FigureMesh& mesh = meshes[zone];
        Vertex vert{};
        vert.pos = (corner.pos < vcount) ? cage.positions[corner.pos] : glm::vec3(0.0f);
        vert.normal = (corner.pos < vcount) ? normals[corner.pos] : glm::vec3(0.0f, 1.0f, 0.0f);
        vert.uv = glm::vec2(corner.uv.x, 1.0f - corner.uv.y); // flip V (Vulkan top-left origin)
        if (corner.pos < cage.skins.size()) {
            vert.joints = glm::uvec4(cage.skins[corner.pos].joints);
            vert.weights = cage.skins[corner.pos].weights;
        }
        const uint32_t local = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(vert);
        mesh.baseVertex.push_back(corner.pos);
        cache.emplace(key, local);
        return local;
    };

    for (const Face& f : cage.faces) {
        const std::size_t zone =
            (f.material >= 0 && f.material < static_cast<int>(zoneCount)) ? static_cast<std::size_t>(f.material) : 0;
        FigureMesh& mesh = meshes[zone];
        auto tri = [&](int a, int b, int c) {
            mesh.indices.push_back(emit(zone, f.c[a]));
            mesh.indices.push_back(emit(zone, f.c[b]));
            mesh.indices.push_back(emit(zone, f.c[c]));
        };
        tri(0, 1, 2);
        if (f.n == 4) {
            tri(0, 2, 3);
        }
    }
    return meshes;
}

CageState buildBaseCage(const GeometryData& geo, const UvSet& uv, const std::vector<VertexSkin>& skins) {
    CageState cage;
    cage.positions = geo.positions;
    cage.zones = geo.materialZones;
    cage.skins = skins;
    if (!cage.skins.empty() && cage.skins.size() < cage.positions.size()) {
        cage.skins.resize(cage.positions.size()); // pad with zero-weight so indexing is safe
    }
    cage.faces.reserve(geo.faces.size());
    for (std::size_t p = 0; p < geo.faces.size(); ++p) {
        const GeometryData::Face& gf = geo.faces[p];
        Face f;
        f.material = gf.material;
        f.n = gf.count;
        for (int i = 0; i < gf.count; ++i) {
            f.c[i].pos = gf.v[i];
            if (!uv.empty()) {
                const uint32_t uvIdx = uv.uvIndexFor(static_cast<uint32_t>(p), gf.v[i]);
                f.c[i].uv = (uvIdx < uv.uvs.size()) ? uv.uvs[uvIdx] : glm::vec2(0.0f);
            }
        }
        cage.faces.push_back(f);
    }
    return cage;
}

} // namespace

SubdivisionResult subdivideFigure(const GeometryData& geo, const UvSet& uv,
                                  const std::vector<VertexSkin>& skins, int levels) {
    CageState cage = buildBaseCage(geo, uv, skins);

    auto topos = std::make_shared<std::vector<LevelTopo>>();
    for (int l = 0; l < levels; ++l) {
        LevelTopo topo = buildTopo(cage);
        cage = subdivideCage(cage, topo);
        topos->push_back(std::move(topo));
    }

    SubdivisionResult result;
    result.vertexCount = cage.positions.size();
    result.meshes = assembleCage(cage);
    result.subdivideDeltaField = [topos](const std::vector<glm::vec3>& base) {
        std::vector<glm::vec3> field = base;
        for (const LevelTopo& t : *topos) {
            field = applyField(t, field);
        }
        return field;
    };
    return result;
}

} // namespace pose
