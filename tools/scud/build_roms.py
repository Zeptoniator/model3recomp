"""Build Scud Race's ROM images from a romset you own.

  python tools/scud/build_roms.py <scud.zip|romdir> <outdir>

Thin wrapper over tools/rom_loader.py, which proves the program-ROM layout
against the exception vector table and picks the banked CROM and VROM chips
by their position on the ROM board. Writes, into <outdir>:

  scud_prog.bin     fixed program CROM, 2 MB at 0xFFE00000
  scud_bank.bin     banked CROM0..2, 48 MB
  scud_vrom.bin     VROM, 32 MB (Real3D models and textures)
  scud_snd.bin      68000 sound program (IC 21)
  scud_samples.bin  SCSP wave ROM (IC 22, 24)

None of them belong in git. The Digital Sound Board's Z80 program and MPEG
ROMs (IC 2, 57..60) carry the music; nothing in the runtime plays them yet,
so they are not written.
"""
import os
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
LOADER = os.path.join(HERE, "..", "rom_loader.py")
sys.path.insert(0, os.path.join(HERE, ".."))
from rom_loader import ic_position, load_files  # noqa: E402


def build_sound(files, out):
    """68000 program at IC 21, SCSP samples at IC 22 then 24 -- each a
    16-bit ROM stored low byte first, so a byte swap gives the 68000's
    big-endian view (MAME's ROM_LOAD16_WORD_SWAP)."""
    want = {"scud_snd.bin": [21], "scud_samples.bin": [22, 24]}
    for image, slots in want.items():
        chips = []
        for slot in slots:
            hits = [n for n, b in files.items()
                    if ic_position(n) == slot and len(b) >= 0x80000]
            if len(hits) != 1:
                print("%s: no single chip at IC %d (%s)"
                      % (image, slot, hits), file=sys.stderr)
                return 1
            chips.append(hits[0])
        data = bytearray(b"".join(files[c] for c in chips))
        data[0::2], data[1::2] = data[1::2], data[0::2]
        with open(os.path.join(out, image), "wb") as f:
            f.write(data)
        print("%-14s : %s  (0x%X bytes from %s)"
              % ("sound" if image == "scud_snd.bin" else "samples",
                 os.path.join(out, image), len(data), ", ".join(chips)))
    return 0


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    rc = subprocess.call([
        sys.executable, LOADER, src,
        os.path.join(out, "scud_prog.bin"),
        "--banked", os.path.join(out, "scud_bank.bin"),
        "--vrom", os.path.join(out, "scud_vrom.bin"),
    ])
    if rc:
        return rc
    return build_sound(load_files(src), out)


if __name__ == "__main__":
    sys.exit(main())
