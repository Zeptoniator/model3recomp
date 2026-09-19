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

/* Triangles submitted by the last frame, for the harness. */
unsigned real3d_last_tri_count(void);

#ifdef __cplusplus
}
#endif
#endif /* MODEL3RECOMP_REAL3D_H */
