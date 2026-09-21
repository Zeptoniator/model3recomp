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
#define VRAM_NAME_BASE    0x0F8000u
#define VRAM_PALETTE      0x100000u
#define VRAM_END          0x120000u

#define LAYERS       4
#define LAYER_STRIDE 0x2000u          /* 64 x 64 entries of 16 bits */
#define MAP_W        64
#define TILE         8
#define TILE_BYTES   32u              /* 8x8 at 4bpp */

static int g_enable_layer[LAYERS] = {1, 1, 1, 1};

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

/* And four bits of register 0x20 say how deep each layer's pixels are: set
 * means four bits per pixel, clear means eight.
 *
 * This was derived wrongly once, as an enable rather than a depth, from a
 * register that takes only four values across a whole attract cycle. The
 * fit was unique and it was still wrong, because the thing it was fitted to
 * -- "layer 0 draws and layer 2 does not" -- has a different cause. What
 * settles it is that the values agree with what the screens plainly are:
 * 0xF300 during the boot report says all four layers are four-bit, and the
 * report is text; 0x3300 during attract says layers 0 and 1 are four-bit
 * and 2 and 3 are eight, and that is text over a photograph.
 */
static int layer_4bpp(int layer)
{
    return (reg(0x20) >> (8 + layer)) & 1u;
}

/* And four more say whether the layer sits in front of the 3D or behind it.
 *
 * These two fields share a byte and were conflated at first, and then
 * swapped, because through most of attract mode both nibbles read 3 and
 * either assignment explains the picture. What separates them is 0xF300,
 * which the guest sets while a photographic sky is on screen: the low
 * nibble is 3, so layers 2 and 3 are eight-bit, and they have to be, or
 * the sky comes out as stripes. The high nibble is F, so everything is in
 * front of the 3D, which costs nothing on a screen with no 3D on it. */
static int layer_above_3d(int layer)
{
    return (reg(0x20) >> (12 + layer)) & 1u;
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

/* One 8x8 4bpp tile. A row is four bytes read as one little-endian word with
 * the leftmost pixel in the top nibble, so the byte the leftmost pixel lives
 * in is the last of the four. Read the row the other way round and every
 * glyph comes out mirrored -- which is invisible on T, H, O and X, and is why
 * this was worth checking against letters that are not symmetric.
 */
static void draw_tile(uint32_t *fb, int fbw, int fbh,
                      const uint8_t *v, uint32_t pattern,
                      unsigned pal_base, int px, int py, int four_bit)
{
    int row, col;
    uint32_t span = four_bit ? TILE_BYTES : TILE_BYTES * 2u;

    if (pattern + span > VRAM_NAME_BASE)
        return;

    for (row = 0; row < TILE; row++) {
        int y = py + row;
        if (y < 0 || y >= fbh)
            continue;
        for (col = 0; col < TILE; col++) {
            int x = px + col;
            uint8_t byte;
            unsigned idx;
            uint32_t argb;
            int transparent;
            if (x < 0 || x >= fbw)
                continue;
            if (four_bit) {
                byte = v[pattern + (uint32_t)row * 4u
                                 + (uint32_t)(3 - (col >> 1))];
                idx = (col & 1) ? (byte & 0x0Fu) : (byte >> 4);
            } else {
                /* Eight bits a pixel: the tile is twice as long and the
                 * pixel is the whole byte. The byte order inside each
                 * 32-bit word is reversed, same as the 4bpp case. */
                byte = v[pattern + (uint32_t)row * 8u + (uint32_t)(col ^ 3)];
                idx = byte;
            }
            argb = palette_argb(v, pal_base + idx, &transparent);
            if (transparent)
                continue;
            fb[y * fbw + x] = argb;
        }
    }
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

void tilegen_render_pass(int above)
{
    int fbw = 0, fbh = 0;
    uint32_t *fb = platform_framebuffer(&fbw, &fbh);
    const uint8_t *v = bus_vram(NULL);
    int layer;

    if (!fb || !v)
        return;

    /* M3_NO_2D: draw the 3D on its own. The tilemaps are opaque over most of
     * the screen and sit above the 3D during a round, so this is the only
     * way to see what the Real3D actually produced. */
    if (getenv("M3_NO_2D"))
        return;

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
            fflush(stderr);
        }
    }

    for (layer = LAYERS - 1; layer >= 0; layer--) {
        uint32_t nt = VRAM_NAME_BASE + (uint32_t)layer * LAYER_STRIDE;
        int ty, tx;
        int four_bit;

        if (!g_enable_layer[layer] || !layer_enabled(layer))
            continue;
        if (layer_above_3d(layer) != (above != 0))
            continue;
        four_bit = layer_4bpp(layer);

        for (ty = 0; ty * TILE < fbh; ty++) {
            for (tx = 0; tx * TILE < fbw; tx++) {
                unsigned e = name_entry(v, nt, (unsigned)(ty * MAP_W + tx));
                uint32_t pattern, tile;
                unsigned pal;
                if (!e)
                    continue;
                /* Bits 14..1 are the tile number, and bit 15 is its low
                 * bit -- the number wraps around the top of the entry rather
                 * than running off it. Bits 14..4 are also the top eleven bits
                 * of the colour index, whose low four come from the pixel.
                 *
                 * The palette field is why the boot report is the wrong thing
                 * to check a decode against: its entries all carry palette
                 * 0x20, so a decode that loses the field entirely still draws
                 * the report in one flat colour that looks deliberate. */
                if (four_bit) {
                    tile = ((uint32_t)(e & 0x3FFFu) << 1)
                         | (uint32_t)((e >> 15) & 1u);
                    pattern = tile * TILE_BYTES;
                    pal = e & 0x7FF0u;
                } else {
                    /* Eight-bit: the whole low fifteen bits are the pattern
                     * number and a tile is 64 bytes, so the byte offset
                     * comes out the same. The palette is the top seven of
                     * those bits and the pixel supplies the low eight. */
                    pattern = (uint32_t)(e & 0x7FFFu) * (TILE_BYTES * 2u);
                    pal = e & 0x7F00u;
                }
                draw_tile(fb, fbw, fbh, v, pattern, pal, tx * TILE, ty * TILE,
                          four_bit);
            }
        }
    }
}
