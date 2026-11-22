/* Platform layer: window/context, input, presentation.
 * platform_sdl.c   - SDL2 window + gamepad/keyboard (Windows, Linux, macOS, Android)
 * platform_headless.c - EGL surfaceless context, saves PNG screenshots (testing) */
#pragma once
/* before anything else: may change the working folder and the arguments (Android: the
 * app folder; desktop, started without arguments: the program's folder and the disc) */
void platform_prepare(int *argc, char ***argv);
int  platform_init(int argc, char **argv);
void platform_vblank(void);      /* called by the hardware model at every field */
void platform_shutdown(void);
