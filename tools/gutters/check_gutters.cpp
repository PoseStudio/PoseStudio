// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file check_gutters.cpp
 * @brief The guard of the import's gutter fill (src/viewport/import/texturegutters.h): every
 *        texel a UV triangle covers is left as decoded, the rest is filled from the islands.
 *
 * Made for the defect of 2026-11-14: a quad that fills the WHOLE UV square — its corners on 0 and
 * 1, a cube's or a plane's default mapping — lost half of every map at import, because the
 * duplicate-triangle test folded a UV of 1 onto 0 and took the quad's second triangle for a copy
 * of its first. On images and UV layouts made here. No Qt, no device (the Vulkan HEADERS only:
 * the vertex layout names them). CTest: gutter-check.
 */

#include "texturegutters.h"
#include "vertex.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <cstdio>
#include <vector>

using pose::UvMeshRef;
using pose::Vertex;

namespace {

int failures = 0;

void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}

constexpr int kSize = 64;

/// An image no fill could reproduce: every texel its own color.
std::vector<uint8_t> pattern() {
    std::vector<uint8_t> pixels(static_cast<std::size_t>(kSize) * kSize * 4);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            uint8_t* t = pixels.data() + (static_cast<std::size_t>(y) * kSize + x) * 4;
            t[0] = static_cast<uint8_t>(x * 4);
            t[1] = static_cast<uint8_t>(y * 4);
            t[2] = static_cast<uint8_t>((x * 7 + y * 13) & 0xff);
            t[3] = 255;
        }
    }
    return pixels;
}

struct Layout {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
};

/// A quad over the UV rectangle (u0, v0)-(u1, v1), as two triangles with corners of their own.
void addQuad(Layout& l, float u0, float v0, float u1, float v1) {
    const uint32_t  first = static_cast<uint32_t>(l.vertices.size());
    const glm::vec2 corners[4] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    for (const glm::vec2& c : corners) {
        Vertex v;
        v.uv = c;
        l.vertices.push_back(v);
    }
    l.indices.insert(l.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
}

/// How many texels of the rectangle [x0, x1) x [y0, y1) differ between @p a and @p b.
int changed(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int x0, int y0, int x1, int y1) {
    int count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
            count += (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3]) ? 1 : 0;
        }
    }
    return count;
}

std::vector<uint8_t> filled(const Layout& l) {
    std::vector<uint8_t> pixels = pattern();
    pose::fillImageGutters(pixels, kSize, kSize, {UvMeshRef{&l.vertices, &l.indices}});
    return pixels;
}

} // namespace

int main() {
    const std::vector<uint8_t> original = pattern();
    const int                  all = kSize * kSize;

    // THE DEFECT: a quad over the whole square. Nothing of the image is background.
    Layout whole;
    addQuad(whole, 0.0f, 0.0f, 1.0f, 1.0f);
    check(changed(filled(whole), original, 0, 0, kSize, kSize) == 0, "a quad over the whole UV square leaves the image as it is");

    // A cube: six such quads, one on top of the other.
    Layout cube;
    for (int f = 0; f < 6; ++f) {
        addQuad(cube, 0.0f, 0.0f, 1.0f, 1.0f);
    }
    check(changed(filled(cube), original, 0, 0, kSize, kSize) == 0, "six of them (a cube's faces) too");

    // The same quad a tile over, and one that straddles the tile's edge: the sampler wraps.
    Layout shifted;
    addQuad(shifted, 1.0f, -1.0f, 2.0f, 0.0f);
    check(changed(filled(shifted), original, 0, 0, kSize, kSize) == 0, "the whole square one tile over is the whole image as well");
    Layout straddling;
    addQuad(straddling, 0.5f, 0.0f, 1.5f, 1.0f);
    check(changed(filled(straddling), original, 0, 0, kSize, kSize) == 0, "a quad a tile wide that straddles the edge covers every texel");

    // Half the square is an island; the other half is background and is filled from it.
    Layout half;
    addQuad(half, 0.0f, 0.0f, 0.5f, 1.0f);
    const std::vector<uint8_t> halfFilled = filled(half);
    check(changed(halfFilled, original, 0, 0, kSize / 2, kSize) == 0, "the left half as an island is left texel for texel");
    const int right = changed(halfFilled, original, kSize / 2, 0, kSize, kSize);
    std::printf("     (%d of %d texels of the other half were filled)\n", right, all / 2);
    check(right > all / 2 * 9 / 10, "... and the background beside it is filled");

    // Each triangle of the half-square quad by itself covers its own half of the island — the
    // duplicate test must tell two triangles that share two corners apart everywhere, not only
    // on the square's border.
    Layout tri;
    {
        Layout quad;
        addQuad(quad, 0.0f, 0.0f, 1.0f, 1.0f);
        tri.vertices = quad.vertices;
        tri.indices.assign(quad.indices.begin(), quad.indices.begin() + 3);
    }
    const std::vector<uint8_t> triFilled = filled(tri);
    int                        kept = 0, lost = 0;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const bool inside = x > y + 1; // (0,0) (1,0) (1,1): right of the diagonal, a texel clear of it
            const bool outside = x < y - 1;
            const bool same = changed(triFilled, original, x, y, x + 1, y + 1) == 0;
            kept += (inside && !same) ? 1 : 0;
            lost += (outside && same) ? 1 : 0;
        }
    }
    check(kept == 0, "one triangle of the square: every texel inside it is left");
    check(lost < all / 100, "... and what lies across its diagonal is background");

    // Copies of one triangle in other tiles are one triangle (the fill's result is the same).
    Layout tiled = half;
    addQuad(tiled, 1.0f, 0.0f, 1.5f, 1.0f);
    addQuad(tiled, 0.0f, -1.0f, 0.5f, 0.0f);
    check(changed(filled(tiled), halfFilled, 0, 0, kSize, kSize) == 0, "copies of an island in other tiles change nothing");

    // A mesh tiled many times over leaves no background: the image is not touched.
    Layout many;
    addQuad(many, 0.0f, 0.0f, 8.0f, 0.4f);
    check(changed(filled(many), original, 0, 0, kSize, kSize) == 0, "a mesh that spans many tiles is not filled");

    std::printf("%d failed\n", failures);
    return failures;
}
