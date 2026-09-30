#include "vr_menu.h"

#include "../config/settings.h"
#include "../log.h"
#include "../render/stereo.h"
#include "../vr.h"
#include "../version.h"
#include "../vrmath.h"
#include "../xr/xr_pose.h"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>

#include <cfloat>
#include <cstring>
#include <vector>

namespace {

constexpr int kTexW = 1280;
constexpr int kTexH = 960;
constexpr float kWidthM = 0.8f;      // panel width in metres
constexpr float kDistanceM = 0.8f;   // in front of the head when opened
constexpr float kDropM = 0.1f;       // below eye height

bool g_ready;
bool g_failed;
bool g_open;
XrPosef g_pose;
ID3D11Texture2D* g_tex;
ID3D11RenderTargetView* g_rtv;
XrSwapchainD3D11 g_swapchain;
bool g_trigger_down;
bool g_b_was_down;
bool g_close_requested;

// Notification ("toast"): a small head-locked panel below the view.
constexpr int kToastW = 1024;
constexpr int kToastH = 160;
constexpr float kToastWidthM = 0.5f;
ID3D11Texture2D* g_toast_tex;
ID3D11RenderTargetView* g_toast_rtv;
XrSwapchainD3D11 g_toast_swapchain;
bool g_toast_ready, g_toast_failed;
char g_toast_text[256];
double g_toast_left;  // seconds until it's gone

bool Init()
{
    if (g_ready || g_failed)
        return g_ready;
    g_failed = true;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kTexW;
    td.Height = kTexH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    // Same byte layout as the swapchain's sRGB format, so a raw copy is correct.
    td.Format = Xr().color_format_is_rgba() ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(Xr().device()->CreateTexture2D(&td, nullptr, &g_tex)) ||
        FAILED(Xr().device()->CreateRenderTargetView(g_tex, nullptr, &g_rtv)) ||
        !g_swapchain.Create(Xr().session(), Xr().color_format(), kTexW, kTexH)) {
        Log("Menu: could not create its render target");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)kTexW, (float)kTexH);
    io.MouseDrawCursor = true;  // the pointer is drawn into the panel
    ImFontConfig font;
    font.SizePixels = 30.0f;
    io.Fonts->AddFontDefault(&font);
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(1.6f);
    style.WindowRounding = 12.0f;
    style.FrameRounding = 6.0f;
    style.GrabMinSize = 30.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;
    if (!ImGui_ImplDX11_Init(Xr().device(), Xr().context())) {
        Log("Menu: ImGui DX11 backend init failed");
        return false;
    }
    g_failed = false;
    g_ready = true;
    Log("Menu: ready");
    return true;
}

// Where the right controller's aim ray hits the panel, in pixels (false if it misses).
bool PointerOnPanel(const XrControllerState& c, ImVec2& out)
{
    float u, v;
    if (!c.pose_valid[1] || !RayHitQuad(c.aim_pose[1], g_pose, kWidthM, kWidthM * kTexH / kTexW, u, v))
        return false;
    out = ImVec2(u * kTexW, v * kTexH);
    return true;
}

void SettingWidget(const SettingInfo& s)
{
    ImGui::PushID(s.name);
    switch (s.type) {
    case SettingType::Bool:
        ImGui::Checkbox(s.label, static_cast<bool*>(s.value));
        break;
    case SettingType::Float:
        ImGui::SetNextItemWidth(520);
        ImGui::SliderFloat(s.label, static_cast<float*>(s.value), s.lo, s.hi, "%.2f");
        break;
    case SettingType::Int:
        ImGui::SetNextItemWidth(520);
        ImGui::SliderInt(s.label, static_cast<int*>(s.value), (int)s.lo, (int)s.hi);
        break;
    }
    ImGui::PopID();
}

// The refresh-rate choice (Performance tab): the rates the headset offers, or a
// hint when the runtime doesn't let apps choose.
void RefreshRateWidget()
{
    const std::vector<float>& rates = Xr().refresh_rates();
    if (rates.empty()) {
        ImGui::TextWrapped("Refresh rate: set it in your streaming app (e.g. Virtual Desktop's frame rate).");
        return;
    }
    float& chosen = Config().refresh_rate;
    char current[32];
    if (chosen > 0)
        snprintf(current, sizeof(current), "%.0f Hz", chosen);
    else
        snprintf(current, sizeof(current), "Headset default");
    ImGui::SetNextItemWidth(520);
    if (ImGui::BeginCombo("Refresh rate", current)) {
        if (ImGui::Selectable("Headset default", chosen <= 0))
            chosen = 0;
        for (float r : rates) {
            char label[32];
            snprintf(label, sizeof(label), "%.0f Hz", r);
            if (ImGui::Selectable(label, chosen == r))
                chosen = r;
        }
        ImGui::EndCombo();
    }
}

// The controls reference (the Controls tab).
void ControlsTab()
{
    struct Row {
        const char* input;
        const char* action;
    };
    static const Row rows[] = {
        {"Left stick", "Move (in the direction you look)"},
        {"Left stick click", "Toggle run"},
        {"Right stick left / right", "Snap turn (or smooth turn, Comfort tab)"},
        {"Right stick up", "Jump"},
        {"Right stick down", "Toggle crouch"},
        {"Right stick click", "Map"},
        {"Right trigger", "Use weapon: hold to draw the bow, release to fire"},
        {"Swing the right hand", "Attack with the sword / blackjack"},
        {"Right grip", "Frob: pick up, open, use the selected item"},
        {"Left trigger", "Block"},
        {"Left grip", "Hold crouch"},
        {"A", "Next weapon"},
        {"B", "Put the weapon away"},
        {"X / Y", "Next / previous item"},
        {"Hold Y", "Objectives"},
        {"Left menu button", "Tap: game menu. Hold: this VR menu"},
        {"Crouch / lean with your body", "Crouch / lean in the game"},
        {"Two-handed bow (Hands tab)", "Bow in the left hand; squeeze the right grip at the bow, pull back, let go"},
        {"Game menus, map, books", "Point with the right hand, trigger = click, B = back"},
        {"Keyboard", "F7 desktop mirror, F8 recenter, F10 this menu"},
    };
    if (ImGui::BeginTable("controls", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("input", ImGuiTableColumnFlags_WidthFixed, 400.0f);
        ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch);
        for (const Row& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextWrapped("%s", r.input);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", r.action);
        }
        ImGui::EndTable();
    }
}

// Settings that only apply to the game's own sword / blackjack arm, shown only
// when the hands are off (with them on, the weapon sits in the hand).
bool HiddenNow(const SettingInfo& s)
{
    static const char* const kArmOnly[] = {"grip_forward_ft", "grip_right_ft",    "grip_down_ft",
                                           "weapon_pitch_deg", "weapon_yaw_deg", "weapon_roll_deg"};
    if (!Config().show_hands)
        return false;
    for (const char* name : kArmOnly)
        if (strcmp(s.name, name) == 0)
            return true;
    return false;
}

void BuildUi()
{
    int count;
    const SettingInfo* settings = AllSettings(count);

    // Tabs in declaration order, with Performance last.
    std::vector<const char*> tabs;
    for (int i = 0; i < count; ++i) {
        bool known = false;
        for (const char* t : tabs)
            known |= strcmp(t, settings[i].tab) == 0;
        if (!known && strcmp(settings[i].tab, "Performance") != 0)
            tabs.push_back(settings[i].tab);
    }
    tabs.insert(tabs.begin(), "Controls");
    tabs.push_back("Performance");

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Thief2VR", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);
    ImGui::Text("Thief2VR " THIEF2VR_VERSION " settings");
    ImGui::SameLine(ImGui::GetWindowWidth() - 190);
    if (ImGui::Button("Close", ImVec2(160, 0)))
        g_close_requested = true;
    ImGui::Separator();

    if (ImGui::BeginTabBar("tabs")) {
        for (const char* tab : tabs) {
            if (!ImGui::BeginTabItem(tab))
                continue;
            bool controls = strcmp(tab, "Controls") == 0;
            ImGui::BeginChild("items", ImVec2(0, controls ? 0.0f : -80.0f));
            if (controls)
                ControlsTab();
            for (int i = 0; i < count; ++i) {
                if (strcmp(settings[i].tab, tab) != 0 || HiddenNow(settings[i]))
                    continue;
                if (strcmp(settings[i].name, "refresh_rate") == 0)
                    RefreshRateWidget();
                else
                    SettingWidget(settings[i]);
            }
            if (strcmp(tab, "Performance") == 0) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", VrTimingSummary());
            }
            ImGui::EndChild();
            if (!controls && ImGui::Button("Reset this tab to defaults"))
                ResetSettingsTab(tab);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::SameLine();
    if (ImGui::Button("Recenter view"))
        VrRecenter();
    ImGui::End();
}

void Close()
{
    g_open = false;
    SaveSettings();
    Log("Menu: closed, settings saved");
}

} // namespace

bool MenuIsOpen()
{
    return g_open;
}

void MenuToggle(const XrFrame& frame)
{
    if (g_open) {
        Close();
        return;
    }
    if (!Init())
        return;

    // World-locked, level, in front of where the head is looking.
    g_pose = PoseInFrontOfHead(frame, kDistanceM, kDropM);

    g_open = true;
    g_close_requested = false;
    g_trigger_down = false;
    g_b_was_down = true;  // don't let the press that opened it close it
    Log("Menu: opened");
}

bool MenuUpdate(const XrControllerState& c, double dt, XrCompositionLayerQuad& quad)
{
    if (!g_open || !g_ready)
        return false;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)kTexW, (float)kTexH);
    io.DeltaTime = (float)(dt > 0.001 ? dt : 0.001);
    ImVec2 pointer;
    if (c.active && PointerOnPanel(c, pointer))
        io.AddMousePosEvent(pointer.x, pointer.y);
    else
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    bool trigger = g_trigger_down ? c.trigger[1] > 0.4f : c.trigger[1] > 0.6f;
    if (trigger != g_trigger_down) {
        io.AddMouseButtonEvent(0, trigger);
        g_trigger_down = trigger;
        if (trigger)
            XrControls().Vibrate(1, 0.15f, 0.02f);
    }
    if (std::fabs(c.turn.y) > 0.2f)
        io.AddMouseWheelEvent(0, c.turn.y * (float)dt * 10.0f);
    if (c.b && !g_b_was_down)
        g_close_requested = true;
    g_b_was_down = c.b;

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    BuildUi();
    ImGui::Render();

    ID3D11DeviceContext* ctx = Xr().context();
    const float clear[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(g_rtv, clear);
    ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, (float)kTexW, (float)kTexH, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    if (ID3D11Texture2D* image = g_swapchain.Acquire()) {
        ctx->CopyResource(image, g_tex);
        g_swapchain.Release();
    }

    if (g_close_requested) {
        Close();
        return false;
    }

    quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = Xr().local_space();
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g_swapchain.handle();
    quad.subImage.imageRect = {{0, 0}, {kTexW, kTexH}};
    quad.pose = g_pose;
    quad.size = {kWidthM, kWidthM * kTexH / kTexW};
    return true;
}

void MenuShowToast(const char* text, double seconds)
{
    strncpy_s(g_toast_text, text, _TRUNCATE);
    g_toast_left = seconds;
}

bool MenuToastUpdate(double dt, XrCompositionLayerQuad& quad)
{
    if (g_toast_left <= 0)
        return false;
    g_toast_left -= dt;
    if (g_toast_left <= 0 || !Init())
        return false;
    if (!g_toast_ready && !g_toast_failed) {
        g_toast_failed = true;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kToastW;
        td.Height = kToastH;
        td.MipLevels = td.ArraySize = 1;
        td.Format = Xr().color_format_is_rgba() ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (SUCCEEDED(Xr().device()->CreateTexture2D(&td, nullptr, &g_toast_tex)) &&
            SUCCEEDED(Xr().device()->CreateRenderTargetView(g_toast_tex, nullptr, &g_toast_rtv)) &&
            g_toast_swapchain.Create(Xr().session(), Xr().color_format(), kToastW, kToastH)) {
            g_toast_failed = false;
            g_toast_ready = true;
        } else {
            Log("Menu: could not create the notification panel");
        }
    }
    if (!g_toast_ready)
        return false;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)kToastW, (float)kToastH);
    io.DeltaTime = (float)(dt > 0.001 ? dt : 0.001);
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    float alpha = (float)(g_toast_left < 1.0 ? g_toast_left : 1.0);  // fade out over the last second
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
    ImVec2 size = ImGui::CalcTextSize(g_toast_text, nullptr, false, kToastW - 60.0f);
    ImGui::SetCursorPos(ImVec2((kToastW - size.x) * 0.5f, (kToastH - size.y) * 0.5f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size.x);
    ImGui::TextUnformatted(g_toast_text);
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::Render();

    ID3D11DeviceContext* ctx = Xr().context();
    const float clear[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(g_toast_rtv, clear);
    ctx->OMSetRenderTargets(1, &g_toast_rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, (float)kToastW, (float)kToastH, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (ID3D11Texture2D* image = g_toast_swapchain.Acquire()) {
        ctx->CopyResource(image, g_toast_tex);
        g_toast_swapchain.Release();
    }

    quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = Xr().view_space();
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g_toast_swapchain.handle();
    quad.subImage.imageRect = {{0, 0}, {kToastW, kToastH}};
    quad.pose.orientation.w = 1.0f;
    quad.pose.position = {0.0f, -0.12f, -0.9f};
    quad.size = {kToastWidthM, kToastWidthM * kToastH / kToastW};
    return true;
}
