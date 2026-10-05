"""Lay Scud Race's CROM out on the disc in the order the game reads it.

  python tools/scud/pack.py <roms_dir> <order.txt> <out SCUD.DAT>

<order.txt> comes from a desktop run with M3_ROM_PAGED=1 and
M3_ROM_ORDER=<file>: one line per 32 KB page, `which page field`, in the
order the game first touched it. A Dreamcast reading 32 KB pages scattered
over 50 MB waits on the drive for every one (~260 ms each under Flycast);
laid out in first-use order, the next pages it will want are the next
sectors on the disc, and the reader fetches several at a time.

Format (little-endian, the SH-4's order):

  0x00  "SCUDPACK"
  0x08  u32 version (1)       u32 page size (32768)
  0x10  u32 page count        u32 program CROM bytes
  0x18  u32 banked CROM bytes u32 offset of the first page (2048-aligned)
  0x20  u32 hot pages (how many lead the file, from the order list)
  0x40  page count x u32: (which << 31) | page, in file order
  ...   the pages, PAGE bytes each, the last one of a ROM zero-padded

Every page of both ROMs is in the file, hot ones first; nothing the game
reads outside the recorded run is missing, it is only slower. The output is
derived from your ROMs: do not distribute it.
"""
import os
import struct
import sys

PAGE = 0x8000


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    roms, order_path, out = sys.argv[1:]
    data = [open(os.path.join(roms, n), "rb").read()
            for n in ("scud_prog.bin", "scud_bank.bin")]
    npages = [(len(d) + PAGE - 1) // PAGE for d in data]

    order, seen = [], set()
    for line in open(order_path):
        f = line.split()
        if len(f) < 2:
            continue
        key = (int(f[0]), int(f[1]))
        if key[0] in (0, 1) and key[1] < npages[key[0]] and key not in seen:
            seen.add(key)
            order.append(key)
    hot = len(order)
    for which in (0, 1):
        for p in range(npages[which]):
            if (which, p) not in seen:
                order.append((which, p))

    index = b"".join(struct.pack("<I", (w << 31) | p) for w, p in order)
    first = (0x40 + len(index) + 2047) // 2048 * 2048
    with open(out, "wb") as f:
        f.write(b"SCUDPACK")
        f.write(struct.pack("<7I", 1, PAGE, len(order), len(data[0]),
                            len(data[1]), first, hot))
        f.write(b"\0" * (0x40 - f.tell()))
        f.write(index)
        f.write(b"\0" * (first - f.tell()))
        for w, p in order:
            chunk = data[w][p * PAGE:(p + 1) * PAGE]
            f.write(chunk + b"\0" * (PAGE - len(chunk)))
    print("%s: %d pages, %d hot first, %u bytes"
          % (out, len(order), hot, os.path.getsize(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
