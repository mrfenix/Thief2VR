#pragma once
#include <d3d9.h>
#include <cstdio>

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

// While non-null, every pre-transformed triangle drawn is written to the file
// as "draw x0 y0 u0 v0 x1 y1 u1 v1 x2 y2 u2 v2" (screen x/y, texture u/v).
void SetGeometryCapture(FILE* file);

// Appends up to max_frames exe return addresses found on the stack above
// stack_top to line (reverse-engineering aid).
void AppendExeCallers(char* line, size_t size, const void* stack_top, int max_frames);
