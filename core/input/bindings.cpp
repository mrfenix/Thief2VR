#include "bindings.h"

#include "../config/settings.h"
#include "../ui/vr_menu.h"

namespace {

enum Kind { kHeld, kPress, kLongPress };

struct ActionInfo {
    int Settings::*setting;
    const char* label;
    Kind kind;
};

const ActionInfo kActions[kActCount] = {
    {&Settings::bind_jump, "Jump", kHeld},
    {&Settings::bind_crouch_toggle, "Toggle crouch", kPress},
    {&Settings::bind_crouch_hold, "Hold crouch", kHeld},
    {&Settings::bind_block, "Block", kHeld},
    {&Settings::bind_run_toggle, "Toggle run", kPress},
    {&Settings::bind_next_weapon, "Next weapon", kPress},
    {&Settings::bind_put_away, "Put the weapon away", kPress},
    {&Settings::bind_next_item, "Next item", kPress},
    {&Settings::bind_prev_item, "Previous item", kPress},
    {&Settings::bind_objectives, "Objectives (hold)", kLongPress},
    {&Settings::bind_map, "Map", kPress},
    {&Settings::bind_game_menu, "Game menu", kPress},
    {&Settings::bind_vr_menu, "VR menu (hold)", kLongPress},
};

struct ButtonState {
    bool down = false;
    double held = 0;        // seconds down
    bool long_fired = false;
};

ButtonState g_button[kButtonCount];
bool g_held[kActCount], g_fired[kActCount];

// With hysteresis, like the old hard-coded checks: analog buttons on above
// 0.6 and off below 0.4; the right stick up / down on past 0.7, off inside 0.5.
bool Analog(bool was, float value, float on, float off)
{
    return was ? value > off : value > on;
}

}  // namespace

void BindingsUpdate(const XrControllerState& c, double dt)
{
    bool now[kButtonCount] = {};
    if (c.active) {
        now[kButtonA] = c.a;
        now[kButtonB] = c.b;
        now[kButtonX] = c.x;
        now[kButtonY] = c.y;
        now[kButtonMenu] = c.menu;
        now[kButtonLeftStickClick] = c.stick_click[0];
        now[kButtonRightStickClick] = c.stick_click[1];
        now[kButtonRightStickUp] = Analog(g_button[kButtonRightStickUp].down, c.turn.y, 0.7f, 0.5f);
        now[kButtonRightStickDown] = Analog(g_button[kButtonRightStickDown].down, -c.turn.y, 0.7f, 0.5f);
        now[kButtonLeftTrigger] = Analog(g_button[kButtonLeftTrigger].down, c.trigger[0], 0.6f, 0.4f);
        now[kButtonLeftGrip] = Analog(g_button[kButtonLeftGrip].down, c.grip[0], 0.6f, 0.4f);
    }
    bool pressed[kButtonCount] = {}, released[kButtonCount] = {}, long_now[kButtonCount] = {};
    bool short_release[kButtonCount] = {};
    for (int b = 1; b < kButtonCount; ++b) {
        ButtonState& s = g_button[b];
        pressed[b] = now[b] && !s.down;
        released[b] = !now[b] && s.down;
        short_release[b] = released[b] && !s.long_fired;
        if (pressed[b])
            s.held = 0, s.long_fired = false;
        else if (now[b])
            s.held += dt;
        if (now[b] && !s.long_fired && s.held >= kMenuHoldSeconds)
            long_now[b] = s.long_fired = true;
        s.down = now[b];
    }
    // A press shares its button with a long press: it's a tap, on the release.
    bool has_long[kButtonCount] = {};
    for (int a = 0; a < kActCount; ++a)
        if (kActions[a].kind == kLongPress)
            has_long[BindButtonOf((BindAction)a)] = true;
    for (int a = 0; a < kActCount; ++a) {
        int b = BindButtonOf((BindAction)a);
        g_held[a] = g_fired[a] = false;
        if (b == kButtonNone)
            continue;
        switch (kActions[a].kind) {
        case kHeld: g_held[a] = now[b]; break;
        case kPress: g_fired[a] = has_long[b] ? short_release[b] : pressed[b]; break;
        case kLongPress: g_fired[a] = long_now[b]; break;
        }
    }
}

bool BindHeld(BindAction action)
{
    return g_held[action];
}

bool BindFired(BindAction action)
{
    return g_fired[action];
}

int& BindSetting(BindAction action)
{
    return Config().*kActions[action].setting;
}

int BindButtonOf(BindAction action)
{
    int b = BindSetting(action);
    return b > kButtonNone && b < kButtonCount ? b : kButtonNone;
}

const char* BindActionLabel(BindAction action)
{
    return kActions[action].label;
}

bool BindActionIsLongPress(BindAction action)
{
    return kActions[action].kind == kLongPress;
}

const char* BindButtonName(int button)
{
    const bool index = XrControls().Profile() == XrControllerProfile::Index;
    switch (button) {
    case kButtonA: return index ? "Right A" : "A";
    case kButtonB: return index ? "Right B" : "B";
    case kButtonX: return index ? "Left A" : "X";
    case kButtonY: return index ? "Left B" : "Y";
    case kButtonMenu: return index ? "Left trackpad press" : "Left menu button";
    case kButtonLeftStickClick: return "Left stick click";
    case kButtonRightStickClick: return "Right stick click";
    case kButtonRightStickUp: return "Right stick up";
    case kButtonRightStickDown: return "Right stick down";
    case kButtonLeftTrigger: return "Left trigger";
    case kButtonLeftGrip: return "Left grip";
    default: return "(none)";
    }
}
