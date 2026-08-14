/**
 * @file texturegutters.cpp
 * @brief Implementation of fillTextureGutters. See texturegutters.h.
 */

#include "texturegutters.h"

#include "modeldata.h"
#include "parallelfor.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

// Rasterizes the mesh's UV triangles into a per-texel coverage mask. A texel (x, y) samples
// uv = ((x+0.5)/w, (y+0.5)/h), so its centre (x+0.5, y+0.5) is tested against the triangles
// scaled into texel space. The runtime sampler REPEATs, so out-of-range UVs wrap into the mask.
std::vector<uint8_t> rasterizeCoverage(const std::vector<Vertex>& vertices,
                                       const std::vector<uint32_t>& indices, int w, int h) {
    std::vector<uint8_t> covered(static_cast<std::size_t>(w) * h, 0);
    const glm::vec2 scale(static_cast<float>(w), static_cast<float>(h));
    const int triCount = static_cast<int>(indices.size() / 3);

    // Parallel over triangle blocks: concurrent writes only ever store the same value (1), so the
    // races are benign.
    constexpr int kBlock = 256;
    const int blocks = (triCount + kBlock - 1) / kBlock;
    parallelFor(blocks, [&](int block) {
        const int tEnd = std::min(triCount, (block + 1) * kBlock);
        for (int t = block * kBlock; t < tEnd; ++t) {
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
            for (int y = y0; y <= y1; ++y) {
                const float py = static_cast<float>(y) + 0.5f;
                const int wy = ((y % h) + h) % h;
                for (int x = x0; x <= x1; ++x) {
                    const float px = static_cast<float>(x) + 0.5f;
                    const float e0 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * sign;
                    const float e1 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) * sign;
                    const float e2 = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * sign;
                    if (e0 >= -kEps && e1 >= -kEps && e2 >= -kEps) {
                        const int wx = ((x % w) + w) % w;
                        covered[static_cast<std::size_t>(wy) * w + wx] = 1;
                    }
                }
            }
        }
    });
    return covered;
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

// Gutter-fills one decoded image against the mesh's UV coverage. Skips work when the coverage is
// empty (nothing to pull colour from) or complete (nothing to fill — e.g. tiled materials).
void fillImage(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
               const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    if (w <= 0 || h <= 0 || pixels.size() < static_cast<std::size_t>(w) * h * 4) {
        return;
    }
    const std::vector<uint8_t> covered = rasterizeCoverage(vertices, indices, w, h);
    const std::size_t coveredCount =
        static_cast<std::size_t>(std::count(covered.begin(), covered.end(), uint8_t(1)));
    if (coveredCount == 0 || coveredCount == covered.size()) {
        return;
    }
    fillFromCoverage(pixels, w, h, covered);
}

} // namespace

void fillTextureGutters(MeshData& mesh) {
    if (mesh.indices.size() < 3 || mesh.vertices.empty()) {
        return;
    }
    if (!mesh.diffusePixels.empty()) {
        fillImage(mesh.diffusePixels, mesh.diffuseWidth, mesh.diffuseHeight, mesh.vertices,
                  mesh.indices);
    }
    // The detail (normal/bump) map bleeds the same way — it shows as lighting seams rather than
    // white lines — and dilating its bytes is equally valid (the fill only touches unused texels).
    if (!mesh.normalPixels.empty()) {
        fillImage(mesh.normalPixels, mesh.normalWidth, mesh.normalHeight, mesh.vertices,
                  mesh.indices);
    }
}

} // namespace pose
