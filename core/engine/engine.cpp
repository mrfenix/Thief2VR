#include "engine.h"

#include "../log.h"
#include "../render/device_hooks.h"
#include "../vrmath.h"
#include "build_id.h"

#include <cmath>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <MinHook.h>
#include <dsound.h>
#include <intrin.h>
#include <cstring>

static void* g_camera_update_trampoline;
static void __cdecl CameraUpdateDetour();
static void* g_objpos_update_trampoline;
static void ObjPosUpdateDetour();
static void InstallWeaponHooks();
static void InstallListenerHooks();

namespace {

EngineAddresses g_engine;

// Per-build RVAs (Ghidra address - 0x400000). See docs/re-map.md.
struct BuildTable {
    const char* sha256;
    const char* name;
    uintptr_t scene_render;
    uintptr_t current_camera;
    uintptr_t command_execute;   // FUN_0041aab0: int(char* cmd /*EAX*/), 0 = ok
    uintptr_t set_forward;       // FUN_0054de40: void(float), -2..2
    uintptr_t set_sidestep;      // FUN_0054de90: void(float), -2..2
    uintptr_t apply_control;     // FUN_0054ec10: void __stdcall(int ctrl)  (RET 4)
    uintptr_t player_control;    // DAT_00aa1444: int* ctrl; ctrl[9] == 7 means no control
    uintptr_t input_locked;      // DAT_00a9f864: nonzero while player input is locked
    uintptr_t player_object;     // DAT_00aa1418: player object id
    uintptr_t get_phys_model;    // FUN_00507d70: void* __stdcall(int obj)  (RET 4)
    uintptr_t camera_update;     // FUN_00546ca0: void __cdecl(), applies look angles each frame
    uintptr_t object_position;   // FUN_0054a330: Position*(int obj /*EAX*/); facing heading at +0x14
};

const BuildTable kBuilds[] = {
    {"d26342c34624a08a0e5fc8d8c9daf14c676642c328f41c01f548170774aafe8f", "NewDark 1.29 (T2Fix 2026-09-16)",
     0x1d0ee0, 0x6a141c, 0x1aab0, 0x14de40, 0x14de90, 0x14ec10, 0x6a1444, 0x69f864, 0x6a1418, 0x107d70,
     0x146ca0, 0x14a330},
};

constexpr int kPositionHeading = 0x14;  // Position = vec(12) + cell/hint(4) + angles tx,ty,tz

void* g_object_position;

__declspec(naked) unsigned char* __cdecl CallObjectPosition(int /*obj*/)
{
    __asm {
        mov eax, [esp + 4]
        call dword ptr [g_object_position]
        ret
    }
}

// The player's look angles live in its physics model: model+0xf0 -> struct
// with 16-bit bank/pitch/heading at +0x10/+0x12/+0x14. Each frame the camera
// update (FUN_00546ca0) adds the heading value to the player's facing and
// zeroes it (that's where mouse turning lands); pitch persists and is what aim
// and frob use.
constexpr int kModelRotation = 0xf0;
constexpr int kRotPitch = 0x12;
constexpr int kRotHeading = 0x14;

// Input-binding variables (140-byte table entries: char name[] at +0, handler
// at +0x64 = int __cdecl handler(char* name, char* value)). RVAs of the entries.
struct InputVar {
    const char* name;
    uintptr_t entry;
};
const InputVar kInputVars[] = {
    {"jump", 0x49fa04},       {"crouch", 0x49fa90},    {"crouchhold", 0x49fb1c}, {"leanleft", 0x49f860},
    {"leanright", 0x49f8ec},  {"runon", 0x49fd4c},     {"use_weapon", 0x4933dc}, {"use_item", 0x493468},
    {"block", 0x493350},      {"lookcenter", 0x49f6bc},
};
constexpr int kInputVarCount = sizeof(kInputVars) / sizeof(kInputVars[0]);
typedef int(__cdecl* InputHandler)(char* name, char* value);
InputHandler g_input_handler[kInputVarCount];

const BuildTable* g_build;
unsigned char* g_base;
bool g_input_ok;

void* g_command_execute;
void(__cdecl* g_set_forward)(float);
void(__cdecl* g_set_sidestep)(float);
void(__stdcall* g_apply_control)(int);
int* g_player_control;
int* g_input_locked;
int* g_player_object;
unsigned char*(__stdcall* g_get_phys_model)(int);

__declspec(naked) int __cdecl CallCommandExecute(char* /*cmd*/)
{
    __asm {
        push ebx
        push esi
        push edi
        mov eax, [esp + 16]
        call dword ptr [g_command_execute]
        pop edi
        pop esi
        pop ebx
        ret
    }
}

// Byte pattern with wildcards ("??").
bool Matches(const unsigned char* at, const char* pattern)
{
    for (const char* p = pattern; *p;) {
        while (*p == ' ')
            ++p;
        if (!*p)
            break;
        if (p[0] == '?') {
            p += 2;
            ++at;
            continue;
        }
        unsigned v = strtoul(p, nullptr, 16);
        if (*at++ != (unsigned char)v)
            return false;
        p += 2;
    }
    return true;
}

} // namespace

const EngineAddresses& Engine()
{
    return g_engine;
}

bool ResolveEngine()
{
    std::string sha = ExeSha256();
    for (const auto& b : kBuilds)
        if (sha == b.sha256)
            g_build = &b;
    if (!g_build) {
        Log("Engine: unsupported exe build %s. Stereo/tracking disabled.", sha.c_str());
        return false;
    }

    g_base = reinterpret_cast<unsigned char*>(GetModuleHandleA(nullptr));
    Log("Engine: %s, module base %p", g_build->name, g_base);

    unsigned char* scene = g_base + g_build->scene_render;
    if (!Matches(scene, "83 ec 28 dd 05 ?? ?? ?? ?? 53 56 dd 54 24 24 dd 05 ?? ?? ?? ?? 8b f0 8b 06")) {
        Log("Engine: scene render signature mismatch at %p. Stereo disabled.", scene);
        g_build = nullptr;
        return false;
    }
    g_engine.scene_render = scene;
    g_engine.current_camera = reinterpret_cast<int**>(g_base + g_build->current_camera);
    Log("Engine: scene render %p, camera ptr %p", g_engine.scene_render, g_engine.current_camera);

    unsigned char* arm_render = g_base + 0x63c60;  // FUN_00463c60
    if (Matches(arm_render, "55 8b ec 83 e4 c0 83 ec 38 56 57 8b 3d ?? ?? ?? ?? 85 ff 75 37"))
        g_engine.arm_render = arm_render;
    else
        Log("Engine: arm render signature mismatch; the weapon is drawn flat");

    // Player input. Everything is verified before use; on a mismatch the VR
    // controls are simply disabled.
    unsigned char* cmd = g_base + g_build->command_execute;
    unsigned char* fwd = g_base + g_build->set_forward;
    unsigned char* side = g_base + g_build->set_sidestep;
    unsigned char* apply = g_base + g_build->apply_control;
    const char* speed_sig = "d9 05 ?? ?? ?? ?? d9 44 24 04 d8 d1 df e0 f6 c4 41 75 0b a1 ?? ?? ?? ?? dd d8 d9 58";
    unsigned char* phys = g_base + g_build->get_phys_model;
    g_input_ok = Matches(cmd, "55 8b ec 83 e4 f8 83 ec 0c 53 56 57 8b f8") && Matches(fwd, speed_sig) &&
                 fwd[28] == 0x14 && Matches(side, speed_sig) && side[28] == 0x18 &&
                 Matches(apply, "a1 ?? ?? ?? ?? 83 ec 14 53 8b 5c 24 1c 56 57 50") &&
                 Matches(phys, "83 ec 14 53 8b 5c 24 1c 56 57 53 8d 74 24 14 bf ?? ?? ?? ?? e8");
    for (int i = 0; g_input_ok && i < kInputVarCount; ++i) {
        const char* entry = reinterpret_cast<const char*>(g_base + kInputVars[i].entry);
        uintptr_t handler = *reinterpret_cast<const uintptr_t*>(entry + 0x64);
        uintptr_t text_begin = reinterpret_cast<uintptr_t>(g_base) + 0x1000;
        if (strcmp(entry, kInputVars[i].name) != 0 || handler < text_begin ||
            handler > reinterpret_cast<uintptr_t>(g_base) + 0x400000) {
            Log("Engine: input variable '%s' not found", kInputVars[i].name);
            g_input_ok = false;
        }
        g_input_handler[i] = reinterpret_cast<InputHandler>(handler);
    }
    if (g_input_ok) {
        g_command_execute = cmd;
        g_set_forward = reinterpret_cast<void(__cdecl*)(float)>(fwd);
        g_set_sidestep = reinterpret_cast<void(__cdecl*)(float)>(side);
        g_apply_control = reinterpret_cast<void(__stdcall*)(int)>(apply);
        g_player_control = reinterpret_cast<int*>(g_base + g_build->player_control);
        g_input_locked = reinterpret_cast<int*>(g_base + g_build->input_locked);
        g_player_object = reinterpret_cast<int*>(g_base + g_build->player_object);
        g_get_phys_model = reinterpret_cast<unsigned char*(__stdcall*)(int)>(phys);
        Log("Engine: player input resolved");

        unsigned char* cam_update = g_base + g_build->camera_update;
        unsigned char* obj_pos = g_base + g_build->object_position;
        if (Matches(cam_update, "a1 ?? ?? ?? ?? 83 ec 14 53 55 8b 2d ?? ?? ?? ?? 56 57 85 ed 0f 84") &&
            Matches(obj_pos, "51 85 c0 7d 24 8b 0d ?? ?? ?? ?? 8b 11 56 8d 74 24 04 56 50 8b 42 50")) {
            g_object_position = obj_pos;
            MH_STATUS st = MH_CreateHook(cam_update, reinterpret_cast<void*>(&CameraUpdateDetour),
                                         &g_camera_update_trampoline);
            if (st == MH_OK)
                st = MH_EnableHook(cam_update);
            if (st != MH_OK)
                g_camera_update_trampoline = nullptr;
            Log("Engine: camera update hook %s (turning / aim pitch)", MH_StatusToString(st));
        } else {
            Log("Engine: camera update signature mismatch; stick turning and aim pitch disabled");
        }

        unsigned char* objpos_update = g_base + 0x14a3c0;  // FUN_0054a3c0
        if (Matches(objpos_update, "a1 ?? ?? ?? ?? 8b 08 8b 51 18 53 8b 5c 24 08 57 50") &&
            MH_CreateHook(objpos_update, reinterpret_cast<void*>(&ObjPosUpdateDetour),
                          &g_objpos_update_trampoline) == MH_OK) {
            MH_EnableHook(objpos_update);
        }
        InstallWeaponHooks();
    } else {
        Log("Engine: player input signatures mismatch, VR controls disabled");
    }
    InstallListenerHooks();
    return true;
}

int EnginePlayerObject()
{
    return g_player_object ? *g_player_object : 0;
}

int EngineArmObject()
{
    // DAT_00aa1410: player arm state; the arm object id is at +4.
    int* arm = g_base ? *reinterpret_cast<int**>(g_base + 0x6a1410) : nullptr;
    return arm ? arm[1] : 0;
}

int EngineLimbMode()
{
    if (!g_base)
        return 0xff;
    int mode = *reinterpret_cast<int*>(g_base + 0x4717f8);
    static int last = -1;
    if (mode != last) {
        Log("Limb mode -> %d", mode);
        last = mode;
    }
    return mode;
}

unsigned char* EngineRenderContext()
{
    return g_base ? *reinterpret_cast<unsigned char**>(g_base + 0x6ac0c8) : nullptr;
}

unsigned char* EngineObjectPosition(int obj)
{
    return obj && g_object_position ? CallObjectPosition(obj) : nullptr;
}

bool EngineInputAllowed()
{
    if (!g_input_ok || *g_input_locked)
        return false;
    int ctrl = *g_player_control;
    return ctrl && reinterpret_cast<int*>(ctrl)[9] != 7;
}

void EngineSetMovement(float forward, float right)
{
    if (!EngineInputAllowed())
        return;
    // Engine speeds are -2..2 (2 = full speed); +forward is forward and
    // +sidestep is right, matching the joystick handlers.
    g_set_forward(forward * 2.0f);
    g_set_sidestep(right * 2.0f);
    g_apply_control(*g_player_control);
}

bool EngineInput(const char* name, bool pressed)
{
    if (!g_input_ok)
        return false;
    for (int i = 0; i < kInputVarCount; ++i) {
        if (strcmp(kInputVars[i].name, name) == 0) {
            char name_buf[32], value[4];
            strcpy_s(name_buf, name);
            strcpy_s(value, pressed ? "1" : "0");
            g_input_handler[i](name_buf, value);
            return true;
        }
    }
    return false;
}

bool EngineCommand(const char* command)
{
    if (!g_input_ok)
        return false;
    char buf[128];
    strcpy_s(buf, command);  // the engine tokenizes the string in place
    return CallCommandExecute(buf) == 0;
}

// The player's look-angle struct in its physics model, or null unless the
// normal first-person camera is active (same checks the engine makes).
static unsigned char* PlayerLookAngles()
{
    if (!EngineInputAllowed())
        return nullptr;
    int* cam = *g_engine.current_camera;
    int obj = *g_player_object;
    if (!cam || cam[0] != 0 || !obj)
        return nullptr;
    unsigned char* model = g_get_phys_model(obj);
    if (!model || ((*reinterpret_cast<uint32_t*>(model + 0x20) >> 9) & 1))
        return nullptr;
    return *reinterpret_cast<unsigned char**>(model + kModelRotation);  // may be null
}

static int16_t ToAngle16(float radians)
{
    return (int16_t)(int)(radians * 32768.0f / 3.14159265f);
}

static float g_queued_turn;
static bool g_have_pitch;
static float g_look_pitch;
static float g_aim = 0;            // desired body heading relative to the world yaw
static bool g_have_world = false;
static float g_world_yaw = 0;      // heading of the tracking forward (turns, mouse, scripts)
static int16_t g_last_heading = 0; // body heading we set at the last update

static float WrapAngle(float a)
{
    while (a > 3.14159265f)
        a -= 6.2831853f;
    while (a < -3.14159265f)
        a += 6.2831853f;
    return a;
}

static float FromAngle16(int16_t a)
{
    return a * (3.14159265f / 32768.0f);
}

// Runs at the start of the engine's player-camera update (FUN_00546ca0), right
// before it reads the look angles, so nothing (e.g. a physics step) can
// overwrite what's set here.
//
// The body heading is set absolutely to world yaw + aim offset. Anything else
// that turned the body since last time (the mouse's pending turn, scripts,
// teleports) is folded into the world yaw, so the view follows it too.
static void __cdecl CameraUpdateDetour()
{
    unsigned char* rot = PlayerLookAngles();
    unsigned char* pos = rot ? CallObjectPosition(*g_player_object) : nullptr;
    if (rot && pos) {
        int16_t& facing = *reinterpret_cast<int16_t*>(pos + kPositionHeading);
        int16_t& pending = *reinterpret_cast<int16_t*>(rot + kRotHeading);
        if (!g_have_world) {
            g_world_yaw = FromAngle16(facing) - g_aim;
            g_last_heading = facing;
            g_have_world = true;
        }
        int16_t external = (int16_t)(facing - g_last_heading + pending);
        g_world_yaw = WrapAngle(g_world_yaw + FromAngle16(external) + g_queued_turn);
        int16_t target = ToAngle16(WrapAngle(g_world_yaw + g_aim));
        pending = (int16_t)(target - facing);  // the update adds this to the facing
        g_last_heading = target;
        if (g_have_pitch)
            *reinterpret_cast<int16_t*>(rot + kRotPitch) = ToAngle16(g_look_pitch);
    }
    g_queued_turn = 0;
    g_have_pitch = false;
    reinterpret_cast<void(__cdecl*)()>(g_camera_update_trampoline)();
}

// --- F11 object-position trace (reverse-engineering aid) --------------------
// Logs every ObjPosUpdate (FUN_0054a3c0: obj in EDI, location in ESI, facing
// on the stack) for two frames, with the distance from the camera and the
// calling code, to find which code places which object (e.g. the player arm).
static int g_objtrace_frames;

static void __cdecl LogObjPosUpdate(int obj, const float* loc, const void* stack)
{
    char line[256];
    float x = 0, y = 0, z = 0, dist = -1;
    __try {
        x = loc[0], y = loc[1], z = loc[2];
        if (int* cam = *g_engine.current_camera) {
            const float* c = reinterpret_cast<const float*>(cam + 2);
            dist = std::sqrt((x - c[0]) * (x - c[0]) + (y - c[1]) * (y - c[1]) + (z - c[2]) * (z - c[2]));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    snprintf(line, sizeof(line), "ObjPosUpdate obj %d at (%.2f %.2f %.2f) %.2f ft from camera, called from", obj, x,
             y, z, dist);
    AppendExeCallers(line, sizeof(line), stack, 6);
    Log("%s", line);
}

__declspec(naked) static void ObjPosUpdateDetour()
{
    __asm {
        cmp dword ptr [g_objtrace_frames], 0
        je passthrough
        pushad
        lea eax, [esp + 32]  // stack at entry: return address first
        push eax
        push esi             // location
        push edi             // object
        call LogObjPosUpdate
        add esp, 12
        popad
    passthrough:
        jmp dword ptr [g_objpos_update_trampoline]
    }
}

void EngineTraceTick()
{
    static bool f11_was_down;
    bool f11_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    if (g_objtrace_frames > 0 && --g_objtrace_frames == 0)
        Log("=== object position trace end");
    if (f11_down && !f11_was_down && g_objpos_update_trampoline) {
        int* cam = *g_engine.current_camera;
        Log("=== object position trace begin (player object %d, camera at %.2f %.2f %.2f)", *g_player_object,
            cam ? reinterpret_cast<float*>(cam)[2] : 0.0f, cam ? reinterpret_cast<float*>(cam)[3] : 0.0f,
            cam ? reinterpret_cast<float*>(cam)[4] : 0.0f);
        g_objtrace_frames = 2;
    }
    f11_was_down = f11_down;
}

void EngineSetAimYaw(float radians)
{
    g_aim = WrapAngle(radians);
}

bool EngineWorldYaw(float& radians)
{
    radians = g_world_yaw;
    return g_have_world;
}

float EngineAimYaw()
{
    return g_aim;
}

void EngineQueueTurn(float radians)
{
    if (g_camera_update_trampoline)
        g_queued_turn += radians;
}

void EngineSetLookPitch(float radians)
{
    const float limit = 80.0f * 3.14159265f / 180.0f;
    g_look_pitch = radians > limit ? limit : radians < -limit ? -limit : radians;
    g_have_pitch = g_camera_update_trampoline != nullptr;
}

// --- Player melee weapon: hit spheres -------------------------------------
// FUN_0055fcd0 (a creature method, __thiscall(creature, weapon obj, weapon
// index), RET 8) places a creature's weapon physics spheres each frame. Each
// sphere sits between two of the creature's joints (world space, creature
// +0x19c) per the creature type's weapon table, and is set with
// FUN_00537270 (cdecl(obj, sphere index), point in EDI). The spheres only have
// physics (and so hit things) while the weapon is swinging.

namespace {

void* g_weapon_update_trampoline;
void* g_sphere_set_trampoline;
int g_player_weapon_in_update;  // weapon obj while the player's arm is placing its spheres, else 0
WeaponPointTransform g_weapon_transform;

// Snapshot of the arm's last weapon update (for the F12 dump / logging).
constexpr int kMaxJoints = 24, kMaxSpheres = 8;
struct ArmWeaponState {
    bool valid = false;
    int weapon = 0, spheres = 0;
    int joint_a[kMaxSpheres], joint_b[kMaxSpheres];
    float t[kMaxSpheres], radius[kMaxSpheres];
    float joints[kMaxJoints][3];
    int joints_read = 0;
    bool physical = false;
} g_arm_weapon;

// The arm's weapon table index per weapon object (from the engine's own
// updates), and how many arm weapon updates there have been.
struct WeaponIndex {
    int weapon = 0, index = -1;
} g_weapon_index[4];
int g_arm_update_count;

int WeaponIndexOf(int weapon)
{
    for (const WeaponIndex& w : g_weapon_index)
        if (w.weapon == weapon)
            return w.index;
    return -1;
}

void SnapshotArmWeapon(unsigned char* creature, int weapon, int index)
{
    ArmWeaponState& a = g_arm_weapon;
    a.valid = false;
    a.weapon = weapon;
    a.spheres = 0;
    a.joints_read = 0;
    __try {
        int type = *reinterpret_cast<int*>(creature + 0x38);
        unsigned char* desc = *reinterpret_cast<unsigned char**>(
            *reinterpret_cast<uintptr_t*>(g_base + 0x6a1554) + type * 4);  // DAT_00aa1554[type]
        unsigned char* table = desc ? *reinterpret_cast<unsigned char**>(desc + 0x3c) : nullptr;
        if (table && index >= 0) {
            int count = *reinterpret_cast<int*>(table + index * 8);
            const int* e = *reinterpret_cast<int**>(table + index * 8 + 4);
            for (int i = 0; e && i < count && i < kMaxSpheres; ++i, e += 5) {
                a.joint_a[i] = e[0];
                a.joint_b[i] = e[1];
                a.t[i] = reinterpret_cast<const float*>(e)[2];
                a.radius[i] = reinterpret_cast<const float*>(e)[3];
                a.spheres = i + 1;
            }
        }
        const float* joints = *reinterpret_cast<float**>(creature + 0x19c);
        for (int j = 0; joints && j < kMaxJoints; ++j) {
            a.joints[j][0] = joints[j * 3];
            a.joints[j][1] = joints[j * 3 + 1];
            a.joints[j][2] = joints[j * 3 + 2];
            a.joints_read = j + 1;
        }
        a.valid = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool __fastcall WeaponUpdateDetour(unsigned char* creature, void* /*edx*/, int weapon, int index)
{
    auto original = reinterpret_cast<bool(__fastcall*)(unsigned char*, void*, int, int)>(g_weapon_update_trampoline);
    int owner = *reinterpret_cast<int*>(creature + 8);
    int arm = EngineArmObject();
    if (!arm || owner != arm || weapon <= 0)
        return original(creature, nullptr, weapon, index);

    SnapshotArmWeapon(creature, weapon, index);
    ++g_arm_update_count;
    if (WeaponIndexOf(weapon) != index) {
        static int next;
        g_weapon_index[next] = {weapon, index};
        next = (next + 1) % 4;
    }
    g_player_weapon_in_update = g_weapon_transform ? weapon : 0;
    bool result = original(creature, nullptr, weapon, index);
    g_player_weapon_in_update = 0;

    g_arm_weapon.physical = g_get_phys_model && g_get_phys_model(weapon) != nullptr;
    return result;
}

void __cdecl TransformWeaponPoint(float* point)
{
    if (g_weapon_transform)
        g_weapon_transform(point);
}

__declspec(naked) void SphereSetDetour()
{
    __asm {
        mov eax, [esp + 4]  // object
        test eax, eax
        je passthrough
        cmp eax, dword ptr [g_player_weapon_in_update]
        jne passthrough
        pushad
        push edi            // the sphere's position (world), changed in place
        call TransformWeaponPoint
        add esp, 4
        popad
    passthrough:
        jmp dword ptr [g_sphere_set_trampoline]
    }
}

// --- VR melee timing ---
// The arm animation normally switches the weapon's hits on and off through
// motion-flag callbacks registered by the attack start (FUN_0046c950):
//   0x1000 -> 0x0046c460 -> FUN_0055aab0: creature->MakeWeaponPhysical(weapon)
//   0x2000 -> 0x0046c490 -> FUN_0055ab00: creature->MakeWeaponNonPhysical(weapon)
// (both: EAX = creature object, ESI = weapon). With VR melee on, those two
// callbacks are ignored and the swing opens/closes the hit window instead.
bool g_vr_melee;
void* g_anim_hits_on_trampoline;
void* g_anim_hits_off_trampoline;
unsigned char* g_cur_weapon_fn;    // FUN_0059db80: weapon obj(owner /*EDX*/)
unsigned char* g_weapon_on_fn;     // FUN_0055aab0
unsigned char* g_weapon_off_fn;    // FUN_0055ab00
int g_strike_arm, g_strike_weapon;  // the open window, or 0

// The attack ends when the swing motion does: FUN_0046bca0 (cdecl(a, b), the
// motion's end callback, also called when the weapon is put away from
// FUN_0046c3a0) switches the hits off, sends EndAttack and clears f94/f98.
// With the swing starting at once, the motion can end before the swing reaches
// its target, so while the VR hit window is open the end waits for it to close
// (EngineMeleeStrike(false)). Putting the weapon away still ends it at once.
void* g_end_attack_tramp;
bool g_end_attack_pending;
int g_end_attack_args[2];

void __cdecl EndAttackDetour(int a, int b)
{
    void* from = _ReturnAddress();
    if (g_vr_melee && g_strike_weapon && from != g_base + 0x6c3b8) {  // not from FUN_0046c3a0
        g_end_attack_pending = true;
        g_end_attack_args[0] = a;
        g_end_attack_args[1] = b;
        return;
    }
    g_end_attack_pending = false;
    reinterpret_cast<void(__cdecl*)(int, int)>(g_end_attack_tramp)(a, b);
}

// The swing starts at once: the release (FUN_0046ca90) requests the swing
// motion with FUN_0054c830, which only starts it if the arm isn't busy
// (motion controller DAT_00aa1410[6] -> vtbl 0x30(arm[2]) == 0); otherwise it
// waits for the wind-up motion to finish. Starting a motion enters the arm's
// state, which plays its sound ("Event Motion" + the state's tags, in
// FUN_0054bf40): the swing sound came ~0.5 s after the swing. With VR melee
// and a sword / blackjack out, the swing always starts at once, as in the
// not-busy case: stop the current motion ((*arm[0])->vtbl 0x24(0)), then
// FUN_0054c780 (ESI = the arm state) starts the requested one.
void* g_swing_request_tramp;

__declspec(naked) void __cdecl CallWithEsi(void* /*fn*/, void* /*esi*/)
{
    __asm {
        push esi
        push ebx
        push edi
        push ebp
        mov esi, [esp + 24]
        call dword ptr [esp + 20]
        pop ebp
        pop edi
        pop ebx
        pop esi
        ret
    }
}

void __cdecl SwingRequestDetour()
{
    int* arm = *reinterpret_cast<int**>(g_base + 0x6a1410);  // DAT_00aa1410
    int limb = *reinterpret_cast<int*>(g_base + 0x4717f8);   // DAT_008717f8
    if (!g_vr_melee || limb != 2 || !arm || !arm[6]) {
        reinterpret_cast<void(__cdecl*)()>(g_swing_request_tramp)();
        return;
    }
    arm[4] = 2;     // the swing
    arm[5] = 0xff;
    if (void* current = reinterpret_cast<void*>(arm[0])) {
        auto stop = reinterpret_cast<void(__thiscall*)(void*, int)>((*reinterpret_cast<void***>(current))[0x24 / 4]);
        stop(current, 0);
    }
    CallWithEsi(g_base + 0x14c780, arm);  // FUN_0054c780
}

int __cdecl AnimHitsOnDetour(int creature_obj)
{
    if (g_vr_melee)
        return 0;
    return reinterpret_cast<int(__cdecl*)(int)>(g_anim_hits_on_trampoline)(creature_obj);
}

void __cdecl AnimHitsOffDetour(int creature_obj)
{
    if (g_vr_melee)
        return;
    reinterpret_cast<void(__cdecl*)(int)>(g_anim_hits_off_trampoline)(creature_obj);
}

// FUN_0059db80 uses a custom register convention and doesn't preserve the
// callee-saved registers, so all of them are saved around the call.
__declspec(naked) int __cdecl CallCurWeapon(int /*owner*/)
{
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov ebp, esp
        mov edx, [ebp + 20]
        xor ecx, ecx
        call dword ptr [g_cur_weapon_fn]
        mov esp, ebp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}

// fn(EAX = creature obj, ESI = weapon)
__declspec(naked) int __cdecl CallCreatureWeapon(void* /*fn*/, int /*creature_obj*/, int /*weapon*/)
{
    __asm {
        push esi
        push ebx
        push edi
        push ebp
        mov eax, [esp + 24]
        mov esi, [esp + 28]
        call dword ptr [esp + 20]
        pop ebp
        pop edi
        pop ebx
        pop esi
        ret
    }
}

// --- Melee hit feedback ---
// The damage model (IDamageModel via AppGetObj FUN_00677250, stdcall, with the
// IID at 0x007e72d0): slot 3 HandleImpact and slot 4 DamageObject, both
// __stdcall(this, victim, culprit, data, ...). Calls involving the player's
// melee weapon shortly after a swing become hit events (EngineTakeMeleeHits).
void* g_dmg_tramp[2];
int g_melee_weapon;       // the weapon of the last strike
ULONGLONG g_melee_until;  // hit events count until this time (GetTickCount64)
int g_melee_events;
bool g_strike_stopped;    // the open window's weapon lost its physics (hit a wall)

void __cdecl OnDamageCall(int slot, int victim, int culprit)
{
    if (!g_melee_weapon || GetTickCount64() > g_melee_until)
        return;
    if (culprit == g_melee_weapon || victim == g_melee_weapon)
        g_melee_events |= slot == 4 ? kMeleeHitDamage : kMeleeHitImpact;
}

// Arity-agnostic: reads victim/culprit, then continues into the original.
#define DAMAGE_THUNK(n)                                                                                   \
    __declspec(naked) void DamageThunk##n()                                                               \
    {                                                                                                     \
        __asm pushad                                                                                      \
        __asm push dword ptr [esp + 44]                                                                   \
        __asm push dword ptr [esp + 44]                                                                   \
        __asm push n                                                                                      \
        __asm call OnDamageCall                                                                           \
        __asm add esp, 12                                                                                 \
        __asm popad                                                                                       \
        __asm jmp dword ptr [g_dmg_tramp + (n - 3) * 4]                                                   \
    }
DAMAGE_THUNK(3)
DAMAGE_THUNK(4)

// Calls an engine function fn(arg), saving every register and restoring the
// stack pointer afterwards, so it works whether fn pops its argument (stdcall,
// like AppGetObj: RET 4) or not, and whether or not it preserves registers.
__declspec(naked) void* __cdecl CallEngine1(void* /*fn*/, const void* /*arg*/)
{
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov ebp, esp
        push dword ptr [ebp + 24]
        call dword ptr [ebp + 20]
        mov esp, ebp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}

// Hooks the damage model on the first strike (it exists once a mission runs).
void InstallHitFeedback()
{
    static bool tried;
    if (tried)
        return;
    tried = true;
    void** model = static_cast<void**>(CallEngine1(g_base + 0x277250, g_base + 0x3e72d0));
    if (!model) {
        Log("Melee: no damage model; no hit feedback");
        return;
    }
    void** vtable = reinterpret_cast<void**>(*model);
    void* thunks[2] = {&DamageThunk3, &DamageThunk4};
    MH_STATUS st = MH_OK;
    for (int i = 0; i < 2 && st == MH_OK; ++i) {
        st = MH_CreateHook(vtable[3 + i], thunks[i], &g_dmg_tramp[i]);
        if (st == MH_OK)
            st = MH_EnableHook(vtable[3 + i]);
    }
    Log("Melee: hit feedback hooks %s", MH_StatusToString(st));
    // AppGetObj added a reference; the damage model lives as long as the game.
    reinterpret_cast<unsigned long(__stdcall*)(void*)>(vtable[2])(model);
}

} // namespace

// --- Audio listener follows the head ---
// NewDark's sound driver is a DirectSound3D-style listener on OpenAL. Its
// methods take vectors in engine world space (x, y, z; z up) and convert them
// for OpenAL (al = -y, z, -x):
//   0x006ac700 SetPosition(this, const float pos[3])                  RET 8  -> AL_POSITION
//   0x006ac7b0 SetOrientation(this, const float front[3], top[3])     RET 0xc -> AL_ORIENTATION
// The game feeds them the player camera, whose heading follows the aiming hand
// in VR; while a head pose is set they get the head's instead.
namespace {
void* g_listener_pos_tramp;
void* g_listener_ori_tramp;
float g_head_pos[3], g_head_front[3], g_head_top[3];
ULONGLONG g_head_until;  // the head pose is used until this time (GetTickCount64)

int __stdcall ListenerPositionDetour(void* self, const float* pos)
{
    if (GetTickCount64() <= g_head_until)
        pos = g_head_pos;
    return reinterpret_cast<int(__stdcall*)(void*, const float*)>(g_listener_pos_tramp)(self, pos);
}

// Most of the game's 3D sounds are positioned relative to the listener
// (AL_SOURCE_RELATIVE, set by 0x006b5280 SetMode), in the frame of the player
// camera (x forward, y left, z up), which OpenAL doesn't rotate by the listener
// orientation. Their positions are re-expressed in the head's frame:
//   p' = R_head^T (R_camera p + camera - head)
//   0x006b4fb0 SetPosition(this, const float pos[3])  RET 8; OpenAL source id at this+0x11d
void* g_source_pos_tramp;
int(__cdecl* g_al_get_sourcei)(unsigned, int, int*);

bool SourceIsRelative(unsigned char* source)
{
    if (!g_al_get_sourcei) {
        HMODULE al = *reinterpret_cast<HMODULE*>(g_base + 0x69d588);  // DAT_00a9d588: the OpenAL module
        if (!al)
            return false;
        g_al_get_sourcei = reinterpret_cast<int(__cdecl*)(unsigned, int, int*)>(GetProcAddress(al, "alGetSourcei"));
        if (!g_al_get_sourcei)
            return false;
    }
    int id = *reinterpret_cast<int*>(source + 0x11d);
    int relative = 0;
    if (id != -1)
        g_al_get_sourcei(static_cast<unsigned>(id), 0x202 /* AL_SOURCE_RELATIVE */, &relative);
    return relative != 0;
}

bool HeadRelativePosition(const float* p, float out[3])
{
    int* cam = *g_engine.current_camera;
    if (!cam || GetTickCount64() > g_head_until)
        return false;
    const float* c = reinterpret_cast<const float*>(cam + 2);
    const uint16_t* a = reinterpret_cast<const uint16_t*>(reinterpret_cast<unsigned char*>(cam) + 0x14);
    Mat3 camera = RotZ(a[2] * (2 * kPi / 65536.0f)) * RotY(a[1] * (2 * kPi / 65536.0f)) *
                  RotX(a[0] * (2 * kPi / 65536.0f));
    Vec3 x{g_head_front[0], g_head_front[1], g_head_front[2]}, z{g_head_top[0], g_head_top[1], g_head_top[2]};
    Vec3 y = Cross(z, x);
    Vec3 world = camera * Vec3{p[0], p[1], p[2]} + Vec3{c[0] - g_head_pos[0], c[1] - g_head_pos[1], c[2] - g_head_pos[2]};
    out[0] = Dot(x, world);
    out[1] = Dot(y, world);
    out[2] = Dot(z, world);
    return true;
}

int __stdcall SourcePositionDetour(unsigned char* self, const float* pos)
{
    float head_rel[3];
    if (GetTickCount64() <= g_head_until && SourceIsRelative(self) && HeadRelativePosition(pos, head_rel))
        pos = head_rel;
    return reinterpret_cast<int(__stdcall*)(unsigned char*, const float*)>(g_source_pos_tramp)(self, pos);
}

int __stdcall ListenerOrientationDetour(void* self, const float* front, const float* top)
{
    if (GetTickCount64() <= g_head_until) {
        front = g_head_front;
        top = g_head_top;
    }
    return reinterpret_cast<int(__stdcall*)(void*, const float*, const float*)>(g_listener_ori_tramp)(self, front,
                                                                                                        top);
}

// --- DirectSound (NewDark's other sound driver) ---
// With DirectSound the game sets the DirectSound3D listener to the player
// camera (which turns with the aiming hand) and plays most sounds head-relative
// (DS3DMODE_HEADRELATIVE) in the camera's frame. As for OpenAL above, the
// listener goes to the head and head-relative positions are re-expressed in
// the head's frame. DirectSound's own methods are hooked (their addresses from
// a throwaway DirectSound object), so NewDark's driver code isn't needed.
// DirectSound axes: x right, y up, z forward (engine: x forward, y left, z up).
Vec3 ToDs(Vec3 e)
{
    return {-e.y, e.z, e.x};
}
Vec3 FromDs(Vec3 d)
{
    return {d.z, -d.x, d.y};
}
float g_ds_scale = 1.0f;  // DirectSound units per foot (from the first listener position)
bool g_ds_scale_known;

using DsSetVector = HRESULT(__stdcall*)(void*, float, float, float, DWORD);
using DsSetOrientation = HRESULT(__stdcall*)(void*, float, float, float, float, float, float, DWORD);
using DsGetMode = HRESULT(__stdcall*)(void*, DWORD*);
using DsSetListenerAll = HRESULT(__stdcall*)(void*, const DS3DLISTENER*, DWORD);
using DsSetBufferAll = HRESULT(__stdcall*)(void*, const DS3DBUFFER*, DWORD);
DsSetVector g_ds_listener_pos_tramp, g_ds_buffer_pos_tramp;
DsSetOrientation g_ds_listener_ori_tramp;
DsSetListenerAll g_ds_listener_all_tramp;
DsSetBufferAll g_ds_buffer_all_tramp;
DsGetMode g_ds_buffer_get_mode;

// The listener at the head: the game's position (the camera) moved by head - camera.
bool DsHeadListener(Vec3& pos)
{
    int* cam = *g_engine.current_camera;
    if (!cam || GetTickCount64() > g_head_until)
        return false;
    const float* c = reinterpret_cast<const float*>(cam + 2);
    Vec3 camera{c[0], c[1], c[2]};
    if (!g_ds_scale_known) {
        Vec3 camera_ds = ToDs(camera);
        float len2 = Dot(camera_ds, camera_ds);
        float s = len2 > 1.0f ? Dot(pos, camera_ds) / len2 : 1.0f;
        g_ds_scale = s > 0.2f && s < 5.0f ? s : 1.0f;
        g_ds_scale_known = true;
        Log("Sound: DirectSound listener at (%.2f %.2f %.2f) for the camera at (%.2f %.2f %.2f): %.3f units per foot",
            pos.x, pos.y, pos.z, c[0], c[1], c[2], g_ds_scale);
    }
    pos = pos + ToDs(Vec3{g_head_pos[0], g_head_pos[1], g_head_pos[2]} - camera) * g_ds_scale;
    return true;
}

bool DsHeadRelative(void* buffer, DWORD mode, Vec3& pos)
{
    if (mode != DS3DMODE_HEADRELATIVE || GetTickCount64() > g_head_until)
        return false;
    Vec3 p = FromDs(pos) * (1.0f / g_ds_scale);
    const float in[3] = {p.x, p.y, p.z};
    float out[3];
    if (!HeadRelativePosition(in, out))
        return false;
    pos = ToDs(Vec3{out[0], out[1], out[2]}) * g_ds_scale;
    return true;
}

HRESULT __stdcall DsListenerPositionDetour(void* self, float x, float y, float z, DWORD apply)
{
    Vec3 p{x, y, z};
    DsHeadListener(p);
    return g_ds_listener_pos_tramp(self, p.x, p.y, p.z, apply);
}

HRESULT __stdcall DsListenerOrientationDetour(void* self, float fx, float fy, float fz, float tx, float ty, float tz,
                                              DWORD apply)
{
    if (GetTickCount64() <= g_head_until) {
        Vec3 f = ToDs({g_head_front[0], g_head_front[1], g_head_front[2]});
        Vec3 t = ToDs({g_head_top[0], g_head_top[1], g_head_top[2]});
        fx = f.x, fy = f.y, fz = f.z, tx = t.x, ty = t.y, tz = t.z;
    }
    return g_ds_listener_ori_tramp(self, fx, fy, fz, tx, ty, tz, apply);
}

HRESULT __stdcall DsListenerAllDetour(void* self, const DS3DLISTENER* params, DWORD apply)
{
    if (!params || GetTickCount64() > g_head_until)
        return g_ds_listener_all_tramp(self, params, apply);
    DS3DLISTENER l = *params;
    Vec3 p{l.vPosition.x, l.vPosition.y, l.vPosition.z};
    if (DsHeadListener(p))
        l.vPosition = {p.x, p.y, p.z};
    Vec3 f = ToDs({g_head_front[0], g_head_front[1], g_head_front[2]});
    Vec3 t = ToDs({g_head_top[0], g_head_top[1], g_head_top[2]});
    l.vOrientFront = {f.x, f.y, f.z};
    l.vOrientTop = {t.x, t.y, t.z};
    return g_ds_listener_all_tramp(self, &l, apply);
}

HRESULT __stdcall DsBufferPositionDetour(void* self, float x, float y, float z, DWORD apply)
{
    DWORD mode = 0;
    Vec3 p{x, y, z};
    if (g_ds_buffer_get_mode && SUCCEEDED(g_ds_buffer_get_mode(self, &mode)))
        DsHeadRelative(self, mode, p);
    return g_ds_buffer_pos_tramp(self, p.x, p.y, p.z, apply);
}

HRESULT __stdcall DsBufferAllDetour(void* self, const DS3DBUFFER* params, DWORD apply)
{
    if (!params)
        return g_ds_buffer_all_tramp(self, params, apply);
    DS3DBUFFER b = *params;
    Vec3 p{b.vPosition.x, b.vPosition.y, b.vPosition.z};
    if (DsHeadRelative(self, b.dwMode, p))
        b.vPosition = {p.x, p.y, p.z};
    return g_ds_buffer_all_tramp(self, &b, apply);
}

void InstallDirectSoundHooks()
{
    // IID_IDirectSound3DListener / IID_IDirectSound3DBuffer
    static const GUID kListener = {0x279afa84, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
    static const GUID kBuffer3d = {0x279afa86, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
    HMODULE dll = LoadLibraryA("dsound.dll");
    auto create = dll ? reinterpret_cast<HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN)>(
                            GetProcAddress(dll, "DirectSoundCreate8"))
                      : nullptr;
    IDirectSound8* ds = nullptr;
    if (!create || FAILED(create(nullptr, &ds, nullptr))) {
        Log("Engine: DirectSound unavailable; its sound stays relative to the body");
        return;
    }
    IDirectSoundBuffer *primary = nullptr, *secondary = nullptr;
    IDirectSound3DListener* listener = nullptr;
    IDirectSound3DBuffer* buffer = nullptr;
    ds->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY);
    DSBUFFERDESC pd{};
    pd.dwSize = sizeof(pd);
    pd.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D;
    if (SUCCEEDED(ds->CreateSoundBuffer(&pd, &primary, nullptr)))
        primary->QueryInterface(kListener, reinterpret_cast<void**>(&listener));
    WAVEFORMATEX fmt{WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};
    DSBUFFERDESC sd{};
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DSBCAPS_CTRL3D;
    sd.dwBufferBytes = 4096;
    sd.lpwfxFormat = &fmt;
    if (SUCCEEDED(ds->CreateSoundBuffer(&sd, &secondary, nullptr)))
        secondary->QueryInterface(kBuffer3d, reinterpret_cast<void**>(&buffer));
    MH_STATUS st = MH_ERROR_NOT_INITIALIZED;
    if (listener && buffer) {
        // IDirectSound3DListener: 10 SetAllParameters, 13 SetOrientation, 14 SetPosition.
        // IDirectSound3DBuffer: 9 GetMode, 12 SetAllParameters, 19 SetPosition.
        void** lv = *reinterpret_cast<void***>(listener);
        void** bv = *reinterpret_cast<void***>(buffer);
        g_ds_buffer_get_mode = reinterpret_cast<DsGetMode>(bv[9]);
        struct {
            void* target;
            void* detour;
            void** tramp;
        } hooks[] = {
            {lv[14], reinterpret_cast<void*>(&DsListenerPositionDetour), reinterpret_cast<void**>(&g_ds_listener_pos_tramp)},
            {lv[13], reinterpret_cast<void*>(&DsListenerOrientationDetour), reinterpret_cast<void**>(&g_ds_listener_ori_tramp)},
            {lv[10], reinterpret_cast<void*>(&DsListenerAllDetour), reinterpret_cast<void**>(&g_ds_listener_all_tramp)},
            {bv[19], reinterpret_cast<void*>(&DsBufferPositionDetour), reinterpret_cast<void**>(&g_ds_buffer_pos_tramp)},
            {bv[12], reinterpret_cast<void*>(&DsBufferAllDetour), reinterpret_cast<void**>(&g_ds_buffer_all_tramp)},
        };
        st = MH_OK;
        for (auto& h : hooks)
            if (st == MH_OK)
                st = MH_CreateHook(h.target, h.detour, h.tramp);
        for (auto& h : hooks)
            if (st == MH_OK)
                st = MH_EnableHook(h.target);
    }
    if (buffer)
        buffer->Release();
    if (secondary)
        secondary->Release();
    if (listener)
        listener->Release();
    if (primary)
        primary->Release();
    ds->Release();
    Log("Engine: DirectSound listener / source hooks %s", MH_StatusToString(st));
}
} // namespace

static void InstallListenerHooks()
{
    InstallDirectSoundHooks();
    unsigned char* set_pos = g_base + 0x2ac700;
    unsigned char* set_ori = g_base + 0x2ac7b0;
    if (!Matches(set_pos, "8b 44 24 08 8b 08 56 8b 74 24 08 83 be 45 01 00 00 00") ||
        !Matches(set_ori, "8b 44 24 08 8b 08 83 ec 18 56 8b 74 24 20 83 be 45 01 00 00 00")) {
        Log("Engine: sound listener signatures mismatch; sound stays relative to the body");
        return;
    }
    MH_STATUS st = MH_CreateHook(set_pos, reinterpret_cast<void*>(&ListenerPositionDetour), &g_listener_pos_tramp);
    if (st == MH_OK)
        st = MH_CreateHook(set_ori, reinterpret_cast<void*>(&ListenerOrientationDetour), &g_listener_ori_tramp);
    if (st == MH_OK)
        st = MH_EnableHook(set_pos);
    if (st == MH_OK)
        st = MH_EnableHook(set_ori);
    Log("Engine: sound listener hooks %s", MH_StatusToString(st));

    unsigned char* source_pos = g_base + 0x2b4fb0;
    if (!Matches(source_pos, "8b 44 24 08 8b 4c 24 04 8b 10 89 91 d1 00 00 00")) {
        Log("Engine: sound source signature mismatch; relative sounds stay relative to the body");
        return;
    }
    st = MH_CreateHook(source_pos, reinterpret_cast<void*>(&SourcePositionDetour), &g_source_pos_tramp);
    if (st == MH_OK)
        st = MH_EnableHook(source_pos);
    Log("Engine: sound source hook %s", MH_StatusToString(st));
}

void EngineSetListenerPose(const float pos[3], const float front[3], const float top[3])
{
    static bool logged;
    if (!logged && g_base) {
        // The 3D sound provider (DAT_0086f81c, from the config key snd3d).
        logged = true;
        int provider = *reinterpret_cast<int*>(g_base + 0x46f81c);
        if (provider == 4 || provider == 2)
            Log("Sound: 3D through %s", provider == 4 ? "OpenAL" : "DirectSound3D");
        else
            Log("Sound: WARNING: the game mixes 3D sound itself (provider %d), so sounds turn with the aiming "
                "hand; set \"snd3d openal\" (or \"snd3d a3d\") in cam_ext.cfg", provider);
    }
    for (int i = 0; i < 3; ++i) {
        g_head_pos[i] = pos[i];
        g_head_front[i] = front[i];
        g_head_top[i] = top[i];
    }
    g_head_until = GetTickCount64() + 250;
}

void EngineClearListenerPose()
{
    g_head_until = 0;
}

static unsigned char* CreatureOf(int obj);

int EngineTakeMeleeHits()
{
    // A wall stops the swing: the engine takes the weapon's physics away.
    if (g_strike_weapon && !g_strike_stopped && g_get_phys_model && !g_get_phys_model(g_strike_weapon)) {
        g_strike_stopped = true;
        g_melee_events |= kMeleeHitWall;
    }
    // The engine only places the weapon's hit spheres (and tests them for hits)
    // while the arm's swing motion runs, which can end before the VR swing
    // does: while the hit window is open, any frame without that update gets
    // one here (FUN_0055fcd0, through its hook, so the spheres are in the hand).
    static int seen_updates;
    if (g_strike_weapon && !g_strike_stopped && g_arm_update_count == seen_updates) {
        unsigned char* creature = CreatureOf(g_strike_arm);
        int index = WeaponIndexOf(g_strike_weapon);
        if (creature && index >= 0)
            WeaponUpdateDetour(creature, nullptr, g_strike_weapon, index);
    }
    seen_updates = g_arm_update_count;
    int events = g_melee_events;
    g_melee_events = 0;
    return events;
}

void EngineSetVrMelee(bool enabled)
{
    g_vr_melee = enabled && g_anim_hits_on_trampoline && g_anim_hits_off_trampoline;
}

bool EngineMeleeStrike(bool open)
{
    if (!g_vr_melee || !g_player_object)
        return false;
    if (!open) {
        if (g_strike_weapon)
            CallCreatureWeapon(g_weapon_off_fn, g_strike_arm, g_strike_weapon);
        g_strike_arm = g_strike_weapon = 0;
        if (g_end_attack_pending && g_end_attack_tramp) {
            g_end_attack_pending = false;
            reinterpret_cast<void(__cdecl*)(int, int)>(g_end_attack_tramp)(g_end_attack_args[0], g_end_attack_args[1]);
            // The attack end also arms "ready for the next attack" (DAT_00890f8c
            // = 1, LAB_0046bc90) for the arm motion's next 0x2000 flag, which
            // has passed by now: ready it here instead.
            int* ready = reinterpret_cast<int*>(g_base + 0x490f8c);
            if (!*reinterpret_cast<int*>(g_base + 0x490f94) && !*reinterpret_cast<int*>(g_base + 0x490f98) &&
                !*reinterpret_cast<int*>(g_base + 0x490f9c))
                *ready = 1;
        }
        g_melee_until = GetTickCount64() + 300;  // late hit reports still count
        return true;
    }
    // Only after the engine accepted the attack (released, swing pending).
    int released = *reinterpret_cast<int*>(g_base + 0x490f94);  // DAT_00890f94
    int arm = EngineArmObject();
    int weapon = CallCurWeapon(*g_player_object);
    if (!released || !arm || weapon <= 0)
        return false;  // e.g. the game refused the attack (still recovering from the last one)
    InstallHitFeedback();
    CallCreatureWeapon(g_weapon_on_fn, arm, weapon);
    g_strike_arm = arm;
    g_strike_weapon = weapon;
    g_strike_stopped = false;
    g_melee_weapon = weapon;
    g_melee_until = GetTickCount64() + 1500;
    return true;
}

// --- The bow shot ---
// The bow fires with FUN_005500e0(player, arrow DAT_00aa0f8c, power, 0x204, 0, 0, 0)
// (from FUN_0046aed0). FUN_005500e0 launches a projectile: cdecl(launcher,
// projectile, power, flags, extra velocity*, ?, start point*). Without a start
// point it takes the projectile's own position, and its facing from the
// launcher; for the player (FUN_0054ffa0) that's the player camera's angles
// (DAT_00aa141c + 0x14: 16-bit bank, pitch, heading). So the arrow left from the
// engine's own (unpinned) bow, along the camera. For the player's nocked arrow
// the start point is the drawn arrow's centre and the camera's angles are set
// to its direction for the call.
using LaunchFn = int(__cdecl*)(int, int, float, unsigned, float*, int, float*);
static LaunchFn g_launch_tramp;
static bool g_bow_shot_valid;
static float g_bow_shot_offset[3], g_bow_shot_dir[3];

// The player's throws (frob "inventory -> world", FUN_00451530: flags 0x802, a
// fixed power) go through the launcher too, from the camera along its facing.
// With VR throwing they leave from the hand instead, along the swing, harder
// the faster it is (EngineSetThrow, set as the grip is let go).
static bool g_throw_valid;
static ULONGLONG g_throw_until;
static float g_throw_offset[3], g_throw_dir[3], g_throw_power = 1.0f;

// Launches from the camera + offset (clamped to arm's reach) along dir: the
// start point is passed, and the camera's angles (the launcher's facing) are
// set to dir for the call.
static int LaunchAlong(int* cam, const float offset[3], const float dir[3], int launcher, int projectile, float power,
                       unsigned flags, float* velocity, int arg6)
{
    const float* c = reinterpret_cast<const float*>(cam + 2);
    float o[3] = {offset[0], offset[1], offset[2]};
    float reach = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
    const float kMaxReach = 2.5f;
    if (reach > kMaxReach)
        for (float& v : o)
            v *= kMaxReach / reach;
    float from[3] = {c[0] + o[0], c[1] + o[1], c[2] + o[2]};

    // Facing: forward = RotZ(heading) * RotY(pitch) * x = (cos p cos h, cos p sin h, -sin p).
    const float to16 = 65536.0f / (2.0f * 3.14159265f);
    float heading = std::atan2(dir[1], dir[0]);
    float pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, -dir[2])));
    uint16_t* angles = reinterpret_cast<uint16_t*>(reinterpret_cast<unsigned char*>(cam) + 0x14);
    uint16_t saved[3] = {angles[0], angles[1], angles[2]};
    angles[0] = 0;
    angles[1] = static_cast<uint16_t>(static_cast<int>(std::lround(pitch * to16)));
    angles[2] = static_cast<uint16_t>(static_cast<int>(std::lround(heading * to16)));
    int result = g_launch_tramp(launcher, projectile, power, flags, velocity, arg6, from);
    angles[0] = saved[0];
    angles[1] = saved[1];
    angles[2] = saved[2];
    return result;
}

static int __cdecl LaunchDetour(int launcher, int projectile, float power, unsigned flags, float* velocity, int arg6,
                                float* start)
{
    int* cam = *g_engine.current_camera;
    int player = *reinterpret_cast<int*>(g_base + 0x6a1418);  // DAT_00aa1418
    if (start || !cam || cam[0] != 0 || launcher != player || projectile <= 0)
        return g_launch_tramp(launcher, projectile, power, flags, velocity, arg6, start);
    if (g_bow_shot_valid && projectile == EngineNockedArrow() && EngineLimbMode() == 1)
        return LaunchAlong(cam, g_bow_shot_offset, g_bow_shot_dir, launcher, projectile, power, flags, velocity, arg6);
    if (g_throw_valid && GetTickCount64() <= g_throw_until && (flags & 0x800) && !(flags & 4)) {
        g_throw_valid = false;
        return LaunchAlong(cam, g_throw_offset, g_throw_dir, launcher, projectile, power * g_throw_power, flags,
                           velocity, arg6);
    }
    return g_launch_tramp(launcher, projectile, power, flags, velocity, arg6, start);
}

void EngineSetThrow(const float offset[3], const float dir[3], float power_scale)
{
    for (int i = 0; i < 3; ++i) {
        g_throw_offset[i] = offset[i];
        g_throw_dir[i] = dir[i];
    }
    g_throw_power = power_scale;
    g_throw_valid = g_launch_tramp != nullptr;
    g_throw_until = GetTickCount64() + 300;
}

// --- Physical frob ---
// The frob target comes from a pick each frame: FUN_0058fed0 resets it (DAT_009a337c
// = 0, best score DAT_009a3384), then while objects are drawn FUN_00463ae0 offers
// each frobbable one to FUN_0058fe80 (object in EAX), which scores it by its
// on-screen box against the screen centre (in VR: where the eyes look) and keeps
// the best; FUN_0044fe00 makes that the highlighted target (DAT_00aa1e08). With
// hand frob, objects are scored against the hand instead: one it touches wins,
// else the one it points at (a cone from the hand).
static bool g_hand_ray_valid;
static ULONGLONG g_hand_ray_until;
static float g_hand_origin[3], g_hand_dir[3];
static float g_hand_pick_best = 1e30f;
static void* g_pick_candidate_tramp;   // FUN_0058fe80
static void* g_pick_reset_tramp;       // FUN_0058fed0
static void* g_pick_exclude_fn;        // FUN_004509b0: nonzero = not pickable (object in ESI)
static int* g_pick_candidate;          // DAT_009a337c

// Calls FUN_004509b0 with the object in ESI, keeping the callee-saved registers.
__declspec(naked) static int __cdecl PickExcluded(int /*obj*/)
{
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov esi, [esp + 20]
        call dword ptr [g_pick_exclude_fn]
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}

// The hand's score for an object (lower is better), or -1 if it doesn't qualify.
static float HandPickScore(int obj)
{
    const float* p = reinterpret_cast<const float*>(EngineObjectPosition(obj));
    if (!p)
        return -1;
    float v[3] = {p[0] - g_hand_origin[0], p[1] - g_hand_origin[1], p[2] - g_hand_origin[2]};
    float dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const float kTouch = 1.0f;  // ft: the hand is on it
    if (dist < kTouch)
        return dist * 0.1f;
    float along = v[0] * g_hand_dir[0] + v[1] * g_hand_dir[1] + v[2] * g_hand_dir[2];
    if (along < 0.1f)
        return -1;
    float perp = std::sqrt(std::fmax(0.0f, dist * dist - along * along));
    float slope = perp / along;  // tan of the angle off the hand's pointing
    if (slope > 0.5f)
        return -1;
    return 0.1f + slope + 0.02f * along;
}

// Within the game's frob reach of the player: its pick allows objects whose
// distance from the camera, less their size, is within DAT_009a3388 (squared).
static bool InFrobReach(int obj)
{
    int* cam = *g_engine.current_camera;
    const float* p = reinterpret_cast<const float*>(EngineObjectPosition(obj));
    float reach2 = *reinterpret_cast<float*>(g_base + 0x5a3388);  // DAT_009a3388
    if (!cam || !p || !(reach2 > 0))
        return true;
    const float* c = reinterpret_cast<const float*>(cam + 2);
    float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
    float d = std::sqrt(dx * dx + dy * dy + dz * dz) - 1.0f;  // ~1 ft for the object's size
    return d <= 0 || d * d <= reach2;
}

// Returns 1 if the hand decided (the candidate is DAT_009a337c), 0 to let the game score it.
static int __cdecl HandPickCandidate(int obj)
{
    if (!g_hand_ray_valid || GetTickCount64() > g_hand_ray_until)
        return 0;
    __try {
        float score = HandPickScore(obj);
        if (score >= 0 && score < g_hand_pick_best && InFrobReach(obj) && !PickExcluded(obj)) {
            g_hand_pick_best = score;
            *g_pick_candidate = obj;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 1;
}

__declspec(naked) static void PickCandidateDetour()
{
    __asm {
        pushad
        push eax
        call HandPickCandidate
        add esp, 4
        test eax, eax
        popad
        jz original
        mov eax, dword ptr [g_pick_candidate]
        mov eax, [eax]
        ret
    original:
        jmp dword ptr [g_pick_candidate_tramp]
    }
}

static void __cdecl PickResetDetour()
{
    reinterpret_cast<void(__cdecl*)()>(g_pick_reset_tramp)();
    g_hand_pick_best = 1e30f;
}

void EngineSetHandRay(bool valid, const float origin[3], const float dir[3])
{
    g_hand_ray_valid = valid && g_pick_candidate_tramp;
    if (!g_hand_ray_valid)
        return;
    for (int i = 0; i < 3; ++i) {
        g_hand_origin[i] = origin[i];
        g_hand_dir[i] = dir[i];
    }
    g_hand_ray_until = GetTickCount64() + 250;
}

static void InstallPickHooks()
{
    unsigned char* candidate = g_base + 0x18fe80;  // FUN_0058fe80
    unsigned char* reset = g_base + 0x18fed0;      // FUN_0058fed0
    unsigned char* exclude = g_base + 0x509b0;     // FUN_004509b0
    if (!Matches(candidate, "51 56 8b f0 e8 ?? ?? ?? ?? d9 5c 24 04 d9 44 24 04 d9 05 ?? ?? ?? ??") ||
        !Matches(reset, "a1 ?? ?? ?? ?? d9 05 ?? ?? ?? ?? 83 ec 34 d9 1d ?? ?? ?? ?? 56 57 c7 05") ||
        !Matches(exclude, "51 85 f6 74 39 a1 ?? ?? ?? ?? 8b 08 8d 14 24 52 56 50")) {
        Log("Engine: frob pick signatures mismatch; frob follows the view");
        return;
    }
    g_pick_exclude_fn = exclude;
    g_pick_candidate = reinterpret_cast<int*>(g_base + 0x5a337c);  // DAT_009a337c
    MH_STATUS st = MH_CreateHook(candidate, reinterpret_cast<void*>(&PickCandidateDetour), &g_pick_candidate_tramp);
    if (st == MH_OK)
        st = MH_CreateHook(reset, reinterpret_cast<void*>(&PickResetDetour), &g_pick_reset_tramp);
    if (st == MH_OK)
        st = MH_EnableHook(candidate);
    if (st == MH_OK)
        st = MH_EnableHook(reset);
    if (st != MH_OK)
        g_pick_candidate_tramp = nullptr;
    Log("Engine: frob pick hooks %s", MH_StatusToString(st));
}

int EngineHeldJunk()
{
    // The inventory (IInventory, DAT_00a9f9fc; __stdcall, this first): vtbl 0x30
    // WieldingJunk(), vtbl 0x2c the wielded object.
    void** inv = g_base ? *reinterpret_cast<void***>(g_base + 0x69f9fc) : nullptr;
    if (!inv)
        return 0;
    __try {
        void** vt = *reinterpret_cast<void***>(inv);
        if (!reinterpret_cast<int(__stdcall*)(void*)>(vt[0x30 / 4])(inv))
            return 0;
        return reinterpret_cast<int(__stdcall*)(void*)>(vt[0x2c / 4])(inv);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The inventory display (FUN_00452780, cdecl(int), drawn after its layout
// FUN_00452f10) skips a slot whose "hidden" flag is set: slot 1 (the item, or the
// wielded junk shown centred) at DAT_00aadbc8. The layout clears it every frame,
// so it's set just before the draw while the held object shows in the hand.
static void* g_hud_draw_tramp;
static bool g_hide_item_slot;

static void __cdecl HudDrawDetour(int arg)
{
    if (g_hide_item_slot)
        *reinterpret_cast<int*>(g_base + 0x6adbc8) = 1;  // DAT_00aadbc8
    reinterpret_cast<void(__cdecl*)(int)>(g_hud_draw_tramp)(arg);
}

void EngineHideItemSlot(bool hide)
{
    g_hide_item_slot = hide && g_hud_draw_tramp;
}

static void InstallHudHook()
{
    unsigned char* draw = g_base + 0x52780;  // FUN_00452780
    if (!Matches(draw, "6a ff 68 ?? ?? ?? ?? 64 a1 00 00 00 00 50 81 ec c8 00 00 00 53 55 56 57")) {
        Log("Engine: inventory display signature mismatch; a held object also shows on screen");
        return;
    }
    MH_STATUS st = MH_CreateHook(draw, reinterpret_cast<void*>(&HudDrawDetour), &g_hud_draw_tramp);
    if (st == MH_OK)
        st = MH_EnableHook(draw);
    if (st != MH_OK)
        g_hud_draw_tramp = nullptr;
    Log("Engine: inventory display hook %s", MH_StatusToString(st));
}

int EngineFrobTarget()
{
    return g_base ? *reinterpret_cast<int*>(g_base + 0x6a1e08) : 0;  // DAT_00aa1e08
}

int EngineNockedArrow()
{
    return g_base ? *reinterpret_cast<int*>(g_base + 0x6a0f8c) : 0;  // DAT_00aa0f8c
}

bool EngineObjectModelName(int obj, char* out, int size)
{
    // The ModelName property (DAT_00ab00c4, set up by the engine), read as
    // FUN_005bd430 does (all __stdcall, this first):
    //   vtbl 0x28 (obj): whether the object has the property itself;
    //   if not, the object it inherits it from: the property's interface
    //   IID 0x007ddd40 (QueryInterface, vtbl 0), vtbl 0x1c (obj);
    //   vtbl 0x54 Get(obj, const char** name).
    void** prop = g_base ? *reinterpret_cast<void***>(g_base + 0x6b00c4) : nullptr;
    if (!prop || obj == 0 || size <= 0)
        return false;
    __try {
        void** vt = *reinterpret_cast<void***>(prop);
        int from = obj;
        auto has = reinterpret_cast<int(__stdcall*)(void*, int)>(vt[0x28 / 4]);
        if (!has(prop, obj)) {
            static void** donors;
            if (!donors) {
                auto query = reinterpret_cast<long(__stdcall*)(void*, const void*, void**)>(vt[0]);
                void* out = nullptr;
                if (query(prop, g_base + 0x3ddd40, &out) != 0 || !out)
                    return false;
                donors = static_cast<void**>(out);
            }
            auto donor = reinterpret_cast<int(__stdcall*)(void*, int)>((*reinterpret_cast<void***>(donors))[0x1c / 4]);
            from = donor(donors, obj);
        }
        auto get = reinterpret_cast<int(__stdcall*)(void*, int, const char**)>(vt[0x54 / 4]);
        const char* name = nullptr;
        if (!get(prop, from, &name) || !name || !name[0])
            return false;
        strncpy_s(out, size, name, _TRUNCATE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void EngineSetBowShot(bool valid, const float offset[3], const float dir[3])
{
    g_bow_shot_valid = valid && g_launch_tramp;
    if (!valid)
        return;
    for (int i = 0; i < 3; ++i) {
        g_bow_shot_offset[i] = offset[i];
        g_bow_shot_dir[i] = dir[i];
    }
}

static void InstallWeaponHooks()
{
    unsigned char* update = g_base + 0x15fcd0;  // FUN_0055fcd0
    unsigned char* set = g_base + 0x137270;     // FUN_00537270
    if (!Matches(update, "55 8b ec 83 e4 f8 6a ff 68 ?? ?? ?? ?? 64 a1 00 00 00 00 50 81 ec 90 00 00 00") ||
        !Matches(set, "53 8b 5c 24 0c 55 8b 6c 24 0c 56 55 e8")) {
        Log("Engine: weapon sphere signatures mismatch; melee hits stay where the game puts them");
        return;
    }
    MH_STATUS st = MH_CreateHook(set, reinterpret_cast<void*>(&SphereSetDetour), &g_sphere_set_trampoline);
    if (st == MH_OK)
        st = MH_CreateHook(update, reinterpret_cast<void*>(&WeaponUpdateDetour), &g_weapon_update_trampoline);
    if (st == MH_OK)
        st = MH_EnableHook(set);
    if (st == MH_OK)
        st = MH_EnableHook(update);
    Log("Engine: weapon sphere hooks %s", MH_StatusToString(st));

    unsigned char* anim_on = g_base + 0x6c460;   // LAB_0046c460
    unsigned char* anim_off = g_base + 0x6c490;  // LAB_0046c490
    unsigned char* cur_weapon = g_base + 0x19db80;
    unsigned char* weapon_on = g_base + 0x15aab0;
    unsigned char* weapon_off = g_base + 0x15ab00;
    if (!Matches(anim_on, "83 3d ?? ?? ?? ?? 00 75 21 83 3d ?? ?? ?? ?? 00 75 18 8b 15 ?? ?? ?? ?? 56 e8") ||
        !Matches(anim_off, "8b 15 ?? ?? ?? ?? 56 e8 ?? ?? ?? ?? 8b f0 8b 44 24 08 e8") ||
        !Matches(weapon_on, "85 f6 75 03 33 c0 c3 85 c0 74 f9 8b 0d") ||
        !Matches(weapon_off, "85 f6 74 40 85 c0 74 3c 8b 0d")) {
        Log("Engine: melee timing signatures mismatch; swings use the game's timing");
        return;
    }
    g_cur_weapon_fn = cur_weapon;
    g_weapon_on_fn = weapon_on;
    g_weapon_off_fn = weapon_off;
    st = MH_CreateHook(anim_on, reinterpret_cast<void*>(&AnimHitsOnDetour), &g_anim_hits_on_trampoline);
    if (st == MH_OK)
        st = MH_CreateHook(anim_off, reinterpret_cast<void*>(&AnimHitsOffDetour), &g_anim_hits_off_trampoline);
    if (st == MH_OK)
        st = MH_EnableHook(anim_on);
    if (st == MH_OK)
        st = MH_EnableHook(anim_off);
    if (st != MH_OK)
        g_anim_hits_on_trampoline = g_anim_hits_off_trampoline = nullptr;
    Log("Engine: melee timing hooks %s", MH_StatusToString(st));

    unsigned char* end_attack = g_base + 0x6bca0;  // FUN_0046bca0
    if (st == MH_OK &&
        Matches(end_attack, "a1 ?? ?? ?? ?? 85 c0 74 05 8b 40 04 eb 02 33 c0 68 ?? ?? ?? ?? 68 00 20 00 00 50 e8") &&
        Matches(g_base + 0x6c3b3, "e8 ?? ?? ?? ?? 83 c4 08")) {
        MH_STATUS es = MH_CreateHook(end_attack, reinterpret_cast<void*>(&EndAttackDetour), &g_end_attack_tramp);
        if (es == MH_OK)
            es = MH_EnableHook(end_attack);
        if (es != MH_OK)
            g_end_attack_tramp = nullptr;
        Log("Engine: attack end hook %s", MH_StatusToString(es));
    } else {
        Log("Engine: attack end signature mismatch; the swing motion ends the hit window");
    }

    unsigned char* swing_request = g_base + 0x14c830;  // FUN_0054c830
    if (st == MH_OK && Matches(swing_request, "a1 ?? ?? ?? ?? 8b 48 18 56 8b f0 c7 40 10 02 00 00 00 c7 40 14 ff 00 00 00") &&
        Matches(g_base + 0x14c780, "8b 46 0c 8b 56 14 53 57 8b 7e 10 85 c0")) {
        MH_STATUS ss = MH_CreateHook(swing_request, reinterpret_cast<void*>(&SwingRequestDetour), &g_swing_request_tramp);
        if (ss == MH_OK)
            ss = MH_EnableHook(swing_request);
        if (ss != MH_OK)
            g_swing_request_tramp = nullptr;
        Log("Engine: swing start hook %s", MH_StatusToString(ss));
    } else {
        Log("Engine: swing start signature mismatch; swings wait for the wind-up animation");
    }

    unsigned char* launch = g_base + 0x1500e0;  // FUN_005500e0
    if (Matches(launch, "55 8b ec 83 e4 f8 a1 ?? ?? ?? ?? 81 ec cc 00 00 00 53 8b 5d 0c 56 57")) {
        MH_STATUS ls = MH_CreateHook(launch, reinterpret_cast<void*>(&LaunchDetour),
                                     reinterpret_cast<void**>(&g_launch_tramp));
        if (ls == MH_OK)
            ls = MH_EnableHook(launch);
        if (ls != MH_OK)
            g_launch_tramp = nullptr;
        Log("Engine: launch hook %s", MH_StatusToString(ls));
    } else {
        Log("Engine: launch signature mismatch; arrows leave from the game's own spot");
    }
    InstallPickHooks();
    InstallHudHook();
}

void EngineSetWeaponPointTransform(WeaponPointTransform transform)
{
    g_weapon_transform = transform;
}

// The creature for an object (as FUN_0055aab0 finds it), or null.
static unsigned char* CreatureOf(int obj)
{
    __try {
        unsigned char* sys = *reinterpret_cast<unsigned char**>(g_base + 0x59b8b0);  // DAT_0099b8b0
        if (!sys || obj <= 0)
            return nullptr;
        void* mapper = sys + 0xe4;
        auto index_of = reinterpret_cast<int(__fastcall*)(void*, void*, int)>(**reinterpret_cast<void***>(mapper));
        int idx = index_of(mapper, nullptr, obj);
        if (idx <= 0)
            return nullptr;
        unsigned char* entry = (*reinterpret_cast<unsigned char***>(sys + 0xd4))[idx];
        return entry ? *reinterpret_cast<unsigned char**>(entry + 4) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool EngineArmJoints(float out[][3], int count)
{
    unsigned char* creature = CreatureOf(EngineArmObject());
    if (!creature)
        return false;
    __try {
        const float* joints = *reinterpret_cast<float**>(creature + 0x19c);
        if (!joints)
            return false;
        for (int j = 0; j < count; ++j)
            for (int k = 0; k < 3; ++k) {
                float v = joints[j * 3 + k];
                if (!(v == v) || std::fabs(v - joints[k]) > 20.0f)  // NaN, or far from the arm's root
                    return false;
                out[j][k] = v;
            }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void EngineMeleeReady()
{
    // No attack under way (winding up, released, swinging) and none held back:
    // the arm is ready for the next one, whatever missed arming it.
    if (!g_vr_melee || !g_base || g_end_attack_pending)
        return;
    if (!*reinterpret_cast<int*>(g_base + 0x490f94) && !*reinterpret_cast<int*>(g_base + 0x490f98) &&
        !*reinterpret_cast<int*>(g_base + 0x490f9c))
        *reinterpret_cast<int*>(g_base + 0x490f8c) = 1;  // DAT_00890f8c
}

int EngineCurrentWeapon()
{
    int limb = EngineLimbMode();
    if (!g_cur_weapon_fn || !g_player_object || !*g_player_object || (limb != 1 && limb != 2))
        return 0;
    return CallCurWeapon(*g_player_object);
}

bool EngineMeleeBusy()
{
    // DAT_00890f9c winding up, DAT_00890f94 released, DAT_00890f98 swinging.
    return g_base && (*reinterpret_cast<int*>(g_base + 0x490f9c) || *reinterpret_cast<int*>(g_base + 0x490f94) ||
                      *reinterpret_cast<int*>(g_base + 0x490f98));
}

void EngineLogPlayerPhysics()
{
    int obj = g_player_object ? *g_player_object : 0;
    unsigned char* model = obj && g_get_phys_model ? g_get_phys_model(obj) : nullptr;
    unsigned char* pos = EngineObjectPosition(obj);
    if (!model || !pos) {
        Log("PhysDump: no player physics model");
        return;
    }
    __try {
        const float* p = reinterpret_cast<const float*>(pos);
        unsigned char* subs = *reinterpret_cast<unsigned char**>(model + 0xf0);
        const float* radii = *reinterpret_cast<float**>(model + 0x1f0);
        Log("PhysDump: player %d at (%.3f %.3f %.3f), model %p flags %08x type %d", obj, p[0], p[1], p[2], model,
            *reinterpret_cast<uint32_t*>(model + 0x20), **reinterpret_cast<int**>(model + 0xd0));
        for (int i = 0; subs && i < 7; ++i) {
            const float* s = reinterpret_cast<const float*>(subs + i * 0x48);
            const uint16_t* a = reinterpret_cast<const uint16_t*>(subs + i * 0x48 + 0x10);
            Log("PhysDump:   sub %d rel (%.3f %.3f %.3f) radius %.3f angles %u %u %u", i, s[0] - p[0], s[1] - p[1],
                s[2] - p[2], radii ? radii[i] : -1.0f, a[0], a[1], a[2]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("PhysDump: unreadable");
    }
}

void EngineLogArmWeapon()
{
    int arm = EngineArmObject();
    unsigned char* creature = CreatureOf(arm);
    Log("ArmDump: arm %d creature %p", arm, creature);
    if (creature) {
        __try {
            const float* joints = *reinterpret_cast<float**>(creature + 0x19c);
            for (int j = 0; joints && j < kMaxJoints; ++j)
                Log("ArmDump:   joint now %2d (%.3f %.3f %.3f)", j, joints[j * 3], joints[j * 3 + 1], joints[j * 3 + 2]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("ArmDump:   joints unreadable");
        }
    }
    const ArmWeaponState& a = g_arm_weapon;
    if (!a.valid) {
        Log("ArmDump: no weapon update seen for the arm");
        return;
    }
    Log("ArmDump: last swing: weapon %d, %s, %d hit spheres", a.weapon, a.physical ? "physical" : "not physical", a.spheres);
    for (int i = 0; i < a.spheres; ++i)
        Log("ArmDump:   sphere %d: joints %d-%d t %.3f radius %.3f", i, a.joint_a[i], a.joint_b[i], a.t[i],
            a.radius[i]);
    for (int j = 0; j < a.joints_read; ++j)
        Log("ArmDump:   joint at swing %2d (%.3f %.3f %.3f)", j, a.joints[j][0], a.joints[j][1], a.joints[j][2]);
}
