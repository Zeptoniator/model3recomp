# Status

## 2026-10-12 (night) — a ROM miss reads the rest of its 32 KB page

- A miss in the ROM piece caches read one 4 KB piece: 2,195 disc reads
  and 62.9 s of the 337.5 s race. Traced on the desktop
  (`M3_PIECE_TRACE`), the guest streams banked CROM: 82 % of its misses
  were followed by the next piece's, mostly pieces never seen before
  (3,400 distinct in 4,438 misses); VROM 59 %.
- Now a miss also brings in the uncached pieces after it in its SCUD.DAT
  page (32 KB), in the same read, up to `M3_PIECE_RA` (8; the desktop
  takes it from the environment). Desktop race with the Dreamcast's cache
  sizes: 1/2/4/8 pieces a read gave 5,499/2,938/1,655/1,058 reads for
  5,499/5,665/5,910/6,442 pieces fetched, and the same RAM signatures.
- Profiled race 0-5400: **312.4 s instead of 337.5 s** (-7.4 %), disc
  reads 768 instead of 2,195 (37.5 s instead of 62.9 s), all 27 RAM
  signatures equal; heap +32 KB (the read buffer). Blocked time in the
  profile: 8.2 % instead of 14.5 %. On a console, where a GD-ROM seek is
  far dearer than Flycast's, it should matter more.
- `sink_submit` walked 2n triangles as `t % n`, and both polygon cutters
  found the next vertex as `(i + 1) % n`: the SH-4 divides in software, a
  call each time (`__udivsi3`/`__sdivsi3`, 1.8 % of the profile). Two
  passes and a compare instead, same order: **307.5 s**, 27 signatures
  equal, desktop sink dumps byte-identical at fields 1300 and 5000.
- The PVR's tilemaps were refreshed by walking all 4,096 name entries of
  every drawn layer whenever anything in tilegen moved -- every field in a
  race, for the HUD. The bus now counts writes to each layer's name table
  (`bus_tilegen_name_generation`), and a layer whose names were not
  written is left as it is: **302.9 s**, 27 signatures equal; on the
  desktop `M3_TG_MAP_CHECK=1` over the race compares 10,000 passes of the
  maps against the screen-space path, 0 differ.

## 2026-10-12 (night) — the sound driver's sample copies, a word at a time

- In a race the 68000 runs ~3,500 instructions a field and copies ~5 KB of
  samples a field from the sample ROM into its chips' RAMs, a long word at
  a time (`movem.l`; `M3_SND_STATS` build). Every byte went through
  `rd16`, `rd8` and `rom8` one way and `wr16`, `wr8` and the AICA's byte
  gatherer the other: together ~5 % of the race profile.
- Musashi's callbacks now read 1, 2 or 4 bytes of the sample ROM in place
  (`smp_at`: through the bank, unless they wrap round the ROM or cross a
  cache page), and write a long word above the RAM kept on the SH-4 to the
  voices at once: `sound_voices_t.ram_write32`, which on the AICA is one
  G2 write of the swapped word -- what four `ram_w8` calls did, the pending
  word sent first, or dropped when this one replaces all of it.
- Profiled race 0-5400: **337.5 s instead of 350.4 s** (-3.7 %), all 27
  RAM signatures equal. On the desktop the 5400-field WAV is bit for bit
  the reference (SHA-256 `73063d1a...`, 16,557,232 bytes) both through the
  paged/AICA-shaped path (`M3_SND_PAGED=1`, which exercises
  `ram_write32`) and with the resident board. Recorded through Flycast
  (`DC_AUDIO_WAV`), the console's sound has the earlier recording's
  spectrogram -- attract music, selection screens, the engine's harmonics
  -- at the same level (RMS -40.5 dB against -40.8, peak -13.7 against
  -14.2), shorter because the race is.
- Tried and dropped, measured on `0x44D94` (6 % of the race): keeping the
  guest's registers in C locals instead of `m3_ctx` compiles to 1,585 to
  1,712 SH-4 instructions against 1,340 -- the SH-4 has eight double
  registers, so twenty-odd live guest registers spill to the stack, which
  costs what `m3_ctx` does. And this toolchain has no `-m4` (double by
  default) to drop the FPSCR switches: `-m4-single` only.

## 2026-10-12 (evening) — a TLB for the guest's CROM reads

- Every guest read of the CROMs went through `direct()`'s chain of range
  checks, then `rom_at()` and `piece_at()`'s index lookup: `rom_at` alone
  was 4.1 % of the race profile, `bus_read32` 2.5 %. A 64-entry TLB from
  4 KB guest page to cached piece now answers first in `bus_read8/16/32`
  (an aligned word is one load and a swap). A hit stamps the piece exactly
  as `piece_at` does, so the cache evicts what it did and the disc is read
  as before; an entry goes when its piece is evicted, when the bank
  register moves the window and on a save state.
- Profiled race 0-5400: **352.6 s instead of 373.4 s** (-5.6 %), all 27
  RAM signatures equal, the same 5,859 pieces fetched in the same 2,195
  disc reads. On the desktop, the race read through SCUD.DAT
  (`M3_SCUD_PACK`) gives the same signatures every 200 fields and the same
  pieces-fetched counts every 500, with and without the TLB.
- Measured in Flycast, which bills instructions by number and not by
  latency (a double-precision divide costs what a `mov` does): what it
  can judge is fewer instructions. Caching each vertex's transform and
  projection in `draw_model` by copying them along with the vertex took
  the race from 373.4 s to 377.5 s -- fewer multiplies and divides, more
  loads and stores.
- Kept instead: on the sink path `draw_model` holds a model's vertices in
  four records (`mvert_t`) and names them by index, so a vertex the next
  polygon reuses costs a byte, not sixteen loads and stores, and is
  neither transformed nor projected again; a quad's two triangles share
  their projections; the colour is worked out once a polygon; the sink
  path no longer decodes vertex normals it does not use; `sx24` is an
  arithmetic shift. Race **350.4 s instead of 352.6 s** (`draw_model`
  1,233 samples -> 1,056), 27 signatures equal; desktop sink dumps
  byte-identical at fields 1300 and 5000, and the software rasteriser's
  pictures pixel-identical there. On a console, where a double divide is
  ~24 cycles, it should be worth more than Flycast shows.

## 2026-10-12 — compact Real3D sink queue

- The PVR path now has its own 100-byte triangle record instead of using the
  software rasteriser's 132-byte `tri_t`. It projects, culls and shades while
  the transformed model vertices are still hot, then writes the final record
  once; `sink_bounds` no longer rereads the view-space triangle and overwrites
  most of the same entry. The software rasteriser keeps its full Gouraud data
  and is unchanged.
- The 6,144-entry queue is 600 KB instead of 792 KB. The profiled Dreamcast
  race reports 10,072 KB of heap instead of 10,264 KB and finishes field 5401
  in **373.416 s instead of 376.080 s** (another 2.664 s / 0.7 %); all 27 RAM
  signatures match. Direct PVR-sink dumps are byte-identical before and after
  at field 1300 (1,644 lines) and in the scripted race at field 5000 (9,160
  lines), including projected floats, depth, colour and texture descriptors.

## 2026-10-12 — sound: what the board does, and a board that only listens

Sound on the Dreamcast is to be the SCSPs' voices played by the AICA (their
descendant: same slots, same 8/16-bit PCM, 64 channels against 2 x 32), with
the 68000 driver run on the SH-4. Measured on the desktop over the scripted
race (`-DM3_SND_STATS` plus Musashi's instruction hook, see sound.c):
- The 68000 runs ~18,800 instructions a field -- all it has: most of it is
  the main loop at 0x601BBC / 0x602028 walking the 2 x 32 voice records
  and selecting each on its chip's monitor (register 0x408).
- Every voice keyed on is 8-bit PCM; no FM, no LFO; both chips run a DSP
  program (364 and 312 words, reverb) and most voices send to it. Voices
  play from the chips' own 512 KB RAMs (447 KB reached), which the driver
  fills from the sample ROM: ~5 KB a field at the start of the race, about
  nothing otherwise. The engine is a looped voice re-pitched, not re-keyed.
- On SH-4, Musashi is 284 KB of code and 590 KB of tables, and the program
  512 KB: more than is free. The guest's 4 MB RAM window (Scud uses ~2) and
  the 68000-only tables are where it would come from.

`M3_SND_DECOUPLED=1`: the board hears the PowerPC's bytes and plays them,
but the PowerPC sees `sound_null.c`'s board (always ready, no replies), so
the RAM signatures are those of a run without sound -- equal over the race
-- and the audio is the same to the ear (loudness per second within a few
per cent of the coupled board's). The Dreamcast will run this way.

`scsp_set_voices()`: a chip's voices played by something else. Every write
to a slot, to the common registers and to the DSP goes to it too, the slot
monitor (0x408) reads back from it, and with `only` the chip itself just
counts time (timers, interrupts, MIDI). `M3_SND_LOOPBACK=1` puts a second
pair of chips on the far side: the race's audio is bit for bit the
original's, and silent when the far pair is muted -- the split is whole.
The AICA goes where that second pair is.

**On the Dreamcast it plays** (on by default; `-DSCUD_DC_NO_SOUND` builds
without). The 68000 (Musashi, one cycle table) runs on the SH-4 against
chips built with `SCSP_NO_SYNTH` (5 KB, no tables); `aica_voices.c`
re-encodes each slot write into AICA channel registers (chip 1 -> 0-31,
chip 2 -> 32-63), the driver's sample writes go into AICA RAM gathered into
32-bit words, and the slot monitor reads back from the AICA. 32 KB of each
chip's RAM is kept on the SH-4 (the driver reads back pages 0-4 and nothing
else), and writes below 0x4000 -- its variables, which no voice plays --
stay there. The 68000 program and the sample ROM are read on demand from
/cd/SCUDSND.DAT (`scud_snd.bin` + `scud_samples.bin`): program in 24 4 KB
pages (11 misses a race), samples in 16 16 KB pages (eight thrashed: 1,834
misses; sixteen, 298 -- a replay of the race's reads). The desktop proves
the whole arrangement bit for bit (`M3_SND_PAGED=1`). Guest RAM is now a
3 MB window (the race never goes above 2 MB).

- AICA KEYONEX acts on all 64 channels, and, unlike the SCSP, a one-shot
  that ran out keeps KEYONB: every later KEYONEX restarted it, and the
  first recording never fell silent. `reap_ended()` unkeys those first.
- The 68000 runs at a quarter of its clock (`SND_CYCLES=64`): the driver
  mostly polls its voices; over the desktop race the audio is as close to
  the full clock's as two full-clock runs are (spectra 0.996) down to 40
  cycles a sample, and breaks at 32.
- Profile race 0-5400: 382 s with sound, 315 s without; RAM signatures
  equal between the two at every check. Recorded through Flycast
  (`DC_AUDIO_WAV`), the spectrogram has the desktop's shape -- attract
  music, silences, the engine's harmonics climbing through the gears --
  3.5 dB quieter, no clipping.
- The 68000's hot memory callbacks now read its local RAM and program
  directly, including 32-bit operands, and a program-page hit stays inline
  while only a miss enters the LRU routine. The same profiled race is
  376.080 s instead of 382.284 s; all 27 RAM signatures still match. On the
  desktop, a complete
  5400-field run through the paged/AICA-shaped path produces the exact same
  16,557,232-byte WAV as the resident board and the pre-optimisation build
  (SHA-256: `73063d1a7727e4a4e55c30bc808e83111dcdbc4f9ddd59dbe83929ddf01dbb92`).
- The board is stepped by the game's fields, so with the game ~5x slow the
  music's tempo is too (pitch is right). Decoupled, it could follow the
  wall clock instead -- at ~5x the 68000's cost.
- No DSP (reverb), so chip 2's voices, all of which send to it, are dry.

## 2026-10-12 — the race 35 % faster: no idle spin, no per-polygon copies

Profiling build, Flycast, fields 0-5400: **316 s instead of 660 s**, RAM
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
- Guest RAM words were read and written a byte at a time (17 SH-4
  instructions a load). An aligned access is now one `mov.l` and a swap;
  a misaligned one goes to the bus, because built from bytes inline it is
  the same value and GCC merged both branches into the byte loop. Guest
  code -5.6 % of samples; 423 -> 415 s. (Times are the console's own
  clock, emulated; Flycast's wall clock follows the load on the machine.)
- **One field drawn in two** (`SCUD_DC_RENDER_EVERY`, default 2;
  `M3_RENDER_EVERY` on a desktop). The guest runs every field; a field
  not drawn still takes in the texture uploads (`real3d_skip`), and the
  2D back end follows the tilegen's generation counters, so nothing is
  lost in between. RAM signatures equal, and the 22 photographed fields
  (1300, 2000-6000) are the same pixels as before. Race samples -29 %;
  415 -> 341 s.
- **Pure wait loops** (`game_loops.h`, `M3_PURE_LOOP`): a loop that only
  reads what changes at a loop tick -- the decrementer, the time base, RAM
  an interrupt handler writes -- goes straight to the iteration that
  ticks, at the same count spinning would reach. Seven in Scud: the two
  at 0x30704, 0xAE9C's wait for an interrupt (also an idle loop now), the
  33-tick delay at 0xAF70 and 0xB420's three. Signatures equal at every
  field to 2500 against a run that spins everything (`M3_NO_IDLE_SKIP=1
  M3_NO_PURE_SKIP=1`), and every 200 to 5400. 341 -> 321 s.
- `fctiw` rounded through newlib's `rint`, software on the SH-4 (1.6 %):
  below 2^51, adding and taking away 1.5 * 2^52 is the same rounding in
  two hardware additions (20 M values checked against `nearbyint`).
  321 -> 316 s.

At this point a race field (~110-150 ms) still spent about 15 % in
`draw_model`, 23 % in the guest, 10 % in `real3d_render` + `sink_project`
and 8 % waiting for the disc. The 132-byte / 792 KB triangle table identified
here is replaced by the compact sink queue described above.

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
