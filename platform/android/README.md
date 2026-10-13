# Android

Scud Race on an Android phone (arm64): the desktop build's SDL2 platform
layer and software renderer, packaged with SDL2's `SDLActivity`.

Neither the game's code nor SDL2 is in this repository:

- the lifted Scud Race C, from your own ROM set (`tools/scud/lift.py`, as for
  the desktop build);
- an SDL2 source tree: `git clone -b release-2.32.10 https://github.com/libsdl-org/SDL.git SDL2`.

Requirements: JDK 17, Android SDK 37, NDK 28.2.13676358, CMake 3.31.6, and
a Gradle compatible with the Android Gradle Plugin 9.4.1.

```sh
cd platform/android
gradle :app:assembleDebug -PscudLiftedDir=/path/to/lifted -Psdl2Dir=/path/to/SDL2
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The ROM images (`tools/scud/build_roms.py`: `scud_prog.bin`, `scud_bank.bin`,
`scud_vrom.bin`, `scud_snd.bin`, `scud_samples.bin`) go in the app's folder
on shared storage, which `adb push` can write and the app reads without any
permission:

```sh
adb shell mkdir -p /sdcard/Android/data/org.model3recomp.scudrace/files/roms
adb shell chmod 2775 /sdcard/Android/data/org.model3recomp.scudrace /sdcard/Android/data/org.model3recomp.scudrace/files /sdcard/Android/data/org.model3recomp.scudrace/files/roms
adb push roms/. /sdcard/Android/data/org.model3recomp.scudrace/files/roms/
```

The `chmod` matters: a folder `adb` makes there belongs to `shell` with mode
2770, which the app cannot enter (`Permission denied` in logcat, then "No
program ROM").

## Controls

Touch controls are drawn over the bars beside the picture: COIN and START at
the top left, VIEW (the next view) at the top right, gear - and + over BRAKE
and GAS at the bottom right. Steering floats: put a thumb anywhere on the
left half, where it lands is straight ahead, and 70 dp to a side is full
lock. A gamepad works as on the desktop. The back button does nothing, so an
edge swipe cannot end a race; leave with Home.

## Measured

| Phone | Field time | Of which software 3D |
|---|---|---|
| Galaxy SM-S948B (Snapdragon, Adreno 840) | 17.4 ms (57.52 Hz, full speed), attract and race | 8.5-10.9 ms |
| Redmi Note 13 (Snapdragon 685, Adreno 610) | boots; 3D not measured yet | |

A mid-range phone will need the 3D on its GPU (a sink like the Dreamcast's
`src/renderer/pvr`): on the desktop it is 7.7 ms of a 12.8 ms field.

Diagnostics: stdout and stderr go to logcat (`adb logcat -s scudrace`), and
`files/env.txt` in that same folder sets the `M3_` variables a desktop run
reads from its environment, one `NAME=value` a line (`M3_PROFILE=1` prints
where each field's time goes).
