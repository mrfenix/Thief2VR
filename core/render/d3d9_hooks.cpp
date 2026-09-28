#include "d3d9_hooks.h"

#include "../log.h"
#include "../vr.h"
#include "device_hooks.h"
#include <MinHook.h>

namespace {

// vtable slots (d3d9.h declaration order, IUnknown included)
constexpr int kD3D9_CreateDevice = 16;
constexpr int kDevice_Reset = 16;
constexpr int kDevice_Present = 17;

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                                const RGNDATA*);

PFN_CreateDevice g_real_create_device;
PFN_Reset g_real_reset;
PFN_Present g_real_present;
IDirect3DDevice9* g_device;
bool g_device_is_ex;
unsigned g_frame;

void** VTable(void* com_object)
{
    return *reinterpret_cast<void***>(com_object);
}

void LogPresentParams(const char* what, const D3DPRESENT_PARAMETERS* pp)
{
    Log("%s: %ux%u fmt=%d buffers=%u msaa=%d windowed=%d swap=%d depth=%d(%d) flags=0x%x refresh=%u interval=0x%x",
        what, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount,
        pp->MultiSampleType, pp->Windowed, pp->SwapEffect, pp->EnableAutoDepthStencil,
        pp->AutoDepthStencilFormat, pp->Flags, pp->FullScreen_RefreshRateInHz, pp->PresentationInterval);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd,
                                      const RGNDATA* dirty)
{
    ++g_frame;
    if (g_frame == 1 || g_frame % 600 == 0)
        Log("Present: frame %u", g_frame);
    FrameTraceOnPresent();
    if (!VrOnPresent(dev))
        return D3D_OK;  // desktop mirror off: the headset gets the frame, the window doesn't
    double t0 = VrNowMs();
    HRESULT hr = g_real_present(dev, src, dst, wnd, dirty);
    VrRecordDesktopPresent(VrNowMs() - t0);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    LogPresentParams("Reset", pp);
    VrOnDeviceReset();
    HRESULT hr = g_real_reset(dev, pp);
    Log("Reset -> 0x%08x", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus,
                                           DWORD behavior, D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice9** out)
{
    LogPresentParams("CreateDevice", pp);
    Log("CreateDevice: adapter=%u type=%d behavior=0x%x hwnd=%p", adapter, type, behavior, focus);
    HRESULT hr = g_real_create_device(d3d, adapter, type, focus, behavior, pp, out);
    Log("CreateDevice -> 0x%08x device=%p", hr, (SUCCEEDED(hr) && out) ? *out : nullptr);
    if (FAILED(hr) || !out || !*out)
        return hr;

    g_device = *out;
    IDirect3DDevice9Ex* ex = nullptr;
    g_device_is_ex = SUCCEEDED(g_device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&ex)));
    if (ex)
        ex->Release();

    // Devices share one vtable per d3d9 implementation, so hook it only once
    // (before returning, so the resource-creation hooks see every resource).
    if (!g_real_present) {
        void** vt = VTable(g_device);
        MH_CreateHook(vt[kDevice_Present], &HookPresent, reinterpret_cast<void**>(&g_real_present));
        MH_CreateHook(vt[kDevice_Reset], &HookReset, reinterpret_cast<void**>(&g_real_reset));
        InstallDeviceHooks(g_device, g_device_is_ex);
        MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
        Log("Device is %s; hooks enabled: %s", g_device_is_ex ? "D3D9Ex" : "D3D9", MH_StatusToString(st));
    }
    return hr;
}

} // namespace

void InstallD3D9Hooks(IDirect3D9* d3d)
{
    if (g_real_create_device)
        return;
    void** vt = VTable(d3d);
    MH_STATUS st = MH_CreateHook(vt[kD3D9_CreateDevice], &HookCreateDevice,
                                 reinterpret_cast<void**>(&g_real_create_device));
    if (st == MH_OK)
        st = MH_EnableHook(MH_ALL_HOOKS);
    Log("IDirect3D9::CreateDevice hook: %s", MH_StatusToString(st));
}

IDirect3DDevice9* GameDevice()
{
    return g_device;
}

bool GameDeviceIsEx()
{
    return g_device_is_ex;
}
