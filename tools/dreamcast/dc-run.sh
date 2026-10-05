#!/usr/bin/env bash
# Run one Dreamcast CDI (tools/dreamcast/make-cdi.sh) under Flycast and print its serial console.
#
#   tools/dreamcast/dc-run.sh <image.cdi> [seconds] [done-token]
#
# Exits 0 when the done-token appears on the console, 1 on timeout, 2 when
# Flycast cannot be found. FLYCAST=<binary> picks the emulator.
#
# The machine this was written on runs several Flycast sessions at once, so
# this keeps to itself: a private config (serial console on stdout, no GDB
# wait), its own Xvfb display (a covered window stalls the emulated machine),
# and on exit it stops only the processes it started -- never a pkill.
set -uo pipefail

ELF=${1:?usage: dc-run.sh <image.cdi> [seconds] [done-token]}
TIMEOUT=${2:-60}
DONE=${3:-}

if [ -z "${FLYCAST:-}" ]; then
    for c in "$HOME/.local/share/flycast/squashfs-root/usr/bin/flycast" \
             "$(command -v flycast || true)"; do
        [ -n "$c" ] && [ -x "$c" ] && { FLYCAST=$c; break; }
    done
fi
[ -n "${FLYCAST:-}" ] && [ -x "$FLYCAST" ] || { echo "flycast not found" >&2; exit 2; }

HOME_DIR=$(mktemp -d)
mkdir -p "$HOME_DIR/flycast" "$HOME_DIR/data/flycast"
# The settings go on the command line. Flycast keeps them in emu.cfg's
# [config] section as Debug.SerialConsoleEnabled and so on; a [Debug]
# section is read as nothing, and the serial console stays off -- a run that
# boots fine and prints not one line.

DISP=${DC_RUN_DISPLAY:-110}
while [ -e "/tmp/.X${DISP}-lock" ]; do DISP=$((DISP + 1)); done
Xvfb ":$DISP" -screen 0 640x480x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1

LOG=$(mktemp)
FLY=""
cleanup() {
    [ -n "$FLY" ] && kill -TERM "$FLY" 2>/dev/null
    sleep 0.5
    [ -n "$FLY" ] && kill -KILL "$FLY" 2>/dev/null
    kill -TERM "$XVFB" 2>/dev/null
    rm -rf "$HOME_DIR" "$LOG"
}
trap cleanup EXIT

DISPLAY=":$DISP" XDG_CONFIG_HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/data" \
    "$FLYCAST" \
    -config config:Debug.SerialConsoleEnabled=yes \
    -config config:Debug.SerialPTY=no \
    -config config:Debug.GDBEnabled=no \
    -config config:RendererType=0 \
    -config config:LimitFPS=no \
    -config window:fullscreen=no \
    -config audio:backend=null \
    "$ELF" >"$LOG" 2>&1 &
FLY=$!

rc=1
for _ in $(seq 1 $((TIMEOUT * 4))); do
    if [ -n "$DONE" ] && grep -aq -- "$DONE" "$LOG"; then rc=0; break; fi
    kill -0 "$FLY" 2>/dev/null || break
    sleep 0.25
done
# DC_RAW=1 prints Flycast's own log as well as the guest's console.
if [ -n "${DC_RAW:-}" ]; then
    cat "$LOG"
else
    # Flycast's own lines start with a timestamp; the guest's do not.
    grep -av '^[0-9][0-9]:[0-9][0-9]:' "$LOG"
fi
exit $rc
