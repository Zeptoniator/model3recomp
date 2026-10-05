#!/usr/bin/env bash
# Wrap a KallistiOS ELF into a bootable CDI.
#
#   tools/dreamcast/make-cdi.sh <program.elf> <out.cdi> [extra-root-dir] [title]
#
# The usual KOS recipe: raw binary, scrambled as 1ST_READ.BIN, an IP.BIN from
# makeip, an ISO with the IP.BIN as its boot sectors (session 2 starts at LBA
# 11702), then cdi4dc. Flycast's HLE BIOS boots this; a bare ELF it loads and
# then prints nothing.
#
# extra-root-dir is grafted onto the disc root (ROM data packs, later). mkisofs
# is given -f so symbolic links are followed: without it an image of linked
# files comes out small and still boots, with nothing in it.
set -euo pipefail

ELF=${1:?usage: make-cdi.sh <program.elf> <out.cdi> [extra-root] [title]}
CDI=${2:?usage: make-cdi.sh <program.elf> <out.cdi> [extra-root] [title]}
EXTRA=${3:-}
TITLE=${4:-MODEL3RECOMP DC}
KOS=${KOS_BASE:?source KallistiOS environ.sh first}

for t in "$KOS/utils/scramble/scramble" "$KOS/utils/makeip/makeip"; do
    [ -x "$t" ] || { echo "missing $t (build KOS utils)" >&2; exit 1; }
done
command -v mkisofs >/dev/null || { echo "mkisofs not found" >&2; exit 1; }
command -v cdi4dc >/dev/null || { echo "cdi4dc not found" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

sh-elf-objcopy -O binary -R .stack "$ELF" "$WORK/raw.bin"
"$KOS/utils/scramble/scramble" "$WORK/raw.bin" "$WORK/1ST_READ.BIN"
cat > "$WORK/ip.txt" <<IP
Device Info   : CD-ROM1/1
Area Symbols  : JUE
Peripherals   : E000F10
Product No    : T0000
Version       : V1.000
Release Date  : $(date +%Y%m%d)
Boot Filename : 1ST_READ.BIN
SW Maker Name : MODEL3RECOMP
Game Title    : $TITLE
IP
"$KOS/utils/makeip/makeip" "$WORK/ip.txt" "$WORK/IP.BIN" >/dev/null

GRAFT=("1ST_READ.BIN=$WORK/1ST_READ.BIN")
[ -n "$EXTRA" ] && GRAFT+=("/=$EXTRA/")
mkisofs -quiet -C 0,11702 -G "$WORK/IP.BIN" -J -r -l -f -graft-points \
    -o "$WORK/out.iso" "${GRAFT[@]}"
cdi4dc "$WORK/out.iso" "$CDI" >/dev/null
echo "CDI: $CDI ($(stat -c%s "$CDI") bytes)"
