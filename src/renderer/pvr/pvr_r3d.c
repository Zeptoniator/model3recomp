/* The Real3D on the PowerVR2.
 *
 * real3d.c walks the scene and hands every polygon over already culled,
 * cut at the near plane and projected to the board's 496x384 screen, with
 * w = 1/z and a flat-shaded colour (see m3_r3d_sink). Here they become PVR
 * triangle strips in three lists: opaque, punch-through (texel alpha cuts
 * them out) and translucent. KOS takes each list once per scene and in
 * order, so the opaque ones stream straight out and the other two are kept
 * until the opaque list is closed.
 *
 * Textures. The board's sheet is 2048x2048 16-bit texels; Scud Race fills
 * three quarters of it during the boot, through the FIFO, from data it has
 * decompressed (the payloads are not in any ROM), so there is nowhere to
 * read them back from later. Kept whole it is 8 MB: more than the PVR has.
 * So the sheet lives in PVR memory at half size each way (2 MB, every
 * other texel, point sampled -- exact whatever the texel format), and the
 * textures the polygons ask for, a rectangle of it read in one of twelve
 * formats, are converted on first use into PVR textures kept in a cache.
 * Uploads are rare (the boot, a change of course); any upload empties the
 * cache.
 *
 * The tilemap layers. tilegen.c draws each priority pass (behind the 3D,
 * in front of it) into a 512x512 ARGB1555 texture, alpha clear where no
 * layer drew; here they are two screen-sized quads, the one behind at the
 * far end of the depth range in the opaque list, the one in front at the
 * near end in the punch-through list. They are drawn after pvr_wait_ready(),
 * so never into a texture the PVR is still reading.
 *
 * Not yet: per-viewport depth ranges (one depth buffer for all
 * viewports). */
#include "pvr_r3d.h"
#include "model3recomp/real3d.h"
#include "model3recomp/tilegen.h"
#include "model3recomp/bus.h"

#include <kos.h>
#include <dc/pvr/pvr_regs.h>
#include <string.h>

#define SX (640.0f / 496.0f)
#define SY (480.0f / 384.0f)
#define PT_MAX 1024                    /* punch-through polygons kept a frame */
#define TR_MAX 512                     /* translucent polygons kept a frame */
#define SHEET_W 1024u                  /* the half-size sheet */
#define SHEET_H 1024u
#define TEX_SLOTS 256
#define TEX_BUDGET (1536u * 1024u)     /* PVR memory for converted textures */
#define LAYER_W 512u                   /* a 496x384 tilemap pass */

typedef struct {
    uint32_t key;                      /* 0 = free */
    pvr_ptr_t ptr;
    uint16_t w, h;                     /* PVR texture size */
    int pvrfmt;
    uint32_t used;                     /* frame last drawn */
    size_t bytes;
} tex_t;

typedef struct {
    m3_r3d_vertex v[4];
    int n;
    uint32_t argb;
    int16_t tex;                       /* slot, -1 untextured */
    uint8_t flip;
} kept_t;

static pvr_poly_hdr_t g_hdr_col[3];    /* untextured, per list */
static kept_t *g_pt, *g_tr;
static unsigned g_npt, g_ntr;
static unsigned g_op, g_last_op, g_last_pt, g_last_tr;
static int g_open;

static uint16_t *g_sheet;
static tex_t g_tex[TEX_SLOTS];
static size_t g_tex_bytes;
static uint32_t g_frame;
static unsigned g_tex_made, g_tex_failed;
static int g_cur_tex = -2, g_cur_flip = -1;   /* last header sent */
static uint16_t *g_layer[2];                   /* behind, in front of the 3D */
static pvr_poly_hdr_t g_layer_hdr[2];
static unsigned g_layer_px[2];
static unsigned g_layer_redraws;

/* ---- texture cache ----------------------------------------------------- */
static unsigned log2u(unsigned v) { unsigned n = 0; while (v > 1u) { v >>= 1; n++; } return n; }

static uint32_t tex_key(const m3_r3d_texture *t)
{
    return 1u | ((t->x >> 5) << 1) | ((t->y >> 5) << 7) | (t->page << 12)
         | ((log2u(t->w) & 15u) << 13) | ((log2u(t->h) & 15u) << 17)
         | ((t->fmt & 15u) << 21);
}

static void tex_drop(tex_t *e)
{
    pvr_mem_free(e->ptr);
    g_tex_bytes -= e->bytes;
    memset(e, 0, sizeof *e);
}

/* The least recently drawn texture not drawn this frame, or NULL. */
static tex_t *tex_victim(void)
{
    tex_t *v = NULL;
    unsigned i;
    for (i = 0; i < TEX_SLOTS; i++)
        if (g_tex[i].key && g_tex[i].used != g_frame
            && (!v || g_tex[i].used < v->used))
            v = &g_tex[i];
    return v;
}

static void sheet_changed(void)
{
    unsigned i;
    for (i = 0; i < TEX_SLOTS; i++)
        if (g_tex[i].key) tex_drop(&g_tex[i]);
}

/* Formats with graded alpha go to ARGB4444, the rest to ARGB1555 (their
 * alpha is all or nothing, and five bits of grey keep more of them). */
static int fmt_4444(unsigned fmt)
{
    return (fmt >= 1u && fmt <= 4u) || fmt == 7u;
}

static int tex_get(const m3_r3d_texture *t)
{
    uint32_t key = tex_key(t);
    unsigned i, free_slot = TEX_SLOTS, w, h, x0, y0, row, col;
    tex_t *e;
    size_t bytes;
    int four = fmt_4444(t->fmt);
    uint16_t *dst;

    for (i = 0; i < TEX_SLOTS; i++) {
        if (g_tex[i].key == key) { g_tex[i].used = g_frame; return (int)i; }
        if (!g_tex[i].key && free_slot == TEX_SLOTS) free_slot = i;
    }
    if (!g_sheet || t->w > 1024u || t->h > 1024u) return -1;
    w = t->w >> 1; h = t->h >> 1;
    if (w < 8u) w = 8u;
    if (h < 8u) h = 8u;
    bytes = (size_t)w * h * 2u;
    for (;;) {
        tex_t *v;
        if (free_slot < TEX_SLOTS && g_tex_bytes + bytes <= TEX_BUDGET) {
            pvr_ptr_t p = pvr_mem_malloc(bytes);
            if (p) {
                e = &g_tex[free_slot];
                e->ptr = p;
                break;
            }
        }
        if (!(v = tex_victim())) { g_tex_failed++; return -1; }
        if (free_slot == TEX_SLOTS) free_slot = (unsigned)(v - g_tex);
        tex_drop(v);
    }
    e->key = key; e->w = (uint16_t)w; e->h = (uint16_t)h; e->bytes = bytes;
    e->pvrfmt = (four ? PVR_TXRFMT_ARGB4444 : PVR_TXRFMT_ARGB1555)
              | PVR_TXRFMT_NONTWIDDLED;
    e->used = g_frame;
    g_tex_bytes += bytes;
    g_tex_made++;

    /* Rows wrap inside the page, as the software sampler wraps them. */
    x0 = t->x >> 1;
    y0 = t->y >> 1;
    dst = (uint16_t *)e->ptr;
    for (row = 0; row < h; row++) {
        const uint16_t *src = g_sheet + (size_t)(((y0 + row) & 511u)
                                                 + t->page * 512u) * SHEET_W;
        for (col = 0; col < w; col++) {
            uint32_t c = real3d_texel_argb(t->fmt, src[(x0 + col) & (SHEET_W - 1u)]);
            uint16_t o;
            if (four)
                o = (uint16_t)(((c >> 16) & 0xF000u) | ((c >> 12) & 0x0F00u)
                             | ((c >> 8) & 0x00F0u) | ((c >> 4) & 0x000Fu));
            else
                o = (uint16_t)(((c >> 31) << 15) | ((c >> 9) & 0x7C00u)
                             | ((c >> 6) & 0x03E0u) | ((c >> 3) & 0x001Fu));
            dst[(size_t)row * w + col] = o;
        }
    }
    return (int)(e - g_tex);
}

/* ---- submission --------------------------------------------------------- */
static void send_header(pvr_list_t list, int slot, int flip)
{
    static const int lists[3] = { PVR_LIST_OP_POLY, PVR_LIST_PT_POLY, PVR_LIST_TR_POLY };
    int li = list == PVR_LIST_OP_POLY ? 0 : list == PVR_LIST_PT_POLY ? 1 : 2;
    if (slot == g_cur_tex && flip == g_cur_flip) return;
    g_cur_tex = slot; g_cur_flip = flip;
    if (slot < 0) {
        pvr_prim(&g_hdr_col[li], sizeof g_hdr_col[li]);
    } else {
        pvr_poly_cxt_t cx;
        pvr_poly_hdr_t hdr;
        const tex_t *e = &g_tex[slot];
        pvr_poly_cxt_txr(&cx, lists[li], e->pvrfmt, e->w, e->h, e->ptr,
                         PVR_FILTER_BILINEAR);
        cx.gen.shading = PVR_SHADE_FLAT;
        cx.gen.culling = PVR_CULLING_NONE;   /* real3d.c culled already */
        cx.txr.uv_flip = flip;
        pvr_poly_compile(&hdr, &cx);
        pvr_prim(&hdr, sizeof hdr);
    }
}

static void strip(const m3_r3d_vertex *v, int n, uint32_t argb,
                  float su, float sv)
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
        pv.u = s->u * su;
        pv.v = s->v * sv;
        pv.argb = argb;
        pv.oargb = 0;
        pvr_prim(&pv, sizeof pv);
    }
}

/* Texture coordinates arrive in the board's texels; the PVR wants them
 * over the texture's size, which is the same fraction at any scale. */
static void tex_scale(int slot, const m3_r3d_texture *t, float *su, float *sv)
{
    (void)slot;
    *su = t ? 1.0f / (float)t->w : 0.0f;
    *sv = t ? 1.0f / (float)t->h : 0.0f;
}

/* One tilemap pass over the whole screen, at depth z (1/z: small is far). */
static void layer_quad(int which, float z)
{
    static const float xs[4] = { 0, 640, 0, 640 }, ys[4] = { 0, 0, 480, 480 };
    const float um = 496.0f / LAYER_W, vm = 384.0f / LAYER_W;
    int k;
    pvr_prim(&g_layer_hdr[which], sizeof g_layer_hdr[which]);
    for (k = 0; k < 4; k++) {
        pvr_vertex_t pv;
        pv.flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        pv.x = xs[k]; pv.y = ys[k]; pv.z = z;
        pv.u = (k & 1) ? um : 0.0f;
        pv.v = (k & 2) ? vm : 0.0f;
        pv.argb = 0xFFFFFFFFu; pv.oargb = 0;
        pvr_prim(&pv, sizeof pv);
    }
    g_cur_tex = -2;
}

static void begin_frame(void)
{
    static uint32_t drawn_gen;
    static int ever;
    uint32_t gen = bus_tilegen_generation();
    pvr_wait_ready();
    /* Only when the guest has written tilegen VRAM or a register since:
     * through most of attract the 2D does not move, and drawing it costs
     * more than the rest of the field. */
    if (!ever || gen != drawn_gen) {
        g_layer_px[0] = tilegen_render_pass_1555(0, g_layer[0], LAYER_W);
        g_layer_px[1] = tilegen_render_pass_1555(1, g_layer[1], LAYER_W);
        drawn_gen = gen;
        ever = 1;
        g_layer_redraws++;
    }
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    g_cur_tex = -2; g_cur_flip = -1;
    if (g_layer_px[0]) layer_quad(0, 1e-6f);
    g_npt = g_ntr = g_op = 0;
    g_frame++;
    g_open = 1;
}

static void keep(kept_t *k, const m3_r3d_vertex *v, int n, uint32_t argb,
                 int slot, int flip, const m3_r3d_texture *t)
{
    float su, sv;
    int i;
    tex_scale(slot, t, &su, &sv);
    for (i = 0; i < n; i++) {
        k->v[i] = v[i];
        k->v[i].u = v[i].u * su;     /* stored already normalised */
        k->v[i].v = v[i].v * sv;
    }
    k->n = n;
    k->argb = argb;
    k->tex = (int16_t)slot;
    k->flip = (uint8_t)flip;
}

static void polygon(const m3_r3d_vertex *v, int n, uint32_t argb,
                    int translucent, const m3_r3d_texture *t)
{
    int slot = -1, flip = 0;
    if (!g_open || n < 3 || n > 4) return;
    if (t) {
        slot = tex_get(t);
        if (slot >= 0) {
            flip = (t->mirror_u ? PVR_UVFLIP_U : 0) | (t->mirror_v ? PVR_UVFLIP_V : 0);
        }
    }
    if (translucent) {
        if (g_ntr < TR_MAX) keep(&g_tr[g_ntr++], v, n, argb, slot, flip, slot >= 0 ? t : NULL);
    } else if (slot >= 0 && (t->alpha_test || t->tex_alpha)) {
        if (g_npt < PT_MAX) keep(&g_pt[g_npt++], v, n, argb | 0xFF000000u, slot, flip, t);
    } else {
        float su, sv;
        tex_scale(slot, slot >= 0 ? t : NULL, &su, &sv);
        send_header(PVR_LIST_OP_POLY, slot, flip);
        strip(v, n, argb | 0xFF000000u, su, sv);
        g_op++;
    }
}

static void flush_kept(pvr_list_t list, const kept_t *k, unsigned n)
{
    unsigned i;
    pvr_list_begin(list);
    g_cur_tex = -2; g_cur_flip = -1;
    for (i = 0; i < n; i++) {
        send_header(list, k[i].tex, k[i].flip);
        strip(k[i].v, k[i].n, k[i].argb, 1.0f, 1.0f);
    }
    if (list == PVR_LIST_PT_POLY && g_layer_px[1])
        layer_quad(1, 1e6f);
    pvr_list_finish();
}

static void end_frame(void)
{
    if (!g_open) return;
    pvr_list_finish();
    flush_kept(PVR_LIST_PT_POLY, g_pt, g_npt);
    flush_kept(PVR_LIST_TR_POLY, g_tr, g_ntr);
    pvr_scene_finish();
    g_last_op = g_op;
    g_last_pt = g_npt;
    g_last_tr = g_ntr;
    g_open = 0;
}

static const m3_r3d_sink k_sink = { begin_frame, polygon, end_frame, sheet_changed };

unsigned pvr_r3d_last_opaque(void) { return g_last_op; }
unsigned pvr_r3d_last_punch(void) { return g_last_pt; }
unsigned pvr_r3d_last_translucent(void) { return g_last_tr; }
unsigned pvr_r3d_textures_made(void) { return g_tex_made; }
unsigned pvr_r3d_textures_failed(void) { return g_tex_failed; }
unsigned pvr_r3d_texture_kb(void) { return (unsigned)(g_tex_bytes >> 10); }
unsigned pvr_r3d_layer_redraws(void) { return g_layer_redraws; }

int pvr_r3d_install(void)
{
    pvr_init_params_t params = {
        /* opaque, opaque modifier, translucent, translucent modifier, punch-through */
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16 },
        768 * 1024,      /* vertex buffer: a frame's strips and headers */
        0, 0, 0,
        2                /* overflow OPBs: no flicker along tile edges */
    };
    pvr_poly_cxt_t cx;
    static const pvr_list_t lists[3] = { PVR_LIST_OP_POLY, PVR_LIST_PT_POLY, PVR_LIST_TR_POLY };
    unsigned i;
    uint32_t *p;

    if (pvr_init(&params) < 0) return 0;
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    /* Punch-through keeps texels with alpha at or above this: half. */
    PVR_SET(0x011C, 0x80);

    for (i = 0; i < 3; i++) {
        pvr_poly_cxt_col(&cx, lists[i]);
        cx.gen.shading = PVR_SHADE_FLAT;
        cx.gen.culling = PVR_CULLING_NONE;
        pvr_poly_compile(&g_hdr_col[i], &cx);
    }

    /* The half-size sheet, cleared with word writes (PVR memory takes no
     * byte writes). */
    g_sheet = (uint16_t *)pvr_mem_malloc(SHEET_W * SHEET_H * 2u);
    if (!g_sheet) return 0;
    for (p = (uint32_t *)g_sheet, i = 0; i < SHEET_W * SHEET_H / 2u; i++) p[i] = 0;
    real3d_set_sheet(g_sheet, 1);

    for (i = 0; i < 2; i++) {
        unsigned k;
        g_layer[i] = (uint16_t *)pvr_mem_malloc(LAYER_W * LAYER_W * 2u);
        if (!g_layer[i]) return 0;
        for (p = (uint32_t *)g_layer[i], k = 0; k < LAYER_W * LAYER_W / 2u; k++) p[k] = 0;
        pvr_poly_cxt_txr(&cx, i ? PVR_LIST_PT_POLY : PVR_LIST_OP_POLY,
                         PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                         LAYER_W, LAYER_W, g_layer[i], PVR_FILTER_BILINEAR);
        cx.gen.culling = PVR_CULLING_NONE;
        /* In front of everything, whatever the 3D wrote to the depth
         * buffer; behind everything for the other one. */
        cx.depth.comparison = i ? PVR_DEPTHCMP_ALWAYS : PVR_DEPTHCMP_GREATER;
        pvr_poly_compile(&g_layer_hdr[i], &cx);
    }

    g_pt = malloc(sizeof(kept_t) * PT_MAX);
    g_tr = malloc(sizeof(kept_t) * TR_MAX);
    if (!g_pt || !g_tr) return 0;
    printf("[pvr] sheet 2048 KB and tilemap 1024 KB in PVR memory, %u KB left for textures"
           " (budget %u KB)\n", (unsigned)(pvr_mem_available() >> 10),
           TEX_BUDGET >> 10);
    real3d_set_sink(&k_sink);
    return 1;
}
