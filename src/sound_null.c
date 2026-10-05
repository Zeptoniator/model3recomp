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

void sound_uart_write(unsigned reg, uint8_t v)
{
    if (reg == 0 || !(v & 0x20))
        irq_ack(M3_IRQ_SOUND);
}

void sound_run_field(void) { }

void sound_state(m3_state_t *st) { (void)st; }
