/* High-level replacement for the Dreamcast BIOS system calls.
 *
 * Written from the publicly documented syscall interface (as used by the
 * open-source KallistiOS SDK); no Sega BIOS code or data is used.
 *
 * Vectors in low RAM point at magic addresses that sh4_dispatch() routes here:
 *   0x8C0000B0 sysinfo   r7: 0 init, 2 icon, 3 id
 *   0x8C0000B4 ROM font  (address of font data)
 *   0x8C0000B8 flashrom  r7: 0 info, 1 read, 2 write, 3 delete
 *   0x8C0000BC GD-ROM    r6 0: r7 = 0 send, 1 check, 2 mainloop, 3 init, 4 drive status,
 *                               8 abort, 9 reset, 10 sector mode
 *                        r6 -1: r7 = 0 misc init, 1 set vector
 *   0x8C0000E0 system    r4: 1 = exit to BIOS menu
 */
#include "sh4ctx.h"
#include <stdio.h>
#include <stdlib.h>

#define HLE_BASE  0x8C000F00u
#define HLE_SYSINFO (HLE_BASE + 0x00)
#define HLE_FONT    (HLE_BASE + 0x10)
#define HLE_FLASH   (HLE_BASE + 0x20)
#define HLE_GDROM   (HLE_BASE + 0x30)
#define HLE_SYSTEM  (HLE_BASE + 0x40)

void hle_halt(const char *why, uint32_t a);
int disc_read_sector(uint32_t fad, uint8_t *dst2048);   /* disc.c */
void disc_toc(int area, uint32_t *toc102);              /* disc.c */
static int bios_log = 1;

/* ---------------- GD-ROM ---------------- */
enum { CMD_PIOREAD = 16, CMD_DMAREAD = 17, CMD_GETTOC = 18, CMD_GETTOC2 = 19, CMD_PLAY = 20, CMD_PLAY2 = 21,
       CMD_PAUSE = 22, CMD_RELEASE = 23, CMD_INIT = 24, CMD_SEEK = 27, CMD_DMAREAD_STREAM = 28,
       CMD_NOP = 29, CMD_REQ_MODE = 30, CMD_SET_MODE = 31, CMD_STOP = 33, CMD_GETSCD = 34, CMD_GETSES = 35 };
enum { STAT_NO_ACTIVE = 0, STAT_PROCESSING = 1, STAT_COMPLETED = 2 };

typedef struct { int32_t id; int32_t state; uint32_t err[4]; } gdreq;
static gdreq reqs[16];
static int32_t next_id = 1;

static void copy_out(uint32_t dst, const uint8_t *src, uint32_t n) { for (uint32_t i = 0; i < n; i++) W8(dst + i, src[i]); }

static int32_t gd_exec(uint32_t cmd, uint32_t params, uint32_t *bytes) {
    uint32_t p0 = params ? R32(params) : 0, p1 = params ? R32(params + 4) : 0, p2 = params ? R32(params + 8) : 0;
    *bytes = 0;
    switch (cmd) {
    case CMD_PIOREAD: case CMD_DMAREAD: {
        uint8_t buf[2048];
        for (uint32_t k = 0; k < p1; k++) {
            if (!disc_read_sector(p0 + k, buf)) { fprintf(stderr, "[gd] read fail FAD %u\n", p0 + k); return -1; }
            copy_out(p2 + k * 2048, buf, 2048);
        }
        *bytes = p1 * 2048;
        { void text_patch(uint32_t a, uint32_t len); text_patch(p2, p1 * 2048); }
        if (bios_log) fprintf(stderr, "[gd] read FAD %u x%u -> %08X\n", p0, p1, p2);
        return 0;
    }
    case CMD_GETTOC: case CMD_GETTOC2: {
        uint32_t toc[102]; disc_toc(p0, toc);
        for (int i = 0; i < 102; i++) W32(p1 + 4 * i, toc[i]);
        *bytes = sizeof toc; return 0;
    }
    case CMD_GETSES: {                     /* p0 session, p1 size, p2 buf: [0]=status [2]=count, [3..5]=FAD */
        uint8_t ses[6] = {1, 0, 1, 0, 0xAF, 0x96};
        copy_out(p2, ses, p1 < 6 ? p1 : 6); return 0;
    }
    case CMD_INIT: case CMD_PAUSE: case CMD_RELEASE: case CMD_STOP: case CMD_SEEK: case CMD_NOP:
    case CMD_SET_MODE: case CMD_PLAY: case CMD_PLAY2:
        return 0;
    case CMD_REQ_MODE: {
        for (uint32_t i = 0; i < p1; i++) W8(p2 + i, 0);
        return 0;
    }
    default:
        fprintf(stderr, "[gd] unhandled command %u (params %08X: %08X %08X %08X)\n", cmd, params, p0, p1, p2);
        return 0;
    }
}

static void gdrom(sh4ctx *c) {
    int32_t super = (int32_t)c->r[6]; uint32_t fn = c->r[7];
    static int n; if (bios_log && n++ < 60) fprintf(stderr, "[gd] syscall super=%d fn=%u r4=%08X r5=%08X\n", super, fn, c->r[4], c->r[5]);
    if (super == -1) { c->r[0] = 0; return; }           /* misc init / set vector */
    switch (fn) {
    case 0: {                                            /* send command (r4 cmd, r5 params) -> id */
        gdreq *q = &reqs[next_id & 15];
        q->id = next_id++; memset(q->err, 0, sizeof q->err);
        uint32_t bytes;
        int32_t r = gd_exec(c->r[4], c->r[5], &bytes);
        q->state = r < 0 ? -1 : STAT_COMPLETED;
        if (r < 0) q->err[0] = 5;                       /* sense: medium error */
        q->err[2] = bytes;
        c->r[0] = (uint32_t)q->id; return;
    }
    case 1: {                                            /* check command (r4 id, r5 status[4]) */
        gdreq *q = &reqs[c->r[4] & 15];
        if (q->id != (int32_t)c->r[4]) { c->r[0] = STAT_NO_ACTIVE; return; }
        for (int i = 0; i < 4; i++) W32(c->r[5] + 4 * i, q->err[i]);
        c->r[0] = (uint32_t)q->state; return;
    }
    case 2: case 3: case 8: case 9: case 10: c->r[0] = 0; return;   /* mainloop / init / abort / reset / sector mode */
    case 4:                                              /* drive status (r4 params[2]) */
        W32(c->r[4], 2);                                 /* standby */
        W32(c->r[4] + 4, 0x80);                          /* GD-ROM disc */
        c->r[0] = 0; return;
    default:
        fprintf(stderr, "[gd] unhandled function %u\n", fn);
        c->r[0] = 0; return;
    }
}

/* ---------------- sysinfo / flash / system ---------------- */
static void sysinfo(sh4ctx *c) {
    switch (c->r[7]) {
    case 0:                                              /* init: copy console info to 0x8C000068 */
        for (int i = 0; i < 24; i++) W8(0x8C000068u + i, 0);
        c->r[0] = 0; return;
    case 3: c->r[0] = 0x8C000068u; return;               /* pointer to 8-byte console ID */
    default: c->r[0] = 0; return;
    }
}

/* Flash: report the partitions as present but empty; the games fall back to defaults. */
static void flashrom(sh4ctx *c) {
    switch (c->r[7]) {
    case 0: {                                            /* info (r4 part, r5 info[2]) */
        static const uint32_t part[5][2] = {{0x1A000, 0x2000}, {0x18000, 0x2000}, {0x1C000, 0x4000}, {0x10000, 0x8000}, {0, 0x10000}};
        uint32_t p = c->r[4];
        if (p > 4) { c->r[0] = (uint32_t)-1; return; }
        W32(c->r[5], part[p][0]); W32(c->r[5] + 4, part[p][1]); c->r[0] = 0; return;
    }
    case 1: for (uint32_t i = 0; i < c->r[6]; i++) W8(c->r[5] + i, 0xFF); c->r[0] = c->r[6]; return;
    case 2: c->r[0] = c->r[6]; return;
    case 3: c->r[0] = 0; return;
    default: c->r[0] = (uint32_t)-1; return;
    }
}

static void system_vec(sh4ctx *c) {
    if (c->r[4] == 1) {
        fprintf(stderr, "[bios] game requested exit to the BIOS menu (pr=%08X)\n", c->pr);
        hle_halt("exit to menu", c->pr);
    }
    c->r[0] = 0;
}

int bios_hle_dispatch(sh4ctx *c, uint32_t a) {
    if (((a & 0x1FFFFFFFu) | 0x80000000u) - HLE_BASE < 0x50 && bios_log > 0) {
        static int k; if (k++ < 80) fprintf(stderr, "[bios] vector call %08X r4=%08X r6=%08X r7=%08X pr=%08X\n", a, c->r[4], c->r[6], c->r[7], c->pr);
    }
    a = (a & 0x1FFFFFFFu) | 0x80000000u;
    if (a < HLE_BASE || a >= HLE_BASE + 0x50) return 0;
    switch (a) {
    case HLE_SYSINFO: sysinfo(c); break;
    case HLE_FONT: c->r[0] = 0xA0100020u; break;          /* no ROM font; points at zeroed memory */
    case HLE_FLASH: flashrom(c); break;
    case HLE_GDROM: gdrom(c); break;
    case HLE_SYSTEM: system_vec(c); break;
    default: return 0;
    }
    return 1;
}

void bios_hle_install(void) {
    W32(0x8C0000B0u, HLE_SYSINFO);
    W32(0x8C0000B4u, HLE_FONT);
    W32(0x8C0000B8u, HLE_FLASH);
    W32(0x8C0000BCu, HLE_GDROM);
    W32(0x8C0000E0u, HLE_SYSTEM);
}
