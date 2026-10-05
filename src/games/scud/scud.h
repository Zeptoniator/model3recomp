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

/* The operator settings this port wants, applied once a field from the frame
 * hook -- what an operator would set in the test menu.
 *
 * LINK ID is the one that matters. Scud Race's Twin/DX set defaults to
 * MASTER when it finds a blank EEPROM (field 76: 0x32C8 writes 1 to
 * 0x0010011E), then looks for the network board, says "CANCELLED / NETWORK
 * BOARD NOT PRESENT" and starts over, for ever. SINGLE (0) is a cabinet on
 * its own. The byte is the one the settings screen prints (0x51614 indexes
 * the SINGLE/MASTER/SLAVE table at 0x8AB8C with it). */
#define SCUD_LINK_ID      0x0010011Eu
#define SCUD_LINK_SINGLE  0u
void scud_operator_settings(void);

#endif
