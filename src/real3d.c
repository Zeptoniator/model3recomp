/* model3recomp -- Real3D scene renderer.
 *
 * Walks the scene the game builds every field -- viewports, a tree of culling
 * nodes, models -- and rasterises it with a depth buffer.
 *
 * Every structure below was read off a live scene dump and then checked
 * against something that could only come out right if the decode was right.
 * That matters more here than anywhere else in the emulator, because a 3D
 * pipeline decoded wrongly does not fail: it draws. It draws confident,
 * plausible, wrong geometry, and there is no screen to check it against --
 * MAME does not render Model 3 3D at all, so the oracle that settled the
 * tilemaps is not available for any of this.
 *
 * The checks, per structure:
 *
 *   viewport      word 0x14 decodes to 496x384, the exact display size, and
 *                 words 0x0C..0x0F are sin/cos pairs for 19.38 and 15
 *                 degrees -- whose tangent ratio, 1.3125, is the aspect of a
 *                 496x384 screen.
 *   culling node  ten words; a run of them at stride 0x0A has [00] rising by
 *                 0x400 and the matrix index in [03] by 1, every time.
 *   pointer list  entries are addresses of valid nodes, and the unused ones
 *                 are all the same dummy, 0x800800.
 *   matrix        twelve floats, translation first, then a 3x3 by rows.
 *                 Matrix 1 is [0,0,0, 1,0,0, 0,1,0, 0,0,1], which is exactly
 *                 identity under this reading and nothing under any other.
 *   polygon       the whole of polygon RAM tiles into models with zero slack
 *                 -- the last model ends on the last word -- under the rule
 *                 that a header is seven words followed by four words per
 *                 vertex not reused from the previous polygon.
 *   normals       every one of the 1260 header normals in that block comes
 *                 out exactly unit length. This is the strongest check here,
 *                 and it is what pinned the 24-bit sign extension and the
 *                 2^22 scale.
 *   VROM          a 16-lane interleave read big-endian is the only
 *                 arrangement in which VROM contains any models at all:
 *                 31265 of them, against none for the 8-lane image the ROM
 *                 builder used to produce. See tools/rom_loader.py.
 *
 * What is NOT settled is the camera. Matrix 0 is used as the view transform
 * because it is the only one of the table's first four that is a plain
 * rotation and the only one that puts any of the scene in front of the eye.
 * That is an argument, not a check, and it has never been held up against a
 * frame whose correct appearance is known -- attract mode does not reach a
 * scene with anything recognisable in it before a run times out. The light
 * direction is invented outright, and there are no textures.
 */
#include "model3recomp/real3d.h"
#include "model3recomp/bus.h"
#include "model3recomp/platform.h"
#include "model3recomp/model3recomp.h"

#include <stdio.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TRIS   40000u
#define MAX_DEPTH  24
#define AMBIENT    0.20f

/* The vertex fixed-point scale: the 24-bit coordinate field over 256.
 *
 * This finally has a real check behind it. Field 2080 of attract mode hands
 * the renderer 4525 triangles across 55 models, and at 1/256 they resolve
 * into a scene you can recognise -- rocks in the foreground, a cliff face
 * behind. At 1/16 the camera is inside the geometry and the screen is one
 * flat wash; at 1/1024 and beyond every object is sub-pixel and 1262
 * triangles between them light two pixels.
 *
 * Two earlier answers here were wrong, in opposite directions, and both came
 * with reasoning:
 *
 *   1/1024, from taking a flat 4:3 model for a backdrop that filled the
 *   screen and reading two matrices as independently agreeing on the size
 *   that implied. They were not agreeing on anything.
 *
 *   1/65536, from the observation that model extents crowd the top of the
 *   24-bit field. That observation is true and it is also worthless here:
 *   the raw integers fill the field whatever divisor you then apply, so it
 *   says nothing about the divisor at all. What it looked like was evidence.
 *
 * The lesson both times was the same. Nothing about this pipeline announces
 * a wrong scale -- it just draws something -- so the only thing that settles
 * it is a frame with a recognisable subject in it. M3_VERTEX_SCALE overrides.
 */
#define VERTEX_SCALE_DEFAULT 256.0f

static float g_vertex_scale = VERTEX_SCALE_DEFAULT;
static int g_tex_size_mode;
static int g_tex_all;

/* Texturing is off unless M3_TEXTURES is set, and that is deliberate.
 *
 * Everything between a polygon and a texel works: the descriptor comes out
 * of the header, the UVs are interpolated with perspective correction and
 * carried through the near clip, the texel is unpacked from 1-5-5-5 with
 * its transparency bit, wrapped to the texture's own size, and modulated by
 * the shading term. Turn it on and the colour count in a frame goes from
 * about a hundred to seven thousand.
 *
 * What does not work is the one thing in the middle: which VROM address a
 * polygon's texture coordinates name. Texels are 16-bit big-endian and they
 * are in the upper half of VROM -- both measured, the byte order by a
 * factor of five in smoothness -- but no arrangement tried turns those
 * coordinates into a picture. Linear at eight widths, 32- and 64-texel
 * tiles row- and column-major, two-level tilings, tile-row strides from 8
 * to 256, Morton order both ways, and undoing the ROM's lane-pair swap: the
 * best of them scores 3407 against a median of 5520 for arbitrary offsets,
 * which is not the margin a correct layout produces.
 *
 * So the polygons get real texels from the wrong places. It looks like
 * texturing -- foliage appears on the rocks -- and it is not, and a
 * renderer that looks right while being wrong is the thing this file exists
 * to avoid. Left in and switched off, because the next person needs the
 * pipeline and not the guess.
 */
static int g_textures;
static uint32_t g_scene_hash;
static uint32_t g_fb_hash;
static float g_vp07;
static unsigned g_tri_count;

unsigned real3d_last_tri_count(void) { return g_tri_count; }

/* ---- memory ------------------------------------------------------------
 * Culling and polygon RAM are little-endian, like everything else the
 * graphics side reads. VROM is the exception: see the note above.
 */
static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* A culling address is a word index, not a byte offset: viewport 0 links to
 * 0x800030 and the next viewport node begins exactly at word 0x30. */
static const uint8_t *cull_at(uint32_t addr, unsigned words)
{
    size_t n = 0;
    const uint8_t *m;
    uint32_t base;

    addr &= 0x00FFFFFFu;
    if (addr >= 0x800000u && addr < 0x840000u) {
        m = bus_cull_hi(&n);
        base = (addr & 0x3FFFFu) * 4u;
    } else if (addr < 0x100000u) {
        m = bus_cull_lo(&n);
        base = addr * 4u;
    } else {
        return NULL;
    }
    if (!m || (size_t)base + (size_t)words * 4u > n)
        return NULL;
    return m + base;
}

static float f32_of(uint32_t w)
{
    float f;
    memcpy(&f, &w, sizeof f);
    return f;
}

/* Sign-extend the top 24 bits of a word. */
static int32_t sx24(uint32_t w)
{
    uint32_t v = w >> 8;
    return (v & 0x800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
}

/* ---- matrices ---------------------------------------------------------- */
typedef struct { float m[3][4]; } mat_t;

static const mat_t IDENT = {{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};

static int mat_load(uint32_t base, uint32_t off, mat_t *out)
{
    const uint8_t *p = cull_at(base + off * 12u, 12);
    float v[12];
    int i;
    if (!p) return 0;
    for (i = 0; i < 12; i++)
        v[i] = f32_of(rd32le(p + i * 4));
    out->m[0][0] = v[3];  out->m[0][1] = v[4];  out->m[0][2] = v[5];
    out->m[1][0] = v[6];  out->m[1][1] = v[7];  out->m[1][2] = v[8];
    out->m[2][0] = v[9];  out->m[2][1] = v[10]; out->m[2][2] = v[11];
    out->m[0][3] = v[0];  out->m[1][3] = v[1];  out->m[2][3] = v[2];
    return 1;
}

static mat_t mat_mul(const mat_t *a, const mat_t *b)
{
    mat_t o;
    int r, c;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++)
            o.m[r][c] = a->m[r][0]*b->m[0][c] + a->m[r][1]*b->m[1][c]
                      + a->m[r][2]*b->m[2][c];
        o.m[r][3] = a->m[r][0]*b->m[0][3] + a->m[r][1]*b->m[1][3]
                  + a->m[r][2]*b->m[2][3] + a->m[r][3];
    }
    return o;
}

static void mat_point(const mat_t *m, const float *p, float *o)
{
    int r;
    for (r = 0; r < 3; r++)
        o[r] = m->m[r][0]*p[0] + m->m[r][1]*p[1] + m->m[r][2]*p[2] + m->m[r][3];
}

static void mat_dir(const mat_t *m, const float *p, float *o)
{
    int r;
    for (r = 0; r < 3; r++)
        o[r] = m->m[r][0]*p[0] + m->m[r][1]*p[1] + m->m[r][2]*p[2];
}

/* ---- textures ----------------------------------------------------------
 *
 * Texture memory is a flat sheet of 2048x2048 texels, built from uploads.
 * The guest streams entries into the FIFO at 0x94000000, each one
 *
 *     word 0   length of the entry in bytes
 *     word 1   the texture header
 *     word 2+  the texels
 *
 * and an entry occupies (2 + length/2)/4 words, which is how the next one is
 * found. The FIFO is drained when the guest rings the doorbell at
 * 0x88000000.
 *
 * The texels are not stored in reading order. They arrive in 8x8 tiles with
 * the pairs inside each tile transposed, and DETILE below is the map from a
 * position in the tile to the word it came from. That is the piece no amount
 * of staring at the ROM produced: the data is tiled at 8x8 and every attempt
 * here assumed 32x32 tiles or a linear sheet, and both are wrong.
 */
#define TEX_W 2048u
#define TEX_H 2048u

/* Position in an 8x8 tile -> index in the arriving data. */
static const unsigned char DETILE[64] = {
     1, 0, 5, 4, 9, 8,13,12,
     3, 2, 7, 6,11,10,15,14,
    17,16,21,20,25,24,29,28,
    19,18,23,22,27,26,31,30,
    33,32,37,36,41,40,45,44,
    35,34,39,38,43,42,47,46,
    49,48,53,52,57,56,61,60,
    51,50,55,54,59,58,63,62
};

static uint16_t *g_sheet;
static uint8_t  *g_sheet_set;          /* TEX_W * TEX_H texels */
static size_t    g_fifo_seen;      /* how much of the FIFO has been drained */
static unsigned  g_tex_uploads;

static void upload_texture(uint32_t header, const uint8_t *src, size_t avail)
{
    uint32_t x = 32u * (header & 0x3Fu);
    uint32_t y = 32u * ((header >> 7) & 0x1Fu);
    uint32_t page = (header >> 20) & 1u;
    uint32_t w = 32u << ((header >> 14) & 7u);
    uint32_t h = 32u << ((header >> 17) & 7u);
    int sixteen = (header >> 23) & 1u;
    uint32_t tx, ty, yy, xx;
    size_t off = 0;

    if (!g_sheet || w > 1024u || h > 1024u) return;
    y += page * 1024u;                      /* two 2048x1024 pages, stacked */
    if (x + w > TEX_W || y + h > TEX_H) return;

    for (ty = 0; ty < h; ty += 8u) {
        for (tx = 0; tx < w; tx += 8u) {
            for (yy = 0; yy < 8u; yy++) {
                for (xx = 0; xx < 8u; xx++) {
                    size_t si = off + DETILE[yy * 8u + xx];
                    uint16_t t;
                    if (sixteen) {
                        if ((si + 1) * 2 > avail) continue;
                        t = (uint16_t)((src[si*2] << 8) | src[si*2 + 1]);
                    } else {
                        if (si >= avail) continue;
                        /* 8-bit: the byte is an intensity until the palette
                         * path exists, which keeps the shape visible rather
                         * than drawing noise. */
                        uint32_t v = src[si] >> 3;
                        t = (uint16_t)((v << 10) | (v << 5) | v);
                    }
                    {
                        size_t o = (size_t)(y + ty + yy) * TEX_W
                                 + (x + tx + xx);
                        g_sheet[o] = t;
                        if (g_sheet_set) g_sheet_set[o] = 1;
                    }
                }
            }
            off += 64;
        }
    }
    g_tex_uploads++;
}

/* Drain whatever the guest has put in the FIFO since last time. */
static void flush_texture_fifo(void)
{
    size_t used = 0, i;
    const uint8_t *f = bus_texfifo(&used, NULL);
    if (!f || used <= g_fifo_seen) return;
    for (i = 0; i + 8 <= used; ) {
        uint32_t len = rd32le(f + i);
        uint32_t words = (2u + len / 2u) / 4u;
        uint32_t header = rd32le(f + i + 4);
        if (getenv("M3_TEX_TRACE")) {
            static unsigned n;
            if (n++ < 12)
                fprintf(stderr, "[tex] entry at %u: len %u -> %u words, "
                        "header %08X%s", (unsigned)i, len, words, header,
                        "\n");
        }
        if (!words || (size_t)words * 4u > used - i) break;
        upload_texture(header, f + i + 8, (size_t)words * 4u - 8u);
        i += (size_t)words * 4u;
    }
    g_fifo_seen = used;
    if (getenv("M3_TEX_TRACE"))
        fprintf(stderr, "[tex] fifo %u bytes, %u uploads so far%s",
                (unsigned)used, g_tex_uploads, "\n");
}

static uint32_t sheet_texel(unsigned x, unsigned y, int *transparent)
{
    uint16_t c;
    uint32_t r, g, b;
    *transparent = 1;
    if (!g_sheet) return 0;
    {
        size_t o = (size_t)(y & (TEX_H - 1u)) * TEX_W + (x & (TEX_W - 1u));
        /* Never uploaded. Say so rather than drawing the cleared sheet,
         * which is opaque black and turns every untextured surface into a
         * silhouette. */
        if (g_sheet_set && !g_sheet_set[o]) { *transparent = -1; return 0; }
        c = g_sheet[o];
    }
    if (c & 0x8000u) return 0;
    *transparent = 0;
    r = (c >> 10) & 0x1Fu; g = (c >> 5) & 0x1Fu; b = c & 0x1Fu;
    r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* ---- the triangle queue ------------------------------------------------ */
typedef struct {
    float v[3][3];          /* view space */
    float n[3];             /* the polygon's own normal, from the header */
    float vn[3][3];         /* per-vertex normals, for Gouraud */
    float uv[3][2];         /* texel coordinates */
    uint16_t tx, ty;        /* texture origin in the sheet */
    uint16_t tw, th;        /* its size, for wrapping */
    uint8_t page;
    uint8_t textured;
    uint8_t r, g, b;
} tri_t;

static tri_t *g_tris;
static unsigned g_ntris;

typedef struct { int on; unsigned x, y, page, w, h, fmt; } texdesc_t;

static void emit(const float a[3], const float b[3], const float c[3],
                 const float n[3], uint32_t rgb,
                 const float na[3], const float nb[3], const float nc[3],
                 const float ua[2], const float ub[2], const float uc[2],
                 const texdesc_t *td)
{
    tri_t *t;
    if (g_ntris >= MAX_TRIS) return;
    t = &g_tris[g_ntris++];
    memcpy(t->v[0], a, sizeof t->v[0]);
    memcpy(t->v[1], b, sizeof t->v[1]);
    memcpy(t->v[2], c, sizeof t->v[2]);
    memcpy(t->n, n, sizeof t->n);
    memcpy(t->vn[0], na, sizeof t->vn[0]);
    memcpy(t->vn[1], nb, sizeof t->vn[1]);
    memcpy(t->vn[2], nc, sizeof t->vn[2]);
    memcpy(t->uv[0], ua, sizeof t->uv[0]);
    memcpy(t->uv[1], ub, sizeof t->uv[1]);
    memcpy(t->uv[2], uc, sizeof t->uv[2]);
    t->textured = (uint8_t)(td->on ? 1 : 0);
    t->tx = (uint16_t)td->x; t->ty = (uint16_t)td->y;
    t->tw = (uint16_t)td->w; t->th = (uint16_t)td->h;
    t->page = (uint8_t)td->page;
    t->r = (uint8_t)((rgb >> 16) & 0xFF);
    t->g = (uint8_t)((rgb >> 8) & 0xFF);
    t->b = (uint8_t)(rgb & 0xFF);
}

/* ---- models ------------------------------------------------------------ */
static void draw_model(uint32_t addr, const mat_t *m)
{
    const uint8_t *mem;
    size_t n = 0;
    uint32_t (*rd)(const uint8_t *);
    size_t i;
    float prev[4][3], prevn[4][3], prevuv[4][2];
    unsigned nprev = 0;
    unsigned guard;

    if (addr & 0x800000u) {
        mem = bus_vrom(&n);
        rd = rd32be;                    /* VROM alone is read big-endian */
        i = (size_t)(addr & 0x7FFFFFu) * 4u;
    } else {
        mem = bus_poly(&n);
        rd = rd32le;
        i = (size_t)addr * 4u;
    }
    if (!mem) return;

    for (guard = 0; guard < 20000u; guard++) {
        uint32_t ph[7];
        float nrm[3], vs[4][3], vn[4][3], uv[4][2];
        texdesc_t td;
        unsigned nv, nnew, k, j, slot;
        uint32_t rgb;
        float len;

        if (i + 7u * 4u > n) return;
        for (k = 0; k < 7; k++) ph[k] = rd(mem + i + k * 4);

        /* Header words 1..3 are the polygon normal, 24-bit fixed over 2^22.
         * A real header's normal is a unit vector; this is the check that
         * catches a model pointer that is not a model, which otherwise
         * produces triangles rather than an error. */
        for (k = 0; k < 3; k++)
            nrm[k] = (float)sx24(ph[k + 1]) / 4194304.0f;
        len = nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2];
        if (len < 0.98f || len > 1.02f) return;

        nv = (ph[0] & 0x40u) ? 4u : 3u;
        slot = 0;
        for (k = 0; k < 4; k++)             /* bits 0..3: reuse from previous */
            if (((ph[0] >> k) & 1u) && k < nprev && slot < 4) {
                memcpy(vs[slot], prev[k], sizeof vs[slot]);
                memcpy(vn[slot], prevn[k], sizeof vn[slot]);
                memcpy(uv[slot], prevuv[k], sizeof uv[slot]);
                slot++;
            }
        if (slot > nv) return;
        nnew = nv - slot;
        if (i + (7u + 4u * nnew) * 4u > n) return;

        for (k = 0; k < nnew && slot < 4; k++, slot++) {
            size_t o = i + (7u + 4u * k) * 4u;
            uint32_t wx = rd(mem + o), wy = rd(mem + o + 4),
                     wz = rd(mem + o + 8);
            vs[slot][0] = (float)sx24(wx) / g_vertex_scale;
            vs[slot][1] = (float)sx24(wy) / g_vertex_scale;
            vs[slot][2] = (float)sx24(wz) / g_vertex_scale;
            /* The low byte of each coordinate word is that vertex's own
             * normal, as a signed byte. They are not unit length -- the
             * lengths run from 0 to 127 -- but they do sit a median 25
             * degrees from the face normal, against the 90 that random
             * directions would give, which is what identifies them. */
            vn[slot][0] = (float)(int8_t)(wx & 0xFFu);
            vn[slot][1] = (float)(int8_t)(wy & 0xFFu);
            vn[slot][2] = (float)(int8_t)(wz & 0xFFu);
            {
                /* Word 3 of a vertex is u in the high half and v in the low.
                 * The spans land on 512, 1024 and 4096, so the unit is an
                 * eighth of a texel. */
                uint32_t wt = rd(mem + o + 12);
                uv[slot][0] = (float)((wt >> 16) & 0xFFFFu) / 8.0f;
                uv[slot][1] = (float)(wt & 0xFFFFu) / 8.0f;
            }
        }

        rgb = (((ph[4] >> 24) & 0xFFu) << 16) | (((ph[4] >> 16) & 0xFFu) << 8)
            | ((ph[4] >> 8) & 0xFFu);

        /* The texture descriptor. Bit 26 of word 6 is the enable: it is set
         * on 1749 of the 1780 polygons in one model and clear on the rest.
         * The origin is in 32-texel units, which is also the tile size. */
        /* The texture descriptor, and three of these were wrong before.
         *
         * The enable is bit 10 of word 6, not bit 26. Width and height are
         * the low bits of word 3 -- the same word whose top 24 bits are the
         * polygon normal, which is why they were never found there. The page
         * is bit 6 of word 4, whose top 24 bits are the colour. Both size
         * fields read 6 or 7 to mean 32. */
        {
            uint32_t ws = (ph[3] >> 3) & 7u, hs = ph[3] & 7u;
            if (ws >= 6u) ws = 0u;
            if (hs >= 6u) hs = 0u;
            td.w = 32u << ws;
            td.h = 32u << hs;
        }
        td.fmt  = (ph[6] >> 7) & 7u;
        td.on   = g_textures && (ph[6] & 0x400u) != 0;
        td.x    = (32u * (((ph[4] & 0x1Fu) << 1) | ((ph[5] >> 7) & 1u))) & 2047u;
        td.y    = (32u * (ph[5] & 0x1Fu)) & 2047u;
        td.page = (ph[4] & 0x40u) >> 6;
        if (td.w > 2048u) td.w = 2048u;
        if (td.h > 2048u) td.h = 2048u;

        if (slot >= 3) {
            float wv[4][3], wn[3], wvn[4][3];
            for (j = 0; j < slot; j++) {
                mat_point(m, vs[j], wv[j]);
                mat_dir(m, vn[j], wvn[j]);
            }
            mat_dir(m, nrm, wn);
            for (j = 1; j + 1 < slot; j++)
                emit(wv[0], wv[j], wv[j+1], wn, rgb,
                     wvn[0], wvn[j], wvn[j+1],
                     uv[0], uv[j], uv[j+1], &td);
        }

        for (j = 0; j < slot; j++) {
            memcpy(prev[j], vs[j], sizeof prev[j]);
            memcpy(prevn[j], vn[j], sizeof prevn[j]);
            memcpy(prevuv[j], uv[j], sizeof prevuv[j]);
        }
        nprev = slot;

        i += (7u + 4u * nnew) * 4u;
        if (ph[1] & 4u) return;             /* last polygon of the model */
    }
}

/* ---- the node tree ----------------------------------------------------- */
static uint32_t g_matbase;

static void descend(uint32_t addr, const mat_t *m, int depth);

/* A pointer's top byte says what it points at: bit 24 a model, bit 26 a
 * pointer list, and otherwise another culling node. */
static void follow(uint32_t p, const mat_t *m, int depth)
{
    uint32_t a = p & 0x00FFFFFFu;

    if (a == 0 || a == 0x800800u)           /* 0x800800 is the dummy entry */
        return;

    if (p & 0x01000000u) {
        draw_model(a, m);
    } else if (p & 0x04000000u) {
        unsigned k;
        if (depth >= MAX_DEPTH) return;
        for (k = 0; k < 256u; k++) {
            const uint8_t *e = cull_at(a + k, 1);
            uint32_t v;
            if (!e) break;
            v = rd32le(e);
            /* Both flag bits together is a skip -- not an entry, and not an
             * end either. Treating it as an end stops the list at its first
             * element, which is exactly what an attract-mode list starts
             * with, and loses every model in it. */
            if ((v & 0x03000000u) == 0x03000000u)
                continue;
            follow(v, m, depth + 1);
            if (v & 0x02000000u)            /* last entry: after, not before */
                break;
        }
    } else {
        descend(a, m, depth + 1);
    }
}

static void descend(uint32_t addr, const mat_t *m, int depth)
{
    const uint8_t *node = cull_at(addr, 10);
    mat_t cur, mm;
    uint32_t mo;
    float tx, ty, tz;

    if (!node || depth >= MAX_DEPTH) return;
    cur = *m;

    mo = rd32le(node + 3 * 4) & 0xFFFu;
    if (mo && mat_load(g_matbase, mo, &mm))
        cur = mat_mul(m, &mm);

    tx = f32_of(rd32le(node + 4 * 4));
    ty = f32_of(rd32le(node + 5 * 4));
    tz = f32_of(rd32le(node + 6 * 4));
    if (tx != 0.0f || ty != 0.0f || tz != 0.0f) {
        mat_t t = IDENT;
        t.m[0][3] = tx; t.m[1][3] = ty; t.m[2][3] = tz;
        cur = mat_mul(&cur, &t);
    }

    follow(rd32le(node + 7 * 4), &cur, depth);
    follow(rd32le(node + 8 * 4), &cur, depth);
}

/* ---- rasteriser -------------------------------------------------------- */
static unsigned g_drawn;
static float *g_zbuf;
static int g_zw, g_zh;

#define ZNEAR 0.05f

/* Sutherland-Hodgman against z >= ZNEAR. Three input vertices give at most
 * four out, so the caller's buffer is [4][3]. */
static int clip_near(const float v[3][5], float out[4][5])
{
    int n = 0, i, k;
    for (i = 0; i < 3; i++) {
        const float *a = v[i];
        const float *b = v[(i + 1) % 3];
        int ain = a[2] >= ZNEAR, bin = b[2] >= ZNEAR;
        if (ain && n < 4) {
            for (k = 0; k < 5; k++) out[n][k] = a[k];
            n++;
        }
        if (ain != bin && n < 4) {
            float t = (ZNEAR - a[2]) / (b[2] - a[2]);
            for (k = 0; k < 5; k++) out[n][k] = a[k] + t * (b[k] - a[k]);
            n++;
        }
    }
    return n;
}

static void raster(uint32_t *fb, int fw, int fh, float sx, float sy,
                   const float L[3], float intensity)
{
    /* The light comes from the viewport now, not from an invention.
     * Words 0x04..0x06 are a unit vector -- (-0.72288, -0.16226, -0.67156)
     * in this game, length 1.0000 to five places, and the same in every
     * scene dumped -- which is what identifies them. Word 0x07 sits with
     * Word 0x07 is a global brightness multiplier, and it took a second
     * look to see it. It was dismissed once for reading 0.93 in one attract
     * shot and 0.06 in another that was plainly not six per cent as bright
     * -- which was two moments compared without asking what happened
     * between them. Sampled every field it is unmistakable: it holds at
     * exactly 1.0000 for a thousand fields, ramps down in even steps to
     * 0.0000, holds there through the gap between demo shots, and ramps
     * back. It is the cross-fade.
     *
     * Ignoring it is what made the renderer look frozen: the game spent
     * twenty fields fading a shot to black and the picture stayed at full
     * brightness, byte-identical, while the scene underneath it changed
     * every field. */
    unsigned t;

    for (t = 0; t < g_ntris; t++) {
        const tri_t *tr = &g_tris[t];
        /* Screen-space vertices in double. A projected triangle here can
         * reach coordinates in the hundreds of thousands -- the scene runs
         * well past the edges of a 496-pixel screen -- and in float the
         * barycentric denominator then loses enough precision that pixels
         * inside the triangle test as outside. It does not look like a
         * precision bug: it looks like a cross-hatch, as though the polygon
         * were deliberately stippled for translucency. */
        double p[3][3], q[3][3];
        float clipped[4][5], src[3][5];
        double d;
        float lit, sh;
        int nclip, fan;
        int x0, x1, y0, y1, x, y, k;
        int r, g, b;
        uint32_t argb;

        /* Backface culling.
         *
         * Safe here because the winding is consistent: in a dumped model all
         * 588 polygons agree in sign between their header normal and the
         * normal computed from their own vertices, every one of them. So a
         * polygon faces away exactly when its normal points along the ray
         * from the eye to it, and there is no need to guess at a
         * double-sided flag to avoid throwing away visible surfaces.
         *
         * It removes about five triangles in six -- 2169 of 2617 in one
         * frame -- and the pixel count barely moves (105903 to 105527),
         * which is what culling only hidden geometry looks like. */
        {
            float cx = (tr->v[0][0] + tr->v[1][0] + tr->v[2][0]) / 3.0f;
            float cy = (tr->v[0][1] + tr->v[1][1] + tr->v[2][1]) / 3.0f;
            float cz = (tr->v[0][2] + tr->v[1][2] + tr->v[2][2]) / 3.0f;
            if (tr->n[0]*cx + tr->n[1]*cy + tr->n[2]*cz > 0.0f)
                continue;
        }

        /* Cut against the near plane rather than dropping the triangle.
         * A triangle that straddles the eye plane cannot be projected, but
         * discarding it throws away exactly the large near-camera polygons
         * -- the ones that would fill the screen -- so a scene can come out
         * completely empty while every individual step looks correct. */
        for (k = 0; k < 3; k++) {
            src[k][0] = tr->v[k][0]; src[k][1] = tr->v[k][1];
            src[k][2] = tr->v[k][2];
            src[k][3] = tr->uv[k][0]; src[k][4] = tr->uv[k][1];
        }
        nclip = clip_near(src, clipped);
        if (nclip < 3) continue;

        for (fan = 1; fan + 1 < nclip; fan++) {
            const float *cv[3];
            cv[0] = clipped[0]; cv[1] = clipped[fan]; cv[2] = clipped[fan + 1];

        for (k = 0; k < 3; k++) {
            double iz = 1.0 / cv[k][2];
            p[k][0] = fw * 0.5 + (double)cv[k][0] * iz * sx;
            p[k][1] = fh * 0.5 - (double)cv[k][1] * iz * sy;
            p[k][2] = cv[k][2];
            /* Perspective correction: interpolate u/z, v/z and 1/z linearly
             * in screen space and divide at the end. Interpolating u and v
             * directly is visibly wrong on polygons this large -- a ground
             * plane running to the horizon is exactly the case it breaks. */
            q[k][0] = (double)cv[k][3] * iz;
            q[k][1] = (double)cv[k][4] * iz;
            q[k][2] = iz;
        }

        d = (p[1][1]-p[2][1])*(p[0][0]-p[2][0])
          + (p[2][0]-p[1][0])*(p[0][1]-p[2][1]);
        if (d > -1e-9 && d < 1e-9) continue;

        /* Flat shade as the fallback, for a polygon whose vertex normals
         * are degenerate. One-sided now that backfaces are culled: with the
         * real light the term runs from -0.72 to +0.93 across a frame, so
         * roughly two triangles in five are in shadow and the picture has
         * some range in it. Taking the absolute value, which is what this
         * did before, lit both sides equally and flattened that out. */
        lit = tr->n[0]*L[0] + tr->n[1]*L[1] + tr->n[2]*L[2];
        if (lit < 0) lit = 0;
        if (lit > 1.0f) lit = 1.0f;
        sh = (AMBIENT + (1.0f - AMBIENT) * lit) * intensity;
        r = (int)(tr->r * sh); g = (int)(tr->g * sh); b = (int)(tr->b * sh);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8)
             | (uint32_t)b;

        {
            double lo, hi;
            lo = p[0][0] < p[1][0] ? p[0][0] : p[1][0];
            if (p[2][0] < lo) lo = p[2][0];
            hi = p[0][0] > p[1][0] ? p[0][0] : p[1][0];
            if (p[2][0] > hi) hi = p[2][0];
            x0 = lo < 0 ? 0 : (lo > fw - 1 ? fw - 1 : (int)lo);
            x1 = hi < 0 ? 0 : (hi > fw - 1 ? fw - 1 : (int)hi + 1);
            if (x1 > fw - 1) x1 = fw - 1;
            lo = p[0][1] < p[1][1] ? p[0][1] : p[1][1];
            if (p[2][1] < lo) lo = p[2][1];
            hi = p[0][1] > p[1][1] ? p[0][1] : p[1][1];
            if (p[2][1] > hi) hi = p[2][1];
            y0 = lo < 0 ? 0 : (lo > fh - 1 ? fh - 1 : (int)lo);
            y1 = hi < 0 ? 0 : (hi > fh - 1 ? fh - 1 : (int)hi + 1);
            if (y1 > fh - 1) y1 = fh - 1;
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fw - 1) x1 = fw - 1;
        if (y1 > fh - 1) y1 = fh - 1;

        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                double l0, l1, l2, z;
                int o;
                l0 = ((p[1][1]-p[2][1])*(x-p[2][0])
                    + (p[2][0]-p[1][0])*(y-p[2][1])) / d;
                l1 = ((p[2][1]-p[0][1])*(x-p[2][0])
                    + (p[0][0]-p[2][0])*(y-p[2][1])) / d;
                l2 = 1.0f - l0 - l1;
                if (l0 < 0 || l1 < 0 || l2 < 0) continue;
                z = l0*p[0][2] + l1*p[1][2] + l2*p[2][2];
                o = y * fw + x;
                if (z < g_zbuf[o]) {
                    uint32_t c = argb;
                    if (tr->textured) {
                        double iz = l0*q[0][2] + l1*q[1][2] + l2*q[2][2];
                        if (iz > 1e-12) {
                            double uu = (l0*q[0][0] + l1*q[1][0] + l2*q[2][0]) / iz;
                            double vv = (l0*q[0][1] + l1*q[1][1] + l2*q[2][1]) / iz;
                            int tp;
                            long ui = (long)uu, vi = (long)vv;
                            unsigned tw = tr->tw ? tr->tw : 32u;
                            unsigned th = tr->th ? tr->th : 32u;
                            uint32_t t;
                            ui %= (long)tw; if (ui < 0) ui += tw;
                            vi %= (long)th; if (vi < 0) vi += th;
                            t = sheet_texel(tr->tx + (unsigned)ui,
                                            tr->ty + tr->page * 1024u
                                                   + (unsigned)vi, &tp);
                            if (tp > 0) continue;   /* transparent texel */
                            if (tp == 0) c = t;     /* tp < 0: never loaded */
                        }
                    }
                    /* Gouraud: interpolate the vertex normals across the
                     * triangle. The hardware shades this way, and the
                     * faceting without it is the most visible thing wrong
                     * with a rounded surface like a boulder. */
                    float nx = (float)(l0*tr->vn[0][0] + l1*tr->vn[1][0]
                                     + l2*tr->vn[2][0]);
                    float ny = (float)(l0*tr->vn[0][1] + l1*tr->vn[1][1]
                                     + l2*tr->vn[2][1]);
                    float nz = (float)(l0*tr->vn[0][2] + l1*tr->vn[1][2]
                                     + l2*tr->vn[2][2]);
                    float nl = nx*nx + ny*ny + nz*nz;
                    if (nl > 1e-6f) {
                        float il = 1.0f / sqrtf(nl);
                        float d = (nx*L[0] + ny*L[1] + nz*L[2]) * il;
                        float s2;
                        int rr, gg, bb;
                        if (d < 0) d = 0;
                        if (d > 1.0f) d = 1.0f;
                        s2 = (AMBIENT + (1.0f - AMBIENT) * d) * intensity;
                        rr = (int)(tr->r * s2); gg = (int)(tr->g * s2);
                        bb = (int)(tr->b * s2);
                        if (rr > 255) rr = 255;
                        if (gg > 255) gg = 255;
                        if (bb > 255) bb = 255;
                        if (tr->textured) {
                            rr = (int)(((c >> 16) & 0xFF) * s2);
                            gg = (int)(((c >> 8) & 0xFF) * s2);
                            bb = (int)((c & 0xFF) * s2);
                        }
                        c = 0xFF000000u | ((uint32_t)rr << 16)
                          | ((uint32_t)gg << 8) | (uint32_t)bb;
                    }
                    g_zbuf[o] = (float)z; fb[o] = c; g_drawn++;
                }
            }
        }
        }
    }
}

/* ---- entry point ------------------------------------------------------- */
void real3d_render(void)
{
    int fw = 0, fh = 0;
    uint32_t *fb = platform_framebuffer(&fw, &fh);
    uint32_t vpaddr;
    unsigned guard, total = 0;
    static int inited;

    if (!fb) return;

    if (!inited) {
        const char *e = getenv("M3_VERTEX_SCALE");
        if (e && atof(e) > 0.0) g_vertex_scale = (float)atof(e);
        e = getenv("M3_TEX_SIZE");
        if (e) g_tex_size_mode = atoi(e);
        g_tex_all = getenv("M3_TEX_ALL") != NULL;
        g_textures = getenv("M3_TEXTURES") != NULL;
        g_tris = (tri_t *)malloc(sizeof(tri_t) * MAX_TRIS);
        g_sheet = (uint16_t *)calloc(TEX_W * TEX_H, sizeof(uint16_t));
        g_sheet_set = (uint8_t *)calloc(TEX_W * TEX_H, 1);
        inited = 1;
    }
    if (!g_tris) return;

    flush_texture_fifo();

    memset(fb, 0, (size_t)fw * (size_t)fh * sizeof(uint32_t));

    if (g_zw != fw || g_zh != fh) {
        free(g_zbuf);
        g_zbuf = (float *)malloc(sizeof(float) * (size_t)fw * (size_t)fh);
        g_zw = fw; g_zh = fh;
    }
    if (!g_zbuf) return;
    {
        int i, nn = fw * fh;
        for (i = 0; i < nn; i++) g_zbuf[i] = 1e30f;
    }

    g_drawn = 0;
    vpaddr = 0x800000u;
    for (guard = 0; guard < 16u && vpaddr; guard++) {
        const uint8_t *vp = cull_at(vpaddr, 0x20);
        mat_t cam;
        float hf, vf, sx, sy, light[3], intensity;
        uint32_t next;

        if (!vp) break;
        g_matbase = rd32le(vp + 0x16 * 4) & 0x00FFFFFFu;

        /* Words 0x0C and 0x0E are the sines of the horizontal and vertical
         * half angles. For this game they are 19.38 and 15 degrees, whose
         * tangents are in the ratio 1.3125 -- the aspect of a 496x384
         * screen, which is how the pair was identified. */
        hf = f32_of(rd32le(vp + 0x0C * 4));
        vf = f32_of(rd32le(vp + 0x0E * 4));
        if (hf < -1.0f) hf = -1.0f;
        if (hf > 1.0f) hf = 1.0f;
        if (vf < -1.0f) vf = -1.0f;
        if (vf > 1.0f) vf = 1.0f;
        hf = asinf(hf); vf = asinf(vf);
        if (hf < 1e-3f || vf < 1e-3f) { hf = 0.338f; vf = 0.262f; }
        sx = (fw * 0.5f) / tanf(hf);
        sy = (fh * 0.5f) / tanf(vf);

        light[0] = f32_of(rd32le(vp + 0x04 * 4));
        light[1] = f32_of(rd32le(vp + 0x05 * 4));
        light[2] = f32_of(rd32le(vp + 0x06 * 4));
        g_vp07 = f32_of(rd32le(vp + 0x07 * 4));
        if (light[0] == 0.0f && light[1] == 0.0f && light[2] == 0.0f) {
            light[0] = 0.3f; light[1] = 0.5f; light[2] = -0.81f;
        }

        /* A fresh depth buffer per viewport. They are separate layers, not
         * one scene: sharing a buffer lets whichever viewport is walked
         * first decide the depth of the whole screen, and the first one
         * here holds a flat quad that covers it. */
        {
            int i, nn = fw * fh;
            for (i = 0; i < nn; i++) g_zbuf[i] = 1e30f;
        }

        /* No camera matrix. Matrix 0 looked like one -- a plain rotation,
         * sitting where a view transform belongs -- and using it turned the
         * one scene available at the time from nothing into something, which
         * was taken for confirmation. On a frame that actually has a scene
         * in it, applying matrix 0 pushes the whole thing off the left of
         * the screen, and leaving it out gives rocks and a cliff face. The
         * node matrices already carry the geometry into view space. */
        cam = IDENT;

        intensity = f32_of(rd32le(vp + 0x07 * 4));
        if (!(intensity >= 0.0f)) intensity = 1.0f;   /* NaN guard */
        if (intensity > 2.0f) intensity = 2.0f;

        g_ntris = 0;
        descend(rd32le(vp + 0x02 * 4) & 0x00FFFFFFu, &cam, 0);
        raster(fb, fw, fh, sx, sy, light, intensity);
        total += g_ntris;

        next = rd32le(vp + 0x01 * 4) & 0x00FFFFFFu;
        if (next == vpaddr) break;
        vpaddr = next;
    }
    g_tri_count = total;

    /* M3_REAL3D_STATS: how much geometry the game handed us, and how much of
     * it landed. Printed on change rather than per field, because it changes
     * a handful of times in an attract cycle and printing it every field
     * buries that. This is how you find a frame that has 3D in it at all. */
    /* A cheap fingerprint of what the guest handed us: the culling tree and
     * the matrix table. If the picture stops changing, this says whether the
     * guest stopped writing a new scene or kept writing the same one. */
    if (getenv("M3_REAL3D_STATS")) {
        size_t cn = 0;
        const uint8_t *cp = bus_cull_hi(&cn);
        uint32_t h = 2166136261u;
        if (cp) {
            size_t k, lim = cn < 0x20000u ? cn : 0x20000u;
            for (k = 0; k < lim; k += 4)
                h = (h ^ rd32le(cp + k)) * 16777619u;
        }
        g_scene_hash = h;
    }
    /* And a fingerprint of what came out, so a scene that changes while the
     * picture does not shows up as exactly that. */
    if (getenv("M3_REAL3D_STATS")) {
        uint32_t h = 2166136261u;
        int k, nn = fw * fh;
        for (k = 0; k < nn; k += 7)
            h = (h ^ fb[k]) * 16777619u;
        g_fb_hash = h;
    }
    if (getenv("M3_REAL3D_STATS")) {
        static unsigned last_total = 0xFFFFFFFFu;
        static unsigned last_drawn = 0xFFFFFFFFu;
        static uint32_t last_hash = 0xFFFFFFFFu;
        if (total != last_total || g_drawn != last_drawn ||
            g_scene_hash != last_hash) {
            last_total = total; last_drawn = g_drawn;
            last_hash = g_scene_hash;
            { size_t _fu = 0; bus_texfifo(&_fu, NULL);
            fprintf(stderr, "[real3d] field %llu trig %llu fifo %llu scene %08X fb %08X vp07 %.4f: %u triangles, %u drawn\n",
                    (unsigned long long)model3recomp_frame_count(),
                    (unsigned long long)bus_r3d_trigger_count(),
                    (unsigned long long)_fu, g_scene_hash, g_fb_hash,
                    g_vp07, total, g_drawn); }
            fflush(stderr);
        }
    }
}
