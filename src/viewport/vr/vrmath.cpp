/**
 * @file vrmath.cpp
 * @brief Implementation of the VR view math. See vrmath.h.
 */

#include "vrmath.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace pose {

glm::mat4 vrProjectionFromFov(float angleLeft, float angleRight, float angleUp, float angleDown,
                              float nearZ, float farZ) {
    // The OpenXR SDK's reference construction (xr_linear.h, GRAPHICS_VULKAN): an off-centre
    // frustum from the tangents of the four half-angles. tanHeight is NEGATIVE for Vulkan — that
    // is the clip-space Y flip, the same sign Camera applies by negating [1][1].
    const float tanLeft = std::tan(angleLeft);
    const float tanRight = std::tan(angleRight);
    const float tanUp = std::tan(angleUp);
    const float tanDown = std::tan(angleDown);
    const float tanWidth = tanRight - tanLeft;
    const float tanHeight = tanDown - tanUp;

    glm::mat4 m(0.0f);
    m[0][0] = 2.0f / tanWidth;
    m[2][0] = (tanRight + tanLeft) / tanWidth;
    m[1][1] = 2.0f / tanHeight;
    m[2][1] = (tanUp + tanDown) / tanHeight;
    m[2][2] = -farZ / (farZ - nearZ);          // depth 0 at the near plane, 1 at the far plane
    m[3][2] = -(farZ * nearZ) / (farZ - nearZ);
    m[2][3] = -1.0f;
    return m;
}

glm::mat4 vrPoseMatrix(const glm::vec3& position, const glm::quat& orientation) {
    return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(orientation);
}

glm::mat4 vrViewFromEyePose(const glm::mat4& worldFromStage, const glm::vec3& eyePosition,
                            const glm::quat& eyeOrientation) {
    return glm::inverse(worldFromStage * vrPoseMatrix(eyePosition, eyeOrientation));
}

glm::mat4 vrRecenterTransform(const glm::vec3& headStagePosition, const glm::quat& headStageOrientation,
                              const glm::vec3& standPoint, float floorOffset) {
    // Where the user is looking, flattened onto the floor. A head pitched straight up or down
    // has no horizontal forward; keep the stage's own forward (-Z) then.
    glm::vec3 forward = headStageOrientation * glm::vec3(0.0f, 0.0f, -1.0f);
    forward.y = 0.0f;
    const float len = glm::length(forward);
    forward = len > 1e-4f ? forward / len : glm::vec3(0.0f, 0.0f, -1.0f);

    // The yaw that turns that forward onto -Z: rotating (fx, fz) about +Y by delta gives
    // (fx cos + fz sin, -fx sin + fz cos), which is (0, -1) for delta = atan2(fx, -fz).
    const float delta = std::atan2(forward.x, -forward.z);
    const glm::mat4 turn = glm::rotate(glm::mat4(1.0f), delta, glm::vec3(0.0f, 1.0f, 0.0f));

    // Slide the head's floor point to the origin, turn, then slide to the stand point. The
    // vertical axis is untouched except for the floor offset, so the user's real height is kept.
    const glm::mat4 toOrigin =
        glm::translate(glm::mat4(1.0f), glm::vec3(-headStagePosition.x, 0.0f, -headStagePosition.z));
    const glm::mat4 toStand =
        glm::translate(glm::mat4(1.0f), glm::vec3(standPoint.x, floorOffset, standPoint.z));
    return toStand * turn * toOrigin;
}

} // namespace pose
