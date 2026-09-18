# Changelog

All notable changes to this project are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed
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
