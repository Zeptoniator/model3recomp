# model3recomp on the Sega Dreamcast

A port of the board runtime to KallistiOS, aimed at running Scud Race as a
statically recompiled program on the SH-4 — the PowerPC game lifted to C and
compiled for the Dreamcast, not emulated. Status: the runtime boots and
lifted PowerPC runs correctly on the SH-4; Scud Race itself does not run on
the console yet. See STATUS.md.

## Requirements

- KallistiOS 2.2.x with its toolchain (`sh-elf-gcc` 13), environment
  sourced: `source /opt/toolchains/dc/kos/environ.sh` (the path is your
  install's; nothing in the build hard-codes it).
- KOS's pthread addon (`addons/lib/dreamcast/libpthread.a`, built with KOS).
- CMake ≥ 3.20, Python 3.
- To make bootable images: `mkisofs` (genisoimage), `cdi4dc`, and KOS's
  `utils/scramble` and `utils/makeip` built.
- To run without hardware: Flycast, and `Xvfb` for the harness.

## Build

```
source /path/to/kos/environ.sh
cmake -S . -B build-dc \
  -DCMAKE_TOOLCHAIN_FILE=$KOS_BASE/utils/cmake/kallistios.toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-dc -j
```

KOS's own toolchain file sets `CMAKE_SYSTEM_NAME` to `dreamcast`, which
selects `src/platform/dreamcast/`, drops netplay and links pthreads. The
desktop build is unchanged (`cmake -S . -B build-host`).

Targets produced:

| ELF | What it shows |
|---|---|
| `dreamcast_boot.elf` | DC-01: KOS boots, the board initialises, 120 fields run; heap use printed |
| `model3recomp_guest_smoke.elf` | DC-02: a lifted PowerPC program runs and its registers and RAM are checked |
| `model3recomp_test_ppc.elf` | the PPC semantic checks, on the SH-4 |

## Run

```
tools/dreamcast/make-cdi.sh build-dc/dreamcast_boot.elf build-dc/boot.cdi
tools/dreamcast/dc-run.sh build-dc/boot.cdi 90 "DC-01 DONE"
```

`dc-run.sh` prints the serial console and exits 0 when the token appears.
It uses its own Flycast settings and Xvfb display and stops only the
processes it started. The first boot after a while can take longer than the
timeout (BIOS boot time varies); re-run once before suspecting the program.
On hardware, the same CDI boots from a burned disc or an SD loader, and the
console output goes to the serial port.

## ROMs

Nothing is downloaded or included. Build the images from your own romset:
see `roms/README.md` and `docs/scud/BRINGUP.md`.

## Debugging

- Everything important goes to the serial console (`printf`, `dbglog`).
- `DC_RAW=1 tools/dreamcast/dc-run.sh …` shows Flycast's own log too.
- Desktop first: Scud Race's bring-up happens on the desktop build
  (`scudrace`, `MODEL3RECOMP_LOOP_GUARD`, `MODEL3RECOMP_WATCH`), where the
  runtime's spin reports and function-entry counters are.

## Known limitations

- No renderer on the Dreamcast yet (no framebuffer; PVR back end not written).
- No sound on the Dreamcast; the sound board compiles but is not run.
- The board does not fit 16 MB as the desktop models it: MEMORY_BUDGET.md.
- Scud Race does not reach attract mode on the desktop yet: docs/scud/BRINGUP.md.
