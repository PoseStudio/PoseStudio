/**
 * @file camera.cpp
 * @brief Implementation of the orbit camera. See camera.h.
 */

#include "camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace pose {

namespace {
// Keep the pitch just shy of straight up/down so the view matrix never degenerates
// (eye direction parallel to the up vector).
constexpr float kMinPitch = -1.5533f; // ~ -89 degrees
constexpr float kMaxPitch = 1.5533f;  // ~ +89 degrees
} // namespace

void Camera::setViewportSize(float width, float height) {
    m_aspect = (height > 0.0f) ? (width / height) : 1.0f;
    rebuildProjection();
}

void Camera::orbit(float deltaYaw, float deltaPitch) {
    m_yaw += deltaYaw;
    m_pitch = std::clamp(m_pitch + deltaPitch, kMinPitch, kMaxPitch);
}

void Camera::dolly(float amount) {
    // Scale by distance so the zoom feels consistent near and far, and clamp so we
    // never cross through the target.
    m_distance = std::max(0.05f, m_distance - amount * m_distance);
}

void Camera::pan(float deltaX, float deltaY) {
    const glm::vec3 eye = position();
    const glm::vec3 forward = glm::normalize(m_target - eye);
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::cross(right, forward);
    // Scale pan by distance so dragging covers a consistent fraction of the view.
    m_target += (-right * deltaX + up * deltaY) * m_distance;
}

glm::vec3 Camera::position() const {
    // Spherical -> Cartesian offset from the target.
    const float cosPitch = std::cos(m_pitch);
    const glm::vec3 offset(
        m_distance * cosPitch * std::sin(m_yaw),
        m_distance * std::sin(m_pitch),
        m_distance * cosPitch * std::cos(m_yaw));
    return m_target + offset;
}

glm::mat4 Camera::view() const {
    return glm::lookAt(position(), m_target, glm::vec3(0.0f, 1.0f, 0.0f));
}

Ray Camera::screenPointToRay(float px, float py, float viewportWidth, float viewportHeight) const {
    // Pixel -> normalized device coords. Vulkan NDC is [-1,1] in x/y; because the projection
    // negates [1][1], a top-of-screen pixel (py == 0) maps to ndcY == -1 with no extra flip.
    const float ndcX = 2.0f * px / viewportWidth - 1.0f;
    const float ndcY = 2.0f * py / viewportHeight - 1.0f;

    // Unproject the near (z=0) and far (z=1) NDC points through the same matrices used to render,
    // so the ray is exactly consistent with what's on screen.
    const glm::mat4 invViewProj = glm::inverse(viewProjection());
    glm::vec4 nearPoint = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    glm::vec4 farPoint = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    Ray ray;
    ray.origin = glm::vec3(nearPoint);
    ray.direction = glm::normalize(glm::vec3(farPoint - nearPoint));
    return ray;
}

void Camera::rebuildProjection() {
    // GLM_FORCE_DEPTH_ZERO_TO_ONE (set in CMake) makes perspective() emit Vulkan's
    // [0,1] depth range. Vulkan's clip-space Y also points opposite to OpenGL's, so we
    // negate [1][1] here rather than flipping the viewport — the conventional fix.
    m_projection = glm::perspective(m_fovYRadians, m_aspect, m_nearPlane, m_farPlane);
    m_projection[1][1] *= -1.0f;
}

} // namespace pose
