# Execution flow, ROM to frame

Every name below is a real symbol or file in this repository; every address
is Scud Race's (MAME `scud`), read from its own code in this session.

```
romset .zip
  │  tools/scud/build_roms.py → tools/rom_loader.py
  │    main(): interleave(), vector_score(), code_score(), ic_position()
  ▼
scud_prog.bin (2 MB @ 0xFFE00000)  scud_bank.bin (48 MB)  scud_vrom.bin (32 MB)
  │  tools/scud/lift.py → tools/ppc_interp.py
  │    Machine, run(), step()  — boots from 0xFFF00100, stops on entering RAM
  ▼
scud_ram.bin (8 MB snapshot; RAM 0x0..0xEFFFF = ROM 0xFFF10000..)
  │  tools/ppc_lifter.py: discover(), scan_reachable(), build(), emit_function()
  ▼
scudrom_code_*.c  scudram_code_*.c  *_funcs.h  *_register.c   (never committed)
  │  cc / sh-elf-gcc, linked with libmodel3recomp
  ▼
main()  (src/games/scud/main_host.c)
  scud_board_config()       step 1.5, scsi_base 0xF9000000
  model3recomp_init()       src/model3recomp.c
     bus_init() → bus_set_scsi_base() → func_table_init() → m3_irq_init()
     → ppc_reset() → sound_init() → platform_init() → netplay_init_from_env()
  scud_register_lifted()    scudrom_register_all(), scudram_register_all()
  model3recomp_run()        func_table_call(0xFFF00100)
  ▼
guest code (lifted C), using lift.h:
  MEM_R*/MEM_W*  → m3_ld*/m3_st*  → work RAM inline, else bus_read*/bus_write*
                                     → direct() for memories, dev_read()/dev_write()
                                       for devices (scsi_read/scsi_write, sound UART,
                                       I/O board, PCI config, Real3D status)
  CALL(a)        → func_table_call()   — also where the runtime gets control
  M3_LOOPED(a)   → m3_loop_tick()      — every 1024 backward branches
  ▼
irq_tick()  (src/irq.c)  from func_table_call(), dev_read(), m3_loop_tick()
  dec_tick() → dec_dispatch() → func_table_call(vector 0x900)   decrementer
  model3recomp_field_due()   — m3_work has advanced FIELD_US (17385) units
  irq_raise(VBLANK) → irq_dispatch() → func_table_call(vector 0x500)
  model3recomp_end_frame()
     tilegen_render_pass(0) → real3d_render() → tilegen_render_pass(1)
     → platform_present() → frame hook → sound_run_field() → m3_input_latch()
```

## Scud Race's boot, as observed

| Step | Where | What |
|---|---|---|
| reset | `0xFFF00100` (ROM) | `lis r10,0xF900 ; addi r10,r10,-0x1000` → MPC105 at `0xF8FFF000`; programs the bridge (20 writes) |
| copy | ROM | copies the program ROM from `0xFFF10000` into RAM `0x00000000` (verified chunk by chunk against `scud_prog.bin`); 18.5 M interpreter instructions |
| hand-off 1 | ROM → `0x00000100` | the RAM reset vector |
| hand-off 2 | `0x00000100` | sets HID0, writes `0xFF440018` byte-reversed to `0xF8FFF0A8`, then `mtspr SRR1,0x3002 ; mtspr SRR0,0x1540 ; rfi` — **the game starts at `0x00001540`** |
| small data | `0x00001540` | `lis r13,0x11 ; addi r13,r13,-0x8000` → `r13 = 0x00108000` |
| VBlank | `0x00000500` | interrupt handler; calls the field routine around `0x1CD4` |
| field routine | `0x1CD4..0x1D40` | reloads DEC from `[r13-0x7FD0]`, calls `0x1F78`, `0x1FD8`, `0x2068`, `0x1E54`, then an optional callback `[r13-0x7FEC]` (`0x00100014`) via `bnelrl`, then acks `0xF1180010` and waits on `0xF0100018` bit 1 |

### Where this differs from The Lost World

The Lost World hands over with `lis r0,0 ; mtlr r0 ; blr`; Scud Race with
`rfi`. The lifter translated every `rfi` as `return;`, so the first build ran
`0xFFF00100`, then `0x00000100`, then fell out of the guest and rendered
black fields forever with no error — the guest-function report showed exactly
two functions entered. See docs/dreamcast/BUGS.md, DC-BUG-002.

### Interpreter vs runtime memory map

`ppc_interp.py` maps the fixed-CROM window below the program ROM as a mirror
of the program ROM; `bus.c` maps it to CROM0 (MAME's `ROM_COPY` of CROM0 into
the fixed region). Scud's boot copies only from `0xFFF10000` up, inside the
program ROM, so its snapshot is unaffected. A title whose boot copies from
`0xFF800000..0xFFDFFFFF` would get a wrong snapshot from the interpreter.
