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

/* For bring-up: isolate a layer when working out which one holds what. */
void tilegen_enable_layer(int layer, int on);

#ifdef __cplusplus
}
#endif
#endif
