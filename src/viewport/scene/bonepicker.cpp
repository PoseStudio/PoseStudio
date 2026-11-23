// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file bonepicker.cpp
 * @brief Joint/bone-body picking. See bonepicker.h.
 */

#include "bonepicker.h"

#include "camera.h"
#include "model.h"

#include <glm/glm.hpp>

#include <cstddef>

namespace pose {

BonePick pickBone(const std::vector<std::unique_ptr<Model>>& models, float px, float py, float vpW,
                  float vpH, const Camera& camera) {
    const glm::mat4 viewProj = camera.viewProjection();
    const glm::vec2 click(px, py);
    const auto distToSegment = [](const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, float& t) {
        const glm::vec2 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        t = (len2 > 1e-6f) ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
        return glm::length(p - (a + ab * t));
    };
    int   jointModel = -1, joint = -1; float jointDist = 1e9f; // nearest joint ORIGIN
    int   segModel = -1,   seg = -1;   float segDist = 1e9f;   // nearest bone SEGMENT (its owner)
    int   segChild = -1;               float segAlong = 0.0f;  // ... the joint it runs to, and where on it
    std::vector<glm::vec2> screen;
    std::vector<char>      visible;
    for (std::size_t m = 0; m < models.size(); ++m) {
        const Model* fig = models[m].get();
        if (!fig->hasSkeleton()) {
            continue;
        }
        const std::size_t n = fig->boneCount();
        screen.assign(n, glm::vec2(0.0f));
        visible.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec4 clip = viewProj * glm::vec4(fig->boneWorldPosition(i), 1.0f);
            if (clip.w <= 1e-4f) {
                continue; // behind the camera
            }
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            // viewProj already carries Vulkan's Y flip, so this matches the mouse convention.
            screen[i] = glm::vec2((ndc.x * 0.5f + 0.5f) * vpW, (ndc.y * 0.5f + 0.5f) * vpH);
            visible[i] = 1;
            const float dist = glm::length(screen[i] - click);
            if (dist < jointDist) {
                jointDist = dist;
                joint = static_cast<int>(i);
                jointModel = static_cast<int>(m);
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            const int parent = fig->boneParent(i);
            if (parent < 0 || fig->boneParent(static_cast<std::size_t>(parent)) < 0 || !visible[i] ||
                !visible[static_cast<std::size_t>(parent)]) {
                continue; // no segment, a root's virtual segment, or off-camera
            }
            float along = 0.0f;
            const float dist = distToSegment(click, screen[static_cast<std::size_t>(parent)], screen[i], along);
            if (dist < segDist) {
                segDist = dist;
                seg = parent; // the segment is the PARENT bone's body
                segModel = static_cast<int>(m);
                segChild = static_cast<int>(i);
                segAlong = along;
            }
        }
    }
    // Priority: a joint under the cursor, then the nearest bone body within the click
    // tolerance, then a joint within it (leaf bones — toes, fingertips — have no segment).
    // (kJointSnapPx — right on a joint origin — is bonepicker.h's: the skin pick's snap shares it.)
    constexpr float kPickRadiusPx = 32.0f; // click tolerance around a bone body or joint
    BonePick pick;
    if (joint >= 0 && jointDist <= kJointSnapPx) {
        pick.model = jointModel;
        pick.bone = joint;
    } else if (seg >= 0 && segDist <= kPickRadiusPx) {
        pick.model = segModel;
        pick.bone = seg;
        pick.child = segChild;
        pick.along = segAlong;
    } else if (joint >= 0 && jointDist <= kPickRadiusPx) {
        pick.model = jointModel;
        pick.bone = joint;
    }
    return pick;
}

} // namespace pose
