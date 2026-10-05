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

## DC-BUG-007 — Scud Race's byte-stream routine runs off its buffers (open)

- **Symptom**: after ~60 fields, writes past tilegen VRAM (`0xF1120000..`)
  and reads at wild addresses (`0x060B16F0`, `0x0BE97E50`); "20000000
  device accesses with no field advance".
- **First bad stage**: unknown — the routine at `0x4170` is the first
  visible victim.
- **Next step**: differential device trace, interpreter vs runtime
  (docs/scud/BRINGUP.md).
