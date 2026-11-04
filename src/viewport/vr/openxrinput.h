/**
 * @file openxrinput.h
 * @brief The VR controllers through OpenXR's action system: the sticks, the triggers, the
 *        buttons, the aim poses — and a haptic pulse back.
 *
 * OpenXR input is declared, not polled: the application names ACTIONS ("move", "select", "aim")
 * and SUGGESTS which physical inputs of each known controller family drive them; the runtime
 * picks the family of the controllers actually in use (and lets the user rebind). Every frame
 * xrSyncActions snapshots the actions and the states are read back. One action set covers the
 * whole of VR Mode:
 *
 *   move      the LEFT thumbstick (x = strafe, y = forward/back), head-relative locomotion
 *   turn      the RIGHT thumbstick (x = snap turn; y = push/pull a dragged joint)
 *   select    each hand's trigger, analog (a press is thresholded with hysteresis by the caller)
 *   pin       each hand's primary face button (A / X on Touch): pin / unpin the selected joint
 *   recenter  the left menu button: back in front of the figure
 *   aim       each hand's aim pose — the controller's pointing ray
 *   haptic    each hand's vibration — a tick on a grab
 *
 * Bindings are suggested for the Meta Touch, Valve Index, Windows Mixed Reality and HTC Vive
 * controllers and for the Khronos "simple" profile (select + menu only); a runtime that knows
 * none of them has no controllers to offer and the headset is view-only. Created with the
 * session, attached before it begins; the aim poses are located in the caller's reference space.
 * Pure OpenXR + GLM + std, no Qt.
 */

#ifndef OPENXRINPUT_H
#define OPENXRINPUT_H

#include <openxr/openxr.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace pose {

/// One controller's state for a frame, poses in the caller's reference space.
struct VrHandInput {
    bool      aimValid = false;  ///< The aim pose is tracked this frame.
    glm::vec3 aimPosition{0.0f};
    glm::quat aimOrientation{1.0f, 0.0f, 0.0f, 0.0f}; ///< -Z = the pointing direction
    float     select = 0.0f;     ///< The trigger, 0..1 (0 when unbound / inactive).
    bool      pinPressed = false; ///< The pin button went down this frame.
};

/// Everything the controllers say for one frame.
struct VrInput {
    bool        focused = false;  ///< The session has input focus (else every value is at rest).
    glm::vec2   move{0.0f};       ///< Left stick: x right, y forward, each -1..1.
    glm::vec2   turn{0.0f};       ///< Right stick.
    bool        recenterPressed = false;
    VrHandInput hand[2];          ///< 0 = left, 1 = right.
};

class OpenXrInput {
public:
    /// Creates the action set, suggests the bindings and ATTACHES the set to @p session (which
    /// must not have begun yet). Null with @p failReason set if the runtime accepts none of the
    /// controller profiles or any OpenXR call fails.
    static std::unique_ptr<OpenXrInput> create(XrInstance instance, XrSession session, std::string& failReason);
    ~OpenXrInput();

    OpenXrInput(const OpenXrInput&) = delete;
    OpenXrInput& operator=(const OpenXrInput&) = delete;

    /// Syncs the actions and reads this frame's state; the aim poses are located in @p baseSpace
    /// at @p time. False only when the runtime refuses the sync (the session is not running).
    bool sync(int64_t time, XrSpace baseSpace, VrInput& out);

    /// A vibration on hand @p hand (0 left, 1 right) of @p seconds at @p amplitude (0..1).
    void pulse(int hand, float seconds, float amplitude);

private:
    OpenXrInput() = default;
    bool createActions(std::string& failReason);
    bool suggestBindings(std::string& failReason);

    XrInstance  m_instance = XR_NULL_HANDLE; // borrowed
    XrSession   m_session = XR_NULL_HANDLE;  // borrowed
    XrActionSet m_actionSet = XR_NULL_HANDLE;
    XrAction    m_move = XR_NULL_HANDLE;
    XrAction    m_turn = XR_NULL_HANDLE;
    XrAction    m_select = XR_NULL_HANDLE;
    XrAction    m_pin = XR_NULL_HANDLE;
    XrAction    m_recenter = XR_NULL_HANDLE;
    XrAction    m_aim = XR_NULL_HANDLE;
    XrAction    m_haptic = XR_NULL_HANDLE;
    XrPath      m_handPaths[2] = {XR_NULL_PATH, XR_NULL_PATH}; // /user/hand/left, /user/hand/right
    XrSpace     m_aimSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
};

} // namespace pose

#endif // OPENXRINPUT_H
