#pragma once
#include <d3d9.h>
#include <openxr/openxr.h>

// The per-eye render targets (headset resolution, independent of the game
// window). shared[] are D3D9Ex share handles, or null when the device isn't Ex.
struct EyeTargets {
    IDirect3DSurface9* color[2] = {};
    HANDLE shared[2] = {};
    UINT width = 0, height = 0;
    unsigned generation = 0;  // bumped whenever the targets are recreated
};

// Hooks the engine's scene render so each frame is rendered once per eye with
// the headset pose applied to the camera. Requires ResolveEngine() to succeed.
bool InstallStereoHook();

// Release D3D9 DEFAULT-pool resources (call before IDirect3DDevice9::Reset).
void StereoOnDeviceReset();

// Average GPU time of the two eye passes (ms) since the last call, or -1 if
// none was measured yet.
double StereoTakeGpuMs();

// Re-capture the neutral head position and forward direction on the next frame.
void StereoRequestRecenter();

// F12: log the render state during the next frame's (unmodified) arm draws.
void StereoRequestArmDump();

// Head and body state from the last rendered frame (for the VR controls).
// Angles in radians, engine convention: heading CCW, pitch positive = down.
struct HeadState {
    bool valid = false;
    unsigned frame = 0;     // increments every stereo frame
    float rel_heading = 0;  // head heading relative to the body's facing (for movement)
    float track_heading = 0;// head heading relative to the tracking forward (recenter)
    float pitch = 0;        // head pitch
    float offset_forward_ft = 0, offset_left_ft = 0, offset_up_ft = 0;  // head vs. neutral, body frame
    float body_heading = 0; // the engine camera's (player's) heading
    float body_pitch = 0;
    int camera_mode = 0;    // 0 = player camera
};
const HeadState& CurrentHeadState();

// Converts a tracked orientation (LOCAL space, e.g. a controller's aim pose)
// into heading relative to the tracking forward and pitch, engine convention.
bool TrackedAngles(const XrQuaternionf& orientation, float& heading, float& pitch);

// The same for a direction (LOCAL space), e.g. from one hand to the other.
bool TrackedDirectionAngles(const XrVector3f& direction, float& heading, float& pitch);

// Converts a tracked point (LOCAL space, metres) into an offset from the game
// camera in engine world axes (feet), using the same mapping as the rendered
// eyes. world_yaw is the view's world yaw (EngineWorldYaw).
bool TrackedPointToCameraOffset(const XrVector3f& point, float world_yaw, float out[3]);
