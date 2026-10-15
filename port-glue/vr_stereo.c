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
 *   NDVR_GUN_X / NDVR_GUN_Y / NDVR_GUN_Z   move the weapon in the hand, in cm (right, up, forward; default 0)
 *   NDVR_GUN_SCALE   weapon size (default 1)
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
extern void vr_d3d_gun_capture(int on, float f, float cx, float cy), vr_d3d_gun_end(void), vr_d3d_gun_replay(const float *m, int mirror, float f);
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
    hud_crosshair = envi("NDVR_HUD_CROSSHAIR", 0); hud_split = envd("NDVR_HUD_SPLIT", 0.21); hud_pull = envd("NDVR_HUD_PULL", 12); hud_lift = envd("NDVR_HUD_LIFT", 6);
    if (hud_split < 0.05 || hud_split > 0.45) hud_split = 0;
    if (hud_radius < 0.3) hud_radius = 0.3; if (hud_angle < 20) hud_angle = 20; if (hud_angle > 170) hud_angle = 170; if (hud_follow < 0) hud_follow = 0; if (hud_follow > 1) hud_follow = 1;
    if (mode == 2) port_debug("NDVR: NewDarkVR build %s", NDVR_BUILD);
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
static void aim_update(const ndvr_hand *hd) {
    V3 f = qrot(hd->aim.orientation, v3(0, 0, -1));
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
 * The engine places its first-person weapon relative to its own camera, which follows the gun hand, so from an eye it
 * is in the wrong place and mostly cut off. Each frame one extra scene render from the engine's camera keeps only the
 * weapon's draws (port/dx/d3d9.c, vr_d3d_gun_*); after each eye they are drawn again, moved by
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
    double D[3] = { (hand_dom ? 1.0 : -1.0) * gun_scale, gun_scale, gun_scale }, k = 1.0 / unit_m;
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
    gun_mirror = !hand_dom;
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

/* ---------------------------------------------------------------- end of a game frame (called from Present) */
static Submit pending_sub;
void vr_frame_present(void) {
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
    if (mode == 2 && gun_enabled) {                               /* the weapon, from the engine's own camera (see "the held weapon") */
        uint32_t zlo = RD32(c->esp + 4), zhi = RD32(c->esp + 8);
        double zc = 0.5; uint64_t zb; memcpy(&zb, &zc, 8);          /* a wide view, so less of the weapon is cut at the screen edges */
        WR32(c->esp + 4, (uint32_t)zb); WR32(c->esp + 8, (uint32_t)(zb >> 32));
        vr_d3d_gun_capture(1, (float)(H * zc / 1.5), W * 0.5f, H * 0.5f);
        nd_00601430(c);
        vr_d3d_gun_capture(0, 0, 0, 0);
        *c = saved; memcpy(GP(p), keep_cam, sizeof keep_cam);
        WR32(c->esp + 4, zlo); WR32(c->esp + 8, zhi); (void)zlo0; (void)zhi0;
        gun_measure();
    }
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
            sub.fov[e] = *f;
            vr_d3d_eye_begin(hmd_tex[e], tw, tht);
            nd_00601430(c);
            if (gun_enabled) { gun_setup(ep); vr_d3d_gun_replay(gun_m, gun_mirror, (float)(H * (zarg > 0 ? zarg : 1.0) / 1.5)); }
            continue;
        }
        vr_d3d_eye_begin(eye_tex[e], W, H);
        nd_00601430(c);
    }
    busy = 0;                                                      /* c is now as the original leaves it after returning */
    vr_d3d_gun_end();
    vr_d3d_view_map(0, 0, 0, 1, 1);
    memcpy(GP(p), keep_cam, sizeof keep_cam);
    memcpy(GP(CAM_COPY), keep_copy, sizeof keep_copy);             /* everything else sees the engine's own camera, */
    if (mode == 2) memcpy(GP(CAM_COPY), keep_cam, 24);            /* as the original would have left it (sound, culling, the aim below) */
    vr_d3d_restore();
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
void vr_frame_present(void) {}
#endif
