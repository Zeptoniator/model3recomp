/* model3recomp -- the sound board: a 68000, two SCSPs, and the UART that
 * joins it to the PowerPC.
 *
 * The 68000 is interpreted (Musashi), not recompiled: it is a separate
 * 11.3 MHz processor running a 512 KB driver, and an interpreter costs a
 * few per cent of one core. The board runs once a field, in step with the
 * guest -- a field's worth of 44.1 kHz samples, 256 68000 cycles each -- so
 * it is as deterministic as the rest of the machine: what the PowerPC reads
 * back from it depends only on the field count and what it was sent.
 */
#ifndef MODEL3RECOMP_SOUND_H
#define MODEL3RECOMP_SOUND_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* prog: the 68000 program (big-endian, 512 KB); samples: the wave ROM
 * (big-endian, a multiple of 8 MB). Either may be NULL: no sound board. */
void     sound_init(const uint8_t *prog, size_t prog_size,
                    const uint8_t *samples, size_t samples_size);
int      sound_present(void);

/* The PowerPC side: 0xF0080000 data, 0xF0080004 status / control. */
uint8_t  sound_uart_read(unsigned reg);
void     sound_uart_write(unsigned reg, uint8_t v);

/* Run the board for one field and hand its audio to the platform. */
void     sound_run_field(void);

/* A board whose voices are played by other hardware (the Dreamcast's
 * AICA), set before sound_init*(). chip[] are the two chips'
 * scsp_voices_t (src/scsp.h). Only the first `local_ram` bytes of each
 * chip's 512 KB RAM are kept here -- the driver's own variables; the rest
 * is samples, which it writes and the voices read, so its bytes go to
 * ram_write8 and live only over there (ram_read8 for the rare read back).
 * field() is called after each field, to flush what the far side buffers.
 * A board with voices elsewhere is always decoupled (M3_SND_DECOUPLED). */
typedef struct {
    const void *chip[2];
    void    (*ram_write8)(int chip, uint32_t addr, uint8_t v);
    uint8_t (*ram_read8)(int chip, uint32_t addr);
    void    (*field)(void);
} sound_voices_t;
/* Writes below mirror_from (inside local_ram) are the driver's own
 * variables and stay here: no voice plays from there, and the driver
 * writes them all the time. Scud Race: 0x4000 (its second chip plays from
 * 0x4000 up). */
void     sound_set_voices(const sound_voices_t *v, uint32_t local_ram, uint32_t mirror_from);

/* The 68000 program and the sample ROM read on demand: `read` fills dst
 * from offset off of a file that is the program (prog_size bytes) followed
 * by the sample ROM. Pages are cached; see sound.c. */
void     sound_init_paged(size_t (*read)(void *ctx, uint32_t off, void *dst, size_t len),
                          void *ctx, size_t prog_size, size_t samples_size);

#ifdef __cplusplus
}
#endif
#endif
