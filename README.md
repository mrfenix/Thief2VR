# Thief2VR

Click the image to play the video preview:
[<img src="https://img.youtube.com/vi/gF1qXak96SI/hqdefault.jpg" width="720" height="480"
/>](https://youtu.be/gF1qXak96SI)

Play **Thief 2: The Metal Age** in VR, with full head and hand tracking.

- **Stereo 3D at your headset's native resolution**, with 6DoF head tracking.
- **Real melee:** swing the sword and blackjack with your hand. Hits land where your blade is.
- **Bow in your hands:** draw it with both hands like a real bow, nocking an arrow from your hand, or aim one-handed.
- **Hands-on world:** frob what your hand touches or points at, carry crates and bottles in your hand, and throw them (or flash bombs) by swinging.
- **HUD on your body:** the light gem and health on your left wrist, the weapon on your right, and the inventory at your waist above a pouch. The selected item appears in your hand as you cycle.
- **Climb ladders and ropes** with the stick.
- **Physical crouch and lean:** crouch and lean for real, and the player does too.
- **Every game screen in VR:** menus, map, objectives and books appear on a screen in front of you, driven by a laser pointer.
- **Sound that follows your head**, and vibration when your weapon hits.
- **An in-game VR menu** for comfort, controls, hands, HUD and weapon alignment settings.

## What you need
- **Thief 2** (Steam or GOG) with **[T2Fix](https://github.com/Xanfre/T2Fix/releases)** installed.
- **A PC VR headset** with a 32-bit OpenXR runtime. It's tested on Meta Quest with **Virtual Desktop**, which provides one.
- **Touch-style motion controllers.**

## Install
1. Download the latest `Thief2VR-x.y.z.zip` from [Releases](../../releases).
2. Extract it into your **Thief 2 folder** (the one with `Thief2.exe`). You get a `Thief2VR` folder inside it.
3. Open the `Thief2VR` folder and run **`install.bat`**.
4. Start your headset streaming, then launch Thief 2 as usual.

To remove the mod, run **`uninstall.bat`** in the same folder. It puts everything back as it was.

## Playing
- **VR menu:** hold the **left menu button** for half a second. It has all your settings, the full controls list and a recenter button.
- **Game menu:** a short tap on the same button.
- **Recenter:** press **F8**, or use the button in the VR menu.
- **Game screens:** point with the right controller and pull the trigger to click.

The zip's `README.txt` covers the full controls, performance tips and troubleshooting.

---

## For developers
The mod is a `d3d9.dll` proxy that loads `thief2vr.dll`. The DLL hooks the NewDark 1.29 engine to render each eye, drive the player from the controllers, and hand the frames to OpenXR through D3D11.

| Path | Contents |
|---|---|
| `proxy/` | `d3d9.dll`: forwards to the system d3d9 and loads `thief2vr.dll` |
| `core/` | `thief2vr.dll`, the mod itself. The version is in `core/version.h`. |
| `docs/re-map.md` | the engine addresses and structures it uses, and how they were found |
| `package/` | the player install/uninstall scripts and README that go in the release zip |
| `third_party/` | MinHook, the OpenXR loader (Win32), Dear ImGui, the WebXR generic hands and the Hafnia Hands texture |

### Build
You need Visual Studio 2022 with the C++ workload. The build is Win32 only, because the game is 32-bit.
```
MSBuild Thief2VR.sln /p:Configuration=Release /p:Platform=x86
```

### Try it on your own game
`tools\deploy.ps1` copies the built DLLs into your game folder; `-Remove` takes them out again. Run the release `install.bat` once first, because it sets up the game's config (windowed mode, vsync off and so on). The mod writes `thief2vr.log` next to `Thief2.exe`.

### Make a release
`tools\package.ps1` builds and writes `dist\Thief2VR-<version>.zip`. Change `THIEF2VR_VERSION` in `core/version.h` first.

## License
MIT: see `LICENSE`. The third-party components keep their own licences:
- MinHook: BSD 2-clause
- Dear ImGui: MIT
- OpenXR loader: Apache 2.0
- WebXR generic hand models: MIT
- Hafnia Hands skin texture: MIT

Thief 2 isn't included; you need your own copy.
