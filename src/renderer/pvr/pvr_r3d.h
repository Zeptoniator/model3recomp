/* The Real3D on the Dreamcast's PowerVR2: decoded polygons as PVR strips,
 * textured from a half-size copy of the texture sheet kept in PVR memory. */
#ifndef PVR_R3D_H
#define PVR_R3D_H

/* Initialise the PVR, put the sheet in its memory (real3d_set_sheet) and
 * register as the Real3D sink (real3d_set_sink). */
int  pvr_r3d_install(void);

/* Polygons submitted in the last frame, per list. */
unsigned pvr_r3d_last_opaque(void);
unsigned pvr_r3d_last_punch(void);
unsigned pvr_r3d_last_translucent(void);

/* Texture cache: converted so far, refused for want of room, KB in use. */
unsigned pvr_r3d_textures_made(void);
unsigned pvr_r3d_textures_failed(void);
unsigned pvr_r3d_texture_kb(void);

#endif
