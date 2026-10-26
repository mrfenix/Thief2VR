#pragma once
#include "../xr/xr_input.h"

// Remappable buttons. Each game action (jump, next weapon, the menus...) is
// bound to one controller button through a bind_* setting (Controls tab). The
// sticks' movement and turning, the right trigger (attack / draw), the right
// grip (frob, bow, throw, doors) and the climbing grips aren't remappable.
//
// An action is one of three kinds:
//   held:       on while its button is down (jump, crouch hold, block);
//   press:      fires once as its button goes down - or, if a long-press action
//               shares the button, as it's let go before that fires (a tap);
//   long press: fires once the button has been held kMenuHoldSeconds.

// Physical buttons. The order is what thief2vr.ini stores: append only.
enum BindButton {
    kButtonNone,
    kButtonA,
    kButtonB,
    kButtonX,
    kButtonY,
    kButtonMenu,
    kButtonLeftStickClick,
    kButtonRightStickClick,
    kButtonRightStickUp,
    kButtonRightStickDown,
    kButtonLeftTrigger,
    kButtonLeftGrip,
    kButtonCount
};

enum BindAction {
    kActJump,
    kActCrouchToggle,
    kActCrouchHold,
    kActBlock,
    kActRunToggle,
    kActNextWeapon,
    kActPutAway,
    kActNextItem,
    kActPrevItem,
    kActObjectives,
    kActMap,
    kActGameMenu,
    kActVrMenu,
    kActCount
};

// Reads the buttons and works out the actions. Call once per frame, before
// anything asks about them.
void BindingsUpdate(const XrControllerState& c, double dt);

bool BindHeld(BindAction action);   // a held action's button is down
bool BindFired(BindAction action);  // a press / long-press action fired this frame

// The button an action is bound to (its bind_* setting), and that setting.
int BindButtonOf(BindAction action);
int& BindSetting(BindAction action);
const char* BindActionLabel(BindAction action);
bool BindActionIsLongPress(BindAction action);
// A button's name on the controllers in use (e.g. "X" on Touch, "Left A" on Index).
const char* BindButtonName(int button);
