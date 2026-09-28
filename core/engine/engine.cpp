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
