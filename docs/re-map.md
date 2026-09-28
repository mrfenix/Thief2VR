# Engine map: Thief2.exe (NewDark 1.29, T2Fix 2026-09-16)

- SHA-256: `d26342c34624a08a0e5fc8d8c9daf14c676642c328f41c01f548170774aafe8f`
- Size: 5,509,120 bytes. Image base 0x400000, 32-bit.
- Ghidra project: `re/T2VR.gpr` (the exe copy is `re/Thief2_129.exe`)

## Confirmed at runtime (thief2vr.log, 2026-09-28)
- d3d9 is loaded with `LoadLibrary("d3d9.dll")` + `GetProcAddress("Direct3DCreate9")`, so it isn't in the import table. The proxy in the game dir is picked up first.
- `Direct3DCreate9` is called twice at startup. The second IDirect3D9 creates the device.
- The device is created with behavior `0x42` (HARDWARE_VERTEXPROCESSING | FPU_PRESERVE), fullscreen, 1 back buffer, D3DFMT_X8R8G8B8 (22), depth D24S8 (75), vsync interval 1.
- `Reset` is called once when entering a mission.

## Prior art (newdarkvr, a different NewDark build, sha af56a1…)
Their addresses don't apply to this build, so use them only to recognise the code:
- The scene render function copies the camera to a stack local. Adding head yaw/pitch there leaves game logic untouched.
- The scene render is called once per frame from the hardware-render branch of the frame handler. There is a separate software fallback call.
- Vertices are pre-transformed (FVF 0x1c4 = XYZRHW|DIFFUSE|SPECULAR|TEX1), so no D3D transforms are used.
- Angles are 16-bit (0x10000 = 360°). Heading +CCW, forward = (cos h, sin h). Positive pitch tilts the view down.
- A view-scale/FOV global is read every frame.
- Screen overlays (light gem, inventory) are drawn by a separate function after the 3D scene.

## Useful config vars (cam_ext.cfg / user.cfg)
- `bob_factor 0` disables head bob (required for comfort)
- `phys_freq <n>`, `use_hi_res_timer`: stable physics at VR frame rates
- `small_portal_repel`: removes the portal-crossing camera jitter
- `player_camera_limit_x/y/z <lo> <hi> <margin>`: camera angle limits
- `fov <deg>`, `widescreen_lock_hfov`, `user_mode1 <w> <h>`, `framerate_cap`, `vsync_mode`

## Addresses for this build
Ghidra addresses use image base 0x400000. **The exe is loaded with ASLR** (seen at 0x1c0000), so the mod uses RVAs (Ghidra address − 0x400000) plus the runtime module base.

| What | Ghidra addr | RVA | Signature / notes | How found |
|---|---|---|---|---|
| Frame render (`FUN_005d1280`) | 0x5d1280 | 0x1d1280 | reads current camera, builds stack-local `Position`, calls scene render, then draws overlays | F9 trace: BeginScene/EndScene return addrs 0x5d143d / 0x5d14da |
| Scene render (`FUN_005d0ee0`) | 0x5d0ee0 | 0x1d0ee0 | `83 ec 28 dd 05 ?? ?? ?? ?? 53 56 dd 54 24 24 dd 05 ?? ?? ?? ?? 8b f0 8b 06` | only caller is 0x5d1451 in frame render |
| Scene render call site | 0x5d1451 | 0x1d1451 | `8d 44 24 1c dd 1c 24 e8` (lea eax,[esp+1c]; fstp qword [esp]; call) | disasm |
| Current camera ptr `DAT_00aa141c` | 0xaa141c | 0x6a141c | `int* cam`: [0]=mode (3,4 = remote/orb cams), [1]=zoom (float), [2..4]=pos, [5]=tx\|ty, [6]=tz | frame render decomp |
| Render position ptr `DAT_009a46c4` | 0x9a46c4 | 0x5a46c4 | points to the frame render's stack `Position` during the scene render | frame render decomp |
| FOV scale `DAT_0085c018` | 0x85c018 | 0x45c018 | float = 1/tan(fov_4:3/2), written by `FUN_005ced40` | xrefs |
| Near/far setter `FUN_005cefc0` | 0x5cefc0 | 0x1cefc0 | z_near / z_far doubles at 0x85c028 / 0x85c030 | renderer init decomp |
| Aspect (16.16) `DAT_00ab8620` | 0xab8620 | 0x6b8620 | = w*0xC000/h (aspect relative to 4:3) | FOV-lock setting decomp `FUN_0068c5d0` |
| Renderer init `FUN_005cf710` | 0x5cf710 | 0x1cf710 | reads render config vars | string xrefs |

### Scene render calling convention
`void SceneRender(Position* pos /*EAX*/, double focal /*stack*/)`. The caller cleans the stack (`add esp,8`). The function saves EBX/ESI/EDI itself.

`Position` (22 bytes, Dark `Location` + `mxs_angvec`):
| Offset | Field |
|---|---|
| +0x0 | float x |
| +0x4 | float y |
| +0x8 | float z |
| +0xC | short cell = -1 (the engine recomputes it) |
| +0xE | short hint = -1 |
| +0x10 | ushort tx (bank) |
| +0x12 | ushort ty (pitch, + = down) |
| +0x14 | ushort tz (heading, CCW, forward = (cos, sin)) |

Rotation composition is Z·Y·X (heading, then pitch, then bank). World axes are +X forward, +Y left, +Z up. Units are feet.

### Projection
`focal = cot(fov_4:3 / 2) * zoom`. The engine renders with tan(half vfov) = 0.75 / focal and tan(half hfov) = (w/h) · tan(half vfov) (widescreen expands horizontally unless `widescreen_lock_hfov`).

### Player input / camera (controls)
| What | Ghidra addr | Notes |
|---|---|---|
| Command executor | 0x41aab0 | `int(char* cmd /*EAX*/)`, 0 = ok, tokenizes in place |
| Input binder object | `DAT_00a9f858` | IInputBinder; vtable 0x7f8148, slot 12 = ProcessCmd(this, char*) (bind/ibset/echo…) |
| Input-variable table | 0x89ee88… | 140-byte entries: name[] +0, handler +0x64 = `int __cdecl(char* name, char* value)` |
| Set forward / strafe speed | 0x54de40 / 0x54de90 | cdecl float, -2..2; control struct `DAT_00aa1444` +0x14 / +0x18 |
| Apply player control | 0x54ec10 | **stdcall** (RET 4), arg = control struct |
| Player object | `DAT_00aa1418` | object id |
| Physics model of object | 0x507d70 | **stdcall** (RET 4); model+0xf0 → look angles bank/pitch/heading at +0x10/+0x12/+0x14 |
| Player camera update | 0x546ca0 | cdecl void; adds look heading (then zeroed) to the player's facing; pitch persists. Hooked to set turns / aim |
| Object position get | 0x54a330 | `Position*(obj /*EAX*/)`: vec +0, cell/hint +0xc, bank/pitch/heading +0x10/+0x12/+0x14 |
| Object position update | 0x54a3c0 | obj in EDI, location in ESI, facing on stack |

### First-person arm (weapon)
- Arm state `DAT_00aa1410`: the arm object id is at +4 (a `PlayerArm` / `PlayerBowArm` creature; weapons are mesh attachments).
- `FUN_004639e0` installs render callbacks: `[0xbe7268] = FUN_00463c60` (the arm renderer), with the previous value chained through `DAT_008d9614`.
- The world renderer `FUN_004dad50` (called from the scene render) calls `[0xbe7268]` at its end. So the arm is drawn inside every scene render (once per eye in stereo), with its own near/far clip range and `dark_zcomp_arm`.
- `FUN_00463c60` needs the render context (`DAT_00aac0c8`) of the world render. Calling it outside that context crashes at 0x5eeb1b.
- Thief2VR hooks `FUN_00463c60` and swaps in a substituted render camera (context +0x44) for that draw, so the arm appears in the hand.
- Limb mode `DAT_008717f8`, with the pending mode in `DAT_008717f4`, applied by `FUN_0054c880`:
  - 0xff = none
  - 0 = empty hands (`FUN_0046a110`)
  - 1 = bow (`FUN_0046a780`, sets a 5000 ms value, likely max draw)
  - 2 = sword / blackjack (`FUN_0046c230`, sets the attack timings)
  - 4 = carrying a body (`FUN_0046a090`)

### Frame (F9 trace, mission, 800x600)
1. BeginScene/EndScene pair with no draws (a UI/2D pass).
2. BeginScene from the frame render.
3. Inside the scene render: Clear(target|z|stencil), then world polys (FVF 0x1c4 stride 32, pre-transformed), then objects (FVF 0x2c4 stride 40).
4. The scene render returns. Overlays/HUD are drawn from the frame render (0x6112aa/0x6236ac), then EndScene. There's no SetRenderTarget call: everything goes to the back buffer.
