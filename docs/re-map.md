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
- Thief2VR hooks `FUN_00463c60` and swaps in a substituted object transform (context +0x44) for that draw, so the arm appears in the hand.
- Render context layout (F12 dump; 9 floats read row by row into a matrix M):
  - +0x44: the current object's transform M, with o at +0x68. Applied as `x_world = M^T x + o`. For the arm it's identity and 0, because creature vertices are already in world space.
  - +0x78: the camera C, with t at +0x9c. Applied as `view = C^T x + t`, where `t = -C^T eye`.
  - +0x114: the camera basis. +0x138: the eye position.
  - Arm in hand, with `D = R_hand R_gamecam^T`: `M' = M D^T`, `o' = p_hand + D (o - p_gamecam)`.
- The sword and the hand share one texture atlas (`swhand`, 256x256). The blade is the strip at x 0–24 over the full height; the hilt wrap is at the top, x 0–29.
- Limb mode `DAT_008717f8`, with the pending mode in `DAT_008717f4`, applied by `FUN_0054c880`:
  - 0xff = none
  - 0 = empty hands (`FUN_0046a110`)
  - 1 = bow (`FUN_0046a780`, sets a 5000 ms value, likely max draw)
  - 2 = sword / blackjack (`FUN_0046c230`, sets the attack timings)
  - 4 = carrying a body (`FUN_0046a090`)

### Melee (sword / blackjack)
- **Attack state machine** (limb mode 2, set up by `FUN_0046c230`; timings in ms at `DAT_00890fa4..fb4`: full charge 1200, max hold 8000, 200, 1200, quick-swing limit 650):
  - `FUN_0046c950` (use_weapon press) needs `DAT_00890f7c` and `DAT_00890f8c` (ready). It sends StartWindup (`FUN_0059c2f0(1,…)`), requests the wind-up motion (`FUN_0054ca30`), sets `DAT_00890f9c` = winding and resets the timer `DAT_00890fa0`. It registers the arm motion-flag callbacks `0x1000 -> 0x0046c460` and `0x2000 -> 0x0046c490`.
  - `FUN_0046ca90` (release) sends StartAttack (`FUN_0059c2f0(2, weapon, level 1..3)`), requests the swing motion (`FUN_0054c830`) and sets `DAT_00890f94` = released.
  - `0x0046c460` → `FUN_0055aab0`: hits on. `0x0046c490` → `FUN_0055ab00`: hits off. Both take EAX = creature object and ESI = weapon, and call `creature->vtbl[0x9c](weapon, 0)` (MakeWeaponPhysical) or `vtbl[0xa0](weapon)` (MakeWeaponNonPhysical). The creature comes from `DAT_0099b8b0`: `+0xe4` maps obj→index (virtual), `+0xd4` is a table of entries whose creature is at `+4`.
  - `FUN_0046bca0` ends the attack (motion finished): hits off, CurWpnDmg, then EndAttack and resetting `f94`/`f98`.
  - In a quick swing the hits switch on about 0.54 s after the press, when the swing animation reaches flag 0x1000.
  - Arm motion requests: `DAT_00aa1410[4]` = 1 wind-up (`[5]` = type), 2 swing, 3 idle, applied by `FUN_0054c780`.
  - A request (e.g. the swing, `FUN_0054c830`) starts at once only if the motion controller (`[6]`, vtbl 0x30(`[2]`)) isn't busy: it then stops the current motion (`(*[0])->vtbl 0x24(0)`) and calls `FUN_0054c780` (ESI = the arm state). Otherwise it waits for the current motion (the wind-up) to finish.
  - **Swing sound:** entering an arm state (`FUN_0054bf40`) plays "Event Motion" plus the state's tags (e.g. `PlyrSword 1, PlyrSwordSwing 1, Direction 1`) through the tag sound player `FUN_005738d0(tags, arm, weapon, params)`, so the swing sound comes when the swing motion starts. (`FUN_0057e290` "Event WeaponSwing" is the AI creatures' motion-flag swing sound, not the player's.)
  - In testing, the OpenAL function pointers resolved by `FUN_006aa0c0` (e.g. `alSourcePlay` at `DAT_00a9d598`, the module at `DAT_00a9d588`) read null at runtime, so sound-level tracing has to happen above the driver (e.g. at `FUN_005738d0`).
- **Hit spheres:** `FUN_0055fcd0` (a creature method, `__thiscall(creature, weapon, weapon index)`, RET 8) runs each frame while the weapon has physics *and* the arm's attack motion runs (it stops once the motion ends, even with the weapon still physical). With the swing motion started at once, Thief2VR calls it itself each frame of its hit window that the engine doesn't (`EngineTakeMeleeHits`), with the weapon index seen in the engine's own calls.
  - `FUN_00537270` is also called by each creature's body update (`0x0055f613`, …) for its own body spheres (object = the creature).
  - The spheres come from the creature type's weapon table: `DAT_00aa1554[creature+0x38] + 0x3c`, then `[index]` = {count, entries of 5 dwords: joint A, joint B, t, radius, ?}.
  - Each sphere is placed at `lerp(joint A, joint B, t)` of the joints (world space, `creature+0x19c`) by `FUN_00537270` (cdecl(obj, sphere), point in EDI).
  - For the player it also raycasts from the player to each sphere, and on a hit calls `IDamageModel::HandleImpact`.
- **Current weapon:** `FUN_0059db80` returns CurWeapon(owner in EDX) in EAX. It doesn't preserve the callee-saved registers.
- **Damage model:** `AppGetObj` (`FUN_00677250`, stdcall, RET 4) with the IID at `0x007e72d0`. Slots:
  - 3 HandleImpact(victim, culprit, impact*, event*);
  - 4 DamageObject(victim, culprit, {amount, type}*, …);
  - 5 SlayObject.
- **Damage seen:** a blackjack hit gives DamageObject {1, -900} (knockout); a sword hit gives {2, -901}.
- **Player arm joints:** 5 joints — 0 root (the arm object's position), 1–2 arm, 3 hand, 4 weapon tip (the sword is about 3 ft from joint 3 to 4).
- **Thief2VR:**
  - The spheres get the same move into the hand as the drawn arm.
  - With VR melee on, the 0x1000/0x2000 callbacks are ignored. The swing opens the hit window right after its quick attack is released (`f94` set) and closes it when the hand slows.
  - With VR melee on and a sword / blackjack out, the swing request always starts the swing motion at once (as in the not-busy case), so the swing sound plays as the player swings.
  - The attack end (`FUN_0046bca0`, cdecl(a, b): the swing motion's end callback, installed by `FUN_0046c230`; also called when the weapon is put away from `FUN_0046c3a0`) is held back while the VR hit window is open and run when it closes, so a short swing motion doesn't switch the hits off mid-swing.
  - "Ready for the next attack" is `DAT_00890f8c`: cleared at release (`FUN_0046ca90`), set again by `LAB_0046bc90`, which the attack end registers for the arm motion's next 0x2000 flag (also set at limb setup, `FUN_0046c230`). `FUN_0046c950` refuses an attack while it's 0. When the attack end was held back, that flag has passed, so Thief2VR sets it itself.
  - The weapon is pinned to a recorded rest pose (joint frame 2-3-4 relative to the game camera), so the arm animation doesn't show.

### Bow arm
- **Joints:** 8 in total.
  - 0 root, 1 shoulder, 2 elbow, 3 wrist.
  - 3, 4 and 6 are rigid: the hand on the grip.
  - 4 → 5 is the upper limb (1.81 ft) and 6 → 7 the lower limb (1.23 ft). The limbs flex while drawing: the tip-to-grip distances change.
- **Full draw, relative to the game camera** (ft; forward, left, up):
  - 3 (1.638, -0.452, -0.629)
  - 4 (2.050, -0.420, -0.071)
  - 6 (1.954, -0.329, -1.309)
  - 5 (1.111, -0.671, 1.456)
  - 7 (1.225, -0.327, -2.295)
- The bow is upright about 2 ft ahead, and the arrow runs parallel to the camera's forward axis (its vanishing point is the screen centre).
- At rest the bow is lowered and tilted.
- Thief2VR always pins the bow to this full-draw pose and puts its grip (between 4 and 6) in the hand. Camera-forward then maps to the arrow direction.
- **First-person bow mesh:** `BOWSITE.BIN` in `mesh.crf` (materials `handar10.gif` arm, `bowsite.gif` bow); the nocked arrow isn't part of it.
- **Firing:** `FUN_0046aed0` calls `FUN_005500e0(player DAT_00aa1418, arrow DAT_00aa0f8c, power, 0x204, 0, 0, 0)`.
  - `FUN_005500e0` launches a projectile: cdecl(launcher, projectile, power, flags, extra velocity*, ?, start point*).
  - Without a start point it uses the projectile's own position and the launcher's facing. For the player (`FUN_0054ffa0`) the facing is the player camera's angles (`DAT_00aa141c` + 0x14: 16-bit bank, pitch, heading).
  - Flag 2 would push the start out along the facing with a collision check; the bow doesn't set it.
- **Nocked arrow:** `DAT_00aa0f8c` (the object fired next).
- **Model names:** the ModelName property interface at `DAT_00ab00c4`; vtbl 0x54 is Get(this, obj, const char** name), `__stdcall` (as used by `FUN_005bd430`). Arrow models are in `obj.crf` (`arrow.bin`, `arrowfir.bin`, …): along X, head at +X, nock at the min X.
- **World bow model:** `bow2.bin`, up along Z (tips at ±1.89 ft), handle near Z = 0 on the −Y side; limbs sweep to +Y.
- **Thief2VR (hands on):** the bow arm is hidden and `bow2.bin` is drawn in the bow hand with a string and the nocked arrow's model. The launcher is hooked: the player's nocked arrow starts at the drawn arrow's centre, and the camera angles are set to its direction for the call.

### Frob (use / pick up)
- **Per frame (`FUN_0044e660`):** `FUN_0044fe00` turns the pick candidate `DAT_009a337c` into the highlighted target `DAT_00aa1e08` (filtered by look speed, `head_focus_speed_tol`, and distance `DAT_00892b74`; notifies through `FUN_00589c40`), then `FUN_0058fed0` resets the pick (candidate 0, best score `DAT_009a3384`, ray from the camera `DAT_00aa141c`).
- **While objects are drawn:** `FUN_00463ae0` offers each one passing the frob filter `FUN_0058a320` to `FUN_0058fe80` (object in EAX, moved to ESI), which scores its on-screen box against the screen centre (`FUN_0058fc40`), drops excluded ones (`FUN_004509b0`, object in ESI) and keeps the best (returns `DAT_009a337c`).
- **Frob handlers** (`FUN_004540f0` fills `DAT_00aa17cc..e0`): in world, in inventory, inventory → world (`LAB_004522d0`: throw the item, `FUN_00451530`), inventory → inventory, world → world, world → inventory.
- **Thief2VR:** with hand frob, `FUN_0058fe80` scores against the right hand instead (touching within 1 ft wins, else a 27° cone from the hand), and `FUN_0058fed0` resets that score.

### Inventory display / HUD layout
- Slots at `DAT_00aadb78 + slot * 0x2c` (slot 0 the weapon, 1 the item): +0 the render resource, +4 the screen rect (shorts x0 y0 x1 y1, canvas pixels), +0x20 the object, +0x24 a hidden flag. Laid out each frame by `FUN_00452f10` (which, while junk is wielded, hides slot 0 and centres slot 1), then drawn by `FUN_00452780` (cdecl(int)), which skips hidden slots. The canvas size is at `DAT_00ab8608` + 8 / + 10.
- `DAT_00a9fa08` is re-read every frame from the quest variable `HIDE_UI_ELEMENTS` (bits 8 << slot hide slots).
- At 1600 x 900: weapon slot (0,666)-(240,846), item slot (1360,666)-(1600,846), the health shields in a row along the bottom left below them, the light gem ("vismeter", an overlay model) at the bottom centre.
- **Thief2VR wrist HUD:** the captured HUD's zones (slots from the engine; health and gem below them) are cut into a wrist atlas and cleared from the HUD in front of the view; each wrist shows its quads while looked at.

### Throwing
- `FUN_00451530(launcher, obj, power)`: a creature (body) is laid down with the launcher's flags `0x804`; anything else goes to `FUN_005500e0(launcher, obj, power, 0x802, 0, 0, 0)`. Flag 2 pushes the start out along the facing with a collision check; 0x800 scales by mass. The power is fixed (`0x7b6f0c`), so there's no charge.
- **Thief2VR:** with nothing highlighted, the grip's "use item" waits for the grip to be let go; the throw then leaves from the hand along the hand's velocity (or pointing, when slow), its power scaled by the hand's speed (`EngineSetThrow`, through the launcher hook).

### Sound listener
- **3D provider:** `DAT_0086f81c` (set in `FUN_00571620` from the config key `snd3d`): `a3d` → 2 (DirectSound3D), `openal` → 4 (OpenAL). Without the key it's the game's own software mixing: 3D sound is panned by NewDark and no listener / 3D source methods are called, so Thief2VR can't move the ears. The installer sets `snd3d openal` (or `a3d` without OpenAL32.dll). The audio options menu (`FUN_0045c960`) shows "OpenAL" / "DirectSound3D" from it.
- With DirectSound3D, Thief2VR hooks DirectSound's own IDirectSound3DListener (SetPosition, SetOrientation, SetAllParameters) and IDirectSound3DBuffer (SetPosition, SetAllParameters; head-relative buffers) methods, found through a throwaway DirectSound object.
- NewDark's sound driver is a DirectSound3D-style interface on OpenAL. The OpenAL entry points are resolved by name in `FUN_006aa0c0`: `alListener3f` → `DAT_00a9b978`, `alListenerfv` → `DAT_00a9ca1c`.
- Listener methods (`__stdcall`, `this` first). Vectors are in engine world space and converted to OpenAL as `(-y, z, -x)`:
  - `0x006ac700` SetPosition(pos), RET 8, sets `AL_POSITION`, scaled by `this+0x13d`;
  - `0x006ac7b0` SetOrientation(front, top), RET 0xc, sets `AL_ORIENTATION`;
  - `0x006ac870` SetVelocity(vel), RET 8.
- The game feeds these from the player camera. With VR, Thief2VR substitutes the head's position and orientation (player camera mode only).
- **Sources:**
  - `0x006b4fb0` SetPosition(this, pos), RET 8. It sets `AL_POSITION` through `alSource3f` (`DAT_00a9ca18`); the source id is at `this+0x11d` and the position is stored at `this+0xd1`.
  - `0x006b5280` SetMode(this, mode) sets `AL_SOURCE_RELATIVE = (mode != 1)`, with the mode at `this+0x105`.
- Most 3D sounds are **listener-relative**, in the player camera's frame (x forward, y left, z up), and OpenAL doesn't rotate relative sources by the listener orientation. Thief2VR re-expresses those positions in the head's frame: `p' = R_head^T (R_camera p + camera - head)`. Relative sources are detected with `alGetSourcei(AL_SOURCE_RELATIVE)`, loaded from the OpenAL module at `DAT_00a9d588`.

### Menus / 2D screens (mouse)
- The exe imports `GetCursorPos`, `SetCursorPos`, `ClipCursor`, `ScreenToClient` and `ClientToScreen`. It also loads `dinput.dll` (`DirectInputCreateA`) and raw input (`RegisterRawInputDevices` / `GetRawInputData`, cfg `raw_mouse_input`).
- Thief2VR drives the menus with `SetCursorPos` plus `SendInput` (real OS input, so every path above sees it). See `core/input/screen_pointer.cpp`.
- Commands: `automap` (bound to m), `objectives` (bound to o), `sim_menu` (Esc).

### Frame (F9 trace, mission, 800x600)
1. BeginScene/EndScene pair with no draws (a UI/2D pass).
2. BeginScene from the frame render.
3. Inside the scene render: Clear(target|z|stencil), then world polys (FVF 0x1c4 stride 32, pre-transformed), then objects (FVF 0x2c4 stride 40).
4. The scene render returns. Overlays/HUD are drawn from the frame render (0x6112aa/0x6236ac), then EndScene. There's no SetRenderTarget call: everything goes to the back buffer.
