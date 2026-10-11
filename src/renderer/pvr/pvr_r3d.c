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
 * The tilemap layers. Each of the four 512x512 maps is an ARGB1555 texture;
 * only changed tiles are redrawn. The PVR applies scroll and stencil while
 * drawing rectangles behind or in front of the 3D. A colour offset uses the
 * older two screen-space textures because it changes every visible pixel.
 * Updates happen after pvr_wait_ready(), never while the PVR reads them.
 *
 * Viewports. The software path draws each viewport on a fresh depth
 * buffer, so a later one (the overpass, the car) covers an earlier one
 * whatever their depths. A PVR scene has one depth buffer, so each
 * viewport's w is scaled by a power of two that puts its whole range just
 * above the one before (begin_viewport): a scale common to a polygon's
 * vertices leaves perspective-correct texturing as it was, and a power of
 * two changes only the exponent. Flycast's depth is
 * log2(1 + 100000 w) / 34 in 24 bits, which saturates above w ~ 2^17 and
 * has few steps left below w ~ 2^-10 (core/rend/gles/gles.cpp), so the
 * stack runs from W_FLOOR = 2^-10 to W_TOP = 2^16. The range real3d.c
 * gives is what the viewport shows on the screen -- 0 to 10 bits each in
 * a race, ~15 for a frame -- and is cut to 2^14 (W_SPAN); should the
 * stack run out, the last viewports share its top.
 *
 * Clipping. polygon() cuts every polygon to its viewport's rectangle, for
 * two reasons. Outside it a polygon's w can be anything (a road's near end
 * runs to w = 4 well off the screen), and clamping such a vertex to the
 * range would bend the depth of what is on the screen. And a polygon cut at
 * the near plane can project to hundreds of thousands of pixels, which the
 * PVR draws wrongly (field 6000 of the race: a dark triangle over the left
 * of the screen from walls entirely off its right). */
#include "pvr_r3d.h"
#include "model3recomp/real3d.h"
#include "model3recomp/tilegen.h"
#include "model3recomp/bus.h"
#include "model3recomp/model3recomp.h"

#include <kos.h>
#include <dc/pvr/pvr_regs.h>
#include <string.h>
#include <math.h>

#define SX (640.0f / 496.0f)
#define SY (480.0f / 384.0f)
#define PT_MAX 1024                    /* punch-through polygons kept a frame */
#define TR_MAX 512                     /* translucent polygons kept a frame */
#define SHEET_W 1024u                  /* the half-size sheet */
#define SHEET_H 1024u
#define TEX_SLOTS 256
#define TEX_BUDGET (1024u * 1024u)     /* PVR memory for converted textures; a race uses ~770 KB */
#define LAYER_W 512u                   /* a 496x384 tilemap pass */

typedef struct {
    uint32_t key;                      /* 0 = free */
    pvr_ptr_t ptr;
    uint16_t w, h;                     /* PVR texture size */
    int pvrfmt;
    uint32_t used;                     /* frame last drawn */
    size_t bytes;
} tex_t;

#define POLY_MAX 8                     /* a quad cut by four rectangle edges */
#define W_FLOOR (1.0f / 1024.0f)       /* the lowest viewport starts here (2^-10) */
#define W_TOP 65536.0f                 /* and the highest ends under 2^16 */
#define W_SPAN 16384.0f                /* a viewport's own range, at most 2^14 */
#define Z_BACK_A 2e-8f                 /* 2D behind: under W_FLOOR, B under A */
#define Z_BACK_B 1e-8f
#define Z_FRONT 1e5f                   /* 2D in front: above W_TOP */

typedef struct {
    m3_r3d_vertex v[POLY_MAX];
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
/* The viewport being drawn: its rectangle, its w clamped to [g_vp_lo,
 * g_vp_hi] and scaled by g_vp_scale; the next one starts above g_vp_next. */
static float g_vp_lo = 1e-30f, g_vp_hi = 1e30f, g_vp_scale = 1.0f;
static float g_vp_next = W_FLOOR;
static float g_vp_x0, g_vp_y0, g_vp_x1 = 496.0f, g_vp_y1 = 384.0f;

static uint16_t *g_sheet;
static tex_t g_tex[TEX_SLOTS];
static size_t g_tex_bytes;
static uint32_t g_frame;
static unsigned g_tex_made, g_tex_failed;
static int g_cur_tex = -2, g_cur_flip = -1;   /* last header sent */
/* The four tilemap layers, each a 512x512 picture of its whole map; the
 * PVR applies the scroll and the stencil (draw_layers). Far headers are for
 * layers behind the 3D, near ones for those in front. */
static uint16_t *g_map[4];
static pvr_poly_hdr_t g_map_far[4], g_map_near[4];
static int g_map_force[4];
/* The screen-space passes (a colour offset, or M3_MAP2D=0) borrow the maps
 * of layers 2 and 0: behind, in front of the 3D. */
static int g_fallback;
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
    /* A convex polygon as a strip zigzagging from both ends:
     * 0 1 n-1 2 n-2 ... (0 1 2; 0 1 3 2 for a quad). */
    int k, lo = 1, hi = n - 1;
    for (k = 0; k < n; k++) {
        pvr_vertex_t pv;
        int i = k == 0 ? 0 : (k & 1) ? lo++ : hi--;
        const m3_r3d_vertex *s = &v[i];
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
    if (which) pvr_prim(&g_map_near[0], sizeof g_map_near[0]);
    else       pvr_prim(&g_map_far[2], sizeof g_map_far[2]);
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

/* One rectangle of a layer's map on screen: screen pixels [x0,x1) x
 * [y0,y1) of the 496x384 display, showing the map from (mx, my), wrapping
 * at 512 (the texture repeats). */
static void map_rect(int x0, int x1, int y0, int y1, int mx, int my, float z)
{
    const float sx = 640.0f / 496.0f, sy = 480.0f / 384.0f;
    float u0 = (float)mx / 512.0f, v0 = (float)my / 512.0f;
    float u1 = u0 + (float)(x1 - x0) / 512.0f, v1 = v0 + (float)(y1 - y0) / 512.0f;
    pvr_vertex_t pv;
    int k;
    for (k = 0; k < 4; k++) {
        pv.flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        pv.x = (float)((k & 1) ? x1 : x0) * sx;
        pv.y = (float)((k & 2) ? y1 : y0) * sy;
        pv.z = z;
        pv.u = (k & 1) ? u1 : u0;
        pv.v = (k & 2) ? v1 : v0;
        pv.argb = 0xFFFFFFFFu; pv.oargb = 0;
        pvr_prim(&pv, sizeof pv);
    }
}

/* The layers of one priority pass, as rectangles of their maps: per pair
 * (B under A), runs of lines whose stencil half and scroll do not change,
 * cut into the 32-pixel columns the stencil gives to each layer of the
 * pair. Most of a race is one run of 384 lines and one rectangle a layer;
 * line scroll is a run a line. */
static void draw_layers(int above)
{
    int pair;
    for (pair = 1; pair >= 0; pair--) {
        int want[2], k, y = 0;
        float z = above ? Z_FRONT : (pair ? Z_BACK_B : Z_BACK_A);
        for (k = 0; k < 2; k++) want[k] = tilegen_layer_drawn(pair * 2 + k, above);
        if (!want[0] && !want[1]) continue;
        while (y < 384) {
            uint32_t sw = tilegen_stencil(y);
            unsigned half = pair ? (sw & 0xFFFFu) : (sw >> 16);
            int lx[2] = { 0, 0 }, ly[2] = { 0, 0 }, y1 = y + 1, c;
            for (k = 0; k < 2; k++)
                if (want[k]) tilegen_line_scroll(pair * 2 + k, y, &lx[k], &ly[k]);
            for (; y1 < 384; y1++) {
                uint32_t sw1 = tilegen_stencil(y1);
                int same = (pair ? (sw1 & 0xFFFFu) : (sw1 >> 16)) == half;
                for (k = 0; same && k < 2; k++)
                    if (want[k]) {
                        int x2, y2;
                        tilegen_line_scroll(pair * 2 + k, y1, &x2, &y2);
                        same = x2 == lx[k] && y2 == ((ly[k] + (y1 - y)) & 511);
                    }
                if (!same) break;
            }
            for (c = 0; c < 16;) {
                int kk = ((half >> (15 - c)) & 1u) ? 0 : 1, c1 = c + 1;
                while (c1 < 16 && ((((half >> (15 - c1)) & 1u) ? 0 : 1) == kk)) c1++;
                if (want[kk]) {
                    int L = pair * 2 + kk, x0 = c * 32, x1 = c1 * 32 > 496 ? 496 : c1 * 32;
                    pvr_poly_hdr_t *h = above ? &g_map_near[L] : &g_map_far[L];
                    pvr_prim(h, sizeof *h);
                    map_rect(x0, x1, y, y1, (lx[kk] + x0) & 511, ly[kk], z);
                }
                c = c1;
            }
            y = y1;
        }
    }
    g_cur_tex = -2;
}

static void begin_frame(void)
{
    static uint32_t drawn_gen;
    static int ever;
    uint32_t gen = bus_tilegen_generation();
    {
        uint64_t t = m3_now_us();
        pvr_wait_ready();
        m3_t_pvrwait_us += m3_now_us() - t;
    }
    /* Only when the guest has written tilegen VRAM or a register since:
     * through most of attract the 2D does not move, and drawing it costs
     * more than the rest of the field. */
    {
        static int old2d = -1;
        int fallback;
        if (old2d < 0) { const char *e = getenv("M3_MAP2D"); old2d = e && *e == '0'; }
        /* Filtering separately is not associative with alpha composition.
         * Keep the exact screen-space path for multi-pair priority planes
         * and line scroll; both are short transition effects in Scud. */
        fallback = old2d || tilegen_colour_offset() || !tilegen_map_filter_safe();
        if (fallback != g_fallback) { ever = 0; g_fallback = fallback; }
        if (!ever || gen != drawn_gen) {
            uint64_t t = m3_now_us();
            if (fallback) {
                g_layer_px[0] = tilegen_render_pass_1555(0, g_map[2], LAYER_W);
                g_layer_px[1] = tilegen_render_pass_1555(1, g_map[0], LAYER_W);
                g_map_force[0] = g_map_force[2] = 1;
            } else {
                int L;
                for (L = 0; L < 4; L++)
                    if (tilegen_layer_drawn(L, 0) || tilegen_layer_drawn(L, 1)) {
                        tilegen_update_map_1555(L, g_map[L], g_map_force[L]);
                        g_map_force[L] = 0;
                    }
            }
            m3_t_2d_us += m3_now_us() - t;
            drawn_gen = gen;
            ever = 1;
            g_layer_redraws++;
        }
    }
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    g_cur_tex = -2; g_cur_flip = -1;
    g_npt = g_ntr = g_op = 0;
    g_vp_lo = 1e-30f; g_vp_hi = 1e30f; g_vp_scale = 1.0f; g_vp_next = W_FLOOR;
    g_vp_x0 = 0.0f; g_vp_y0 = 0.0f; g_vp_x1 = 496.0f; g_vp_y1 = 384.0f;
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

/* A new viewport, whose w lies in [wmin, wmax]: the smallest power of two
 * that lifts wmin to the top of the last viewport. Only the farthest part
 * of a range deeper than W_SPAN is merged. */
static void begin_viewport(float wmin, float wmax, float x0, float y0,
                           float x1, float y1)
{
    int e;
    g_vp_x0 = x0; g_vp_y0 = y0; g_vp_x1 = x1; g_vp_y1 = y1;
    if (!(wmin <= wmax) || wmax <= 0.0f) return;     /* draws nothing */
    if (wmin < wmax / W_SPAN) wmin = wmax / W_SPAN;
    frexpf(g_vp_next / wmin, &e);                    /* 2^e > next / wmin */
    g_vp_scale = ldexpf(1.0f, e);
    if (wmax * g_vp_scale > W_TOP) {                 /* out of room: share */
        frexpf(W_TOP / wmax, &e);
        g_vp_scale = ldexpf(1.0f, e - 1);
    }
    g_vp_lo = wmin; g_vp_hi = wmax;
    g_vp_next = wmax * g_vp_scale;
}

static float vp_w(float w)
{
    if (w < g_vp_lo) w = g_vp_lo;
    if (w > g_vp_hi) w = g_vp_hi;
    return w * g_vp_scale;
}

/* Sutherland-Hodgman against one edge: keep where s*(coord - lim) <= 0.
 * x, y and w are affine in screen space, and u and v through u*w and v*w. */
static int cut(const m3_r3d_vertex *in, int n, m3_r3d_vertex *out,
               int axis, float lim, float sgn)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const m3_r3d_vertex *a = &in[i], *b = &in[(i + 1) % n];
        float da = sgn * ((axis ? a->y : a->x) - lim);
        float db = sgn * ((axis ? b->y : b->x) - lim);
        if (da <= 0.0f) out[m++] = *a;
        if ((da <= 0.0f) != (db <= 0.0f)) {
            float t = da / (da - db), w = a->w + t * (b->w - a->w);
            m3_r3d_vertex *o = &out[m++];
            o->x = a->x + t * (b->x - a->x);
            o->y = a->y + t * (b->y - a->y);
            o->w = w;
            o->u = (a->u * a->w + t * (b->u * b->w - a->u * a->w)) / w;
            o->v = (a->v * a->w + t * (b->v * b->w - a->v * a->w)) / w;
        }
    }
    return m;
}

static void polygon(const m3_r3d_vertex *in, int n, uint32_t argb,
                    int translucent, const m3_r3d_texture *t)
{
    int slot = -1, flip = 0, k, out = 0;
    m3_r3d_vertex a[POLY_MAX], b[POLY_MAX];
    const m3_r3d_vertex *v = a;
    if (!g_open || n < 3 || n > 4) return;
    for (k = 0; k < n; k++)
        if (in[k].x < g_vp_x0 || in[k].x > g_vp_x1 ||
            in[k].y < g_vp_y0 || in[k].y > g_vp_y1) out = 1;
    memcpy(a, in, sizeof *in * (size_t)n);
    if (out) {
        n = cut(a, n, b, 0, g_vp_x0, -1.0f);
        if (n >= 3) n = cut(b, n, a, 0, g_vp_x1, 1.0f);
        if (n >= 3) n = cut(a, n, b, 1, g_vp_y0, -1.0f);
        if (n >= 3) n = cut(b, n, a, 1, g_vp_y1, 1.0f);
        if (n < 3) return;
    }
    for (k = 0; k < n; k++) a[k].w = vp_w(a[k].w);
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
    /* Behind the 3D: far, so the depth test puts every 3D polygon over
     * them; punch-through, so an empty texel shows what is under it. */
    if (list == PVR_LIST_PT_POLY) {
        if (!g_fallback) draw_layers(0);
        else if (g_layer_px[0]) layer_quad(0, Z_BACK_B);
        g_cur_tex = -2; g_cur_flip = -1;
    }
    for (i = 0; i < n; i++) {
        send_header(list, k[i].tex, k[i].flip);
        strip(k[i].v, k[i].n, k[i].argb, 1.0f, 1.0f);
    }
    if (list == PVR_LIST_PT_POLY) {
        if (!g_fallback) draw_layers(1);
        else if (g_layer_px[1]) layer_quad(1, Z_FRONT);
    }
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

static const m3_r3d_sink k_sink = { begin_frame, polygon, end_frame, sheet_changed,
                                    begin_viewport };

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

    for (i = 0; i < 4; i++) {
        unsigned k;
        g_map[i] = (uint16_t *)pvr_mem_malloc(LAYER_W * LAYER_W * 2u);
        if (!g_map[i]) return 0;
        for (p = (uint32_t *)g_map[i], k = 0; k < LAYER_W * LAYER_W / 2u; k++) p[k] = 0;
        g_map_force[i] = 1;
        pvr_poly_cxt_txr(&cx, PVR_LIST_PT_POLY,
                         PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                         LAYER_W, LAYER_W, g_map[i], PVR_FILTER_BILINEAR);
        cx.gen.culling = PVR_CULLING_NONE;
        /* Behind everything the 3D draws, or in front of everything,
         * whatever the 3D wrote to the depth buffer. */
        cx.depth.comparison = PVR_DEPTHCMP_GREATER;
        pvr_poly_compile(&g_map_far[i], &cx);
        cx.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        pvr_poly_compile(&g_map_near[i], &cx);
    }

    g_pt = malloc(sizeof(kept_t) * PT_MAX);
    g_tr = malloc(sizeof(kept_t) * TR_MAX);
    if (!g_pt || !g_tr) return 0;
    printf("[pvr] sheet 2048 KB and tilemaps 2048 KB in PVR memory, %u KB left for textures"
           " (budget %u KB)\n", (unsigned)(pvr_mem_available() >> 10),
           TEX_BUDGET >> 10);
    real3d_set_sink(&k_sink);
    return 1;
}
