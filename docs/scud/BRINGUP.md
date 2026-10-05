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
| SCUD-PC-08 hardware init progresses | **in progress** | VBlank handler and the game's field routine run ~500 fields; see "Where it stops" |
| SCUD-PC-09 attract mode reached | not yet | |

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
4. **Where it stops now.** The byte-stream routine at `0x4170` (`lbzu`
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
