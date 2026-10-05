# Code size: Scud Race, lifted

Measured 2026-10-05 on the lift produced by `tools/scud/lift.py` from MAME
`scud` (`scuda.zip`), entries from `src/games/scud/entries.txt` (5 extra).

## Lifter output

| Half | Functions | PPC instructions | Untranslated | C files |
|---|---:|---:|---:|---:|
| `scudrom` (program ROM @ `0xFFE00000`) | 978 | 56 087 | 36 (0.064 %) | 5 |
| `scudram` (RAM snapshot @ `0x0`) | 1 033 + entries | 58 430 | 31 (0.053 %) | 6 |
| Total | | 114 517 | | 11 + 2 headers + 2 register files |

6.5 MB of C, 135 313 lines. The untranslated words are mostly data the walk
reached (`op0`, i.e. zero words after a non-returning branch) and five
`lfsux`, which the lifter does not emit yet.

For comparison, The Lost World lifts to 2 014 functions and 342 753
instructions (README). Scud Race's game is about a sixth of that.

## Compiled for the SH-4

`sh-elf-gcc 13.2.0`, `$KOS_CFLAGS -O2 -ffunction-sections`, 11 files in
parallel: 50 s wall, 3 min 42 s CPU.

| Half | text |
|---|---:|
| `scudrom_*.o` | 1 414 060 bytes |
| `scudram_*.o` | 1 474 068 bytes |
| **Total** | **2 888 128 bytes** |

About 25 bytes of SH-4 per PPC instruction. Largest objects:
`scudram_code_002.o` 462 KB, `scudrom_code_002.o` 415 KB. Largest single
functions: `scudram_00058190` 6.2 KB, `scudram_00057D80` 3.5 KB;
`scudram_register_all` is 16 KB of `func_table_register` calls.

## The ROM half is mostly a second copy of the game

The boot copies the program ROM from `0xFFF10000` up into RAM `0x0`, so the
ROM half lifts the same code twice. In a host run of 600 fields the only
`scudrom_` function entered is `0xFFF00100`. Lifting the ROM half from the
reset vector's call tree only (no prologue scan past the boot) should take
it from 1.41 MB to a few KB; to be done in the lifter as an option, and
confirmed by a run that still boots.

## Host build

`cmake --build build-scud` (GCC 15, `-O2 -w`): 1 min 07 s for the library and
the lifted game together.
