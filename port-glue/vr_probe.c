/* NewDarkVR probe: finds out how the engine draws the HUD and the held weapon, and where a shot's direction comes from.
 * Not part of the vanilla port; tools/install_into_port.py --probe copies this and vr_probe.h into port/core/.
 * Measurement only: with NDVR_PROBE unset it costs one integer test per draw, with it set it logs and changes nothing
 * (except the optional NDVR_AIMTEST experiment below).
 *
 *   NDVR_PROBE=1         switch the probe on (run it WITHOUT NDVR_VR / NDVR_STEREO: the stereo hook draws the scene twice)
 *   NDVR_PROBE_IDLE=n    capture two frames in detail when frame n is presented (default 360 with NDVR_PROBE_CAPTURE=1)
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
 *   NDVR_HUNT=1          find the master heading and pitch, then aim-test them (see "NDVR_HUNT" below).
 *   NDVR_MOUSETEST=n     measure how mouse motion turns the camera (see "NDVR_MOUSETEST" below).
 *   NDVR_PROBE_CAPTURE=1 the per-draw frame dumps (idle frame 360 and after each of the first six shots); off by default
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
extern int vr_input_mouse_motion(int dx, int dy);   /* port/win32/user32.c (probe patch) */
extern int vr_heap_arenas(uint32_t *out, int max, uint32_t *arena_size);   /* port/core/heap.c (probe patch) */
#define P(fmt, ...) port_log("NDVR PROBE " fmt, ##__VA_ARGS__)

int vr_probe_on;
static int idle_frame = 360, aim_frames = 1, aim_cand = 0;
static double aim_head, aim_pitch;
static int hunt, capture, mousetest;                                                   /* NDVR_HUNT, see below */
static unsigned frame_no, seq, n_draw, n_noz, n_rhw, n_ovl, n_clear, fires;
static int cap_left, overlay, zclears;
#define AIM_MAX 16
static struct { uint32_t pitch_at, head_at; uint16_t keep_p, keep_h; } aim_t[AIM_MAX]; static int aim_n, aim_hold;

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
    hunt = envi("NDVR_HUNT", 0); capture = envi("NDVR_PROBE_CAPTURE", 0); mousetest = envi("NDVR_MOUSETEST", 0);
    if (!capture) idle_frame = envi("NDVR_PROBE_IDLE", 0);
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

typedef struct { uint32_t addr, base; float pos[3], dist; } Cand;

/* ---- NDVR_HUNT=1: find the player's master heading and pitch (values the game keeps, not copies it rebuilds every
 * frame), then aim-test them. One step per fire press:
 *   - heading: turn the next untested facing copy 10 degrees and leave it; 10 frames later it is the master if the value
 *     stuck AND the view turned. Copies that are rebuilt (or stick without turning the view) are ruled out and put back.
 *   - pitch: every press collects the places that hold the current pitch (u16 like the camera's, or float radians of
 *     either sign) and keeps only those that held it at every earlier press too. Look up or down a little between
 *     shots so that set shrinks. Once it is small, its entries are tested the same way (the view tilts on a hit).
 *   - with both found (or the pitch list exhausted): every later press turns the masters by NDVR_AIMTEST_HEAD /
 *     NDVR_AIMTEST_PITCH degrees (default 30 / 0) for NDVR_AIMTEST_FRAMES frames (default 2), so the shot shows
 *     whether the game aims with them. */
static int presses;
static uint32_t mh_at; static int mp_kind = -1; static uint32_t mp_at;
static uint32_t ruled[512]; static int nruled;
typedef struct { uint32_t a; int kind; } PCand;                  /* kind 0: u16 like the camera; 1: float radians; 2: float -radians */
static PCand pcs[4096]; static int npc = -1;
static struct { int wait, is_pitch, kind; uint32_t at, wrote, orig; uint16_t cam_before; } T;
static struct { int hold; uint32_t h_orig, p_orig; } HA;
#define TAU 6.283185307179586

static int is_ruled(uint32_t a) { for (int i = 0; i < nruled; i++) if (ruled[i] == a) return 1; return 0; }
static void rule_out(uint32_t a) { if (nruled < 512) ruled[nruled++] = a; }
static int s16(int v) { v &= 0xFFFF; return v > 32767 ? v - 65536 : v; }
static uint32_t rd_val(int kind, uint32_t a) { return kind ? RD32(a) : RD16(a); }
static void wr_val(int kind, uint32_t a, uint32_t v) { if (kind) WR32(a, v); else WR16(a, (uint16_t)v); }
static uint32_t turned(int kind, uint32_t v, double deg) {      /* v turned by deg, in its own representation */
    if (!kind) return (uint16_t)(v + (int)floor(deg * 65536.0 / 360.0 + 0.5));
    float f = bf(v); f += (float)((kind == 1 ? 1 : -1) * deg * TAU / 360.0); uint32_t u; memcpy(&u, &f, 4); return u;
}
static int pitch_match(int kind, uint32_t a, int camp, double pt) {
    if (!kind) return RD16(a) == camp;
    float f = rdf(a); return isfinite(f) && fabs(f - (kind == 1 ? pt : -pt)) < 2e-4;
}
static void pitch_collect(uint32_t lo, uint32_t hi, int camp, double pt) {
    for (uint32_t a = lo; a + 4 <= hi && npc < 4096; a += 2) {
        if (!g_valid(a, 4)) break;
        if (a >= CAM_COPY && a < CAM_COPY + 32) continue;
        for (int k = 0; k < 3 && npc < 4096; k++) { if (k && (a & 3)) continue; if (pitch_match(k, a, camp, pt)) { pcs[npc].a = a; pcs[npc].kind = k; npc++; } }
    }
}

static void hunt_frame(void) {
    if (HA.hold > 0 && --HA.hold == 0) {
        if (mh_at) WR16(mh_at, (uint16_t)HA.h_orig);
        if (mp_kind >= 0) wr_val(mp_kind, mp_at, HA.p_orig);
        P("HUNT AIM: masters put back");
    }
    if (T.wait > 0 && --T.wait == 0) {
        uint32_t now = rd_val(T.kind, T.at);
        int kept = T.kind ? now == T.wrote : abs(s16((int)now - (int)T.wrote)) < 64;
        double moved = s16(RD16(CAM_COPY + (T.is_pitch ? 18 : 20)) - T.cam_before) * 360.0 / 65536.0;
        int view = moved > 6.0 && moved < 14.0;                  /* we turn +10; recoil kicks the pitch the other way (-6 in tests) */
        P("HUNT %s test of %08x (%s): value %s, view %s %+.1f degrees -> %s", T.is_pitch ? "pitch" : "heading", T.at,
          T.kind == 0 ? "u16" : T.kind == 1 ? "float rad" : "float -rad", kept ? "KEPT" : "rebuilt by the game", T.is_pitch ? "pitch moved" : "heading moved", moved,
          kept && view ? "MASTER" : "ruled out");
        if (kept && view) { if (T.is_pitch) { mp_at = T.at; mp_kind = T.kind; } else mh_at = T.at; }
        else { rule_out(T.at); if (kept) wr_val(T.kind, T.at, T.orig); }
        if (mh_at && T.is_pitch == 0 && kept && view) P("HUNT: heading master found at %08x. Now look up or down a little between shots for the pitch.", mh_at);
        if (mp_kind >= 0 && T.is_pitch && kept && view) P("HUNT: pitch master found at %08x. Every next shot is an aim test: say where it lands.", mp_at);
    }
}

static void hunt_press(const Cand *cand, const int *layout, int nc, int nglob, const uint32_t *ar, int nar, uint32_t asz) {
    presses++;
    int camp = RD16(CAM_COPY + 18); double pt = s16(camp) * TAU / 65536.0;
    /* the pitch list */
    if (mp_kind < 0) {
        if (abs(s16(camp)) < 300) P("HUNT pitch: you are looking almost level; look a little up or down for the pitch search");
        else if (npc < 0) {
            npc = 0; pitch_collect(GLOBALS_LO, GLOBALS_HI, camp, pt);
            for (int i = 0; i < nar; i++) pitch_collect(ar[i], ar[i] + asz, camp, pt);
            P("HUNT pitch: %d places hold the current pitch%s", npc, npc >= 4096 ? " (list full, some missed)" : "");
        } else {
            int k = 0; for (int i = 0; i < npc; i++) if (pitch_match(pcs[i].kind, pcs[i].a, camp, pt) && !is_ruled(pcs[i].a)) pcs[k++] = pcs[i];
            npc = k; P("HUNT pitch: %d places held the pitch at every shot so far", npc);
            for (int i = 0; i < npc && i < 12; i++) P("  PITCH @%08x %s", pcs[i].a, pcs[i].kind == 0 ? "u16" : pcs[i].kind == 1 ? "float rad" : "float -rad");
        }
    }
    if (T.wait > 0 || HA.hold > 0) return;
    if (!mh_at) {                                   /* heading: heap heading-only copies first, then the rest */
        int j = -1;
        for (int pass = 0; pass < 2 && j < 0; pass++)
            for (int i = 0; i < nc && j < 0; i++) {
                if (!layout[i]) continue;
                uint32_t ha = cand[i].addr + (layout[i] == 1 ? 20u : 16u), pa = ha - 2;
                int pref = i >= nglob && RD16(pa) == 0;
                if ((pass == 0) != pref || is_ruled(ha)) continue;
                j = i;
            }
        if (j < 0) { P("HUNT heading: every facing copy has been ruled out; the heading lives somewhere else"); return; }
        T.is_pitch = 0; T.kind = 0; T.at = cand[j].addr + (layout[j] == 1 ? 20u : 16u); T.orig = RD16(T.at);
        T.wrote = turned(0, T.orig, 10.0); T.cam_before = RD16(CAM_COPY + 20); wr_val(0, T.at, T.wrote); T.wait = 10;
        P("HUNT heading: CAND %d (%08x) turned 10 degrees; keep the mouse still for a moment", j, T.at);
        return;
    }
    if (mp_kind < 0 && npc > 0 && npc <= 24 && presses >= 3) {
        int j = -1;                                 /* heap first: the global ones and the camera object are rebuilt copies */
        for (int pass = 0; pass < 2 && j < 0; pass++)
            for (int i = 0; i < npc && j < 0; i++) {
                int heap = pcs[i].a >= 0x01000000u && !(pcs[i].a >= RD32(PLAYER_CAM) && pcs[i].a < RD32(PLAYER_CAM) + 0x400);
                if ((pass == 0) == heap && !is_ruled(pcs[i].a)) j = i;
            }
        if (j >= 0) {
            T.is_pitch = 1; T.kind = pcs[j].kind; T.at = pcs[j].a; T.orig = rd_val(T.kind, T.at);
            T.wrote = turned(T.kind, T.orig, 10.0); T.cam_before = RD16(CAM_COPY + 18); wr_val(T.kind, T.at, T.wrote); T.wait = 10;
            P("HUNT pitch: %08x tilted 10 degrees; keep the mouse still for a moment", T.at);
            return;
        }
    }
    if (mp_kind < 0 && npc != 0 && npc > 24) { P("HUNT pitch: still %d places; look up or down a little more between shots", npc); }
    double dh = aim_head != 0 ? aim_head : 30.0;
    HA.h_orig = RD16(mh_at); WR16(mh_at, (uint16_t)turned(0, HA.h_orig, dh));
    if (mp_kind >= 0 && aim_pitch != 0) { HA.p_orig = rd_val(mp_kind, mp_at); wr_val(mp_kind, mp_at, turned(mp_kind, HA.p_orig, aim_pitch)); }
    HA.hold = aim_frames > 1 ? aim_frames : 2;
    P("HUNT AIM: master heading turned %+.1f%s for %d frames: where does this shot land?", dh, mp_kind >= 0 && aim_pitch != 0 ? " (and pitch)" : "", HA.hold);
}

/* ---- NDVR_MOUSETEST=n: every 300 frames, alternately, move the mouse n counts sideways or n counts up/down for one
 * frame (as if the player did) and log how the camera's heading and pitch respond over the next 30 frames: the
 * degrees per count, the delay and any smoothing. That is what aiming with a controller through the game's own mouse
 * look needs. Stand still and do not touch the mouse during the test. */
static int mt_phase = -1, mt_axis, mt_sign = 1, mt_n; static uint16_t mt_h0, mt_p0; static char mt_line[600]; static int mt_len;
static void mouse_test_frame(void) {
    if (mt_phase < 0) {
        if (frame_no < 600 || frame_no % 150) return;
        mt_axis = (frame_no / 150) & 1; if (!mt_axis) mt_sign = -mt_sign;
        mt_h0 = RD16(CAM_COPY + 20); mt_p0 = RD16(CAM_COPY + 18);
        if (!vr_input_mouse_motion(mt_axis ? 0 : mt_sign * mousetest, mt_axis ? mt_sign * mousetest : 0)) return;   /* menus: not captured */
        mt_phase = 0; mt_n++;
        mt_len = snprintf(mt_line, sizeof mt_line, "MOUSE test %d: %s %+d counts; frames after (heading, pitch change in degrees):", mt_n, mt_axis ? "dy" : "dx", mt_sign * mousetest);
        return;
    }
    mt_phase++;
    double dh = s16(RD16(CAM_COPY + 20) - mt_h0) * 360.0 / 65536.0, dp = s16(RD16(CAM_COPY + 18) - mt_p0) * 360.0 / 65536.0;
    if (mt_phase <= 6 || mt_phase == 10 || mt_phase == 20 || mt_phase == 30)
        mt_len += snprintf(mt_line + mt_len, sizeof mt_line - (size_t)mt_len, " %d:(%+.2f,%+.2f)", mt_phase, dh, dp);
    if (mt_phase == 30) { P("%s", mt_line); mt_phase = -1; }
}

void vr_probe_present(void) {
    if (!vr_probe_on) return;
    if (cap_left > 0) { P("---- end of frame %u: %u draws (%u without depth test, %u pre-transformed, %u in overlay), %u clears, %d depth clears", frame_no, n_draw, n_noz, n_rhw, n_ovl, n_clear, zclears); cap_left--; }
    else if (frame_no % 300 == 0) P("frame %u: %u draws (%u without depth test, %u pre-transformed, %u in overlay), %u clears, %d depth clears", frame_no, n_draw, n_noz, n_rhw, n_ovl, n_clear, zclears);
    if (aim_hold > 0 && --aim_hold == 0) { for (int i = 0; i < aim_n; i++) { WR16(aim_t[i].pitch_at, aim_t[i].keep_p); WR16(aim_t[i].head_at, aim_t[i].keep_h); } P("AIMTEST: %d facing copies put back", aim_n); aim_n = 0; }
    if (hunt) hunt_frame();
    if (mousetest) mouse_test_frame();
    frame_no++; seq = 0; n_draw = n_noz = n_rhw = n_ovl = n_clear = 0; zclears = 0; overlay = 0;
    if (idle_frame > 0 && frame_no == (unsigned)idle_frame) { cap_left = 2; P("---- idle capture of frames %u and %u ----", frame_no, frame_no + 1); }
}

/* ---- the fire log ---- */
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
    if (capture && fires < 6) { cap_left = 3; seq = 0; zclears = 0; }
    fires++;
    float cam[3] = { rdf(CAM_COPY), rdf(CAM_COPY + 4), rdf(CAM_COPY + 8) };
    P("FIRE render camera copy: pos (%.3f, %.3f, %.3f) cell %08x bank %u pitch %u heading %u (65536 = 360 deg)", (double)cam[0], (double)cam[1], (double)cam[2], RD32(CAM_COPY + 12), RD16(CAM_COPY + 16), RD16(CAM_COPY + 18), RD16(CAM_COPY + 20));
    uint32_t obj = RD32(PLAYER_CAM);
    P("FIRE *0xa3acb8 = %08x (%s)", obj, g_valid(obj, 64) ? "looks like a pointer" : "not a pointer: an object id?");
    static Cand cand[48]; int nc = 0;
    if (g_valid(obj, 64)) {
        nc = scan(obj, 0x400, cam, cand, 48, nc);
        for (uint32_t o = 0; o < 0x100 && nc < 48; o += 4) { uint32_t w = RD32(obj + o); if (w >= 0x10000u && w != obj && g_valid(w, 0x200)) nc = scan(w, 0x200, cam, cand, 48, nc); }
    }
    int nobj = nc;
    nc = scan(GLOBALS_LO, GLOBALS_HI - GLOBALS_LO, cam, cand, 48, nc);
    int nglob = nc;
    uint32_t ar[64], asz = 0; int nar = vr_heap_arenas(ar, 64, &asz);
    for (int i = 0; i < nar && nc < 48; i++) nc = scan(ar[i], asz, cam, cand, 48, nc);
    P("FIRE %d place(s) holding a position within 8 units of the camera (%d in the camera object, %d in global data, %d on the heap: %d arenas of %u MB):", nc, nobj, nglob - nobj, nc - nglob, nar, asz >> 20);
    int camp = RD16(CAM_COPY + 18), camh = RD16(CAM_COPY + 20), nmatch = 0; int layout[48];
    for (int i = 0; i < nc; i++) {
        uint32_t a = cand[i].addr;
        int A = ang_close(RD16(a + 18), camp) && ang_close(RD16(a + 20), camh);       /* bank, pitch, heading at +16, +18, +20 (after a cell word) */
        int B = ang_close(RD16(a + 14), camp) && ang_close(RD16(a + 16), camh);       /* bank, pitch, heading at +12, +14, +16 (straight after the position) */
        int Ah = !A && ang_close(RD16(a + 20), camh), Bh = !B && ang_close(RD16(a + 16), camh);   /* heading only: a body facing has no pitch */
        layout[i] = A || Ah ? 1 : B || Bh ? 2 : 0; if (layout[i]) nmatch++;
        P("  CAND %d @%08x (%s) pos (%.3f, %.3f, %.3f) off %.2f  facing layout %s  raw %08x %08x %08x %08x %08x %08x", i, a, i < nobj ? "object" : i < nglob ? "global" : "heap",
          (double)cand[i].pos[0], (double)cand[i].pos[1], (double)cand[i].pos[2], (double)cand[i].dist, A ? "A (+18 pitch, +20 heading) = same as camera" : B ? "B (+14 pitch, +16 heading) = same as camera" : Ah ? "A, heading only (+20)" : Bh ? "B, heading only (+16)" : "none/different",
          RD32(a), RD32(a + 4), RD32(a + 8), RD32(a + 12), RD32(a + 16), RD32(a + 20));
    }
    P("FIRE %d of them hold the same facing (or heading) as the render camera", nmatch);
    {   /* the facing as a direction vector or matrix row: floats (cos h cos p, sin h cos p, +-sin p), heading 0 = +x */
        double h = camh * (2.0 * 3.14159265358979 / 65536.0), pt = (int16_t)camp * (2.0 * 3.14159265358979 / 65536.0);
        float f[3] = { (float)(cos(h) * cos(pt)), (float)(sin(h) * cos(pt)), (float)sin(pt) };
        int hits = 0;
        for (int pass = 0; pass < 2 + nar; pass++) {
            uint32_t lo = pass >= 2 ? ar[pass - 2] : pass ? GLOBALS_LO : obj, hi = pass >= 2 ? ar[pass - 2] + asz : pass ? GLOBALS_HI : obj + 0x400;
            if (!pass && !g_valid(obj, 0x400)) continue;
            for (uint32_t a = lo; a + 12 <= hi && hits < 24; a += 4) {
                if (!g_valid(a, 12)) break;
                float v[3]; memcpy(v, GP(a), 12);
                if (!(fabsf(v[0] - f[0]) < 0.01f && fabsf(v[1] - f[1]) < 0.01f && (fabsf(v[2] - f[2]) < 0.01f || fabsf(v[2] + f[2]) < 0.01f))) continue;
                if (fabsf(f[0]) + fabsf(f[1]) < 0.05f) continue;
                hits++;
                P("  DIR @%08x (%s) (%.4f, %.4f, %.4f)  next %.4f %.4f %.4f %.4f %.4f %.4f", a, pass >= 2 ? "heap" : pass ? "global" : "object", (double)v[0], (double)v[1], (double)v[2],
                  (double)rdf(a + 12), (double)rdf(a + 16), (double)rdf(a + 20), (double)rdf(a + 24), (double)rdf(a + 28), (double)rdf(a + 32));
            }
        }
        P("FIRE %d place(s) hold the camera's forward direction as floats (%.4f, %.4f, +-%.4f)", hits, (double)f[0], (double)f[1], (double)f[2]);
    }
    if (hunt) hunt_press(cand, layout, nc, nglob, ar, nar, asz);
    if (!hunt && (aim_head != 0 || aim_pitch != 0) && aim_hold == 0) {                       /* NDVR_AIMTEST_CAND=-1 (default): every copy that matches the camera */
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
