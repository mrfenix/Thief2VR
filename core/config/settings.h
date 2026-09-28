#pragma once

// Every user-tunable setting is declared once in SETTINGS_LIST. That single list
// drives the struct below, thief2vr.ini load/save and the in-game VR settings
// menu, so adding a setting here makes it appear everywhere.
//
// X(type, name, default, min, max, tab, label)
#define SETTINGS_LIST(X)                                                                                        \
    X(bool, stereo, true, 0, 1, "View", "Stereo 3D (off = flat screen in VR)")                                  \
    X(bool, d3d9ex, true, 0, 1, "Performance", "Zero-copy GPU sharing via D3D9Ex (restart)")                  \
    X(float, render_scale, 1.0f, 0.5f, 1.5f, "View", "Resolution scale (x headset recommended, restart)")       \
    X(float, render_fov_scale, 1.0f, 0.8f, 1.2f, "View", "Rendered FOV adjust")                                 \
    X(float, world_scale, 3.2808f, 2.0f, 5.0f, "View", "World units per metre (IPD / scale)")                   \
    X(bool, positional_tracking, true, 0, 1, "View", "Positional head tracking")                                \
    X(float, position_clamp_ft, 1.0f, 0.0f, 3.0f, "View", "Max head offset from body (ft)")                     \
    X(bool, invert_roll, false, 0, 1, "View", "Invert head roll")                                               \
    X(bool, desktop_mirror, true, 0, 1, "View", "Show the game in the desktop window during missions")          \
    X(bool, snap_turn, true, 0, 1, "Comfort", "Snap turn (off = smooth turn)")                                  \
    X(float, snap_turn_angle, 30.0f, 10.0f, 90.0f, "Comfort", "Snap turn angle (deg)")                          \
    X(float, smooth_turn_speed, 120.0f, 30.0f, 360.0f, "Comfort", "Smooth turn speed (deg/s)")                  \
    X(bool, move_follows_head, true, 0, 1, "Movement", "Movement direction follows the head (off = body)")      \
    X(float, stick_deadzone, 0.15f, 0.0f, 0.5f, "Movement", "Thumbstick deadzone")                              \
    X(bool, invert_strafe, false, 0, 1, "Movement", "Invert strafing direction")                                \
    X(bool, aim_follows_head, true, 0, 1, "Movement", "Aim / frob follow the head (when not aiming by hand)")    \
    X(bool, aim_with_hand, true, 0, 1, "Hands", "Aim, frob and weapons follow the right hand")                  \
    X(bool, weapon_in_hand, true, 0, 1, "Hands", "Hold the weapon in your hand (off = in front of the view)")   \
    X(bool, swing_to_attack, true, 0, 1, "Hands", "Swing the sword / blackjack to attack")                       \
    X(float, swing_speed, 2.5f, 1.0f, 6.0f, "Hands", "Swing speed needed to attack (m/s at the blade tip)")    \
    X(float, grip_forward_ft, 1.2f, -1.0f, 4.0f, "Hands", "Weapon grip: forward of the view (ft)")              \
    X(float, grip_right_ft, 0.5f, -2.0f, 2.0f, "Hands", "Weapon grip: right of the view (ft)")                   \
    X(float, grip_down_ft, 0.9f, -1.0f, 4.0f, "Hands", "Weapon grip: below the view (ft)")                      \
    X(float, weapon_pitch_deg, 0.0f, -90.0f, 90.0f, "Hands", "Weapon angle: pitch (deg)")                        \
    X(float, weapon_yaw_deg, 0.0f, -90.0f, 90.0f, "Hands", "Weapon angle: yaw (deg)")                            \
    X(float, weapon_roll_deg, 0.0f, -180.0f, 180.0f, "Hands", "Weapon angle: roll (deg)")                        \
    X(bool, physical_crouch, true, 0, 1, "Movement", "Crouch by physically crouching")                          \
    X(float, physical_crouch_ft, 1.0f, 0.3f, 3.0f, "Movement", "Physical crouch depth (ft)")                    \
    X(bool, physical_lean, true, 0, 1, "Movement", "Lean by physically leaning")                                \
    X(float, physical_lean_ft, 0.4f, 0.1f, 1.5f, "Movement", "Physical lean distance (ft)")                     \
    X(bool, hud_enabled, true, 0, 1, "HUD", "Show HUD (light gem, inventory)")                                  \
    X(float, hud_distance, 1.2f, 0.3f, 3.0f, "HUD", "HUD distance (m)")                                         \
    X(float, hud_width, 1.4f, 0.3f, 4.0f, "HUD", "HUD width (m)")                                               \
    X(float, hud_vertical_offset, -0.05f, -1.0f, 1.0f, "HUD", "HUD vertical offset (m)")                        \
    X(float, hud_capture_scale, 0.5f, 0.25f, 1.0f, "HUD", "HUD resolution (fraction of game resolution)")       \
    X(float, screen_distance, 2.0f, 0.5f, 5.0f, "HUD", "Menu screen distance (m)")                              \
    X(float, screen_width, 2.4f, 0.5f, 6.0f, "HUD", "Menu screen width (m)")

struct Settings {
#define SETTINGS_FIELD(type, name, def, lo, hi, tab, label) type name = def;
    SETTINGS_LIST(SETTINGS_FIELD)
#undef SETTINGS_FIELD
};

// The live settings. Read freely from the render thread.
Settings& Config();

void LoadSettings();  // from thief2vr.ini next to the exe (writes defaults if missing)
void SaveSettings();

// Registry metadata, in declaration order (for the in-game menu).
enum class SettingType { Bool, Float, Int };
struct SettingInfo {
    const char* name;
    const char* label;
    const char* tab;
    SettingType type;
    void* value;  // points into Config()
    float lo, hi, def;
};
const SettingInfo* AllSettings(int& count);
void ResetSettingsTab(const char* tab);  // restore defaults for one tab
