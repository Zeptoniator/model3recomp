# Real3D materials -> PowerVR2

Every polygon and material attribute `src/real3d.c` decodes: where it comes
from, what the software renderer does with it, and how a PVR backend could
reproduce it.

**Every PVR column below is proposed. Nothing on the PVR side is
implemented.** Names are KallistiOS (`pvr_poly_cxt_t`, `PVR_*`). Where the
PVR cannot match the software result exactly, the entry says so and what it
looks like.

Header words are `ph[0..6]`, read in `draw_model()` (`src/real3d.c:697`),
little-endian from polygon RAM or big-endian from VROM. "Software" means the
current renderer: `draw_model()` -> `emit()` -> `tri_t` -> `raster()`. The
pipeline itself is in `REAL3D_PIPELINE.md`.

## Summary: choosing the list (proposed)

    alpha < 255                                    -> PVR_LIST_TR_POLY
    textured && (fmt == 0 || alpha_test || tex_alpha)
             && the converted texture has a texel with alpha < 128
                                                   -> PVR_LIST_PT_POLY
    everything else                                -> PVR_LIST_OP_POLY

This mirrors the software's two passes (`src/real3d.c:1122`: opaque, then
`alpha < 255`) and its discard condition (`src/real3d.c:1276-1277`). The
"has a texel below 128" check is made once at texture conversion; without
it every format-0 polygon would land in PT, which costs more than OP.

Common to all lists (proposed):

| `pvr_poly_cxt_t` field | value | why |
|---|---|---|
| `gen.culling` | `PVR_CULLING_NONE` | backfaces are rejected on the CPU, see "double-sided" |
| `gen.shading` | `PVR_SHADE_GOURAUD` | lighting computed per vertex on the CPU |
| `depth.comparison` | `PVR_DEPTHCMP_GREATER` | vertex z = 1/z_view; software is `z < zbuf`, strict (`src/real3d.c:1262`) |
| `depth.write` | `ENABLE` (OP, PT), `DISABLE` (TR) | software writes depth only for opaque (`src/real3d.c:1321-1330`) |
| `txr.filter` | `PVR_FILTER_BILINEAR` | `sample()` is bilinear (`src/real3d.c:516`) |
| `txr.mipmap` | disabled | no mip level is stored (`src/real3d.c:311-319`) |
| `gen.fog_type` | `PVR_FOG_DISABLE` | no fog decoded |

## Attributes

### Discard (polygon not drawn)

- **Source:** `ph[0]` bits 8 and 9 both set (`src/real3d.c:809`). Same bits
  on a culling node hide the node and its sibling chain (`src/real3d.c:907`).
- **Software:** no triangle emitted; its vertices are still kept for the next
  polygon's reuse (`src/real3d.c:822-827`).
- **PVR (proposed):** identical -- nothing submitted. Exact.

### Lighting / luminous

- **Source:** `ph[6]` bit 16 = luminous (`src/real3d.c:795`). Light vector
  viewport words 0x04..0x06, intensity word 0x07 (clamped 0..2), ambient
  word 0x24 bits 8..15 / 255 (`src/real3d.c:1624-1638`, `1680-1682`).
- **Software:** luminous -> shade = `intensity`. Otherwise
  `shade = (ambient + (1 - ambient) * clamp(N.L, 0, 1)) * intensity`,
  evaluated **per pixel** with the interpolated, renormalised vertex normals
  (`src/real3d.c:1292-1320`); the face normal is used only when those
  degenerate (`src/real3d.c:1189-1199`).
- **PVR (proposed):** compute the same shade on the SH-4 per vertex, from
  `vn` normalised (`tri_t.vn`, `src/real3d.c:498`); vertex ARGB =
  `colour * shade`. Luminous: vertex colour = `colour * intensity`.
- **Not exact:**
  - Per-vertex instead of per-pixel lighting: highlights and terminators on
    large polygons smear across the face; flat-ish terrain looks the same,
    curved low-poly models (boulders, the T-Rex) show slightly softer
    shading bands. Expected to be minor; the hardware itself shades
    Gouraud.
  - `intensity` up to 2.0 (`src/real3d.c:1682`): PVR vertex colours saturate
    at 1.0, so `shade > 1` cannot brighten a texture. Software multiplies
    textured channels without clamping (`src/real3d.c:1313-1316`), so it is
    wrong there too (channel overflow). A PVR backend would clamp; the
    offset (specular) colour could add the excess for untextured polygons.

### Smooth / Gouraud

- **Source:** no header bit is decoded for it.
- **Software:** every polygon gets interpolated vertex normals (above).
- **PVR (proposed):** `PVR_SHADE_GOURAUD` always. `PVR_SHADE_FLAT` only if a
  smooth bit is later found and decoded. Exact apart from the per-pixel
  point above.

### Colour

- **Source:** `ph[4]` bits 31..24 R, 23..16 G, 15..8 B (`src/real3d.c:747-748`).
  When `ph[1]` bit 1 is **clear**, `ph[4]` bits 8..19 index a colour table in
  polygon RAM at word `g_ctab + index`, low 24 bits used
  (`src/real3d.c:751-756`); `g_ctab` is set by a culling node with word 0
  bit 2 (`src/real3d.c:909-912`).
- **Software:** untextured -> the lit colour; textured -> texel RGB *
  colour / 255 (`src/real3d.c:1280-1283`), then lit.
- **PVR (proposed):** vertex colour = colour * shade, 8 bits per channel,
  `txr.env = PVR_TXRENV_MODULATE` (or `MODULATEALPHA` in TR). Exact.

### Translucency (polygon opacity)

- **Source:** `ph[6]` bit 23 set = opaque; clear -> opacity =
  `bits 18..23 * 255 / 32` (`src/real3d.c:800-802`), so 0..247 since bit 23
  is clear on that path.
- **Software:** drawn in a second pass after all opaque triangles of the
  same viewport, in submission order, depth-tested, no depth write,
  `dst = src*a + dst*(255-a)` (`src/real3d.c:1321-1327`).
- **PVR (proposed):** `PVR_LIST_TR_POLY`, `gen.alpha = PVR_ALPHA_ENABLE`,
  `blend.src = PVR_BLEND_SRCALPHA`, `blend.dst = PVR_BLEND_INVSRCALPHA`,
  vertex alpha = opacity, `depth.write = DISABLE`. Textured:
  `txr.env = PVR_TXRENV_MODULATEALPHA` with `txr.alpha` as for the texture
  variant (below).
- **Not exact:** the PVR sorts the TR list per pixel by depth (autosort);
  software blends in submission order. Overlapping translucent polygons
  (mist sheets, the attract title's 40% veil over its backdrop) may stack
  in a different order -- arguably more correct, but it can differ from the
  oracle. Disabling autosort (presort mode) would match submission order
  instead.

### Contour (alpha test)

- **Source:** `ph[6]` bit 31 (`src/real3d.c:776`). With it, formats 1..4 are
  remapped to 8..11 (`src/real3d.c:781`).
- **Software:** a textured pixel whose bilinearly filtered alpha is < 128 is
  discarded -- no colour, no depth (`src/real3d.c:1276-1277`). Before
  filtering, transparent taps borrow the RGB of the most opaque tap
  (`src/real3d.c:540-547`).
- **PVR (proposed):** `PVR_LIST_PT_POLY` with the punch-through alpha
  reference set to 128; texture converted with its alpha (or 1-bit alpha:
  `a >= 128`). `txr.alpha = PVR_TXRALPHA_ENABLE`.
- **Not exact:** the neighbour-colour trick has no hardware equivalent.
  Without it, bilinear filtering blends the RGB of transparent texels into
  the edge -- a dark or coloured fringe on foliage and fences. Mitigation:
  at conversion, copy the nearest opaque texel's RGB into transparent
  texels (dilation); this matches the software wherever the most opaque tap
  is unambiguous.

### Texel alpha (`tex_alpha`)

- **Source:** `ph[6]` bits 0..2 non-zero (`src/real3d.c:777`).
- **Software:** same discard as contour (< 128), nothing else; texel alpha
  never blends (`src/real3d.c:1276`).
- **PVR (proposed):** as contour -> PT. Exact up to filtering, as above.

### Translucent *and* cut-out

- **Source:** opacity < 255 together with format 0, contour or `tex_alpha`.
- **Software:** discard below 128, then blend the survivor at the polygon
  opacity.
- **PVR (proposed):** TR list, `txr.env = PVR_TXRENV_MODULATEALPHA`,
  texture converted with **1-bit** alpha (ARGB1555, `A = a >= 128`) so that
  `texel_a * vertex_a` is either 0 or the opacity -- the same as threshold
  then blend.
- **Not exact:** bilinear filtering of the 1-bit alpha gives a 1-texel soft
  edge where software has a hard one.

### Double-sided

- **Source:** `ph[1]` bit 4 (`src/real3d.c:803`).
- **Software:** one-sided triangles are culled when
  `dot(face normal, centroid) > 0` in view space (`src/real3d.c:1140-1146`)
  -- the header normal decides, not the winding.
- **PVR (proposed):** run the same test on the CPU and submit survivors with
  `gen.culling = PVR_CULLING_NONE`. Using `PVR_CULLING_CW/CCW` instead would
  rely on screen winding, which is unverified for this data (the Y flip in
  `g_proj.ay < 0` reverses it) and differs from the software for any
  polygon whose header normal disagrees with its winding. Exact on the CPU
  path.

### Texture enable

- **Source:** `ph[6]` bit 10, ANDed with the global `g_textures`
  (`M3_NO_TEXTURES`) (`src/real3d.c:782`).
- **Software:** samples the sheet; if any tap was never uploaded the pixel
  keeps the untextured colour (`src/real3d.c:1270`) -- and then has the
  shade applied a second time.
- **PVR (proposed):** textured header (`pvr_poly_cxt_txr`) when enabled and
  the texture rectangle has been written; otherwise untextured
  (`pvr_poly_cxt_col`).
- **Not exact:** "missing" is per tap in software, per texture on the PVR.
  A partly uploaded rectangle would show black/garbage regions instead of
  the flat colour. The double shading of missing texels is a software bug
  the PVR would not copy.

### Texture page, origin and size

- **Source:** origin x = `32 * ((ph[4] & 0x1F) << 1 | (ph[5] >> 7) & 1)`,
  y = `32 * (ph[5] & 0x1F)` (`src/real3d.c:787-789`); page = `ph[4]` bit 6
  XOR the inherited node page (`src/real3d.c:790`); inherited node offset
  added, `x &= 2047`, `y &= 1023` (`src/real3d.c:791-792`). Width
  `32 << ph[3] bits 3..5`, height `32 << ph[3] bits 0..2`, exponents 6 and 7
  read as 0 (`src/real3d.c:769-773`), so 32..1024.
- **Software:** a rectangle of the 2048x2048 sheet; page 1 is rows
  1024..2047 (`src/real3d.c:529`).
- **PVR (proposed):** a texture cache keyed by
  `(x, y, page, w, h, fmt, alpha variant)`; each entry a separate twiddled
  PVR texture of exactly `w x h` (power of two, 32..1024: within PVR's
  8..1024). UVs normalised by `w`, `h`.
- **Not exact:** nothing in the mapping, but two polygons that address
  overlapping rectangles of the sheet get two copies in VRAM, and an upload
  that overwrites part of a rectangle must invalidate every cached entry
  that overlaps it.

### UV, UV scale, mirroring and wrap

- **Source:** vertex word 3, u = bits 31..16, v = bits 15..0, unsigned, in
  1/8 texel, or whole texels when `ph[1]` bit 6 (`src/real3d.c:740-743`).
  Mirror `ph[2]` bits 0..1: bit 0 mirrors v, bit 1 mirrors u
  (`src/real3d.c:778`).
- **Software:** always repeat; a mirrored axis is mirrored-repeat
  (`wrap_axis()`, `src/real3d.c:477`). No clamp mode is decoded.
  Perspective-correct (`src/real3d.c:1174-1176`, `1265-1268`).
- **PVR (proposed):** `txr.uv_flip`: bit 1 -> `PVR_UVFLIP_U`, bit 0 ->
  `PVR_UVFLIP_V`, both -> `PVR_UVFLIP_UV`; `txr.uv_clamp = PVR_UVCLAMP_NONE`.
  u/v = texel coordinate / size, as floats. Exact; the PVR is always
  perspective-correct.
- **Note:** u, v reach 65535/8 = 8192 texels; as normalised floats this is
  fine, but if a 16-bit-UV vertex format is used, repeat counts that large
  lose precision.

### Depth

- **Source:** none in the header; per viewport.
- **Software:** view-space z, interpolated linearly in screen space
  (`src/real3d.c:1260`), strict less-than, buffer cleared to `1e30f` per
  viewport (`src/real3d.c:1647-1650`). Near clip at z = 0.05
  (`clip_near()`, `src/real3d.c:1012`).
- **PVR (proposed):** clip on the CPU with `clip_near()` (the PVR does not
  clip against the near plane), vertex z = 1/z. One depth buffer per frame,
  so each viewport gets its own disjoint 1/z band, later viewports nearer.
- **Not exact:** software depth is not perspective-correct, the PVR's is;
  intersecting polygons can resolve along a slightly different line.

### Viewport rectangle

- **Source:** viewport words 0x1A / 0x14 (`src/real3d.c:1595-1610`).
- **Software:** per-pixel scissor in `raster()` (`src/real3d.c:1218-1223`).
- **PVR (proposed):** PVR user clip, which is in 32x32 tile units; for a
  rectangle not on tile boundaries, clip the geometry on the CPU as well.
- **Not exact** with user clip alone: up to 31 pixels of overdraw past a
  HUD viewport's edge.

## The twelve texel formats

As `texel_argb()` decodes them (`src/real3d.c:448-473`), `v` the 16-bit sheet
word, L a grey level written to R, G and B. Formats 0..7 come from `ph[6]`
bits 7..9 (`src/real3d.c:775`); 8..11 only from 1..4 with the contour bit
(`src/real3d.c:781`). Texel alpha is used by the software only for format 0,
or when contour / `tex_alpha` is set (`src/real3d.c:1276`); otherwise the
texture is drawn opaque whatever its alpha.

| fmt | decode in `texel_argb()` | line | proposed PVR format | exact? |
|---|---|---|---|---|
| 0 | R = bits 14..10, G = 9..5, B = 4..0 (5->8 by replication); A = 0 if bit 15 set, else 255 | 452-456 | `ARGB1555`, bit 15 inverted on conversion (PVR: 1 = opaque) | yes |
| 1 | L = bits 0..3, A = bits 4..7 (x17) | 457 | `ARGB4444`, RGB = L, A = A | yes |
| 2 | L = bits 4..7, A = bits 0..3 | 458 | `ARGB4444` | yes |
| 3 | L = bits 8..11, A = bits 12..15 | 459 | `ARGB4444` | yes |
| 4 | L = bits 12..15, A = bits 8..11 | 460 | `ARGB4444` | yes |
| 5 | L = bits 0..7; A = 0 if L == 255, else 255 | 461 | `PAL8BPP`, grey-ramp palette with entry 255 transparent | yes, with an ARGB8888 palette |
| 6 | L = bits 8..15; same key | 462 | `PAL8BPP`, same palette | as 5 |
| 7 | R = bits 15..12, G = 11..8, B = 7..4, A = 3..0 (x17) | 463-466 | `ARGB4444`, nibbles rotated to A,R,G,B | yes |
| 8 | L = bits 0..3 (x17); A = 0 if L == 255, else 255 | 467 | `PAL4BPP`, 16-grey palette, entry 15 transparent | yes |
| 9 | L = bits 4..7; same key | 468 | `PAL4BPP` | yes |
| 10 | L = bits 8..11; same key | 469 | `PAL4BPP` | yes |
| 11 | L = bits 12..15; same key (the `default:` case) | 470 | `PAL4BPP` | yes |

Notes, all proposed:

- Every format needs a conversion pass anyway: the sheet is linear, PVR
  textures should be twiddled, 8-bit textures share 16-bit sheet words
  (upload bits 21/22, `src/real3d.c:301`), and greyscale must be expanded or
  paletted. So the PVR format is free to differ from the sheet's.
- Formats 1..4 and 7 could also be `PAL8BPP` (one byte per texel, half the
  VRAM), but the four nibble orders need different palettes; ARGB4444 is
  simpler and exact.
- For a polygon that does not use texel alpha (no contour, no `tex_alpha`,
  format != 0), the same texture can be converted without alpha: `RGB565`
  for 0 and 7, the paletted forms with an opaque palette for the rest.
  `fmt` alone is therefore not a sufficient cache key.
- Palette format on the PVR is global. An ARGB8888 palette keeps 5/6's eight
  bits of grey exactly; ARGB4444 or ARGB1555 palettes would drop to 4 or 5
  bits (visible banding on smooth greyscale gradients, e.g. sky or smoke).
  Its filtering/fill cost on the PVR should be measured before choosing.
- `PVR_TXRENV_MODULATE` tints greyscale formats by the vertex colour, which
  is what the software does with the polygon colour
  (`src/real3d.c:1280-1283`).

## What the PVR has that the software does not use

Recorded so a later decode has somewhere to go; none is mapped today.

- **Fog:** PVR fog table / vertex fog. No Real3D fog field is decoded.
- **Specular:** PVR offset colour (`gen.specular`). No Real3D specular field
  is decoded.
- **Mipmaps:** PVR mipmapped twiddled textures. Real3D mip data is uploaded
  (type 0 / 2 entries) but discarded at `src/real3d.c:318`.
- **Spotlight:** no PVR equivalent beyond per-vertex CPU lighting; not
  decoded on the Real3D side either.
