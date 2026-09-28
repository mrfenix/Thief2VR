#pragma once
#include "../xr/xr_input.h"

#include <windows.h>

// Laser pointer for the game's own 2D screens (menus, map, objectives, books)
// shown on the virtual screen quad. The right controller's aim ray moves the
// game's mouse cursor to where it hits the screen, the right trigger clicks and
// B presses Esc (back / close). Input is injected as real OS input (SetCursorPos
// and SendInput), so it only goes out while the game window has the focus.
//
// Per frame while the game screen is shown (and the VR menu is closed). Returns
// true and fills dot with a small pointer layer when the ray hits the screen.
bool ScreenPointerUpdate(const XrControllerState& c, const XrPosef& screen_pose, float width_m, float height_m,
                         HWND window, XrCompositionLayerQuad& dot);

// Per frame while the game screen isn't shown: lets go of a held click.
void ScreenPointerIdle();

// Presses Esc in the game window (released on the next frame). Returns false
// when the game window doesn't have the focus.
bool ScreenPointerSendEsc(HWND window);
