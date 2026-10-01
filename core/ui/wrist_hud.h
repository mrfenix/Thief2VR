#pragma once
#include <openxr/openxr.h>

#include "../render/frame_transfer.h"
#include "../vrmath.h"

// The HUD's elements on the body: the light gem and health on the left wrist and
// the weapon on the right, like a watch (shown while you look at them); the
// general inventory at the waist, front left, above a pouch (or on the left
// wrist). They're cut out of the captured HUD (and so left out of the HUD in
// front of the view) into a small texture, shown as quads.
namespace wrist_hud {

constexpr int kAtlasWidth = 1024, kAtlasHeight = 512;
constexpr int kCuts = 4;  // light gem, health, general inventory, weapon

// Before the HUD upload: the zones to cut (in the HUD image, image_w x image_h)
// and their cells in the atlas. False when the wrist HUD is off.
bool PrepareCuts(int image_w, int image_h, TransferCut cuts[kCuts]);

// After it: the quads for what was found (cuts from the last upload), on the
// wrists, for the ones being looked at. Returns how many were written to out.
int Layers(const TransferCut cuts[kCuts], XrSwapchain atlas, XrSpace space, XrTime time, const XrPosef& head,
           XrCompositionLayerQuad out[kCuts]);

// The pouch at the waist (below the inventory there), in the tracking space
// (LOCAL, metres): its centre, and its right / forward directions (level).
// False when it isn't shown.
bool PouchPose(Vec3& centre, Vec3& right, Vec3& forward);

// Whether the game's inventory display shows the selected item (as of the last
// HUD capture). known is false when that can't be told (the wrist HUD off).
bool ItemShown(bool& known);

}  // namespace wrist_hud
