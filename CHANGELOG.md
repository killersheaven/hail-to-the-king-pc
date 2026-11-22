# Changelog

Version numbers: `0.<build>.<fix>` until a complete playthrough is verified and the known gaps
are closed, which will be 1.0.0. The test builds before the first public release were numbered
1–14, so the first release is 0.14.0.

## Unreleased

- Saving: Save Slots 1 and 2 are the two memory cards (`vmu_a1.bin`, `vmu_a2.bin`), and each
  holds up to eight saves, so sixteen in all. A card's file is made on its first save instead
  of at start-up.
- `save_slots = 6` in `settings.ini` adds Slots 3–6 (cards on emulated controllers B and C,
  `vmu_b1.bin` … `vmu_c2.bin`). Slots 7–8 can't work: the game never checks controller D.
- Moving or copying the game folder no longer breaks it: `build.bat` starts the compile over
  when the folder was built somewhere else, and the `.bat` files find the disc relative to
  their own folder.
- `tools/vmu_import.py` copies a Dreamcast save in VMI/VMS form onto a memory card file.
- Sound: the AICA effect DSP (the chip's reverb/echo processor) is emulated. The game loads an
  84-step effect program, but in the areas tested so far it sends no sound to it, so there's no
  audible change yet; `log.txt` notes the first time it does.
- Android: the display rows that only apply on PC are tagged PC ONLY; 4:3 explained.

## v0.15.0 — pre-release (2026-11-08)

- HD font is back: the game's font sheet is redrawn at 4x with Courier Prime while the game
  runs, keeping every character's place, so no game art is stored. HD art now works.
- With Xbox, PlayStation or Switch prompts, the button icons are HD as well.
- Not in this release: the HD title-screen art from the test builds (it's the game's own art and
  logos), sound reverb, save slots 3–8, AVI movies.

## v0.14.0 — first public release (2026-11-02)

Everything from the 14 test builds, now built from your own disc:

- Native Windows and Linux builds via static recompilation (about 6,000 functions)
- Plays straight from a Redump `.cue`/`.bin` set, movies included
- Fullscreen at your monitor's resolution, render resolution and aspect options, sharp or smooth texture filtering, sharpened menus
- Options window: display, monitor, window size, resolution, aspect, filtering, button prompts (Dreamcast / Xbox / PlayStation / Switch), keyboard controls, testing cheats
- Memory-card saves as save slots (slots 1 and 2)
- Launcher with Play, Options and Extras: document reader (17), cutscene player (56 Sofdec movies), model viewer (35 characters)
- F8 model capture to OBJ, F12 screenshots
- Keyboard controls: every Dreamcast button can be rebound in the options window
- Every option and Extras item shows whether it works, partly works or isn't done yet
- Fixes: freezes from masked interrupts, cut-off menu text and buttons, the legal screen flashing while skipping movies

- The start-up legal screen redrawn in high resolution, with the line "Unofficial PC recompilation © 2026 Killers' Heaven."

Not in this release: the HD font from the test builds, sound reverb, save slots 3–8, AVI movies.
