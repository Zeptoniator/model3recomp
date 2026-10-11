#!/usr/bin/env bash
# Run one Dreamcast CDI (tools/dreamcast/make-cdi.sh) under Flycast and print its serial console.
#
#   tools/dreamcast/dc-run.sh <image.cdi> [seconds] [done-token]
#
# DC_SHOT=<token>:<file.png> takes a screenshot when <token> is printed.
# DC_SHOTS=<dir> photographs every "SCUD SHOT field N" line as <dir>/fN.png.
# DC_RUN_VISIBLE=1 opens the window on the current $DISPLAY instead of a
# private Xvfb, to watch a run (keep it uncovered: see below).
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

XVFB=""
if [ -n "${DC_RUN_VISIBLE:-}" ]; then
    DISP=${DISPLAY#:}
else
    DISP=${DC_RUN_DISPLAY:-110}
    while [ -e "/tmp/.X${DISP}-lock" ]; do DISP=$((DISP + 1)); done
    Xvfb ":$DISP" -screen 0 640x480x24 >/dev/null 2>&1 &
    XVFB=$!
    sleep 1
fi

LOG=$(mktemp)
FLY=""
cleanup() {
    [ -n "$FLY" ] && kill -TERM "$FLY" 2>/dev/null
    sleep 0.5
    [ -n "$FLY" ] && kill -KILL "$FLY" 2>/dev/null
    [ -n "$XVFB" ] && kill -TERM "$XVFB" 2>/dev/null
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

# DC_SHOT=<token>:<file.png> photographs the display when <token> first
# appears on the console -- while the picture is up, not after Flycast has
# exited (a capture then is of an empty desktop, and reads as "black").
SHOT_TOKEN=${DC_SHOT:-}; SHOT_TOKEN=${SHOT_TOKEN%%:*}
SHOT_FILE=${DC_SHOT:-}; SHOT_FILE=${SHOT_FILE#*:}
shot_done=""

rc=1
for _ in $(seq 1 $((TIMEOUT * 4))); do
    if [ -n "${DC_SHOT:-}" ] && [ -z "$shot_done" ] && grep -aq -- "$SHOT_TOKEN" "$LOG"; then
        sleep 0.3
        DISPLAY=":$DISP" import -window root "$SHOT_FILE" 2>/dev/null \
            && echo "[dc-run] captured $SHOT_FILE" >&2 \
            || echo "[dc-run] capture FAILED: $SHOT_FILE" >&2
        shot_done=1
    fi
    if [ -n "${DC_SHOTS:-}" ]; then
        for n in $(grep -ao 'SCUD SHOT field [0-9]*' "$LOG" | awk '{print $4}'); do
            [ -e "$DC_SHOTS/f$n.png" ] && continue
            DISPLAY=":$DISP" import -window root "$DC_SHOTS/f$n.png" 2>/dev/null \
                || echo "[dc-run] capture FAILED: field $n" >&2
        done
    fi
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
