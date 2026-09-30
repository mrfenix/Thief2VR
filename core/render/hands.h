#pragma once
#include <d3d9.h>

#include "../vrmath.h"
#include "device_hooks.h"

// Visible hands: the WebXR generic hand models (tools/make_hands.py) as black
// fingerless leather gloves, with articulated fingers, plus the game's own
// sword / blackjack model (RES\obj.crf, loaded at runtime) in the right hand
// instead of the game's first-person arm.
//
// Everything is drawn by us inside each eye pass with the engine's own
// projection and depth mapping, so walls hide it. The weapon model is held at
// the right controller's grip; the melee hit spheres are moved onto it.
namespace hands {

// The draw capture that reads the (hidden) arm each frame while a sword /
// blackjack is out (with skip_draw): its lighting (average vertex colour) and
// which weapon it holds (its texture coordinates are matched against the sword
// arm's and the blackjack arm's meshes).
DrawCapture ArmLightCapture();
enum class Weapon { None, Sword, Blackjack };
Weapon ArmWeapon();       // None until known
void ResetArmWeapon();    // a different arm / weapon was drawn
// The game's current weapon object (0 if none): once seen with a clear arm, it
// decides at once on later draws.
void SetWeaponObject(int obj);

// --- Depth mapping ----------------------------------------------------------------
bool NeedDepthSample();
DrawCapture DepthSampleCapture();
void EndDepthSample();
bool Ready();  // the engine's depth mapping is known

// --- Drawing ----------------------------------------------------------------------
struct Eye {
    Mat3 rot;         // engine orientation of the eye (heading * pitch * bank)
    Vec3 pos;         // world position
    float f, cx, cy;  // projection in the eye target's pixels
};
// A hand: its controller's grip pose (LOCAL tracking space, metres, OpenXR
// axes), the tracking space -> engine world mapping (world = b + a * p), and
// how curled each finger is (0 straight .. 1 fully curled).
struct Hand {
    bool visible = false;
    Mat3 grip_rot;
    Vec3 grip_pos;
    Mat3 a;
    Vec3 b;
    float thumb = 0, index = 0, middle = 0, ring = 0, pinky = 0;
};
// The weapon: which one, and the world pose of its frame (origin at the grip,
// x towards the tip). See WeaponFrame.
struct WeaponPose {
    Weapon kind = Weapon::None;
    Mat3 r;
    Vec3 p;
};
void Draw(IDirect3DDevice9* dev, IDirect3DSurface9* color, IDirect3DSurface9* depth, const Eye& eye,
          const Hand& left, const Hand& right, const WeaponPose& weapon, float brightness);

// True once the game's weapon models were loaded (on the first Draw).
bool WeaponModelsLoaded();

// The weapon's frame when held in the right hand: the grip at the controller's
// grip point, the blade along the handle (towards the index finger), its width
// towards the knuckles. world_scale: feet per metre.
void WeaponFrame(const Hand& right, float world_scale, Mat3& r, Vec3& p);

// Release D3D9 resources (before IDirect3DDevice9::Reset).
void OnDeviceReset();

}  // namespace hands
