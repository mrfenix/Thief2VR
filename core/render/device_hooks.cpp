#include "device_hooks.h"

#include "../log.h"
#include <MinHook.h>

#include <intrin.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// IDirect3DDevice9 vtable slots (d3d9.h declaration order, IUnknown included).
enum Slot {
    kCreateTexture = 23,
    kCreateVolumeTexture = 24,
    kCreateCubeTexture = 25,
    kCreateVertexBuffer = 26,
    kCreateIndexBuffer = 27,
    kStretchRect = 34,
    kSetRenderTarget = 37,
    kSetDepthStencilSurface = 39,
    kBeginScene = 41,
    kEndScene = 42,
    kClear = 43,
    kSetViewport = 47,
    kGetViewport = 48,
    kSetScissorRect = 75,
    kDrawPrimitive = 81,
    kDrawIndexedPrimitive = 82,
    kDrawPrimitiveUP = 83,
    kDrawIndexedPrimitiveUP = 84,
    kSetVertexDeclaration = 87,
    kSetFVF = 89,
};

// ---------------------------------------------------------------------------
// Scene redirect state

struct Redirect {
    bool active = false;
    bool scaling = false;               // current render target is the redirected canvas
    IDirect3DSurface9* canvas = nullptr;        // the game's back buffer (what the engine thinks it draws to)
    IDirect3DSurface9* canvas_depth = nullptr;
    IDirect3DSurface9* color = nullptr;         // our eye target
    IDirect3DSurface9* depth = nullptr;
    D3DVIEWPORT9 saved_viewport{};
    D3DVIEWPORT9 engine_viewport{};     // last viewport the engine set, in canvas pixels
    // Canvas -> eye mapping: uniform scale about the centres, so the engine's
    // image keeps its proportions; whatever falls outside the eye target is
    // clipped. eye = offset + canvas * scale (in pixel-edge coordinates).
    float scale = 1, ox = 0, oy = 0;
    LONG eye_w = 0, eye_h = 0;
} g_redirect;

bool g_position_rhw;  // current vertex format is pre-transformed (XYZRHW / POSITIONT)
int g_uv_offset = -1;       // byte offset of the first texture coordinates, or -1
int g_color_offset = -1;    // byte offset of the diffuse colour, or -1
DrawCapture g_capture;

// Passes the triangles of a pre-transformed draw to the capture callback.
void CaptureTriangles(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT prims, const void* data, UINT stride,
                      const void* idx, D3DFORMAT idx_fmt)
{
    if (!g_capture.callback || !g_position_rhw || !data ||
        (t != D3DPT_TRIANGLELIST && t != D3DPT_TRIANGLESTRIP && t != D3DPT_TRIANGLEFAN))
        return;
    IDirect3DBaseTexture9* texture = nullptr;
    d->GetTexture(0, &texture);
    for (UINT p = 0; p < prims; ++p) {
        CapturedVertex tri[3];
        for (UINT k = 0; k < 3; ++k) {
            UINT i = t == D3DPT_TRIANGLELIST ? p * 3 + k : t == D3DPT_TRIANGLESTRIP ? p + k : (k == 0 ? 0 : p + k);
            if (idx)
                i = idx_fmt == D3DFMT_INDEX32 ? static_cast<const uint32_t*>(idx)[i] : static_cast<const uint16_t*>(idx)[i];
            const unsigned char* v = static_cast<const unsigned char*>(data) + i * stride;
            const float* f = reinterpret_cast<const float*>(v);
            tri[k] = {f[0], f[1], f[2], f[3], 0, 0, 0xffffffffu};
            if (g_uv_offset >= 0) {
                tri[k].u = reinterpret_cast<const float*>(v + g_uv_offset)[0];
                tri[k].v = reinterpret_cast<const float*>(v + g_uv_offset)[1];
            }
            if (g_color_offset >= 0)
                tri[k].diffuse = *reinterpret_cast<const unsigned*>(v + g_color_offset);
        }
        g_capture.callback(tri, texture, g_capture.user);
    }
    if (texture)
        texture->Release();
}
void (*g_draw_probe)();  // one-shot, see SetDrawProbe

void RunDrawProbe()
{
    if (void (*probe)() = g_draw_probe) {
        g_draw_probe = nullptr;
        probe();
    }
}
std::vector<unsigned char> g_scratch;

// D3D9 pixel centres are at integer coordinates, so pixel edges are at -0.5.
inline float ScaleX(float x) { return g_redirect.ox + (x + 0.5f) * g_redirect.scale - 0.5f; }
inline float ScaleY(float y) { return g_redirect.oy + (y + 0.5f) * g_redirect.scale - 0.5f; }
inline LONG EdgeX(float x) { return (LONG)(g_redirect.ox + x * g_redirect.scale + 0.5f); }
inline LONG EdgeY(float y) { return (LONG)(g_redirect.oy + y * g_redirect.scale + 0.5f); }

// Maps a canvas rectangle to the eye target, clamped to its bounds.
RECT ScaleRect(const RECT& r)
{
    RECT s{EdgeX((float)r.left), EdgeY((float)r.top), EdgeX((float)r.right), EdgeY((float)r.bottom)};
    s.left = max(0L, min(s.left, g_redirect.eye_w));
    s.right = max(0L, min(s.right, g_redirect.eye_w));
    s.top = max(0L, min(s.top, g_redirect.eye_h));
    s.bottom = max(0L, min(s.bottom, g_redirect.eye_h));
    return s;
}

bool IsCanvas(IDirect3DSurface9* s)
{
    return s && (s == g_redirect.canvas || s == g_redirect.color);
}

// Copies count vertices and scales their screen-space x/y. Returns the copy.
const void* ScaledVertices(const void* data, UINT count, UINT stride)
{
    g_scratch.resize((size_t)count * stride);
    memcpy(g_scratch.data(), data, g_scratch.size());
    unsigned char* v = g_scratch.data();
    for (UINT i = 0; i < count; ++i, v += stride) {
        float* p = reinterpret_cast<float*>(v);
        p[0] = ScaleX(p[0]);
        p[1] = ScaleY(p[1]);
    }
    return g_scratch.data();
}

UINT VertexCount(D3DPRIMITIVETYPE type, UINT prims)
{
    switch (type) {
    case D3DPT_POINTLIST: return prims;
    case D3DPT_LINELIST: return prims * 2;
    case D3DPT_LINESTRIP: return prims + 1;
    case D3DPT_TRIANGLELIST: return prims * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return prims + 2;
    default: return 0;
    }
}

// ---------------------------------------------------------------------------
// Frame trace

bool g_trace_active;
bool g_trace_requested;
int g_trace_draws;
char g_last_line[512];
int g_repeat;
uintptr_t g_text_begin, g_text_end;

void FindExeText()
{
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            g_text_begin = base + sec->VirtualAddress;
            g_text_end = g_text_begin + sec->Misc.VirtualSize;
            return;
        }
    }
}

// True if the bytes before addr end in a CALL instruction.
bool FollowsCall(uintptr_t addr)
{
    if (addr < g_text_begin + 7 || addr >= g_text_end)
        return false;
    auto p = reinterpret_cast<const unsigned char*>(addr);
    if (p[-5] == 0xE8)
        return true;
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10)
        return true;
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)
        return true;
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)
        return true;
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10)
        return true;
    return false;
}

void Emit(const char* line)
{
    if (strcmp(line, g_last_line) == 0) {
        ++g_repeat;
        return;
    }
    if (g_repeat)
        Log("    (previous line x%d more)", g_repeat);
    g_repeat = 0;
    strcpy_s(g_last_line, line);
    Log("%s", line);
}

// The engine is built without frame pointers, so this scans the stack for
// values that follow a CALL instruction in the exe.
void Trace(void* stack_top, const char* fmt, ...)
{
    if (!g_trace_active)
        return;
    char line[512];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    auto sp = reinterpret_cast<const uintptr_t*>(stack_top);
    int found = 0;
    for (int i = 0; i < 1024 && found < 8 && len < (int)sizeof(line) - 12; ++i) {
        uintptr_t v;
        __try {
            v = sp[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (FollowsCall(v)) {
            len += snprintf(line + len, sizeof(line) - len, " %06x", (unsigned)v);
            ++found;
        }
    }
    Emit(line);
}

// ---------------------------------------------------------------------------
// Hooks

#define DECLARE_HOOK(ret, name, ...)                              \
    typedef ret(STDMETHODCALLTYPE* PFN_##name)(__VA_ARGS__);      \
    PFN_##name g_real_##name;

DECLARE_HOOK(HRESULT, CreateTexture, IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
             IDirect3DTexture9**, HANDLE*)
DECLARE_HOOK(HRESULT, CreateVolumeTexture, IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
             IDirect3DVolumeTexture9**, HANDLE*)
DECLARE_HOOK(HRESULT, CreateCubeTexture, IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
             IDirect3DCubeTexture9**, HANDLE*)
DECLARE_HOOK(HRESULT, CreateVertexBuffer, IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**,
             HANDLE*)
DECLARE_HOOK(HRESULT, CreateIndexBuffer, IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**,
             HANDLE*)
DECLARE_HOOK(HRESULT, StretchRect, IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*,
             const RECT*, D3DTEXTUREFILTERTYPE)
DECLARE_HOOK(HRESULT, SetRenderTarget, IDirect3DDevice9*, DWORD, IDirect3DSurface9*)
DECLARE_HOOK(HRESULT, SetDepthStencilSurface, IDirect3DDevice9*, IDirect3DSurface9*)
DECLARE_HOOK(HRESULT, BeginScene, IDirect3DDevice9*)
DECLARE_HOOK(HRESULT, EndScene, IDirect3DDevice9*)
DECLARE_HOOK(HRESULT, Clear, IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD)
DECLARE_HOOK(HRESULT, SetViewport, IDirect3DDevice9*, const D3DVIEWPORT9*)
DECLARE_HOOK(HRESULT, GetViewport, IDirect3DDevice9*, D3DVIEWPORT9*)
DECLARE_HOOK(HRESULT, SetScissorRect, IDirect3DDevice9*, const RECT*)
DECLARE_HOOK(HRESULT, DrawPrimitive, IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT)
DECLARE_HOOK(HRESULT, DrawIndexedPrimitive, IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)
DECLARE_HOOK(HRESULT, DrawPrimitiveUP, IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT)
DECLARE_HOOK(HRESULT, DrawIndexedPrimitiveUP, IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
             D3DFORMAT, const void*, UINT)
DECLARE_HOOK(HRESULT, SetVertexDeclaration, IDirect3DDevice9*, IDirect3DVertexDeclaration9*)
DECLARE_HOOK(HRESULT, SetFVF, IDirect3DDevice9*, DWORD)

// --- D3D9Ex: no MANAGED pool ---

HRESULT STDMETHODCALLTYPE HookCreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT levels, DWORD usage,
                                            D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared)
{
    if (pool == D3DPOOL_MANAGED) {
        HRESULT hr = g_real_CreateTexture(d, w, h, levels, usage | D3DUSAGE_DYNAMIC, fmt, D3DPOOL_DEFAULT, out, shared);
        if (SUCCEEDED(hr))
            return hr;
        static int logged;
        if (logged++ < 10)
            Log("D3D9Ex: managed texture %ux%u fmt %d usage 0x%x as dynamic failed 0x%08x", w, h, fmt, usage, hr);
        return g_real_CreateTexture(d, w, h, levels, usage, fmt, D3DPOOL_DEFAULT, out, shared);
    }
    return g_real_CreateTexture(d, w, h, levels, usage, fmt, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE HookCreateVolumeTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT depth, UINT levels,
                                                  DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                  IDirect3DVolumeTexture9** out, HANDLE* shared)
{
    if (pool == D3DPOOL_MANAGED) {
        pool = D3DPOOL_DEFAULT;
        usage |= D3DUSAGE_DYNAMIC;
    }
    return g_real_CreateVolumeTexture(d, w, h, depth, levels, usage, fmt, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE HookCreateCubeTexture(IDirect3DDevice9* d, UINT edge, UINT levels, DWORD usage,
                                                D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9** out,
                                                HANDLE* shared)
{
    if (pool == D3DPOOL_MANAGED) {
        pool = D3DPOOL_DEFAULT;
        usage |= D3DUSAGE_DYNAMIC;
    }
    return g_real_CreateCubeTexture(d, edge, levels, usage, fmt, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE HookCreateVertexBuffer(IDirect3DDevice9* d, UINT len, DWORD usage, DWORD fvf,
                                                 D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* shared)
{
    if (pool == D3DPOOL_MANAGED)
        pool = D3DPOOL_DEFAULT;
    return g_real_CreateVertexBuffer(d, len, usage, fvf, pool, out, shared);
}

HRESULT STDMETHODCALLTYPE HookCreateIndexBuffer(IDirect3DDevice9* d, UINT len, DWORD usage, D3DFORMAT fmt,
                                                D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* shared)
{
    if (pool == D3DPOOL_MANAGED)
        pool = D3DPOOL_DEFAULT;
    return g_real_CreateIndexBuffer(d, len, usage, fmt, pool, out, shared);
}

// --- redirect + trace ---

HRESULT STDMETHODCALLTYPE HookStretchRect(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT* sr,
                                          IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f)
{
    Trace(_AddressOfReturnAddress(), "StretchRect(%p -> %p)", src, dst);
    if (g_redirect.active) {
        RECT srs, drs;
        if (IsCanvas(src)) {
            src = g_redirect.color;
            if (sr) {
                srs = ScaleRect(*sr);
                sr = &srs;
            }
        }
        if (IsCanvas(dst)) {
            dst = g_redirect.color;
            if (dr) {
                drs = ScaleRect(*dr);
                dr = &drs;
            }
        }
    }
    return g_real_StretchRect(d, src, sr, dst, dr, f);
}

HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s)
{
    Trace(_AddressOfReturnAddress(), "SetRenderTarget(%u, %p)", i, s);
    if (g_redirect.active && i == 0) {
        g_redirect.scaling = IsCanvas(s);
        if (g_redirect.scaling)
            s = g_redirect.color;
    }
    return g_real_SetRenderTarget(d, i, s);
}

HRESULT STDMETHODCALLTYPE HookSetDepthStencilSurface(IDirect3DDevice9* d, IDirect3DSurface9* s)
{
    if (g_redirect.active && s && s == g_redirect.canvas_depth)
        s = g_redirect.depth;
    return g_real_SetDepthStencilSurface(d, s);
}

HRESULT STDMETHODCALLTYPE HookBeginScene(IDirect3DDevice9* d)
{
    Trace(_AddressOfReturnAddress(), "BeginScene");
    return g_real_BeginScene(d);
}

HRESULT STDMETHODCALLTYPE HookEndScene(IDirect3DDevice9* d)
{
    Trace(_AddressOfReturnAddress(), "EndScene (draws so far %d)", g_trace_draws);
    return g_real_EndScene(d);
}

HRESULT STDMETHODCALLTYPE HookClear(IDirect3DDevice9* d, DWORD n, const D3DRECT* rects, DWORD flags, D3DCOLOR c,
                                    float z, DWORD s)
{
    Trace(_AddressOfReturnAddress(), "Clear(%u rects, flags %x)", n, flags);
    if (g_capture.skip_draw)
        return D3D_OK;
    if (g_redirect.scaling && n && rects) {
        std::vector<D3DRECT> scaled(rects, rects + n);
        for (auto& r : scaled) {
            RECT e = ScaleRect({r.x1, r.y1, r.x2, r.y2});
            r = {e.left, e.top, e.right, e.bottom};
        }
        return g_real_Clear(d, n, scaled.data(), flags, c, z, s);
    }
    return g_real_Clear(d, n, rects, flags, c, z, s);
}

HRESULT STDMETHODCALLTYPE HookSetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* v)
{
    Trace(_AddressOfReturnAddress(), "SetViewport(%u,%u %ux%u)", v->X, v->Y, v->Width, v->Height);
    if (g_redirect.scaling) {
        g_redirect.engine_viewport = *v;
        RECT r = ScaleRect({(LONG)v->X, (LONG)v->Y, (LONG)(v->X + v->Width), (LONG)(v->Y + v->Height)});
        D3DVIEWPORT9 s = *v;
        s.X = r.left;
        s.Y = r.top;
        s.Width = max(1L, r.right - r.left);
        s.Height = max(1L, r.bottom - r.top);
        return g_real_SetViewport(d, &s);
    }
    return g_real_SetViewport(d, v);
}

HRESULT STDMETHODCALLTYPE HookGetViewport(IDirect3DDevice9* d, D3DVIEWPORT9* v)
{
    if (g_redirect.scaling && v) {  // report the canvas-sized viewport the engine set
        *v = g_redirect.engine_viewport;
        return D3D_OK;
    }
    return g_real_GetViewport(d, v);
}

HRESULT STDMETHODCALLTYPE HookSetScissorRect(IDirect3DDevice9* d, const RECT* r)
{
    if (g_redirect.scaling && r) {
        RECT s = ScaleRect(*r);
        return g_real_SetScissorRect(d, &s);
    }
    return g_real_SetScissorRect(d, r);
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT count)
{
    RunDrawProbe();
    ++g_trace_draws;
    Trace(_AddressOfReturnAddress(), "DrawPrimitive(type %d)", t);
    if (g_capture.skip_draw)
        return D3D_OK;
    if (g_redirect.scaling && g_position_rhw) {
        static int logged;
        if (logged++ < 5)
            Log("Redirect: pre-transformed vertex-buffer draw can't be scaled (drawn unscaled)");
    }
    return g_real_DrawPrimitive(d, t, start, count);
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base, UINT minv,
                                                   UINT numv, UINT start, UINT count)
{
    RunDrawProbe();
    ++g_trace_draws;
    Trace(_AddressOfReturnAddress(), "DrawIndexedPrimitive(type %d)", t);
    if (g_capture.skip_draw)
        return D3D_OK;
    if (g_redirect.scaling && g_position_rhw) {
        static int logged;
        if (logged++ < 5)
            Log("Redirect: pre-transformed indexed vertex-buffer draw can't be scaled (drawn unscaled)");
    }
    return g_real_DrawIndexedPrimitive(d, t, base, minv, numv, start, count);
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT count, const void* data,
                                              UINT stride)
{
    RunDrawProbe();
    ++g_trace_draws;
    Trace(_AddressOfReturnAddress(), "DrawPrimitiveUP(type %d stride %u)", t, stride);
    CaptureTriangles(d, t, count, data, stride, nullptr, D3DFMT_UNKNOWN);
    if (g_capture.skip_draw)
        return D3D_OK;
    if (g_redirect.scaling && g_position_rhw && data)
        data = ScaledVertices(data, VertexCount(t, count), stride);
    return g_real_DrawPrimitiveUP(d, t, count, data, stride);
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT minv, UINT numv,
                                                     UINT count, const void* idx, D3DFORMAT fmt, const void* data,
                                                     UINT stride)
{
    RunDrawProbe();
    ++g_trace_draws;
    Trace(_AddressOfReturnAddress(), "DrawIndexedPrimitiveUP(type %d stride %u)", t, stride);
    CaptureTriangles(d, t, count, data, stride, idx, fmt);
    if (g_capture.skip_draw)
        return D3D_OK;
    if (g_redirect.scaling && g_position_rhw && data)
        data = ScaledVertices(data, minv + numv, stride);
    return g_real_DrawIndexedPrimitiveUP(d, t, minv, numv, count, idx, fmt, data, stride);
}

HRESULT STDMETHODCALLTYPE HookSetVertexDeclaration(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* decl)
{
    g_position_rhw = false;
    g_uv_offset = g_color_offset = -1;
    if (decl) {
        D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH + 1];
        UINT n = MAXD3DDECLLENGTH + 1;
        if (SUCCEEDED(decl->GetDeclaration(elems, &n))) {
            for (UINT i = 0; i < n && elems[i].Stream != 0xff; ++i) {
                const D3DVERTEXELEMENT9& e = elems[i];
                if (e.Stream != 0)
                    continue;
                if (e.Usage == D3DDECLUSAGE_POSITIONT && e.Offset == 0)
                    g_position_rhw = true;
                else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == 0 && e.Type == D3DDECLTYPE_FLOAT2)
                    g_uv_offset = e.Offset;
                else if (e.Usage == D3DDECLUSAGE_COLOR && e.UsageIndex == 0 && e.Type == D3DDECLTYPE_D3DCOLOR)
                    g_color_offset = e.Offset;
            }
        }
    }
    return g_real_SetVertexDeclaration(d, decl);
}

HRESULT STDMETHODCALLTYPE HookSetFVF(IDirect3DDevice9* d, DWORD fvf)
{
    Trace(_AddressOfReturnAddress(), "SetFVF(0x%x)", fvf);
    g_position_rhw = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    int offset = 16;  // XYZRHW
    g_color_offset = (fvf & D3DFVF_DIFFUSE) ? offset : -1;
    offset += (fvf & D3DFVF_DIFFUSE) ? 4 : 0;
    offset += (fvf & D3DFVF_SPECULAR) ? 4 : 0;
    g_uv_offset = (fvf & D3DFVF_TEXCOUNT_MASK) ? offset : -1;
    return g_real_SetFVF(d, fvf);
}

template <typename Fn>
void Hook(void** vt, int slot, Fn hook, Fn* real)
{
    MH_STATUS st = MH_CreateHook(vt[slot], reinterpret_cast<void*>(hook), reinterpret_cast<void**>(real));
    if (st != MH_OK)
        Log("Device hook slot %d failed: %s", slot, MH_StatusToString(st));
}

} // namespace

void InstallDeviceHooks(IDirect3DDevice9* device, bool is_ex)
{
    FindExeText();
    void** vt = *reinterpret_cast<void***>(device);
    if (is_ex) {
        Hook(vt, kCreateTexture, &HookCreateTexture, &g_real_CreateTexture);
        Hook(vt, kCreateVolumeTexture, &HookCreateVolumeTexture, &g_real_CreateVolumeTexture);
        Hook(vt, kCreateCubeTexture, &HookCreateCubeTexture, &g_real_CreateCubeTexture);
        Hook(vt, kCreateVertexBuffer, &HookCreateVertexBuffer, &g_real_CreateVertexBuffer);
        Hook(vt, kCreateIndexBuffer, &HookCreateIndexBuffer, &g_real_CreateIndexBuffer);
    }
    Hook(vt, kStretchRect, &HookStretchRect, &g_real_StretchRect);
    Hook(vt, kSetRenderTarget, &HookSetRenderTarget, &g_real_SetRenderTarget);
    Hook(vt, kSetDepthStencilSurface, &HookSetDepthStencilSurface, &g_real_SetDepthStencilSurface);
    Hook(vt, kBeginScene, &HookBeginScene, &g_real_BeginScene);
    Hook(vt, kEndScene, &HookEndScene, &g_real_EndScene);
    Hook(vt, kClear, &HookClear, &g_real_Clear);
    Hook(vt, kSetViewport, &HookSetViewport, &g_real_SetViewport);
    Hook(vt, kGetViewport, &HookGetViewport, &g_real_GetViewport);
    Hook(vt, kSetScissorRect, &HookSetScissorRect, &g_real_SetScissorRect);
    Hook(vt, kDrawPrimitive, &HookDrawPrimitive, &g_real_DrawPrimitive);
    Hook(vt, kDrawIndexedPrimitive, &HookDrawIndexedPrimitive, &g_real_DrawIndexedPrimitive);
    Hook(vt, kDrawPrimitiveUP, &HookDrawPrimitiveUP, &g_real_DrawPrimitiveUP);
    Hook(vt, kDrawIndexedPrimitiveUP, &HookDrawIndexedPrimitiveUP, &g_real_DrawIndexedPrimitiveUP);
    Hook(vt, kSetVertexDeclaration, &HookSetVertexDeclaration, &g_real_SetVertexDeclaration);
    Hook(vt, kSetFVF, &HookSetFVF, &g_real_SetFVF);
    Log("Device hooks installed (D3D9Ex resource fixes %s). F9 = frame trace", is_ex ? "on" : "off");
}

float RedirectScale(UINT canvas_w, UINT canvas_h, UINT eye_w, UINT eye_h)
{
    return max((float)eye_w / canvas_w, (float)eye_h / canvas_h);
}

void BeginSceneRedirect(IDirect3DDevice9* device, IDirect3DSurface9* color, IDirect3DSurface9* depth)
{
    Redirect& r = g_redirect;
    device->GetRenderTarget(0, &r.canvas);
    device->GetDepthStencilSurface(&r.canvas_depth);
    g_real_GetViewport(device, &r.saved_viewport);

    D3DSURFACE_DESC canvas_desc, eye_desc;
    r.canvas->GetDesc(&canvas_desc);
    color->GetDesc(&eye_desc);
    r.color = color;
    r.depth = depth;
    r.eye_w = (LONG)eye_desc.Width;
    r.eye_h = (LONG)eye_desc.Height;
    r.scale = RedirectScale(canvas_desc.Width, canvas_desc.Height, eye_desc.Width, eye_desc.Height);
    r.ox = (eye_desc.Width - canvas_desc.Width * r.scale) * 0.5f;
    r.oy = (eye_desc.Height - canvas_desc.Height * r.scale) * 0.5f;
    r.engine_viewport = r.saved_viewport;
    r.active = true;
    r.scaling = true;

    g_real_SetRenderTarget(device, 0, color);
    g_real_SetDepthStencilSurface(device, depth);
    D3DVIEWPORT9 vp = r.saved_viewport;
    HookSetViewport(device, &vp);  // scaled copy of the game's viewport
}

void EndSceneRedirect(IDirect3DDevice9* device)
{
    Redirect& r = g_redirect;
    if (!r.active)
        return;
    r.active = false;
    r.scaling = false;
    g_real_SetRenderTarget(device, 0, r.canvas);
    g_real_SetDepthStencilSurface(device, r.canvas_depth);
    g_real_SetViewport(device, &r.saved_viewport);
    if (r.canvas)
        r.canvas->Release();
    if (r.canvas_depth)
        r.canvas_depth->Release();
    r.canvas = r.canvas_depth = nullptr;
}

void AppendExeCallers(char* line, size_t size, const void* stack_top, int max_frames)
{
    if (!g_text_begin)
        FindExeText();
    size_t len = strlen(line);
    auto sp = reinterpret_cast<const uintptr_t*>(stack_top);
    int found = 0;
    for (int i = 0; i < 1024 && found < max_frames && len + 12 < size; ++i) {
        uintptr_t v;
        __try {
            v = sp[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (FollowsCall(v)) {
            // Ghidra addresses (image base 0x400000) for easy lookup.
            uintptr_t ghidra = v - reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) + 0x400000;
            len += snprintf(line + len, size - len, " %06x", (unsigned)ghidra);
            ++found;
        }
    }
}

void SetDrawProbe(void (*probe)())
{
    g_draw_probe = probe;
}

DrawCapture SetDrawCapture(const DrawCapture& capture)
{
    DrawCapture previous = g_capture;
    g_capture = capture;
    return previous;
}


void FrameTraceOnPresent()
{
    if (g_trace_active) {
        Emit("");
        Log("=== frame trace end: %d draws", g_trace_draws);
        g_trace_active = false;
    }
    static bool f9_was_down;
    bool f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9_down && !f9_was_down)
        g_trace_requested = true;
    f9_was_down = f9_down;
    if (g_trace_requested) {
        g_trace_requested = false;
        g_trace_active = true;
        g_trace_draws = 0;
        g_last_line[0] = 0;
        g_repeat = 0;
        Log("=== frame trace begin");
    }
}
