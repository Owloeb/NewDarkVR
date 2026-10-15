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
