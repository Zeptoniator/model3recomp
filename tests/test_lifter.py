"""Lifter cases that have been wrong, each shaped like the code that showed it.

  python tests/test_lifter.py      (also run by ctest)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
import ppc_lifter as L  # noqa: E402

BASE = 0x1000


def image(words):
    return b"".join(struct.pack(">I", w) for w in words)


def lr_target_through_float_work():
    # Scud Race's 0x1F014 (DC-BUG-016): r31 is built as the address of a
    # point in its own loop, f31 is then used for float work, and a local
    # routine reached by a plain branch leaves with mtlr r31 ; blr. The scan
    # took `fadds f31` for a write of r31, forgot the target, and the blr
    # became a return that unwound the game out of the reset vector.
    img = image([
        0x3FE00000,   # 1000 lis    r31, 0
        0x3BFF1010,   # 1004 addi   r31, r31, 0x1010
        0xEFF3F02A,   # 1008 fadds  f31, f19, f30
        0x4800000C,   # 100C b      0x1018
        0x60000000,   # 1010 nop          <- where the routine comes back
        0x4E800020,   # 1014 blr          (the real return)
        0x7FE803A6,   # 1018 mtlr   r31
        0x4E800020,   # 101C blr          (a jump to 0x1010)
    ])
    fn = L.build(img, BASE, BASE, set())
    return 0x1010 in fn.lr_targets


def lr_target_through_ori():
    # ori rA, rS: the destination is rA. lis r3 ; ori r4, r3, lo builds r4.
    img = image([
        0x3C600000,   # 1000 lis    r3, 0
        0x60641010,   # 1004 ori    r4, r3, 0x1010
        0x7C8803A6,   # 1008 mtlr   r4
        0x4E800020,   # 100C blr
        0x60000000,   # 1010 nop
        0x4E800020,   # 1014 blr
    ])
    fn = L.build(img, BASE, BASE, set())
    return 0x1010 in fn.lr_targets


def lr_target_past_an_epilogue():
    # The real shape of 0x1F014: r31 is built before the loop and reloaded
    # by the epilogue, which lies, in address order, between the build and
    # the local routine's mtlr r31 ; blr.
    img = image([
        0x3FE00000,   # 1000 lis    r31, 0
        0x3BFF1010,   # 1004 addi   r31, r31, 0x1010
        0x41820014,   # 1008 beq    0x101C
        0x48000004,   # 100C b      0x1010
        0x60000000,   # 1010 nop          <- where the routine comes back
        0x83E10000,   # 1014 lwz    r31, 0(r1)   (epilogue)
        0x4E800020,   # 1018 blr          (the real return)
        0x7FE803A6,   # 101C mtlr   r31
        0x4E800020,   # 1020 blr          (a jump to 0x1010)
    ])
    fn = L.build(img, BASE, BASE, set())
    return 0x1010 in fn.lr_targets


def no_lr_target_at_a_call_return():
    # A recursive return lands after its own bl; a goto there would carry
    # on in the inner C frame. The address-order rule must not claim it.
    img = image([
        0x3C600000,   # 1000 lis    r3, 0
        0x38631010,   # 1004 addi   r3, r3, 0x1010
        0x4182000C,   # 1008 beq    0x1014
        0x4BFFFFF5,   # 100C bl     0x1000   (returns to 0x1010)
        0x60000000,   # 1010 nop
        0x80610000,   # 1014 lwz    r3, 0(r1)
        0x7C6803A6,   # 1018 mtlr   r3
        0x4E800020,   # 101C blr
    ])
    fn = L.build(img, BASE, BASE, set())
    return 0x1010 not in fn.lr_targets


def no_lr_target_at_a_table_base():
    # Scud Race's 0x19EC: LR = 0x1A24 + 12*r8 ; blr into a table of stubs.
    # The base is built from immediates and read by an mtlr, but it is not
    # a point the function's branches join; making it a label broke the
    # computed jump (DC-BUG-016's first fix stopped the boot at field 62).
    img = image([
        0x3C000000,   # 1000 lis    r0, 0
        0x38001010,   # 1004 addi   r0, r0, 0x1010
        0x7C004214,   # 1008 add    r0, r0, r8
        0x7C0803A6,   # 100C mtlr   r0
        0x4E800020,   # 1010 blr    (computed jump)
        0x60000000,   # 1014 nop    <- table, not a label
    ])
    fn = L.build(img, BASE, BASE, set())
    return 0x1014 not in fn.lr_targets and 0x1010 not in fn.lr_targets


CASES = [no_lr_target_at_a_table_base, lr_target_through_float_work, lr_target_through_ori,
         lr_target_past_an_epilogue, no_lr_target_at_a_call_return]


def main():
    bad = [c.__name__ for c in CASES if not c()]
    for n in bad:
        print("FAIL %s" % n)
    if bad:
        return 1
    print("all lifter cases passed (%d)" % len(CASES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
