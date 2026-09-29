#pragma once
#include <cstdint>

// Dark engine camera position (Location + mxs_angvec), 22 bytes.
// World axes: +X forward, +Y left, +Z up; units are feet.
// Angles are 16-bit (0x10000 = 360 deg), composed heading * pitch * bank.
#pragma pack(push, 2)
struct EnginePosition {
    float x, y, z;
    int16_t cell;  // -1: engine recomputes
    int16_t hint;
    uint16_t bank;     // tx
    uint16_t pitch;    // ty, positive = down
    uint16_t heading;  // tz, CCW, forward = (cos, sin)
};
#pragma pack(pop)
static_assert(sizeof(EnginePosition) == 22, "EnginePosition layout");

// Addresses of engine functions/globals for the running exe, resolved from
// the module base. Every field is null when the build isn't supported.
struct EngineAddresses {
    void* scene_render = nullptr;    // void(EnginePosition* /*EAX*/, double focal)
    int** current_camera = nullptr;  // int* cam: [0] mode, [1] zoom (float)
    void* arm_render = nullptr;      // void __cdecl(): draws the first-person arm after the scene
};

// Identifies the exe build and verifies signatures. Returns false if this
// build isn't supported (the mod then stays out of the engine's way).
bool ResolveEngine();
const EngineAddresses& Engine();

// --- Player input (driven by the VR controllers) ---------------------------

// False while the game ignores player control (menus, cutscenes, dead...).
bool EngineInputAllowed();

// Analog movement, -1..1 each (1 = full speed forward / right). Applied through
// the same player-control path the joystick uses.
void EngineSetMovement(float forward, float right);

// Presses (true) or releases an input the game binds keys to, by name:
// "jump", "crouch", "crouchhold", "leanleft", "leanright", "runon",
// "use_weapon", "use_item", "block", "lookcenter". Returns false if unknown.
bool EngineInput(const char* name, bool pressed);

// Runs a game console command, e.g. "inv_select sword", "next_item", "sim_menu".
bool EngineCommand(const char* command);

// Turns the player by an exact angle (radians, positive = left). Applied at the
// start of the engine's next player-camera update, where it can't be lost.
void EngineQueueTurn(float radians);

// The player body's heading is kept at "world yaw + aim offset". The world yaw
// is the heading of the tracking forward: it changes with turns (EngineQueueTurn)
// and anything else that turns the player (mouse, scripts). The aim offset is
// where the hand (or head) points relative to the tracking forward; set it each
// frame. The view is rendered from the world yaw, so it doesn't follow the aim.
void EngineSetAimYaw(float radians);
float EngineAimYaw();
bool EngineWorldYaw(float& radians);  // false until the player camera has run

// Once per frame: F11 logs every object position update for two frames.
void EngineTraceTick();

// The first-person arm (weapon) object, or 0 if none, and an object's
// Position (float x,y,z at +0; 16-bit bank/pitch/heading at +0x10), or null.
int EngineArmObject();
unsigned char* EngineObjectPosition(int obj);

// The active render context (DAT_00aac0c8), or null outside rendering.
unsigned char* EngineRenderContext();

// The player's current limb mode (DAT_008717f8): which first-person arm is
// out (0xff = none; 4 = carrying a body). Changes are logged.
int EngineLimbMode();

// Sets the player's look pitch (radians, positive = down) for the next
// player-camera update; aim and frob use it. Call every frame to keep it set.
void EngineSetLookPitch(float radians);

// The player's melee weapon hit spheres. The engine places them each frame from
// the first-person arm's joints (world space); the transform set here is applied
// to each position so hits land where the weapon is drawn (nullptr = off).
typedef void (*WeaponPointTransform)(float* point);
void EngineSetWeaponPointTransform(WeaponPointTransform transform);

// Logs the arm's weapon spheres and joint positions (F12 dump).
void EngineLogArmWeapon();

// VR melee: the swing decides when the weapon hits, not the arm animation.
// While enabled, the animation's own hits-on/off callbacks are ignored and
// EngineMeleeStrike opens (after the engine accepted a quick attack, i.e.
// right after "use_weapon" was released) and closes the hit window.
void EngineSetVrMelee(bool enabled);
bool EngineMeleeStrike(bool open);

// The first-person arm's 5 joints (world space; 0 = root, 3 = hand, 4 = the
// weapon's tip), or false.
bool EngineArmJoints(float out[5][3]);

// True while a sword / blackjack attack is under way (wind-up to recovery).
bool EngineMeleeBusy();
