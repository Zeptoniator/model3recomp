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
typedef struct {
    void (*begin_frame)(void);
    void (*polygon)(const m3_r3d_vertex *v, int n, uint32_t argb, int translucent);
    void (*end_frame)(void);
} m3_r3d_sink;
void real3d_set_sink(const m3_r3d_sink *sink);

#ifdef __cplusplus
}
#endif
#endif /* MODEL3RECOMP_REAL3D_H */
