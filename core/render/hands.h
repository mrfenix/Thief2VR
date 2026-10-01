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
// --- The bow ------------------------------------------------------------------------
// The game's bow model (bow2.bin) in the bow hand, a string, and the nocked
// arrow's own model. The bow's frame: x along the arrow (towards the target),
// z up the bow, origin the handle's centre (in the fist).
struct BowInput {
    Mat3 r;                        // the bow's frame (world, unit axes)
    Vec3 grip;                     // the handle's centre (world)
    bool pulled = false;           // the other hand is drawing the string (two-handed)
    Vec3 string_hand;              // the drawing hand's grip point (world)
    float auto_draw = -1;          // one-handed: how far it's drawn (0..1), -1 = not drawing
    bool left_hand = true;         // the bow is in the left hand (the arrow passes on its left side)
    const char* arrow = nullptr;   // the nocked arrow's model file (e.g. "arrow.bin"), or null
};
struct BowPose {
    bool visible = false;
    Mat3 r;                        // pivoted onto the arrow line while drawing
    Vec3 grip;
    Vec3 nock;                     // where the string is pulled to (world)
    float draw = 0;                // limb bend, 0..1
    const char* arrow = nullptr;
    // Not yet on the string: the arrow held in the drawing hand (its frame: x
    // along the arrow, and where its nock is).
    bool arrow_in_hand = false;
    Mat3 hand_r;
    Vec3 hand_nock;
};
// The bow this frame, and where its arrow would leave: the arrow's centre and
// direction (world). False until the bow model is loaded (from the game files).
bool MakeBow(const BowInput& in, BowPose& out, Vec3& shot_centre, Vec3& shot_dir);
// The bow's frame for a hand holding it: the handle through the fist (up the
// bow out of the index-finger end), the arrow towards the fingers.
Mat3 BowFrameInHand(const Hand& hand, float world_scale);

// A picked-up object held in the right hand: its model file and the hand's frame
// (see WeaponFrame); the model is centred in the fist.
struct HeldPose {
    bool visible = false;
    const char* model = nullptr;
    Mat3 r;
    Vec3 p;
    bool in_hand = true;  // false: centred at p at its own size (e.g. the pouch at the waist)
    float scale = 1.0f;   // in the hand: on top of the usual size
};

void Draw(IDirect3DDevice9* dev, IDirect3DSurface9* color, IDirect3DSurface9* depth, const Eye& eye,
          const Hand& left, const Hand& right, const WeaponPose& weapon, const BowPose& bow, const HeldPose& held,
          const HeldPose& pouch, float brightness);

// True once the game's weapon models were loaded (on the first Draw).
bool WeaponModelsLoaded();

// The weapon's frame when held in the right hand: the grip at the controller's
// grip point, the blade along the handle (towards the index finger), its width
// towards the knuckles. world_scale: feet per metre.
void WeaponFrame(const Hand& right, float world_scale, Mat3& r, Vec3& p);

// Release D3D9 resources (before IDirect3DDevice9::Reset).
void OnDeviceReset();

}  // namespace hands
