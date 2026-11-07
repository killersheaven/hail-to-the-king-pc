/* GD-ROM disc access from the user's own disc image.
 *
 * Two sources are supported:
 *  - lite image: lite.bin + lite.ranges (HD-area sectors minus movie files; built by tools/mklite.py)
 *  - (planned) the Redump .cue/.bin set directly
 * FAD = physical sector address used by the syscalls = ISO LBA + 150.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

typedef struct { uint32_t start, end, off; } range_t;
static range_t *ranges; static uint32_t nranges;
static FILE *lite;
static uint64_t reads, zero_reads;

/* ---- Redump .cue/.bin set (raw 2352-byte sectors) ---- */
typedef struct { uint32_t start, end; FILE *f; int audio; } trk_t;
static trk_t trk[8]; static int ntrk;
static int open_cue(const char *cue) {
    FILE *c = fopen(cue, "r");
    if (!c) return 0;
    char dir[1024]; snprintf(dir, sizeof dir, "%s", cue);
    char *sl = strrchr(dir, '/'), *bs = strrchr(dir, '\\'); if (bs > sl) sl = bs;
    if (sl) sl[1] = 0; else dir[0] = 0;
    char line[1024]; int hd = 0; uint32_t lba = 0; FILE *cur = NULL;
    while (fgets(line, sizeof line, c)) {
        if (strstr(line, "HIGH-DENSITY")) { hd = 1; lba = 45000; }
        char *q = strchr(line, '"');
        if (!strncmp(line, "FILE", 4) && q) {
            char *e = strchr(q + 1, '"'); if (!e) continue; *e = 0;
            char p[2048]; snprintf(p, sizeof p, "%s%s", dir, q + 1);
            cur = fopen(p, "rb");
            if (!cur) { fprintf(stderr, "[disc] cannot open %s\n", p); fclose(c); return 0; }
        } else if (strstr(line, "TRACK") && cur && ntrk < 8) {
            fseek(cur, 0, SEEK_END); long long sz = ftell(cur);
#ifdef _WIN32
            sz = _ftelli64(cur);
#endif
            uint32_t n = (uint32_t)(sz / 2352);
            trk[ntrk].start = lba; trk[ntrk].end = lba + n; trk[ntrk].f = cur; trk[ntrk].audio = strstr(line, "AUDIO") != NULL;
            ntrk++; lba += n; cur = NULL;
        }
    }
    fclose(c);
    (void)hd;
    fprintf(stderr, "[disc] cue image: %d tracks\n", ntrk);
    for (int i = 0; i < ntrk; i++) fprintf(stderr, "[disc]   LBA %u-%u %s\n", trk[i].start, trk[i].end, trk[i].audio ? "audio" : "data");
    return ntrk > 0;
}
static int cue_read(uint32_t lba, uint8_t *dst) {
    for (int i = 0; i < ntrk; i++) if (lba >= trk[i].start && lba < trk[i].end && !trk[i].audio) {
#ifdef _WIN32
        if (_fseeki64(trk[i].f, (long long)(lba - trk[i].start) * 2352 + 16, SEEK_SET)) return 0;
#else
        if (fseeko(trk[i].f, (off_t)(lba - trk[i].start) * 2352 + 16, SEEK_SET)) return 0;
#endif
        return fread(dst, 1, 2048, trk[i].f) == 2048;
    }
    return -1;
}
/* IP.BIN from the start of the high-density data track */
int disc_read_ip(uint8_t *dst32k) {
    if (!ntrk) return 0;
    for (int i = 0; i < 16; i++) if (cue_read(45000 + i, dst32k + i * 2048) != 1) return 0;
    return 1;
}

int disc_open(const char *dir) {
    char p[1024];
    size_t L = strlen(dir);
    if (L > 4 && (!strcmp(dir + L - 4, ".cue") || !strcmp(dir + L - 4, ".CUE"))) return open_cue(dir);
    snprintf(p, sizeof p, "%s/lite.ranges", dir);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "[disc] cannot open %s\n", p); return 0; }
    if (fread(&nranges, 4, 1, f) != 1) { fclose(f); return 0; }
    ranges = calloc(nranges, sizeof *ranges);
    if (fread(ranges, sizeof *ranges, nranges, f) != nranges) { fclose(f); return 0; }
    fclose(f);
    snprintf(p, sizeof p, "%s/lite.bin", dir);
    lite = fopen(p, "rb");
    if (!lite) { fprintf(stderr, "[disc] cannot open %s\n", p); return 0; }
    fprintf(stderr, "[disc] lite image: %u ranges\n", nranges);
    return 1;
}

/* optional extra sectors for testing with the lite image (e.g. movie files):
 * DC_EXTRA=<dir> with extra.bin + extra.ranges.txt ("startLBA endLBA offsetSectors" per line) */
static range_t xr[16]; static int nxr = -1; static FILE *xbin;
static int extra_read(uint32_t lba, uint8_t *dst) {
    if (nxr < 0) {
        nxr = 0; const char *d = getenv("DC_EXTRA");
        if (d) {
            char p[1024]; snprintf(p, sizeof p, "%s/extra.ranges.txt", d);
            FILE *f = fopen(p, "r");
            if (f) { while (nxr < 16 && fscanf(f, "%u %u %u", &xr[nxr].start, &xr[nxr].end, &xr[nxr].off) == 3) nxr++; fclose(f); }
            snprintf(p, sizeof p, "%s/extra.bin", d); xbin = fopen(p, "rb");
            fprintf(stderr, "[disc] extra sectors: %d ranges\n", nxr);
        }
    }
    for (int i = 0; i < nxr; i++) if (lba >= xr[i].start && lba < xr[i].end && xbin) {
        fseek(xbin, (long)(xr[i].off + lba - xr[i].start) * 2048L, SEEK_SET);
        return fread(dst, 1, 2048, xbin) == 2048;
    }
    return -1;
}

int disc_read_sector(uint32_t fad, uint8_t *dst) {
    uint32_t lba = fad - 150;
    reads++;
    { static int lg = -1; if (lg < 0) lg = getenv("DC_DISCLOG") != NULL;
      static uint32_t last = ~0u; extern uint64_t dc_frame_count(void);
      if (lg && lba != last + 1) fprintf(stderr, "[disclog] field %llu LBA %u\n", (unsigned long long)dc_frame_count(), lba);
      last = lba; }
    if (ntrk) { int r = cue_read(lba, dst); if (r >= 0) return r; zero_reads++; memset(dst, 0, 2048); return 1; }
    { int r = extra_read(lba, dst); if (r >= 0) return r; }
    for (uint32_t i = 0; i < nranges; i++) {
        if (lba >= ranges[i].start && lba < ranges[i].end) {
            if (!lite) return 0;
            if (fseek(lite, (long)(ranges[i].off + (lba - ranges[i].start)) * 2048L, SEEK_SET)) return 0;
            return fread(dst, 1, 2048, lite) == 2048;
        }
    }
    /* sector not in the lite image (movie data / low-density area): zeros */
    zero_reads++;
    memset(dst, 0, 2048);
    return 1;
}

/* Track layout of Evil Dead: Hail to the King (USA), Redump GD-ROM:
 * low density: T1 data @ FAD 150, T2 audio; high density: T3 data @ 45150, T4 audio, T5 data. */
#define TOC_ENT(ctrl, adr, fad) (((uint32_t)(ctrl) << 28) | ((uint32_t)(adr) << 24) | (fad))
void disc_toc(int area, uint32_t *toc) {
    for (int i = 0; i < 99; i++) toc[i] = 0xFFFFFFFFu;
    if (area == 0) {
        toc[0] = TOC_ENT(4, 1, 150);
        toc[1] = TOC_ENT(0, 1, 604 + 150 + 150);
        toc[99] = TOC_ENT(4, 1, 0) | (1u << 16);
        toc[100] = TOC_ENT(0, 1, 0) | (2u << 16);
        toc[101] = TOC_ENT(0, 1, 604 + 675 + 150);
    } else {
        toc[2] = TOC_ENT(4, 1, 45000 + 150);
        toc[3] = TOC_ENT(0, 1, 48386 + 150 + 150);
        toc[4] = TOC_ENT(4, 1, 48912 + 225 + 150);
        toc[99] = TOC_ENT(4, 1, 0) | (3u << 16);
        toc[100] = TOC_ENT(4, 1, 0) | (5u << 16);
        toc[101] = TOC_ENT(4, 1, 549150 + 150);
    }
}

void disc_report(void) {
    fprintf(stderr, "[disc] sectors read: %llu (%llu outside lite image)\n",
            (unsigned long long)reads, (unsigned long long)zero_reads);
}
