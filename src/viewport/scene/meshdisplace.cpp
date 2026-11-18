/**
 * @file meshdisplace.cpp
 * @brief displaceMesh — a posed mesh subdivided and moved by a displacement map. See meshdisplace.h
 *        for what it does and why; this file is how.
 *
 * THE BOOKKEEPING. A render vertex is a position with ONE normal and ONE UV, so the same point
 * of a surface is several vertices wherever its UVs or normals split. Three levels of identity
 * are kept apart:
 *   - the VERTEX (an index): what triangles name, what carries a UV — the height is sampled here;
 *   - the POINT (a "group"): the vertices at one place (to a micrometre) — what MOVES, as one;
 *   - the CLUSTER: the vertices of one point whose normals agree (within kClusterCos) — one
 *     smooth side of the point. A point on a smooth surface has one cluster however many UV
 *     seams run through it; a box's edge has two, its corner three.
 * Subdivision keeps all three exact: the midpoint of two positions is the same number whichever
 * side of a seam computes it (a + b = b + a), so the midpoints of one welded edge are one point
 * without any search — the edges are sorted by their welded ends, and a run of the sort is a point.
 */

#include "meshdisplace.h"

#include "parallelfor.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pose {

namespace {

/// Two normals of one point are one smooth side when they agree within this cosine (26 degrees):
/// a hard edge kept by the smoothing stands a good deal wider, a seam's two copies at zero.
constexpr float kClusterCos = 0.9f;
/// The sides one point is solved for; a point with more (a fan's apex) takes their mean.
constexpr int kMaxClusters = 3;
/// A point's offset is never more than this many times its largest per-side displacement (two
/// faces meeting at a knife's edge would otherwise send their shared edge to the horizon).
constexpr float kMaxMeetFactor = 4.0f;

struct PosKey {
    int32_t x;
    int32_t y;
    int32_t z;
    bool    operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

/// A place, to a micrometre (world units are metres): two vertices are one point of the surface
/// when they round to the same key. Files written by a modelling package repeat a position's
/// numbers exactly at a seam; a mesh made by arithmetic may differ in the last digit (a sphere's
/// seam at 0 and at two pi) — and a displacement that told those apart would open the seam.
/// THE LIMIT: the key is a ROUNDING GRID (cells of 2^-20 m), not a distance test. Bit-equal
/// positions always weld; two that differ in the last digit weld unless a cell boundary happens
/// to fall between them — for coordinates of 0.1 to 1 m a few times in a hundred per coordinate —
/// and then they stay two points, displaced by heights sampled apart: a pinhole in that seam. No
/// mesh met so far shows it (a file's seams and their skinned copies are bit-equal); the remedy,
/// should one turn up, is to look a value near a boundary up in the neighbouring cell as well.
PosKey keyOf(const glm::vec3& p) {
    const auto snap = [](float v) {
        constexpr double kGrid = 1048576.0; // 2^20 a metre
        const double     scaled = std::clamp(static_cast<double>(v) * kGrid, -2147483000.0, 2147483000.0);
        return static_cast<int32_t>(std::llround(scaled));
    };
    return {snap(p.x), snap(p.y), snap(p.z)};
}

struct PosHash {
    std::size_t operator()(const PosKey& k) const {
        uint64_t h = (static_cast<uint64_t>(static_cast<uint32_t>(k.x)) + 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
        h ^= h >> 31;
        h = (h + static_cast<uint32_t>(k.y)) * 0x94D049BB133111EBull;
        h ^= h >> 29;
        h = (h + static_cast<uint32_t>(k.z)) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 32;
        return static_cast<std::size_t>(h);
    }
};

uint64_t pairKey(uint32_t a, uint32_t b) {
    return a < b ? (static_cast<uint64_t>(a) << 32) | b : (static_cast<uint64_t>(b) << 32) | a;
}

/// One triangle edge, by the points and by the vertices it joins; `slot` is its place in the
/// index list (3 × triangle + edge).
struct Edge {
    uint64_t welded;
    uint64_t render;
    uint32_t slot;
};

/// v turned by the least rotation that takes unit vector @p from onto unit vector @p to.
glm::vec3 rotateBetween(const glm::vec3& from, const glm::vec3& to, const glm::vec3& v) {
    const float c = glm::dot(from, to);
    if (c > 0.9999995f || c < -0.9999f) {
        return v; // (nothing to turn — or turned right over, which names no axis)
    }
    const glm::vec3 w = glm::cross(from, to);
    return v * c + glm::cross(w, v) + w * (glm::dot(w, v) / (1.0f + c));
}

/// The height map reduced by whole halvings (a box filter), as floats in 0..1, sampled
/// bilinearly with wrap-around.
class HeightField {
public:
    HeightField(const HeightMap& map, int mip) {
        m_width = std::max(1, map.width >> mip);
        m_height = std::max(1, map.height >> mip);
        const int   channel = std::clamp(map.channel, 0, 3);
        const auto  texel = [&](int x, int y) { return map.pixels[(static_cast<std::size_t>(y) * map.width + x) * 4u + channel]; };
        m_data.assign(static_cast<std::size_t>(m_width) * m_height, 0.0f);
        if (m_width == map.width && m_height == map.height) {
            for (int y = 0; y < m_height; ++y) {
                for (int x = 0; x < m_width; ++x) {
                    m_data[static_cast<std::size_t>(y) * m_width + x] = texel(x, y) * (1.0f / 255.0f);
                }
            }
            return;
        }
        // Each source texel into the cell it falls in; a cell's value is the mean of its texels.
        std::vector<uint32_t> count(m_data.size(), 0);
        std::vector<int>      column(static_cast<std::size_t>(map.width));
        for (int x = 0; x < map.width; ++x) {
            column[static_cast<std::size_t>(x)] = std::min(m_width - 1, static_cast<int>(static_cast<int64_t>(x) * m_width / map.width));
        }
        for (int y = 0; y < map.height; ++y) {
            const std::size_t row = static_cast<std::size_t>(std::min(m_height - 1, static_cast<int>(static_cast<int64_t>(y) * m_height / map.height))) *
                                    static_cast<std::size_t>(m_width);
            for (int x = 0; x < map.width; ++x) {
                const std::size_t cell = row + static_cast<std::size_t>(column[static_cast<std::size_t>(x)]);
                m_data[cell] += texel(x, y);
                ++count[cell];
            }
        }
        for (std::size_t i = 0; i < m_data.size(); ++i) {
            m_data[i] = count[i] > 0 ? m_data[i] / (255.0f * static_cast<float>(count[i])) : 0.0f;
        }
    }

    float sample(const glm::vec2& uv) const {
        const float x = uv.x * static_cast<float>(m_width) - 0.5f;
        const float y = uv.y * static_cast<float>(m_height) - 0.5f;
        const float fx0 = std::floor(x);
        const float fy0 = std::floor(y);
        const float tx = x - fx0;
        const float ty = y - fy0;
        const int   x0 = wrap(static_cast<int64_t>(fx0), m_width);
        const int   y0 = wrap(static_cast<int64_t>(fy0), m_height);
        const int   x1 = x0 + 1 == m_width ? 0 : x0 + 1;
        const int   y1 = y0 + 1 == m_height ? 0 : y0 + 1;
        const float top = at(x0, y0) * (1.0f - tx) + at(x1, y0) * tx;
        const float bottom = at(x0, y1) * (1.0f - tx) + at(x1, y1) * tx;
        return top * (1.0f - ty) + bottom * ty;
    }

private:
    static int wrap(int64_t i, int n) {
        const int64_t m = i % n;
        return static_cast<int>(m < 0 ? m + n : m);
    }
    float at(int x, int y) const { return m_data[static_cast<std::size_t>(y) * m_width + x]; }

    int                m_width = 1;
    int                m_height = 1;
    std::vector<float> m_data;
};

/// The material's UV transform, as mesh.vert applies it: scale, rotate about the origin, translate.
struct UvTransform {
    glm::vec2 tiling{1.0f};
    glm::vec2 offset{0.0f};
    float     c = 1.0f;
    float     s = 0.0f;
    bool      rotated = false;

    explicit UvTransform(const DisplaceSettings& settings) : tiling(settings.uvTiling), offset(settings.uvOffset) {
        if (settings.uvRotation != 0.0f) {
            c = std::cos(settings.uvRotation);
            s = std::sin(settings.uvRotation);
            rotated = true;
        }
    }
    /// The linear part (what a DIFFERENCE of two UVs goes through).
    glm::vec2 linear(const glm::vec2& uv) const {
        const glm::vec2 t = uv * tiling;
        return rotated ? glm::vec2(c * t.x + s * t.y, -s * t.x + c * t.y) : t;
    }
    glm::vec2 apply(const glm::vec2& uv) const { return linear(uv) + offset; }
};

} // namespace

// ------------------------------------------------------------------------------------------------
// SharedPositions
// ------------------------------------------------------------------------------------------------

struct SharedPositions::Impl {
    struct Entry {
        uint32_t lastMesh = 0; // the last mesh (from 1) that had a vertex here
        bool     shared = false;
    };
    std::unordered_map<PosKey, Entry, PosHash> places;
    uint32_t                                   meshes = 0;
};

SharedPositions::SharedPositions() : m_impl(std::make_unique<Impl>()) {}
SharedPositions::~SharedPositions() = default;

void SharedPositions::addMesh(const std::vector<glm::vec3>& positions) {
    const uint32_t id = ++m_impl->meshes;
    m_impl->places.reserve(m_impl->places.size() + positions.size());
    for (const glm::vec3& p : positions) {
        Impl::Entry& entry = m_impl->places[keyOf(p)];
        if (entry.lastMesh != 0 && entry.lastMesh != id) {
            entry.shared = true;
        }
        entry.lastMesh = id;
    }
}

bool SharedPositions::shared(const glm::vec3& position) const {
    const auto it = m_impl->places.find(keyOf(position));
    return it != m_impl->places.end() && it->second.shared;
}

// ------------------------------------------------------------------------------------------------
// displaceMesh
// ------------------------------------------------------------------------------------------------

DisplaceReport displaceMesh(PosedMesh& mesh, const HeightMap& map, const DisplaceSettings& settings, const SharedPositions* neighbours) {
    DisplaceReport report;
    std::vector<glm::vec3>& positions = mesh.positions;
    std::vector<glm::vec3>& normals = mesh.normals;
    std::vector<glm::vec2>& uvs = mesh.uvs;
    std::vector<glm::vec4>& tangents = mesh.tangents;
    std::vector<uint32_t>&  indices = mesh.indices;

    const std::size_t sourceVertices = positions.size();
    const std::size_t sourceTriangles = indices.size() / 3;
    if (map.pixels == nullptr || map.width < 1 || map.height < 1 || settings.scale == 0.0f || sourceTriangles == 0 || sourceVertices == 0 ||
        normals.size() != sourceVertices || uvs.size() != sourceVertices || sourceVertices > 0x7FFFFFFFu) {
        return report;
    }
    for (const uint32_t index : indices) {
        if (index >= sourceVertices) {
            return report; // (a mesh that names a vertex it has not: left alone)
        }
    }
    indices.resize(sourceTriangles * 3);
    const bool hasTangents = tangents.size() == sourceVertices;

    // The levels the budget allows.
    int levels = std::clamp(settings.levels, 0, kDisplaceMaxLevels);
    while (levels > 0 && (sourceTriangles << (2 * levels)) > settings.maxTriangles) {
        --levels;
    }

    // --- 2 (first, on the mesh as it came): the points — vertices welded by position ---
    std::vector<uint32_t> group(sourceVertices);
    std::vector<uint32_t> groupVertex; // a vertex of each point (its position)
    {
        std::unordered_map<PosKey, uint32_t, PosHash> seen;
        seen.reserve(sourceVertices);
        for (std::size_t v = 0; v < sourceVertices; ++v) {
            const auto [it, added] = seen.try_emplace(keyOf(positions[v]), static_cast<uint32_t>(groupVertex.size()));
            if (added) {
                groupVertex.push_back(static_cast<uint32_t>(v));
            }
            group[v] = it->second;
        }
    }
    std::vector<uint8_t> pinned(groupVertex.size(), 0);

    std::vector<Edge> edges;
    const auto        buildEdges = [&]() {
        edges.resize(indices.size());
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const std::size_t triangle = i / 3;
            const uint32_t    a = indices[i];
            const uint32_t    b = indices[triangle * 3 + (i - triangle * 3 + 1) % 3];
            edges[i] = {pairKey(group[a], group[b]), pairKey(a, b), static_cast<uint32_t>(i)};
        }
        std::sort(edges.begin(), edges.end(), [](const Edge& x, const Edge& y) {
            return x.welded != y.welded ? x.welded < y.welded : x.render < y.render;
        });
    };

    // --- 5 (found now, kept through the subdivision): the borders shared with another mesh ---
    // An edge one triangle uses is a border; with both its ends at places another mesh of the
    // object has vertices too, it is that mesh's border as well and must stay where it is.
    std::unordered_set<uint64_t> pinnedEdges;
    const bool                   wantEdges = levels > 0 || neighbours != nullptr;
    if (wantEdges) {
        buildEdges();
    }
    if (neighbours != nullptr) {
        for (std::size_t i = 0; i < edges.size();) {
            std::size_t j = i + 1;
            while (j < edges.size() && edges[j].welded == edges[i].welded) {
                ++j;
            }
            const uint32_t lo = static_cast<uint32_t>(edges[i].welded >> 32);
            const uint32_t hi = static_cast<uint32_t>(edges[i].welded);
            if (j - i == 1 && lo != hi && neighbours->shared(positions[groupVertex[lo]]) && neighbours->shared(positions[groupVertex[hi]])) {
                pinnedEdges.insert(edges[i].welded);
                pinned[lo] = 1;
                pinned[hi] = 1;
            }
            i = j;
        }
    }

    // --- 1: subdivide — every triangle into four at its edges' midpoints ---
    for (int level = 0; level < levels; ++level) {
        if (level > 0) {
            buildEdges();
        }
        std::vector<uint32_t>        middle(indices.size());
        std::unordered_set<uint64_t> nextPinned;
        // (Upper bounds: a midpoint per edge slot at most.)
        const std::size_t grown = positions.size() + edges.size() / 2 + 16;
        positions.reserve(grown);
        normals.reserve(grown);
        uvs.reserve(grown);
        group.reserve(grown);
        if (hasTangents) {
            tangents.reserve(grown);
        }
        for (std::size_t i = 0; i < edges.size();) {
            std::size_t j = i + 1;
            while (j < edges.size() && edges[j].welded == edges[i].welded) {
                ++j;
            }
            // One welded edge = one new point (an edge between two vertices of ONE point has its
            // midpoint at that point).
            const uint32_t lo = static_cast<uint32_t>(edges[i].welded >> 32);
            const uint32_t hi = static_cast<uint32_t>(edges[i].welded);
            uint32_t       point = lo;
            if (lo != hi) {
                point = static_cast<uint32_t>(groupVertex.size());
                const bool pin = pinnedEdges.count(edges[i].welded) != 0;
                pinned.push_back(pin ? 1 : 0);
                groupVertex.push_back(static_cast<uint32_t>(positions.size()));
                if (pin) {
                    nextPinned.insert(pairKey(lo, point));
                    nextPinned.insert(pairKey(point, hi));
                }
            }
            // … and one new vertex per pair of vertices that names it (each side of a seam its own).
            for (std::size_t k = i; k < j;) {
                std::size_t l = k + 1;
                while (l < j && edges[l].render == edges[k].render) {
                    ++l;
                }
                const uint32_t  a = static_cast<uint32_t>(edges[k].render >> 32);
                const uint32_t  b = static_cast<uint32_t>(edges[k].render);
                const uint32_t  vertex = static_cast<uint32_t>(positions.size());
                const glm::vec3 position = 0.5f * (positions[a] + positions[b]); // (a + b = b + a: one place from either side)
                const glm::vec3 sum = normals[a] + normals[b];
                const float     length = glm::length(sum);
                const glm::vec3 normal = length > 1.0e-12f ? sum / length : normals[a];
                const glm::vec2 uv = 0.5f * (uvs[a] + uvs[b]);
                positions.push_back(position);
                normals.push_back(normal);
                uvs.push_back(uv);
                group.push_back(point);
                if (hasTangents) {
                    const glm::vec4 tangent(0.5f * (glm::vec3(tangents[a]) + glm::vec3(tangents[b])), tangents[a].w);
                    tangents.push_back(tangent);
                }
                for (std::size_t m = k; m < l; ++m) {
                    middle[edges[m].slot] = vertex;
                }
                k = l;
            }
            i = j;
        }
        std::vector<uint32_t> next;
        next.reserve(indices.size() * 4);
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            const uint32_t a = indices[t];
            const uint32_t b = indices[t + 1];
            const uint32_t c = indices[t + 2];
            const uint32_t ab = middle[t];
            const uint32_t bc = middle[t + 1];
            const uint32_t ca = middle[t + 2];
            const uint32_t four[12] = {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca};
            next.insert(next.end(), four, four + 12);
        }
        indices.swap(next);
        pinnedEdges.swap(nextPinned);
    }
    edges = std::vector<Edge>();

    const std::size_t vertexCount = positions.size();
    const std::size_t pointCount = groupVertex.size();
    const std::size_t triangleCount = indices.size() / 3;

    // --- the clusters: each point's vertices by the side of the surface their normals are on ---
    // (The vertices of a point, listed point by point.)
    std::vector<uint32_t> memberStart(pointCount + 1, 0);
    for (std::size_t v = 0; v < vertexCount; ++v) {
        ++memberStart[group[v] + 1];
    }
    for (std::size_t g = 0; g < pointCount; ++g) {
        memberStart[g + 1] += memberStart[g];
    }
    std::vector<uint32_t> members(vertexCount);
    {
        std::vector<uint32_t> fill(memberStart.begin(), memberStart.end() - 1);
        for (std::size_t v = 0; v < vertexCount; ++v) {
            members[fill[group[v]]++] = static_cast<uint32_t>(v);
        }
    }
    std::vector<uint32_t>  clusterOf(vertexCount);
    std::vector<uint32_t>  clusterStart(pointCount + 1, 0);
    std::vector<glm::vec3> clusterNormal; // the first member's normal, then (below) the mean
    clusterNormal.reserve(pointCount + pointCount / 8);
    for (std::size_t g = 0; g < pointCount; ++g) {
        const uint32_t first = static_cast<uint32_t>(clusterNormal.size());
        clusterStart[g] = first;
        for (uint32_t m = memberStart[g]; m < memberStart[g + 1]; ++m) {
            const uint32_t   v = members[m];
            const glm::vec3& n = normals[v];
            uint32_t         best = first;
            float            bestCos = -2.0f;
            for (uint32_t c = first; c < clusterNormal.size(); ++c) {
                const float cosine = glm::dot(clusterNormal[c], n);
                if (cosine > bestCos) {
                    bestCos = cosine;
                    best = c;
                }
            }
            if (bestCos < kClusterCos && clusterNormal.size() - first < static_cast<std::size_t>(kMaxClusters + 1)) {
                best = static_cast<uint32_t>(clusterNormal.size());
                clusterNormal.push_back(n);
            }
            clusterOf[v] = best;
        }
    }
    clusterStart[pointCount] = static_cast<uint32_t>(clusterNormal.size());
    const std::size_t clusterCount = clusterNormal.size();

    // --- 3: the height at every vertex, from the map reduced to the mesh's own fineness ---
    const UvTransform transform(settings);
    int               mip = 0;
    {
        // The mean length of a triangle's edge, in the map's texels (a sample of the triangles is
        // enough for a mean).
        const std::size_t step = std::max<std::size_t>(1, triangleCount / 200000);
        const glm::vec2   size(static_cast<float>(map.width), static_cast<float>(map.height));
        double            total = 0.0;
        std::size_t       counted = 0;
        for (std::size_t t = 0; t < triangleCount; t += step) {
            for (int e = 0; e < 3; ++e) {
                const glm::vec2 d = transform.linear(uvs[indices[t * 3 + (e + 1) % 3]] - uvs[indices[t * 3 + e]]) * size;
                total += std::sqrt(static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y);
                ++counted;
            }
        }
        const double mean = counted > 0 ? total / static_cast<double>(counted) : 1.0;
        if (mean > 1.0) {
            mip = static_cast<int>(std::floor(std::log2(mean) + 0.5));
        }
        while (mip > 0 && (std::min(map.width, map.height) >> mip) < 1) {
            --mip;
        }
    }
    const HeightField  field(map, mip);
    std::vector<float> amount(vertexCount); // (height - mid level) × scale, per vertex
    {
        constexpr std::size_t kChunk = 8192;
        const int             chunks = static_cast<int>((vertexCount + kChunk - 1) / kChunk);
        const float           mid = settings.midLevel;
        const float           scale = settings.scale;
        parallelFor(chunks, [&](int chunk) {
            const std::size_t begin = static_cast<std::size_t>(chunk) * kChunk;
            const std::size_t end = std::min(vertexCount, begin + kChunk);
            for (std::size_t v = begin; v < end; ++v) {
                amount[v] = (field.sample(transform.apply(uvs[v])) - mid) * scale;
            }
        });
    }

    // Each cluster's mean normal and mean amount.
    std::vector<glm::vec3> clusterSum(clusterCount, glm::vec3(0.0f));
    std::vector<float>     clusterAmount(clusterCount, 0.0f);
    std::vector<uint32_t>  clusterSize(clusterCount, 0);
    for (std::size_t v = 0; v < vertexCount; ++v) {
        const uint32_t c = clusterOf[v];
        clusterSum[c] += normals[v];
        clusterAmount[c] += amount[v];
        ++clusterSize[c];
    }
    for (std::size_t c = 0; c < clusterCount; ++c) {
        const float length = glm::length(clusterSum[c]);
        if (length > 1.0e-12f) {
            clusterNormal[c] = clusterSum[c] / length;
        }
        if (clusterSize[c] > 0) {
            clusterAmount[c] /= static_cast<float>(clusterSize[c]);
        }
    }
    clusterSum = std::vector<glm::vec3>();

    // --- 4: where each point goes ---
    std::vector<glm::vec3> offset(pointCount, glm::vec3(0.0f));
    float                  farthest = 0.0f;
    std::size_t            held = 0;
    for (std::size_t g = 0; g < pointCount; ++g) {
        if (pinned[g] != 0) {
            ++held;
            continue;
        }
        const uint32_t first = clusterStart[g];
        const uint32_t count = clusterStart[g + 1] - first;
        if (count == 0) {
            continue;
        }
        glm::vec3 mean(0.0f);
        float     largest = 0.0f;
        for (uint32_t c = first; c < first + count; ++c) {
            mean += clusterNormal[c] * clusterAmount[c];
            largest = std::max(largest, std::abs(clusterAmount[c]));
        }
        mean /= static_cast<float>(count);
        glm::vec3 moved = mean;
        // Several sides: the least offset D with D · n = amount on every side — where the
        // displaced faces meet. (One side: its normal × its amount, which the mean already is.)
        if (count == 2) {
            const glm::vec3& n1 = clusterNormal[first];
            const glm::vec3& n2 = clusterNormal[first + 1];
            const float      c = glm::dot(n1, n2);
            const float      det = 1.0f - c * c;
            if (det > 0.05f) {
                const float d1 = clusterAmount[first];
                const float d2 = clusterAmount[first + 1];
                moved = n1 * ((d1 - c * d2) / det) + n2 * ((d2 - c * d1) / det);
            }
        } else if (count == 3) {
            const glm::vec3& n1 = clusterNormal[first];
            const glm::vec3& n2 = clusterNormal[first + 1];
            const glm::vec3& n3 = clusterNormal[first + 2];
            const glm::mat3  gram(glm::vec3(1.0f, glm::dot(n1, n2), glm::dot(n1, n3)), glm::vec3(glm::dot(n2, n1), 1.0f, glm::dot(n2, n3)),
                                  glm::vec3(glm::dot(n3, n1), glm::dot(n3, n2), 1.0f));
            if (std::abs(glm::determinant(gram)) > 0.05f) {
                const glm::vec3 x = glm::inverse(gram) * glm::vec3(clusterAmount[first], clusterAmount[first + 1], clusterAmount[first + 2]);
                moved = n1 * x.x + n2 * x.y + n3 * x.z;
            }
        }
        const float length = glm::length(moved);
        if (length > kMaxMeetFactor * largest && length > 0.0f) {
            moved *= kMaxMeetFactor * largest / length;
        }
        offset[g] = moved;
        farthest = std::max(farthest, glm::length(moved));
    }

    // --- 6: the normals and tangents turned by what the displacement did to the surface ---
    // Per cluster, the surface's own normal before and after: the area-weighted sum of its
    // triangles' normals, each taken on the side its vertices' shading normals are on (a mesh's
    // winding is not to be trusted).
    std::vector<glm::vec3> before(clusterCount, glm::vec3(0.0f));
    std::vector<glm::vec3> after(clusterCount, glm::vec3(0.0f));
    for (std::size_t t = 0; t < triangleCount; ++t) {
        const uint32_t  a = indices[t * 3];
        const uint32_t  b = indices[t * 3 + 1];
        const uint32_t  c = indices[t * 3 + 2];
        const glm::vec3 pa = positions[a];
        const glm::vec3 pb = positions[b];
        const glm::vec3 pc = positions[c];
        const glm::vec3 flat = glm::cross(pb - pa, pc - pa);
        const glm::vec3 qa = pa + offset[group[a]];
        const glm::vec3 moved = glm::cross(pb + offset[group[b]] - qa, pc + offset[group[c]] - qa);
        const float     side = glm::dot(flat, normals[a] + normals[b] + normals[c]) < 0.0f ? -1.0f : 1.0f;
        for (const uint32_t v : {a, b, c}) {
            before[clusterOf[v]] += side * flat;
            after[clusterOf[v]] += side * moved;
        }
    }
    {
        constexpr std::size_t kChunk = 8192;
        const int             chunks = static_cast<int>((vertexCount + kChunk - 1) / kChunk);
        parallelFor(chunks, [&](int chunk) {
            const std::size_t begin = static_cast<std::size_t>(chunk) * kChunk;
            const std::size_t end = std::min(vertexCount, begin + kChunk);
            for (std::size_t v = begin; v < end; ++v) {
                if (pinned[group[v]] != 0) {
                    continue; // (a held border is shaded as its neighbour shades it)
                }
                const uint32_t c = clusterOf[v];
                const float    lengthBefore = glm::length(before[c]);
                const float    lengthAfter = glm::length(after[c]);
                if (lengthBefore < 1.0e-20f || lengthAfter < 1.0e-20f) {
                    continue;
                }
                const glm::vec3 from = before[c] / lengthBefore;
                const glm::vec3 to = after[c] / lengthAfter;
                const glm::vec3 turned = rotateBetween(from, to, normals[v]);
                const float     length = glm::length(turned);
                if (length > 1.0e-12f) {
                    normals[v] = turned / length;
                }
                if (hasTangents) {
                    glm::vec3 tangent = rotateBetween(from, to, glm::vec3(tangents[v]));
                    tangent -= normals[v] * glm::dot(normals[v], tangent); // (square to the new normal)
                    const float tangentLength = glm::length(tangent);
                    if (tangentLength > 1.0e-12f) {
                        tangents[v] = glm::vec4(tangent / tangentLength, tangents[v].w);
                    }
                }
            }
        });
    }
    for (std::size_t v = 0; v < vertexCount; ++v) {
        positions[v] += offset[group[v]];
    }

    report.displaced = true;
    report.levels = levels;
    report.triangles = triangleCount;
    report.vertices = vertexCount;
    report.points = pointCount;
    report.pinned = held;
    report.mipLevel = mip;
    report.maxOffset = farthest;
    return report;
}

} // namespace pose
