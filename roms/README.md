# roms/

Put the ROM images you build from **your own** romset here. Nothing in this
directory except this file is ever committed (`.gitignore` excludes the rest),
and no ROM is downloaded, bundled or distributed by this repository.

Scud Race (MAME set `scud`, Export Twin/DX):

```
python tools/scud/build_roms.py /path/to/your/scud.zip roms/
```

writes `scud_prog.bin`, `scud_bank.bin`, `scud_vrom.bin`, `scud_snd.bin` and
`scud_samples.bin`. See `docs/dreamcast/README.md` for the rest of the build.
