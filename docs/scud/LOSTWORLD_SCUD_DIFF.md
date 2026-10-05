# The Lost World vs Scud Race

The runtime was brought up against The Lost World (`lostwsga`). Both are
Step 1.5 boards, so most of the machine is shared; this is what is not.
Sources as in BOARD_PROFILE.md.

| | The Lost World | Scud Race | Consequence for the runtime |
|---|---|---|---|
| Step / CPU | 1.5 / 100 MHz | 1.5 / 100 MHz | same; but `lift.h`'s work calibration says 66 MHz for both |
| Vector idiom | `li r7, n ; b handler` | `bl 0xFFF00000` | `rom_loader.py` accepted only the first (fixed) |
| Program EPROM lane order | first permutation tried happened to be right | needs the board order (IC 20→17) | lanes 2–3 were never checked (fixed) |
| Boot copy | table at `0xFFF00354`: CROM `0x0`→RAM, CROM `0x120004`→`0x100000` | program ROM `0xFFF10000..` → RAM `0x0` | interpreter's fixed-CROM mirror is harmless for Scud |
| Hand-off into RAM | `lis r0,0 ; mtlr r0 ; blr` | `mtspr SRR0,0x1540 ; rfi` | lifter needed `rfi` as a computed jump (fixed) |
| 53C810 window | `0xC1000000` | `0xF9000000` | was hard-wired (now `m3_config_t.scsi_base`) |
| Banked CROM | 4 groups, 64 MB | 3 groups, 48 MB | bank select `((~b) & 7) * 8 MB` covers both |
| VROM | 16 chips, IC 26–41 | same | the loader's size-based pick swallowed Scud's MPEG ROMs (fixed: by IC) |
| Extra boards | light-gun board on the I/O serial port | Digital Sound Board (Z80 + MPEG music) on the main UART; drive board; network board at `0xC0000000` | gun answers are hard-wired in `bus.c`; no DSB, no network |
| Inputs | two light guns, trigger on bit 0 of regs `0x08`/`0x0C` | wheel + two pedals on the ADC (`0x3C`), view and shift buttons | `m3_input_t` has no analog axes; the ADC returns a constant |
| MAME patches | `init_lostwsga` patches one ROM word ("HACK") | `init_scud` patches nothing (debug-menu patch commented out) | |
| Lift size | 2 014 functions, 342 753 instructions | 2 011 functions (ROM + RAM), 114 517 instructions | |
| Indirect targets the walk misses | via `--entries` feedback | 5 so far: a callback with an `addi r1,r1,-16` prologue, and four arms of an LR-relative `bctr` dispatch | same mechanism |

## Interrupts

Both take VBlank at `0x500` and the decrementer at `0x900` (MSR[IP] clear).
Scud Race reloads the decrementer inside its field routine from a RAM word;
The Lost World computes its period from two Real3D status-bit flips (see the
note in `bus.c`), which Scud has not been seen to do — *hypothesis*, not yet
traced.

## Not compared yet

DMA programs (Scud's 53C810 traffic), Real3D FIFO volume per frame, tilemap
usage, EEPROM layout. Each needs the game further than the current bring-up
reaches (see BRINGUP.md).
