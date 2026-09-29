#pragma once
#include "../xr/xr_input.h"

// In-game VR settings menu: a world-locked panel (Dear ImGui) opened in front
// of the player. Point with the right controller, trigger = click / drag,
// right stick = scroll, B = close. The game keeps running (NewDark has no
// pause short of its own Esc menu) but controller input goes to the menu.
// Tabs and widgets come from the settings registry (config/settings.h);
// changes apply live and are saved on close.
// How long the left menu button is held to open this menu (a tap is the game's own menu).
constexpr double kMenuHoldSeconds = 0.5;

bool MenuIsOpen();
void MenuToggle(const XrFrame& frame);

// Per frame (from Present). Renders the menu when open; returns true and fills
// quad with the layer to submit.
bool MenuUpdate(const XrControllerState& c, double dt, XrCompositionLayerQuad& quad);

// A short notification on a small head-locked panel below the view, fading out
// after `seconds`. Shown while the menu is closed.
void MenuShowToast(const char* text, double seconds);
// Per frame when MenuUpdate returned false: renders the notification if one is
// showing; returns true and fills quad with its layer.
bool MenuToastUpdate(double dt, XrCompositionLayerQuad& quad);
