/* model3recomp -- no sound board (the Dreamcast build).
 *
 * Exactly what sound.c does when it was given no ROMs, without the 68000 and
 * the SCSPs: those bring 1.1 MB of tables into BSS, which a 16 MB machine
 * cannot spare for a board it does not run. Identical guest-visible
 * behaviour matters: frame signatures from a desktop run with M3_NO_SOUND
 * and from this build must agree. */
#include "model3recomp/sound.h"
#include "model3recomp/irq.h"
#include "model3recomp/savestate.h"

uint64_t g_board_us, g_wait_us;   /* read by the M3_PROFILE line */

void sound_init(const uint8_t *prog, size_t prog_size,
                const uint8_t *samples, size_t samples_size)
{
    (void)prog; (void)prog_size; (void)samples; (void)samples_size;
}

int sound_present(void) { return 0; }

uint8_t sound_uart_read(unsigned reg) { return reg ? 0x05 : 0; }   /* always ready */

/* The transmitter is always ready, so with its interrupt turned on (a
 * command with bit 5 set) the interrupt is simply asserted, and the guest
 * empties its command queue into nothing. Sending a byte drops the line
 * until the handler has returned (MAME): Scud's handler sends one byte and
 * then polls the status register until the bit clears. Raised only once a field, as it
 * was, the queue drained a byte a field: Scud Race's burst of sound
 * commands at the start of a race filled its 32-entry queue and the game
 * waited on it for ever (DC-BUG-015). */
void sound_uart_write(unsigned reg, uint8_t v)
{
    if (reg == 0)
        irq_ack(M3_IRQ_SOUND);
    else
        irq_set_level(M3_IRQ_SOUND, (v & 0x20) != 0);
}

void sound_run_field(void) { }

void sound_state(m3_state_t *st) { (void)st; }
