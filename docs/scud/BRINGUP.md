# Scud Race bring-up on the desktop (SCUD-PC milestones)

| Milestone | State | Evidence |
|---|---|---|
| SCUD-PC-01 ROM identified | done | `scuda.zip` = MAME `scud` by chip names and CRC comments |
| SCUD-PC-02 program ROM assembled | done | `tools/scud/build_roms.py`; byte-identical to MAME's lane order |
| SCUD-PC-03 reset vector verified | done | 7/7 vectors (`bl 0xFFF00000` idiom); a lanes-2–3 swap scores 40 vs 65 on the decode check |
| SCUD-PC-04 boot interpreter runs | done | `ppc_interp.py` reaches RAM `0x100` after 18 513 937 instructions |
| SCUD-PC-05 RAM image generated | done | `scud_ram.bin`; RAM `0x0..0xEFFFF` = program ROM `0x110000..` |
| SCUD-PC-06 lift generated | done | 2 011 functions, 114 517 instructions, 0.06 % untranslated |
| SCUD-PC-07 generated C compiles | done | `scudrace` links (host, GCC 15) |
| SCUD-PC-08 hardware init progresses | **reached** | the game draws its boot settings screen (run log 4) |
| SCUD-PC-09 attract mode reached | **reached (2D)** | INSERT COIN(S), CREDIT 0, © SEGA 1996; the 3D is submitted but not drawn |

## Run log

1. **Silent exit after two functions.** `0xFFF00100` and `0x00000100` were
   the only guest functions entered; black fields forever, no error.
   Cause: `rfi` lifted as `return;` (DC-BUG-002). Fixed in the lifter.
2. **Callback miss.** `no function lifted at 00018144 (called from guest
   function 0000CDFC, LR 00001D08)`, then a miss at `FFFFFFFF`. The pointer
   at `0x00100014` is written only with `0x00018144` (`M3_WATCH`, field 63),
   so the `FFFFFFFF` call is downstream of the first miss, not a bad pointer.
   `0x18144` opens with `addi r1,r1,-16`, not `stwu`, so the prologue scan
   does not see it. Added to `entries.txt`.
3. **LR-relative dispatch.** Misses at `0x41F8`, `0x42F0`, `0x4308`,
   `0x432C`: a hand-written routine at `0x4170` calls `bl 0x41C0`, which
   computes a target and leaves through `mtctr ; bctr`. Added to
   `entries.txt`.
4. **Fixed: the printer's non-local return** (DC-BUG-007) and **the computed
   `blr` jumps** (DC-BUG-009). With both, the 1500-field run draws Scud
   Race's own boot settings screen (LINK ID, CABINET TWIN, COUNTRY JAPAN,
   GAME MODE NORMAL (SPRINT), MOTOR POWER 80%, COIN/CREDIT #1) through the
   tilemap — the first picture of the game from recompiled code.
5. **JTAG.** "ERROR : (1A) VIDEO BOARD ABNORMAL STATUS, LR 0000F944": the
   game reads the Real3D ASICs' IDCODEs over JTAG (system registers 0x0C /
   0x10) and compares them with `0x?16C3057..0x?16C7057`. Added
   `src/jtag.c` (DC-BUG-010).
6. **Waiting on a DMA.** The main program sat in `0xAE9C`, waiting for
   the flag at `0x1005B0` that the SCSI interrupt handler (`0xAEF8`) clears.
   Scud starts every transfer with DCNTL = STD|SSM — single-step — and its
   handler wants SSI with DSP on the next instruction (DC-BUG-011).
   Found with `M3_WHERE` (where the main program was when a field fired)
   and `M3_WATCH=0x1005B0`.
7. **NETWORK BOARD NOT PRESENT.** With the DMA running, the game showed
   "CANCELLED / NETWORK BOARD NOT PRESENT" and restarted, for ever: a blank
   EEPROM defaults this Twin set to LINK ID MASTER (written by `0x32C8` at
   field 76 into `0x0010011E`). `scud_operator_settings()` sets SINGLE, as
   an operator would in the test menu.
8. **Region warning, title screen, attract.** Three more lifter cases on
   the way (DC-BUG-012): discovery to a fixed point, hand-made return
   addresses (`lis/addi ; mtlr` then a branch), float leaves that open with
   `addi r1,r1,-N`. `M3_UNWIND_TRACE` named the frames a stray return went
   past. Attract runs from field ~3500.
9. **The 3D.** 23 738 triangles a field are decoded and none is drawn; the
   viewport node's frustum words read as `inf` (`M3_VP_TRACE`). Next.
10. **Was: where it stopped.** The byte-stream routine at `0x4170` (`lbzu`
   in, `stwbrx` out through `r11`) runs off its buffers: writes past the
   tilegen VRAM (`0xF1120000..`), reads at `0x060B16F0`, `0x0BE97E50`,
   twenty million device accesses with no field advancing. The routine is
   probably working as written on inputs that are already wrong, so the
   next step is to find the first divergence upstream: run the same field
   count in `ppc_interp.py` with `--no-stop-in-ram --trace-io`, and the
   runtime with `M3_TRACE_IO`, and diff the two device logs — the method
   the README describes, which has found each of The Lost World's runtime
   bugs.

## Commands

```
python tools/scud/build_roms.py /path/to/scud.zip roms/
python tools/scud/lift.py roms/ lifted/ --entries src/games/scud/entries.txt
cmake -S . -B build-scud -DSCUD_LIFTED_DIR=$PWD/lifted -DMODEL3RECOMP_LOOP_GUARD=ON
cmake --build build-scud -j
SDL_VIDEODRIVER=dummy M3_DROPPED=1 build-scud/scudrace 600 --roms roms
```
