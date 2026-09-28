#include "screen_pointer.h"

#include "../log.h"
#include "../xr/xr_pose.h"

namespace {

constexpr int kDotSize = 32;
constexpr float kDotM = 0.025f;  // pointer dot diameter on the screen

bool g_click_down;      // left button held (sent down, not yet up)
bool g_trigger;         // trigger state with hysteresis
bool g_b_was_down = true;
int g_esc_frames;       // Esc is down: frames left until it's released
POINT g_last_cursor{-1, -1};
bool g_logged_click, g_logged_focus;

XrSwapchainD3D11 g_dot;
bool g_dot_ready, g_dot_failed;

bool GameHasFocus(HWND window)
{
    return window && GetForegroundWindow() == GetAncestor(window, GA_ROOT);
}

void SendMouseButton(bool down)
{
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
}

void SendKey(WORD vk, bool down)
{
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}

// Releases Esc once it has been down for a frame (so a polling game sees it).
void TickEsc()
{
    if (g_esc_frames > 0 && --g_esc_frames == 0)
        SendKey(VK_ESCAPE, false);
}

// Releases a held click (Esc goes up on its own, see TickEsc).
void ReleaseHeld()
{
    if (g_click_down) {
        SendMouseButton(false);
        g_click_down = false;
    }
}

// A small ring (dark edge, white centre) in a static swapchain, premultiplied alpha.
bool EnsureDot()
{
    if (g_dot_ready || g_dot_failed)
        return g_dot_ready;
    g_dot_failed = true;
    if (!g_dot.Create(Xr().session(), Xr().color_format(), kDotSize, kDotSize))
        return false;
    uint32_t pixels[kDotSize * kDotSize];
    for (int y = 0; y < kDotSize; ++y)
        for (int x = 0; x < kDotSize; ++x) {
            float dx = x + 0.5f - kDotSize * 0.5f, dy = y + 0.5f - kDotSize * 0.5f;
            float r = std::sqrt(dx * dx + dy * dy) / (kDotSize * 0.5f);  // 0 centre .. 1 edge
            float a = r < 0.9f ? 1.0f : r < 1.0f ? (1.0f - r) * 10.0f : 0.0f;
            float c = r < 0.6f ? 1.0f : 0.0f;  // white centre, black ring
            uint8_t ab = (uint8_t)(a * 255.0f), cb = (uint8_t)(c * a * 255.0f);
            // Grey levels only, so BGRA vs RGBA doesn't matter.
            pixels[y * kDotSize + x] = (uint32_t)ab << 24 | (uint32_t)cb << 16 | (uint32_t)cb << 8 | cb;
        }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = kDotSize;
    td.MipLevels = td.ArraySize = 1;
    td.Format = Xr().color_format_is_rgba() ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA data{pixels, kDotSize * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(Xr().device()->CreateTexture2D(&td, &data, &tex)))
        return false;
    ID3D11Texture2D* image = g_dot.Acquire();
    if (image) {
        Xr().context()->CopyResource(image, tex);
        g_dot.Release();
    }
    tex->Release();
    if (!image)
        return false;
    g_dot_failed = false;
    g_dot_ready = true;
    return true;
}

} // namespace

bool ScreenPointerUpdate(const XrControllerState& c, const XrPosef& screen_pose, float width_m, float height_m,
                         HWND window, XrCompositionLayerQuad& dot)
{
    TickEsc();
    if (!c.active) {
        ReleaseHeld();
        return false;
    }

    float u = 0, v = 0;
    bool hit = c.pose_valid[1] && RayHitQuad(c.aim_pose[1], screen_pose, width_m, height_m, u, v);
    bool focus = GameHasFocus(window);
    if (hit && !focus && !g_logged_focus) {
        g_logged_focus = true;
        Log("ScreenPointer: the game window doesn't have the focus; pointer input is not sent");
    }

    // Cursor: the hit point in the window's client area.
    if (hit && focus) {
        RECT rc;
        GetClientRect(window, &rc);
        POINT p{(LONG)(u * (rc.right - rc.left)), (LONG)(v * (rc.bottom - rc.top))};
        if (p.x >= rc.right)
            p.x = rc.right - 1;
        if (p.y >= rc.bottom)
            p.y = rc.bottom - 1;
        ClientToScreen(window, &p);
        if (p.x != g_last_cursor.x || p.y != g_last_cursor.y) {
            SetCursorPos(p.x, p.y);
            g_last_cursor = p;
        }
    }

    // Click: right trigger (hysteresis). Presses only start on the screen;
    // a held press is always released.
    bool trigger = g_trigger ? c.trigger[1] > 0.4f : c.trigger[1] > 0.6f;
    if (trigger != g_trigger) {
        g_trigger = trigger;
        if (trigger && hit && focus) {
            SendMouseButton(true);
            g_click_down = true;
            XrControls().Vibrate(1, 0.15f, 0.02f);
            if (!g_logged_click) {
                g_logged_click = true;
                Log("ScreenPointer: first click at (%.3f, %.3f) of the screen, cursor (%ld, %ld)", u, v,
                    g_last_cursor.x, g_last_cursor.y);
            }
        } else if (!trigger && g_click_down) {
            SendMouseButton(false);
            g_click_down = false;
        }
    }

    // B: Esc (back / close the map, objectives, books).
    if (c.b && !g_b_was_down)
        ScreenPointerSendEsc(window);
    g_b_was_down = c.b;

    if (!hit || !EnsureDot())
        return false;
    Quat q{screen_pose.orientation.x, screen_pose.orientation.y, screen_pose.orientation.z, screen_pose.orientation.w};
    Vec3 center{screen_pose.position.x, screen_pose.position.y, screen_pose.position.z};
    Vec3 pos = center + Rotate(q, {(u - 0.5f) * width_m, (0.5f - v) * height_m, 0.01f});
    dot = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    dot.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    dot.space = Xr().local_space();
    dot.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    dot.subImage.swapchain = g_dot.handle();
    dot.subImage.imageRect = {{0, 0}, {kDotSize, kDotSize}};
    dot.pose.orientation = screen_pose.orientation;
    dot.pose.position = {pos.x, pos.y, pos.z};
    dot.size = {kDotM, kDotM};
    return true;
}

void ScreenPointerIdle()
{
    TickEsc();
    ReleaseHeld();
    // A trigger or B held from the game doesn't count as a press on the next screen.
    g_trigger = true;
    g_b_was_down = true;
    g_last_cursor = {-1, -1};
}

bool ScreenPointerSendEsc(HWND window)
{
    if (!GameHasFocus(window))
        return false;
    if (g_esc_frames > 0)
        return true;  // still down from the last press
    SendKey(VK_ESCAPE, true);
    g_esc_frames = 2;
    return true;
}
