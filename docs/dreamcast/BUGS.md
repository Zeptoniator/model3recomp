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

## DC-BUG-008 — with no framebuffer, VROM texture loads vanish (FIFO half fixed)

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

## DC-BUG-010 — the video board's JTAG chain was not modelled (fixed)

- **Symptom**: Scud draws "ERROR : (1A) VIDEO BOARD ABNORMAL STATUS, LR
  0000F944" on its settings screen and stops.
- **Root cause**: system registers 0x0C (TCK/TMS/TDI/TRST) and 0x10 (TDO)
  were ignored; the game shifts every ASIC's IDCODE out of the chain and
  compares it with the Step 1.5 values.
- **Fix**: `src/jtag.c`, an IEEE 1149.1 TAP and the board's chain (order,
  IR lengths and IDCODEs as Supermodel documents them; code written here).

## DC-BUG-011 — 53C810 single-step mode ignored (fixed)

- **Symptom**: the main program waits in `0xAE9C` for ever after the first
  DMA (field 430); the flag at `0x1005B0` is set and never cleared.
- **Root cause**: Scud sets DCNTL to STD|SSM. The engine ran the whole
  program and raised SIR; Scud's handler looks for SSI and for DSP one
  instruction along, then for the INT there.
- **Fix**: single-step stops after each instruction with SSI raised and DSP
  updated; DSP is also left past an INT, as on the chip.

## DC-BUG-012 — returns the lifter did not see coming (fixed)

- **Symptoms**: "guest returned to 00021838 / 0002F718 / 00013CFC, which no
  host frame was waiting for", and misses at `0x5D80`, `0x789C`, `0x37528`.
- **Root causes**: (1) code found by a scan was never walked for its own
  `bl` targets; (2) functions that load one of their own addresses into LR
  and branch away — conditionally or not — are making a call by hand, and
  the lifter treated the branch as a tail call; (3) float leaves open with
  `addi r1,r1,-N`, no `mflr`.
- **Fix**: discovery to a fixed point with soft entries; hand-made return
  addresses become labels claimed after the branch; one more prologue form.

### DC-BUG-008, the FIFO half (fixed)

`flush_texture_fifo()` now consumes what it uploaded (`bus_texfifo_consume`).
Before, the FIFO filled to its 4 MB cap during Scud Race's title screens and
every texture sent after that was dropped: the attract race drew its later
textures as white rectangles. After: `fifo 0` at the end of every field and
"0 never uploaded" texels. The no-framebuffer half (VROM loads skipped on
the Dreamcast) is still open.

## DC-BUG-013 — mcrfs decoded as fcmpu; FPSCR result flags not kept (fixed)

- **Symptom**: Scud Race's attract submits 23 738 triangles a field and
  draws none; the viewport's frustum words are `inf`, its sun vector `NaN`.
- **Chased**: viewport word ← DMA from `0xE0010` ← `0x5740` (tangent table
  lookup) ← FOV half-angles at `0x100344..` ← `0x2E7C0`, four `atan2`
  calls (`0x58D8`) with arguments `(1.43, +1)` and `(1.43, -1)` returning
  the same angle. `M3_WATCH` with the new `M3_WATCH_VAL`, then one probe at
  each step; the inputs were checked at every step before moving up.
- **Root cause**: `0x58D8` takes its arguments' signs from the FPSCR —
  `frsp f1,f1 ; mcrfs cr7,cr4` — and the decoder read mcrfs (XO 64) as
  fcmpu (5-bit XO 0), so the lift compared f16 and f12 with f0 instead.
  fcmpo (XO 32) had the same problem. The FPSCR's FPRF was never kept.
- **Fix**: `83e309f` — decoder takes the 5-bit XO only for the A-form set;
  `m3_fprf`, `m3_mcrfs`, FPCC from fcmpu/fcmpo; arithmetic keeps FPRF in
  functions that read the FPSCR. `tests/test_disasm.py` (ctest).
- **Result**: the attract race draws, textured.
- **Interpreter**: `ppc_interp.py` still treats mcrfs as a no-op.

## DC-BUG-014 — Real3D sink receives no polygon on the Dreamcast (fixed)

- **Symptom**: with the PVR sink installed, `[pvr] last frame: 0 opaque` at
  every field to 1400 while the desktop draws the attract race; screenshot
  black.
- **Chased**: two causes stacked. (1) `real3d.c` read culling and polygon
  RAM through `bus_cull_lo/hi/bus_poly`, which return NULL on the Dreamcast
  (paged). Fixed with `bus_r3d_read`, a copying reader for all four Real3D
  memories — still 0. (2) A desktop counting sink (`M3_SINK_COUNT=1`)
  reproduced the zero only when `scud_vrom.bin` was absent: **Scud Race's
  models are in VROM**, and the Dreamcast build had no VROM at all.
- **Fix**: VROM streamed — SCUD.DAT version 2 carries it (2-bit `which`),
  a VROM page cache separate from the CROM one (48 × 32 KB), and the
  renderer reads models a polygon at a time through `bus_r3d_read`.
- **Made affordable**: the renderer always took the most detailed LOD and
  never culled: 0.5–1.7 MB of VROM read per field. Supermodel's LOD
  selection (blend table at viewport word 0x17, radii at node word 9, model
  scale at word 1) and a cull-radius frustum test bring it to ≤ 456 KB per
  field (34 × 32 KB pages at most). Desktop pictures unchanged at fields
  1500–6000 (`M3_LOD_FINEST=1` for the old behaviour).
- **Result**: 455 polygons at field 1000 on the Dreamcast, the same count
  as the desktop sink; signatures identical; first 3D picture (flat).
- **Note**: compare desktop runs only from a fresh NVRAM: a leftover
  `scudrace.nv` changes the run from field 100 on.

## DC-BUG-015 — race start waits for ever on the sound command queue (fixed)

- **Symptom**: scripted race on the desktop (`M3_NO_SOUND=1`, coin 1500,
  start 1600, accelerator held): course, car and transmission chosen,
  "レーススタートです", then the HUD over a black screen and no progress
  from field ~4400.
- **Chased**: `M3_WHERE` put the main program in `0x2744`, the enqueue of
  a 32-entry ring of 4-byte sound commands, spinning while it is full. The
  ring is drained by `0x2798` from the 0x500 handler, one byte per sound
  interrupt; the enqueue writes `0x27` to the UART's control register
  (transmitter interrupt on) when the ring goes non-empty and the drain
  writes `0x06` (off) when it empties. With no sound board the runtime
  raised the interrupt once a field, so the ring drained a byte a field
  and the start-of-race burst filled it.
- **First fix, wrong**: the interrupt held as a level while enabled. Attract
  then hung at field 967: the handler sends one byte and then polls
  `0xF0100018` until the sound bit clears, and a level that stayed visible
  inside the handler never cleared.
- **Fix**: a level source (`irq_set_level`) that `irq_tick` re-raises
  outside a dispatch, and a data write that drops the pending bit (as MAME
  does) until the handler has returned. Both `sound_null.c` and the
  no-board path of `sound.c`. In the save state (version 3).
- **Result**: the race starts: rolling start under AUTO DRIVE, 3-2-GO!,
  304 km/h, the HUD (lap 1/4, 40th, timer), 3D textured. Then the next
  bug: frozen on GO! from field 4919 (DC-BUG-016).

## DC-BUG-016 — frozen on GO!: a jump through LR lifted as a return

- **Symptom**: from field 4919 the guest RAM hash and the Real3D count stop
  (`[sig]` constant), fields still advance, no spin reported, no VBlank
  dispatched (`M3_WHERE` silent; the new "VBlank not delivered" line too).
- **First reading, wrong**: `no function lifted at 00035740` and `guest
  returned to 0001EEF0, which no host frame was waiting for` looked like
  indirect targets only a race reaches. Added to `entries.txt`, the next
  run unwound one step further (`0x1DF90`), a chain of returns with no
  owner -- the shape of something broken upstream, not of missing entries.
- **Chased**: `M3_UNWIND_TRACE=60 M3_UNWIND_ONLY=0x1F0E0`: the first stray
  return is to `0x1F0E0`, which follows no `bl`; it is inside the loop of
  `0x1F014`. That function sets `r31 = 0x20000 - 3872 = 0x1F0E0` before the
  loop; the loop branches (`blt`, not `bl`) to a local routine at `0x1F158`
  which leaves with `mtlr r31 ; blr` -- a jump back into the loop, LR used
  as a jump register. Lifted as `M3_RETURN()`, it unwound every host frame
  and out of the reset vector, and `model3recomp_run` presented empty
  fields.
- **Why the lifter missed it**: it already turns `lis/addi ; mtlr` targets
  inside a function into labels with a runtime check (`if (PPC_LR == t)
  goto`). Its scan goes in address order and read each instruction's `rd`
  field as the register written: `fadds f31` erased r31's constant, `ori`
  was read backwards, and the epilogue's `lwz r31` sits, by address,
  between the build and the local routine's `mtlr`.
- **Fix** (`tools/ppc_lifter.py`): `gpr_writes()` gives the general
  registers an instruction really writes; and any address of the body
  built from immediates into a register that some `mtlr` in the body reads
  becomes a checked target (never new code to walk, never a call's return
  site). Four new checked targets in the whole Scud lift: `0x1F0E0`,
  `0x19B0`, `0x1A24`, `0x21860`. `tests/test_lifter.py`, four cases, the
  old lifter fails the two that matter.
- **Also**: `model3recomp_run` now says when the guest's code exits and it
  is only presenting empty fields -- this freeze was silent.

## DC-BUG-017 — the race parts from the desktop at field 4234: a sign lost by fneg in double mode (fixed)

- **Symptom**: the scripted race (`SCUD_DC_COIN_AT=1500 SCUD_DC_START_AT=1600
  SCUD_DC_ADC_SET=1:255`) renders and plays on the Dreamcast, but its RAM
  signatures leave the desktop's at field 4234 -- equal at 4232 -- just
  after GO!. By field 5000 the car is at 310 km/h on the Dreamcast.
- **Ruled out on the way**, each by a run: denormals (`M3_FTZ=1` on the
  desktop, signatures unchanged); the FPSCR KOS starts with (0x00040000,
  round to nearest); `fctiw`'s cast out of range (`m3_f2i32` now
  saturates as the 603e does -- kept; desktop signatures unchanged to field 4250, the furthest that lift was run); the CROM page
  cache (the desktop through `M3_SCUD_PACK` matches itself); the time base
  (it runs off `m3_work`).
- **Narrowed**, three tools, one Dreamcast run each:
  `M3_RAMREF=field,path` (the desktop writes a hash per 4 KB block, the
  disc carries it, the Dreamcast prints the blocks that differ: 8, matrices
  and car state off by ~1e-3); `M3_WORKSIG` (a hash of those blocks at
  every dispatched call: first change after the call to `0x2ADD0`, the car
  on its track segment); `MODEL3RECOMP_FN_TRACE` + `M3_FNSIG` (registers
  at every function entry: between two calls of `0x5B8C` inside
  `0x2C814`, the cubic `2t^3 - 3t^2 + 1` of a Hermite blend).
- **Cause**: GCC 13 (`-m4-single`, `-ffp-contract=fast` by default in GNU C)
  turned `F32(F32(t4 + t4) - t3)` into single precision and contracted it:
  `fneg fr1 ; fmac fr0,fr2,fr1` for `2 * t4 + (-t3)`. The `fneg` was
  scheduled after the `lds` that puts the FPSCR in double mode, where
  `fneg` on an odd register negates nothing: `2*t4 + t3`. The same bug as
  field 546's -0, by another route. One odd `fneg` in all the lifted code.
- **Fix**: `-ffp-contract=off` for every target (CMakeLists.txt): the
  lifted code computes `a*b + c` as written, on both hosts. No `fneg` is
  left in lifted code, and `tools/dreamcast/check-fneg.sh` runs after
  every Dreamcast link of `scudrace` and fails the build if one comes back
  (it flags the old ELF: three `fneg fr0`, one `fneg fr1`).
- **Lesson**: on the desktop the option changes nothing (signatures to
  field 6000 identical): the divergence was only ever the Dreamcast's
  compiler, invisible to every host test.

## DC-BUG-018 — field 6000: the overpass missing, a dark triangle, palms and sails that should be hidden (fixed)

- **Symptom**: at field 6000 of the scripted race the Dreamcast draws a
  large dark triangle over the left of the screen where the desktop draws
  an overpass, and shows palms and sails the desktop does not. RAM
  signatures equal: the renderers differ, not the game.
- **Narrowed** on the desktop, no Dreamcast run needed: `M3_SINK_DUMP=
  field,path` (with `M3_SINK_COUNT=1`) writes every polygon as the PVR
  receives it, and `tools/dreamcast/sink_render.py` draws that list with an
  exact rasteriser. Drawn with one depth buffer it shows the palms and the
  sails and no overpass, so the PVR was not alone at fault; drawn with a
  fresh depth buffer per viewport, as `raster()` does, it shows the
  desktop's picture.
- **Two causes**:
  1. *Viewports are layers.* The software path clears its depth buffer
     for each viewport (lowest priority first), so the overpass, in the
     last one, covers the sails and palms of an earlier one although they
     are nearer. The sink had no notion of viewports, and the PVR sorted
     them all on one depth buffer.
  2. *Far off-screen vertices.* Walls entirely off the right of the
     screen, cut at the near plane, project to x ~ 330 000; the PVR
     (Flycast at least) draws them as a dark triangle across the left.
- **Fix**: the sink gets `begin_viewport(wmin, wmax, x0, y0, x1, y1)`,
  where the range is that of w over the part of each polygon inside the
  viewport's rectangle (`sink_bounds` in real3d.c). The PVR back end cuts
  every polygon to that rectangle, as `raster()` draws only inside it, and
  scales each viewport's w by the power of two that stacks its range just
  above the previous one (exact, and harmless to perspective-correct
  texturing), from 2^-10 to 2^16.
- **On the way**: Flycast writes `gl_FragDepth = log2(1 + 100000 w) / 34`
  (core/rend/gles/gles.cpp) into a 24-bit fixed depth buffer: every w above
  ~2^17 is one depth, and w under ~2^-10 has few steps left. That leaves
  ~27 bits of w. Bounding each viewport by its triangles' z wanted ~11 bits
  apiece (a road's near end runs off the screen to w = 4, a backdrop's dome
  round behind the camera), 33 for the three viewports of field 5000, and
  every way of fitting that failed somewhere: a fixed 2^24 per viewport
  lost field 5000's road, a stack to 2^33 saturated (the strict GREATER
  compare then lets the sky drawn first win), a stack from 2^-24 blacked
  out field 1300's sky. What the screen shows is 0 to 10 bits a viewport,
  ~15 a frame. Clamping a vertex to the on-screen range would bend the
  depth across its polygon, hence the cut to the rectangle, which also
  replaces a 256-pixel guard band.
- **Tool**: `sink_render.py --pvr` draws a dump as Flycast would (lists in
  order, strict compare, its depth formula and the stack); it gave 0
  differing pixels against the per-viewport picture at fields 1300, 5000,
  5400 and 6000 before the Dreamcast run.
