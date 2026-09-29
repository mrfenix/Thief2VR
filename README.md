# Thief2VR

An OpenXR VR mod for Thief 2: The Metal Age running on NewDark 1.29 (T2Fix). It adds stereo rendering, head and hand tracking, and an in-game VR settings menu.

## Layout
- `proxy/`: `d3d9.dll`, which forwards to the system d3d9 and loads `thief2vr.dll`
- `core/`: `thief2vr.dll`, the mod itself
- `docs/re-map.md`: engine addresses and structs for each supported exe build
- `third_party/`: MinHook, OpenXR loader (Win32), Dear ImGui
- `package/`: the player-facing install/uninstall scripts and README that go in a release zip
- `core/version.h`: the mod's version

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
The mod writes `thief2vr.log` next to `Thief2.exe`. `deploy.ps1` only copies the DLLs; the game's config
changes (windowed mode, vsync off, head bob off...) are made once, by the installer below or by hand.

## Release
```
powershell -File tools\package.ps1
```
Builds Release and writes `dist\Thief2VR-<version>.zip`: the DLLs, the OpenXR loader, `install.bat` /
`uninstall.bat` (the game config changes, recorded in `thief2vr_install.json` so they can be undone), the
player `README.txt`, the mod's `LICENSE.txt` and third-party licences.

## License
MIT (see `LICENSE`). Third-party components keep their own licences: MinHook (BSD 2-clause), Dear ImGui
(MIT), OpenXR loader (Apache 2.0). Bump `THIEF2VR_VERSION` in `core/version.h` for a new release.
