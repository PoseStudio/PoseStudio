// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file bodymesh.h
 * @brief A figure's BODY MESH SAMPLE for the self-collision volumes: the skinned mesh's bind
 *        positions with the bone each vertex mostly follows. The Model hands it to the Armature
 *        at import; the rig fits its body volumes to it (IkRig::buildBodyVolumes) instead of
 *        sizing them from the skeleton alone — the chest's depth, the thighs' taper, the
 *        shoulders' width are in the mesh, not in the joint spacing. Qt-free, Vulkan-free; the
 *        skeleton dump writes it as a sidecar so the IK harness fits the same volumes.
 */

#ifndef BODYMESH_H
#define BODYMESH_H

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace pose {

/// One sampled vertex: its bind position (model space) and the bone with the largest skin
/// weight on it.
struct BodyMeshPoint {
    glm::vec3 pos{0.0f};
    int       bone = -1;
};

/// Writes @p mesh as text (one point per line: x y z bone) — the skeleton dump's sidecar.
bool saveBodyMesh(const std::string& path, const std::vector<BodyMeshPoint>& mesh);
/// Reads a file written by saveBodyMesh. False (and @p out empty) if the file is absent or
/// malformed.
bool loadBodyMesh(const std::string& path, std::vector<BodyMeshPoint>& out);

} // namespace pose

#endif // BODYMESH_H
