/**
 * @file camera.h
 * @brief A simple orbit camera: yaw/pitch/distance around a target point.
 *
 * Pure math — no Vulkan, no Qt — so it lives in scene/ rather than rendering/ and can
 * be unit-tested in isolation. The renderer asks it for a view-projection matrix each
 * frame; input handling (drag to orbit, wheel to dolly) calls the orbit/dolly/pan
 * mutators. The projection is built for Vulkan's clip space: depth in [0, 1] and the
 * Y axis flipped (see CMake's GLM_FORCE_DEPTH_ZERO_TO_ONE plus the explicit Y negation).
 */

#ifndef CAMERA_H
#define CAMERA_H

#include <glm/glm.hpp>

namespace pose {

/**
 * @class Camera
 * @brief Orbit-style viewport camera producing Vulkan-ready view/projection matrices.
 */
class Camera {
public:
    /// Recomputes the projection for a new viewport aspect ratio (call on resize).
    void setViewportSize(float width, float height);

    /// Orbit around the target by the given yaw/pitch deltas (radians).
    void orbit(float deltaYaw, float deltaPitch);

    /// Move toward/away from the target. Positive @p amount zooms in.
    void dolly(float amount);

    /// Slide the target (and eye) within the view plane.
    void pan(float deltaX, float deltaY);

    glm::mat4 view() const;
    glm::mat4 projection() const { return m_projection; }
    glm::mat4 viewProjection() const { return m_projection * view(); }
    glm::vec3 position() const;

private:
    void rebuildProjection();

    glm::vec3 m_target = glm::vec3(0.0f);
    float m_distance = 4.0f;
    float m_yaw = glm::radians(45.0f);   // around the world up axis
    float m_pitch = glm::radians(25.0f); // above the horizon
    float m_fovYRadians = glm::radians(50.0f);
    float m_aspect = 1.0f;
    float m_nearPlane = 0.05f;
    float m_farPlane = 1000.0f;
    glm::mat4 m_projection = glm::mat4(1.0f);
};

} // namespace pose

#endif // CAMERA_H
