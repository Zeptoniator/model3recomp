# Status

## 2026-10-12 — the race 35 % faster: no idle spin, no per-polygon copies

Profiling build, Flycast, fields 0-5400: **423 s instead of 660 s**, RAM
signatures unchanged at every check. In order:
- `draw_model` read every polygon through `bus_r3d_read`, a `memcpy` into
  an unaligned byte buffer: 14 % of the samples were in `memcpy`, most in
  its byte loop. `bus_r3d_span` hands back the bytes in place when they lie
  in one page (nearly always); `emit` copies its 12-byte arrays element by
  element (SH-4 GCC called `memcpy` for each). 660 -> 617 s.
- **Idle skip.** The game waits for its interrupts by spinning at 0x30704,
  and the field clock advanced 64 units per 1024 iterations: 12 % of the
  samples. `m3_idle_loop()` names such a loop; at its tick the runtime
  moves `m3_work` straight to the tick before the next event
  (`irq_work_to_event`: decrementer crossing or field). Landing on the
  event itself was not exact: the loop holds the decrementer it last read
  in r4, the handler saves r4 on the stack, and the stack word differed --
  so the loop gets one more slice and reads it. Signatures equal with and
  without (`M3_NO_IDLE_SKIP=1`) over the desktop race. 617 -> 458 s.
- `tex_get` walked all 256 texture slots per polygon: a 64-entry cache of
  where each key was last seen. Polygon words are read as one load (byte
  swapped for VROM), not four byte loads through a function pointer, and
  the vertex scale (2048) is a multiplication. `sink_project` skips the
  near-plane cut when no vertex needs it. 458 -> 429 s.
- Vertex normals are transformed and stored only for the software
  rasteriser's Gouraud; the sink shades flat. Each triangle is projected
  once: `sink_bounds` keeps the polygon in the triangle's own `proj`,
  over the view-space fields it no longer reads. 429 -> 423 s.

Left in a race field (~110-150 ms): `draw_model` 15 %, guest 23 %,
`real3d_render` + `sink_project` ~10 %, disc waits ~8 %. What is left of
`draw_model` is mostly the stores of 132-byte triangles into a 768 KB
table, not arithmetic.

## 2026-10-11 — viewports as layers: field 6000 drawn as on the desktop

- DC-BUG-018 (BUGS.md): the PVR sorted every viewport on one depth buffer,
  where the software path gives each its own, and drew polygons projected
  hundreds of thousands of pixels off the screen wrongly. The sink now
  starts each viewport with its rectangle and the range of w it shows on
  the screen; the PVR back end cuts polygons to the rectangle and stacks
  the viewports' w ranges by powers of two, inside the ~27 bits Flycast's
  depth buffer resolves.
- Flycast race to field 6000: the overpass is back and the dark triangle,
  palms and sails gone; fields 1300, 5000, 5400 and 6000 checked by eye
  against the desktop, all 30 RAM signatures unchanged. 817 s instead of
  791 s for the same build without the bounds pass (+3 %).
- `M3_SINK_DUMP=field,path` and `tools/dreamcast/sink_render.py` (`--pvr`
  simulates Flycast's depth) reproduce a frame's PVR picture on the desktop.

## 2026-10-10 — four persistent tilemaps, exact picture at 1.35x speed

- The PVR now keeps the four 512x512 tile layers as separate ARGB1555 map
  textures. `tilegen_update_map_1555()` redraws only changed name entries;
  PVR rectangles apply scroll, stencil and the behind/in-front priority.
- Filtering separate alpha layers is not always pixel-equivalent to filtering
  the composed pass. Colour offsets, two layer pairs on the same priority
  plane, and active line scroll therefore use the established screen-space
  path. This keeps transitions and the early race exact while retaining the
  incremental path for the expensive live HUD.
- Full scripted Flycast race through field 6000: **783.299 s instead of
  1057.818 s** (screenshot holds excluded), 25.9 % less time / 1.35x as fast.
  All 30 RAM signatures match the desktop reference; all 22 A/B screenshots
  are pixel-identical to the old Dreamcast path. The texture cache peaks at
  697 KB with no refused texture inside its new 1 MB budget.
- `M3_TG_MAP_CHECK=1` composes the persistent maps on the desktop and compares
  every eligible pass with the screen-space renderer. A 6000-field scripted
  race completed with no difference, including stencil, scroll, line-scroll
  and layer-depth changes.
- The Dreamcast executable now rejects a version-1 `SCUD.DAT`: it has no VROM
  and can only produce 2D over a black scene. Rebuild the pack with the current
  `tools/scud/pack.py` (version 2).

## 2026-10-07 (afternoon) — the HUD stops costing half a field

- **Profile of the race** (fields 4400-5400, `SCUD_DC_PROFILE`, now dumped
  whole and summed by function with `tools/dreamcast/profile.py`): 50 % in
  tilegen's `render_pass` -- the 2D layers drawn whole, pixel by pixel,
  every field, because the HUD changes every field (`M3_TG_CHANGES=1`: the
  name table of layer 0 every field; late in the race the scroll of layers
  0 and 1 too, so drawing only changed tiles would not have been enough).
  Lifted guest code: 13 %. The 3D path (`draw_model`, `polygon`, clipping,
  `real3d_render`): ~15 %.
- **The 1555 path draws a tile at a time** (`pair_line_1555`): name entry
  and pattern row fetched once per tile, empty tiles skipped whole, palette
  straight to 1555. A colour offset (fades) keeps the per-pixel path.
  `M3_TG_CHECK=1` draws every pass both ways and compares: 12 000 passes of
  the desktop race, 0 differences (and it does report a deliberately broken
  fast path, from field 408).
- Scripted race to field 6000: **1200 s instead of 1828 s** (2983 s this
  morning). Signatures equal the desktop's at all 30 checkpoints; race
  screenshots pixel-identical but one, taken a field late by the harness.
- Next in the profile: the 3D path and `sqrt` (3 % in `__ieee754_sqrt`).

## 2026-10-07 (later) — the disc stops being the race's bottleneck

- **The ROM caches hold 4 KB pieces**, 1 MB each for the CROMs and the
  VROM, with an index from piece to slot. In a race the renderer touches up
  to 38 different 32 KB pages of VROM a field and the guest 35 of banked
  CROM -- more than the 32-page caches, which thrashed every field -- but
  only ~580 KB and ~420 KB of them (`M3_VROM_WS=1`, new `M3_CROM_WS=1`).
- **The pack reads ahead only in its hot pages** (the boot's first-use
  order); elsewhere a miss reads exactly its 4 KB, by DMA into an aligned
  slot. A miss used to read 256 KB of pages nobody asked for.
- Scripted race to field 6000, Flycast: **1828 s instead of 2983 s**; disc
  **63 s (2526 reads) instead of 1214 s (9111)**; fields 5500-6000 232 s
  instead of 895 s. Signatures equal the desktop's at all 30 checkpoints,
  race screenshots pixel-identical to before. The boot reads a little more
  (37 s of disc by field 4000, was 22).
- What is left is the CPU: ~300 ms a field. Next: a profile of the race,
  aggregated by function.

## 2026-10-07 — DC-08: the race on the Dreamcast, field for field the desktop's

- **The scripted race runs on the Dreamcast** (Flycast): coin at 1500,
  start at 1600, accelerator held -- the selection screens, the rolling
  start, GO!, the beginner course with the HUD live. RAM signatures every
  200 fields from 200 to 6000: **all 30 equal to the desktop's**. Field
  6000 on both: lap 1/4, 41 s left, 101 km/h, 39th, one retired.
- `SCUD_DC_COIN_AT`, `SCUD_DC_START_AT`, `SCUD_DC_ADC_SET` give the
  Dreamcast build the desktop's scripted player; `SCUD_DC_SHOT_EVERY` /
  `SCUD_DC_SHOT_FROM` with `DC_SHOTS=<dir>` in `dc-run.sh` photograph it.
- **DC-BUG-017**: until the fix the race parted from the desktop at field
  4234, just after GO! -- GCC turned a Hermite blend into `fneg ; fmac`
  with the `fneg` in double mode, which lost the sign. `-ffp-contract=off`
  everywhere, and `tools/dreamcast/check-fneg.sh` fails the Dreamcast link
  if lifted code contains an `fneg`. `fctiw` now saturates as the 603e does
  (`m3_f2i32`, tested; it was not the cause).
- **New tools for a divergence between hosts**, each one Dreamcast run:
  `M3_SIG_FROM` (dense signatures from a field); `M3_RAMREF=field,path`
  (blocks that differ, against a table the desktop wrote and the disc
  carries); `M3_WORKSIG` (a RAM hash at each dispatched call);
  `MODEL3RECOMP_FN_TRACE` + `M3_FNSIG` (registers at each function entry);
  `M3_FTZ` on the desktop (denormals flushed, as the SH-4 does).
- **Measured** (Flycast, not hardware): ~330 ms a field on the selection
  screens, 400-600 ms in the race. **Disc reads: 1214 s of 2983 s** by
  field 6000, 8568 VROM page misses -- the race streams its VROM through a
  32-page cache. That, not the CPU, is the first thing to fix for speed.
- **Seen, not chased**: at field 6000 the Dreamcast draws a large dark
  triangle across the left of the screen where the desktop draws the
  overpass (near-plane clipping, probably), and shows palms and sails the
  desktop capture does not. The game state is the same; the renderers
  differ.
- Not yet: sound; steering under a script; real hardware.

## 2026-10-06 (night) — SCUD-PC: a race, played on the desktop

- **Scud Race runs a race on the desktop**, driven by a script (coin at
  field 1500, start at 1600, accelerator held, wheel centred): course,
  car and transmission screens time out to their defaults, rolling start
  under AUTO DRIVE, 3-2-GO!, then the beginner course -- tunnels, the
  glass tube, the viaducts -- with the HUD live (lap 1/4, lap timer to
  0'54", countdown 60 to 5, position 40th to 37th). 8000 fields, no stall.
- **Cabinet inputs**: Supermodel's drive-cabinet model -- an 8-channel ADC
  at register 0x3C (wheel, accelerator, brake), view buttons and the
  four-position gear lever on register 0x08 -- fed from a host pad through
  a per-game `input_map` (`scud_input_map`): stick/d-pad the wheel, R or A
  the accelerator, L or B the brake, up/down the gears, X the view, Y a
  coin, Start. SDL (keyboard and pad) and Maple fill the same `m3_pad_t`.
  `M3_ADC_SET` / `M3_IN08_AT` script them.
- Two bugs on the way (BUGS.md): **DC-BUG-015**, the race start waited for
  ever on the sound command queue (the UART's transmitter interrupt is now
  a level, and sending a byte drops it until the handler returns);
  **DC-BUG-016**, frozen on GO!: a jump through LR (`mtlr r31 ; blr` back
  into a loop) lifted as a return -- a lifter fix, with
  `tests/test_lifter.py` -- plus one race-only entry point (`0x35740`).
- The guest exiting to the idle loop is now reported; it used to look like
  a game that had frozen.
- Not yet: the race on the Dreamcast; steering under a script (the car
  rides the right-hand wall at ~101 km/h); sound (the run has none).

## 2026-10-05 (night) — DC-07 tilemap layers

- **The 2D is on the Dreamcast**: AM2 logo, CREDIT 0, © SEGA 1996 and the
  SEGA logo over the textured 3D (Flycast, field 1300).
- tilegen.c draws each priority pass into a 512x512 ARGB1555 texture in
  PVR memory (`tilegen_render_pass_1555`); the PVR back end shows them as
  two screen quads, behind (opaque list, far) and in front (punch-through,
  depth test off).
- Drawn on the CPU per pixel, the 2D first cost more than the rest of the
  field (135 → 350 ms). Redrawn now only when the guest changes tilegen
  VRAM or a register (`bus_tilegen_generation`, value compared on write):
  Scud rewrites 67 identical bytes and six identical registers every
  field, and the picture actually changes in ~6 % of attract fields
  (up to 32 % in places). 40 redraws by field 1400; 152 ms a field.
- Desktop pictures identical (fields 500–2500).

## 2026-10-05 (evening) — DC-06 textures

- **Textured 3D on the Dreamcast.** Village course with sky, stone
  houses, tiled roofs, road, grass (Flycast capture at field 1300).
- Measured first (desktop, `M3_TEX_STATS=1`): all 108 texture uploads of a
  6000-field attract come through the FIFO, none through the VROM port;
  6.9 M texels uploaded, 3 M of the sheet written by field 800 (6 MB).
  The payloads are not in any ROM (decompressed by the game), so they
  cannot be re-read later. A field uses ≤ 73 textures, ≤ 718 K texels.
- So the sheet is kept in PVR memory at half size (2 MB, point sampled:
  exact for every texel format); textures are converted on first use
  per (rectangle, format) into ARGB1555/4444, LRU in a 2.5 MB budget;
  any upload empties the cache. Three lists: opaque, punch-through
  (texel alpha), translucent. `M3_SHEET_SHIFT=1` shows the half-size
  sheet on the desktop.
- Field 1400: 78 textures, 398 KB, none refused; heap 10.6 MB (VROM cache
  cut to 32 pages, kept lists to 1024/512 — the first try ran out of
  memory at 11.2 MB of heap). VROM misses unchanged (53).
- Next: the tilemap layers on the PVR (no 2D at all yet on the console).

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
