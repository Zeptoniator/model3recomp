/* The Real3D on the Dreamcast's PowerVR2 (milestone DC-05: decoded
 * polygons on the PVR). Flat-shaded, untextured so far. */
#ifndef PVR_R3D_H
#define PVR_R3D_H

/* Initialise the PVR and register as the Real3D sink (real3d_set_sink). */
int  pvr_r3d_install(void);

/* Polygons submitted in the last frame, opaque and translucent. */
unsigned pvr_r3d_last_opaque(void);
unsigned pvr_r3d_last_translucent(void);

#endif
