#pragma once
#include <openxr/openxr.h>

#include "../render/frame_transfer.h"

// The HUD's elements on the wrists, like a watch: the light gem, health and the
// general inventory on the left wrist, the weapon on the right. They're cut out
// of the captured HUD (and so left out of the HUD in front of the view) into a
// small texture, and each shows as a quad on the back of its wrist while you
// look at it.
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

}  // namespace wrist_hud
