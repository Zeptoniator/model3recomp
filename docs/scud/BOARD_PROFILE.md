# Scud Race board profile

Sources are named on every line: **MAME** is `src/mame/sega/model3.cpp`
(fetched 2026-10-05), **run** is a run of this repository's tools on the
user's own `scuda.zip`, **code** is Scud Race's own PowerPC read in this
session. Anything else is marked *hypothesis*.

## Revisions

| MAME set | Title | Program EPROMs (IC 17–20) | Notes (MAME) |
|---|---|---|---|
| `scud` | Scud Race / Sega Super GT — Twin/DX | epr-19688..19691 | Export; ROM board 834-13042 |
| `scuddx`, `scuddxo` | Deluxe (Rev A / original) | epr-19607a / 19607 at IC 20 | Japan/USA/Export |
| `scudau` | Twin/DX (Australia) | epr-19731..19734 | "Australian Version" on the title |
| `scudplus`, `scudplusa` | Scud Race Plus | epr-20092a..20095a (+ IC 21 sound) | game ID 833-13260 |

The user's dumps: `scuda.zip` = MAME `scud` (CRCs match by name),
`scudj.zip` = a Deluxe set (epr-19607..19610), `scudp.zip` = Scud Race Plus
Rev A. **The port targets `scud`.**

## The board

| Item | Value | Source |
|---|---|---|
| Model 3 Step | 1.5 | MAME (`ROM_START( scud )` comment, `scud` machine = `model3_15`) |
| PowerPC 603e clock | 100 MHz | MAME `model3_15` → `add_cpu_100mhz` |
| Program ROM | 4 × 512 KB, 2 MB at `0xFFE00000`, reset `0xFFF00100` | MAME offsets; run: 7/7 vectors, identical to MAME's lane order |
| Fixed CROM below the program | CROM0's first 6 MB mirrored at `0xFF800000` | MAME `ROM_COPY("user1", 0x800000, 0x000000, 0x600000)` |
| Banked CROM | 3 groups of 4 × 4 MB (CROM0..2), 48 MB, IC 1–12 | MAME |
| VROM | 16 × 2 MB, 32 MB, IC 26–41 | MAME `ROM_LOAD_VROM`, `interleave_vroms()` |
| 53C810 SCSI (DMA engine) | `0xF9000000` | MAME `init_scud` |
| Network board | `0xC0000000` | MAME `init_scud` ("TODO"), and `M3COMM` in `model3_15` |
| Sound | 68000 program epr-19692 (IC 21), SCSP samples mpr-19670/19671 (IC 22/24) | MAME |
| Digital Sound Board | Z80 epr-19612 (IC 2) + MPEG mpr-19603..19606 (IC 57–60, 8 MB), fed from the main UART | MAME `scud()` machine: `DSBZ80`, `m_uart->txd_handler().append(dsbz80…)` |
| Drive board | epr-19338a (Z80) | in the romset; force feedback — DEFER |
| Security board | none | Step 1.x |
| Inputs | AN0 steering, AN1 accelerator, AN2 brake; IN2: view 1–4 (bits 0–3), shift 1–4 (bits 6, 7, 4, 5) | MAME `INPUT_PORTS_START( scud )` |

## Observed in Scud Race's code

| Item | Value | Source |
|---|---|---|
| Exception vectors | each is `bl 0xFFF00000`; the handler tells them apart by LR | code |
| Reset | programs the MPC105 at `0xF8FFF000`, copies ROM `0xFFF10000..` to RAM `0x0` | run (interpreter, 18.5 M instructions) |
| Game entry | `0x00001540`, through `mtspr SRR0 ; rfi` at RAM `0x0138` | code |
| Small-data base | `r13 = 0x00108000` | code at `0x1540` |
| VBlank handler | `0x00000500` → field routine `0x1CD4` | run (guest-function report) |
| Decrementer | reloaded from `[r13-0x7FD0]` each field (`0x1CD4`) | code |
| Optional field callback | pointer at `0x00100014`, set to `0x00018144` at field 63 | run (`M3_WATCH`) |

## Where the profile lives

- `src/games/scud/scud.h` / `scud.c`: `scud_board_config()` sets the Step
  and `scsi_base`. That is all the title-specific board data the runtime
  currently consumes.
- `src/games/scud/entries.txt`: entry points the static walk misses.
- *To add* as the runtime grows: the analog inputs, the DSB, and the CPU
  clock (the work-unit calibration in `lift.h` assumes 66 MHz).
