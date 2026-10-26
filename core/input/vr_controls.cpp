#include "vr_controls.h"

#include "../config/settings.h"
#include "../engine/engine.h"
#include "bindings.h"
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
Edge g_snap;
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

// Whether a world point is at a door's handle: near the door, towards the
// upright edge away from its hinge (either edge if that isn't known, e.g. a
// sliding door), at hand height. Its face direction (world, the door's thin
// axis) goes in normal.
bool AtDoorHandle(int door, const float p[3], Vec3& normal)
{
    float pos[3], rot[9], bmin[3], bmax[3];
    if (!EngineObjectBox(door, pos, rot, bmin, bmax))
        return false;
    float d[3] = {p[0] - pos[0], p[1] - pos[1], p[2] - pos[2]};
    float local[3];
    for (int j = 0; j < 3; ++j)
        local[j] = d[0] * rot[0 * 3 + j] + d[1] * rot[1 * 3 + j] + d[2] * rot[2 * 3 + j];
    const int wide = bmax[0] - bmin[0] >= bmax[1] - bmin[1] ? 0 : 1, thin = 1 - wide;
    normal = {rot[0 * 3 + thin], rot[1 * 3 + thin], rot[2 * 3 + thin]};
    float half = (bmax[wide] - bmin[wide]) * 0.5f, mid = (bmax[wide] + bmin[wide]) * 0.5f;
    float edge = half > 0 ? std::fabs(local[wide] - mid) / half : 0;  // 0 middle, 1 edge
    float hinge[3];
    if (EngineDoorHinge(door, hinge)) {
        float h = (hinge[0] - pos[0]) * rot[0 * 3 + wide] + (hinge[1] - pos[1]) * rot[1 * 3 + wide] +
                  (hinge[2] - pos[2]) * rot[2 * 3 + wide];
        if ((h - mid) * (local[wide] - mid) > 0)
            return false;  // the hinge side
    }
    float height = bmax[2] > bmin[2] ? (local[2] - bmin[2]) / (bmax[2] - bmin[2]) : 0;
    float off_face = std::fabs(local[thin] - (bmax[thin] + bmin[thin]) * 0.5f) - (bmax[thin] - bmin[thin]) * 0.5f;
    return edge > 0.7f && edge < 1.1f && height > 0.3f && height < 0.6f && off_face < 0.35f;
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
    // --- Climbing with the hands. A grip on a ladder or rope grabs it: off it,
    //     the player moves onto the one near the hand (the engine climbs on
    //     touching it); on it, pulling the hand down climbs up and pushing it up
    //     climbs down, hand over hand. Not with the bow out (the grips draw it;
    //     the stick still climbs), and the right grip only with nothing to frob.
    //     A grip taken here isn't a frob / crouch until it's let go.
    const bool climbing = EnginePlayerMode() == kPlayerModeClimb;
    const bool bow_out = EngineLimbMode() == 1;
    static bool climb_grip[2], climb_grip_was[2], grip_taken[2];
    static int climb_hand = -1;       // the hand pulling, -1 none
    static float climb_hand_y;        // its tracked height last frame (m)
    static float climb_target_z;      // where the pulls put the player (world ft)
    static double attach_time = -1;   // >= 0 while moving onto a grabbed ladder
    static float attach_point[3];     // where to stand to get on (world)
    static float attach_face;         // the heading that faces it
    float player[3];
    const bool have_player = EnginePlayerPosition(player);
    for (int side = 0; side < 2; ++side) {
        Analog(climb_grip[side], c.grip[side]);
        bool pressed = climb_grip[side] && !climb_grip_was[side];
        climb_grip_was[side] = climb_grip[side];
        if (!climb_grip[side]) {
            grip_taken[side] = false;
            if (climb_hand == side)
                climb_hand = grip_taken[1 - side] ? 1 - side : -1;
            continue;
        }
        bool free_hand = side == 0 || (EngineFrobTarget() == 0 && EngineHeldJunk() == 0);
        if (!pressed || !s.hand_climb || bow_out || !free_hand || !c.pose_valid[side] || !have_player)
            continue;
        float hand[3], point[3];
        int found = 0;
        if (!climbing && StereoHandWorld(side, hand)) {
            found = EngineFindClimbable(hand, 1.5f, point);
        }
        if (climbing || found) {
            grip_taken[side] = true;
            if (climb_hand < 0)
                climb_target_z = player[2];
            climb_hand = side;
            climb_hand_y = c.grip_pose[side].position.y;
            XrControls().Vibrate(side, 0.4f, 0.04f);
            if (!climbing) {
                // A ladder: just off its face on the player's side (also from
                // above, stepping off a ledge onto it), facing it. A rope: at it.
                attach_time = 0;
                float pos[3], rot[9], bmin[3], bmax[3];
                float nx = point[0] - player[0], ny = point[1] - player[1];
                attach_point[0] = point[0], attach_point[1] = point[1], attach_point[2] = point[2];
                if (EngineObjectBox(found, pos, rot, bmin, bmax)) {
                    int thin = bmax[0] - bmin[0] <= bmax[1] - bmin[1] ? 0 : 1;
                    float ax = rot[0 * 3 + thin], ay = rot[1 * 3 + thin];
                    float alen = std::sqrt(ax * ax + ay * ay);
                    if (alen > 1e-3f) {
                        ax /= alen, ay /= alen;
                        float from = (player[0] - pos[0]) * ax + (player[1] - pos[1]) * ay >= 0 ? 1.0f : -1.0f;
                        nx = -ax * from, ny = -ay * from;  // towards the ladder
                        attach_point[0] = point[0], attach_point[1] = point[1];
                    }
                }
                attach_face = std::atan2(ny, nx);
            }
        }
    }
    if (climb_hand >= 0 && (bow_out || !c.pose_valid[climb_hand]))
        climb_hand = -1;
    // Off the ladder: move to just in front of it, facing it, and let the engine
    // grab it (as on touching it); for up to 2 s.
    float attach_forward = 0, attach_right = 0;
    if (attach_time >= 0) {
        attach_time += dt;
        if (!climbing)
            EngineTryClimb();
        if (EnginePlayerMode() == kPlayerModeClimb || attach_time > 2.0 || !have_player) {
            attach_time = -1;
        } else {
            float dx = attach_point[0] - player[0], dy = attach_point[1] - player[1];
            float len = std::sqrt(dx * dx + dy * dy);
            const unsigned char* pos = EngineObjectPosition(EnginePlayerObject());
            if (pos && len > 0.05f) {
                float h = *reinterpret_cast<const int16_t*>(pos + 0x14) * (kPi / 32768.0f);  // body heading
                dx /= len, dy /= len;
                attach_forward = dx * std::cos(h) + dy * std::sin(h);
                attach_right = dx * std::sin(h) - dy * std::cos(h);
            }
        }
    }
    // On it: the pulling hand's height change moves the player the other way
    // (the engine's climb speed, steered to where the pulls put the player).
    float hand_climb = 0;  // -1..1, + up
    if (climbing && climb_hand >= 0 && have_player) {
        float y = c.grip_pose[climb_hand].position.y;
        climb_target_z -= (y - climb_hand_y) * s.world_scale;
        climb_hand_y = y;
        // Blocked (the top, the floor): don't build up a pull it can't follow.
        climb_target_z = std::fmax(player[2] - 1.0f, std::fmin(player[2] + 1.0f, climb_target_z));
        float error = climb_target_z - player[2];
        if (std::fabs(error) > 0.05f)
            hand_climb = std::fmax(-1.0f, std::fmin(1.0f, error / 0.4f));
    }

    // --- Movement (left stick), relative to where you look ---
    XrVector2f stick = Deadzone(c.move, s.stick_deadzone);
    if (BindFired(kActRunToggle)) {
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
    // pitch is set to match below, only while climbing, so the bow still aims
    // when holding still), wherever you look or point. Or the hands' pulls.
    const float kClimbPitch = 60.0f * kPi / 180.0f;
    bool climb_move = false;
    float climb_pitch = 0;
    if (climbing) {
        float up = stick.y != 0 ? stick.y * speed : hand_climb;
        climb_move = up != 0 || stick.x != 0;
        climb_pitch = up >= 0 ? -kClimbPitch : kClimbPitch;  // positive = down
        forward = std::fabs(up);
        right = stick.x * speed;
    } else if (attach_time >= 0 && stick.x == 0 && stick.y == 0) {
        climb_move = true;  // onto the grabbed ladder, facing it (looking level)
        climb_pitch = 0;
        forward = attach_forward * 0.8f;
        right = attach_right * 0.8f;
    }
    // While climbing or getting on, the body faces the ladder / rope: the engine
    // climbs along the body's facing, which otherwise follows the hand (and
    // would drift sideways when the hand points off to the side).
    bool face_climb = false;
    float face_heading = 0;
    if (climb_move && have_player) {
        float point[3];
        if (!climbing) {
            face_climb = attach_time >= 0;
            face_heading = attach_face;
        } else if (EngineClimbTarget(player, point)) {
            float dx = point[0] - player[0], dy = point[1] - player[1];
            if (dx * dx + dy * dy > 0.2f * 0.2f) {
                face_climb = true;
                face_heading = std::atan2(dy, dx);
            }
        }
    }
    if (s.invert_strafe)
        right = -right;
    if (stick.x != 0 || stick.y != 0 || climb_move) {
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
        if (climb_move)
            EngineSetLookPitch(climb_pitch);
        float world_yaw;
        if (face_climb && EngineWorldYaw(world_yaw))
            EngineSetAimYaw(std::atan2(std::sin(face_heading - world_yaw), std::cos(face_heading - world_yaw)));
    }

    // --- Jump, toggle crouch (by default the right stick up / down) ---
    g_jump.Set(BindHeld(kActJump));
    // (Crouch toggles on each call, like its key binding, which has no release.)
    if (BindFired(kActCrouchToggle))
        EngineInput("crouch", true);

    // --- Triggers and grips ---
    static bool rt, rg;

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
    const bool grip_use = grip && !grip_draws && !grip_taken[1];
    // What the squeeze does, decided as it starts:
    //   use:    "use item" held while squeezed (doors, levers; an inventory
    //           item selected, e.g. lockpicks on a lock);
    //   tapped: a world frob with no item selected - "use" tapped at once (the
    //           game frobs on the release), so a picked-up object is in the hand
    //           while the grip is still squeezed;
    //   throw:  nothing highlighted, or holding a picked-up object - let go to
    //           throw / drop it.
    //   gesture: a door, lever or button - it's frobbed when the hand moves
    //           with it (pull or push); held still with an item selected (a
    //           key, lockpicks), "use item" is held as above.
    enum GripMode { kGripNone, kGripUse, kGripTapped, kGripThrow, kGripGesture, kGripDone };
    static GripMode mode = kGripNone;
    static bool grip_use_was;
    static int use_tap;  // a tap of "use item": 2 = press this frame, 1 = release
    static XrVector3f gesture_start;  // the hand where the grip closed (tracking space)
    static Vec3 gesture_normal;       // a door's face direction (world); zero for levers / buttons
    static double gesture_time;
    float hand_now[3];
    const bool hand_known = StereoHandWorld(1, hand_now);
    if (grip_use && !grip_use_was) {
        int target = EngineFrobTarget();
        bool highlighted = target != 0, holding = EngineHeldJunk() != 0;
        int kind = s.hand_gestures && highlighted && !holding && hand_known ? EngineGestureKind(target) : kGestureNone;
        Vec3 normal{};
        if (kind == kGestureDoor && !AtDoorHandle(target, hand_now, normal)) {
            // Away from the handle: only a key / lockpicks (held on it), else nothing.
            mode = EngineSelectedItem() != 0 ? kGripUse : kGripDone;
        } else if (kind != kGestureNone) {
            mode = kGripGesture;
            gesture_time = 0;
            gesture_start = c.grip_pose[1].position;
            gesture_normal = kind == kGestureDoor ? normal : Vec3{};
        } else if (s.swing_to_throw && (!highlighted || holding)) {
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
    if (mode == kGripGesture && grip_use) {
        gesture_time += dt;
        // The hand's own movement (metres, not the player's): for a door, across it.
        const XrVector3f& now = c.grip_pose[1].position;
        XrVector3f delta{now.x - gesture_start.x, now.y - gesture_start.y, now.z - gesture_start.z};
        float moved = Length(Vec3{delta.x, delta.y, delta.z}), across = moved;
        float w[3];
        if (Length(gesture_normal) > 0 && StereoTrackedDirToWorld(delta, w))
            across = std::fabs(w[0] * gesture_normal.x + w[1] * gesture_normal.y + w[2] * gesture_normal.z);
        if (across > 0.08f) {  // ~8 cm: pulled or pushed
            use_tap = 2;
            mode = kGripDone;
            XrControls().Vibrate(1, 0.6f, 0.05f);
        } else if (gesture_time > 0.4 && moved < 0.04f && EngineSelectedItem() != 0) {
            mode = kGripUse;  // a key or lockpicks on it
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
    // A push with the open hand: it moves into a button quickly.
    static float push_was = -1;  // the hand's distance from the target's box last frame (ft)
    static int push_target;
    static double push_cooldown;
    push_cooldown -= dt;
    {
        int target = EngineFrobTarget();
        float d = -1;
        if (s.hand_gestures && !grip && hand_known && target && EngineGestureKind(target) == kGestureButton)
            d = EngineObjectBoxDistance(target, hand_now);
        float speed = c.velocity_valid[1] ? Length(Vec3{c.linear_velocity[1].x, c.linear_velocity[1].y,
                                                        c.linear_velocity[1].z})
                                          : 0.0f;
        if (d >= 0 && d < 0.15f && push_was >= 0.15f && target == push_target && speed > 0.6f &&
            push_cooldown <= 0 && use_tap == 0) {
            use_tap = 2;
            push_cooldown = 0.8;
            XrControls().Vibrate(1, 0.6f, 0.05f);
        }
        push_was = d, push_target = target;
    }
    if (use_tap > 0)
        want_use = use_tap-- == 2;
    grip_use_was = grip_use;
    g_use_item.Set(want_use);
    g_block.Set(BindHeld(kActBlock));

    // --- Crouch: left grip, or physically crouching ---
    static bool body_crouched;
    if (s.physical_crouch)
        body_crouched = body_crouched ? head.offset_up_ft < -s.physical_crouch_ft * 0.7f
                                      : head.offset_up_ft < -s.physical_crouch_ft;
    else
        body_crouched = false;
    // (Not while the left grip holds a ladder, if that's the crouch button.)
    const bool crouch_grip_climbing = BindButtonOf(kActCrouchHold) == kButtonLeftGrip && grip_taken[0];
    g_crouch_hold.Set((BindHeld(kActCrouchHold) && !crouch_grip_climbing) || body_crouched);

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

    // --- Buttons (bindings.h; by default A / B / X / Y, Y held, right stick click) ---
    if (BindFired(kActNextWeapon))
        EngineCommand("next_weapon");
    if (BindFired(kActPutAway))
        EngineCommand("clear_weapon");
    if (BindFired(kActNextItem))
        EngineCommand("next_item");
    if (BindFired(kActPrevItem))
        EngineCommand("prev_item");
    if (BindFired(kActObjectives)) {
        EngineCommand("objectives");
        XrControls().Vibrate(0, 0.3f, 0.05f);
    }
    if (BindFired(kActMap))
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
