#include "stereo.h"

#include "../config/settings.h"
#include "../engine/engine.h"
#include "../input/vr_controls.h"
#include "../log.h"
#include "../vr.h"
#include "../vrmath.h"
#include "../ui/vr_menu.h"
#include "../xr/xr_input.h"
#include "d3d9_hooks.h"
#include "device_hooks.h"

#include <cstring>

#include <MinHook.h>
#include <d3d9.h>

namespace {

void* g_trampoline;  // MinHook's copy of the original scene render entry

// The first-person arm is drawn by a render hook at the end of the world render
// (FUN_004dad50 -> [0xbe7268] = FUN_00463c60), so once per eye pass. The arm is
// a creature placed (in the game update) relative to the game camera G. To show
// it in the hand H instead, its draw gets an extra world-space transform:
//   x' = p_H + D (x - p_G),   D = R_H R_G^T
// i.e. the arm moved rigidly from the game camera into the hand.
// Render context (measured with the F12 dump, see docs/re-map.md), reading the
// 9 floats row by row into M:
//   +0x78: camera C + t at +0x9c, applied as view = C^T x + t (t = -C^T eye).
//   +0x44: the current object's transform M + o at +0x68, same layout:
//          x_world = M^T x + o. Identity/0 for the arm (creature vertices are
//          already in world space).
// So for the arm's draw:  M' = M D^T,   o' = p_H + D (o - p_G).
// With the pinning correction C (see below) first: A = D Rc, b = p_H + D (tc - p_G),
//   M' = M A^T,   o' = A o + b.
// (The first version used M' = D, o' = p_G - D^T p_H, which is the INVERSE
// move: that's why roll and forward/back showed reversed.)
void* g_arm_trampoline;
bool g_arm_override;   // set for the eye passes of this frame
Mat3 g_arm_rg, g_arm_rh;
Vec3 g_arm_pg, g_arm_ph;

// Pinning: the attack animation moves the weapon away from the hand (wind-up
// over the shoulder, the swing arc). While an attack is under way, a correction
// C (x -> Rc x + tc) first moves the arm so the weapon is back where it was when
// idle, relative to the game camera; then the move into the hand applies. The
// weapon's frame comes from the arm's joints: origin = hand (3), x = towards the
// tip (4), in the plane of the wrist/forearm joint (2).
struct Frame {
    Mat3 r;
    Vec3 o;
};
Frame g_idle_rel;  // the idle weapon frame relative to the game camera
bool g_have_idle;
int g_idle_arm;

// A frame from three points: origin o, x towards a, in the plane of b.
bool FrameFrom(Vec3 o, Vec3 a, Vec3 b, Frame& f)
{
    Vec3 x = a - o, z = Cross(x, b - o);
    float lx = Length(x), lz = Length(z);
    if (lx < 1e-3f || lz < 1e-4f)
        return false;
    x = x * (1.0f / lx);
    z = z * (1.0f / lz);
    Vec3 y = Cross(z, x);
    for (int r = 0; r < 3; ++r) {
        f.r.m[r][0] = (&x.x)[r];
        f.r.m[r][1] = (&y.x)[r];
        f.r.m[r][2] = (&z.x)[r];
    }
    f.o = o;
    return true;
}

// The bow as a rigid body (the hand on its grip, joints 3, 4, 6): origin at the
// grip (between 4 and 6), x up the bow (6 -> 4), in the plane of the wrist (3).
bool BowFrame(Vec3 j3, Vec3 j4, Vec3 j6, Frame& f)
{
    return FrameFrom((j4 + j6) * 0.5f, j4, j3, f);
}

// The weapon's frame from the arm's joints. Sword / blackjack: origin = hand
// (3), x towards the tip (4), in the plane of the forearm joint (2).
bool WeaponFrame(int limb, Frame& f)
{
    float j[8][3];
    auto v = [&](int i) { return Vec3{j[i][0], j[i][1], j[i][2]}; };
    if (limb == 1)
        return EngineArmJoints(j, 8) && BowFrame(v(3), v(4), v(6), f);
    return EngineArmJoints(j, 5) && FrameFrom(v(3), v(4), v(2), f);
}

// The bow at full draw, relative to the game camera (feet; forward, left, up),
// measured with the F12 dump: upright ~2 ft ahead, its arrow along the camera's
// forward axis. The bow is always held in this pose, so the arrow leaves along
// the frame's camera-forward axis.
const Frame& BowAimPose()
{
    static Frame f;
    static bool made = BowFrame({1.638f, -0.452f, -0.629f}, {2.050f, -0.420f, -0.071f},
                                {1.954f, -0.329f, -1.309f}, f);
    (void)made;
    return f;
}

// The correction for the current arm pose, given the game camera (position,
// rotation). The reference is the weapon's rest pose relative to the camera:
// taken (when record is set) once the weapon has held still for a moment
// outside an attack, and dropped when the arm or limb mode changes. Every
// frame the weapon is moved back onto it, so no arm animation shows (swing,
// wind-up, recovery, idle sway).
Frame g_settle_rel;       // candidate rest pose
double g_settle_since = -1;
int g_ref_limb = -1;

void PinCorrection(Vec3 cam, const Mat3& rg, bool record, Mat3& rc, Vec3& tc)
{
    rc = Mat3{};
    tc = {};
    Frame cur;
    int arm = EngineArmObject();
    int limb = EngineLimbMode();
    if (!Config().weapon_in_hand || (limb != 1 && limb != 2) || !WeaponFrame(limb, cur)) {
        if (record)
            g_have_idle = false;
        return;
    }
    if (limb == 1) {
        // The bow: always pinned to its measured full-draw pose.
        const Frame& aim = BowAimPose();
        rc = (rg * aim.r) * Transpose(cur.r);
        tc = (cam + rg * aim.o) - rc * cur.o;
        return;
    }
    bool busy = EngineMeleeBusy();
    if (record) {
        if (arm != g_idle_arm || limb != g_ref_limb) {
            g_have_idle = false;
            g_settle_since = -1;
            g_idle_arm = arm;
            g_ref_limb = limb;
        }
        Frame rel{Transpose(rg) * cur.r, Transpose(rg) * (cur.o - cam)};
        double now = VrNowMs();
        bool still = g_settle_since >= 0 && Length(rel.o - g_settle_rel.o) < 0.02f &&
                     Dot({rel.r.m[0][0], rel.r.m[1][0], rel.r.m[2][0]},
                         {g_settle_rel.r.m[0][0], g_settle_rel.r.m[1][0], g_settle_rel.r.m[2][0]}) > 0.9995f;
        if (!still || busy) {
            g_settle_rel = rel;
            g_settle_since = busy ? -1 : now;
        } else if (!g_have_idle && now - g_settle_since > 300) {
            g_idle_rel = rel;
            g_have_idle = true;
            Log("Arm: rest pose recorded (arm %d)", arm);
        }
    }
    if (!g_have_idle)
        return;
    Mat3 idle_r = rg * g_idle_rel.r;
    Vec3 idle_o = cam + rg * g_idle_rel.o;
    rc = idle_r * Transpose(cur.r);
    tc = idle_o - rc * cur.o;
}

Mat3 g_pin_rc;  // this frame's correction for the render
Vec3 g_pin_tc;

// Melee hits: the weapon's hit spheres get the same move into the hand, kept
// relative to the game camera (the game simulates between our frames):
//   x' = c + h + D (x - c),  c = the game camera now, h = p_H - p_G.
Mat3 g_weapon_d;
Vec3 g_weapon_offset;
ULONGLONG g_weapon_valid_until;  // GetTickCount64 deadline (stale when VR stops rendering)

void WeaponPointToHand(float* p)
{
    int* cam = *Engine().current_camera;
    if (!cam || GetTickCount64() > g_weapon_valid_until)
        return;
    const float* c = reinterpret_cast<const float*>(cam + 2);
    const uint16_t* a = reinterpret_cast<const uint16_t*>(reinterpret_cast<unsigned char*>(cam) + 0x14);
    Vec3 cv{c[0], c[1], c[2]};
    Mat3 rg = RotZ(Angle16ToRad(a[2])) * RotY(Angle16ToRad(a[1])) * RotX(Angle16ToRad(a[0]));
    Mat3 rc;
    Vec3 tc;
    PinCorrection(cv, rg, false, rc, tc);
    Vec3 pinned = rc * Vec3{p[0], p[1], p[2]} + tc;
    Vec3 r = cv + g_weapon_offset + g_weapon_d * (pinned - cv);
    p[0] = r.x;
    p[1] = r.y;
    p[2] = r.z;
}

// F12 arm-transform dump: for one frame the arm is drawn unmodified, and at its
// first draw call in each eye the render context and related state are logged.
bool g_arm_dump_requested, g_arm_dump_active;
EnginePosition g_dump_eye;

void DumpArmContext()
{
    const EnginePosition& e = g_dump_eye;
    Log("ArmDump: eye (%.3f %.3f %.3f) bank/pitch/heading %u %u %u", e.x, e.y, e.z, e.bank, e.pitch, e.heading);
    if (int* cam = *Engine().current_camera) {
        const float* c = reinterpret_cast<const float*>(cam + 2);
        const uint16_t* a = reinterpret_cast<const uint16_t*>(reinterpret_cast<unsigned char*>(cam) + 0x14);
        Log("ArmDump: game camera (%.3f %.3f %.3f) bank/pitch/heading %u %u %u", c[0], c[1], c[2], a[0], a[1], a[2]);
    }
    int arm = EngineArmObject();
    if (unsigned char* ap = EngineObjectPosition(arm)) {
        const float* p = reinterpret_cast<const float*>(ap);
        const uint16_t* a = reinterpret_cast<const uint16_t*>(ap + 0x10);
        Log("ArmDump: arm object %d at (%.3f %.3f %.3f) bank/pitch/heading %u %u %u", arm, p[0], p[1], p[2], a[0], a[1],
            a[2]);
    }
    if (unsigned char* ctx = EngineRenderContext()) {
        for (int off = 0; off < 0x200; off += 32) {
            const float* f = reinterpret_cast<const float*>(ctx + off);
            Log("ArmDump ctx+%03x: %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f", off, f[0], f[1], f[2], f[3],
                f[4], f[5], f[6], f[7]);
        }
    }
}

void __cdecl ArmRenderDetour()
{
    if (g_arm_dump_active) {
        SetDrawProbe(&DumpArmContext);
        reinterpret_cast<void(__cdecl*)()>(g_arm_trampoline)();
        SetDrawProbe(nullptr);
        return;
    }
    unsigned char* ctx = g_arm_override ? EngineRenderContext() : nullptr;
    if (!ctx) {
        reinterpret_cast<void(__cdecl*)()>(g_arm_trampoline)();
        return;
    }
    float* block = reinterpret_cast<float*>(ctx + 0x44);
    float saved[12];
    memcpy(saved, block, sizeof(saved));

    Mat3 me;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            me.m[r][c] = block[r * 3 + c];
    Vec3 oe{block[9], block[10], block[11]};
    Mat3 d = g_arm_rh * Transpose(g_arm_rg);
    Mat3 a = d * g_pin_rc;
    Vec3 b = g_arm_ph + d * (g_pin_tc - g_arm_pg);
    Mat3 m = me * Transpose(a);
    Vec3 o = a * oe + b;

    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            block[r * 3 + c] = m.m[r][c];
    block[9] = o.x;
    block[10] = o.y;
    block[11] = o.z;
    reinterpret_cast<void(__cdecl*)()>(g_arm_trampoline)();
    memcpy(block, saved, sizeof(saved));
}

bool g_recenter = true;
Vec3 g_neutral_head;  // LOCAL-space metres
float g_neutral_yaw;  // engine-space heading of the head at recenter

// Eye render targets at headset resolution, independent of the game window.
// On a D3D9Ex device they're shareable textures (handed to D3D11 on the GPU).
EyeTargets g_eyes;
IDirect3DTexture9* g_eye_texture[2];
IDirect3DSurface9* g_eye_depth;
HeadState g_head;
Vec3 g_last_head;         // tracking-space head centre of the last frame
Vec3 g_last_head_offset;  // head offset used for the eyes (feet, tracking-forward frame)

float SignedAngle(uint16_t a)
{
    return (int16_t)a * (kPi / 32768.0f);
}

// Calls the original scene render: pos goes in EAX, focal on the stack.
__declspec(naked) void __cdecl CallOriginal(EnginePosition* /*pos*/, double /*focal*/)
{
    __asm {
        mov eax, [esp + 4]        // pos
        push dword ptr [esp + 12] // focal (high dword)
        push dword ptr [esp + 12] // focal (low dword)
        call dword ptr [g_trampoline]
        add esp, 8
        ret
    }
}

void __cdecl OnSceneRender(EnginePosition* pos, double focal);

// Replaces the engine's scene render entry point.
__declspec(naked) void Detour()
{
    __asm {
        push dword ptr [esp + 8]  // focal (high dword)
        push dword ptr [esp + 8]  // focal (low dword)
        push eax                  // pos
        call OnSceneRender
        add esp, 12
        ret
    }
}

bool EnsureEyeTargets(IDirect3DDevice9* dev, UINT w, UINT h)
{
    if (g_eyes.color[0] && w == g_eyes.width && h == g_eyes.height)
        return true;
    StereoOnDeviceReset();
    bool shareable = GameDeviceIsEx();
    for (int eye = 0; eye < 2; ++eye) {
        // A8R8G8B8 matches the headset swapchain's BGRA format family, so D3D11
        // can copy it directly.
        HANDLE* share = shareable ? &g_eyes.shared[eye] : nullptr;
        HRESULT hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                        &g_eye_texture[eye], share);
        if (SUCCEEDED(hr))
            hr = g_eye_texture[eye]->GetSurfaceLevel(0, &g_eyes.color[eye]);
        if (FAILED(hr)) {
            Log("Stereo: eye target %ux%u failed 0x%08x", w, h, hr);
            StereoOnDeviceReset();
            return false;
        }
    }
    HRESULT hr = dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &g_eye_depth,
                                                nullptr);
    if (FAILED(hr)) {
        Log("Stereo: eye depth %ux%u failed 0x%08x", w, h, hr);
        StereoOnDeviceReset();
        return false;
    }
    g_eyes.width = w;
    g_eyes.height = h;
    ++g_eyes.generation;
    Log("Stereo: eye targets %ux%u (%s)", w, h, shareable ? "shared with D3D11" : "CPU copy");
    return true;
}

Vec3 ToVec(const XrVector3f& v)
{
    return {v.x, v.y, v.z};
}

// Engine-space orientation of an OpenXR view.
Mat3 EngineOrientation(const XrQuaternionf& q)
{
    Mat3 c = XrToEngineBasis();
    return c * FromQuat(q.x, q.y, q.z, q.w) * Transpose(c);
}

void __cdecl OnSceneRender(EnginePosition* pos, double focal)
{
    static bool logged;
    if (!logged) {
        logged = true;
        int* cam = *Engine().current_camera;
        Log("Scene render: focal %.4f, camera mode %d zoom %.3f, pos (%.1f %.1f %.1f) hpb %u %u %u", focal,
            cam ? cam[0] : -1, cam ? *reinterpret_cast<float*>(&cam[1]) : 0.0f, pos->x, pos->y, pos->z,
            pos->heading, pos->pitch, pos->bank);
    }

    const XrFrame* frame = Config().stereo ? VrBeginFrameForRender() : nullptr;
    IDirect3DDevice9* dev = GameDevice();
    const XrViewConfigurationView& view_config = Xr().view_config(0);
    UINT eye_w = (UINT)(view_config.recommendedImageRectWidth * Config().render_scale) & ~1u;
    UINT eye_h = (UINT)(view_config.recommendedImageRectHeight * Config().render_scale) & ~1u;
    IDirect3DSurface9* canvas = nullptr;
    if (!frame || !dev || !EnsureEyeTargets(dev, eye_w, eye_h) || FAILED(dev->GetRenderTarget(0, &canvas))) {
        CallOriginal(pos, focal);
        return;
    }
    D3DSURFACE_DESC canvas_desc;
    canvas->GetDesc(&canvas_desc);
    canvas->Release();

    // Symmetric frustum that covers both eyes' (asymmetric) FOVs, widened to the
    // eye target's aspect ratio.
    float tan_h_need = 0, tan_v_need = 0;
    for (const XrView& v : frame->views) {
        tan_h_need = max(tan_h_need, max(std::tan(-v.fov.angleLeft), std::tan(v.fov.angleRight)));
        tan_v_need = max(tan_v_need, max(std::tan(v.fov.angleUp), std::tan(-v.fov.angleDown)));
    }
    float aspect = (float)eye_w / (float)eye_h;
    float tan_v = max(tan_v_need, tan_h_need / aspect) * Config().render_fov_scale;
    float tan_h = tan_v * aspect;
    XrFovf fov{-std::atan(tan_h), std::atan(tan_h), std::atan(tan_v), -std::atan(tan_v)};

    // The engine projects into the game's canvas (window) with
    // tan(v/2) = 0.75 / focal over the canvas height, keeping square pixels.
    // The redirect scales that image uniformly by s into the eye target, so the
    // canvas must span tan_v * s * canvas_h / eye_h vertically for the eye to
    // see exactly tan_v (the canvas's extra width is clipped off).
    float s = RedirectScale(canvas_desc.Width, canvas_desc.Height, eye_w, eye_h);
    float canvas_tan_v = tan_v * s * canvas_desc.Height / eye_h;
    double eye_focal = 0.75 / canvas_tan_v;

    // Head pose relative to the neutral (recentered) pose, in engine axes.
    Mat3 basis = XrToEngineBasis();
    Vec3 head = (ToVec(frame->views[0].pose.position) + ToVec(frame->views[1].pose.position)) * 0.5f;
    Mat3 head_rot = EngineOrientation(frame->views[0].pose.orientation);
    if (g_recenter) {
        g_recenter = false;
        g_neutral_head = head;
        g_neutral_yaw = HeadingOf(head_rot);
        Log("Stereo: recentered, yaw %.1f deg, FOV rendered %.1f x %.1f deg at %ux%u per eye",
            g_neutral_yaw * 180 / kPi, 2 * std::atan(tan_h) * 180 / kPi, 2 * std::atan(tan_v) * 180 / kPi, eye_w,
            eye_h);
    }
    Mat3 unyaw = RotZ(-g_neutral_yaw);
    Vec3 raw_offset = unyaw * (basis * (head - g_neutral_head)) * Config().world_scale;

    {
        float rel_heading, rel_pitch, rel_bank;
        ToHeadingPitchBank(unyaw * head_rot, rel_heading, rel_pitch, rel_bank);
        int* cam = *Engine().current_camera;
        g_head.valid = true;
        ++g_head.frame;
        g_head.track_heading = rel_heading;
        // The body is turned by the aim offset (hand / head aiming).
        float vs_body = rel_heading - EngineAimYaw();
        while (vs_body > kPi)
            vs_body -= 2 * kPi;
        while (vs_body < -kPi)
            vs_body += 2 * kPi;
        g_head.rel_heading = vs_body;
        g_head.pitch = rel_pitch;
        g_head.offset_forward_ft = raw_offset.x;
        g_head.offset_left_ft = raw_offset.y;
        g_head.offset_up_ft = raw_offset.z;
        g_head.body_heading = SignedAngle(pos->heading);
        g_head.body_pitch = SignedAngle(pos->pitch);
        g_head.camera_mode = cam ? cam[0] : 0;
    }

    Vec3 head_offset;  // feet, engine axes, relative to body facing
    if (Config().positional_tracking) {
        head_offset = raw_offset;
        float horiz = std::sqrt(head_offset.x * head_offset.x + head_offset.y * head_offset.y);
        float limit = Config().position_clamp_ft;
        if (horiz > limit && horiz > 0) {
            head_offset.x *= limit / horiz;
            head_offset.y *= limit / horiz;
        }
        head_offset.z = max(-limit, min(limit, head_offset.z));
    }
    g_last_head = head;
    g_last_head_offset = head_offset;

    double render_start = VrNowMs();
    const EnginePosition body = *pos;
    // The view comes from the world yaw (turns, mouse, scripts), not the body,
    // which points wherever the hand aims. Other cameras (scouting orb, security
    // cameras) use their own heading.
    float yaw = Angle16ToRad(body.heading);
    if (g_head.camera_mode != 0 || !EngineWorldYaw(yaw))
        yaw = Angle16ToRad(body.heading);
    Mat3 world_yaw = RotZ(yaw);

    // Sound: the ears go where the head is and face where it looks (the game
    // would use the body, which follows the aiming hand). Other cameras keep
    // the game's listener.
    if (g_head.camera_mode == 0) {
        Mat3 head_world = world_yaw * unyaw * head_rot;
        Vec3 ears = Vec3{body.x, body.y, body.z} + world_yaw * head_offset;
        const float pos3[3] = {ears.x, ears.y, ears.z};
        const float front[3] = {head_world.m[0][0], head_world.m[1][0], head_world.m[2][0]};
        const float top[3] = {head_world.m[0][2], head_world.m[1][2], head_world.m[2][2]};
        EngineSetListenerPose(pos3, front, top);
    } else {
        EngineClearListenerPose();
    }

    // Weapon in hand: the frame the arm should be drawn in (H) and the game
    // camera it's placed relative to (G). The arm render hook uses them.
    // The bow is held in the left hand when two-handed (the right one draws).
    XrPosef grip_pose, aim_pose;
    int* cam = *Engine().current_camera;
    const bool bow = EngineLimbMode() == 1;
    const bool bow_left = bow && Config().bow_two_handed;
    g_arm_override = false;
    if (g_arm_trampoline && Config().weapon_in_hand && g_head.camera_mode == 0 && cam &&
        XrControls().LocateHand(bow_left ? 0 : 1, frame->display_time, grip_pose, aim_pose)) {
        const float* cam_pos = reinterpret_cast<const float*>(cam + 2);
        const uint16_t* cam_ang = reinterpret_cast<const uint16_t*>(reinterpret_cast<unsigned char*>(cam) + 0x14);
        g_arm_rg = RotZ(Angle16ToRad(cam_ang[2])) * RotY(Angle16ToRad(cam_ang[1])) * RotX(Angle16ToRad(cam_ang[0]));
        g_arm_pg = {cam_pos[0], cam_pos[1], cam_pos[2]};
        // The hand's orientation, plus the user alignment (Hands tab) in the
        // hand's frame, about the grip.
        const Settings& cfg = Config();
        const float deg = kPi / 180.0f;
        Mat3 adjust = bow ? RotZ(cfg.bow_yaw_deg * deg) * RotY(cfg.bow_pitch_deg * deg) *
                                     RotX(cfg.bow_roll_deg * deg)
                               : RotZ(cfg.weapon_yaw_deg * deg) * RotY(cfg.weapon_pitch_deg * deg) *
                                     RotX(cfg.weapon_roll_deg * deg);
        g_arm_rh = world_yaw * unyaw * EngineOrientation(aim_pose.orientation) * adjust;
        // Drawing the two-handed bow: it pivots on the bow hand to lie along the
        // arrow line (string hand -> bow hand), the line the arrow flies along.
        XrPosef string_grip, string_aim;
        if (bow_left && ControlsBowDrawing() &&
            XrControls().LocateHand(1, frame->display_time, string_grip, string_aim)) {
            Mat3 to_world = world_yaw * unyaw * XrToEngineBasis();
            Vec3 forward = g_arm_rh * Vec3{1, 0, 0};
            Vec3 line = BowArrowLine(forward, to_world * ToVec(grip_pose.position),
                                     to_world * ToVec(string_grip.position));
            g_arm_rh = RotationBetween(forward, line) * g_arm_rh;
        }
        // Place H so the arm's usual grip point (in view: forward/right/down)
        // lands on the controller.
        float hand[3];
        TrackedPointToCameraOffset(grip_pose.position, yaw, hand);
        // The bow: its grip goes to the controller (adjusted by the Bow position
        // sliders); its arrow then points along the controller. Swords: the
        // Weapon grip sliders.
        Vec3 grip_in_view{cfg.grip_forward_ft, -cfg.grip_right_ft, -cfg.grip_down_ft};
        if (bow)
            grip_in_view = BowAimPose().o + Vec3{cfg.bow_forward_ft, -cfg.bow_right_ft, -cfg.bow_down_ft};
        g_arm_ph = g_arm_pg + Vec3{hand[0], hand[1], hand[2]} - g_arm_rh * grip_in_view;
        g_arm_override = true;
        PinCorrection(g_arm_pg, g_arm_rg, true, g_pin_rc, g_pin_tc);
        g_weapon_d = g_arm_rh * Transpose(g_arm_rg);
        g_weapon_offset = g_arm_ph - g_arm_pg;
        g_weapon_valid_until = GetTickCount64() + 250;
    } else {
        g_weapon_valid_until = 0;
    }

    g_arm_dump_active = g_arm_dump_requested;
    g_arm_dump_requested = false;
    if (g_arm_dump_active) {
        Log("ArmDump: ---- frame dump (arm drawn unmodified) ----");
        EngineLogArmWeapon();
    }

    XrPosef poses[2];
    for (int eye = 0; eye < 2; ++eye) {
        const XrView& view = frame->views[eye];
        poses[eye] = view.pose;

        Mat3 eye_rot = world_yaw * unyaw * EngineOrientation(view.pose.orientation);
        float heading, pitch, bank;
        ToHeadingPitchBank(eye_rot, heading, pitch, bank);
        if (Config().invert_roll)
            bank = -bank;

        Vec3 eye_from_head = unyaw * (basis * (ToVec(view.pose.position) - head)) * Config().world_scale;
        Vec3 offset = world_yaw * (head_offset + eye_from_head);

        EnginePosition p = body;
        p.x += offset.x;
        p.y += offset.y;
        p.z += offset.z;
        p.cell = -1;  // the eye may be in a different cell than the body
        p.hint = -1;
        p.heading = RadToAngle16(heading);
        p.pitch = RadToAngle16(pitch);
        p.bank = RadToAngle16(bank);
        *pos = p;
        g_dump_eye = p;

        BeginSceneRedirect(dev, g_eyes.color[eye], g_eye_depth);
        CallOriginal(pos, eye_focal);
        EndSceneRedirect(dev);
    }
    *pos = body;
    g_arm_dump_active = false;
    g_arm_override = false;

    // The overlays the frame render draws next (light gem, inventory, text) then
    // land on black in the back buffer, and are captured at Present as the HUD.
    if (Config().hud_enabled)
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

    VrOnEyesRendered(dev, g_eyes, poses, fov, VrNowMs() - render_start);
}

} // namespace

bool InstallStereoHook()
{
    void* target = Engine().scene_render;
    if (!target)
        return false;
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&Detour), &g_trampoline);
    if (st == MH_OK)
        st = MH_EnableHook(target);
    Log("Stereo: scene render hook %s", MH_StatusToString(st));
    if (void* arm = Engine().arm_render) {
        MH_STATUS as = MH_CreateHook(arm, reinterpret_cast<void*>(&ArmRenderDetour), &g_arm_trampoline);
        if (as == MH_OK)
            as = MH_EnableHook(arm);
        if (as != MH_OK)
            g_arm_trampoline = nullptr;
        Log("Stereo: arm render hook %s", MH_StatusToString(as));
        if (g_arm_trampoline)
            EngineSetWeaponPointTransform(&WeaponPointToHand);
    }
    return st == MH_OK;
}

void StereoOnDeviceReset()
{
    for (int eye = 0; eye < 2; ++eye) {
        if (g_eyes.color[eye])
            g_eyes.color[eye]->Release();
        if (g_eye_texture[eye])
            g_eye_texture[eye]->Release();
        g_eyes.color[eye] = nullptr;
        g_eye_texture[eye] = nullptr;
        g_eyes.shared[eye] = nullptr;
    }
    if (g_eye_depth)
        g_eye_depth->Release();
    g_eye_depth = nullptr;
    g_eyes.width = g_eyes.height = 0;
}

void StereoRequestRecenter()
{
    g_recenter = true;
}

const HeadState& CurrentHeadState()
{
    return g_head;
}

void StereoRequestArmDump()
{
    g_arm_dump_requested = true;
}

bool TrackedPointToCameraOffset(const XrVector3f& point, float world_yaw, float out[3])
{
    if (!g_head.valid)
        return false;
    Vec3 rel = RotZ(-g_neutral_yaw) * (XrToEngineBasis() * (ToVec(point) - g_last_head)) * Config().world_scale;
    Vec3 world = RotZ(world_yaw) * (g_last_head_offset + rel);
    out[0] = world.x;
    out[1] = world.y;
    out[2] = world.z;
    return true;
}

bool TrackedDirectionAngles(const XrVector3f& direction, float& heading, float& pitch)
{
    if (!g_head.valid)
        return false;
    Vec3 v = RotZ(-g_neutral_yaw) * (XrToEngineBasis() * ToVec(direction));
    float len = Length(v);
    if (len < 1e-4f)
        return false;
    heading = std::atan2(v.y, v.x);
    pitch = std::asin(-v.z / len);  // positive = down
    return true;
}

bool TrackedAngles(const XrQuaternionf& orientation, float& heading, float& pitch)
{
    if (!g_head.valid)
        return false;
    float bank;
    ToHeadingPitchBank(RotZ(-g_neutral_yaw) * EngineOrientation(orientation), heading, pitch, bank);
    return true;
}
