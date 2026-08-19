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

/// A world-space ray (origin + normalized direction), e.g. for mouse picking.
struct Ray {
    glm::vec3 origin;
    glm::vec3 direction;
};

/**
 * @class Camera
 * @brief Orbit-style viewport camera producing Vulkan-ready view/projection matrices.
 */
class Camera {
public:
    /// Recomputes the projection for a new viewport aspect ratio (call on resize).
    void setViewportSize(float width, float height);

    /// Restores the default framing (target/distance/yaw/pitch) — the "home" view the viewport
    /// starts at. The projection is left alone: aspect/fov are viewport properties, not framing.
    void reset();

    /// Orbit around the target by the given yaw/pitch deltas (radians).
    void orbit(float deltaYaw, float deltaPitch);

    /// Move toward/away from the target. Positive @p amount zooms in.
    void dolly(float amount);

    /// Slide the target (and eye) within the view plane.
    void pan(float deltaX, float deltaY);

    glm::mat4 view() const;
    glm::mat4 viewProjection() const { return m_projection * view(); }
    glm::vec3 position() const;

    /// Builds a world-space picking ray through the pixel (@p px, @p py) of a viewport of size
    /// @p viewportWidth x @p viewportHeight. Pixel coords are top-left origin, y down — matching
    /// both Qt mouse coords and Vulkan's framebuffer convention (the projection's Y-flip already
    /// accounts for the sign), so no extra flip is needed. The direction is normalized.
    Ray screenPointToRay(float px, float py, float viewportWidth, float viewportHeight) const;

private:
    void rebuildProjection();

    // Defaults framed for a standing figure imported at the origin (feet at y=0, ~1.8 units tall):
    // aim at mid-torso and sit close enough that the whole body loads centered in the view. Targeting
    // the floor/feet (y=0) at a larger distance is what left the figure high and pushed back in frame.
    // Named so the initial framing and reset() can't drift apart — reset() is the viewport's Home button.
    static constexpr float kDefaultTargetHeight = 0.9f; // ~mid-torso of a standing figure
    static constexpr float kDefaultDistance = 2.6f;
    static constexpr float kDefaultYawDegrees = 25.0f;   // around the world up axis; slight three-quarter view
    static constexpr float kDefaultPitchDegrees = 10.0f; // just above the horizon — a gentle look-down

    glm::vec3 m_target = glm::vec3(0.0f, kDefaultTargetHeight, 0.0f);
    float m_distance = kDefaultDistance;
    float m_yaw = glm::radians(kDefaultYawDegrees);
    float m_pitch = glm::radians(kDefaultPitchDegrees);
    float m_fovYRadians = glm::radians(50.0f);
    float m_aspect = 1.0f;
    float m_nearPlane = 0.05f;
    float m_farPlane = 1000.0f;
    glm::mat4 m_projection = glm::mat4(1.0f);
};

} // namespace pose

#endif // CAMERA_H
