Evil Dead: Hail to the King - unofficial PC recompilation, v0.15.0
(c) 2026 Killers' Heaven - free software under the GNU GPL v3 (LICENSE.txt)

This folder was built on your computer from your own disc. Start:
  launcher.bat   Play, Options and Extras (documents, cutscenes, models)
  play.bat       straight into the game
  options.bat    the settings window
The disc image you built from must stay where it is: the game reads it while
it runs (the paths are in the .bat files; rebuild if you move it).

CONTROLS
  Keyboard: arrows = D-pad, Enter = Start, Z = A, X = B, A = X, S = Y,
  Q / W = left / right trigger (change any of them in Options > Keyboard
  controls). A game controller works too (plug it in before starting). In the game, A attacks and B examines / picks up.
  F11 fullscreen / window, F10 render resolution, F9 4:3 / stretch,
  F12 screenshot, F8 save the 3D models on screen (captures folder), Esc quit.

LEGAL SCREEN
  The THQ legal screen at start-up is redrawn in high resolution, with the
  credit line for this port (textures folder; turn off with HD art = off).

OPTIONS (options.bat, or settings.ini)
  Hover over an option to see what it does and whether it works yet;
  unfinished ones are tagged PARTIAL or NOT YET.
  Works: display mode and monitor, window size, render resolution, 4:3 or
  stretch, texture filtering (smooth / sharp), menu sharpening, button
  prompts (Dreamcast, Xbox, PlayStation, Switch), keyboard controls, testing
  cheats (immortality, unlimited ammo, unlimited chainsaw
  fuel, Damascene blade - all off by default), HD art (an HD font drawn
  while the game runs, and the HD legal screen).

SAVING
  The memory cards are emulated. Save Slots 1-6 work and are files in this
  folder: Slots 1-2 = vmu_a1.bin, vmu_a2.bin; 3-4 = vmu_b1.bin, vmu_b2.bin;
  5-6 = vmu_c1.bin, vmu_c2.bin (each file appears on its first save).
  Slots 7-8 stay greyed out: the game itself never checks the fourth
  controller port, where those cards would be. Pick up the Save Tape from the tape recorder in
  the cabin, then use it at a save box.

EXTRAS (launcher.bat)
  Documents: all 17 notes, journals and Necronomicon pages, read from the disc.
  Cutscenes (partly): the 56 Sofdec movies, picture and sound. The 62 AVI
  movies don't play yet.
  Models (partly): 35 characters and creatures in their rest pose, no
  animations yet; turn with the arrows / mouse / right stick, zoom with
  +/- / wheel / triggers.

KNOWN GAPS
  No reverb / echo in the sound yet. Save slots 7-8 stay greyed out. The
  title screen keeps its original art. See HOW-THE-RELEASE-WORKS.md in the project folder for
  the full list.

If the screen stays black or the game closes at once, send log.txt from this
folder when you report it.

Evil Dead and its related characters are trademarks of their respective
owners. Evil Dead: Hail to the King (c) 2000 THQ Inc. This project is not
affiliated with or endorsed by THQ, Heavy Iron Studios or the rights holders,
and it contains no game data.
