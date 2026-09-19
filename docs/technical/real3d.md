# The Real3D scene

How the game describes a frame of 3D, and how much of that description is
actually known rather than assumed.

There is no oracle for any of this. MAME does not render Model 3 3D at all,
so the trick that settled the tilemaps -- put our picture next to MAME's and
look -- is not available. A 3D pipeline decoded wrongly does not fail, it
draws: it produces confident, plausible, wrong geometry, and nothing about
the output says which. So every structure here is paired with a check that
could only come out right if the decode is right, and the ones that have no
such check are marked as guesses.

## Where it lives

    0x8C000000   culling RAM low    4 MB    empty in every scene dumped so far
    0x8E000000   culling RAM high   1 MB    the scene graph and the matrices
    0x98000000   polygon RAM        4 MB    models the game builds per frame
    VROM        32 MB                       models and textures, fixed

A culling address is a **word index, not a byte offset**, and the top byte is
flags. `0x800000..0x83FFFF` is culling RAM high, below `0x100000` is culling
RAM low. The first viewport links to `0x800030` and the second viewport node
begins exactly at word `0x30`, which is what fixes this.

Everything except VROM is read little-endian, like the rest of the graphics
side. VROM is the exception -- see below.

## Viewports

A list starting at culling address `0x800000`, each `0x20` words, linked
through word `0x01`. This game builds three, all full-screen, with priorities
3, 1 and 0.

    [0x01]  next viewport
    [0x02]  root culling node
    [0x0C]  sin of the horizontal half-angle
    [0x0E]  sin of the vertical half-angle
    [0x14]  (w & 0x3FFF) >> 2 = width, (w >> 16) >> 2 = height
    [0x16]  base of the matrix table
    [0x1A]  viewport x and y, same packing as [0x14]

**Check:** `[0x14]` decodes to 496x384, the exact display size. `[0x0C]` and
`[0x0E]` are 0.33175 and 0.258819 -- the sines of 19.38 and 15 degrees --
and the ratio of their tangents is 1.3125, which is the aspect of a 496x384
screen. Two independent quantities agreeing on the display geometry is not
something a wrong reading produces.

## Culling nodes

Ten words.

    [0x00]  flags
    [0x03]  matrix index in the low 12 bits
    [0x04]  x, y, z translation, floats
    [0x07]  first child pointer
    [0x08]  second child pointer

A child pointer's top byte says what it points at: bit 24 a model, bit 26 a
pointer list, otherwise another culling node.

**Check:** nodes appear in runs at stride `0x0A` where `[0x00]` rises by
`0x400` and the matrix index in `[0x03]` by exactly 1, node after node. A
wrong stride or a wrong field offset breaks that immediately.

## Pointer lists

An array of addresses, each with flags in the top byte:

    bit 24 and bit 25 together   skip -- not an entry, and not an end either
    bit 25 alone                 last entry: process it, then stop

The skip case is worth its own line because getting it wrong is silent and
expensive. Attract mode's lists *begin* with `0x03000000`, so treating bit 25
as an end wherever it appears stops every list at its first element and loses
every model in it. That produced an empty scene that looked exactly like a
scene the game had not built yet.

**Check:** every address a correctly-walked list yields is a valid culling
node, and the unused entries are all the same dummy value, `0x800800`.

## Matrices

Twelve floats at `matrix_base + index * 12`: the translation first, then a
3x3 by rows.

    [ m3 m4  m5  | m0 ]
    [ m6 m7  m8  | m1 ]
    [ m9 m10 m11 | m2 ]

**Check:** matrix 1 is `[0,0,0, 1,0,0, 0,1,0, 0,0,1]`, which is exactly
identity under this reading and nothing under any other.

Matrix 0 is the camera. That one is weaker: it is the only one of the table's
first four that puts any of the scene in front of the eye, and it is a plain
rotation, which is what a view matrix should be. It has not been checked
against a scene whose correct appearance is known.

## Models and polygons

A model is a run of polygons. Each polygon is a seven-word header followed by
four words for every vertex that is **not** reused from the previous polygon.

    [0]  bits 0..3  reuse vertex n from the previous polygon
         bit 6      four vertices rather than three
    [1]  normal x, and bit 2 marks the last polygon of the model
    [2]  normal y
    [3]  normal z
    [4]  colour, r/g/b in bits 31..8

The normals are 24-bit fixed point over 2^22, taken from the top 24 bits of
each word. Vertices are the same: the top 24 bits are the coordinate, the low
byte is a per-vertex normal, and word 3 of each vertex is u and v.

**Check, and it is the strongest one here:** the whole of polygon RAM tiles
into models under this rule with *zero slack* -- two models, back to back,
the second ending on the word after the last non-zero word -- and every one
of the 1260 header normals in that block comes out exactly unit length, worst
deviation 0.000000. That is what pinned the sign extension and the 2^22
scale; nothing else does both at once.

## VROM

VROM is the one region read **big-endian**, and it is built as a **16-lane**
interleave -- all sixteen chips in one group, two bytes each, 32 bytes per
group.

This was wrong for a long time and nothing complained. Built as two groups of
eight, the image is the right size, the culling tree still walks, the models
still have seven-word headers, and the whole thing is nonsense: the chips
supplying the high half of every word end up 16 MB from the ones supplying
the low half, so each word the Real3D reads is half of one model and half of
another.

The check is the normals again. A correct image has 31265 models whose every
header normal is unit length. The eight-lane image has **none at all** --
not few, none -- which is about as clear a signal as a ROM layout ever gives.

## The reference frame

Field 2080 of attract mode. The game hands the renderer 4525 triangles
across 55 models, and they resolve into rocks in the foreground with a cliff
face behind. That frame is what settled the vertex scale and the camera, and
it is what to render first after changing anything here -- everything before
it in attract mode is 2D over an empty or parked scene, which is exactly the
condition under which a broken renderer looks fine.

    M3_REAL3D_STATS=1 ./lostworld 2080

`M3_REAL3D_STATS` prints the triangle count and how many pixels reached the
framebuffer, on change. That is how field 2080 was found in the first place:
4525 triangles submitted and zero drawn, for twenty fields running.

## Two wrong answers, kept on purpose

The vertex scale is 1/256. It was committed twice before that, wrongly, and
both times with an argument:

- **1/1024**, from taking a flat 4:3 model for a backdrop that filled the
  screen and reading two matrices as independently agreeing on the size that
  implied. They were not agreeing on anything; they were being asked a
  leading question, with arithmetic on top to make it look derived.
- **1/65536**, from the observation that model extents crowd the top of the
  24-bit coordinate field. The observation is true and it is worthless: the
  raw integers fill the field whatever divisor is applied afterwards, so it
  says nothing whatever about the divisor. It looked like evidence.

The camera went the same way. Matrix 0 is a plain rotation sitting exactly
where a view transform belongs, and using it turned the only scene then
available from nothing into something -- which was taken as confirmation. On
a frame that actually has a scene in it, matrix 0 pushes everything off the
left of the screen and leaving it out gives the rocks. The node matrices
already carry geometry into view space.

Both failures have the same shape, and it is the shape to watch for here: a
3D pipeline never reports a wrong constant, it just draws. Only a frame with
a recognisable subject in it can say no.

## Why so much of attract mode is still black

Roughly a tenth of a sampled attract cycle shows substantial 3D: the rock
faces around fields 1500-1520 and 2020-2080, and a further sequence at
3220-3400. The rest is 2D over a scene that is genuinely nearly empty (the
warning and title screens submit a few hundred triangles, most of them a flat
backdrop) or over geometry that falls outside the frustum.

That last case is worth being precise about, because it looks like a bug and
may not be one. At field 1500 the scene's models sit a median 50 degrees off
the view axis, and rendering it at a 70-degree half-angle shows a coherent
landscape off to the side. At field 2080, with the identical viewport
frustum, the models sit a median 4 degrees off axis and the frame looks
right. So the transform is not obviously wrong -- 2080 is evidence that it
works -- and the scene at 1500 really does extend past the edges of a
21-degree view, which terrain does.

What has been ruled out: the traversal is complete (all 59 nodes that exist
in culling RAM are visited, and none of them is an LOD table); node
translations are all zero, so placement comes entirely from the matrix table;
and no single matrix serves as a per-frame camera -- the best candidate for
one scene scores 1358 triangles on screen and 1 on the next scene along.

## Textures: the pipeline works, the address mapping does not

`M3_TEXTURES=1` turns texturing on. It is off by default, and that is the
honest setting rather than a cautious one -- see the end of this section.

Working, and checked:

- **Where the data is.** VROM splits in half: models below 16 MB, textures
  above. Correlate each byte with the one two along and the model half scores
  near zero while the texture half scores +0.6, which is what 16-bit pixels
  look like; correlate with the *next* byte instead and the texture half goes
  negative, which is a pixel's two halves alternating.
- **Texel format.** 16-bit, 1-5-5-5, bit 15 transparent -- the same packing
  the tilemap palette uses. Big-endian, by a factor of five: read that way a
  block scores 5205 for smoothness and the other way 24279.
- **The polygon header.** Word 6 bit 26 is the texture enable (set on 1749 of
  1780 polygons in a model, clear on the rest). Bits 7..9 are the pixel
  format: 0 on 5614 of 6093 headers and 5, 6 or 7 on the others, and only 0
  is a plain 16-bit texel. Origin is in 32-texel units out of word 4's low
  five bits and word 5.
- **UVs.** Vertex word 3, u in the high half, v in the low, in eighths of a
  texel. Interpolated with perspective correction -- u/z, v/z and 1/z linearly
  in screen space, divided at the end -- and carried through the near clip,
  because a ground plane running to the horizon is exactly where affine
  interpolation falls apart.

Not working: **which VROM address a polygon's texture coordinates name.**

Tried, and measured by how smooth the result comes out against a median of
5520 for arbitrary offsets: linear at eight widths; 32- and 64-texel tiles,
row-major and column-major; two-level tilings with inner blocks of 1, 2, 4,
8, 16 and 32; tile-row strides from 8 to 256; Morton order with x and y both
ways; sheets of 2048x2048, 2048x1024 and 4096x1024 across two and four
pages; and undoing the lane-pair swap the ROM builder applies, on the theory
that a 32-bit model word can read correctly while the two 16-bit texels
inside it are transposed. The best scored 3407. A correct layout does not
win by that margin, it wins by the margin the model decode won by, which was
31265 models against none.

So the polygons fetch real texels from the wrong places. Switched on it
looks like texturing -- foliage appears on the rocks, the colour count in a
frame goes from about a hundred to seven thousand -- and it is not
texturing. That is the specific failure this file exists to avoid, so the
switch defaults to off and the pipeline stays in place for whoever finds the
mapping.

One thing that has been ruled out: the textures are not uploaded. Every
SCRIPTS DMA in a run was traced. 1.5 MB goes to culling RAM, 321 KB to the
FIFO at 0x94000000 -- all of it from work RAM, and its contents are commands
and runs of zeros rather than pixels -- 3.7 KB to the trigger, and 36 bytes
to the texture port. The rejected transfers are all boot-test noise with
garbage addresses. Nothing is being dropped; there is no upload to find.

## The scene animates, and then it stops

Worth knowing before reading too much into a screenshot. Sampled every field
through the attract sequence at 2040-2075:

    2040..2061   the picture changes every field, by 35 down to 17 grey
                 levels per pixel, decaying smoothly -- a camera in motion
    2062..2075   byte-identical, field after field, fourteen of them
    2092         cuts to 2D only

The Real3D trigger keeps firing at four writes a field the whole time, so the
game is still asking for frames; it has simply stopped changing what it asks
for. Something the demo advances its own state with is not advancing.

So what the renderer shows is a fragment of a demo -- about twenty fields of
real motion -- and then a freeze. It is not the attract sequence playing
through, and a still taken from the frozen part cannot tell you which.

## Things that look wrong and are not

- **Long thin slivers.** Some polygons rasterise as near-degenerate spikes
  across the screen. They are real: all 53 in one frame are thin in three
  dimensions as well as on screen, with aspect ratios from 85 to 512. They
  are terrain strips seen at a grazing angle, not a decode fault.

## What is still not settled

- **The light's intensity.** The direction comes from the viewport now --
  words 0x04..0x06 are a unit vector, (-0.72288, -0.16226, -0.67156), length
  1.0000 to five places and identical in every scene dumped, which is what
  identifies them. Word 0x07 sits with them and looks like an intensity, but
  it reads 0.93 in one attract shot and 0.06 in another that is plainly not
  six per cent as bright, so it is left alone.
- **Textures.** Not done. Polygons draw in their header colour, which is why
  the reference frame is grey.
- **Layer priority.** The tilemaps are all drawn over the 3D. Model 3 can put
  a tilemap layer behind it.
- **Culling.** The node bounding data is read but not used; everything
  submitted is transformed.
