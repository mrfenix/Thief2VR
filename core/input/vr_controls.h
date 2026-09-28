#pragma once
#include "../xr/xr_input.h"

// Maps Touch controllers (and physical head movement) onto the game's own
// player controls. Call once per frame from Present.
//
// Default layout:
//   Left stick        move (relative to where you look)     Left stick click   toggle run
//   Right stick L/R   snap / smooth turn                    Right stick up     jump
//   Right stick down  toggle crouch                         Right stick click  map
//   Right trigger     use weapon (swing / draw bow)          Right grip         frob / use item
//   Left trigger      block                                 Left grip          hold crouch
//   A / B             next weapon / sheathe                 X / Y              next / previous item
//   Y (hold)          objectives
//   Menu (left)       tap: game menu (Esc), hold: VR settings menu (handled in vr.cpp)
//   On the game's 2D screens (menus, map, objectives, books; input/screen_pointer.h):
//   point with the right controller, right trigger = click, B = Esc (back / close).
//   Crouch or lean with your body to crouch / lean in game.
void ControlsUpdate(const XrControllerState& controllers, bool in_mission, double dt_seconds);
