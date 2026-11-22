/*
 * Killers' Heaven launcher (khl): see khl.h.
 * Unofficial recompilation (c) 2026 Killers' Heaven, GPL-3.0.
 *
 * The look started in Hail to the King's launcher (options_ui.c) and is the
 * same here: dark red glow, Courier Prime, red selection bars with a bright
 * edge, status tags in green / amber / red / grey.
 */
#include "khl.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "third_party/stb_truetype.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "third_party/courierprime_ttf.h"

enum { UI_W = 960, UI_H = 720, NSIZES = 4 };
static const float ui_px[NSIZES] = { 19, 25, 40, 96 };  /* small, normal, title, banner (banner: ' '..'_' only) */

/* colours */
#define C_BG       0x0C0A0C
#define C_RED      0xB3261E
#define C_SEL      0x8E1F1A
#define C_EDGE     0xE0453B
#define C_HEAD     0xE8E2D8
#define C_SUB      0xD0463C
#define C_TEXT     0xC8C2B8
#define C_ITEM     0xD8D2C8
#define C_HELP     0xA8A29A
#define C_DIM      0x8A847C
#define C_ARROW    0x6A6460

static const char *status_tag[4] = { "", "PARTIAL", "NOT YET", "PC ONLY" };
static const uint32_t status_rgb[4] = { 0x7FB069, 0xE0A040, 0xE0453B, 0x8A847C };

/* ---- drawing --------------------------------------------------------------- */

static SDL_Window *ui_w;
static SDL_Renderer *ui_r;
static struct { SDL_Texture *tex; stbtt_packedchar ch[NSIZES][96]; float scale; } uf;
enum { ATLAS = 2048 };

static int ui_font_init(float scale)
{
    unsigned char *bmp = calloc(ATLAS, ATLAS);
    uint32_t *rgba;
    stbtt_pack_context pc;
    int s, i;
    if (!bmp || !stbtt_PackBegin(&pc, bmp, ATLAS, ATLAS, 0, 1, NULL)) { free(bmp); return 0; }
    stbtt_PackSetOversampling(&pc, 2, 2);
    for (s = 0; s < NSIZES; s++)
        stbtt_PackFontRange(&pc, courierprime_ttf, 0, ui_px[s] * scale, 32, s == 3 ? 64 : 95, uf.ch[s]);
    stbtt_PackEnd(&pc);
    rgba = malloc((size_t)ATLAS * ATLAS * 4);
    if (!rgba) { free(bmp); return 0; }
    for (i = 0; i < ATLAS * ATLAS; i++) rgba[i] = 0x00FFFFFFu | (uint32_t)bmp[i] << 24;
    uf.tex = SDL_CreateTexture(ui_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, ATLAS, ATLAS);
    if (uf.tex) {
        SDL_UpdateTexture(uf.tex, NULL, rgba, ATLAS * 4);
        SDL_SetTextureBlendMode(uf.tex, SDL_BLENDMODE_BLEND);
    }
    free(rgba); free(bmp);
    uf.scale = scale;
    return uf.tex != NULL;
}

static float ui_text_w(int sz, const char *s)
{
    float w = 0;
    for (; *s; s++) { int c = (unsigned char)*s; if (c < 32 || c > 126) c = '?'; w += uf.ch[sz][c - 32].xadvance; }
    return w / uf.scale;
}

/* align: 0 left, 1 centre, 2 right; y = baseline */
static void ui_text(int sz, float x, float y, int align, uint32_t rgb, int alpha, const char *s)
{
    float px, py;
    if (align) x -= ui_text_w(sz, s) * (align == 1 ? 0.5f : 1.0f);
    px = x * uf.scale; py = y * uf.scale;
    SDL_SetTextureColorMod(uf.tex, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
    SDL_SetTextureAlphaMod(uf.tex, (Uint8)alpha);
    for (; *s; s++) {
        int c = (unsigned char)*s;
        stbtt_aligned_quad q;
        SDL_Rect src;
        SDL_FRect dst;
        if (c < 32 || c > 126) c = '?';
        if (sz == 3 && c > 95) c = '?';
        stbtt_GetPackedQuad(uf.ch[sz], ATLAS, ATLAS, c - 32, &px, &py, &q, 0);
        src.x = (int)(q.s0 * ATLAS + 0.5f); src.y = (int)(q.t0 * ATLAS + 0.5f);
        src.w = (int)((q.s1 - q.s0) * ATLAS + 0.5f); src.h = (int)((q.t1 - q.t0) * ATLAS + 0.5f);
        dst.x = q.x0 / uf.scale; dst.y = q.y0 / uf.scale;
        dst.w = (q.x1 - q.x0) / uf.scale; dst.h = (q.y1 - q.y0) / uf.scale;
        SDL_RenderCopyF(ui_r, uf.tex, &src, &dst);
    }
}

static void ui_fill(float x, float y, float w, float h, uint32_t rgb, int alpha)
{
    SDL_FRect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    SDL_SetRenderDrawBlendMode(ui_r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ui_r, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, (Uint8)alpha);
    SDL_RenderFillRectF(ui_r, &r);
}

static void ui_frame(float x, float y, float w, float h, uint32_t rgb, int alpha)
{
    ui_fill(x, y, w, 2, rgb, alpha); ui_fill(x, y + h - 2, w, 2, rgb, alpha);
    ui_fill(x, y, 2, h, rgb, alpha); ui_fill(x + w - 2, y, 2, h, rgb, alpha);
}

/* a selected list row: red bar with a bright left edge */
static void ui_selbar(float x, float y, float w, float h, int alpha, uint32_t rgb)
{
    ui_fill(x, y, w, h, rgb, alpha);
    ui_fill(x, y, 4, h, C_EDGE, 255);
}

/* text cut to fit `w`, with "..." */
static void ui_fit(int sz, float w, const char *s, char *out, size_t n)
{
    size_t l;
    snprintf(out, n, "%s", s);
    if (ui_text_w(sz, out) <= w) return;
    for (l = strlen(out); l > 3; l--) {
        out[l - 3] = '.'; out[l - 2] = '.'; out[l - 1] = '.'; out[l] = 0;
        if (ui_text_w(sz, out) <= w) return;
    }
}

/* two help lines split at '\n' */
static void ui_help(float y, const char *help)
{
    char a[160] = "", b[160] = "";
    const char *nl;
    if (!help) return;
    nl = strchr(help, '\n');
    if (nl) { snprintf(a, sizeof a, "%.*s", (int)(nl - help), help); snprintf(b, sizeof b, "%s", nl + 1); }
    else snprintf(a, sizeof a, "%s", help);
    ui_text(0, 60, y, 0, C_HELP, 255, a);
    if (b[0]) ui_text(0, 60, y + 21, 0, C_HELP, 255, b);
}

static void ui_header(const char *title, const char *sub)
{
    ui_text(2, 60, 64, 0, C_HEAD, 255, title);
    if (sub) ui_text(1, 60, 98, 0, C_SUB, 255, sub);
}

static void ui_glow(void)
{
    static SDL_Texture *glow;
    if (!glow) {   /* 64 x 64 radial glow, stretched over the window */
        static uint32_t px[64 * 64];
        int xx, yy;
        for (yy = 0; yy < 64; yy++) for (xx = 0; xx < 64; xx++) {
            float dx = (xx - 31.5f) / 40.f, dy = (yy - 20.f) / 44.f, t = 1 - sqrtf(dx * dx + dy * dy);
            if (t < 0) t = 0;
            px[yy * 64 + xx] = (uint32_t)(t * t * 255) << 24 | 0x3A0F0C;
        }
        glow = SDL_CreateTexture(ui_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 64, 64);
        if (glow) {
            SDL_UpdateTexture(glow, NULL, px, 64 * 4);
            SDL_SetTextureBlendMode(glow, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(glow, SDL_ScaleModeLinear);
        }
    }
    if (glow) SDL_RenderCopy(ui_r, glow, NULL, NULL);
}

/* word-wrap s into lines no wider than w (logical px) at size sz */
enum { LINE_MAX = 120, LINES_MAX = 3000 };
static int wrap(int sz, const char *s, float w, char (*lines)[LINE_MAX], int maxl)
{
    int n = 0;
    char cur[LINE_MAX] = "";
    while (*s && n < maxl) {
        const char *e = s;
        char word[LINE_MAX], trial[2 * LINE_MAX + 2];
        size_t l;
        if (*s == '\r') { s++; continue; }
        if (*s == '\n') { snprintf(lines[n++], LINE_MAX, "%s", cur); cur[0] = 0; s++; continue; }
        while (*e && *e != ' ' && *e != '\n' && *e != '\r') e++;
        l = (size_t)(e - s) < LINE_MAX - 1 ? (size_t)(e - s) : LINE_MAX - 1;
        memcpy(word, s, l); word[l] = 0;
        snprintf(trial, sizeof trial, "%s%s%s", cur, cur[0] ? " " : "", word);
        if (ui_text_w(sz, trial) > w && cur[0]) { snprintf(lines[n++], LINE_MAX, "%s", cur); snprintf(cur, sizeof cur, "%s", word); }
        else snprintf(cur, sizeof cur, "%.*s", LINE_MAX - 1, trial);
        s = e; if (*s == ' ') s++;
    }
    if (cur[0] && n < maxl) snprintf(lines[n++], LINE_MAX, "%s", cur);
    return n;
}

/* ---- messages ------------------------------------------------------------- */

static char msg_text[200];
static int msg_lvl;
static Uint32 msg_until;

void khl_message(int lvl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg_text, sizeof msg_text, fmt, ap);
    va_end(ap);
    msg_lvl = lvl;
    msg_until = SDL_GetTicks() + 4000;
}

SDL_Window *khl_window(void) { return ui_w; }

/* ---- drawing for games (khl.h) ----------------------------------------------- */

SDL_Renderer *khl_renderer(void) { return ui_r; }
void khl_text(int size, float x, float y, int align, uint32_t rgb, const char *s)
{
    if (ui_r && s) ui_text(size < 0 || size > 2 ? 1 : size, x, y, align, rgb, 255, s);
}
float khl_text_width(int size, const char *s) { return ui_r && s ? ui_text_w(size < 0 || size > 2 ? 1 : size, s) : 0; }
void khl_fill(float x, float y, float w, float h, uint32_t rgb, int alpha) { if (ui_r) ui_fill(x, y, w, h, rgb, alpha); }
void khl_selbar(float x, float y, float w, float h) { if (ui_r) ui_selbar(x, y, w, h, 170, C_SEL); }
void khl_header(const char *title, const char *sub) { if (ui_r) ui_header(title, sub); }
void khl_keys_line(const char *keys) { if (ui_r && keys) ui_text(0, 60, 690, 0, C_DIM, 255, keys); }

static void draw_message(void)
{
    if (!msg_text[0] || SDL_TICKS_PASSED(SDL_GetTicks(), msg_until)) return;
    /* a band over the help lines, so it never mixes with other text */
    ui_fill(40, 548, 880, 84, 0x140E0E, 250);
    ui_frame(40, 548, 880, 84, msg_lvl ? C_EDGE : C_ARROW, 255);
    ui_text(1, 480, 598, 1, msg_lvl ? 0xFF8A80 : 0xFFFFFF, 255, msg_text);
}

/* ---- the launcher ---------------------------------------------------------- */

enum { S_HOME = KHL_SCREEN_HOME, S_OPTIONS = KHL_SCREEN_OPTIONS, S_BINDS = KHL_SCREEN_BINDS, S_EXTRAS = KHL_SCREEN_EXTRAS,
       S_READ = KHL_SCREEN_READ, S_LIST = KHL_SCREEN_LIST, S_CUSTOM = KHL_SCREEN_CUSTOM };
enum { B_SAVE, B_DEFAULTS, B_CANCEL, NBTN };
static const char *btn_label[NBTN] = { "Save", "Defaults", "Cancel" };
static const char *btn_help[NBTN] = { "Save these settings.", "Put every setting back to its default (not saved until you press Save).",
                                      "Leave without saving." };
static const float btn_x[NBTN] = { 60, 240, 440 }, btn_w[NBTN] = { 160, 180, 160 };
#define ROW_H 31.0f
#define BTN_Y 672.0f
#define TAB_Y 140.0f

static void row_status(const khl_game *g, const khl_row *r, int *lvl, const char **text)
{
    *lvl = KHL_WORKS; *text = "Works.";
    if (g->status) g->status(r->id, lvl, text);
    if (*lvl < 0 || *lvl > 3) *lvl = KHL_WORKS;
}

/* an extra's name on the Extras screen: "Cutscenes (56)" when it counts its items */
static void extra_label(const khl_extra *x, char *out, size_t n)
{
    if (x->count) snprintf(out, n, "%s (%d)", x->label, x->count());
    else snprintf(out, n, "%s", x->label);
}

static int sdl_pad_dir(const SDL_Event *e, int *dx, int *dy)
{
    /* the left stick as a d-pad for menus (edge-triggered) */
    static int last_x, last_y;
    int v = e->caxis.value, d = v > 20000 ? 1 : v < -20000 ? -1 : 0;
    if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) { if (d != last_x) { last_x = d; if (d) { *dx = d; return 1; } } }
    else if (e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) { if (d != last_y) { last_y = d; if (d) { *dy = d; return 1; } } }
    return 0;
}

int khl_run(const khl_game *g)
{
    SDL_DisplayMode dm;
    SDL_GameController *pad = NULL;
    SDL_Texture *bg = NULL;
    SDL_Rect bg_src = { 0, 0, 0, 0 };
    float dpi = 96, f, sc;
    int ww, wh, ow, oh, i;
    int screen = g->start_screen == KHL_SCREEN_OPTIONS ? S_OPTIONS : S_HOME, result = -1;
    int waiting = 0;                      /* Play chosen, play() said KHL_PLAY_WAIT */
    int home_sel = 0, page = 0, sel = 0, btn = B_SAVE;
    int bsel = 0, btop = 0, bwait = -1, bidx = 0; Uint32 bwait_since = 0;
    int extras_sel = 0, read_top = 0, nlines = 0, read_back = S_EXTRAS;
    int xidx = 0, lsel = 0, ltop = 0;     /* the open list or custom extra; list selection and scroll */
    char read_title_buf[160];
    const char *read_title = "";
    static char lines[LINES_MAX][LINE_MAX];
    const char *keys = getenv("KHL_KEYS"), *shot = getenv("KHL_SHOT");
    float mx = -1, my = -1;
    const int next = g->nextras + 2;      /* + About, Back */

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_GetDisplayDPI(0, &dpi, NULL, NULL);
    f = dpi > 96 ? dpi / 96 : 1;
    ww = (int)(UI_W * f); wh = (int)(UI_H * f);
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && wh > dm.h * 85 / 100) { wh = dm.h * 85 / 100; ww = wh * UI_W / UI_H; }
    ui_w = SDL_CreateWindow(g->window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                            SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!ui_w) { fprintf(stderr, "[launcher] window: %s\n", SDL_GetError()); return 0; }
    ui_r = SDL_CreateRenderer(ui_w, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ui_r) ui_r = SDL_CreateRenderer(ui_w, -1, SDL_RENDERER_SOFTWARE);
    if (!ui_r) { fprintf(stderr, "[launcher] renderer: %s\n", SDL_GetError()); SDL_DestroyWindow(ui_w); ui_w = NULL; return 0; }
    SDL_GetRendererOutputSize(ui_r, &ow, &oh);
    SDL_RenderSetLogicalSize(ui_r, UI_W, UI_H);
    sc = (float)ow / UI_W < (float)oh / UI_H ? (float)ow / UI_W : (float)oh / UI_H;
    if (!ui_font_init(sc < 1 ? 1 : sc)) { SDL_DestroyRenderer(ui_r); SDL_DestroyWindow(ui_w); ui_w = NULL; return 0; }
    if (g->background) {
        bg = g->background(ui_r, &bg_src);
        if (bg && (!bg_src.w || !bg_src.h)) SDL_QueryTexture(bg, NULL, NULL, &bg_src.w, &bg_src.h);
    }
    if (g->load) g->load();

    for (i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }

    while (result < 0) {
        int act = 0, dx = 0, dy = 0, cancel = 0, click = 0, tab = 0, del = 0, wheel = 0;
        float drag_x = 0, drag_y = 0;
        const khl_page *pg = &g->pages[page];
        const khl_binds *bd = g->nbinds ? &g->binds[bidx] : NULL;
        SDL_Event e;

        /* ---- input ---- */
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { result = 0; break; }
            if (screen == S_BINDS && bwait >= 0) {            /* waiting for the new key or button */
                int r = 0, esc = 0;
                if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                    SDL_Scancode s = e.key.keysym.scancode;
                    if (s == SDL_SCANCODE_ESCAPE || s == SDL_SCANCODE_AC_BACK) esc = 1;
                    else if (s == SDL_SCANCODE_DELETE && bd->clear) { bd->clear(bwait); r = 1; }
                    else if (bd->capture == KHL_CAPTURE_KEY) r = bd->set(bwait, &e);
                } else if (bd->capture == KHL_CAPTURE_PAD &&
                           (e.type == SDL_CONTROLLERBUTTONDOWN ||
                            (e.type == SDL_CONTROLLERAXISMOTION && (e.caxis.value > 16384 || e.caxis.value < -16384)))) {
                    r = bd->set(bwait, &e);
                } else if (bd->capture == KHL_CAPTURE_KEY && e.type == SDL_CONTROLLERBUTTONDOWN &&
                           e.cbutton.button == SDL_CONTROLLER_BUTTON_B) esc = 1;
                if (r < 0) khl_message(KHL_NOT_YET, "That one can't be used here.");
                if (r || esc) bwait = -1;
                continue;
            }
            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_UP) dy = -1; else if (k == SDLK_DOWN) dy = 1;
                else if (k == SDLK_LEFT) dx = -1; else if (k == SDLK_RIGHT) dx = 1;
                else if (k == SDLK_PAGEUP) dy = -10; else if (k == SDLK_PAGEDOWN) dy = 10;
                else if (k == SDLK_TAB) tab = (e.key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
                else if (k == SDLK_DELETE) del = 1;
                else if (k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_KP_ENTER) { if (!e.key.repeat) act = 1; }
                else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE || k == SDLK_AC_BACK) cancel = 1;
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: dy = -1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: dy = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: dx = -1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dx = 1; break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: tab = -1; break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: tab = 1; break;
                case SDL_CONTROLLER_BUTTON_A: act = 1; break;
                case SDL_CONTROLLER_BUTTON_B: cancel = 1; break;
                case SDL_CONTROLLER_BUTTON_X: del = 1; break;
                case SDL_CONTROLLER_BUTTON_START: if (screen == S_HOME) { act = 1; home_sel = 0; } break;
                }
            } else if (e.type == SDL_CONTROLLERAXISMOTION) sdl_pad_dir(&e, &dx, &dy);
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
            else if (e.type == SDL_MOUSEMOTION) {
                mx = (float)e.motion.x; my = (float)e.motion.y;
                if (e.motion.state & SDL_BUTTON_LMASK) { drag_x += (float)e.motion.xrel; drag_y += (float)e.motion.yrel; }
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) { mx = (float)e.button.x; my = (float)e.button.y; click = 1; }
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) cancel = 1;
            else if (e.type == SDL_MOUSEWHEEL && e.wheel.y) { dy = e.wheel.y > 0 ? -3 : 3; wheel = dy; }
        }
        if (result >= 0) break;
        if (keys && *keys) {                                   /* scripted keys (test hook): one per frame */
            const char *c = strchr(keys, ',');
            size_t l = c ? (size_t)(c - keys) : strlen(keys);
            if (l > 4 && !strncmp(keys, "key:", 4)) {
                char nm[32];
                SDL_Event fake;
                snprintf(nm, sizeof nm, "%.*s", (int)(l - 4), keys + 4);
                memset(&fake, 0, sizeof fake);
                fake.type = SDL_KEYDOWN;
                fake.key.keysym.scancode = SDL_GetScancodeFromName(nm);
                fake.key.keysym.sym = SDL_GetKeyFromScancode(fake.key.keysym.scancode);
                SDL_PushEvent(&fake);
            } else if (!strncmp(keys, "up", l)) dy = -1; else if (!strncmp(keys, "down", l)) dy = 1;
            else if (!strncmp(keys, "left", l)) dx = -1; else if (!strncmp(keys, "right", l)) dx = 1;
            else if (!strncmp(keys, "enter", l)) act = 1; else if (!strncmp(keys, "esc", l)) cancel = 1;
            else if (!strncmp(keys, "tab", l)) tab = 1; else if (!strncmp(keys, "del", l)) del = 1;
            keys = c ? c + 1 : keys + l;
        }

        /* ---- background ---- */
        SDL_SetRenderDrawColor(ui_r, C_BG >> 16 & 255, C_BG >> 8 & 255, C_BG & 255, 255);
        SDL_RenderClear(ui_r);
        if (bg) {
            SDL_SetTextureColorMod(bg, 255, 255, 255); SDL_SetTextureAlphaMod(bg, 255);
            SDL_RenderCopy(ui_r, bg, &bg_src, NULL);
            ui_fill(0, 0, UI_W, UI_H, 0x0A080A, screen == S_HOME ? 120 : 215);
        } else {
            ui_glow();
        }

        /* ---- screens ---- */
        if (waiting) {                         /* Play is waiting for the game to be ready */
            int r = g->play ? g->play() : KHL_PLAY_GO;
            if (r == KHL_PLAY_GO) { result = 1; break; }
            if (r != KHL_PLAY_WAIT) waiting = 0;
        }
        if (screen == S_HOME) {
            static const char *items[4] = { "Play", "Options", "Extras", "Quit" };
            static const float HY = 392;
            if (!bg) {
                ui_text(3, 484, 236, 1, 0x000000, 160, g->banner);
                ui_text(3, 480, 230, 1, C_RED, 255, g->banner);
                ui_text(2, 480, 292, 1, C_ITEM, 255, g->subtitle);
            }
            for (i = 0; i < 4; i++)
                if (mx >= 0 && my >= HY + i * 48 - 34 && my < HY + i * 48 + 10 && mx > 340 && mx < 620) { home_sel = i; if (click) act = 1; }
            if (dy) home_sel = (home_sel + (dy > 0 ? 1 : 3)) % 4;
            if (cancel) { if (waiting) waiting = 0; else result = 0; }
            if (act) {
                if (home_sel == 0) {
                    if (g->save && !g->save()) khl_message(KHL_NOT_YET, "Could not save the settings file.");
                    else {
                        int r = g->play ? g->play() : KHL_PLAY_GO;
                        if (r == KHL_PLAY_GO) result = 1;
                        else if (r == KHL_PLAY_WAIT) waiting = 1;
                    }
                } else if (home_sel == 1) { screen = S_OPTIONS; page = 0; sel = 0; btn = B_SAVE; }
                else if (home_sel == 2) { screen = S_EXTRAS; extras_sel = 0; }
                else result = 0;
                act = 0;
            }
            if (screen == S_HOME) {
                for (i = 0; i < 4; i++) {
                    float y = HY + i * 48;
                    int on = home_sel == i;
                    if (on) { ui_fill(340, y - 34, 280, 44, C_SEL, 210); ui_frame(340, y - 34, 280, 44, C_EDGE, 255); }
                    ui_text(2, 480, y, 1, on ? 0xFFFFFF : C_ITEM, 255, items[i]);
                }
                ui_text(0, 480, 700, 1, C_DIM, 255, g->credit);
            }
        } else if (screen == S_OPTIONS) {
            /* sel: -1 the tab bar (several pages only), 0..nrows-1 a row, nrows the buttons */
            const int tabs = g->npages > 1;
            const float row_y0 = tabs ? 196.0f : 140.0f;
            int nrows = pg->nrows;
            if (tab && tabs) { page = (page + tab + g->npages) % g->npages; pg = &g->pages[page]; nrows = pg->nrows; if (sel >= 0) sel = 0; }
            if (mx >= 0) {
                if (tabs) {
                    float x = 60;
                    for (i = 0; i < g->npages; i++) {
                        float w = ui_text_w(1, g->pages[i].name) + 36;
                        if (my >= TAB_Y - 26 && my < TAB_Y + 12 && mx >= x && mx < x + w) {
                            if (click) { page = i; pg = &g->pages[page]; nrows = pg->nrows; sel = -1; click = 0; }
                        }
                        x += w + 10;
                    }
                }
                for (i = 0; i < nrows; i++)
                    if (my >= row_y0 + i * ROW_H - 23 && my < row_y0 + i * ROW_H + 8 && mx >= 40 && mx < 920) {
                        sel = i;
                        if (click) {
                            if (pg->rows[i].kind == KHL_ROW_CHOICE) { if (g->change) g->change(pg->rows[i].id, mx < 680 ? -1 : 1); }
                            else act = 1;
                            click = 0;
                        }
                    }
                for (i = 0; i < NBTN; i++)
                    if (my >= BTN_Y - 34 && my < BTN_Y + 14 && mx >= btn_x[i] && mx < btn_x[i] + btn_w[i]) { sel = nrows; btn = i; if (click) act = 1; }
            }
            if (dy) {
                int lo = tabs ? -1 : 0;
                sel += dy > 0 ? 1 : -1;
                if (sel < lo) sel = nrows;
                if (sel > nrows) sel = lo;
            }
            if (dx) {
                if (sel < 0) { page = (page + dx + g->npages) % g->npages; pg = &g->pages[page]; nrows = pg->nrows; }
                else if (sel < nrows) { if (pg->rows[sel].kind == KHL_ROW_CHOICE && g->change) g->change(pg->rows[sel].id, dx); }
                else { btn += dx; if (btn < 0) btn = NBTN - 1; if (btn >= NBTN) btn = 0; }
            }
            if (act) {
                if (sel < 0) { sel = 0; }
                else if (sel < nrows) {
                    const khl_row *r = &pg->rows[sel];
                    if (r->kind == KHL_ROW_CHOICE) { if (g->change) g->change(r->id, 1); }
                    else if (r->kind == KHL_ROW_ACTION) { if (g->activate) g->activate(r->id); }
                    else if (r->binds >= 0 && r->binds < g->nbinds) { screen = S_BINDS; bidx = r->binds; bsel = 0; btop = 0; bwait = -1; }
                } else if (btn == B_SAVE) {
                    if (g->save && !g->save()) khl_message(KHL_NOT_YET, "Could not save the settings file.");
                    else { khl_message(KHL_WORKS, "Settings saved."); screen = S_HOME; }
                } else if (btn == B_DEFAULTS) {
                    if (g->defaults) g->defaults();
                    khl_message(KHL_WORKS, "Defaults restored. Press Save to keep them.");
                } else { if (g->load) g->load(); screen = S_HOME; }
            }
            if (cancel) { if (g->load) g->load(); screen = S_HOME; }
            if (screen == S_OPTIONS) {
                ui_header(g->window_title, "Options");
                if (tabs) {
                    float x = 60;
                    for (i = 0; i < g->npages; i++) {
                        float w = ui_text_w(1, g->pages[i].name) + 36;
                        int cur = i == page;
                        if (cur) ui_fill(x, TAB_Y - 26, w, 38, C_SEL, sel < 0 ? 230 : 150);
                        ui_frame(x, TAB_Y - 26, w, 38, cur ? (sel < 0 ? 0xFFFFFF : C_EDGE) : C_ARROW, 255);
                        ui_text(1, x + w / 2, TAB_Y, 1, cur ? 0xFFFFFF : C_TEXT, 255, g->pages[i].name);
                        x += w + 10;
                    }
                }
                for (i = 0; i < nrows; i++) {
                    const khl_row *r = &pg->rows[i];
                    float y = row_y0 + i * ROW_H;
                    int on = sel == i, lvl;
                    const char *st;
                    char v[160], vf[160];
                    row_status(g, r, &lvl, &st);
                    if (on) ui_selbar(40, y - 23, 880, 30, 150, C_SEL);
                    ui_text(1, 64, y, 0, on ? 0xFFFFFF : C_TEXT, 255, r->label);
                    if (lvl) ui_text(0, 64 + ui_text_w(1, r->label) + 12, y - 2, 0, status_rgb[lvl], 255, status_tag[lvl]);
                    v[0] = 0;
                    if (g->value) g->value(r->id, v, sizeof v);
                    if (r->kind == KHL_ROW_CHOICE) {
                        ui_fit(1, 400, v, vf, sizeof vf);
                        ui_text(1, 680, y, 1, on ? 0xFFFFFF : C_HEAD, 255, vf);
                        ui_text(1, 470, y, 1, on ? 0xFFFFFF : C_ARROW, 255, "<");
                        ui_text(1, 904, y, 2, on ? 0xFFFFFF : C_ARROW, 255, ">");
                    } else {
                        ui_fit(1, 440, v[0] ? v : (r->kind == KHL_ROW_BINDS ? "Enter to change" : ""), vf, sizeof vf);
                        ui_text(1, 680, y, 1, on ? 0xFFFFFF : C_HEAD, 255, vf);
                    }
                }
                if (sel >= 0 && sel < nrows) {
                    int lvl; const char *st;
                    row_status(g, &pg->rows[sel], &lvl, &st);
                    ui_help(576, pg->rows[sel].help);
                    ui_text(0, 60, 620, 0, status_rgb[lvl], 255, st);
                } else if (sel < 0) ui_help(576, "Left / right (or Tab, LB / RB) switches the page.");
                else ui_help(576, btn_help[btn]);
                for (i = 0; i < NBTN; i++) {
                    int on = sel == nrows && btn == i;
                    if (on) ui_fill(btn_x[i], BTN_Y - 34, btn_w[i], 48, C_SEL, 200);
                    ui_frame(btn_x[i], BTN_Y - 34, btn_w[i], 48, on ? C_EDGE : C_ARROW, 255);
                    ui_text(1, btn_x[i] + btn_w[i] / 2, BTN_Y, 1, 0xFFFFFF, 255, btn_label[i]);
                }
                ui_text(0, 900, BTN_Y - 4, 2, C_DIM, 255, "Esc / B: back");
            }
        } else if (screen == S_BINDS && bd) {
            enum { VIS = 13 };
            const int krows = bd->n + 2;      /* + reset, back */
            const float y0 = 150;
            if (bwait >= 0 && bd->capture == KHL_CAPTURE_PAD && SDL_TICKS_PASSED(SDL_GetTicks(), bwait_since + 6000)) bwait = -1;
            if (bwait < 0) {
                if (mx >= 0)
                    for (i = 0; i < VIS && btop + i < krows; i++)
                        if (my >= y0 + i * ROW_H - 23 && my < y0 + i * ROW_H + 8 && mx >= 40 && mx < 920) { bsel = btop + i; if (click) act = 1; }
                if (dy) {
                    int step = dy > 1 || dy < -1 ? dy : (dy > 0 ? 1 : -1);
                    bsel += step;
                    if (step == 1 || step == -1) { if (bsel < 0) bsel = krows - 1; if (bsel >= krows) bsel = 0; }
                    else { if (bsel < 0) bsel = 0; if (bsel >= krows) bsel = krows - 1; }
                }
                if (dx && bsel < bd->n && bd->change) bd->change(bsel, dx);
                if (del && bsel < bd->n && bd->clear) bd->clear(bsel);
                if (act) {
                    if (bsel < bd->n) { bwait = bsel; bwait_since = SDL_GetTicks(); }
                    else if (bsel == bd->n) { if (bd->reset) bd->reset(); khl_message(KHL_WORKS, "Back to the defaults. Save on the Options screen to keep them."); }
                    else screen = S_OPTIONS;
                }
                if (cancel) screen = S_OPTIONS;
            }
            if (bsel < btop) btop = bsel;
            if (bsel >= btop + VIS) btop = bsel - VIS + 1;
            if (screen == S_BINDS) {
                ui_header(g->window_title, bd->title);
                for (i = 0; i < VIS && btop + i < krows; i++) {
                    int r = btop + i, on = bsel == r;
                    float y = y0 + i * ROW_H;
                    if (on) ui_selbar(40, y - 23, 880, 30, 150, bwait == r ? C_RED : C_SEL);
                    if (r < bd->n) {
                        char v[160], vf[160];
                        v[0] = 0;
                        bd->value(r, v, sizeof v);
                        ui_text(1, 64, y, 0, on ? 0xFFFFFF : C_TEXT, 255, bd->label(r));
                        ui_fit(1, 440, bwait == r ? (bd->capture == KHL_CAPTURE_PAD ? "press a button..." : "press a key...") : v, vf, sizeof vf);
                        ui_text(1, 680, y, 1, on ? 0xFFFFFF : C_HEAD, 255, vf);
                    } else ui_text(1, 64, y, 0, on ? 0xFFFFFF : C_TEXT, 255, r == bd->n ? "Reset to defaults" : "Back");
                }
                if (btop > 0) ui_text(1, 904, y0 - 30, 2, C_DIM, 255, "^");
                if (btop + VIS < krows) ui_text(1, 904, y0 + VIS * ROW_H - 12, 2, C_DIM, 255, "v");
                if (bwait >= 0) {
                    char h[200];
                    snprintf(h, sizeof h, "%s Esc keeps the old one.%s",
                             bd->capture == KHL_CAPTURE_PAD ? "Press the controller button for this." : "Press the new key.",
                             bd->clear ? "\nDelete clears it." : "");
                    ui_help(576, h);
                } else ui_help(576, bd->hint);
                {   /* the keys this screen takes */
                    char k[200];
                    snprintf(k, sizeof k, "Enter / A: change     %s%sEsc / B: back", bd->change ? "Left / right: pick     " : "",
                             bd->clear ? "Del / X: clear     " : "");
                    ui_text(0, 60, 690, 0, C_DIM, 255, k);
                }
            }
        } else if (screen == S_EXTRAS) {
            const float y0 = 250;
            for (i = 0; i < next; i++)
                if (mx >= 0 && my >= y0 + i * 56 - 36 && my < y0 + i * 56 + 14 && mx > 40 && mx < 600) { extras_sel = i; if (click) act = 1; }
            if (dy) extras_sel = (extras_sel + (dy > 0 ? 1 : next - 1)) % next;
            if (cancel || (act && extras_sel == next - 1)) { screen = S_HOME; act = 0; }
            if (act) {
                const char *t = NULL;
                const khl_extra *x = extras_sel < g->nextras ? &g->extras[extras_sel] : NULL;
                if (x && x->count && x->count() <= 0) khl_message(KHL_NOT_YET, "Nothing to show.");
                else if (x && x->frame) {                              /* a screen of the game's own */
                    xidx = extras_sel; screen = S_CUSTOM;
                    if (x->enter) x->enter();
                } else if (x && x->count) {                            /* a list */
                    xidx = extras_sel; screen = S_LIST; lsel = 0; ltop = 0;
                } else {
                    if (x) { t = x->text ? x->text() : NULL; read_title = x->label; }
                    else { t = g->about; read_title = "About this port"; }
                    if (t && *t) { screen = S_READ; read_top = 0; read_back = S_EXTRAS; nlines = wrap(1, t, 820, lines, LINES_MAX); }
                    else khl_message(KHL_NOT_YET, "Nothing to show.");
                }
                act = 0;
            }
            if (screen == S_EXTRAS) {
                ui_header(g->window_title, "Extras");
                for (i = 0; i < next; i++) {
                    float y = y0 + i * 56;
                    int on = extras_sel == i, lvl = i < g->nextras ? g->extras[i].status : 0;
                    char lbl[160];
                    if (i < g->nextras) extra_label(&g->extras[i], lbl, sizeof lbl);
                    else snprintf(lbl, sizeof lbl, "%s", i == next - 2 ? "About this port" : "Back");
                    if (lvl < 0 || lvl > 3) lvl = 0;
                    if (on) ui_selbar(40, y - 36, 560, 48, 170, C_SEL);
                    ui_text(2, 64, y, 0, on ? 0xFFFFFF : C_ITEM, 255, lbl);
                    if (lvl) ui_text(0, 64 + ui_text_w(2, lbl) + 14, y - 4, 0, status_rgb[lvl], 255, status_tag[lvl]);
                }
                if (extras_sel < g->nextras) {
                    const khl_extra *x = &g->extras[extras_sel];
                    int lvl = x->status < 0 || x->status > 3 ? 0 : x->status;
                    ui_help(576, x->help);
                    if (x->status_text) ui_text(0, 60, 620, 0, status_rgb[lvl], 255, x->status_text);
                } else ui_help(576, extras_sel == next - 2 ? "What this is, and credits." : "Back to the main screen.");
            }
        } else if (screen == S_LIST) {
            enum { VIS = 14 };
            const khl_extra *x = &g->extras[xidx];
            const int n = x->count(), cols = x->columns > 1 ? x->columns : 1, rows = (n + cols - 1) / cols;
            const float y0 = 150, rh = 36, cw = 880.0f / cols;
            int r, c;
            if (mx >= 0)
                for (r = 0; r < VIS && ltop + r < rows; r++) for (c = 0; c < cols; c++) {
                    int k = (ltop + r) * cols + c;
                    if (k >= n) break;
                    if (my >= y0 + r * rh - 26 && my < y0 + r * rh + 10 && mx >= 40 + c * cw && mx < 40 + (c + 1) * cw) { lsel = k; if (click) act = 1; }
                }
            if (dy) {      /* one row, three for the wheel, a page for Page Up / Down */
                int step = dy == 1 || dy == -1 || dy == 3 || dy == -3 ? dy : (dy > 0 ? VIS : -VIS);
                lsel += step * cols;
            }
            if (dx && cols > 1) lsel += dx;
            if (lsel >= n) lsel = n - 1;
            if (lsel < 0) lsel = 0;
            if (lsel / cols < ltop) ltop = lsel / cols;
            if (lsel / cols >= ltop + VIS) ltop = lsel / cols - VIS + 1;
            if (cancel) screen = S_EXTRAS;
            else if (act && n > 0) {
                const char *t = x->item_text ? x->item_text(lsel) : NULL;
                if (t && *t) {
                    read_title_buf[0] = 0;
                    if (x->item) x->item(lsel, read_title_buf, sizeof read_title_buf);
                    read_title = read_title_buf; read_back = S_LIST; read_top = 0;
                    nlines = wrap(1, t, 820, lines, LINES_MAX); screen = S_READ;
                } else if (x->item_run) {
                    x->item_run(lsel);
                    mx = my = -1;        /* don't let the pointer's old place pick another item */
                }
            }
            if (screen == S_LIST) {
                ui_header(x->label, x->list_sub);
                for (r = 0; r < VIS && ltop + r < rows; r++) for (c = 0; c < cols; c++) {
                    int k = (ltop + r) * cols + c, on = k == lsel;
                    float y = y0 + r * rh, xx = 40 + c * cw;
                    char v[160], vf[160];
                    if (k >= n) break;
                    v[0] = 0;
                    if (x->item) x->item(k, v, sizeof v);
                    if (on) ui_selbar(xx, y - 26, cw - (cols > 1 ? 12 : 0), 34, 170, C_SEL);
                    ui_fit(1, cw - 48, v, vf, sizeof vf);
                    ui_text(1, xx + 24, y, 0, on ? 0xFFFFFF : C_ITEM, 255, vf);
                }
                if (ltop > 0) ui_text(1, 904, y0 - 30, 2, C_DIM, 255, "^");
                if (ltop + VIS < rows) ui_text(1, 904, y0 + VIS * rh - 12, 2, C_DIM, 255, "v");
                ui_text(0, 60, 690, 0, C_DIM, 255, x->list_keys ? x->list_keys : "Enter / A: open     Esc / B: back");
            }
        } else if (screen == S_CUSTOM) {
            khl_input in;
            memset(&in, 0, sizeof in);
            in.dx = dx; in.dy = dy; in.wheel = wheel; in.act = act; in.cancel = cancel; in.del = del; in.tab = tab;
            in.click = click; in.mx = mx; in.my = my; in.drag_x = drag_x; in.drag_y = drag_y; in.pad = pad;
            if (g->extras[xidx].frame(&in)) { screen = S_EXTRAS; mx = my = -1; }
        } else if (screen == S_READ) {
            enum { VIS = 16 };
            int maxtop = nlines > VIS ? nlines - VIS : 0;
            if (dy) read_top += dy;
            if (read_top > maxtop) read_top = maxtop;
            if (read_top < 0) read_top = 0;
            if (cancel || act) screen = read_back;
            if (screen == S_READ) {
                ui_header(g->window_title, read_title);
                for (i = 0; i < VIS && read_top + i < nlines; i++) ui_text(1, 70, 160 + i * 31, 0, C_ITEM, 255, lines[read_top + i]);
                if (read_top > 0) ui_text(1, 904, 140, 2, C_DIM, 255, "^");
                if (read_top < maxtop) ui_text(1, 904, 160 + VIS * 31 - 12, 2, C_DIM, 255, "v");
                ui_text(0, 60, 690, 0, C_DIM, 255, "Up / down: scroll     Esc / B: back");
            }
        }

        if (g->overlay) {
            int id = -1;
            if (screen == S_OPTIONS && sel >= 0 && sel < g->pages[page].nrows) id = g->pages[page].rows[sel].id;
            g->overlay(screen, page, id, waiting);
        }
        draw_message();

        if (shot && !(keys && *keys)) {     /* test hook: screenshot then close */
            static int frames;
            if (++frames > 2) {
                int w, h;
                SDL_Surface *s;
                SDL_GetRendererOutputSize(ui_r, &w, &h);
                s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
                if (s && SDL_RenderReadPixels(ui_r, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) SDL_SaveBMP(s, shot);
                if (s) SDL_FreeSurface(s);
                result = 0;
            }
        }
        SDL_RenderPresent(ui_r);
        if (!(SDL_GetWindowFlags(ui_w) & SDL_WINDOW_SHOWN) || shot) SDL_Delay(16);
    }

    if (pad) SDL_GameControllerClose(pad);
    if (bg) SDL_DestroyTexture(bg);
    SDL_DestroyTexture(uf.tex); uf.tex = NULL;
    SDL_DestroyRenderer(ui_r); ui_r = NULL;
    SDL_DestroyWindow(ui_w); ui_w = NULL;
    return result == 1;
}
