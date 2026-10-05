/* The Real3D on the PowerVR2.
 *
 * real3d.c walks the scene and hands every polygon over already culled,
 * cut at the near plane and projected to the board's 496x384 screen, with
 * w = 1/z and a flat-shaded colour (see m3_r3d_sink). Here they become PVR
 * triangle strips: opaque ones straight into the opaque list, translucent
 * ones kept until the opaque list is closed, because KOS takes each list
 * once per scene and in order.
 *
 * Not yet: textures (the 8 MB sheet does not fit; it becomes a texture
 * cache), per-viewport depth ranges (one depth buffer for all viewports),
 * the tilemap layers. */
#include "pvr_r3d.h"
#include "model3recomp/real3d.h"

#include <kos.h>
#include <string.h>

#define SX (640.0f / 496.0f)
#define SY (480.0f / 384.0f)
#define TR_MAX 1024                    /* translucent polygons kept a frame */

typedef struct { m3_r3d_vertex v[4]; int n; uint32_t argb; } kept_t;

static pvr_poly_hdr_t g_hdr_op, g_hdr_tr;
static kept_t *g_kept;
static unsigned g_nkept, g_op, g_tr, g_last_op, g_last_tr;
static int g_open;

static void strip(const m3_r3d_vertex *v, int n, uint32_t argb)
{
    /* A convex polygon of 3 or 4 vertices as a strip: 0 1 2, or 0 1 3 2. */
    static const int order3[3] = { 0, 1, 2 }, order4[4] = { 0, 1, 3, 2 };
    const int *o = n == 4 ? order4 : order3;
    int k;
    for (k = 0; k < n; k++) {
        pvr_vertex_t pv;
        const m3_r3d_vertex *s = &v[o[k]];
        pv.flags = k == n - 1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        pv.x = s->x * SX;
        pv.y = s->y * SY;
        pv.z = s->w;
        pv.u = pv.v = 0.0f;
        pv.argb = argb;
        pv.oargb = 0;
        pvr_prim(&pv, sizeof pv);
    }
}

static void begin_frame(void)
{
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&g_hdr_op, sizeof g_hdr_op);
    g_nkept = g_op = g_tr = 0;
    g_open = 1;
}

static void polygon(const m3_r3d_vertex *v, int n, uint32_t argb, int translucent)
{
    if (!g_open || n < 3 || n > 4) return;
    if (!translucent) {
        strip(v, n, argb | 0xFF000000u);
        g_op++;
    } else if (g_nkept < TR_MAX) {
        memcpy(g_kept[g_nkept].v, v, sizeof(m3_r3d_vertex) * (size_t)n);
        g_kept[g_nkept].n = n;
        g_kept[g_nkept].argb = argb;
        g_nkept++;
    }
}

static void end_frame(void)
{
    unsigned i;
    if (!g_open) return;
    pvr_list_finish();
    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(&g_hdr_tr, sizeof g_hdr_tr);
    for (i = 0; i < g_nkept; i++, g_tr++)
        strip(g_kept[i].v, g_kept[i].n, g_kept[i].argb);
    pvr_list_finish();
    pvr_scene_finish();
    g_last_op = g_op;
    g_last_tr = g_tr;
    g_open = 0;
}

static const m3_r3d_sink k_sink = { begin_frame, polygon, end_frame };

unsigned pvr_r3d_last_opaque(void) { return g_last_op; }
unsigned pvr_r3d_last_translucent(void) { return g_last_tr; }

int pvr_r3d_install(void)
{
    pvr_init_params_t params = {
        /* opaque, opaque modifier, translucent, translucent modifier, punch-through */
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0 },
        1024 * 1024,     /* vertex buffer: ~16 000 strip vertices a frame */
        0, 0, 0,
        2                /* overflow OPBs: no flicker along tile edges */
    };
    pvr_poly_cxt_t cx;

    if (pvr_init(&params) < 0) return 0;
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

    pvr_poly_cxt_col(&cx, PVR_LIST_OP_POLY);
    cx.gen.shading = PVR_SHADE_FLAT;
    cx.gen.culling = PVR_CULLING_NONE;     /* real3d.c culled already */
    pvr_poly_compile(&g_hdr_op, &cx);

    pvr_poly_cxt_col(&cx, PVR_LIST_TR_POLY);
    cx.gen.shading = PVR_SHADE_FLAT;
    cx.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&g_hdr_tr, &cx);

    g_kept = malloc(sizeof(kept_t) * TR_MAX);
    if (!g_kept) return 0;
    real3d_set_sink(&k_sink);
    return 1;
}
