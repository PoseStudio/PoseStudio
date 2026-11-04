/**
 * @file openxrinput.cpp
 * @brief Implementation of OpenXrInput. See openxrinput.h.
 */

#include "openxrinput.h"

#include "openxrsystem.h" // resultName

#include <cstdio>
#include <cstring>
#include <vector>

namespace pose {

namespace {

constexpr const char* kHandPathStrings[2] = {"/user/hand/left", "/user/hand/right"};

} // namespace

std::unique_ptr<OpenXrInput> OpenXrInput::create(XrInstance instance, XrSession session, std::string& failReason) {
    std::unique_ptr<OpenXrInput> input(new OpenXrInput());
    input->m_instance = instance;
    input->m_session = session;
    for (int hand = 0; hand < 2; ++hand) {
        if (XR_FAILED(xrStringToPath(instance, kHandPathStrings[hand], &input->m_handPaths[hand]))) {
            failReason = "The OpenXR runtime does not know the hand paths.";
            return nullptr;
        }
    }
    if (!input->createActions(failReason) || !input->suggestBindings(failReason)) {
        return nullptr;
    }

    // The aim pose spaces: one per hand, the controller's own pointing frame.
    for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = input->m_aim;
        spaceInfo.subactionPath = input->m_handPaths[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        const XrResult result = xrCreateActionSpace(session, &spaceInfo, &input->m_aimSpaces[hand]);
        if (XR_FAILED(result)) {
            failReason = "The controller aim space could not be created (" +
                         OpenXrSystem::resultName(instance, result) + ").";
            return nullptr;
        }
    }

    // Attach: once per session, before it begins. After this the action set is fixed.
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &input->m_actionSet;
    const XrResult result = xrAttachSessionActionSets(session, &attach);
    if (XR_FAILED(result)) {
        failReason = "The controller actions could not be attached to the session (" +
                     OpenXrSystem::resultName(instance, result) + ").";
        return nullptr;
    }
    return input;
}

OpenXrInput::~OpenXrInput() {
    for (XrSpace& space : m_aimSpaces) {
        if (space != XR_NULL_HANDLE) {
            xrDestroySpace(space);
            space = XR_NULL_HANDLE;
        }
    }
    if (m_actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(m_actionSet); // destroys its actions with it
    }
}

bool OpenXrInput::createActions(std::string& failReason) {
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(setInfo.actionSetName, "posestudio", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(setInfo.localizedActionSetName, "PoseStudio", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    setInfo.priority = 0;
    XrResult result = xrCreateActionSet(m_instance, &setInfo, &m_actionSet);
    if (XR_FAILED(result)) {
        failReason = "The controller action set could not be created (" +
                     OpenXrSystem::resultName(m_instance, result) + ").";
        return false;
    }

    const auto make = [&](const char* name, const char* localized, XrActionType type, bool perHand,
                          XrAction& out) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.actionType = type;
        info.countSubactionPaths = perHand ? 2u : 0u;
        info.subactionPaths = perHand ? m_handPaths : nullptr;
        const XrResult r = xrCreateAction(m_actionSet, &info, &out);
        if (XR_FAILED(r)) {
            failReason = std::string("The controller action '") + name + "' could not be created (" +
                         OpenXrSystem::resultName(m_instance, r) + ").";
            return false;
        }
        return true;
    };
    return make("move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, false, m_move) &&
           make("turn", "Turn", XR_ACTION_TYPE_VECTOR2F_INPUT, false, m_turn) &&
           make("select", "Select / Grab", XR_ACTION_TYPE_FLOAT_INPUT, true, m_select) &&
           make("pin", "Pin Joint", XR_ACTION_TYPE_BOOLEAN_INPUT, true, m_pin) &&
           make("recenter", "Recenter", XR_ACTION_TYPE_BOOLEAN_INPUT, false, m_recenter) &&
           make("aim", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT, true, m_aim) &&
           make("haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true, m_haptic);
}

bool OpenXrInput::suggestBindings(std::string& failReason) {
    struct Binding {
        XrAction    action;
        const char* path;
    };
    struct Profile {
        const char*          path;
        std::vector<Binding> bindings;
    };
    // The physical inputs of each controller family, by the OpenXR interaction-profile paths.
    // The analog trigger value drives "select" where there is one (the caller thresholds it with
    // hysteresis); the simple profile's select/click arrives as 0/1 through the same action.
    const std::vector<Profile> profiles = {
        {"/interaction_profiles/oculus/touch_controller",
         {{m_move, "/user/hand/left/input/thumbstick"},
          {m_turn, "/user/hand/right/input/thumbstick"},
          {m_select, "/user/hand/left/input/trigger/value"},
          {m_select, "/user/hand/right/input/trigger/value"},
          {m_pin, "/user/hand/left/input/x/click"},
          {m_pin, "/user/hand/right/input/a/click"},
          {m_recenter, "/user/hand/left/input/menu/click"},
          {m_aim, "/user/hand/left/input/aim/pose"},
          {m_aim, "/user/hand/right/input/aim/pose"},
          {m_haptic, "/user/hand/left/output/haptic"},
          {m_haptic, "/user/hand/right/output/haptic"}}},
        {"/interaction_profiles/valve/index_controller",
         {{m_move, "/user/hand/left/input/thumbstick"},
          {m_turn, "/user/hand/right/input/thumbstick"},
          {m_select, "/user/hand/left/input/trigger/value"},
          {m_select, "/user/hand/right/input/trigger/value"},
          {m_pin, "/user/hand/left/input/a/click"},
          {m_pin, "/user/hand/right/input/a/click"},
          {m_recenter, "/user/hand/left/input/b/click"},
          {m_aim, "/user/hand/left/input/aim/pose"},
          {m_aim, "/user/hand/right/input/aim/pose"},
          {m_haptic, "/user/hand/left/output/haptic"},
          {m_haptic, "/user/hand/right/output/haptic"}}},
        {"/interaction_profiles/microsoft/motion_controller",
         {{m_move, "/user/hand/left/input/thumbstick"},
          {m_turn, "/user/hand/right/input/thumbstick"},
          {m_select, "/user/hand/left/input/trigger/value"},
          {m_select, "/user/hand/right/input/trigger/value"},
          {m_pin, "/user/hand/left/input/trackpad/click"},
          {m_pin, "/user/hand/right/input/trackpad/click"},
          {m_recenter, "/user/hand/left/input/menu/click"},
          {m_aim, "/user/hand/left/input/aim/pose"},
          {m_aim, "/user/hand/right/input/aim/pose"},
          {m_haptic, "/user/hand/left/output/haptic"},
          {m_haptic, "/user/hand/right/output/haptic"}}},
        {"/interaction_profiles/htc/vive_controller",
         {{m_move, "/user/hand/left/input/trackpad"},
          {m_turn, "/user/hand/right/input/trackpad"},
          {m_select, "/user/hand/left/input/trigger/value"},
          {m_select, "/user/hand/right/input/trigger/value"},
          {m_pin, "/user/hand/left/input/squeeze/click"},
          {m_pin, "/user/hand/right/input/squeeze/click"},
          {m_recenter, "/user/hand/left/input/menu/click"},
          {m_aim, "/user/hand/left/input/aim/pose"},
          {m_aim, "/user/hand/right/input/aim/pose"},
          {m_haptic, "/user/hand/left/output/haptic"},
          {m_haptic, "/user/hand/right/output/haptic"}}},
        {"/interaction_profiles/khr/simple_controller",
         {{m_select, "/user/hand/left/input/select/click"},
          {m_select, "/user/hand/right/input/select/click"},
          {m_recenter, "/user/hand/left/input/menu/click"},
          {m_aim, "/user/hand/left/input/aim/pose"},
          {m_aim, "/user/hand/right/input/aim/pose"},
          {m_haptic, "/user/hand/left/output/haptic"},
          {m_haptic, "/user/hand/right/output/haptic"}}},
    };

    int accepted = 0;
    for (const Profile& profile : profiles) {
        XrPath profilePath = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(m_instance, profile.path, &profilePath))) {
            continue;
        }
        std::vector<XrActionSuggestedBinding> suggested;
        bool complete = true;
        for (const Binding& binding : profile.bindings) {
            XrPath path = XR_NULL_PATH;
            if (XR_FAILED(xrStringToPath(m_instance, binding.path, &path))) {
                complete = false;
                break;
            }
            suggested.push_back({binding.action, path});
        }
        if (!complete) {
            continue;
        }
        XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile = profilePath;
        info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
        info.suggestedBindings = suggested.data();
        const XrResult result = xrSuggestInteractionProfileBindings(m_instance, &info);
        if (XR_SUCCEEDED(result)) {
            ++accepted;
        } else {
            // A runtime that does not know a family (XR_ERROR_PATH_UNSUPPORTED) just lacks it.
            std::fprintf(stderr, "[vr] controller profile %s not accepted: %s\n", profile.path,
                         OpenXrSystem::resultName(m_instance, result).c_str());
        }
    }
    if (accepted == 0) {
        failReason = "The OpenXR runtime accepted none of the controller profiles.";
        return false;
    }
    std::fprintf(stderr, "[vr] controller bindings suggested for %d profile(s)\n", accepted);
    return true;
}

bool OpenXrInput::sync(int64_t time, XrSpace baseSpace, VrInput& out) {
    out = VrInput{};
    XrActiveActionSet active{m_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    const XrResult synced = xrSyncActions(m_session, &syncInfo);
    if (XR_FAILED(synced)) {
        return false;
    }
    out.focused = synced == XR_SUCCESS; // XR_SESSION_NOT_FOCUSED: a success code, every action inactive

    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};

    get.action = m_move;
    get.subactionPath = XR_NULL_PATH;
    XrActionStateVector2f vec{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &get, &vec)) && vec.isActive) {
        out.move = glm::vec2(vec.currentState.x, vec.currentState.y);
    }
    get.action = m_turn;
    vec = XrActionStateVector2f{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &get, &vec)) && vec.isActive) {
        out.turn = glm::vec2(vec.currentState.x, vec.currentState.y);
    }
    get.action = m_recenter;
    XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &get, &button)) && button.isActive) {
        out.recenterPressed = button.changedSinceLastSync == XR_TRUE && button.currentState == XR_TRUE;
    }

    for (int hand = 0; hand < 2; ++hand) {
        VrHandInput& h = out.hand[hand];
        get.subactionPath = m_handPaths[hand];

        get.action = m_select;
        XrActionStateFloat trigger{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &get, &trigger)) && trigger.isActive) {
            h.select = trigger.currentState;
        }
        get.action = m_pin;
        button = XrActionStateBoolean{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &get, &button)) && button.isActive) {
            h.pinPressed = button.changedSinceLastSync == XR_TRUE && button.currentState == XR_TRUE;
        }
        get.action = m_aim;
        XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
        if (XR_SUCCEEDED(xrGetActionStatePose(m_session, &get, &pose)) && pose.isActive) {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            const XrSpaceLocationFlags needed =
                XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if (XR_SUCCEEDED(xrLocateSpace(m_aimSpaces[hand], baseSpace, time, &location)) &&
                (location.locationFlags & needed) == needed) {
                h.aimValid = true;
                h.aimPosition = glm::vec3(location.pose.position.x, location.pose.position.y, location.pose.position.z);
                h.aimOrientation = glm::quat(location.pose.orientation.w, location.pose.orientation.x,
                                             location.pose.orientation.y, location.pose.orientation.z);
            }
        }
    }
    return true;
}

void OpenXrInput::pulse(int hand, float seconds, float amplitude) {
    if (hand < 0 || hand > 1) {
        return;
    }
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.duration = static_cast<XrDuration>(seconds * 1.0e9f);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    vibration.amplitude = amplitude;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = m_haptic;
    info.subactionPath = m_handPaths[hand];
    xrApplyHapticFeedback(m_session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

} // namespace pose
