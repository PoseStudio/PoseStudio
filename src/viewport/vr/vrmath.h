/**
 * @file vrmath.h
 * @brief The matrix math of a VR view: an eye's asymmetric projection, its view matrix from a
 *        tracked pose, and the "stand here, facing the figure" placement of the play space.
 *
 * GLM only (no OpenXR types), so it stays unit-testable and the conventions are in one place:
 * OpenXR hands over poses in a right-handed, Y-up, -Z-forward space in METRES — the same
 * conventions as the scene (figures stand on y = 0 at their true height), which is why a tracked
 * headset lands in the world through one rigid transform (worldFromStage) and no scale.
 */

#ifndef VRMATH_H
#define VRMATH_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace pose {

/// The projection for an eye whose frustum is bounded by the four half-angles (radians; left and
/// down are negative), built for Vulkan clip space: depth in [0, 1] and Y flipped (the same
/// convention Camera::rebuildProjection applies to its symmetric perspective).
glm::mat4 vrProjectionFromFov(float angleLeft, float angleRight, float angleUp, float angleDown,
                              float nearZ, float farZ);

/// The rigid transform of a tracked pose (position + orientation) — stage <- eye for an eye pose,
/// stage <- head for the head.
glm::mat4 vrPoseMatrix(const glm::vec3& position, const glm::quat& orientation);

/// The view matrix of an eye: world <- stage <- eye, inverted.
glm::mat4 vrViewFromEyePose(const glm::mat4& worldFromStage, const glm::vec3& eyePosition,
                            const glm::quat& eyeOrientation);

/// The world-from-stage transform that puts the user — whose head is at @p headStagePosition
/// looking along @p headStageOrientation's forward, in stage space — at @p standPoint (world x/z;
/// its y is ignored) facing the world's -Z: toward a figure standing at the origin, which faces
/// +Z. The floor stays the floor (stage y = 0 maps to world y = @p floorOffset — 0 for a
/// floor-level stage space, the user's eye height for an eye-level fallback space), so the user
/// keeps their real height; only a yaw and a horizontal slide are applied.
glm::mat4 vrRecenterTransform(const glm::vec3& headStagePosition, const glm::quat& headStageOrientation,
                              const glm::vec3& standPoint, float floorOffset);

} // namespace pose

#endif // VRMATH_H
