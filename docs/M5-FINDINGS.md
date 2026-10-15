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
