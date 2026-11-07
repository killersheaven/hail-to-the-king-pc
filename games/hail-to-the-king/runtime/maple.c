/* Maple bus: answers the game's controller command lists.
 *
 * One standard controller on port A with a memory card (VMU) in its first
 * slot (sub-unit 1); ports B-D empty. The memory card is a 128 KB file
 * (vmu_a1.bin; DC_VMU=path overrides), created pre-formatted when missing and
 * written through on every block write. Its screen and clock accept writes.
 * Frame layout from public documentation (KallistiOS maple.h, dreamcast.wiki).
 *
 * Input comes from maple_pad (set by the platform layer) or, for headless
 * runs, from DC_INPUT="frame:BUTTONS,frame:BUTTONS,..." e.g. "600:START,606:-".
 */
#include "sh4ctx.h"
#include "maple.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

maple_pad_t maple_pad = { 0, 0, 0, 0x80, 0x80 };
uint64_t dc_frame_count(void);

enum { CMD_DEVINFO = 1, CMD_ALLINFO = 2, CMD_RESET = 3, CMD_KILL = 4, CMD_GETCOND = 9,
       CMD_MEMINFO = 10, CMD_BREAD = 11, CMD_BWRITE = 12, CMD_GETLASTERR = 13, CMD_SETCOND = 14 };
enum { RSP_DEVINFO = 5, RSP_ALLINFO = 6, RSP_ACK = 7, RSP_DATATRF = 8, RSP_UNKNOWN = 0xFE };
#define FUNC_CONTROLLER 0x01000000u
#define FUNC_STORAGE    0x02000000u
#define FUNC_LCD        0x04000000u
#define FUNC_CLOCK      0x08000000u
enum { RSP_FILEERR = 0xFB };

/* ---- memory card (VMU) image ---- */
/* two cards: controller A slots 1 and 2 = "Save Slot 1" and "Save Slot 2" */
enum { NCARDS = 2 };
static uint8_t vmu_img[NCARDS][256 * 512];
static char vmu_paths[NCARDS][1024];
static int vmu_readys[NCARDS];
static int vc;                                  /* card the current frame is addressed to */
#define vmu vmu_img[vc]
#define vmu_path vmu_paths[vc]
#define vmu_ready vmu_readys[vc]
void maple_set_vmu_path(const char *path) { snprintf(vmu_paths[0], sizeof vmu_paths[0], "%s", path); }
static void wr16(uint8_t *b, int off, uint16_t v) { b[off] = v & 0xFF; b[off + 1] = v >> 8; }
static void vmu_format(void) {
    memset(vmu, 0, sizeof vmu);
    uint8_t *root = vmu + 255 * 512;
    memset(root, 0x55, 16);                               /* formatted */
    root[0x30] = 0x20; root[0x31] = 0x00; root[0x32] = 0x01; root[0x33] = 0x01;   /* BCD date 2000-01-01 */
    wr16(root, 0x44, 255); wr16(root, 0x46, 254); wr16(root, 0x48, 1);   /* root block, FAT location and size */
    wr16(root, 0x4A, 253); wr16(root, 0x4C, 13);           /* directory location and size */
    wr16(root, 0x4E, 0); wr16(root, 0x50, 200);            /* icon, user blocks */
    wr16(root, 0x52, 31);
    uint8_t *fat = vmu + 254 * 512;
    for (int b = 0; b < 256; b++) wr16(fat, b * 2, 0xFFFC);           /* free */
    wr16(fat, 255 * 2, 0xFFFA); wr16(fat, 254 * 2, 0xFFFA);          /* root, FAT: single blocks */
    for (int b = 253; b > 241; b--) wr16(fat, b * 2, (uint16_t)(b - 1));   /* directory chain 253 -> 241 */
    wr16(fat, 241 * 2, 0xFFFA);
}
static void vmu_load(void) {
    if (vmu_ready) return;
    vmu_ready = 1;
    if (!vmu_path[0]) {
        const char *e = getenv("DC_VMU");
        if (vc == 0) snprintf(vmu_path, sizeof vmu_path, "%s", e ? e : "vmu_a1.bin");
        else if (e) snprintf(vmu_path, sizeof vmu_path, "%s.%d", e, vc + 1);
        else snprintf(vmu_path, sizeof vmu_path, "vmu_a%d.bin", vc + 1);
    }
    FILE *f = fopen(vmu_path, "rb");
    if (f && fread(vmu, 1, sizeof vmu, f) == sizeof vmu) { fclose(f); fprintf(stderr, "[vmu] memory card %s loaded\n", vmu_path); return; }
    if (f) fclose(f);
    vmu_format();
    f = fopen(vmu_path, "wb");
    if (f) { fwrite(vmu, 1, sizeof vmu, f); fclose(f); }
    fprintf(stderr, "[vmu] new formatted memory card %s%s\n", vmu_path, f ? "" : " (cannot write it!)");
}
static void vmu_save_range(uint32_t off, uint32_t len) {
    FILE *f = fopen(vmu_path, "r+b");
    if (!f) f = fopen(vmu_path, "wb");
    if (!f) { static int w; if (!w++) fprintf(stderr, "[vmu] cannot write %s\n", vmu_path); return; }
    if (fseek(f, 0, SEEK_END) == 0 && ftell(f) < (long)sizeof vmu) { fseek(f, 0, SEEK_SET); fwrite(vmu, 1, sizeof vmu, f); }
    else { fseek(f, (long)off, SEEK_SET); fwrite(vmu + off, 1, len, f); }
    fclose(f);
}

static const struct { const char *name; uint16_t bit; } btn_names[] = {
    {"C", MAPLE_C}, {"B", MAPLE_B}, {"A", MAPLE_A}, {"START", MAPLE_START}, {"UP", MAPLE_UP}, {"DOWN", MAPLE_DOWN},
    {"LEFT", MAPLE_LEFT}, {"RIGHT", MAPLE_RIGHT}, {"Z", MAPLE_Z}, {"Y", MAPLE_Y}, {"X", MAPLE_X}, {"D", MAPLE_D}};

static void scripted_input(void) {
    static const char *s; static int init;
    if (!init) { init = 1; s = getenv("DC_INPUT"); }
    if (!s) return;
    uint64_t now = dc_frame_count();
    /* apply every event whose frame has passed, in order */
    static const char *p; if (!p) p = s;
    while (*p) {
        char *end; unsigned long f = strtoul(p, &end, 10);
        if (end == p || *end != ':' || f > now) break;
        p = end + 1;
        uint16_t b = 0; uint8_t jx = 0x80, jy = 0x80;
        while (*p && *p != ',') {
            const char *q = p; while (*q && *q != ',' && *q != '+') q++;
            for (size_t i = 0; i < sizeof btn_names / sizeof *btn_names; i++)
                if ((size_t)(q - p) == strlen(btn_names[i].name) && !strncmp(p, btn_names[i].name, q - p)) b |= btn_names[i].bit;
            if (q - p == 2 && p[0] == 'S') { if (p[1] == 'L') jx = 0; if (p[1] == 'R') jx = 255; if (p[1] == 'U') jy = 0; if (p[1] == 'D') jy = 255; }   /* stick: SL SR SU SD */
            p = *q == '+' ? q + 1 : q;
        }
        if (*p == ',') p++;
        maple_pad.buttons = b; maple_pad.joyx = jx; maple_pad.joyy = jy;
        fprintf(stderr, "[maple] frame %llu: buttons %04X\n", (unsigned long long)now, b);
    }
}

static void put(uint32_t *a, uint32_t v) { W32(*a, v); *a += 4; }
static void put_str(uint32_t *a, const char *s, int n) {
    uint8_t b[64]; int l = (int)strlen(s);
    for (int i = 0; i < n; i++) b[i] = i < l ? (uint8_t)s[i] : ' ';
    for (int i = 0; i < n; i++) W8(*a + i, b[i]);
    *a += n;
}

static void devinfo(uint32_t *out) {
    put(out, FUNC_CONTROLLER);
    put(out, 0xFE060F00u); put(out, 0); put(out, 0);        /* controller capabilities: standard pad */
    W8(*out, 0xFF); W8(*out + 1, 0); *out += 2;             /* area code (all regions), connector direction */
    put_str(out, "Dreamcast Controller", 30);
    put_str(out, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
    W16(*out, 0x01AE); W16(*out + 2, 0x01F4); *out += 4;    /* standby / max power, 0.1 mA */
}

static void vmu_devinfo(uint32_t *out) {
    put(out, FUNC_STORAGE | FUNC_LCD | FUNC_CLOCK);
    put(out, 0x403F7E7Eu); put(out, 0x00100500u); put(out, 0x00410F00u);   /* clock, LCD, storage function data */
    W8(*out, 0xFF); W8(*out + 1, 0); *out += 2;
    put_str(out, "Visual Memory", 30);
    put_str(out, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
    W16(*out, 0x007C); W16(*out + 2, 0x0082); *out += 4;
}
static uint32_t rsp(int code, int from, uint32_t me, uint32_t words) { return (uint32_t)code | (uint32_t)from << 8 | me << 16 | words << 24; }
/* one frame addressed to the memory card; data = request payload, n = its length in words */
static void vmu_frame(uint32_t out, int cmd, int from, uint32_t me, uint32_t data, int n) {
    vmu_load();
    uint32_t func = n > 0 ? R32(data) : 0;
    switch (cmd) {
    case CMD_DEVINFO: case CMD_ALLINFO:
        put(&out, rsp(cmd == CMD_DEVINFO ? RSP_DEVINFO : RSP_ALLINFO, from, me, cmd == CMD_DEVINFO ? 28u : 48u));
        vmu_devinfo(&out);
        if (cmd == CMD_ALLINFO) for (int i = 0; i < 20; i++) put(&out, 0);
        return;
    case CMD_MEMINFO:
        if (func != FUNC_STORAGE) break;
        put(&out, rsp(RSP_DATATRF, from, me, 7));
        put(&out, FUNC_STORAGE);
        { static const uint16_t mi[] = { 255, 0, 255, 254, 1, 253, 13 };   /* last block, partition, root, FAT, FAT size, dir, dir size */
          for (int i = 0; i < 7; i++) { W16(out, mi[i]); out += 2; } }
        W8(out, 0); W8(out + 1, 0); out += 2;                              /* icon, reserved */
        W16(out, 200); W16(out + 2, 31); out += 4;                         /* user blocks, execution-file area */
        put(&out, 0);
        return;
    case CMD_BREAD: {
        if (func != FUNC_STORAGE || n < 2) break;
        uint32_t loc = R32(data + 4);
        uint32_t blk = ((loc >> 16) & 0xFF) << 8 | (loc >> 24);         /* bytes: partition, phase, block hi, block lo */
        if ((loc & 0xFF) != 0 || blk > 255) { put(&out, rsp(RSP_FILEERR, from, me, 1)); put(&out, 0x00000010u); return; }
        put(&out, rsp(RSP_DATATRF, from, me, 2 + 128));
        put(&out, FUNC_STORAGE); put(&out, loc);
        for (int i = 0; i < 512; i++) W8(out + i, vmu[blk * 512 + i]);
        return;
    }
    case CMD_BWRITE: {
        if (func == FUNC_STORAGE && n >= 2) {
            uint32_t loc = R32(data + 4);
            uint32_t blk = ((loc >> 16) & 0xFF) << 8 | (loc >> 24), phase = (loc >> 8) & 0xFF;
            uint32_t len = (uint32_t)(n - 2) * 4, off = blk * 512 + phase * 128;
            if ((loc & 0xFF) == 0 && blk <= 255 && off + len <= sizeof vmu) {
                for (uint32_t i = 0; i < len; i++) vmu[off + i] = R8(data + 8 + i);
                vmu_save_range(off, len);
                static int logged; if (logged++ < 4) fprintf(stderr, "[vmu] write block %u phase %u (%u bytes)\n", blk, phase, len);
            }
        }
        /* LCD (screen image) and clock writes are accepted and ignored */
        put(&out, rsp(RSP_ACK, from, me, 0));
        return;
    }
    case CMD_GETLASTERR: case CMD_SETCOND: case CMD_RESET: case CMD_KILL:
        put(&out, rsp(RSP_ACK, from, me, 0));
        return;
    case CMD_GETCOND:
        if (func == FUNC_CLOCK) {                                         /* date/time: report 2000-01-01 00:00 */
            put(&out, rsp(RSP_DATATRF, from, me, 3)); put(&out, FUNC_CLOCK);
            W16(out, 2000); W8(out + 2, 1); W8(out + 3, 1); W8(out + 4, 0); W8(out + 5, 0); W8(out + 6, 0); W8(out + 7, 5); return;
        }
        break;
    }
    static int warned;
    if (warned++ < 8) fprintf(stderr, "[vmu] unhandled command %d function %08X\n", cmd, func);
    put(&out, rsp(RSP_UNKNOWN, from, me, 0));
}

uint64_t maple_frames;
void maple_dma(uint32_t list) {
    scripted_input();
    { static int dumps; if (getenv("DC_MAPLEDEBUG") && dumps++ < 6) { fprintf(stderr, "[maple] list %08X:", list); for (int i = 0; i < 24; i++) fprintf(stderr, " %08X", R32(list + 4 * i)); fprintf(stderr, "\n"); } }
    uint32_t p = list;
    for (int guard = 0; guard < 64; guard++) {
        uint32_t desc = R32(p), recv = R32(p + 4);
        int last = desc >> 31, port = (desc >> 16) & 3, pattern = (desc >> 8) & 7, len = desc & 0xFF;
        if (pattern == 7) { p += 4; if (last) break; continue; }      /* NOP pattern: a single instruction word */
        uint32_t hdr = R32(p + 8);
        int cmd = hdr & 0xFF, to = (hdr >> 8) & 0xFF, from = (hdr >> 16) & 0xFF;
        maple_frames++;
        { static int dbg = -1; if (dbg < 0) dbg = getenv("DC_MAPLEDEBUG") != NULL;
          static uint32_t seen[64]; static int nseen; uint32_t key = (uint32_t)cmd | (uint32_t)to << 8 | (uint32_t)port << 16;
          int k = 0; for (; k < nseen && seen[k] != key; k++) ;
          if (dbg && k == nseen && nseen < 64) { seen[nseen++] = key; fprintf(stderr, "[maple] port %d to %02X cmd %d len %d (frame %llu)\n", port, to, cmd, len, (unsigned long long)dc_frame_count()); } }
        uint32_t out = recv;
        int unit = to & 0x3F;                                         /* 0x20 = main device, 0x01-0x10 = sub-units */
        if (port == 0 && (unit == 0x01 || unit == 0x02) && !getenv("DC_NOVMU")) {
            vc = unit == 0x02;
            vmu_frame(out, cmd, from, (uint32_t)(port << 6) | (uint32_t)unit, p + 12, len);
        } else if (port != 0 || unit != 0x20) {
            W32(out, 0xFFFFFFFFu);                                    /* no device */
        } else {
            uint32_t me = (uint32_t)(port << 6) | 0x20 | (getenv("DC_NOVMU") ? 0 : 0x03);   /* bits 0, 1: memory cards in slots 1 and 2 */
            switch (cmd) {
            case CMD_DEVINFO: case CMD_ALLINFO:
                put(&out, (cmd == CMD_DEVINFO ? RSP_DEVINFO : RSP_ALLINFO) | (uint32_t)from << 8 | me << 16 | (cmd == CMD_DEVINFO ? 28u : 48u) << 24);
                devinfo(&out);
                if (cmd == CMD_ALLINFO) for (int i = 0; i < 20; i++) put(&out, 0);
                break;
            case CMD_GETCOND: {
                const maple_pad_t *k = &maple_pad;
                put(&out, RSP_DATATRF | (uint32_t)from << 8 | me << 16 | 3u << 24);
                put(&out, FUNC_CONTROLLER);
                put(&out, (uint32_t)(uint16_t)~k->buttons | (uint32_t)k->rtrig << 16 | (uint32_t)k->ltrig << 24);
                put(&out, (uint32_t)k->joyx | (uint32_t)k->joyy << 8 | 0x80u << 16 | 0x80u << 24);
                break;
            }
            case CMD_RESET: case CMD_KILL:
                put(&out, RSP_ACK | (uint32_t)from << 8 | me << 16);
                break;
            default: {
                static int warned;
                if (warned++ < 8) fprintf(stderr, "[maple] unhandled command %d to %02X\n", cmd, to);
                put(&out, RSP_UNKNOWN | (uint32_t)from << 8 | me << 16);
            }
            }
        }
        p += 12 + 4u * len;
        if (last) break;
    }
}
