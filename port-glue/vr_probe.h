/* NewDarkVR probe: the interface between port/dx/d3d9.c, port/core/fastpaths.c and port-glue/vr_probe.c.
 * tools/install_into_port.py --probe copies this and vr_probe.c into port/core/. */
#ifndef VR_PROBE_H
#define VR_PROBE_H
#include <stdint.h>
#include "rt.h"

/* One Direct3D draw call, as port/dx/d3d9.c sees it (before the vertices are transformed or queued). */
typedef struct {
    uint32_t fvf;                  /* the device's current FVF (0 when a vertex declaration is in use) */
    int rhw;                       /* vertices are pre-transformed (screen space x, y, z, 1/w) */
    int ztest, zwrite, zfunc;      /* depth test enabled, depth write enabled, depth compare function */
    int blend, src_blend, dst_blend, atest;
    int ntex;                      /* texture coordinate sets */
    uint32_t prims, span, stride;  /* primitive count, vertices referenced, vertex size */
    int pos;                       /* byte offset of the position in a vertex */
    uint32_t vaddr;                /* guest address of the first referenced vertex */
    uint32_t tex, tex_w, tex_h;    /* texture 0: guest handle (0 = none) and size */
    int32_t scissor[4];
    uint32_t viewport[6];          /* X, Y, Width, Height, MinZ, MaxZ (float bits) */
    uint32_t prim_type;
} VrProbeDraw;

extern int vr_probe_on;            /* set from NDVR_PROBE=1 before the first frame */
void vr_probe_draw(const VrProbeDraw *d);
void vr_probe_clear(uint32_t count, uint32_t flags, uint32_t color, uint32_t z_bits);
void vr_probe_scene(int begin);    /* BeginScene / EndScene */
void vr_probe_present(void);       /* once per Present */
void vr_probe_event(CPU *c, const char *name, int overlay);   /* a hooked engine function was entered; overlay: -1 none, 1 starts, 0 ends */
int vr_probe_fire(CPU *c);         /* cmd_fire_weapon (0x0043b220) was entered; always returns 0 (the original runs) */
#endif
