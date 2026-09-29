// thief2vr.dll entry points. Loaded by the d3d9.dll proxy on the game's first
// Direct3DCreate9 call.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <MinHook.h>

#include "config/settings.h"
#include "engine/build_id.h"
#include "engine/engine.h"
#include "log.h"
#include "render/d3d9_hooks.h"
#include "render/stereo.h"
#include "version.h"
#include "vr.h"

namespace {

bool g_initialized;

void Initialize()
{
    if (g_initialized)
        return;
    g_initialized = true;
    LogInit();
    Log("Thief2VR " THIEF2VR_VERSION " starting (built " __DATE__ " " __TIME__ ")");
    Log("Game exe SHA-256: %s", ExeSha256().c_str());
    MH_STATUS st = MH_Initialize();
    Log("MinHook init: %s", MH_StatusToString(st));
    LoadSettings();
    if (!ResolveEngine())
        return;
    InstallStereoHook();
    // Start OpenXR now rather than on the first frame (avoids a hitch later).
    VrInitEarly();
}

} // namespace

// Asked by the proxy before the game's Direct3D object is created: whether to
// create it with Direct3DCreate9Ex, so eye textures can be shared with D3D11.
extern "C" __declspec(dllexport) int __cdecl T2VR_UseD3D9Ex()
{
    Initialize();
    return Config().d3d9ex ? 1 : 0;
}

// __cdecl so the x86 export name is undecorated (the proxy looks it up by name).
extern "C" __declspec(dllexport) void __cdecl T2VR_OnDirect3DCreated(IDirect3D9* d3d)
{
    Initialize();
    Log("Direct3DCreate9 -> %p", d3d);
    InstallD3D9Hooks(d3d);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(inst);
    return TRUE;
}
