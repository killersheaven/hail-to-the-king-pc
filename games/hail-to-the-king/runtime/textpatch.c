/* Replaces the game's memory-card wording with save-slot wording.
 *
 * The messages live in a text table inside the game's data files (and a few
 * in 1ST_READ.BIN) as zero-terminated strings. Each replacement is no longer
 * than the original and is padded with zero bytes, so the table layout and
 * every pointer into it stay valid. Called on each disc read as it lands in
 * RAM (with a small overlap for strings split across reads) and once on the
 * program at start-up. DC_NOTEXTPATCH=1 turns it off.
 */
#include "sh4ctx.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *const tp[][2] = {
    { "VMU Selection", "Save Slots" },
    { "A VMU with at least 23 blocks of free space is required for saving. Please insert a VMU or press $A032048128{$C to exit.",
      "This save slot cannot be used. Please choose another slot or press $A032048128{$C to exit." },
    { "No game files were found on the VMU. Please insert another VMU or press $A032048128{$C to exit.",
      "This save slot is empty. Please choose another slot or press $A032048128{$C to exit." },
    { "There are not enough free memory blocks. A VMU with at least 23 clocks of space is required for saving. Please insert a VMU or press $A000096032~$C to exit.",
      "This save slot cannot be used. Please choose another slot or press $A000096032~$C to exit." },
    { "Please insert a VMU or press $A032048128{$C to exit.", "Choose a save slot or press $A032048128{$C to exit." },
    { "A game file already exists on the VMU.  Are you sure you want to overwrite it?",
      "This slot already holds a saved game. Are you sure you want to overwrite it?" },
    { "VMU is not formatted. Do you want to format?", "This save slot is not set up. Set it up now?" },
    { "Formatting VMU. Do not remove VMU, reset, power off or interrupt in any way.", "Setting up the save slot. Please wait." },
    { "Insufficient free blocks or no VMU is inserted. Please insert a VMU with at least 23 free blocks or press $A032048128{$C to exit.",
      "No save slot is available. Press $A032048128{$C to exit." },
    { "Saving, do not Power off.", "Saving, please wait." },
    { "Available block", "Free" },
    { "Unavailable block", "In use" },
    { "Controller A, slot 1", "Save Slot 1" }, { "Controller A, slot 2", "Save Slot 2" },
    { "Controller B, slot 1", "Save Slot 3" }, { "Controller B, slot 2", "Save Slot 4" },
    { "Controller C, slot 1", "Save Slot 5" }, { "Controller C, slot 2", "Save Slot 6" },
    { "Controller D, slot 1", "Save Slot 7" }, { "Controller D, slot 2", "Save Slot 8" },
    { "Controller A, Slot 1", "Save Slot 1" }, { "Controller A, Slot 2", "Save Slot 2" },
    { "Controller B, Slot 1", "Save Slot 3" }, { "Controller B, Slot 2", "Save Slot 4" },
    { "Controller C, Slot 1", "Save Slot 5" }, { "Controller C, Slot 2", "Save Slot 6" },
    { "Controller D, Slot 1", "Save Slot 7" }, { "Controller D, Slot 2", "Save Slot 8" },
};
enum { NTP = sizeof tp / sizeof *tp };
static uint8_t *find(uint8_t *p, uint8_t *end, const char *s, size_t n) {   /* memmem is not portable (mingw) */
    for (; p + n <= end; p++) { p = memchr(p, (unsigned char)s[0], (size_t)(end - p - n + 1)); if (!p) return NULL; if (!memcmp(p, s, n)) return p; }
    return NULL;
}

/* region [a, a+len) of guest RAM */
void text_patch(uint32_t a, uint32_t len) {
    static int off = -1; if (off < 0) off = getenv("DC_NOTEXTPATCH") != NULL;
    if (off) return;
    uint32_t base = a & 0x00FFFFFFu;
    if (base > 0x00FFFFFFu) return;
    if (base >= 256) { base -= 256; len += 256; }                 /* strings split across two reads */
    if (base + len > 0x01000000u) len = 0x01000000u - base;
    uint8_t *m = dc_ram + base;
    static int logged;
    for (int i = 0; i < NTP; i++) {
        size_t fl = strlen(tp[i][0]) + 1, tl = strlen(tp[i][1]);
        if (tl >= fl) { static int w; if (!w++) fprintf(stderr, "[text] replacement %d is too long, skipped\n", i); continue; }
        uint8_t *p = m, *end = m + len;
        while (p + fl <= end && (p = find(p, end, tp[i][0], fl))) {
            if (p > dc_ram && p[-1] != 0) { p++; continue; }       /* whole strings only */
            memset(p, 0, fl); memcpy(p, tp[i][1], tl);
            if (logged++ < 40) fprintf(stderr, "[text] \"%.24s\" -> \"%s\" at %08X\n", tp[i][0], tp[i][1], 0x8C000000u + (uint32_t)(p - dc_ram));
            p += fl;
        }
    }
}
