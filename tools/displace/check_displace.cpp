/**
 * @file check_displace.cpp
 * @brief The guard of DISPLACEMENT (src/viewport/scene/meshdisplace.h): a posed mesh subdivided
 *        and moved by a height map on its way to a render engine.
 *
 * On meshes and maps made here: a UV sphere (its seam and its poles duplicated, as a render mesh
 * has them), a plane, a box with hard edges, two planes that share a border; a white, a black
 * and a grey map, a ramp, a step, a one-texel checker. No Qt, no device. CTest: displace-check.
 */

#include "meshdisplace.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using pose::DisplaceReport;
using pose::DisplaceSettings;
using pose::HeightMap;
using pose::PosedMesh;
using pose::SharedPositions;

namespace {

int failures = 0;

void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}

const float kPi = 3.14159265358979f;

/// A UV sphere about the origin: `around` segments round and `down` from pole to pole, the seam
/// column and the pole rows duplicated (one vertex per UV), radial normals.
PosedMesh makeSphere(float radius, int around, int down) {
    PosedMesh m;
    for (int j = 0; j <= down; ++j) {
        const float theta = kPi * static_cast<float>(j) / static_cast<float>(down);
        const float sinTheta = (j == 0 || j == down) ? 0.0f : std::sin(theta);
        const float cosTheta = j == 0 ? 1.0f : j == down ? -1.0f : std::cos(theta);
        for (int i = 0; i <= around; ++i) {
            const float phi = 2.0f * kPi * static_cast<float>(i % around) / static_cast<float>(around); // (the seam: one place)
            const glm::vec3 n(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
            m.positions.push_back(n * radius);
            m.normals.push_back(n);
            m.uvs.emplace_back(static_cast<float>(i) / static_cast<float>(around), static_cast<float>(j) / static_cast<float>(down));
            m.tangents.emplace_back(-std::sin(phi), 0.0f, std::cos(phi), 1.0f);
        }
    }
    const auto at = [around](int i, int j) { return static_cast<uint32_t>(j * (around + 1) + i); };
    for (int j = 0; j < down; ++j) {
        for (int i = 0; i < around; ++i) {
            const uint32_t quad[6] = {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j), at(i + 1, j + 1), at(i, j + 1)};
            m.indices.insert(m.indices.end(), quad, quad + 6);
        }
    }
    return m;
}

/// A level plane of `cells` × `cells` squares over x in [x0, x0 + 1], z in [0, 1], facing +y;
/// u runs with x (from 0 at x0), v with z.
PosedMesh makePlane(int cells, float x0 = 0.0f) {
    PosedMesh m;
    for (int j = 0; j <= cells; ++j) {
        for (int i = 0; i <= cells; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(cells);
            const float v = static_cast<float>(j) / static_cast<float>(cells);
            m.positions.emplace_back(x0 + u, 0.0f, v);
            m.normals.emplace_back(0.0f, 1.0f, 0.0f);
            m.uvs.emplace_back(u, v);
            m.tangents.emplace_back(1.0f, 0.0f, 0.0f, 1.0f);
        }
    }
    const auto at = [cells](int i, int j) { return static_cast<uint32_t>(j * (cells + 1) + i); };
    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            const uint32_t quad[6] = {at(i, j), at(i, j + 1), at(i + 1, j + 1), at(i, j), at(i + 1, j + 1), at(i + 1, j)};
            m.indices.insert(m.indices.end(), quad, quad + 6);
        }
    }
    return m;
}

/// A cube of side 1 about the origin with HARD edges: four vertices a face, the face's normal.
PosedMesh makeBox() {
    PosedMesh       m;
    const glm::vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const glm::vec3& n : axes) {
        const glm::vec3 a = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 b = glm::cross(n, a);
        const uint32_t  first = static_cast<uint32_t>(m.positions.size());
        const float     corners[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
        for (const auto& corner : corners) {
            m.positions.push_back(n * 0.5f + a * corner[0] + b * corner[1]);
            m.normals.push_back(n);
            m.uvs.emplace_back(corner[0] + 0.5f, corner[1] + 0.5f);
            m.tangents.emplace_back(a, 1.0f);
        }
        const uint32_t quad[6] = {first, first + 1, first + 2, first, first + 2, first + 3};
        m.indices.insert(m.indices.end(), quad, quad + 6);
    }
    return m;
}

struct Image {
    std::vector<uint8_t> pixels;
    int                  width = 0;
    int                  height = 0;
    HeightMap            map(int channel = 0) const { return {pixels.data(), width, height, channel}; }
};

template <typename Fn>
Image makeImage(int width, int height, Fn&& value) {
    Image image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width) * height * 4u);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t v = static_cast<uint8_t>(std::clamp(value(x, y), 0, 255));
            uint8_t*      p = &image.pixels[(static_cast<std::size_t>(y) * width + x) * 4u];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    return image;
}

Image flat(int value) {
    return makeImage(8, 8, [value](int, int) { return value; });
}

bool sameMesh(const PosedMesh& a, const PosedMesh& b) {
    const auto same = [](const auto& x, const auto& y) {
        return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
    };
    return same(a.positions, b.positions) && same(a.normals, b.normals) && same(a.uvs, b.uvs) && same(a.tangents, b.tangents) &&
           same(a.indices, b.indices);
}

/// The largest |radius - wanted| over the vertices, and the largest angle (degrees) between a
/// vertex's normal and its radial direction.
void sphereError(const PosedMesh& m, float wanted, float& radiusError, float& normalError) {
    radiusError = 0.0f;
    normalError = 0.0f;
    for (std::size_t v = 0; v < m.positions.size(); ++v) {
        const float r = glm::length(m.positions[v]);
        radiusError = std::max(radiusError, std::abs(r - wanted));
        const float c = std::clamp(glm::dot(m.normals[v], m.positions[v] / r), -1.0f, 1.0f);
        normalError = std::max(normalError, std::acos(c) * 180.0f / kPi);
    }
}

/// The largest distance between two vertices of @p after whose places in @p before were the same
/// (a seam's two sides): how far a displacement tore the surface.
float largestTear(const PosedMesh& before, const PosedMesh& after) {
    float tear = 0.0f;
    for (std::size_t a = 0; a < before.positions.size(); ++a) {
        for (std::size_t b = a + 1; b < before.positions.size(); ++b) {
            if (before.positions[a] == before.positions[b]) {
                tear = std::max(tear, glm::length(after.positions[a] - after.positions[b]));
            }
        }
    }
    return tear;
}

void checkNothingToDo() {
    const PosedMesh source = makeSphere(1.0f, 16, 8);
    const Image     white = flat(255);
    DisplaceSettings s;
    s.scale = 0.05f;
    s.midLevel = 0.0f;

    PosedMesh m = source;
    check(!pose::displaceMesh(m, HeightMap(), s).displaced && sameMesh(m, source), "no map: the mesh is left exactly as it came");
    m = source;
    DisplaceSettings zero = s;
    zero.scale = 0.0f;
    zero.levels = 3;
    check(!pose::displaceMesh(m, white.map(), zero).displaced && sameMesh(m, source), "no strength: nothing is moved, and nothing subdivided");
    PosedMesh broken = source;
    broken.indices[5] = 1000000;
    const PosedMesh brokenBefore = broken;
    check(!pose::displaceMesh(broken, white.map(), s).displaced && sameMesh(broken, brokenBefore), "a mesh that names a vertex it has not is left alone");
}

void checkAmounts() {
    const PosedMesh source = makeSphere(1.0f, 32, 16);
    const Image     white = flat(255);
    const Image     black = flat(0);
    DisplaceSettings s;
    s.scale = 0.05f;
    s.midLevel = 0.0f;

    PosedMesh      m = source;
    DisplaceReport r = pose::displaceMesh(m, white.map(), s);
    float          radiusError = 0.0f;
    float          normalError = 0.0f;
    sphereError(m, 1.05f, radiusError, normalError);
    check(r.displaced && radiusError < 1.0e-5f && std::abs(r.maxOffset - 0.05f) < 1.0e-5f, "a white map from a mid level of 0 moves every point out by the strength");
    check(normalError < 0.05f && m.indices == source.indices && m.positions.size() == source.positions.size(),
          "… along its normal, the normals as they were, and with no subdivision asked the mesh keeps its vertices");

    m = source;
    s.midLevel = 1.0f;
    pose::displaceMesh(m, white.map(), s);
    sphereError(m, 1.0f, radiusError, normalError);
    check(radiusError < 1.0e-6f, "a map at the mid level moves nothing");

    m = source;
    s.midLevel = 0.5f;
    pose::displaceMesh(m, black.map(), s);
    sphereError(m, 0.975f, radiusError, normalError);
    check(radiusError < 1.0e-5f, "darker than the mid level sinks in: black from 0.5 by half the strength");

    m = source;
    s.scale = -0.05f;
    pose::displaceMesh(m, black.map(), s);
    sphereError(m, 1.025f, radiusError, normalError);
    check(radiusError < 1.0e-5f, "a negative strength inverts the map");

    // The height in ONE channel of a packed image.
    Image packed = flat(0);
    for (std::size_t i = 1; i < packed.pixels.size(); i += 4) {
        packed.pixels[i] = 255; // green
    }
    m = source;
    s.scale = 0.05f;
    s.midLevel = 0.0f;
    pose::displaceMesh(m, packed.map(1), s);
    sphereError(m, 1.05f, radiusError, normalError);
    PosedMesh red = source;
    pose::displaceMesh(red, packed.map(0), s);
    float redError = 0.0f;
    sphereError(red, 1.0f, redError, normalError);
    check(radiusError < 1.0e-5f && redError < 1.0e-6f, "the height is read from the channel named");
}

void checkSubdivision() {
    const PosedMesh source = makeSphere(1.0f, 16, 8);
    const Image     grey = flat(128);
    DisplaceSettings s;
    s.scale = 0.05f;
    s.midLevel = 128.0f / 255.0f; // (the map's own value: nothing moves)
    s.levels = 2;

    PosedMesh      m = source;
    DisplaceReport r = pose::displaceMesh(m, grey.map(), s);
    const std::size_t triangles = source.indices.size() / 3;
    check(r.displaced && r.levels == 2 && r.triangles == triangles * 16 && m.indices.size() == source.indices.size() * 16,
          "two levels cut every triangle into sixteen");
    bool kept = m.positions.size() > source.positions.size();
    for (std::size_t v = 0; v < source.positions.size() && kept; ++v) {
        kept = glm::length(m.positions[v] - source.positions[v]) < 1.0e-6f && glm::length(m.normals[v] - source.normals[v]) < 1.0e-5f &&
               m.uvs[v] == source.uvs[v];
    }
    check(kept, "… the mesh's own vertices stay where they were, with their normals and UVs");
    // Linear: every new vertex lies INSIDE the sphere's polyhedron (on a face), none outside the
    // sphere, and its normal is a unit vector near the radial one.
    bool  inside = true;
    float worst = 0.0f;
    for (std::size_t v = source.positions.size(); v < m.positions.size(); ++v) {
        const float radius = glm::length(m.positions[v]);
        inside = inside && radius <= 1.0f + 1.0e-5f && radius > 0.9f && std::abs(glm::length(m.normals[v]) - 1.0f) < 1.0e-4f;
        worst = std::max(worst, std::acos(std::clamp(glm::dot(m.normals[v], m.positions[v] / radius), -1.0f, 1.0f)) * 180.0f / kPi);
    }
    check(inside && worst < 1.0f, "the new vertices lie on the faces (the shape is not rounded) and carry the surface's smooth normals");
    check(r.points < r.vertices && r.pinned == 0, "the seam's and the poles' vertices are welded into points; nothing is held");

    // The budget.
    m = source;
    s.levels = 5;
    s.maxTriangles = triangles * 16 + 1;
    r = pose::displaceMesh(m, grey.map(), s);
    check(r.levels == 2 && r.triangles == triangles * 16, "over the triangle budget the levels are reduced until the mesh fits");
    m = source;
    s.levels = 99;
    s.maxTriangles = pose::kDisplaceMaxTriangles;
    r = pose::displaceMesh(m, grey.map(), s);
    check(r.levels == pose::kDisplaceMaxLevels, "… and never more than five");
}

void checkSeam() {
    // A ramp along u: the two sides of the sphere's seam read black and white.
    const Image ramp = makeImage(256, 4, [](int x, int) { return x; });
    for (const int levels : {0, 2}) {
        PosedMesh source = makeSphere(1.0f, 24, 12);
        DisplaceSettings s;
        s.scale = 0.2f;
        s.midLevel = 0.0f;
        s.levels = levels;
        // (The subdivided mesh to compare places against: the same subdivision with nothing moved.)
        PosedMesh        still = source;
        DisplaceSettings none = s;
        none.midLevel = 0.5f;
        const Image grey = makeImage(4, 4, [](int, int) { return 128; });
        none.midLevel = 128.0f / 255.0f;
        pose::displaceMesh(still, grey.map(), none);
        PosedMesh moved = source;
        const DisplaceReport r = pose::displaceMesh(moved, ramp.map(), s);
        const float          tear = largestTear(still, moved);
        check(r.displaced && r.maxOffset > 0.05f && tear == 0.0f,
              levels == 0 ? "a UV seam whose two sides read different heights stays closed" : "… and stays closed through the subdivision");
    }
}

void checkNearSeam() {
    // A seam whose two sides differ in the LAST DIGIT (a mesh made by arithmetic: a sphere's seam
    // at 0 and at two pi) is still one seam.
    const Image ramp = makeImage(256, 4, [](int x, int) { return x; });
    PosedMesh   source = makeSphere(1.0f, 24, 12);
    const int   around = 24;
    for (int j = 0; j <= 12; ++j) {
        glm::vec3& p = source.positions[static_cast<std::size_t>(j * (around + 1) + around)];
        for (int k = 0; k < 3; ++k) {
            p[k] = std::nextafter(p[k], 10.0f);
        }
    }
    DisplaceSettings s;
    s.scale = 0.2f;
    s.midLevel = 0.0f;
    s.levels = 1;
    PosedMesh moved = source;
    pose::displaceMesh(moved, ramp.map(), s);
    // (The mesh's own vertices keep their indices: the seam's two columns.)
    float gap = 0.0f;
    for (int j = 0; j <= 12; ++j) {
        gap = std::max(gap, glm::length(moved.positions[static_cast<std::size_t>(j * (around + 1))] -
                                        moved.positions[static_cast<std::size_t>(j * (around + 1) + around)]));
    }
    check(gap < 1.0e-5f, "a seam whose two sides differ in the last digit is welded all the same (a micrometre's tolerance)");
}

void checkBox() {
    const Image white = flat(255);
    for (const int levels : {0, 2}) {
        PosedMesh        m = makeBox();
        DisplaceSettings s;
        s.scale = 0.1f;
        s.midLevel = 0.0f;
        s.levels = levels;
        const DisplaceReport r = pose::displaceMesh(m, white.map(), s);
        // Every face moved out by 0.1: the box is 1.2 wide, its edges and corners where the
        // displaced faces meet.
        bool  crisp = r.displaced;
        float normalError = 0.0f;
        for (std::size_t v = 0; v < m.positions.size(); ++v) {
            const glm::vec3& p = m.positions[v];
            const glm::vec3& n = m.normals[v];
            // (The vertex is on the face its normal names: that coordinate is the face's.)
            const int axis = std::abs(n.x) > 0.5f ? 0 : std::abs(n.y) > 0.5f ? 1 : 2;
            crisp = crisp && std::abs(std::abs(p[axis]) - 0.6f) < 1.0e-5f;
            for (int k = 0; k < 3; ++k) {
                crisp = crisp && std::abs(p[k]) < 0.6f + 1.0e-5f;
            }
            normalError = std::max(normalError, 1.0f - std::abs(n[axis]));
        }
        float corner = 0.0f;
        for (const glm::vec3& p : m.positions) {
            corner = std::max(corner, std::min(std::abs(p.x), std::min(std::abs(p.y), std::abs(p.z))));
        }
        check(crisp && normalError < 1.0e-5f && std::abs(corner - 0.6f) < 1.0e-5f,
              levels == 0 ? "a box displaced evenly is a larger box: its hard edges go where the displaced faces meet"
                          : "… and its subdivided faces stay flat, its edges closed");
    }
}

void checkSharedBorder() {
    const Image white = flat(255);
    DisplaceSettings s;
    s.scale = 0.1f;
    s.midLevel = 0.0f;
    for (const int levels : {0, 2}) {
        s.levels = levels;
        PosedMesh       a = makePlane(4);
        const PosedMesh b = makePlane(4, 1.0f); // (shares the edge x = 1 with a)
        SharedPositions shared;
        shared.addMesh(a.positions);
        shared.addMesh(b.positions);
        const DisplaceReport r = pose::displaceMesh(a, white.map(), s, &shared);
        bool held = r.displaced && r.pinned > 0;
        bool moved = true;
        for (const glm::vec3& p : a.positions) {
            if (p.x == 1.0f) {
                held = held && p.y == 0.0f;
            } else {
                moved = moved && std::abs(p.y - 0.1f) < 1.0e-6f;
            }
        }
        check(held && moved, levels == 0 ? "a border shared with another zone is held; the rest moves, a free border too"
                                         : "… and the vertices subdivision puts on that border are held with it");
    }
    s.levels = 1;
    PosedMesh alone = makePlane(4);
    const DisplaceReport r = pose::displaceMesh(alone, white.map(), s);
    bool                 all = r.pinned == 0;
    for (const glm::vec3& p : alone.positions) {
        all = all && std::abs(p.y - 0.1f) < 1.0e-6f;
    }
    check(all, "a mesh that stands alone moves whole");

    // A mesh's own duplicates are not "another mesh".
    SharedPositions own;
    const PosedMesh sphere = makeSphere(1.0f, 8, 4);
    own.addMesh(sphere.positions);
    bool none = true;
    for (const glm::vec3& p : sphere.positions) {
        none = none && !own.shared(p);
    }
    own.addMesh(sphere.positions);
    check(none && own.shared(sphere.positions[5]) && !own.shared(glm::vec3(9.0f)), "a place is shared when two MESHES have a vertex there");
}

void checkRelief() {
    // A ramp along u on a plane: the surface tilts, and its normals with it.
    const Image ramp = makeImage(256, 4, [](int x, int) { return x; });
    PosedMesh   m = makePlane(16);
    DisplaceSettings s;
    s.scale = 0.5f; // y = 0.5 × u: a slope of one half
    s.midLevel = 0.0f;
    pose::displaceMesh(m, ramp.map(), s);
    const glm::vec3 wanted = glm::normalize(glm::vec3(-0.5f, 1.0f, 0.0f));
    float           worst = 0.0f;
    float           square = 0.0f;
    float           height = 0.0f;
    for (std::size_t v = 0; v < m.positions.size(); ++v) {
        const float u = m.uvs[v].x;
        if (u < 0.1f || u > 0.9f) {
            continue; // (the map wraps: its two ends are averaged by the filter)
        }
        worst = std::max(worst, std::acos(std::clamp(glm::dot(m.normals[v], wanted), -1.0f, 1.0f)) * 180.0f / kPi);
        square = std::max(square, std::abs(glm::dot(glm::vec3(m.tangents[v]), m.normals[v])));
        height = std::max(height, std::abs(m.positions[v].y - 0.5f * u));
    }
    check(height < 0.01f && worst < 1.5f, "relief turns the shading normals: a ramp tilts the plane and its normals with it");
    check(square < 1.0e-4f, "… and the tangents stay square to them");
}

void checkUvTransform() {
    // Left half black, right half white.
    const Image step = makeImage(64, 4, [](int x, int) { return x < 32 ? 0 : 255; });
    const auto  heightAt = [](const PosedMesh& m, float u, float v) {
        float best = 1.0e9f;
        float y = 0.0f;
        for (std::size_t i = 0; i < m.positions.size(); ++i) {
            const float d = std::abs(m.uvs[i].x - u) + std::abs(m.uvs[i].y - v);
            if (d < best) {
                best = d;
                y = m.positions[i].y;
            }
        }
        return y;
    };
    DisplaceSettings s;
    s.scale = 1.0f;
    s.midLevel = 0.0f;

    PosedMesh plain = makePlane(16);
    pose::displaceMesh(plain, step.map(), s);
    check(heightAt(plain, 0.25f, 0.5f) < 0.05f && heightAt(plain, 0.75f, 0.5f) > 0.95f, "the map as painted: black on the left, white on the right");

    PosedMesh tiled = makePlane(16);
    s.uvTiling = glm::vec2(2.0f, 1.0f);
    pose::displaceMesh(tiled, step.map(), s);
    check(heightAt(tiled, 0.125f, 0.5f) < 0.05f && heightAt(tiled, 0.375f, 0.5f) > 0.95f && heightAt(tiled, 0.625f, 0.5f) < 0.05f &&
              heightAt(tiled, 0.875f, 0.5f) > 0.95f,
          "the material's tiling repeats it");

    PosedMesh shifted = makePlane(16);
    s.uvTiling = glm::vec2(1.0f);
    s.uvOffset = glm::vec2(0.5f, 0.0f);
    pose::displaceMesh(shifted, step.map(), s);
    check(heightAt(shifted, 0.25f, 0.5f) > 0.95f && heightAt(shifted, 0.75f, 0.5f) < 0.05f, "… its offset moves it");

    PosedMesh turned = makePlane(16);
    s.uvOffset = glm::vec2(0.0f);
    s.uvRotation = kPi * 0.5f; // (mesh.vert: u' = c·u + s·v = v)
    pose::displaceMesh(turned, step.map(), s);
    check(heightAt(turned, 0.5f, 0.25f) < 0.05f && heightAt(turned, 0.5f, 0.75f) > 0.95f, "… and its rotation turns it, as the vertex shader turns the UVs");
}

void checkFiltering() {
    // A one-texel checker on a mesh far coarser than it: the mean, not a coin's toss.
    const Image checker = makeImage(256, 256, [](int x, int y) { return ((x + y) & 1) != 0 ? 255 : 0; });
    PosedMesh   m = makePlane(4);
    DisplaceSettings s;
    s.scale = 1.0f;
    s.midLevel = 0.0f;
    const DisplaceReport r = pose::displaceMesh(m, checker.map(), s);
    float                worst = 0.0f;
    for (const glm::vec3& p : m.positions) {
        worst = std::max(worst, std::abs(p.y - 0.5f));
    }
    check(r.mipLevel >= 4 && worst < 0.02f, "a map finer than the mesh is averaged down to it, not point-sampled");

    // … and a mesh as fine as the map reads it whole.
    const Image halves = makeImage(8, 8, [](int x, int) { return x < 4 ? 0 : 255; });
    PosedMesh   fine = makePlane(16);
    const DisplaceReport fineReport = pose::displaceMesh(fine, halves.map(), s);
    check(fineReport.mipLevel == 0, "a mesh as fine as the map samples it as it is");
}

} // namespace

int main() {
    checkNothingToDo();
    checkAmounts();
    checkSubdivision();
    checkSeam();
    checkNearSeam();
    checkBox();
    checkSharedBorder();
    checkRelief();
    checkUvTransform();
    checkFiltering();
    std::printf("%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
