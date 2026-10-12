/* model3recomp -- Real3D scene renderer.
 *
 * The 3D half of the picture. Draws before the tilemaps, which sit over it.
 *
 * READ docs/technical/real3d.md before changing the structure decoding in
 * real3d.c. None of those layouts is a guess -- each one was read off a live
 * scene dump and checked against something that could only come out right if
 * the decode was right -- and the file says which check per structure.
 */
#ifndef MODEL3RECOMP_REAL3D_H
#define MODEL3RECOMP_REAL3D_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Draw the scene into the framebuffer. Clears it; the tilemaps go on top. */
void real3d_render(void);
/* A field that is not drawn: the texture uploads the guest made in it are
 * still taken in, so the next drawn field has them. */
void real3d_skip(void);

/* The VROM texture port at 0x90000000: three registers, and the write to
 * the last one loads a texture that is already in VROM. */
void real3d_vrom_texture(uint32_t addr, uint32_t header, uint32_t ctrl);

/* Triangles submitted by the last frame, for the harness. */
unsigned real3d_last_tri_count(void);

/* A hardware back end. With no software framebuffer (the Dreamcast) and a
 * sink set, real3d_render() still walks the scene; every polygon, culled,
 * cut at the near plane and projected by the same code the software
 * rasteriser uses, goes to the sink instead of being rasterised. No
 * depth buffer, texture sheet or band pool is allocated in that mode.
 *
 * Coordinates are the board's 496x384 screen; `w` is 1/z (larger is
 * nearer, as the PVR wants it); colour is the flat-shaded ARGB the software
 * path would use. Opaque polygons come first, then translucent ones. */
typedef struct { float x, y, w, u, v; } m3_r3d_vertex;
/* A polygon's texture: a rectangle of the texture sheet in full-size
 * texels (x, y within page 0 or 1, w and h powers of two), the texel
 * format (real3d_texel_argb), mirroring per axis, and whether texel alpha
 * cuts it out. u and v of the vertices are in full-size texels, from the
 * rectangle's origin, unwrapped. */
typedef struct {
    unsigned x, y, page, w, h, fmt;
    int mirror_u, mirror_v;
    int alpha_test, tex_alpha;
} m3_r3d_texture;
typedef struct {
    void (*begin_frame)(void);
    /* tex is NULL for an untextured polygon. */
    void (*polygon)(const m3_r3d_vertex *v, int n, uint32_t argb,
                    int translucent, const m3_r3d_texture *tex);
    void (*end_frame)(void);
    /* An upload has written the sheet: anything made from it is stale. */
    void (*sheet_changed)(void);
    /* A viewport starts; the polygons that follow, up to the next call,
     * belong to it. It draws only inside x0 <= x <= x1, y0 <= y <= y1;
     * outside it a polygon's w can be anything, up to 1/0.05 (the near
     * plane). Viewports come lowest priority (`layer`, 0..3) first. Those
     * of one priority are one scene and share a depth buffer; a higher
     * priority covers a lower one whatever their depths (the software path
     * clears its depth buffer when the priority changes). */
    void (*begin_viewport)(unsigned layer,
                           float x0, float y0, float x1, float y1);
} m3_r3d_sink;
void real3d_set_sink(const m3_r3d_sink *sink);

/* Where uploads go when there is no software framebuffer (and so no sheet
 * of real3d.c's own): a 2048x2048 grid of 16-bit texels, two pages of
 * 1024 rows stacked, stored at 1/(1 << shift) size each way (point
 * sampled: a texel is kept as it is, whatever its format). The Dreamcast
 * keeps it in PVR memory at shift 1, 2 MB. Row stride is 2048 >> shift. */
void real3d_set_sheet(uint16_t *texels, unsigned shift);

/* A raw 16-bit texel as 0xAARRGGBB in one of the twelve formats. */
uint32_t real3d_texel_argb(unsigned fmt, unsigned v);

#ifdef __cplusplus
}
#endif
#endif /* MODEL3RECOMP_REAL3D_H */
