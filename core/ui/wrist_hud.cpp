#include "wrist_hud.h"

#include "../config/settings.h"
#include "../engine/engine.h"
#include "../vrmath.h"
#include "../xr/xr_input.h"

#include <cmath>

namespace wrist_hud {
namespace {

enum { kGem, kHealth, kItem, kWeapon };

// Atlas cells (x, y, w, h).
const int kCells[kCuts][4] = {
    {0, 0, 256, 112},      // light gem
    {0, 120, 1024, 112},   // health (a long row of shields)
    {0, 240, 320, 272},    // general inventory slot
    {336, 240, 320, 272},  // weapon slot
};

float g_scale = 1;  // HUD image pixels per canvas pixel (the capture scale)
bool g_shown[2];    // per wrist (0 left, 1 right), with hysteresis

Quat QuatFromColumns(Vec3 x, Vec3 y, Vec3 z)
{
    // Rotation matrix with columns x, y, z -> quaternion.
    float m00 = x.x, m11 = y.y, m22 = z.z, trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0) {
        float s = std::sqrt(trace + 1.0f) * 2;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    return q;
}

Vec3 ToVec(const XrVector3f& v)
{
    return {v.x, v.y, v.z};
}

}  // namespace

bool PrepareCuts(int image_w, int image_h, TransferCut cuts[kCuts])
{
    int canvas[2], rect[2][4], obj[2], hidden[2];
    if (!Config().wrist_hud || !EngineHudLayout(canvas, rect, obj, hidden) || canvas[0] <= 0)
        return false;
    float k = (float)image_w / canvas[0];
    g_scale = k;
    auto zone = [&](TransferCut& c, float x0, float y0, float x1, float y1) {
        c.zone[0] = (int)(x0 * k), c.zone[1] = (int)(y0 * k), c.zone[2] = (int)(x1 * k), c.zone[3] = (int)(y1 * k);
    };
    for (int i = 0; i < kCuts; ++i) {
        for (int j = 0; j < 4; ++j) {
            cuts[i].cell[j] = kCells[i][j];
            cuts[i].zone[j] = cuts[i].clear[j] = 0;
        }
    }
    // The inventory slots: where the game puts them (slot 0 the weapon, 1 the item).
    // Cleared with a margin around them (the display's arrows sit just outside).
    float w = (float)canvas[0], h = (float)canvas[1];
    for (int slot = 0; slot < 2; ++slot)
        if (obj[slot] && !hidden[slot]) {
            TransferCut& c = cuts[slot == 0 ? kWeapon : kItem];
            zone(c, (float)rect[slot][0], (float)rect[slot][1], (float)rect[slot][2], (float)rect[slot][3]);
            float mx = w * 0.08f, my = h * 0.08f;
            c.clear[0] = (int)(std::fmax(0.0f, rect[slot][0] - mx) * k);
            c.clear[1] = (int)(std::fmax(0.0f, rect[slot][1] - my) * k);
            c.clear[2] = (int)(std::fmin(w, rect[slot][2] + mx) * k);
            c.clear[3] = (int)(std::fmin(h, rect[slot][3] + my) * k);
        }
    // Below them: the health row (left) and the light gem (centre).
    float below = std::fmax((float)rect[0][3], (float)rect[1][3]);
    if (below <= 0 || below > h - 10)
        below = h * 0.93f;
    zone(cuts[kHealth], 0, below, w * 0.45f, h);
    zone(cuts[kGem], w * 0.45f, below, w * 0.6f, h);
    (void)image_h;
    return true;
}

int Layers(const TransferCut cuts[kCuts], XrSwapchain atlas, XrSpace space, XrTime time, const XrPosef& head,
           XrCompositionLayerQuad out[kCuts])
{
    const Settings& s = Config();
    if (!s.wrist_hud || atlas == XR_NULL_HANDLE)
        return 0;
    const Quat head_q{head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w};
    const Vec3 head_pos = ToVec(head.position), head_fwd = Rotate(head_q, {0, 0, -1});
    int count = 0;
    for (int side = 0; side < 2; ++side) {
        // The wrist's settings (VR menu, HUD tab).
        const float size_setting = side == 0 ? s.wrist_left_size : s.wrist_right_size;
        const float rotation_deg = side == 0 ? s.wrist_left_rotation_deg : s.wrist_right_rotation_deg;
        const float along_m = (side == 0 ? s.wrist_left_along_cm : s.wrist_right_along_cm) * 0.01f;
        const float up_m = (side == 0 ? s.wrist_left_up_cm : s.wrist_right_up_cm) * 0.01f;
        // Metres per HUD canvas pixel, then per HUD image pixel.
        const float px = 0.00028f * size_setting / g_scale;
        XrPosef grip, aim;
        if (!XrControls().LocateHand(side, time, grip, aim))
            continue;
        // The back of the wrist: behind the hand along the forearm, on the back-of-hand side.
        Quat q{aim.orientation.x, aim.orientation.y, aim.orientation.z, aim.orientation.w};
        Vec3 forward = Rotate(q, {0, 0, -1}), right = Rotate(q, {1, 0, 0});
        Vec3 normal = side == 0 ? right * -1.0f : right;  // out of the back of the hand
        Vec3 anchor = ToVec(grip.position) - forward * 0.09f + normal * 0.035f;
        // Readable when the wrist is raised: x along the forearm (left to right as
        // you look at it), y up, z towards you.
        Vec3 x = side == 0 ? forward : forward * -1.0f;
        Vec3 z = normal;
        Vec3 y = Cross(z, x);
        // Turned to line up with the wrist (counter-clockwise as you look at it) and
        // moved along the forearm / up and down, as set.
        const float turn = rotation_deg * kPi / 180.0f;
        Vec3 xt = x * std::cos(turn) + y * std::sin(turn);
        Vec3 yt = y * std::cos(turn) - x * std::sin(turn);
        anchor = anchor + forward * along_m;
        x = xt;
        y = yt;
        anchor = anchor + y * up_m;
        Quat orient = QuatFromColumns(x, y, z);

        // Shown while you look at it and it faces you.
        Vec3 to = anchor - head_pos;
        float dist = Length(to);
        if (dist < 1e-3f)
            continue;
        to = to * (1.0f / dist);
        float look = Dot(head_fwd, to), facing = -Dot(normal, to);
        float angle = s.wrist_hud_look_angle * kPi / 180.0f;
        float need = std::cos(g_shown[side] ? angle + 0.1f : angle);
        g_shown[side] = look > need && facing > (g_shown[side] ? 0.2f : 0.35f);
        if (!g_shown[side])
            continue;

        // The elements on this wrist, laid out around the anchor (metres, x / y).
        auto scale_of = [&](int cut) { return cut == kHealth ? s.wrist_left_health_size : 1.0f; };
        auto add = [&](int cut, float ox, float oy) {
            const TransferCut& c = cuts[cut];
            if (!c.found)
                return;
            const float cpx = px * scale_of(cut);
            int bw = c.box[2] - c.box[0], bh = c.box[3] - c.box[1];
            XrCompositionLayerQuad& quad = out[count++];
            quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.space = space;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = atlas;
            quad.subImage.imageRect = {{c.cell[0], c.cell[1]}, {bw, bh}};
            Vec3 p = anchor + x * ox + y * oy;
            quad.pose = {{orient.x, orient.y, orient.z, orient.w}, {p.x, p.y, p.z}};
            quad.size = {bw * cpx, bh * cpx};
        };
        auto size = [&](int cut, bool height) {
            const TransferCut& c = cuts[cut];
            return c.found ? (height ? c.box[3] - c.box[1] : c.box[2] - c.box[0]) * px * scale_of(cut) : 0.0f;
        };
        if (side == 0) {
            // Left wrist: the health row, with the light gem and the item above it.
            float row = std::fmax(size(kGem, true), size(kItem, true));
            add(kHealth, 0, 0);
            float up = size(kHealth, true) * 0.5f + 0.008f + row * 0.5f;
            add(kGem, -size(kGem, false) * 0.5f - 0.006f, up);
            add(kItem, size(kItem, false) * 0.5f + 0.006f, up);
        } else {
            add(kWeapon, 0, 0);
        }
    }
    return count;
}

}  // namespace wrist_hud
