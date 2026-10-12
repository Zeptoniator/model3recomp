/* Scud Race's pure wait loops (lift.h, M3_PURE_LOOP), by the address of
 * their backward branch:
 *   0x3072C  spins until the decrementer goes negative
 *   0x3074C  waits for the field interrupt to change a counter
 *   0xAEBC   waits for an interrupt to clear a busy byte
 *   0xAF7C   waits 33 time base ticks
 *   0xB430, 0xB44C, 0xB450   0xB420's wait for a time base value
 * Each reads only the decrementer, the time base or RAM that an interrupt
 * handler writes; none writes memory. */
#ifndef SCUD_GAME_LOOPS_H
#define SCUD_GAME_LOOPS_H
#define M3_PURE_LOOP(a) ((uint32_t)(a) == 0x0003072Cu || (uint32_t)(a) == 0x0003074Cu || \
                         (uint32_t)(a) == 0x0000AEBCu || (uint32_t)(a) == 0x0000AF7Cu || \
                         (uint32_t)(a) == 0x0000B430u || (uint32_t)(a) == 0x0000B44Cu || \
                         (uint32_t)(a) == 0x0000B450u)
#endif
