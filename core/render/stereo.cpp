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
#include "hands.h"

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
hands::Hand g_hand[2];  // the visible hands this frame (left, right)
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

// Weapon smoothing: a One Euro filter per hand (adaptive low-pass). Slow
// movement is smoothed strongly (no jitter), fast movement hardly at all (swings
// stay responsive): the cutoff frequency rises with the speed. Filters the grip
// position and the aim orientation, the two parts of the pose the weapon uses.
struct HandFilter {
    bool valid = false;
    XrTime last = 0;
    Vec3 pos, speed;     // filtered position (m) and velocity (m/s)
    Quat rot;            // filtered orientation
    float turn = 0;      // filtered angular speed (rad/s)
};
HandFilter g_hand_filter[2];

float SmoothingAlpha(float cutoff_hz, float dt)
{
    float tau = 1.0f / (2.0f * kPi * cutoff_hz);
    return 1.0f / (1.0f + tau / dt);
}

void SmoothHand(int hand, XrTime time, XrPosef& grip, XrPosef& aim)
{
    const float strength = Config().weapon_smoothing;
    HandFilter& f = g_hand_filter[hand];
    Vec3 p{grip.position.x, grip.position.y, grip.position.z};
    Quat q{aim.orientation.x, aim.orientation.y, aim.orientation.z, aim.orientation.w};
    float dt = f.valid ? (float)((time - f.last) * 1e-9) : 0.0f;
    if (strength <= 0 || !f.valid || dt <= 0 || dt > 0.2f) {
        f.valid = true;
        f.last = time;
        f.pos = p;
        f.speed = {};
        f.rot = q;
        f.turn = 0;
        return;
    }
    f.last = time;
    const float min_cutoff = 1.0f + (1.0f - strength) * 9.0f;  // Hz while still: 1 (strong) .. 10 (light)
    const float speed_cutoff = 1.0f, beta_move = 10.0f, beta_turn = 2.0f;

    f.speed = f.speed + ((p - f.pos) * (1.0f / dt) - f.speed) * SmoothingAlpha(speed_cutoff, dt);
    f.pos = f.pos + (p - f.pos) * SmoothingAlpha(min_cutoff + beta_move * Length(f.speed), dt);

    float d = q.x * f.rot.x + q.y * f.rot.y + q.z * f.rot.z + q.w * f.rot.w;
    if (d < 0) {
        q = {-q.x, -q.y, -q.z, -q.w};
        d = -d;
    }
    float angle = 2.0f * std::acos(d > 1.0f ? 1.0f : d);
    f.turn = f.turn + (angle / dt - f.turn) * SmoothingAlpha(speed_cutoff, dt);
    float a = SmoothingAlpha(min_cutoff + beta_turn * f.turn, dt);
    Quat r{f.rot.x + (q.x - f.rot.x) * a, f.rot.y + (q.y - f.rot.y) * a, f.rot.z + (q.z - f.rot.z) * a,
           f.rot.w + (q.w - f.rot.w) * a};
    float len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    f.rot = {r.x / len, r.y / len, r.z / len, r.w / len};

    grip.position = {f.pos.x, f.pos.y, f.pos.z};
    aim.orientation = {f.rot.x, f.rot.y, f.rot.z, f.rot.w};
}

Mat3 g_pin_rc;  // this frame's correction for the render
Vec3 g_pin_tc;

// Melee hits: the weapon's hit spheres get the same move into the hand, kept
// relative to the game camera (the game simulates between our frames):
//   x' = c + h + D (x - c),  c = the game camera now, h = p_H - p_G.
Mat3 g_weapon_d;
Vec3 g_weapon_offset;
ULONGLONG g_weapon_valid_until;  // GetTickCount64 deadline (stale when VR stops rendering)

// With the visible hands, the weapon drawn in the hand (its frame: origin at
// the grip, x to the tip), relative to the game camera.
Mat3 g_drawn_weapon_r;
Vec3 g_drawn_weapon_offset;
float g_drawn_weapon_length = 1;  // the model's grip-to-tip length / the arm's (both arms' joints are ~3 ft)
ULONGLONG g_drawn_weapon_until;

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
    float j[5][3];
    if (GetTickCount64() <= g_drawn_weapon_until && EngineArmJoints(j, 5)) {
        // Onto the weapon drawn in the hand. The engine puts each hit sphere on
        // the line from the arm's hand joint (3) to its weapon tip (4): the same
        // fraction of the way along the drawn weapon, from its grip (sword 2.99
        // ft, like the arm; the blackjack shorter).
        Vec3 hand{j[3][0], j[3][1], j[3][2]}, tip{j[4][0], j[4][1], j[4][2]};
        Vec3 along = tip - hand;
        float len2 = Dot(along, along);
        float t = len2 > 1e-6f ? Dot(Vec3{p[0], p[1], p[2]} - hand, along) / len2 : 0.0f;
        const float kArmHandToTip = 2.99f;
        r = cv + g_drawn_weapon_offset + g_drawn_weapon_r * Vec3{t * kArmHandToTip * g_drawn_weapon_length, 0, 0};
    }
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

// Visible hands (render/hands.h): set per frame by OnSceneRender.
bool g_hands_replace_arm;  // the hands and the game's weapon model are drawn instead of the game's arm

void __cdecl ArmRenderDetour()
{
    if (g_arm_dump_active) {
        SetDrawProbe(&DumpArmContext);
        reinterpret_cast<void(__cdecl*)()>(g_arm_trampoline)();
        SetDrawProbe(nullptr);
        return;
    }
    if (g_hands_replace_arm) {
        // Not drawn (the hands show the weapon); only its lighting is read.
        DrawCapture previous = SetDrawCapture(hands::ArmLightCapture());
        reinterpret_cast<void(__cdecl*)()>(g_arm_trampoline)();
        SetDrawCapture(previous);
        return;
    }
    // The arm has its own depth range: keep it out of any depth sampling.
    DrawCapture outer = SetDrawCapture(DrawCapture{});
    unsigned char* ctx = g_arm_override ? EngineRenderContext() : nullptr;
    if (!ctx) {
        reinterpret_cast<void(__cdecl*)()>(g_arm_trampoline)();
        SetDrawCapture(outer);
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
    SetDrawCapture(outer);
}

bool g_recenter = true;
Vec3 g_neutral_head;  // LOCAL-space metres
float g_neutral_yaw;  // engine-space heading of the head at recenter

// Eye render targets at headset resolution, independent of the game window.
// On a D3D9Ex device they're shareable textures (handed to D3D11 on the GPU).
EyeTargets g_eyes;
IDirect3DTexture9* g_eye_texture[2];
IDirect3DSurface9* g_eye_depth;           // multisampled when MSAA is on
IDirect3DSurface9* g_eye_msaa_color[2];   // MSAA: rendered into, then resolved into g_eyes.color
int g_eye_msaa;                           // samples in use (0 = off)

// GPU time of the eye passes: timestamp queries in a small ring, read back a
// few frames later without flushing (so the pipeline never waits on them).
constexpr int kGpuQuerySets = 4;
struct GpuQuerySet {
    IDirect3DQuery9* disjoint = nullptr;
    IDirect3DQuery9* freq = nullptr;
    IDirect3DQuery9* begin = nullptr;
    IDirect3DQuery9* end = nullptr;
    bool pending = false;
};
GpuQuerySet g_gpu_queries[kGpuQuerySets];
int g_gpu_query_next;
double g_gpu_ms_sum;
int g_gpu_ms_count;

void ReleaseQuery(IDirect3DQuery9*& q)
{
    if (q)
        q->Release();
    q = nullptr;
}

void ReleaseGpuQueries()
{
    for (GpuQuerySet& q : g_gpu_queries) {
        ReleaseQuery(q.disjoint);
        ReleaseQuery(q.freq);
        ReleaseQuery(q.begin);
        ReleaseQuery(q.end);
        q.pending = false;
    }
}

// Before the eye passes: collects the oldest set's result, then starts timing.
GpuQuerySet* BeginGpuTiming(IDirect3DDevice9* dev)
{
    GpuQuerySet& q = g_gpu_queries[g_gpu_query_next];
    g_gpu_query_next = (g_gpu_query_next + 1) % kGpuQuerySets;
    if (!q.disjoint) {
        if (FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &q.disjoint)) ||
            FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &q.freq)) ||
            FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q.begin)) ||
            FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q.end))) {
            ReleaseGpuQueries();
            return nullptr;
        }
    }
    if (q.pending) {
        BOOL disjoint = TRUE;
        UINT64 freq = 0, t0 = 0, t1 = 0;
        if (q.disjoint->GetData(&disjoint, sizeof(disjoint), 0) == S_OK &&
            q.freq->GetData(&freq, sizeof(freq), 0) == S_OK && q.begin->GetData(&t0, sizeof(t0), 0) == S_OK &&
            q.end->GetData(&t1, sizeof(t1), 0) == S_OK && !disjoint && freq && t1 > t0) {
            g_gpu_ms_sum += (double)(t1 - t0) * 1000.0 / (double)freq;
            ++g_gpu_ms_count;
        }
    }
    q.disjoint->Issue(D3DISSUE_BEGIN);
    q.begin->Issue(D3DISSUE_END);
    q.pending = true;
    return &q;
}

void EndGpuTiming(GpuQuerySet* q)
{
    if (!q)
        return;
    q->end->Issue(D3DISSUE_END);
    q->freq->Issue(D3DISSUE_END);
    q->disjoint->Issue(D3DISSUE_END);
}
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

// The MSAA sample count to use (0, 2 or 4), if the device supports it.
int SupportedMsaa(IDirect3DDevice9* dev, int want)
{
    int samples = want >= 4 ? 4 : want >= 2 ? 2 : 0;
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (!samples || FAILED(dev->GetDirect3D(&d3d)))
        return 0;
    if (FAILED(dev->GetCreationParameters(&cp))) {
        d3d->Release();
        return 0;
    }
    while (samples >= 2 &&
           (FAILED(d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_A8R8G8B8, TRUE,
                                                   (D3DMULTISAMPLE_TYPE)samples, nullptr)) ||
            FAILED(d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_D24S8, TRUE,
                                                   (D3DMULTISAMPLE_TYPE)samples, nullptr))))
        samples /= 2;
    d3d->Release();
    return samples >= 2 ? samples : 0;
}

bool EnsureEyeTargets(IDirect3DDevice9* dev, UINT w, UINT h)
{
    static int checked_want = -1, supported = 0;
    if (Config().msaa != checked_want) {
        checked_want = Config().msaa;
        supported = SupportedMsaa(dev, checked_want);
    }
    int msaa = supported;
    if (g_eyes.color[0] && w == g_eyes.width && h == g_eyes.height && msaa == g_eye_msaa)
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
    auto ms_type = (D3DMULTISAMPLE_TYPE)msaa;  // 0 = D3DMULTISAMPLE_NONE
    for (int eye = 0; msaa && eye < 2; ++eye) {
        HRESULT hr = dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, ms_type, 0, FALSE, &g_eye_msaa_color[eye], nullptr);
        if (FAILED(hr)) {
            Log("Stereo: %dx MSAA eye target failed 0x%08x; anti-aliasing off", msaa, hr);
            for (IDirect3DSurface9*& ms : g_eye_msaa_color) {
                if (ms)
                    ms->Release();
                ms = nullptr;
            }
            msaa = 0;
            ms_type = D3DMULTISAMPLE_NONE;
        }
    }
    HRESULT hr = dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, ms_type, 0, TRUE, &g_eye_depth, nullptr);
    if (FAILED(hr)) {
        Log("Stereo: eye depth %ux%u failed 0x%08x", w, h, hr);
        StereoOnDeviceReset();
        return false;
    }
    g_eye_msaa = msaa;
    g_eyes.width = w;
    g_eyes.height = h;
    ++g_eyes.generation;
    Log("Stereo: eye targets %ux%u (%s), MSAA %s", w, h, shareable ? "shared with D3D11" : "CPU copy",
        msaa ? (msaa == 4 ? "4x" : "2x") : "off");
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

// Tracking space (LOCAL, metres) -> engine world, the same mapping the eyes and
// controllers use (TrackedPointToCameraOffset): world = b + a * p.
void TrackedToWorld(float world_yaw, Vec3 camera, Mat3& a, Vec3& b)
{
    a = RotZ(world_yaw) * RotZ(-g_neutral_yaw) * XrToEngineBasis();
    float scale = Config().world_scale;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            a.m[r][c] *= scale;
    b = camera + RotZ(world_yaw) * g_last_head_offset - a * g_last_head;
}

Mat3 QuatMatrix(const XrQuaternionf& q)
{
    return FromQuat(q.x, q.y, q.z, q.w);
}

// A visible hand from its controller: the grip pose with the same smoothing as
// the weapon (the aim orientation's smoothing carried over to the grip), and
// finger curls from the grip / trigger / buttons.
void FillHand(int side, const XrPosef& grip_raw, const XrPosef& aim_raw, const XrPosef& grip_smooth,
              const XrPosef& aim_smooth, const Mat3& a, Vec3 b, bool holding_weapon)
{
    hands::Hand& h = g_hand[side];
    h.visible = true;
    h.grip_rot = QuatMatrix(aim_smooth.orientation) * Transpose(QuatMatrix(aim_raw.orientation)) *
                 QuatMatrix(grip_raw.orientation);
    h.grip_pos = {grip_smooth.position.x, grip_smooth.position.y, grip_smooth.position.z};
    h.a = a;
    h.b = b;
    // Grip curls the hand into a fist; the trigger points it (index straight,
    // the other fingers and the thumb curled); a face button tucks the thumb.
    const XrControllerState& c = ControlsLastState();
    float grip = c.grip[side], point = c.trigger[side];
    bool thumb = side == 1 ? (c.a || c.b || c.stick_click[1]) : (c.x || c.y || c.stick_click[0] || c.menu);
    const float relaxed = 0.25f;
    float fist = std::fmax(relaxed, grip);
    h.index = std::fmax(relaxed, grip) * (1.0f - point);
    h.middle = h.ring = h.pinky = std::fmax(fist, point);
    h.thumb = std::fmax(thumb ? 1.0f : 0.3f, point * 0.8f);
    if (holding_weapon)
        h.index = h.middle = h.ring = h.pinky = h.thumb = 1.0f;
}

// The right hand (world) for physical frob and throwing, from the last frame.
bool g_right_hand_valid;
Vec3 g_right_hand_offset, g_right_hand_aim;  // grip point relative to the game camera; pointing
Mat3 g_tracked_to_world;                     // tracking -> world rotation

// --- The bow ---
// Where the bow is (in the bow hand, or the game's bow pinned there), how far
// it's drawn, the nocked arrow, and where that arrow would leave from; handed
// to the engine for the shot (EngineSetBowShot). At a release the drawn shot is
// kept a moment, so the game's fire (on the release) uses it.
Vec3 g_bow_aim_rel;          // the arrow's direction relative to the view's world yaw
ULONGLONG g_bow_aim_until;   // StereoBowAim is valid until then
ULONGLONG g_bow_drawn_until; // the last drawn shot is kept until then
Vec3 g_bow_shot_offset, g_bow_shot_dir;

bool UpdateBow(int bow_hand, bool two_handed, const Mat3& tw_a, Vec3 tw_b, const XrPosef sm_grip[2],
               const bool located[2], const XrPosef& bow_grip, hands::BowPose& pose)
{
    const Settings& cfg = Config();
    const bool drawn_hands = cfg.show_hands && two_handed;
    hands::BowInput in;
    if (drawn_hands) {
        // In the fist, adjusted by the Bow angle / position sliders.
        const float deg = kPi / 180.0f;
        Mat3 adjust = RotZ(cfg.bow_yaw_deg * deg) * RotY(cfg.bow_pitch_deg * deg) * RotX(cfg.bow_roll_deg * deg);
        in.r = hands::BowFrameInHand(g_hand[bow_hand], cfg.world_scale) * adjust;
        in.grip = tw_b + tw_a * g_hand[bow_hand].grip_pos + in.r * Vec3{cfg.bow_forward_ft, -cfg.bow_right_ft, -cfg.bow_down_ft};
    } else {
        // Pointing along the hand (one-handed), or the game's pinned bow (hands off).
        in.r = g_arm_rh;
        in.grip = tw_b + tw_a * ToVec(bow_grip.position);
    }
    in.left_hand = bow_hand == 0;
    in.pulled = two_handed && ControlsBowDrawing() && located[1];
    in.string_hand = tw_b + tw_a * ToVec(sm_grip[1].position);
    double held = ControlsBowDrawSeconds();
    if (!two_handed && held >= 0)
        in.auto_draw = (float)min(1.0, held / 0.8);  // drawn in 0.8 s

    // The arrow's model, looked up once per arrow: the nocked arrow (the game
    // makes it as the draw starts), or before that the selected arrows (the
    // current weapon with the bow out).
    static int named = 0;
    static char file[48];
    int nocked = EngineNockedArrow();
    int arrow = nocked ? nocked : EngineCurrentWeapon();
    if (arrow != named) {
        named = arrow;
        file[0] = 0;
        char name[32];
        auto is_arrow = [](const char* n) {  // arrow model names all contain "arr"
            for (; n[0] && n[1] && n[2]; ++n)
                if (_strnicmp(n, "arr", 3) == 0)
                    return true;
            return false;
        };
        if (arrow && EngineObjectModelName(arrow, name, sizeof(name)) && is_arrow(name)) {
            snprintf(file, sizeof(file), "%s.bin", name);
        } else if (nocked) {
            snprintf(file, sizeof(file), "arrow.bin");  // a nocked arrow without a model name: a broadhead
        }
    }
    in.arrow = file[0] ? file : nullptr;

    Vec3 centre, dir;
    if (!hands::MakeBow(in, pose, centre, dir))
        return false;
    pose.visible = cfg.show_hands;
    if (cfg.show_hands && in.pulled) {
        // The drawing hand hooks the string.
        hands::Hand& s = g_hand[1];
        s.thumb = 0.6f;
        s.index = s.middle = s.ring = 0.55f;
        s.pinky = 0.8f;
    } else if (cfg.show_hands && two_handed && in.arrow && g_hand[1].visible) {
        // Not on the string yet: the arrow is in the drawing hand, as if just
        // taken from the quiver - through the fist, out of the thumb end, the
        // nock just behind the hand.
        hands::Hand& s = g_hand[1];
        Vec3 fist;
        hands::WeaponFrame(s, cfg.world_scale, pose.hand_r, fist);
        pose.hand_nock = fist - pose.hand_r * Vec3{0.3f, 0, 0};
        pose.arrow_in_hand = true;
        s.index = s.middle = 0.8f;
        s.ring = s.pinky = 0.9f;
        s.thumb = 0.8f;
    }

    ULONGLONG now = GetTickCount64();
    const bool drawing = in.pulled || in.auto_draw >= 0;
    if (drawing || now > g_bow_drawn_until) {
        g_bow_shot_offset = centre - g_arm_pg;
        g_bow_shot_dir = dir;
        if (drawing)
            g_bow_drawn_until = now + 300;
    }
    const float offset[3] = {g_bow_shot_offset.x, g_bow_shot_offset.y, g_bow_shot_offset.z};
    const float d[3] = {g_bow_shot_dir.x, g_bow_shot_dir.y, g_bow_shot_dir.z};
    EngineSetBowShot(true, offset, d);
    // It aims the body while drawing (and a moment after), or held one-handed.
    if (drawing || now <= g_bow_drawn_until || !two_handed) {
        float world_yaw = 0;
        if (EngineWorldYaw(world_yaw)) {
            g_bow_aim_rel = RotZ(-world_yaw) * g_bow_shot_dir;
            g_bow_aim_until = now + 100;
        }
    }
    return true;
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
    // Standing height: the neutral height follows the highest the head has been,
    // so the view never rises above the game's eye height, whatever the height
    // at recenter or the headset's floor setup. Small drift (within 15 cm, e.g.
    // a recenter on tiptoe or a tracking shift) eases back down at 2 cm/s; a real
    // crouch is lower than that and isn't affected.
    {
        static double last_ms = VrNowMs();
        double now_ms = VrNowMs();
        float dt = (float)min(0.1, (now_ms - last_ms) / 1000.0);
        last_ms = now_ms;
        float below = g_neutral_head.y - head.y;
        if (below < 0)
            g_neutral_head.y = head.y;
        else if (below < 0.15f)
            g_neutral_head.y -= min(below, 0.02f * dt);
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
        head_offset.z = Config().vertical_head_tracking ? max(-limit, min(limit, head_offset.z)) : 0.0f;
    }
    g_last_head = head;
    g_last_head_offset = head_offset;

    double render_start = VrNowMs();
    const EnginePosition body = *pos;

    // The eyes start from the game camera. When "Lean adds the game's camera
    // shift" is off, the game's lean slide is taken out: the camera's usual spot
    // relative to the player (tracked while not leaning, in the player's facing
    // frame) is used instead, so the view follows only the real head.
    EnginePosition eye_base = body;
    {
        static Vec3 rest_offset;  // camera - player object, player facing frame (x, y only)
        static bool have_rest;
        static double lean_released_ms = -1e9;
        int player = EnginePlayerObject();
        const unsigned char* ppos = g_head.camera_mode == 0 && player ? EngineObjectPosition(player) : nullptr;
        if (ppos) {
            const float* pp = reinterpret_cast<const float*>(ppos);
            Mat3 facing = RotZ(Angle16ToRad(body.heading));
            Vec3 offset = Transpose(facing) * Vec3{body.x - pp[0], body.y - pp[1], 0.0f};
            double now_ms = VrNowMs();
            if (ControlsLeaning())
                lean_released_ms = now_ms;
            else if (now_ms - lean_released_ms > 700) {  // the lean-out animation is over
                rest_offset = offset;
                have_rest = true;
            }
            if (!Config().lean_camera_shift && have_rest) {
                Vec3 base = facing * rest_offset;
                eye_base.x = pp[0] + base.x;
                eye_base.y = pp[1] + base.y;
            }
        }
    }
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
    const int main_hand = bow_left ? 0 : 1;  // the weapon / bow hand
    g_arm_override = false;
    g_hand[0].visible = g_hand[1].visible = false;
    // Both controllers, located and smoothed once this frame.
    XrPosef raw_grip[2], raw_aim[2], sm_grip[2], sm_aim[2];
    bool located[2];
    for (int h = 0; h < 2; ++h) {
        located[h] = XrControls().LocateHand(h, frame->display_time, raw_grip[h], raw_aim[h]);
        sm_grip[h] = raw_grip[h];
        sm_aim[h] = raw_aim[h];
        if (located[h])
            SmoothHand(h, frame->display_time, sm_grip[h], sm_aim[h]);
    }
    hands::BowPose bow_pose;
    hands::HeldPose held_pose;
    bool bow_shot = false;
    if (g_arm_trampoline && Config().weapon_in_hand && g_head.camera_mode == 0 && cam && located[main_hand]) {
        grip_pose = sm_grip[main_hand];
        aim_pose = sm_aim[main_hand];
        const XrPosef grip_raw = raw_grip[main_hand], aim_raw = raw_aim[main_hand];
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
        if (bow_left && ControlsBowDrawing() && located[1]) {
            Mat3 to_world = world_yaw * unyaw * XrToEngineBasis();
            Vec3 forward = g_arm_rh * Vec3{1, 0, 0};
            Vec3 line = BowArrowLine(forward, to_world * ToVec(grip_pose.position),
                                     to_world * ToVec(sm_grip[1].position));
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
        // Relative to where the eyes start: the game camera, less the game's
        // lean slide when "Lean adds the game's camera shift" is off (eye_base),
        // so the hands and weapons stay with the view.
        const Vec3 view_base = g_arm_pg + Vec3{eye_base.x - body.x, eye_base.y - body.y, eye_base.z - body.z};
        g_arm_ph = view_base + Vec3{hand[0], hand[1], hand[2]} - g_arm_rh * grip_in_view;
        g_arm_override = true;
        PinCorrection(g_arm_pg, g_arm_rg, true, g_pin_rc, g_pin_tc);

        // The visible hands: the weapon / bow hand, and the other one.
        Mat3 tw_a;
        Vec3 tw_b;
        TrackedToWorld(yaw, view_base, tw_a, tw_b);
        if (Config().show_hands) {
            bool holding = (EngineLimbMode() == 2 && hands::WeaponModelsLoaded()) || bow;
            FillHand(main_hand, grip_raw, aim_raw, grip_pose, aim_pose, tw_a, tw_b, holding);
            int other = 1 - main_hand;
            if (located[other])
                FillHand(other, raw_grip[other], raw_aim[other], sm_grip[other], sm_aim[other], tw_a, tw_b, false);
        }
        if (bow)
            bow_shot = UpdateBow(main_hand, bow_left, tw_a, tw_b, sm_grip, located, grip_pose, bow_pose);
        // A picked-up object (junk) shows in the right hand, not in the inventory display.
        static int named_junk;
        static char junk_file[48];
        int junk = Config().show_hands && g_hand[1].visible ? EngineHeldJunk() : 0;
        if (junk != named_junk) {
            named_junk = junk;
            junk_file[0] = 0;
            char name[32];
            if (junk && EngineObjectModelName(junk, name, sizeof(name)))
                snprintf(junk_file, sizeof(junk_file), "%s.bin", name);
        }
        if (junk && junk_file[0]) {
            held_pose.visible = true;
            held_pose.model = junk_file;
            hands::WeaponFrame(g_hand[1], Config().world_scale, held_pose.r, held_pose.p);
            hands::Hand& h = g_hand[1];
            h.index = h.middle = h.ring = h.pinky = std::fmax(h.middle, 0.7f);
            h.thumb = std::fmax(h.thumb, 0.6f);
        }
        EngineHideItemSlot(held_pose.visible);
        // Physical frob and throwing: the right hand's grip point and pointing (world).
        g_tracked_to_world = world_yaw * unyaw * XrToEngineBasis();
        g_right_hand_valid = located[1];
        if (located[1]) {
            Vec3 grip_w = tw_b + tw_a * ToVec(sm_grip[1].position);
            g_right_hand_offset = grip_w - g_arm_pg;
            g_right_hand_aim = world_yaw * unyaw * EngineOrientation(sm_aim[1].orientation) * Vec3{1, 0, 0};
            const float o[3] = {grip_w.x, grip_w.y, grip_w.z};
            const float d[3] = {g_right_hand_aim.x, g_right_hand_aim.y, g_right_hand_aim.z};
            EngineSetHandRay(Config().hand_frob, o, d);
        } else {
            EngineSetHandRay(false, nullptr, nullptr);
        }
        g_weapon_d = g_arm_rh * Transpose(g_arm_rg);
        g_weapon_offset = g_arm_ph - g_arm_pg;
        g_weapon_valid_until = GetTickCount64() + 250;
    } else {
        g_weapon_valid_until = 0;
        EngineHideItemSlot(false);
    }

    g_arm_dump_active = g_arm_dump_requested;
    g_arm_dump_requested = false;
    if (g_arm_dump_active) {
        Log("ArmDump: ---- frame dump (arm drawn unmodified) ----");
        EngineLogPlayerPhysics();
        EngineLogArmWeapon();
    }

    // Visible hands this frame: for empty hands and the sword / blackjack (the
    // bow and a carried body keep the game's arm). Until the hands are ready
    // (a fist captured, the engine's depth mapping known) the game's arm shows.
    const int limb = EngineLimbMode();
    const bool hands_on = Config().show_hands && g_arm_override && g_head.camera_mode == 0 &&
                          (limb == 0xff || limb == 0 || limb == 1 || limb == 2);
    if (!bow_shot)
        EngineSetBowShot(false, nullptr, nullptr);
    {
        // A different arm / weapon: find out again which weapon it holds.
        static int last_arm = -1, last_limb = -1;
        int arm = EngineArmObject();
        hands::SetWeaponObject(limb == 2 ? EngineCurrentWeapon() : 0);
        if (arm != last_arm || limb != last_limb)
            hands::ResetArmWeapon();
        last_arm = arm;
        last_limb = limb;
    }
    g_hands_replace_arm = hands_on && hands::Ready() &&
                          ((limb == 2 && hands::WeaponModelsLoaded()) || (limb == 1 && bow_pose.visible));
    // The weapon model in the right hand (at the controller's grip); the melee
    // hit spheres are moved onto it (WeaponPointToHand).
    hands::WeaponPose weapon_pose;
    g_drawn_weapon_until = 0;
    if (g_hands_replace_arm && limb == 2 && g_hand[1].visible) {
        weapon_pose.kind = hands::ArmWeapon();
        hands::WeaponFrame(g_hand[1], Config().world_scale, weapon_pose.r, weapon_pose.p);
        if (weapon_pose.kind != hands::Weapon::None) {
            g_drawn_weapon_r = weapon_pose.r;
            g_drawn_weapon_offset = weapon_pose.p - g_arm_pg;
            // Sword: grip to tip 2.99 ft, like the arm; blackjack: 1.58 ft.
            g_drawn_weapon_length = weapon_pose.kind == hands::Weapon::Blackjack ? 1.58f / 2.98f : 1.0f;
            g_drawn_weapon_until = GetTickCount64() + 250;
        }
    }

    XrPosef poses[2];
    GpuQuerySet* gpu_timing = BeginGpuTiming(dev);
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

        EnginePosition p = eye_base;
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

        Mat3 engine_eye_rot = RotZ(Angle16ToRad(p.heading)) * RotY(Angle16ToRad(p.pitch)) *
                              RotX(Angle16ToRad(p.bank));
        const bool depth_sample = eye == 0 && hands_on && hands::NeedDepthSample();
        DrawCapture before_depth;
        if (depth_sample)
            before_depth = SetDrawCapture(hands::DepthSampleCapture());

        IDirect3DSurface9* target = g_eye_msaa ? g_eye_msaa_color[eye] : g_eyes.color[eye];
        BeginSceneRedirect(dev, target, g_eye_depth);
        CallOriginal(pos, eye_focal);
        EndSceneRedirect(dev);
        if (depth_sample) {
            SetDrawCapture(before_depth);
            hands::EndDepthSample();
        }
        if (hands_on) {
            hands::Eye he{engine_eye_rot, {p.x, p.y, p.z}, (eye_h * 0.5f) / tan_v, eye_w * 0.5f, eye_h * 0.5f};
            hands::Draw(dev, target, g_eye_depth, he, g_hand[0], g_hand[1], weapon_pose, bow_pose, held_pose,
                        Config().hand_brightness);
        }
        if (g_eye_msaa)
            dev->StretchRect(target, nullptr, g_eyes.color[eye], nullptr, D3DTEXF_NONE);  // resolve
    }
    EndGpuTiming(gpu_timing);
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
    for (IDirect3DSurface9*& ms : g_eye_msaa_color) {
        if (ms)
            ms->Release();
        ms = nullptr;
    }
    g_eye_msaa = 0;
    ReleaseGpuQueries();
    hands::OnDeviceReset();
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

bool StereoRightHand(float offset[3], float aim[3])
{
    if (!g_right_hand_valid)
        return false;
    offset[0] = g_right_hand_offset.x, offset[1] = g_right_hand_offset.y, offset[2] = g_right_hand_offset.z;
    aim[0] = g_right_hand_aim.x, aim[1] = g_right_hand_aim.y, aim[2] = g_right_hand_aim.z;
    return true;
}

bool StereoTrackedDirToWorld(const XrVector3f& v, float out[3])
{
    if (!g_right_hand_valid)
        return false;
    Vec3 w = g_tracked_to_world * ToVec(v);
    out[0] = w.x, out[1] = w.y, out[2] = w.z;
    return true;
}

bool StereoBowAim(float& heading, float& pitch)
{
    if (GetTickCount64() > g_bow_aim_until || Length(g_bow_aim_rel) < 1e-4f)
        return false;
    Vec3 v = Normalize(g_bow_aim_rel);
    heading = std::atan2(v.y, v.x);
    pitch = std::asin(-v.z);  // positive = down
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

double StereoTakeGpuMs()
{
    double ms = g_gpu_ms_count ? g_gpu_ms_sum / g_gpu_ms_count : -1.0;
    g_gpu_ms_sum = 0;
    g_gpu_ms_count = 0;
    return ms;
}
