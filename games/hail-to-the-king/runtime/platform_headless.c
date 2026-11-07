/* Headless platform: EGL surfaceless (Mesa) + GLES3, screenshots as PNG.
 *   DC_SHOT_EVERY=N   save every Nth rendered frame
 *   DC_SHOTDIR=dir    where to put shot_<render#>.png (default .)
 *   DC_NOGL=1         no GL at all (fast runs; display lists are still parsed) */
#include "platform.h"
#include "render_gl.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <stdlib.h>

int png_write(const char *path, const uint8_t *rgba, int w, int h, int bottom_up);
static int shot_every; static const char *shot_dir = ".";
static uint32_t renders;

static void *getproc(const char *n) { return (void *)eglGetProcAddress(n); }

void platform_notice(const char *msg) { fprintf(stderr, "[headless] %s\n", msg); }
static void on_render(const pvr_frame *f) {
    renders++;
    /* DC_CAPTURE_FIELD=N: capture the models of the first frame drawn after field N (test of the F8 key) */
    { extern int render_gl_capture_pending; uint64_t dc_frame_count(void);
      static long cf = -2; static int done; if (cf == -2) cf = getenv("DC_CAPTURE_FIELD") ? atol(getenv("DC_CAPTURE_FIELD")) : -1;
      if (cf >= 0 && !done && dc_frame_count() >= (uint64_t)cf) { render_gl_capture_pending = 1; done = 1; } }
    render_gl_frame(f);
    /* DC_SHOT_FIELDS=1: shoot the first render after every Nth display field and
     * name it by field number (the clock DC_INPUT uses); otherwise every Nth render */
    uint64_t dc_frame_count(void);
    static int by_field = -1; static uint64_t last_slot = ~0ull;
    if (by_field < 0) by_field = getenv("DC_SHOT_FIELDS") != NULL;
    unsigned num = renders;
    int take = shot_every && renders % shot_every == 0;
    if (shot_every && by_field) {
        uint64_t fc = dc_frame_count(), slot = fc / (uint64_t)shot_every;
        take = slot != last_slot; last_slot = slot; num = (unsigned)fc;
    }
    if (take) {
        int w, h; render_gl_read(NULL, &w, &h);
        uint8_t *buf = malloc((size_t)w * h * 4);
        render_gl_read(buf, &w, &h);
        char p[1024]; snprintf(p, sizeof p, "%s/shot_%05u.png", shot_dir, num);
        png_write(p, buf, w, h, 1);
        free(buf);
    }
}

int platform_init(int argc, char **argv) {
    (void)argc; (void)argv;
    if (getenv("DC_NOGL")) return 1;
    if (getenv("DC_SHOT_EVERY")) shot_every = atoi(getenv("DC_SHOT_EVERY"));
    if (getenv("DC_SHOTDIR")) shot_dir = getenv("DC_SHOTDIR");
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint ma, mi;
    if (!eglInitialize(d, &ma, &mi)) { fprintf(stderr, "[egl] init failed\n"); return 0; }
    int desktop = getenv("DC_DESKTOPGL") != NULL;     /* test the desktop OpenGL 3.3 path */
    eglBindAPI(desktop ? EGL_OPENGL_API : EGL_OPENGL_ES_API);
    EGLint ca[] = { EGL_RENDERABLE_TYPE, desktop ? EGL_OPENGL_BIT : EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    eglChooseConfig(d, ca, &cfg, 1, &n);
    EGLint cx[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, desktop ? 3 : 0,
                    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    if (!desktop) cx[4] = EGL_NONE;
    EGLContext c = eglCreateContext(d, n ? cfg : (EGLConfig)0, EGL_NO_CONTEXT, cx);
    if (!c || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { fprintf(stderr, "[egl] no context\n"); return 0; }
    int scale = getenv("DC_SCALE") ? atoi(getenv("DC_SCALE")) : 1;
    if (!render_gl_init(getproc, scale, desktop)) return 0;
    pvr_set_renderer(on_render);
    if (getenv("DC_RES")) { int w = 0, h = 0; if (sscanf(getenv("DC_RES"), "%dx%d", &w, &h) == 2) render_gl_resize(w, h); }
    return 1;
}
void platform_vblank(void) {}
void platform_shutdown(void) {}
