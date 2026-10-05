# Port audit: model3recomp on the Dreamcast

What each part of the repository depends on, and what the Dreamcast port does
with it. "Measured" means a number from a run in this session; everything
else is read from the source.

| Module | File(s) | Host dependency | Dreamcast action |
|---|---|---|---|
| PPC decoder | `tools/ppc_disasm.py` | Python, build time only | KEEP |
| PPC lifter | `tools/ppc_lifter.py` | Python, build time only | KEEP_WITH_FIXES — `rfi` as a computed jump (done, `17c59f6`); restrict the ROM half to the boot path (see CODE_SIZE.md) |
| Boot interpreter | `tools/ppc_interp.py` | Python, build time only | KEEP_WITH_FIXES — `lhbrx`/`sthbrx`, `--scsi-base` (done); its fixed-CROM window mirrors the program ROM where the runtime maps CROM0 (harmless for Scud's boot, see EXECUTION_FLOW.md) |
| ROM loader | `tools/rom_loader.py` | Python, build time only | KEEP_WITH_FIXES — both vector idioms, board-position chip selection (done, `f5ab337`) |
| CPU context | `include/model3recomp/ppc.h`, `src/ppc.c` | portable C | KEEP — FPRs are `double`; KOS builds with `-m4-single`, so `double` is 64-bit and uses the FPU's double mode (checked: `fmul dr6,dr4`). `spr[1024]` is 4 KB, fine |
| Lift surface | `include/model3recomp/lift.h` | portable C | KEEP — RAM fast path assembles bytes big-endian by hand, so it is endian-neutral; proven on SH-4 by `guest_smoke` |
| Function table | `src/func_table.c` | portable C | KEEP — open-addressed hash; profile on SH-4 later (prompt §63) |
| Memory bus | `src/bus.c`, `include/model3recomp/bus.h` | portable C, `getenv` in hot paths | ADAPT — Real3D regions lazy on Dreamcast (done); SCSI window per title (done); ROM access must become streamed (`rom_read`, not a pointer) for 80 MB of CROM+VROM |
| IRQ / field clock | `src/irq.c` | portable C | KEEP — renamed `irq_init` → `m3_irq_init` (KOS has one). Fields are paced off `m3_work`, not wall time, which is what a slow host needs |
| 53C810 DMA | `src/scsi.c`, `include/model3recomp/scsi.h` | portable C | KEEP |
| Real3D scene + raster | `src/real3d.c` | pthreads (banded raster), 12 MB texture sheet + flags, ~6 MB triangle queue | REFERENCE on desktop; REWRITE the back end as `renderer/pvr` (see docs/renderer/) |
| Tilemaps | `src/tilegen.c` | software, into `platform_framebuffer()` | REWRITE for PVR (tiles as quads, prompt §43); software one stays as reference |
| Sound board | `src/sound.c`, `src/scsp.c`, `src/m68k/` | pthreads; Musashi | DEFER — builds for SH-4 (typedef clash with KOS fixed); an interpreted 68000 plus two SCSPs on a 200 MHz SH-4 is unaffordable next to the guest; phase 1 is mixed PCM to AICA |
| Digital Sound Board (Z80 + MPEG) | — | not implemented anywhere in the runtime | DEFER — Scud's music; see LOSTWORLD_SCUD_DIFF.md |
| Inputs | `src/input.c`, `include/model3recomp/input.h` | portable | ADAPT — `m3_input_t` has buttons and light guns, no analog axes; Scud needs steering and two pedals on the ADC |
| Platform: SDL | `src/platform_sdl.c` | SDL2, Win32 menus | KEEP for desktop |
| Platform: headless | `src/platform_null.c` | none | KEEP |
| Platform: Dreamcast | `src/platform/dreamcast/platform_dreamcast.c` | KallistiOS | NEW (done): serial log, work-paced fields, Maple pad, no framebuffer |
| Netplay | `src/netplay.c` | BSD/Winsock sockets | REMOVE on Dreamcast — `src/netplay_null.c` (done) |
| Save states | `src/savestate.c` | stdio | DEFER — a state is ~13 MB on desktop |
| Screenshot / scene dump | `src/screenshot.c`, `src/scenedump.c` | stdio | KEEP (desktop tooling) |
| Battery RAM / EEPROM | `src/bus.c`, `src/model3recomp.c` | stdio file | ADAPT later to VMU (prompt §52–53) |
| Title glue | `src/games/scud/` | — | NEW: board config, entries file, host `main()` |

## Host dependencies by kind

- **Threads**: `sound.c` (board thread), `real3d.c` (raster bands). KOS has
  pthreads in an addon (`addons/lib/dreamcast/libpthread.a`), now linked.
- **Sockets**: `netplay.c` only.
- **`getenv`**: dozens of call sites, many on hot paths (`bus.c` device reads
  probe `M3_IO_TRACE`, `M3_GUN_TRACE`, `M3_NO_BUTTONS` on every access).
  Works under newlib, costs a scan of an empty environment each time.
  Candidate for a one-time probe when the bus is profiled on SH-4.
- **Allocation**: everything is `calloc` at init or first use; no `mmap`.

## Title assumptions found in the "title-agnostic" runtime

These are The Lost World's facts living in board code. None blocks the boot;
each is a place Scud Race can read the wrong thing.

| Where | Assumption |
|---|---|
| `bus.c` inputs, reg `0x34` | "a gun board is fitted" is always answered yes |
| `bus.c` inputs, regs `0x08`/`0x0C` | bit 0 is a gun trigger |
| `bus.c` ADC, reg `0x3C` | returns a constant (`M3_ADC`, default all ones) — Scud steers through it |
| `irq.c` `dec_dispatch` | log text names The Lost World's counter address |
| `lift.h` `M3_LOOP_CREDIT` | calibrated for a 66 MHz 603e; Step 1.5 is 100 MHz (MAME `model3_15` → `add_cpu_100mhz`) |
| `bus.h` `M3_SCSI_BASE` | was the only SCSI window; now a default (`m3_config_t.scsi_base`) |
