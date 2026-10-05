# Status

## 2026-10-05 (late morning) — DC-05

- **First Real3D polygons on the PowerVR2.** The attract race's village
  (bridge, houses, roofs, crash barriers) drawn by the PVR on the Dreamcast
  (Flycast), flat-shaded, no textures yet. Same polygon count as the
  desktop's counting sink (455 at field 1000), same frame signatures.
- VROM is now streamed from SCUD.DAT v2 (DC-BUG-014); distance LOD and
  culling after Supermodel cut VROM reads per field by ~4×.
- Heap 11.1 MB at field 1400 (VROM cache 1.5 MB added); 53 VROM page
  misses by field 1400.
- Next: textures (a PVR texture cache fed from the FIFO and VROM loads),
  then the tilemap layers on the PVR, then speed (~8 fields/s here).

## 2026-10-05 (night) — DC-03

- **Scud Race runs on the Dreamcast** (Flycast), headless: 600 fields in
  96 s, heap 7.6 MB, Real3D RAM paged in 1 088 KB, 161 CROM pages read off
  the disc, exit clean.
- **The SH-4 runs it bit for bit like the desktop**: frame signatures (hash
  of the first 4 MB of guest RAM + Real3D trigger count) at fields 100,
  200, 300, 400 and 500 are identical to the desktop build's.
- Speed (Flycast, not hardware): ~16 fields/s during boot, ~2.4 fields/s
  once the 3D scene is built (fields 500–600). Not a measurement of the
  console — Flycast does not model cycle costs (see the Flycast notes) —
  but the order of magnitude says the guest must get much cheaper.
- Next: the PVR back end fed from the Real3D decode; a profile of where an
  SH-4 field goes (bus accesses are the first suspect).

## 2026-10-05 (evening)

- **Scud Race's attract race draws in 3D on the desktop**, textured: the
  village course, buildings, trees, signs, with the 2D over it. Two fixes:
  mcrfs/FPRF (DC-BUG-013), and the texture FIFO now drains (DC-BUG-008).
- Remaining visual faults: black holes in some textures (transparency or
  sheet regions overwritten), not yet looked at. No cars seen yet in the
  sampled frames.
- Next: Dreamcast — ROM streaming and memory sizing, then Scud headless on
  the console (DC-03); the 3D on the desktop is now the reference for the
  PVR back end.

## 2026-10-05 (later the same day)

### Working

- **SCUD-PC-09: Scud Race reaches attract mode on the desktop.** Region
  warning, title screen (Scud Race / BPR organisation), then INSERT
  COIN(S) / CREDIT 0 / © SEGA 1996 with the AM2 and SEGA logos, all drawn
  by the recompiled game through the tilemap. Five fixes got it there, each
  general rather than Scud-specific: non-local returns and computed `blr`
  in the lifter, discovery to a fixed point, the video board's JTAG chain,
  53C810 single-step, and the operator's LINK ID setting.
- **PVR**: flat, Gouraud and textured triangles on the PowerVR2
  (`tests/dreamcast/pvr_triangle`, photographed under Flycast).

### Broken

- The 3D is submitted (23 738 triangles a field in attract) and none is
  drawn: the viewport words decode as `inf`, and one matrix is loaded where
  a scene needs many. The Real3D decode was settled on The Lost World only.

### Measured

| | |
|---|---|
| Lift | RAM 2 054 functions / 131 778 instructions, ROM 1 604 / 109 128 |
| Real3D memories in use during attract | ~3.5 MB of the 22 MB modelled (MEMORY_BUDGET.md) |
| Guest RAM high-water mark during attract | `0x1EFFBF` of 8 MB |

### Next

1. Real3D viewport and matrix decode for Scud (vs Supermodel's Legacy3D),
   until the attract race draws on the desktop — the reference for the PVR.
2. Reset the texture FIFO per frame (DC-BUG-008) and measure its real volume.
3. Confirm the memory high-water marks during a race; then size guest RAM
   and the Real3D regions for the Dreamcast.
4. ROM streaming (`rom_read`, page cache) — Scud on the Dreamcast (DC-03).
5. Analog inputs (wheel, pedals) through the ADC.

## 2026-10-05 (morning)

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
