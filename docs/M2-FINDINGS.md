# M2 findings: what the port's Direct3D 9 layer allows

Read from `port/dx/d3d9.c` at NewDarkRecompiled `1fd62bd`. The runtime numbers (how many draws are pre-transformed,
which FVFs, what the camera looks like) still need one logged run with `patches/m2-d3d9-stats.patch`.

## What the code already guarantees

1. **Both vertex paths exist.** `vertex()` has an explicit pre-transformed branch (`L->rhw`, FVF `XYZRHW`) that maps
   screen coordinates straight to clip space, and an untransformed branch that multiplies by `world * view * proj`
   on the CPU. So whether stereo replay works is decided by which branch the engine's world geometry takes, not by
   the port.
2. **The transforms are plain device state.** `SetTransform` (view = 2, projection = 3, world = 256) writes
   `Device.st.view/proj/world`. `prepare()` multiplies them into `Xf.wv` and `Xf.wvp` once per draw.
3. **Every draw is already a self-contained record.** In the common case `draw()` copies the raw guest vertices,
   the expanded 16-bit indices, the layout, the `Xf` (including `wv`, `wvp`, lights) and a `VState` snapshot into
   a `Deferred` blob, and `deferred_draw` runs `vertex()` later on the render thread. That is almost exactly a
   replay record: to draw a second eye, re-run `vertex()` on the same blob with a different `Xf`.
4. **View-space effects are covered.** Lighting, fog distance and camera-space texture coordinates all derive from
   `Xf.wv`, so an eye offset in the view matrix carries through them with no extra work.
5. **Frame boundary.** `dev_present` calls `plat_gfx_present()` then `plat_gfx_begin_frame()`. That is the hook for
   `ndvr_begin_frame` / `ndvr_end_frame`.

## Consequences for the design

- The record/replay plan in `ARCHITECTURE.md` section 3 is viable on the port side. The cheap implementation is in
  `draw()`: for an untransformed draw, build the `Xf` twice (left and right `view`/`proj` composed with the engine
  `world`) and submit the same blob to two targets, instead of capturing and replaying a separate stream.
- A pre-transformed draw cannot be stereoised this way. If the engine sends the 3D world as XYZRHW (the
  `rhw_depth` counter in the log tells), the plan changes to rendering the engine twice with a modified camera.
- Draws with `rs[7]` (Z enable) off and XYZRHW are almost certainly HUD and menus: they go to the panel, not both eyes.
- Weapons and first-person models are probably drawn with their own projection (`distinct proj > 1` or
  `proj changes > 0` in the log). Those draws need their own handling in M5.

## Measured (Quest-free PC run, level loaded, 2560x1440)

```
in a level: draws/frame 74-138, pretransformed 100%, depth-tested 100%, untransformed 0
FVF 0x2C4 (XYZRHW, diffuse, specular, 2 tex sets): 46-99 draws/frame
FVF 0x1C4 (XYZRHW, diffuse, specular, 1 tex set):  12-53 draws/frame
menus/cutscenes: 2-3 draws/frame, FVF 0x144, not depth-tested
SetTransform: 3 view/proj/world calls per 120 frames, never used by a draw
```

**Verdict: the world is pre-transformed.** The engine does its own transform, clipping and projection on the CPU and
sends screen-space vertices, so there is no view or projection matrix in the D3D9 layer to swap per eye. The cheap
replay plan is dead. The two options left:

- **A. Re-run the engine's world render per eye with a modified camera (chosen).** `symbols/manual.sym` names the
  entry points: `render_scene_from_camera` (0x00601430) takes `{float x,y,z; u32 cell; u16 bank,pitch; u16 heading}`,
  and `render_frame_world` (0x006017d0) builds that from the player camera object at `*0xa3acb8`. A hook (see
  NewDarkRecompiled `docs/MODDING.md`) can call the scene render twice per frame with per-eye camera values. This also gives
  correct culling and an exact per-eye field of view, and head/body decoupling falls out of the same camera struct.
- **B. Un-project the pre-transformed vertices back to camera space** using the known focal length and re-project per
  eye. Rejected: the engine already clipped to the mono frustum, so the eye edges would be missing.

Costs and risks of A: the engine's render path runs twice per frame (it is CPU-side, measure it), per-frame
side effects inside the render function must be found and not doubled, and the camera `cell` must stay valid when
the eye offset crosses a portal (rare at IPD scale; do a cell lookup when it does).

## Still unknown

- the engine's near/far planes, fov and aspect (`proj` in the log)
- engine units per metre (compare camera movement per step with a known distance)
- whether any engine-side culling uses a frustum that must be widened for the headset's field of view

## M3a probe result

From a real play session (`patches/m3a-camera-probe.patch`):

- `render_frame_world` and `render_scene_from_camera` run exactly once per frame, in lockstep (about 97 per second at 2560x1440).
- The camera struct sits on the guest stack (eax = `0x0312fccc`, a local of `render_frame_world`) and matches the layout in
  `symbols/manual.sym`: floats x, y, z, then a cell id, then u16 bank, pitch, heading. Heading 16384 = 90 degrees (65536 = 360);
  pitch and bank are signed. `cell` is `0xFFFFFFFF` at entry, so the engine finds the cell itself and a sideways eye offset
  needs no cell lookup.
- Moving forward facing 90 degrees increased y, so forward = (cos heading, sin heading) and right = (sin heading, -cos heading).
- z sat near 7.0 to 7.7 while standing, which suggests one engine unit is about a foot (to confirm).
- The function also takes stack arguments (a double, 1.0). The recompiled code ignores return addresses, so a hook can call
  the lifted function itself and then restore the CPU state, without knowing the argument count.

## M3b result: two scene renders per frame work

First run of the stereo preview (`patches/m3b-stereo-preview.patch`, a real level, 2560x1440, RTX 4070 Super):

- Both eye renders came out correct and complete. Nothing doubled, flickered or went missing in the world view.
- CPU time for the two scene renders: 2.4 to 7.2 ms per frame (about 5 ms typical, up to 374 draws per frame). The base game ran at about 97 fps, so VR's 90 fps budget fits.
- The first build had the eyes swapped. Cross-correlating the two halves of a screenshot gave a shift of +104 px (the
  right-eye image moved right, a right eye should see it move left). Fix: the camera's sideways axis is (-sin h, cos h).

## Calibration result (NDVR_CALIB, 2560x1440, zoom 0.5)

Measured by rendering the scene turned, tilted and shifted and matching the vertices (hundreds per test, consistent across runs):

- **Projection:** square pixels; focal length 479.5 px at zoom 0.5, so 959 px at zoom 1 = canvas height / 1.5. That is
  90 degrees across a 4:3 frame, widened for wider screens (106.3 x 73.7 degrees at 16:9). The zoom argument of the scene
  render (a double, 1.0 normally) divides the field of view's tangent, so the engine can render as wide as a headset needs.
- **Heading:** increasing turns LEFT (counter-clockwise from above). Forward = (cos h, sin h).
- **Pitch:** increasing looks DOWN.
- **Sideways:** the camera's right-hand axis is (sin h, -cos h). The earlier "eyes swapped" fix from the side-by-side
  screenshot was wrong: the halves were cropped from a guessed window position, which faked the shift. Reverted.
- Bank (roll) direction is not measured yet.

## Sharpness: eye textures at the headset's resolution

The engine's projected frame is symmetric and 16:9, while each Quest eye is taller than wide and off-centre. Instead of
stretching a 16:9 canvas, each eye now renders into its own texture at the runtime's recommended size (2496x2688 on
Quest 3 via VDXR), covering exactly that eye's field of view. `vr_d3d_view_map` in `d3d9.c` maps the engine's canvas
pixels into that texture (a GL viewport larger than the texture, offset so the frame's tangent space lines up with the
eye's); the GPU clips what falls outside, so only visible pixels are filled. The layer is submitted with the eye's own
field of view. `NDVR_RES` scales the eye textures (supersampling above 1.0).
