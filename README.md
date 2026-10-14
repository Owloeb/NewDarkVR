# NewDarkVR

A VR mod for **System Shock 2 (NewDark 2.48)**, built on the 64-bit
[NewDarkRecompiled](https://github.com/Owloeb/NewDarkRecompiled) port (`port/build.py --target windows`).
Target: OpenXR on Meta Quest 3 over PC link, head tracking decoupled from the body, tracked hands and
weapons, a left-handed mode, and the HUD on a floating panel you point at.

This repository holds **only VR code**. The recompiler and the vanilla port stay in NewDarkRecompiled.
No game content or game-derived code is in this repo.

## Try it (about a minute, no game needed)

1. Connect your headset and make sure the active OpenXR runtime is one that offers OpenGL
   (see [Runtimes](#runtimes) below: Quest Link, SteamVR, or Virtual Desktop).
2. Download **`NewDarkVR-test-win64.zip`** from the [Latest build](../../releases/tag/latest) release (rebuilt
   automatically on every push to `main`) and unzip it anywhere. Tagged versions are on the
   [Releases](../../releases) page.
3. Run `ndvr_gltest.exe` and put the headset on.

You should see a spinning cube 1.5 m ahead with real depth, a floor grid, and a cube in each hand (blue left,
orange right; the trigger makes it grow). Turning your head must not move the world. Esc quits. If it fails,
the console window says which OpenXR call failed and why.

No release yet? Every push is built by GitHub Actions: open the latest run under the Actions tab and download the
`NewDarkVR-test-win64` artifact.

## Runtimes

The mod talks standard OpenXR, so it is not tied to one headset app. It needs a runtime that offers the OpenGL
graphics binding (`XR_KHR_opengl_enable`), because the port renders with OpenGL.

| Setup | How to select the runtime | OpenGL |
|---|---|---|
| Quest Link / Air Link | Meta Quest Link app, Settings, General | expected to work |
| SteamVR (also what Virtual Desktop streams) | SteamVR, Settings, OpenXR, set as runtime | expected to work |
| Virtual Desktop's own runtime (VDXR) | Virtual Desktop Streamer, Settings, OpenXR runtime | **works** (VirtualDesktopXR 1.0.10, NVIDIA GL 4.6, Quest 3, 100 fps) |

Virtual Desktop supports OpenXR through its own runtime, VDXR, alongside the SteamVR path, and VDXR offers OpenGL.
`ndvr_gltest.exe` prints the active runtime, its name and version, and every graphics API it offers, and if OpenGL
is missing it says so and what to change. If your setup behaves differently, open an issue with that output.

If a runtime turns out to be D3D11-only, the planned fix is to render in OpenGL and hand the frames to a D3D11
swapchain through `WGL_NV_DX_interop2`, so the mod works on any runtime.

## Build from source

You only need Python 3. Everything else, including the OpenXR loader, is built for you.

```powershell
git clone https://github.com/Owloeb/NewDarkVR
cd NewDarkVR
.\install.ps1 -Run
```

`install.ps1` installs the Zig compiler from pip, runs `python build.py`, checks that an OpenXR runtime is
registered, and starts the test. `build.py` fetches the OpenXR SDK source at a pinned release tag from Khronos and builds
`openxr_loader.dll` from it, builds the test app, and packs both into `build\NewDarkVR-test-win64.zip`.
No binaries are downloaded.

## Status

| Milestone | What | State |
|---|---|---|
| M0 | OpenXR session on a GL context, tracking and input | **done, verified on Quest 3 via Virtual Desktop** |
| M1 | Standalone stereo scene in the headset (cube, grid, hands) | **done, verified** (`ndvr_gltest.exe`, 100 fps) |
| M2 | Find the camera and the per-frame view/projection in the port | **done** ([findings](docs/M2-FINDINGS.md)): the world is pre-transformed, so M3 re-runs the engine's scene render per eye |
| M3 | Stereo: render the scene once per eye (hook `render_scene_from_camera`) into the eye framebuffers | **done**, side-by-side preview verified on the desktop (about 5 ms CPU for both eyes) |
| M3c | Headset output: eye textures to OpenXR, head-tracked camera | **working on Quest 3** (stereo verified; yaw/pitch signs fixed); view is still letterboxed to the engine's 16:9 field of view |
| M4 | Head decoupled from body, snap turn, physical crouch | not started |
| M5 | Tracked hands and weapons, aim and frob from the controller, handedness option | not started |
| M6 | HUD and inventory on a pointable panel | not started |

The binaries are cross-compiled on Linux. They have been run on one setup so far (Quest 3, Virtual Desktop's VDXR
runtime, NVIDIA GPU). Quest Link and SteamVR are untested, so reports are welcome.

## What's here

```
include/ndvr.h        the whole API: init, begin_frame, bind_eye, end_frame, poses, input, matrix helpers
src/ndvr_xr.c         OpenXR (XR_KHR_opengl_enable) session, per-eye swapchains and FBOs, input actions
tools/ndvr_gltest.c   standalone test app: draws a test scene in both eyes, no game needed
docs/ARCHITECTURE.md  design, threading rules, stereo plan and integration surface for the port
install.ps1           one-step build and run on Windows
build.py              builds the loader, the test app and the release zip (Zig)
third_party/openxr/   vendored OpenXR headers (Apache-2.0)
.github/workflows/    CI: builds on every push, attaches the zip to a Release when you tag vX.Y.Z
```

The module runs inside the game process: the port is a real 64-bit executable, so the OpenXR runtime loads
directly and there is no IPC. The engine renders each eye into a framebuffer the module hands out.

## Releasing

Tag a commit and push the tag; CI builds and publishes the zip:

```powershell
git tag v0.1.0
git push origin v0.1.0
```

## Legal

The code here is MIT licensed (see LICENSE). The OpenXR headers and loader are Copyright The Khronos Group Inc.
and licensed Apache-2.0 (the license ships inside the release zip). You need your own legitimate copy of System
Shock 2. Nothing derived from the game binary is distributed from this repository.
