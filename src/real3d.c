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
#include "model3recomp/savestate.h"
#include "model3recomp/bus.h"
#include "model3recomp/platform.h"
#include "model3recomp/model3recomp.h"

#include <stdio.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TRIS   40000u
#define SINK_MAX_TRIS 6144u      /* 600 KB of sink_tri_t; an attract viewport decodes ~4 750 */
static unsigned g_max_tris = MAX_TRIS;
#define MAX_DEPTH  24
#define AMBIENT    g_ambient

/* The vertex fixed-point scale: the 24-bit coordinate field over 2048.
 *
 * It was 256 for a long time, on a check that looked sound: a frame of
 * attract mode resolved into rocks and a cliff face at 1/256 and into
 * nothing recognisable at 1/16 or 1/1024. That check was run while the
 * matrix stack push was being lifted into unreachable code (see
 * tools/ppc_lifter.py), so every matrix in the scene carried the previous
 * one's scale. A wrong scale in the matrices and a wrong scale here
 * cancelled, and the frame looked right.
 *
 * With the push fixed the scale could be measured at all, and "the frame
 * looks like a scene" put it at 4096. That is a weak test: a scene looks
 * plausible over a wide range, and the range is wide because everything
 * in it scales together.
 *
 * A model either joins up or it does not, and that is a sharp test. Only
 * the vertices scale here -- the node translations that place a model's
 * parts are floats in world units and do not -- so at the wrong scale a
 * jointed model comes apart. The human figure and the jeep beside it on
 * the jungle floor are whole at 2048, spaced out at 4096 and scattered
 * into confetti at 8192. The T-Rex in attract survives any of them
 * because it is one rigid model, which is why it never showed the fault.
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
#define VERTEX_SCALE_DEFAULT 2048.0f

static float g_vertex_scale = VERTEX_SCALE_DEFAULT;
static float g_vertex_inv = 1.0f / VERTEX_SCALE_DEFAULT;
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
static int g_old_proj;      /* M3_OLD_PROJ: the symmetric whole-screen projection */
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
static const uint8_t *cull_at(uint32_t addr, unsigned words, uint8_t *buf)
{
    int region;
    size_t base;

    addr &= 0x00FFFFFFu;
    if (addr >= 0x800000u && addr < 0x840000u) {
        region = 1;
        base = (size_t)(addr & 0x3FFFFu) * 4u;
    } else if (addr < 0x100000u) {
        region = 0;
        base = (size_t)addr * 4u;
    } else {
        return NULL;
    }
    /* Copied rather than pointed at: on a Dreamcast these memories are
     * paged and have no flat copy (bus_r3d_read). */
    if (bus_r3d_read(region, base, buf, (size_t)words * 4u)
        != (size_t)words * 4u)
        return NULL;
    return buf;
}

static float f32_of(uint32_t w)
{
    float f;
    memcpy(&f, &w, sizeof f);
    return f;
}

/* Sign-extend the top 24 bits of a word: an arithmetic shift, which GCC
 * and Clang give a signed right shift (two instructions on the SH-4, where
 * the test and the OR were a branch and four). */
static int32_t sx24(uint32_t w)
{
    return (int32_t)w >> 8;
}

/* ---- matrices ---------------------------------------------------------- */
typedef struct { float m[3][4]; } mat_t;

static const mat_t IDENT = {{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};

static int mat_load(uint32_t base, uint32_t off, mat_t *out)
{
    uint8_t pb_[12 * 4];
    const uint8_t *p = cull_at(base + off * 12u, 12, pb_);
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
/* The sheet may be kept at reduced size (real3d_set_sheet, M3_SHEET_SHIFT
 * on a desktop to see what that looks like): 1 << g_sheet_shift texels of
 * the board's per stored texel, each way, point sampled. */
static unsigned g_sheet_shift;
static const m3_r3d_sink *g_sink;      /* real3d_set_sink */
#define SHEET_AT(X, Y) ((size_t)((Y) >> g_sheet_shift) * (TEX_W >> g_sheet_shift) \
                        + ((X) >> g_sheet_shift))
static uint8_t  *g_sheet_set;          /* TEX_W * TEX_H texels */
static size_t    g_fifo_seen;      /* how much of the FIFO has been drained */
static unsigned  g_tex_uploads;

/* Set while a VROM texture is being loaded: that source is big-endian,
 * the FIFO is not. */
static int g_src_be;

static uint64_t g_up_texels[2], g_up_count[2];   /* FIFO, VROM port */
static void upload_texture(uint32_t header, const uint8_t *src, size_t avail)
{
    /* The origin and size of an upload. Dumping the sheet to a PNG shows
     * a coherent atlas -- jeeps, Ian and Sarah, dinosaur hide, INGEN
     * signage -- so these fields are right, and the payload length agrees
     * with w*h*2 on every accepted entry, which is what pins the two size
     * exponents.
     *
     * The page is bit 20, and the reason it is never set in a FIFO
     * header is that the FIFO only ever fills the lower half. The upper
     * half comes from the VROM texture port; see real3d_vrom_texture().
     * Before that was found, bit 6 (set in 460 of 464 FIFO uploads), bit
     * 5, and reading y as six bits instead of five-plus-a-page were all
     * tried, and each fixed one scene by breaking the other. */
    uint32_t x = 32u * (header & 0x3Fu);
    uint32_t y = 32u * ((header >> 7) & 0x1Fu);
    uint32_t page = (header >> 20) & 1u;
    uint32_t w = 32u << ((header >> 14) & 7u);
    uint32_t h = 32u << ((header >> 17) & 7u);
    int sixteen = (header >> 23) & 1u;
    /* An 8-bit upload writes one byte of each 16-bit texel: bit 21 the low
     * byte, bit 22 the high, so two 8-bit textures can share one spot of
     * the sheet. Neither bit means nothing is written. */
    unsigned bytesel = ((header >> 21) & 1u) | (((header >> 22) & 1u) << 1);
    static const uint16_t keep[4] = { 0xFFFFu, 0xFF00u, 0x00FFu, 0x0000u };
    uint32_t tx, ty, yy, xx;
    size_t off = 0;

    /* An entry whose payload is not exactly the texture's own size is a
     * mipmap chain, not the texture: the base arrives as type 0x01 with
     * w*h*2 bytes and the chain follows as type 0x02 with about a third of
     * that. Uploading the chain as though it were the base is what turned
     * every surface into coloured static. */
    if (!g_sheet || w > 1024u || h > 1024u) return;
    /* The top byte says what the payload is: 0x00 a texture and its mip
     * chain, 0x01 the texture alone, 0x02 the chain alone (0x80 is a gamma
     * table). The size check below only screens FIFO entries -- a VROM load
     * has the rest of the ROM after it -- so without this a mip-only load
     * from VROM was written over the texture it belongs to. Mips are not
     * sampled, so only the base level is stored. */
    if ((header >> 24) > 1u) return;
    if (avail < (size_t)w * h * (sixteen ? 2u : 1u)) return;
    y += page * 1024u;                      /* two 2048x1024 pages, stacked */
    if (x + w > TEX_W || y + h > TEX_H) return;

    for (ty = 0; ty < h; ty += 8u) {
        for (tx = 0; tx < w; tx += 8u) {
            for (yy = 0; yy < 8u; yy++) {
                for (xx = 0; xx < 8u; xx++) {
                    size_t o;
                    size_t si;
                    uint16_t t;
                    if (g_sheet_shift && (((x + tx + xx) | (y + ty + yy))
                                          & ((1u << g_sheet_shift) - 1u)))
                        continue;            /* not kept at this size */
                    o = SHEET_AT(x + tx + xx, y + ty + yy);
                    if (sixteen) {
                        si = off + DETILE[yy * 8u + xx];
                        if ((si + 1) * 2 > avail) continue;
                        t = g_src_be
                          ? (uint16_t)((src[si*2] << 8) | src[si*2 + 1])
                          : (uint16_t)(src[si*2] | (src[si*2 + 1] << 8));
                    } else {
                        /* Supermodel's 8-bit path: the same tile map with
                         * its rows and columns paired the other way, two
                         * texels to a 16-bit word, even column in the high
                         * byte. A tile is 32 words rather than 64. */
                        unsigned idx = ((yy ^ 1u) * 8u) + (xx ^ 1u);
                        unsigned sh = 8u * ((xx & 1u) ^ 1u);
                        uint16_t w;
                        if (!bytesel) continue;
                        si = off / 2u + DETILE[idx] / 2u;
                        if ((si + 1) * 2 > avail) continue;
                        w = g_src_be
                          ? (uint16_t)((src[si*2] << 8) | src[si*2 + 1])
                          : (uint16_t)(src[si*2] | (src[si*2 + 1] << 8));
                        t = (uint16_t)((w >> sh) & 0xFFu);
                        t = (uint16_t)((g_sheet[o] & keep[bytesel])
                                       | ((t | (t << 8)) & (uint16_t)~keep[bytesel]));
                    }
                    g_sheet[o] = t;
                    if (g_sheet_set) g_sheet_set[o] = 1;
                }
            }
            off += 64;
        }
    }
    g_tex_uploads++;
    if (g_sink && g_sink->sheet_changed) g_sink->sheet_changed();
    g_up_texels[g_src_be ? 1 : 0] += (uint64_t)w * h;
    g_up_count[g_src_be ? 1 : 0]++;
}

/* The VROM texture port at 0x90000000.
 *
 * Three registers. The game writes an address and a header and the third
 * write loads a texture that is already in VROM, rather than pushing its
 * pixels through the FIFO. A stage's scenery comes this way: in a round
 * the FIFO stops growing at field 1500 and every terrain and tree polygon
 * then looks for a texture that was never uploaded.
 *
 * Which register is which is worked out from what the game writes; see
 * M3_VROMTEX_TRACE. */
static uint32_t bswap32_(uint32_t v)
{
    return ((v >> 24) & 0xFFu) | ((v >> 8) & 0xFF00u)
         | ((v << 8) & 0xFF0000u) | ((v << 24) & 0xFF000000u);
}

void real3d_vrom_texture(uint32_t addr, uint32_t header, uint32_t ctrl)
{
    size_t n = 0;
    const uint8_t *v = bus_vrom(&n);
    /* The guest writes this port byte-reversed, the way it writes every
     * other Real3D register, and the address counts 16-bit texels. Read
     * as bytes, or little-endian, the sheet fills with noise; this way
     * it fills with planks, gravel, foliage and rock. */
    uint32_t a = bswap32_(addr), h = bswap32_(header);
    size_t off;
    if (getenv("M3_VROMTEX_TRACE"))
        fprintf(stderr, "[vromtex] load addr %08X header %08X ctrl %08X\n",
                a, h, bswap32_(ctrl));
    if (!n || !g_sheet) return;
    /* The address counts 32-bit words, like a model pointer: consecutive
     * loads step by exactly one texture's size in words. The image is 32 MB
     * and the addresses reach past it, so it is mirrored -- the way bit 23
     * of a model address selects VROM and is then dropped. */
    off = ((size_t)a * 4u) % n;
    g_src_be = 1;                  /* VROM is big-endian; the FIFO is not */
    if (v) {
        upload_texture(h, v + off, n - off);
    } else {
        /* VROM streamed (m3_roms_t.read): copy the base level out. */
        size_t want = (size_t)(32u << ((h >> 14) & 7u))
                    * (size_t)(32u << ((h >> 17) & 7u)) * 2u;
        uint8_t *buf = malloc(want);
        if (buf) {
            size_t got = bus_r3d_read(3, off, buf, want);
            upload_texture(h, buf, got);
            free(buf);
        }
    }
    g_src_be = 0;
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
        if (getenv("M3_TEX_SAVE")) {        /* each payload, for tracing */
            static unsigned saved;
            char path[512];
            FILE *fp;
            snprintf(path, sizeof path, "%s/up%03u_%08X.bin",
                     getenv("M3_TEX_SAVE"), saved++, header);
            if ((fp = fopen(path, "wb"))) {
                fwrite(f + i + 8, 1, (size_t)words * 4u - 8u, fp);
                fclose(fp);
            }
        }
        upload_texture(header, f + i + 8, (size_t)words * 4u - 8u);
        i += (size_t)words * 4u;
    }
    /* What was uploaded leaves the FIFO, as it does on the board. Keeping
     * it -- which this did -- re-uploaded every texture ever sent on every
     * field, and once the 4 MB buffer filled, every texture sent after
     * that was dropped: Scud Race's attract race showed its later
     * textures as white rectangles. */
    bus_texfifo_consume(i);
    g_fifo_seen = 0;
    if (getenv("M3_TEX_TRACE"))
        fprintf(stderr, "[tex] fifo %u bytes, %u uploads so far%s",
                (unsigned)used, g_tex_uploads, "\n");
}

/* The raw 16-bit texel, or -1 where nothing was ever uploaded. */
static int sheet_word(unsigned x, unsigned y)
{
    size_t o;
    if (!g_sheet) return -1;
    o = SHEET_AT(x & (TEX_W - 1u), y & (TEX_H - 1u));
    if (g_sheet_set && !g_sheet_set[o]) return -1;
    return g_sheet[o];
}

/* A texel in one of the twelve formats (Supermodel's ExtractColour), as
 * 0xAARRGGBB. Format 0 is 1-5-5-5 with red the top field -- the opposite
 * way round to the tilemap palette, which a green jeep cannot show and
 * Ian and Sarah's faces can. The rest are greyscale with alpha packed in
 * one byte or nibble of the texel: 1-4 four-bit luminance and alpha, 5-6
 * eight-bit luminance with full white transparent, 7 RGBA4, 8-11 four-bit
 * luminance with the top value transparent. */
uint32_t real3d_texel_argb(unsigned fmt, unsigned v)
{
    unsigned r, g, b, a = 255;
    switch (fmt) {
    case 0:
        r = (v >> 10) & 0x1Fu; g = (v >> 5) & 0x1Fu; b = v & 0x1Fu;
        r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
        a = (v & 0x8000u) ? 0 : 255;
        return (a << 24) | (r << 16) | (g << 8) | b;
    case 1: r = (v & 0xFu) * 17u;         a = ((v >> 4) & 0xFu) * 17u;  break;
    case 2: r = ((v >> 4) & 0xFu) * 17u;  a = (v & 0xFu) * 17u;         break;
    case 3: r = ((v >> 8) & 0xFu) * 17u;  a = ((v >> 12) & 0xFu) * 17u; break;
    case 4: r = ((v >> 12) & 0xFu) * 17u; a = ((v >> 8) & 0xFu) * 17u;  break;
    case 5: r = v & 0xFFu;          a = r == 255u ? 0 : 255; break;
    case 6: r = (v >> 8) & 0xFFu;   a = r == 255u ? 0 : 255; break;
    case 7:
        r = ((v >> 12) & 0xFu) * 17u; g = ((v >> 8) & 0xFu) * 17u;
        b = ((v >> 4) & 0xFu) * 17u;  a = (v & 0xFu) * 17u;
        return (a << 24) | (r << 16) | (g << 8) | b;
    case 8:  r = (v & 0xFu) * 17u;         a = r == 255u ? 0 : 255; break;
    case 9:  r = ((v >> 4) & 0xFu) * 17u;  a = r == 255u ? 0 : 255; break;
    case 10: r = ((v >> 8) & 0xFu) * 17u;  a = r == 255u ? 0 : 255; break;
    default: r = ((v >> 12) & 0xFu) * 17u; a = r == 255u ? 0 : 255; break;
    }
    return (a << 24) | (r << 16) | (r << 8) | r;
}

/* One texel coordinate, wrapped: a mirrored axis runs forwards then
 * backwards, so odd repeats are reflected. */
static unsigned wrap_axis(long i, unsigned size, int mirror)
{
    /* Texture sizes are 32 << n, so a mask wraps -- two's complement makes
     * it right for negative coordinates too. */
    unsigned u = (unsigned)i, m = size - 1u;
    if (mirror && (u & size))
        return m - (u & m);
    return u & m;
}

/* Bilinear, the way Supermodel samples (the hardware filters too). Each
 * texel's colour is first taken from whichever neighbour is more opaque,
 * so a cut-out's transparent texels do not bleed dark into its edge.
 * Returns -1 when the texture was never uploaded. */
typedef struct tri_s tri_t;
static int64_t sample(const tri_t *tr, double u, double v);

/* ---- the triangle queue ------------------------------------------------ */
struct tri_s {
    union {
        struct {
            float v[3][3];  /* view space */
            float vn[3][3]; /* per-vertex normals, for Gouraud */
            float uv[3][2]; /* texel coordinates */
        };
        /* The sink path, once sink_bounds has run: the polygon as the sink
         * is handed it (nproj vertices, 0 when culled or cut away), in the
         * place of the three above, which it no longer reads. Projecting
         * once and keeping it here costs no memory; a table of its own
         * would be ~300 KB. */
        m3_r3d_vertex proj[4];
    };
    float n[3];             /* the polygon's own normal, from the header */
    uint16_t tx, ty;        /* texture origin in the sheet */
    uint16_t tw, th;        /* its size, for wrapping */
    uint8_t page;
    uint8_t textured;
    uint8_t luminous;
    uint8_t two_sided;
    uint8_t alpha;          /* 255 opaque */
    uint8_t fmt;            /* texel format, 0..11 */
    uint8_t alpha_test;     /* contour: drop low-alpha texels */
    uint8_t tex_alpha;      /* texel alpha makes it see-through */
    uint8_t mirror;         /* bit 0 mirror v, bit 1 mirror u (header word 2) */
    uint8_t r, g, b;
    uint8_t nproj;
};

static tri_t *g_tris;

static int64_t sample(const tri_t *tr, double u, double v)
{
    unsigned tw = tr->tw ? tr->tw : 32u, th = tr->th ? tr->th : 32u;
    double fu = u - 0.5, fv = v - 0.5;
    double flu = floor(fu), flv = floor(fv);
    long u0 = (long)flu, v0 = (long)flv;
    /* Weights in 8.8 fixed point: the sum of four products fits easily,
     * and it is a good deal cheaper than doing this in doubles per pixel. */
    unsigned wa = (unsigned)((fu - flu) * 256.0), wb = (unsigned)((fv - flv) * 256.0);
    unsigned w[4], all, k, c;
    uint32_t px[4], out;
    unsigned x0 = wrap_axis(u0, tw, tr->mirror & 2u), x1 = wrap_axis(u0 + 1, tw, tr->mirror & 2u);
    unsigned y0 = wrap_axis(v0, th, tr->mirror & 1u), y1 = wrap_axis(v0 + 1, th, tr->mirror & 1u);
    unsigned base = tr->page * 1024u;
    int t;

    t = sheet_word(tr->tx + x0, ((tr->ty + y0) & 1023u) + base); if (t < 0) return -1; px[0] = real3d_texel_argb(tr->fmt, (unsigned)t);
    t = sheet_word(tr->tx + x1, ((tr->ty + y0) & 1023u) + base); if (t < 0) return -1; px[1] = real3d_texel_argb(tr->fmt, (unsigned)t);
    t = sheet_word(tr->tx + x0, ((tr->ty + y1) & 1023u) + base); if (t < 0) return -1; px[2] = real3d_texel_argb(tr->fmt, (unsigned)t);
    t = sheet_word(tr->tx + x1, ((tr->ty + y1) & 1023u) + base); if (t < 0) return -1; px[3] = real3d_texel_argb(tr->fmt, (unsigned)t);

    /* Where the four are not all opaque, each takes its colour from the
     * most opaque of them, so a cut-out's clear texels do not bleed dark
     * into its edge. */
    all = px[0] & px[1] & px[2] & px[3];
    if ((all >> 24) != 255u) {
        unsigned best = 0;
        for (k = 1; k < 4; k++) if ((px[k] >> 24) > (px[best] >> 24)) best = k;
        for (k = 0; k < 4; k++)
            if ((px[k] >> 24) < (px[best] >> 24))
                px[k] = (px[k] & 0xFF000000u) | (px[best] & 0x00FFFFFFu);
    }
    w[0] = (256u - wa) * (256u - wb); w[1] = wa * (256u - wb);
    w[2] = (256u - wa) * wb;          w[3] = wa * wb;
    out = 0;
    for (c = 0; c < 32u; c += 8u) {
        unsigned acc = 0;
        for (k = 0; k < 4; k++) acc += w[k] * ((px[k] >> c) & 0xFFu);
        out |= ((acc + 32768u) >> 16) << c;
    }
    return (int64_t)out;
}
static unsigned g_ntris;

typedef struct { int on, lum, two_sided, alpha, alpha_test, tex_alpha, mirror; unsigned x, y, page, w, h, fmt; } texdesc_t;

/* The hardware sink never needs the software rasteriser's vertex normals or
 * its view-space vertices after projection. Keep a separate compact queue so
 * drawing a model does not stride over 132-byte software triangles, then
 * rewrite most of each one with projected vertices in sink_bounds. */
typedef struct {
    m3_r3d_vertex proj[4];
    uint32_t argb;
    uint16_t tx, ty;
    uint16_t tw, th;
    uint8_t page;
    uint8_t textured;
    uint8_t fmt;
    uint8_t alpha_test;
    uint8_t tex_alpha;
    uint8_t mirror;
    uint8_t nproj;
} sink_tri_t;

static sink_tri_t *g_sink_tris;
static float g_sink_light[3], g_sink_intensity;

/* A model's vertex on the sink path: as read (model space, texture
 * coordinates), then in view space once transformed (wok) and on the screen
 * once projected (pok). draw_model keeps four and names them by index, so a
 * vertex the next polygon reuses is neither copied nor transformed nor
 * projected again: the same model, the same matrix, the same projection. */
typedef struct {
    float vs[3], uv[2], wv[3];
    m3_r3d_vertex pj;
    uint8_t wok, pok;
} mvert_t;

static void sink_polygon(mvert_t *pool, const uint8_t cur[4], unsigned nv,
                         const float n[3], uint32_t rgb, const texdesc_t *td);

static void emit(const float a[3], const float b[3], const float c[3],
                 const float n[3], uint32_t rgb,
                 const float na[3], const float nb[3], const float nc[3],
                 const float ua[2], const float ub[2], const float uc[2],
                 const texdesc_t *td)
{
    tri_t *t;
    int k;
    if (g_ntris >= g_max_tris) return;
    t = &g_tris[g_ntris++];
    /* Element by element: SH-4 GCC calls memcpy for these, and a call per
     * 12 bytes was most of a race's time in memcpy. */
    for (k = 0; k < 3; k++) {
        t->v[0][k] = a[k]; t->v[1][k] = b[k]; t->v[2][k] = c[k];
        t->n[k] = n[k];
    }
    for (k = 0; k < 3; k++) {
        t->vn[0][k] = na[k]; t->vn[1][k] = nb[k]; t->vn[2][k] = nc[k];
    }
    for (k = 0; k < 2; k++) {
        t->uv[0][k] = ua[k]; t->uv[1][k] = ub[k]; t->uv[2][k] = uc[k];
    }
    t->textured = (uint8_t)(td->on ? 1 : 0);
    t->luminous = (uint8_t)(td->lum ? 1 : 0);
    t->two_sided = (uint8_t)(td->two_sided ? 1 : 0);
    t->alpha = (uint8_t)td->alpha;
    t->fmt = (uint8_t)td->fmt;
    t->alpha_test = (uint8_t)td->alpha_test;
    t->tex_alpha = (uint8_t)td->tex_alpha;
    t->mirror = (uint8_t)td->mirror;
    t->tx = (uint16_t)td->x; t->ty = (uint16_t)td->y;
    t->tw = (uint16_t)td->w; t->th = (uint16_t)td->h;
    t->page = (uint8_t)td->page;
    t->r = (uint8_t)((rgb >> 16) & 0xFF);
    t->g = (uint8_t)((rgb >> 8) & 0xFF);
    t->b = (uint8_t)(rgb & 0xFF);
}

/* ---- models ------------------------------------------------------------ */
/* Texture offset inherited down the node tree (node word 2): added to every
 * polygon's texture origin below it, and its page bit flips the polygon's.
 * The same model is drawn with different skins this way. */
static unsigned g_texoff_x, g_texoff_y, g_texoff_page;
static uint32_t g_ctab;     /* colour table, a word index into polygon RAM */

/* A word of a polygon, at a 4-byte aligned address (every model is, and so
 * are the pages and the copy buffers): one load, swapped for VROM, where
 * draw_model's loop used to call rd32be or rd32le through a pointer for
 * four byte loads. Both hosts are little-endian. */
typedef uint32_t __attribute__((may_alias)) u32_alias;
static inline uint32_t poly_word(const uint8_t *p, int be)
{
    uint32_t w = *(const u32_alias *)(const void *)p;
    return be ? __builtin_bswap32(w) : w;
}

/* len bytes of a Real3D memory: in place when they lie in one page, else
 * copied into buf. NULL when the range runs off the end. */
static const uint8_t *r3d_get(int region, size_t off, uint32_t *buf,
                              size_t len)
{
    const uint8_t *p = bus_r3d_span(region, off, len);
    if (p) return p;
    return bus_r3d_read(region, off, buf, len) == len
         ? (const uint8_t *)buf : NULL;
}

static void draw_model(uint32_t addr, const mat_t *m)
{
    int region;
    uint8_t pb[(7u + 16u) * 4u];        /* M3_MODEL_DUMP's reads */
    uint32_t hbuf[7], vbuf[16];         /* a header, four vertices: copied
                                         * only across a page boundary */
    uint32_t (*rd)(const uint8_t *);
    size_t i;
    float prev[4][3], prevn[4][3], prevuv[4][2];
    mvert_t pool[4];                    /* the sink's vertices (mvert_t) */
    uint8_t pprev[4];                   /* the previous polygon's, in pool */
    unsigned nprev = 0;
    unsigned guard;

    /* Read a polygon at a time through bus_r3d_read: on a Dreamcast
     * polygon RAM is paged and VROM is streamed off the disc. */
    if (addr & 0x800000u) {
        region = 3;
        rd = rd32be;                    /* VROM alone is read big-endian */
        i = (size_t)(addr & 0x7FFFFFu) * 4u;
    } else {
        region = 2;
        rd = rd32le;
        i = (size_t)addr * 4u;
    }

    /* M3_MODEL_DUMP=<n>: the first n models drawn, with where they live,
     * how many polygons they have, the biggest raw coordinate in them and
     * the OR and AND of every polygon header word. Two models that want
     * different vertex scales differ in a header bit, and this is what
     * shows which. */
    {
        static long cap = -2;
        static unsigned told;
        static uint64_t m3_model_from;
        if (cap == -2) { const char *e = getenv("M3_MODEL_DUMP");
                         const char *f2 = getenv("M3_NODE_FROM");
                         m3_model_from = f2 ? strtoull(f2, NULL, 0) : 0;
                         cap = e ? strtol(e, NULL, 0) : 0; }
        if (cap > 0 && told < (unsigned)cap
            && model3recomp_frame_count() >= m3_model_from) {
            uint32_t hor[7], han[7];
            long biggest = 0;
            unsigned polys = 0, kk, g2;
            size_t j2 = i;
            for (kk = 0; kk < 7; kk++) { hor[kk] = 0; han[kk] = 0xFFFFFFFFu; }
            for (g2 = 0; g2 < 20000u; g2++) {
                uint32_t h2[7]; unsigned nv2, new2, q;
                if (bus_r3d_read(region, j2, pb, 28u) != 28u) break;
                for (kk = 0; kk < 7; kk++) h2[kk] = rd(pb + kk * 4);
                { float nn[3]; float l2;
                  for (kk = 0; kk < 3; kk++)
                      nn[kk] = (float)sx24(h2[kk + 1]) / 4194304.0f;
                  l2 = nn[0]*nn[0] + nn[1]*nn[1] + nn[2]*nn[2];
                  if (l2 < 0.98f || l2 > 1.02f) break; }
                for (kk = 0; kk < 7; kk++) { hor[kk] |= h2[kk]; han[kk] &= h2[kk]; }
                nv2 = (h2[0] & 0x40u) ? 4u : 3u;
                new2 = nv2;
                for (kk = 0; kk < 4; kk++) if ((h2[0] >> kk) & 1u) new2--;
                for (q = 0; q < new2; q++) {
                    size_t o2 = j2 + 28u + (size_t)q * 16u;
                    unsigned c2;
                    if (bus_r3d_read(region, o2, pb, 12u) != 12u) break;
                    for (c2 = 0; c2 < 3; c2++) {
                        long v2 = sx24(rd(pb + c2 * 4));
                        if (v2 < 0) v2 = -v2;
                        if (v2 > biggest) biggest = v2;
                    }
                }
                j2 += 28u + (size_t)new2 * 16u;
                polys++;
                if (h2[1] & 4u) break;
            }
            told++;
            fprintf(stderr, "[model] @%06X %4u polys  biggest raw %8ld"
                            "  (%.2f units at 1/%g)\n",
                    addr, polys, biggest,
                    (double)biggest / (double)g_vertex_scale,
                    (double)g_vertex_scale);
            fprintf(stderr, "          placed at %.1f %.1f %.1f\n",
                    (double)m->m[0][3], (double)m->m[1][3],
                    (double)m->m[2][3]);
            fprintf(stderr, "          hdr or  %08X %08X %08X %08X %08X %08X %08X\n",
                    hor[0], hor[1], hor[2], hor[3], hor[4], hor[5], hor[6]);
            fprintf(stderr, "          hdr and %08X %08X %08X %08X %08X %08X %08X\n",
                    han[0], han[1], han[2], han[3], han[4], han[5], han[6]);
        }
    }

    for (guard = 0; guard < 20000u; guard++) {
        uint32_t ph[7];
        float nrm[3], vs[4][3], vn[4][3], uv[4][2];
        uint8_t cur[4];                 /* this polygon's, in pool (sink) */
        texdesc_t td;
        unsigned nv, nnew, k, j, slot;
        uint32_t rgb;
        float len;
        const uint8_t *hp, *vp = NULL;

        /* The header is decoded before the vertices are fetched: fetching
         * them can evict the VROM piece it points into. */
        if (!(hp = r3d_get(region, i, hbuf, 28u))) return;
        for (k = 0; k < 7; k++) ph[k] = poly_word(hp + k * 4, region == 3);

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
        if (g_sink) {
            unsigned busy = 0;
            for (k = 0; k < 4; k++)         /* bits 0..3: reuse from previous */
                if (((ph[0] >> k) & 1u) && k < nprev && slot < 4) {
                    cur[slot++] = pprev[k];
                    busy |= 1u << pprev[k];
                }
            if (slot > nv) return;
            nnew = nv - slot;
            if (nnew && !(vp = r3d_get(region, i + 28u, vbuf, nnew * 16u)))
                return;
            for (k = 0; k < nnew && slot < 4; k++, slot++) {
                const uint8_t *o = vp + 16u * k;
                unsigned e = 0;
                mvert_t *v;
                uint32_t wt;
                float us;
                while ((busy >> e) & 1u) e++;   /* fewer than four busy */
                busy |= 1u << e;
                cur[slot] = (uint8_t)e;
                v = &pool[e];
                v->vs[0] = (float)sx24(poly_word(o, region == 3)) * g_vertex_inv;
                v->vs[1] = (float)sx24(poly_word(o + 4, region == 3)) * g_vertex_inv;
                v->vs[2] = (float)sx24(poly_word(o + 8, region == 3)) * g_vertex_inv;
                wt = poly_word(o + 12, region == 3);
                us = (ph[1] & 0x40u) ? 1.0f : 0.125f;
                v->uv[0] = (float)((wt >> 16) & 0xFFFFu) * us;
                v->uv[1] = (float)(wt & 0xFFFFu) * us;
                v->wok = v->pok = 0;
            }
            goto decoded;
        }
        for (k = 0; k < 4; k++)             /* bits 0..3: reuse from previous */
            if (((ph[0] >> k) & 1u) && k < nprev && slot < 4) {
                for (j = 0; j < 3; j++) {
                    vs[slot][j] = prev[k][j];
                    vn[slot][j] = prevn[k][j];
                }
                uv[slot][0] = prevuv[k][0]; uv[slot][1] = prevuv[k][1];
                slot++;
            }
        if (slot > nv) return;
        nnew = nv - slot;
        if (nnew && !(vp = r3d_get(region, i + 28u, vbuf, nnew * 16u)))
            return;

        for (k = 0; k < nnew && slot < 4; k++, slot++) {
            const uint8_t *o = vp + 16u * k;
            uint32_t wx = poly_word(o, region == 3);
            uint32_t wy = poly_word(o + 4, region == 3);
            uint32_t wz = poly_word(o + 8, region == 3);
            /* Times the reciprocal: the same float as the division while
             * the scale is a power of two, as the default 2048 is. */
            vs[slot][0] = (float)sx24(wx) * g_vertex_inv;
            vs[slot][1] = (float)sx24(wy) * g_vertex_inv;
            vs[slot][2] = (float)sx24(wz) * g_vertex_inv;
            /* The low byte of each coordinate word is that vertex's own
             * normal, as a signed byte. They are not unit length -- the
             * lengths run from 0 to 127 -- but they do sit a median 25
             * degrees from the face normal, against the 90 that random
             * directions would give, which is what identifies them. */
            vn[slot][0] = (float)(int8_t)(wx & 0xFFu);
            vn[slot][1] = (float)(int8_t)(wy & 0xFFu);
            vn[slot][2] = (float)(int8_t)(wz & 0xFFu);
            {
                /* Word 3 of a vertex is u in the high half and v in the low,
                 * in eighths of a texel -- or whole texels when bit 6 of
                 * header word 1 is set (Supermodel's UVScale). */
                uint32_t wt = poly_word(o + 12, region == 3);
                float us = (ph[1] & 0x40u) ? 1.0f : 0.125f;
                uv[slot][0] = (float)((wt >> 16) & 0xFFFFu) * us;
                uv[slot][1] = (float)(wt & 0xFFFFu) * us;
            }
        }
    decoded:

        rgb = (((ph[4] >> 24) & 0xFFu) << 16) | (((ph[4] >> 16) & 0xFFu) << 8)
            | ((ph[4] >> 8) & 0xFFu);
        /* Bit 1 of word 1 clear: the colour is an index into a table in
         * polygon RAM that a culling node above set up. */
        if (!(ph[1] & 2u)) {
            uint8_t cb[4];
            size_t co = ((size_t)g_ctab + ((ph[4] >> 8) & 0xFFFu)) * 4u;
            if (bus_r3d_read(2, co, cb, 4u) == 4u)
                rgb = rd32le(cb) & 0xFFFFFFu;
        }

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
        td.alpha_test = (ph[6] & 0x80000000u) != 0;
        td.tex_alpha  = (ph[6] & 7u) != 0;
        td.mirror     = ph[2] & 3u;
        /* Contour on: formats 1-4 trade their alpha nibble for a 1-bit
         * test, which Supermodel models as formats 8-11. */
        if (td.alpha_test && td.fmt >= 1u && td.fmt <= 4u) td.fmt += 7u;
        td.on   = g_textures && (ph[6] & 0x400u) != 0;
        /* x is five bits of word 4 with its low bit borrowed from the top
         * of word 5, which looks odd and is right: reading it as six
         * contiguous bits of word 4, the way the upload header carries
         * it, drops a round from 89678 texels found to 18841. */
        td.x    = (32u * (((ph[4] & 0x1Fu) << 1)
                          | ((ph[5] >> 7) & 1u))) & 2047u;
        td.y    = (32u * (ph[5] & 0x1Fu)) & 2047u;
        td.page = ((ph[4] & 0x40u) >> 6) ^ g_texoff_page;
        td.x    = (td.x + g_texoff_x) & 2047u;
        td.y    = (td.y + g_texoff_y) & 1023u;
        /* Bit 16 of word 6 disables lighting for the polygon:
         * it is drawn at full brightness. */
        td.lum  = (ph[6] & 0x00010000u) != 0;
        /* Supermodel's reading of the rest. Bit 23 of word 6 set is opaque;
         * clear, bits 23..18 are the opacity in 32nds. The attract title
         * sequence puts a 40% mist sheet in front of the photographic
         * backdrop -- drawn opaque, it is 800 fields of flat grey. */
        td.alpha = (ph[6] & 0x00800000u) ? 255
                 : (int)(((ph[6] >> 18) & 0x3Fu) * 255u / 32u);
        if (td.alpha > 255) td.alpha = 255;
        td.two_sided = (ph[1] & 0x10u) != 0;
        if (td.w > 2048u) td.w = 2048u;
        if (td.h > 2048u) td.h = 2048u;

        /* Both discard bits set: the polygon is in the list but not drawn.
         * Its vertices still count for the next polygon's reuse. */
        if (slot >= 3 && (ph[0] & 0x300u) != 0x300u) {
            float wv[4][3], wn[3], wvn[4][3];
            if (g_sink) {
                /* Flat shaded with the face normal: no vertex normals. */
                for (j = 0; j < slot; j++) {
                    mvert_t *v = &pool[cur[j]];
                    if (!v->wok) { mat_point(m, v->vs, v->wv); v->wok = 1; }
                }
                mat_dir(m, nrm, wn);
                sink_polygon(pool, cur, slot, wn, rgb, &td);
            } else {
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
        }

        if (g_sink) {
            for (j = 0; j < slot; j++) pprev[j] = cur[j];
            goto next;
        }
        for (j = 0; j < slot; j++) {
            for (k = 0; k < 3; k++) {
                prev[j][k] = vs[j][k];
                prevn[j][k] = vn[j][k];
            }
            prevuv[j][0] = uv[j][0]; prevuv[j][1] = uv[j][1];
        }
    next:
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
static unsigned g_lod_culled;      /* nodes not drawn: too small or out of view */
static unsigned g_node_translate, g_node_matrix, g_node_neither;
static float    g_mat_bigt;


/* A child pointer's top byte says what it points at: bit 24 a model, bit 26
 * a pointer list, neither another culling node (Supermodel's DescendNodePtr).
 * Entries of a pointer list are always culling nodes; bit 24 on one means
 * the list is empty from there, bit 25 that it is the last. */
static void follow(uint32_t p, const mat_t *m, int depth)
{
    uint32_t a = p & 0x00FFFFFFu;

    if (a == 0 || a == 0x800800u)           /* 0x800800 is the dummy entry */
        return;

    switch ((p >> 24) & 5u) {
    case 1:
        draw_model(a, m);
        break;
    case 4: {
        unsigned k;
        if (depth >= MAX_DEPTH) return;
        for (k = 0; k < 256u; k++) {
            uint8_t eb_[4];
            const uint8_t *e = cull_at(a + k, 1, eb_);
            uint32_t v;
            if (!e) break;
            v = rd32le(e);
            if (v & 0x01000000u) break;
            descend(v & 0x00FFFFFFu, m, depth + 1);
            if (v & 0x02000000u) break;
        }
        break;
    }
    case 0:
        descend(a, m, depth + 1);
        break;
    default:
        break;
    }
}

/* Level of detail and culling, after Supermodel's New3D. Each viewport
 * names a table in culling RAM (word 0x17) of 128 entries, four levels
 * each, a (delete size, blend factor) pair of floats per level. A node
 * names its entry (word 3 bits 12..18) and its blend and cull radii (word
 * 9, two 16-bit Real3D floats) scaled by the inherited model scale (word
 * 1). The node's "size" is blend radius over distance from the eye: below
 * level 3's delete size it is not drawn at all; otherwise the first of
 * levels 0..2 whose delete size it reaches, and which exists, is the one
 * drawn. Taking the most detailed level always, as this did, read every
 * model at full detail -- a megabyte and a half of VROM a field. Blending
 * between levels is not done. M3_LOD_FINEST=1 brings that back. */
static uint32_t g_lodtab;            /* culling address, 0 = none */
static float g_mscale = 1.0f;
static int g_nocull;
static float g_fr_sh, g_fr_ch, g_fr_sv, g_fr_cv;   /* frustum half angles */
static int g_lod_finest = -1;

static float r3d_float16(uint32_t v)
{
    uint32_t a = (v & 0xFFFFu) << 15;
    uint32_t e = (a & 0x7E000000u) >> 25;
    e += e <= 31u ? 127u : 127u - 64u;
    return f32_of((a & 0x80000000u) | (e << 23) | ((a & 0x1FFFFFFu) >> 2));
}

/* One culling node and then its siblings. Siblings share the parent's
 * transform, not this node's, and are always culling nodes -- bit 24 on the
 * sibling pointer means there is none. They are walked in a loop: a level of
 * scenery is a long sibling chain, and recursing on it ran into MAX_DEPTH
 * and dropped the tail of the chain. */
static void descend(uint32_t addr, const mat_t *m, int depth)
{
    unsigned chain;

    if (depth >= MAX_DEPTH) return;
    for (chain = 0; addr && chain < 4096u; chain++) {
        uint8_t nb_[10 * 4];
        const uint8_t *node = cull_at(addr, 10, nb_);
        mat_t cur, mm;
        uint32_t mo, w0, child, sib;
        unsigned sx_ = g_texoff_x, sy_ = g_texoff_y, sp_ = g_texoff_page;
        float ms_ = g_mscale;
        int nc_ = g_nocull;
        float tx, ty, tz;
        float lodscale = 1e30f, del[4] = { 0, 0, 0, 0 };
        int have_lod = 0, visible = 1;

        if (!node) return;
        cur = *m;
        w0 = rd32le(node);
        /* Both discard bits: the node and everything after it is hidden. */
        if ((w0 & 0x300u) == 0x300u) return;
        sib = rd32le(node + 8 * 4) & 0x01FFFFFFu;
        if (w0 & 4u)
            g_ctab = ((rd32le(node + 3 * 4) >> 19)
                    | ((rd32le(node + 7 * 4) >> 28) << 13)
                    | ((rd32le(node + 8 * 4) >> 25) << 17)) & 0xFFFFFu;
        if ((w0 & 7u) == 6u) sib = 0;      /* colour-table node: no siblings */

        {
            uint32_t w1 = rd32le(node + 1 * 4);
            if (w1 & 1u) g_mscale = f32_of(w1 & ~3u);
            if (w1 & 2u) g_nocull = 1;
        }
        {
            uint32_t w2 = rd32le(node + 2 * 4);
            if (w2 & 0x8000u) {
                g_texoff_x = 32u * ((w2 >> 7) & 0x3Fu);
                g_texoff_y = 32u * (w2 & 0x1Fu);
                g_texoff_page = (w2 >> 14) & 1u;
            }
        }

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
    /* How the scene places things: a node carries either a translation or
     * a matrix, and this counts which. */
    if (w0 & 0x10u) g_node_translate++; else if (mo) g_node_matrix++;
    else g_node_neither++;
    if (w0 & 0x10u) {
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
            fprintf(stderr, "[node] @%06X w0=%08X w1=%08X w2=%08X"
                            " w3=%08X w9=%08X mo=%u"
                            "  |rows| %.3g %.3g %.3g  t %.4g %.4g %.4g\n",
                    addr, rd32le(node), rd32le(node + 1 * 4),
                    rd32le(node + 2 * 4), rd32le(node + 3 * 4),
                    rd32le(node + 9 * 4), mo,
                    (double)__builtin_sqrtf(mm.m[0][0]*mm.m[0][0]+mm.m[0][1]*mm.m[0][1]+mm.m[0][2]*mm.m[0][2]),
                    (double)__builtin_sqrtf(mm.m[1][0]*mm.m[1][0]+mm.m[1][1]*mm.m[1][1]+mm.m[1][2]*mm.m[1][2]),
                    (double)__builtin_sqrtf(mm.m[2][0]*mm.m[2][0]+mm.m[2][1]*mm.m[2][1]+mm.m[2][2]*mm.m[2][2]),
                    (double)mm.m[0][3], (double)mm.m[1][3], (double)mm.m[2][3]); }
        if (mo > g_mat_max) g_mat_max = mo;
        if (big > g_mat_bigt) { g_mat_bigt = big; g_mat_bigi = mo; }
        if (mm.m[0][0] == 0.0f && mm.m[0][1] == 0.0f && mm.m[0][2] == 0.0f
         && mm.m[1][0] == 0.0f && mm.m[1][1] == 0.0f && mm.m[1][2] == 0.0f)
            g_mat_zero++;
        cur = mat_mul(m, &mm);
    }

        if (g_lod_finest < 0) g_lod_finest = getenv("M3_LOD_FINEST") != NULL;
        if (g_lodtab && !g_lod_finest) {
            uint8_t tb_[8 * 4];
            unsigned sel = (rd32le(node + 3 * 4) >> 12) & 0x7Fu;
            const uint8_t *t = cull_at(g_lodtab + sel * 8u, 8, tb_);
            if (t) {
                uint32_t w9 = rd32le(node + 9 * 4);
                float cr = r3d_float16(w9) * g_mscale;
                float br = r3d_float16(w9 >> 16) * g_mscale;
                float x = cur.m[0][3], y = cur.m[1][3], z = cur.m[2][3];
                float d = __builtin_sqrtf(x * x + y * y + z * z);
                unsigned k;
                for (k = 0; k < 4u; k++) del[k] = f32_of(rd32le(t + k * 8));
                have_lod = 1;
                if (!g_nocull) {
                    lodscale = d > 0.0f ? br / d : 1e30f;
                    if (!(lodscale >= 0.0f)) lodscale = 0.0f;
                    /* Outside the view volume by more than the cull
                     * radius: behind the eye, or past a side plane. View
                     * space has z forward. */
                    if (z < -cr
                        || x * g_fr_ch - z * g_fr_sh > cr
                        || -x * g_fr_ch - z * g_fr_sh > cr
                        || y * g_fr_cv - z * g_fr_sv > cr
                        || -y * g_fr_cv - z * g_fr_sv > cr
                        || lodscale < del[3])
                        visible = 0;
                }
            }
        }
        if (!visible) g_lod_culled++;

        child = rd32le(node + 7 * 4) & 0x07FFFFFFu;
        if (!visible) {
            /* not drawn; its siblings still are */
        } else if (w0 & 0x08u) {
            /* A four-entry LOD table: the first level whose delete size
             * the node reaches and which exists (bit 24), else level 3.
             * Without a blend table, the most detailed level that exists. */
            uint8_t lb_[4 * 4];
            const uint8_t *lod = cull_at(child & 0x00FFFFFFu, 4, lb_);
            if (lod) {
                unsigned k;
                uint32_t e = 0;
                for (k = 0; k < 4u; k++) {
                    e = rd32le(lod + k * 4);
                    if (k == 3u) break;
                    if (have_lod && !g_nocull
                        ? (lodscale >= del[k] && (e & 0x01000000u))
                        : (e & 0x01000000u) != 0)
                        break;
                }
                if (rd32le(node + 3 * 4) & 0x20000000u)
                    descend(e & 0x00FFFFFFu, &cur, depth + 1);
                else
                    draw_model(e & 0x00FFFFFFu, &cur);
            }
        } else {
            follow(child, &cur, depth);
        }

        g_texoff_x = sx_; g_texoff_y = sy_; g_texoff_page = sp_;
        g_mscale = ms_; g_nocull = nc_;
        /* 0x800800 is the dummy node, and an unused node is all dummy
         * words, so its sibling is itself: stop rather than spin. */
        sib = (sib & 0x01000000u) ? 0 : (sib & 0x00FFFFFFu);
        addr = (sib == addr || sib == 0x800800u) ? 0 : sib;
    }
}

/* ---- rasteriser -------------------------------------------------------- */
static _Thread_local unsigned g_drawn;
static float *g_zbuf;
static int g_zw, g_zh;

#define ZNEAR 0.05f

/* The sink's near plane. No pixel of a race lies nearer than z ~0.9 (fields
 * 5000 and 6000, tools/dreamcast/sink_render.py --near), and a hardware
 * depth range is short (see pvr_r3d.c): vertices cut at 0.05 have w = 20,
 * at 0.25 only 4, which is two bits fewer per viewport. */
#define ZNEAR_SINK 0.25f

/* Sutherland-Hodgman against z >= znear. Three input vertices give at most
 * four out, so the caller's buffer is [4][3]. */
static int clip_near(const float v[3][5], float out[4][5], float znear)
{
    int n = 0, i, k;
    for (i = 0; i < 3; i++) {
        const float *a = v[i];
        const float *b = v[(i + 1) % 3];
        int ain = a[2] >= znear, bin = b[2] >= znear;
        if (ain && n < 4) {
            for (k = 0; k < 5; k++) out[n][k] = a[k];
            n++;
        }
        if (ain != bin && n < 4) {
            float t = (znear - a[2]) / (b[2] - a[2]);
            for (k = 0; k < 5; k++) out[n][k] = a[k] + t * (b[k] - a[k]);
            n++;
        }
    }
    return n;
}

/* Thread-local: each band of the screen is rasterised on its own thread
 * (see raster_bands), and these are diagnostics. With M3_REAL3D_STATS or
 * M3_PICK set the whole screen is one band, so they stay exact. */
static _Thread_local unsigned g_cull_back, g_cull_near, g_cull_degen, g_cull_off;
static _Thread_local unsigned g_tex_tris, g_flat_tris, g_tex_px, g_flat_px;
static _Thread_local unsigned g_texel_hit, g_texel_clear, g_texel_missing;
/* M3_PICK=x,y: which polygon last wrote that pixel, and its texture. */
static long g_pick = -1;
static _Thread_local long g_pick_tri = -1;
/* Where the polygons that find nothing are looking. */
enum { MISS_SLOTS = 24 };
static _Thread_local struct { unsigned x, y, page, w, h, n; } g_miss[MISS_SLOTS];
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

/* Where a viewport's view space lands on the screen: pixel = a * (X/Z) + b,
 * and the rectangle it may draw in. */
static struct { double ax, bx, ay, by; int x0, y0, x1, y1; } g_proj;

static void raster(uint32_t *fb, int fw, int fh, float sx, float sy,
                   const float L[3], float intensity, int by0, int by1)
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
    if (by0 == 0 && getenv("M3_REAL3D_STATS") && g_ntris) {
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

    /* Opaque first, then the translucent ones over them in submission
     * order, blended and leaving the depth buffer alone. */
    for (t = 0; t < 2u * g_ntris; t++) {
        const tri_t *tr = &g_tris[t % g_ntris];
        /* Screen-space vertices in double. A projected triangle here can
         * reach coordinates in the hundreds of thousands -- the scene runs
         * well past the edges of a 496-pixel screen -- and in float the
         * barycentric denominator then loses enough precision that pixels
         * inside the triangle test as outside. It does not look like a
         * precision bug: it looks like a cross-hatch, as though the polygon
         * were deliberately stippled for translucency. */
        double p[3][3], q[3][3];
        float clipped[4][5], src[3][5];
        double d, invd;
        float lit, sh;
        if ((tr->alpha < 255) != (t >= g_ntris)) continue;
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
            if (!tr->two_sided && tr->n[0]*cx + tr->n[1]*cy + tr->n[2]*cz > 0.0f)
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
        nclip = clip_near(src, clipped, ZNEAR);
        if (nclip < 3) { g_cull_near++; continue; }

        for (fan = 1; fan + 1 < nclip; fan++) {
            const float *cv[3];
            cv[0] = clipped[0]; cv[1] = clipped[fan]; cv[2] = clipped[fan + 1];

        for (k = 0; k < 3; k++) {
            double iz = 1.0 / cv[k][2];
            p[k][0] = g_proj.ax * (double)cv[k][0] * iz + g_proj.bx;
            p[k][1] = g_proj.ay * (double)cv[k][1] * iz + g_proj.by;
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
        if (x0 < g_proj.x0) x0 = g_proj.x0;
        if (y0 < g_proj.y0) y0 = g_proj.y0;
        if (y0 < by0) y0 = by0;
        if (y1 > by1) y1 = by1;
        if (x1 > g_proj.x1) x1 = g_proj.x1;
        if (y1 > g_proj.y1) y1 = g_proj.y1;
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

        invd = 1.0 / d;
        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                double l0, l1, l2, z;
                int o;
                l0 = ((p[1][1]-p[2][1])*(x-p[2][0])
                    + (p[2][0]-p[1][0])*(y-p[2][1])) * invd;
                l1 = ((p[2][1]-p[0][1])*(x-p[2][0])
                    + (p[0][0]-p[2][0])*(y-p[2][1])) * invd;
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
                            int64_t w = sample(tr, uu, vv);
                            if (w < 0) { g_texel_missing++; note_miss(tr); }
                            else {
                                uint32_t t = (uint32_t)w;
                                /* Format 0's transparent bit has always
                                 * been honoured here; the others only
                                 * when the polygon asks for it. */
                                if ((tr->fmt == 0 || tr->alpha_test || tr->tex_alpha)
                                    && (t >> 24) < 128u) { g_texel_clear++; continue; }
                                /* Modulated by the polygon colour: that is
                                 * what tints the greyscale formats. */
                                c = 0xFF000000u
                                  | ((((t >> 16) & 0xFFu) * tr->r / 255u) << 16)
                                  | ((((t >> 8) & 0xFFu) * tr->g / 255u) << 8)
                                  | ((t & 0xFFu) * tr->b / 255u);
                                g_texel_hit++;
                            }
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
                        float il = 1.0f / __builtin_sqrtf(nl);
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
                    if (tr->alpha < 255) {
                        uint32_t a = tr->alpha, d0 = fb[o], m = 0;
                        int sh2;
                        for (sh2 = 0; sh2 < 24; sh2 += 8)
                            m |= ((((c >> sh2) & 0xFFu) * a
                                  + ((d0 >> sh2) & 0xFFu) * (255u - a)) / 255u) << sh2;
                        fb[o] = 0xFF000000u | m;
                    } else {
                        g_zbuf[o] = (float)z; fb[o] = c;
                    }
                    g_drawn++;
                    if (tr->textured) g_tex_px++; else g_flat_px++;
                    if (o == g_pick) g_pick_tri = (long)(t % g_ntris);
                }
            }
        }
        }
    }
}

/* ---- rasterising on every core -----------------------------------------
 *
 * The rasteriser is software and was the whole frame budget on its own in a
 * busy scene. The screen is cut into horizontal bands, one per thread, and
 * each thread draws every triangle clipped to its rows, in the same order --
 * so the picture is byte-identical to drawing it on one thread, the depth
 * buffer and the translucent pass included. Nothing here feeds back into
 * the guest, so netplay and save states are untouched.
 */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef SRWLOCK            m3_lock;
typedef CONDITION_VARIABLE m3_cond;
#  define LOCK(l)       AcquireSRWLockExclusive(l)
#  define UNLOCK(l)     ReleaseSRWLockExclusive(l)
#  define WAIT(c, l)    SleepConditionVariableSRW(c, l, INFINITE, 0)
#  define WAKE_ALL(c)   WakeAllConditionVariable(c)
#else
#  include <pthread.h>
#  include <unistd.h>
typedef pthread_mutex_t m3_lock;
typedef pthread_cond_t  m3_cond;
#  define LOCK(l)       pthread_mutex_lock(l)
#  define UNLOCK(l)     pthread_mutex_unlock(l)
#  define WAIT(c, l)    pthread_cond_wait(c, l)
#  define WAKE_ALL(c)   pthread_cond_broadcast(c)
#endif

#define MAX_BANDS 16
static struct {
    int      bands;             /* threads + 1: the caller draws band 0 */
    m3_lock  lock;
    m3_cond  go, done;
    unsigned gen, pending;
    uint32_t *fb; int fw, fh; float sx, sy, L[3], intensity;
    int      y0, y1;            /* the rows being banded */
} g_pool;

static void band_rows(int i, int *by0, int *by1)
{
    int rows = g_pool.y1 - g_pool.y0 + 1;
    *by0 = g_pool.y0 + rows * i / g_pool.bands;
    *by1 = g_pool.y0 + rows * (i + 1) / g_pool.bands - 1;
}

#ifdef _WIN32
static DWORD WINAPI band_worker(LPVOID arg)
#else
static void *band_worker(void *arg)
#endif
{
    int i = (int)(intptr_t)arg, by0, by1;
    unsigned seen = 0;
    for (;;) {
        LOCK(&g_pool.lock);
        while (g_pool.gen == seen) WAIT(&g_pool.go, &g_pool.lock);
        seen = g_pool.gen;
        UNLOCK(&g_pool.lock);
        band_rows(i, &by0, &by1);
        raster(g_pool.fb, g_pool.fw, g_pool.fh, g_pool.sx, g_pool.sy,
               g_pool.L, g_pool.intensity, by0, by1);
        LOCK(&g_pool.lock);
        if (--g_pool.pending == 0) WAKE_ALL(&g_pool.done);
        UNLOCK(&g_pool.lock);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void pool_start(void)
{
    int n = 0, i;
    const char *e = getenv("M3_RENDER_THREADS");
#ifdef _WIN32
    n = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    InitializeSRWLock(&g_pool.lock);
    InitializeConditionVariable(&g_pool.go);
    InitializeConditionVariable(&g_pool.done);
#else
    n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    pthread_mutex_init(&g_pool.lock, NULL);
    pthread_cond_init(&g_pool.go, NULL);
    pthread_cond_init(&g_pool.done, NULL);
#endif
    /* One core is the guest's; the rest draw, up to MAX_BANDS in all. */
    n = n > 2 ? n - 1 : 1;
    if (e) n = atoi(e);
    if (n < 1) n = 1;
    if (n > MAX_BANDS) n = MAX_BANDS;
    g_pool.bands = n;
    for (i = 1; i < n; i++) {
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, band_worker, (LPVOID)(intptr_t)i, 0, NULL);
        if (!h) { g_pool.bands = i; break; }
        CloseHandle(h);
#else
        pthread_t t;
        if (pthread_create(&t, NULL, band_worker, (void *)(intptr_t)i)) { g_pool.bands = i; break; }
        pthread_detach(t);
#endif
    }
}

/* M3_TEX_STATS=1: every 100 fields, how many distinct textures (origin,
 * size, page, format) the last field's polygons used, their area, and how
 * much of the sheet has ever been written -- what a texture cache on a
 * host without room for the sheet has to hold. */
static int g_texstats = -1;
enum { TS_SLOTS = 4096 };
static struct { uint32_t key, area; } g_ts[TS_SLOTS];
static unsigned g_ts_n, g_ts_fmts[12];
static uint64_t g_ts_area;
static void tex_stats_note(void)
{
    unsigned i;
    for (i = 0; i < g_ntris; i++) {
        unsigned textured, tx, ty, page, fmt, tw, th;
        uint32_t key, h;
        if (g_sink) {
            const sink_tri_t *t = &g_sink_tris[i];
            textured = t->textured;
            if (!textured) continue;
            tx = t->tx; ty = t->ty;
            page = t->page; fmt = t->fmt; tw = t->tw; th = t->th;
        } else {
            const tri_t *t = &g_tris[i];
            textured = t->textured;
            if (!textured) continue;
            tx = t->tx; ty = t->ty;
            page = t->page; fmt = t->fmt; tw = t->tw; th = t->th;
        }
        key = ((uint32_t)(tx >> 5) | ((uint32_t)(ty >> 5) << 6)
               | ((uint32_t)page << 11) | ((uint32_t)fmt << 12))
            ^ ((uint32_t)tw << 16) ^ ((uint32_t)th << 22);
        for (h = (key * 2654435761u) >> 20;; h = (h + 1) & (TS_SLOTS - 1)) {
            if (!g_ts[h].area) {
                if (g_ts_n >= TS_SLOTS / 2) break;
                g_ts[h].key = key;
                g_ts[h].area = (uint32_t)tw * th;
                g_ts_n++;
                g_ts_area += g_ts[h].area;
                g_ts_fmts[fmt < 12 ? fmt : 11]++;
                break;
            }
            if (g_ts[h].key == key) break;
        }
    }
}
static void tex_stats_field(void)
{
    if (model3recomp_frame_count() % 100 == 0) {
        size_t k, set = 0;
        if (g_sheet_set)
            for (k = 0; k < ((size_t)TEX_W * TEX_H) >> (2 * g_sheet_shift); k++)
                set += g_sheet_set[k];
        fprintf(stderr, "[texstats] field %llu: %u textures, %llu Ktexels; "
                "sheet %zu Ktexels written; fmt", 
                (unsigned long long)model3recomp_frame_count(), g_ts_n,
                (unsigned long long)(g_ts_area >> 10), set >> 10);
        for (k = 0; k < 12; k++) fprintf(stderr, " %u", g_ts_fmts[k]);
        fprintf(stderr, "; uploads FIFO %llu (%llu Ktexels) VROM %llu (%llu Ktexels)",
                (unsigned long long)g_up_count[0],
                (unsigned long long)(g_up_texels[0] >> 10),
                (unsigned long long)g_up_count[1],
                (unsigned long long)(g_up_texels[1] >> 10));
        fprintf(stderr, "\n");
    }
    memset(g_ts, 0, sizeof g_ts);
    memset(g_ts_fmts, 0, sizeof g_ts_fmts);
    g_ts_n = 0;
    g_ts_area = 0;
}

/* ---- the hardware path (real3d_set_sink) ------------------------------- */
void real3d_set_sink(const m3_r3d_sink *sink) { g_sink = sink; }
void real3d_set_sheet(uint16_t *texels, unsigned shift)
{
    g_sheet = texels;
    g_sheet_shift = shift;
}

/* One vertex onto the screen, as raster() projects it: x and y in pixels,
 * w = 1/z. */
static void sink_project_vertex(const float v[3], const float uv[2],
                                m3_r3d_vertex *out)
{
    double iz = 1.0 / v[2];
    out->x = (float)(g_proj.ax * (double)v[0] * iz + g_proj.bx);
    out->y = (float)(g_proj.ay * (double)v[1] * iz + g_proj.by);
    out->w = (float)iz;
    out->u = uv[0];
    out->v = uv[1];
}

/* A polygon's triangles (a fan from vertex 0) for the sink, with the same
 * culling, near-plane cut (at ZNEAR_SINK) and projection as raster(), each
 * written once as its final record while draw_model's transformed vertices
 * are still hot. A vertex is projected the first time a triangle that needs
 * no cut uses it (pok) and keeps that for the polygon's other triangle and
 * for later polygons that reuse it; the colour is the polygon's. The
 * arithmetic is the triangle-at-a-time path's, so the PVR gets the same
 * bytes. */
static void sink_polygon(mvert_t *pool, const uint8_t cur[4], unsigned nv,
                         const float n[3], uint32_t rgb, const texdesc_t *td)
{
    uint32_t argb = 0;
    int shaded = 0;
    unsigned j;

    for (j = 1; j + 1 < nv; j++) {
        mvert_t *t3[3];
        sink_tri_t *tr;
        int k, nproj;

        t3[0] = &pool[cur[0]]; t3[1] = &pool[cur[j]]; t3[2] = &pool[cur[j + 1]];
        if (g_ntris >= g_max_tris) return;
        tr = &g_sink_tris[g_ntris++];
        /* Culled triangles keep their place in the queue, as they did in
         * the software triangle table. sink_bounds and sink_submit skip
         * nproj == 0 before reading any of the fields left unwritten. */
        tr->nproj = 0;
        tr->textured = (uint8_t)(td->on ? 1 : 0);
        if (tr->textured) {
            tr->tx = (uint16_t)td->x; tr->ty = (uint16_t)td->y;
            tr->tw = (uint16_t)td->w; tr->th = (uint16_t)td->h;
            tr->page = (uint8_t)td->page;
            tr->fmt = (uint8_t)td->fmt;
            tr->alpha_test = (uint8_t)td->alpha_test;
            tr->tex_alpha = (uint8_t)td->tex_alpha;
            tr->mirror = (uint8_t)td->mirror;
        }
        {
            const float *a = t3[0]->wv, *b = t3[1]->wv, *c = t3[2]->wv;
            float cx = (a[0] + b[0] + c[0]) / 3.0f;
            float cy = (a[1] + b[1] + c[1]) / 3.0f;
            float cz = (a[2] + b[2] + c[2]) / 3.0f;
            if (!td->two_sided && n[0]*cx + n[1]*cy + n[2]*cz > 0.0f)
                continue;
            if (a[2] >= ZNEAR_SINK && b[2] >= ZNEAR_SINK && c[2] >= ZNEAR_SINK) {
                /* Nothing to cut, the usual case: clip_near would hand
                 * back the same three vertices in the same order. */
                for (k = 0; k < 3; k++) {
                    mvert_t *v = t3[k];
                    if (!v->pok) {
                        sink_project_vertex(v->wv, v->uv, &v->pj);
                        v->pok = 1;
                    }
                    tr->proj[k].x = v->pj.x; tr->proj[k].y = v->pj.y;
                    tr->proj[k].w = v->pj.w;
                    tr->proj[k].u = v->pj.u; tr->proj[k].v = v->pj.v;
                }
                nproj = 3;
            } else {
                float src[3][5], clipped[4][5];
                for (k = 0; k < 3; k++) {
                    const mvert_t *v = t3[k];
                    src[k][0] = v->wv[0]; src[k][1] = v->wv[1];
                    src[k][2] = v->wv[2];
                    src[k][3] = v->uv[0]; src[k][4] = v->uv[1];
                }
                nproj = clip_near(src, clipped, ZNEAR_SINK);
                for (k = 0; k < nproj; k++)
                    sink_project_vertex(clipped[k], &clipped[k][3], &tr->proj[k]);
            }
        }
        if (nproj < 3) continue;
        tr->nproj = (uint8_t)nproj;

        if (!shaded) {
            float lit, sh;
            int r, g, b8;
            lit = n[0]*g_sink_light[0] + n[1]*g_sink_light[1]
                + n[2]*g_sink_light[2];
            if (lit < 0) lit = 0;
            if (lit > 1.0f) lit = 1.0f;
            sh = td->lum ? g_sink_intensity
                         : (AMBIENT + (1.0f - AMBIENT) * lit) * g_sink_intensity;
            r = (int)(((rgb >> 16) & 0xFFu) * sh);
            g = (int)(((rgb >> 8) & 0xFFu) * sh);
            b8 = (int)((rgb & 0xFFu) * sh);
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b8 > 255) b8 = 255;
            argb = ((uint32_t)td->alpha << 24) | ((uint32_t)r << 16)
                 | ((uint32_t)g << 8) | (uint32_t)b8;
            shaded = 1;
        }
        tr->argb = argb;
    }
}

/* One side of the viewport's rectangle, for sink_bounds: x (axis 0) or y
 * (axis 1) kept >= lim (ge) or <= lim. w is linear on the screen, so the
 * cut interpolates it as it does x and y. */
static int cut_xyw(const float in[][3], int n, float out[][3], int axis,
                   float lim, int ge)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const float *a = in[i], *b = in[i + 1 < n ? i + 1 : 0];   /* no % */
        int ia = ge ? a[axis] >= lim : a[axis] <= lim;
        int ib = ge ? b[axis] >= lim : b[axis] <= lim;
        if (ia) { out[m][0] = a[0]; out[m][1] = a[1]; out[m][2] = a[2]; m++; }
        if (ia != ib) {
            float t = (lim - a[axis]) / (b[axis] - a[axis]);
            out[m][0] = a[0] + t * (b[0] - a[0]);
            out[m][1] = a[1] + t * (b[1] - a[1]);
            out[m][2] = a[2] + t * (b[2] - a[2]);
            m++;
        }
    }
    return m;
}

/* The range of w the viewport shows: over the part of each polygon inside
 * its rectangle, not over its triangles. A road's near end runs far off the
 * screen to w = 1/ZNEAR_SINK and a backdrop's dome far round behind, and
 * counting those gave three viewports ~42 bits of w between them, more than
 * a depth buffer holds (pvr_r3d.c); what is on the screen is ~15. Returns
 * wmin > wmax when nothing is. */
static void sink_bounds(float *wmin, float *wmax, float x0, float y0,
                        float x1, float y1)
{
    float lo = 1e30f, hi = 0.0f;
    unsigned t;
    for (t = 0; t < g_ntris; t++) {
        const sink_tri_t *tr = &g_sink_tris[t];
        float a[12][3], b[12][3];
        int n = tr->nproj, k, inside = 1;
        for (k = 0; k < n; k++) {
            a[k][0] = tr->proj[k].x;
            a[k][1] = tr->proj[k].y;
            a[k][2] = tr->proj[k].w;
            if (tr->proj[k].x < x0 || tr->proj[k].x > x1 ||
                tr->proj[k].y < y0 || tr->proj[k].y > y1) inside = 0;
        }
        if (n >= 3 && !inside) {
            n = cut_xyw(a, n, b, 0, x0, 1);
            n = cut_xyw(b, n, a, 0, x1, 0);
            n = cut_xyw(a, n, b, 1, y0, 1);
            n = cut_xyw(b, n, a, 1, y1, 0);
        }
        if (n < 3) continue;
        for (k = 0; k < n; k++) {
            if (a[k][2] < lo) lo = a[k][2];
            if (a[k][2] > hi) hi = a[k][2];
        }
    }
    *wmin = lo; *wmax = hi;
}

/* sink_polygon's triangles, flat shaded, handed to the sink one by one
 * instead of rasterised: the opaque ones first, then the translucent. */
static void sink_submit(void)
{
    unsigned pass, t;
    /* Two passes, not one loop over 2n with t % n: the SH-4 divides in
     * software, a call per triangle per pass. */
    for (pass = 0; pass < 2u; pass++)
    for (t = 0; t < g_ntris; t++) {
        const sink_tri_t *tr = &g_sink_tris[t];
        m3_r3d_texture tex;
        int translucent;
        if (tr->nproj < 3) continue;
        translucent = (tr->argb >> 24) < 255u;
        if (translucent != (int)pass) continue;
        g_drawn++;
        if (tr->textured && g_textures) {
            tex.x = tr->tx; tex.y = tr->ty; tex.page = tr->page;
            tex.w = tr->tw ? tr->tw : 32u; tex.h = tr->th ? tr->th : 32u;
            tex.fmt = tr->fmt;
            tex.mirror_u = (tr->mirror & 2u) != 0;
            tex.mirror_v = (tr->mirror & 1u) != 0;
            tex.alpha_test = tr->alpha_test;
            tex.tex_alpha = tr->tex_alpha;
        }
        g_sink->polygon(tr->proj, tr->nproj, tr->argb, translucent,
                        tr->textured && g_textures ? &tex : NULL);
    }
}

static void raster_bands(uint32_t *fb, int fw, int fh, float sx, float sy,
                         const float L[3], float intensity)
{
    static int started;
    int by0, by1;
    if (getenv("M3_REAL3D_STATS") || g_pick >= 0) {
        raster(fb, fw, fh, sx, sy, L, intensity, 0, fh - 1);
        return;
    }
    if (!started) { started = 1; pool_start(); }
    if (g_pool.bands <= 1) {
        raster(fb, fw, fh, sx, sy, L, intensity, 0, fh - 1);
        return;
    }
    LOCK(&g_pool.lock);
    g_pool.fb = fb; g_pool.fw = fw; g_pool.fh = fh; g_pool.sx = sx; g_pool.sy = sy;
    g_pool.L[0] = L[0]; g_pool.L[1] = L[1]; g_pool.L[2] = L[2];
    g_pool.intensity = intensity;
    g_pool.y0 = g_proj.y0; g_pool.y1 = g_proj.y1;
    g_pool.pending = (unsigned)g_pool.bands - 1u;
    g_pool.gen++;
    WAKE_ALL(&g_pool.go);
    UNLOCK(&g_pool.lock);

    band_rows(0, &by0, &by1);
    raster(fb, fw, fh, sx, sy, L, intensity, by0, by1);

    LOCK(&g_pool.lock);
    while (g_pool.pending) WAIT(&g_pool.done, &g_pool.lock);
    UNLOCK(&g_pool.lock);
}

/* ---- entry point ------------------------------------------------------- */
void real3d_skip(void)
{
    flush_texture_fifo();
}

void real3d_render(void)
{
    int fw = 0, fh = 0;
    /* A sink, when installed, takes the scene instead of the software
     * rasteriser -- on any host, so the hardware path can be checked on a
     * desktop too. */
    uint32_t *fb = g_sink ? NULL : platform_framebuffer(&fw, &fh);
    uint32_t vpaddr;
    unsigned guard, total = 0, prio;
    static int inited;

    if (!fb && !g_sink) return;
    if (!fb) { fw = 496; fh = 384; }     /* the board's screen, for g_proj */

    if (!inited) {
        const char *e = getenv("M3_VERTEX_SCALE");
        if (e && atof(e) > 0.0) g_vertex_scale = (float)atof(e);
        g_vertex_inv = 1.0f / g_vertex_scale;
        e = getenv("M3_TEX_SIZE");
        if (e) g_tex_size_mode = atoi(e);
        g_tex_all = getenv("M3_TEX_ALL") != NULL;
        g_textures = getenv("M3_NO_TEXTURES") == NULL;
        g_light_flip = getenv("M3_LIGHT_FLIP") != NULL;
        g_old_proj = getenv("M3_OLD_PROJ") != NULL;
        e = getenv("M3_AMBIENT");
        if (e) { g_ambient = (float)atof(e); g_ambient_forced = 1; }
        /* A sink gets a smaller queue (one viewport at a time is all it
         * holds) and no texture sheet: 12 MB a Dreamcast does not have. */
        g_max_tris = fb ? MAX_TRIS : SINK_MAX_TRIS;
        if (fb)
            g_tris = (tri_t *)malloc(sizeof(tri_t) * g_max_tris);
        else
            g_sink_tris = (sink_tri_t *)malloc(sizeof(sink_tri_t) * g_max_tris);
        if (fb && !g_sheet) {
            const char *sh = getenv("M3_SHEET_SHIFT");
            g_sheet_shift = sh ? (unsigned)atoi(sh) & 3u : 0u;
            g_sheet = (uint16_t *)calloc((TEX_W * TEX_H) >> (2 * g_sheet_shift),
                                         sizeof(uint16_t));
            g_sheet_set = (uint8_t *)calloc((TEX_W * TEX_H) >> (2 * g_sheet_shift), 1);
        }
        inited = 1;
    }
    if ((fb && !g_tris) || (!fb && !g_sink_tris)) return;

    flush_texture_fifo();

    /* No clear: the tilemap layers the guest put behind the 3D have
     * already been drawn into this buffer. */

    if (!fb) {
        g_sink->begin_frame();
    } else if (g_zw != fw || g_zh != fh) {
        free(g_zbuf);
        g_zbuf = (float *)malloc(sizeof(float) * (size_t)fw * (size_t)fh);
        g_zw = fw; g_zh = fh;
    }
    if (fb && !g_zbuf) return;
    if (fb) {
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
            if (fp) { fwrite(g_sheet, 2, ((size_t)TEX_W * TEX_H) >> (2 * g_sheet_shift), fp);
                      fclose(fp); }
        }
    }
    g_drawn = 0;
    if (g_texstats < 0) g_texstats = getenv("M3_TEX_STATS") != NULL;
    { static int probed; if (!probed) { probed = 1;
        { const char *e = getenv("M3_NODE_DUMP");
          m3_node_dump = e ? (int)strtol(e, NULL, 0) : 0;
          e = getenv("M3_NODE_FROM");
          m3_node_from = e ? strtoull(e, NULL, 0) : 0; } } }
    /* Viewports are drawn lowest priority first (word 0 bits 3..4), and a
     * viewport with bit 5 set is switched off. */
    for (prio = 0; prio < 4u; prio++) {
    vpaddr = 0x800000u;
    for (guard = 0; guard < 16u && vpaddr; guard++) {
        uint8_t vb_[0x30 * 4];
        const uint8_t *vp = cull_at(vpaddr, 0x30, vb_);
        mat_t cam;
        float hf, vf, sx, sy, light[3], intensity;
        uint32_t next;

        if (!vp) break;
        if (((rd32le(vp) >> 3) & 3u) != prio || (rd32le(vp) & 0x20u)) {
            next = rd32le(vp + 0x01 * 4) & 0x00FFFFFFu;
            if (next == vpaddr) break;
            vpaddr = next;
            continue;
        }
        g_matbase = rd32le(vp + 0x16 * 4) & 0x00FFFFFFu;
        g_lodtab = rd32le(vp + 0x17 * 4) & 0x00FFFFFFu;
        g_mscale = 1.0f;
        g_nocull = 0;

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
        g_fr_sh = sinf(hf); g_fr_ch = cosf(hf);
        g_fr_sv = sinf(vf); g_fr_cv = cosf(vf);
        sx = (fw * 0.5f) / tanf(hf);
        sy = (fh * 0.5f) / tanf(vf);
        g_proj.ax = sx; g_proj.bx = fw * 0.5;
        g_proj.ay = -sy; g_proj.by = fh * 0.5;
        g_proj.x0 = 0; g_proj.y0 = 0; g_proj.x1 = fw - 1; g_proj.y1 = fh - 1;

        /* Supermodel's reading, which replaces the above when the words are
         * there: words 8..0xB give an asymmetric frustum as tangents --
         * 1/(right-left), 1/(top-bottom) and where the centre sits in each
         * -- and words 0x1A / 0x14 the viewport's own rectangle on the
         * screen, X/Y in 12.4 and width/height in 14.2. A HUD is a viewport
         * of its own; projected as though it were the whole screen, its
         * counters land somewhere they cannot be seen. */
        {
            float cvv = f32_of(rd32le(vp + 0x08 * 4)), cww = f32_of(rd32le(vp + 0x09 * 4));
            float io = f32_of(rd32le(vp + 0x0A * 4)), jo = f32_of(rd32le(vp + 0x0B * 4));
            uint32_t w14 = rd32le(vp + 0x14 * 4), w1a = rd32le(vp + 0x1A * 4);
            double vx = (w1a & 0xFFFFu) / 16.0, vy = (w1a >> 16) / 16.0;
            double vw = (w14 & 0xFFFFu) / 4.0, vh = (w14 >> 16) / 4.0;
            if (cvv > 1e-6f && cww > 1e-6f && vw >= 1.0 && vh >= 1.0 && !g_old_proj) {
                double l = (0.0 - jo) / cvv, r = (1.0 - jo) / cvv;
                double b = -(1.0 - io) / cww, t = io / cww;
                g_proj.ax = vw / (r - l);
                g_proj.bx = vx - l * g_proj.ax;
                g_proj.ay = -vh / (t - b);
                g_proj.by = vy + t * vh / (t - b);
                g_proj.x0 = (int)vx; g_proj.y0 = (int)vy;
                g_proj.x1 = (int)(vx + vw) - 1; g_proj.y1 = (int)(vy + vh) - 1;
                if (g_proj.x0 < 0) g_proj.x0 = 0;
                if (g_proj.y0 < 0) g_proj.y0 = 0;
                if (g_proj.x1 > fw - 1) g_proj.x1 = fw - 1;
                if (g_proj.y1 > fh - 1) g_proj.y1 = fh - 1;
            }
            if (getenv("M3_VP_TRACE"))
                fprintf(stderr, "[vp] %u w0 %08X prio %u%s cv %g cw %g io %g jo %g rect %g,%g %gx%g\n",
                        guard, rd32le(vp), (rd32le(vp) >> 3) & 3u,
                        (rd32le(vp) & 0x20u) ? " disabled" : "",
                        cvv, cww, io, jo, vx, vy, vw, vh);
        }

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
        if (fb) {
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
        if (!fb) {
            g_sink_light[0] = light[0];
            g_sink_light[1] = light[1];
            g_sink_light[2] = light[2];
            g_sink_intensity = intensity;
        }

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
                { const char *e = getenv("M3_PICK");
                  g_pick = -1; g_pick_tri = -1;
                  if (e) { int px = atoi(e), py = 0; const char *c = strchr(e, ',');
                           if (c) py = atoi(c + 1);
                           g_pick = (long)py * fw + px; } }
                if (g_texstats) tex_stats_note();
                if (fb) raster_bands(fb, fw, fh, sx, sy, light, intensity);
                else {
                    if (g_sink->begin_viewport) {
                        float wmin, wmax;
                        float x0 = (float)g_proj.x0, x1 = (float)(g_proj.x1 + 1);
                        float y0 = (float)g_proj.y0, y1 = (float)(g_proj.y1 + 1);
                        sink_bounds(&wmin, &wmax, x0, y0, x1, y1);
                        g_sink->begin_viewport(wmin, wmax, x0, y0, x1, y1);
                    }
                    sink_submit();
                }
                if (g_pick_tri >= 0) {
                    const tri_t *p = &g_tris[g_pick_tri];
                    fprintf(stderr, "[pick] vp%u tri %ld tex %d at %u,%u page %u"
                            " %ux%u fmt %u alpha %u test %u texalpha %u lum %u"
                            " rgb %02X%02X%02X uv %.1f,%.1f %.1f,%.1f %.1f,%.1f\n",
                            guard, g_pick_tri, p->textured, p->tx, p->ty,
                            p->page, p->tw, p->th, p->fmt, p->alpha,
                            p->alpha_test, p->tex_alpha, p->luminous,
                            p->r, p->g, p->b, p->uv[0][0], p->uv[0][1],
                            p->uv[1][0], p->uv[1][1], p->uv[2][0], p->uv[2][1]);
                }
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
            if (getenv("M3_REAL3D_STATS"))
                fprintf(stderr, "[real3d]     nodes: %u by translation,"
                                "  %u by matrix,  %u with neither\n",
                        g_node_translate, g_node_matrix, g_node_neither);
            g_node_translate = g_node_matrix = g_node_neither = 0;
            g_mat_loads = g_mat_max = g_mat_zero = g_mat_bigi = 0;
            g_mat_bigt = 0.0f;
        }
        total += g_ntris;

        next = rd32le(vp + 0x01 * 4) & 0x00FFFFFFu;
        if (next == vpaddr) break;
        vpaddr = next;
    }
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
    if (g_texstats > 0) tex_stats_field();
    if (!fb) g_sink->end_frame();
    if (fb && getenv("M3_REAL3D_STATS")) {
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

/* The texture sheet is built from uploads the guest made long ago, so it is
 * state too: restoring RAM without it leaves every surface wrong. */
void real3d_state(m3_state_t *st)
{
    if (g_sheet_shift) return;     /* a reduced sheet is not saved */
    if (!g_sheet) g_sheet = (uint16_t *)calloc(TEX_W * TEX_H, sizeof(uint16_t));
    if (!g_sheet_set) g_sheet_set = (uint8_t *)calloc(TEX_W * TEX_H, 1);
    if (!g_sheet || !g_sheet_set) return;
    m3_state_io(st, g_sheet, (size_t)TEX_W * TEX_H * sizeof(uint16_t));
    m3_state_io(st, g_sheet_set, (size_t)TEX_W * TEX_H);
    M3_STATE_VAR(st, g_fifo_seen);
    M3_STATE_VAR(st, g_tex_uploads);
}
