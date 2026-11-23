// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file meshrepair.cpp
 * @brief Implementation of the format-neutral mesh repairs. See meshrepair.h.
 */

#include "meshrepair.h"

#include "modeldata.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace pose {

void fillMissingNormals(MeshData& mesh) {
    std::vector<char> missing(mesh.vertices.size(), 0);
    bool              any = false;
    bool              all = !mesh.vertices.empty();
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const float lengthSquared = glm::dot(mesh.vertices[i].normal, mesh.vertices[i].normal);
        missing[i] = std::isfinite(lengthSquared) && lengthSquared >= 1e-12f ? 0 : 1;
        any = any || missing[i];
        all = all && missing[i];
    }
    if (!any) {
        return;
    }
    mesh.normalsGenerated = all; // (a file with no normals at all: the import smooths it by angle, smoothnormals.h)
    std::vector<glm::vec3> accum(mesh.vertices.size(), glm::vec3(0.0f));
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t  i0 = mesh.indices[i];
        const uint32_t  i1 = mesh.indices[i + 1];
        const uint32_t  i2 = mesh.indices[i + 2];
        const glm::vec3 faceNormal = glm::cross(mesh.vertices[i1].pos - mesh.vertices[i0].pos, mesh.vertices[i2].pos - mesh.vertices[i0].pos);
        accum[i0] += faceNormal;
        accum[i1] += faceNormal;
        accum[i2] += faceNormal;
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (!missing[i]) {
            continue;
        }
        const float len = glm::length(accum[i]);
        mesh.vertices[i].normal = len > 1e-8f ? accum[i] / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

std::size_t removeNonFiniteVertices(MeshData& mesh) {
    const auto finite = [](const glm::vec3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); };
    std::size_t bad = 0;
    for (const Vertex& v : mesh.vertices) {
        bad += finite(v.pos) ? 0 : 1;
    }
    if (bad == 0) {
        return 0;
    }
    constexpr uint32_t    kGone = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> newIndex(mesh.vertices.size(), kGone);
    std::vector<Vertex>   kept;
    kept.reserve(mesh.vertices.size() - bad);
    const bool            hasBase = mesh.baseVertex.size() == mesh.vertices.size(); // (parallel to the vertices where present)
    std::vector<uint32_t> keptBase;
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (!finite(mesh.vertices[i].pos)) {
            continue;
        }
        newIndex[i] = static_cast<uint32_t>(kept.size());
        kept.push_back(mesh.vertices[i]);
        if (hasBase) {
            keptBase.push_back(mesh.baseVertex[i]);
        }
    }
    std::vector<uint32_t> indices;
    indices.reserve(mesh.indices.size());
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        uint32_t corner[3];
        bool     whole = true;
        for (int c = 0; c < 3; ++c) {
            const uint32_t old = mesh.indices[i + static_cast<std::size_t>(c)];
            corner[c] = old < newIndex.size() ? newIndex[old] : kGone;
            whole = whole && corner[c] != kGone;
        }
        if (whole) {
            indices.insert(indices.end(), {corner[0], corner[1], corner[2]});
        }
    }
    mesh.vertices.swap(kept);
    mesh.indices.swap(indices);
    if (hasBase) {
        mesh.baseVertex.swap(keptBase);
    }
    return bad;
}

std::size_t removeNonFiniteVertices(ModelData& model) {
    std::size_t removed = 0;
    for (MeshData& mesh : model.meshes) {
        removed += removeNonFiniteVertices(mesh);
    }
    if (removed > 0) {
        model.meshes.erase(std::remove_if(model.meshes.begin(), model.meshes.end(), [](const MeshData& m) { return m.indices.size() < 3; }),
                           model.meshes.end());
    }
    return removed;
}

} // namespace pose
