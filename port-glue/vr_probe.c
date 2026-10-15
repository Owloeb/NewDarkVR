/* NewDarkVR probe: finds out how the engine draws the HUD and the held weapon, and where a shot's direction comes from.
 * Not part of the vanilla port; tools/install_into_port.py --probe copies this and vr_probe.h into port/core/.
 * Measurement only: with NDVR_PROBE unset it costs one integer test per draw, with it set it logs and changes nothing
 * (except the optional NDVR_AIMTEST experiment below).
 *
 *   NDVR_PROBE=1         switch the probe on (run it WITHOUT NDVR_VR / NDVR_STEREO: the stereo hook draws the scene twice)
 *   NDVR_PROBE_IDLE=n    capture two frames in detail when frame n is presented (default 360; 0 = never)
 *
 * What it logs (lines start with "NDVR PROBE", written with port_log, so no --verbose needed):
 *   - every 300 frames a one-line summary: draws, draws without depth test, pre-transformed draws, draws between
 *     d3d_display_set_overlay and d3d_display_end_overlay, clears, depth clears;
 *   - a "capture" is every draw, clear, BeginScene/EndScene and hooked engine function of one frame, in order. Each draw
 *     line gives the FVF, depth flags, blend, texture and size, the vertex count and the bounding box of the vertices
 *     (x, y, z and 1/w when pre-transformed) plus viewport and scissor. The HUD should show up as draws without depth
 *     test or inside the overlay markers; the held weapon as draws after a second depth clear (zc=2) or with a squashed z range;
 *   - the first time each hooked engine function runs, with its first stack words;
 *   - captures start by themselves for the first six presses of the fire key, and at the idle frame.
 *
 * The fire log: when cmd_fire_weapon (0x0043b220) runs it prints the engine's render camera copy (0xa57e00), the
 * player camera object pointer (*0xa3acb8) and every place in that object, in the things it points to, and in the
 * exe's global data that holds a position within 8 units of the camera (floats x, y, z) and tells whether
 * its bank/pitch/heading words (two layouts exist) equal the camera's. One of them is where the engine keeps the player's facing, which is
 * what a shot is aimed with.
 *
 *   NDVR_AIMTEST_HEAD=deg NDVR_AIMTEST_PITCH=deg   experiment: when the fire key goes down, add this to every copy of the
 *                        facing that matches the camera (or only candidate NDVR_AIMTEST_CAND=n) for NDVR_AIMTEST_FRAMES
 *                        frames (default 1), then put it back. Only "fire..." key commands trigger it. If the shot lands
 *                        that many degrees off the crosshair, one of those copies is what aims; narrow it down with _CAND.
 *   NDVR_PERSIST=deg     experiment: on each fire press turn ONE of those copies (the next one each shot) and never put it
 *                        back; 10 frames later log whether the value stuck (a master copy) and whether the view turned.
 * Also logged on each press: every place holding the camera's forward direction as three floats (a vector or a
 * matrix row), in the camera object and in global data. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "core.h"
#include "vr_probe.h"

#define CAM_COPY   0x00a57e00u
#define PLAYER_CAM 0x00a3acb8u
#define GLOBALS_LO 0x00780000u
#define GLOBALS_HI 0x00a80000u
#define P(fmt, ...) port_log("NDVR PROBE " fmt, ##__VA_ARGS__)

int vr_probe_on;
static int idle_frame = 360, aim_frames = 1, aim_cand = 0;
static double aim_head, aim_pitch;
static unsigned frame_no, seq, n_draw, n_noz, n_rhw, n_ovl, n_clear, fires;
static int cap_left, overlay, zclears;
#define AIM_MAX 16
static struct { uint32_t pitch_at, head_at; uint16_t keep_p, keep_h; } aim_t[AIM_MAX]; static int aim_n, aim_hold;
static double persist_deg; static int persist_k, persist_wait; static uint32_t persist_at, persist_cam_before; static uint16_t persist_wrote;

static int envi(const char *n, int d) { const char *e = getenv(n); return e && *e ? atoi(e) : d; }
static double envd(const char *n, double d) { const char *e = getenv(n); return e && *e ? atof(e) : d; }
static float bf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static float rdf(uint32_t a) { return bf(RD32(a)); }

__attribute__((constructor)) static void probe_init(void) {
    vr_probe_on = envi("NDVR_PROBE", 0) != 0;
    idle_frame = envi("NDVR_PROBE_IDLE", 360);
    aim_head = envd("NDVR_AIMTEST_HEAD", 0); aim_pitch = envd("NDVR_AIMTEST_PITCH", 0);
    aim_frames = envi("NDVR_AIMTEST_FRAMES", 1); if (aim_frames < 1) aim_frames = 1;
    aim_cand = envi("NDVR_AIMTEST_CAND", -1);
    persist_deg = envd("NDVR_PERSIST", 0);
}

void vr_probe_draw(const VrProbeDraw *d) {
    n_draw++; if (!d->ztest) n_noz++; if (d->rhw) n_rhw++; if (overlay) n_ovl++;
    if (cap_left <= 0 || seq >= 400) return;
    float lo[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, hi[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
    uint32_t n = d->span < 4096 ? d->span : 4096;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = d->vaddr + i * d->stride + (uint32_t)d->pos; float v[4];
        if (!g_valid(a, 16)) continue;
        memcpy(v, GP(a), 16);
        for (int k = 0; k < (d->rhw ? 4 : 3); k++) { if (!isfinite(v[k])) continue; if (v[k] < lo[k]) lo[k] = v[k]; if (v[k] > hi[k]) hi[k] = v[k]; }
    }
    P("D%03u %szc=%d fvf=%03X %s z%d%d/f%d bl%d(%d,%d) at%d nt%d tex=%x(%ux%u) tri=%u v=%u x[%.0f,%.0f] y[%.0f,%.0f] z[%.4f,%.4f]%s vp=%.0f,%.0f,%.0fx%.0f z%.2f-%.2f sc=%d,%d,%d,%d",
      seq, overlay ? "OVERLAY " : "", zclears, d->fvf, d->rhw ? "RHW" : "3D ",
      d->ztest, d->zwrite, d->zfunc, d->blend, d->src_blend, d->dst_blend, d->atest, d->ntex, d->tex, d->tex_w, d->tex_h,
      d->prims, d->span, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], "", (double)bf(d->viewport[0]), (double)bf(d->viewport[1]),
      (double)bf(d->viewport[2]), (double)bf(d->viewport[3]), (double)bf(d->viewport[4]), (double)bf(d->viewport[5]),
      d->scissor[0], d->scissor[1], d->scissor[2], d->scissor[3]);
    if (d->rhw && lo[3] <= hi[3]) P("     1/w[%.5f,%.5f]", lo[3], hi[3]);
    seq++;
}

void vr_probe_clear(uint32_t count, uint32_t flags, uint32_t color, uint32_t z_bits) {
    n_clear++;
    if (flags & 2) zclears++;                                                 /* D3DCLEAR_ZBUFFER */
    if (cap_left > 0) P("CLEAR rects=%u flags=%u (%s%s%s) color=%08x z=%.3f", count, flags, flags & 1 ? "target " : "", flags & 2 ? "depth " : "", flags & 4 ? "stencil" : "", color, (double)bf(z_bits));
}

void vr_probe_scene(int begin) { if (cap_left > 0) P("%s", begin ? "BeginScene" : "EndScene"); }

void vr_probe_event(CPU *c, const char *name, int ov) {
    if (!vr_probe_on) return;
    if (ov >= 0) overlay = ov;
    static const char *seen[24]; static int nseen; int first = 1;
    for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], name)) first = 0;
    if (first && nseen < 24) seen[nseen++] = name;
    if (first || cap_left > 0) {
        char b[160]; int n = 0;
        for (int i = 0; i < 4 && g_valid(c->esp + 4 + 4u * (uint32_t)i, 4); i++) n += snprintf(b + n, sizeof b - (size_t)n, " %08x", RD32(c->esp + 4 + 4u * (uint32_t)i));
        P("%sEVENT %s (frame %u) eax=%08x ecx=%08x edx=%08x stack:%s", first ? "FIRST " : "", name, frame_no, c->eax, c->ecx, c->edx, b);
    }
}

void vr_probe_present(void) {
    if (!vr_probe_on) return;
    if (cap_left > 0) { P("---- end of frame %u: %u draws (%u without depth test, %u pre-transformed, %u in overlay), %u clears, %d depth clears", frame_no, n_draw, n_noz, n_rhw, n_ovl, n_clear, zclears); cap_left--; }
    else if (frame_no % 300 == 0) P("frame %u: %u draws (%u without depth test, %u pre-transformed, %u in overlay), %u clears, %d depth clears", frame_no, n_draw, n_noz, n_rhw, n_ovl, n_clear, zclears);
    if (aim_hold > 0 && --aim_hold == 0) { for (int i = 0; i < aim_n; i++) { WR16(aim_t[i].pitch_at, aim_t[i].keep_p); WR16(aim_t[i].head_at, aim_t[i].keep_h); } P("AIMTEST: %d facing copies put back", aim_n); aim_n = 0; }
    if (persist_wait > 0 && --persist_wait == 0) {
        int dv = (RD16(persist_at) - persist_wrote) & 0xFFFF, dc = (RD16(CAM_COPY + 20) - (int)persist_cam_before) & 0xFFFF;
        if (dv > 32767) dv -= 65536;
        if (dc > 32767) dc -= 65536;
        P("PERSIST: heading at %08x is now %u (we wrote %u: %s); the camera heading moved %+.1f degrees since (%s)", persist_at, RD16(persist_at), persist_wrote,
          dv > -64 && dv < 64 ? "KEPT, this is a master copy" : "overwritten, derived from somewhere else", dc * 360.0 / 65536.0,
          dc * 360.0 / 65536.0 > persist_deg * 0.5 ? "the view turned with it" : "the view did not turn");
    }
    frame_no++; seq = 0; n_draw = n_noz = n_rhw = n_ovl = n_clear = 0; zclears = 0; overlay = 0;
    if (idle_frame > 0 && frame_no == (unsigned)idle_frame) { cap_left = 2; P("---- idle capture of frames %u and %u ----", frame_no, frame_no + 1); }
}

/* ---- the fire log ---- */
typedef struct { uint32_t addr, base; float pos[3], dist; } Cand;
static int scan(uint32_t base, uint32_t len, const float cam[3], Cand *out, int max, int n) {
    for (uint32_t o = 0; o + 24 <= len && n < max; o += 4) {
        uint32_t a = base + o; if (!g_valid(a, 24)) break;
        if (a >= CAM_COPY && a < CAM_COPY + 32) continue;                     /* the camera copy itself */
        float v[3]; memcpy(v, GP(a), 12); int ok = 1; float d = 0;
        for (int k = 0; k < 3; k++) { if (!isfinite(v[k])) { ok = 0; break; } float e = fabsf(v[k] - cam[k]); if (e > d) d = e; }
        if (!ok || d > 8.0f) continue;
        int dup = 0; for (int i = 0; i < n; i++) if (out[i].addr == a) dup = 1;
        if (dup) continue;
        out[n].addr = a; out[n].base = base; memcpy(out[n].pos, v, 12); out[n].dist = d; n++;
    }
    return n;
}
static int ang_close(int a, int b) { int d = (a - b) & 0xFFFF; if (d > 32767) d -= 65536; return d > -16 && d < 16; }
static int is_press(CPU *c) {                                                  /* an argument string like "1.0" (key down) */
    for (int i = 0; i < 4; i++) {
        uint32_t w = RD32(c->esp + 4 + 4u * (uint32_t)i);
        if (w >= 0x10000u && g_valid(w, 4)) { const char *s = (const char *)GP(w); if ((s[0] >= '0' && s[0] <= '9') || s[0] == '-') return atof(s) != 0.0; }
    }
    return 0;
}
int vr_probe_fire(CPU *c) {
    if (!vr_probe_on) return 0;
    int press = is_press(c);
    const char *cmd = ""; { uint32_t w = RD32(c->esp + 4); if (w >= 0x10000u && g_valid(w, 16)) cmd = (const char *)GP(w); }
    int is_fire = !strncmp(cmd, "fire", 4);                                  /* "fire_weapon"; the handler is shared by all key commands */
    char b[400]; int n = 0;
    for (int i = 0; i < 6 && g_valid(c->esp + 4 + 4u * (uint32_t)i, 4); i++) {
        uint32_t w = RD32(c->esp + 4 + 4u * (uint32_t)i); n += snprintf(b + n, sizeof b - (size_t)n, " %08x", w);
        if (w >= 0x10000u && g_valid(w, 8)) { const char *s = (const char *)GP(w); int ok = 1; for (int k = 0; k < 6 && s[k]; k++) if ((unsigned char)s[k] < 32 || (unsigned char)s[k] > 126) ok = 0; if (ok && s[0]) n += snprintf(b + n, sizeof b - (size_t)n, "(\"%.20s\")", s); }
    }
    P("FIRE command (frame %u, %s) eax=%08x ecx=%08x edx=%08x stack:%s", frame_no, press ? "press" : "release/other", c->eax, c->ecx, c->edx, b);
    if (!press || !is_fire) return 0;
    if (fires < 6) { cap_left = 3; seq = 0; zclears = 0; }
    fires++;
    float cam[3] = { rdf(CAM_COPY), rdf(CAM_COPY + 4), rdf(CAM_COPY + 8) };
    P("FIRE render camera copy: pos (%.3f, %.3f, %.3f) cell %08x bank %u pitch %u heading %u (65536 = 360 deg)", (double)cam[0], (double)cam[1], (double)cam[2], RD32(CAM_COPY + 12), RD16(CAM_COPY + 16), RD16(CAM_COPY + 18), RD16(CAM_COPY + 20));
    uint32_t obj = RD32(PLAYER_CAM);
    P("FIRE *0xa3acb8 = %08x (%s)", obj, g_valid(obj, 64) ? "looks like a pointer" : "not a pointer: an object id?");
    static Cand cand[24]; int nc = 0;
    if (g_valid(obj, 64)) {
        nc = scan(obj, 0x400, cam, cand, 24, nc);
        for (uint32_t o = 0; o < 0x100 && nc < 24; o += 4) { uint32_t w = RD32(obj + o); if (w >= 0x10000u && w != obj && g_valid(w, 0x200)) nc = scan(w, 0x200, cam, cand, 24, nc); }
    }
    int nobj = nc;
    nc = scan(GLOBALS_LO, GLOBALS_HI - GLOBALS_LO, cam, cand, 24, nc);
    P("FIRE %d place(s) holding a position within 8 units of the camera (%d inside/behind the camera object, the rest in global data):", nc, nobj);
    int camp = RD16(CAM_COPY + 18), camh = RD16(CAM_COPY + 20), nmatch = 0; int layout[24];
    for (int i = 0; i < nc; i++) {
        uint32_t a = cand[i].addr;
        int A = ang_close(RD16(a + 18), camp) && ang_close(RD16(a + 20), camh);       /* bank, pitch, heading at +16, +18, +20 (after a cell word) */
        int B = ang_close(RD16(a + 14), camp) && ang_close(RD16(a + 16), camh);       /* bank, pitch, heading at +12, +14, +16 (straight after the position) */
        layout[i] = A ? 1 : B ? 2 : 0; if (layout[i]) nmatch++;
        P("  CAND %d @%08x (%s) pos (%.3f, %.3f, %.3f) off %.2f  facing layout %s  raw %08x %08x %08x %08x %08x %08x", i, a, i < nobj ? "object" : "global",
          (double)cand[i].pos[0], (double)cand[i].pos[1], (double)cand[i].pos[2], (double)cand[i].dist, A ? "A (+18 pitch, +20 heading) = same as camera" : B ? "B (+14 pitch, +16 heading) = same as camera" : "none/different",
          RD32(a), RD32(a + 4), RD32(a + 8), RD32(a + 12), RD32(a + 16), RD32(a + 20));
    }
    P("FIRE %d of them hold the same facing as the render camera", nmatch);
    {   /* the facing as a direction vector or matrix row: floats (cos h cos p, sin h cos p, +-sin p), heading 0 = +x */
        double h = camh * (2.0 * 3.14159265358979 / 65536.0), pt = (int16_t)camp * (2.0 * 3.14159265358979 / 65536.0);
        float f[3] = { (float)(cos(h) * cos(pt)), (float)(sin(h) * cos(pt)), (float)sin(pt) };
        int hits = 0;
        for (int pass = 0; pass < 2; pass++) {
            uint32_t lo = pass ? GLOBALS_LO : obj, hi = pass ? GLOBALS_HI : obj + 0x400;
            if (!pass && !g_valid(obj, 0x400)) continue;
            for (uint32_t a = lo; a + 12 <= hi && hits < 16; a += 4) {
                if (!g_valid(a, 12)) break;
                float v[3]; memcpy(v, GP(a), 12);
                if (!(fabsf(v[0] - f[0]) < 0.01f && fabsf(v[1] - f[1]) < 0.01f && (fabsf(v[2] - f[2]) < 0.01f || fabsf(v[2] + f[2]) < 0.01f))) continue;
                if (fabsf(f[0]) + fabsf(f[1]) < 0.05f) continue;
                hits++;
                P("  DIR @%08x (%s) (%.4f, %.4f, %.4f)  next %.4f %.4f %.4f %.4f %.4f %.4f", a, pass ? "global" : "object", (double)v[0], (double)v[1], (double)v[2],
                  (double)rdf(a + 12), (double)rdf(a + 16), (double)rdf(a + 20), (double)rdf(a + 24), (double)rdf(a + 28), (double)rdf(a + 32));
            }
        }
        P("FIRE %d place(s) hold the camera's forward direction as floats (%.4f, %.4f, +-%.4f)", hits, (double)f[0], (double)f[1], (double)f[2]);
    }
    if (persist_deg != 0 && persist_wait == 0 && nmatch) {     /* NDVR_PERSIST=deg: turn one copy per shot, never put back, see if it sticks */
        int k = persist_k++ % nmatch, j = -1;
        for (int i = 0; i < nc; i++) if (layout[i] && k-- == 0) { j = i; break; }
        if (j >= 0) {
            persist_at = cand[j].addr + (layout[j] == 1 ? 20u : 16u); persist_cam_before = RD16(CAM_COPY + 20);
            persist_wrote = (uint16_t)(RD16(persist_at) + (int)floor(persist_deg * 65536.0 / 360.0 + 0.5));
            WR16(persist_at, persist_wrote); persist_wait = 10;
            P("PERSIST: CAND %d heading (%08x) turned %+.1f degrees and left there; checking in 10 frames (keep the mouse still)", j, persist_at, persist_deg);
        }
    }
    if ((aim_head != 0 || aim_pitch != 0) && aim_hold == 0) {                       /* NDVR_AIMTEST_CAND=-1 (default): every copy that matches the camera */
        int dh = (int)floor(aim_head * 65536.0 / 360.0 + 0.5), dp = (int)floor(aim_pitch * 65536.0 / 360.0 + 0.5);
        aim_n = 0;
        for (int i = 0; i < nc && aim_n < AIM_MAX; i++) {
            if (!layout[i] || (aim_cand >= 0 && aim_cand != i)) continue;
            uint32_t pa = cand[i].addr + (layout[i] == 1 ? 18u : 14u), ha = cand[i].addr + (layout[i] == 1 ? 20u : 16u);
            aim_t[aim_n].pitch_at = pa; aim_t[aim_n].head_at = ha; aim_t[aim_n].keep_p = RD16(pa); aim_t[aim_n].keep_h = RD16(ha);
            WR16(pa, (uint16_t)(RD16(pa) + dp)); WR16(ha, (uint16_t)(RD16(ha) + dh)); aim_n++;
            P("AIMTEST: CAND %d facing moved by heading %+.1f, pitch %+.1f degrees", i, aim_head, aim_pitch);
        }
        aim_hold = aim_n ? aim_frames : 0;
        if (!aim_n) P("AIMTEST: no matching facing copy to change");
        else P("AIMTEST: held for %d frame(s); look where this shot lands", aim_frames);
    }
    return 0;
}
