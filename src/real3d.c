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
#define AMBIENT    g_ambient

/* The vertex fixed-point scale: the 24-bit coordinate field over 4096,
 * which is twelve fractional bits and twelve integer ones.
 *
 * It was 256 for a long time, on a check that looked sound: a frame of
 * attract mode resolved into rocks and a cliff face at 1/256 and into
 * nothing recognisable at 1/16 or 1/1024. That check was run while the
 * matrix stack push was being lifted into unreachable code (see
 * tools/ppc_lifter.py), so every matrix in the scene carried the previous
 * one's scale. A wrong scale in the matrices and a wrong scale here
 * cancelled, and the frame looked right.
 *
 * With the push fixed both scenes agree on 4096, and both now have a
 * subject in them: attract is a textured T-Rex roaring in the rain, and a
 * round is a jungle floor with trees, a horizon and figures in the
 * distance. At 1/1024 the same round is one black box filling the screen.
 *
 * Supermodel takes a scale per model rather than a constant, so this may
 * yet turn out to be the common case rather than the rule.
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
#define VERTEX_SCALE_DEFAULT 4096.0f

static float g_vertex_scale = VERTEX_SCALE_DEFAULT;
static int g_tex_size_mode;
static int g_tex_all;

/* Texturing. M3_NO_TEXTURES turns it off.
 *
 * This works now, and what unblocked it was not the renderer. The guest
 * could not read its own data ROM: the banked CROM window decoded the bank
 * select by shifting the written byte into an offset, which for the values
 * this game writes lands outside the image, so every read through the
 * window returned zero. With that fixed the guest streams 3.4 MB to the
 * Real3D in a run instead of 321 KB, and the difference is every texture in
 * the game.
 */
static int g_textures;
static float g_ambient = 0.20f;
static int   g_ambient_forced;
static int g_light_flip;
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
    /* The origin and size of an upload. Dumping the sheet to a PNG shows
     * a coherent atlas -- jeeps, Ian and Sarah, dinosaur hide, INGEN
     * signage -- so these fields are right, and the payload length agrees
     * with w*h*2 on every accepted entry, which is what pins the two size
     * exponents.
     *
     * The page is the open question. Bit 20 is never set in a whole boot
     * of this game, so nothing is ever uploaded to the upper half of
     * texture memory, and every polygon in a round asks for it. Bit 6
     * (set in 460 of 464 uploads), bit 5 (set in none of the early ones
     * and 200 of the late ones) and reading y as six bits instead of
     * five-plus-a-page were all tried: bit 6 changes nothing, bit 5 is
     * worse, and six bits of y fixes the round and breaks attract, which
     * finds every one of its texels today. */
    uint32_t x = 32u * (header & 0x3Fu);
    uint32_t y = 32u * ((header >> 7) & 0x1Fu);
    uint32_t page = (header >> 20) & 1u;
    uint32_t w = 32u << ((header >> 14) & 7u);
    uint32_t h = 32u << ((header >> 17) & 7u);
    int sixteen = (header >> 23) & 1u;
    uint32_t tx, ty, yy, xx;
    size_t off = 0;

    /* An entry whose payload is not exactly the texture's own size is a
     * mipmap chain, not the texture: the base arrives as type 0x01 with
     * w*h*2 bytes and the chain follows as type 0x02 with about a third of
     * that. Uploading the chain as though it were the base is what turned
     * every surface into coloured static. */
    if (!g_sheet || w > 1024u || h > 1024u) return;
    if (avail < (size_t)w * h * (sixteen ? 2u : 1u)) return;
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
                        t = (uint16_t)(src[si*2] | (src[si*2 + 1] << 8));
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
    /* Red is the top field and blue the bottom -- the opposite way round
     * to the tilemap palette.
     *
     * This was settled once on the game's green jeep, which is exactly the
     * wrong subject: swapping red and blue leaves green alone, so the jeep
     * looks right either way. Dumping the sheet to an image and looking at
     * it settles it in one glance -- Ian and Sarah's faces are natural
     * skin this way round and cyanotic the other, the dinosaur hide is
     * brown rather than blue, and the T-Rex in attract stops being a blue
     * silhouette. Use a subject with a colour that a swap can change. */
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
    uint8_t luminous;
    uint8_t r, g, b;
} tri_t;

static tri_t *g_tris;
static unsigned g_ntris;

typedef struct { int on, lum; unsigned x, y, page, w, h, fmt; } texdesc_t;

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
    t->luminous = (uint8_t)(td->lum ? 1 : 0);
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
        /* x is five bits of word 4 with its low bit borrowed from the top
         * of word 5, which looks odd and is right: reading it as six
         * contiguous bits of word 4, the way the upload header carries
         * it, drops a round from 89678 texels found to 18841. */
        td.x    = (32u * (((ph[4] & 0x1Fu) << 1)
                          | ((ph[5] >> 7) & 1u))) & 2047u;
        td.y    = (32u * (ph[5] & 0x1Fu)) & 2047u;
        /* M3_TEX_PAGE=0|1 forces the page. Every polygon in a round asks
         * for page 1 and no upload in a whole boot sets the page bit, so
         * one of the two fields is misread and this says which. */
        { static long force = -2;
          if (force == -2) { const char *e = getenv("M3_TEX_PAGE");
                             force = e ? strtol(e, NULL, 0) : -1; }
          td.page = force >= 0 ? (uint32_t)force
                               : ((ph[4] & 0x40u) >> 6); }
        /* Bit 16 of word 6 disables lighting for the polygon:
         * it is drawn at full brightness. */
        td.lum  = (ph[6] & 0x00010000u) != 0;
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

/* Which matrices a viewport actually uses. The table is sparse -- most
 * of it is zeros -- so "how many entries look valid" is not the question;
 * "which ones does the scene reach for" is. */
int m3_node_dump;
uint64_t m3_node_from;
static unsigned g_mat_loads, g_mat_max, g_mat_zero, g_mat_bigi;
static float    g_mat_bigt;

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
    tx = f32_of(rd32le(node + 4 * 4));
    ty = f32_of(rd32le(node + 5 * 4));
    tz = f32_of(rd32le(node + 6 * 4));

    /* A node carries EITHER a translation OR a matrix, and bit 4 of word
     * 0 says which. Applying both -- matrix first, then the translation
     * whenever it was non-zero -- is right for every node that has one
     * and wrong for every node that has the other, because a node with
     * the bit set still has something in its matrix field and it is not
     * a matrix index. */
    if (rd32le(node) & 0x10u) {
        mat_t t = IDENT;
        t.m[0][3] = tx; t.m[1][3] = ty; t.m[2][3] = tz;
        cur = mat_mul(m, &t);
    } else if (mo && mat_load(g_matbase, mo, &mm)) {
        unsigned k;
        float big = 0.0f;
        for (k = 0; k < 3; k++) {
            float v = mm.m[k][3] < 0 ? -mm.m[k][3] : mm.m[k][3];
            if (v > big) big = v;
        }
        g_mat_loads++;
        { static unsigned told; extern int m3_node_dump;
          extern uint64_t m3_node_from;
          if (m3_node_dump && model3recomp_frame_count() >= m3_node_from
              && told++ < (unsigned)m3_node_dump)
            fprintf(stderr, "[node] @%06X w0=%08X w3=%08X mo=%u"
                            "  |rows| %.3g %.3g %.3g  t %.4g %.4g %.4g\n",
                    addr, rd32le(node), rd32le(node + 3 * 4), mo,
                    (double)sqrtf(mm.m[0][0]*mm.m[0][0]+mm.m[0][1]*mm.m[0][1]+mm.m[0][2]*mm.m[0][2]),
                    (double)sqrtf(mm.m[1][0]*mm.m[1][0]+mm.m[1][1]*mm.m[1][1]+mm.m[1][2]*mm.m[1][2]),
                    (double)sqrtf(mm.m[2][0]*mm.m[2][0]+mm.m[2][1]*mm.m[2][1]+mm.m[2][2]*mm.m[2][2]),
                    (double)mm.m[0][3], (double)mm.m[1][3], (double)mm.m[2][3]); }
        if (mo > g_mat_max) g_mat_max = mo;
        if (big > g_mat_bigt) { g_mat_bigt = big; g_mat_bigi = mo; }
        if (mm.m[0][0] == 0.0f && mm.m[0][1] == 0.0f && mm.m[0][2] == 0.0f
         && mm.m[1][0] == 0.0f && mm.m[1][1] == 0.0f && mm.m[1][2] == 0.0f)
            g_mat_zero++;
        cur = mat_mul(m, &mm);
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

static unsigned g_cull_back, g_cull_near, g_cull_degen, g_cull_off;
static unsigned g_tex_tris, g_flat_tris, g_tex_px, g_flat_px;
static unsigned g_texel_hit, g_texel_clear, g_texel_missing;
/* Where the polygons that find nothing are looking. */
enum { MISS_SLOTS = 24 };
static struct { unsigned x, y, page, w, h, n; } g_miss[MISS_SLOTS];
static void note_miss(const tri_t *t)
{
    unsigned k;
    for (k = 0; k < MISS_SLOTS; k++) {
        if (g_miss[k].n && (g_miss[k].x != t->tx || g_miss[k].y != t->ty
                            || g_miss[k].page != t->page))
            continue;
        g_miss[k].x = t->tx; g_miss[k].y = t->ty; g_miss[k].page = t->page;
        g_miss[k].w = t->tw; g_miss[k].h = t->th; g_miss[k].n++;
        return;
    }
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

    /* Where the geometry actually is, in view space. A scene that comes
     * out empty is usually not a rasteriser problem: it is geometry that
     * never reaches the screen, and the bounds say so in one line. */
    if (getenv("M3_REAL3D_STATS") && g_ntris) {
        float lo[3], hi[3];
        unsigned i, k, behind = 0;
        for (k = 0; k < 3; k++) { lo[k] = 1e30f; hi[k] = -1e30f; }
        for (i = 0; i < g_ntris; i++) {
            unsigned j;
            for (j = 0; j < 3; j++) {
                for (k = 0; k < 3; k++) {
                    float v = g_tris[i].v[j][k];
                    if (v < lo[k]) lo[k] = v;
                    if (v > hi[k]) hi[k] = v;
                }
                if (g_tris[i].v[j][2] <= 0.0f) behind++;
            }
        }
        fprintf(stderr, "[real3d]     view bounds x %.0f..%.0f  y %.0f..%.0f"
                        "  z %.0f..%.0f  %u/%u verts at z<=0\n",
                lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], behind, g_ntris * 3u);
        g_cull_back = g_cull_near = g_cull_degen = g_cull_off = 0;
    }

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
                { g_cull_back++; continue; }
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
        if (nclip < 3) { g_cull_near++; continue; }

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
        if (d > -1e-9 && d < 1e-9) { g_cull_degen++; continue; }

        /* Flat shade as the fallback, for a polygon whose vertex normals
         * are degenerate. One-sided now that backfaces are culled: with the
         * real light the term runs from -0.72 to +0.93 across a frame, so
         * roughly two triangles in five are in shadow and the picture has
         * some range in it. Taking the absolute value, which is what this
         * did before, lit both sides equally and flattened that out. */
        lit = tr->n[0]*L[0] + tr->n[1]*L[1] + tr->n[2]*L[2];
        if (lit < 0) lit = 0;
        if (lit > 1.0f) lit = 1.0f;
        sh = tr->luminous ? intensity
                          : (AMBIENT + (1.0f - AMBIENT) * lit) * intensity;
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
        if (x1 < x0 || y1 < y0) { g_cull_off++; continue; }
        if (tr->textured) g_tex_tris++; else g_flat_tris++;

        /* M3_TRI_MAX=n: drop any triangle whose screen bounding box is
         * bigger than n pixels, and say what it was. A scene that comes
         * out as two flat blocks is a scene with two polygons in front
         * of it; this is how you find out which two. */
        {
            static long cap = -2;
            long area = (long)(x1 - x0 + 1) * (long)(y1 - y0 + 1);
            if (cap == -2) { const char *e = getenv("M3_TRI_MAX");
                             cap = e ? strtol(e, NULL, 0) : -1; }
            if (cap > 0 && area > cap) {
                static unsigned told;
                if (told++ < 24)
                    fprintf(stderr, "[real3d] huge tri %ld px  rgb %02X%02X%02X"
                            "  tex %d lum %d  z %.1f %.1f %.1f"
                            "  v0 %.0f,%.0f,%.0f\n",
                            area, tr->r, tr->g, tr->b, tr->textured,
                            tr->luminous, p[0][2], p[1][2], p[2][2],
                            tr->v[0][0], tr->v[0][1], tr->v[0][2]);
                continue;
            }
        }

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
                            if (tp > 0) { g_texel_clear++; continue; }
                            if (tp == 0) { c = t; g_texel_hit++; }
                            else { g_texel_missing++; note_miss(tr); }
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
                        s2 = tr->luminous ? intensity
                           : (AMBIENT + (1.0f - AMBIENT) * d) * intensity;
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
                    if (tr->textured) g_tex_px++; else g_flat_px++;
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
        g_textures = getenv("M3_NO_TEXTURES") == NULL;
        g_light_flip = getenv("M3_LIGHT_FLIP") != NULL;
        e = getenv("M3_AMBIENT");
        if (e) { g_ambient = (float)atof(e); g_ambient_forced = 1; }
        g_tris = (tri_t *)malloc(sizeof(tri_t) * MAX_TRIS);
        g_sheet = (uint16_t *)calloc(TEX_W * TEX_H, sizeof(uint16_t));
        g_sheet_set = (uint8_t *)calloc(TEX_W * TEX_H, 1);
        inited = 1;
    }
    if (!g_tris) return;

    flush_texture_fifo();

    /* No clear: the tilemap layers the guest put behind the 3D have
     * already been drawn into this buffer. */

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

    /* M3_TEX_DUMP=<file>, M3_TEX_DUMP_AT=<field>: write the texture
     * sheet once. It used to fire after the hundredth upload, which
     * is during the boot, and made the sheet look as though only its
     * first 256 columns were ever written. */
    if (getenv("M3_TEX_DUMP")) {
        static int done, probed;
        static uint64_t at;
        if (!probed) { const char *e = getenv("M3_TEX_DUMP_AT");
                       probed = 1;
                       at = e ? strtoull(e, NULL, 0) : 0; }
        if (!done && g_sheet && model3recomp_frame_count() >= at) {
            FILE *fp = fopen(getenv("M3_TEX_DUMP"), "wb");
            done = 1;
            if (fp) { fwrite(g_sheet, 2, (size_t)TEX_W * TEX_H, fp);
                      fclose(fp); }
        }
    }
    g_drawn = 0;
    { static int probed; if (!probed) { probed = 1;
        { const char *e = getenv("M3_NODE_DUMP");
          m3_node_dump = e ? (int)strtol(e, NULL, 0) : 0;
          e = getenv("M3_NODE_FROM");
          m3_node_from = e ? strtoull(e, NULL, 0) : 0; } } }
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

        /* Words 0x04..0x06 are the sun vector in that order. Supermodel
         * takes it as (w5, w6, w4), and both readings are unit vectors in
         * every scene dumped, so only a lit picture tells them apart: on
         * the jungle floor this order lights the tree trunks and the
         * signpost, and the other leaves them flat black. */
        light[0] = f32_of(rd32le(vp + 0x04 * 4));
        light[1] = f32_of(rd32le(vp + 0x05 * 4));
        light[2] = f32_of(rd32le(vp + 0x06 * 4));
        if (g_light_flip) { light[0] = -light[0];
            light[1] = -light[1]; light[2] = -light[2]; }
        g_vp07 = f32_of(rd32le(vp + 0x07 * 4));

        /* Ambient comes from the scene, not from a constant. Word 0x24
         * bits 8..15 over 255: this game writes 0x5A, so 0.353, where
         * the invented constant here was 0.20 and left the jungle floor
         * black. M3_AMBIENT still overrides, for comparing. */
        if (!g_ambient_forced) {
            unsigned a24 = (rd32le(vp + 0x24 * 4) >> 8) & 0xFFu;
            if (a24) g_ambient = (float)a24 / 255.0f;
        }
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
         * node matrices already carry the geometry into view space.
         *
         * M3_CAM0 puts it back for comparison: 1 as stored, 2 transposed.
         * The scene a round builds is almost entirely off screen, which
         * is what a missing axis permutation at the root of the matrix
         * stack looks like. */
        cam = IDENT;
        {
            static long which = -2;
            if (which == -2) { const char *e = getenv("M3_CAM0");
                               which = e ? strtol(e, NULL, 0) : 0; }
            if (which == 1) mat_load(g_matbase, 0, &cam);
            else if (which == 2) {
                mat_t t; int r, c;
                if (mat_load(g_matbase, 0, &t)) {
                    for (r = 0; r < 3; r++)
                        for (c = 0; c < 3; c++) cam.m[r][c] = t.m[c][r];
                    for (r = 0; r < 3; r++) cam.m[r][3] = t.m[r][3];
                }
            }
        }

        intensity = f32_of(rd32le(vp + 0x07 * 4));
        if (!(intensity >= 0.0f)) intensity = 1.0f;   /* NaN guard */
        if (intensity > 2.0f) intensity = 2.0f;

        g_ntris = 0;
        {
            /* M3_VP=n draws only viewport n. Three of them share the
             * screen in a round and each gets its own depth buffer, so
             * whichever is walked last wins wherever it draws -- and
             * the only way to tell which one is painting over the
             * scene is to draw them one at a time. */
            static long only = -2;
            unsigned before = g_drawn;
            if (only == -2) { const char *e = getenv("M3_VP");
                              only = e ? strtol(e, NULL, 0) : -1; }
            if (only < 0 || (long)guard == only) {
                descend(rd32le(vp + 0x02 * 4) & 0x00FFFFFFu, &cam, 0);
                raster(fb, fw, fh, sx, sy, light, intensity);
            }
            if (getenv("M3_REAL3D_STATS"))
                fprintf(stderr, "[real3d]   vp%u @%06X rect %08X/%08X"
                                "  %u tris  %u px  back %u near %u"
                                "  degen %u off %u\n",
                        guard, vpaddr & 0x00FFFFFFu,
                        rd32le(vp + 0x14 * 4), rd32le(vp + 0x1A * 4),
                        g_ntris, g_drawn - before,
                        g_cull_back, g_cull_near, g_cull_degen, g_cull_off);
            if (getenv("M3_REAL3D_STATS"))
                fprintf(stderr, "[real3d]     textured %u tris / %u px,"
                                "  flat %u tris / %u px\n",
                        g_tex_tris, g_tex_px, g_flat_tris, g_flat_px);
            if (getenv("M3_REAL3D_STATS"))
                fprintf(stderr, "[real3d]     texels: %u from the sheet,"
                                "  %u transparent,  %u never uploaded\n",
                        g_texel_hit, g_texel_clear, g_texel_missing);
            if (getenv("M3_REAL3D_STATS")) {
                unsigned k;
                for (k = 0; k < MISS_SLOTS; k++)
                    if (g_miss[k].n)
                        fprintf(stderr, "[real3d]       miss at %4u,%4u"
                                        " page %u  %ux%u  x%u\n",
                                g_miss[k].x, g_miss[k].y, g_miss[k].page,
                                g_miss[k].w, g_miss[k].h, g_miss[k].n);
                memset(g_miss, 0, sizeof g_miss);
            }
            g_texel_hit = g_texel_clear = g_texel_missing = 0;
            g_tex_tris = g_flat_tris = g_tex_px = g_flat_px = 0;
            if (getenv("M3_REAL3D_STATS"))
                fprintf(stderr, "[real3d]     matrices: %u loads, max index"
                                " %u, %u all-zero, biggest |t| %.0f at %u\n",
                        g_mat_loads, g_mat_max, g_mat_zero, g_mat_bigt,
                        g_mat_bigi);
            g_mat_loads = g_mat_max = g_mat_zero = g_mat_bigi = 0;
            g_mat_bigt = 0.0f;
        }
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
