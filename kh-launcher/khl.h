/*
 * Killers' Heaven launcher (khl): the shared launcher of the Evil Dead
 * Collection ports. Unofficial recompilation (c) 2026 Killers' Heaven, GPL-3.0.
 *
 * One look for every game: a title screen with Play / Options / Extras / Quit,
 * an options screen of pages and rows with a status tag on each row (works,
 * partly, not yet, PC only), remap screens for keys and controller buttons,
 * and an Extras screen with a text reader. Keyboard, mouse and game
 * controller all work. Drawn with SDL2's 2D renderer and stb_truetype
 * (Courier Prime, embedded) in a 960x720 logical canvas.
 *
 * The module knows nothing about any game. Each game fills in a khl_game
 * (its title, its option rows and callbacks that read and change its own
 * settings) and calls khl_run(). Each game ships its own copy of this folder,
 * so a game never needs another game to be installed. Keep the copies
 * identical; KHL_VERSION says which one a game has.
 *
 * An extra can also be a list of things read from the game (documents,
 * movies: khl_extra.count) or a screen of the game's own (a model viewer:
 * khl_extra.frame), drawn with the khl_text / khl_fill helpers below so it
 * keeps the same look.
 *
 * Test hooks: KHL_KEYS="down,down,right,enter" feeds one key per frame
 * (up down left right enter esc tab del, key:<SDL key name>), then
 * KHL_SHOT=file.bmp saves a screenshot and closes the window.
 */
#ifndef KHL_H
#define KHL_H

/* Games include SDL as <SDL.h> or <SDL2/SDL.h>; KHL_SDL_HEADER overrides. */
#if defined(KHL_SDL_HEADER)
#include KHL_SDL_HEADER
#elif defined(__has_include)
#if __has_include(<SDL.h>)
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#else
#include <SDL.h>
#endif
#include <stddef.h>
#include <stdint.h>

#define KHL_VERSION "1.1.0"

/* The logical canvas every screen is drawn in, scaled to the window. */
enum { KHL_CANVAS_W = 960, KHL_CANVAS_H = 720 };

/* The launcher's screens (khl_game.start_screen, khl_game.overlay). */
enum { KHL_SCREEN_HOME, KHL_SCREEN_OPTIONS, KHL_SCREEN_BINDS, KHL_SCREEN_EXTRAS,
       KHL_SCREEN_READ, KHL_SCREEN_LIST, KHL_SCREEN_CUSTOM };

/* What khl_game.play returns. */
enum {
    KHL_PLAY_STAY = 0,   /* stay in the launcher (say why with khl_message) */
    KHL_PLAY_GO = 1,     /* close the launcher and start the game */
    KHL_PLAY_WAIT = 2,   /* not ready yet: play() is asked again every frame until it says GO or STAY */
};

/* What an option does in this release. */
enum { KHL_WORKS, KHL_PARTIAL, KHL_NOT_YET, KHL_PC_ONLY };

/* Row kinds on an options page. */
enum {
    KHL_ROW_CHOICE,    /* left/right (or a click) steps the value: game->change(id, dir) */
    KHL_ROW_ACTION,    /* Enter runs game->activate(id) */
    KHL_ROW_BINDS,     /* Enter opens remap screen game->binds[row.binds] */
};

typedef struct khl_row {
    const char *label;
    const char *help;          /* one or two lines, split with '\n' */
    int kind;                  /* KHL_ROW_* */
    int id;                    /* the game's own id, passed back to its callbacks */
    int binds;                 /* KHL_ROW_BINDS: index into game->binds */
} khl_row;

typedef struct khl_page {
    const char *name;          /* tab name; one page shows no tabs */
    int nrows;
    const khl_row *rows;
} khl_page;

/* What a remap screen waits for after Enter. */
enum { KHL_CAPTURE_KEY, KHL_CAPTURE_PAD };

typedef struct khl_binds {
    const char *title;         /* "Keyboard controls" */
    const char *hint;          /* shown under the list; '\n' splits two lines */
    int capture;               /* KHL_CAPTURE_* */
    int n;
    const char *(*label)(int i);
    void (*value)(int i, char *out, size_t n);
    /* The captured event: an SDL_KEYDOWN for KHL_CAPTURE_KEY, an
     * SDL_CONTROLLERBUTTONDOWN or SDL_CONTROLLERAXISMOTION (pushed past half
     * way) for KHL_CAPTURE_PAD. Return 1 when bound, 0 to keep waiting, -1 when
     * the input can't be used (the module says so). */
    int (*set)(int i, const SDL_Event *e);
    void (*change)(int i, int dir);    /* optional: left/right steps the value */
    void (*clear)(int i);              /* optional: Delete or controller X clears it */
    void (*reset)(void);               /* "Reset to defaults" row */
} khl_binds;

/* One frame of input, for an extra with a screen of its own (khl_extra.frame). */
typedef struct khl_input {
    int dx, dy;            /* arrows, d-pad, left stick: -1/+1; dy is -10/+10 for Page Up/Down */
    int wheel;             /* -3/+3 when the mouse wheel turned (dy is set to it too) */
    int act, cancel, del, tab;   /* Enter/A, Esc/B/right click, Delete/X, Tab/LB/RB (-1/+1) */
    int click;             /* the left mouse button went down at mx, my */
    float mx, my;          /* the mouse in canvas coordinates (-1 until it moves) */
    float drag_x, drag_y;  /* mouse movement this frame with the left button held */
    SDL_GameController *pad;   /* the open controller, or NULL */
} khl_input;

typedef struct khl_extra {
    const char *label;
    const char *help;
    int status; const char *status_text;
    const char *(*text)(void);         /* opens the text reader with this text (NULL: nothing to show) */

    /* Optional, since 1.1.0. With count set the label shows the number of
     * items, and (unless frame is set) Enter opens a list of them: Enter on an
     * item shows item_text(i) in the reader, or runs item_run(i) (for example
     * a movie played in khl_renderer()). */
    int (*count)(void);
    void (*item)(int i, char *out, size_t n);      /* an item's name in the list */
    const char *(*item_text)(int i);
    void (*item_run)(int i);
    int columns;                       /* items side by side (0: one) */
    const char *list_sub;              /* the list screen's subtitle */
    const char *list_keys;             /* the list screen's key line (NULL: a default) */
    /* A screen of the extra's own: enter() when it opens, then frame() every
     * frame to handle `in` and draw (the background is already drawn). frame
     * returns 1 to go back to Extras. */
    void (*enter)(void);
    int (*frame)(const khl_input *in);
} khl_extra;

typedef struct khl_game {
    const char *window_title;  /* "Evil Dead: A Fistful of Boomstick" */
    const char *banner;        /* big red title on the home screen: "EVIL DEAD" */
    const char *subtitle;      /* "A Fistful of Boomstick" */
    const char *credit;        /* footer line */
    const char *about;         /* Extras -> About this port */

    int npages; const khl_page *pages;
    int nbinds; const khl_binds *binds;
    int nextras; const khl_extra *extras;   /* About this port and Back are added after these */

    void (*value)(int id, char *out, size_t n);
    void (*change)(int id, int dir);
    void (*activate)(int id);
    /* Optional: the status of row `id`. Unset rows say "Works.". */
    void (*status)(int id, int *lvl, const char **text);

    void (*load)(void);        /* (re)read the settings file */
    int  (*save)(void);        /* write it; 0 on failure */
    void (*defaults)(void);    /* reset everything (not saved until Save) */
    /* Play was chosen (settings are already saved). Returns KHL_PLAY_*:
     * GO (1) closes the launcher, STAY (0) stays (call khl_message to say
     * why), WAIT asks again next frame (Esc stops waiting). */
    int  (*play)(void);

    /* Optional: art behind the menus. `src` may be left as the whole texture. */
    SDL_Texture *(*background)(SDL_Renderer *r, SDL_Rect *src);

    /* Optional, since 1.1.0. */
    int start_screen;          /* KHL_SCREEN_HOME (0) or KHL_SCREEN_OPTIONS (an options-only shortcut) */
    /* Draws the game's own things over a screen, every frame: `screen` is a
     * KHL_SCREEN_*, `page` and `id` the options page and the selected row's id
     * (-1: none), `waiting` is 1 while Play waits for play() (KHL_PLAY_WAIT). */
    void (*overlay)(int screen, int page, int id, int waiting);
} khl_game;

/* Runs the launcher until the player plays (returns 1) or quits (returns 0).
 * SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) must have been called. */
int khl_run(const khl_game *g);

/* A short message at the bottom of the window for a few seconds.
 * lvl: KHL_WORKS (plain) or KHL_NOT_YET (an error, in red). */
void khl_message(int lvl, const char *fmt, ...);

/* The launcher's window, while khl_run is running (for native dialogs). */
SDL_Window *khl_window(void);

/* Drawing in the launcher's look, for overlays and an extra's own screen
 * (while khl_run is running; coordinates in the KHL_CANVAS_W x KHL_CANVAS_H canvas). */
enum { KHL_SMALL, KHL_NORMAL, KHL_TITLE };      /* text sizes: 19, 25, 40 px */
enum { KHL_LEFT, KHL_CENTRE, KHL_RIGHT };
SDL_Renderer *khl_renderer(void);
void  khl_text(int size, float x, float y, int align, uint32_t rgb, const char *s);   /* y is the baseline */
float khl_text_width(int size, const char *s);
void  khl_fill(float x, float y, float w, float h, uint32_t rgb, int alpha);
void  khl_selbar(float x, float y, float w, float h);   /* the selected-row bar */
void  khl_header(const char *title, const char *sub);   /* a screen's title and red subtitle */
void  khl_keys_line(const char *keys);                  /* the key help at the bottom */

#endif
