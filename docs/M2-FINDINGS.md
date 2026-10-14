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
