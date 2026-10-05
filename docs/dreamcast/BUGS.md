# Bug log (Dreamcast port and Scud Race bring-up)

## DC-BUG-001 — baseline does not compile with GCC 15

- **Symptom**: `src/real3d.c:515: error: conflicting types for 'sample'`.
- **First bad stage**: desktop build, before any port change.
- **Expected**: the library builds (it does under MSVC).
- **Root cause**: `sample()` forward-declared with `const struct tri_s *`
  before `struct tri_s` was defined, so the parameter named a scope-local
  type distinct from the later one.
- **Fix**: `fe32f1e` — typedef before the prototype.
- **Regression test**: the desktop build itself (`ctest` runs after it).

## DC-BUG-002 — Scud Race falls out of the guest after two functions

- **Symptom**: 600 black fields, no error; guest-function report lists only
  `FFF00100` and `00000100`.
- **First bad stage**: lifter output for RAM `0x100`.
- **Expected**: the game's main path from `0x1540`.
- **Observed**: `mtspr SRR0` / `rfi` emitted as `return; /* rfi */`.
- **Root cause**: every `rfi` was lifted as an interrupt return. Scud Race
  uses `rfi` as its hand-off jump, the way The Lost World uses `mtlr`+`blr`.
- **Fix**: `17c59f6` — SRR0 built from an immediate in the same function
  makes `rfi` a computed jump (`MSR = SRR1; CALL(SRR0); return;`). Fires
  once in each half of Scud's lift.
- **Regression test**: none automated yet; the guest-function report of a
  600-field run shows the field routine running ~500 times.

## DC-BUG-003 — `rom_loader.py` rejects Scud Race; accepts lane swaps

- **Symptom**: "could not find a program-ROM interleave that validates
  (best 0/7)" for all three Scud sets.
- **Root cause**: the check only knew `li r7, n` vectors, and only reads
  lanes 0–1, so lanes 2–3 were unchecked for every title.
- **Fix**: `f5ab337` — accept the `bl common` idiom, try the board's IC
  order first, break ties with a decode score over the reset code.
- **Regression test**: manual — the swapped image scores 40 vs 65.

## DC-BUG-004 — VROM built from the wrong chips

- **Symptom**: none visible yet; found by reading the loader against MAME.
- **Root cause**: VROM chips were chosen by size (2 MB); Scud's MPEG ROMs
  (IC 57–60) are 2 MB and sort ahead of the VROM (IC 26–41), so the first
  16 "VROM" chips would have included four MPEG ROMs.
- **Fix**: `f5ab337` — select by IC position.

## DC-BUG-005 — Dreamcast build: type and symbol clashes with KallistiOS

- **Symptom**: `conflicting types for 'uint32'` (Musashi vs KOS
  `arch/types.h`), then `conflicting types for 'irq_init'`, then undefined
  `pthread_*`.
- **Fix**: `afc978d` — Musashi/SoftFloat use KOS's typedefs under
  `_arch_dreamcast`; `irq_init` → `m3_irq_init`; link KOS's pthread addon.

## DC-BUG-006 — the Flycast harness printed nothing

- **Symptom**: a KOS "hello" ELF and CDI both produced zero console lines.
- **Root cause**: the private `emu.cfg` used a `[Debug]` section; Flycast
  reads `Debug.SerialConsoleEnabled` under `[config]`, so the console was
  off. A bare ELF additionally boots under the HLE BIOS without output.
- **Fix**: `tools/dreamcast/dc-run.sh` passes the settings with `-config`;
  programs are wrapped by `tools/dreamcast/make-cdi.sh`. Calibrated against
  the same CDI under a known-good harness before trusting it.

## DC-BUG-007 — Scud Race's text printer runs off its string (fixed)

- **Symptom**: after ~60 fields, writes past tilegen VRAM (`0xF1120000..`)
  and reads at wild addresses (`0x060B16F0`, `0x0BE97E50`); "20000000
  device accesses with no field advance".
- **First bad stage**: lifted `0x4130`, a text printer. Oracle: the
  interpreter, run past the RAM hand-off for 136 fields, calls it six times
  and it returns every time; the runtime's first call never returns.
- **Root cause**: `0x4130` calls a local routine with `bl 0x41C0`; that
  routine leaves through a jump table, and the arm (`0x41F8`) runs the outer
  function's epilogue and `blr`s to the outer caller. A lifted `blr` was a C
  `return`, which landed after the `bl` instead, inside the printer's loop.
- **Fix**: every lifted return publishes its target (`M3_RETURN`), every
  call site checks it is the one being returned to (`M3_RETURNED`) and
  otherwise keeps returning; an orphaned target is resumed from
  `model3recomp_run()`. Interrupt dispatch saves and restores the pending
  target. One compare per call.
- **Regression test**: `guest_smoke` (desktop and SH-4) still passes with a
  `bl`; Scud's 1500-field run reaches the settings screen.

## DC-BUG-009 — `mtlr`/`blr` jumps through computed LR were dropped

- **Symptom**: found by DC-BUG-007's fix — "guest returned to 00001A3C,
  which no host frame was waiting for".
- **Root cause**: the lifter only read `mtlr rX ; blr` as a jump when rX was
  built purely from immediates. Scud's PCI configuration helpers build LR as
  `0x1A24 + 12*size` and `blr` into a table of three-instruction stubs
  (`mtlr r0 ; stbx/sthbrx/stwbrx ; blr`), and its printers load LR from a
  jump table (`lwzx r3,r3,r4 ; mtlr r3 ; blr`). As returns, **the stubs never
  ran**: every PCI config access of size 1, 2 or 4 was silently skipped,
  and every table dispatch fell through. That is why the guest's stack
  pointer read `0x40` at the first decrementer interrupt in every earlier
  run, and `0x001EFFC0` after.
- **Fix**: the lifter tracks a provenance per GPR (immediate-derived,
  loaded through an immediate-derived address, loaded from elsewhere, mflr).
  LR from the first two is a jump; the jump-table scanner also follows
  `mtlr`. 17 sites in Scud's lift, all reviewed by hand: table dispatches,
  the two stub tables, and the boot hand-off. The stubs are in
  `src/games/scud/entries.txt`.

## DC-BUG-008 — with no framebuffer, VROM texture loads vanish (open)

- **Symptom**: none yet (nothing renders on the Dreamcast); found reading
  `src/real3d.c` for docs/renderer/.
- **Root cause**: `real3d_render()` returns early when
  `platform_framebuffer()` is NULL, which is how the Dreamcast platform asks
  the software renderers to stand down. `g_sheet` is allocated only past
  that return, and `real3d_vrom_texture()` drops every load while `g_sheet`
  is NULL (`real3d.c:310`) — silently. The FIFO drain is skipped the same way.
- **Consequence**: a PVR back end cannot simply sit beside the software one;
  texture uploads must be decoupled from rasterising first.
- Related: `flush_texture_fifo()` re-parses the FIFO from offset 0 whenever
  it grows and `bus_texfifo_reset()` has no caller, so every texture ever
  sent is re-uploaded and the 4 MB cap eventually drops data silently.
