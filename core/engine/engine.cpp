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
#include <cstring>

static void* g_camera_update_trampoline;
static void __cdecl CameraUpdateDetour();
static void* g_objpos_update_trampoline;
static void ObjPosUpdateDetour();
static void InstallWeaponHooks();

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
    return true;
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
unsigned char* g_arm_creature_seen;  // `this` of the last arm weapon update (diagnostics)

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
    g_arm_creature_seen = creature;
    g_player_weapon_in_update = g_weapon_transform ? weapon : 0;
    bool result = original(creature, nullptr, weapon, index);
    g_player_weapon_in_update = 0;

    bool physical = g_get_phys_model && g_get_phys_model(weapon) != nullptr;
    if (physical != g_arm_weapon.physical)
        Log("Weapon: %d %s (%d hit spheres)", weapon, physical ? "swinging, hits enabled" : "hits disabled",
            g_arm_weapon.spheres);
    g_arm_weapon.physical = physical;
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

// --- Hit logging (diagnostics) ---
// The damage model (IDamageModel, from AppGetObj FUN_00677250 with the IID at
// 0x007e72d0): slot 3 HandleImpact, 4 DamageObject, 5 SlayObject, all
// (this, victim, culprit, data, ...). Logged when the player's weapon, arm or
// the player is involved, or within 1 s of a VR hit window.
void* g_dmg_tramp[3];
ULONGLONG g_last_strike_ms;

void __cdecl LogDamageCall(int slot, int victim, int culprit, const int* data)
{
    int player = g_player_object ? *g_player_object : 0;
    int arm = EngineArmObject();
    bool related = culprit == player || culprit == arm || culprit == g_strike_weapon ||
                   GetTickCount64() - g_last_strike_ms < 1000;
    if (!related)
        return;
    static const char* names[] = {"HandleImpact", "DamageObject", "SlayObject"};
    int d0 = 0, d1 = 0;
    __try {
        if (data) {
            d0 = data[0];
            d1 = data[1];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Log("Hit: %s victim %d culprit %d (player %d, arm %d, weapon %d) data %d %d / %.2f", names[slot - 3], victim,
        culprit, player, arm, g_strike_weapon, d0, d1, *reinterpret_cast<float*>(&d0));
}

#define DAMAGE_THUNK(n)                                                                                       __declspec(naked) void DamageThunk##n()                                                                   {                                                                                                             __asm pushad                                                                                              __asm push dword ptr [esp + 48]                                                                           __asm push dword ptr [esp + 48]                                                                           __asm push dword ptr [esp + 48]                                                                           __asm push n                                                                                              __asm call LogDamageCall                                                                                  __asm add esp, 16                                                                                         __asm popad                                                                                               __asm jmp dword ptr [g_dmg_tramp + (n - 3) * 4]                                                       }
DAMAGE_THUNK(3)
DAMAGE_THUNK(4)
DAMAGE_THUNK(5)

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

void InstallDamageLogging()
{
    static bool tried;
    if (tried)
        return;
    tried = true;
    // AppGetObj (FUN_00677250) with the damage model's IID.
    void** model = static_cast<void**>(CallEngine1(g_base + 0x277250, g_base + 0x3e72d0));
    if (!model) {
        Log("Hit logging: no damage model");
        return;
    }
    void** vtable = reinterpret_cast<void**>(*model);
    void* thunks[3] = {&DamageThunk3, &DamageThunk4, &DamageThunk5};
    for (int i = 0; i < 3; ++i) {
        MH_STATUS st = MH_CreateHook(vtable[3 + i], thunks[i], &g_dmg_tramp[i]);
        if (st == MH_OK)
            st = MH_EnableHook(vtable[3 + i]);
        Log("Hit logging: damage model slot %d at %p: %s", 3 + i, vtable[3 + i], MH_StatusToString(st));
    }
    // AppGetObj added a reference; the damage model lives as long as the game.
    reinterpret_cast<unsigned long(__stdcall*)(void*)>(vtable[2])(model);
}

} // namespace

void EngineSetVrMelee(bool enabled)
{
    g_vr_melee = enabled && g_anim_hits_on_trampoline && g_anim_hits_off_trampoline;
}

bool EngineMeleeStrike(bool open)
{
    if (!g_vr_melee || !g_player_object)
        return false;
    if (!open) {
        g_last_strike_ms = GetTickCount64();
        if (g_strike_weapon)
            CallCreatureWeapon(g_weapon_off_fn, g_strike_arm, g_strike_weapon);
        g_strike_arm = g_strike_weapon = 0;
        return true;
    }
    // Only after the engine accepted the attack (released, swing pending).
    int released = *reinterpret_cast<int*>(g_base + 0x490f94);  // DAT_00890f94
    int arm = EngineArmObject();
    int weapon = CallCurWeapon(*g_player_object);
    if (!released || !arm || weapon <= 0) {
        Log("Melee: strike not started (released %d, arm %d, weapon %d)", released, arm, weapon);
        return false;
    }
    InstallDamageLogging();
    g_last_strike_ms = GetTickCount64();
    CallCreatureWeapon(g_weapon_on_fn, arm, weapon);
    g_strike_arm = arm;
    g_strike_weapon = weapon;
    return true;
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

bool EngineArmJoints(float out[5][3])
{
    unsigned char* creature = CreatureOf(EngineArmObject());
    if (!creature)
        return false;
    __try {
        const float* joints = *reinterpret_cast<float**>(creature + 0x19c);
        if (!joints)
            return false;
        for (int j = 0; j < 5; ++j)
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

bool EngineMeleeBusy()
{
    // DAT_00890f9c winding up, DAT_00890f94 released, DAT_00890f98 swinging.
    return g_base && (*reinterpret_cast<int*>(g_base + 0x490f9c) || *reinterpret_cast<int*>(g_base + 0x490f94) ||
                      *reinterpret_cast<int*>(g_base + 0x490f98));
}

void EngineLogArmWeapon()
{
    int arm = EngineArmObject();
    unsigned char* creature = CreatureOf(arm);
    Log("ArmDump: arm %d creature %p (weapon update saw %p)", arm, creature, g_arm_creature_seen);
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
