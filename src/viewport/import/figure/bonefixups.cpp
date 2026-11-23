// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file bonefixups.cpp
 * @brief Implementation of the skeleton fix-ups. See bonefixups.h.
 */

#include "bonefixups.h"

#include "figureutils.h" // toLowerAscii

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstddef>

namespace pose {

namespace {

// Builds a bone's rest-orientation matrix from its Euler orientation (degrees), applied in x, y, z
// order (Rz·Ry·Rx) — the same frame the posing runtime rotates in (Armature::build has the check
// that settled the order). Since the bind transform is translation-only, this matrix's columns are
// the bone's three world-space rotation-channel axes (the axes its pose Euler angles rotate about).
glm::mat3 orientationAxes(const glm::vec3& degrees) {
    const glm::mat4 m =
        glm::rotate(glm::mat4(1.0f), glm::radians(degrees.z), glm::vec3(0.0f, 0.0f, 1.0f)) *
        glm::rotate(glm::mat4(1.0f), glm::radians(degrees.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::rotate(glm::mat4(1.0f), glm::radians(degrees.x), glm::vec3(1.0f, 0.0f, 0.0f));
    return glm::mat3(m);
}

} // namespace

void applyJointCenterOffsets(std::vector<FigureBone>& bones, const JointCenterOffsets& offsets) {
    if (offsets.empty()) {
        return;
    }
    for (FigureBone& bone : bones) {
        const auto it = offsets.find(bone.name);
        if (it != offsets.end()) {
            bone.origin += it->second;
        }
    }
}

// A "twist" bone (lThighTwist, rForearmTwist, …) sits partway along a limb — mid-femur, mid-forearm —
// where a human has no joint; it exists only to spread the limb's axial twist across the skin. It
// should rotate about its own length (the twist) but must never bend, or the limb folds mid-bone where
// nothing can. Newer figures say so themselves through the format's per-channel `locked` flag, which
// nodeparser honours as a hard [0,0] clamp — and beware those bones are NOT reliably named "twist"
// (in several generations the mid-forearm bone is named like a wrist joint), which is exactly why
// the name-based rule below is only a LEGACY FALLBACK for generations that don't write `locked` at
// all. For those: find the rotation-channel axis most aligned with the bone's length (its twist axis)
// and lock the other two to zero. Detection needs the "twist" name plus a child bone to define the
// length direction; a bone missing either is left untouched. The runtime's existing clampBoneEuler()
// then enforces the lock across every posing path (FK drag, IK, loaded pose).
void lockTwistBoneBendAxes(std::vector<FigureBone>& bones) {
    std::vector<int> firstChild(bones.size(), -1);
    for (int i = 0; i < static_cast<int>(bones.size()); ++i) {
        const int p = bones[static_cast<std::size_t>(i)].parent;
        if (p >= 0 && p < static_cast<int>(bones.size()) && firstChild[static_cast<std::size_t>(p)] < 0) {
            firstChild[static_cast<std::size_t>(p)] = i;
        }
    }
    for (std::size_t i = 0; i < bones.size(); ++i) {
        FigureBone& bone = bones[i];
        if (toLowerAscii(bone.name).find("twist") == std::string::npos) {
            continue; // not a twist bone
        }
        // If the figure already pins an axis via the format's `locked` channel flag (read by
        // nodeparser), trust that authored data over this geometric fallback — re-deriving the
        // twist axis here could disagree with it and kill the legitimate twist channel. This
        // heuristic only remains for old figure generations that don't write `locked` at all.
        bool formatLocked = false;
        for (int a = 0; a < 3; ++a) {
            if (bone.rotationLimited[a] && bone.rotationMin[a] == bone.rotationMax[a]) {
                formatLocked = true;
                break;
            }
        }
        if (formatLocked) {
            continue;
        }
        const int child = firstChild[i];
        if (child < 0) {
            continue; // no child to define the bone's length direction
        }
        const glm::vec3 along = bones[static_cast<std::size_t>(child)].origin - bone.origin;
        if (glm::dot(along, along) < 1e-12f) {
            continue; // degenerate: child coincides with this joint
        }
        const glm::vec3 dir = glm::normalize(along);
        const glm::mat3 axes = orientationAxes(bone.orientation);
        int   twistAxis = 0;
        float bestAlignment = -1.0f;
        for (int a = 0; a < 3; ++a) {
            const float alignment = std::abs(glm::dot(dir, glm::normalize(axes[a])));
            if (alignment > bestAlignment) {
                bestAlignment = alignment;
                twistAxis = a;
            }
        }
        for (int a = 0; a < 3; ++a) {
            if (a == twistAxis) {
                continue; // keep the twist axis as the figure defined it
            }
            bone.rotationMin[a] = 0.0f;
            bone.rotationMax[a] = 0.0f;
            bone.rotationLimited[a] = true; // clamped to [0,0] => no bend
        }
    }
}

} // namespace pose
