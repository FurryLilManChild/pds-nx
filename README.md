# pds-nx

Panzer Dragoon Saga (AZEL -Panzer Dragoon RPG-) for Nintendo Switch homebrew.

A single-game Switch app built on the Yaba Sanshiro Sega Saturn emulator by devMiyax, using the
libretro `yabasanshiro` branch of the core with its ARM64 SH-2 dynarec. It plays from your own
disc images; no game data or BIOS is included.

**Status:** early test builds.

## What you need

- A Switch running Atmosphère with the homebrew menu.
- Your own Panzer Dragoon Saga discs as `.cue` + `.bin` files or `.chd` (US, Europe, or Japanese).
- Your own Sega Saturn BIOS file.

## SD card layout

```
/switch/pds-nx/
    pds-nx.nro
    saturn_bios.bin        (sega_101.bin or mpr-17933.bin also work)
    discs/                 all four discs: each .cue with its .bin tracks, or .chd
    config.ini             created on first run
    saves/                 Saturn save memory (Yaba Sanshiro backup.bin format)
    states/                save states
    pds-nx.log             log from the last run
```

Discs are matched by the "Disc 1" to "Disc 4" in their file names.

## Launching

Hold **R** while starting any game, then pick pds-nx from the homebrew menu. It needs full RAM
access for its CPU recompiler, so launching from the Album will not work.

## Controls

| Switch | Saturn |
| --- | --- |
| Y / B / A | A / B / C |
| ZL / X / ZR | X / Y / Z |
| L / R | L / R |
| + | Start |
| D-pad or left stick | D-pad |

Hold **−** for shortcuts:

| Shortcut | Action |
| --- | --- |
| − + R | Save state |
| − + L | Load state |
| − + D-pad Right / Left | Next / previous disc |
| − + + | Quit |

## Settings

`config.ini` is created with comments on first run. Changes apply the next launch.

## Building

From the devkitPro MSYS2 shell, with `switch-dev switch-sdl2 switch-mesa switch-libdrm_nouveau`
installed:

```
make -j8
```

## Credits and license

- Yaba Sanshiro and its SH-2 dynarec by devMiyax, from Yabause by the Yabause team
  (see `core/AUTHORS`). Source taken from the libretro `yabasanshiro` branch of
  [libretro/yabause](https://github.com/libretro/yabause).
- libchdr, FLAC, LZMA SDK, zlib, Musashi, and libretro-common, as bundled with the core.

Licensed under the GNU General Public License v2 or later (see `LICENSE`), the same as Yabause.
An unofficial project, not affiliated with SEGA.
