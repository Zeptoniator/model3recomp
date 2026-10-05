# Status

## 2026-10-05

### Working

- Desktop baseline builds with GCC 15 (one pre-existing compile error
  fixed); 2/2 tests.
- Scud Race (MAME `scud`): ROM images built and verified, boot interpreted
  to the RAM hand-off, both halves lifted (114 517 instructions, 0.06 %
  untranslated), linked into a desktop executable; the game's VBlank handler
  and field routine run (SCUD-PC-01..07, -08 in progress).
- **DC-01**: the runtime cross-builds with KOS 2.2.1's CMake toolchain and
  boots under Flycast; `model3recomp_init` + 120 fields.
- **DC-02**: lifted PowerPC runs on the SH-4 with the right results
  (`guest_smoke`, plus `test_ppc`'s semantic checks). Negative control:
  one mutated instruction makes it fail.

### Broken

- Scud Race on the desktop stops making progress after ~60 fields: a
  byte-stream routine runs off its buffers (DC-BUG-007).
- Nothing renders on the Dreamcast; no PVR back end exists yet.

### Measured

| | |
|---|---|
| Heap after `model3recomp_init` on Dreamcast, no game | 9 572 KB |
| Runtime code over KOS | ~352 KB text; 1.2 MB BSS, ~1.1 MB of it sound-board tables |
| Scud lifted code, SH-4 `-O2` | 2.89 MB (1.41 MB is the ROM half, of which one function runs) |
| Scud boot in the interpreter | 18.5 M instructions, 80 s |
| Desktop build of library + Scud | 67 s |

### Next

1. Find DC-BUG-007's first divergence: interpreter vs runtime device traces.
2. Measure the Real3D regions' high-water marks over a Scud run (memory
   budget's open term).
3. Restrict the ROM half of the lift to the boot path (−1.4 MB of code).
4. `renderer/pvr`: a triangle, then a Gouraud triangle, then a textured one
   (prompt §27, §127) — independent of Scud.
5. Analog inputs in `m3_input_t` and the ADC (steering, pedals).

### Risks

- **Memory** (high): 8 MB guest RAM + 1.1 MB tilegen VRAM are fixed; the
  Real3D's 9 MB of culling/polygon RAM and the 8 MB texture sheet must come
  down to measured use or move to PVR VRAM, or the port does not fit.
- **CPU** (unknown): the guest's cost per field on a 200 MHz SH-4 is not
  measured; the PPC is 100 MHz with doubles, and every guest access goes
  through byte-wise big-endian assembly.
- **Music**: the Digital Sound Board (MPEG) is not modelled anywhere.
