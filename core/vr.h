#pragma once
#include <d3d9.h>

#include "render/stereo.h"
#include "xr/xr_system.h"

// Per-frame VR work. OpenXR starts on first use; if that fails (no headset or
// runtime) every function here is a no-op and the game stays flat.

// Starts OpenXR now (normally it starts on first use). Returns false when no
// headset/runtime is available.
bool VrInitEarly();

// From the game's Present: submits this frame to the headset. In menus (no 3D
// scene rendered this frame) the flat back buffer is shown on a virtual screen.
// Returns whether the game's own Present (to the desktop window) should run.
bool VrOnPresent(IDirect3DDevice9* device);

// Time the desktop Present took, for the timing log.
void VrRecordDesktopPresent(double ms);

// Must be called before IDirect3DDevice9::Reset.
void VrOnDeviceReset();

// From the scene render hook: begins the OpenXR frame (if not already begun
// this game frame) and returns it, or nullptr when nothing should be rendered.
const XrFrame* VrBeginFrameForRender();

// From the scene render hook after both eyes were rendered into eyes.
// poses/fov are what the images were rendered with; render_ms is the CPU time
// the two eye passes took (for the timing log).
void VrOnEyesRendered(IDirect3DDevice9* device, const EyeTargets& eyes, const XrPosef poses[2], const XrFovf& fov,
                      double render_ms);

// Milliseconds on a monotonic clock.
double VrNowMs();

// Recent frame timing, as text (for the VR menu's Performance tab).
const char* VrTimingSummary();
