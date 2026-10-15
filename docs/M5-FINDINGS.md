# M5 findings: HUD, held weapon, fire direction

From a probe run (`patches/m5-hud-weapon-probe.patch`, `NDVR_PROBE=1`) in the Ramsey tutorial centre with the pistol. Canvas 2560x1440.

## Frame structure (one frame)

1. `render_frame_world` (0x006017d0) is entered.
2. `d3d_display_set_overlay` (0x006c9e30) opens the 3D pass. Despite the name it brackets the **3D scene**, not the HUD.
3. `render_scene_from_camera` (0x00601430): a depth+target clear, then the world draws (pre-transformed, depth test on, scissor 0,0,2560,1440, FVF 0x2C4/0x1C4).
4. Still inside the scene render: world objects, then a **second clear, depth only (flags=2)**, then the held weapon (3 draws for the pistol: 36+30+6 triangles, FVF 0x1C4, z 0.89-0.96, lower right of the screen).
5. `d3d_display_end_overlay` (0x006c9ee0) closes the 3D pass.
6. The HUD is drawn: FVF 0x144, no depth test, scissor 0,0,1280,720: 30-ish small textured quads (crosshair at 632..648 x 352..368, health, ammo, text...). It goes into a **1280x720 render-target texture** (texture 0xe39420 in that run).
7. The last draw of the frame is one full-screen quad (1280x720 texture, alpha blend, 0..2560 x 0..1440) that composites that HUD texture over the 3D image.

Consequences:
- **HUD:** it is already a separate 1280x720 texture. For VR, take that texture (or skip the final composite quad and draw it as a panel), no HUD parsing needed.
- **Weapon:** every draw after the mid-scene depth-only clear, until `d3d_display_end_overlay`. Vertices are pre-transformed (x, y, z, 1/w), so camera-space positions can be rebuilt from them (1/w = 1/depth, focal length from the calibration) and re-projected with a hand pose.

## Fire direction

`cmd_fire_weapon` (0x0043b220 in `symbols/manual.sym`) is really the shared handler of all key commands (`fire_weapon`, `drag_and_drop_mode`...); the command name is the first stack argument, the argument string ("1.0" / "0") the second.

The player camera object pointer `*0xa3acb8` (0xd9ba10 in that run) has the player's position at +8 (x, y, z) and the facing straight after it: **bank u16 at +20, pitch u16 at +22, heading u16 at +24** (the render camera copy at 0xa57e00 has the same facing but with a cell word between, so +16/+18/+20 there). Its pitch and heading equal the render camera's exactly. A second copy with the camera copy's layout sits in global data (0x8cdb70). A third at 0xa3acc8 never changes (stale). Which copy a shot is aimed with is what `NDVR_AIMTEST_*` finds out.

## Aim test 1 (NDVR_AIMTEST_HEAD=40, 2 frames, all four matching copies)

Shots still hit the crosshair; the **muzzle flash** appeared on the left of the screen (heading +40 turns left), so the
flash / gun attachment reads one of those copies, but the projectile direction does not, or reads it at a moment when
it has already been rewritten from somewhere else. ("Stopped after reloading" was the probe's own six-shot limit.)
Next: `NDVR_PERSIST (now NDVR_HUNT) finds which copy is the master (a value that sticks and turns the view), and the probe now also
lists every place holding the camera's forward direction as floats (a vector or a rotation matrix row).

## Persistence test 1 (NDVR_PERSIST=10)

All three facing copies (camera object +0x18, globals 0x8cdb70 and 0x93d43c) are **derived**: a value written there
is overwritten within 10 frames and the view does not turn. So the player's real facing lives elsewhere, most likely
on the heap (the player object's position property). Two more derived forms were found in global data:
- 0x93d450: forward vector (x, y, -sin pitch) right after the 0x93d43c position/facing, followed by -454.1, 45.0, 0, 0,
  1e9, 50.0: looks like a ray (aim / frob / pick ray).
- 0x93e45c: a 3x3 rotation matrix, forward row first, then (-1, 0, 0)-ish (side) and (0, -sin p, cos p) (up).
The probe now also searches the heap arenas (`vr_heap_arenas` added to port/core/heap.c by the probe patch), accepts
heading-only matches (a body facing has no pitch), and tests untested copies first.

## Heap search (NDVR_PERSIST=10, probe 6f9cb93): the heading master

Found on the heap at 0x01d509c0 in that run (the address changes per run/level): the player body's position
(camera x, y; z = eye - 2.77) followed by a cell word (0xffffffff), bank 0, pitch 0 and **heading u16 at +20**:
heading only, no pitch. Turning it 10 degrees stuck and turned the view. It sits among the player's physics
submodels (positions at z = eye - 5.77, -5.37, -4.97, -3.37, -0.97 next to it, 0x2c apart). Every other copy is rebuilt
from it each frame.

**A shot fired right after that write lands 10 degrees to the left** (confirmed by Owen over several retests): the
projectile is aimed with this body heading. So controller aim horizontally = write this heading at the moment of the
shot. Pitch is stored somewhere else (the body facing has none); `NDVR_HUNT=1` searches for it next.

## Hunt run 1 (NDVR_HUNT=1, 5 shots)

Heading master found again on the third shot (heap 0x01d4f904 that run). Pitch: 88 places held the pitch at the first
shot, 10 after the second; still in: global copies 0x8cdb82, 0x93a066, 0x93cfb6, 0x93d44a, the camera object +0x16,
and heap 0x01998a22, 0x01d4fab2, 0x01d4fac8, 0x01d4fade (0x1ae past the heading master, 0x16 apart: per-submodel
facings of the physics model), 0x023a61c6. The two tested (0x8cdb82, 0x93a066) are rebuilt. Recoil moves the view
pitch about -6 degrees, so "the view turned" now means +6..+14 for our +10, and heap entries are tested first.

## Hunt run 2 (13+ shots)

- Pitch: every u16 copy of the camera pitch (globals, camera object, physics submodels) and every float-radian copy is
  rebuilt each frame; none is the master. The master pitch is not stored as the camera's u16 nor as radians.
- Aim test with the heading master turned +30 for 2 frames after the press: shots still hit the crosshair, while the
  earlier permanent write moved them. So the projectile is launched more than 2 frames after the key press (or the
  launch reads the facing again later); writing the master only around the press is not enough.
- Visible side effect on presses during a reload: the view swings left for a moment.

Change of approach for aiming: drive the game's own mouse look instead of writing its state. Synthetic mouse
motion (`vr_input_mouse_motion` in port/win32/user32.c) turns heading and pitch exactly like the player's mouse, so
shots, flash and gun follow by construction; a closed loop can steer the camera to the controller's direction.
`NDVR_MOUSETEST` measures degrees per count, delay and smoothing first. The probe's draw dumps are now opt-in
(`NDVR_PROBE_CAPTURE=1`) because the PowerShell window truncates long output.

## Mouse-look response (NDVR_MOUSETEST=200, 25 tests)

Perfectly linear, no smoothing, no drift, identical every time:
- **heading: 32 units (0.1758 degrees) per count**; dx < 0 turns left (heading increases)
- **pitch: 48 units (0.2637 degrees) per count**; dy < 0 looks up (pitch decreases)
- **delay: 2 frames** (the change shows on the 3rd presented frame), then it holds
- nothing happens in menus (pointer not captured), as intended

So controller aim = inject `round((target - current - pending) / 32)` counts sideways and `/ 48` up/down each frame,
with `pending` = counts sent in the last 2 frames. Shots, flash and gun follow by construction. Granularity is 0.18 /
0.26 degrees (3 / 5 cm at 10 m); the in-game mouse sensitivity setting changes the scale, so measure it at start-up
(one known nudge) rather than hard-coding it. Pitch limits not measured yet.

## Headset HUD, first impressions (Owen, Quest 3)

- Controller aim through mouse look: follows the hand as it should.
- Flat HUD at 1.1 m: too small, boxy, did not follow the head. Now a curved helmet-style panel following the head.
- Curved HUD at 80 degrees: the top console reads perfectly; health and ammo in the corners were too far out to read.
  In play the HUD is now cut into three strips with the outer ones pulled 12 degrees in and 6 up (left edge at 28
  instead of 40 degrees).
- The target brackets / health bar around an enemy are drawn by the game for its own camera, which now points where
  the gun points, not where the head looks, so on the head-following panel they sit off the enemy. A fix would pick
  those draws out of the HUD and place them in 3D along the engine camera (like the gun model); not done yet.

## Weapon in the hand (first version)

Inside each eye render, everything after the depth-only clear is the first-person weapon. port/dx/d3d9.c turns its
screen-space vertices back into camera space (z = 1/rhw in engine units, x/y through the measured focal length
H * zoom / 1.5), moves them with a per-eye matrix and projects them again; depth is refitted per draw as a + b * rhw
from the draw's own vertices (the weapon is drawn after a depth clear, so it only has to agree with itself). The rest
position is the middle of the weapon's bounding box over its first 30 frames (re-measured when the vertex count
changes by a quarter for 15 frames: a weapon change). Left hand: mirrored on x, culling flipped. Unknowns: whether
1/rhw is exactly the depth in engine units (if not, the weapon sits nearer or further than the hand), and the foot
as the engine unit (NDVR_UNIT_M).

## Weapon, second version: a capture pass

Two headset runs of the first version logged no weapon lines at all: the build that ran did not have the weapon code.
What Owen saw (a broken, flickering pistol) was the game's own weapon in the eye renders. The engine places the
weapon relative to its own camera, which now follows the gun hand, while the eyes look from the head: the weapon hangs
off the aim direction at the eye position and the eye's frustum cuts it, more the further aim and gaze differ. So the
eye renders cannot be the source. Now each frame one extra scene render from the engine's own camera (zoom 0.5, wide,
so the screen edges cut less) keeps only the weapon's draws (nothing drawn, no clears); the eye renders drop the
engine's weapon; after each eye the kept draws are moved to the hand and drawn after a depth clear. The startup log
now names the NewDarkVR build (stamped by the installer).

Run of 0a1f9ad: the capture pass kept 0 weapon draws every frame. The weapon is not drawn inside
render_scene_from_camera at all: the engine draws it after that returns (depth-only clear, weapon draws, EndScene),
with the camera state the last scene render left. In headset mode that was the right eye's camera, drawn onto the
desktop back buffer: the broken pistol on the monitor and nothing in the headset. Now: after the eyes one more
scene render from the engine's own camera (zoom 0.5, nothing drawn) sets that camera; the weapon draws that follow, up
to EndScene, are kept instead of drawn; at Present they are moved to the hand and drawn into each eye's image.

VR probe run (d479969 + probe): the engine batches its draws. A scene render's weapon polygons, queued after its
depth-only clear, reach the device only at the next flush: the next render's first clear, or EndScene. In headset
mode eye 0's weapon arrived in eye 1's image just before eye 1's clear, eye 1's in the camera pass, and the camera
pass's at EndScene after the hook had returned. That last batch is the weapon to keep: mode 3 now starts in the
weapon phase when the camera pass did its depth clear, and keeps everything up to EndScene.

Run of e37e2d4 (Owen): the weapon is in the hand, but huge and missing half its model. The first-person model is
built for one fixed view: whatever the game never shows from that view is not modelled, and whatever it does not draw
from that view (back faces, the parts clipped at the screen edge and near plane) never reaches us. Moving the captured
triangles cannot bring that back, and a better first-person model does not help either, because the view it is drawn
from is the same every frame. Plan: draw the weapon ourselves in each eye, from the game's own model files
read at run time (the weapon's world model is complete; replacement packs such as Tacticool would then work as they
are), at real scale. The capture path stays as the fallback (NDVR_GUN_SCALE to shrink it).

## The weapon models themselves (tools/lgmd_info.py, Tacticool 1.05)

- Tacticool's pistol in the hand (Atek_H.bin, LGMD v4): 1033 polygons, 2 sub-objects (`Grip00c` and `@s00sli`, the
  slide, which the game moves when it fires), 6 materials, one of them the arm (`hrpistarm`): hiding the arm is
  "skip that material's polygons". About 20% of its edges are open (the cut end of the sleeve, seams).
- Tacticool's world pistol (Atek_W.bin): 474 polygons, 9% open edges.
- Tacticool's AR15 in the hand: 520 polygons, 38% open edges.
- The captured weapon in the probe runs was about 80 triangles per frame. The files hold far more than what reaches
  the device, so drawing the model from its file will show much more of it. The game's own models are inside the
  .crf archives (zip files) in Data/res.

### The original game's first-person models (Data/res/OBJ.CRF, tools/lgmd_info.py)

| model | polygons | open edges | | model | polygons | open edges |
|---|---|---|---|---|---|---|
| ATEK_H (pistol) | 79 | 45% | | FSN_H (fusion) | 126 | 7% |
| AR15_H (assault) | 126 | 38% | | AL_H (laser) | 111 | 16% |
| SG_H (shotgun) | 60 | 39% | | AMP_H (psi amp) | 114 | 24% |
| GREN_H (grenade l.) | 47 | 50% | | VIRO_H (viral prolif.) | 102 | 32% |
| EMPGUN_H (EMP rifle) | 59 | 53% | | SFG_H (stasis) | 54 | 32% |
| ES_H (electro shock) | 36 | 61% | | PIPE_H (pipe) | 8 | 67% |
| ATEK_W (world pistol) | 78 | 17% | | | | |

The original pistol is 79 polygons and the probe captured about 75 triangles of it per frame: the engine drew nearly
all of it, so the missing half of the vanilla pistol really is not modelled. Most originals are open shells built for
one view; the fusion cannon and laser are close to complete. Tacticool's are far more detailed (the pistol 1033).
Plan for originals: draw both sides of every polygon (an open shell then looks solid from most angles instead of
see-through), and per weapon fall back to the world model (more complete, e.g. ATEK_W 17% open) with the arm hidden.
