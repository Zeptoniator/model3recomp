/* model3recomp -- Model 3 tilemap generator.
 *
 * Four scrolling 8x8 tile layers over a 496x384 display. This draws the 2D
 * half of the picture: the service menu, the boot report, and the flat parts
 * of attract mode.
 *
 * VRAM is one block at 0xF1000000 and the layout below was read off a live
 * dump, then checked by rendering text whose wording is known in advance --
 * the boot report reads "THE LOST WORLD / Copyright (c) 1997 SEGA Enterprises
 * CO.,LTD". A decode that produces those words is right; one that produces
 * plausible-looking noise is not, and several did.
 *
 *     0x000000..0x0F5FFF   tile pattern data
 *     0x0F6000..0x0F7FFF   scroll tables
 *     0x0F8000..0x0FFFFF   four name tables, 0x2000 each
 *     0x100000..0x11FFFF   palette, 32768 entries of 32 bits
 *
 * The scroll tables are not name tables, which is what the previous version
 * of this file assumed. The boot fills 0x0F6000 with the entry 0x0080 and
 * 0x0F7000 with 0xFFFF -- visible as two of the memsets it issues -- so
 * reading them as a map draws one tile over the whole screen, forever.
 *
 * Everything the graphics side reads is little-endian, because the Real3D and
 * its tile generator sit on the PCI side of the board; the PowerPC is not.
 * That one fact explains all three of the orderings below, and getting any of
 * them wrong still produces a picture, just not the right one.
 */
#include "model3recomp/tilegen.h"
#include "model3recomp/bus.h"
#include "model3recomp/platform.h"
#include "model3recomp/model3recomp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define VRAM_PATTERN      0x000000u
#define VRAM_LINESCROLL   0x0F6000u
#define VRAM_STENCIL      0x0F7000u
#define VRAM_NAME_BASE    0x0F8000u
#define VRAM_PALETTE      0x100000u
#define VRAM_END          0x120000u

#define LAYERS       4
#define LAYER_STRIDE 0x2000u          /* 64 x 64 entries of 16 bits */
#define MAP_W        64
#define TILE         8
#define TILE_BYTES   32u              /* 8x8 at 4bpp */

static int g_enable_layer[LAYERS] = {1, 1, 1, 1};
static unsigned g_layer_mask = 0xFu;

void tilegen_enable_layer(int layer, int on)
{
    if (layer >= 0 && layer < LAYERS)
        g_enable_layer[layer] = on;
}

/* The tilegen's registers are on the PCI side, so a word read back here is
 * reversed from the way the guest stored it -- 0x7F010000 written is 0x17F
 * seen, which is 383, the last scanline of a 384-line display. */
static uint32_t reg(unsigned idx)
{
    uint32_t v = bus_tilegen_regs()[idx / 4];
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}

/* A layer is off when the top bit of its scroll register is clear. */
static int layer_enabled(int layer)
{
    return (reg(0x60u + (unsigned)layer * 4u) >> 31) & 1u;
}

/* Register 0x20: bits 12..15 say a layer is four bits a pixel (clear means
 * eight), bits 8..11 that it is drawn in front of the 3D. This follows
 * Supermodel, and it was the other way round here for a while: through most
 * of attract both nibbles read 3, so the two orders agree. They part at
 * 0x3200, which Lost World sets for its title logo and its credits roll --
 * read backwards, layer 0 decodes as eight-bit and both screens are noise. */
static int layer_4bpp(int layer)
{
    return (reg(0x20) >> (12 + layer)) & 1u;
}

static int layer_above_3d(int layer)
{
    return (reg(0x20) >> (8 + layer)) & 1u;
}

/* A name-table entry. Two of them share each 32-bit word, and the whole word
 * is little-endian -- so the tile drawn first is the one in the second
 * halfword, and the halfword itself is byte-swapped too. Get the pair order
 * wrong and every two characters trade places: "THE LOST WORLD" comes out as
 * "T.EHL SO TOWLR.D", which is legible enough to look like a font problem.
 */
static inline unsigned name_entry(const uint8_t *v, uint32_t base, unsigned i)
{
    uint32_t off = base + (uint32_t)(i ^ 1u) * 2u;
    return ((unsigned)v[off + 1] << 8) | v[off];
}

/* Palette entry: a little-endian 32-bit word whose low 16 bits are the
 * colour, 1-5-5-5 with bit 15 meaning transparent. */
static uint32_t palette_argb(const uint8_t *v, unsigned index, int *transparent)
{
    uint32_t off = VRAM_PALETTE + index * 4u;
    unsigned c;
    uint32_t r, g, b;

    *transparent = 1;
    if (off + 4u > VRAM_END)
        return 0;

    /* Bytes 0 and 1 of the big-endian word are the low half once reversed. */
    c = ((unsigned)v[off + 1] << 8) | v[off];
    if (c & 0x8000u)
        return 0;
    *transparent = 0;

    /* Blue is the top field and red the bottom, not the other way round. A
     * palette read the wrong way round still produces a picture in plausible
     * colours -- it only ever swaps red and blue, so greys and the boot
     * report's flat white come out identical either way, and the attract
     * screen's orange text comes out as the exact same blue every time.
     * 0x00A5FF reversed is 0xFFA500, which is how this was caught. */
    b = (c >> 10) & 0x1Fu;
    g = (c >> 5) & 0x1Fu;
    r = c & 0x1Fu;
    /* 5 bits to 8, replicating the high bits so 0x1F maps to 0xFF. */
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* The palette index of one pixel of a layer, scroll already applied: (sx, sy)
 * is a position in the layer's 512x512 map. A 4bpp row is four bytes read as
 * one little-endian word with the leftmost pixel in the top nibble, so the
 * byte the leftmost pixel lives in is the last of the four -- read the other
 * way round every glyph comes out mirrored, invisible on T, H, O and X. An
 * 8bpp row is two such words, a byte a pixel, same reversal.
 *
 * Name entries: in 4bpp, bits 14..1 are the tile number and bit 15 its low
 * bit, and bits 14..4 are also the top of the colour index. In 8bpp the low
 * fourteen bits are the tile (64 bytes each) and bits 14..8 the palette. The
 * palette field is why the boot report is the wrong thing to check a decode
 * against: all its entries carry palette 0x20. */
static int layer_pixel(const uint8_t *v, int layer, int four_bit,
                       unsigned sx, unsigned sy)
{
    uint32_t nt = VRAM_NAME_BASE + (uint32_t)layer * LAYER_STRIDE;
    unsigned e = name_entry(v, nt, (sy / TILE) * MAP_W + sx / TILE);
    unsigned row = sy & 7u, col = sx & 7u;
    uint32_t pattern;

    if (!e)
        return -1;
    if (four_bit) {
        uint8_t byte;
        pattern = (((uint32_t)(e & 0x3FFFu) << 1) | ((e >> 15) & 1u))
                * TILE_BYTES;
        byte = v[pattern + row * 4u + (3u - (col >> 1))];
        return (int)((e & 0x7FF0u) | ((col & 1u) ? (byte & 0x0Fu) : (byte >> 4)));
    }
    pattern = (uint32_t)(e & 0x3FFFu) * (TILE_BYTES * 2u);
    return (int)((e & 0x7F00u) | v[pattern + row * 8u + (col ^ 3u)]);
}

static uint32_t le32(const uint8_t *v, uint32_t off)
{
    return (uint32_t)v[off] | ((uint32_t)v[off + 1] << 8) |
           ((uint32_t)v[off + 2] << 16) | ((uint32_t)v[off + 3] << 24);
}

/* Registers 0x40 (layers 0/1) and 0x44 (2/3): signed 8-bit offsets added to
 * every pixel's colour, at twice the palette's resolution. Fades and the
 * lightning flashes in attract are done with these. */
static uint32_t offset_colour(uint32_t argb, uint32_t r)
{
    int o[3], c[3], i;
    o[0] = (int8_t)(r & 0xFFu) * 2;           /* red */
    o[1] = (int8_t)((r >> 8) & 0xFFu) * 2;    /* green */
    o[2] = (int8_t)((r >> 16) & 0xFFu) * 2;   /* blue */
    c[0] = (int)((argb >> 16) & 0xFFu);
    c[1] = (int)((argb >> 8) & 0xFFu);
    c[2] = (int)(argb & 0xFFu);
    for (i = 0; i < 3; i++) {
        c[i] += o[i];
        if (c[i] < 0) c[i] = 0; else if (c[i] > 255) c[i] = 255;
    }
    return 0xFF000000u | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | (uint32_t)c[2];
}

#ifdef M3_TILEGEN_DUMP
/* The registers and the layer occupancy, printed whenever register 0x20
 * changes. Printing it per field is useless -- it changes four times in an
 * attract cycle, and it was reading it on change that showed the enable bits
 * do not move at all. */
static void dump_state(const uint8_t *v)
{
    const uint32_t *rg = bus_tilegen_regs();
    unsigned i, L;
    fprintf(stderr, "[tilegen] f=%llu r20=%08X r40=%08X r44=%08X "
                    "scroll=%08X/%08X/%08X/%08X",
            (unsigned long long)model3recomp_frame_count(),
            rg[0x20/4], rg[0x40/4], rg[0x44/4],
            rg[0x60/4], rg[0x64/4], rg[0x68/4], rg[0x6C/4]);
    (void)i;
    for (L = 0; L < LAYERS; L++) {
        uint32_t b = VRAM_NAME_BASE + L * LAYER_STRIDE;
        unsigned nz = 0, first = 0xFFFFu, last = 0;
        for (i = 0; i < LAYER_STRIDE / 2u; i++)
            if (name_entry(v, b, i)) {
                nz++; if (first == 0xFFFFu) first = i; last = i;
            }
        if (nz)
            fprintf(stderr, "  L%u=%u(r%u-%u)", L, nz, first / MAP_W, last / MAP_W);
    }
    fprintf(stderr, "%s", "\n");
    fflush(stderr);
}
#endif

static unsigned render_pass(int above, uint32_t *fb, int fbw, int fbh,
                            uint16_t *t16, unsigned stride);

void tilegen_render_pass(int above)
{
    int fbw = 0, fbh = 0;
    uint32_t *fb = platform_framebuffer(&fbw, &fbh);
    if (fb)
        render_pass(above, fb, fbw, fbh, NULL, 0);
}

static int g_tg_slow;      /* render_pass: the per-pixel path even for 1555 */

/* M3_TG_CHECK=1: every pass drawn both ways and compared, the first lines
 * that differ reported -- the fast path is only an optimisation if it draws
 * the same picture. */
unsigned tilegen_render_pass_1555(int above, uint16_t *dst, unsigned stride)
{
    static int check = -1;
    unsigned n;
    if (check < 0) check = getenv("M3_TG_CHECK") != NULL;
    n = render_pass(above, NULL, 496, 384, dst, stride);
    if (check) {
        static uint16_t *ref;
        static unsigned bad;
        unsigned m, y;
        if (!ref) ref = malloc((size_t)stride * 384u * 2u);
        if (!ref) return n;
        g_tg_slow = 1;
        m = render_pass(above, NULL, 496, 384, ref, stride);
        g_tg_slow = 0;
        for (y = 0; y < 384; y++)
            if (memcmp(ref + (size_t)y * stride, dst + (size_t)y * stride, 496u * 2u)) {
                if (bad++ < 20)
                    fprintf(stderr, "[tgcheck] field %llu pass %d line %u differs\n",
                            (unsigned long long)model3recomp_frame_count(), above, y);
                break;
            }
        if (m != n && bad++ < 20)
            fprintf(stderr, "[tgcheck] field %llu pass %d: %u pixels, %u slow\n",
                    (unsigned long long)model3recomp_frame_count(), above, n, m);
        {
            static unsigned passes;
            if (++passes % 2000u == 0)
                fprintf(stderr, "[tgcheck] %u passes compared, %u differ, "
                        "%u pixels in the last\n", passes, bad, n);
        }
    }
    return n;
}

/* Which name entries draw nothing at all: every one of the 64 pixels of the
 * tile, through the entry's palette, transparent. Scud Race fills every
 * name of every layer -- the HUD's empty areas are a real tile of
 * transparent colours -- so "skip the zero entry" skipped nothing. Per
 * entry value and depth, two bits (known, clear); forgotten whenever a
 * pattern or the palette changes (bus_tilegen_content_generation). */
static uint8_t g_tile_known[2][8192], g_tile_clear[2][8192];
static uint32_t g_tile_gen = 0xFFFFFFFFu;

static int tile_clear(const uint8_t *v, unsigned e, int four_bit)
{
    unsigned b = e >> 3, m = 1u << (e & 7u), row, col;
    int d = four_bit ? 1 : 0;
    if (g_tile_known[d][b] & m)
        return (g_tile_clear[d][b] & m) != 0;
    g_tile_known[d][b] |= (uint8_t)m;
    for (row = 0; row < 8u; row++)
        for (col = 0; col < 8u; col++) {
            unsigned idx;
            uint32_t off;
            if (four_bit) {
                uint32_t pattern = (((uint32_t)(e & 0x3FFFu) << 1) | ((e >> 15) & 1u))
                                 * TILE_BYTES;
                uint8_t byte = v[pattern + row * 4u + (3u - (col >> 1))];
                idx = (e & 0x7FF0u) | ((col & 1u) ? (byte & 0x0Fu) : (byte >> 4u));
            } else {
                uint32_t pattern = (uint32_t)(e & 0x3FFFu) * (TILE_BYTES * 2u);
                idx = (e & 0x7F00u) | v[pattern + row * 8u + (col ^ 3u)];
            }
            off = VRAM_PALETTE + idx * 4u;
            if (off + 4u <= VRAM_END && !(v[off + 1] & 0x80u))
                return 0;                      /* an opaque pixel */
        }
    g_tile_clear[d][b] |= (uint8_t)m;
    return 1;
}

/* One pair of layers on one line, into a 1555 line buffer -- what the
 * per-pixel loop in render_pass() does, a tile at a time: the name entry
 * and pattern row are fetched once per tile, an empty tile is skipped
 * whole, and the palette goes straight to 1555 (the same bits the 8888
 * round trip gave). Drawing the HUD whole every field was half of a race
 * field on the Dreamcast. Not for a colour offset (fades): that keeps the
 * slow path. */
static unsigned pair_line_1555(const uint8_t *v, int pair, unsigned lmask,
                               const int want[2], const int four[2],
                               const int lx[2], const int ly[2], uint16_t *line)
{
    unsigned drawn = 0;
    int x = 0;
    while (x < 496) {
        int blk_end = (x | 31) + 1;
        int k = ((lmask >> (15 - x / 32)) & 1u) ? 0 : 1;
        uint32_t nt;
        unsigned sy, row, maprow;
        if (blk_end > 496) blk_end = 496;
        if (!want[k]) { x = blk_end; continue; }
        nt = VRAM_NAME_BASE + (uint32_t)(pair * 2 + k) * LAYER_STRIDE;
        sy = (unsigned)ly[k] & 511u;
        row = sy & 7u;
        maprow = (sy / TILE) * MAP_W;
        while (x < blk_end) {
            unsigned sx = (unsigned)(x + lx[k]) & 511u;
            unsigned col = sx & 7u, e = name_entry(v, nt, maprow + sx / TILE);
            int run = 8 - (int)col;
            if (x + run > blk_end) run = blk_end - x;
            if (!e || tile_clear(v, e, four[k])) { x += run; continue; }
            for (; run > 0; run--, x++, col++) {
                unsigned idx, c;
                uint32_t off;
                if (four[k]) {
                    uint32_t pattern = (((uint32_t)(e & 0x3FFFu) << 1) | ((e >> 15) & 1u))
                                     * TILE_BYTES;
                    uint8_t byte = v[pattern + row * 4u + (3u - (col >> 1))];
                    idx = (e & 0x7FF0u) | ((col & 1u) ? (byte & 0x0Fu) : (byte >> 4u));
                } else {
                    uint32_t pattern = (uint32_t)(e & 0x3FFFu) * (TILE_BYTES * 2u);
                    idx = (e & 0x7F00u) | v[pattern + row * 8u + (col ^ 3u)];
                }
                off = VRAM_PALETTE + idx * 4u;
                if (off + 4u > VRAM_END) continue;
                c = ((unsigned)v[off + 1] << 8) | v[off];
                if (c & 0x8000u) continue;
                line[x] = (uint16_t)(0x8000u | ((c & 0x1Fu) << 10) | (c & 0x03E0u)
                                   | ((c >> 10) & 0x1Fu));
                drawn++;
            }
        }
    }
    return drawn;
}

/* Into a 32-bit framebuffer, or (fb NULL) a 1555 target: each line built in
 * a line buffer and copied a word at a time, because the target may be
 * video memory that takes no byte writes. */
static unsigned render_pass(int above, uint32_t *fb, int fbw, int fbh,
                            uint16_t *t16, unsigned stride)
{
    const uint8_t *v = bus_vram(NULL);
    int x, y;
    unsigned drawn = 0;
    uint16_t line[512];

    if ((!fb && !t16) || !v)
        return 0;
    if (bus_tilegen_content_generation() != g_tile_gen) {
        g_tile_gen = bus_tilegen_content_generation();
        memset(g_tile_known, 0, sizeof g_tile_known);
        memset(g_tile_clear, 0, sizeof g_tile_clear);
    }

    /* M3_NO_2D: draw the 3D on its own. The tilemaps are opaque over most of
     * the screen and sit above the 3D during a round, so this is the only
     * way to see what the Real3D actually produced. */
    if (getenv("M3_NO_2D"))
        return 0;

    /* No clear here: real3d_render() has already drawn the 3D scene into
     * this buffer and cleared what it did not cover. The tilemaps go over
     * it, which is why they are drawn second. */

#ifdef M3_TILEGEN_DUMP
    {
        static uint32_t last = 0xFFFFFFFFu;
        uint32_t r20 = bus_tilegen_regs()[0x20 / 4];
        if (r20 != last) { last = r20; dump_state(v); }
    }
#endif


#ifdef M3_LOOP_GUARD
    /* What the renderer is actually handed, every so often. A black screen is
     * either an empty map or a decode that drops everything, and the two look
     * identical from the outside. */
    {
        static unsigned n;
        if ((n++ & 0x3Fu) == 0) {
            int L;
            fprintf(stderr, "[model3recomp] tilegen:");
            for (L = 0; L < LAYERS; L++) {
                uint32_t b = VRAM_NAME_BASE + (uint32_t)L * LAYER_STRIDE;
                unsigned nz = 0, i;
                for (i = 0; i < LAYER_STRIDE / 2u; i++)
                    if (name_entry(v, b, i)) nz++;
                fprintf(stderr, "  L%d=%u", L, nz);
            }
            fprintf(stderr, "\n");
        }
    }
#endif

    /* Back to front. Transparency comes from the palette rather than from
     * "index 0 on any layer but the bottom", so the bottom layer is not a
     * special case and a screen nothing wrote to stays black. */
    if (getenv("M3_TILEGEN_REGS")) {
        static uint32_t last = 0xFFFFFFFFu;
        const uint32_t *rg = bus_tilegen_regs();
        if (rg[0x20/4] != last) {
            last = rg[0x20/4];
            fprintf(stderr, "[tilegen] field %llu r20=%08X (swapped %08X)\n",
                    (unsigned long long)model3recomp_frame_count(),
                    rg[0x20/4],
                    ((rg[0x20/4] & 0xFFu) << 24) | ((rg[0x20/4] & 0xFF00u) << 8) |
                    ((rg[0x20/4] >> 8) & 0xFF00u) | ((rg[0x20/4] >> 24) & 0xFFu));
            { int L;
              for (L = 0; L < LAYERS; L++)
                  fprintf(stderr, "          L%d scroll %08X  enabled %d"
                                  "  4bpp %d  above3d %d\n",
                          L, reg(0x60u + (unsigned)L * 4u),
                          layer_enabled(L), layer_4bpp(L),
                          layer_above_3d(L)); }
            fflush(stderr);
        }
    }

    /* M3_TG_USAGE=1: how the game uses the layers, every 100 fields -- which
     * are on and in front of the 3D, their scroll and line scroll, how many
     * distinct stencil words the screen has, the colour offsets. What a
     * renderer that keeps each layer as a texture in map space must handle. */
    if (!above && getenv("M3_TG_USAGE") &&
        model3recomp_frame_count() % 100u == 0) {
        uint32_t words[8];
        unsigned nw = 0, yy, L;
        for (yy = 0; yy < 384; yy++) {
            uint32_t w = le32(v, VRAM_STENCIL + yy * 4u), k;
            for (k = 0; k < nw && words[k] != w; k++) ;
            if (k == nw && nw < 8) words[nw++] = w;
        }
        fprintf(stderr, "[tgusage] field %llu stencil %u word(s):",
                (unsigned long long)model3recomp_frame_count(), nw);
        for (yy = 0; yy < nw; yy++) fprintf(stderr, " %08X", words[yy]);
        fprintf(stderr, " coff %08X %08X |", reg(0x40), reg(0x44));
        for (L = 0; L < LAYERS; L++) {
            uint32_t sr = reg(0x60u + L * 4u), b = VRAM_NAME_BASE + L * LAYER_STRIDE;
            unsigned nz = 0, i;
            for (i = 0; i < LAYER_STRIDE / 2u; i++) if (name_entry(v, b, i)) nz++;
            fprintf(stderr, " L%u %s%s%s%s x%u y%u tiles %u", L,
                    layer_enabled((int)L) ? "on" : "off",
                    layer_above_3d((int)L) ? " front" : " back",
                    layer_4bpp((int)L) ? " 4bpp" : " 8bpp",
                    (sr & 0x8000u) ? " LINESCROLL" : "",
                    sr & 0x3FFu, (sr >> 16) & 0x1FFu, nz);
        }
        fprintf(stderr, "\n");
    }

    /* M3_LAYER_MASK=<bits>: draw only the layers whose bit is set. */
    {
        static int probed;
        if (!probed) { const char *e = getenv("M3_LAYER_MASK");
                       probed = 1;
                       if (e) g_layer_mask = (unsigned)strtoul(e, NULL, 0); }
    }

    /* Supermodel's model: the layers come in pairs, A/A' (0/1) and B/B'
     * (2/3), and per pixel only one of each pair shows. The stencil at
     * 0x0F7000 picks which, a word per screen line -- high half for A, low
     * for B, a bit per 32 pixels, set meaning the primary. The stencil does
     * not scroll; the layers do, each by its register (Y in bits 24..16, X
     * in 9..0), or per line from the table at 0x0F6000 when bit 15 is set.
     * Pair B is drawn first and A over it. */
    for (y = 0; y < fbh && y < 384; y++) {
        uint32_t mask_word = le32(v, VRAM_STENCIL + (uint32_t)y * 4u);
        int pair;
        if (t16) memset(line, 0, sizeof line);
        for (pair = 1; pair >= 0; pair--) {
            int lx[2], ly[2], want[2], four[2], k;
            unsigned lmask = pair ? (mask_word & 0xFFFFu) : (mask_word >> 16);
            uint32_t coff = reg(0x40u + (unsigned)pair * 4u);
            for (k = 0; k < 2; k++) {
                int L = pair * 2 + k;
                uint32_t sr = reg(0x60u + (unsigned)L * 4u);
                want[k] = g_enable_layer[L] && layer_enabled(L) &&
                          ((g_layer_mask >> L) & 1u) &&
                          layer_above_3d(L) == (above != 0);
                four[k] = layer_4bpp(L);
                ly[k] = (int)(((sr >> 16) & 0x1FFu) + (unsigned)y);
                if (sr & 0x8000u) {
                    uint32_t w = le32(v, VRAM_LINESCROLL + (uint32_t)L * 0x400u
                                         + (uint32_t)(y / 2) * 4u);
                    lx[k] = (int)((y & 1) ? (w & 0xFFFFu) : (w >> 16));
                } else {
                    lx[k] = (int)(sr & 0x3FFu);
                }
            }
            if (!want[0] && !want[1])
                continue;
            if (t16 && !coff && !g_tg_slow) {
                drawn += pair_line_1555(v, pair, lmask, want, four, lx, ly, line);
                continue;
            }
            for (x = 0; x < fbw && x < 496; x++) {
                int k = ((lmask >> (15 - x / 32)) & 1u) ? 0 : 1;
                int idx, transparent;
                uint32_t argb;
                if (!want[k])
                    continue;
                idx = layer_pixel(v, pair * 2 + k, four[k],
                                  (unsigned)(x + lx[k]) & 511u,
                                  (unsigned)ly[k] & 511u);
                if (idx < 0)
                    continue;
                argb = palette_argb(v, (unsigned)idx, &transparent);
                if (transparent)
                    continue;
                if (coff) argb = offset_colour(argb, coff);
                drawn++;
                if (fb)
                    fb[y * fbw + x] = argb;
                else
                    line[x] = (uint16_t)(0x8000u | ((argb >> 9) & 0x7C00u)
                                       | ((argb >> 6) & 0x03E0u)
                                       | ((argb >> 3) & 0x001Fu));
            }
        }
        if (t16) {
            const uint32_t *s32 = (const uint32_t *)line;
            uint32_t *d32 = (uint32_t *)(t16 + (size_t)y * stride);
            int k;
            for (k = 0; k < 496 / 2; k++) d32[k] = s32[k];
        }
    }
    return drawn;
}
