# The Real3D pipeline, as implemented

What `src/real3d.c` does with a frame, stage by stage, in the order the code
runs it. Every reference is `file:line` in this tree; a symbol that is not
named here does not exist. For *why* each structure decodes the way it does,
and the check behind each decode, read `docs/technical/real3d.md` first --
this file only says what the code does.

Written as the map for a PowerVR2 backend (Dreamcast). Nothing PVR-side
exists yet; the last section says where it would plug in.

## 0. Constants that shape everything

    MAX_TRIS              40000u     src/real3d.c:60    triangle queue capacity
    MAX_DEPTH             24         src/real3d.c:61    node recursion limit
    VERTEX_SCALE_DEFAULT  2048.0f    src/real3d.c:106   vertex divisor (M3_VERTEX_SCALE)
    TEX_W, TEX_H          2048u      src/real3d.c:254   texture sheet
    ZNEAR                 0.05f      src/real3d.c:1008  near plane, view space
    MAX_BANDS             16         src/real3d.c:1370  raster threads incl. caller

## 1. Where a frame starts

There is no command processor. The trigger register `0x88000000`
(`M3_R3D_TRIGGER`, `include/model3recomp/bus.h:23`) only counts writes
(`src/bus.c:895`, read back by `bus_r3d_trigger_count()`); it does not start
a render, and nothing is double-buffered. The renderer reads whatever is in
culling / polygon RAM at the moment it runs.

It runs once per field, from the VBlank tick, *after* the guest's VBlank
handler has run:

    irq_tick()                        src/irq.c:141
      irq_raise(VBLANK_START); irq_dispatch()
      model3recomp_end_frame()        src/irq.c:171
        memset(fb, 0)                 src/model3recomp.c:150
        tilegen_render_pass(0)        src/model3recomp.c:159   layers behind 3D
        real3d_render()               src/model3recomp.c:161
        tilegen_render_pass(1)        src/model3recomp.c:163   layers over 3D
        platform_present()            src/model3recomp.c:165

`real3d_render()` (`src/real3d.c:1481`) does not clear the framebuffer: the
behind-layers are already in it (`src/real3d.c:1511`). It returns at once if
`platform_framebuffer()` returns NULL (`src/real3d.c:1484,1489`) -- which the
Dreamcast platform always does (`src/platform/dreamcast/platform_dreamcast.c:29`).

One-time init (`src/real3d.c:1491-1506`): reads the `M3_*` environment
switches, `malloc`s `g_tris` (MAX_TRIS `tri_t`), `calloc`s `g_sheet` and
`g_sheet_set`. `g_tex_size_mode` and `g_tex_all` are set here
(`src/real3d.c:1495-1496`) and read nowhere else.

Per frame, before the walk:

- `flush_texture_fifo()` (`src/real3d.c:1509`) -- see section 8.
- Z-buffer (re)allocated to `fw*fh` floats on a size change and cleared to
  `1e30f` (`src/real3d.c:1514-1523`). It is cleared again per viewport
  (`src/real3d.c:1647-1650`), so the first clear is redundant.

## 2. Viewports

Loop at `src/real3d.c:1550-1762`. For `prio` 0..3, the list is walked from
the fixed address `0x800000` (`src/real3d.c:1551`), at most 16 entries
(`guard`), via `cull_at(vpaddr, 0x20)` (`src/real3d.c:1553`). A viewport is
skipped unless word 0 bits 3..4 equal `prio`, and skipped if word 0 bit 5 is
set (disabled) (`src/real3d.c:1559`). So viewports are drawn lowest priority
first; within a priority, in list order. The list links through word `0x01`;
a self-link ends it (`src/real3d.c:1560-1562`, `1758-1760`).

Per viewport:

    word   meaning (as read)                                    line
    0x00   bits 3..4 priority, bit 5 disabled                   1559
    0x01   next viewport                                        1758
    0x02   root culling node                                    1696
    0x04-06 light vector (x,y,z), floats                        1624-1626
    0x07   global intensity / cross-fade, clamped [0,2]         1680-1682
    0x08,09 1/(right-left), 1/(top-bottom), floats               1593
    0x0A,0x0B frustum centre (io, jo), floats                   1594
    0x0C,0x0E sin of horizontal / vertical half-angle            1571-1572
    0x14   width/height, 14.2 fixed (low / high halfword)       1595-1597
    0x16   matrix table base (culling address)                  1565
    0x1A   viewport x/y, 12.4 fixed (low / high halfword)        1595-1596
    0x24   bits 8..15: ambient / 255, if non-zero               1635-1638

Projection is `pixel = a * (X/Z) + b` in `g_proj` (`src/real3d.c:1059`):

- Symmetric fallback (`src/real3d.c:1571-1583`): `asinf` of words 0x0C/0x0E,
  `sx = (fw/2)/tan(h)`, `sy = (fh/2)/tan(v)`, whole-screen rectangle. If
  either angle is under 1e-3 rad it substitutes 0.338 / 0.262 rad
  (`src/real3d.c:1578`) -- The Lost World's own 19.4 / 15 degrees.
- Asymmetric frustum (`src/real3d.c:1592-1611`), used whenever words 0x08 and
  0x09 are > 1e-6 and the 0x14 rectangle is at least 1x1, unless
  `M3_OLD_PROJ`: `l = -jo/cvv`, `r = (1-jo)/cvv`, `b = -(1-io)/cww`,
  `t = io/cww`; `ax = vw/(r-l)`, `bx = vx - l*ax`, `ay = -vh/(t-b)`,
  `by = vy + t*vh/(t-b)`; scissor rectangle `g_proj.x0..y1` from 0x1A/0x14,
  clamped to the framebuffer.

`sx`/`sy` from the symmetric path are passed into `raster_bands()` /
`raster()` (`src/real3d.c:1702`) and unused there; `raster()` uses `g_proj`.

Lighting state: light from words 0x04..0x06, negated under `M3_LIGHT_FLIP`
(`src/real3d.c:1627`); an all-zero vector is replaced by the invented
(0.3, 0.5, -0.81) (`src/real3d.c:1639-1641`). Ambient `g_ambient` is a global
(default 0.20, `src/real3d.c:123`) and is only overwritten when word 0x24's
byte is non-zero -- a viewport that writes 0 inherits the previous
viewport's (or previous frame's) ambient. `M3_AMBIENT` pins it.

Camera: `cam = IDENT` (`src/real3d.c:1664`). Matrix 0 is not applied; node
matrices already carry geometry to view space. `M3_CAM0=1/2` loads matrix 0
as stored / transposed for comparison (`src/real3d.c:1665-1678`).

Then `g_ntris = 0` (`src/real3d.c:1684`), the walk
`descend(root, &cam, 0)` (`src/real3d.c:1696`), and the raster
`raster_bands(...)` (`src/real3d.c:1702`). Each viewport is walked and
rasterised to completion before the next; `M3_VP=n` limits to list index n.

## 3. Memory accessors

- `cull_at(addr, words)` (`src/real3d.c:152`): a culling address is a word
  index. `0x800000..0x83FFFF` -> `bus_cull_hi()` (`src/bus.c:315`), below
  `0x100000` -> `bus_cull_lo()` (`src/bus.c:309`), anything else NULL. Bounds
  checked against the region size.
- `rd32le` (`src/real3d.c:138`) for culling RAM, polygon RAM, FIFO;
  `rd32be` (`src/real3d.c:144`) for VROM only.
- `f32_of` (`src/real3d.c:173`) bit-casts a word to float; `sx24`
  (`src/real3d.c:181`) sign-extends the top 24 bits of a word.

## 4. Matrix stack

`mat_t` is a 3x4 float, `m[r][3]` the translation (`src/real3d.c:188`).

- `mat_load(base, off, out)` (`src/real3d.c:192`): 12 floats at culling
  address `base + off*12`; words 0..2 translation, 3..11 the 3x3 by rows.
- `mat_mul(a, b)` (`src/real3d.c:207`): `a * b` (b applied first).
- `mat_point` (`src/real3d.c:221`) transforms a point; `mat_dir`
  (`src/real3d.c:228`) a direction (no translation, no renormalise).

There is no explicit stack: the current matrix is passed by pointer down the
recursion of `descend()` / `follow()` / `draw_model()`, so C call frames are
the stack. Nothing is inverted or normalised; normals are transformed with
the same matrix as points (correct only for rotation + uniform scale).

## 5. Culling-node walk

`descend(addr, m, depth)` (`src/real3d.c:891`) handles one node and then its
sibling chain in a loop (at most 4096, `src/real3d.c:896`), returning at
`depth >= MAX_DEPTH` (`src/real3d.c:895`). Per node, `cull_at(addr, 10)`:

    word  as read                                                    line
    [0]   bits 8|9 both set: this node and the rest of the chain     907
          are hidden (return)
          bit 2: load colour-table base g_ctab from bits of           909-912
          [3]>>19, [7]>>28 (<<13), [8]>>25 (<<17), 20 bits
          bits 0..2 == 6: colour-table node, no siblings              913
          bit 3: child is a 4-entry LOD table                         974
          bit 4: node carries a translation, not a matrix             939
    [2]   bit 15: texture offset for the subtree: x = 32*bits 7..12,  915-922
          y = 32*bits 0..4, page = bit 14
    [3]   bits 0..11 matrix index; bit 29: LOD entry is a node        924, 986
    [4-6] translation x/y/z, floats                                   925-927
    [7]   child pointer, low 27 bits                                  973
    [8]   sibling pointer, low 25 bits; bit 24 = none                 908, 998

Transform (`src/real3d.c:939-971`): bit 4 set -> `cur = m * T(tx,ty,tz)`;
else if the matrix index is non-zero and loads -> `cur = m * M[mo]`; else
`cur = m`. Never both.

Children go through `follow(p, m, depth)` (`src/real3d.c:853`), on the
pointer's top byte `(p >> 24) & 5`:

- `1` -> `draw_model(a, m)` (model).
- `4` -> pointer list, up to 256 entries via `cull_at(a + k, 1)`; entry bit 24
  ends the list before it, bit 25 after it; each entry -> `descend(...,
  depth + 1)` (`src/real3d.c:864-877`).
- `0` -> `descend(a, m, depth + 1)` (another node).
- anything else (5) is ignored.

Address 0 and the dummy `0x800800` are skipped (`src/real3d.c:857`).

The texture offset and page are saved before each node and restored after its
subtree (`src/real3d.c:900`, `995`). `g_ctab` is not restored: it leaks to
siblings and to everything walked later. Siblings share the parent's
transform, since `cur` is rebuilt from `*m` each iteration
(`src/real3d.c:904`). A sibling equal to the node or to `0x800800` ends the
chain (`src/real3d.c:999`).

No culling is done: node bounding data is not read, and nothing is rejected
before the rasteriser.

## 6. LOD selection

`src/real3d.c:974-990`. When node word 0 bit 3 is set, the child pointer is
a 4-word table at `cull_at(child, 4)`. The loop takes the first entry with
bit 24 set, or entry 3 if none has it -- no distance test. If node word 3
bit 29 is set the chosen entry is `descend()`ed, otherwise it is drawn with
`draw_model()`.

## 7. Model and polygon decode

`draw_model(addr, m)` (`src/real3d.c:604`). Address bit 23 selects VROM
(`bus_vrom()`, big-endian, word index `addr & 0x7FFFFF`) else polygon RAM
(`bus_poly()`, little-endian) (`src/real3d.c:614-622`). `M3_MODEL_DUMP`
prints header statistics (`src/real3d.c:630-686`). At most 20000 polygons per
model (`src/real3d.c:688`).

Polygon = 7 header words (`ph[0..6]`, `src/real3d.c:697`), then 4 words per
*new* vertex.

    ph[0] bits 0..3  reuse previous polygon's vertex k                 710-716
          bit 6      quad (4 vertices), else triangle                   708
          bits 8|9   both set: not drawn, vertices still kept           809
    ph[1] top 24     normal x (s24 / 2^22)                              703-704
          bit 1      colour is direct (clear: colour-table index)       751
          bit 2      last polygon of the model                          830
          bit 4      double-sided                                       803
          bit 6      UVs in whole texels (else 1/8 texel)               741
    ph[2] top 24     normal y;  bits 0..1 UV mirror (bit0 v, bit1 u)     778
    ph[3] top 24     normal z;  bits 3..5 width exp, 0..2 height exp     769-773
    ph[4] bits 31..8 colour R,G,B (or bits 8..19 colour-table index)    747-756
          bit 6      texture page                                       790
          bits 0..4  texture x (high 5 bits of a 6-bit field)           787
    ph[5] bit 7      texture x low bit;  bits 0..4 texture y            787-789
    ph[6] bit 31     contour (alpha test)                               776
          bit 23     opaque; else bits 18..22 opacity in 32nds          800-802
          bit 16     luminous (no lighting)                             795
          bit 10     texture enable                                     782
          bits 7..9  texel format (0..7)                                775
          bits 0..2  non-zero: texel alpha cuts out                     777

Validation: the header normal must have length^2 in [0.98, 1.02] or the
model ends (`src/real3d.c:705-706`) -- the guard against a pointer that is
not a model.

Vertex (`src/real3d.c:721-745`), 4 words:

- words 0..2: `sx24(w) / g_vertex_scale` is x, y, z (`src/real3d.c:725-727`);
  the low byte of each is the signed per-vertex normal component, **not**
  normalised (lengths 0..127) (`src/real3d.c:733-735`).
- word 3: u in bits 31..16, v in bits 15..0, unsigned, times 1/8 (or 1 when
  ph[1] bit 6) -> texel units (`src/real3d.c:740-743`).

Reuse: up to 4 slots from the previous polygon's vertices, in order of the
set bits (`src/real3d.c:710-716`); `nnew = nv - slot`. All slots are saved
for the next polygon whether or not this one is drawn (`src/real3d.c:822-827`).

Emission (`src/real3d.c:809-820`): if at least 3 slots and not discarded,
vertices go through `mat_point`, per-vertex normals and the face normal
through `mat_dir`, and the polygon is fanned from slot 0 into `emit()`.

## 8. Textures

### Sheet

One 2048x2048 16-bit sheet `g_sheet` (`src/real3d.c:269`) plus a one-byte
"written" flag per texel `g_sheet_set` (`src/real3d.c:270`). Viewed as two
2048x1024 pages stacked: page 1 is rows 1024..2047 (`src/real3d.c:320`,
`529`). Saved and restored with save states by `real3d_state()`
(`src/real3d.c:1813`).

### Upload decode

`upload_texture(header, src, avail)` (`src/real3d.c:278`):

    header bits   meaning                                        line
    0..5          x / 32                                         292
    7..11         y / 32                                         293
    14..16        width  = 32 << n                               295
    17..19        height = 32 << n                               296
    20            page                                           294
    21, 22        8-bit upload: write low / high byte            301
    23            16-bit texels (else 8-bit)                     297
    24..31        type: 0 texture+mips, 1 texture, 2 mips only   318

Rejected: no sheet, w or h > 1024 (`src/real3d.c:311`), type > 1
(`src/real3d.c:318`), payload shorter than `w*h*(2 or 1)`
(`src/real3d.c:319`), rectangle past the sheet (`src/real3d.c:321`). Only the
base level is stored; mip data is never written.

Texels arrive in 8x8 tiles, `DETILE` (`src/real3d.c:258`) mapping position in
tile to source index. 16-bit: one tile is 64 words (`src/real3d.c:330-335`).
8-bit: Supermodel's map, `(yy^1, xx^1)` through `DETILE`, two texels per
16-bit word, even column in the high byte; the byte is written into the
lane(s) selected by bits 21/22, the other lane kept (`src/real3d.c:336-353`).
Every written texel sets `g_sheet_set` (`src/real3d.c:355`). Byte order:
little-endian from the FIFO, big-endian when `g_src_be` (VROM).

### Sources

- **FIFO** (`0x94000000`, `bus_texfifo()`, `src/bus.c:294`). Bytes are
  appended in arrival order (`src/bus.c:1049-1057`) to a 4 MiB buffer
  (`TEXFIFO_CAP`, `src/bus.c:188`). `flush_texture_fifo()`
  (`src/real3d.c:405`) parses entries `{len, header, texels}`, stride
  `(2 + len/2)/4` words, and calls `upload_texture()`. It runs once per
  frame from `real3d_render()`, not on the doorbell the comment at
  `src/real3d.c:245` mentions. It returns early when the FIFO has not grown,
  but when it has, it re-parses **from offset 0** (`src/real3d.c:410`):
  every entry ever received is re-uploaded each time anything new arrives.
  `bus_texfifo_reset()` (`src/bus.c:301`) has no caller, so the buffer only
  grows; bytes past 4 MiB are dropped (`src/bus.c:1052`).
- **VROM texture port** (`0x90000000..0x9000000B`). Writes land in
  `g_vromtex[]`; the write to register 2 calls `real3d_vrom_texture(addr,
  header, ctrl)` (`src/bus.c:927-936`, `src/real3d.c:380`). Address and
  header are byte-swapped, the address is a 32-bit word index into VROM
  (mod the VROM size), and `upload_texture()` runs with `g_src_be = 1`.
  This is synchronous, at the moment of the guest write; if `g_sheet` has
  not been allocated yet (before the first `real3d_render()` with a
  framebuffer, or ever, on Dreamcast) the upload is silently dropped
  (`src/real3d.c:311`).

### Lookup per polygon

The polygon's descriptor (`texdesc_t`, `src/real3d.c:560`) is filled in
`draw_model()` (`src/real3d.c:768-805`): size from `ph[3]` (exponent 6 or 7
reads as 32), origin from `ph[4]`/`ph[5]`, page from `ph[4]` bit 6 XOR the
inherited page, plus the inherited node offset; `x &= 2047`, `y &= 1023`.

### Texel formats

`texel_argb(fmt, v)` (`src/real3d.c:448`) -> 0xAARRGGBB, after Supermodel's
ExtractColour. Twelve formats; 8..11 are reached only by remapping 1..4 when
the contour bit is set (`src/real3d.c:781`). Full table in
`MATERIAL_MAPPING.md`.

### Sampling

`sample(tr, u, v)` (`src/real3d.c:516`): bilinear on texel centres
(`u - 0.5`), weights in 8.8 fixed point. Each of the 4 taps is wrapped by
`wrap_axis()` (`src/real3d.c:477`) -- repeat by mask, mirrored repeat when
the axis's mirror bit is set -- then read with `sheet_word()`
(`src/real3d.c:432`), y wrapped within the 1024-row page. If any tap was never
written the sample returns -1. If the four are not all opaque, each tap
less opaque than the most opaque one takes the most opaque one's RGB before
filtering (`src/real3d.c:540-547`), so transparent texels do not darken a
cut-out's edge.

## 9. Material attributes

Decoded in `draw_model()` into `texdesc_t` and copied into `tri_t` by
`emit()` (`src/real3d.c:562`). See `MATERIAL_MAPPING.md` for every bit and
its effect.

## 10. The triangle queue

`tri_t` (`src/real3d.c:495-512`), 128 bytes on both x86-64 and SH-4:

    v[3][3]      view-space positions          36 B
    n[3]         face normal (view space)      12 B
    vn[3][3]     per-vertex normals             36 B
    uv[3][2]     texel coordinates              24 B
    tx,ty,tw,th  texture origin / size (u16)    8 B
    page, textured, luminous, two_sided, alpha, fmt,
    alpha_test, tex_alpha, mirror, r, g, b (u8)  12 B

`g_tris` (`src/real3d.c:514`) holds up to `MAX_TRIS` per **viewport**
(`g_ntris` is reset per viewport, `src/real3d.c:1684`). `emit()` silently
drops triangles past the cap (`src/real3d.c:569`). The total for the frame
goes to `g_tri_count` / `real3d_last_tri_count()` (`src/real3d.c:132`,
`1763`).

## 11. Rasterisation

`raster(fb, fw, fh, sx, sy, L, intensity, by0, by1)` (`src/real3d.c:1061`)
draws the whole queue clipped to rows `by0..by1`.

Order (`src/real3d.c:1109-1122`): two passes over the queue -- pass 1 the
triangles with `alpha == 255`, pass 2 those with `alpha < 255`, each in
submission order. No sorting.

Per triangle:

1. **Backface cull** (`src/real3d.c:1140-1146`): unless `two_sided`, drop if
   `dot(n, centroid) > 0` (eye at origin, view space). Uses the header
   normal, not winding.
2. **Near clip**: `clip_near(src, out)` (`src/real3d.c:1012`),
   Sutherland-Hodgman against `z >= ZNEAR` carrying x,y,z,u,v; result fanned
   (`src/real3d.c:1153-1163`). No other plane is clipped; x/y are handled by
   the bounding box.
3. **Project** (`src/real3d.c:1165-1177`), in double: `x = ax*X/Z + bx`,
   `y = ay*Y/Z + by`; keeps `z` and `u/z, v/z, 1/z` for perspective-correct
   texturing.
4. **Degenerate** if `|area| < 1e-9` (`src/real3d.c:1179-1181`).
5. **Flat colour** (`src/real3d.c:1189-1199`): `lit = clamp(n.L, 0, 1)`;
   `sh = intensity` if luminous else `(AMBIENT + (1-AMBIENT)*lit) * intensity`;
   colour * sh, clamped to 255.
6. **Bounding box** clipped to the framebuffer, `g_proj` rectangle and the
   band (`src/real3d.c:1201-1224`). `M3_TRI_MAX` drops oversized ones
   (`src/real3d.c:1231-1247`).
7. **Pixels** (`src/real3d.c:1249-1336`), barycentric per pixel:
   - depth `z` = barycentric mix of **view-space z** (linear in screen
     space, not perspective-correct); test `z < g_zbuf[o]` (strict)
     (`src/real3d.c:1260-1262`);
   - texture: perspective-correct u, v; `sample()`; if any tap missing,
     colour stays the flat lit colour (and is then lit a second time in the
     next step); if `fmt == 0 || alpha_test || tex_alpha` and texel alpha <
     128 the pixel is discarded (`src/real3d.c:1276-1277`); otherwise texel
     RGB is multiplied by the polygon colour (`src/real3d.c:1280-1283`) --
     texel alpha is otherwise discarded;
   - lighting, **per pixel** (`src/real3d.c:1292-1320`): the vertex normals
     are interpolated, normalised and dotted with L; same ambient formula;
     multiplied into the colour. Textured channels are not clamped after
     this multiply (`src/real3d.c:1313-1316`), so `intensity > 1` can carry
     into the next channel. Skipped when the interpolated normal is near
     zero (then the step-5 flat value stands for untextured pixels);
   - write: `alpha < 255` blends `c*a + dst*(255-a)` and does **not** write
     depth; opaque writes colour and depth (`src/real3d.c:1321-1330`).

So shading is per-pixel normal interpolation (Phong-style), not per-vertex
Gouraud, despite the comment's name for it.

### Banding / threads

`raster_bands()` (`src/real3d.c:1448`) splits the viewport's rows
`g_proj.y0..y1` into `g_pool.bands` horizontal bands (`band_rows()`,
`src/real3d.c:1380`). `pool_start()` (`src/real3d.c:1414`) sets the band count to
`ncpu - 1` when there are more than 2 CPUs, else 1 (`M3_RENDER_THREADS`
overrides, capped at `MAX_BANDS`), and starts one detached worker per band
after the first (`band_worker()`, `src/real3d.c:1388/1390`); the caller
draws band 0. Every band walks every triangle, so output is
byte-identical to one thread. With `M3_REAL3D_STATS` or `M3_PICK` it runs
single-banded (`src/real3d.c:1453-1456`). Threads are pthreads / Win32 only.

## 12. Not implemented

Verified absent from `src/real3d.c`:

- **Mipmaps**: mip-only uploads are rejected, mixed ones store only the base
  (`src/real3d.c:311-319`); `sample()` always reads level 0.
- **Fog**: no viewport fog word is read; no fog term in `raster()`.
- **Specular**: no header bit decoded, no term in the lighting.
- **Spotlight**: no viewport spotlight word is read.
- **Smooth/flat shading bit**: not decoded; every polygon gets
  interpolated normals.
- **Culling-node bounding volumes / view-frustum culling**: not read.
- **Distance LOD**: LOD tables pick by flag only (section 6).
- **Texel-alpha blending**: texel alpha is only a < 128 discard; blending
  uses the polygon opacity alone.
- **Translucency sorting**: submission order within pass 2.
- **Camera matrix**: matrix 0 unused (section 2).
- **Layer priority with the tilemap**: decided outside, by
  `tilegen_render_pass(0/1)` (`src/model3recomp.c:159,163`).

## 13. Seams for a PVR backend

The point to cut is **between `emit()` and `raster()`**. Everything up to and
including the queue is platform-independent decode and stays; everything
from `raster_bands()` down is what a hardware backend replaces.

### Where to hook

1. `real3d_render()` (`src/real3d.c:1481`): stop requiring
   `platform_framebuffer()` (`src/real3d.c:1484-1489`). On Dreamcast it is
   NULL, so today *nothing* past that line runs -- not the walk, not
   `flush_texture_fifo()`, and not the sheet allocation that
   `real3d_vrom_texture()` needs. The Z-buffer setup (`1514-1523`,
   `1647-1650`) and the band pool do not exist on the PVR path.
2. Per viewport, replace `raster_bands(...)` (`src/real3d.c:1702`) with a
   submit of `g_tris[0 .. g_ntris)`. All per-viewport state it needs is
   already computed: `g_proj` (`ax, bx, ay, by`, scissor `x0..y1`), `light`,
   `intensity`, `g_ambient`.
3. `emit()` (`src/real3d.c:562`) is the alternative seam: a backend could
   transform/light/clip and write PVR vertices directly there, never
   materialising `g_tris`. Keep `g_tris` while the software rasteriser is the
   oracle; it is the record to diff against.
4. Textures: `upload_texture()` (`src/real3d.c:278`) is where an upload
   becomes known; the backend needs "rectangle (x, y, page, w, h) changed"
   from it to invalidate a texture cache. `sample()`/`sheet_word()`/
   `texel_argb()` are the reference conversion.
5. Viewport begin/end: the top of the per-viewport body
   (`src/real3d.c:1565`) and after `raster_bands` (`1702`) -- a PVR backend
   needs these to switch depth ranges (see below).

### Per-triangle data a backend consumes (`tri_t`)

| field | use on PVR |
|---|---|
| `v[3][3]` | CPU: near-clip (`clip_near`), project with `g_proj` to x, y; z -> 1/z for the PVR depth |
| `n[3]` | CPU backface test (as `src/real3d.c:1144`); vertex lighting fallback |
| `vn[3][3]` | CPU per-vertex lighting -> vertex ARGB (Gouraud) |
| `uv[3][2]` | divide by `tw`, `th` -> normalised u, v; clipped with the vertex |
| `tx, ty, page, tw, th, fmt` | texture cache key (+ `alpha_test` / `tex_alpha` for the alpha variant) |
| `textured` | textured vs untextured header |
| `luminous` | skip lighting: vertex colour = colour * intensity |
| `two_sided` | skip the backface test |
| `alpha` | list choice (TR if < 255) and vertex alpha |
| `alpha_test`, `tex_alpha` | list choice (PT) and texture alpha conversion |
| `mirror` | UV flip bits |
| `r, g, b` | base vertex colour |

Plus per viewport: `g_proj`, `light[3]`, `intensity`, `g_ambient`.

### What the software path costs today

From the `#define`s, for the 496x384 default (`src/model3recomp.c:28-29`):

| structure | size | where |
|---|---|---|
| `g_sheet` | 2048 x 2048 x 2 B = 8,388,608 B (8 MiB) | `src/real3d.c:1503` |
| `g_sheet_set` | 2048 x 2048 x 1 B = 4,194,304 B (4 MiB) | `src/real3d.c:1504` |
| `g_tris` | 40,000 x 128 B = 5,120,000 B (4.88 MiB) | `src/real3d.c:1502` |
| `g_zbuf` | 496 x 384 x 4 B = 761,856 B (744 KiB) | `src/real3d.c:1516` |
| framebuffer (platform) | 496 x 384 x 4 B = 761,856 B (744 KiB) | `platform_framebuffer()` |

Renderer-owned total: 19,226,624 B (18.3 MiB), more than the Dreamcast's 16 MB main
RAM before a byte of the guest. Alongside it, the bus regions the walk reads
(`src/bus.c:188,194-196`): culling low 4 MiB, culling high 1 MiB, polygon RAM
4 MiB, FIFO 4 MiB (lazily allocated on Dreamcast, `src/bus.c:205-209`).

On a PVR backend:

- `g_zbuf`, the framebuffer and the band pool go away.
- The full sheet cannot live in PVR VRAM (8 MiB total, also holding the
  frame and the tile/vertex buffers). It needs a cache of converted
  textures keyed by `(x, y, page, w, h, fmt, alpha variant)`, filled from a
  CPU-side source. Either keep `g_sheet` in main RAM (8 MiB) or keep only
  the uploads' source references (FIFO offset / VROM address) and convert
  from those on a miss. `g_sheet_set` (4 MiB) can become a coarse map: one
  bit per 32x32 block is 4096 bits (512 B), since every upload is aligned to
  32.
- `g_tris` can shrink to what one viewport really submits (the docs record
  ~4,500 in attract and ~21,000 in a round, `docs/technical/real3d.md`), or
  disappear if `emit()` writes PVR vertices directly.
- Separate depth buffers per viewport (`src/real3d.c:1647-1650`) do not exist
  on the PVR, which has one depth per render; giving each viewport its own
  disjoint band of the 1/z range, later viewports nearer, reproduces "each
  viewport over the last" for opaque geometry.
