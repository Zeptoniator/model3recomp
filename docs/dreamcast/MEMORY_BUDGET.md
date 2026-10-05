# Memory budget: Scud Race on a Dreamcast

The Dreamcast has 16 MB of main RAM, 8 MB of PVR video RAM and 2 MB of AICA
sound RAM. The practical target for main RAM is 13–14 MB used, leaving room
for KOS, the stack and fragmentation.

**Bottom line: the board as the desktop runtime models it does not fit, and
the port cannot be "the desktop runtime, recompiled".** Three things have to
change before Scud Race can run on the console: ROMs streamed instead of
mapped, the Real3D's memories sized to what the game uses, and the texture
sheet moved out of main RAM. Each is below with what is measured and what is
still an estimate.

## What the desktop runtime allocates

Read from the source (`src/bus.c`, `src/real3d.c`, `src/sound.c`):

| Item | Size | Where |
|---|---:|---|
| Guest work RAM | 8 192 KB | `bus_init`, `M3_RAM_SIZE` |
| Tilegen VRAM + palette | 1 152 KB | `bus_init`, `TILEGEN_VRAM_SIZE` |
| Battery RAM | 128 KB | `bus_init` |
| Culling RAM low / high | 4 096 + 1 024 KB | `CULL_LO_SIZE`, `CULL_HI_SIZE` |
| Polygon RAM | 4 096 KB | `POLY_SIZE` |
| Real3D FIFO | 4 096 KB | `TEXFIFO_CAP` |
| Texture sheet 2048×2048×16 bit | 8 192 KB | `real3d.c`, `g_sheet` |
| Sheet "texel set" flags | 4 096 KB | `g_sheet_set` |
| Triangle queue (40 000 × `tri_t`) | ~6 000 KB | `MAX_TRIS` |
| Z-buffer + framebuffer (496×384) | 1 488 KB | `real3d.c`, platform |
| Sound board RAM (2 × SCSP) | 2 × `SND_RAM` | `sound.c` |
| **ROMs, as pointers** | **2 + 48 + 32 + 0.5 + 8 MB** | handed to `model3recomp_init` |

Roughly 40 MB of board plus 90 MB of ROM. A desktop does not notice.

## Measured on the Dreamcast (Flycast, KOS 2.2.1)

`examples/dreamcast_boot` — the board brought up with no game:

| Measurement | Value |
|---|---:|
| Heap in use before `model3recomp_init` | 92 KB |
| Heap in use after `model3recomp_init` | **9 572 KB** |
| …of which guest RAM + tilegen VRAM + battery RAM | 9 472 KB |
| Real3D regions | 0 KB (allocated on first use on Dreamcast since `afc978d`) |
| ELF text, runtime + KOS | 645 KB (KOS "hello": 293 KB, so the runtime is ~352 KB) |
| ELF BSS | 1 201 KB — Musashi's jump table (256 KB) and cycle table (320 KB), SCSP `LPANTABLE`/`RPANTABLE` (256 KB each): the sound board, reserved even when it never runs |

Lifted Scud Race code compiled with `sh-elf-gcc -O2` (`KOS_CFLAGS`), see
CODE_SIZE.md: **2.89 MB** as lifted today, of which 1.41 MB is the ROM half,
which at runtime enters exactly one function (`0xFFF00100`).

## Measured on Scud Race's attract mode (desktop, field 4300)

`M3_DUMP_SCENE` at field 4300 (attract: INSERT COIN(S), the 3D scene
submitted every field), then for each region the 4 KB pages holding any
non-zero byte and the highest non-zero offset:

| Region | Modelled | Pages touched | Highest non-zero |
|---|---:|---:|---:|
| Guest work RAM | 8 192 KB | 1 372 KB | `0x1EFFBF` (≈ 2 MB; the stack top is `0x1F0000`) |
| Culling RAM low | 4 096 KB | 940 KB | `0x1B82F7` |
| Culling RAM high | 1 024 KB | 44 KB | `0x162BB` |
| Polygon RAM | 4 096 KB | 492 KB | `0x7A4B3` |
| Tilegen VRAM + palette | 1 152 KB | 676 KB | `0x11A47D` |
| Real3D FIFO | 4 096 KB | 4 096 KB | full: it is never reset (DC-BUG-008), so this measures the bug, not the game |

So the regions the desktop allocates at 22 MB hold about 3.5 MB during
attract. Two consequences, both to confirm over a race before relying on
them:

- Guest RAM above 2 MB is untouched; a 2 MB window with a bus fault (or a
  page allocated on demand) above it would save 6 MB.
- Culling and polygon RAM allocated by page on first write would cost
  ~1.5 MB instead of 9 MB.

## Estimate for Scud Race (to be replaced by measurements)

| Item | Estimate | Basis |
|---|---:|---|
| KOS + runtime code | 0.65 MB | measured |
| Runtime BSS without the sound board | 0.1 MB | measured 1.2 MB minus the four tables above |
| Lifted code, RAM half | 1.47 MB | measured |
| Lifted code, ROM half cut to the boot path | ≤ 0.1 MB | hypothesis: only `0xFFF00100`'s call tree runs |
| Guest work RAM | 8.0 MB | **hard**: the guest addresses it directly. Scud's boot touches `0x0..0x200000`; the game's high-water mark is not measured yet |
| Tilegen VRAM | 1.1 MB | could live in PVR VRAM if the tilemap is drawn there |
| Real3D culling + polygon RAM | ? | **measure**: high-water mark per region on the desktop over an attract loop and a race |
| CROM bank cache | 0.5–1 MB | streamed from disc in 64 KB pages; size from the access trace (prompt §66) |
| VROM cache | 0.5–1 MB | same; the Real3D reads models from it every frame |
| Geometry staging for the PVR | 0.3 MB | hypothesis |
| Stack, KOS heap slack | 0.5 MB | |
| **Total** | **≈ 13 MB + Real3D regions** | |

The total only closes if the Real3D's culling and polygon RAM, which are
9 MB on the desktop, come to well under 1 MB in practice — or are paged.
That is the first number to measure, and it can be measured on the desktop
today: add a high-water mark to `direct()` for each region and run the
attract loop.

## Where the texture sheet goes

8 MB of 16-bit texels cannot sit in main RAM next to 8 MB of guest RAM. The
PVR's 8 MB holds the framebuffers (2 × 640×480×16 bit = 1.2 MB) and the
polygon buffers, leaving roughly 5–6 MB for textures. Options, in order of
preference:

1. Decode uploads straight into a PVR texture cache keyed by texture
   (position, size, format), LRU-evicted; keep no copy of the sheet. Needs
   to know which sheet rectangles a frame uses (counted per frame, §42).
2. Keep the sheet in PVR VRAM as raw 16-bit texels (it is exactly 8 MB, so
   only part of it fits) and convert on use.
3. Keep a compressed or sparse copy in main RAM, decoding on demand.

None is chosen yet: the first measurement needed is how many distinct
texels a race actually references.

## ROMs

Banked CROM (48 MB), VROM (32 MB) and the DSB's MPEG ROMs (8 MB) are read
through `rom_read(offset, dst, size)` with a page cache, from the disc in
`SCUD.DAT` (prompt §22–23). The fixed program ROM (2 MB) is only read by the
boot, which a static build has already done: at runtime it is needed only
for whatever data the game reads from `0xFFE00000..` — to be traced.
