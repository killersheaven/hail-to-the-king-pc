/* Options window, shown before the game when the program is started with
 * --options (options.bat). #included by platform_sdl.c after the settings.ini
 * code, so it edits the same `cfg` and saves with cfg_save().
 *
 * Drawn with SDL's 2D renderer and stb_truetype (Courier Prime, embedded), in a
 * 960x720 logical canvas scaled to the window. Keyboard, mouse and game
 * controller all work: up/down picks a row, left/right changes it, Enter/A
 * activates a button, Esc/B cancels.
 *
 * Test hooks: DC_OPTIONS_KEYS="down,down,right,enter" feeds keys, then
 * DC_OPTIONS_SHOT=file.png saves a screenshot and closes the window.
 */
#include <ctype.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#include "third_party/courierprime_ttf.h"
#include "third_party/stb_image.h"

int png_write(const char *path, const uint8_t *rgba, int w, int h, int bottom_up);

enum { UI_W = 960, UI_H = 720, NSIZES = 4 };
static const float ui_px[NSIZES] = { 19, 25, 40, 96 };  /* small, normal, title, banner (banner: ' '..'_' only) */
typedef struct { SDL_Texture *tex; stbtt_packedchar ch[NSIZES][96]; float scale; } ui_font;
static SDL_Renderer *ui_r;
static ui_font uf;

static int ui_font_init(float scale) {
    enum { AW = 2048, AH = 2048 };
    unsigned char *bmp = calloc(AW, AH);
    stbtt_pack_context pc;
    if (!stbtt_PackBegin(&pc, bmp, AW, AH, 0, 1, NULL)) return 0;
    stbtt_PackSetOversampling(&pc, 2, 2);
    for (int s = 0; s < NSIZES; s++)
        stbtt_PackFontRange(&pc, courierprime_ttf, 0, ui_px[s] * scale, 32, s == 3 ? 64 : 95, uf.ch[s]);
    stbtt_PackEnd(&pc);
    uint32_t *rgba = malloc((size_t)AW * AH * 4);
    for (int i = 0; i < AW * AH; i++) rgba[i] = 0x00FFFFFFu | (uint32_t)bmp[i] << 24;
    uf.tex = SDL_CreateTexture(ui_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, AW, AH);
    SDL_UpdateTexture(uf.tex, NULL, rgba, AW * 4);
    SDL_SetTextureBlendMode(uf.tex, SDL_BLENDMODE_BLEND);
    free(rgba); free(bmp);
    uf.scale = scale;
    return 1;
}
static float ui_text_w(int sz, const char *s) {
    float w = 0;
    for (; *s; s++) { int c = (unsigned char)*s; if (c < 32 || c > 126) c = '?'; w += uf.ch[sz][c - 32].xadvance; }
    return w / uf.scale;
}
/* align: 0 left, 1 centre, 2 right; y = baseline */
static void ui_text(int sz, float x, float y, int align, uint32_t rgb, int alpha, const char *s) {
    if (align) x -= ui_text_w(sz, s) * (align == 1 ? 0.5f : 1.0f);
    float px = x * uf.scale, py = y * uf.scale;
    SDL_SetTextureColorMod(uf.tex, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
    SDL_SetTextureAlphaMod(uf.tex, (Uint8)alpha);
    for (; *s; s++) {
        int c = (unsigned char)*s; if (c < 32 || c > 126) c = '?';
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(uf.ch[sz], 1024, 1024, c - 32, &px, &py, &q, 0);
        SDL_Rect src = { (int)(q.s0 * 1024 + 0.5f), (int)(q.t0 * 1024 + 0.5f), (int)((q.s1 - q.s0) * 1024 + 0.5f), (int)((q.t1 - q.t0) * 1024 + 0.5f) };
        SDL_FRect dst = { q.x0 / uf.scale, q.y0 / uf.scale, (q.x1 - q.x0) / uf.scale, (q.y1 - q.y0) / uf.scale };
        SDL_RenderCopyF(ui_r, uf.tex, &src, &dst);
    }
}
static void ui_fill(float x, float y, float w, float h, uint32_t rgb, int alpha) {
    SDL_SetRenderDrawBlendMode(ui_r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ui_r, rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, (Uint8)alpha);
    SDL_FRect r = { x, y, w, h }; SDL_RenderFillRectF(ui_r, &r);
}
static void ui_frame(float x, float y, float w, float h, uint32_t rgb, int alpha) {
    ui_fill(x, y, w, 2, rgb, alpha); ui_fill(x, y + h - 2, w, 2, rgb, alpha);
    ui_fill(x, y, 2, h, rgb, alpha); ui_fill(x + w - 2, y, 2, h, rgb, alpha);
}
/* a PNG from the textures folder as a texture; *w,*h = its size */
static SDL_Texture *ui_load_png(const char *path, int *w, int *h) {
    int n; unsigned char *px = stbi_load(path, w, h, &n, 4);
    if (!px) return NULL;
    SDL_Texture *t = SDL_CreateTexture(ui_r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, *w, *h);
    SDL_UpdateTexture(t, NULL, px, *w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    stbi_image_free(px);
    return t;
}

/* ---- the rows ---- */
enum { R_DISPLAY, R_MONITOR, R_WINDOW, R_RENDER, R_ASPECT, R_FILTER, R_HD, R_UI, R_PROMPTS,
       R_KEYS, R_IMMORTAL, R_AMMO, R_FUEL, R_BLADE, NROWS };
static const char *row_label[NROWS] = { "Display", "Monitor", "Window size", "Render resolution", "Aspect ratio",
                                        "Texture filtering", "HD art", "Menu sharpening", "Button prompts", "Keyboard controls",
                                        "Cheat: Immortality", "Cheat: Unlimited ammo", "Cheat: Unlimited fuel", "Cheat: Damascene blade" };
static const char *row_help[NROWS][2] = {
    { "Fullscreen uses your monitor's own resolution.", "Windowed opens a resizable window (F11 switches in game)." },
    { "Which monitor to use for fullscreen.", "" },
    { "Size of the window when not fullscreen.", "You can also resize the window by dragging its edges." },
    { "Internal resolution the game is drawn at.", "Auto matches your screen, which is the sharpest." },
    { "4:3 keeps the original shape with black bars on wide", "screens. Stretch fills the screen (wider image)." },
    { "Smooth is the original look. Sharp shows the game's", "textures as unfiltered pixels." },
    { "High-resolution replacements for the game's own images", "(textures folder). Off shows the original images." },
    { "Sharpens menu and HUD art at high resolutions when no", "HD image replaces it. Text only is recommended." },
    { "Which controller's buttons the menus show. Buttons are", "matched by position: the bottom button is the 'A' button." },
    { "Choose which keys act as the Dreamcast pad's buttons.", "Press Enter to change them." },
    { "Testing aid: Ash's health stays full.", "" },
    { "Testing aid: Knowby's pistol always has 999 rounds.", "" },
    { "Testing aid: the chainsaw never runs out of fuel.", "" },
    { "Testing aid: the chainsaw has the Damascene Steel blade", "from the start." } };
/* what each option does in this release: 0 works, 1 partly, 2 not yet */
static const struct { int lvl; const char *text; } row_status[NROWS] = {
    { 0, "Works." }, { 0, "Works." }, { 0, "Works." }, { 0, "Works." }, { 0, "Works." }, { 0, "Works." },
    { 0, "Works: HD font and legal screen. The title screen keeps its original art." },
    { 0, "Works." },
    { 0, "Works." },
    { 0, "Works." },
    { 0, "Works (testing aid)." }, { 0, "Works (testing aid)." }, { 0, "Works (testing aid)." }, { 0, "Works (testing aid)." } };
static const struct { int lvl; const char *text; } ex_status[5] = {
    { 0, "Works." },
    { 1, "Partly: the 56 Sofdec movies play; the 62 AVI movies not yet." },
    { 1, "Partly: 35 characters, rest pose only (no animations yet)." },
    { 0, "" }, { 0, "" } };
static const char *status_tag[3] = { "", "PARTIAL", "NOT YET" };
static const uint32_t status_rgb[3] = { 0x7FB069, 0xE0A040, 0xE0453B };
static const char *disp_names[2] = { "Fullscreen", "Windowed" };
static const char *aspect_names[2] = { "4:3 (original)", "Stretch to fill" };
static const char *filter_names[2] = { "Smooth (original)", "Sharp pixels" };
static const char *onoff_names[2] = { "On", "Off" };
static const char *ui_names[3] = { "Off", "Text only", "All menu art" };
static const char *prompt_label[4] = { "Dreamcast", "Xbox", "PlayStation", "Nintendo Switch" };
static const int win_sizes[][2] = { {960, 720}, {1280, 960}, {1440, 1080}, {1600, 1200}, {1920, 1440}, {2560, 1920}, {2880, 2160} };
enum { NWIN = sizeof win_sizes / sizeof *win_sizes };

static void row_value(int r, char *out, size_t n) {
    switch (r) {
    case R_DISPLAY: snprintf(out, n, "%s", disp_names[!cfg.fullscreen]); break;
    case R_MONITOR: { SDL_DisplayMode dm; const char *nm = SDL_GetDisplayName(cfg.monitor);
        if (SDL_GetDesktopDisplayMode(cfg.monitor, &dm) == 0) snprintf(out, n, "%d: %.18s (%dx%d)", cfg.monitor + 1, nm ? nm : "Display", dm.w, dm.h);
        else snprintf(out, n, "%d", cfg.monitor + 1); break; }
    case R_WINDOW: snprintf(out, n, "%d x %d", cfg.win_w, cfg.win_h); break;
    case R_RENDER:
        if (cfg.render_mode == 0) snprintf(out, n, "Auto (match screen)");
        else if (cfg.render_mode > 0) snprintf(out, n, "%dx  (%d x %d)", cfg.render_mode, 640 * cfg.render_mode, 480 * cfg.render_mode);
        else snprintf(out, n, "Custom  %d x %d", cfg.render_w, cfg.render_h);
        break;
    case R_ASPECT: snprintf(out, n, "%s", aspect_names[cfg.stretch]); break;
    case R_FILTER: snprintf(out, n, "%s", filter_names[cfg.sharp]); break;
    case R_HD: snprintf(out, n, "%s", onoff_names[!cfg.hd]); break;
    case R_UI: snprintf(out, n, "%s", ui_names[cfg.ui_mode]); break;
    case R_PROMPTS: snprintf(out, n, "%s", prompt_label[cfg.prompts & 3]); break;
    case R_KEYS: snprintf(out, n, "Change..."); break;
    default: snprintf(out, n, "%s", cfg.cheats >> (r - R_IMMORTAL) & 1 ? "On" : "Off"); break;
    }
}
static void row_change(int r, int d) {
    switch (r) {
    case R_DISPLAY: cfg.fullscreen = !cfg.fullscreen; break;
    case R_MONITOR: { int nd = SDL_GetNumVideoDisplays(); if (nd < 1) nd = 1; cfg.monitor = (cfg.monitor + d + nd) % nd; break; }
    case R_WINDOW: {
        SDL_DisplayMode dm; int mw = 1 << 30, mh = 1 << 30;
        if (SDL_GetDesktopDisplayMode(cfg.monitor, &dm) == 0) { mw = dm.w; mh = dm.h; }
        int cur = -1, cnt = 0, fit[NWIN];
        for (int i = 0; i < NWIN; i++) if (i == 0 || (win_sizes[i][0] <= mw && win_sizes[i][1] <= mh)) fit[cnt++] = i;
        for (int i = 0; i < cnt; i++) if (win_sizes[fit[i]][0] == cfg.win_w && win_sizes[fit[i]][1] == cfg.win_h) cur = i;
        if (cur < 0) cur = d > 0 ? -1 : 0;
        cur = (cur + d + cnt) % cnt;
        cfg.win_w = win_sizes[fit[cur]][0]; cfg.win_h = win_sizes[fit[cur]][1];
        break; }
    case R_RENDER: { int m = cfg.render_mode < 0 ? 0 : cfg.render_mode; cfg.render_mode = (m + d + 5) % 5; break; }
    case R_ASPECT: cfg.stretch = !cfg.stretch; break;
    case R_FILTER: cfg.sharp = !cfg.sharp; break;
    case R_HD: cfg.hd = !cfg.hd; break;
    case R_UI: cfg.ui_mode = (cfg.ui_mode + d + 3) % 3; break;
    case R_PROMPTS: cfg.prompts = (cfg.prompts + d + 4) & 3; break;
    default: cfg.cheats ^= 1 << (r - R_IMMORTAL); break;
    }
}

/* button icons for the preview: the sheet's icon row holds Y B A X (top, right, bottom, left) */
static SDL_Texture *icon_tex[4]; static int icon_scale[4];
static const uint32_t icon_rgb[4][4] = {
    { 0x3FB54A, 0x2D7DD2, 0xE0333A, 0xF2C21B },    /* Dreamcast: Y green, B blue, A red, X yellow */
    { 0xF2C21B, 0xE0333A, 0x3FB54A, 0x2D7DD2 },    /* Xbox */
    { 0x3BB89A, 0xE5505A, 0x6A8CE8, 0xD98ACB },    /* PlayStation */
    { 0xE8E8E8, 0xE8E8E8, 0xE8E8E8, 0xE8E8E8 } };  /* Switch */
static int icon_y0[4];
static void load_icons(const char *base) {
    static const char *dirs[4] = { "", "prompts/xbox/hd/", "prompts/playstation/hd/", "prompts/switch/hd/" };
    for (int i = 0; i < 4; i++) {
        char p[1200]; int w, h;
        snprintf(p, sizeof p, "%stextures/%s9B28D9CBB4D57B31_256x256.png", base, dirs[i]);
        icon_tex[i] = ui_load_png(p, &w, &h);
        icon_scale[i] = icon_tex[i] ? w / 256 : 0; icon_y0[i] = 144;
        if (!icon_tex[i]) {   /* release builds: just the four icons (prompticons.py), 4x */
            static const char *st[4] = { "dreamcast", "xbox", "playstation", "switch" };
            snprintf(p, sizeof p, "%stextures/prompts/%s/icons.png", base, st[i]);
            icon_tex[i] = ui_load_png(p, &w, &h); icon_scale[i] = 4; icon_y0[i] = 0;
        }
    }
}
static void draw_pad(float cx, float cy, int style) {
    SDL_Texture *t = icon_tex[style];
    if (!t) return;                                      /* no button images in this build */
    int s = icon_scale[style];
    static const float off[4][2] = { { 0, -34 }, { 34, 0 }, { 0, 34 }, { -34, 0 } };   /* Y top, B right, A bottom, X left */
    for (int k = 0; k < 4; k++) {
        SDL_Rect src = { 18 * k * s, icon_y0[style] * s, 18 * s, 24 * s };
        SDL_FRect dst = { cx + off[k][0] - 18, cy + off[k][1] - 24, 36, 48 };
        uint32_t c = icon_rgb[style][k];
        SDL_SetTextureColorMod(t, c >> 16 & 255, c >> 8 & 255, c & 255); SDL_SetTextureAlphaMod(t, 255);
        SDL_RenderCopyF(ui_r, t, &src, &dst);
    }
}

/* ---- documents from the player's disc (AZ01.DC text table): "Title@@Body", '@' = new line, '^' = apostrophe ---- */
uint8_t *iso_load(const char *path, uint32_t *size_out);
typedef struct { char title[64]; char *body; } ui_doc;
static ui_doc docs[64]; static int ndocs;
static void load_docs(void) {
    uint32_t n = 0; uint8_t *d = iso_load("AZ01.DC", &n);
    if (!d) { fprintf(stderr, "[launcher] AZ01.DC not found on the disc\n"); return; }
    for (uint32_t i = 0; i + 8 < n && ndocs < 64; ) {
        uint32_t j = i; while (j < n && d[j] >= 32 && d[j] < 127) j++;
        if (j - i > 60) {
            char *s = (char *)d + i; char *at = memchr(s, '@', j - i);
            if (at && at + 1 < (char *)d + j && at[1] == '@' && at - s >= 3 && at - s < 60) {
                int ok = 1; for (char *q = s; q < at; q++) if (!(isalnum((unsigned char)*q) || strchr(" ',.^-", *q))) ok = 0;
                int dup = 0; for (int k = 0; k < ndocs; k++) if (!strncmp(docs[k].title, s, (size_t)(at - s)) && !docs[k].title[at - s]) dup = 1;
                if (ok && !dup) {
                    ui_doc *u = &docs[ndocs++]; size_t tl = (size_t)(at - s);
                    for (size_t k = 0; k < tl; k++) u->title[k] = s[k] == '^' ? '\'' : s[k];
                    u->title[tl] = 0; if (tl && u->title[tl - 1] == ',') u->title[tl - 1] = 0;
                    size_t bl = (size_t)((char *)d + j - (at + 2)); u->body = malloc(bl + 1); size_t o = 0;
                    for (const char *q = at + 2; q < (char *)d + j; q++) {
                        if (q[0] == '$' && q[1] == 'A') { const char *e = strstr(q, "$C"); if (e) { q = e + 1; continue; } }
                        u->body[o++] = *q == '@' ? '\n' : *q == '^' ? '\'' : *q;
                    }
                    u->body[o] = 0;
                }
            }
        }
        i = j + 1;
    }
    free(d);
    fprintf(stderr, "[launcher] %d documents\n", ndocs);
}
/* word-wrap s into lines no wider than w (logical px) at size sz */
static int wrap(int sz, const char *s, float w, char lines[][96], int maxl) {
    int n = 0; char cur[96] = ""; 
    while (*s && n < maxl) {
        if (*s == '\n') { snprintf(lines[n++], 96, "%s", cur); cur[0] = 0; s++; continue; }
        const char *e = s; while (*e && *e != ' ' && *e != '\n') e++;
        char word[96]; size_t l = (size_t)(e - s) < 95 ? (size_t)(e - s) : 95; memcpy(word, s, l); word[l] = 0;
        char trial[200]; snprintf(trial, sizeof trial, "%s%s%s", cur, cur[0] ? " " : "", word);
        if (ui_text_w(sz, trial) > w && cur[0]) { snprintf(lines[n++], 96, "%s", cur); snprintf(cur, sizeof cur, "%s", word); }
        else snprintf(cur, sizeof cur, "%.95s", trial);
        s = e; if (*s == ' ') s++;
    }
    if (cur[0] && n < maxl) snprintf(lines[n++], 96, "%s", cur);
    return n;
}
#ifndef HTTK_VERSION
#define HTTK_VERSION "0.14.0"
#endif
static const char *about_text =
    "Evil Dead: Hail to the King (Dreamcast, 2000) by Heavy Iron Studios and THQ, "
    "running natively on this computer through static recompilation: the game's own "
    "SH-4 machine code is translated to C ahead of time and runs on an emulation of "
    "the Dreamcast's graphics, sound and controller hardware.\n\n"
    "Version " HTTK_VERSION ". Unofficial PC recompilation (c) 2026 Killers' Heaven, "
    "free software under the GNU GPL v3.\n\n"
    "All game content is read from your own disc image; none is included.\n\n"
    "Fonts: Courier Prime and EB Garamond (SIL Open Font License). "
    "Libraries: SDL2, pl_mpeg, stb_image and stb_truetype. Cheat addresses from the published "
    "Dreamcast CodeBreaker codes.\n\n"
    "Evil Dead and its related characters are trademarks of their respective owners. "
    "This port is not affiliated with or endorsed by them.";

enum { B_PLAY, B_SAVE, B_CANCEL, NBTN };
static const char *btn_label_opt[NBTN] = { "Play", "Save and close", "Cancel" };
static const char *btn_label_launch[NBTN] = { "Play", "Save", "Cancel" };
static const float btn_x[NBTN] = { 60, 220, 500 }, btn_w[NBTN] = { 140, 260, 160 };
#define ROW_Y0 132.0f
#define ROW_H 31.0f
#define BTN_Y 672.0f
enum { S_HOME, S_OPTIONS, S_EXTRAS, S_DOCS, S_READ, S_ABOUT, S_MOVIES, S_MODELS, S_KEYS };

/* ---- model viewer: the game's characters, read from the disc (models.c) ---- */
#include "models.h"
static model_ent ments[96]; static int nments;
static model_t *mcur; static int mcur_i = -1; static SDL_Texture *mtex;
static float m_yaw = 0.6f, m_pitch = 0.12f, m_zoom = 1; static int m_props = 1;
/* descriptions by appearance (the disc doesn't name them) */
static const struct { const char *key, *name; } mnames[] = {
    { "AZ00.DC/00024000", "Ash" }, { "AZ00.DC/06040000", "Man in blue shirt" }, { "AZ00.DC/02002000", "Knight in armour" },
    { "AB50.DC/06494000", "Skeleton" }, { "AZ00.DC/063d4000", "Skeleton in jeans" }, { "AZ00.DC/06480000", "Skeleton, sword and shield" },
    { "AZ00.DC/0608c000", "Skeleton with axe" }, { "AG03.DC/020c0000", "Skeleton in rags" }, { "AC01.DC/06304000", "Deadite woman" },
    { "AK09.DC/06082000", "Deadite in torn dress" }, { "AC01.DC/06318000", "Green deadite" }, { "AC01.DC/06300000", "Deadite with bow" },
    { "AC05.DC/02496000", "Deadite in hat" }, { "AH01.DC/061ca000", "Bloody deadite" }, { "AH01.DC/061d8000", "Deadite in straw hat" },
    { "AG01.DC/06016000", "Man in black coat" }, { "AZ00.DC/06008000", "Robed man with book" }, { "AZ00.DC/06326000", "Villager in vest" },
    { "AZ00.DC/025b2000", "Figure in purple robe" }, { "AZ00.DC/06198000", "Cultist in red robe" }, { "AZ00.DC/06358000", "Figure in gold robe" },
    { "AZ00.DC/06584000", "Thin figure" }, { "AZ00.DC/0230c000", "Hulking ghoul" }, { "AZ00.DC/040c6000", "Bone demon" },
    { "AZ00.DC/04482000", "Giant skull" }, { "AH01.DC/063cc000", "Demon face" }, { "AH06.DC/02310000", "Winged creature" },
    { "AT02.DC/024e2000", "Tree creature" }, { "AT02.DC/024c0000", "Thorn" }, { "AU04.DC/02032000", "Stretched creature" },
    { "AZ00.DC/020a8000", "Long red creature" }, { "AZ00.DC/0635e000", "Clawed creature" }, { "AZ00.DC/06360000", "Orange creature" },
    { "AZ00.DC/06362000", "Green creature" }, { "AZ00.DC/001c0000", "Crystal" },
};
static int ment_rank(const model_ent *e) {
    char key[32]; snprintf(key, sizeof key, "%s/%08x", e->file, e->type);
    for (size_t k = 0; k < sizeof mnames / sizeof *mnames; k++) if (!strcasecmp(mnames[k].key, key)) return (int)k;
    return 1000;
}
static int ment_cmp(const void *a, const void *b) { return ment_rank(a) - ment_rank(b); }
static void ment_label(int i, char *out, size_t n) {
    char key[32]; snprintf(key, sizeof key, "%s/%08x", ments[i].file, ments[i].type);
    for (size_t k = 0; k < sizeof mnames / sizeof *mnames; k++) if (!strcasecmp(mnames[k].key, key)) { snprintf(out, n, "%s", mnames[k].name); return; }
    snprintf(out, n, "%.*s object %03X", (int)(strchr(ments[i].file, '.') ? strchr(ments[i].file, '.') - ments[i].file : 8), ments[i].file, (ments[i].type >> 12) & 0x7FF);
}
typedef struct { float z; int a; } mtri;
static int mtri_cmp(const void *a, const void *b) { float d = ((const mtri *)a)->z - ((const mtri *)b)->z; return d < 0 ? -1 : d > 0; }
static void model_select(int i) {
    if (i == mcur_i) return;
    model_free(mcur); mcur = NULL; if (mtex) { SDL_DestroyTexture(mtex); mtex = NULL; }
    mcur_i = i; m_zoom = 1;
    mcur = model_load(&ments[i]);
    m_props = 0;      /* alternate parts (Ash has 13 hands, one per weapon grip) are hidden until Enter */
    if (mcur && mcur->tex) {
        mtex = SDL_CreateTexture(ui_r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, mcur->tw, mcur->th);
        if (mtex) { SDL_UpdateTexture(mtex, NULL, mcur->tex, mcur->tw * 4); SDL_SetTextureBlendMode(mtex, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(mtex, SDL_ScaleModeLinear); }
    }
}
/* draws the current model in the box (x, y, w, h): back-to-front, lit by its normals */
static void model_draw(float bx, float by, float bw, float bh) {
    model_t *m = mcur; if (!m) return;
    static SDL_Vertex *vb; static mtri *tb; static size_t vcap, tcap;
    size_t nt = 0, nv = 0;
    for (int pi = 0; pi < m->nparts; pi++) if (pi == m->body || m_props || m->parts[pi].primary) { nt += (size_t)m->parts[pi].nt; nv += (size_t)m->parts[pi].nt * 3; }
    if (nv > vcap) { vcap = nv; vb = realloc(vb, vcap * sizeof *vb); }
    if (nt > tcap) { tcap = nt; tb = realloc(tb, tcap * sizeof *tb); }
    float cy = cosf(m_yaw), sy = sinf(m_yaw), cp = cosf(m_pitch), sp = sinf(m_pitch);
    float D = m->radius * 3.2f, f = bh * 0.46f * (D - m->radius) / m->radius * m_zoom;
    float lx = -0.35f, ly = 0.55f, lz = 0.76f;                      /* light from upper left, toward the viewer */
    size_t k = 0, t = 0;
    for (int pi = 0; pi < m->nparts; pi++) {
        if (pi != m->body && !m_props && !m->parts[pi].primary) continue;
        model_part *p = &m->parts[pi];
        for (int i = 0; i < p->nt; i++) {
            float zs = 0;
            for (int c = 0; c < 3; c++) {
                model_vert *v = &p->v[p->idx[i * 3 + c]];
                float x = v->p[0] - m->center[0], y = v->p[1] - m->center[1], z = v->p[2] - m->center[2];
                float xr = x * cy - z * sy, zr = x * sy + z * cy;              /* turn */
                float yr = y * cp - zr * sp; zr = y * sp + zr * cp;            /* tilt */
                float nx = v->n[0] * cy - v->n[2] * sy, nz0 = v->n[0] * sy + v->n[2] * cy;
                float ny = v->n[1] * cp - nz0 * sp, nz = v->n[1] * sp + nz0 * cp;
                float sh = 0.42f + 0.68f * fabsf(-nx * lx + ny * ly + nz * lz); if (sh > 1) sh = 1;
                float dz = D - zr; if (dz < 0.05f) dz = 0.05f;
                SDL_Vertex *o = &vb[k + c];
                o->position.x = bx + bw / 2 - f * xr / dz; o->position.y = by + bh / 2 - f * yr / dz;
                Uint8 g = (Uint8)(sh * 255); o->color.r = o->color.g = o->color.b = g; o->color.a = 255;
                o->tex_coord.x = v->uv[0]; o->tex_coord.y = v->uv[1];
                zs += zr;
            }
            for (int ax = 0; ax < 2; ax++) {   /* SDL can't repeat textures: shift the triangle into 0..1, clamp what still spills */
                float *c0 = ax ? &vb[k].tex_coord.y : &vb[k].tex_coord.x;
                float lo = c0[0]; for (int c = 1; c < 3; c++) { float u = ax ? vb[k + c].tex_coord.y : vb[k + c].tex_coord.x; if (u < lo) lo = u; }
                float sh2 = floorf(lo);
                for (int c = 0; c < 3; c++) { float *u = ax ? &vb[k + c].tex_coord.y : &vb[k + c].tex_coord.x; *u -= sh2; if (*u > 1) *u = 1; if (*u < 0) *u = 0; }
            }
            tb[t].z = zs; tb[t].a = (int)k; t++; k += 3;
        }
    }
    qsort(tb, t, sizeof *tb, mtri_cmp);                                    /* far first */
    static SDL_Vertex *sorted; static size_t scap;
    if (k > scap) { scap = k; sorted = realloc(sorted, scap * sizeof *sorted); }
    for (size_t i = 0; i < t; i++) memcpy(&sorted[i * 3], &vb[tb[i].a], 3 * sizeof *vb);
    SDL_Rect clip = { (int)bx, (int)by, (int)bw, (int)bh }; SDL_RenderSetClipRect(ui_r, &clip);
    if (SDL_RenderGeometry(ui_r, mtex, sorted, (int)k, NULL, 0) < 0) { static int w; if (!w++) fprintf(stderr, "[models] draw failed: %s\n", SDL_GetError()); }
    { static int once; if (!once++ && getenv("DC_MODELS_DEBUG")) fprintf(stderr, "[models] %zu verts, first at %.1f,%.1f tex %p\n", k, k ? sorted[0].position.x : 0.f, k ? sorted[0].position.y : 0.f, (void *)mtex); }
    SDL_RenderSetClipRect(ui_r, NULL);
}

/* ---- cutscenes: the disc's Sofdec movies (sfd.c) ---- */
#include "third_party/pl_mpeg.h"
typedef struct sfd_t sfd_t;
sfd_t *sfd_open(uint8_t *data, size_t len); plm_frame_t *sfd_next_frame(sfd_t *s); void sfd_close(sfd_t *s);
size_t sfd_take_audio(sfd_t *s, int16_t *dst, size_t max); int sfd_audio_channels(sfd_t *s); int sfd_audio_rate(sfd_t *s); double sfd_fps(sfd_t *s);
typedef struct { char name[16]; uint32_t size; } ui_movie;
static ui_movie movies[128]; static int nmovies;
typedef struct { char name[64]; uint32_t fad, size; int dir; } iso_entry_ui;
void iso_list_cb(void (*fn)(const char *name, uint32_t size, void *ud), void *ud);
static void movie_cb(const char *name, uint32_t size, void *ud) {
    (void)ud; size_t l = strlen(name);
    if (l > 4 && !strcasecmp(name + l - 4, ".SFD") && nmovies < 128) { snprintf(movies[nmovies].name, sizeof movies[0].name, "%s", name); movies[nmovies++].size = size; }
}
/* plays one movie in the window; returns when it ends or the player presses Esc / B / Enter */
static void play_movie(const char *name) {
    uint8_t *iso_load(const char *path, uint32_t *size_out);
    uint32_t n = 0; uint8_t *d = NULL;
    const char *mdir = getenv("DC_MOVIE_DIR");                 /* test hook: movies from a folder instead of the disc */
    if (mdir) { char pth[1024]; snprintf(pth, sizeof pth, "%s/%s", mdir, name); FILE *f = fopen(pth, "rb");
        if (f) { fseek(f, 0, SEEK_END); n = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET); d = malloc(n + 1); if (fread(d, 1, n, f) != n) { free(d); d = NULL; } fclose(f); } }
    else d = iso_load(name, &n);
    if (!d) { fprintf(stderr, "[movie] cannot read %s\n", name); return; }
    sfd_t *m = sfd_open(d, n); free(d);
    SDL_AudioDeviceID ad = 0;
    if (sfd_audio_rate(m) > 0) {
        if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
        SDL_AudioSpec want = {0}, got; want.freq = sfd_audio_rate(m); want.format = AUDIO_S16SYS; want.channels = (Uint8)sfd_audio_channels(m); want.samples = 2048;
        ad = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
        if (ad) { static int16_t buf[1 << 16]; size_t k; while ((k = sfd_take_audio(m, buf, 1 << 16)) > 0) SDL_QueueAudio(ad, buf, (Uint32)(k * 2)); SDL_PauseAudioDevice(ad, 0); }
    }
    SDL_Texture *tex = NULL; int tw = 0, th = 0;
    Uint64 t0 = SDL_GetPerformanceCounter(), hz = SDL_GetPerformanceFrequency();
    double fps = sfd_fps(m); long shown = 0; int stop = 0;
    const char *shot = getenv("DC_MOVIE_SHOT"); long shot_frame = getenv("DC_MOVIE_SHOT_FRAME") ? atol(getenv("DC_MOVIE_SHOT_FRAME")) : -1;
    while (!stop) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) stop = 1;
            else if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_BACKSPACE)) stop = 1;
            else if (e.type == SDL_CONTROLLERBUTTONDOWN && (e.cbutton.button == SDL_CONTROLLER_BUTTON_B || e.cbutton.button == SDL_CONTROLLER_BUTTON_A || e.cbutton.button == SDL_CONTROLLER_BUTTON_START)) stop = 1;
            else if (e.type == SDL_MOUSEBUTTONDOWN) stop = 1;
        }
        double t = (double)(SDL_GetPerformanceCounter() - t0) / (double)hz;
        if (shot) t = (shown + 0.5) / fps;                               /* test hook: no real-time wait */
        int fresh = 0;
        while (shown <= (long)(t * fps)) {
            plm_frame_t *f = sfd_next_frame(m);
            if (!f) { stop = 1; break; }
            if (!tex || tw != (int)f->width || th != (int)f->height) {
                if (tex) SDL_DestroyTexture(tex);
                tw = (int)f->width; th = (int)f->height;
                tex = SDL_CreateTexture(ui_r, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, tw, th);
            }
            SDL_UpdateYUVTexture(tex, NULL, f->y.data, (int)f->y.width, f->cb.data, (int)f->cb.width, f->cr.data, (int)f->cr.width);
            shown++; fresh = 1;
        }
        if (stop) break;
        if (fresh) {
            SDL_SetRenderDrawColor(ui_r, 0, 0, 0, 255); SDL_RenderClear(ui_r);
            if (tex) {   /* keep the movie's 4:3 shape */
                int w = UI_W, h = UI_W * 3 / 4; if (h > UI_H) { h = UI_H; w = UI_H * 4 / 3; }
                SDL_Rect dst = { (UI_W - w) / 2, (UI_H - h) / 2, w, h }; SDL_RenderCopy(ui_r, tex, NULL, &dst); }
            if (shot && shown >= shot_frame && shot_frame >= 0) {
                int ow, oh; SDL_GetRendererOutputSize(ui_r, &ow, &oh);
                uint8_t *px = malloc((size_t)ow * oh * 4); SDL_RenderReadPixels(ui_r, NULL, SDL_PIXELFORMAT_ABGR8888, px, ow * 4);
                png_write(shot, px, ow, oh, 0); free(px); fprintf(stderr, "[movie] %s frame %ld saved\n", name, shown); stop = 1;
            }
            SDL_RenderPresent(ui_r);
        } else SDL_Delay(2);
    }
    if (ad) SDL_CloseAudioDevice(ad);
    if (tex) SDL_DestroyTexture(tex);
    sfd_close(m);
}

/* launcher = 1: home screen (Play, Options, Extras, Quit); 0: options only. Returns 1 = play, 0 = quit */
static int options_window_ex(int launcher) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    float dpi = 96; SDL_GetDisplayDPI(cfg.monitor < SDL_GetNumVideoDisplays() ? cfg.monitor : 0, &dpi, NULL, NULL);
    float f = dpi > 96 ? dpi / 96 : 1;
    SDL_DisplayMode dm; int ww = (int)(UI_W * f), wh = (int)(UI_H * f);
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && wh > dm.h * 85 / 100) { wh = dm.h * 85 / 100; ww = wh * UI_W / UI_H; }
    SDL_Window *w = SDL_CreateWindow(launcher ? "Evil Dead: Hail to the King" : "Evil Dead: Hail to the King - Options",
                                     SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!w) { fprintf(stderr, "[options] window: %s\n", SDL_GetError()); return 1; }
    ui_r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ui_r) ui_r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
    if (!ui_r) { fprintf(stderr, "[options] renderer: %s\n", SDL_GetError()); SDL_DestroyWindow(w); return 1; }
    int ow, oh; SDL_GetRendererOutputSize(ui_r, &ow, &oh);
    SDL_RenderSetLogicalSize(ui_r, UI_W, UI_H);
    float sc = (float)ow / UI_W < (float)oh / UI_H ? (float)ow / UI_W : (float)oh / UI_H;
    if (!ui_font_init(sc < 1 ? 1 : sc)) return 1;

    char base[1024] = ""; { char *b = SDL_GetBasePath(); if (b) { snprintf(base, sizeof base, "%s", b); SDL_free(b); } }
    char p[1200]; int bw = 0, bh = 0;
    snprintf(p, sizeof p, "%stextures/3BA3ED33C3D7FC12_512x256.png", base);
    SDL_Texture *bg = ui_load_png(p, &bw, &bh);          /* title art (with the HD logo), if present */
    load_icons(base);
    if (launcher) { load_docs(); nmovies = 0; iso_list_cb(movie_cb, NULL); nments = models_scan(ments, 96); qsort(ments, (size_t)nments, sizeof *ments, ment_cmp);
        if (getenv("DC_MOVIE_DIR")) { movie_cb("AX00.SFD", 4u << 20, NULL); movie_cb("AX01A.SFD", 40u << 20, NULL); } }

    SDL_GameController *pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }
    int screen = launcher ? S_HOME : S_OPTIONS, sel = 0, btn = launcher ? B_SAVE : B_PLAY, result = -1;
    int home_sel = 0, extras_sel = 0, doc_sel = 0, doc_top = 0, read_top = 0, nlines = 0;
    static char lines[400][96];
    const char *keys = getenv("DC_OPTIONS_KEYS"), *shot = getenv("DC_OPTIONS_SHOT");
    const char **btn_label = launcher ? btn_label_launch : btn_label_opt;
    int btn_first = launcher ? B_SAVE : B_PLAY;
    float mx = -1, my = -1;
    int ksel = 0, kwait = -1;            /* key screen: selected row, row waiting for a key */
    while (result < 0) {
        /* ---- input ---- */
        int act = 0, dx = 0, dy = 0, cancel = 0, click = 0, wheel = 0; float drag_x = 0, drag_y = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { result = 0; break; }
            else if (e.type == SDL_KEYDOWN && screen == S_KEYS && kwait >= 0) {   /* capturing a key */
                SDL_Scancode sc = e.key.keysym.scancode;
                if (sc != SDL_SCANCODE_ESCAPE && !key_reserved(sc)) {
                    for (int i = 0; i < NKEYS; i++) if (i != kwait && keymap[i] == sc) keymap[i] = keymap[kwait];   /* swap */
                    keymap[kwait] = sc;
                }
                kwait = -1;
            }
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_UP) dy = -1; else if (k == SDLK_DOWN) dy = 1; else if (k == SDLK_LEFT) dx = -1; else if (k == SDLK_RIGHT) dx = 1;
                else if (k == SDLK_PAGEUP) dy = -10; else if (k == SDLK_PAGEDOWN) dy = 10;
                else if (k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_KP_ENTER) act = 1; else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) cancel = 1;
            } else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: dy = -1; break; case SDL_CONTROLLER_BUTTON_DPAD_DOWN: dy = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: dx = -1; break; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dx = 1; break;
                case SDL_CONTROLLER_BUTTON_A: act = 1; break; case SDL_CONTROLLER_BUTTON_B: cancel = 1; break;
                case SDL_CONTROLLER_BUTTON_START: if (screen == S_HOME || !launcher) { cfg_save(); result = 1; } break;
                }
            } else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
            else if (e.type == SDL_MOUSEMOTION) { mx = (float)e.motion.x; my = (float)e.motion.y;
                if ((e.motion.state & SDL_BUTTON_LMASK) && mx > 380) { drag_x += (float)e.motion.xrel; drag_y += (float)e.motion.yrel; } }
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) { mx = (float)e.button.x; my = (float)e.button.y; click = 1; }
            else if (e.type == SDL_MOUSEWHEEL) { dy = e.wheel.y > 0 ? -3 : 3; wheel = dy; }
        }
        if (result >= 0) break;
        if (keys && *keys) {                                   /* scripted keys (test hook): one per frame */
            const char *c = strchr(keys, ','); size_t l = c ? (size_t)(c - keys) : strlen(keys);
            if (!strncmp(keys, "up", l)) dy = -1; else if (!strncmp(keys, "down", l)) dy = 1;
            else if (!strncmp(keys, "left", l)) dx = -1; else if (!strncmp(keys, "right", l)) dx = 1;
            else if (!strncmp(keys, "enter", l)) act = 1; else if (!strncmp(keys, "esc", l)) cancel = 1;
            else if (l > 4 && !strncmp(keys, "key:", 4) && screen == S_KEYS && kwait >= 0) {   /* a key for the key screen */
                char nm[32]; snprintf(nm, sizeof nm, "%.*s", (int)(l - 4), keys + 4);
                SDL_Scancode sc = SDL_GetScancodeFromName(nm);
                if (!key_reserved(sc)) { for (int i = 0; i < NKEYS; i++) if (i != kwait && keymap[i] == sc) keymap[i] = keymap[kwait]; keymap[kwait] = sc; }
                kwait = -1;
            }
            keys = c ? c + 1 : keys + l;
        }

        /* ---- background ---- */
        SDL_SetRenderDrawColor(ui_r, 12, 10, 12, 255); SDL_RenderClear(ui_r);
        if (bg) {   /* the title art's 4:3 picture area, darkened */
            SDL_Rect src = { 0, 0, bw * 320 / 512 - 4, bh * 240 / 256 - 4 };   /* stay inside the picture: the texture holds other data below it */
            SDL_SetTextureColorMod(bg, 255, 255, 255); SDL_SetTextureAlphaMod(bg, 255);
            SDL_RenderCopy(ui_r, bg, &src, NULL);
            ui_fill(0, 0, UI_W, UI_H, 0x0A080A, screen == S_HOME ? 120 : 215);
        } else {    /* no title art (release builds): a dark red glow behind the menu */
            static SDL_Texture *glow;
            if (!glow) {   /* 64 x 64 radial glow, stretched over the window */
                static uint32_t px[64 * 64];
                for (int yy = 0; yy < 64; yy++) for (int xx = 0; xx < 64; xx++) {
                    float dx = (xx - 31.5f) / 40.f, dy = (yy - 20.f) / 44.f, t = 1 - sqrtf(dx * dx + dy * dy); if (t < 0) t = 0;
                    px[yy * 64 + xx] = (uint32_t)(t * t * 255) << 24 | 0x3A0F0C;
                }
                glow = SDL_CreateTexture(ui_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 64, 64);
                if (glow) { SDL_UpdateTexture(glow, NULL, px, 64 * 4); SDL_SetTextureBlendMode(glow, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(glow, SDL_ScaleModeLinear); }
            }
            if (glow) SDL_RenderCopy(ui_r, glow, NULL, NULL);
            if (screen == S_HOME) {
                ui_text(3, 484, 236, 1, 0x000000, 160, "EVIL DEAD");
                ui_text(3, 480, 230, 1, 0xB3261E, 255, "EVIL DEAD");
                ui_text(2, 480, 292, 1, 0xD8D2C8, 255, "Hail to the King");
            }
        }

        if (screen == S_HOME) {
            static const char *items[4] = { "Play", "Options", "Extras", "Quit" };
            for (int i = 0; i < 4; i++) if (mx >= 0 && my >= 392 + i * 48 - 34 && my < 392 + i * 48 + 10 && mx > 340 && mx < 620) { home_sel = i; if (click) act = 1; }
            if (dy) home_sel = (home_sel + (dy > 0 ? 1 : 3)) % 4;
            if (cancel) result = 0;
            if (act) { if (home_sel == 0) { cfg_save(); result = 1; } else if (home_sel == 1) { screen = S_OPTIONS; sel = 0; btn = B_SAVE; }
                       else if (home_sel == 2) { screen = S_EXTRAS; extras_sel = 0; } else result = 0; act = 0; }
            for (int i = 0; i < 4 && screen == S_HOME; i++) {
                float y = 392 + i * 48; int on = home_sel == i;
                if (on) { ui_fill(340, y - 34, 280, 44, 0x8E1F1A, 210); ui_frame(340, y - 34, 280, 44, 0xE0453B, 255); }
                ui_text(2, 480, y, 1, on ? 0xFFFFFF : 0xD8D2C8, 255, items[i]);
            }
            ui_text(0, 480, 700, 1, 0x8A847C, 255, "Unofficial PC recompilation (c) 2026 Killers' Heaven");
        } else if (screen == S_OPTIONS) {
            /* mouse: hover selects; a click on a row's value steps it (left half back, right half forward) */
            if (mx >= 0) {
                for (int r = 0; r < NROWS; r++) if (my >= ROW_Y0 + r * ROW_H - 23 && my < ROW_Y0 + r * ROW_H + 8 && mx >= 40 && mx < 920) {
                    sel = r; if (click) { if (r == R_KEYS) { screen = S_KEYS; ksel = 0; kwait = -1; click = 0; } else row_change(r, mx < 680 ? -1 : 1); } }
                for (int b = btn_first; b < NBTN; b++) if (my >= BTN_Y - 34 && my < BTN_Y + 14 && mx >= btn_x[b] && mx < btn_x[b] + btn_w[b]) {
                    sel = NROWS; btn = b; if (click) act = 1; }
            }
            if (dy) { sel += dy > 0 ? 1 : -1; if (sel < 0) sel = NROWS; if (sel > NROWS) sel = 0; }
            if (dx) { if (sel < NROWS) { if (sel != R_KEYS) row_change(sel, dx); } else { btn += dx; if (btn < btn_first) btn = NBTN - 1; if (btn >= NBTN) btn = btn_first; } }
            if (act && sel == R_KEYS) { screen = S_KEYS; ksel = 0; kwait = -1; act = 0; }
            if (act) {
                if (sel < NROWS) row_change(sel, 1);
                else if (btn == B_PLAY) { cfg_save(); result = 1; }
                else if (btn == B_SAVE) { cfg_save(); if (launcher) screen = S_HOME; else result = 0; }
                else { if (launcher) { cfg_load(); screen = S_HOME; } else result = 0; }
            }
            if (cancel) { if (launcher) { cfg_load(); screen = S_HOME; } else result = 0; }
            if (screen == S_OPTIONS) {
                ui_text(2, 60, 64, 0, 0xE8E2D8, 255, "Evil Dead: Hail to the King");
                ui_text(1, 60, 98, 0, 0xD0463C, 255, "Options");
                for (int r = 0; r < NROWS; r++) {
                    float y = ROW_Y0 + r * ROW_H;
                    int on = sel == r;
                    if (on) { ui_fill(40, y - 23, 880, 30, 0x8E1F1A, 150); ui_fill(40, y - 23, 4, 30, 0xE0453B, 255); }
                    ui_text(1, 64, y, 0, on ? 0xFFFFFF : 0xC8C2B8, 255, row_label[r]);
                    if (row_status[r].lvl) ui_text(0, 64 + ui_text_w(1, row_label[r]) + 12, y - 2, 0, status_rgb[row_status[r].lvl], 255, status_tag[row_status[r].lvl]);
                    char v[96]; row_value(r, v, sizeof v);
                    ui_text(1, 680, y, 1, on ? 0xFFFFFF : 0xE8E2D8, 255, v);
                    ui_text(1, 440, y, 1, on ? 0xFFFFFF : 0x6A6460, 255, "<");
                    ui_text(1, 920 - 16, y, 2, on ? 0xFFFFFF : 0x6A6460, 255, ">");
                }
                int hr = sel < NROWS ? sel : -1;
                if (hr >= 0) { ui_text(0, 60, 576, 0, 0xA8A29A, 255, row_help[hr][0]); ui_text(0, 60, 597, 0, 0xA8A29A, 255, row_help[hr][1]);
                               ui_text(0, 60, 620, 0, status_rgb[row_status[hr].lvl], 255, row_status[hr].text); }
                else ui_text(0, 60, 586, 0, 0xA8A29A, 255, btn == B_PLAY ? "Save these settings and start the game." :
                                                         btn == B_SAVE ? (launcher ? "Save these settings." : "Save these settings and close.") : "Leave without saving.");
                for (int b = btn_first; b < NBTN; b++) {
                    int on = sel == NROWS && btn == b;
                    if (on) ui_fill(btn_x[b], BTN_Y - 34, btn_w[b], 48, 0x8E1F1A, 200);
                    ui_frame(btn_x[b], BTN_Y - 34, btn_w[b], 48, on ? 0xE0453B : 0x6A6460, 255);
                    ui_text(1, btn_x[b] + btn_w[b] / 2, BTN_Y, 1, 0xFFFFFF, 255, btn_label[b]);
                }
                if (icon_tex[cfg.prompts & 3]) { draw_pad(840, 612, cfg.prompts & 3); ui_text(0, 840, 690, 1, 0x8A847C, 255, prompt_label[cfg.prompts & 3]); }
            }
        } else if (screen == S_KEYS) {
            enum { KROWS = NKEYS + 2 };      /* + reset, back */
            if (kwait < 0) {
                if (mx >= 0) for (int r = 0; r < KROWS; r++) if (my >= ROW_Y0 + r * ROW_H - 23 && my < ROW_Y0 + r * ROW_H + 8 && mx >= 40 && mx < 920) { ksel = r; if (click) act = 1; }
                if (dy) { ksel += dy > 0 ? 1 : -1; if (ksel < 0) ksel = KROWS - 1; if (ksel >= KROWS) ksel = 0; }
                if (act) {
                    if (ksel < NKEYS) kwait = ksel;
                    else if (ksel == NKEYS) memcpy(keymap, key_default, sizeof keymap);
                    else { screen = S_OPTIONS; sel = R_KEYS; }
                }
                if (cancel) { screen = S_OPTIONS; sel = R_KEYS; }
            }
            if (screen == S_KEYS) {
                ui_text(2, 60, 64, 0, 0xE8E2D8, 255, "Evil Dead: Hail to the King");
                ui_text(1, 60, 98, 0, 0xD0463C, 255, "Keyboard controls");
                for (int r = 0; r < KROWS; r++) {
                    float y = ROW_Y0 + r * ROW_H + (r >= NKEYS ? 12 : 0); int on = ksel == r;
                    if (on) { ui_fill(40, y - 23, 880, 30, kwait == r ? 0xB3261E : 0x8E1F1A, 150); ui_fill(40, y - 23, 4, 30, 0xE0453B, 255); }
                    if (r < NKEYS) {
                        ui_text(1, 64, y, 0, on ? 0xFFFFFF : 0xC8C2B8, 255, key_label[r]);
                        ui_text(1, 680, y, 1, on ? 0xFFFFFF : 0xE8E2D8, 255, kwait == r ? "press a key..." : SDL_GetScancodeName(keymap[r]));
                    } else ui_text(1, 64, y, 0, on ? 0xFFFFFF : 0xC8C2B8, 255, r == NKEYS ? "Reset to defaults" : "Back");
                }
                ui_text(0, 60, 586, 0, 0xA8A29A, 255, kwait >= 0 ? "Press the new key. Esc keeps the old one." :
                        "Enter: change the key. A key already in use swaps with this one.");
                ui_text(0, 60, 608, 0, 0xA8A29A, 255, "Esc and F8-F12 are kept for the hotkeys. Save on the Options page to keep changes.");
            }
        } else if (screen == S_EXTRAS) {
            char d0[64], d1[64], d2[64]; snprintf(d0, sizeof d0, "Documents (%d)", ndocs); snprintf(d1, sizeof d1, "Cutscenes (%d)", nmovies); snprintf(d2, sizeof d2, "Models (%d)", nments);
            const char *items[5] = { d0, d1, d2, "About this port", "Back" };
            for (int i = 0; i < 5; i++) if (mx >= 0 && my >= 250 + i * 56 - 36 && my < 250 + i * 56 + 14 && mx > 60 && mx < 600) { extras_sel = i; if (click) act = 1; }
            if (dy) extras_sel = (extras_sel + (dy > 0 ? 1 : 4)) % 5;
            if (cancel || (act && extras_sel == 4)) { screen = S_HOME; act = 0; }
            if (act && extras_sel == 0 && ndocs) { screen = S_DOCS; doc_sel = 0; doc_top = 0; }
            if (act && extras_sel == 1 && nmovies) { screen = S_MOVIES; doc_sel = 0; doc_top = 0; act = 0; }
            if (act && extras_sel == 2 && nments) { screen = S_MODELS; doc_sel = 0; doc_top = 0; act = 0; model_select(0); }
            if (act && extras_sel == 3) { screen = S_ABOUT; read_top = 0; nlines = wrap(1, about_text, 820, lines, 400); }
            if (screen == S_EXTRAS) {
                ui_text(2, 60, 64, 0, 0xE8E2D8, 255, "Evil Dead: Hail to the King");
                ui_text(1, 60, 98, 0, 0xD0463C, 255, "Extras");
                for (int i = 0; i < 5; i++) {
                    float y = 250 + i * 56; int on = extras_sel == i;
                    if (on) { ui_fill(40, y - 36, 560, 48, 0x8E1F1A, 170); ui_fill(40, y - 36, 4, 48, 0xE0453B, 255); }
                    ui_text(2, 64, y, 0, on ? 0xFFFFFF : 0xD8D2C8, 255, items[i]);
                    if (ex_status[i].lvl) ui_text(0, 64 + ui_text_w(2, items[i]) + 14, y - 4, 0, status_rgb[ex_status[i].lvl], 255, status_tag[ex_status[i].lvl]);
                }
                ui_text(0, 60, 620, 0, status_rgb[ex_status[extras_sel].lvl], 255, ex_status[extras_sel].text);
                ui_text(0, 60, 576, 0, 0xA8A29A, 255, extras_sel == 0 ? "Every note, journal and page Ash can find, read from your disc." :
                                                     extras_sel == 1 ? "Watch the game's movies, played from your disc." :
                                                     extras_sel == 2 ? "Turn and inspect the game's characters, read from your disc." :
                                                     extras_sel == 3 ? "What this is, and credits." : "Back to the main screen.");
            }
        } else if (screen == S_DOCS) {
            enum { VIS = 14 };
            for (int i = 0; i < VIS && doc_top + i < ndocs; i++) if (mx >= 0 && my >= 150 + i * 36 - 26 && my < 150 + i * 36 + 10 && mx > 40 && mx < 920) { doc_sel = doc_top + i; if (click) act = 1; }
            if (dy) { doc_sel += dy > 0 ? (dy > 1 ? dy : 1) : (dy < -1 ? dy : -1); if (doc_sel < 0) doc_sel = 0; if (doc_sel >= ndocs) doc_sel = ndocs - 1; }
            if (doc_sel < doc_top) doc_top = doc_sel; if (doc_sel >= doc_top + VIS) doc_top = doc_sel - VIS + 1;
            if (cancel) { screen = S_EXTRAS; act = 0; }
            if (act) { screen = S_READ; read_top = 0; nlines = wrap(1, docs[doc_sel].body, 820, lines, 400); }
            if (screen == S_DOCS) {
                ui_text(2, 60, 64, 0, 0xE8E2D8, 255, "Documents");
                ui_text(1, 60, 98, 0, 0xD0463C, 255, "Found on your disc");
                for (int i = 0; i < VIS && doc_top + i < ndocs; i++) {
                    float y = 150 + i * 36; int on = doc_sel == doc_top + i;
                    if (on) { ui_fill(40, y - 26, 880, 34, 0x8E1F1A, 170); ui_fill(40, y - 26, 4, 34, 0xE0453B, 255); }
                    ui_text(1, 64, y, 0, on ? 0xFFFFFF : 0xD8D2C8, 255, docs[doc_top + i].title);
                }
                ui_text(0, 60, 690, 0, 0x8A847C, 255, "Enter / A: read     Esc / B: back");
            }
        } else if (screen == S_MOVIES) {
            enum { VIS = 14, COLS = 3 };
            int rows = (nmovies + COLS - 1) / COLS;
            for (int i = 0; i < VIS && doc_top + i < rows; i++) for (int c = 0; c < COLS; c++) {
                int k = (doc_top + i) * COLS + c; if (k >= nmovies) break;
                if (mx >= 0 && my >= 150 + i * 36 - 26 && my < 150 + i * 36 + 10 && mx > 40 + c * 293 && mx < 40 + (c + 1) * 293) { doc_sel = k; if (click) act = 1; }
            }
            if (dy) { doc_sel += (dy > 0 ? 1 : -1) * (abs(dy) > 1 ? VIS * COLS : COLS); }
            if (dx) doc_sel += dx;
            if (doc_sel < 0) doc_sel = 0; if (doc_sel >= nmovies) doc_sel = nmovies - 1;
            if (doc_sel / COLS < doc_top) doc_top = doc_sel / COLS; if (doc_sel / COLS >= doc_top + VIS) doc_top = doc_sel / COLS - VIS + 1;
            if (cancel) { screen = S_EXTRAS; act = 0; }
            if (act) { play_movie(movies[doc_sel].name); act = 0; mx = my = -1; }
            if (screen == S_MOVIES) {
                ui_text(2, 60, 64, 0, 0xE8E2D8, 255, "Cutscenes");
                ui_text(1, 60, 98, 0, 0xD0463C, 255, "Played from your disc");
                for (int i = 0; i < VIS && doc_top + i < rows; i++) for (int c = 0; c < COLS; c++) {
                    int k = (doc_top + i) * COLS + c; if (k >= nmovies) break;
                    float y = 150 + i * 36, x = 40 + c * 293; int on = doc_sel == k;
                    if (on) { ui_fill(x, y - 26, 280, 34, 0x8E1F1A, 170); ui_fill(x, y - 26, 4, 34, 0xE0453B, 255); }
                    char lbl[48]; char nm[16]; snprintf(nm, sizeof nm, "%s", movies[k].name); char *dot = strchr(nm, '.'); if (dot) *dot = 0;
                    snprintf(lbl, sizeof lbl, "%s  %.1f MB", nm, movies[k].size / 1048576.0);
                    ui_text(1, x + 20, y, 0, on ? 0xFFFFFF : 0xD8D2C8, 255, lbl);
                }
                ui_text(0, 60, 690, 0, 0x8A847C, 255, "Enter / A: play     Esc / B: back (also stops a movie)");
            }
        } else if (screen == S_MODELS) {
            enum { VIS = 15 };
            for (int i = 0; i < VIS && doc_top + i < nments; i++) if (mx >= 0 && my >= 150 + i * 34 - 25 && my < 150 + i * 34 + 9 && mx > 40 && mx < 360) { doc_sel = doc_top + i; }
            if (dy && !wheel) { doc_sel += dy > 0 ? 1 : -1; }
            if (wheel && mx > 380) { m_zoom *= wheel < 0 ? 1.12f : 1 / 1.12f; }
            else if (wheel) { doc_sel += wheel > 0 ? 3 : -3; }
            if (doc_sel < 0) doc_sel = 0; if (doc_sel >= nments) doc_sel = nments - 1;
            if (doc_sel < doc_top) doc_top = doc_sel; if (doc_sel >= doc_top + VIS) doc_top = doc_sel - VIS + 1;
            model_select(doc_sel);
            { const Uint8 *ks = SDL_GetKeyboardState(NULL); float spin = 0, tilt = 0, zm = 0;
              if (ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_A]) spin -= 1; if (ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_D]) spin += 1;
              if (ks[SDL_SCANCODE_W]) tilt += 1; if (ks[SDL_SCANCODE_S]) tilt -= 1;
              if (ks[SDL_SCANCODE_EQUALS] || ks[SDL_SCANCODE_KP_PLUS] || ks[SDL_SCANCODE_PAGEUP]) zm += 1;
              if (ks[SDL_SCANCODE_MINUS] || ks[SDL_SCANCODE_KP_MINUS] || ks[SDL_SCANCODE_PAGEDOWN]) zm -= 1;
              if (pad) {
                  float rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.f, ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.f;
                  float lx2 = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.f;
                  if (fabsf(rx) > 0.2f) spin += rx; else if (fabsf(lx2) > 0.3f) spin += lx2;
                  if (fabsf(ry) > 0.2f) tilt -= ry;
                  zm += SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.f - SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.f;
              }
              static Uint32 idle_since; Uint32 now = SDL_GetTicks();
              if (spin || tilt || zm || drag_x || drag_y) idle_since = now;
              m_yaw += spin * 0.035f + drag_x * 0.01f; m_pitch += tilt * 0.025f + drag_y * 0.01f;
              if (m_pitch > 1.4f) m_pitch = 1.4f; if (m_pitch < -1.4f) m_pitch = -1.4f;
              m_zoom *= 1 + zm * 0.02f; if (m_zoom < 0.3f) m_zoom = 0.3f; if (m_zoom > 6) m_zoom = 6;
              if (now - idle_since > 2500) m_yaw += 0.006f;                 /* turn slowly when left alone */
            }
            if (act) { m_props = !m_props; act = 0; }
            if (cancel) { screen = S_EXTRAS; act = 0; }
            if (screen == S_MODELS) {
                ui_text(2, 60, 64, 0, 0xE8E2D8, 255, "Models");
                ui_text(1, 60, 98, 0, 0xD0463C, 255, "Read from your disc");
                for (int i = 0; i < VIS && doc_top + i < nments; i++) {
                    float y = 150 + i * 34; int on = doc_sel == doc_top + i;
                    if (on) { ui_fill(40, y - 25, 330, 32, 0x8E1F1A, 170); ui_fill(40, y - 25, 4, 32, 0xE0453B, 255); }
                    char lbl[48]; ment_label(doc_top + i, lbl, sizeof lbl);
                    ui_text(0, 60, y - 2, 0, on ? 0xFFFFFF : 0xD8D2C8, 255, lbl);
                }
                ui_fill(390, 120, 540, 540, 0x0A080A, 235);
                if (mcur) model_draw(390, 120, 540, 540);
                else ui_text(1, 660, 400, 1, 0x8A847C, 255, "Couldn't read this one");
                if (mcur) {
                    char info[96]; int tris = 0; for (int i = 0; i < mcur->nparts; i++) if (i == mcur->body || m_props || mcur->parts[i].primary) tris += mcur->parts[i].nt;
                    snprintf(info, sizeof info, "%s  -  %d triangles%s", ments[doc_sel].file, tris, mcur->nparts > 1 ? (m_props ? "  -  all parts" : "") : "");
                    ui_text(0, 920, 650, 2, 0x8A847C, 255, info);
                }
                ui_text(0, 60, 690, 0, 0x8A847C, 255, "Left/Right, drag: turn   +/-, wheel: zoom   Enter: all parts   Esc: back");
            }
        } else if (screen == S_READ || screen == S_ABOUT) {
            enum { VIS = 17 };
            if (dy) { read_top += dy; if (read_top > nlines - VIS) read_top = nlines - VIS; if (read_top < 0) read_top = 0; }
            if (cancel || act) { screen = screen == S_READ ? S_DOCS : S_EXTRAS; act = 0; }
            else {
                ui_text(2, 60, 64, 0, 0xE8E2D8, 255, screen == S_READ ? docs[doc_sel].title : "About this port");
                ui_fill(60, 84, 840, 2, 0x8E1F1A, 255);
                for (int i = 0; i < VIS && read_top + i < nlines; i++) ui_text(1, 70, 130 + i * 31, 0, 0xE8E2D8, 255, lines[read_top + i]);
                if (nlines > VIS) ui_text(0, 900, 690, 2, 0x8A847C, 255, "Up / Down to scroll");
                ui_text(0, 60, 690, 0, 0x8A847C, 255, "Esc / B: back");
            }
        }
        static int shot_wait = 2;
        if (shot && (!keys || !*keys) && --shot_wait <= 0) {     /* test hook: screenshot (a frame after the last key), then close */
            uint8_t *px = malloc((size_t)ow * oh * 4);
            SDL_RenderReadPixels(ui_r, NULL, SDL_PIXELFORMAT_ABGR8888, px, ow * 4);
            png_write(shot, px, ow, oh, 0); free(px);
            fprintf(stderr, "[options] saved %s (%dx%d)\n", shot, ow, oh);
            result = 0;
        }
        SDL_RenderPresent(ui_r);
        if (!shot) SDL_Delay(8);
    }
    if (pad) SDL_GameControllerClose(pad);
    for (int i = 0; i < 4; i++) if (icon_tex[i]) { SDL_DestroyTexture(icon_tex[i]); icon_tex[i] = NULL; }
    if (bg) SDL_DestroyTexture(bg);
    SDL_DestroyTexture(uf.tex); SDL_DestroyRenderer(ui_r); SDL_DestroyWindow(w);
    ui_r = NULL;
    return result;
}
static int options_window(void) { return options_window_ex(0); }
