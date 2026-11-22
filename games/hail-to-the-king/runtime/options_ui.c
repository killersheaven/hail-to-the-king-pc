/* The launcher (launcher.bat: --launcher) and its options shortcut
 * (options.bat: --options). #included by platform_sdl.c after the
 * settings.ini code, so it edits the same `cfg` and saves with cfg_save().
 *
 * The screens themselves are the Evil Dead Collection's shared launcher
 * (kh-launcher/, the same module Fistful of Boomstick uses): this file only
 * says what Hail to the King's options are and adds its Extras, which read
 * documents, movies and character models from the player's own disc.
 *
 * Test hooks (kh-launcher): KHL_KEYS="down,down,right,enter" feeds keys, then
 * KHL_SHOT=file.bmp saves a screenshot and closes the window.
 */
#include <ctype.h>
#include <math.h>
#include "khl.h"
#include "third_party/stb_image.h"

int png_write(const char *path, const uint8_t *rgba, int w, int h, int bottom_up);

/* a PNG from the textures folder as a texture; *w,*h = its size */
static SDL_Texture *ui_load_png(SDL_Renderer *r, const char *path, int *w, int *h) {
    int n; unsigned char *px = stbi_load(path, w, h, &n, 4);
    if (!px) return NULL;
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, *w, *h);
    if (t) { SDL_UpdateTexture(t, NULL, px, *w * 4); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); }
    stbi_image_free(px);
    return t;
}

/* ---- the options ---- */
enum { R_DISPLAY, R_MONITOR, R_WINDOW, R_RENDER, R_ASPECT, R_FILTER, R_HD, R_UI, R_PROMPTS,
       R_KEYS, R_IMMORTAL, R_AMMO, R_FUEL, R_BLADE, NROWS };
static const khl_row rows_display[] = {
    { "Display", "Fullscreen uses your monitor's own resolution.\nWindowed opens a resizable window (F11 switches in game).", KHL_ROW_CHOICE, R_DISPLAY, 0 },
    { "Monitor", "Which monitor to use for fullscreen.", KHL_ROW_CHOICE, R_MONITOR, 0 },
    { "Window size", "Size of the window when not fullscreen.\nYou can also resize the window by dragging its edges.", KHL_ROW_CHOICE, R_WINDOW, 0 },
    { "Render resolution", "Internal resolution the game is drawn at.\nAuto matches your screen, which is the sharpest.", KHL_ROW_CHOICE, R_RENDER, 0 },
    { "Aspect ratio", "4:3 keeps the original shape with black bars on wide\nscreens. Stretch fills the screen (wider image).", KHL_ROW_CHOICE, R_ASPECT, 0 },
    { "Texture filtering", "Smooth is the original look. Sharp shows the game's\ntextures as unfiltered pixels.", KHL_ROW_CHOICE, R_FILTER, 0 },
    { "HD art", "High-resolution replacements for the game's own images\n(textures folder). Off shows the original images.", KHL_ROW_CHOICE, R_HD, 0 },
    { "Menu sharpening", "Sharpens menu and HUD art at high resolutions when no\nHD image replaces it. Text only is recommended.", KHL_ROW_CHOICE, R_UI, 0 },
};
static const khl_row rows_controls[] = {
    { "Keyboard controls", "Choose which keys act as the Dreamcast pad's buttons.\nPress Enter to change them.", KHL_ROW_BINDS, R_KEYS, 0 },
    { "Button prompts", "Which controller's buttons the menus show. Buttons are\nmatched by position: the bottom button is the 'A' button.", KHL_ROW_CHOICE, R_PROMPTS, 0 },
};
static const khl_row rows_cheats[] = {
    { "Immortality", "Testing aid: Ash's health stays full.", KHL_ROW_CHOICE, R_IMMORTAL, 0 },
    { "Unlimited ammo", "Testing aid: Knowby's pistol always has 999 rounds.", KHL_ROW_CHOICE, R_AMMO, 0 },
    { "Unlimited fuel", "Testing aid: the chainsaw never runs out of fuel.", KHL_ROW_CHOICE, R_FUEL, 0 },
    { "Damascene blade", "Testing aid: the chainsaw has the Damascene Steel blade\nfrom the start.", KHL_ROW_CHOICE, R_BLADE, 0 },
};
enum { PAGE_DISPLAY, PAGE_CONTROLS, PAGE_CHEATS };
static const khl_page pages[] = {
    { "Display", (int)(sizeof rows_display / sizeof *rows_display), rows_display },
    { "Controls", (int)(sizeof rows_controls / sizeof *rows_controls), rows_controls },
    { "Cheats", (int)(sizeof rows_cheats / sizeof *rows_cheats), rows_cheats },
};

/* what each option does in this release, on this platform */
static void row_status(int r, int *lvl, const char **text) {
    *lvl = KHL_WORKS; *text = "Works.";
#ifdef __ANDROID__
    if (r == R_DISPLAY || r == R_MONITOR || r == R_WINDOW) { *lvl = KHL_PC_ONLY; *text = "PC only: on Android the game always fills the screen."; return; }
    if (r == R_ASPECT) { *text = "Works: 4:3 puts black bars at the sides of a wide phone screen."; return; }
#endif
    if (r == R_HD) *text = "Works: HD font and legal screen. The title screen keeps its original art.";
    else if (r >= R_IMMORTAL) *text = "Works (testing aid).";
}
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
    case R_KEYS: out[0] = 0; break;                    /* the launcher shows "Enter to change" */
    default: snprintf(out, n, "%s", cfg.cheats >> (r - R_IMMORTAL) & 1 ? "On" : "Off"); break;
    }
}
static void row_change(int r, int d) {
    switch (r) {
    case R_DISPLAY: cfg.fullscreen = !cfg.fullscreen; break;
    case R_MONITOR: { int nd = SDL_GetNumVideoDisplays(); if (nd < 1) nd = 1; cfg.monitor = (cfg.monitor + d + nd) % nd; break; }
    case R_WINDOW: {      /* the sizes that fit the monitor */
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
    case R_KEYS: break;
    default: cfg.cheats ^= 1 << (r - R_IMMORTAL); break;
    }
}

/* ---- keyboard controls: keymap (platform_sdl.c), saved as key_* in settings.ini ---- */
static const char *key_row_label(int i) { return key_label[i]; }
static void key_row_value(int i, char *out, size_t n) { snprintf(out, n, "%s", SDL_GetScancodeName(keymap[i])); }
static int key_row_set(int i, const SDL_Event *e) {
    SDL_Scancode sc = e->key.keysym.scancode;
    if (key_reserved(sc)) return -1;                   /* Esc and F8-F12 are the hotkeys */
    for (int k = 0; k < NKEYS; k++) if (k != i && keymap[k] == sc) keymap[k] = keymap[i];   /* a key in use swaps */
    keymap[i] = sc;
    return 1;
}
static void key_rows_reset(void) { memcpy(keymap, key_default, sizeof keymap); }
static const khl_binds binds[] = {
    { "Keyboard controls", "Enter: change the key. A key already in use swaps with this one.\nEsc and F8-F12 stay hotkeys. Save on the Options screen to keep it.",
      KHL_CAPTURE_KEY, NKEYS, key_row_label, key_row_value, key_row_set, NULL, NULL, key_rows_reset },
};

/* ---- button icons for the prompts preview: the sheet's icon row holds Y B A X (top, right, bottom, left) ---- */
static SDL_Texture *icon_tex[4]; static int icon_scale[4];
static const uint32_t icon_rgb[4][4] = {
    { 0x3FB54A, 0x2D7DD2, 0xE0333A, 0xF2C21B },    /* Dreamcast: Y green, B blue, A red, X yellow */
    { 0xF2C21B, 0xE0333A, 0x3FB54A, 0x2D7DD2 },    /* Xbox */
    { 0x3BB89A, 0xE5505A, 0x6A8CE8, 0xD98ACB },    /* PlayStation */
    { 0xE8E8E8, 0xE8E8E8, 0xE8E8E8, 0xE8E8E8 } };  /* Switch */
static int icon_y0[4];
static void load_icons(SDL_Renderer *r, const char *base) {
    static const char *dirs[4] = { "", "prompts/xbox/hd/", "prompts/playstation/hd/", "prompts/switch/hd/" };
    for (int i = 0; i < 4; i++) {
        char p[1200]; int w, h;
        snprintf(p, sizeof p, "%stextures/%s9B28D9CBB4D57B31_256x256.png", base, dirs[i]);
        icon_tex[i] = ui_load_png(r, p, &w, &h);
        icon_scale[i] = icon_tex[i] ? w / 256 : 0; icon_y0[i] = 144;
        if (!icon_tex[i]) {   /* release builds: just the four icons (prompticons.py), 4x */
            static const char *st[4] = { "dreamcast", "xbox", "playstation", "switch" };
            snprintf(p, sizeof p, "%stextures/prompts/%s/icons.png", base, st[i]);
            icon_tex[i] = ui_load_png(r, p, &w, &h); icon_scale[i] = 4; icon_y0[i] = 0;
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
        SDL_RenderCopyF(khl_renderer(), t, &src, &dst);
    }
}

/* ---- the Extras ---- */
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
        mtex = SDL_CreateTexture(khl_renderer(), SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, mcur->tw, mcur->th);
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
    SDL_Rect clip = { (int)bx, (int)by, (int)bw, (int)bh }; SDL_RenderSetClipRect(khl_renderer(), &clip);
    if (SDL_RenderGeometry(khl_renderer(), mtex, sorted, (int)k, NULL, 0) < 0) { static int w; if (!w++) fprintf(stderr, "[models] draw failed: %s\n", SDL_GetError()); }
    { static int once; if (!once++ && getenv("DC_MODELS_DEBUG")) fprintf(stderr, "[models] %zu verts, first at %.1f,%.1f tex %p\n", k, k ? sorted[0].position.x : 0.f, k ? sorted[0].position.y : 0.f, (void *)mtex); }
    SDL_RenderSetClipRect(khl_renderer(), NULL);
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
            else if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_AC_BACK || e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_BACKSPACE)) stop = 1;
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
                tex = SDL_CreateTexture(khl_renderer(), SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, tw, th);
            }
            SDL_UpdateYUVTexture(tex, NULL, f->y.data, (int)f->y.width, f->cb.data, (int)f->cb.width, f->cr.data, (int)f->cr.width);
            shown++; fresh = 1;
        }
        if (stop) break;
        if (fresh) {
            SDL_SetRenderDrawColor(khl_renderer(), 0, 0, 0, 255); SDL_RenderClear(khl_renderer());
            if (tex) {   /* keep the movie's 4:3 shape */
                int w = KHL_CANVAS_W, h = KHL_CANVAS_W * 3 / 4; if (h > KHL_CANVAS_H) { h = KHL_CANVAS_H; w = KHL_CANVAS_H * 4 / 3; }
                SDL_Rect dst = { (KHL_CANVAS_W - w) / 2, (KHL_CANVAS_H - h) / 2, w, h }; SDL_RenderCopy(khl_renderer(), tex, NULL, &dst); }
            if (shot && shown >= shot_frame && shot_frame >= 0) {
                int ow, oh; SDL_GetRendererOutputSize(khl_renderer(), &ow, &oh);
                uint8_t *px = malloc((size_t)ow * oh * 4); SDL_RenderReadPixels(khl_renderer(), NULL, SDL_PIXELFORMAT_ABGR8888, px, ow * 4);
                png_write(shot, px, ow, oh, 0); free(px); fprintf(stderr, "[movie] %s frame %ld saved\n", name, shown); stop = 1;
            }
            SDL_RenderPresent(khl_renderer());
        } else SDL_Delay(2);
    }
    if (ad) SDL_CloseAudioDevice(ad);
    if (tex) SDL_DestroyTexture(tex);
    sfd_close(m);
}
static int docs_count(void) { return ndocs; }
static void docs_item(int i, char *out, size_t n) { snprintf(out, n, "%s", docs[i].title); }
static const char *docs_text(int i) { return docs[i].body; }

static int movies_count(void) { return nmovies; }
static void movies_item(int i, char *out, size_t n) {
    char nm[16]; snprintf(nm, sizeof nm, "%s", movies[i].name);
    char *dot = strchr(nm, '.'); if (dot) *dot = 0;
    snprintf(out, n, "%s  %.1f MB", nm, movies[i].size / 1048576.0);
}
static void movies_run(int i) { play_movie(movies[i].name); }

/* the model viewer: a list on the left, the model turning on the right */
static int models_count(void) { return nments; }
static int models_sel, models_top;
static void models_enter(void) { models_sel = models_top = 0; model_select(0); }
static int models_frame(const khl_input *in) {
    enum { VIS = 15 };
    int i;
    if (in->cancel) return 1;
    for (i = 0; i < VIS && models_top + i < nments; i++)
        if (in->mx >= 0 && in->my >= 150 + i * 34 - 25 && in->my < 150 + i * 34 + 9 && in->mx > 40 && in->mx < 360) models_sel = models_top + i;
    if (in->dy && !in->wheel) models_sel += in->dy > 0 ? 1 : -1;
    if (in->wheel && in->mx > 380) m_zoom *= in->wheel < 0 ? 1.12f : 1 / 1.12f;     /* the wheel over the model zooms */
    else if (in->wheel) models_sel += in->wheel > 0 ? 3 : -3;
    if (models_sel >= nments) models_sel = nments - 1;
    if (models_sel < 0) models_sel = 0;
    if (models_sel < models_top) models_top = models_sel;
    if (models_sel >= models_top + VIS) models_top = models_sel - VIS + 1;
    model_select(models_sel);
    {   /* turn, tilt and zoom: held keys, the sticks and triggers, or a drag over the model */
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        float spin = 0, tilt = 0, zm = 0, drag_x = in->mx > 380 ? in->drag_x : 0, drag_y = in->mx > 380 ? in->drag_y : 0;
        if (ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_A]) spin -= 1;
        if (ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_D]) spin += 1;
        if (ks[SDL_SCANCODE_W]) tilt += 1;
        if (ks[SDL_SCANCODE_S]) tilt -= 1;
        if (ks[SDL_SCANCODE_EQUALS] || ks[SDL_SCANCODE_KP_PLUS] || ks[SDL_SCANCODE_PAGEUP]) zm += 1;
        if (ks[SDL_SCANCODE_MINUS] || ks[SDL_SCANCODE_KP_MINUS] || ks[SDL_SCANCODE_PAGEDOWN]) zm -= 1;
        if (in->pad) {
            float rx = SDL_GameControllerGetAxis(in->pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.f, ry = SDL_GameControllerGetAxis(in->pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.f;
            float lx2 = SDL_GameControllerGetAxis(in->pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.f;
            if (fabsf(rx) > 0.2f) spin += rx; else if (fabsf(lx2) > 0.3f) spin += lx2;
            if (fabsf(ry) > 0.2f) tilt -= ry;
            zm += SDL_GameControllerGetAxis(in->pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.f - SDL_GameControllerGetAxis(in->pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.f;
        }
        static Uint32 idle_since; Uint32 now = SDL_GetTicks();
        if (spin || tilt || zm || drag_x || drag_y) idle_since = now;
        m_yaw += spin * 0.035f + drag_x * 0.01f; m_pitch += tilt * 0.025f + drag_y * 0.01f;
        if (m_pitch > 1.4f) m_pitch = 1.4f;
        if (m_pitch < -1.4f) m_pitch = -1.4f;
        m_zoom *= 1 + zm * 0.02f; if (m_zoom < 0.3f) m_zoom = 0.3f; if (m_zoom > 6) m_zoom = 6;
        if (now - idle_since > 2500) m_yaw += 0.006f;                 /* turn slowly when left alone */
    }
    if (in->act) m_props = !m_props;
    khl_header("Models", "Read from your disc");
    for (i = 0; i < VIS && models_top + i < nments; i++) {
        float y = 150 + i * 34; int on = models_sel == models_top + i;
        char lbl[48]; ment_label(models_top + i, lbl, sizeof lbl);
        if (on) khl_selbar(40, y - 25, 330, 32);
        khl_text(KHL_SMALL, 60, y - 2, KHL_LEFT, on ? 0xFFFFFF : 0xD8D2C8, lbl);
    }
    khl_fill(390, 120, 540, 540, 0x0A080A, 235);
    if (mcur) {
        char info[96]; int tris = 0;
        model_draw(390, 120, 540, 540);
        for (i = 0; i < mcur->nparts; i++) if (i == mcur->body || m_props || mcur->parts[i].primary) tris += mcur->parts[i].nt;
        snprintf(info, sizeof info, "%s  -  %d triangles%s", ments[models_sel].file, tris, mcur->nparts > 1 ? (m_props ? "  -  all parts" : "") : "");
        khl_text(KHL_SMALL, 920, 650, KHL_RIGHT, 0x8A847C, info);
    } else khl_text(KHL_NORMAL, 660, 400, KHL_CENTRE, 0x8A847C, "Couldn't read this one");
    khl_keys_line("Left/Right, drag: turn   +/-, wheel: zoom   Enter: all parts   Esc: back");
    return 0;
}

static const khl_extra extras[] = {
    { "Documents", "Every note, journal and page Ash can find, read from your disc.", KHL_WORKS, NULL, NULL,
      docs_count, docs_item, docs_text, NULL, 1, "Found on your disc", "Enter / A: read     Esc / B: back", NULL, NULL },
    { "Cutscenes", "Watch the game's movies, played from your disc.", KHL_PARTIAL, "Partly: the 56 Sofdec movies play; the 62 AVI movies not yet.", NULL,
      movies_count, movies_item, NULL, movies_run, 3, "Played from your disc", "Enter / A: play     Esc / B: back (also stops a movie)", NULL, NULL },
    { "Models", "Turn and inspect the game's characters, read from your disc.", KHL_PARTIAL, "Partly: 35 characters, rest pose only (no animations yet).", NULL,
      models_count, NULL, NULL, NULL, 0, NULL, NULL, models_enter, models_frame },
};

#ifndef HTTK_VERSION
#define HTTK_VERSION "0.16.0"
#endif
static const char *about_text =
    "Evil Dead: Hail to the King (Dreamcast, 2000) by Heavy Iron Studios and THQ, "
    "running natively on this computer through static recompilation: the game's own "
    "SH-4 machine code is translated to C ahead of time and runs on an emulation of "
    "the Dreamcast's graphics, sound and controller hardware.\n\n"
    "Version " HTTK_VERSION ". Unofficial recompilation (c) 2026 Killers' Heaven, "
    "free software under the GNU GPL v3.\n\n"
    "All game content is read from your own disc image; none is included.\n\n"
    "Fonts: Courier Prime and EB Garamond (SIL Open Font License). "
    "Libraries: SDL2, pl_mpeg, stb_image and stb_truetype. Cheat addresses from the published "
    "Dreamcast CodeBreaker codes.\n\n"
    "Evil Dead and its related characters are trademarks of their respective owners. "
    "This port is not affiliated with or endorsed by them.";

/* The one-click build prepares the game code on a second thread while the launcher is
 * open (sh4_code_progress, code.h: 0..1000, or below 0 on failure). Play waits for it. */
extern volatile int sh4_code_progress;
static int ui_play(void) {
    int pr = sh4_code_progress;
    if (pr >= 1000) return KHL_PLAY_GO;
    if (pr < 0) { khl_message(KHL_NOT_YET, "Couldn't prepare the game from your disc: see log.txt."); return KHL_PLAY_STAY; }
    return KHL_PLAY_WAIT;
}
static void ui_overlay(int screen, int page, int id, int waiting) {
    int pr = sh4_code_progress;
    (void)id;
    if (screen == KHL_SCREEN_HOME || waiting) {      /* how far the game code has got */
        if (pr >= 0 && pr < 1000) {
            char t[96];
            snprintf(t, sizeof t, "Preparing the game from your disc... %d%%", pr / 10);
            khl_text(KHL_SMALL, 480, 648, KHL_CENTRE, 0xD8D2C8, t);
            khl_fill(330, 658, 300, 6, 0x2A2224, 255);
            khl_fill(330, 658, 300.f * (float)pr / 1000.f, 6, 0xB3261E, 255);
        } else if (pr < 0) khl_text(KHL_SMALL, 480, 652, KHL_CENTRE, 0xE0453B, "The game could not be prepared from your disc: see log.txt");
    }
    if (screen == KHL_SCREEN_OPTIONS && page == PAGE_CONTROLS && icon_tex[cfg.prompts & 3]) {   /* the menus' buttons, as chosen */
        draw_pad(700, 360, cfg.prompts & 3);
        khl_text(KHL_SMALL, 700, 440, KHL_CENTRE, 0x8A847C, prompt_label[cfg.prompts & 3]);
    }
}

static char ui_base[1024];
/* the title art (with the HD logo) behind the menus, if the textures folder has it */
static SDL_Texture *ui_background(SDL_Renderer *r, SDL_Rect *src) {
    char p[1200]; int bw = 0, bh = 0;
    load_icons(r, ui_base);
    snprintf(p, sizeof p, "%stextures/3BA3ED33C3D7FC12_512x256.png", ui_base);
    SDL_Texture *bg = ui_load_png(r, p, &bw, &bh);
    /* the 4:3 picture area only: the texture holds other data below it */
    src->x = 0; src->y = 0; src->w = bw * 320 / 512 - 4; src->h = bh * 240 / 256 - 4;
    return bg;
}
static int ui_save(void) { return cfg_save(); }
static void ui_defaults(void) { cfg_defaults(); }
static void ui_value(int id, char *out, size_t n) { row_value(id, out, n); }

/* launcher = 1: the home screen (launcher.bat); 0: straight to Options (options.bat).
 * Returns 1 = play, 0 = quit. */
static int options_window_ex(int launcher) {
    static int scanned;
    khl_game g = {
        "Evil Dead: Hail to the King", "EVIL DEAD", "Hail to the King",
        "Unofficial recompilation (c) 2026 Killers' Heaven", about_text,
        (int)(sizeof pages / sizeof *pages), pages, 1, binds,
        (int)(sizeof extras / sizeof *extras), extras,
        ui_value, row_change, NULL, row_status,
        cfg_load, ui_save, ui_defaults, ui_play, ui_background,
        launcher ? KHL_SCREEN_HOME : KHL_SCREEN_OPTIONS, ui_overlay,
    };
    { char *b = SDL_GetBasePath(); if (b) { snprintf(ui_base, sizeof ui_base, "%s", b); SDL_free(b); } }
    if (!scanned) {      /* the Extras: what's on the player's disc */
        scanned = 1;
        load_docs();
        nmovies = 0; iso_list_cb(movie_cb, NULL);
        if (getenv("DC_MOVIE_DIR")) { movie_cb("AX00.SFD", 4u << 20, NULL); movie_cb("AX01A.SFD", 40u << 20, NULL); }
        nments = models_scan(ments, 96); qsort(ments, (size_t)nments, sizeof *ments, ment_cmp);
    }
    int r = khl_run(&g);
    /* the launcher's renderer is gone, and its textures with it */
    for (int i = 0; i < 4; i++) icon_tex[i] = NULL;
    model_free(mcur); mcur = NULL; mcur_i = -1; mtex = NULL;
    return r;
}
