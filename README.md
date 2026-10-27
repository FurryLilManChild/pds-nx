# pds-nx

Panzer Dragoon Saga (AZEL -Panzer Dragoon RPG-) for Nintendo Switch homebrew.

A single-game Switch app built on the Yaba Sanshiro Sega Saturn emulator by devMiyax, using the
libretro `yabasanshiro` branch of the core and its ARM64 SH-2 recompiler. It plays from your own
disc images. No game data or BIOS is included.

**Version 1.0.0.** Plays at full speed on a Switch with the default settings, with an in-game menu
for save states, disc changes, video settings and button remapping.

An unofficial project, not affiliated with SEGA.

## What you need

- A Switch running Atmosphère with the homebrew menu.
- Your own Panzer Dragoon Saga discs as `.cue` + `.bin` files, or `.chd`. US (MK-81307), European
  and Japanese (GS-9076) releases are supported.
- Your own Sega Saturn BIOS file.

## Setup

1. Copy the `switch` folder from the release zip to the root of your SD card.
2. Put your Saturn BIOS in `/switch/pds-nx/` named `saturn_bios.bin`
   (`sega_101.bin` or `mpr-17933.bin` also work).
3. Put all four discs in `/switch/pds-nx/discs/`: each `.cue` with its `.bin` tracks, or one `.chd`
   per disc. They are matched by the "Disc 1" to "Disc 4" in their file names.

```
/switch/pds-nx/
    pds-nx.nro
    saturn_bios.bin
    discs/
        Panzer Dragoon Saga (USA) (Disc 1).cue
        Panzer Dragoon Saga (USA) (Disc 1) (Track 1).bin
        ...
    config.ini             created on first run
    saves/                 in-game saves (Saturn memory, Yaba Sanshiro backup.bin format)
    states/                save states
    pds-nx.log             log from the last run
```

## Launching

Hold **R** while starting any game, then pick Panzer Dragoon Saga from the homebrew menu. The app
needs full RAM access for its CPU recompiler, so launching from the Album will not work (it tells
you if you try).

## Controls

| Switch | Saturn |
| --- | --- |
| Y / B / A | A / B / C |
| ZL / X / ZR | X / Y / Z |
| L / R | L / R |
| + | Start |
| D-pad or left stick | D-pad |

Every button can be reassigned in the menu.

## In-game menu

Tap **−** (Minus) to open the menu. The game pauses while it is open; −, B or Resume closes it.

- **Save state / Load state**: 10 slots, pick one with Left/Right
- **Change disc**: pick disc 1 to 4 when the game asks for it
- **Video**: screen shape (4:3 or stretched 16:9), filter, internal resolution, polygon mode,
  sky/floor resolution and compute shader
- **Performance**: CPU and GPU clocks, audio or video timing, auto frameskip, FPS counter
- **Controls**: assign any Switch button to each Saturn button, or reset to the default layout
- **Reset game**, **Quit**

Changes apply right away and are saved to `config.ini`.

## Recommended settings

The defaults are tuned for full speed:

- CPU clock 1785 MHz. The app raises it while you play and restores it when you quit. If you use
  sys-clk, a sys-clk profile for the game you launch through will override this.
- Sky/floor on compute shader: on. This moves the game's sky and ground drawing to the GPU and is
  what brings it to 60 FPS.
- Polygon mode: perspective correction (no warped textures, and the fastest).
- Internal resolution can go up to 1080p with no slowdown. Sky/floor resolution is fine at 2x;
  720p and above need more GPU (try GPU clock 768 MHz, plugged in).
- Auto frameskip: on. It keeps sound clean if a scene briefly runs slow.

## Troubleshooting

- **Error screen right after launch:** make sure you launched by holding R over a game.
- **"Saturn BIOS not found" or "No discs found":** check the file names and folders above.
- **Something crashes:** send `/switch/pds-nx/pds-nx.log` and the newest file in
  `/atmosphere/crash_reports/`. Setting `debug_log=on` in `config.ini` adds timing details to the log.

## Building

From the devkitPro MSYS2 shell, with `switch-dev switch-sdl2 switch-mesa switch-libdrm_nouveau`
installed:

```
make -j8
```

Or with Docker: `docker run --rm -v $PWD:/src -w /src devkitpro/devkita64 make -j8`

## What changed from upstream Yaba Sanshiro

Switch-specific fixes and additions, all in this repository:

- SH-2 recompiler runs from libnx JIT code memory, flushes the instruction cache after each block
  (the upstream call was Linux-only), and reuses its code buffer across CPU resets instead of
  leaking 16 MB each time.
- Emulator threads are pinned to separate CPU cores (Horizon does not time-slice equal-priority
  threads, and the sound thread busy-waits).
- In-game saves are kept in memory and only the changed bytes are written to the SD card.
- Save states use a scratch file on the SD card (there is no `tmpfile()` on Switch).
- The GLES 3.1 compute entry points used by the rotating background renderer are now resolved
  (they were left NULL).
- Changing the polygon mode no longer also changes the screen rotation setting.
- Mid-game disc swapping without rebooting the game.

## Credits and license

- Yaba Sanshiro and its SH-2 recompiler by devMiyax, from Yabause by the Yabause team
  (see `core/AUTHORS`). Source from the libretro `yabasanshiro` branch of
  [libretro/yabause](https://github.com/libretro/yabause).
- libchdr, FLAC, LZMA SDK, zlib, Musashi, and libretro-common, as bundled with the core.
- 8x8 menu font: font8x8 by Daniel Hepper, public domain.
- Built with devkitPro, libnx, SDL2 and Mesa for Switch.

Licensed under the GNU General Public License v2 or later (see `LICENSE`), the same as Yabause.
