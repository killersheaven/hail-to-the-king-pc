# How the release works

This page explains what you download, what `build.bat` does with your disc, and what ends up on
your computer. The short version: **the download contains no part of the game. Your computer
builds the game from your own disc, and the result stays with you.**

## Why there's no ready-made .exe

A recompilation turns the game's own program into C code and compiles that into a Windows
program. Code made that way still belongs to the game's publisher, so it can't be shared. What
can be shared is everything around it: the recompiler that does the translating, and the runtime
that plays the part of the Dreamcast hardware. So the release is a kit, and the last step,
turning your copy of the game into a PC program, happens on your machine.

## What's in the download

| Folder or file | What it is |
|---|---|
| `build.bat`, `build.ps1` | The one-click Windows build (`build.ps1` does the work, `build.bat` starts it) |
| `build.sh` | The same for Linux |
| `games/hail-to-the-king/tools/` | The recompiler (`recomp.py` and friends) and the disc reader |
| `games/hail-to-the-king/runtime/` | The stand-in Dreamcast: graphics, sound, controller, memory card, disc drive, BIOS, plus the launcher and options window |
| `games/hail-to-the-king/notes/` | Names and hints for the recompiler (no game code) |
| `games/hail-to-the-king/textures/` | The redrawn HD legal screen and the button-prompt icons (made by this project, no game art) |
| `games/hail-to-the-king/dist/README.txt` | The player's guide that ends up next to the game |

## What `build.bat` does, step by step

1. **Finds your disc.** It looks for a `.cue` with "Hail to the King" in its name next to the
   project folder, or one level up. If it can't find one, it asks you to pick it.
2. **Gets the build tools, once.** It downloads these from their official release pages into a
   hidden `.tools` folder inside the project (about 400 MB unpacked). Nothing is installed on
   your system, and deleting the folder removes them.
   - llvm-mingw: the C compiler
   - CMake and Ninja: they run the compile
   - Python: it runs the recompiler
   - SDL2: windows, sound and controllers
3. **Reads the program from your disc.** `extract_disc.py` copies `1ST_READ.BIN` (the game's
   program) out of your disc image into `games/hail-to-the-king/orig/`.
4. **Translates it to C.** `recomp.py` finds the roughly 6,000 functions in the program and
   writes each one out as C in `games/hail-to-the-king/build/gen/`.
5. **Compiles and packages.** The C code and the runtime are compiled into
   `hail-to-the-king.exe`, which goes into a new `pc-build` folder with the files below.

| In `pc-build` | |
|---|---|
| `launcher.bat` | Start here: Play, Options, Extras |
| `play.bat` | Straight into the game |
| `options.bat` | Settings window only |
| `hail-to-the-king.exe`, `SDL2.dll` | The game |
| `textures/` | HD legal screen and button-prompt icons |
| `README.txt`, `LICENSE.txt` | Player's guide and licence |
| `settings.ini`, `vmu_a1.bin`, `vmu_a2.bin`, `log.txt` | Created when you play: settings, your two save slots, a log for bug reports |

The first build takes 15–30 minutes, mostly compiling. Running `build.bat` again later
reuses the tools.

## The Android app

`build-android.bat` runs the same first four steps, then:

1. downloads Java (Temurin 17), SDL2's source and Google's Android command-line tools, which
   fetch the Android platform library, build tools and the NDK (about 3 GB in all, once, into `.tools`);
2. compiles the game and SDL2 for 64-bit ARM, so they match the PC build bit for bit;
3. packages them with a small Java start-up class into `android-build/hail-to-the-king.apk`,
   signed with a key made on your PC (kept in `.tools`, so updates install over the old app).

On the phone, the app looks for your disc in its own folder
(`Android/data/com.killersheaven.hailtotheking/files`), which you fill from a PC over USB.
Saves, settings and `log.txt` live there too. Details: `games/hail-to-the-king/dist/README-android.txt`.

## While you play

The game still reads your disc image while it runs: models, sounds, music, movies and text all
come from it, just as on a Dreamcast. The `.bat` files hold the path to your `.cue`. If you move
the disc image, run `build.bat` again (or edit the path in the `.bat` files).

## Keeping things private

- Nothing is uploaded anywhere. The only network use is downloading the build tools in step 2.
- Please don't share your `pc-build` folder or the files in `orig/` and `build/`. They contain
  the game's code, translated or not.

## Updating to a new version

Download the new release, unzip it over the old project folder (or into a new folder), and run
`build.bat`. Copy `settings.ini` and your `vmu_a*.bin` saves from the old `pc-build` if you
used a new folder.

## What works in this version

| Feature | State |
|---|---|
| The game: playing, saving and loading (slots 1 and 2) | Works |
| Display options: fullscreen, monitor, window size, render resolution, aspect | Works |
| Texture filtering, menu sharpening | Works |
| Keyboard controls (rebind in Options) and game controllers | Works |
| Button prompts for Dreamcast / Xbox / PlayStation / Switch | Works |
| Testing cheats | Works |
| Documents viewer | Works |
| F8 model capture, F12 screenshots | Works |
| HD art: HD font and legal screen | Works (the title screen keeps its original art) |
| Cutscene viewer | Partly: the 56 Sofdec movies play; the 62 AVI movies don't yet |
| Model viewer | Partly: 35 characters in their rest pose; no animations yet |
| Android app (`build-android.bat`, controller only) | Early test: not yet tried on a phone |
| Save slots 3–8, sound reverb, Android touch controls | Not yet |

The options window shows the same thing: hover over an option or an Extras item and its status
appears under the description, and unfinished ones carry a **PARTIAL** or **NOT YET** tag.
