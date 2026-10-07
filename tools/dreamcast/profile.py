#!/usr/bin/env python3
"""Sum a Dreamcast sampling profile by function.

    tools/dreamcast/profile.py <console.log> <scudrace.elf> [top]

The console holds `[prof] PC:count ...` lines from a SCUD_DC_PROFILE build
(16-byte buckets). Each bucket is charged to the function whose symbol
starts at or below it (sh-elf-nm), then the functions are grouped: lifted
guest code, the runtime, KOS/libc. A table of buckets is not a profile --
lifted code is thousands of small functions, and their sum is in the tail.
"""
import bisect, collections, re, subprocess, sys

log, elf = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40

syms = []
for line in subprocess.run(["sh-elf-nm", "-n", elf], capture_output=True,
                           text=True, check=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[1] in "tTwW":
        syms.append((int(parts[0], 16), parts[2]))
addrs = [a for a, _ in syms]

per_fn = collections.Counter()
total = 0
for line in open(log, "rb").read().decode("latin1").splitlines():
    if not line.startswith("[prof] ") or "samples" in line:
        continue
    for pc, n in re.findall(r"([0-9A-F]{8}):(\d+)", line):
        i = bisect.bisect_right(addrs, int(pc, 16)) - 1
        per_fn[syms[i][1] if i >= 0 else "?"] += int(n)
        total += int(n)

def group(fn):
    if fn.startswith(("_scudram_", "_scudrom_")):
        return "lifted guest code"
    if fn.startswith(("_m3_", "_bus_", "_func_table", "_model3recomp", "_real3d",
                      "_pvr_", "_tilegen", "_irq", "_scsi", "_dev_", "_direct",
                      "_region", "_rom_at", "_piece_at", "_scudpack")):
        return "runtime"
    return "KOS, libc, other"

groups = collections.Counter()
for fn, n in per_fn.items():
    groups[group(fn)] += n
print("%d samples" % total)
for g, n in groups.most_common():
    print("  %-20s %6d  %5.1f%%" % (g, n, 100.0 * n / total))
print()
for fn, n in per_fn.most_common(top):
    print("%6d  %5.1f%%  %s" % (n, 100.0 * n / total, fn))
