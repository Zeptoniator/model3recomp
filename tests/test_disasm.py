"""Decoder cases that have been wrong, each with the word that showed it.

  python tests/test_disasm.py      (also run by ctest)
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
import ppc_disasm as D  # noqa: E402

CASES = [
    # Opcode 63 with a 10-bit XO whose low five bits are fcmpu's 0. Scud
    # Race's atan2 (0x58EC) -- read as fcmpu, it lost its arguments' signs.
    (0xFF900080, "mcrfs"),     # mcrfs cr7, cr4
    (0xFF0C0080, "mcrfs"),     # mcrfs cr6, cr3
    (0xFC000040, "fcmpo"),
    (0xFC000000, "fcmpu"),
    # A-form arithmetic still takes its 5-bit XO.
    (0xFC41082A, "fadd"), (0xFC620072, "fmul"), (0xFC200834, "frsqrte"),
    (0xEC62017A, "fmadds"),
    # Other 10-bit forms.
    (0xFC200818, "frsp"), (0xFC80181E, "fctiwz"), (0xFC000050, "fneg"),
    (0xFC205090, "fmr"), (0xFC00048E, "mffs"),
    # The indexed float load with update the lifter used not to emit.
    (0x7C45346E, "lfsux"),
]


def main():
    bad = 0
    for word, want in CASES:
        got = D.decode(word, 0).mn
        if got != want:
            print("FAIL %08X: %s, expected %s" % (word, got, want))
            bad += 1
    if bad:
        return 1
    print("all decoder cases passed (%d)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
