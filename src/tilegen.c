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

/* A layer the guest has switched off.
 *
 * The tilegen's registers are on the PCI side, so a word read back here is
 * reversed from the way the guest stored it -- 0x7F010000 written is 0x17F
 * seen, which is 383, the last scanline of a 384-line display. That is what
 * fixes the byte order; the rest of this follows from it.
 *
 * Reversed, register 0x20 carries four enable bits at bit 9. That position is
 * derived, not documented: across a whole attract cycle the register takes
 * exactly four values, layer 0 is displayed under all of them and layer 2
 * under none, and bit 9 is the only base for four consecutive bits where
 * layer 0's is always set and layer 2's never is.
 *
 * It is worth being clear about how weak that is. The field does not change
 * once during the run, so this is fitted to a single observation -- "layers 0
 * and 3, always" -- and any game state that switches a layer on will either
 * confirm the position or move it. What it is not is a guess: every other
 * position is ruled out by the values the register actually takes.
 *
 * Getting it wrong is not subtle, which is the one piece of luck here. Layer
 * 2 spends attract mode holding a solid full-screen rectangle -- one tile of
 * flat colour, repeated 2492 times -- and a renderer that draws it paints
 * that rectangle over the entire picture.
 */
static int layer_enabled(int layer)
{
    uint32_t r20 = bus_tilegen_regs()[0x20 / 4];
    r20 = ((r20 & 0xFFu) << 24) | ((r20 & 0xFF00u) << 8) |
          ((r20 >> 8) & 0xFF00u) | ((r20 >> 24) & 0xFFu);
    return (r20 >> (9 + layer)) & 1u;
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
                      unsigned pal_base, int px, int py)
{
    int row, col;

    if (pattern + TILE_BYTES > VRAM_NAME_BASE)
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
            byte = v[pattern + (uint32_t)row * 4u + (uint32_t)(3 - (col >> 1))];
            idx = (col & 1) ? (byte & 0x0Fu) : (byte >> 4);
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

void tilegen_render(void)
{
    int fbw = 0, fbh = 0;
    uint32_t *fb = platform_framebuffer(&fbw, &fbh);
    const uint8_t *v = bus_vram(NULL);
    int layer;

    if (!fb || !v)
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
    for (layer = LAYERS - 1; layer >= 0; layer--) {
        uint32_t nt = VRAM_NAME_BASE + (uint32_t)layer * LAYER_STRIDE;
        int ty, tx;

        if (!g_enable_layer[layer] || !layer_enabled(layer))
            continue;

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
                tile = ((uint32_t)(e & 0x3FFFu) << 1) | (uint32_t)((e >> 15) & 1u);
                pattern = tile * TILE_BYTES;
                pal = e & 0x7FF0u;
                draw_tile(fb, fbw, fbh, v, pattern, pal, tx * TILE, ty * TILE);
            }
        }
    }
}
