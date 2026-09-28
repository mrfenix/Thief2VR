#pragma once
#include "../xr/xr_input.h"

// In-game VR settings menu: a world-locked panel (Dear ImGui) opened in front
// of the player. Point with the right controller, trigger = click / drag,
// right stick = scroll, B = close. The game keeps running (NewDark has no
// pause short of its own Esc menu) but controller input goes to the menu.
// Tabs and widgets come from the settings registry (config/settings.h);
// changes apply live and are saved on close.
bool MenuIsOpen();
void MenuToggle(const XrFrame& frame);

// Per frame (from Present). Renders the menu when open; returns true and fills
// quad with the layer to submit.
bool MenuUpdate(const XrControllerState& c, double dt, XrCompositionLayerQuad& quad);
