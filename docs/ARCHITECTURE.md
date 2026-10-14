# Architecture

## 1. In-process, on the port's GL context

The 64-bit port lifts the 32-bit x86 code to C and runs it inside a reserved 4 GB guest address space, so
the host process is a normal 64-bit program. That is why the OpenXR runtime, the GPU driver and the VR code can all live
in the same process, and why there is no bridge, no IPC and no shared textures.

The port's sdl2 backend renders through OpenGL (`gl_render.c`), and OpenXR has a native OpenGL binding
(`XR_KHR_opengl_enable`). So the module creates the OpenXR session on the renderer's existing GL context,
creates one swapchain per eye, and wraps every swapchain image in a framebuffer with a depth/stencil
renderbuffer. The renderer draws into those framebuffers instead of (or as well as) the window.

File and function names for the port in this document (`gl_render.c`, `plat_sdl2.c`, `port/dx/d3d9.c`) come from the
port's author and have not been checked against the code from this repo yet.

## 2. Rules the module relies on

- **Threading.** Every `ndvr_*` call (except `ndvr_wants_exit`) runs on the thread that owns the GL context,
  with it current. OpenXR's OpenGL binding requires this. In the port that is the renderer thread, which
  has its own context, which is a good fit for OpenXR's frame loop.
- **Frame pacing.** `ndvr_begin_frame` blocks in `xrWaitFrame`, so the headset paces the renderer. Desktop
  vsync must be off for the VR path or the two will fight.
- **Color.** The swapchain is `GL_SRGB8_ALPHA8` and `GL_FRAMEBUFFER_SRGB` stays off. The engine writes
  gamma-encoded values, and an sRGB swapchain makes the compositor decode them correctly.
- **GL state.** `ndvr_bind_eye` / `ndvr_unbind` save and restore the framebuffer binding. The module otherwise
  leaves GL state alone; the integrator owns everything else.
- **Coordinates.** All poses are OpenXR LOCAL space (right-handed, +Y up, -Z forward, meters). Converting to the
  Dark Engine's frame and scale is part of M2/M3.

## 3. Stereo strategy (M3)

The game thread (the engine, lifted to C) produces one camera and one stream of draw calls per frame, and that thread is
the single-threaded bottleneck. VR needs about 90 fps with two views, so the design goal is: **run the engine
once per frame, and draw its output twice.**

The port's D3D9 layer (`port/dx/d3d9.c`) sees every transform and draw call the engine makes. The plan is
to record the frame's world draw stream there and replay it once per eye with different view and
projection matrices, using `ndvr_view_matrix` and `ndvr_projection_matrix` on the eye poses. That keeps
the expensive engine work (portal culling, object lists, script updates) at one pass per frame, and only the GL
replay doubles.

Everything depends on one fact to check first (the M2 spike): **does the engine submit world-space vertices
with real view/projection transforms, or pre-transformed (XYZRHW) screen-space vertices?**

- Real transforms (`SetTransform` view/projection, untransformed FVF): replay with swapped matrices works.
- Pre-transformed: there is no view matrix to swap. The only option is to re-run the engine's own render per
  eye with a modified camera, which costs two engine render passes per frame.

The spike is small: log, per frame, which D3D9 transform and vertex-format calls the engine makes, and how many draws are
pre-transformed.

Other open points:

- **Asymmetric fov.** OpenXR gives off-center frusta. If the engine bakes a symmetric projection into
  culling, cull with a widened symmetric frustum and draw with the exact per-eye projection.
- **2D and HUD.** Menus, HUD and inventory are drawn in screen space. Split them from the world pass in the
  recorded stream; M6 renders them to a texture shown on a quad layer.
- **World scale.** Establish the engine-units-to-meters ratio early. A wrong scale feels wrong even when
  tracking is perfect.
- **Headroom.** Before any of this, run the port uncapped on the PC and see how far above 90 fps the game
  thread gets.

## 4. Integration surface with NewDarkRecompiled

NewDarkVR should stay out of the vanilla repo. The module compiles into the port as a few extra C files; what the
port has to expose is small, and M2 will pin it down:

| Need | Why |
|---|---|
| a per-frame begin/end callback on the renderer thread | `ndvr_begin_frame` / `ndvr_end_frame` |
| a way to override the world view/projection in the D3D9 layer | per-eye replay |
| access to the engine's camera position and orientation | head tracking, decoupling head from body |
| input injection (mouse position, buttons, movement) | laser-pointer cursor for the inventory, controller mapping |
| a build flag to add VR sources | keeps the default build vanilla |

## 5. Milestone checks

- **M0/M1:** `ndvr_gltest.exe` shows correct stereo depth, the hands, and a stable world while the head turns.
- **M2:** a log line per frame with the camera position and orientation while you walk and look around,
  plus the D3D9 transform/format statistics above.
- **M3:** correct depth in both eyes with the head still, in the actual game.

## 6. Runtime compatibility

The module speaks plain OpenXR, so any conformant runtime works as long as it offers a graphics binding the port can
use. Today that is OpenGL (`XR_KHR_opengl_enable`), because the port renders through GL. `ndvr_init` logs the active
runtime (registry `ActiveRuntime`, `XR_RUNTIME_JSON` override, and the runtime's reported name and version) and every
`XR_KHR_*_enable` extension it offers, and fails early with a specific message if OpenGL is missing.

Virtual Desktop streams through two OpenXR paths: SteamVR as the runtime, or its own runtime (VDXR). Verified:
VDXR 1.0.10 offers `XR_KHR_opengl_enable` alongside D3D11, D3D12 and Vulkan, and the stereo test ran at 100 fps
on it, so no fallback is needed for that setup.

Fallback if a runtime is D3D11-only: keep rendering in OpenGL, create the OpenXR session on a D3D11 device, and copy
each eye through `WGL_NV_DX_interop2` (register the D3D11 swapchain texture as a GL texture for the duration of the
frame). That keeps the engine side unchanged and works on any runtime that offers D3D11. The `ndvr.h` API would not
change; only `src/ndvr_xr.c` grows a second backend.
