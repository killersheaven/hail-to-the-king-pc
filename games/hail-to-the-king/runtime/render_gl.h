#pragma once
#include <stdint.h>
#include "pvr.h"
int  render_gl_init(void *(*getproc)(const char *name), int scale_factor, int desktop_gl);
void render_gl_frame(const pvr_frame *f);
void render_gl_read(uint8_t *rgba, int *w, int *h);    /* rows bottom-up */
void render_gl_present(int win_w, int win_h);
void render_gl_resize(int w, int h);    /* render-target size; the game still draws in 640x480 units */
int  render_gl_height(void);
void render_gl_set_stretch(int on);    /* 1: fill the whole window; 0: keep 4:3 with black bars */
void render_gl_set_ui_mode(int m);     /* menu/HUD upscale: 0 off, 1 text only, 2 all */
void render_gl_set_options(int sharp_textures, int hd_textures, int prompt_style);   /* prompt_style: 0 Dreamcast, 1 Xbox, 2 PlayStation, 3 Switch */
