/* model3recomp -- Model 3 tilemap generator.
 *
 * Four 8x8 tile layers over the 496x384 display: the service menu, the HUD,
 * and the flat parts of attract mode. Called once per field, before the
 * framebuffer is presented.
 */
#ifndef MODEL3RECOMP_TILEGEN_H
#define MODEL3RECOMP_TILEGEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One priority pass: the layers the guest has put behind the 3D, or the
 * ones it has put in front. */
void tilegen_render_pass(int above);

/* The same pass for a back end that composites itself (the Dreamcast's
 * PVR): the 496x384 picture as ARGB1555 into dst (row stride in texels),
 * alpha bit clear wherever no layer drew. Returns the pixels drawn, 0 when
 * the pass is empty -- the caller can then skip it. */
unsigned tilegen_render_pass_1555(int above, uint16_t *dst, unsigned stride);

/* Layers as textures of their maps (see tilegen.c): bring a layer's
 * 512x512 ARGB1555 map picture (stride 512) up to date, redrawing only the
 * tiles whose name changed unless force; returns the tiles redrawn. Then,
 * per screen line, the map position to show (scroll and line scroll
 * applied, x and y in 0..511), the stencil word (high half pair A, low half
 * pair B, a bit per 32 pixels, set = the pair's first layer), whether a
 * layer is drawn in this pass, and whether a colour offset is set (the map
 * pictures cannot show one). */
unsigned tilegen_update_map_1555(int layer, uint16_t *dst, int force);
void tilegen_line_scroll(int layer, int y, int *x, int *ly);
uint32_t tilegen_stencil(int y);
int tilegen_layer_drawn(int layer, int above);
int tilegen_colour_offset(void);
/* Separately filtering map rectangles is pixel-equivalent to filtering the
 * composed screen pass only while one layer pair occupies each priority
 * plane and no active layer uses per-line scroll. */
int tilegen_map_filter_safe(void);

/* For bring-up: isolate a layer when working out which one holds what. */
void tilegen_enable_layer(int layer, int on);

#ifdef __cplusplus
}
#endif
#endif
