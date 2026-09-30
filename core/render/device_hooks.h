#pragma once
#include <d3d9.h>

// All IDirect3DDevice9 method hooks except Present/Reset (d3d9_hooks.cpp):
//  - scene redirect: while active, the engine's drawing goes to our eye render
//    target instead of the back buffer, with its screen-space (XYZRHW) vertices,
//    viewports, clears and copies scaled from the game's resolution to the eye
//    resolution. The headset resolution is then independent of the game window.
//  - D3D9Ex support: MANAGED-pool resources (not allowed on an Ex device) are
//    created in the DEFAULT pool instead (textures as DYNAMIC so they stay lockable).
//  - F9 frame trace (reverse-engineering aid, see docs/re-map.md).
void InstallDeviceHooks(IDirect3DDevice9* device, bool is_ex);

// The uniform canvas -> eye scale the redirect uses (scaled about the centres,
// so the engine's image keeps its proportions and any overhang is clipped).
float RedirectScale(UINT canvas_w, UINT canvas_h, UINT eye_w, UINT eye_h);

// Redirects rendering into color/depth (eye size) from the game's canvas
// (the back buffer) until EndSceneRedirect.
void BeginSceneRedirect(IDirect3DDevice9* device, IDirect3DSurface9* color, IDirect3DSurface9* depth);
void EndSceneRedirect(IDirect3DDevice9* device);

// Called from the Present hook once per frame (F9 trace).
void FrameTraceOnPresent();

// Calls probe once, at the next draw call (then clears it). Pass nullptr to
// cancel. Used to inspect engine state in the middle of a specific draw.
void SetDrawProbe(void (*probe)());

// Draw capture: while a callback is set, every triangle of the engine's
// pre-transformed draws (DrawPrimitiveUP / DrawIndexedPrimitiveUP; lists, strips
// and fans) is passed to it, in canvas coordinates (before the redirect's
// scaling). With skip_draw, the draws themselves (and clears) are dropped.
struct CapturedVertex {
    float x, y, z, rhw;
    float u, v;           // 0 when the vertex has no texture coordinates
    unsigned diffuse;     // 0xffffffff when the vertex has no colour
};
typedef void (*TriangleCallback)(const CapturedVertex tri[3], IDirect3DBaseTexture9* texture, void* user);
struct DrawCapture {
    TriangleCallback callback = nullptr;
    void* user = nullptr;
    bool skip_draw = false;
};
// Sets the capture and returns the previous one (to restore it afterwards).
DrawCapture SetDrawCapture(const DrawCapture& capture);

// Appends up to max_frames exe return addresses found on the stack above
// stack_top to line (reverse-engineering aid).
void AppendExeCallers(char* line, size_t size, const void* stack_top, int max_frames);
