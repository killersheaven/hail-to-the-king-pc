/* Minimal ISO 9660 reader for the game's data track (high-density area).
 * Used by the launcher to read game files (texts, art) from the player's disc.
 * Flat root directory plus one level of subdirectories; names without ";1".
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

int disc_read_sector(uint32_t fad, uint8_t *dst);
#define HD_FAD 45150u                 /* LBA 45000 + 150: start of the data track */

typedef struct { char name[64]; uint32_t fad, size; int dir; } iso_entry;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* calls fn for each entry of the directory at (lba, size); returns count */
static int iso_dir(uint32_t lba, uint32_t size, void (*fn)(const iso_entry *, void *), void *ud) {
    uint8_t s[2048]; int n = 0;
    for (uint32_t off = 0; off < size; off += 2048) {
        if (!disc_read_sector(150 + lba + off / 2048, s)) return n;
        for (uint32_t i = 0; i < 2048; ) {
            uint8_t len = s[i]; if (!len) break;
            uint8_t nl = s[i + 32];
            iso_entry e; memset(&e, 0, sizeof e);
            e.fad = 150 + le32(s + i + 2);          /* GD-ROM: extents are absolute LBAs */ e.size = le32(s + i + 10); e.dir = (s[i + 25] & 2) != 0;
            if (!(nl == 1 && (s[i + 33] == 0 || s[i + 33] == 1))) {
                int k = 0; for (; k < nl && k < 63 && s[i + 33 + k] != ';'; k++) e.name[k] = (char)s[i + 33 + k];
                e.name[k] = 0; fn(&e, ud); n++;
            }
            i += len;
        }
    }
    return n;
}
typedef struct { const char *want; iso_entry found; int ok; } find_ctx;
static void find_cb(const iso_entry *e, void *ud) {
    find_ctx *f = ud;
    if (!f->ok && !strcasecmp(e->name, f->want)) { f->found = *e; f->ok = 1; }
}
static int root(uint32_t *lba, uint32_t *size) {
    uint8_t pvd[2048];
    if (!disc_read_sector(HD_FAD + 16, pvd) || memcmp(pvd + 1, "CD001", 5)) return 0;
    *lba = le32(pvd + 156 + 2); *size = le32(pvd + 156 + 10);
    return 1;
}
/* path "NAME.EXT" or "DIR/NAME.EXT" */
int iso_find(const char *path, uint32_t *fad, uint32_t *size) {
    uint32_t lba, sz; if (!root(&lba, &sz)) return 0;
    char part[64]; const char *p = path;
    for (;;) {
        const char *sl = strchr(p, '/'); size_t l = sl ? (size_t)(sl - p) : strlen(p);
        if (l >= sizeof part) return 0;
        memcpy(part, p, l); part[l] = 0;
        find_ctx f = { part, {{0}}, 0 };
        iso_dir(lba, sz, find_cb, &f);
        if (!f.ok) return 0;
        if (!sl) { *fad = f.found.fad; *size = f.found.size; return 1; }
        lba = f.found.fad - 150; sz = f.found.size; p = sl + 1;
    }
}
/* read a whole file into a malloc'ed buffer */
uint8_t *iso_load(const char *path, uint32_t *size_out) {
    uint32_t fad, size; if (!iso_find(path, &fad, &size)) return NULL;
    uint8_t *buf = malloc(size + 2048 + 1);
    for (uint32_t off = 0; off < size; off += 2048) if (!disc_read_sector(fad + off / 2048, buf + off)) { free(buf); return NULL; }
    buf[size] = 0; if (size_out) *size_out = size;
    return buf;
}
static void list_cb(const iso_entry *e, void *ud) { (void)ud; fprintf(stderr, "[iso] %-24s FAD %7u  %10u bytes%s\n", e->name, e->fad, e->size, e->dir ? "  <dir>" : ""); }
void iso_list(void) { uint32_t lba, sz; if (root(&lba, &sz)) iso_dir(lba, sz, list_cb, NULL); else fprintf(stderr, "[iso] no ISO 9660 volume\n"); }
/* calls fn(name, size) for each file in the root directory */
typedef struct { void (*fn)(const char *, uint32_t, void *); void *ud; } list_ctx;
static void listcb2(const iso_entry *e, void *ud) { list_ctx *c = ud; if (!e->dir) c->fn(e->name, e->size, c->ud); }
void iso_list_cb(void (*fn)(const char *name, uint32_t size, void *ud), void *ud) {
    uint32_t lba, sz; if (!root(&lba, &sz)) return;
    list_ctx c = { fn, ud }; iso_dir(lba, sz, listcb2, &c);
}
