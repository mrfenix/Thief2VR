#pragma once
#include "xr_system.h"

// Controller state for one frame (Touch controllers; other profiles map onto
// the same actions where the runtime supports them).
struct XrControllerState {
    bool active = false;  // actions synced while the session has focus

    XrVector2f move{};  // left thumbstick
    XrVector2f turn{};  // right thumbstick
    float trigger[2] = {};  // 0 left, 1 right
    float grip[2] = {};
    bool a = false, b = false, x = false, y = false;
    bool stick_click[2] = {};
    bool menu = false;  // left menu button

    // Hand poses in LOCAL space (valid[] false when not tracked).
    XrPosef grip_pose[2]{};
    XrPosef aim_pose[2]{};
    bool pose_valid[2] = {};
    // Grip velocities in LOCAL space (m/s, rad/s), when the runtime provides them.
    XrVector3f linear_velocity[2]{};
    XrVector3f angular_velocity[2]{};
    bool velocity_valid[2] = {};
};

class XrInput {
public:
    // Creates the action set and suggests bindings; call once after the session
    // exists. Returns false if input isn't available (the game still works).
    bool Init();
    // xrSyncActions + read everything. Call once per frame.
    void Update(XrTime display_time, XrControllerState& state);
    // Haptic pulse on hand 0 (left) / 1 (right). amplitude 0..1.
    void Vibrate(int hand, float amplitude, float seconds);
    // Grip and aim poses of a hand at a given time (LOCAL space). False if untracked.
    bool LocateHand(int hand, XrTime time, XrPosef& grip, XrPosef& aim);

private:
    XrActionSet set_ = XR_NULL_HANDLE;
    XrPath hand_path_[2] = {};
    XrAction move_ = XR_NULL_HANDLE, turn_ = XR_NULL_HANDLE, trigger_ = XR_NULL_HANDLE, grip_ = XR_NULL_HANDLE;
    XrAction a_ = XR_NULL_HANDLE, b_ = XR_NULL_HANDLE, x_ = XR_NULL_HANDLE, y_ = XR_NULL_HANDLE;
    XrAction stick_click_ = XR_NULL_HANDLE, menu_ = XR_NULL_HANDLE;
    XrAction grip_pose_ = XR_NULL_HANDLE, aim_pose_ = XR_NULL_HANDLE, haptic_ = XR_NULL_HANDLE;
    XrSpace grip_space_[2] = {}, aim_space_[2] = {};
};

XrInput& XrControls();
