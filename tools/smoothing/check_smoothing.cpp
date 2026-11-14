/**
 * @file check_smoothing.cpp
 * @brief The guard of THE SMOOTHING BY ANGLE (src/viewport/import/smoothnormals.h): a mesh's
 *        shading normals computed from its faces — what makes a faceted file round — and the
 *        import's rule for which meshes get it.
 *
 * On meshes made here: a sphere with a normal per face (a modelling package's default export), a
 * cube, a hexagonal prism, a capped cylinder. No Qt, no device (the Vulkan HEADERS only: the
 * vertex layout names them). CTest: smoothing-check.
 */

#include "modeldata.h"
#include "smoothnormals.h"
#include "tangentgen.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using pose::MeshData;
using pose::ModelData;
using pose::SmoothedMesh;
using pose::SmoothMesh;
using pose::Vertex;

namespace {

int failures = 0;

void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        ++failures;
    }
}

const float kPi = 3.14159265358979f;

struct Shape {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
};

glm::vec3 faceNormal(const Shape& s, std::size_t triangle) {
    const glm::vec3 a = s.vertices[s.indices[triangle * 3]].pos;
    const glm::vec3 b = s.vertices[s.indices[triangle * 3 + 1]].pos;
    const glm::vec3 c = s.vertices[s.indices[triangle * 3 + 2]].pos;
    return glm::normalize(glm::cross(b - a, c - a));
}

/// Adds a polygon (counter-clockwise seen from outside) as a fan, every corner a vertex of its
/// own carrying the polygon's normal — what a file with a normal per face imports as.
void addFlatPolygon(Shape& s, const std::vector<glm::vec3>& corners, const std::vector<glm::vec2>& uvs = {}) {
    const glm::vec3 n = glm::normalize(glm::cross(corners[1] - corners[0], corners[2] - corners[0]));
    const uint32_t  first = static_cast<uint32_t>(s.vertices.size());
    for (std::size_t i = 0; i < corners.size(); ++i) {
        Vertex v;
        v.pos = corners[i];
        v.normal = n;
        v.uv = i < uvs.size() ? uvs[i] : glm::vec2(0.0f);
        s.vertices.push_back(v);
    }
    for (uint32_t i = 1; i + 1 < corners.size(); ++i) {
        s.indices.insert(s.indices.end(), {first, first + i, first + i + 1});
    }
}

glm::vec3 spherePoint(int i, int j, int around, int down) {
    const float th = kPi * static_cast<float>(j) / static_cast<float>(down);
    const float ph = 2.0f * kPi * static_cast<float>(i % around) / static_cast<float>(around);
    if (j == 0) {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }
    if (j == down) {
        return glm::vec3(0.0f, -1.0f, 0.0f);
    }
    return glm::vec3(std::sin(th) * std::cos(ph), std::cos(th), -std::sin(th) * std::sin(ph));
}

/// A unit sphere with a normal per face. @p rows: the latitude bands to make (all by default) —
/// two halves are two zones of one model. @p withUv: a seam down one meridian.
Shape flatSphere(int around = 32, int down = 16, int fromRow = 0, int toRow = -1, bool withUv = false) {
    Shape s;
    if (toRow < 0) {
        toRow = down;
    }
    for (int j = fromRow; j < toRow; ++j) {
        for (int i = 0; i < around; ++i) {
            const glm::vec3 a = spherePoint(i, j, around, down), b = spherePoint(i + 1, j, around, down);
            const glm::vec3 c = spherePoint(i + 1, j + 1, around, down), d = spherePoint(i, j + 1, around, down);
            const float     u0 = static_cast<float>(i) / around, u1 = static_cast<float>(i + 1) / around;
            const float     v0 = static_cast<float>(j) / down, v1 = static_cast<float>(j + 1) / down;
            if (j == 0) {
                addFlatPolygon(s, {a, d, c}, withUv ? std::vector<glm::vec2>{{u0, v0}, {u0, v1}, {u1, v1}} : std::vector<glm::vec2>{});
            } else if (j == down - 1) {
                addFlatPolygon(s, {a, d, b}, withUv ? std::vector<glm::vec2>{{u0, v0}, {u0, v1}, {u1, v0}} : std::vector<glm::vec2>{});
            } else {
                addFlatPolygon(s, {a, d, c, b},
                               withUv ? std::vector<glm::vec2>{{u0, v0}, {u0, v1}, {u1, v1}, {u1, v0}} : std::vector<glm::vec2>{});
            }
        }
    }
    return s;
}

/// A cube of half side 1 with a normal per face (24 vertices).
Shape flatCube() {
    Shape           s;
    const glm::vec3 p[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    const int       f[6][4] = {{4, 5, 6, 7}, {1, 0, 3, 2}, {5, 1, 2, 6}, {0, 4, 7, 3}, {7, 6, 2, 3}, {0, 1, 5, 4}};
    for (const auto& face : f) {
        addFlatPolygon(s, {p[face[0]], p[face[1]], p[face[2]], p[face[3]]});
    }
    return s;
}

/// The same cube with ONE vertex per corner and a normal along the diagonal — what a reader
/// that makes its own normals for a file without any hands over.
Shape sharedCube() {
    Shape           s;
    const glm::vec3 p[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    const int       f[6][4] = {{4, 5, 6, 7}, {1, 0, 3, 2}, {5, 1, 2, 6}, {0, 4, 7, 3}, {7, 6, 2, 3}, {0, 1, 5, 4}};
    for (const glm::vec3& corner : p) {
        Vertex v;
        v.pos = corner;
        v.normal = glm::normalize(corner);
        s.vertices.push_back(v);
    }
    for (const auto& face : f) {
        s.indices.insert(s.indices.end(), {static_cast<uint32_t>(face[0]), static_cast<uint32_t>(face[1]), static_cast<uint32_t>(face[2]),
                                           static_cast<uint32_t>(face[0]), static_cast<uint32_t>(face[2]), static_cast<uint32_t>(face[3])});
    }
    return s;
}

/// A prism of @p sides about the y axis with its two caps, a normal per face.
Shape flatPrism(int sides) {
    Shape                  s;
    std::vector<glm::vec3> top, bottom;
    for (int i = 0; i < sides; ++i) {
        const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(sides);
        top.emplace_back(std::cos(a), 1.0f, -std::sin(a));
        bottom.emplace_back(std::cos(a), -1.0f, -std::sin(a));
    }
    for (int i = 0; i < sides; ++i) {
        const int j = (i + 1) % sides;
        addFlatPolygon(s, {bottom[i], bottom[j], top[j], top[i]});
    }
    addFlatPolygon(s, top);
    std::vector<glm::vec3> under(bottom.rbegin(), bottom.rend());
    addFlatPolygon(s, under);
    return s;
}

std::vector<SmoothedMesh> smoothOne(const Shape& s, float angle) {
    SmoothMesh mesh;
    mesh.vertices = &s.vertices;
    mesh.indices = &s.indices;
    mesh.smooth = true;
    mesh.angleDeg = angle;
    return pose::smoothByAngle({mesh});
}

Shape shapeOf(const SmoothedMesh& m) {
    return {m.vertices, m.indices};
}

/// The least cosine between a vertex's normal and @p expected(vertex) over the mesh.
template <typename F> float worstAgainst(const Shape& s, F expected) {
    float worst = 1.0f;
    for (const Vertex& v : s.vertices) {
        worst = std::min(worst, glm::dot(glm::normalize(v.normal), glm::normalize(expected(v))));
    }
    return worst;
}

/// Every triangle's corners at the places they were (the smoothing moves no point).
bool samePlaces(const Shape& a, const Shape& b) {
    if (a.indices.size() != b.indices.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.indices.size(); ++i) {
        if (a.vertices[a.indices[i]].pos != b.vertices[b.indices[i]].pos) {
            return false;
        }
    }
    return true;
}

void checkSphere() {
    std::printf("-- a sphere with a normal per face\n");
    const Shape sphere = flatSphere();
    check(pose::hasOnlyFaceNormals(sphere.vertices, sphere.indices), "it is seen as a file with no smoothing of its own");
    const std::vector<SmoothedMesh> out = smoothOne(sphere, 60.0f);
    const Shape                     round = shapeOf(out[0]);
    const float                     radial = worstAgainst(round, [](const Vertex& v) { return v.pos; });
    std::printf("     (smoothed: %zu vertices of %zu; the normal furthest from radial is %.2f degrees off)\n", round.vertices.size(),
                sphere.vertices.size(), glm::degrees(std::acos(std::min(radial, 1.0f))));
    check(radial > 0.999f, "smoothed, its normals are the sphere's own: radial");
    check(samePlaces(sphere, round), "no point moved, the triangles in the file's order");
    check(round.vertices.size() == 32u * 15u + 2u, "the corners that meet at a point are one vertex again (482 points)");
    check(!pose::hasOnlyFaceNormals(round.vertices, round.indices), "... and it no longer reads as faceted");
    bool origins = out[0].origin.size() == round.vertices.size();
    for (std::size_t i = 0; origins && i < out[0].origin.size(); ++i) {
        origins = out[0].origin[i] < sphere.vertices.size() && sphere.vertices[out[0].origin[i]].pos == round.vertices[i].pos;
    }
    check(origins, "each new vertex names the file vertex it came from");

    const Shape faceted = shapeOf(smoothOne(sphere, 0.0f)[0]);
    bool        flat = faceted.indices.size() == sphere.indices.size();
    for (std::size_t t = 0; flat && t < faceted.indices.size() / 3; ++t) {
        for (int c = 0; c < 3; ++c) {
            flat = flat && glm::dot(faceted.vertices[faceted.indices[t * 3 + c]].normal, faceNormal(faceted, t)) > 0.9999f;
        }
    }
    check(flat && pose::hasOnlyFaceNormals(faceted.vertices, faceted.indices), "at 0 degrees it is faceted: every corner its face's normal");
    // (Its gentlest edges are between the triangles round a pole: 1.1 degrees.)
    const Shape barely = shapeOf(smoothOne(sphere, 0.5f)[0]);
    check(pose::hasOnlyFaceNormals(barely.vertices, barely.indices), "at an angle under its gentlest edge (half a degree) it stays faceted");
    const Shape partly = shapeOf(smoothOne(sphere, 5.0f)[0]);
    check(!pose::hasOnlyFaceNormals(partly.vertices, partly.indices) && worstAgainst(partly, [](const Vertex& v) { return v.pos; }) < 0.999f,
          "at 5 degrees only its gentlest edges round off (the poles'): neither faceted nor a sphere's normals");
    const Shape all = shapeOf(smoothOne(sphere, 180.0f)[0]);
    check(worstAgainst(all, [](const Vertex& v) { return v.pos; }) > 0.999f, "at 180 degrees it is as round as at 60");

    // The same sphere with its quads cut the other way: the same normals (a face weighs in by
    // the angle it has at the point, not by how many triangles it was cut into).
    Shape other = sphere;
    for (std::size_t i = 0; i + 5 < other.indices.size(); i += 3) {
        // (a quad is two triangles 0 1 2 / 0 2 3 of four consecutive vertices)
        const uint32_t a = other.indices[i];
        if (other.indices[i + 3] == a && other.indices[i + 4] == other.indices[i + 2]) {
            const uint32_t b = other.indices[i + 1], c = other.indices[i + 2], d = other.indices[i + 5];
            other.indices[i] = a;
            other.indices[i + 1] = b;
            other.indices[i + 2] = d;
            other.indices[i + 3] = b;
            other.indices[i + 4] = c;
            other.indices[i + 5] = d;
            i += 3;
        }
    }
    const Shape roundOther = shapeOf(smoothOne(other, 60.0f)[0]);
    float       apart = 1.0f;
    for (const Vertex& v : roundOther.vertices) {
        for (const Vertex& w : round.vertices) {
            if (w.pos == v.pos) {
                apart = std::min(apart, glm::dot(v.normal, w.normal));
                break;
            }
        }
    }
    check(roundOther.vertices.size() == round.vertices.size() && apart > 0.99999f, "the same normals however its quads were triangulated");
}

void checkEdges() {
    std::printf("-- edges that stay crisp\n");
    const Shape cube = flatCube();
    const Shape cube60 = shapeOf(smoothOne(cube, 60.0f)[0]);
    bool        axis = cube60.vertices.size() == 24;
    for (const Vertex& v : cube60.vertices) {
        const glm::vec3 a = glm::abs(v.normal);
        axis = axis && std::max(a.x, std::max(a.y, a.z)) > 0.99999f;
    }
    check(axis, "a cube at 60 degrees keeps its six flat faces (24 vertices, normals along the axes)");
    const Shape cube90 = shapeOf(smoothOne(cube, 90.0f)[0]);
    check(cube90.vertices.size() == 24 && pose::hasOnlyFaceNormals(cube90.vertices, cube90.indices),
          "... and at exactly 90: an edge AT the angle stays crisp");
    const Shape pillow = shapeOf(smoothOne(cube, 120.0f)[0]);
    check(pillow.vertices.size() == 8 && worstAgainst(pillow, [](const Vertex& v) { return v.pos; }) > 0.9999f,
          "at 120 degrees it is a pillow: 8 vertices, normals along the diagonals");

    // A file with no normals: the reader's own, one vertex per corner. Crisp edges need the
    // corners' vertices apart again.
    const Shape shared = sharedCube();
    check(!pose::hasOnlyFaceNormals(shared.vertices, shared.indices), "a cube with a reader's diagonal normals is not 'faceted'");
    const Shape crisp = shapeOf(smoothOne(shared, 60.0f)[0]);
    check(crisp.vertices.size() == 24 && pose::hasOnlyFaceNormals(crisp.vertices, crisp.indices) && samePlaces(shared, crisp),
          "... smoothed at 60 it gets its edges back: 8 vertices become 24");

    const Shape hexagon = flatPrism(6);
    const Shape hex60 = shapeOf(smoothOne(hexagon, 60.0f)[0]);
    check(pose::hasOnlyFaceNormals(hex60.vertices, hex60.indices), "a hexagonal prism keeps its six sides at 60 degrees (its own angle)");
    const Shape hex61 = shapeOf(smoothOne(hexagon, 61.0f)[0]);
    bool        sidesRound = true, capsFlat = true;
    for (std::size_t t = 0; t < hex61.indices.size() / 3; ++t) {
        const glm::vec3 n = faceNormal(hex61, t);
        for (int c = 0; c < 3; ++c) {
            const Vertex& v = hex61.vertices[hex61.indices[t * 3 + c]];
            if (std::abs(n.y) > 0.5f) {
                capsFlat = capsFlat && glm::dot(v.normal, n) > 0.9999f;
            } else {
                sidesRound = sidesRound && std::abs(v.normal.y) < 1.0e-4f && glm::dot(v.normal, glm::normalize(glm::vec3(v.pos.x, 0.0f, v.pos.z))) > 0.9999f;
            }
        }
    }
    check(sidesRound && capsFlat, "at 61 its sides round off — radial normals — and its caps stay flat");

    const Shape cylinder = flatPrism(32);
    const Shape tube = shapeOf(smoothOne(cylinder, 60.0f)[0]);
    bool        radial = true, caps = true;
    for (std::size_t t = 0; t < tube.indices.size() / 3; ++t) {
        const glm::vec3 n = faceNormal(tube, t);
        for (int c = 0; c < 3; ++c) {
            const Vertex& v = tube.vertices[tube.indices[t * 3 + c]];
            if (std::abs(n.y) > 0.5f) {
                caps = caps && glm::dot(v.normal, n) > 0.9999f;
            } else {
                radial = radial && glm::dot(v.normal, glm::normalize(glm::vec3(v.pos.x, 0.0f, v.pos.z))) > 0.9999f;
            }
        }
    }
    check(radial && caps && tube.vertices.size() == 32u * 4u, "a capped cylinder: a round side, flat caps, the rim's vertices apart (128)");
}

void checkZones() {
    std::printf("-- a surface across material zones\n");
    const Shape upper = flatSphere(32, 16, 0, 8), lower = flatSphere(32, 16, 8, 16);
    SmoothMesh  a, b;
    a.vertices = &upper.vertices;
    a.indices = &upper.indices;
    a.smooth = true;
    b.vertices = &lower.vertices;
    b.indices = &lower.indices;
    b.smooth = true;
    std::vector<SmoothedMesh> both = pose::smoothByAngle({a, b});
    const Shape               top = shapeOf(both[0]), bottom = shapeOf(both[1]);
    check(worstAgainst(top, [](const Vertex& v) { return v.pos; }) > 0.999f && worstAgainst(bottom, [](const Vertex& v) { return v.pos; }) > 0.999f,
          "two zones of one sphere smooth as one: no seam at their border");
    float border = 1.0f;
    for (const Vertex& v : top.vertices) {
        if (std::abs(v.pos.y) < 1.0e-5f) {
            for (const Vertex& w : bottom.vertices) {
                if (w.pos == v.pos) {
                    border = std::min(border, glm::dot(v.normal, w.normal));
                }
            }
        }
    }
    check(border > 0.99999f, "the two zones agree on the normal at each border point");

    b.smooth = false;
    std::vector<SmoothedMesh> one = pose::smoothByAngle({a, b});
    check(!one[0].indices.empty() && one[1].indices.empty() && one[1].vertices.empty(), "a zone that does not ask is not touched");
    float lean = 1.0f;
    for (const Vertex& v : one[0].vertices) {
        if (std::abs(v.pos.y) < 1.0e-5f) {
            lean = std::min(lean, glm::dot(glm::normalize(v.normal), glm::normalize(v.pos)));
        }
    }
    check(lean < 0.9999f && lean > 0.9f, "... and its faces join no group: the smoothed zone's border leans to its own side");

    b.smooth = true;
    b.angleDeg = 0.5f; // (under the sphere's gentlest edge: faceted)
    std::vector<SmoothedMesh> mixed = pose::smoothByAngle({a, b});
    const Shape               faceted = shapeOf(mixed[1]);
    check(pose::hasOnlyFaceNormals(faceted.vertices, faceted.indices), "zones with different angles: each by its own");
    float across = 1.0f;
    for (const Vertex& v : mixed[0].vertices) {
        if (std::abs(v.pos.y) < 1.0e-5f) {
            across = std::min(across, glm::dot(glm::normalize(v.normal), glm::normalize(v.pos)));
        }
    }
    check(across < 0.9999f, "... and the edge between them by the LESSER angle (crisp here)");
}

void checkAttributes() {
    std::printf("-- what a vertex carries\n");
    Shape sphere = flatSphere(32, 16, 0, -1, /*withUv=*/true);
    for (std::size_t i = 0; i < sphere.vertices.size(); ++i) {
        sphere.vertices[i].joints = glm::uvec4(3, 1, 0, 0);
        sphere.vertices[i].weights = glm::vec4(0.75f, 0.25f, 0.0f, 0.0f);
        sphere.vertices[i].ao = (i % 2 == 0) ? 0.5f : 1.0f;
        sphere.vertices[i].correctiveRange = 7;
    }
    const SmoothedMesh out = smoothOne(sphere, 60.0f)[0];
    bool               carried = true;
    float              occlusionLow = 1.0f, occlusionHigh = 0.0f;
    for (std::size_t i = 0; i < out.vertices.size(); ++i) {
        const Vertex& v = out.vertices[i];
        const Vertex& from = sphere.vertices[out.origin[i]];
        carried = carried && v.uv == from.uv && v.joints == from.joints && v.weights == from.weights && v.correctiveRange == 7 && v.tangent == glm::vec4(0.0f);
        occlusionLow = std::min(occlusionLow, v.ao);
        occlusionHigh = std::max(occlusionHigh, v.ao);
    }
    check(carried, "the UV, the skin weights and the corrective range are the file vertex's; the tangent is left to the caller");
    check(occlusionLow > 0.55f && occlusionHigh < 0.95f, "the baked occlusion is averaged over each smooth group (no banding per face)");
    // The seam: the same place with two UVs is two vertices with ONE normal.
    int   seamPairs = 0;
    float seam = 1.0f;
    for (std::size_t i = 0; i < out.vertices.size(); ++i) {
        for (std::size_t j = i + 1; j < out.vertices.size(); ++j) {
            if (out.vertices[i].pos == out.vertices[j].pos && out.vertices[i].uv != out.vertices[j].uv) {
                ++seamPairs;
                seam = std::min(seam, glm::dot(out.vertices[i].normal, out.vertices[j].normal));
            }
        }
    }
    check(seamPairs > 0 && seam > 0.99999f, "a UV seam keeps its two vertices, and they share the normal: no crease down the seam");
    std::vector<Vertex> withTangents = out.vertices;
    pose::computeTangents(withTangents, out.indices);
    int tangents = 0;
    for (const Vertex& v : withTangents) {
        tangents += (v.tangent.w != 0.0f && std::abs(glm::dot(glm::vec3(v.tangent), v.normal)) < 1.0e-3f) ? 1 : 0;
    }
    check(tangents > static_cast<int>(withTangents.size() * 9 / 10), "tangents made for the new normals lie square to them");

    // A file whose normals face against its winding: the smoothed ones stay on the file's side.
    Shape inside = flatSphere();
    for (Vertex& v : inside.vertices) {
        v.normal = -v.normal;
    }
    check(pose::hasOnlyFaceNormals(inside.vertices, inside.indices), "normals facing against the winding still read as a normal per face");
    const Shape inward = shapeOf(smoothOne(inside, 60.0f)[0]);
    check(worstAgainst(inward, [](const Vertex& v) { return -v.pos; }) > 0.999f, "... and smooth on the side the file had them");

    // A triangle with no area changes nothing and breaks nothing.
    Shape sliver = flatCube();
    Vertex extra = sliver.vertices[0];
    const uint32_t at = static_cast<uint32_t>(sliver.vertices.size());
    sliver.vertices.push_back(extra);
    sliver.vertices.push_back(extra);
    sliver.vertices.push_back(extra);
    sliver.indices.insert(sliver.indices.end(), {at, at + 1, at + 2});
    const SmoothedMesh kept = smoothOne(sliver, 60.0f)[0];
    check(kept.indices.size() == sliver.indices.size() && pose::hasOnlyFaceNormals(kept.vertices, kept.indices),
          "a triangle with no area is carried along and joins no group");

    Shape none;
    check(!pose::hasOnlyFaceNormals(none.vertices, none.indices) && smoothOne(none, 60.0f)[0].indices.empty(), "an empty mesh is neither faceted nor smoothed");
    Shape plane;
    addFlatPolygon(plane, {{0, 0, 0}, {1, 0, 0}, {1, 0, -1}, {0, 0, -1}});
    check(pose::hasOnlyFaceNormals(plane.vertices, plane.indices) && shapeOf(smoothOne(plane, 60.0f)[0]).vertices.size() == 4,
          "a flat sheet is the same either way (4 vertices)");
}

MeshData meshOf(const Shape& s, const char* name) {
    MeshData mesh;
    mesh.vertices = s.vertices;
    mesh.indices = s.indices;
    mesh.name = name;
    return mesh;
}

void checkImport() {
    std::printf("-- the import's rule\n");
    // A sphere whose file has smooth normals: the author's.
    Shape authored = flatSphere();
    for (Vertex& v : authored.vertices) {
        v.normal = glm::normalize(v.pos);
    }
    const Shape faceted = flatSphere();
    Shape       generated = sharedCube();
    Shape       figure = flatSphere();

    ModelData data;
    data.meshes.push_back(meshOf(faceted, "faceted"));
    data.meshes.push_back(meshOf(authored, "authored"));
    data.meshes.push_back(meshOf(generated, "generated"));
    data.meshes.back().normalsGenerated = true;
    data.meshes.push_back(meshOf(figure, "figure"));
    data.meshes.back().baseVertex.assign(figure.vertices.size(), 0);
    // (Apart, so that the four are not one surface.)
    for (std::size_t m = 0; m < data.meshes.size(); ++m) {
        for (Vertex& v : data.meshes[m].vertices) {
            v.pos.x += 10.0f * static_cast<float>(m);
        }
    }
    const std::vector<Vertex> authoredBefore = data.meshes[1].vertices;
    const int                 done = pose::applyImportSmoothing(data);
    check(done == 2, "two of four meshes are smoothed: the faceted file's and the one with no normals");
    const MeshData& a = data.meshes[0];
    check(a.smoothByAngle && a.smoothAngle == pose::kDefaultSmoothAngle && !pose::hasOnlyFaceNormals(a.vertices, a.indices) &&
              a.fileVertices.size() == faceted.vertices.size() && a.fileIndices == faceted.indices &&
              pose::hasOnlyFaceNormals(a.fileVertices, a.fileIndices) && a.indices.size() == a.fileIndices.size(),
          "the faceted mesh is smooth now, by 60 degrees, with the file's geometry kept beside it corner for corner");
    const MeshData& b = data.meshes[1];
    bool            untouched = !b.smoothByAngle && b.fileVertices.empty() && b.vertices.size() == authoredBefore.size();
    for (std::size_t i = 0; untouched && i < b.vertices.size(); ++i) {
        untouched = b.vertices[i].normal == authoredBefore[i].normal && b.vertices[i].pos == authoredBefore[i].pos;
    }
    check(untouched, "authored normals are left exactly as they are");
    const MeshData& c = data.meshes[2];
    check(c.smoothByAngle && c.vertices.size() == 24 && pose::hasOnlyFaceNormals(c.vertices, c.indices) && c.fileVertices.size() == 8,
          "a cube whose file had no normals gets crisp edges, not a reader's pillow");
    const MeshData& d = data.meshes[3];
    check(!d.smoothByAngle && d.fileVertices.empty() && d.vertices.size() == figure.vertices.size(), "a native figure's mesh is not the import's to smooth");
    check(data.notes.size() == 1 && data.notes[0].find("smoothed 2 zones") != std::string::npos, "the import says what it did");
    check(pose::applyImportSmoothing(data) == 0 && data.notes.size() == 1, "a second pass changes nothing");

    ModelData plain;
    plain.meshes.push_back(meshOf(authored, "authored"));
    check(pose::applyImportSmoothing(plain) == 0 && plain.notes.empty() && !plain.meshes[0].smoothByAngle, "a model of authored normals: nothing to do, nothing said");
}

} // namespace

int main() {
    checkSphere();
    checkEdges();
    checkZones();
    checkAttributes();
    checkImport();
    std::printf("%d failed\n", failures);
    return failures;
}
