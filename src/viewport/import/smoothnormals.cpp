/**
 * @file smoothnormals.cpp
 * @brief Implementation of the smoothing by angle. See smoothnormals.h.
 */

#include "smoothnormals.h"

#include "modeldata.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace pose {

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;

/// Past this many vertices in the meshes it would smooth, the import leaves a model as its file
/// has it (a scan of several million faces: the file's geometry is kept beside the smoothed one).
constexpr std::size_t kImportSmoothingVertexCap = 4000000;

/// A corner's file normal this close to its face's (about a degree) is the face's.
constexpr float kFaceNormalCos = 0.9998f;

uint32_t bitsOf(float f) {
    if (f == 0.0f) {
        f = 0.0f; // (-0 is the same place as +0)
    }
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

std::size_t hashWords(const uint32_t* words, std::size_t count) {
    uint64_t h = 1469598103934665603ull; // FNV-1a over the words
    for (std::size_t i = 0; i < count; ++i) {
        h = (h ^ words[i]) * 1099511628211ull;
    }
    return static_cast<std::size_t>(h ^ (h >> 32));
}

/// A point of space, bit for bit.
struct PointKey {
    uint32_t w[3];
    bool     operator==(const PointKey& o) const { return w[0] == o.w[0] && w[1] == o.w[1] && w[2] == o.w[2]; }
};
struct PointHash {
    std::size_t operator()(const PointKey& k) const noexcept { return hashWords(k.w, 3); }
};

PointKey pointKey(const glm::vec3& p) {
    return {{bitsOf(p.x), bitsOf(p.y), bitsOf(p.z)}};
}

/// What makes two vertices of a mesh the same point OF THE SURFACE, their normals aside: the
/// place, the UV, the skin weights and the corrective range. (A file with a normal per face has
/// a vertex per face corner; smoothed, the corners of one group are one vertex again.)
struct AttributeKey {
    uint32_t w[14];
    bool     operator==(const AttributeKey& o) const { return std::memcmp(w, o.w, sizeof w) == 0; }
};
struct AttributeHash {
    std::size_t operator()(const AttributeKey& k) const noexcept { return hashWords(k.w, 14); }
};

AttributeKey attributeKey(const Vertex& v) {
    AttributeKey k{};
    k.w[0] = bitsOf(v.pos.x);
    k.w[1] = bitsOf(v.pos.y);
    k.w[2] = bitsOf(v.pos.z);
    k.w[3] = bitsOf(v.uv.x);
    k.w[4] = bitsOf(v.uv.y);
    for (int i = 0; i < 4; ++i) {
        k.w[5 + i] = v.joints[i];
        k.w[9 + i] = bitsOf(v.weights[i]);
    }
    k.w[13] = v.correctiveRange;
    return k;
}

/// One triangle of a mesh that is being smoothed.
struct Face {
    uint32_t  mesh = 0;
    uint32_t  point[3] = {kNone, kNone, kNone}; ///< its corners' points (welded across the model)
    glm::vec3 normal{0.0f};                     ///< unit, on the side the file's normals are on
    float     angle[3] = {0.0f, 0.0f, 0.0f};    ///< the angle it has at each corner: the corner's weight
    bool      real = false;                     ///< has an area (a degenerate one joins no group)
};

/// How a mesh's angle reads in the edge test.
struct Crease {
    float cosAngle = 1.0f;
    bool  never = true;   ///< 0 degrees: every edge is crisp
    bool  always = false; ///< 180 degrees: no edge is
};

Crease creaseOf(float angleDeg) {
    Crease c;
    c.never = !(angleDeg > 0.01f);
    c.always = angleDeg >= 179.99f;
    c.cosAngle = std::cos(glm::radians(std::clamp(angleDeg, 0.0f, 180.0f)));
    return c;
}

/// Whether the edge two faces share is smooth: they meet at LESS than the lesser of their
/// meshes' angles (an edge AT the angle stays crisp, so the threshold never falls on a hexagon's
/// or a cube's own edges by a rounding error).
bool smoothAcross(const Face& a, const Face& b, const std::vector<Crease>& crease) {
    const Crease& ca = crease[a.mesh];
    const Crease& cb = crease[b.mesh];
    if (ca.never || cb.never) {
        return false;
    }
    if (ca.always && cb.always) {
        return true;
    }
    return glm::dot(a.normal, b.normal) > std::max(ca.cosAngle, cb.cosAngle) + 1.0e-5f;
}

uint32_t findRoot(std::vector<uint32_t>& parent, uint32_t i) {
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

} // namespace

std::vector<SmoothedMesh> smoothByAngle(const std::vector<SmoothMesh>& meshes) {
    std::vector<SmoothedMesh> out(meshes.size());

    // --- 1. The faces of the meshes being smoothed, their corners welded into the model's points.
    std::vector<Face>        faces;
    std::vector<std::size_t> firstFace(meshes.size(), 0);
    std::vector<Crease>      crease(meshes.size());
    std::vector<char>        smoothed(meshes.size(), 0);
    std::size_t              vertexTotal = 0;
    std::size_t              triangleTotal = 0;
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        const SmoothMesh& mesh = meshes[m];
        if (mesh.smooth && mesh.vertices != nullptr && mesh.indices != nullptr && !mesh.vertices->empty() && mesh.indices->size() >= 3) {
            smoothed[m] = 1;
            crease[m] = creaseOf(mesh.angleDeg);
            vertexTotal += mesh.vertices->size();
            triangleTotal += mesh.indices->size() / 3;
        }
    }
    if (triangleTotal == 0) {
        return out;
    }
    std::unordered_map<PointKey, uint32_t, PointHash> points;
    points.reserve(vertexTotal);
    faces.reserve(triangleTotal);
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        if (!smoothed[m]) {
            continue;
        }
        const std::vector<Vertex>&   vertices = *meshes[m].vertices;
        const std::vector<uint32_t>& indices = *meshes[m].indices;
        firstFace[m] = faces.size();
        std::vector<uint32_t> pointOf(vertices.size(), kNone);
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            Face face;
            face.mesh = static_cast<uint32_t>(m);
            const uint32_t v[3] = {indices[t], indices[t + 1], indices[t + 2]};
            if (v[0] < vertices.size() && v[1] < vertices.size() && v[2] < vertices.size()) {
                for (int c = 0; c < 3; ++c) {
                    if (pointOf[v[c]] == kNone) {
                        pointOf[v[c]] = points.try_emplace(pointKey(vertices[v[c]].pos), static_cast<uint32_t>(points.size())).first->second;
                    }
                    face.point[c] = pointOf[v[c]];
                }
                const glm::vec3 p[3] = {vertices[v[0]].pos, vertices[v[1]].pos, vertices[v[2]].pos};
                glm::vec3       n = glm::cross(p[1] - p[0], p[2] - p[0]);
                const float     length = glm::length(n);
                if (length > 0.0f && std::isfinite(length) && face.point[0] != face.point[1] && face.point[1] != face.point[2] &&
                    face.point[0] != face.point[2]) {
                    n /= length;
                    // (The side the file's normals are on: a face drawn as it was drawn before.)
                    if (glm::dot(n, vertices[v[0]].normal + vertices[v[1]].normal + vertices[v[2]].normal) < 0.0f) {
                        n = -n;
                    }
                    face.normal = n;
                    for (int c = 0; c < 3; ++c) {
                        const glm::vec3 a = glm::normalize(p[(c + 1) % 3] - p[c]);
                        const glm::vec3 b = glm::normalize(p[(c + 2) % 3] - p[c]);
                        face.angle[c] = std::acos(std::clamp(glm::dot(a, b), -1.0f, 1.0f));
                    }
                    face.real = true;
                }
            }
            faces.push_back(face);
        }
    }

    // --- 2. Per point, the face corners that meet there.
    const std::size_t        pointCount = points.size();
    std::vector<uint32_t>    cornerStart(pointCount + 1, 0);
    for (const Face& face : faces) {
        if (face.real) {
            for (int c = 0; c < 3; ++c) {
                ++cornerStart[face.point[c] + 1];
            }
        }
    }
    for (std::size_t p = 0; p < pointCount; ++p) {
        cornerStart[p + 1] += cornerStart[p];
    }
    std::vector<uint32_t> cornersAt(cornerStart[pointCount]);
    {
        std::vector<uint32_t> fill(cornerStart.begin(), cornerStart.end() - 1);
        for (std::size_t f = 0; f < faces.size(); ++f) {
            if (faces[f].real) {
                for (int c = 0; c < 3; ++c) {
                    cornersAt[fill[faces[f].point[c]]++] = static_cast<uint32_t>(f * 3 + static_cast<std::size_t>(c));
                }
            }
        }
    }

    // --- 3. The smooth groups: round each point, the corners joined across every smooth edge.
    std::vector<uint32_t>  groupOf(faces.size() * 3, kNone);
    std::vector<glm::vec3> groupNormal;
    std::vector<float>     groupOcclusion;
    std::vector<uint32_t>  groupCorners;
    // (A corner's occlusion is its vertex's: looked up per mesh below.)
    std::vector<uint32_t>                      parent;
    std::vector<std::pair<uint32_t, uint32_t>> edges; // (the edge's other point, the corner's place in this point's list)
    std::vector<uint32_t>                      groupAt;
    for (std::size_t p = 0; p < pointCount; ++p) {
        const uint32_t begin = cornerStart[p];
        const uint32_t count = cornerStart[p + 1] - begin;
        if (count == 0) {
            continue;
        }
        parent.resize(count);
        edges.clear();
        for (uint32_t i = 0; i < count; ++i) {
            parent[i] = i;
            const uint32_t corner = cornersAt[begin + i];
            const Face&    face = faces[corner / 3];
            const uint32_t c = corner % 3;
            edges.emplace_back(face.point[(c + 1) % 3], i);
            edges.emplace_back(face.point[(c + 2) % 3], i);
        }
        std::sort(edges.begin(), edges.end());
        for (std::size_t run = 0; run < edges.size();) {
            std::size_t end = run + 1;
            while (end < edges.size() && edges[end].first == edges[run].first) {
                ++end;
            }
            for (std::size_t a = run; a < end; ++a) {
                for (std::size_t b = a + 1; b < end; ++b) {
                    const uint32_t ra = findRoot(parent, edges[a].second);
                    const uint32_t rb = findRoot(parent, edges[b].second);
                    if (ra != rb && smoothAcross(faces[cornersAt[begin + edges[a].second] / 3], faces[cornersAt[begin + edges[b].second] / 3], crease)) {
                        parent[rb] = ra;
                    }
                }
            }
            run = end;
        }
        groupAt.assign(count, kNone);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t root = findRoot(parent, i);
            if (groupAt[root] == kNone) {
                groupAt[root] = static_cast<uint32_t>(groupNormal.size());
                groupNormal.emplace_back(0.0f);
                groupOcclusion.push_back(0.0f);
                groupCorners.push_back(0);
            }
            const uint32_t corner = cornersAt[begin + i];
            const Face&    face = faces[corner / 3];
            const uint32_t c = corner % 3;
            const uint32_t group = groupAt[root];
            groupOf[corner] = group;
            groupNormal[group] += face.angle[c] * face.normal;
            const std::size_t triangle = corner / 3 - firstFace[face.mesh];
            groupOcclusion[group] += (*meshes[face.mesh].vertices)[(*meshes[face.mesh].indices)[triangle * 3 + c]].ao;
            ++groupCorners[group];
        }
    }
    for (std::size_t g = 0; g < groupNormal.size(); ++g) {
        const float length = glm::length(groupNormal[g]);
        groupNormal[g] = length > 1.0e-8f ? groupNormal[g] / length : glm::vec3(0.0f); // (0: faces that cancel — the corners keep the file's)
        groupOcclusion[g] /= static_cast<float>(std::max<uint32_t>(groupCorners[g], 1));
    }

    // --- 4. Each smoothed mesh's new vertices: one per (point of the surface, smooth group).
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        if (!smoothed[m]) {
            continue;
        }
        const std::vector<Vertex>&   vertices = *meshes[m].vertices;
        const std::vector<uint32_t>& indices = *meshes[m].indices;
        SmoothedMesh&                made = out[m];
        made.indices.reserve(indices.size());
        made.vertices.reserve(vertices.size());
        made.origin.reserve(vertices.size());
        std::unordered_map<AttributeKey, uint32_t, AttributeHash> same;
        same.reserve(vertices.size());
        std::vector<uint32_t> sameAs(vertices.size(), kNone);
        std::unordered_map<uint64_t, uint32_t> vertexOf; // (the surface point, the group) -> the new vertex
        vertexOf.reserve(vertices.size());
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            const std::size_t face = firstFace[m] + i / 3;
            for (std::size_t c = 0; c < 3; ++c) {
                const uint32_t v = std::min<uint32_t>(indices[i + c], static_cast<uint32_t>(vertices.size() - 1));
                const uint32_t group = groupOf[face * 3 + c];
                Vertex         vertex = vertices[v];
                uint64_t       key = 0;
                if (group == kNone || groupNormal[group] == glm::vec3(0.0f)) {
                    key = (static_cast<uint64_t>(v) << 32) | kNone; // as the file has it
                } else {
                    if (sameAs[v] == kNone) {
                        sameAs[v] = same.try_emplace(attributeKey(vertex), v).first->second;
                    }
                    key = (static_cast<uint64_t>(sameAs[v]) << 32) | group;
                    vertex.normal = groupNormal[group];
                    vertex.ao = groupOcclusion[group];
                    vertex.tangent = glm::vec4(0.0f);
                }
                const auto [it, added] = vertexOf.try_emplace(key, static_cast<uint32_t>(made.vertices.size()));
                if (added) {
                    made.vertices.push_back(vertex);
                    made.origin.push_back(v);
                }
                made.indices.push_back(it->second);
            }
        }
    }
    return out;
}

bool hasOnlyFaceNormals(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    bool any = false;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t v[3] = {indices[i], indices[i + 1], indices[i + 2]};
        if (v[0] >= vertices.size() || v[1] >= vertices.size() || v[2] >= vertices.size()) {
            continue;
        }
        const glm::vec3 n = glm::cross(vertices[v[1]].pos - vertices[v[0]].pos, vertices[v[2]].pos - vertices[v[0]].pos);
        const float     length = glm::length(n);
        if (!(length > 0.0f) || !std::isfinite(length)) {
            continue; // no area: it says nothing either way
        }
        for (const uint32_t index : v) {
            const glm::vec3& normal = vertices[index].normal;
            const float      l = glm::length(normal);
            // (Either side: a file's normals may face against its winding.)
            if (!(l > 1.0e-6f) || std::abs(glm::dot(normal, n)) < kFaceNormalCos * l * length) {
                return false;
            }
        }
        any = true;
    }
    return any;
}

float facetedShare(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    std::size_t real = 0, faceted = 0;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t v[3] = {indices[i], indices[i + 1], indices[i + 2]};
        if (v[0] >= vertices.size() || v[1] >= vertices.size() || v[2] >= vertices.size()) {
            continue;
        }
        const glm::vec3 n = glm::cross(vertices[v[1]].pos - vertices[v[0]].pos, vertices[v[2]].pos - vertices[v[0]].pos);
        const float     length = glm::length(n);
        if (!(length > 0.0f) || !std::isfinite(length)) {
            continue;
        }
        ++real;
        bool flat = true;
        for (const uint32_t index : v) {
            const glm::vec3& normal = vertices[index].normal;
            const float      l = glm::length(normal);
            flat = flat && l > 1.0e-6f && std::abs(glm::dot(normal, n)) >= kFaceNormalCos * l * length;
        }
        faceted += flat ? 1 : 0;
    }
    return real > 0 ? static_cast<float>(faceted) / static_cast<float>(real) : 0.0f;
}

int applyImportSmoothing(ModelData& data) {
    std::vector<SmoothMesh> meshes(data.meshes.size());
    std::size_t             vertices = 0;
    int                     chosen = 0;
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        MeshData& mesh = data.meshes[m];
        meshes[m].vertices = &mesh.vertices;
        meshes[m].indices = &mesh.indices;
        meshes[m].angleDeg = kDefaultSmoothAngle;
        // (A native figure's normals are the importer's own, smooth across the whole surface: its
        // meshes carry their base-vertex map, and are left alone.)
        if (mesh.indices.size() < 3 || mesh.vertices.empty() || !mesh.baseVertex.empty() || !mesh.fileVertices.empty()) {
            continue;
        }
        if (mesh.normalsGenerated || hasOnlyFaceNormals(mesh.vertices, mesh.indices)) {
            meshes[m].smooth = true;
            vertices += mesh.vertices.size();
            ++chosen;
        }
    }
    if (chosen == 0) {
        return 0;
    }
    if (vertices > kImportSmoothingVertexCap) {
        data.notes.push_back("the file carries no smoothing of its own, and is too large to smooth at import (" + std::to_string(vertices) +
                             " vertices): shaded as the file has it");
        return 0;
    }
    std::vector<SmoothedMesh> smoothed = smoothByAngle(meshes);
    int                       done = 0;
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        if (!meshes[m].smooth || smoothed[m].indices.size() != data.meshes[m].indices.size()) {
            continue;
        }
        MeshData& mesh = data.meshes[m];
        mesh.fileVertices = std::move(mesh.vertices);
        mesh.fileIndices = std::move(mesh.indices);
        mesh.vertices = std::move(smoothed[m].vertices);
        mesh.indices = std::move(smoothed[m].indices);
        mesh.smoothByAngle = true;
        mesh.smoothAngle = kDefaultSmoothAngle;
        ++done;
    }
    if (done > 0) {
        data.notes.push_back("smoothed " + std::to_string(done) + (done == 1 ? " zone" : " zones") + " by angle (" +
                             std::to_string(static_cast<int>(kDefaultSmoothAngle)) + " degrees): the file carries no smoothing of its own");
    }
    return done;
}

} // namespace pose
