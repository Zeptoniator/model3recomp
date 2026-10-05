/* model3recomp -- the video board's JTAG chain.
 *
 * The Real3D's ASICs and its 3D-RAMs are daisy-chained on one IEEE 1149.1
 * test access port, driven by the CPU through the system controller:
 * register 0x0C (write) carries TCK, TMS, TDI and TRST, register 0x10 (read)
 * returns TDO. Games read every ASIC's IDCODE at boot and refuse to go on if
 * they are not the ones the board should have -- Scud Race stops with
 * "ERROR : (1A) VIDEO BOARD ABNORMAL STATUS". They also write each ASIC's
 * MODEWORD, which configures the renderer.
 */
#ifndef MODEL3RECOMP_JTAG_H
#define MODEL3RECOMP_JTAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build the chain for a board Step (0x10, 0x15, 0x20, 0x21) and reset it. */
void     jtag_init(unsigned step);

/* One write of system register 0x0C, as the guest wrote it (a byte). */
void     jtag_write(uint8_t v);

/* TDO, as the guest reads it from system register 0x10 (bit 5). */
uint8_t  jtag_read(void);

/* The last MODEWORD written to the first ASIC of a kind, or 0. Kinds:
 * 0 Jupiter, 1 Mercury, 2 Venus, 3 Earth, 4 Mars. */
uint32_t jtag_modeword(unsigned kind);

#ifdef __cplusplus
}
#endif
#endif
