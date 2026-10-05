"""Assemble a flat Model 3 program image from a MAME romset.

  python tools/rom_loader.py <romset.zip|romdir> <out.bin> [--verify]

The four program EPROMs interleave as 16-bit words into one image: EPROM n
supplies bytes 2n and 2n+1 of every 8-byte group, each 16-bit word
byte-swapped (MAME's ROM_LOAD64_WORD_SWAP). Lane order is the EPROM number
ascending.

Which four are the program ROMs is not guessed. Every candidate set is
interleaved and checked against the PowerPC exception vector table, which is
self-validating in one of two ways, depending on the title's boot ROM:

  - the vector at 0xFFF00000 + 0x100*n opens by loading its own number,
    `li r7, n` (The Lost World), or
  - every vector opens with `bl` to one common handler, which tells them
    apart by LR (Scud Race).

Only the correct interleave produces either, so a romset this tool accepts is
a romset it has actually verified.

No ROM is written into the repository -- the output path is yours, and the
repo's .gitignore excludes the usual spellings.
"""

import argparse
import itertools
import os
import struct
import sys
import zipfile

VEC_CHECK = (0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x900)


def load_files(src):
    """Return {name: bytes} from a zip or a directory."""
    if os.path.isdir(src):
        out = {}
        for n in os.listdir(src):
            p = os.path.join(src, n)
            if os.path.isfile(p):
                out[n] = open(p, "rb").read()
        return out
    with zipfile.ZipFile(src) as z:
        return {i.filename: z.read(i) for i in z.infolist()}


def interleave(bufs):
    """bufs[0..3] -> one image, 16-bit word interleaved and byte-swapped."""
    n = len(bufs[0])
    out = bytearray(n * 4)
    for lane, buf in enumerate(bufs):
        for i in range(0, n, 2):
            out[i * 4 + lane * 2] = buf[i + 1]
            out[i * 4 + lane * 2 + 1] = buf[i]
    return bytes(out)


def vector_score(img):
    """How many exception vectors carry one of the two self-identifying
    openings: `li r7, <their own number>`, or `bl` to a handler that every
    checked vector shares. The better of the two counts is the score.

    The image is 2 MB and sits at 0xFFE00000, so the vector page at
    0xFFF00000 is at offset 0x100000.
    """
    vbase = len(img) - 0x100000
    if vbase < 0:
        return 0
    li_hits = 0
    bl_targets = []
    for v in VEC_CHECK:
        off = vbase + v
        if off + 4 > len(img):
            continue
        w = struct.unpack_from(">I", img, off)[0]
        # li r7, imm  ==  addi r7, 0, imm  ==  0x38E0____
        if (w & 0xFFFF0000) == 0x38E00000 and (w & 0xFFFF) == (v >> 8):
            li_hits += 1
        # bl rel  ==  opcode 18, AA=0, LK=1
        if (w & 0xFC000003) == 0x48000001:
            rel = w & 0x03FFFFFC
            if rel & 0x02000000:
                rel -= 0x04000000
            bl_targets.append((0xFFF00000 + v + rel) & 0xFFFFFFFF)
    bl_hits = 0
    if bl_targets:
        common = max(set(bl_targets), key=bl_targets.count)
        bl_hits = bl_targets.count(common)
    return max(li_hits, bl_hits)


def code_score(img):
    """How many of the 256 words from the reset vector on decode as 603e
    instructions. The vector check only ever reads lanes 0-1 -- every vector
    is 8-byte aligned -- so two interleaves that differ in lanes 2-3 score
    the same there. This reads all four lanes."""
    import ppc_disasm
    off = len(img) - 0x100000 + 0x100
    n = 0
    for i in range(256):
        w = struct.unpack_from(">I", img, off + 4 * i)[0]
        if ppc_disasm.decode(w, 0xFFF00100 + 4 * i).mn:
            n += 1
    return n


def ic_position(name):
    """The board location a chip was dumped from, as MAME names it: the
    number after the last dot (`mpr-19672.26` -> 26, `epr-19607.20a` -> 20).
    None when the name carries none.

    This is the ROM board's own layout, not a title's: on every Step 1.x
    board seen so far the banked CROM sits at IC 1..16, the program EPROMs
    at 17..20, the sound program at 21, the SCSP samples at 22 and 24, the
    VROM at 26..41, and a Digital Sound Board's MPEG ROMs at 57..60. Sizes
    alone cannot tell them apart -- Scud Race's MPEG ROMs are the size of a
    VROM chip and sort ahead of them.
    """
    tail = name.rsplit(".", 1)
    if len(tail) != 2:
        return None
    digits = ""
    for ch in tail[1]:
        if not ch.isdigit():
            break
        digits += ch
    return int(digits) if digits else None


def by_position(files, size, lo, hi, exclude):
    """Chips of `size` at IC lo..hi, in board order; None if any chip of
    that size has no readable position (fall back to sorting by name)."""
    named = [(n, ic_position(n)) for n, b in files.items()
             if len(b) == size and n not in exclude]
    if not named or any(p is None for _, p in named):
        return None
    return [n for n, p in sorted(named, key=lambda t: t[1]) if lo <= p <= hi]


def interleave_n(bufs, lanes, word=2):
    """Interleave `lanes` chips, `word` bytes at a time, byte-swapped.

    The program and banked CROMs use four chips two bytes apart; VROM uses
    sixteen. Same shape, different width.
    """
    n = len(bufs[0])
    out = bytearray(n * lanes)
    for lane, buf in enumerate(bufs):
        for i in range(0, n, word):
            out[i * lanes + lane * word] = buf[i + 1]
            out[i * lanes + lane * word + 1] = buf[i]
    return bytes(out)


def swap_pairs(g):
    """(a, b, c, d, ...) -> (b, a, d, c, ...)"""
    out = list(g)
    for i in range(0, len(out) - 1, 2):
        out[i], out[i + 1] = out[i + 1], out[i]
    return out


def build_region(files, names, lanes, out_path, label, group_order=None):
    """Assemble `names` (a multiple of `lanes`) into one interleaved image.

    `group_order` reorders each group of `lanes` chips before interleaving.
    Chip numbers do not run in lane order on this board, and the image comes
    out the right size either way, so this is not something a size check
    catches.
    """
    if len(names) % lanes:
        # The SCSP sample ROMs are the same size as the VROM chips and sort
        # after them, so trim to a whole number of lanes rather than refusing.
        keep = len(names) // lanes * lanes
        if not keep:
            print("%s: %d chips, need a multiple of %d; skipping"
                  % (label, len(names), lanes), file=sys.stderr)
            return 0
        print("%s: using the first %d of %d chips (the rest are another "
              "region)" % (label, keep, len(names)), file=sys.stderr)
        names = names[:keep]
    parts = []
    for g in range(0, len(names), lanes):
        group = names[g:g + lanes]
        if group_order:
            group = group_order(group)
        parts.append(interleave_n([files[n] for n in group], lanes))
    img = b"".join(parts)
    with open(out_path, "wb") as f:
        f.write(img)
    print("%-14s : %s  (0x%X bytes from %d chips)"
          % (label, out_path, len(img), len(names)))
    return len(img)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", help="romset .zip or a directory of chip dumps")
    ap.add_argument("out")
    ap.add_argument("--size", type=lambda s: int(s, 0), default=0x80000,
                    help="program EPROM size (default 0x80000)")
    ap.add_argument("--banked", help="also write the banked CROM (CROM0..3)")
    ap.add_argument("--vrom", help="also write the VROM (texture/model data)")
    a = ap.parse_args()

    files = load_files(a.src)
    cands = sorted(n for n, b in files.items() if len(b) == a.size)
    if len(cands) < 4:
        print("found %d chips of size 0x%X; need at least 4"
              % (len(cands), a.size), file=sys.stderr)
        return 2

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

    # The board's own order first: lane 0 is IC 20, lane 3 is IC 17 (MAME's
    # ROM_LOAD64_WORD_SWAP offsets 0, 2, 4, 6). Only when that does not
    # validate is every permutation scored, and then the code check breaks
    # ties the vector check cannot see.
    best = None
    board = sorted((n for n in cands if ic_position(n) in (17, 18, 19, 20)),
                   key=ic_position, reverse=True)
    if len(board) == 4:
        img = interleave([files[n] for n in board])
        if vector_score(img) == len(VEC_CHECK):
            best = (len(VEC_CHECK), tuple(board), img)
    if best is None:
        for combo in itertools.combinations(cands, 4):
            for order in itertools.permutations(combo):
                img = interleave([files[n] for n in order])
                s = vector_score(img)
                if s == len(VEC_CHECK):
                    s = (s, code_score(img))
                else:
                    s = (s, 0)
                if best is None or s > best[0]:
                    best = (s, order, img)
        best = (best[0][0], best[1], best[2])

    score, order, img = best
    if score < len(VEC_CHECK):
        print("could not find a program-ROM interleave that validates "
              "(best %d/%d vectors matched)." % (score, len(VEC_CHECK)),
              file=sys.stderr)
        print("candidates were: %s" % ", ".join(cands), file=sys.stderr)
        return 1

    with open(a.out, "wb") as f:
        f.write(img)

    base = 0x100000000 - len(img)
    print("program EPROMs : %s" % ", ".join(order))
    print("image          : %s  (0x%X bytes)" % (a.out, len(img)))
    print("maps at        : 0x%08X" % base)
    print("reset vector   : 0x%08X" % (base + len(img) - 0x100000 + 0x100))
    print("verified       : %d/%d exception vectors" % (score, len(VEC_CHECK)))

    # The banked CROM holds the bulk of the game's data and the VROM its
    # models and textures. A game whose banked CROM reads as zero boots and
    # then has nothing to show.
    #
    # Which chips are which is not a matter of taste, and the obvious reading
    # is the wrong one. The bigger parts are the banked CROM and the smaller
    # ones the VROM -- the reverse of what "more data means more textures"
    # suggests -- and the order inside each interleaved group is not the order
    # the chip numbers come in:
    #
    #     banked CROM   4 chips per group, group reversed   (n+3, n+2, n+1, n)
    #     VROM         16 chips per group, adjacent swapped (n+1, n, n+3, n+2)
    #
    # Getting either wrong still produces an image of the right size, and a
    # game reading it finds plausible-looking nonsense rather than nothing --
    # which is worse, because it gets further before it goes wrong.
    #
    # The VROM is all sixteen chips in ONE group, not two groups of eight.
    # Both spellings produce a 32 MB image and both look like data; the
    # difference is that one of them contains models. Eight lanes puts the
    # chips that supply the high half of every word 16 MB away from the ones
    # that supply the low half, so every 32-bit word the Real3D reads is half
    # of one model and half of another. Nothing complains: the culling tree
    # still walks, the polygon headers still have seven words, and the
    # normals in them are simply not unit vectors any more.
    #
    # That last part is the check. A Model 3 polygon header carries its own
    # normal as three 24-bit fixed-point fields over 2^22, and a correct
    # image has tens of thousands of models whose every normal comes out
    # unit length. The eight-lane image has none at all.
    if a.banked:
        names = by_position(files, 0x400000, 1, 16, order)
        if names is None:
            names = sorted(n for n, b in files.items()
                           if len(b) == 0x400000 and n not in order)
        build_region(files, names, 4, a.banked, "banked CROM",
                     group_order=lambda g: list(reversed(g)))
    if a.vrom:
        names = by_position(files, 0x200000, 26, 41, order)
        if names is None:
            names = sorted(n for n, b in files.items()
                           if len(b) == 0x200000 and n not in order)
        build_region(files, names, 16, a.vrom, "VROM",
                     group_order=swap_pairs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
