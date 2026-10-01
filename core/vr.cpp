#include "vr.h"

#include "config/settings.h"
#include "engine/engine.h"
#include "input/screen_pointer.h"
#include "input/vr_controls.h"
#include "log.h"
#include "ui/vr_menu.h"
#include "ui/wrist_hud.h"

#include <cstdio>
#include "xr/xr_input.h"
#include "render/frame_transfer.h"
#include "render/stereo.h"
#include "xr/xr_pose.h"

namespace {

bool g_xr_tried;
bool g_frame_begun;
XrFrame g_frame;
bool g_scene_this_frame;

// Timing log (averaged, written every kStatFrames frames with a 3D scene).
constexpr int kStatFrames = 600;
struct Stats {
    int frames = 0;
    double render = 0, eye_upload = 0, eye_capture = 0, hud = 0, wait = 0, end = 0, desktop = 0, frame = 0,
           last_present = 0;
} g_stats;
char g_timing_text[256] = "Timing appears after about 10 seconds in a mission.";

// The game's window (the device's focus window), for the screen pointer.
HWND GameWindow(IDirect3DDevice9* device)
{
    D3DDEVICE_CREATION_PARAMETERS cp{};
    return SUCCEEDED(device->GetCreationParameters(&cp)) ? cp.hFocusWindow : nullptr;
}

void EnsureSwapchain(XrSwapchainD3D11& sc, UINT w, UINT h)
{
    if (sc.width() != w || sc.height() != h) {
        sc.Destroy();
        sc.Create(Xr().session(), Xr().color_format(), w, h);
    }
}

// The game's back buffer shown on a quad: the whole screen in menus, or only the
// overlays (HUD) in game. Each Update uploads last frame's capture and queues
// this frame's (see FrameTransfer).
struct FlatLayer {
    FrameTransfer transfer;
    XrSwapchainD3D11 swapchain;
    bool has_image = false;
    IDirect3DSurface9* scaled = nullptr;  // GPU-downscaled copy when capture_scale < 1

    // capture_scale < 1 captures a smaller, GPU-filtered copy (cheaper readback).
    // wrist (the HUD): its elements are cut into the wrist atlas (wrist_hud), and
    // what was found kept in wrist_cuts.
    bool Update(IDirect3DDevice9* device, bool should_render, bool alpha_from_color, float capture_scale = 1.0f,
                XrSwapchainD3D11* wrist = nullptr, TransferCut* wrist_cuts = nullptr)
    {
        IDirect3DSurface9* back = nullptr;
        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
            return false;
        D3DSURFACE_DESC desc;
        back->GetDesc(&desc);
        UINT w = desc.Width, h = desc.Height;
        IDirect3DSurface9* source = back;
        if (capture_scale < 1.0f) {
            w = max(2u, (UINT)(desc.Width * capture_scale));
            h = max(2u, (UINT)(desc.Height * capture_scale));
            D3DSURFACE_DESC sd{};
            if (scaled)
                scaled->GetDesc(&sd);
            if (!scaled || sd.Width != w || sd.Height != h || sd.Format != desc.Format) {
                ReleaseScaled();
                device->CreateRenderTarget(w, h, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &scaled, nullptr);
            }
            if (!scaled || FAILED(device->StretchRect(back, nullptr, scaled, nullptr, D3DTEXF_LINEAR))) {
                back->Release();
                return false;
            }
            source = scaled;
        }

        UINT old_w = swapchain.width(), old_h = swapchain.height();
        EnsureSwapchain(swapchain, w, h);
        if (swapchain.width() != old_w || swapchain.height() != old_h)
            has_image = false;

        if (should_render && swapchain.IsValid() && transfer.width() == w && transfer.height() == h) {
            if (ID3D11Texture2D* image = swapchain.Acquire()) {
                TransferCut cuts[wrist_hud::kCuts];
                ID3D11Texture2D* atlas = nullptr;
                if (wrist && wrist_cuts && wrist_hud::PrepareCuts((int)w, (int)h, cuts)) {
                    EnsureSwapchain(*wrist, wrist_hud::kAtlasWidth, wrist_hud::kAtlasHeight);
                    if (wrist->IsValid())
                        atlas = wrist->Acquire();
                }
                if (transfer.Upload(Xr().device(), Xr().context(), image, Xr().color_format_is_rgba(),
                                    alpha_from_color, atlas ? cuts : nullptr, atlas ? wrist_hud::kCuts : 0, atlas)) {
                    has_image = true;
                    for (int i = 0; wrist_cuts && i < wrist_hud::kCuts; ++i)
                        if (atlas)
                            wrist_cuts[i] = cuts[i];
                        else
                            wrist_cuts[i].found = false;
                }
                if (atlas)
                    wrist->Release();
                swapchain.Release();
            }
        }
        transfer.Capture(device, source);
        back->Release();
        return has_image;
    }

    void ReleaseScaled()
    {
        if (scaled)
            scaled->Release();
        scaled = nullptr;
    }

    void OnDeviceLost()
    {
        transfer.OnDeviceLost();
        ReleaseScaled();
    }

    void Fill(XrCompositionLayerQuad& quad, XrSpace space, const XrPosef& pose, float width) const
    {
        quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        quad.space = space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = swapchain.handle();
        quad.subImage.imageRect = {{0, 0}, {(int32_t)swapchain.width(), (int32_t)swapchain.height()}};
        quad.pose = pose;
        quad.size = {width, width * swapchain.height() / swapchain.width()};
    }
};

FlatLayer g_screen;  // menus, or in game with stereo off
// The screen is world-locked where you look when it appears (re-placed on
// recenter or when its distance setting changes).
XrPosef g_screen_pose;
bool g_screen_placed;
float g_screen_placed_distance;
// Right trigger ignored by the game controls until released (after clicking
// "Continue" on a menu, so the click doesn't also swing the weapon).
bool g_suppress_trigger;
FlatLayer g_hud;     // overlays only, drawn over black by the game
// The HUD elements moved onto the wrists (wrist_hud): their atlas, and what the
// last HUD upload found.
XrSwapchainD3D11 g_wrist_atlas;
TransferCut g_wrist_cuts[wrist_hud::kCuts];

// Stereo eyes.
// Shared path (D3D9Ex): the eye textures are opened in D3D11 and copied into
// the swapchains on the GPU at Present, in the same frame they were rendered.
// CPU path (fallback): read back and uploaded one frame later (FrameTransfer).
XrSwapchainD3D11 g_eye_swapchain[2];
bool g_eye_has_image;          // swapchains hold an image for g_submit_pose/fov
XrPosef g_submit_pose[2];
XrFovf g_submit_fov;
IDirect3DSurface9* g_mirror_eye;  // copied to the window after the HUD capture

ID3D11Texture2D* g_shared_eye[2];  // D3D11 views of the D3D9 eye textures
unsigned g_shared_generation;
IDirect3DQuery9* g_eyes_done;      // signalled when D3D9 finished rendering the eyes
bool g_shared_pending;             // eyes rendered this frame, copy at Present
XrPosef g_frame_pose[2];
XrFovf g_frame_fov;

FrameTransfer g_eye_transfer[2];
bool g_eye_pending;
XrPosef g_pending_pose[2];
XrFovf g_pending_fov;

void ReleaseShared()
{
    for (auto& t : g_shared_eye) {
        if (t)
            t->Release();
        t = nullptr;
    }
    if (g_eyes_done)
        g_eyes_done->Release();
    g_eyes_done = nullptr;
    g_shared_pending = false;
}

// Opens the D3D9 eye textures in D3D11. Returns false if sharing isn't possible
// (the CPU path is used instead).
bool EnsureShared(IDirect3DDevice9* device, const EyeTargets& eyes)
{
    if (!eyes.shared[0] || !eyes.shared[1] || Xr().color_format_is_rgba())
        return false;
    if (g_shared_eye[0] && g_shared_generation == eyes.generation)
        return true;
    ReleaseShared();
    for (int eye = 0; eye < 2; ++eye) {
        HRESULT hr = Xr().device()->OpenSharedResource(eyes.shared[eye], __uuidof(ID3D11Texture2D),
                                                       reinterpret_cast<void**>(&g_shared_eye[eye]));
        if (FAILED(hr)) {
            Log("VR: OpenSharedResource failed 0x%08x, using the CPU copy path", hr);
            ReleaseShared();
            return false;
        }
    }
    if (FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &g_eyes_done))) {
        ReleaseShared();
        return false;
    }
    g_shared_generation = eyes.generation;
    Log("VR: eye textures shared with D3D11 (zero-copy)");
    return true;
}

// Copies the shared eye textures into the swapchains once D3D9 has finished them.
bool SubmitSharedEyes()
{
    double t0 = VrNowMs();
    while (g_eyes_done->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
        if (VrNowMs() - t0 > 50.0) {
            Log("VR: timed out waiting for the eye render");
            break;
        }
        YieldProcessor();
    }
    g_stats.eye_capture += VrNowMs() - t0;  // time waiting on the GPU
    double t1 = VrNowMs();
    bool ok = true;
    for (int eye = 0; eye < 2; ++eye) {
        ID3D11Texture2D* image = g_eye_swapchain[eye].Acquire();
        if (!image) {
            ok = false;
            continue;
        }
        Xr().context()->CopyResource(image, g_shared_eye[eye]);
        g_eye_swapchain[eye].Release();
    }
    g_stats.eye_upload += VrNowMs() - t1;
    return ok;
}

// CPU path: upload last frame's readback, then queue this frame's.
void CpuTransferEyes(IDirect3DDevice9* device, const EyeTargets& eyes, const XrPosef poses[2], const XrFovf& fov)
{
    double t0 = VrNowMs();
    if (g_eye_pending) {
        bool ok = true;
        for (int eye = 0; eye < 2; ++eye) {
            ID3D11Texture2D* image = g_eye_swapchain[eye].Acquire();
            if (!image) {
                ok = false;
                continue;
            }
            ok &= g_eye_transfer[eye].width() == eyes.width &&
                  g_eye_transfer[eye].Upload(Xr().device(), Xr().context(), image, Xr().color_format_is_rgba());
            g_eye_swapchain[eye].Release();
        }
        if (ok) {
            g_submit_pose[0] = g_pending_pose[0];
            g_submit_pose[1] = g_pending_pose[1];
            g_submit_fov = g_pending_fov;
            g_eye_has_image = true;
        }
    }
    double t1 = VrNowMs();
    g_eye_pending = g_eye_transfer[0].Capture(device, eyes.color[0]) && g_eye_transfer[1].Capture(device, eyes.color[1]);
    g_pending_pose[0] = poses[0];
    g_pending_pose[1] = poses[1];
    g_pending_fov = fov;
    g_stats.eye_upload += t1 - t0;
    g_stats.eye_capture += VrNowMs() - t1;
}

// Copies the centre of the right eye into the window at the window's aspect ratio.
void MirrorToWindow(IDirect3DDevice9* device)
{
    IDirect3DSurface9* back = nullptr;
    if (!g_mirror_eye || FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
        return;
    D3DSURFACE_DESC bd, ed;
    back->GetDesc(&bd);
    g_mirror_eye->GetDesc(&ed);
    float window_aspect = (float)bd.Width / bd.Height;
    RECT src{0, 0, (LONG)ed.Width, (LONG)ed.Height};
    if ((float)ed.Width / ed.Height > window_aspect) {
        LONG w = (LONG)(ed.Height * window_aspect);
        src.left = ((LONG)ed.Width - w) / 2;
        src.right = src.left + w;
    } else {
        LONG h = (LONG)(ed.Width / window_aspect);
        src.top = ((LONG)ed.Height - h) / 2;
        src.bottom = src.top + h;
    }
    device->StretchRect(g_mirror_eye, &src, back, nullptr, D3DTEXF_LINEAR);
    back->Release();
}

bool EnsureXr()
{
    if (!g_xr_tried) {
        g_xr_tried = true;
        if (!Xr().Init())
            Log("VR disabled: OpenXR init failed, the game continues flat");
        else if (!XrControls().Init())
            Log("VR controllers unavailable; keyboard and mouse still work");
    }
    return Xr().IsInitialized();
}

bool EnsureFrame()
{
    if (!g_frame_begun) {
        double t0 = VrNowMs();
        g_frame_begun = Xr().BeginFrame(g_frame);  // blocks in xrWaitFrame until the headset wants a frame
        g_stats.wait += VrNowMs() - t0;
    }
    return g_frame_begun;
}

} // namespace

bool VrInitEarly()
{
    return EnsureXr();
}

const char* VrTimingSummary()
{
    return g_timing_text;
}

const XrFrame* VrBeginFrameForRender()
{
    if (!EnsureXr() || !EnsureFrame() || !g_frame.should_render)
        return nullptr;
    return &g_frame;
}

double VrNowMs()
{
    static LARGE_INTEGER freq;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart * 1000.0 / freq.QuadPart;
}

void VrOnEyesRendered(IDirect3DDevice9* device, const EyeTargets& eyes, const XrPosef poses[2], const XrFovf& fov,
                      double render_ms)
{
    g_scene_this_frame = true;
    g_stats.render += render_ms;
    g_mirror_eye = eyes.color[1];
    for (auto& sc : g_eye_swapchain) {
        UINT old_w = sc.width(), old_h = sc.height();
        EnsureSwapchain(sc, eyes.width, eyes.height);
        if (sc.width() != old_w || sc.height() != old_h)
            g_eye_has_image = false;
    }
    if (!g_eye_swapchain[0].IsValid() || !g_eye_swapchain[1].IsValid())
        return;

    if (EnsureShared(device, eyes)) {
        g_eyes_done->Issue(D3DISSUE_END);
        g_shared_pending = true;
        g_frame_pose[0] = poses[0];
        g_frame_pose[1] = poses[1];
        g_frame_fov = fov;
    } else {
        CpuTransferEyes(device, eyes, poses, fov);
    }
}

void VrRecenter()
{
    StereoRequestRecenter();
    g_screen_placed = false;
}

void VrRecordDesktopPresent(double ms)
{
    g_stats.desktop += ms;
}

bool VrOnPresent(IDirect3DDevice9* device)
{
    static bool f8_was_down;
    bool f8_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (f8_down && !f8_was_down)
        VrRecenter();
    f8_was_down = f8_down;

    EngineTraceTick();

    static bool f12_was_down;
    bool f12_down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (f12_down && !f12_was_down) {
        StereoRequestArmDump();
        EngineLogHudLayout();
        g_hud.transfer.RequestDump("thief2vr_hud.bmp");
        Log("HudDump: HUD image (capture scale %.2f) to thief2vr_hud.bmp", Config().hud_capture_scale);
    }
    f12_was_down = f12_down;

    static bool f7_was_down;
    bool f7_down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7_down && !f7_was_down) {
        Config().desktop_mirror = !Config().desktop_mirror;
        Log("Desktop mirror %s (F7)", Config().desktop_mirror ? "on" : "off");
    }
    f7_was_down = f7_down;

    if (!EnsureXr() || !EnsureFrame()) {
        g_scene_this_frame = false;
        return true;
    }

    const Settings& s = Config();
    const bool in_scene = g_scene_this_frame;

    // Refresh rate (Performance tab), when the runtime lets apps choose it.
    static float applied_refresh = -1;
    if (s.refresh_rate != applied_refresh && !Xr().refresh_rates().empty()) {
        applied_refresh = s.refresh_rate;
        Xr().RequestRefreshRate(s.refresh_rate);
    }

    static double last_controls = VrNowMs();
    double now_ms = VrNowMs();

    // The desktop window: in missions it's a mirror of the right eye, updated at
    // mirror_fps (the game's Present costs several ms, so it doesn't run every
    // frame); in menus it gets every frame (it's where the mouse works).
    static double last_mirror_ms;
    bool present_to_desktop = !in_scene;
    if (in_scene && s.desktop_mirror && now_ms - last_mirror_ms >= 1000.0 / s.mirror_fps - 1.0) {
        present_to_desktop = true;
        last_mirror_ms = now_ms;
    }
    double dt = (now_ms - last_controls) / 1000.0;
    last_controls = now_ms;

    XrControllerState controllers;
    XrControls().Update(g_frame.display_time, controllers);

    // Left menu button: tap = the game's menu (Esc), hold = VR settings menu. F10 also toggles it.
    static double menu_held_ms = -1;
    static bool f10_was_down;
    bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10_down && !f10_was_down)
        MenuToggle(g_frame);
    f10_was_down = f10_down;
    if (controllers.menu) {
        if (menu_held_ms == -1)
            menu_held_ms = 0;
        else if (menu_held_ms >= 0 && (menu_held_ms += dt * 1000.0) >= kMenuHoldSeconds * 1000.0) {
            MenuToggle(g_frame);
            menu_held_ms = -2;  // fired; wait for release
        }
    } else {
        if (menu_held_ms >= 0 && !MenuIsOpen()) {
            // In a mission: the game's menu command. On a 2D screen (commands may
            // not run there): Esc, which backs out of menus.
            if (in_scene)
                EngineCommand("sim_menu");
            else
                ScreenPointerSendEsc(GameWindow(device));
        }
        menu_held_ms = -1;
    }

    XrControllerState game_controls = controllers;
    if (g_suppress_trigger) {
        if (controllers.trigger[1] < 0.2f)
            g_suppress_trigger = false;
        else
            game_controls.trigger[1] = 0;
    }
    ControlsUpdate(game_controls, in_scene && !MenuIsOpen(), dt);

    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad, menu_quad, dot_quad;
    XrCompositionLayerQuad wrist_quads[wrist_hud::kCuts];
    const XrCompositionLayerBaseHeader* layers[4 + wrist_hud::kCuts];
    bool pointer_used = false;
    uint32_t layer_count = 0;

    if (g_scene_this_frame) {
        if (g_shared_pending) {
            g_shared_pending = false;
            if (SubmitSharedEyes()) {
                g_submit_pose[0] = g_frame_pose[0];
                g_submit_pose[1] = g_frame_pose[1];
                g_submit_fov = g_frame_fov;
                g_eye_has_image = true;
            }
        }
        if (g_eye_has_image) {
            for (int eye = 0; eye < 2; ++eye) {
                views[eye].pose = g_submit_pose[eye];
                views[eye].fov = g_submit_fov;
                views[eye].subImage.swapchain = g_eye_swapchain[eye].handle();
                views[eye].subImage.imageRect = {
                    {0, 0}, {(int32_t)g_eye_swapchain[eye].width(), (int32_t)g_eye_swapchain[eye].height()}};
            }
            projection.space = Xr().local_space();
            projection.viewCount = 2;
            projection.views = views;
            layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        }
        if (s.hud_enabled) {
            double t0 = VrNowMs();
            bool hud_ok = g_hud.Update(device, g_frame.should_render, true, s.hud_capture_scale, &g_wrist_atlas,
                                       g_wrist_cuts);
            g_stats.hud += VrNowMs() - t0;
            if (hud_ok) {
                XrPosef hud_pose{{0, 0, 0, 1}, {0.0f, s.hud_vertical_offset, -s.hud_distance}};
                g_hud.Fill(quad, Xr().view_space(), hud_pose, s.hud_width);
                quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                // The wrists' HUD elements, while looked at.
                XrPosef head = g_frame.views[0].pose;
                head.position = {(g_frame.views[0].pose.position.x + g_frame.views[1].pose.position.x) * 0.5f,
                                 (g_frame.views[0].pose.position.y + g_frame.views[1].pose.position.y) * 0.5f,
                                 (g_frame.views[0].pose.position.z + g_frame.views[1].pose.position.z) * 0.5f};
                int wrist_count = wrist_hud::Layers(g_wrist_cuts, g_wrist_atlas.handle(), Xr().local_space(),
                                                    g_frame.display_time, head, wrist_quads);
                for (int i = 0; i < wrist_count; ++i)
                    layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&wrist_quads[i]);
            }
        }
        // The window holds only the HUD (on black): show the right eye there instead.
        if (present_to_desktop)
            MirrorToWindow(device);
    } else if (g_screen.Update(device, g_frame.should_render, false)) {
        if (!g_screen_placed || g_screen_placed_distance != s.screen_distance) {
            g_screen_pose = PoseInFrontOfHead(g_frame, s.screen_distance, 0.0f);
            g_screen_placed = true;
            g_screen_placed_distance = s.screen_distance;
            Log("Screen: placed %.2f m in front of the view", s.screen_distance);
        }
        g_screen.Fill(quad, Xr().local_space(), g_screen_pose, s.screen_width);
        layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        if (!MenuIsOpen()) {
            pointer_used = true;
            g_suppress_trigger = true;
            if (ScreenPointerUpdate(controllers, g_screen_pose, quad.size.width, quad.size.height,
                                    GameWindow(device), dot_quad))
                layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&dot_quad);
        }
    }
    if (in_scene)
        g_screen_placed = false;  // the next 2D screen appears where you look then
    if (!pointer_used)
        ScreenPointerIdle();
    // A new mission (new game, loaded save, next mission): remind how to open the VR menu.
    static int last_player;
    if (in_scene) {
        int player = EnginePlayerObject();
        if (player && player != last_player) {
            last_player = player;
            if (s.show_menu_hint) {
                char text[128];
                snprintf(text, sizeof(text), "Hold the left menu button for %.1f seconds to open the VR menu",
                         kMenuHoldSeconds);
                MenuShowToast(text, 6.0);
            }
        }
    }
    if (MenuUpdate(controllers, dt, menu_quad) || MenuToastUpdate(dt, menu_quad))
        layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menu_quad);

    double end_start = VrNowMs();
    Xr().EndFrame(g_frame, layers, layer_count);
    g_frame_begun = false;

    double now = VrNowMs();
    if (in_scene) {
        g_stats.end += now - end_start;
        if (g_stats.last_present > 0)
            g_stats.frame += now - g_stats.last_present;
        if (++g_stats.frames == kStatFrames) {
            double n = kStatFrames;
            double f = g_stats.frame / n;
            double measured = (g_stats.render + g_stats.eye_upload + g_stats.eye_capture + g_stats.hud +
                               g_stats.wait + g_stats.end + g_stats.desktop) / n;
            double gpu = StereoTakeGpuMs();
            Log("Timing (ms/frame avg): frame %.2f (%.1f fps) | xr wait %.2f | eye render %.2f (GPU %.2f) "
                "| eye upload %.2f | eye capture %.2f | hud %.2f | xr end %.2f | desktop present %.2f "
                "| game+other %.2f",
                f, 1000.0 / f, g_stats.wait / n, g_stats.render / n, gpu, g_stats.eye_upload / n,
                g_stats.eye_capture / n, g_stats.hud / n, g_stats.end / n, g_stats.desktop / n, f - measured);
            snprintf(g_timing_text, sizeof(g_timing_text),
                     "%.1f fps (%.2f ms/frame)\nEye render: CPU %.2f ms, GPU %.2f ms; HUD %.2f ms\n"
                     "Spare time (waiting for the headset) %.2f ms, desktop window %.2f ms",
                     1000.0 / f, f, g_stats.render / n, gpu, g_stats.hud / n, g_stats.wait / n,
                     g_stats.desktop / n);
            g_stats = Stats{};
        }
    }
    g_stats.last_present = now;
    g_scene_this_frame = false;
    return present_to_desktop;
}

void VrOnDeviceReset()
{
    g_screen.OnDeviceLost();
    g_hud.OnDeviceLost();
    for (auto& t : g_eye_transfer)
        t.OnDeviceLost();
    g_mirror_eye = nullptr;
    g_eye_pending = false;
    ReleaseShared();
    StereoOnDeviceReset();
}
