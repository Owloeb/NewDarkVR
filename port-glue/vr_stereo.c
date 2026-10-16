/* NewDarkVR: stereo rendering and headset output for the NewDarkRecompiled port. Not part of the vanilla port;
 * tools/install_into_port.py copies this file into port/core/.
 *
 * The engine transforms and projects the world itself and hands the Direct3D layer screen-space vertices, so there is
 * no view matrix to change per eye. Instead the engine's own scene render (render_scene_from_camera, 0x00601430,
 * eax -> { float x, y, z; u32 cell; u16 bank, pitch, heading }) runs twice per frame, once per eye, from a camera
 * built from the eye's pose, into two render-target textures.
 *
 *   NDVR_STEREO=1   side-by-side preview in the window, camera moved sideways by NDVR_IPD engine units (default 0.207)
 *   NDVR_VR=1       headset: OpenXR session on the renderer's GL context, head-tracked camera (see below)
 *   NDVR_CALIB=1    desktop measurement, no headset: every 4 seconds renders the scene three extra times (turned, tilted,
 *                   moved sideways) and compares where the vertices land, which gives the engine's focal length (so its
 *                   field of view) and the true direction of heading, pitch and the sideways axis. Logs NDVR CALIB lines.
 *   NDVR_ZOOM=f     multiplies the zoom the engine passes to the scene render (1.0 normally); works in every mode
 *
 * Headset camera. The body's facing is our own (it starts as the engine's heading and changes with snap turn); the
 * head turns relative to it: camera heading = body facing + head yaw since the last recentre; pitch and roll come
 * from the head; eye position = engine eye position + the head's physical movement since the recentre, turned by the
 * body facing. Recentre: automatic on the first frame and whenever the headset goes on (session focus gained), and
 * with the left controller's menu button.
 *
 * Gun hand (NDVR_HAND=left or right, default right). The engine's own mouse look follows where it points: every
 * frame the difference between the engine's heading/pitch and the hand's aim is sent as mouse motion (32 heading /
 * 48 pitch units per count, measured; 2 frames in flight are accounted for), so shots, the muzzle flash and the gun
 * all aim with the controller, and the game's first-person weapon is drawn in that hand (mirrored for the left hand).
 * Walking with the keyboard is relative to that aim for now. Controls on that hand:
 *   trigger = left mouse button (fire; click in the inventory and menus), A / X = right mouse button,
 *   B / Y = Tab (inventory / cursor mode), thumbstick left/right = snap turn.
 * HUD panel. The game draws its HUD into a texture of its own and pastes it over the 3D view; in the headset that
 * texture is a curved panel (flat if the runtime has no cylinder layers) around your head, like a helmet display: it
 * follows where you look with a little lag, and the game's own crosshair is hidden (the gun aims, not the head). Menus
 * and videos (no 3D view) show the whole screen on the same panel. When the game shows a cursor (inventory, menus)
 * the panel holds still and the gun hand points at it: a pointer dot shows where, and the game's cursor follows.
 *
 * Tuning (environment variables, all optional):
 *   NDVR_UNIT_M      metres per engine unit (default 0.3048, a foot)
 *   NDVR_FOV_H       override the horizontal field of view in degrees the engine is assumed to render with
 *                    (default: computed from the measured projection, see below)
 *   NDVR_FOV_V       override the vertical field of view in degrees (default: from the canvas aspect ratio)
 *   NDVR_FLIP_YAW / NDVR_FLIP_PITCH / NDVR_FLIP_ROLL   set to 1 to reverse that axis
 *   NDVR_FLIP_SIDE   set to 1 to reverse sideways head movement (and so swap the eyes)
 *   NDVR_FLIP_Y      set to 0 if the headset image is upside down (default 1)
 *   NDVR_NO_POS      set to 1 to ignore the head's physical movement (rotation only)
 *   NDVR_RES         eye resolution relative to the headset's recommended size (default 1.0, 0.25..2.0)
 *   NDVR_HAND        left or right: the gun hand (default right)
 *   NDVR_AIM         0 to keep aiming with the mouse
 *   NDVR_AIM_UNITS_H / NDVR_AIM_UNITS_V   engine units per mouse count (default 32 / 48, the game's default sensitivity)
 *   NDVR_SNAP        snap turn in degrees (default 30)
 *   NDVR_HUD_ANGLE   how wide the HUD is, in degrees of your view (default 80)
 *   NDVR_HUD_DIST    its distance in metres (default 1.2)
 *   NDVR_HUD_FOLLOW  how fast it follows your head per frame, 0..1 (default 0.1; 1 = fixed to the head, 0 = stays put)
 *   NDVR_HUD_CROSSHAIR  1 to keep the game's crosshair in the HUD
 *   NDVR_HUD_PULL / NDVR_HUD_LIFT   in play, the HUD's left and right edges (health, ammo) are moved this many degrees
 *                    in towards the middle / up (default 12 / 6)
 *   NDVR_GUN         0 to leave the weapon where the game draws it
 *   NDVR_GUN_MODE    how the weapon gets into the hand:
 *                      engine (default): the engine draws its own first-person model (arm included with the 25th
 *                        Anniversary models, tools/convert_remaster.py) at the hand in each eye, whole, with its own
 *                        materials, lighting, recoil and reload (see "the weapon drawn by the engine")
 *                      capture: the first-person draws of one extra render, moved to the hand (older; only what the
 *                        engine shows from its own view, so parts are missing)
 *                      world: the weapon's floor model (needs vr/weapons.vrw from tools/extract_weapons.py)
 *   NDVR_GUN_X / NDVR_GUN_Y / NDVR_GUN_Z   move the weapon in the hand, in cm (right, up, forward; default 0)
 *   NDVR_GUN_SCALE   weapon size (default 1)
 *   NDVR_GUN_MIRROR  0 to keep the left hand's weapon unmirrored (default 1: mirrored, as the right hand's in a mirror)
 *   NDVR_ZERO        metres at which shots cross the world model's barrel line (default 10; 0 = along the controller)
 *   NDVR_GUN_ONTOP   1 to draw the weapon over walls instead of letting them hide it (world mode; the engine's own
 *                    weapon is always drawn over the world, as in the game)
 *   NDVR_HUD_SPLIT   how much of each side moves, as a fraction of the HUD's width (default 0.21; 0 = keep it whole)
 * Run with SS2PORT_NOVSYNC=1 so the desktop window does not pace the headset. */
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "core.h"
#include "plat.h"
#include "ndvr.h"

extern void nd_00601430(CPU *c);                                  /* the recompiled function itself */
extern void vr_d3d_eye_begin(PlatTexture *t, int w, int h);       /* port/dx/d3d9.c */
extern void vr_d3d_view_map(int on, float ox, float oy, float kx, float ky);
extern void vr_d3d_restore(void);
extern unsigned vr_gl_tex_fbo(PlatTexture *t, int *w, int *h);    /* port/backends/sdl2/gl_render.c */
extern void *__stdcall wglGetCurrentDC(void);                      /* opengl32 */
extern void *__stdcall wglGetCurrentContext(void);
extern void glt_call(void (*fn)(void *), void *arg);
extern int vr_cap_on, vr_cap_n, vr_cap_max; extern float *vr_cap_buf;   /* port/dx/d3d9.c */              /* port/backends/sdl2/gl_thread.c: run on the render thread, wait */

#ifndef NDVR_BUILD
#define NDVR_BUILD "unknown (copied by hand)"   /* tools/install_into_port.py puts the NewDarkVR commit here */
#endif
#define CAM_COPY 0x00a57e00u                                      /* the engine's copy of the render camera (24 bytes used) */
#define PI_D 3.14159265358979323846
static float rdf(uint32_t a) { float f; uint32_t u = RD32(a); memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); WR32(a, u); }
static uint16_t ang16(double rad) { return (uint16_t)((int)floor(rad * (65536.0 / (2.0 * PI_D)) + 0.5) & 0xFFFF); }

static int envi(const char *n, int d) { const char *e = getenv(n); return e && *e ? atoi(e) : d; }
static double envd(const char *n, double d) { const char *e = getenv(n); return e && *e ? atof(e) : d; }

typedef struct { double x, y, z; } V3;
static V3 v3(double x, double y, double z) { V3 r = { x, y, z }; return r; }
static double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static V3 norm(V3 a) { double l = sqrt(dot(a, a)); return l > 1e-9 ? v3(a.x / l, a.y / l, a.z / l) : a; }
static V3 qrot(ndvr_quat q, V3 v) {
    V3 u = v3(q.x, q.y, q.z), t = cross(u, v); t = v3(2 * t.x, 2 * t.y, 2 * t.z);
    V3 c = cross(u, t); return v3(v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z);
}
static double wrap_pi(double a) { while (a > PI_D) a -= 2 * PI_D; while (a < -PI_D) a += 2 * PI_D; return a; }

/* ---------------------------------------------------------------- state */
static PlatTexture *eye_tex[2]; static int tex_w, tex_h;
static PlatTexture *hmd_tex[2]; static int hmd_w[2], hmd_h[2]; static double res_scale = 1.0;   /* headset: one texture per eye at the headset's size */
static int busy, mode = -1;                                       /* 0 off, 1 side-by-side preview, 2 headset */
static float ipd = 0.207f;
static double unit_m = 0.3048, fov_h = 90, fov_v = 0;
static int flip_yaw, flip_pitch, flip_roll, flip_side, flip_y = 1, no_pos;
static unsigned frames; static clock_t acc;
static double zoom_mul = 0; static int zoom_warned;

/* headset */
static int xr_state;                                              /* 0 not tried, 1 running, -1 failed */
static ndvr_frame fr; static int fr_ok;
static int ref_valid, was_focused; static double ref_yaw; static V3 ref_pos; static uint32_t prev_btn;
static void init_env(void);
/* controls (see the header) */
static int hand_dom = 1, aim_on = 1, hud_crosshair; static double aim_uh = 32, aim_uv = 48, snap_deg = 30, hud_radius = 1.2, hud_angle = 80, hud_follow = 0.1, hud_split = 0.21, hud_pull = 12, hud_lift = 6;
static double body_yaw; static int body_valid, snap_armed = 1;   /* the body's facing in the game: snap turn; the engine's own heading follows the gun */
static int pend_h[2], pend_v[2];                                  /* aim: heading / pitch units sent as mouse motion in the last two frames */
static ndvr_pose panel_pose; static int panel_valid; static double pan_yaw, pan_pitch; static V3 pan_pos; static int pan_curved;
static int eng_h, eng_p;                                          /* the engine camera's heading and pitch at the last scene render */
static double ptr_u = -1, ptr_v = -1;
static int frame_state;                                           /* 0: no headset frame yet this game frame, 1: begun by the scene render, 2: already closed */
static int ptr_gx = -1, ptr_gy = -1; static uint32_t btn_was[2]; static int trig_was;
extern PlatTexture *vr_hud_tex; extern int vr_hud_w, vr_hud_h, vr_hud_seen;   /* port/dx/d3d9.c */
extern unsigned vr_gl_bb_fbo(int *w, int *h);                     /* port/backends/sdl2/gl_render.c */
extern int vr_input_captured(void), vr_input_mouse_motion(int dx, int dy), vr_input_mouse_abs(int x, int y);   /* port/win32/user32.c */
extern void vr_input_mouse_button(int button, int down), vr_input_key(int dik, int down);
extern int vr_gun_nv, vr_gun_near, vr_gun_draws; extern float vr_gun_bmin[3], vr_gun_bmax[3];   /* port/dx/d3d9.c */
extern void vr_d3d_gun_pass(int mode, float f, float cx, float cy);
extern void vr_d3d_gun_replay_eye(PlatTexture *t, float ox, float oy, float kx, float ky, int W, int H, const float *m, int mirror, float f);
static float eye_map[2][4]; static float gun_f_eye; static int gun_W, gun_H;
#define VR_GUN_MAXD 32
extern int vr_gun_dtw[VR_GUN_MAXD], vr_gun_dth[VR_GUN_MAXD], vr_gun_dtris[VR_GUN_MAXD]; extern double vr_gun_mom[10], vr_gun_rgb[4];
static double gun_zero_m = 10; static int gun_ontop, gun_use_world, eng_mode = 1, gun_mirror_on = 1;   /* eng_mode: 1 engine, 2 capture, 3 world */
static float gun_m[12]; static int gun_mirror;
static int gun_enabled = 1, gun_ready, gun_frames, gun_ref_nv, gun_odd; static double gun_scale = 1, gun_off[3], gun_sum[3], gun_g0[3];

static void xr_log(const char *line) { port_debug("NDVR: %s", line); }
static void rt_init(void *a) {
    (void)a; ndvr_set_log(xr_log);
    if (ndvr_init(wglGetCurrentDC(), wglGetCurrentContext())) xr_state = 1;
    else { xr_state = -1; port_debug("NDVR: headset not available: %s", ndvr_last_error()); }
}
static void rt_begin(void *a) { (void)a; fr_ok = ndvr_begin_frame(&fr); }

typedef struct { ndvr_pose pose[2]; ndvr_fov fov[2]; int w[2], h[2]; int mirror; PlatTexture *tex[2]; int flip; int rendered;
                 int panel; PlatTexture *ptex; int hole, cx, cy, nparts; ndvr_panel_part part[3]; } Submit;   /* panel: 0 none, 1 HUD texture, 2 back buffer */
static void rt_submit(void *data, PlatDrawFn draw) {
    Submit *s = data; (void)draw;
    if (s->rendered) {
        for (int e = 0; e < 2; e++) {
            int w, h; unsigned fbo = vr_gl_tex_fbo(s->tex[e], &w, &h);
            ndvr_blit_eye(e, fbo, w, h, s->flip);
        }
        ndvr_set_submit_views(s->pose, s->fov);
    }
    if (s->panel) {
        int w, h; unsigned fbo = s->panel == 1 ? vr_gl_tex_fbo(s->ptex, &w, &h) : vr_gl_bb_fbo(&w, &h);
        ndvr_panel_submit_parts(fbo, w, h, s->flip, s->panel == 2, s->hole, s->cx, s->cy, s->part, s->nparts);
    }
    ndvr_end_frame();
}

/* ---------------------------------------------------------------- camera from a head pose */
typedef struct { double yaw, pitch, roll; V3 fwd; } HeadAngles;
static HeadAngles head_angles(ndvr_quat q) {
    HeadAngles a; V3 f = qrot(q, v3(0, 0, -1)), u = qrot(q, v3(0, 1, 0)), r = qrot(q, v3(1, 0, 0));
    a.fwd = f; a.yaw = atan2(-f.x, -f.z); a.pitch = asin(f.y < -1 ? -1 : f.y > 1 ? 1 : f.y);
    V3 r0 = norm(cross(f, v3(0, 1, 0))), u0 = cross(r0, f);
    a.roll = atan2(-dot(r, u0), dot(r, r0)); (void)u;
    return a;
}


static void init_env(void) {
    mode = envi("NDVR_VR", 0) ? 2 : envi("NDVR_CALIB", 0) ? 3 : envi("NDVR_STEREO", 0) ? 1 : 0;
    zoom_mul = envd("NDVR_ZOOM", 0);
    const char *i = getenv("NDVR_IPD"); if (i && *i) ipd = (float)atof(i);
    unit_m = envd("NDVR_UNIT_M", 0.3048); fov_h = envd("NDVR_FOV_H", 0); fov_v = envd("NDVR_FOV_V", 0);
    flip_yaw = envi("NDVR_FLIP_YAW", 0); flip_pitch = envi("NDVR_FLIP_PITCH", 0); flip_roll = envi("NDVR_FLIP_ROLL", 0); flip_side = envi("NDVR_FLIP_SIDE", 0);
    flip_y = envi("NDVR_FLIP_Y", 1); res_scale = envd("NDVR_RES", 1.0); if (res_scale < 0.25) res_scale = 0.25; if (res_scale > 2.0) res_scale = 2.0; no_pos = envi("NDVR_NO_POS", 0);
    if (mode || zoom_mul > 0) port_debug("NDVR: %s mode; unit %.4f m, fov_h %.1f, fov_v %.1f, flips yaw %d pitch %d roll %d side %d y %d, no_pos %d, zoom x%.3f",
        mode == 2 ? "headset" : mode == 3 ? "calibration" : mode == 1 ? "side-by-side preview" : "normal", unit_m, fov_h, fov_v, flip_yaw, flip_pitch, flip_roll, flip_side, flip_y, no_pos, zoom_mul > 0 ? zoom_mul : 1.0);
    hand_dom = 1; { const char *h = getenv("NDVR_HAND"); if (h && (*h == 'l' || *h == 'L')) hand_dom = 0; }
    aim_on = envi("NDVR_AIM", 1); aim_uh = envd("NDVR_AIM_UNITS_H", 32); aim_uv = envd("NDVR_AIM_UNITS_V", 48);
    snap_deg = envd("NDVR_SNAP", 30); hud_radius = envd("NDVR_HUD_DIST", 1.2); hud_angle = envd("NDVR_HUD_ANGLE", 80); hud_follow = envd("NDVR_HUD_FOLLOW", 0.1);
    gun_enabled = envi("NDVR_GUN", 1); gun_scale = envd("NDVR_GUN_SCALE", 1.0);
    gun_off[0] = envd("NDVR_GUN_X", 0); gun_off[1] = envd("NDVR_GUN_Y", 0); gun_off[2] = envd("NDVR_GUN_Z", 0);
    gun_zero_m = envd("NDVR_ZERO", 10); gun_ontop = envi("NDVR_GUN_ONTOP", 0); gun_mirror_on = envi("NDVR_GUN_MIRROR", 1);
    { const char *g = getenv("NDVR_GUN_MODE"); eng_mode = !g || !*g || *g == 'e' || *g == 'E' ? 1 : *g == 'c' || *g == 'C' ? 2 : *g == 'w' || *g == 'W' ? 3 : 1; }
    if (!getenv("NDVR_GUN_MODE") && getenv("NDVR_GUN_WORLD")) eng_mode = envi("NDVR_GUN_WORLD", 0) ? 3 : 2;   /* the older switch */
    gun_use_world = eng_mode == 3;
    hud_crosshair = envi("NDVR_HUD_CROSSHAIR", 0); hud_split = envd("NDVR_HUD_SPLIT", 0.21); hud_pull = envd("NDVR_HUD_PULL", 12); hud_lift = envd("NDVR_HUD_LIFT", 6);
    if (hud_split < 0.05 || hud_split > 0.45) hud_split = 0;
    if (hud_radius < 0.3) hud_radius = 0.3; if (hud_angle < 20) hud_angle = 20; if (hud_angle > 170) hud_angle = 170; if (hud_follow < 0) hud_follow = 0; if (hud_follow > 1) hud_follow = 1;
    if (mode == 2) port_debug("NDVR: NewDarkVR build %s", NDVR_BUILD);
    if (mode == 2 && gun_enabled) port_debug("NDVR: weapon: %s, scale %.2f, offset %.0f %.0f %.0f cm%s", eng_mode == 1 ? "drawn by the engine at the hand" : eng_mode == 2 ? "captured and moved to the hand" : "floor model in the hand",
                                             gun_scale, gun_off[0], gun_off[1], gun_off[2], !hand_dom && gun_mirror_on ? ", mirrored for the left hand" : "");
    if (mode == 2) port_debug("NDVR: %s hand, aim %s (%.0f / %.0f units per count), snap turn %.0f deg, HUD %.0f deg wide at %.2f m, follow %.2f, crosshair %s",
        hand_dom ? "right" : "left", aim_on ? "on" : "off", aim_uh, aim_uv, snap_deg, hud_angle, hud_radius, hud_follow, hud_crosshair ? "shown" : "hidden");
}

/* ---------------------------------------------------------------- recentre, snap turn, the HUD panel's place */
static ndvr_quat qmul(ndvr_quat a, ndvr_quat b) {
    ndvr_quat r = { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    return r;
}
static void place_panel(int snap) {                               /* the HUD follows the head (smoothed); snap: jump there now */
    HeadAngles ha = head_angles(fr.head.orientation);
    V3 hp = v3(fr.head.position.x, fr.head.position.y, fr.head.position.z);
    if (snap || !panel_valid) { pan_yaw = ha.yaw; pan_pitch = ha.pitch; }
    else { pan_yaw = wrap_pi(pan_yaw + wrap_pi(ha.yaw - pan_yaw) * hud_follow); pan_pitch += (ha.pitch - pan_pitch) * hud_follow; }
    pan_pos = hp;
    pan_curved = ndvr_has_curved_panels();
    ndvr_quat qy = { 0, (float)sin(pan_yaw / 2), 0, (float)cos(pan_yaw / 2) }, qx = { (float)sin(pan_pitch / 2), 0, 0, (float)cos(pan_pitch / 2) };
    panel_pose.orientation = qmul(qy, qx);
    V3 c = pan_pos;
    if (!pan_curved) { V3 f = qrot(panel_pose.orientation, v3(0, 0, -1)); c = v3(c.x + f.x * hud_radius, c.y + f.y * hud_radius, c.z + f.z * hud_radius); }
    panel_pose.position.x = (float)c.x; panel_pose.position.y = (float)c.y; panel_pose.position.z = (float)c.z;
    panel_pose.flags = NDVR_POSE_ORIENT_VALID | NDVR_POSE_POS_VALID; panel_valid = 1;
}
static void panel_size(int src_w, int src_h, double *pw, double *ph) {   /* arc length (curved) or width (flat), and height */
    double a = hud_angle * PI_D / 180.0;
    *pw = pan_curved ? hud_radius * a : 2.0 * hud_radius * tan(a / 2);
    *ph = *pw * (src_w > 0 ? (double)src_h / src_w : 9.0 / 16.0);
}
static void track_update(void) {
    int focused = ndvr_focused();                                 /* recentre when the headset goes on (focus gained), on the */
    if (!ref_valid || (focused && !was_focused) ||                /* first frame, and with the left controller's menu button */
        ((fr.hand[0].buttons & NDVR_BTN_MENU) && !(prev_btn & NDVR_BTN_MENU))) {
        HeadAngles ha = head_angles(fr.head.orientation);
        ref_yaw = ha.yaw; ref_pos = v3(fr.head.position.x, fr.head.position.y, fr.head.position.z); ref_valid = 1;
        place_panel(1);
    }
    prev_btn = fr.hand[0].buttons;
    was_focused = focused;
    float sx = fr.hand[hand_dom].stick_x;                         /* snap turn on the gun hand's thumbstick */
    if (snap_armed && fabsf(sx) > 0.75f && body_valid) { body_yaw += (sx > 0 ? -1.0 : 1.0) * snap_deg * PI_D / 180.0; snap_armed = 0; }
    else if (fabsf(sx) < 0.3f) snap_armed = 1;
}

/* ---------------------------------------------------------------- the gun hand: aim through mouse look, or a pointer on the panel */
static int s16i(int v) { v &= 0xFFFF; return v > 32767 ? v - 65536 : v; }
static int vw_muzzle_aim(const ndvr_hand *hd, V3 *dir);
static void aim_update(const ndvr_hand *hd) {
    V3 f = qrot(hd->aim.orientation, v3(0, 0, -1));
    vw_muzzle_aim(hd, &f);                                        /* with a world model in the hand: along its barrel, zeroed */
    double ya = atan2(-f.x, -f.z), pa = asin(f.y < -1 ? -1 : f.y > 1 ? 1 : f.y);
    double th = body_yaw + wrap_pi(ya - ref_yaw), tp = -pa;      /* engine: heading grows to the left, positive pitch looks down */
    int eh = s16i((int)ang16(th) - eng_h - pend_h[0] - pend_h[1]);   /* what is still missing once the motion in flight lands (2 frames) */
    int ep = s16i((int)ang16(tp) - eng_p - pend_v[0] - pend_v[1]);
    int dx = (int)lround(-eh / aim_uh), dy = (int)lround(ep / aim_uv);   /* mouse left (dx < 0) turns left, mouse up (dy < 0) looks up */
    if (dx > 600) dx = 600; if (dx < -600) dx = -600; if (dy > 600) dy = 600; if (dy < -600) dy = -600;
    if ((dx || dy) && !vr_input_mouse_motion(dx, dy)) dx = dy = 0;
    pend_h[1] = pend_h[0]; pend_h[0] = (int)lround(-dx * aim_uh);
    pend_v[1] = pend_v[0]; pend_v[0] = (int)lround(dy * aim_uv);
}
static int panel_hit(const ndvr_hand *hd, double pw, double ph, double *u, double *v) {
    if (!panel_valid || !(hd->aim.flags & NDVR_POSE_POS_VALID)) return 0;
    ndvr_quat qi = { -panel_pose.orientation.x, -panel_pose.orientation.y, -panel_pose.orientation.z, panel_pose.orientation.w };
    V3 d = qrot(qi, qrot(hd->aim.orientation, v3(0, 0, -1)));    /* the ray in the panel's own frame */
    V3 o = qrot(qi, v3(hd->aim.position.x - panel_pose.position.x, hd->aim.position.y - panel_pose.position.y, hd->aim.position.z - panel_pose.position.z));
    if (pan_curved) {                                             /* inside a cylinder of radius r around +Y; the image is centred on -Z */
        double r = hud_radius, a = d.x * d.x + d.z * d.z, b = 2 * (o.x * d.x + o.z * d.z), c = o.x * o.x + o.z * o.z - r * r, disc = b * b - 4 * a * c;
        if (a < 1e-9 || disc < 0) return 0;
        double t = (-b + sqrt(disc)) / (2 * a); if (t <= 0) return 0;
        V3 h = v3(o.x + d.x * t, o.y + d.y * t, o.z + d.z * t);
        *u = atan2(h.x, -h.z) / (pw / r) + 0.5; *v = 0.5 - h.y / ph;
    } else {                                                      /* a flat quad facing +Z */
        if (fabs(d.z) < 1e-6) return 0;
        double t = -o.z / d.z; if (t <= 0) return 0;
        *u = (o.x + d.x * t) / pw + 0.5; *v = 0.5 - (o.y + d.y * t) / ph;
    }
    return *u >= 0 && *u <= 1 && *v >= 0 && *v <= 1;
}
static void controls(int in_game, double pw, double ph) {
    const ndvr_hand *hd = &fr.hand[hand_dom];
    int captured = vr_input_captured();
    ptr_u = ptr_v = -1;
    if (!hd->active) {                                            /* controller lost or headset off: let go of anything held */
        if (trig_was) { vr_input_mouse_button(0, 0); trig_was = 0; }
        if (btn_was[hand_dom] & NDVR_BTN_PRIMARY) vr_input_mouse_button(1, 0);
        if (btn_was[hand_dom] & NDVR_BTN_SECONDARY) vr_input_key(0x0F, 0);
        btn_was[hand_dom] = 0; pend_h[0] = pend_h[1] = pend_v[0] = pend_v[1] = 0;
        return;
    }
    if (captured) {                                               /* mouse look: the game aims where the gun hand points */
        if (in_game && aim_on && body_valid && (hd->aim.flags & NDVR_POSE_ORIENT_VALID)) aim_update(hd);
    } else {                                                      /* a cursor on screen (inventory, menus): point at the panel */
        pend_h[0] = pend_h[1] = pend_v[0] = pend_v[1] = 0;
        double u, v; int W, H; plat_video_window_size(&W, &H);
        if (W > 0 && H > 0 && panel_hit(hd, pw, ph, &u, &v)) {
            ptr_u = u; ptr_v = v;
            int gx = (int)(u * W), gy = (int)(v * H); if (gx >= W) gx = W - 1; if (gy >= H) gy = H - 1;
            if (gx != ptr_gx || gy != ptr_gy) { vr_input_mouse_abs(gx, gy); ptr_gx = gx; ptr_gy = gy; }
        }
    }
    /* buttons on the gun hand: trigger = left mouse button (fire / click), A or X = right mouse button, B or Y = Tab (inventory) */
    int trig = hd->trigger > 0.6f ? 1 : hd->trigger < 0.4f ? 0 : trig_was;
    if (trig != trig_was) { vr_input_mouse_button(0, trig); trig_was = trig; }
    uint32_t b = hd->buttons, chg = b ^ btn_was[hand_dom];
    if (chg & NDVR_BTN_PRIMARY) vr_input_mouse_button(1, (b & NDVR_BTN_PRIMARY) != 0);
    if (chg & NDVR_BTN_SECONDARY) vr_input_key(0x0F, (b & NDVR_BTN_SECONDARY) != 0);
    btn_was[hand_dom] = b;
}

/* ---------------------------------------------------------------- the held weapon on the gun hand
 * The engine draws its first-person weapon after the scene render, with the camera of the last scene render, placed
 * relative to its own camera (which follows the gun hand). So after the eye renders one more scene render from the
 * engine's own camera (zoom 0.5, drawing nothing) leaves that camera in place, the weapon's draws that follow are kept
 * (port/dx/d3d9.c, vr_d3d_gun_*), and at the end of the frame they are drawn into each eye, moved by
 *   eye camera space <- p' = R (D (p - g0) + off) + hand <- capture camera space p
 * g0: the weapon's rest position (the middle of its bounding box, measured over its first 30 frames, again after a
 * weapon change), D: scale and the left-hand mirror, R: the gun hand's aim orientation seen from the eye, hand: the
 * gun hand's grip position seen from the eye. Without a tracked hand or a measured g0, the weapon hangs in front of
 * the eye the way the game draws it. */
static V3 qconjrot(ndvr_quat q, V3 v) { ndvr_quat c = { -q.x, -q.y, -q.z, q.w }; return qrot(c, v); }
static void gun_setup(const ndvr_pose *ep) {
    const ndvr_hand *hd = &fr.hand[hand_dom];
    float *m = gun_m; memset(m, 0, sizeof gun_m); m[0] = m[5] = m[10] = 1; gun_mirror = 0;
    if (!gun_ready || !hd->active || !(hd->aim.flags & NDVR_POSE_ORIENT_VALID) || !(hd->grip.flags & NDVR_POSE_POS_VALID)) return;
    double D[3] = { (hand_dom || !gun_mirror_on ? 1.0 : -1.0) * gun_scale, gun_scale, gun_scale }, k = 1.0 / unit_m;
    V3 col[3];
    for (int i = 0; i < 3; i++) {                                 /* the gun's axes (camera convention) -> controller -> LOCAL -> eye -> camera convention */
        V3 q = v3(i == 0, i == 1, i == 2), c = v3(q.x, -q.y, -q.z), l = qrot(hd->aim.orientation, c), e = qconjrot(ep->orientation, l);
        col[i] = v3(e.x, -e.y, -e.z);
    }
    V3 hp = qconjrot(ep->orientation, v3(hd->grip.position.x - ep->position.x, hd->grip.position.y - ep->position.y, hd->grip.position.z - ep->position.z));
    V3 hand = v3(hp.x * k, -hp.y * k, -hp.z * k);
    double off[3] = { gun_off[0] / 100.0 * k, -gun_off[1] / 100.0 * k, gun_off[2] / 100.0 * k };   /* cm: right, up, forward */
    double u[3] = { off[0] - D[0] * gun_g0[0], off[1] - D[1] * gun_g0[1], off[2] - D[2] * gun_g0[2] };
    for (int r = 0; r < 3; r++) {
        double cr[3] = { r == 0 ? col[0].x : r == 1 ? col[0].y : col[0].z, r == 0 ? col[1].x : r == 1 ? col[1].y : col[1].z, r == 0 ? col[2].x : r == 1 ? col[2].y : col[2].z };
        double h = r == 0 ? hand.x : r == 1 ? hand.y : hand.z;
        for (int c2 = 0; c2 < 3; c2++) m[r * 4 + c2] = (float)(cr[c2] * D[c2]);
        m[r * 4 + 3] = (float)(h + cr[0] * u[0] + cr[1] * u[1] + cr[2] * u[2]);
    }
    gun_mirror = !hand_dom && gun_mirror_on;
}
static unsigned gun_log_n;
static void gun_measure(void) {                                   /* after the capture: learn the weapon's rest position */
    int nv = vr_gun_nv;
    if (++gun_log_n % 90 == 0) {                                  /* once a second or so: what the weapon is doing */
        const ndvr_hand *hd = &fr.hand[hand_dom];
        port_debug("NDVR GUN: %s, %d draws %d vertices (%d behind the eye last frame), box x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f, rest %.2f %.2f %.2f, hand %s, m %.2f %.2f %.2f",
            !gun_ready ? "measuring" : "in the hand", vr_gun_draws, nv, vr_gun_near, vr_gun_bmin[0], vr_gun_bmax[0], vr_gun_bmin[1], vr_gun_bmax[1], vr_gun_bmin[2], vr_gun_bmax[2],
            gun_g0[0], gun_g0[1], gun_g0[2], hd->active ? "tracked" : "NOT tracked", gun_m[3], gun_m[7], gun_m[11]);
    }
    if (nv < 12) return;                                          /* no weapon drawn this frame */
    if (gun_ready && gun_ref_nv > 0 && abs(nv - gun_ref_nv) * 5 > gun_ref_nv * 2) {   /* a different weapon: measure again */
        if (++gun_odd > 60) { gun_ready = 0; gun_frames = 0; gun_odd = 0; port_debug("NDVR GUN: vertex count %d (was %d): measuring the weapon again", nv, gun_ref_nv); }
        return;
    }
    gun_odd = 0;
    if (gun_ready) return;
    for (int i = 0; i < 3; i++) gun_sum[i] += 0.5 * (vr_gun_bmin[i] + vr_gun_bmax[i]);
    if (++gun_frames == 1) gun_ref_nv = nv;
    if (gun_frames >= 30) {
        for (int i = 0; i < 3; i++) { gun_g0[i] = gun_sum[i] / gun_frames; gun_sum[i] = 0; }
        gun_ready = 1; gun_ref_nv = nv;
        port_debug("NDVR: weapon rest position %.2f %.2f %.2f (camera units, %d vertices); now on the %s hand", gun_g0[0], gun_g0[1], gun_g0[2], nv, hand_dom ? "right" : "left");
    }
}

/* ---------------------------------------------------------------- the weapon's world model in the hand
 * tools/extract_weapons.py writes vr/weapons.vrw from the player's own install: per first-person model the texture
 * size and triangle count of each material, per world (floor) model its triangles in metres, turned so that x is
 * right, y down, z forward (the barrel), with the grip at the origin, plus the muzzle point and the textures.
 * Each frame the kept first-person draws (port/dx/d3d9.c) say which weapon is out (their texture sizes and triangle
 * counts against the first-person models) and how it moves (its centroid and principal axes against its rest pose:
 * recoil, the reload dip); the matching world model is drawn in the gun hand with that movement, both sides of every
 * triangle, lit roughly like the game lights the weapon, and depth-tested against the world. */
typedef struct { char name[16]; uint32_t world, nmat; uint32_t (*mat)[3]; } VwHand;
typedef struct { char name[16]; float muzzle[3], length; uint32_t nv, nt; float *v; uint16_t *t; } VwWorld;
typedef struct { char name[32]; uint32_t w, h; uint8_t *rgba; PlatTexture *pt; } VwTex;
static VwHand *vw_h; static VwWorld *vw_w; static VwTex *vw_t; static uint32_t vw_nh, vw_nw, vw_nt; static int vw_state;   /* 0 not tried, 1 loaded, -1 none */
static int vw_cur = -1, vw_cand = -1, vw_cand_n;                 /* current world model; a challenger and how many frames it has won */
static int vw_rest_n; static double vw_rest_c[3], vw_rest_E[9], vw_rest_acc[10];   /* the weapon's rest pose (centroid, axes) */
static double vw_R[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }, vw_T[3];  /* its movement now (rotation, offset in metres) */
static float vw_light[3] = { 1, 1, 1 };

static int vw_load(void) {
    FILE *f = fopen("vr/weapons.vrw", "rb");
    if (!f) { port_debug("NDVR: no vr/weapons.vrw: run  python3 NewDarkVR/tools/extract_weapons.py \"<game folder>\"  (the weapon stays as the game draws it)"); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n); if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return -1; }
    fclose(f);
    size_t o = 8; uint32_t u;
#define RU32() (o + 4 <= (size_t)n ? (memcpy(&u, b + o, 4), o += 4, u) : (o = (size_t)n + 1, 0u))
    if (n < 12 || memcmp(b, "NDVW", 4)) { free(b); return -1; }
    vw_nt = RU32(); vw_t = calloc(vw_nt ? vw_nt : 1, sizeof *vw_t);
    for (uint32_t i = 0; i < vw_nt && o <= (size_t)n; i++) {
        if (o + 32 > (size_t)n) { o = (size_t)n + 1; break; }
        memcpy(vw_t[i].name, b + o, 32); vw_t[i].name[31] = 0; o += 32; vw_t[i].w = RU32(); vw_t[i].h = RU32();
        size_t sz = (size_t)vw_t[i].w * vw_t[i].h * 4; if (o + sz > (size_t)n) { o = (size_t)n + 1; break; }
        vw_t[i].rgba = b + o; o += sz;
    }
    vw_nh = RU32(); vw_h = calloc(vw_nh ? vw_nh : 1, sizeof *vw_h);
    for (uint32_t i = 0; i < vw_nh && o <= (size_t)n; i++) {
        if (o + 16 > (size_t)n) { o = (size_t)n + 1; break; }
        memcpy(vw_h[i].name, b + o, 16); vw_h[i].name[15] = 0; o += 16; vw_h[i].world = RU32(); vw_h[i].nmat = RU32();
        if (o + 12ull * vw_h[i].nmat > (size_t)n) { o = (size_t)n + 1; break; }
        vw_h[i].mat = (uint32_t (*)[3])(void *)(b + o); o += 12ull * vw_h[i].nmat;
    }
    vw_nw = RU32(); vw_w = calloc(vw_nw ? vw_nw : 1, sizeof *vw_w);
    for (uint32_t i = 0; i < vw_nw && o <= (size_t)n; i++) {
        if (o + 32 > (size_t)n) { o = (size_t)n + 1; break; }
        memcpy(vw_w[i].name, b + o, 16); vw_w[i].name[15] = 0; o += 16; memcpy(vw_w[i].muzzle, b + o, 12); memcpy(&vw_w[i].length, b + o + 12, 4); o += 16;
        vw_w[i].nv = RU32(); if (o + 32ull * vw_w[i].nv > (size_t)n) { o = (size_t)n + 1; break; }
        vw_w[i].v = (float *)(void *)(b + o); o += 32ull * vw_w[i].nv;
        vw_w[i].nt = RU32(); if (o + 8ull * vw_w[i].nt > (size_t)n) { o = (size_t)n + 1; break; }
        vw_w[i].t = (uint16_t *)(void *)(b + o); o += 8ull * vw_w[i].nt;
    }
#undef RU32
    if (o > (size_t)n) { port_debug("NDVR: vr/weapons.vrw is damaged; run extract_weapons.py again"); vw_nh = vw_nw = 0; return -1; }
    port_debug("NDVR: vr/weapons.vrw: %u first-person models, %u world models, %u textures", vw_nh, vw_nw, vw_nt);
    return 1;
}

static int vw_identify(void) {                                    /* the first-person model whose materials fit the kept draws best */
    int nd = vr_gun_draws < VR_GUN_MAXD ? vr_gun_draws : VR_GUN_MAXD, best = -1, total = 0; double bscore = -1e9;
    if (nd <= 0) return -1;
    for (int d = 0; d < nd; d++) total += vr_gun_dtris[d];
    for (uint32_t i = 0; i < vw_nh; i++) {
        if (vw_h[i].world >= vw_nw || !strncmp(vw_h[i].name, "COPY", 4)) continue;
        uint8_t used[64] = { 0 }; int drawn[64] = { 0 }; double got = 0, miss = 0, mod = 0, unused = 0;
        for (int d = 0; d < nd; d++) {                            /* each draw takes the closest-sized material with the same texture size and room left */
            int bj = -1, bd = 1 << 30;                            /* (an unused one first: a material may come in more than one draw) */
            for (uint32_t j = 0; j < vw_h[i].nmat && j < 64; j++) {
                int diff = (int)vw_h[i].mat[j][2] - drawn[j] - vr_gun_dtris[d];
                if ((int)vw_h[i].mat[j][0] != vr_gun_dtw[d] || (int)vw_h[i].mat[j][1] != vr_gun_dth[d] || diff < -2) continue;
                diff += used[j] ? 100000 : 0;
                if (diff < bd) { bd = diff; bj = (int)j; }
            }
            if (bj < 0) { miss += vr_gun_dtris[d]; continue; }
            if (!used[bj]) mod += vw_h[i].mat[bj][2];
            used[bj] = 1; drawn[bj] += vr_gun_dtris[d]; got += vr_gun_dtris[d];
        }
        for (uint32_t j = 0; j < vw_h[i].nmat && j < 64; j++) if (!used[j]) unused += vw_h[i].mat[j][2];
        double rho = mod > 0 ? got / mod : 0, spread = 0;         /* the share of its triangles the game drew (it drops back faces) */
        for (uint32_t j = 0; j < vw_h[i].nmat && j < 64; j++) if (used[j]) spread += fabs(drawn[j] - rho * vw_h[i].mat[j][2]);
        double score = got - 2 * miss - rho * unused - 0.5 * spread;
        if (score > bscore) { bscore = score; best = (int)i; }
    }
    return best >= 0 && bscore > 0.5 * total ? best : -1;
}

static void jacobi3(double A[9], double V[9]) {                   /* eigenvectors (columns of V) of a symmetric 3x3, A becomes diagonal */
    for (int i = 0; i < 9; i++) V[i] = i % 4 == 0;
    for (int sweep = 0; sweep < 12; sweep++)
        for (int p = 0; p < 2; p++) for (int q = p + 1; q < 3; q++) {
            double apq = A[p * 3 + q]; if (fabs(apq) < 1e-12) continue;
            double th = 0.5 * atan2(2 * apq, A[q * 3 + q] - A[p * 3 + p]), c = cos(th), s = sin(th);
            for (int k = 0; k < 3; k++) { double akp = A[k * 3 + p], akq = A[k * 3 + q]; A[k * 3 + p] = c * akp - s * akq; A[k * 3 + q] = s * akp + c * akq; }
            for (int k = 0; k < 3; k++) { double apk = A[p * 3 + k], aqk = A[q * 3 + k]; A[p * 3 + k] = c * apk - s * aqk; A[q * 3 + k] = s * apk + c * aqk; }
            for (int k = 0; k < 3; k++) { double vkp = V[k * 3 + p], vkq = V[k * 3 + q]; V[k * 3 + p] = c * vkp - s * vkq; V[k * 3 + q] = s * vkp + c * vkq; }
        }
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2 - i; j++)   /* sort by eigenvalue, largest first */
        if (A[j * 4] < A[(j + 1) * 4]) { double t = A[j * 4]; A[j * 4] = A[(j + 1) * 4]; A[(j + 1) * 4] = t;
            for (int k = 0; k < 3; k++) { t = V[k * 3 + j]; V[k * 3 + j] = V[k * 3 + j + 1]; V[k * 3 + j + 1] = t; } }
}
static int vw_pose(const double *mo, double c[3], double E[9]) {  /* centroid and principal axes from the moment sums */
    if (mo[0] < 12) return 0;
    double n = mo[0]; c[0] = mo[1] / n; c[1] = mo[2] / n; c[2] = mo[3] / n;
    double A[9] = { mo[4] / n - c[0] * c[0], mo[7] / n - c[0] * c[1], mo[8] / n - c[0] * c[2],
                    0, mo[5] / n - c[1] * c[1], mo[9] / n - c[1] * c[2], 0, 0, mo[6] / n - c[2] * c[2] };
    A[3] = A[1]; A[6] = A[2]; A[7] = A[5];
    jacobi3(A, E);
    return A[0] > 1.2 * A[4] && A[4] > 1.2 * A[8] ? 1 : 2;        /* 2: axes not well defined, use the centroid only */
}
static void vw_update(void) {                                     /* after the capture: which weapon, and how it moves */
    if (vw_state == 0) vw_state = vw_load();
    if (vw_state < 0) { vw_cur = -1; return; }
    int h = vw_identify(), w = h >= 0 && vw_h[h].world < vw_nw ? (int)vw_h[h].world : -1;
    static int quiet; if (h < 0 && vr_gun_draws > 0 && ++quiet % 90 == 1) {   /* say what the unknown weapon looks like, to tune the match */
        char b[512]; int o = 0, nd = vr_gun_draws < VR_GUN_MAXD ? vr_gun_draws : VR_GUN_MAXD;
        for (int d = 0; d < nd && o < 480; d++) o += snprintf(b + o, sizeof b - o, " %dx%d:%d", vr_gun_dtw[d], vr_gun_dth[d], vr_gun_dtris[d]);
        b[o] = 0; port_debug("NDVR GUN: weapon not recognised, draws%s (the game's own model is shown)", b);
    }
    if (w != vw_cur) { if (w == vw_cand) { if (++vw_cand_n > 10) { vw_cur = w; vw_rest_n = 0; memset(vw_rest_acc, 0, sizeof vw_rest_acc);
                                                                    port_debug("NDVR GUN: holding %s (%s)", w >= 0 ? vw_w[w].name : "nothing", h >= 0 ? vw_h[h].name : "?"); } }
                       else { vw_cand = w; vw_cand_n = 0; } }
    else vw_cand_n = 0;
    if (vw_cur < 0) return;
    if (vr_gun_rgb[3] > 0) for (int k = 0; k < 3; k++) vw_light[k] = (float)(vr_gun_rgb[k] / vr_gun_rgb[3] / 255.0);
    if (vw_rest_n < 30) {                                         /* the rest pose: the first second or so after the weapon comes out */
        if (vr_gun_mom[0] >= 12) { for (int k = 0; k < 10; k++) vw_rest_acc[k] += vr_gun_mom[k]; vw_rest_n++; }
        if (vw_rest_n == 30) vw_pose(vw_rest_acc, vw_rest_c, vw_rest_E);
        for (int i = 0; i < 9; i++) vw_R[i] = i % 4 == 0; vw_T[0] = vw_T[1] = vw_T[2] = 0;
        return;
    }
    double c[3], E[9]; int ok = vw_pose(vr_gun_mom, c, E);
    if (!ok) return;
    for (int k = 0; k < 3; k++) vw_T[k] = (c[k] - vw_rest_c[k]) * unit_m;
    for (int i = 0; i < 9; i++) vw_R[i] = i % 4 == 0;
    if (ok == 1) {
        for (int j = 0; j < 3; j++) {                             /* the same axis direction as at rest */
            double d = E[j] * vw_rest_E[j] + E[3 + j] * vw_rest_E[3 + j] + E[6 + j] * vw_rest_E[6 + j];
            if (d < 0) for (int k = 0; k < 3; k++) E[k * 3 + j] = -E[k * 3 + j];
        }
        double R[9];                                              /* R = E * Erest^T */
        for (int r = 0; r < 3; r++) for (int cc = 0; cc < 3; cc++) R[r * 3 + cc] = E[r * 3] * vw_rest_E[cc * 3] + E[r * 3 + 1] * vw_rest_E[cc * 3 + 1] + E[r * 3 + 2] * vw_rest_E[cc * 3 + 2];
        double tr = R[0] + R[4] + R[8], ang = acos(fmax(-1.0, fmin(1.0, (tr - 1) / 2)));
        if (ang > 1.0 * PI_D / 180 && ang < 80 * PI_D / 180) memcpy(vw_R, R, sizeof R);   /* small wobble: none; a jump: not trusted */
    }
}

static V3 gun_to_local(const ndvr_hand *hd, V3 q) {               /* gun axes (x right, y down, z forward) -> LOCAL direction */
    return qrot(hd->aim.orientation, v3(q.x, -q.y, -q.z));
}
static void vw_draw_eye(int e, const ndvr_pose *ep) {
    const ndvr_hand *hd = &fr.hand[hand_dom];
    if (vw_cur < 0 || !hd->active || !(hd->aim.flags & NDVR_POSE_ORIENT_VALID) || !(hd->grip.flags & NDVR_POSE_POS_VALID)) return;
    VwWorld *m = &vw_w[vw_cur];
    static PlatVertex *pv; static uint16_t *ix; static uint32_t cap;
    if (cap < m->nv) { cap = m->nv; pv = realloc(pv, cap * sizeof *pv); ix = realloc(ix, 3 * (size_t)m->nt * sizeof *ix); }
    double s = gun_scale, nearm = 0.25 * unit_m, f = gun_f_eye, hw = gun_W * 0.5, hh = gun_H * 0.5;
    V3 grip = v3(hd->grip.position.x, hd->grip.position.y, hd->grip.position.z), eye = v3(ep->position.x, ep->position.y, ep->position.z);
    V3 off = gun_to_local(hd, v3(gun_off[0] / 100.0, -gun_off[1] / 100.0, gun_off[2] / 100.0));
    V3 up = qconjrot(ep->orientation, v3(0, 1, 0)); V3 L = norm(v3(up.x + 0.3, -up.y + 0.0, -up.z - 0.4));   /* light from above, camera convention */
    for (uint32_t i = 0; i < m->nv; i++) {
        const float *v = m->v + 8 * i;
        V3 q = v3(vw_R[0] * v[0] + vw_R[1] * v[1] + vw_R[2] * v[2], vw_R[3] * v[0] + vw_R[4] * v[1] + vw_R[5] * v[2], vw_R[6] * v[0] + vw_R[7] * v[1] + vw_R[8] * v[2]);
        q = v3(q.x * s + vw_T[0], q.y * s + vw_T[1], q.z * s + vw_T[2]);
        V3 w = gun_to_local(hd, q); w = v3(grip.x + off.x + w.x - eye.x, grip.y + off.y + w.y - eye.y, grip.z + off.z + w.z - eye.z);
        V3 c = qconjrot(ep->orientation, w); c = v3(c.x, -c.y, -c.z);       /* eye camera space, metres */
        V3 nq = v3(vw_R[0] * v[5] + vw_R[1] * v[6] + vw_R[2] * v[7], vw_R[3] * v[5] + vw_R[4] * v[6] + vw_R[5] * v[7], vw_R[6] * v[5] + vw_R[7] * v[6] + vw_R[8] * v[7]);
        V3 n = qconjrot(ep->orientation, gun_to_local(hd, nq)); n = v3(n.x, -n.y, -n.z);
        if (dot(n, c) > 0) n = v3(-n.x, -n.y, -n.z);                          /* both sides: light the side that faces the eye */
        double lit = 0.45 + 0.75 * fmax(0.0, dot(n, L));
        uint32_t col = 0xFF000000u;
        for (int k = 0; k < 3; k++) { double ch = lit * (0.35 + 0.9 * vw_light[k]) * 255.0; col |= (uint32_t)(ch > 255 ? 255 : ch) << (16 - 8 * k); }
        double z = c.z > 0.02 ? c.z : 0.02;
        PlatVertex *o = &pv[i]; memset(o, 0, sizeof *o);
        o->x = (float)(f * c.x / hw); o->y = (float)(-f * c.y / hh); o->z = (float)(z - nearm); o->w = (float)z;
        o->diffuse = col; o->fog = 1; o->u0 = v[3]; o->v0 = v[4];
    }
    PlatDrawState st; memset(&st, 0, sizeof st);
    st.depth_test = 1; st.depth_write = 1; st.depth_func = PLAT_CMP_LEQUAL; st.cull = PLAT_CULL_NONE; st.color_write = 0xF;
    st.stage[0].color_op = PLAT_TOP_MODULATE; st.stage[0].color_arg1 = PLAT_ARG_TEXTURE; st.stage[0].color_arg2 = PLAT_ARG_DIFFUSE;
    st.stage[0].alpha_op = PLAT_TOP_SELECTARG1; st.stage[0].alpha_arg1 = PLAT_ARG_DIFFUSE;
    st.stage[0].min_filter = st.stage[0].mag_filter = PLAT_FILTER_LINEAR; st.stage[0].mip_filter = PLAT_MIP_NONE;
    st.stage[1].color_op = PLAT_TOP_DISABLE; st.stage[1].alpha_op = PLAT_TOP_DISABLE;
    plat_gfx_set_target(hmd_tex[e]);
    PlatViewport vp = { (int)lroundf(eye_map[e][0]), (int)lroundf(eye_map[e][1]), (int)lroundf(gun_W * eye_map[e][2]), (int)lroundf(gun_H * eye_map[e][3]), 0.0f, 1.0f };
    plat_gfx_viewport(&vp); plat_gfx_scissor(0, 0, 0, 0, 0);
    if (gun_ontop) plat_gfx_clear(0, 0, 1, 1.0f, 0, 0);
    for (int pass = -1; pass < (int)vw_nt; pass++) {                          /* one draw per texture (-1: untextured) */
        uint32_t k = 0;
        for (uint32_t t = 0; t < m->nt; t++) {
            const uint16_t *tr = m->t + 4 * t; int tx = tr[3] == 0xFFFF ? -1 : tr[3];
            if (tx != pass) continue;
            if (tr[0] < m->nv && tr[1] < m->nv && tr[2] < m->nv) { ix[k++] = tr[0]; ix[k++] = tr[1]; ix[k++] = tr[2]; }
        }
        if (!k) continue;
        if (pass >= 0) {
            VwTex *T = &vw_t[pass];
            if (!T->pt && T->w && T->h) { T->pt = plat_tex_create((int)T->w, (int)T->h, 1, PLAT_TEX_RGBA8, 0); if (T->pt) plat_tex_upload(T->pt, 0, 0, 0, (int)T->w, (int)T->h, T->rgba, (int)T->w * 4); }
            st.stage[0].texture = T->pt; st.stage[0].color_op = PLAT_TOP_MODULATE;
        } else { st.stage[0].texture = NULL; st.stage[0].color_op = PLAT_TOP_SELECTARG2; }
        plat_gfx_draw(PLAT_PRIM_TRIANGLES, pv, (int)m->nv, ix, (int)k, &st);
    }
}
static int vw_muzzle_aim(const ndvr_hand *hd, V3 *dir) {          /* aim so a shot from the eye crosses the barrel line NDVR_ZERO metres out */
    if (vw_cur < 0 || gun_zero_m <= 0 || !(hd->grip.flags & NDVR_POSE_POS_VALID) || !(fr.head.flags & NDVR_POSE_POS_VALID)) return 0;
    const float *mz = vw_w[vw_cur].muzzle; double s = gun_scale;
    V3 m = gun_to_local(hd, v3(mz[0] * s, mz[1] * s, mz[2] * s + gun_zero_m));
    V3 off = gun_to_local(hd, v3(gun_off[0] / 100.0, -gun_off[1] / 100.0, gun_off[2] / 100.0));
    V3 p = v3(hd->grip.position.x + off.x + m.x, hd->grip.position.y + off.y + m.y, hd->grip.position.z + off.z + m.z);
    *dir = norm(v3(p.x - fr.head.position.x, p.y - fr.head.position.y, p.z - fr.head.position.z));
    return 1;
}

/* ---------------------------------------------------------------- the weapon drawn by the engine, at the hand (default)
 * The engine draws its first-person model (with the 25th Anniversary models from tools/convert_remaster.py: the whole
 * weapon and the arm) in every scene render, after a depth-only clear, placed in front of that render's camera. The
 * hook on md_set_model (0x627280) runs as the engine starts on a model; its r3 context (*0xa458b0, read from the light
 * dumps of SS2PORT_PPL_LIGHTS) then holds
 *   +0x04, +0x08  the focal lengths (pixels * 65536)
 *   +0x44, +0x68  the model's world pose: rotation (rows: the model's axes in the world) and position; world = v R + p
 *   +0x78, +0x9c  the camera: world to view, view = world M + t (x right, y down, z forward; engine units)
 *   +0xac, +0xd0  the same with the columns scaled by the focal lengths (x, y) - what the vertices go through
 * The camera pass (one render from the engine's own camera, nothing drawn; it comes first) records each first-person
 * model's pose against that camera. In each eye render the camera is replaced, for those models only, by one that sends
 * the recorded pose to the gun hand as this eye sees it:
 *   view = K( ((world - p_e) R_e^T R_c + p_c) M_c + t_c )      (e: this render's pose of the model, c: the camera pass's)
 * with K = gun_m (gun_setup: engine camera space to eye camera space, the rest position g0 at the controller's grip;
 * g0 is the model's origin in the camera pass, which the game puts at the hand). The engine then transforms, lights,
 * clips and draws the whole model in the eye itself, sub-objects (slide, recoil, reload) included. Its polygons reach
 * Direct3D one render late; port/dx/d3d9.c puts them into the right eye (vr_d3d_spill). */
#define R3CTX 0x00a458b0u
extern int vr_d3d_weapon_phase(void); extern int vr_d3d_weapon_stats(char *buf, size_t n);
extern void vr_d3d_spill(int how, PlatTexture *t, float ox, float oy, float kx, float ky);
extern int vr_gun_cull_flip; extern float vr_ppl_zoom, vr_ppl_wzoom;   /* port/dx/d3d9.c */
typedef struct { uint32_t model; double R[9], p[3], M[9], t[3]; } EngPose;
static EngPose eng_rec[4]; static int eng_nrec, eng_pass;         /* eng_pass: 0 off, 1 camera pass (record), 2 eye render (move) */
static uint32_t eng_sv_ctx, eng_sv[26]; static int eng_sv_on;      /* the context words changed for the model being drawn */
static double eng_sf[2];                                          /* this eye render's focal lengths (from its scene models) */
static uint32_t eng_g0_model; static int eng_g0_n; static double eng_g0_sum[3];
static unsigned eng_n_rec, eng_n_moved, eng_n_fallback, eng_n_unknown, eng_n_nofocal, eng_n_aim, eng_n_cam, eng_n_far;
static double eng_last_dist;

static void ctx_rd(uint32_t a, double *d, int n) { for (int i = 0; i < n; i++) d[i] = rdf(a + 4u * (uint32_t)i); }
static void ctx_wr(uint32_t a, const double *d, int n) { for (int i = 0; i < n; i++) wrf(a + 4u * (uint32_t)i, (float)d[i]); }
static void eng_restore(void) {
    if (!eng_sv_on) return;
    WR32(eng_sv_ctx + 4, eng_sv[0]); WR32(eng_sv_ctx + 8, eng_sv[1]);
    for (uint32_t i = 0; i < 12; i++) { WR32(eng_sv_ctx + 0x78 + 4 * i, eng_sv[2 + i]); WR32(eng_sv_ctx + 0xac + 4 * i, eng_sv[14 + i]); }
    eng_sv_on = 0;
}
static void eng_read(uint32_t ctx, uint32_t model, EngPose *e) {
    e->model = model; ctx_rd(ctx + 0x44, e->R, 9); ctx_rd(ctx + 0x68, e->p, 3); ctx_rd(ctx + 0x78, e->M, 9); ctx_rd(ctx + 0x9c, e->t, 3);
}
static int eng_sane(const EngPose *e) {                           /* a rotation and a camera that look like ones */
    for (int i = 0; i < 9; i++) if (!isfinite(e->R[i]) || !isfinite(e->M[i]) || fabs(e->R[i]) > 2 || fabs(e->M[i]) > 2) return 0;
    for (int i = 0; i < 3; i++) if (!isfinite(e->p[i]) || !isfinite(e->t[i])) return 0;
    return 1;
}
static void row_mul(const double *v, const double *A, double *o) { for (int j = 0; j < 3; j++) o[j] = v[0] * A[j] + v[1] * A[3 + j] + v[2] * A[6 + j]; }
static void cam_pos(const EngPose *e, double *c) {                /* the camera's world position: -t M^T */
    for (int i = 0; i < 3; i++) c[i] = -(e->t[0] * e->M[i * 3] + e->t[1] * e->M[i * 3 + 1] + e->t[2] * e->M[i * 3 + 2]);
}
int vr_model_hook(CPU *c) {
    if (!eng_pass) return 0;
    eng_restore();
    uint32_t ctx = RD32(R3CTX);
    if (!ctx || !g_valid(ctx, 0xdc)) return 0;
    if (!vr_d3d_weapon_phase()) {                                  /* a model of the scene: remember this eye's focal lengths */
        if (eng_pass == 2) { eng_sf[0] = rdf(ctx + 4); eng_sf[1] = rdf(ctx + 8); }
        return 0;
    }
    EngPose cur; eng_read(ctx, c->eax, &cur);
    if (!eng_sane(&cur)) return 0;
    if (eng_pass == 1) {                                           /* the camera pass: where the model sits against the engine's camera */
        double cc[3]; cam_pos(&cur, cc);                           /* (only a pose near the camera is the model's own) */
        if (!((cur.p[0] - cc[0]) * (cur.p[0] - cc[0]) + (cur.p[1] - cc[1]) * (cur.p[1] - cc[1]) + (cur.p[2] - cc[2]) * (cur.p[2] - cc[2]) < 64)) { eng_n_far++; return 0; }
        int k = 0; while (k < eng_nrec && eng_rec[k].model != cur.model) k++;
        if (k == eng_nrec) { if (eng_nrec == 4) return 0; eng_nrec++; }
        eng_rec[k] = cur; eng_n_rec++;
        if (k == 0) {                                              /* the first one (the weapon): its rest position, over 30 frames */
            double o[3]; row_mul(cur.p, cur.M, o); for (int i = 0; i < 3; i++) o[i] += cur.t[i];
            if (cur.model != eng_g0_model) { eng_g0_model = cur.model; eng_g0_n = 0; eng_g0_sum[0] = eng_g0_sum[1] = eng_g0_sum[2] = 0; gun_ready = 0;
                                             port_debug("NDVR GUN: a new first-person model (%08x): measuring where the game holds it", cur.model); }
            if (eng_g0_n < 30) {
                for (int i = 0; i < 3; i++) eng_g0_sum[i] += o[i];
                if (++eng_g0_n == 30) {
                    for (int i = 0; i < 3; i++) gun_g0[i] = eng_g0_sum[i] / 30; gun_ready = 1;
                    port_debug("NDVR GUN: the game holds it %.2f m right, %.2f m down, %.2f m ahead of the eye; that point is now at your %s hand",
                               gun_g0[0] * unit_m, gun_g0[1] * unit_m, gun_g0[2] * unit_m, hand_dom ? "right" : "left");
                }
            }
        }
        return 0;
    }
    int k = 0; while (k < eng_nrec && eng_rec[k].model != cur.model) k++;
    if (k == eng_nrec || !gun_ready) { eng_n_unknown++; return 0; }   /* not seen in the camera pass: drawn where the engine puts it */
    const EngPose *r = &eng_rec[k];
    double sx = eng_sf[0], sy = eng_sf[1];
    if (!(sx > 0 && sy > 0)) { sx = rdf(ctx + 4); sy = rdf(ctx + 8); eng_n_nofocal++; }
    /* This render's pose of the model (e) is trusted when it is near this render's camera and turned either as in the
     * camera pass (the game turns the weapon with the aim) or with the camera (R_e M_e = R_c M_c). Otherwise the context
     * did not hold the model's pose yet: then the model is taken to be placed as in the camera pass, moved along with
     * the camera. */
    double ce[3], cc[3], Re[9], pe[3], e2 = 0, e3 = 0, RM[9]; cam_pos(&cur, ce); cam_pos(r, cc);
    memcpy(Re, cur.R, sizeof Re); memcpy(pe, cur.p, sizeof pe);
    eng_last_dist = sqrt((pe[0] - ce[0]) * (pe[0] - ce[0]) + (pe[1] - ce[1]) * (pe[1] - ce[1]) + (pe[2] - ce[2]) * (pe[2] - ce[2]));
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {     /* R_c M_c M_e^T: turned with the camera */
        double a = 0; for (int q = 0; q < 3; q++) { double rmq = r->R[i * 3] * r->M[q] + r->R[i * 3 + 1] * r->M[3 + q] + r->R[i * 3 + 2] * r->M[6 + q]; a += rmq * cur.M[j * 3 + q]; }
        RM[i * 3 + j] = a;
    }
    for (int i = 0; i < 9; i++) { e2 += (Re[i] - r->R[i]) * (Re[i] - r->R[i]); e3 += (Re[i] - RM[i]) * (Re[i] - RM[i]); }
    if (eng_last_dist < 8 && e2 < 0.01) eng_n_aim++;
    else if (eng_last_dist < 8 && e3 < 0.01) eng_n_cam++;
    else { memcpy(Re, r->R, sizeof Re); for (int i = 0; i < 3; i++) pe[i] = r->p[i] + ce[i] - cc[i]; eng_n_fallback++; }
    double X[9], Y[9], AT[9], N[9], w[3], w2[3], c0[3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) X[i * 3 + j] = Re[i] * r->R[j] + Re[3 + i] * r->R[3 + j] + Re[6 + i] * r->R[6 + j];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) Y[i * 3 + j] = X[i * 3] * r->M[j] + X[i * 3 + 1] * r->M[3 + j] + X[i * 3 + 2] * r->M[6 + j];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) AT[i * 3 + j] = gun_m[j * 4 + i];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) N[i * 3 + j] = Y[i * 3] * AT[j] + Y[i * 3 + 1] * AT[3 + j] + Y[i * 3 + 2] * AT[6 + j];
    row_mul(pe, X, w); for (int i = 0; i < 3; i++) w[i] = r->p[i] - w[i];
    row_mul(w, r->M, w2); for (int i = 0; i < 3; i++) w2[i] += r->t[i];
    row_mul(w2, AT, c0); for (int i = 0; i < 3; i++) c0[i] += gun_m[i * 4 + 3];
    eng_sv_ctx = ctx; eng_sv[0] = RD32(ctx + 4); eng_sv[1] = RD32(ctx + 8);
    for (uint32_t i = 0; i < 12; i++) { eng_sv[2 + i] = RD32(ctx + 0x78 + 4 * i); eng_sv[14 + i] = RD32(ctx + 0xac + 4 * i); }
    eng_sv_on = 1;
    double Q[9], u[3], s3[3] = { sx, sy, 1 };
    for (int i = 0; i < 9; i++) Q[i] = N[i] * s3[i % 3];
    for (int j = 0; j < 3; j++) u[j] = c0[j] * s3[j];
    wrf(ctx + 4, (float)sx); wrf(ctx + 8, (float)sy);
    ctx_wr(ctx + 0x78, N, 9); ctx_wr(ctx + 0x9c, c0, 3); ctx_wr(ctx + 0xac, Q, 9); ctx_wr(ctx + 0xd0, u, 3);
    eng_n_moved++;
    return 0;
}
static void eng_log(void) {                                       /* about once a second: what the engine-drawn weapon is doing */
    static unsigned n;
    if (++n % 90) return;
    const ndvr_hand *hd = &fr.hand[hand_dom];
    port_debug("NDVR GUN: engine-drawn: %u models recorded in the camera pass (%u not near the camera), %u moved to the hand (pose turned with the aim %u, with the camera %u, fallback %u; last model-to-eye distance %.2f units), %u not recognised, %u without the scene's focal length; %s, hand %s",
               eng_n_rec, eng_n_far, eng_n_moved, eng_n_aim, eng_n_cam, eng_n_fallback, eng_last_dist, eng_n_unknown, eng_n_nofocal, gun_ready ? "rest position measured" : "measuring the rest position", hd->active ? "tracked" : "NOT tracked");
    { char b[400]; vr_d3d_weapon_stats(b, sizeof b); port_debug("NDVR GUN: %s (window %dx%d)", b, gun_W, gun_H); }
    eng_n_rec = eng_n_moved = eng_n_unknown = eng_n_fallback = eng_n_nofocal = eng_n_aim = eng_n_cam = eng_n_far = 0;
}

/* ---------------------------------------------------------------- end of a game frame (called from Present) */
static Submit pending_sub;
void vr_frame_present(void) {
    vr_ppl_zoom = vr_ppl_wzoom = 0;                               /* the eye renders' zoom for per-pixel lighting: this frame only */
    if (mode < 0) init_env();
    if (mode != 2) return;
    if (xr_state == 0) glt_call(rt_init, NULL);
    if (xr_state < 0) { mode = 0; return; }
    int st = frame_state; frame_state = 0;
    int hud = vr_hud_seen; vr_hud_seen = 0;
    if (st == 2) return;                                          /* the scene render already closed this headset frame */
    Submit s;
    if (st == 1) s = pending_sub;
    else {                                                        /* no 3D view this frame (menus, videos): the panel alone */
        memset(&s, 0, sizeof s); s.flip = flip_y;
        glt_call(rt_begin, NULL);
        if (!(fr_ok && fr.should_render && (fr.head.flags & NDVR_POSE_ORIENT_VALID))) { plat_gfx_deferred(rt_submit, &s, sizeof s); return; }
        track_update();
    }
    if (st == 1 && gun_enabled && eng_mode == 1) eng_log();
    if (st == 1 && gun_enabled && eng_mode != 1) {                /* the weapon the engine drew after the scene, into each eye at the hand */
        gun_measure();
        if (gun_use_world) vw_update();
        for (int e = 0; e < 2; e++) {
            if (gun_use_world && vw_cur >= 0) { vw_draw_eye(e, &s.pose[e]); continue; }   /* its world model */
            gun_setup(&s.pose[e]);                                                       /* or the game's own first-person model */
            vr_d3d_gun_replay_eye(hmd_tex[e], eye_map[e][0], eye_map[e][1], eye_map[e][2], eye_map[e][3], gun_W, gun_H, gun_m, gun_mirror, gun_f_eye);
        }
        vr_d3d_restore();
    }
    vr_d3d_gun_pass(0, 0, 0, 0);
    int W, H; plat_video_window_size(&W, &H);
    int src_w = 0, src_h = 0;
    if (st == 1 && hud && vr_hud_tex && vr_hud_w > 0) { s.panel = 1; s.ptex = vr_hud_tex; src_w = vr_hud_w; src_h = vr_hud_h; }
    else if (st != 1 && W > 0 && H > 0) { s.panel = 2; src_w = W; src_h = H; }
    if (vr_input_captured() || !panel_valid) place_panel(0);     /* the HUD follows the head in play; it holds still while a cursor is up */
    double pw, ph; panel_size(src_w, src_h, &pw, &ph);
    controls(st == 1, pw, ph);
    if (s.panel && panel_valid) {
        ndvr_panel_part *pp = s.part; float r = pan_curved ? (float)hud_radius : 0;
        if (s.panel == 1 && pan_curved && hud_split > 0 && vr_input_captured()) {
            /* In play the HUD's corners (health, ammo) are pulled in towards the middle of the view: three strips, the
             * middle one where it belongs, the outer ones turned in by NDVR_HUD_PULL and raised by NDVR_HUD_LIFT degrees. */
            int sw = (int)(src_w * hud_split + 0.5); double side = pw * hud_split, mid = pw - 2 * side;
            double off = (mid / 2 + side / 2) / hud_radius - hud_pull * PI_D / 180.0, lift = hud_lift * PI_D / 180.0;
            for (int k = 0; k < 3; k++) {
                double yaw = k == 0 ? off : k == 2 ? -off : 0, pit = k == 1 ? 0 : lift;   /* left of the image = turned left (+yaw) */
                ndvr_quat qy = { 0, (float)sin(yaw / 2), 0, (float)cos(yaw / 2) }, qx = { (float)sin(pit / 2), 0, 0, (float)cos(pit / 2) };
                pp[k].pose = panel_pose; pp[k].pose.orientation = qmul(qmul(panel_pose.orientation, qy), qx);
                pp[k].x = k == 0 ? 0 : k == 1 ? sw : src_w - sw; pp[k].w = k == 1 ? src_w - 2 * sw : sw;
                pp[k].width_m = (float)(k == 1 ? mid : side); pp[k].height_m = (float)ph; pp[k].radius_m = r;
            }
            s.nparts = 3;
        } else {
            pp[0].pose = panel_pose; pp[0].x = 0; pp[0].w = src_w; pp[0].width_m = (float)pw; pp[0].height_m = (float)ph; pp[0].radius_m = r;
            s.nparts = 1;
        }
        s.hole = s.panel == 1 && !hud_crosshair ? (src_w * 28 + 1279) / 1280 : 0;   /* the game's crosshair (16 px at 1280) points the head's way: hide it */
        s.cx = ptr_u >= 0 ? (int)(ptr_u * src_w) : -1; s.cy = ptr_v >= 0 ? (int)(ptr_v * src_h) : -1;
    } else s.panel = 0;
    plat_gfx_deferred(rt_submit, &s, sizeof s);
}

/* ---------------------------------------------------------------- calibration (NDVR_CALIB=1) */
#define CAP_MAX (3 * 400000)
static float *cap[4]; static int cap_n[4];
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double median(double *v, int n) { if (n <= 0) return NAN; qsort(v, (size_t)n, sizeof *v, dcmp); return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]); }
/* pairs the vertices of two captures draw by draw (stops at the first draw whose vertex count differs) */
static int pair_up(const float *a, int na, const float *b, int nb, double *pa, double *pb, int max, int *draws) {
    int ia = 0, ib = 0, k = 0; *draws = 0;
    while (ia + 3 <= na && ib + 3 <= nb && k < max) {
        if (a[ia] != -1e30f || b[ib] != -1e30f) break;
        int sa = (int)a[ia + 1], sb = (int)b[ib + 1]; ia += 3; ib += 3;
        if (sa != sb || ia + 3 * sa > na || ib + 3 * sb > nb) break;
        (*draws)++;
        for (int i = 0; i < sa && k < max; i++) {
            const float *x = a + ia + 3 * i, *y = b + ib + 3 * i;
            if (!isfinite(x[0]) || !isfinite(x[1]) || !isfinite(y[0]) || !isfinite(y[1]) || !(x[2] > 0) || !(y[2] > 0)) continue;
            pa[2 * k] = x[0]; pa[2 * k + 1] = x[1]; pb[2 * k] = y[0]; pb[2 * k + 1] = y[1]; k++;
        }
        ia += 3 * sa; ib += 3 * sa;
    }
    return k;
}
/* focal length (pixels) from a rotation by t = tan(delta) about one screen axis; u, u' relative to the centre:
 * u' = (u - s t f) / (1 + s t u / f), s = +1 if the points moved toward negative u. */
static void rot_estimate(const double *pa, const double *pb, int n, int axis, double centre, double half, double t, double *f_out, double *shift_out, int *used) {
    static double sh[400000], fs[400000]; int m = 0;
    for (int i = 0; i < n && m < 400000; i++) { double u = pa[2 * i + axis] - centre; if (fabs(u) < 0.25 * half) sh[m++] = pb[2 * i + axis] - pa[2 * i + axis]; }
    double med = median(sh, m); *shift_out = med; *used = m;
    if (!(fabs(med) > 0.5)) { *f_out = NAN; return; }
    double s = med > 0 ? -1.0 : 1.0, f0 = fabs(med) / t; int q = 0;
    for (int i = 0; i < n && q < 400000; i++) {
        double u = pa[2 * i + axis] - centre, up = pb[2 * i + axis] - centre;
        if (fabs(u) > 0.6 * half) continue;
        double A = t, B = s * (up - u), C = t * u * up, D = B * B - 4 * A * C;     /* t f^2 + s (u' - u) f + t u u' = 0 */
        if (D < 0) continue;
        double r1 = (-B + sqrt(D)) / (2 * A), r2 = (-B - sqrt(D)) / (2 * A), f = fabs(r1 - f0) < fabs(r2 - f0) ? r1 : r2;
        if (f > 0.3 * f0 && f < 3 * f0) fs[q++] = f;
    }
    *f_out = median(fs, q);
}
static void calib_report(int W, int H, double delta_deg, double side_units, double zoom) {
    static double pa[2 * 400000], pb[2 * 400000];
    double t = tan(delta_deg * PI_D / 180.0); int n, draws, used;
    double fx = NAN, fy = NAN, shx = 0, shy = 0;
    port_debug("NDVR CALIB: captured vertices base %d, heading+ %d, pitch+ %d, side+ %d (zoom %.3f, canvas %dx%d)", cap_n[0] / 3, cap_n[1] / 3, cap_n[2] / 3, cap_n[3] / 3, zoom, W, H);
    n = pair_up(cap[0], cap_n[0], cap[1], cap_n[1], pa, pb, 400000, &draws);
    rot_estimate(pa, pb, n, 0, W * 0.5, W * 0.5, t, &fx, &shx, &used);
    port_debug("NDVR CALIB: heading +%.1f deg: %d matched vertices in %d draws, centre points moved %+.1f px -> heading increase turns %s; focal %.1f px -> horizontal fov %.2f deg",
               delta_deg, n, draws, shx, shx > 0 ? "LEFT" : shx < 0 ? "RIGHT" : "?", fx, 2 * atan(W * 0.5 / fx) * 180 / PI_D);
    n = pair_up(cap[0], cap_n[0], cap[2], cap_n[2], pa, pb, 400000, &draws);
    rot_estimate(pa, pb, n, 1, H * 0.5, H * 0.5, t, &fy, &shy, &used);
    port_debug("NDVR CALIB: pitch +%.1f deg: %d matched vertices in %d draws, centre points moved %+.1f px (screen y down) -> pitch increase looks %s; focal %.1f px -> vertical fov %.2f deg",
               delta_deg, n, draws, shy, shy > 0 ? "UP" : shy < 0 ? "DOWN" : "?", fy, 2 * atan(H * 0.5 / fy) * 180 / PI_D);
    n = pair_up(cap[0], cap_n[0], cap[3], cap_n[3], pa, pb, 400000, &draws);
    { static double sx[400000]; int m = 0; for (int i = 0; i < n; i++) sx[m++] = pb[2 * i] - pa[2 * i];
      double med = median(sx, m);
      port_debug("NDVR CALIB: side +%.2f units along (-sin h, cos h): %d matched vertices, points moved %+.2f px -> that axis points to the camera's %s",
                 side_units, n, med, med < 0 ? "RIGHT" : med > 0 ? "LEFT" : "?"); }
    if (isfinite(fx) && isfinite(fy)) port_debug("NDVR CALIB: pixel aspect fy/fx = %.4f (1.0 = square pixels)", fy / fx);
}

int vr_scene_hook(CPU *c) {
    if (busy) return 0;                                           /* our own nested call: let the original run */
    if (mode < 0) init_env();
    if (zoom_mul > 0) {                                           /* scale the engine's zoom argument (a double at [esp+4]) */
        double z; uint32_t lo = RD32(c->esp + 4), hi = RD32(c->esp + 8); uint64_t b = (uint64_t)hi << 32 | lo; memcpy(&z, &b, 8);
        if (z > 0.05 && z < 20) { z *= zoom_mul; memcpy(&b, &z, 8); WR32(c->esp + 4, (uint32_t)b); WR32(c->esp + 8, (uint32_t)(b >> 32)); }
        else if (!zoom_warned++) port_debug("NDVR: the zoom argument looks wrong (%g), NDVR_ZOOM ignored", z);
    }
    if (!mode) return 0;
    int W, H; plat_video_window_size(&W, &H);
    if (W <= 0 || H <= 0) return 0;
    if (mode == 2) {
        if (xr_state == 0) glt_call(rt_init, NULL);
        if (xr_state < 0) { mode = 0; return 0; }
    }
    if (mode != 2 && (!eye_tex[0] || W != tex_w || H != tex_h)) {
        for (int e = 0; e < 2; e++) { if (eye_tex[e]) plat_tex_destroy(eye_tex[e]); eye_tex[e] = plat_tex_create(W, H, 1, PLAT_TEX_RGBA8, 1); }
        tex_w = W; tex_h = H;
        if (!eye_tex[0] || !eye_tex[1]) { port_debug("NDVR: could not create the eye textures"); mode = 0; return 0; }
    }
    if (mode == 3) {
        if (++frames % 240 != 120) return 0;                      /* one measurement every 4 seconds; other frames render normally */
        for (int k = 0; k < 4; k++) if (!cap[k]) cap[k] = malloc(sizeof(float) * CAP_MAX);
        uint32_t p = c->eax; uint8_t keep_cam[24], keep_copy[32];
        memcpy(keep_cam, GP(p), sizeof keep_cam); memcpy(keep_copy, GP(CAM_COPY), sizeof keep_copy);
        double h = (double)RD16(p + 20) * (2.0 * PI_D / 65536.0), delta = 2.0, side = 0.5;
        uint16_t d16 = ang16(delta * PI_D / 180.0);
        double z; { uint64_t b = (uint64_t)RD32(c->esp + 8) << 32 | RD32(c->esp + 4); memcpy(&z, &b, 8); }
        CPU saved = *c; busy = 1;
        for (int k = 0; k < 4; k++) {
            *c = saved; memcpy(GP(p), keep_cam, sizeof keep_cam);
            if (k == 1) WR16(p + 20, (uint16_t)(RD16(p + 20) + d16));
            if (k == 2) WR16(p + 18, (uint16_t)(RD16(p + 18) + d16));
            if (k == 3) { wrf(p, rdf(p) - (float)(sin(h) * side)); wrf(p + 4, rdf(p + 4) + (float)(cos(h) * side)); }
            vr_cap_buf = cap[k]; vr_cap_max = CAP_MAX; vr_cap_n = 0; vr_cap_on = 1;
            vr_d3d_eye_begin(eye_tex[0], W, H);
            nd_00601430(c);
            vr_cap_on = 0; cap_n[k] = vr_cap_n;
        }
        busy = 0; *c = saved;                                     /* back to the entry state: the original renders the frame normally */
        memcpy(GP(p), keep_cam, sizeof keep_cam); memcpy(GP(CAM_COPY), keep_copy, sizeof keep_copy);
        vr_d3d_restore();
        port_debug("NDVR CALIB: camera %.2f %.2f %.2f bank %u pitch %u heading %u", rdf(p), rdf(p + 4), rdf(p + 8), RD16(p + 16), RD16(p + 18), RD16(p + 20));
        calib_report(W, H, delta, side, z);
        return 0;
    }
    clock_t t0 = clock();
    int vr = 0;
    if (mode == 2) {
        if (frame_state) return 0;                                /* a second scene render in one frame: the engine draws it normally */
        glt_call(rt_begin, NULL);                                 /* waits for the headset's frame timing, polls tracking */
        frame_state = 2;
        vr = fr_ok && fr.should_render && (fr.head.flags & NDVR_POSE_ORIENT_VALID);
        if (!vr) {                                                /* nothing to draw this frame: close it and let the engine render normally */
            Submit s; memset(&s, 0, sizeof s); plat_gfx_deferred(rt_submit, &s, sizeof s);
            return 0;
        }
        frame_state = 1;
        if (!body_valid) { body_yaw = (double)RD16(c->eax + 20) * (2.0 * PI_D / 65536.0); body_valid = 1; }
        eng_h = RD16(c->eax + 20); eng_p = RD16(c->eax + 18);    /* the engine's own aim this frame (mouse look) */
        track_update();
    }

    uint32_t p = c->eax;
    uint8_t keep_cam[24], keep_copy[32];
    memcpy(keep_cam, GP(p), sizeof keep_cam); memcpy(keep_copy, GP(CAM_COPY), sizeof keep_copy);
    float x = rdf(p), y = rdf(p + 4), z = rdf(p + 8);
    double body_h = (double)RD16(p + 20) * (2.0 * PI_D / 65536.0);
    CPU saved = *c; busy = 1;
    Submit sub; memset(&sub, 0, sizeof sub);
    /* The engine's projection, measured with NDVR_CALIB: square pixels, focal length = canvas height / 1.5 at zoom 1
     * (90 degrees across a 4:3 frame, widened for wider screens), and the focal length scales with the zoom argument.
     * In the headset the zoom is chosen so the engine's symmetric frustum covers both eyes' fields of view. */
    double asp = (double)W / (double)H, zarg = 1.0;
    { uint64_t b = (uint64_t)RD32(c->esp + 8) << 32 | RD32(c->esp + 4); memcpy(&zarg, &b, 8); if (!(zarg > 0.05 && zarg < 20)) zarg = 0; }
    uint32_t zlo0 = RD32(c->esp + 4), zhi0 = RD32(c->esp + 8);       /* the engine's own zoom, for the weapon capture */
    if (mode == 2 && zarg > 0 && !(zoom_mul > 0)) {
        double need = 0;                                          /* largest tan(half angle) the headset shows, in vertical units */
        for (int e = 0; e < 2; e++) {
            const ndvr_fov *f = &fr.eye[e].fov; double t[4] = { tan(-f->left) / asp, tan(f->right) / asp, tan(f->up), tan(-f->down) };
            for (int i = 0; i < 4; i++) if (t[i] > need) need = t[i];
        }
        if (need > 0.1) {
            double z = 0.75 / (need * 1.02); uint64_t b; memcpy(&b, &z, 8);
            WR32(c->esp + 4, (uint32_t)b); WR32(c->esp + 8, (uint32_t)(b >> 32)); zarg = z;
        }
    }
    double tv = zarg > 0 ? 0.75 / zarg : 0.75, th = tv * asp;
    if (fov_h > 0) { th = tan(fov_h * PI_D / 360.0); tv = fov_v > 0 ? tan(fov_v * PI_D / 360.0) : th / asp; }
    int eng = mode == 2 && gun_enabled && eng_mode == 1;
    if (eng) {                                                    /* the camera pass: the engine's own camera, nothing drawn (see "the weapon drawn by the engine") */
        *c = saved; memcpy(GP(p), keep_cam, sizeof keep_cam);
        eng_nrec = 0; eng_pass = 1;
        vr_d3d_gun_pass(1, 0, 0, 0);
        nd_00601430(c);
        vr_d3d_gun_pass(0, 0, 0, 0);
        eng_restore(); eng_pass = 0;
        vr_d3d_spill(1, NULL, 0, 0, 0, 0);                         /* its weapon reaches Direct3D in the next render: dropped */
        vr_ppl_wzoom = zarg > 0 ? (float)zarg : 1.0f;             /* the weapon is drawn with the eye's projection */
    }
    if (mode == 2) vr_ppl_zoom = zarg > 0 ? (float)zarg : 1.0f;   /* per-pixel lighting rebuilds view positions with the eye renders' zoom */
    for (int e = 0; e < 2; e++) {
        *c = saved;
        if (mode == 1) {
            double s = (e ? 0.5 : -0.5) * ipd;                    /* the camera's right-hand axis is (sin h, -cos h) (NDVR_CALIB) */
            wrf(p, x + (float)(sin(body_h) * s)); wrf(p + 4, y - (float)(cos(body_h) * s));
        } else {
            const ndvr_pose *ep = &fr.eye[e].pose;
            HeadAngles ha = head_angles(ep->orientation);
            double yaw_rel = wrap_pi(ha.yaw - ref_yaw);
            double heading = body_yaw + (flip_yaw ? -1.0 : 1.0) * yaw_rel;   /* measured: engine heading grows turning left, like head yaw */
            double pitch = (flip_pitch ? 1.0 : -1.0) * ha.pitch;             /* measured (NDVR_CALIB): positive engine pitch looks down */
            double bank = (flip_roll ? -1.0 : 1.0) * ha.roll;
            V3 d = v3(ep->position.x - ref_pos.x, ep->position.y - ref_pos.y, ep->position.z - ref_pos.z);
            if (no_pos) d = v3(0, 0, 0);
            V3 fr_ref = v3(-sin(ref_yaw), 0, -cos(ref_yaw)), rr_ref = v3(cos(ref_yaw), 0, -sin(ref_yaw));
            double along_f = dot(d, fr_ref), along_r = (flip_side ? -1.0 : 1.0) * dot(d, rr_ref), along_u = d.y;
            double k = 1.0 / unit_m;
            /* forward (cos h, sin h), right (sin h, -cos h): both measured with NDVR_CALIB */
            double ex = (cos(body_yaw) * along_f + sin(body_yaw) * along_r) * k, ey = (sin(body_yaw) * along_f - cos(body_yaw) * along_r) * k;
            wrf(p, x + (float)ex); wrf(p + 4, y + (float)ey); wrf(p + 8, z + (float)(along_u * k));
            WR16(p + 16, ang16(bank)); WR16(p + 18, ang16(pitch)); WR16(p + 20, ang16(heading));
            sub.pose[e] = *ep;
            /* The eye texture covers exactly this eye's field of view at the headset's resolution. The engine draws its whole
             * (wider, symmetric) frame through a GL viewport placed so that the frame's tangent space lines up with the eye's;
             * whatever falls outside the texture is clipped by the GPU. */
            const ndvr_fov *f = &fr.eye[e].fov;
            double L = tan(f->left), R = tan(f->right), U = tan(f->up), D = tan(f->down);
            int tw = (int)(fr.eye[e].width * res_scale + 0.5), tht = (int)(fr.eye[e].height * res_scale + 0.5);
            if (!hmd_tex[e] || hmd_w[e] != tw || hmd_h[e] != tht) {
                if (hmd_tex[e]) plat_tex_destroy(hmd_tex[e]);
                hmd_tex[e] = plat_tex_create(tw, tht, 1, PLAT_TEX_RGBA8, 1); hmd_w[e] = tw; hmd_h[e] = tht;
                port_debug("NDVR: eye %d texture %dx%d, field of view L %.1f R %.1f U %.1f D %.1f deg", e, tw, tht,
                           f->left * 180 / PI_D, f->right * 180 / PI_D, f->up * 180 / PI_D, f->down * 180 / PI_D);
            }
            double px = tw / (R - L), py = tht / (U - D);                         /* texture pixels per unit of tangent */
            vr_d3d_view_map(1, (float)((-th - L) * px), (float)((U - tv) * py), (float)(2 * th * px / W), (float)(2 * tv * py / H));
            eye_map[e][0] = (float)((-th - L) * px); eye_map[e][1] = (float)((U - tv) * py); eye_map[e][2] = (float)(2 * th * px / W); eye_map[e][3] = (float)(2 * tv * py / H);
            sub.fov[e] = *f;
            vr_d3d_eye_begin(hmd_tex[e], tw, tht);
            if (eng) { gun_setup(ep); vr_gun_cull_flip = gun_mirror; eng_sf[0] = eng_sf[1] = 0; eng_pass = 2; }
            nd_00601430(c);
            if (eng) { eng_restore(); eng_pass = 0; vr_d3d_spill(2, hmd_tex[e], eye_map[e][0], eye_map[e][1], eye_map[e][2], eye_map[e][3]); }
            continue;
        }
        vr_d3d_eye_begin(eye_tex[e], W, H);
        nd_00601430(c);
    }
    if (mode == 2 && gun_enabled && !eng) {                       /* the engine's own camera, for the weapon drawn after this (see "the held weapon") */
        *c = saved; memcpy(GP(p), keep_cam, sizeof keep_cam);
        double zc = 0.5; uint64_t zb; memcpy(&zb, &zc, 8);          /* a wide view, so less of the weapon is cut at the screen edges */
        WR32(c->esp + 4, (uint32_t)zb); WR32(c->esp + 8, (uint32_t)(zb >> 32));
        vr_d3d_gun_pass(1, 0, 0, 0);
        nd_00601430(c);
        vr_d3d_gun_pass(0, 0, 0, 0);
        WR32(c->esp + 4, zlo0); WR32(c->esp + 8, zhi0);
        gun_f_eye = (float)(H * (zarg > 0 ? zarg : 1.0) / 1.5); gun_W = W; gun_H = H;
    }
    busy = 0;                                                      /* c is now as the original leaves it after returning */
    vr_d3d_view_map(0, 0, 0, 1, 1);
    memcpy(GP(p), keep_cam, sizeof keep_cam);
    memcpy(GP(CAM_COPY), keep_copy, sizeof keep_copy);             /* everything else sees the engine's own camera, */
    if (mode == 2) memcpy(GP(CAM_COPY), keep_cam, 24);            /* as the original would have left it (sound, culling, the aim below) */
    vr_d3d_restore();
    if (mode == 2 && gun_enabled && !eng) vr_d3d_gun_pass(3, (float)(H * 0.5 / 1.5), W * 0.5f, H * 0.5f);   /* keep the weapon the engine draws next */
    if (mode == 1) {
        plat_gfx_copy(eye_tex[0], 0, 0, W, H, NULL, 0, 0, W / 2, H, 1);
        plat_gfx_copy(eye_tex[1], 0, 0, W, H, NULL, W / 2, 0, W - W / 2, H, 1);
    } else {
        { int mw = W, mh = (int)((double)W * hmd_h[0] / hmd_w[0]); if (mh > H) { mh = H; mw = (int)((double)H * hmd_w[0] / hmd_h[0]); }
          plat_gfx_clear(1, 0xFF000000u, 0, 1.0f, 0, 0);
          plat_gfx_copy(hmd_tex[0], 0, 0, hmd_w[0], hmd_h[0], NULL, (W - mw) / 2, (H - mh) / 2, mw, mh, 1); }   /* the desktop window mirrors the left eye */
        sub.rendered = 1; sub.tex[0] = hmd_tex[0]; sub.tex[1] = hmd_tex[1]; sub.flip = flip_y;
        pending_sub = sub;                                        /* submitted at Present, with this frame's HUD (vr_frame_present) */
    }
    acc += clock() - t0;
    if (++frames % 120 == 0) { port_debug("NDVR: scene renders + headset sync took %.2f ms per frame (CPU side), camera %.2f %.2f %.2f heading %u", 1000.0 * acc / CLOCKS_PER_SEC / 120, x, y, z, (unsigned)RD16(p + 20)); acc = 0; }
    return 1;
}
#else
#include "core.h"
int vr_scene_hook(CPU *c) { (void)c; return 0; }
int vr_model_hook(CPU *c) { (void)c; return 0; }
void vr_frame_present(void) {}
#endif
