/* pvr_triangle: the PowerVR2 path on its own, before any Real3D data.
 *
 * Three triangles side by side, one per stage of the plan (prompt §27):
 *   left    opaque, flat red
 *   middle  opaque, Gouraud (red, green, blue corners)
 *   right   textured: an 8x8 ARGB1555 checkerboard, generated here, twiddled
 *           for the PVR, modulated by white
 * on a dark blue background, for a fixed number of frames, then exit.
 *
 * The console says which stage was submitted; the picture is the evidence,
 * so the harness photographs the screen (tools/dreamcast/dc-run.sh with a
 * capture), rather than trusting that a submitted polygon was drawn. */
#include <kos.h>
#include <stdio.h>

#define FRAMES 600

static pvr_ptr_t g_tex;

static void vert(pvr_vertex_t *v, int eol, float x, float y, float z,
                 float u, float t, uint32_t argb)
{
    v->flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v->x = x; v->y = y; v->z = z;
    v->u = u; v->v = t;
    v->argb = argb;
    v->oargb = 0;
}

static void tri(const pvr_poly_hdr_t *h, float x0, const uint32_t c[3],
                int textured)
{
    pvr_vertex_t v;
    pvr_prim((void *)h, sizeof *h);
    vert(&v, 0, x0,         360.0f, 1.0f, 0.0f, 1.0f, c[0]); pvr_prim(&v, sizeof v);
    vert(&v, 0, x0 + 90.0f, 120.0f, 1.0f, 0.5f, 0.0f, c[1]); pvr_prim(&v, sizeof v);
    vert(&v, 1, x0 + 180.0f, 360.0f, 1.0f, 1.0f, 1.0f, c[2]); pvr_prim(&v, sizeof v);
    (void)textured;
}

/* 8x8 checkerboard, white and orange, ARGB1555, written twiddled -- the
 * order the PVR reads a non-strided texture in. For a square power-of-two
 * texture, the twiddled index interleaves the bits of x and y. */
static uint32_t twiddle(uint32_t x, uint32_t y)
{
    uint32_t i, r = 0;
    for (i = 0; i < 3; i++)
        r |= ((y >> i) & 1u) << (2 * i) | ((x >> i) & 1u) << (2 * i + 1);
    return r;
}

static void make_texture(void)
{
    uint16_t px[64];
    uint32_t x, y;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            px[twiddle(x, y)] = ((x ^ y) & 1) ? 0xFFFF : 0xFE00; /* white / orange */
    g_tex = pvr_mem_malloc(sizeof px);
    pvr_txr_load(px, g_tex, sizeof px);
}

int main(void)
{
    pvr_poly_cxt_t cx;
    pvr_poly_hdr_t flat, gouraud, tex;
    static const uint32_t red[3] = { 0xFFFF0000, 0xFFFF0000, 0xFFFF0000 };
    static const uint32_t rgb[3] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF };
    static const uint32_t white[3] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    int f;

    printf("PVR TRIANGLE TEST\n");
    pvr_init_defaults();
    pvr_set_bg_color(0.05f, 0.05f, 0.25f);
    make_texture();

    pvr_poly_cxt_col(&cx, PVR_LIST_OP_POLY);
    cx.gen.shading = PVR_SHADE_FLAT;
    pvr_poly_compile(&flat, &cx);

    pvr_poly_cxt_col(&cx, PVR_LIST_OP_POLY);
    cx.gen.shading = PVR_SHADE_GOURAUD;
    pvr_poly_compile(&gouraud, &cx);

    pvr_poly_cxt_txr(&cx, PVR_LIST_OP_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_TWIDDLED,
                     8, 8, g_tex, PVR_FILTER_NONE);
    pvr_poly_compile(&tex, &cx);

    printf("stage 1: opaque flat     submitted\n");
    printf("stage 2: opaque Gouraud  submitted\n");
    printf("stage 3: textured        submitted\n");
    for (f = 0; f < FRAMES; f++) {
        pvr_wait_ready();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        tri(&flat, 20.0f, red, 0);
        tri(&gouraud, 230.0f, rgb, 0);
        tri(&tex, 440.0f, white, 1);
        pvr_list_finish();
        pvr_scene_finish();
        if (f == FRAMES / 2)
            printf("PVR FRAME %d\n", f);   /* the harness photographs here */
    }
    printf("PVR TRIANGLE DONE\n");
    return 0;
}
