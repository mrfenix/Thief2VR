#pragma once
#include <d3d9.h>

// Headset brightness: a gamma / brightness pass over each eye's image (the 3D
// world and the hands; the HUD and menus are separate layers and stay as they
// are). The game's own gamma option only sets the monitor's gamma ramp, so the
// ramp it sets is taken in here and applied too (View tab: follow_game_gamma).
namespace world_gamma {

// The game's gamma ramp, from the device's SetGammaRamp (device_hooks).
void OnGameGammaRamp(const D3DGAMMARAMP* ramp);

// Brightens one eye's image (w x h) in place. Nothing at the neutral settings.
void Apply(IDirect3DDevice9* dev, IDirect3DSurface9* eye, UINT w, UINT h);

void OnDeviceReset();

}  // namespace world_gamma
