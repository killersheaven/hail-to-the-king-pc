/* OpenGL ES 3.0 renderer for the captured PowerVR2 display lists.
 *
 * Draws one pvr_frame into an offscreen framebuffer (RTT). The platform layer
 * (SDL window or headless EGL) presents or saves it. GL entry points are
 * loaded through a callback so the same code runs on Windows, Linux, macOS
 * and Android.
 *
 * PowerVR semantics implemented from public documentation:
 *  - vertices are screen x/y plus z = 1/w; depth comparisons are on 1/w
 *  - ISP word: depth compare mode, culling, Z-write disable, texture/offset/gouraud/16-bit UV
 *  - TSP word: blend factors, use-alpha, ignore-texture-alpha, flip/clamp UV, filter, shading, size
 *  - TCW: pixel format, twiddled/stride, VQ, mipmaps, palette selector, address
 */
#include "pvr.h"
#include "render_gl.h"
#include <GLES3/gl3.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t hw_reg(uint32_t a);

/* ---------------- GL loader ---------------- */
#define GLFUNCS(X) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM4FPROC, glUniform4f) X(PFNGLUNIFORM4IVPROC, glUniform4iv) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLDELETETEXTURESPROC, glDeleteTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLTEXIMAGE2DPROC, glTexImage2D) X(PFNGLTEXPARAMETERIPROC, glTexParameteri) X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
    X(PFNGLVIEWPORTPROC, glViewport) X(PFNGLCLEARCOLORPROC, glClearColor) X(PFNGLCLEARPROC, glClear) \
    X(PFNGLENABLEPROC, glEnable) X(PFNGLDISABLEPROC, glDisable) X(PFNGLDEPTHFUNCPROC, glDepthFunc) X(PFNGLDEPTHMASKPROC, glDepthMask) \
    X(PFNGLBLENDFUNCPROC, glBlendFunc) X(PFNGLCULLFACEPROC, glCullFace) X(PFNGLDRAWARRAYSPROC, glDrawArrays) \
    X(PFNGLREADPIXELSPROC, glReadPixels) X(PFNGLGETSTRINGPROC, glGetString) X(PFNGLGETERRORPROC, glGetError) \
    X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) X(PFNGLPIXELSTOREIPROC, glPixelStorei) X(PFNGLSCISSORPROC, glScissor)
#define DECL(t, n) static t p_##n;
GLFUNCS(DECL)
typedef void (*PFNCLEARDEPTHD)(double);
static PFNGLCLEARDEPTHFPROC p_glClearDepthf; static PFNCLEARDEPTHD p_glClearDepth;
static void clear_depth1(void) { if (p_glClearDepthf) p_glClearDepthf(1.0f); else if (p_glClearDepth) p_glClearDepth(1.0); }
#define LOAD(t, n) p_##n = (t)getproc(#n); if (!p_##n) { fprintf(stderr, "[gl] missing %s\n", #n); ok = 0; }

/* ---------------- shaders ---------------- */
static const char *VS =
    "layout(location=0) in vec3 pos; layout(location=1) in vec4 col; layout(location=2) in vec4 ofs; layout(location=3) in vec2 uv;\n"
    "uniform vec4 scr;\n"
    "out vec4 vcol; out vec4 vofs; out vec2 vuv; flat out vec4 fcol; flat out vec4 fofs;\n"
    "void main() {\n"
    "  float W = pos.z > 1e-12 ? 1.0 / pos.z : 1e12;\n"
    "  vcol = col.zyxw; vofs = ofs.zyxw; fcol = vcol; fofs = vofs; vuv = uv;\n"
    "  gl_Position = vec4((pos.x * scr.x + scr.z) * W, (pos.y * scr.y + scr.w) * W, 0.0, W);\n"
    "}\n";
static const char *FS =
    "precision highp float;\n"
    "in vec4 vcol; in vec4 vofs; in vec2 vuv; flat in vec4 fcol; flat in vec4 fofs;\n"
    "uniform sampler2D tex; uniform sampler2D acc2; uniform ivec4 mode; uniform ivec4 mode2; uniform float ptref;\n"
    /* mode: x tex on, y shading instr, z use alpha, w ignore tex alpha; mode2: x offset, y gouraud, z punch-through */
    "out vec4 o;\n"
    "void main() {\n"
    "  vec4 c = mode2.y == 1 ? vcol : fcol; vec4 of = mode2.y == 1 ? vofs : fofs;\n"
    "  if (mode.z == 0) c.a = 1.0;\n"
    "  if (mode.x == 1) {\n"
    "    vec4 t = texture(tex, vuv); if (mode.w == 1) t.a = 1.0;\n"
    "    if (mode.y == 0) c = t;\n"
    "    else if (mode.y == 1) c = vec4(t.rgb * c.rgb, t.a);\n"
    "    else if (mode.y == 2) c = vec4(mix(c.rgb, t.rgb, t.a), c.a);\n"
    "    else c = t * c;\n"
    "    if (mode2.x == 1) c.rgb = clamp(c.rgb + of.rgb, 0.0, 1.0);\n"
    "  }\n"
    "  if (mode2.z == 1 && c.a < ptref) discard;\n"
    "  if (mode2.w == 1) c = texelFetch(acc2, ivec2(gl_FragCoord.xy), 0);\n"   /* source select: secondary accumulation buffer */
    "  o = c;\n"
    "  gl_FragDepth = 1.0 / (1.0 + gl_FragCoord.w);\n"
    "}\n";

static GLuint prog, vao, vbo, fbo, fb_tex, fb_depth, white_tex;
static GLuint fbo2, acc2_tex, fbo3, acc2_copy;      /* secondary accumulation buffer + a copy to sample from */
static int acc2_dirty, cur_fbo;
static GLint u_scr, u_mode, u_mode2, u_ptref, u_tex, u_acc2;
/* options (settings.ini via the platform layer) */
static int opt_sharp;                     /* texture_filter = sharp: nearest-neighbour for the game's own textures */
static int opt_hd = 1;                    /* hd_textures: use the textures/ folder */
static int opt_prompts;                   /* 0 Dreamcast, 1 Xbox, 2 PlayStation, 3 Switch */
static int fb_w = 640, fb_h = 480, scale = 1;
static int rt_w = 640, rt_h = 480;          /* render-target size in pixels (any size; the game draws in 640x480 units) */

static int gl_desktop;
static GLuint compile(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    const char *srcs[2] = { gl_desktop ? "#version 330 core\n" : "#version 300 es\n", src };
    p_glShaderSource(s, 2, srcs, NULL); p_glCompileShader(s);
    GLint ok; p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "[gl] shader: %s\n", log); }
    return s;
}

static void alloc_targets(void) {
    p_glBindTexture(GL_TEXTURE_2D, fb_tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rt_w, rt_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glBindRenderbuffer(GL_RENDERBUFFER, fb_depth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, rt_w, rt_h);
    p_glBindTexture(GL_TEXTURE_2D, acc2_tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rt_w, rt_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glBindTexture(GL_TEXTURE_2D, acc2_copy);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rt_w, rt_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}
/* render at w x h pixels (the 4:3 area of the window); takes effect from the next frame */
void render_gl_resize(int w, int h) {
    if (w < 64 || h < 48 || (w == rt_w && h == rt_h)) return;
    if (w > 8192) w = 8192;
    if (h > 6144) h = 6144;
    rt_w = w; rt_h = h;
    alloc_targets();
    fprintf(stderr, "[gl] rendering at %dx%d\n", w, h);
}
int render_gl_height(void) { return rt_h; }

int render_gl_init(void *(*getproc)(const char *name), int scale_factor, int desktop_gl) {
    int ok = 1;
    gl_desktop = desktop_gl;
    GLFUNCS(LOAD)
    p_glClearDepthf = (PFNGLCLEARDEPTHFPROC)getproc("glClearDepthf");
    p_glClearDepth = (PFNCLEARDEPTHD)getproc("glClearDepth");
    if (!ok) return 0;
    scale = scale_factor < 1 ? 1 : scale_factor;
    if (getenv("DC_PROMPTS")) opt_prompts = atoi(getenv("DC_PROMPTS")) & 3;   /* test switches (the SDL build uses settings.ini) */
    if (getenv("DC_SHARP")) opt_sharp = 1;
    if (getenv("DC_NOHD")) opt_hd = 0;
    fprintf(stderr, "[gl] %s | %s\n", p_glGetString(GL_RENDERER), p_glGetString(GL_VERSION));
    prog = p_glCreateProgram();
    p_glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
    p_glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    p_glLinkProgram(prog);
    GLint lk; p_glGetProgramiv(prog, GL_LINK_STATUS, &lk);
    if (!lk) { char log[2048]; p_glGetProgramInfoLog(prog, sizeof log, NULL, log); fprintf(stderr, "[gl] link: %s\n", log); return 0; }
    u_scr = p_glGetUniformLocation(prog, "scr"); u_mode = p_glGetUniformLocation(prog, "mode");
    u_mode2 = p_glGetUniformLocation(prog, "mode2"); u_ptref = p_glGetUniformLocation(prog, "ptref");
    u_tex = p_glGetUniformLocation(prog, "tex"); u_acc2 = p_glGetUniformLocation(prog, "acc2");

    p_glGenVertexArrays(1, &vao); p_glBindVertexArray(vao);
    p_glGenBuffers(1, &vbo); p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glEnableVertexAttribArray(0); p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(pvr_vert), (void *)offsetof(pvr_vert, x));
    p_glEnableVertexAttribArray(1); p_glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(pvr_vert), (void *)offsetof(pvr_vert, col));
    p_glEnableVertexAttribArray(2); p_glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(pvr_vert), (void *)offsetof(pvr_vert, ofs));
    p_glEnableVertexAttribArray(3); p_glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(pvr_vert), (void *)offsetof(pvr_vert, u));

    rt_w = fb_w * scale; rt_h = fb_h * scale;
    p_glGenTextures(1, &fb_tex); p_glBindTexture(GL_TEXTURE_2D, fb_tex);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glGenRenderbuffers(1, &fb_depth);
    p_glGenTextures(1, &acc2_tex); p_glGenTextures(1, &acc2_copy);
    alloc_targets();
    p_glGenFramebuffers(1, &fbo); p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_tex, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fb_depth);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "[gl] framebuffer incomplete\n"); return 0; }

    uint32_t w = 0xFFFFFFFFu;
    for (int k = 0; k < 2; k++) {
        GLuint *t = k ? &acc2_copy : &acc2_tex, *fb = k ? &fbo3 : &fbo2;
        p_glBindTexture(GL_TEXTURE_2D, *t);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glGenFramebuffers(1, fb); p_glBindFramebuffer(GL_FRAMEBUFFER, *fb);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *t, 0);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fb_depth);
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glGenTextures(1, &white_tex); p_glBindTexture(GL_TEXTURE_2D, white_tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &w);
    return 1;
}

/* ---------------- textures ---------------- */
static inline uint16_t v16(uint32_t a) { uint16_t x; memcpy(&x, vram + vram64_off(a), 2); return x; }
static inline uint8_t  v8(uint32_t a)  { return vram[vram64_off(a)]; }

static uint32_t argb_to_rgba(uint32_t c) {           /* -> bytes R,G,B,A in memory */
    return (c >> 16 & 0xFF) | (c & 0xFF00) | (c & 0xFF) << 16 | (c & 0xFF000000u);
}
static uint32_t conv16(uint16_t p, int fmt) {
    uint32_t a, r, g, b;
    switch (fmt) {
    case 0: a = p & 0x8000 ? 255 : 0; r = (p >> 10 & 31) * 255 / 31; g = (p >> 5 & 31) * 255 / 31; b = (p & 31) * 255 / 31; break;
    case 1: a = 255; r = (p >> 11 & 31) * 255 / 31; g = (p >> 5 & 63) * 255 / 63; b = (p & 31) * 255 / 31; break;
    case 2: a = (p >> 12 & 15) * 17; r = (p >> 8 & 15) * 17; g = (p >> 4 & 15) * 17; b = (p & 15) * 17; break;
    default: a = 255; r = g = b = p >> 8; break;    /* bump map: show height as grey */
    }
    return r | g << 8 | b << 16 | a << 24;
}
static uint32_t palette(uint32_t idx) {
    uint32_t e = hw_reg(0x005F9000u + 4 * (idx & 1023));
    int pf = hw_reg(0x005F8108u) & 3;
    if (pf == 3) return argb_to_rgba(e);
    return conv16((uint16_t)e, pf);
}
static uint32_t yuv(int y, int u, int v) {
    float R = y + 1.375f * (v - 128), G = y - 0.6875f * (v - 128) - 0.34375f * (u - 128), B = y + 1.71875f * (u - 128);
    #define CL(x) ((uint32_t)((x) < 0 ? 0 : (x) > 255 ? 255 : (x)))
    return CL(R) | CL(G) << 8 | CL(B) << 16 | 0xFF000000u;
}
static uint32_t twiddle(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t m = w < h ? w : h, r = 0;
    uint32_t bx = x / m, by = y / m; x %= m; y %= m;
    for (uint32_t b = 0, s = 1; s < m; b++, s <<= 1) r |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
    return r + (bx + by) * m * m;
}
static int ilog2(uint32_t v) { int n = 0; while ((1u << n) < v) n++; return n; }

typedef struct { uint32_t key_tcw, key_tsp, pal_key, gen_sum; uint32_t a0, a1; GLuint tex; uint32_t last_used, updates; uint8_t never_ui, uploaded_ui, hd, legal; } texent;
#define NTEX 1024
static texent tcache[NTEX];
static uint32_t frame_no;
static uint32_t *tbuf; static size_t tbuf_cap;

static uint32_t gen_sum(uint32_t a0, uint32_t a1) {
    uint32_t s = 0;
    for (uint32_t p = a0 >> 12; p <= (a1 >> 12) && p < 2048; p++) s = s * 31 + vram_gen[p];
    return s;
}

/* ---------------- texture packs ----------------
 * Every decoded texture is named by a hash of its pixels: <hash>_<w>x<h>.png.
 *   DC_TEXDUMP=<dir>  write each new texture there once (dynamic textures such as movie frames are skipped)
 *   DC_TEXPACK=<dir>  (default "textures") load <dir>/<name>.png instead when it exists, at any resolution
 * so a pack can be made by upscaling the dumped images and keeping their names. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_OVERRIDE
#include "third_party/stb_image.h"
int png_write(const char *path, const uint8_t *rgba, int w, int h, int bottom_up);
static const char *dump_dir, *pack_dir; static int tp_init, dump_ui_only;
static uint64_t *dumped; static size_t ndumped, cap_dumped;
uint32_t texpack_loaded, texpack_dumped;
static uint64_t hash_pixels(const uint32_t *p, size_t n, uint32_t w, uint32_t h) {
    uint64_t x = 1469598103934665603ull ^ ((uint64_t)w << 32 | h);
    for (size_t i = 0; i < n; i++) { x ^= p[i]; x *= 1099511628211ull; x ^= x >> 29; }
    return x;
}
uint32_t *upscale_rgba(const uint32_t *src, int w, int h, int factor);
/* DC_UIUPSCALE: "text" (default) = only text/font sheets, "all" = all menu/HUD art, "0" = off */
static int ui_mode = -1;                  /* 0 off, 1 text only, 2 all */
static const char *prompt_dir[4] = { NULL, "xbox", "playstation", "switch" };
void render_gl_set_options(int sharp, int hd, int prompts) { opt_sharp = sharp; opt_hd = hd; opt_prompts = prompts & 3; }
#define FONT_SHEET_HASH 0x9B28D9CBB4D57B31ull
static uint32_t font_tex_addr = ~0u;      /* texture memory address of the font/button sheet, once seen */
static int last_upload_hd, last_upload_legal;
#define LEGAL_SHEET_HASH 0x33522E637940C0F6ull
void render_gl_set_ui_mode(int m) { ui_mode = m; }
static int ui_factor(void) {               /* menu/HUD art upscale factor for the current render size */
    if (ui_mode < 0) {
        const char *e = getenv("DC_UIUPSCALE");
        ui_mode = !e ? 1 : !strcmp(e, "0") ? 0 : !strcmp(e, "all") ? 2 : 1;
    }
    if (!ui_mode) return 0;
    return rt_h > 960 ? 4 : rt_h > 480 ? 2 : 0;
}
/* text/font sheet: (nearly) every visible pixel is grey/white, i.e. lettering, not pictures */
static int is_text_sheet(const uint32_t *px, uint32_t w, uint32_t h) {
    uint32_t vis = 0, color = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t v = px[i]; if ((v >> 24) < 48) continue;
        int r = v & 0xFF, g = v >> 8 & 0xFF, b = v >> 16 & 0xFF;
        int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        vis++; if (mx - mn > 40) color++;
    }
    return vis > 64 && color * 50 < vis;   /* under 2% coloured pixels */
}
uint32_t ui_upscaled;
static uint32_t cur_tex_addr;
/* Button prompts without an image pack: paint the controller's icons (textures/prompts/<style>/icons.png,
 * four 18x24 cells at 4x, see tools/fontpack/prompticons.py) over the icon row of the game's own font
 * sheet (y 144-168, cells Y B A X) as it is uploaded. */
static const uint8_t *prompt_icons(int st) {
    static uint8_t *icons[4]; static int tried[4];
    if (!st) return NULL;
    if (!tried[st]) {
        tried[st] = 1; char p[1024]; int iw, ih, n;
        snprintf(p, sizeof p, "%s/prompts/%s/icons.png", pack_dir ? pack_dir : "textures", prompt_dir[st]);
        icons[st] = stbi_load(p, &iw, &ih, &n, 4);
        if (icons[st] && (iw != 288 || ih != 96)) { stbi_image_free(icons[st]); icons[st] = NULL; }
        fprintf(stderr, "[prompts] %s %s\n", p, icons[st] ? "loaded" : "missing");
    }
    return icons[st];
}
static void prompt_patch(uint32_t *px, uint32_t w) {
    if (w != 256) return;
    const uint8_t *ic = prompt_icons(opt_prompts & 3); if (!ic) return;
    for (int y = 0; y < 24; y++) for (int x = 0; x < 76; x++) {
        uint32_t t = 0;
        if (x < 72) {   /* 4x4 box average of the alpha */
            for (int dy = 0; dy < 4; dy++) for (int dx = 0; dx < 4; dx++) t += ic[((y * 4 + dy) * 288 + x * 4 + dx) * 4 + 3];
            t = (t + 8) / 16;
        }                /* 72-75: the old X icon's overhang, cleared */
        px[(144 + y) * w + x] = t | t << 8 | t << 16 | t << 24;
    }
}
static int texpack_upload(uint32_t *px, uint32_t w, uint32_t h, uint32_t updates, int want_ui) {
    last_upload_hd = 0;
    if (!tp_init) {
        tp_init = 1; dump_dir = getenv("DC_TEXDUMP"); pack_dir = getenv("DC_TEXPACK");
        dump_ui_only = getenv("DC_TEXDUMP_UIONLY") != NULL;
        if (!pack_dir) pack_dir = "textures";
    }
    if (updates > 3) return 0;                 /* changes every frame (movie, render target): not packable */
    uint64_t hs = hash_pixels(px, (size_t)w * h, w, h);
    char name[64]; snprintf(name, sizeof name, "%016llX_%ux%u.png", (unsigned long long)hs, w, h);
    char path[1024];
    int is_font = hs == FONT_SHEET_HASH && w == 256 && h == 256;
    last_upload_legal = hs == LEGAL_SHEET_HASH;
    if (is_font) font_tex_addr = cur_tex_addr;
    const char *dir = opt_hd ? pack_dir : NULL; char pdir[1024];
    if (is_font && opt_prompts && pack_dir) {   /* button prompts for another controller: a variant of the font sheet */
        snprintf(pdir, sizeof pdir, "%s/prompts/%s/%s", pack_dir, prompt_dir[opt_prompts], opt_hd ? "hd" : "sd");
        dir = pdir;
    }
    if (dir) {
        snprintf(path, sizeof path, "%s/%s", dir, name);
        int rw, rh, n;
        unsigned char *img = stbi_load(path, &rw, &rh, &n, 4);
        if (img) {
            p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
            stbi_image_free(img); texpack_loaded++; last_upload_hd = rw > (int)w;
            static int logged; if (logged++ < 20) fprintf(stderr, "[texpack] replaced %s with %dx%d\n", name, rw, rh);
            return 1;
        }
    }
    if (is_font && opt_hd && !getenv("DC_NOHDFONT")) {   /* no HD sheet file: draw the HD font now (hdfont.c) */
        uint32_t *hdfont_build(const uint32_t *px, const uint8_t *icons4x);
        uint32_t *hf = hdfont_build(px, prompt_icons(opt_prompts & 3));
        if (hf) {
            p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, hf);
            free(hf); last_upload_hd = 1;
            static int logged; if (!logged++) fprintf(stderr, "[hdfont] font sheet redrawn at 1024x1024\n");
            return 1;
        }
    }
    if (is_font && opt_prompts) prompt_patch(px, w);   /* no full variant sheet: just the icons */
    if (dump_dir && (want_ui || !dump_ui_only)) {
        int seen = 0;
        for (size_t i = 0; i < ndumped && !seen; i++) seen = dumped[i] == hs;
        if (!seen) {
        if (ndumped == cap_dumped) { cap_dumped = cap_dumped ? cap_dumped * 2 : 256; dumped = realloc(dumped, cap_dumped * sizeof *dumped); }
        dumped[ndumped++] = hs;
        snprintf(path, sizeof path, "%s/%s", dump_dir, name);
        png_write(path, (const uint8_t *)px, (int)w, (int)h, 0);
        texpack_dumped++;
        }
    }
    /* no HD image supplied: menu/HUD art gets the built-in edge-directed upscale */
    int f = want_ui ? ui_factor() : 0;
    if (f && ui_mode == 1 && !is_text_sheet(px, w, h)) f = 0;
    if (f && w <= 1024 && h <= 1024) {
        static int dbg = -1; if (dbg < 0) dbg = getenv("DC_UIDEBUG") != NULL;
        if (dbg) fprintf(stderr, "[ui] upscaling %s x%d\n", name, f);
        uint32_t *up = upscale_rgba(px, (int)w, (int)h, f);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * f, h * f, 0, GL_RGBA, GL_UNSIGNED_BYTE, up);
        free(up); ui_upscaled++; last_upload_hd = 1;
        return 1;
    }
    return 0;
}

static int last_tex_hd;              /* the texture get_texture returned is an HD replacement or upscale */
static GLuint black_tex(void) {
    static GLuint t; if (t) return t;
    static const uint32_t px = 0xFF000000u;
    p_glGenTextures(1, &t); p_glBindTexture(GL_TEXTURE_2D, t);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &px);
    return t;
}
/* decodes the texture described by tcw/tsp into tbuf (w x h RGBA); returns its data address */
static uint32_t decode_tex(uint32_t tcw, uint32_t tsp, uint32_t stride_reg) {
    int fmt = (tcw >> 27) & 7, mip = tcw >> 31, vq = (tcw >> 30) & 1;
    int pal = fmt == 5 || fmt == 6;
    int scan = !pal && ((tcw >> 26) & 1), stride = scan && ((tcw >> 25) & 1);
    uint32_t w = 8u << ((tsp >> 3) & 7), h = 8u << (tsp & 7);
    if (mip) h = w;
    uint32_t pitch = stride ? stride_reg * 32 : w;
    if (!pitch) pitch = w;
    uint32_t addr = (tcw & 0x1FFFFF) << 3;
    if (tbuf_cap < (size_t)w * h) { tbuf_cap = (size_t)w * h; tbuf = realloc(tbuf, tbuf_cap * 4); }
    int n = ilog2(w);
    if (vq) {
        uint32_t idx = addr + 2048;
        if (mip) idx += n >= 1 ? 1 + ((1u << (2 * (n - 1))) - 1) / 3 : 0;
        for (uint32_t y = 0; y < h; y += 2) for (uint32_t x = 0; x < w; x += 2) {
            uint32_t ti = scan ? (y / 2) * (w / 2) + x / 2 : twiddle(x / 2, y / 2, w / 2, h / 2);
            uint32_t cb = addr + v8(idx + ti) * 8u;
            /* codebook entry: 4 texels in twiddled order (0:x0y0 1:x0y1 2:x1y0 3:x1y1) */
            tbuf[y * w + x]           = conv16(v16(cb + 0), fmt);
            tbuf[(y + 1) * w + x]     = conv16(v16(cb + 2), fmt);
            tbuf[y * w + x + 1]       = conv16(v16(cb + 4), fmt);
            tbuf[(y + 1) * w + x + 1] = conv16(v16(cb + 6), fmt);
        }
    } else if (pal) {
        uint32_t sel = (tcw >> 21) & 63;
        uint32_t base = fmt == 5 ? sel << 4 : (sel >> 4) << 8;
        if (mip) addr += fmt == 5 ? ((1u << (2 * n)) - 1) / 3 / 2 + 1 : 3 + ((1u << (2 * n)) - 1) / 3;
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint32_t ti = twiddle(x, y, w, h), i;
            if (fmt == 6) i = v8(addr + ti);
            else { uint8_t b = v8(addr + ti / 2); i = ti & 1 ? b >> 4 : b & 15; }
            tbuf[y * w + x] = palette(base + i);
        }
    } else {
        if (mip) addr += 6 + 2 * (((1u << (2 * n)) - 1) / 3);
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            if (scan && x >= pitch) { tbuf[y * w + x] = 0; continue; }
            uint32_t ti = scan ? y * pitch + x : twiddle(x, y, w, h);
            if (fmt == 3) {                           /* YUV422: pairs of texels share U/V */
                uint32_t tp = scan ? y * pitch + (x & ~1u) : twiddle(x & ~1u, y, w, h);
                uint32_t tq = scan ? tp + 1 : twiddle(x | 1u, y, w, h);
                uint16_t p0 = v16(addr + tp * 2), p1 = v16(addr + tq * 2);
                int Y = x & 1 ? p1 >> 8 : p0 >> 8;
                tbuf[y * w + x] = yuv(Y, p0 & 0xFF, p1 & 0xFF);
            } else tbuf[y * w + x] = conv16(v16(addr + ti * 2), fmt);
        }
    }
    return addr;
}
static GLuint get_texture(uint32_t tcw, uint32_t tsp, int ui, uint32_t stride_reg) {
    int fmt = (tcw >> 27) & 7, mip = tcw >> 31, vq = (tcw >> 30) & 1;
    int pal = fmt == 5 || fmt == 6;
    int scan = !pal && ((tcw >> 26) & 1), stride = scan && ((tcw >> 25) & 1);
    uint32_t w = 8u << ((tsp >> 3) & 7), h = 8u << (tsp & 7);
    if (mip) h = w;                                   /* mipmapped textures are square */
    uint32_t pitch = stride ? stride_reg * 32 : w;   /* stride: row pitch in memory, U size still scales u */
    if (!pitch) pitch = w;
    uint32_t addr = (tcw & 0x1FFFFF) << 3;
    int bpp = fmt == 5 ? 4 : fmt == 6 ? 8 : 16;
    /* size of the data in texture memory (for invalidation) */
    uint32_t bytes = vq ? 2048 + w * h / 4 : pitch * h * bpp / 8;
    if (mip) bytes = bytes * 4 / 3 + 16;
    uint32_t a0 = addr, a1 = (addr + bytes - 1) & 0x7FFFFF;
    uint32_t palkey = pal ? ((tcw >> 21) & 63) | (hw_reg(0x005F8108u) & 3) << 8 : 0;
    uint32_t gs = gen_sum(a0, a1);
    if (pal) for (int i = 0; i < 1024; i += 16) gs = gs * 7 + hw_reg(0x005F9000u + 4 * i);   /* cheap palette change check */
    uint32_t key_tsp = (tsp & 0x3F) | (stride ? pitch << 8 : 0);
    uint32_t h0 = (tcw * 2654435761u ^ key_tsp * 40503u) % NTEX;
    texent *e = NULL;
    for (int i = 0; i < 8; i++) {
        texent *t = &tcache[(h0 + i) % NTEX];
        if (t->tex && t->key_tcw == tcw && t->key_tsp == key_tsp && t->pal_key == palkey) { e = t; break; }
    }
    if (e && !ui) e->never_ui = 1;            /* used on 3D or in-game geometry: never upscale this one */
    int want_ui = ui && !(e && e->never_ui);
    /* The legal screen appears once, at boot. After the first movie frame, a quad still
     * textured with it (the movie player's frame before decoding starts, when skipping)
     * would flash it for a frame: draw black instead, like the frames around it. */
    { extern uint64_t yuv_frames;
      if (e && e->legal && e->gen_sum == gs && yuv_frames > 0) { last_tex_hd = 0; return black_tex(); } }
    if (e && e->gen_sum == gs && e->uploaded_ui == want_ui) { e->last_used = frame_no; last_tex_hd = e->hd; return e->tex; }
    if (!e) {   /* pick the least recently used slot in the probe window */
        e = &tcache[h0];
        for (int i = 0; i < 8; i++) { texent *t = &tcache[(h0 + i) % NTEX]; if (!t->tex) { e = t; break; } if (t->last_used < e->last_used) e = t; }
        if (!e->tex) p_glGenTextures(1, &e->tex);
    }
    int same_slot = e->key_tcw == tcw && e->key_tsp == key_tsp && e->pal_key == palkey;
    e->updates = same_slot ? e->updates + (e->gen_sum != gs) : 0;
    if (!same_slot) e->never_ui = !ui;
    e->uploaded_ui = (uint8_t)want_ui;
    e->key_tcw = tcw; e->key_tsp = key_tsp; e->pal_key = palkey; e->gen_sum = gs; e->last_used = frame_no; e->a0 = a0; e->a1 = a1;

    addr = decode_tex(tcw, tsp, stride_reg);
    p_glBindTexture(GL_TEXTURE_2D, e->tex);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    cur_tex_addr = addr;
    e->hd = 0;
    last_upload_legal = 0;
    if (texpack_upload(tbuf, w, h, e->updates, want_ui)) e->hd = (uint8_t)last_upload_hd;
    else p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, tbuf);
    e->legal = (uint8_t)last_upload_legal;
    last_tex_hd = e->hd;
    return e->tex;
}

/* ---------------- drawing ---------------- */
static pvr_vert *tri; static size_t tri_cap;
typedef struct { uint32_t param, first, count; float depth; uint32_t order; } draw_t;
static int trans_autosort;          /* translucent list sorted by depth this frame */
static draw_t *draws; static size_t draws_cap;

static const GLenum depth_fn[8] = { GL_NEVER, GL_GREATER, GL_EQUAL, GL_GEQUAL, GL_LESS, GL_NOTEQUAL, GL_LEQUAL, GL_ALWAYS };
static const GLenum blend_src[8] = { GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA };
static const GLenum blend_dst[8] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA };

static int cmp_depth(const void *a, const void *b) {
    float x = ((const draw_t *)a)->depth, y = ((const draw_t *)b)->depth;
    if (x != y) return x < y ? -1 : 1;                  /* small 1/w (far) first */
    uint32_t i = ((const draw_t *)a)->order, j = ((const draw_t *)b)->order;
    return i < j ? -1 : i > j;                          /* equal depth: keep submission order (qsort is not stable) */
}

static void set_state(const pvr_param *p, int list, int ui) {
    uint32_t isp = p->isp, tsp = p->tsp, tcw = p->tcw;
    int tex = (isp >> 25) & 1;
    int cull = (isp >> 27) & 3;
    if (cull >= 2) { p_glEnable(GL_CULL_FACE); p_glCullFace(cull == 2 ? GL_FRONT : GL_BACK); } else p_glDisable(GL_CULL_FACE);
    /* Sorted translucent polygons: the PVR sorts them per pixel and keeps submission
     * order for equal depth, so a later polygon at the same depth is never rejected
     * by an earlier one's depth (overlapping text and icon quads). Test "closer or
     * equal" against what is already drawn, whatever the polygon's own compare mode. */
    p_glDepthFunc(list == PVR_TRANS && trans_autosort ? depth_fn[6] : depth_fn[isp >> 29]);
    p_glDepthMask(((isp >> 26) & 1) ? GL_FALSE : GL_TRUE);
    if (list == PVR_TRANS) { p_glEnable(GL_BLEND); p_glBlendFunc(blend_src[tsp >> 29], blend_dst[(tsp >> 26) & 7]); }
    else p_glDisable(GL_BLEND);
    GLint mode[4] = { tex, (tsp >> 6) & 3, (tsp >> 20) & 1, (tsp >> 19) & 1 };
    int src_sel = list == PVR_TRANS && ((tsp >> 25) & 1), dst_sel = list == PVR_TRANS && ((tsp >> 24) & 1);
    GLint mode2[4] = { (isp >> 24) & 1, (isp >> 23) & 1, list == PVR_PUNCH, src_sel };
    if (src_sel && acc2_dirty) {                 /* snapshot the secondary buffer so it can be sampled while drawing */
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo2); p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo3);
        p_glBlitFramebuffer(0, 0, rt_w, rt_h, 0, 0, rt_w, rt_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        acc2_dirty = 0; cur_fbo = -1;
    }
    GLuint want = dst_sel ? fbo2 : fbo;
    if ((GLint)want != cur_fbo) { p_glBindFramebuffer(GL_FRAMEBUFFER, want); cur_fbo = (GLint)want; }
    if (dst_sel) acc2_dirty = 1;
    p_glActiveTexture(GL_TEXTURE1); p_glBindTexture(GL_TEXTURE_2D, acc2_copy); p_glActiveTexture(GL_TEXTURE0);
    if (p->sprite) mode2[1] = 1;
    p_glUniform4iv(u_mode, 1, mode); p_glUniform4iv(u_mode2, 1, mode2);
    if (tex) {
        p_glBindTexture(GL_TEXTURE_2D, get_texture(tcw, tsp, ui, p->stride));
        int filt = (tsp >> 13) & 3;
        if (opt_sharp && !last_tex_hd) filt = 0;   /* texture_filter = sharp: original pixels, unsmoothed */
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt ? GL_LINEAR : GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt ? GL_LINEAR : GL_NEAREST);
        int cu = (tsp >> 16) & 1, cv = (tsp >> 15) & 1, fu = (tsp >> 18) & 1, fv = (tsp >> 17) & 1;
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, cu ? GL_CLAMP_TO_EDGE : fu ? GL_MIRRORED_REPEAT : GL_REPEAT);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, cv ? GL_CLAMP_TO_EDGE : fv ? GL_MIRRORED_REPEAT : GL_REPEAT);
    } else p_glBindTexture(GL_TEXTURE_2D, white_tex);
}

uint32_t render_frames;
int render_frame3d;
/* menu/HUD detection: flat, screen-aligned rectangles. In a frame that also contains
 * 3D geometry only small ones count (HUD), so room backgrounds and full-screen
 * effects keep their original texture. */
static uint8_t *pui; static size_t pui_cap;
static void classify(const pvr_frame *f) {
    if (f->nparams > pui_cap) { pui_cap = f->nparams * 2; pui = realloc(pui, pui_cap); }
    static float *area, *pz; static size_t area_cap;
    if (f->nparams > area_cap) { area_cap = f->nparams * 2; area = realloc(area, area_cap * sizeof *area); pz = realloc(pz, area_cap * sizeof *pz); }
    for (uint32_t i = 0; i < f->nparams; i++) { pui[i] = 1; area[i] = 0; pz[i] = 1e30f; }
    int frame3d = 0; float zmax3d = 0;
    extern int render_frame3d;
    for (uint32_t i = 0; i < f->nstrips; i++) {
        const pvr_strip *st = &f->strips[i];
        if (st->count < 3 || st->list == PVR_OPAQUE_MOD || st->list == PVR_TRANS_MOD) continue;
        const pvr_vert *v = f->verts + st->first;
        float z0 = v[0].z, x0 = v[0].x, y0 = v[0].y, x1 = x0, y1 = y0;
        float xs[2] = { x0, x0 }, ys[2] = { y0, y0 };
        int flat = 1, nx = 1, ny = 1;
        for (uint32_t k = 1; k < st->count; k++) {
            if (fabsf(v[k].z - z0) > 1e-5f * (fabsf(z0) + 1e-6f)) flat = 0;
            if (fabsf(v[k].x - xs[0]) > 0.5f && (nx < 2 || fabsf(v[k].x - xs[1]) > 0.5f)) { if (nx < 2) xs[nx++] = v[k].x; else flat = 0; }
            if (fabsf(v[k].y - ys[0]) > 0.5f && (ny < 2 || fabsf(v[k].y - ys[1]) > 0.5f)) { if (ny < 2) ys[ny++] = v[k].y; else flat = 0; }
            if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x;
            if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
        }
        if (!flat && !f->params[st->param].sprite) {
            pui[st->param] = 0; frame3d = 1;
            for (uint32_t k = 0; k < st->count; k++) if (v[k].z > zmax3d) zmax3d = v[k].z;
        }
        area[st->param] += (x1 - x0) * (y1 - y0);
        if (z0 < pz[st->param]) pz[st->param] = z0;
    }
    render_frame3d = frame3d;
    for (uint32_t i = 0; i < f->nparams; i++) {
        const pvr_param *p = &f->params[i];
        int yuv = ((p->tcw >> 27) & 7) == 3;
        /* in gameplay only overlays drawn in front of the whole scene count as HUD (z is 1/w: larger = nearer) */
        int in_front = (p->isp >> 29) == 7 || pz[i] > zmax3d;
        if (frame3d && (area[i] > 0.35f * 640 * 480 || yuv || !in_front)) pui[i] = 0;
    }
}

/* ---------------- model capture (F8) ----------------
 * Saves the 3D geometry of the next frame as a Wavefront OBJ with its textures,
 * in captures/capture_NNN.obj (+ .mtl and _texNN.png). The game hands the
 * graphics chip triangles already projected to the screen with 1/w per vertex,
 * so each vertex is lifted back into camera space: Z = w, X/Y from the screen
 * position scaled by w. Menus/HUD and the flat pre-rendered room backgrounds
 * are left out, so what remains is the characters and objects in view, posed
 * as they were on that frame. */
int render_gl_capture_pending;
#ifdef _WIN32
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mkdir_one(p) mkdir(p, 0755)
#endif
static void capture_frame(const pvr_frame *f) {
    static int num;
    char base[96], path[160];
    mkdir_one("captures");
    for (;;) {   /* next unused number */
        snprintf(base, sizeof base, "captures/capture_%03d", ++num);
        snprintf(path, sizeof path, "%s.obj", base);
        FILE *t = fopen(path, "rb"); if (!t) break; fclose(t);
    }
    FILE *obj = fopen(path, "w");
    snprintf(path, sizeof path, "%s.mtl", base);
    FILE *mtl = fopen(path, "w");
    if (!obj || !mtl) { if (obj) fclose(obj); if (mtl) fclose(mtl); fprintf(stderr, "[capture] cannot write %s\n", base); return; }
    const char *leaf = strrchr(base, '/') + 1;
    fprintf(obj, "# Evil Dead: Hail to the King - geometry captured from one frame\nmtllib %s.mtl\n", leaf);
    fprintf(mtl, "newmtl untextured\nKd 1 1 1\n");
    /* The game's camera (0x8C72D388, row-vector 4x4: view = world * R + t; view is X right,
     * Y up, Z depth). With it the capture goes back to world space, Y up, in the game's
     * units (about a metre: Ash is ~1.9 tall). If it doesn't look like a rotation, the
     * capture stays in camera space. */
    float cm[16]; int world;
    { extern uint8_t *dc_ram; memcpy(cm, dc_ram + 0x72D388u, sizeof cm);
      world = 1;
      for (int r = 0; r < 3; r++) { float l = cm[r*4]*cm[r*4] + cm[r*4+1]*cm[r*4+1] + cm[r*4+2]*cm[r*4+2]; if (!(fabsf(l - 1) < 0.02f)) world = 0; }
      for (int k = 0; k < 16; k++) if (!isfinite(cm[k])) world = 0; }
    fprintf(obj, world ? "# world space: Y up, units about a metre\n" : "# camera space: +X right, +Y up, -Z into the screen\n");
    enum { MAXT = 512 };
    static uint32_t tk[MAXT][3]; int ntk = 0;
    const float F = 566.0f;            /* the game's focal length in pixels on its 640x480 screen (measured: Ash's shadow lies flat on the floor) */
    long nv = 0, ntri = 0; int cur_m = -2;
    for (uint32_t i = 0; i < f->nstrips; i++) {
        const pvr_strip *st = &f->strips[i];
        if (st->count < 3 || (st->list != PVR_OPAQUE && st->list != PVR_PUNCH && st->list != PVR_TRANS)) continue;
        const pvr_param *p = &f->params[st->param];
        if (p->sprite || pui[st->param]) continue;               /* menus and HUD */
        const pvr_vert *v = f->verts + st->first;
        float x0 = v[0].x, x1 = x0, y0 = v[0].y, y1 = y0; int flat = 1;
        for (uint32_t k = 0; k < st->count; k++) {
            if (v[k].z <= 0) { flat = 2; break; }
            if (fabsf(v[k].z - v[0].z) > 1e-5f * fabsf(v[0].z)) flat = 0;
            if (v[k].x < x0) x0 = v[k].x; if (v[k].x > x1) x1 = v[k].x; if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y;
        }
        if (flat == 2 || (flat && (x1 - x0) * (y1 - y0) > 0.03f * 640 * 480)) continue;   /* backgrounds, screen overlays */
        /* material */
        int m = -1;
        if ((p->isp >> 25) & 1) {
            uint32_t fmt = (p->tcw >> 27) & 7, palk = (fmt == 5 || fmt == 6) ? (hw_reg(0x005F8108u) & 3) + 1 : 0;
            for (int j = 0; j < ntk; j++) if (tk[j][0] == p->tcw && tk[j][1] == (p->tsp & 0x3F) && tk[j][2] == palk) { m = j; break; }
            if (m < 0 && ntk < MAXT) {
                m = ntk++; tk[m][0] = p->tcw; tk[m][1] = p->tsp & 0x3F; tk[m][2] = palk;
                decode_tex(p->tcw, p->tsp, p->stride);
                int w = 8 << ((p->tsp >> 3) & 7), h = (p->tcw >> 31) ? w : 8 << (p->tsp & 7);
                snprintf(path, sizeof path, "%s_tex%02d.png", base, m);
                png_write(path, (const uint8_t *)tbuf, w, h, 0);
                fprintf(mtl, "\nnewmtl tex%02d\nKd 1 1 1\nmap_Kd %s_tex%02d.png\n", m, leaf, m);
                if (st->list != PVR_OPAQUE) fprintf(mtl, "map_d %s_tex%02d.png\n", leaf, m);
            }
        }
        if (m != cur_m) { if (m >= 0) fprintf(obj, "g tex%02d\nusemtl tex%02d\n", m, m); else fprintf(obj, "g untextured\nusemtl untextured\n"); cur_m = m; }
        for (uint32_t k = 0; k < st->count; k++) {
            float w = 1.0f / v[k].z;
            uint32_t c = v[k].col;
            float g[3] = { (v[k].x - 320) * w / F, -(v[k].y - 240) * w / F, w }, o[3] = { g[0], g[1], -g[2] };
            if (world) for (int r = 0; r < 3; r++)
                o[r] = (g[0] - cm[12]) * cm[r*4] + (g[1] - cm[13]) * cm[r*4+1] + (g[2] - cm[14]) * cm[r*4+2];
            if (world) o[2] = -o[2];                                       /* the game is left-handed, OBJ right-handed */
            fprintf(obj, "v %.5f %.5f %.5f %.4f %.4f %.4f\n", o[0], o[1], o[2],
                    ((c >> 16) & 0xFF) / 255.0, ((c >> 8) & 0xFF) / 255.0, (c & 0xFF) / 255.0);
            fprintf(obj, "vt %.6f %.6f\n", v[k].u, 1.0f - v[k].v);
        }
        for (uint32_t k = 0; k + 2 < st->count; k++) {
            long a = nv + k + 1, b = nv + k + 2, c = nv + k + 3;
            if (k & 1) { long t = a; a = b; b = t; }
            fprintf(obj, "f %ld/%ld %ld/%ld %ld/%ld\n", a, a, b, b, c, c); ntri++;
        }
        nv += st->count;
    }
    fclose(obj); fclose(mtl);
    fprintf(stderr, "[capture] %s.obj: %ld triangles, %d textures\n", base, ntri, ntk);
    extern void platform_notice(const char *msg);
    char msg[96]; snprintf(msg, sizeof msg, "model saved: %s.obj", base); platform_notice(msg);
}

void render_gl_frame(const pvr_frame *f) {
    frame_no++; render_frames++;
    classify(f);
    if (render_gl_capture_pending) { render_gl_capture_pending = 0; capture_frame(f); }
    /* strips -> triangle list, one draw per strip (keeps per-strip state and translucent sorting simple) */
    size_t need = 0;
    for (uint32_t i = 0; i < f->nstrips; i++) if (f->strips[i].count >= 3) need += (f->strips[i].count - 2) * 3;
    if (need > tri_cap) { tri_cap = need * 2; tri = realloc(tri, tri_cap * sizeof *tri); }
    if (f->nstrips > draws_cap) { draws_cap = f->nstrips * 2; draws = realloc(draws, draws_cap * sizeof *draws); }
    size_t nt = 0;
    for (uint32_t i = 0; i < f->nstrips; i++) {
        const pvr_strip *s = &f->strips[i];
        const pvr_vert *v = f->verts + s->first;
        float dsum = 0;
        draws[i].param = s->param; draws[i].first = (uint32_t)nt;
        int icon = -1;
        if (opt_prompts && s->count >= 3 && font_tex_addr != ~0u && ((f->params[s->param].tcw & 0x1FFFFF) << 3) == font_tex_addr) {
            /* a button icon from the font sheet (row 6, 18-texel cells Y B A X): colour it like that controller's button */
            float vmin = 1, vmax = 0, umid = 0;
            for (uint32_t k = 0; k < s->count; k++) { if (v[k].v < vmin) vmin = v[k].v; if (v[k].v > vmax) vmax = v[k].v; umid += v[k].u; }
            umid /= s->count;
            if (vmin > 140.f / 256 && vmax < 172.f / 256) { int c = (int)(umid * 256 / 18); if (c >= 0 && c < 4) icon = c; }
        }
        size_t first_tri = nt;
        for (uint32_t k = 0; k + 2 < s->count; k++) {
            /* keep strip winding consistent: swap every odd triangle */
            const pvr_vert *a = &v[k], *b = &v[k + 1], *c = &v[k + 2];
            if (k & 1) { const pvr_vert *t = a; a = b; b = t; }
            tri[nt++] = *a; tri[nt++] = *b; tri[nt++] = *c;
        }
        if (icon >= 0) {
            /* sheet order Y B A X = top, right, bottom, left button */
            static const uint32_t pal[4][4] = {
                { 0, 0, 0, 0 },
                { 0xF2C21B, 0xE0333A, 0x3FB54A, 0x2D7DD2 },           /* Xbox: Y yellow, B red, A green, X blue */
                { 0x3BB89A, 0xE5505A, 0x6A8CE8, 0xD98ACB },           /* PlayStation: triangle, circle, cross, square */
                { 0xE8E8E8, 0xE8E8E8, 0xE8E8E8, 0xE8E8E8 } };         /* Switch: light grey */
            uint32_t rgb = pal[opt_prompts][icon];
            for (size_t q = first_tri; q < nt; q++) {
                uint32_t c0 = tri[q].col, mx = c0 & 0xFF; if (((c0 >> 8) & 0xFF) > mx) mx = (c0 >> 8) & 0xFF; if (((c0 >> 16) & 0xFF) > mx) mx = (c0 >> 16) & 0xFF;
                uint32_t r = ((rgb >> 16) & 0xFF) * mx / 255, g = ((rgb >> 8) & 0xFF) * mx / 255, b = (rgb & 0xFF) * mx / 255;   /* keep the game's fades */
                tri[q].col = (c0 & 0xFF000000u) | r << 16 | g << 8 | b;
            }
        }
        for (uint32_t k = 0; k < s->count; k++) dsum += v[k].z;
        draws[i].count = (uint32_t)nt - draws[i].first;
        draws[i].depth = s->count ? dsum / s->count : 0;
    }
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glViewport(0, 0, rt_w, rt_h);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDepthMask(GL_TRUE);
    p_glClearColor(0, 0, 0, 1); clear_depth1();
    p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo2); p_glClearColor(0, 0, 0, 0); p_glClear(GL_COLOR_BUFFER_BIT);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo); cur_fbo = (GLint)fbo; acc2_dirty = 1;
    p_glEnable(GL_DEPTH_TEST);
    p_glUseProgram(prog);
    p_glUniform4f(u_scr, 2.0f / fb_w, -2.0f / fb_h, -1.0f, 1.0f);
    p_glUniform1f(u_ptref, (hw_reg(0x005F811Cu) & 0xFF) / 255.0f);
    p_glUniform1i(u_tex, 0); p_glUniform1i(u_acc2, 1); p_glActiveTexture(GL_TEXTURE0);
    p_glBindVertexArray(vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, nt * sizeof *tri, tri, GL_STREAM_DRAW);

    static const int order[3] = { PVR_OPAQUE, PVR_PUNCH, PVR_TRANS };
    int autosort = !(hw_reg(0x005F8098u) & 1);
    for (uint32_t i = 0; i < f->nparams && autosort; i++) if (f->params[i].list == PVR_TRANS && (f->params[i].tsp & 0x03000000u)) autosort = 0;
    for (int li = 0; li < 3; li++) {
        int list = order[li];
        draw_t *sel = malloc(sizeof *sel * (f->nstrips + 1)); size_t ns = 0;
        for (uint32_t i = 0; i < f->nstrips; i++) if (f->strips[i].list == list && draws[i].count) { sel[ns] = draws[i]; sel[ns].order = i; ns++; }
        trans_autosort = list == PVR_TRANS && autosort;
        if (list == PVR_TRANS && autosort) qsort(sel, ns, sizeof *sel, cmp_depth);
        int32_t last_param = -1;
        static long skip = -2; if (skip == -2) skip = getenv("DC_SKIPPARAM") ? atol(getenv("DC_SKIPPARAM")) : -1;
        for (size_t i = 0; i < ns; i++) {
            if ((long)sel[i].param == skip) continue;
            if ((int32_t)sel[i].param != last_param) { set_state(&f->params[sel[i].param], list, pui[sel[i].param]); last_param = (int32_t)sel[i].param; }
            p_glDrawArrays(GL_TRIANGLES, sel[i].first, sel[i].count);
        }
        free(sel);
    }
    p_glDisable(GL_BLEND); p_glDisable(GL_CULL_FACE); p_glDisable(GL_DEPTH_TEST);
    GLenum err = p_glGetError();
    if (err) { static int n; if (n++ < 5) fprintf(stderr, "[gl] error %04X\n", err); }
}

void render_gl_read(uint8_t *rgba, int *w, int *h) {
    *w = rt_w; *h = rt_h;
    if (!rgba) return;
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glReadPixels(0, 0, *w, *h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    /* the frame's alpha channel is working data for blending (destination alpha), not transparency */
    for (size_t i = 3; i < (size_t)*w * *h * 4; i += 4) rgba[i] = 255;
}

/* copy the last frame to the window's default framebuffer, letterboxed to 4:3 */
static int present_stretch;
void render_gl_set_stretch(int on) { present_stretch = on; }
void render_gl_present(int win_w, int win_h) {
    int w = win_w, h = win_w * 3 / 4;
    if (h > win_h) { h = win_h; w = win_h * 4 / 3; }
    if (present_stretch) { w = win_w; h = win_h; }
    int x = (win_w - w) / 2, y = (win_h - h) / 2;
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    p_glViewport(0, 0, win_w, win_h);
    p_glClearColor(0, 0, 0, 1); p_glClear(GL_COLOR_BUFFER_BIT);
    p_glBlitFramebuffer(0, 0, rt_w, rt_h, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}
