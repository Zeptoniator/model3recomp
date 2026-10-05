/* Scud Race / Sega Super GT -- what is specific to the title, and nothing
 * else: which board it wants, where it keeps its ROM images, and the lifted
 * code's two halves. Gameplay is never recoded here; it is the lifted
 * PowerPC program. */
#ifndef SCUD_H
#define SCUD_H

#include "model3recomp/model3recomp.h"

/* MAME `scud` (833-13041, ROM board 834-13042): Step 1.5, the 53C810 at
 * 0xF9000000 rather than The Lost World's 0xC1000000. See
 * docs/scud/BOARD_PROFILE.md for where each value came from. */
#define SCUD_SCSI_BASE 0xF9000000u

/* Fill the parts of a board config that come from the title. ROM pointers
 * are the caller's: a desktop reads files, a Dreamcast streams. */
void scud_board_config(m3_config_t *cfg);

/* Register both lifted halves with the function table. */
void scud_register_lifted(void);

#endif
