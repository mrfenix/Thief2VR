// d3d9.dll proxy: forwards every export to the system d3d9.dll and hands the
// created IDirect3D9 to thief2vr.dll so it can install its hooks.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

typedef IDirect3D9* (WINAPI* PFN_Direct3DCreate9)(UINT);
typedef HRESULT (WINAPI* PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);
typedef void (__cdecl* PFN_OnDirect3DCreated)(IDirect3D9*);
typedef int (__cdecl* PFN_UseD3D9Ex)();

static HMODULE g_self;
static HMODULE g_system_d3d9;
static HMODULE g_core;
static PFN_OnDirect3DCreated g_on_created;

static HMODULE SystemD3D9()
{
    if (!g_system_d3d9) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        lstrcpyA(path + n, "\\d3d9.dll");
        g_system_d3d9 = LoadLibraryA(path);
        if (!g_system_d3d9)
            MessageBoxA(nullptr, "Thief2VR: could not load the system d3d9.dll", "Thief2VR", MB_ICONERROR);
    }
    return g_system_d3d9;
}

static void LoadCore()
{
    if (g_core)
        return;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(g_self, path, MAX_PATH);
    while (n > 0 && path[n - 1] != '\\')
        --n;
    lstrcpyA(path + n, "thief2vr.dll");
    g_core = LoadLibraryA(path);
    if (g_core)
        g_on_created = (PFN_OnDirect3DCreated)GetProcAddress(g_core, "T2VR_OnDirect3DCreated");
}

static FARPROC Real(const char* name)
{
    HMODULE m = SystemD3D9();
    return m ? GetProcAddress(m, name) : nullptr;
}

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdk)
{
    LoadCore();

    // The mod may ask for a D3D9Ex object (a superset of IDirect3D9, so the game
    // can't tell), which lets its eye textures be shared with D3D11 on the GPU.
    IDirect3D9* d3d = nullptr;
    auto use_ex = g_core ? (PFN_UseD3D9Ex)GetProcAddress(g_core, "T2VR_UseD3D9Ex") : nullptr;
    auto create_ex = (PFN_Direct3DCreate9Ex)Real("Direct3DCreate9Ex");
    if (use_ex && use_ex() && create_ex) {
        IDirect3D9Ex* ex = nullptr;
        if (SUCCEEDED(create_ex(sdk, &ex)))
            d3d = ex;
    }
    if (!d3d) {
        auto fn = (PFN_Direct3DCreate9)Real("Direct3DCreate9");
        if (!fn)
            return nullptr;
        d3d = fn(sdk);
    }
    if (d3d && g_on_created)
        g_on_created(d3d);
    return d3d;
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    auto fn = (PFN_Direct3DCreate9Ex)Real("Direct3DCreate9Ex");
    if (!fn)
        return E_FAIL;
    HRESULT hr = fn(sdk, out);
    LoadCore();
    if (SUCCEEDED(hr) && out && *out && g_on_created)
        g_on_created(*out);
    return hr;
}

// Plain forwarders for the rest. Their argument lists don't matter to us, so each
// one is a naked jump to the real export (x86 only).
#define RESOLVER(name)                                                 \
    extern "C" FARPROC __stdcall Resolve_##name()                      \
    {                                                                  \
        if (!g_##name) g_##name = Real(#name);                         \
        return g_##name;                                               \
    }

#define D3D9_FORWARD(name)                                              \
    static FARPROC g_##name;                                           \
    RESOLVER(name)                                                     \
    extern "C" __declspec(naked) void Proxy_##name()                   \
    {                                                                  \
        __asm { call Resolve_##name }                                  \
        __asm { jmp eax }                                              \
    }

D3D9_FORWARD(Direct3DShaderValidatorCreate9)
D3D9_FORWARD(PSGPError)
D3D9_FORWARD(PSGPSampleTexture)
D3D9_FORWARD(D3DPERF_BeginEvent)
D3D9_FORWARD(D3DPERF_EndEvent)
D3D9_FORWARD(D3DPERF_GetStatus)
D3D9_FORWARD(D3DPERF_QueryRepeatFrame)
D3D9_FORWARD(D3DPERF_SetMarker)
D3D9_FORWARD(D3DPERF_SetOptions)
D3D9_FORWARD(D3DPERF_SetRegion)
D3D9_FORWARD(DebugSetLevel)
D3D9_FORWARD(DebugSetMute)
D3D9_FORWARD(Direct3D9EnableMaximizedWindowedModeShim)

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
