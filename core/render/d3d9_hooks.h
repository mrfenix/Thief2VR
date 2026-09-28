#pragma once
#include <d3d9.h>

// Installs vtable hooks on IDirect3D9::CreateDevice and, once the game's device
// exists, on the device (Present / Reset here, the rest in device_hooks.cpp).
void InstallD3D9Hooks(IDirect3D9* d3d);

// The game's device, or nullptr before CreateDevice has succeeded.
IDirect3DDevice9* GameDevice();

// True when the device is D3D9Ex (the proxy created the Direct3D object with
// Direct3DCreate9Ex), which allows sharing eye textures with D3D11 on the GPU.
bool GameDeviceIsEx();
