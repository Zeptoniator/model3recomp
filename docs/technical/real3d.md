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

## Textures: the format, from a reference rather than from guessing

`M3_TEXTURES=1` turns texturing on; it is off by default because it is not
finished. What changed is that the format is no longer being guessed at. It
was read out of Supermodel, which is the only place it exists in public.

Blind search had it wrong in several places, and it is worth recording which,
because every one of them looked settled at the time.

**Texture memory is a flat 2048x2048 sheet**, two 2048x1024 pages stacked,
built by uploads. It is not addressed into VROM at all. Weeks of layout
search here -- linear at eight widths, 32- and 64-texel tiles both ways
round, two-level tilings, strides from 8 to 256, Morton order -- were
searching for a mapping that does not exist.

**The tiling is 8x8, and it is in the arriving data, not in the sheet.**
Texels come in 8x8 tiles with the pairs inside each transposed. `DETILE` in
`src/real3d.c` is that map. Every attempt here had assumed 32x32 tiles,
because the polygon header's coordinates are in units of 32.

**The FIFO carries the textures after all.** An entry is a length word, a
header word, then texels, and it occupies `(2 + length/2)/4` words, which is
how the next entry is found. The stream had been dismissed as "commands and
runs of zeros" on the strength of its first block, which really is a gamma
ramp.

**Three polygon-header fields were wrong:**

    field        was                          is
    enabled      word 6 bit 26                word 6 bit 10
    width        word 6 bits 3..5             word 3 bits 3..5
    height       word 6 bits 0..2             word 3 bits 0..2
    page         word 5 bit 6                 word 4 bit 6

Width and height live in the low bits of word 3, whose top 24 bits are the
polygon normal -- which is why they were never found by looking at word 6.
The enable bit was right by luck: bit 26 and bit 10 agree on every sample
that was checked.

What still does not work: in the window this runtime reaches, every FIFO
entry carries the same header, 0x80000000, which is one 32x32 texture at the
origin. 312 entries of it per frame. The parse is not in doubt -- the entry
sizes are self-consistent and the walk never desynchronises -- so either the
game uploads its real textures at a point this build does not reach, or the
capture of the stream is losing something. Until that is resolved, most
surfaces sample a sheet position that was never written, and the renderer
falls back to flat shading for those rather than drawing the cleared sheet,
which is opaque black and turns every untextured surface into a silhouette.

## There is no environment in the scene, and that is upstream

The renderer draws what it is given, and what it is given is not a scene.
Field 2040, every model in it:

    matrices 400-420   the figure, 21 parts          ~3500 triangles
    matrix  421        one object in polygon RAM        764
    matrices 422-445   the same 8-triangle model,       192
                       instanced 24 times along a
                       curve running out to 36000 units
    matrices 393,394,  two 2-triangle and two            12
            446,447    4-triangle quads

That is the entire 4525. No terrain, no foliage, no ground -- a figure and
some scattered markers. Every shot sampled has the same shape: around 55
models and around 4500 triangles.

The traversal is not at fault. A loose scan of culling RAM for anything that
could be a node finds 62 addresses, and the only three the walk does not
reach through the node run are the viewport roots, which it reaches directly.
Culling RAM high is 3% populated -- 7782 non-zero words of 262144, nothing
past 0x352F. Culling RAM low is completely empty, 0 of 4194304 bytes, and no
DMA in an entire run targets it.

So the game is describing a small scene, and the place to look is what the
game is executing, not how the scene is walked. Worth stating plainly because
the renderer has been the suspect for everything up to now and here it is
not: it is drawing all of what arrives.

## Five parts of the model never draw

Prompted by someone looking at a screenshot and saying it was half a model
in a clump rather than a scene. It is worth writing down how right that was.

The attract scene is flat: three root nodes, fifty-five leaves, no nesting,
no node with two live children. Each leaf is one matrix and one model. Eleven
of those leaves are parts of a single articulated object -- matrices 400 to
414, translations spread over about two hundred units, rotation rows all
unit length. That is an assembled figure, not a pile.

Five consecutive parts of it never render, every frame, in every scene
dumped:

    matrix 395   -> 0100003C   polygon RAM word 0x3C, which is empty.
                                Polygon RAM only ever has content from word
                                0x10000 up.
    matrix 396   -> 0FFF5488   top byte 0x0F
    matrix 397   -> 0FFF6028
    matrix 398   -> 0FFF65F8
    matrix 399   -> 0FFF6BC8

The four with top byte 0x0F have bits 24, 25, 26 and 27 all set, which is not
a combination the rest of the scene uses -- every model pointer that works
has 0x01. Read as models they resolve to nothing: masked to 23, 24, 22, 21
and 28 bits, in VROM and in polygon RAM, nothing parses.

They are clearly a related run. The gaps between their addresses are 0xBA0,
0x5D0, 0x5D0, and the gaps between their node[0x09] fields are 0xBA0, 0x5D0,
0x5D0 -- identical. So they are consecutive models in some buffer, and
node[0x09] is their offset within it. Whatever fills that buffer, this
runtime does not see it: the FIFO at 0x94000000 contains no polygon header
anywhere in it (zero unit-normal triples in 80390 words, read either way
round), and no DMA in a whole run targets polygon RAM at all.

So the figure is drawn with a run of its middle missing, which is exactly
what it looks like.

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
