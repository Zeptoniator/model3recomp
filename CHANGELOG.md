# Changelog

All notable changes to this project are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Known issues
- **The CROM bank mapping is wrong, so a guest reads zeros for its data.** The
  banked window is 8 MB of a 32 MB image, so at most two bits of the register
  can be the bank; the current reading shifts the whole byte, and The Lost
  World writes 0xF7 for most of its run, landing at offset 0xF700000 -- far
  outside the image. Every read through the window then returns zero: 299,984
  of them in a 2,500-field run, about 1.2 MB of the guest's own data.
  `tools/rom_loader.py` warns of exactly this in a comment, and the symptom is
  what it says -- the game boots and then has nothing to show.

  The right mapping is not yet known, so the default is unchanged rather than
  replaced by a guess. `M3_CROM_BANK` selects between candidate readings and
  `M3_TRACE_BANK` reports which window offsets a guest reads under which
  register value: The Lost World reads `0x000000`, `0x010000`, `0x120000`,
  `0x150000` and `0x400000` under `F1`, `F2`, `F3` and `F5`. Every candidate
  tried leaves it worse off than zeros do, which says it finds wrong data
  rather than none.

### Fixed
- **A frame was presented before the guest drew it.** `irq_tick()` rendered and
  presented and then ran the guest's VBlank handler, so what reached the
  screen was the state before that field's drawing -- and a guest that clears
  its tilemap and then prints into it was caught in the gap. Hardware scans
  the tilemap out continuously and the guest draws during blanking precisely
  so the picture is whole when it is not; one render per field belongs after
  the drawing, and now is.
- **The tilemap renderer drew one tile over the whole screen.** It read the
  name table from 0x0F6000, which is a scroll table, not a map -- a guest
  fills it with a single entry and the result is a screen of that one tile
  forever. The four name tables are at 0x0F8000 and 0x2000 apart. Two more
  orderings were wrong with it, and neither announces itself: the two 16-bit
  entries in each 32-bit word are stored low half first, so every pair of
  characters traded places, and a pattern row is a little-endian word with the
  leftmost pixel in the top nibble, so every glyph came out mirrored -- which
  is invisible on T, H, O and X. The whole graphics side is little-endian,
  being on the PCI side of the board, and all three follow from that.

  The decode is checked against text whose wording is known before it is
  rendered rather than against a document, which is the only way to tell a
  right answer from a plausible one here.
- **The field clock stopped for a guest looping in pure lifted code.** The
  runtime took its turn at `func_table_call()` and at a device access, and a
  loop that waits on a RAM word written only by an interrupt handler does
  neither -- so no field fell due, so no interrupt arrived, so the word never
  changed. The lifter now marks backward branches and the runtime gets a turn
  every 1024 of them.
- **The sound board never reported itself ready.** A guest's command loop
  waits on bit `0x01000000` of `0xF0080004` before writing each byte, and that
  loop runs inside the VBlank handler. A board that is never ready therefore
  did not merely lose sound: `irq_tick()` will not re-enter while a dispatch
  is in progress, so no further field ever fell due and the machine wedged.
  With no 68000 to be busy, the board now answers "always ready to accept,
  never anything to say" -- the second bit, `0x02000000`, stays clear so the
  reply poll returns "no data" rather than reading a byte that is not there.
- **Real3D status at `0x84000000` was a constant.** The boot copies nine dwords
  of it into RAM with `stwbrx` — the chip is on the PCI side, so the cached
  copy is byte-reversed — and then waits for bit `0x02000000` of that copy to
  change. Byte-reversed that is bit 1 of the register, and a constant never
  changes, so the guest never left the wait. Toggled per read now.
- **`tools/ppc_interp.py` performed SCRIPTS memory moves that `src/scsi.c`
  already rejected.** The runtime had a `mapped()` guard on both source and
  destination; the interpreter had only a size check, so the two disagreed
  about what a malformed script does. A garbage DSP value is routine — the
  register image is written a word at a time and one word lands on offset
  `0x2C` — and one resulting move zeroed 8 KB of a guest's work RAM. The guard
  is now the same on both sides.

### Added
- **The Real3D's upload FIFO at 0x94000000** (`src/bus.c`, `src/scsi.c`). It is
  one port rather than a window, so the DMA guard's range check threw every
  transfer to it away as out of range -- 464 of them in a two-thousand-field
  run of The Lost World, which is most of what that guest sends its graphics
  processor. The bytes are now kept in arrival order and `bus_texfifo()` hands
  them to whatever comes to render them. (In this title they turn out to be a
  gamma ramp rather than geometry, but they were being discarded unseen, and
  "discarded unseen" is the part that mattered.)
- **A guest function-entry histogram** and **a device-write watch**, both under
  `M3_LOOP_GUARD`. `M3_TRACE_CALLS` only sees indirect dispatch, so a frame
  task's whole call tree was invisible; counting every function entry is the
  difference between "the frame task ran" and "here is what it did". The
  device watch names the guest function behind a register write, which the RAM
  watch cannot see because device writes leave the inline path.
- **The decrementer** (`src/irq.c`). SPR 22 counts down and its 0 -> -1
  crossing takes an exception at vector 0x900 -- a separate exception from
  the external interrupt at 0x500, gated only by MSR[EE]. The Lost World's
  timing calibration waits on a counter that only the handler it installs
  there ever writes, and the pointer to that handler appears nowhere in RAM
  because the processor calls it, not the game. Without this the wait cannot
  end, and the interpreter and the recompiled binary sat in it alike.
- **A watch on a guest RAM address** (`M3_WATCH`). `tools/ppc_interp.py` has
  `--watch` and the runtime had no equivalent, because lifted code writes work
  RAM through the inline path in `lift.h` without the bus ever seeing it. Set
  it to an address at build time and every write is reported with the guest
  function that made it. Compiles to nothing otherwise.
- **`model3recomp_dump_scene()`** (`src/scenedump.c`): writes Real3D culling
  and polygon RAM, tilemap VRAM and work RAM to files from the frame hook, so
  a renderer can be written against what a game really put there.
- **A loop guard for locating a spinning guest** (`M3_LOOP_GUARD`). A
  recompiled game has no program counter to inspect; when one wedges in a
  loop that touches no device and dispatches through no pointer, nothing
  outside it knows where "there" is. Built with this defined, the lifter's
  marks let the runtime name the guest address it is looping at and the
  function it entered to get there. Compiles to nothing otherwise.
- **PCI configuration space** (`src/bus.c`, `tools/ppc_interp.py`): the MPC105
  port pair at `0xF0800CF8` / `0xF0C00CFC`, answering for the Real3D at device
  13 (`0x16C311DB`) and the 53C810 at device 14 (`0x00011000`). A game that
  probes for these will not finish booting without them. Config writes are
  dropped: both devices are at fixed addresses in this memory map, so there is
  nothing to relocate.
- **Floating point in the boot interpreter**: loads, stores, arithmetic,
  compares and conversions. FPRs stay raw 64-bit patterns so the boot path's
  `lfd`/`stfd` block copies remain bit-exact, and arithmetic converts at the
  edges. `tests/test_fp.py` covers the round trip and the cases where the
  hardware answers with an infinity and Python would raise.
- `tools/ppc_interp.py --break-at ADDR`: report whether execution ever reaches
  an address, without stopping the run. Repeatable, so one run answers a whole
  call chain — which is how the boot stalls above were located. Watch hits now
  carry an instruction count and field number.
- **Boot interpreter** (`tools/ppc_interp.py`): runs the reset path against the
  same memory map as the runtime, recovers the CROM-to-RAM segment table, and
  snapshots RAM for the lifter. Also the conformance oracle.
- **53C810 SCSI DMA** (`src/scsi.c`): SCRIPTS memory move, jump and interrupt,
  with manual-start mode and range guards.
- **Tilemap renderer** (`src/tilegen.c`): four 8x8 layers, 4bpp, 1-5-5-5
  palette. Name-table base and entry bit layout still unverified.
- **Screenshot capture** (`src/screenshot.c`) and a per-field frame hook, since
  a recompiled game never returns and that is the only way to see it.
- **Headless platform** (`src/platform_null.c`) so the library builds and runs
  without SDL2.
- Spin diagnosis in the bus: when the guest stops making progress it names the
  register being polled, ranked by traffic.
- `M3_TRACE_CALLS` to print dispatched guest addresses.
- `M3_TRACE_IO` records device accesses in the same format the interpreter's
  `--trace-io` writes, so the two can be diffed line for line.
- ROM loader assembles the banked CROM and the VROM, not just the program
  image. A port without them boots and then has nothing to show, because every
  read of either returns zero.
- Accessors for tilemap VRAM, Real3D culling and polygon RAM, and the VROM.
- Inline fast path for guest memory: work RAM is handled in `lift.h` instead of
  calling into the bus for every load and store.

### Fixed
- **Interrupts are delivered on a timer, not on a status poll.** The frame was
  originally hung off a read of `0xF0100018`, by analogy with Model 2. The Lost
  World's main loop never reads it -- it waits on a RAM flag only the VBlank
  handler sets -- so nothing ever advanced.
- **`blr` is not always a return.** PowerPC uses `mtlr`+`blr` as a computed
  jump, and the boot's handoff into RAM is exactly that. Emitting `return;`
  dropped out of the guest silently. Now told apart by whether LR was restored
  from the stack or built from an immediate.
- **Functions are no longer truncated at the next discovered entry.** Only a
  `bl` target or a prologue is a function boundary; a jump-table arm or an
  address built by `lis`/`addi` is an extra way *into* existing code. The game's
  entry point is nine consecutive `bl`s, and bounding it at a soft entry
  emitted five of them and returned.
- Function discovery walks control flow from seeds instead of scanning the
  whole image for `bl`. A mostly-data ROM yields plenty of data words that
  decode as branches; that inflated the lift from 340 K instructions to 2.2 M.
- Device registers are byte-addressable: `0xF0100008` is a byte register the
  boot writes and reads back, and the broken lane handling hung it there.
- `0xF0040000` must read back what was written, and `0xF0040004` must toggle
  its ready line, or I/O init spins forever.
- Out-of-image branch targets are no longer emitted as C labels.
- Headless frame pacing: a virtual clock that advances per query, rather than
  reporting a field as always due, which starved the guest's main loop.

## [0.1.0] — 2026-09-15 — _"Reset Vector"_

First entry. The toolchain is verified end to end against a real romset; the
board itself is a skeleton.

### Added
- **PowerPC 603e decoder** (`tools/ppc_disasm.py`) covering the integer,
  floating-point, branch, condition-register and system instruction sets,
  including the 603e-specific software TLB reload (`tlbld`, `tlbli`).
- **Static recompiler** (`tools/ppc_lifter.py`): PPC 603e to C, with function
  discovery from call targets, frame prologues and the exception vectors, and
  an optional code-pointer scan. Emits the house layout — `<prefix>_code_NNN.c`,
  `<prefix>_funcs.h`, `<prefix>_register.c`.
- **ROM loader** (`tools/rom_loader.py`) that assembles the four interleaved
  program EPROMs and *proves* the layout against the PowerPC exception vector
  table rather than assuming a chip order.
- **Decode conformance harness** (`tools/check_decode.py`), scoring the decoder
  against capstone over a whole ROM and excluding instructions a 603e cannot
  execute.
- **Runtime**: PPC 603e context, memory bus with the full Step 1.x address map,
  function dispatch table, interrupt controller, frame pacing, SDL2 platform
  layer.
- **Semantics self-test** (`tests/test_ppc.c`) for the rotate-mask wrap case,
  shift counts of 32 or more, carry and overflow rules, CR bit numbering and
  func-table growth.
- `docs/technical/execution-model.md`, documenting the frame boundary, the
  interrupt acknowledge spin, and the RAM-execution problem, with the
  disassembly each conclusion was drawn from.

### Verified against *The Lost World: Jurassic Park* (`lostwsga`)
- ROM loader: 7/7 exception vectors matched, output byte-identical to a
  hand-assembled image.
- Decoder: 100.00% of the 603e-valid instruction set across the 2 MB program
  ROM; the 12 remaining capstone decodes are 64-bit and AltiVec encodings a
  603e cannot execute, found in data.
- Lifter: 2,014 functions, 342,753 instructions, 164 untranslated (0.048%).
- Generated C compiles with zero errors and zero warnings at MSVC `/W3`.

### Known gaps
- The game executes from RAM, not ROM, so the boot interpreter and RAM
  snapshot are required before the main program body can be lifted at all.
- No renderer: the tilemap and Real3D layers store and read back correctly but
  nothing draws.
- No 53C810 SCSI DMA, no sound, inputs are stubbed.
