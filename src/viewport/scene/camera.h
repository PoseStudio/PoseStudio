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

#include "ray.h" // the picking ray screenPointToRay() builds

#include <glm/glm.hpp>

namespace pose {

/// The six axis-aligned views, in Blender's numpad convention: Front looks at the scene from +Z
/// (the side a default-imported figure faces), Right from +X (screen-right in the front view, so
/// it shows a figure's LEFT side, as in every DCC), Top from above with the front at the bottom
/// of the screen; Back/Left/Bottom are the opposite sides (Bottom keeps the front at the bottom,
/// mirroring X — the top view seen through the floor).
enum class AxisView { Front, Back, Right, Left, Top, Bottom };

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

    /// Restores an ABSOLUTE framing (the .pss load path puts back a saved camera): target + orbit
    /// angles (radians) + distance. Like reset(), the projection is left to setOrthographic().
    void restoreFraming(const glm::vec3& target, float yaw, float pitch, float distance);

    /// Orbit around the target by the given yaw/pitch deltas (radians).
    void orbit(float deltaYaw, float deltaPitch);

    /// Move toward/away from the target. Positive @p amount zooms in.
    void dolly(float amount);

    /// Slide the target (and eye) within the view plane.
    void pan(float deltaX, float deltaY);

    /// Snaps the orbit to an axis-aligned view, keeping the target and distance, and switches
    /// the projection to ORTHOGRAPHIC — a true elevation/plan drawing with no vanishing lines
    /// (the floor is edge-on in the side views), the way a DCC's fixed side cameras work.
    void setAxisView(AxisView view);

    /// Rotates the orbit exactly 180° about the world up axis — the view from the opposite side —
    /// keeping the pitch, target, distance, and projection (an orthographic front becomes an
    /// orthographic back).
    void flip();

    /// Re-aims at @p center and sets the distance so a sphere of @p radius fills the view (its
    /// narrower dimension, with a small margin), keeping the orbit angles — "frame selected".
    void frame(const glm::vec3& center, float radius);

    /// Perspective (the default) or orthographic projection. The orthographic half-height is
    /// distance · tan(fov/2), so the framing at the target plane is identical across a switch
    /// and dolly still zooms. Any orbit() switches back to perspective ("auto perspective":
    /// an axis view is a locked drawing only until the user turns it).
    void setOrthographic(bool on);
    bool orthographic() const { return m_orthographic; }

    const glm::vec3& target() const { return m_target; }
    float            pitch() const { return m_pitch; }
    float            yaw() const { return m_yaw; }
    float            distance() const { return m_distance; }

    /// An EXPLICIT view: @p view (world -> eye) and @p projection as given, bypassing the orbit
    /// framing — a VR eye as the headset tracked it (rendering/vreyeview.h), whose off-centre
    /// frustum and pose no orbit state can express. The orbit members are left untouched; the
    /// object just answers view()/viewProjection()/position() from the given matrices until an
    /// orbit mutator is called. The renderer makes a throwaway Camera per eye this way, so the
    /// desktop camera never carries VR state.
    void setExplicitView(const glm::mat4& view, const glm::mat4& projection);

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
    bool  m_orthographic = false; // see setOrthographic(); the projection depends on m_distance then
    glm::mat4 m_projection = glm::mat4(1.0f);

    // setExplicitView: the given view matrix and the eye it implies (its inverse's translation).
    bool      m_explicit = false;
    glm::mat4 m_explicitView = glm::mat4(1.0f);
    glm::vec3 m_explicitEye = glm::vec3(0.0f);
};

} // namespace pose

#endif // CAMERA_H
