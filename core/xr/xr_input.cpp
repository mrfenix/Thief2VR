#include "xr_input.h"

#include "../log.h"

#include <cstring>
#include <vector>

XrInput& XrControls()
{
    static XrInput input;
    return input;
}

namespace {

XrPath Path(const char* s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(Xr().instance(), s, &p);
    return p;
}

XrAction MakeAction(XrActionSet set, const char* name, const char* label, XrActionType type,
                    const XrPath* subpaths, uint32_t subpath_count)
{
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    strcpy_s(info.actionName, name);
    strcpy_s(info.localizedActionName, label);
    info.actionType = type;
    info.countSubactionPaths = subpath_count;
    info.subactionPaths = subpaths;
    XrAction action = XR_NULL_HANDLE;
    XrCheck(xrCreateAction(set, &info, &action), name);
    return action;
}

} // namespace

bool XrInput::Init()
{
    if (set_)
        return true;
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "gameplay");
    strcpy_s(si.localizedActionSetName, "Gameplay");
    if (!XrCheck(xrCreateActionSet(Xr().instance(), &si, &set_), "xrCreateActionSet"))
        return false;

    hand_path_[0] = Path("/user/hand/left");
    hand_path_[1] = Path("/user/hand/right");
    move_ = MakeAction(set_, "move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, nullptr, 0);
    turn_ = MakeAction(set_, "turn", "Turn / jump / crouch", XR_ACTION_TYPE_VECTOR2F_INPUT, nullptr, 0);
    trigger_ = MakeAction(set_, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, hand_path_, 2);
    grip_ = MakeAction(set_, "grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, hand_path_, 2);
    a_ = MakeAction(set_, "button_a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr, 0);
    b_ = MakeAction(set_, "button_b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr, 0);
    x_ = MakeAction(set_, "button_x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr, 0);
    y_ = MakeAction(set_, "button_y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr, 0);
    stick_click_ = MakeAction(set_, "stick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT, hand_path_, 2);
    menu_ = MakeAction(set_, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr, 0);
    grip_pose_ = MakeAction(set_, "grip_pose", "Hand pose", XR_ACTION_TYPE_POSE_INPUT, hand_path_, 2);
    aim_pose_ = MakeAction(set_, "aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT, hand_path_, 2);
    haptic_ = MakeAction(set_, "haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, hand_path_, 2);

    std::vector<XrActionSuggestedBinding> b = {
        {move_, Path("/user/hand/left/input/thumbstick")},
        {turn_, Path("/user/hand/right/input/thumbstick")},
        {trigger_, Path("/user/hand/left/input/trigger/value")},
        {trigger_, Path("/user/hand/right/input/trigger/value")},
        {grip_, Path("/user/hand/left/input/squeeze/value")},
        {grip_, Path("/user/hand/right/input/squeeze/value")},
        {a_, Path("/user/hand/right/input/a/click")},
        {b_, Path("/user/hand/right/input/b/click")},
        {x_, Path("/user/hand/left/input/x/click")},
        {y_, Path("/user/hand/left/input/y/click")},
        {stick_click_, Path("/user/hand/left/input/thumbstick/click")},
        {stick_click_, Path("/user/hand/right/input/thumbstick/click")},
        {menu_, Path("/user/hand/left/input/menu/click")},
        {grip_pose_, Path("/user/hand/left/input/grip/pose")},
        {grip_pose_, Path("/user/hand/right/input/grip/pose")},
        {aim_pose_, Path("/user/hand/left/input/aim/pose")},
        {aim_pose_, Path("/user/hand/right/input/aim/pose")},
        {haptic_, Path("/user/hand/left/output/haptic")},
        {haptic_, Path("/user/hand/right/output/haptic")},
    };
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
    suggested.suggestedBindings = b.data();
    suggested.countSuggestedBindings = (uint32_t)b.size();
    XrCheck(xrSuggestInteractionProfileBindings(Xr().instance(), &suggested), "suggest Touch bindings");

    for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo asi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asi.subactionPath = hand_path_[hand];
        asi.poseInActionSpace.orientation.w = 1.0f;
        asi.action = grip_pose_;
        xrCreateActionSpace(Xr().session(), &asi, &grip_space_[hand]);
        asi.action = aim_pose_;
        xrCreateActionSpace(Xr().session(), &asi, &aim_space_[hand]);
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &set_;
    if (!XrCheck(xrAttachSessionActionSets(Xr().session(), &attach), "xrAttachSessionActionSets"))
        return false;
    Log("OpenXR input: Touch controller actions ready");
    return true;
}

void XrInput::Update(XrTime display_time, XrControllerState& s)
{
    s = XrControllerState{};
    if (!set_)
        return;
    XrActiveActionSet active{set_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (xrSyncActions(Xr().session(), &sync) != XR_SUCCESS)  // XR_SESSION_NOT_FOCUSED etc.
        return;
    s.active = true;

    auto vec2 = [&](XrAction a) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        xrGetActionStateVector2f(Xr().session(), &gi, &st);
        return st.isActive ? st.currentState : XrVector2f{};
    };
    auto flt = [&](XrAction a, XrPath sub) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        xrGetActionStateFloat(Xr().session(), &gi, &st);
        return st.isActive ? st.currentState : 0.0f;
    };
    auto boolean = [&](XrAction a, XrPath sub) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        xrGetActionStateBoolean(Xr().session(), &gi, &st);
        return st.isActive && st.currentState;
    };

    s.move = vec2(move_);
    s.turn = vec2(turn_);
    s.a = boolean(a_, XR_NULL_PATH);
    s.b = boolean(b_, XR_NULL_PATH);
    s.x = boolean(x_, XR_NULL_PATH);
    s.y = boolean(y_, XR_NULL_PATH);
    s.menu = boolean(menu_, XR_NULL_PATH);
    for (int hand = 0; hand < 2; ++hand) {
        s.trigger[hand] = flt(trigger_, hand_path_[hand]);
        s.grip[hand] = flt(grip_, hand_path_[hand]);
        s.stick_click[hand] = boolean(stick_click_, hand_path_[hand]);

        XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION, &vel};
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (grip_space_[hand] &&
            XR_SUCCEEDED(xrLocateSpace(grip_space_[hand], Xr().local_space(), display_time, &loc)) &&
            (loc.locationFlags & need) == need) {
            s.grip_pose[hand] = loc.pose;
            s.pose_valid[hand] = true;
            const XrSpaceVelocityFlags vneed = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
            if ((vel.velocityFlags & vneed) == vneed) {
                s.linear_velocity[hand] = vel.linearVelocity;
                s.angular_velocity[hand] = vel.angularVelocity;
                s.velocity_valid[hand] = true;
            }
        }
        loc = {XR_TYPE_SPACE_LOCATION};  // no velocity chained for the aim pose
        if (aim_space_[hand] &&
            XR_SUCCEEDED(xrLocateSpace(aim_space_[hand], Xr().local_space(), display_time, &loc)) &&
            (loc.locationFlags & need) == need)
            s.aim_pose[hand] = loc.pose;
    }
}

bool XrInput::LocateHand(int hand, XrTime time, XrPosef& grip, XrPosef& aim)
{
    if (!set_ || !grip_space_[hand] || !aim_space_[hand])
        return false;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    XrSpaceLocation g{XR_TYPE_SPACE_LOCATION}, a{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(grip_space_[hand], Xr().local_space(), time, &g)) || (g.locationFlags & need) != need ||
        XR_FAILED(xrLocateSpace(aim_space_[hand], Xr().local_space(), time, &a)) || (a.locationFlags & need) != need)
        return false;
    grip = g.pose;
    aim = a.pose;
    return true;
}

void XrInput::Vibrate(int hand, float amplitude, float seconds)
{
    if (!set_)
        return;
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amplitude;
    v.duration = (XrDuration)(seconds * 1e9);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = haptic_;
    info.subactionPath = hand_path_[hand];
    xrApplyHapticFeedback(Xr().session(), &info, reinterpret_cast<const XrHapticBaseHeader*>(&v));
}
