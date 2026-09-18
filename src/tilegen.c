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

/* A name-table entry. Two of them share each 32-bit word, and the word holds
 * them in little-endian order -- so the tile drawn first is the one in the
 * second halfword. Each entry is itself big-endian, which is why this is a
 * halfword swap and not a bswap32. Read it the other way round and every pair
 * of characters on screen trades places: "THE LOST WORLD" comes out as
 * "T.EHL SO TOWLR.D", which is legible enough to look like a font problem.
 */
static inline unsigned name_entry(const uint8_t *v, uint32_t base, unsigned i)
{
    uint32_t off = base + (uint32_t)(i ^ 1u) * 2u;
    return ((unsigned)v[off] << 8) | v[off + 1];
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

    r = (c >> 10) & 0x1Fu;
    g = (c >> 5) & 0x1Fu;
    b = c & 0x1Fu;
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

void tilegen_render(void)
{
    int fbw = 0, fbh = 0;
    uint32_t *fb = platform_framebuffer(&fbw, &fbh);
    const uint8_t *v = bus_vram(NULL);
    int layer;

    if (!fb || !v)
        return;

    memset(fb, 0, (size_t)fbw * (size_t)fbh * sizeof(uint32_t));

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

        if (!g_enable_layer[layer])
            continue;

        for (ty = 0; ty * TILE < fbh; ty++) {
            for (tx = 0; tx * TILE < fbw; tx++) {
                unsigned e = name_entry(v, nt, (unsigned)(ty * MAP_W + tx));
                uint32_t pattern;
                unsigned pal;
                if (!e)
                    continue;
                /* Tile number is the top nine bits: the boot report's entries
                 * are the character's ASCII code shifted up by seven, and the
                 * font is laid out so tile N is character N.
                 *
                 * ponytail: the low seven bits are taken as the palette
                 * block, which every entry seen so far leaves at zero, so it
                 * is the natural reading rather than a verified one. Upgrade
                 * path: a screen that uses more than one palette block. */
                pattern = (uint32_t)(e >> 7) * TILE_BYTES;
                pal = (e & 0x7Fu) * 16u;
                draw_tile(fb, fbw, fbh, v, pattern, pal, tx * TILE, ty * TILE);
            }
        }
    }
}
