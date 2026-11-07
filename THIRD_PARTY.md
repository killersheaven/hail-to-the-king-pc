# Third-party components

| Component | Where | License |
|---|---|---|
| pl_mpeg (Dominic Szablewski) | `games/hail-to-the-king/runtime/third_party/pl_mpeg.h` | MIT |
| stb_image, stb_truetype (Sean Barrett) | `games/hail-to-the-king/runtime/third_party/` | MIT / public domain |
| Courier Prime (Alan Dague-Greene) | embedded in `runtime/third_party/courierprime_ttf.h`, `tools/fontpack/` | SIL Open Font License 1.1 (`tools/fontpack/OFL.txt`) |
| EB Garamond (Georg Duffner, Octavio Pardo) | `tools/fontpack/EBGaramond.ttf` | SIL Open Font License 1.1 (`tools/fontpack/EBGaramond-OFL.txt`) |
| DejaVu Sans Bold (letters in the button-prompt icons, `tools/fontpack/prompticons.py`) | rendered into `textures/prompts/*/icons.png` | Bitstream Vera / DejaVu font license |
| Khronos EGL / OpenGL ES headers | `games/hail-to-the-king/third_party/khronos/` | Apache 2.0 / MIT (see file headers) |

Downloaded by the build scripts, not stored here: SDL2 (zlib license), llvm-mingw (Apache 2.0
with LLVM exception and others), CMake (BSD 3-clause), Ninja (Apache 2.0), Python (PSF license).
SDL2.dll is copied next to the game.

Cheat addresses come from the published Dreamcast CodeBreaker codes for the game.
