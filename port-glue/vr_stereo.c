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
 * Headset camera. The engine's own heading (mouse look, later thumbstick) is the body. The head turns relative to
 * it: engine heading = body heading + head yaw since the last recentre; pitch and roll come from the head; eye
 * position = engine eye position + the head's physical movement since the recentre, turned by the body heading.
 * Recentre: automatic on the first frame, then with the left controller's menu button.
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
extern void vr_d3d_restore(void);
extern unsigned vr_gl_tex_fbo(PlatTexture *t, int *w, int *h);    /* port/backends/sdl2/gl_render.c */
extern void *__stdcall wglGetCurrentDC(void);                      /* opengl32 */
extern void *__stdcall wglGetCurrentContext(void);
extern void glt_call(void (*fn)(void *), void *arg);
extern int vr_cap_on, vr_cap_n, vr_cap_max; extern float *vr_cap_buf;   /* port/dx/d3d9.c */              /* port/backends/sdl2/gl_thread.c: run on the render thread, wait */

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
static int busy, mode = -1;                                       /* 0 off, 1 side-by-side preview, 2 headset */
static float ipd = 0.207f;
static double unit_m = 0.3048, fov_h = 90, fov_v = 0;
static int flip_yaw, flip_pitch, flip_roll, flip_side, flip_y = 1, no_pos;
static unsigned frames; static clock_t acc;
static double zoom_mul = 0; static int zoom_warned;

/* headset */
static int xr_state;                                              /* 0 not tried, 1 running, -1 failed */
static ndvr_frame fr; static int fr_ok;
static int ref_valid; static double ref_yaw; static V3 ref_pos; static uint32_t prev_btn;

static void xr_log(const char *line) { port_debug("NDVR: %s", line); }
static void rt_init(void *a) {
    (void)a; ndvr_set_log(xr_log);
    if (ndvr_init(wglGetCurrentDC(), wglGetCurrentContext())) xr_state = 1;
    else { xr_state = -1; port_debug("NDVR: headset not available: %s", ndvr_last_error()); }
}
static void rt_begin(void *a) { (void)a; fr_ok = ndvr_begin_frame(&fr); }

typedef struct { ndvr_pose pose[2]; ndvr_fov fov[2]; int w[2], h[2]; int mirror; PlatTexture *tex[2]; int flip; int rendered; } Submit;
static void rt_submit(void *data, PlatDrawFn draw) {
    Submit *s = data; (void)draw;
    if (s->rendered) {
        for (int e = 0; e < 2; e++) {
            int w, h; unsigned fbo = vr_gl_tex_fbo(s->tex[e], &w, &h);
            ndvr_blit_eye(e, fbo, w, h, s->flip);
        }
        ndvr_set_submit_views(s->pose, s->fov);
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
    if (mode < 0) {
        mode = envi("NDVR_VR", 0) ? 2 : envi("NDVR_CALIB", 0) ? 3 : envi("NDVR_STEREO", 0) ? 1 : 0;
        zoom_mul = envd("NDVR_ZOOM", 0);
        const char *i = getenv("NDVR_IPD"); if (i && *i) ipd = (float)atof(i);
        unit_m = envd("NDVR_UNIT_M", 0.3048); fov_h = envd("NDVR_FOV_H", 0); fov_v = envd("NDVR_FOV_V", 0);
        flip_yaw = envi("NDVR_FLIP_YAW", 0); flip_pitch = envi("NDVR_FLIP_PITCH", 0); flip_roll = envi("NDVR_FLIP_ROLL", 0); flip_side = envi("NDVR_FLIP_SIDE", 0);
        flip_y = envi("NDVR_FLIP_Y", 1); no_pos = envi("NDVR_NO_POS", 0);
        if (mode || zoom_mul > 0) port_debug("NDVR: %s mode; unit %.4f m, fov_h %.1f, fov_v %.1f, flips yaw %d pitch %d roll %d side %d y %d, no_pos %d, zoom x%.3f",
            mode == 2 ? "headset" : mode == 3 ? "calibration" : mode == 1 ? "side-by-side preview" : "normal", unit_m, fov_h, fov_v, flip_yaw, flip_pitch, flip_roll, flip_side, flip_y, no_pos, zoom_mul > 0 ? zoom_mul : 1.0);
    }
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
    if (!eye_tex[0] || W != tex_w || H != tex_h) {
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
        glt_call(rt_begin, NULL);                                 /* waits for the headset's frame timing, polls tracking */
        vr = fr_ok && fr.should_render && (fr.head.flags & NDVR_POSE_ORIENT_VALID);
        if (!vr) {                                                /* nothing to draw this frame: close it and let the engine render normally */
            Submit s; memset(&s, 0, sizeof s); plat_gfx_deferred(rt_submit, &s, sizeof s);
            return 0;
        }
        if (!ref_valid || ((fr.hand[0].buttons & NDVR_BTN_MENU) && !(prev_btn & NDVR_BTN_MENU))) {
            HeadAngles ha = head_angles(fr.head.orientation);
            ref_yaw = ha.yaw; ref_pos = v3(fr.head.position.x, fr.head.position.y, fr.head.position.z); ref_valid = 1;
        }
        prev_btn = fr.hand[0].buttons;
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
    for (int e = 0; e < 2; e++) {
        *c = saved;
        if (mode == 1) {
            double s = (e ? 0.5 : -0.5) * ipd;                    /* the camera's right-hand axis is (sin h, -cos h) (NDVR_CALIB) */
            wrf(p, x + (float)(sin(body_h) * s)); wrf(p + 4, y - (float)(cos(body_h) * s));
        } else {
            const ndvr_pose *ep = &fr.eye[e].pose;
            HeadAngles ha = head_angles(ep->orientation);
            double yaw_rel = wrap_pi(ha.yaw - ref_yaw);
            double heading = body_h + (flip_yaw ? -1.0 : 1.0) * yaw_rel;     /* measured: engine heading grows turning left, like head yaw */
            double pitch = (flip_pitch ? 1.0 : -1.0) * ha.pitch;             /* measured (NDVR_CALIB): positive engine pitch looks down */
            double bank = (flip_roll ? -1.0 : 1.0) * ha.roll;
            V3 d = v3(ep->position.x - ref_pos.x, ep->position.y - ref_pos.y, ep->position.z - ref_pos.z);
            if (no_pos) d = v3(0, 0, 0);
            V3 fr_ref = v3(-sin(ref_yaw), 0, -cos(ref_yaw)), rr_ref = v3(cos(ref_yaw), 0, -sin(ref_yaw));
            double along_f = dot(d, fr_ref), along_r = (flip_side ? -1.0 : 1.0) * dot(d, rr_ref), along_u = d.y;
            double k = 1.0 / unit_m;
            /* forward (cos h, sin h), right (sin h, -cos h): both measured with NDVR_CALIB */
            double ex = (cos(body_h) * along_f + sin(body_h) * along_r) * k, ey = (sin(body_h) * along_f - cos(body_h) * along_r) * k;
            wrf(p, x + (float)ex); wrf(p + 4, y + (float)ey); wrf(p + 8, z + (float)(along_u * k));
            WR16(p + 16, ang16(bank)); WR16(p + 18, ang16(pitch)); WR16(p + 20, ang16(heading));
            sub.pose[e] = *ep;
            sub.fov[e].left = (float)-atan(th); sub.fov[e].right = (float)atan(th); sub.fov[e].up = (float)atan(tv); sub.fov[e].down = (float)-atan(tv);
        }
        vr_d3d_eye_begin(eye_tex[e], W, H);
        nd_00601430(c);
    }
    busy = 0;                                                      /* c is now as the original leaves it after returning */
    memcpy(GP(p), keep_cam, sizeof keep_cam);
    memcpy(GP(CAM_COPY), keep_copy, sizeof keep_copy);             /* everything else sees the engine's own camera */
    vr_d3d_restore();
    if (mode == 1) {
        plat_gfx_copy(eye_tex[0], 0, 0, W, H, NULL, 0, 0, W / 2, H, 1);
        plat_gfx_copy(eye_tex[1], 0, 0, W, H, NULL, W / 2, 0, W - W / 2, H, 1);
    } else {
        plat_gfx_copy(eye_tex[0], 0, 0, W, H, NULL, 0, 0, W, H, 1);   /* the desktop window mirrors the left eye */
        sub.rendered = 1; sub.tex[0] = eye_tex[0]; sub.tex[1] = eye_tex[1]; sub.flip = flip_y;
        plat_gfx_deferred(rt_submit, &sub, sizeof sub);
    }
    acc += clock() - t0;
    if (++frames % 120 == 0) { port_debug("NDVR: scene renders + headset sync took %.2f ms per frame (CPU side), camera %.2f %.2f %.2f heading %u", 1000.0 * acc / CLOCKS_PER_SEC / 120, x, y, z, (unsigned)RD16(p + 20)); acc = 0; }
    return 1;
}
#else
#include "core.h"
int vr_scene_hook(CPU *c) { (void)c; return 0; }
#endif
