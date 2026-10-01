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
bool g_bow_drawing;         // two-handed bow: the right hand is drawing the string
double g_bow_draw_seconds = -1;  // how long "use weapon" has been held with the bow out, or -1

Vec3 GripPos(const XrControllerState& c, int hand)
{
    const XrVector3f& p = c.grip_pose[hand].position;
    return {p.x, p.y, p.z};
}

void CloseStrike()
{
    if (g_strike_time >= 0) {
        EngineMeleeStrike(false);
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
    g_bow_drawing = false;
    g_bow_draw_seconds = -1;
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

XrControllerState g_last_state;

const XrControllerState& ControlsLastState()
{
    return g_last_state;
}

void ControlsUpdate(const XrControllerState& c, bool in_mission, double dt)
{
    g_last_state = c;
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
    // Climbing (a ladder or rope): the engine climbs in the direction the body
    // looks (look up and go forward to climb up). That pitch follows the hand
    // here, so instead: stick forward climbs up, back climbs down (the look
    // pitch is set to match below), wherever you look or point.
    const bool climbing = EnginePlayerMode() == kPlayerModeClimb;
    float climb_pitch = 0;
    if (climbing) {
        const float kClimbPitch = 60.0f * kPi / 180.0f;
        climb_pitch = stick.y >= 0 ? -kClimbPitch : kClimbPitch;  // positive = down
        forward = std::fabs(stick.y) * speed;
        right = stick.x * speed;
    }
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
    //     The bow: along the bow's arrow (the hand's pointing plus the Bow angle
    //     sliders, as it's drawn); while drawing the two-handed bow, blended into
    //     the line from the string hand through the bow hand.
    if (head.camera_mode == 0) {
        float aim_yaw = 0, aim_pitch = 0;
        const bool bow_out = EngineLimbMode() == 1;
        const int bow_hand = s.bow_two_handed ? 0 : 1;
        bool bow_aim = bow_out && StereoBowAim(aim_yaw, aim_pitch);
        if (!bow_aim && bow_out && c.pose_valid[bow_hand] && (!s.bow_two_handed || g_bow_drawing)) {
            // (Before the bow is shown: the hand's pointing, plus the Bow angle sliders.)
            const float deg = kPi / 180.0f;
            Mat3 adjust = RotZ(s.bow_yaw_deg * deg) * RotY(s.bow_pitch_deg * deg) * RotX(s.bow_roll_deg * deg);
            const XrQuaternionf& q = c.aim_pose[bow_hand].orientation;
            Vec3 arrow = Rotate({q.x, q.y, q.z, q.w}, Transpose(XrToEngineBasis()) * (adjust * Vec3{1, 0, 0}));
            if (g_bow_drawing)
                arrow = BowArrowLine(arrow, GripPos(c, 0), GripPos(c, 1));
            bow_aim = TrackedDirectionAngles({arrow.x, arrow.y, arrow.z}, aim_yaw, aim_pitch);
        }
        if (bow_aim) {
            EngineSetAimYaw(aim_yaw);
            EngineSetLookPitch(aim_pitch);
        } else if (s.aim_with_hand && c.pose_valid[1] &&
                   TrackedAngles(c.aim_pose[1].orientation, aim_yaw, aim_pitch)) {
            EngineSetAimYaw(aim_yaw);
            EngineSetLookPitch(aim_pitch);
        } else if (s.aim_follows_head) {
            EngineSetAimYaw(head.track_heading);
            EngineSetLookPitch(head.pitch);
        } else {
            EngineSetAimYaw(0);  // body faces the tracking forward
        }
        if (climbing)
            EngineSetLookPitch(climb_pitch);
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
            // A quick follow-up: the last strike ends first (its attack ends and
            // the arm is ready), or the game would refuse this one.
            CloseStrike();
            EngineMeleeReady();
            swing_hold = 0.001;  // press for one frame, then release = quick swing
            swing_cooldown = 0.4;
            XrControls().Vibrate(1, 0.5f, 0.06f);
        }
    } else {
        have_last_tip = false;
    }

    // --- Two-handed bow (limb mode 1): the bow is held in the left hand.
    //     Squeezing the right grip at the bow nocks an arrow and draws; letting
    //     go fires. A grip press away from the bow still frobs / uses items.
    const bool bow = s.bow_two_handed && EngineLimbMode() == 1 && c.pose_valid[0] && c.pose_valid[1];
    const bool grip = Analog(rg, c.grip[1]);
    static bool grip_was, grip_draws;
    const float hands_m = bow ? Length(GripPos(c, 0) - GripPos(c, 1)) : 0.0f;
    if (grip && !grip_was) {
        grip_draws = bow && hands_m < 0.25f;
        if (grip_draws)
            XrControls().Vibrate(1, 0.4f, 0.03f);  // nock
    }
    if (!grip || !bow)
        grip_draws = false;
    grip_was = grip;
    const bool was_drawing = g_bow_drawing;
    g_bow_drawing = grip_draws;
    if (g_bow_drawing) {
        // String tension on the drawing hand, growing with the draw length.
        float tension = std::fmin(1.0f, std::fmax(0.0f, (hands_m - 0.1f) / 0.5f));
        XrControls().Vibrate(1, 0.05f + 0.3f * tension, 0.02f);
    } else if (was_drawing) {
        XrControls().Vibrate(0, 0.8f, 0.05f);  // release
        XrControls().Vibrate(1, 0.6f, 0.04f);
    }

    bool weapon = Analog(rt, c.trigger[1]) || swing_hold > 0 || g_bow_drawing;
    if (weapon && !g_use_weapon.down)
        XrControls().Vibrate(1, 0.2f, 0.03f);
    bool was_down = g_use_weapon.down;
    g_use_weapon.Set(weapon);
    if (EngineLimbMode() == 1 && g_use_weapon.down)
        g_bow_draw_seconds = g_bow_draw_seconds < 0 ? 0.0 : g_bow_draw_seconds + dt;
    else
        g_bow_draw_seconds = -1;

    // --- Melee hit window: the weapon hits while your hand swings, not when
    //     the arm animation says. Opens as the attack is released (the engine
    //     has then registered it), stays open while the blade moves fast.
    EngineSetVrMelee(s.swing_to_attack);
    // Hit feedback on the weapon hand: a solid thump for a hit that did damage
    // (or a knockout), a jolt when a wall stops the swing, a sharper knock for a
    // blocked blade or an object.
    int hits = EngineTakeMeleeHits();
    if (hits & kMeleeHitDamage)
        XrControls().Vibrate(1, 1.0f, 0.12f);
    else if (hits & kMeleeHitWall)
        XrControls().Vibrate(1, 0.8f, 0.08f);
    else if (hits & kMeleeHitImpact)
        XrControls().Vibrate(1, 0.6f, 0.06f);
    if (g_strike_time >= 0) {
        g_strike_time += dt;
        bool slowed = g_strike_time > 0.25 && tip_speed < s.swing_speed * 0.35f;
        if (slowed || g_strike_time > 0.6 || !melee)
            CloseStrike();
    }
    if (s.swing_to_attack && melee && was_down && !g_use_weapon.down) {
        CloseStrike();
        if (EngineMeleeStrike(true))
            g_strike_time = 0;
    }
    // --- Right grip: frob / use item. With nothing highlighted, the use waits
    //     for the grip to be let go: hold, swing and let go to throw (the item
    //     leaves the hand along the swing, harder the faster it is; a gentle
    //     release drops it). Potions and the like are used on the release.
    //     Without "Throw by swinging" the use is at once, and a throw leaves
    //     the hand where it points.
    const bool grip_use = grip && !grip_draws;
    // What the squeeze does, decided as it starts:
    //   use:    "use item" held while squeezed (doors, levers; an inventory
    //           item selected, e.g. lockpicks on a lock);
    //   tapped: a world frob with no item selected - "use" tapped at once (the
    //           game frobs on the release), so a picked-up object is in the hand
    //           while the grip is still squeezed;
    //   throw:  nothing highlighted, or holding a picked-up object - let go to
    //           throw / drop it.
    enum GripMode { kGripNone, kGripUse, kGripTapped, kGripThrow };
    static GripMode mode = kGripNone;
    static bool grip_use_was;
    static int use_tap;  // a tap of "use item": 2 = press this frame, 1 = release
    if (grip_use && !grip_use_was) {
        bool highlighted = EngineFrobTarget() != 0, holding = EngineHeldJunk() != 0;
        if (s.swing_to_throw && (!highlighted || holding)) {
            mode = kGripThrow;
        } else if (s.swing_to_throw && EngineSelectedItem() == 0) {
            mode = kGripTapped;
            use_tap = 2;
        } else {
            mode = kGripUse;
            float offset[3], aim[3];
            if (!highlighted && StereoRightHand(offset, aim))
                EngineSetThrow(offset, aim, 1.0f);  // a throw on the press leaves the hand
        }
    }
    // The tap picked something up: it's held while squeezed, let go to throw / drop it.
    if (mode == kGripTapped && grip_use && use_tap == 0 && EngineHeldJunk() != 0)
        mode = kGripThrow;
    bool want_use = mode == kGripUse && grip_use;
    if (!grip_use) {
        if (mode == kGripThrow) {
            float offset[3], aim[3], v[3];
            if (StereoRightHand(offset, aim)) {
                Vec3 velocity{};
                if (c.velocity_valid[1] && StereoTrackedDirToWorld(c.linear_velocity[1], v))
                    velocity = {v[0], v[1], v[2]};
                float speed = Length(velocity);  // m/s
                Vec3 dir = speed > 1.0f ? velocity * (1.0f / speed) : Vec3{aim[0], aim[1], aim[2]};
                const float d[3] = {dir.x, dir.y, dir.z};
                EngineSetThrow(offset, d, std::fmin(1.6f, std::fmax(0.35f, speed / 3.5f)));
            }
            use_tap = 2;
        }
        mode = kGripNone;
    }
    if (use_tap > 0)
        want_use = use_tap-- == 2;
    grip_use_was = grip_use;
    g_use_item.Set(want_use);
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

bool ControlsLeaning()
{
    return g_lean_left.down || g_lean_right.down;
}

bool ControlsBowDrawing()
{
    return g_bow_drawing;
}

double ControlsBowDrawSeconds()
{
    return g_bow_draw_seconds;
}
