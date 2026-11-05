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
// Keep the pitch just shy of straight up/down so the view matrix never degenerates (eye
// direction parallel to the up vector). 89.9°: close enough that the Top/Bottom axis views read
// as exactly vertical (a 1° tilt visibly skews vertical edges under perspective), still far
// enough from the pole that lookAt's right vector — normalize(cross(forward, up)), of length
// sin(0.1°) ≈ 1.7e-3 before normalizing — stays numerically clean in float.
constexpr float kMinPitch = -1.56905f; // -89.9 degrees
constexpr float kMaxPitch = 1.56905f;  // +89.9 degrees
constexpr float kPi = 3.14159265f;
} // namespace

void Camera::setViewportSize(float width, float height) {
    m_viewportWidth = width > 0.0f ? width : 1.0f;
    m_viewportHeight = height > 0.0f ? height : 1.0f;
    m_aspect = (height > 0.0f) ? (width / height) : 1.0f;
    rebuildProjection();
}

void Camera::reset() {
    m_target = glm::vec3(0.0f, kDefaultTargetHeight, 0.0f);
    m_distance = kDefaultDistance;
    m_yaw = glm::radians(kDefaultYawDegrees);
    m_pitch = glm::radians(kDefaultPitchDegrees);
    setOrthographic(false); // the home view is the perspective three-quarter framing
}

void Camera::restoreFraming(const glm::vec3& target, float yaw, float pitch, float distance) {
    m_target = target;
    m_yaw = yaw;
    m_pitch = std::clamp(pitch, kMinPitch, kMaxPitch); // a hand-edited file can't tip the camera under the floor
    m_distance = distance > 0.0f ? distance : kDefaultDistance; // guard a zero/negative dolly
}

void Camera::orbit(float deltaYaw, float deltaPitch) {
    m_explicit = false; // an orbit gesture returns the camera to its orbit framing
    m_yaw += deltaYaw;
    m_pitch = std::clamp(m_pitch + deltaPitch, kMinPitch, kMaxPitch);
    setOrthographic(false); // auto perspective: turning an axis view unlocks it
}

void Camera::dolly(float amount) {
    // Scale by distance so the zoom feels consistent near and far, and clamp so we
    // never cross through the target.
    m_distance = std::max(0.05f, m_distance - amount * m_distance);
    if (m_orthographic) {
        rebuildProjection(); // the orthographic extent follows the distance
    }
}

void Camera::setOrthographic(bool on) {
    if (m_orthographic == on) {
        return;
    }
    m_orthographic = on;
    rebuildProjection();
}

void Camera::pan(float deltaX, float deltaY) {
    const glm::vec3 eye = position();
    const glm::vec3 forward = glm::normalize(m_target - eye);
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::cross(right, forward);
    // Scale pan by distance so dragging covers a consistent fraction of the view.
    m_target += (-right * deltaX + up * deltaY) * m_distance;
}

void Camera::setAxisView(AxisView view) {
    // position() puts the eye at target + (sin yaw, ·, cos yaw): yaw 0 is +Z (front), +90° is
    // +X (right). The vertical views sit AT the pitch clamp: the eye leans a hair toward +Z (Top,
    // yaw 0) or -Z (Bottom, yaw 180°), which is what puts the scene's front at the bottom of the
    // screen in both — and lets a following orbit drag continue smoothly from the clamp.
    switch (view) {
    case AxisView::Front:  m_yaw = 0.0f;        m_pitch = 0.0f;      break;
    case AxisView::Back:   m_yaw = kPi;         m_pitch = 0.0f;      break;
    case AxisView::Right:  m_yaw = 0.5f * kPi;  m_pitch = 0.0f;      break;
    case AxisView::Left:   m_yaw = -0.5f * kPi; m_pitch = 0.0f;      break;
    case AxisView::Top:    m_yaw = 0.0f;        m_pitch = kMaxPitch; break;
    case AxisView::Bottom: m_yaw = kPi;         m_pitch = kMinPitch; break;
    }
    setOrthographic(true);
}

void Camera::flip() {
    m_yaw += kPi;
}

void Camera::frame(const glm::vec3& center, float radius) {
    m_target = center;
    // The sphere must fit the view's NARROWER field: the vertical fov, or the horizontal one
    // (tan(h/2) = aspect · tan(v/2)) on a tall viewport. A 5% margin keeps it off the edges.
    // The same distance frames the orthographic view: its half-height is distance · tan(fov/2).
    const float halfV = 0.5f * m_fovYRadians;
    const float halfH = std::atan(std::tan(halfV) * m_aspect);
    const float half = std::min(halfV, halfH);
    m_distance = std::max(0.05f, 1.05f * radius / std::sin(half));
    if (m_orthographic) {
        rebuildProjection();
    }
}

void Camera::setExplicitView(const glm::mat4& view, const glm::mat4& projection) {
    m_explicit = true;
    m_explicitView = view;
    m_explicitEye = glm::vec3(glm::inverse(view)[3]);
    m_projection = projection;
}

glm::vec3 Camera::position() const {
    if (m_explicit) {
        return m_explicitEye;
    }
    // Spherical -> Cartesian offset from the target.
    const float cosPitch = std::cos(m_pitch);
    const glm::vec3 offset(
        m_distance * cosPitch * std::sin(m_yaw),
        m_distance * std::sin(m_pitch),
        m_distance * cosPitch * std::cos(m_yaw));
    return m_target + offset;
}

glm::mat4 Camera::view() const {
    if (m_explicit) {
        return m_explicitView;
    }
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
    // GLM_FORCE_DEPTH_ZERO_TO_ONE (set in CMake) makes perspective()/ortho() emit Vulkan's
    // [0,1] depth range. Vulkan's clip-space Y also points opposite to OpenGL's, so we
    // negate [1][1] here rather than flipping the viewport — the conventional fix.
    if (m_orthographic) {
        // The extent that the perspective frustum has AT the target plane, so what sits at the
        // target keeps its on-screen size across a projection switch and dolly still zooms.
        const float halfH = m_distance * std::tan(0.5f * m_fovYRadians);
        const float halfW = halfH * m_aspect;
        m_projection = glm::ortho(-halfW, halfW, -halfH, halfH, m_nearPlane, m_farPlane);
    } else {
        m_projection = glm::perspective(m_fovYRadians, m_aspect, m_nearPlane, m_farPlane);
    }
    m_projection[1][1] *= -1.0f;
}

} // namespace pose
