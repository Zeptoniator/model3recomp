"""Recompile Scud Race from its ROM images.

  python tools/scud/lift.py <roms_dir> <out_dir> [--entries FILE] [--max N]

  1. tools/ppc_interp.py boots from the reset vector against the board's
     memory map and stops when the guest branches into RAM. Scud Race's boot
     copies the program ROM from 0xFFF10000 on into RAM 0x00000000 and
     enters at 0x00000100 -- 18.5 M instructions.
  2. The RAM snapshot is lifted at RAM addresses (prefix scudram).
  3. The ROM half -- the reset routine only -- is lifted separately
     (scudrom), because the recompiled program starts where the processor
     does.

<out_dir> receives the generated C. It is derived from your dump: never
commit it.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, ".."))
ROM_BASE = "0xFFE00000"
SCSI_BASE = "0xF9000000"       # MAME init_scud


def run(args):
    print("  " + " ".join(os.path.basename(x) if x.endswith(".py") else x
                          for x in args[1:]))
    return subprocess.call(args)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roms")
    ap.add_argument("out")
    ap.add_argument("--entries", help="extra entry addresses, one per line")
    ap.add_argument("--max", default="60000000",
                    help="interpreter instruction budget for the boot")
    a = ap.parse_args()

    prog = os.path.join(a.roms, "scud_prog.bin")
    bank = os.path.join(a.roms, "scud_bank.bin")
    snap = os.path.join(a.out, "scud_ram.bin")
    if not os.path.exists(prog):
        print("no %s -- run tools/scud/build_roms.py first" % prog,
              file=sys.stderr)
        return 2
    os.makedirs(a.out, exist_ok=True)
    # A smaller lift than the last one would leave its tail behind to be
    # globbed into the build twice.
    for f in os.listdir(a.out):
        if f.startswith(("scudram_", "scudrom_")):
            os.remove(os.path.join(a.out, f))

    print("boot: running to the hand-off into RAM")
    cmd = [sys.executable, os.path.join(TOOLS, "ppc_interp.py"), prog,
           "--base", ROM_BASE, "--scsi-base", SCSI_BASE, "--max", a.max,
           "--snapshot", snap]
    if os.path.exists(bank):
        cmd += ["--crom-bank", bank]
    if run(cmd) or not os.path.exists(snap):
        print("boot interpreter did not produce a snapshot", file=sys.stderr)
        return 1

    extra = ["--entries", a.entries] if a.entries else []
    # The program ROM holds the boot and, from 0xFFF10000 on, the copy of the
    # game the boot puts in RAM. That copy is lifted from RAM below; from ROM
    # only the reset routine runs (the runtime's function report shows no
    # other ROM function entered), so only it is lifted. 58 instructions
    # instead of 53 000, and 1.4 MB less SH-4 code. A ROM function that did
    # get called would be a reported func-table miss.
    print("lift: boot code (from ROM)")
    if run([sys.executable, os.path.join(TOOLS, "ppc_lifter.py"), prog, a.out,
            "scudrom", "--base", ROM_BASE, "--entry-only",
            "--entry", "0xFFF00100", "--only", "0xFFF00100", "--stats"]):
        return 1
    print("lift: game (from the RAM snapshot)")
    return run([sys.executable, os.path.join(TOOLS, "ppc_lifter.py"), snap,
                a.out, "scudram", "--base", "0x00000000", "--stats"] + extra)


if __name__ == "__main__":
    sys.exit(main())
