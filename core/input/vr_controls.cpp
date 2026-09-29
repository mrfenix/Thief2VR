#include "vr_controls.h"

#include "../config/settings.h"
#include "../engine/engine.h"
#include "../log.h"
#include "../render/stereo.h"
#include "../vrmath.h"
#include "../xr/xr_input.h"

#include <cmath>

namespace {

// An engine input held while a condition is true (edge-triggered press/release).
struct HeldInput {
    const char* name;
    bool down = false;

    void Set(bool want)
    {
        if (want != down && EngineInput(name, want))
            down = want;
    }
};

// A button that fires once per press (with hysteresis for analog values).
struct Edge {
    bool down = false;

    bool Pressed(bool now)
    {
        bool fired = now && !down;
        down = now;
        return fired;
    }
};

HeldInput g_use_weapon{"use_weapon"}, g_use_item{"use_item"}, g_block{"block"};
HeldInput g_crouch_hold{"crouchhold"}, g_jump{"jump"}, g_lean_left{"leanleft"}, g_lean_right{"leanright"};
Edge g_snap, g_crouch_toggle, g_run_toggle, g_map, g_a, g_b, g_x;
bool g_run_on;
bool g_moving;
double g_strike_time = -1;  // seconds the melee hit window has been open, -1 closed

void CloseStrike()
{
    if (g_strike_time >= 0) {
        EngineMeleeStrike(false);
        Log("Melee: hit window closed after %.2f s", g_strike_time);
        g_strike_time = -1;
    }
}

bool Analog(bool& state, float value)  // hysteresis: on above 0.6, off below 0.4
{
    state = state ? value > 0.4f : value > 0.6f;
    return state;
}

void ReleaseAll()
{
    for (HeldInput* h : {&g_use_weapon, &g_use_item, &g_block, &g_crouch_hold, &g_jump, &g_lean_left, &g_lean_right})
        h->Set(false);
    if (g_moving) {
        EngineSetMovement(0, 0);
        g_moving = false;
    }
    CloseStrike();
}

XrVector2f Deadzone(XrVector2f v, float dz)
{
    float len = std::sqrt(v.x * v.x + v.y * v.y);
    if (len <= dz)
        return {0, 0};
    float scaled = std::fmin(1.0f, (len - dz) / (1.0f - dz));
    return {v.x / len * scaled, v.y / len * scaled};
}

} // namespace

void ControlsUpdate(const XrControllerState& c, bool in_mission, double dt)
{
    const Settings& s = Config();
    const HeadState& head = CurrentHeadState();

    if (!c.active || !in_mission || !head.valid || !EngineInputAllowed()) {
        ReleaseAll();
        return;
    }
    // --- Movement (left stick), relative to where you look ---
    XrVector2f stick = Deadzone(c.move, s.stick_deadzone);
    if (g_run_toggle.Pressed(c.stick_click[0])) {
        g_run_on = !g_run_on;
        XrControls().Vibrate(0, 0.3f, 0.05f);
    }
    float speed = g_run_on ? 1.0f : 0.5f;
    float d = s.move_follows_head ? head.rel_heading : 0.0f;
    float forward = (stick.y * std::cos(d) + stick.x * std::sin(d)) * speed;
    float right = (stick.x * std::cos(d) - stick.y * std::sin(d)) * speed;
    if (s.invert_strafe)
        right = -right;
    if (stick.x != 0 || stick.y != 0) {
        EngineSetMovement(forward, right);
        g_moving = true;
    } else if (g_moving) {
        EngineSetMovement(0, 0);
        g_moving = false;
    }

    // --- Turning (right stick X): exact angles, applied at the engine's next
    //     player-camera update ---
    float turn_x = std::fabs(c.turn.x) > s.stick_deadzone ? c.turn.x : 0.0f;
    if (s.snap_turn) {
        bool deflected = std::fabs(turn_x) > 0.7f || (g_snap.down && std::fabs(turn_x) > 0.3f);
        if (g_snap.Pressed(deflected))
            EngineQueueTurn(-std::copysign(s.snap_turn_angle * kPi / 180.0f, turn_x));
    } else if (turn_x != 0) {
        EngineQueueTurn(-turn_x * s.smooth_turn_speed * kPi / 180.0f * (float)dt);
    }

    // --- Aim: the body's heading and pitch follow the right hand (or the head).
    //     The engine aims frob, arrows, swings and throws from the body, and
    //     places the first-person weapon relative to it. The view is unaffected.
    if (head.camera_mode == 0) {
        float aim_yaw = 0, aim_pitch = 0;
        if (s.aim_with_hand && c.pose_valid[1] && TrackedAngles(c.aim_pose[1].orientation, aim_yaw, aim_pitch)) {
            EngineSetAimYaw(aim_yaw);
            EngineSetLookPitch(aim_pitch);
        } else if (s.aim_follows_head) {
            EngineSetAimYaw(head.track_heading);
            EngineSetLookPitch(head.pitch);
        } else {
            EngineSetAimYaw(0);  // body faces the tracking forward
        }
    }

    // --- Right stick up / down: jump, toggle crouch ---
    g_jump.Set(c.turn.y > 0.7f);
    if (g_crouch_toggle.Pressed(c.turn.y < -0.7f)) {
        EngineInput("crouch", true);
        EngineInput("crouch", false);
    }

    // --- Triggers and grips ---
    static bool rt, rg, lt, lg;

    // --- Swing to attack: a fast swing taps "use weapon" (Thief's quick
    //     attack). Holding the trigger instead still winds up a charged attack.
    //     Only for the sword / blackjack arm, never the bow or a carried body.
    // Limb modes (see docs/re-map.md): 1 = bow, 2 = sword / blackjack, 4 = body.
    static double swing_hold = 0, swing_cooldown = 0;
    float tip_speed = 0;
    static Vec3 last_tip;
    static bool have_last_tip;
    swing_hold -= dt;
    swing_cooldown -= dt;
    const bool melee = EngineLimbMode() == 2;
    if (c.pose_valid[1]) {
        // A point ~40 cm along the blade.
        const XrPosef& aim = c.aim_pose[1];
        Quat q{aim.orientation.x, aim.orientation.y, aim.orientation.z, aim.orientation.w};
        Vec3 r = Rotate(q, {0, 0, -0.4f});
        Vec3 tip = Vec3{aim.position.x, aim.position.y, aim.position.z} + r;

        // Its speed: from the runtime's velocities if it reports them (v + w x r),
        // otherwise from the movement since last frame.
        float speed = 0;
        static bool logged_source;
        if (c.velocity_valid[1]) {
            const XrVector3f& v = c.linear_velocity[1];
            const XrVector3f& w = c.angular_velocity[1];
            speed = Length(Vec3{v.x + (w.y * r.z - w.z * r.y), v.y + (w.z * r.x - w.x * r.z),
                                v.z + (w.x * r.y - w.y * r.x)});
        } else if (have_last_tip && dt > 0.001) {
            speed = Length(tip - last_tip) / (float)dt;
        }
        if (!logged_source) {
            logged_source = true;
            Log("Controls: swing speed from %s", c.velocity_valid[1] ? "runtime velocities" : "pose differences");
        }
        last_tip = tip;
        have_last_tip = true;
        tip_speed = speed;

        if (s.swing_to_attack && melee && !rt && swing_cooldown <= 0 && speed > s.swing_speed) {
            swing_hold = 0.001;  // press for one frame, then release = quick swing
            swing_cooldown = 0.4;
            XrControls().Vibrate(1, 0.5f, 0.06f);
            Log("Controls: swing attack (tip %.1f m/s)", speed);
        }
    } else {
        have_last_tip = false;
    }

    bool weapon = Analog(rt, c.trigger[1]) || swing_hold > 0;
    if (weapon && !g_use_weapon.down)
        XrControls().Vibrate(1, 0.2f, 0.03f);
    bool was_down = g_use_weapon.down;
    g_use_weapon.Set(weapon);

    // --- Melee hit window: the weapon hits while your hand swings, not when
    //     the arm animation says. Opens as the attack is released (the engine
    //     has then registered it), stays open while the blade moves fast.
    EngineSetVrMelee(s.swing_to_attack);
    if (g_strike_time >= 0) {
        g_strike_time += dt;
        bool slowed = g_strike_time > 0.25 && tip_speed < s.swing_speed * 0.35f;
        if (slowed || g_strike_time > 0.6 || !melee)
            CloseStrike();
    }
    if (s.swing_to_attack && melee && was_down && !g_use_weapon.down) {
        CloseStrike();
        if (EngineMeleeStrike(true)) {
            g_strike_time = 0;
            Log("Melee: hit window open (tip %.1f m/s)", tip_speed);
        }
    }
    g_use_item.Set(Analog(rg, c.grip[1]));
    g_block.Set(Analog(lt, c.trigger[0]));

    // --- Crouch: left grip, or physically crouching ---
    static bool body_crouched;
    if (s.physical_crouch)
        body_crouched = body_crouched ? head.offset_up_ft < -s.physical_crouch_ft * 0.7f
                                      : head.offset_up_ft < -s.physical_crouch_ft;
    else
        body_crouched = false;
    g_crouch_hold.Set(Analog(lg, c.grip[0]) || body_crouched);

    // --- Lean: physically leaning sideways (relative to the body) ---
    static int body_lean;  // -1 left, 0 none, 1 right
    if (s.physical_lean) {
        float side = -head.offset_left_ft;  // + right
        float on = s.physical_lean_ft, off = s.physical_lean_ft * 0.6f;
        if (body_lean == 0)
            body_lean = side > on ? 1 : side < -on ? -1 : 0;
        else if ((body_lean > 0 && side < off) || (body_lean < 0 && side > -off))
            body_lean = 0;
    } else {
        body_lean = 0;
    }
    g_lean_left.Set(body_lean < 0);
    g_lean_right.Set(body_lean > 0);

    // --- Buttons ---
    if (g_a.Pressed(c.a))
        EngineCommand("next_weapon");
    if (g_b.Pressed(c.b))
        EngineCommand("clear_weapon");
    if (g_x.Pressed(c.x))
        EngineCommand("next_item");
    // Y: tap = previous item (on release), hold = objectives.
    static double y_held = -1;  // seconds held; -1 up, -2 hold already fired
    if (c.y) {
        if (y_held == -1)
            y_held = 0;
        else if (y_held >= 0 && (y_held += dt) >= 0.5) {
            EngineCommand("objectives");
            XrControls().Vibrate(0, 0.3f, 0.05f);
            y_held = -2;
        }
    } else {
        if (y_held >= 0)
            EngineCommand("prev_item");
        y_held = -1;
    }
    if (g_map.Pressed(c.stick_click[1]))
        EngineCommand("automap");
}
