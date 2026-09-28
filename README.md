# Thief2VR

An OpenXR VR mod for Thief 2: The Metal Age running on NewDark 1.29 (T2Fix). It adds stereo rendering, head and hand tracking, and an in-game VR settings menu.

## Layout
- `proxy/`: `d3d9.dll`, which forwards to the system d3d9 and loads `thief2vr.dll`
- `core/`: `thief2vr.dll`, the mod itself
- `docs/re-map.md`: engine addresses and structs for each supported exe build
- `third_party/`: MinHook, OpenXR loader (Win32), Dear ImGui

## Build
You need VS 2022 with the C++ workload. The build is Win32 only, because the game is 32-bit.

```
MSBuild Thief2VR.sln /p:Configuration=Release /p:Platform=x86
```

## Deploy / remove
```
powershell -File tools\deploy.ps1
powershell -File tools\deploy.ps1 -Remove
```
The mod writes `thief2vr.log` next to `Thief2.exe`.
