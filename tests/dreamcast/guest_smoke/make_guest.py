"""Write the guest_smoke PowerPC program: a few dozen hand-assembled 603e
instructions, so the lifter has something to lift that is not a game ROM.

  python make_guest.py <out.bin>

The program lives at 0x00010000 (above the exception vectors, which the
lifter seeds on its own) and leaves its answers in registers and RAM:

  r3  = 55          sum 10..1, a counted loop with a backward bne
  r7  = 0x1278      bytes 0 and 3 of a big-endian stw, read back with lbz
  r9  = 0x1234      rlwinm extract of the high half
  r11 = 18          3.0f + 3.0f, times 3.0f, through fctiwz / stfd
  r12 = 0x77        set by a bl'd subroutine
  RAM 0x2000..3     12 34 56 78 (the guest is big-endian; the host is not)

Each word is checked against tools/ppc_disasm.py before it is written, so a
mis-encoded instruction fails here rather than as a wrong answer later.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
import ppc_disasm as D  # noqa: E402

BASE = 0x00010000

PROG = [
    (0x38600000, "addi"),    # li    r3, 0
    (0x3880000A, "addi"),    # li    r4, 10
    (0x7C632214, "add"),     # loop: add r3, r3, r4
    (0x3884FFFF, "addi"),    # addi  r4, r4, -1
    (0x2C040000, "cmpi"),    # cmpwi r4, 0
    (0x4082FFF4, "bc"),      # bne   loop
    (0x3CA01234, "addis"),   # lis   r5, 0x1234
    (0x60A55678, "ori"),     # ori   r5, r5, 0x5678
    (0x38C02000, "addi"),    # li    r6, 0x2000
    (0x90A60000, "stw"),     # stw   r5, 0(r6)
    (0x88E60000, "lbz"),     # lbz   r7, 0(r6)
    (0x89060003, "lbz"),     # lbz   r8, 3(r6)
    (0x54E7402E, "rlwinm"),  # slwi  r7, r7, 8
    (0x7CE74378, "or"),      # or    r7, r7, r8
    (0x54A9843E, "rlwinm"),  # srwi  r9, r5, 16
    (0x3D404040, "addis"),   # lis   r10, 0x4040        3.0f
    (0x91460010, "stw"),     # stw   r10, 16(r6)
    (0xC0260010, "lfs"),     # lfs   f1, 16(r6)
    (0xFC41082A, "fadd"),    # fadd  f2, f1, f1         6.0
    (0xFC620072, "fmul"),    # fmul  f3, f2, f1         18.0
    (0xFC80181E, "fctiwz"),  # fctiwz f4, f3
    (0xD8860018, "stfd"),    # stfd  f4, 24(r6)
    (0x8166001C, "lwz"),     # lwz   r11, 28(r6)        low word: 18
    None,                    # bl    sub (filled in below)
    (0x4E800020, "bclr"),    # blr
    # sub:
    (0x39800077, "addi"),    # li    r12, 0x77
    (0x4E800020, "bclr"),    # blr
]


def main():
    out = sys.argv[1]
    bl_at = PROG.index(None)
    sub_at = bl_at + 2
    PROG[bl_at] = (0x48000001 | ((sub_at - bl_at) * 4), "b")
    words = []
    for k, (w, mn) in enumerate(PROG):
        got = D.decode(w, BASE + 4 * k).mn
        if got != mn:
            print("word %d: %08X decodes as %s, expected %s" % (k, w, got, mn),
                  file=sys.stderr)
            return 1
        words.append(w)
    with open(out, "wb") as f:
        f.write(b"".join(struct.pack(">I", w) for w in words))
    return 0


if __name__ == "__main__":
    sys.exit(main())
