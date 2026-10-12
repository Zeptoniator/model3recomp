/* model3recomp -- the sound board's voices on the AICA. See aica_voices.c. */
#ifndef MODEL3RECOMP_AICA_VOICES_H
#define MODEL3RECOMP_AICA_VOICES_H

#include "model3recomp/sound.h"

/* Resets the AICA's channels and returns the board's voice back end, for
 * sound_set_voices(). spu_init() (KOS) must have run. */
const sound_voices_t *aica_voices(void);
void aica_voices_report(void);

#endif
