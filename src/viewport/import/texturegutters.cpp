/**
 * @file texturegutters.cpp
 * @brief Implementation of fillImageGutters. See texturegutters.h.
 */

#include "texturegutters.h"

#include "parallelfor.h"
#include "vertex.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

namespace pose {

namespace {

// One level of the pull-push pyramid above the base image: the average colour of the covered
// texels beneath each coarse texel, plus whether any were covered at all.
struct PyramidLevel {
    int                  w = 0;
    int                  h = 0;
    std::vector<uint8_t> rgba; // 4 bytes per texel
    std::vector<uint8_t> has;  // 1 = carries a colour
};

// Rasterizes one mesh's UV triangles into the shared per-texel coverage mask (accumulating — a
// shared atlas collects coverage from every mesh that samples it). A texel (x, y) samples
// uv = ((x+0.5)/w, (y+0.5)/h), so its centre (x+0.5, y+0.5) is tested against the triangles
// scaled into texel space. The runtime sampler REPEATs, so out-of-range UVs wrap into the mask.
void rasterizeCoverage(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices,
                       int w, int h, std::vector<uint8_t>& covered) {
    const glm::vec2 scale(static_cast<float>(w), static_cast<float>(h));
    const int triCount = static_cast<int>(indices.size() / 3);

    // Coverage is a set union, so triangles identical in (quarter-texel-quantized, wrap-folded) UV
    // space contribute nothing after the first — and fibermesh-style meshes map THOUSANDS of
    // strands onto the same tiny UV strip (a 150k-triangle brow follower re-marked the same texels
    // ~3 s per map before this). Deduplicate first; ordinary atlas meshes pass through unchanged.
    std::vector<int> uniqueTris;
    uniqueTris.reserve(static_cast<std::size_t>(triCount));
    {
        std::unordered_set<uint64_t> seen;
        seen.reserve(static_cast<std::size_t>(triCount));
        const auto quantize = [&](const glm::vec2& uv) -> uint64_t {
            // Quarter-texel grid, folded by the sampler's REPEAT wrap so tiled copies collide too.
            const int qw = 4 * w, qh = 4 * h;
            const int qx = ((static_cast<int>(std::lround(uv.x * scale.x * 4.0f)) % qw) + qw) % qw;
            const int qy = ((static_cast<int>(std::lround(uv.y * scale.y * 4.0f)) % qh) + qh) % qh;
            return (static_cast<uint64_t>(qx) << 32) | static_cast<uint64_t>(qy);
        };
        for (int t = 0; t < triCount; ++t) {
            uint64_t key = 1469598103934665603ull; // FNV-1a over the three quantized corners
            for (int c = 0; c < 3; ++c) {
                key ^= quantize(vertices[indices[3 * t + c]].uv);
                key *= 1099511628211ull;
            }
            if (seen.insert(key).second) {
                uniqueTris.push_back(t);
            }
        }
    }
    const int uniqueCount = static_cast<int>(uniqueTris.size());

    // Parallel over triangle blocks: concurrent writes only ever store the same value (1), so the
    // races are benign.
    constexpr int kBlock = 256;
    const int blocks = (uniqueCount + kBlock - 1) / kBlock;
    parallelFor(blocks, [&](int block) {
        const int tEnd = std::min(uniqueCount, (block + 1) * kBlock);
        for (int u = block * kBlock; u < tEnd; ++u) {
            const int t = uniqueTris[static_cast<std::size_t>(u)];
            const glm::vec2 a = vertices[indices[3 * t + 0]].uv * scale;
            const glm::vec2 b = vertices[indices[3 * t + 1]].uv * scale;
            const glm::vec2 c = vertices[indices[3 * t + 2]].uv * scale;

            const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::abs(area) < 1e-8f) {
                continue; // degenerate in UV space
            }
            const float sign = (area > 0.0f) ? 1.0f : -1.0f;

            // Texel centres are at integer+0.5: centre (x+0.5) >= minU  =>  x >= minU-0.5.
            int x0 = static_cast<int>(std::floor(std::min({a.x, b.x, c.x}) - 0.5f));
            int x1 = static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}) - 0.5f));
            int y0 = static_cast<int>(std::floor(std::min({a.y, b.y, c.y}) - 0.5f));
            int y1 = static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}) - 0.5f));
            // A triangle spanning a full wrap already touches every column/row once wrapped —
            // cap the walk so a heavily tiled material can't multiply the raster cost.
            if (x1 - x0 >= w) { x0 = 0; x1 = w - 1; }
            if (y1 - y0 >= h) { y0 = 0; y1 = h - 1; }

            constexpr float kEps = 1e-3f; // texel-space tolerance so shared-edge centres are kept
            // Scanline walk: per row the covered texels form one contiguous interval, and each
            // edge's interval bound is LINEAR in the row coordinate — precomputed here so a row
            // costs one multiply-add per edge. Cost tracks the triangle's actual area plus its
            // bbox HEIGHT, never its bbox area. (The previous per-texel bbox test made a long thin
            // strip triangle — a lash card spanning the atlas — walk millions of background
            // texels, and a 150k-strand fibermesh brow took seconds per map.)
            // Edge (p, q): e(px, py) = ((q.x−p.x)(py−p.y) − (q.y−p.y)(px−p.x)) · sign ≥ −kEps,
            // i.e. m·px + kA + kB·py ≥ −kEps with
            //   m = −sign·(q.y−p.y),  kB = sign·(q.x−p.x),  kA = sign·((q.y−p.y)p.x − (q.x−p.x)p.y)
            // m > 0 ⇒ a lower bound px ≥ (−kEps−kA−kB·py)/m; m < 0 ⇒ an upper bound (flip);
            // m ≈ 0 (horizontal edge) ⇒ an all-or-nothing y-constraint folded into the y-range.
            float loA[3], loB[3], hiA[3], hiB[3];
            int   nLo = 0, nHi = 0;
            float yMin = static_cast<float>(y0) + 0.5f;
            float yMax = static_cast<float>(y1) + 0.5f;
            bool  degenerate = false;
            const glm::vec2 edges[3][2] = {{b, c}, {c, a}, {a, b}};
            for (const auto& e : edges) {
                const glm::vec2& p = e[0];
                const glm::vec2& q = e[1];
                const float m = -sign * (q.y - p.y);
                const float kB = sign * (q.x - p.x);
                const float kA = sign * ((q.y - p.y) * p.x - (q.x - p.x) * p.y);
                if (std::abs(m) > 1e-12f) {
                    const float A = (-kEps - kA) / m;
                    const float B = -kB / m;
                    if (m > 0.0f) {
                        loA[nLo] = A; loB[nLo] = B; ++nLo;
                    } else {
                        hiA[nHi] = A; hiB[nHi] = B; ++nHi;
                    }
                } else if (std::abs(kB) > 1e-12f) {
                    // kB·py ≥ −kEps−kA: clip the y-range instead.
                    const float yBound = (-kEps - kA) / kB;
                    if (kB > 0.0f) {
                        yMin = std::max(yMin, yBound);
                    } else {
                        yMax = std::min(yMax, yBound);
                    }
                } else if (kA < -kEps) {
                    degenerate = true; // constraint never holds
                    break;
                }
            }
            if (degenerate) {
                continue;
            }
            const int ys = std::max(y0, static_cast<int>(std::ceil(yMin - 0.5f)));
            const int ye = std::min(y1, static_cast<int>(std::floor(yMax - 0.5f)));
            for (int y = ys; y <= ye; ++y) {
                const float py = static_cast<float>(y) + 0.5f;
                const int wy = ((y % h) + h) % h;
                float xLo = static_cast<float>(x0) + 0.5f;
                float xHi = static_cast<float>(x1) + 0.5f;
                for (int e = 0; e < nLo; ++e) {
                    xLo = std::max(xLo, loA[e] + loB[e] * py);
                }
                for (int e = 0; e < nHi; ++e) {
                    xHi = std::min(xHi, hiA[e] + hiB[e] * py);
                }
                if (xLo > xHi) {
                    continue;
                }
                const int xs = std::max(x0, static_cast<int>(std::ceil(xLo - 0.5f)));
                const int xe = std::min(x1, static_cast<int>(std::floor(xHi - 0.5f)));
                for (int x = xs; x <= xe; ++x) {
                    const int wx = ((x % w) + w) % w;
                    covered[static_cast<std::size_t>(wy) * w + wx] = 1;
                }
            }
        }
    });
}

// Fills every uncovered texel of `pixels` (tightly-packed RGBA8, w×h) with colour dilated from the
// covered ones via a pull-push pyramid: PULL averages covered children upward level by level, PUSH
// hands colours back down into the gaps. Covered texels are never modified.
void fillFromCoverage(std::vector<uint8_t>& pixels, int w, int h,
                      const std::vector<uint8_t>& covered) {
    // --- PULL: build the pyramid bottom-up. Level 0 is the image itself (implicit).
    std::vector<PyramidLevel> levels;
    int fineW = w, fineH = h;
    while (fineW > 1 || fineH > 1) {
        PyramidLevel level;
        level.w = std::max(1, (fineW + 1) / 2);
        level.h = std::max(1, (fineH + 1) / 2);
        level.rgba.assign(static_cast<std::size_t>(level.w) * level.h * 4, 0);
        level.has.assign(static_cast<std::size_t>(level.w) * level.h, 0);

        const PyramidLevel* fine = levels.empty() ? nullptr : &levels.back();
        const int coarseW = level.w, coarseH = level.h;
        parallelFor(coarseH, [&, fine, fineW, fineH](int y) {
            for (int x = 0; x < coarseW; ++x) {
                uint32_t sum[4] = {0, 0, 0, 0};
                uint32_t cnt = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int cx = std::min(2 * x + dx, fineW - 1);
                        const int cy = std::min(2 * y + dy, fineH - 1);
                        const std::size_t ci = static_cast<std::size_t>(cy) * fineW + cx;
                        const bool has = fine ? (fine->has[ci] != 0) : (covered[ci] != 0);
                        if (!has) {
                            continue;
                        }
                        const uint8_t* src = (fine ? fine->rgba.data() : pixels.data()) + ci * 4;
                        for (int k = 0; k < 4; ++k) {
                            sum[k] += src[k];
                        }
                        ++cnt;
                    }
                }
                const std::size_t ti = static_cast<std::size_t>(y) * coarseW + x;
                if (cnt > 0) {
                    for (int k = 0; k < 4; ++k) {
                        level.rgba[ti * 4 + k] = static_cast<uint8_t>(sum[k] / cnt);
                    }
                    level.has[ti] = 1;
                }
            }
        });
        levels.push_back(std::move(level));
        fineW = levels.back().w;
        fineH = levels.back().h;
    }

    // --- PUSH: from the coarsest level down, plug each level's gaps from its (already filled)
    // parent, then finally fill the base image's uncovered texels from the first pyramid level.
    // The 1×1 top always has a colour (there is at least one covered texel), so this terminates.
    for (int li = static_cast<int>(levels.size()) - 2; li >= 0; --li) {
        PyramidLevel& level = levels[static_cast<std::size_t>(li)];
        const PyramidLevel& parent = levels[static_cast<std::size_t>(li) + 1];
        const int lw = level.w;
        parallelFor(level.h, [&](int y) {
            for (int x = 0; x < lw; ++x) {
                const std::size_t ti = static_cast<std::size_t>(y) * lw + x;
                if (level.has[ti]) {
                    continue;
                }
                const std::size_t pi =
                    static_cast<std::size_t>(std::min(y / 2, parent.h - 1)) * parent.w +
                    std::min(x / 2, parent.w - 1);
                for (int k = 0; k < 4; ++k) {
                    level.rgba[ti * 4 + k] = parent.rgba[pi * 4 + k];
                }
                level.has[ti] = 1;
            }
        });
    }
    const PyramidLevel& first = levels.front();
    parallelFor(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t ti = static_cast<std::size_t>(y) * w + x;
            if (covered[ti]) {
                continue;
            }
            const std::size_t pi = static_cast<std::size_t>(std::min(y / 2, first.h - 1)) * first.w +
                                   std::min(x / 2, first.w - 1);
            for (int k = 0; k < 4; ++k) {
                pixels[ti * 4 + k] = first.rgba[pi * 4 + k];
            }
        }
    });
}

// True if the mesh's referenced UVs span more than kMaxTileSpan tiles on either axis — a tiled/
// wrapping material (lash strips, tileable detail maps), not an atlas. Such a mesh effectively
// samples its whole texture (the sampler REPEATs), so there is no unused background to fill — and
// rasterizing it is pathological: every tile-spanning triangle's raster bbox clamps to the FULL
// image, which turned one figure's tiled lash strip into ~15 s of coverage rasterization for a
// mask that came out fully covered anyway.
bool spansMultipleTiles(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    constexpr float kMaxTileSpan = 2.0f; // an atlas mesh sits in [0,1] (+epsilon); tiled UVs span many
    glm::vec2 lo(std::numeric_limits<float>::max());
    glm::vec2 hi(std::numeric_limits<float>::lowest());
    for (const uint32_t i : indices) {
        const glm::vec2& uv = vertices[i].uv;
        lo = glm::min(lo, uv);
        hi = glm::max(hi, uv);
    }
    return (hi.x - lo.x) > kMaxTileSpan || (hi.y - lo.y) > kMaxTileSpan;
}

} // namespace

void fillImageGutters(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
                      const std::vector<UvMeshRef>& users) {
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    if (w <= 0 || h <= 0 || pixels.size() < static_cast<std::size_t>(w) * h * 4) {
        return;
    }
    for (const UvMeshRef& user : users) {
        if (user.vertices && user.indices && spansMultipleTiles(*user.vertices, *user.indices)) {
            return; // a tiled user leaves no fillable background (and rasterizing it is pathological)
        }
    }
    std::vector<uint8_t> covered(static_cast<std::size_t>(w) * h, 0);
    for (const UvMeshRef& user : users) {
        if (!user.vertices || !user.indices || user.indices->size() < 3 || user.vertices->empty()) {
            continue;
        }
        rasterizeCoverage(*user.vertices, *user.indices, w, h, covered);
    }
    const std::size_t coveredCount =
        static_cast<std::size_t>(std::count(covered.begin(), covered.end(), uint8_t(1)));
    if (coveredCount == 0 || coveredCount == covered.size()) {
        return;
    }
    fillFromCoverage(pixels, w, h, covered);
}

} // namespace pose
