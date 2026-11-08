# Evil Dead: Hail to the King — PC recompilation

An unofficial native PC version of **Evil Dead: Hail to the King** (Dreamcast, 2000), made by
*static recompilation*: the game's own SH-4 program is translated to C ahead of time and runs on
a reimplementation of the Dreamcast's graphics, sound and controller hardware.

**No game data is included.** You build the game on your own computer from your own disc image,
and it reads everything (models, sound, movies) from that disc while it runs.

Current version: **v0.14.0** — see [CHANGELOG.md](CHANGELOG.md).

## What you get

- The full game running natively, at your monitor's resolution, in a window or fullscreen
- Keyboard (every key can be rebound) and modern controllers
- Button prompts for Dreamcast, Xbox, PlayStation or Switch controllers
- Memory-card saves as **save slots** (no VMU or block talk)
- A **launcher** with Extras:
  - **Documents**: all 17 notes, journals and Necronomicon pages
  - **Cutscenes**: the game's 56 Sofdec movies with sound (*partial: the 62 AVI movies don't play yet*)
  - **Models**: 35 characters and creatures, read straight from the disc's data files (*partial: rest pose only*)
- **F8 model capture**: saves the 3D models on screen as OBJ + textures
- An HD font, drawn while the game runs from Courier Prime (no game art is stored), and the start-up legal screen redrawn in high resolution with the port's credit line
- Optional testing cheats (immortality, unlimited ammo and fuel, Damascene blade)

## What you need

- Your own copy of the game as a **Redump-style `.cue` + `.bin` set** (USA release)
- Windows 10 or 11 (64-bit), or Linux
- About 1.5 GB of free space for the build tools and the build, and 15–30 minutes the first time

New here? [How the release works](HOW-THE-RELEASE-WORKS.md) explains what you download, what the build does and what works so far.

## Building on Windows

1. Download this project (green **Code** button → *Download ZIP*, or from the [latest release](../../releases/latest)) and unzip it.
2. Double-click **`build.bat`**. It looks for your disc's `.cue` next to the project and asks you to pick it if it can't find it.
3. The first run downloads the build tools into a `.tools` folder (llvm-mingw, CMake, Ninja, an embedded Python and SDL2 — about 400 MB, from their official release pages), reads the program from your disc, translates it to C and compiles it.
4. When it says **Done**, open the new `pc-build` folder and start **`launcher.bat`**.

## Building on Linux

Install `python3`, `cmake`, `ninja` (or `make`), `clang` or `gcc`, and the SDL2 development package, then:

```sh
./build.sh "/path/to/Evil Dead - Hail to the King (USA).cue"
```

The game ends up in `pc-build-linux/` (start `launcher.sh`).

## How it works

`tools/extract_disc.py` copies the game's program (`1ST_READ.BIN`) out of your disc image.
`tools/recomp.py` finds every function in it and translates each SH-4 instruction to C
(about 6,000 functions). The runtime in `runtime/` supplies everything the Dreamcast hardware
did: the PowerVR2 graphics chip (drawn with OpenGL ES 3 / OpenGL 3.3), the AICA sound chip and
its ARM7 CPU, the Maple controller bus and memory card, the GD-ROM drive (reading your `.cue`
directly) and the BIOS calls. See [TECHNICAL_NOTES.md](games/hail-to-the-king/TECHNICAL_NOTES.md)
for the details and for undocumented findings about the game (data formats, RAM addresses).

## Status and known gaps

The game boots, plays, saves and loads. Not done yet: sound reverb/echo (the AICA DSP), save
slots 3–8, the disc's AVI (TrueMotion) movies, model animations, and an Android version. The full
list is in [How the release works](HOW-THE-RELEASE-WORKS.md#what-works-in-this-version).

## Legal

This is a fan project, not affiliated with or endorsed by THQ, Heavy Iron Studios or the owners
of *Evil Dead*. *Evil Dead* and its related characters are trademarks of their respective
owners; *Evil Dead: Hail to the King* © 2000 THQ Inc. The repository contains only original
code and tools; you need your own legally obtained copy of the game. Please don't share builds:
a build contains code translated from the game.

The project's own code is © 2026 Killers' Heaven and licensed under the
[GNU General Public License v3](LICENSE). Third-party components are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
