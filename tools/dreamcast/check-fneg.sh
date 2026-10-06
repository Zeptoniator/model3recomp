#!/usr/bin/env bash
# Fail if lifted code in a Dreamcast ELF contains a single-precision fneg.
#
#   tools/dreamcast/check-fneg.sh <scudrace.elf> [sh-elf-objdump]
#
# GCC 13 for the SH-4 (-m4-single) can emit `fneg frN` while the FPSCR is in
# double mode, where it negates nothing on an odd register: a sign silently
# lost (DC-BUG-017, and the -0 of field 546). Lifted code negates through
# m3_fneg(), a bit flip, so a compiler-made fneg in it is the bug coming back
# by another route -- it came through fmac contraction the second time.
set -euo pipefail
ELF=${1:?usage: check-fneg.sh <elf> [objdump]}
OBJDUMP=${2:-sh-elf-objdump}
hits=$("$OBJDUMP" -d "$ELF" | awk '/>:$/{fn=$2} /\tfneg\t/ && fn ~ /<_scud(ram|rom)_/ {print fn, $NF}')
if [ -n "$hits" ]; then
    echo "check-fneg: single-precision fneg in lifted code (see DC-BUG-017):" >&2
    echo "$hits" >&2
    exit 1
fi
echo "check-fneg: no fneg in lifted code"
