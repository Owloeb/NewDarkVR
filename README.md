# NewDarkVR

A VR mod for **System Shock 2 (NewDark 2.48)**, built on the 64-bit
[NewDarkRecompiled](https://github.com/Owloeb/NewDarkRecompiled) port (`port/build.py --target windows`).
Target: OpenXR on Meta Quest 3 over PC link, head tracking decoupled from the body, tracked hands and
weapons, a left-handed mode, and the HUD on a floating panel you point at.

This repository holds **only VR code**. The recompiler and the vanilla port stay in NewDarkRecompiled.
No game content or game-derived code is in this repo.

## Try it (about a minute, no game needed)

1. Connect your Quest with Quest Link or Air Link and make sure Quest Link is the active OpenXR runtime
   (Meta Quest Link app, Settings, General).
2. Download **`NewDarkVR-test-win64.zip`** from the [Releases](../../releases) page and unzip it anywhere.
3. Run `ndvr_gltest.exe` and put the headset on.

You should see a spinning cube 1.5 m ahead with real depth, a floor grid, and a cube in each hand (blue left,
orange right; the trigger makes it grow). Turning your head must not move the world. Esc quits. If it fails,
the console window says which OpenXR call failed and why.

No release yet? Every push is built by GitHub Actions: open the latest run under the Actions tab and download the
`NewDarkVR-test-win64` artifact.

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
| M0 | OpenXR session on the port's GL context, tracking and input | module written and compiles; needs a headset test |
| M1 | Standalone stereo scene in the headset (cube, grid, hands) | `ndvr_gltest.exe`, same state |
| M2 | Find the camera and the per-frame view/projection in the port | not started |
| M3 | Stereo: draw the world once per eye into the eye framebuffers | not started |
| M4 | Head decoupled from body, snap turn, physical crouch | not started |
| M5 | Tracked hands and weapons, aim and frob from the controller, handedness option | not started |
| M6 | HUD and inventory on a pointable panel | not started |

The binaries are cross-compiled on Linux and have not been run on a Windows machine with a headset yet. The first
run is the real test, so please report what the console prints.

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
