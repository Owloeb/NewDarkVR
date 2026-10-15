/*
 * NewDarkVR: in-process OpenXR + OpenGL module for the 64-bit NewDarkRecompiled port.
 *
 * Plain C99, 64-bit Windows. Everything runs in the game's own process, so there is no IPC:
 * the renderer asks this module for a frame, renders into the two eye framebuffers it hands
 * out, and gives the frame back.
 *
 * THREADING: every call except ndvr_wants_exit() must be made from the thread that owns the
 * OpenGL context passed to ndvr_init(), with that context current. OpenXR's OpenGL binding
 * requires it. In the port that is the renderer thread.
 *
 * COORDINATES: all poses are OpenXR LOCAL space: right-handed, +Y up, -Z forward, meters.
 * Mapping to the Dark Engine frame is the integrator's job (settled when the camera is found).
 *
 * Typical frame:
 *
 *     ndvr_frame f;
 *     if (ndvr_begin_frame(&f)) {
 *         for (int e = 0; e < 2; ++e) {
 *             ndvr_bind_eye(&f.eye[e]);          // binds the eye FBO and sets the viewport
 *             render_scene(view_from(f.eye[e]), projection_from(f.eye[e]));
 *         }
 *         ndvr_unbind();
 *     }
 *     ndvr_end_frame();                          // always call, even if begin returned 0
 */
#ifndef NDVR_H
#define NDVR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ndvr_vec3 { float x, y, z; }            ndvr_vec3;
typedef struct ndvr_quat { float x, y, z, w; }         ndvr_quat;
typedef struct ndvr_fov  { float left, right, up, down; } ndvr_fov; /* radians; left and down are negative */

#define NDVR_POSE_ORIENT_VALID 0x1u
#define NDVR_POSE_POS_VALID    0x2u

typedef struct ndvr_pose {
    ndvr_quat orientation;
    ndvr_vec3 position;
    uint32_t  flags;                 /* NDVR_POSE_* */
} ndvr_pose;

#define NDVR_BTN_PRIMARY      0x1u   /* A (right) / X (left) */
#define NDVR_BTN_SECONDARY    0x2u   /* B (right) / Y (left) */
#define NDVR_BTN_STICK_CLICK  0x4u
#define NDVR_BTN_MENU         0x8u   /* left controller only */

#define NDVR_HAND_LEFT   0
#define NDVR_HAND_RIGHT  1

typedef struct ndvr_hand {
    ndvr_pose grip;                  /* where the hand is: use for held objects */
    ndvr_pose aim;                   /* where the controller points: use for rays */
    float     trigger;               /* 0..1 */
    float     squeeze;               /* 0..1 */
    float     stick_x, stick_y;      /* -1..1 */
    uint32_t  buttons;               /* NDVR_BTN_* */
    int       active;                /* nonzero if the controller is tracked */
} ndvr_hand;

typedef struct ndvr_eye {
    uint32_t  fbo;                   /* framebuffer to render this eye into (color + depth/stencil) */
    int       width, height;
    ndvr_pose pose;                  /* eye pose in LOCAL space */
    ndvr_fov  fov;                   /* off-center frustum from the runtime */
} ndvr_eye;

typedef struct ndvr_frame {
    int64_t   display_time;          /* XrTime the frame will be displayed at */
    int       should_render;
    ndvr_pose head;
    ndvr_eye  eye[2];                /* [0] = left, [1] = right */
    ndvr_hand hand[2];
} ndvr_frame;

/* ---- lifecycle -------------------------------------------------------------------------- */

/* Create the OpenXR instance, session and per-eye swapchains on the given GL context
 * (a Win32 HDC and HGLRC, passed as void*; the context must be current on this thread).
 * Needs a 64-bit openxr_loader.dll next to the exe and an active headset runtime.
 * Returns 1 on success; on failure returns 0 and ndvr_last_error() says why. */
int         ndvr_init(void *hdc, void *hglrc);
void        ndvr_shutdown(void);
const char *ndvr_last_error(void);

/* Optional: route log lines somewhere (default: stderr). */
void        ndvr_set_log(void (*fn)(const char *line));

/* 1 once the runtime has started the session (headset on and focused or visible). */
int         ndvr_session_running(void);
/* 1 while the session is focused: the headset is on and the app has input (Quest: headset worn, app in front). */
int         ndvr_focused(void);
/* 1 if the runtime asked the app to quit (user pressed Home -> Quit, headset lost, ...). */
int         ndvr_wants_exit(void);

/* ---- per frame -------------------------------------------------------------------------- */

/* Waits for the headset's frame timing, begins the frame, polls tracking and input, and
 * acquires both eye framebuffers. Returns 1 if the app should render this frame. */
int         ndvr_begin_frame(ndvr_frame *out);
/* Releases the eye images and submits the frame. Safe to call after begin returned 0. */
void        ndvr_end_frame(void);

/* Optional, call before ndvr_end_frame(): submit the frame with these eye poses and fovs instead of the ones from
 * ndvr_begin_frame(). Use it when the images were rendered for other poses/fovs than the runtime reported (the
 * compositor then reprojects correctly). Applies to one frame. */
void        ndvr_set_submit_views(const ndvr_pose pose[2], const ndvr_fov fov[2]);
/* Copy (and scale) a framebuffer you rendered elsewhere into the eye image acquired by ndvr_begin_frame().
 * src_fbo is a framebuffer id in the same GL context; flip_y flips vertically (for images stored top row first). */
void        ndvr_blit_eye(int eye, uint32_t src_fbo, int src_w, int src_h, int flip_y);

/* Optional, call between ndvr_begin_frame() (returned 1) and ndvr_end_frame(): show a flat panel (an OpenXR quad
 * layer) on top of the 3D view, e.g. the game's HUD or a menu. src_fbo/src_w/src_h is copied into the panel's own
 * image each call (flip_y as for ndvr_blit_eye). opaque = 0 blends with the image's alpha. pose is the panel centre in
 * LOCAL space, the visible side facing +Z of the pose; width_m x height_m its size in metres. cursor_x/cursor_y (image
 * pixels, top-left origin; -1 for none) draws a pointer cursor. A frame where no eye was written (no ndvr_blit_eye or
 * ndvr_bind_eye) shows only the panel, for menus. Applies to one frame. */
void        ndvr_panel_submit(uint32_t src_fbo, int src_w, int src_h, int flip_y, int opaque, const ndvr_pose *pose,
                              float width_m, float height_m, int cursor_x, int cursor_y);

/* Bind an eye's framebuffer and set the viewport to cover it. ndvr_unbind() restores the
 * framebuffer binding that was current when ndvr_bind_eye() was first called this frame. */
void        ndvr_bind_eye(const ndvr_eye *eye);
void        ndvr_unbind(void);

/* ---- math helpers (column-major, OpenGL convention) ------------------------------------- */

/* Model matrix for a pose (object-to-LOCAL), e.g. to draw something in a hand. */
void        ndvr_pose_to_matrix(const ndvr_pose *p, float out[16]);
/* View matrix for a pose (LOCAL-to-eye): the rigid inverse of the pose. */
void        ndvr_view_matrix(const ndvr_pose *p, float out[16]);
/* Off-center OpenGL projection (clip z in -1..1) from the runtime's fov. */
void        ndvr_projection_matrix(const ndvr_fov *fov, float znear, float zfar, float out[16]);

#ifdef __cplusplus
}
#endif

#endif /* NDVR_H */
