Thief2VR - VR for Thief 2: The Metal Age
========================================

Play Thief 2 in VR with head tracking, hand tracking and motion controls:
swing the sword and blackjack with your hand, draw the bow (one or two
handed), pick things up and throw them with your hand, lean and crouch with
your body, check the light gem and health on your wrist, and navigate every
game menu, the map and objectives with a laser pointer.


REQUIREMENTS
------------
- Thief 2: The Metal Age (Steam or GOG) with T2Fix installed:
  https://github.com/Xanfre/T2Fix/releases
  (T2Fix updates the game to NewDark 1.29, which this mod is made for.)
- A PC VR setup with a 32-bit OpenXR runtime. Tested with a Meta Quest over
  Virtual Desktop, which provides one (VDXR). Thief 2 is a 32-bit game, so
  runtimes without 32-bit OpenXR support won't work.
- Touch-style motion controllers.


INSTALL
-------
1. Install T2Fix and check that the game runs normally.
2. Extract this zip into your Thief 2 folder (the one with Thief2.exe).
   You get a Thief2VR folder inside it, for example:
     ...\steamapps\common\thief_2\Thief2VR
3. Open that Thief2VR folder and run install.bat.

The installer copies the mod files into the game folder and changes a few
game settings: windowed mode (the headset renders at its own resolution),
vsync off and head bob off. It records everything it changes, so the
uninstaller can put it back. If another d3d9.dll (ReShade, dgVoodoo...)
was in the game folder, it's saved and restored on uninstall.


PLAYING
-------
- Start your headset streaming (e.g. Virtual Desktop) first, then launch
  Thief 2 as usual.
- Game menus appear on a screen in front of you: point with the right
  controller, trigger to click, B to go back.
- Hold the LEFT MENU button for half a second to open the VR menu:
  settings, the full controls list, recenter. A short tap opens the game's
  own menu.
- F8 (or "Recenter view" in the VR menu) recenters your view. Stand upright
  and face forward when you recenter.


CONTROLS
--------
  Left stick               Move (in the direction you look)
  Left stick click         Toggle run
  Right stick left/right   Snap turn (smooth turn: Comfort tab)
  Right stick up / down    Jump / toggle crouch
  Right stick click        Map
  Right trigger            Use weapon (hold to draw the bow, release to fire)
  Swing your right hand    Attack with the sword / blackjack
  Right grip               Frob what your hand touches or points at:
                           pick up, open, use the selected item
  Right grip (crates,      Hold to carry it in your hand; let go to drop
   bottles...)             it, or swing and let go to throw it
  Right grip, nothing      Hold, swing and let go to throw the selected
   highlighted             item (flash bombs, mines...)
  Left trigger             Block
  Left grip                Hold crouch
  A / B                    Next weapon / put the weapon away
  X / Y                    Next / previous item (shown in your hand)
  Hold Y                   Objectives
  Left menu button         Tap: game menu. Hold: VR menu
  Crouch / lean for real   Crouch / lean in the game
  Ladders and ropes        Left stick forward climbs up, back climbs down;
                           jump or crouch to let go
  Bow (two-handed)         Bow in the left hand; squeeze the right grip at
                           the bow (an arrow appears in your hand), pull
                           back, let go. One-handed: Hands tab.


THE HUD ON YOUR BODY
--------------------
- Look at your left wrist for the light gem and health, at your right
  wrist for the selected weapon.
- The inventory sits at your left waist, above a pouch.
- Sizes, positions and the viewing angle are in the VR menu's HUD tab;
  the inventory can go back on the left wrist there.


PERFORMANCE
-----------
- The frame rate follows your headset's refresh rate. With Virtual Desktop,
  set it in VD's streaming settings (72 / 80 / 90 / 120 fps). If your
  runtime lets apps choose, it's also in the VR menu's Performance tab.
- The Performance tab shows how much time each frame takes, including the
  GPU and the spare time: while there's spare time, you can raise the
  refresh rate or turn on anti-aliasing (MSAA).
- The desktop window shows the game at a reduced frame rate during
  missions to save time (View tab; F7 turns it off completely).


TROUBLESHOOTING
---------------
- The game runs flat on the monitor: the headset wasn't streaming when the
  game started, or no 32-bit OpenXR runtime is set up. Check thief2vr.log in
  the game folder.
- "VR stays off" / stereo disabled in the log: the game isn't the NewDark
  1.29 build from T2Fix (a different T2Fix version needs a mod update).
- You feel too tall or too short, or can't fit under low places: check your
  headset's floor / boundary setup, then recenter while standing upright.
  "Vertical head tracking" (View tab) can be turned off to always use the
  game's eye height.
- The weapon feels twitchy: raise "Weapon smoothing" (Hands tab).
- Sounds turn with your hand instead of your head: the game is using
  software sound mixing. The installer sets "snd3d openal" (or "a3d") in
  cam_ext.cfg; check that it's there.
- Your settings are in thief2vr.ini in the game folder; delete it to go back
  to the defaults. The log is thief2vr.log.


UNINSTALL
---------
Run uninstall.bat in the Thief2VR folder. It removes the mod and restores
the game settings it changed. Your VR settings (thief2vr.ini) are left in
place. Afterwards you can delete the Thief2VR folder.


LICENCE AND CREDITS
-------------------
Thief2VR is released under the MIT License (LICENSE.txt). Thief 2 itself
isn't included; you need your own copy. Uses MinHook (BSD 2-clause), Dear ImGui (MIT) and the Khronos OpenXR loader
(Apache 2.0). The hands are based on the WebXR generic hand models (MIT,
Copyright 2019 Amazon), textured with the Hafnia Hands skin texture (MIT,
Copyright 2021 Aske Mottelson). The licences are in the licenses folder.
