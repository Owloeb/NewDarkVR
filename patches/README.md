# Patches against NewDarkRecompiled

Applied to a checkout of `Owloeb/NewDarkRecompiled` at `1fd62bd` with `git apply patches/<name>.patch`.
The vanilla repo is never modified from here.

## m2-d3d9-stats.patch

Diagnostic only. Every 120 presented frames it logs (through `port_debug`) what the engine feeds the Direct3D 9 layer:

- draws per frame, split into pre-transformed (XYZRHW) and untransformed, and how many of each are depth-tested
- `SetTransform` calls per frame (view, projection, world), view and projection changes between draws, distinct projections
- the first untransformed draw's camera position, vertical fov, aspect and view matrix rows
- draws per frame for each FVF the engine uses

Build the port with `port/build.py --target windows`, run the game, walk and look around, and send the `M2:` log lines.

## m3a-camera-probe.patch

Diagnostic only (adds two fast paths that log and always decline, so behaviour is unchanged). Logs every call of
`render_scene_from_camera` (0x00601430: camera position, cell, bank/pitch/heading) and `render_frame_world` (0x006017d0)
for the first 20 calls and then every 120th, with a millisecond timestamp. Needs `--fastpaths` on the build and `--verbose` at run time.
Lines start with `M3a:`.
