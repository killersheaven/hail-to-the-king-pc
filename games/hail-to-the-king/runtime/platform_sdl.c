/* SDL2 platform: window + OpenGL ES 3 context, keyboard and gamepad -> Maple controller.
 * Works on Windows, Linux, macOS and Android (SDL provides the GLES context;
 * on desktop drivers this is the ES profile of the native GL driver).
 *
 * Keyboard: arrows = d-pad, Z = A, X = B, A = X, S = Y, Enter = Start,
 *           Q/W = left/right trigger, Esc = quit, F11 = fullscreen,
 *           F10 = cycle render resolution, F9 = 4:3 / stretch, F12 = screenshot.
 *
 * Display settings live in settings.ini next to the program (written with
 * defaults on first run; changes made with the hotkeys are saved back):
 *   display = fullscreen | windowed     fullscreen uses the monitor's own resolution
 *   monitor = 0                          which monitor (0 = primary)
 *   window = 1280x960                    window size when windowed
 *   render = auto | 2x | 1920x1440 ...   auto = match the screen/window (sharpest)
 *   aspect = 4:3 | stretch
 *   ui_upscale = text | all | off        sharpening of menu and HUD art
 */
#include "platform.h"
#include "render_gl.h"
#include "maple.h"
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *win;
static SDL_GLContext ctx;
static SDL_GameController *pad;
static int have_frame;
static SDL_AudioDeviceID audio;
static int resize_pending, cfg_dirty;
int aica_audio_read(int16_t *dst, int frames);
int aica_audio_available(void);
void aica_audio_skip(int frames);
static void audio_pump(void) {
    if (!audio) return;
    static int16_t buf[8192 * 2];
    Uint32 queued = SDL_GetQueuedAudioSize(audio) / 4;        /* frames waiting in the device queue */
    int avail = aica_audio_available();
    if (queued > 44100 / 4) { aica_audio_skip(avail); return; }   /* more than 250 ms behind: drop to stay in sync */
    while (avail > 0) {
        int n = aica_audio_read(buf, avail > 8192 ? 8192 : avail);
        if (n <= 0) break;
        SDL_QueueAudio(audio, buf, (Uint32)n * 4);
        avail -= n;
    }
}

static void *getproc(const char *n) { return SDL_GL_GetProcAddress(n); }

/* ---- settings.ini ---- */
static struct {
    int fullscreen, monitor, win_w, win_h;
    int render_mode;            /* 0 auto, N>0: N x 640x480, -1: fixed size */
    int render_w, render_h, stretch, ui_mode;
    int sharp, hd, prompts;     /* texture_filter, hd_textures, button_prompts */
    int cheats;                 /* bit 0 immortality, 1 ammo, 2 fuel, 3 Damascene blade */
} cfg = { 1, 0, 1280, 960, 0, 0, 0, 0, 1, 0, 1, 0, 0 };
/* keyboard -> Dreamcast pad (rebindable in the options window; key_* in settings.ini) */
enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_X, K_Y, K_START, K_L, K_R, NKEYS };
static const char *key_cfg[NKEYS] = { "key_up", "key_down", "key_left", "key_right", "key_a", "key_b", "key_x", "key_y", "key_start", "key_ltrigger", "key_rtrigger" };
static const char *key_label[NKEYS] = { "Up", "Down", "Left", "Right", "A button", "B button", "X button", "Y button", "Start", "Left trigger", "Right trigger" };
static const SDL_Scancode key_default[NKEYS] = { SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_RETURN, SDL_SCANCODE_Q, SDL_SCANCODE_W };
static SDL_Scancode keymap[NKEYS] = { SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_RETURN, SDL_SCANCODE_Q, SDL_SCANCODE_W };
static int key_reserved(SDL_Scancode s) {   /* hotkeys that can't be used for the pad */
    return s == SDL_SCANCODE_ESCAPE || (s >= SDL_SCANCODE_F8 && s <= SDL_SCANCODE_F12) || s == SDL_SCANCODE_UNKNOWN;
}
static const char *cheat_keys[4] = { "cheat_immortality", "cheat_unlimited_ammo", "cheat_unlimited_fuel", "cheat_damascene_blade" };
extern int hw_cheats;
static const char *prompt_names[4] = { "dreamcast", "xbox", "playstation", "switch" };
static char cfg_path[1024];
static void cfg_save(void) {
    FILE *f = fopen(cfg_path, "w");
    if (!f) return;
    fprintf(f, "# Evil Dead: Hail to the King - display settings\n"
               "# display: fullscreen (uses your monitor's own resolution) or windowed\n"
               "display = %s\n"
               "# monitor: which monitor to use for fullscreen (0 = primary)\n"
               "monitor = %d\n"
               "# window: window size when windowed\n"
               "window = %dx%d\n"
               "# render: internal resolution. auto = match the screen (sharpest);\n"
               "#         1x = original 640x480, 2x, 3x, 4x; or a size such as 1920x1440\n",
            cfg.fullscreen ? "fullscreen" : "windowed", cfg.monitor, cfg.win_w, cfg.win_h);
    if (cfg.render_mode == 0) fprintf(f, "render = auto\n");
    else if (cfg.render_mode > 0) fprintf(f, "render = %dx\n", cfg.render_mode);
    else fprintf(f, "render = %dx%d\n", cfg.render_w, cfg.render_h);
    fprintf(f, "# aspect: 4:3 (original, black bars on wide screens) or stretch\n"
               "aspect = %s\n"
               "# ui_upscale: sharpen menu/HUD art at high resolutions: text, all or off\n"
               "ui_upscale = %s\n"
               "# texture_filter: smooth (original bilinear filtering) or sharp (unfiltered pixels)\n"
               "texture_filter = %s\n"
               "# hd_textures: on = use the HD images in the textures folder (font, logo, legal screen)\n"
               "hd_textures = %s\n"
               "# button_prompts: dreamcast, xbox, playstation or switch (icons shown in menus)\n"
               "button_prompts = %s\n"
               "#\n# Hotkeys: F11 fullscreen, F10 render resolution, F9 aspect, F12 screenshot\n"
               "# Run options.bat for a settings window.\n"
               "#\n# Testing cheats (on/off)\n"
               "cheat_immortality = %s\ncheat_unlimited_ammo = %s\ncheat_unlimited_fuel = %s\ncheat_damascene_blade = %s\n",
            cfg.stretch ? "stretch" : "4:3", cfg.ui_mode == 0 ? "off" : cfg.ui_mode == 2 ? "all" : "text",
            cfg.sharp ? "sharp" : "smooth", cfg.hd ? "on" : "off", prompt_names[cfg.prompts & 3],
            cfg.cheats & 1 ? "on" : "off", cfg.cheats & 2 ? "on" : "off", cfg.cheats & 4 ? "on" : "off", cfg.cheats & 8 ? "on" : "off");
    fprintf(f, "#\n# Keyboard: which key acts as each Dreamcast pad button (key names as SDL writes them)\n");
    for (int i = 0; i < NKEYS; i++) fprintf(f, "%s = %s\n", key_cfg[i], SDL_GetScancodeName(keymap[i]));
    fclose(f);
}
static void cfg_load(void) {
    char *base = SDL_GetBasePath();
    snprintf(cfg_path, sizeof cfg_path, "%ssettings.ini", base ? base : "");
    SDL_free(base);
    memcpy(keymap, key_default, sizeof keymap);
    FILE *f = fopen(cfg_path, "r");
    if (!f) { cfg_save(); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char k[64], v[64];
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %63[^\r\n]", k, v) != 2) continue;
        for (size_t l = strlen(v); l && (v[l - 1] == ' ' || v[l - 1] == '\t'); ) v[--l] = 0;
        if (!SDL_strncasecmp(k, "key_", 4)) {
            for (int i = 0; i < NKEYS; i++) if (!SDL_strcasecmp(k, key_cfg[i])) {
                SDL_Scancode s = SDL_GetScancodeFromName(v); if (!key_reserved(s)) keymap[i] = s; }
            continue;
        }
        int a, b;
        if (!SDL_strcasecmp(k, "display")) cfg.fullscreen = SDL_strcasecmp(v, "windowed") != 0;
        else if (!SDL_strcasecmp(k, "monitor")) cfg.monitor = atoi(v);
        else if (!SDL_strcasecmp(k, "window") && sscanf(v, "%dx%d", &a, &b) == 2 && a >= 320 && b >= 240) { cfg.win_w = a; cfg.win_h = b; }
        else if (!SDL_strcasecmp(k, "render")) {
            if (!SDL_strcasecmp(v, "auto")) cfg.render_mode = 0;
            else if (sscanf(v, "%dx%d", &a, &b) == 2 && a >= 64 && b >= 48) { cfg.render_mode = -1; cfg.render_w = a; cfg.render_h = b; }
            else if (sscanf(v, "%dx", &a) == 1 && a >= 1 && a <= 8) cfg.render_mode = a;
        }
        else if (!SDL_strcasecmp(k, "aspect")) cfg.stretch = !SDL_strcasecmp(v, "stretch");
        else if (!SDL_strncasecmp(k, "cheat_", 6)) { for (int i = 0; i < 4; i++) if (!SDL_strcasecmp(k, cheat_keys[i])) {
            if (!SDL_strcasecmp(v, "on")) cfg.cheats |= 1 << i; else cfg.cheats &= ~(1 << i); } }
        else if (!SDL_strcasecmp(k, "texture_filter")) cfg.sharp = !SDL_strcasecmp(v, "sharp");
        else if (!SDL_strcasecmp(k, "hd_textures")) cfg.hd = SDL_strcasecmp(v, "off") != 0;
        else if (!SDL_strcasecmp(k, "button_prompts")) { for (int i = 0; i < 4; i++) if (!SDL_strcasecmp(v, prompt_names[i])) cfg.prompts = i; }
        else if (!SDL_strcasecmp(k, "ui_upscale")) cfg.ui_mode = !SDL_strcasecmp(v, "off") || !strcmp(v, "0") ? 0 : !SDL_strcasecmp(v, "all") ? 2 : 1;
    }
    fclose(f);
}
#include "options_ui.c"
static char osd[96];                   /* last setting change, shown in the window title */
static void show_setting(const char *msg) {
    SDL_strlcpy(osd, msg, sizeof osd);
    char t[160]; snprintf(t, sizeof t, "Evil Dead: Hail to the King  -  %s", msg);
    SDL_SetWindowTitle(win, t);
    fprintf(stderr, "[sdl] %s\n", msg);
}
void platform_notice(const char *msg) { show_setting(msg); }
static void on_render(const pvr_frame *f) { render_gl_frame(f); have_frame = 1; }

int platform_init(int argc, char **argv) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");   /* real pixels on scaled (125%/150%) Windows displays */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) { fprintf(stderr, "[sdl] %s\n", SDL_GetError()); return 0; }
    cfg_load();
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--options") || !strcmp(argv[i], "--launcher")) {   /* options.bat / launcher.bat */
        if (!options_window_ex(!strcmp(argv[i], "--launcher"))) { SDL_Quit(); exit(0); }
        break;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);       /* opaque window: the game uses alpha only for blending */
    if (getenv("DC_WINDOWED")) cfg.fullscreen = 0;
    if (cfg.monitor < 0 || cfg.monitor >= SDL_GetNumVideoDisplays()) cfg.monitor = 0;
    Uint32 wflags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    win = SDL_CreateWindow("Evil Dead: Hail to the King", SDL_WINDOWPOS_CENTERED_DISPLAY(cfg.monitor), SDL_WINDOWPOS_CENTERED_DISPLAY(cfg.monitor),
                           cfg.win_w, cfg.win_h, wflags);
    if (!win) { fprintf(stderr, "[sdl] window: %s\n", SDL_GetError()); return 0; }
    ctx = SDL_GL_CreateContext(win);
    int desktop = 0;
    if (!ctx || getenv("DC_DESKTOPGL")) {        /* no OpenGL ES on this driver: use desktop OpenGL 3.3 core */
        if (ctx) SDL_GL_DeleteContext(ctx);
        fprintf(stderr, "[sdl] OpenGL ES 3 unavailable (%s); trying OpenGL 3.3 core\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        win = SDL_CreateWindow("Evil Dead: Hail to the King", SDL_WINDOWPOS_CENTERED_DISPLAY(cfg.monitor), SDL_WINDOWPOS_CENTERED_DISPLAY(cfg.monitor),
                               cfg.win_w, cfg.win_h, wflags);
        ctx = win ? SDL_GL_CreateContext(win) : NULL;
        desktop = 1;
    }
    if (!ctx) { fprintf(stderr, "[sdl] no OpenGL context: %s\n", SDL_GetError()); return 0; }
    SDL_GL_SetSwapInterval(0);           /* the game paces itself on its own vblank interrupts */
    int scale = getenv("DC_SCALE") ? atoi(getenv("DC_SCALE")) : 2;
    if (!render_gl_init(getproc, scale, desktop)) return 0;
    pvr_set_renderer(on_render);
    render_gl_set_stretch(cfg.stretch);
    render_gl_set_options(cfg.sharp, cfg.hd, cfg.prompts);
    if (!getenv("DC_CHEATS")) hw_cheats = cfg.cheats;
    if (!getenv("DC_UIUPSCALE")) render_gl_set_ui_mode(cfg.ui_mode);
    { SDL_DisplayMode dm; if (SDL_GetDesktopDisplayMode(cfg.monitor, &dm) == 0)
        fprintf(stderr, "[sdl] monitor %d: %dx%d @ %d Hz, %s\n", cfg.monitor, dm.w, dm.h, dm.refresh_rate, cfg.fullscreen ? "fullscreen" : "windowed"); }
    if (!getenv("DC_NOAUDIO")) {
        SDL_AudioSpec want = {0}, got;
        want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        audio = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
        if (audio) SDL_PauseAudioDevice(audio, 0);
        else fprintf(stderr, "[sdl] no audio device: %s\n", SDL_GetError());
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }
    return 1;
}

static uint16_t key_buttons(void) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint16_t b = 0;
    static const uint16_t bit[K_START + 1] = { MAPLE_UP, MAPLE_DOWN, MAPLE_LEFT, MAPLE_RIGHT, MAPLE_A, MAPLE_B, MAPLE_X, MAPLE_Y, MAPLE_START };
    for (int i = 0; i <= K_START; i++) if (k[keymap[i]]) b |= bit[i];
    return b;
}

void platform_vblank(void) {
    audio_pump();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) exit(0);
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11) {
            Uint32 fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
            SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            cfg.fullscreen = !fs; cfg_save();
            show_setting(cfg.fullscreen ? "fullscreen" : "windowed");
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F10) {
            /* auto -> 1x -> 2x -> 3x -> 4x -> auto */
            cfg.render_mode = cfg.render_mode < 0 ? 0 : cfg.render_mode >= 4 ? 0 : cfg.render_mode + 1;
            cfg_save();
            resize_pending = 1;
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F9) {
            cfg.stretch = !cfg.stretch; render_gl_set_stretch(cfg.stretch); cfg_save();
            resize_pending = 1;
            show_setting(cfg.stretch ? "aspect: stretch" : "aspect: 4:3");
        }
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && !cfg.fullscreen &&
            !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
            int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
            if (ww >= 320 && wh >= 240 && (ww != cfg.win_w || wh != cfg.win_h)) { cfg.win_w = ww; cfg.win_h = wh; cfg_dirty = 1; }
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F8) {   /* save the models on screen (captures/) */
            extern int render_gl_capture_pending; render_gl_capture_pending = 1;
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12) {
            int png_write(const char *path, const uint8_t *rgba, int w, int h, int bottom_up);
            static int shot; int w, h; render_gl_read(NULL, &w, &h);
            uint8_t *buf = malloc((size_t)w * h * 4); render_gl_read(buf, &w, &h);
            char p[64]; snprintf(p, sizeof p, "screenshot_%03d.png", ++shot);
            png_write(p, buf, w, h, 1); free(buf);
            fprintf(stderr, "[sdl] saved %s\n", p);
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
    }
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    maple_pad_t p = { key_buttons(), k[keymap[K_R]] ? 255 : 0, k[keymap[K_L]] ? 255 : 0, 0x80, 0x80 };
    if (pad) {
        static const struct { SDL_GameControllerButton b; uint16_t m; } map[] = {
            {SDL_CONTROLLER_BUTTON_A, MAPLE_A}, {SDL_CONTROLLER_BUTTON_B, MAPLE_B}, {SDL_CONTROLLER_BUTTON_X, MAPLE_X},
            {SDL_CONTROLLER_BUTTON_Y, MAPLE_Y}, {SDL_CONTROLLER_BUTTON_START, MAPLE_START},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, MAPLE_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, MAPLE_DOWN},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, MAPLE_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, MAPLE_RIGHT}};
        for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (SDL_GameControllerGetButton(pad, map[i].b)) p.buttons |= map[i].m;
        int lt = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT), rt = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        int lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (lt > p.ltrig * 128) p.ltrig = (uint8_t)(lt >> 7);
        if (rt > p.rtrig * 128) p.rtrig = (uint8_t)(rt >> 7);
        if (lx < -4000 || lx > 4000) p.joyx = (uint8_t)((lx + 32768) >> 8);
        if (ly < -4000 || ly > 4000) p.joyy = (uint8_t)((ly + 32768) >> 8);
    }
    extern int render_frame3d;
    if (!getenv("DC_INPUT")) maple_pad = p;
    if (cfg_dirty && !(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK)) { cfg_dirty = 0; cfg_save(); }   /* window resized: remember the size */
    if (have_frame) {
        int w, h; SDL_GL_GetDrawableSize(win, &w, &h);
        if (!getenv("DC_SCALE")) {
            int vw, vh;
            if (cfg.render_mode > 0) { vw = 640 * cfg.render_mode; vh = 480 * cfg.render_mode; }
            else if (cfg.render_mode < 0) { vw = cfg.render_w; vh = cfg.render_h; }
            else if (cfg.stretch) { vw = w; vh = h; }   /* auto: the screen's own pixels */
            else { vw = w; vh = w * 3 / 4; if (vh > h) { vh = h; vw = h * 4 / 3; } }
            render_gl_resize(vw, vh);
            if (resize_pending) {
                resize_pending = 0;
                char m[64];
                if (cfg.render_mode > 0) snprintf(m, sizeof m, "render %dx (%dx%d)", cfg.render_mode, vw, vh);
                else if (cfg.render_mode < 0) snprintf(m, sizeof m, "render %dx%d", vw, vh);
                else snprintf(m, sizeof m, "render auto (%dx%d)", vw, vh);
                show_setting(m);
            }
        }
        render_gl_present(w, h);
        SDL_GL_SwapWindow(win);
        have_frame = 0;
    }
}
void platform_shutdown(void) { if (ctx) SDL_GL_DeleteContext(ctx); if (win) SDL_DestroyWindow(win); SDL_Quit(); }
