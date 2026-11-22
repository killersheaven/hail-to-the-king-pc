# Killers' Heaven launcher (khl)

The shared launcher of the Evil Dead Collection ports: one look and one way of
working for every game (title screen with Play / Options / Extras / Quit,
option pages with status tags, key and controller remapping, Extras with a
text reader). Keyboard, mouse and game controller all work.

- `khl.h` / `khl.c`: the module (SDL2 2D renderer, stb_truetype, Courier Prime).
  It knows nothing about any game; a game fills in a `khl_game` and calls `khl_run()`.
- `third_party/`: stb_truetype (public domain / MIT) and Courier Prime
  (SIL Open Font License 1.1, `OFL-CourierPrime.txt`).

**Each game keeps its own copy of this folder**, so every game builds and runs
on its own and never needs another game installed. Keep the copies identical
and bump `KHL_VERSION` in `khl.h` when it changes.

Test hooks: `KHL_KEYS="down,enter,tab,..."` feeds one key per frame
(`up down left right enter esc tab del key:<SDL key name>`), then
`KHL_SHOT=file.bmp` saves a screenshot and closes the window
(`SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software` runs it headless).

License: GPL-3.0 (Killers' Heaven).
