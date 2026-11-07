/* Sofdec (.SFD) movie decoding for the launcher's cutscene viewer.
 * An SFD is an MPEG-1 program stream: MPEG-1 video on stream 0xE0 (decoded by
 * pl_mpeg) and CRI ADX audio on stream 0xC0 (decoded here: 4-bit ADPCM in
 * 18-byte blocks, one block per channel in turn).
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define PL_MPEG_IMPLEMENTATION
#include "third_party/pl_mpeg.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    const uint8_t *data; size_t len, pos;          /* the whole file and our demux position */
    plm_buffer_t *vbuf; plm_video_t *video;
    /* ADX */
    uint8_t *abytes; size_t alen, acap, apos; int adx_ok, ch, rate, data_off; int c1, c2; int hist[2][2];
    int16_t *pcm; size_t npcm, cappcm;          /* decoded, interleaved */
    double fps; int w, h;
} sfd_t;

static void adx_header(sfd_t *s) {
    const uint8_t *h = s->abytes;
    if (s->alen < 32 || h[0] != 0x80 || h[1] != 0) return;
    int off = (h[2] << 8 | h[3]) + 4;
    if ((size_t)off > s->alen) return;
    s->ch = h[7]; s->rate = h[8] << 24 | h[9] << 16 | h[10] << 8 | h[11];
    int cutoff = h[16] << 8 | h[17];
    double a = sqrt(2.0) - cos(2.0 * M_PI * cutoff / s->rate), b = sqrt(2.0) - 1.0, c = (a - sqrt((a + b) * (a - b))) / b;
    s->c1 = (int)floor(c * 8192); s->c2 = (int)floor(c * c * -4096);
    s->data_off = off; s->apos = (size_t)off; s->adx_ok = s->ch >= 1 && s->ch <= 2 && s->rate > 0;
}
static void adx_decode(sfd_t *s) {
    while (s->adx_ok && s->apos + 18 * (size_t)s->ch <= s->alen) {
        const uint8_t *blk = s->abytes + s->apos;
        int scale0 = blk[0] << 8 | blk[1];
        if (scale0 & 0x8000) { s->adx_ok = 0; break; }     /* end marker */
        if (s->npcm + 32 * (size_t)s->ch > s->cappcm) { s->cappcm = (s->cappcm + 32 * (size_t)s->ch) * 2; s->pcm = realloc(s->pcm, s->cappcm * 2); }
        for (int c = 0; c < s->ch; c++) {
            const uint8_t *b = blk + 18 * c; int scale = (b[0] << 8 | b[1]) & 0x1FFF;
            for (int i = 0; i < 32; i++) {
                int nib = i & 1 ? b[2 + i / 2] & 15 : b[2 + i / 2] >> 4; if (nib & 8) nib -= 16;
                int v = nib * scale + ((s->c1 * s->hist[c][0] + s->c2 * s->hist[c][1]) >> 12);
                if (v > 32767) v = 32767; if (v < -32768) v = -32768;
                s->hist[c][1] = s->hist[c][0]; s->hist[c][0] = v;
                s->pcm[s->npcm + (size_t)i * s->ch + c] = (int16_t)v;
            }
        }
        s->npcm += 32 * (size_t)s->ch; s->apos += 18 * (size_t)s->ch;
    }
}
/* next PES payload (MPEG-1 program stream); returns stream id or 0 at the end */
static int demux_next(sfd_t *s, const uint8_t **pay, size_t *plen) {
    const uint8_t *d = s->data;
    while (s->pos + 6 <= s->len) {
        if (d[s->pos] || d[s->pos + 1] || d[s->pos + 2] != 1) { s->pos++; continue; }
        int id = d[s->pos + 3];
        if (id == 0xBA) { s->pos += (d[s->pos + 4] >> 6) == 1 ? 14 + (d[s->pos + 13] & 7) : 12; continue; }   /* pack header */
        if (id < 0xB9) { s->pos += 4; continue; }
        if (id == 0xB9) return 0;                                                   /* end code */
        size_t plen_all = (size_t)(d[s->pos + 4] << 8 | d[s->pos + 5]), start = s->pos + 6, end = start + plen_all;
        if (end > s->len) end = s->len;
        s->pos = end;
        if (id != 0xE0 && id != 0xC0) continue;                                     /* system header, padding, private */
        size_t q = start;
        while (q < end && d[q] == 0xFF) q++;                                        /* stuffing */
        if (q < end && (d[q] & 0xC0) == 0x40) q += 2;                               /* STD buffer size */
        if (q < end && (d[q] & 0xF0) == 0x20) q += 5;                               /* PTS */
        else if (q < end && (d[q] & 0xF0) == 0x30) q += 10;                         /* PTS + DTS */
        else if (q < end && d[q] == 0x0F) q += 1;
        if (q > end) continue;
        *pay = d + q; *plen = end - q;
        return id;
    }
    return 0;
}
/* demux the whole file: video stream into one buffer for pl_mpeg, audio decoded to PCM */
sfd_t *sfd_open(uint8_t *data, size_t len) {
    sfd_t *s = calloc(1, sizeof *s);
    s->data = data; s->len = len;
    uint8_t *v = malloc(len); size_t vl = 0;
    const uint8_t *pay; size_t plen; int id;
    while ((id = demux_next(s, &pay, &plen))) {
        if (id == 0xE0) { memcpy(v + vl, pay, plen); vl += plen; }
        else {
            if (s->alen + plen > s->acap) { s->acap = (s->alen + plen) * 2; s->abytes = realloc(s->abytes, s->acap); }
            memcpy(s->abytes + s->alen, pay, plen); s->alen += plen;
        }
    }
    adx_header(s); adx_decode(s);
    s->vbuf = plm_buffer_create_with_memory(v, vl, 1);
    s->video = plm_video_create_with_buffer(s->vbuf, 1);
    s->fps = plm_video_get_framerate(s->video); if (s->fps < 1) s->fps = 29.97;
    return s;
}
plm_frame_t *sfd_next_frame(sfd_t *s) {
    plm_frame_t *f = plm_video_decode(s->video);
    if (f) { s->w = f->width; s->h = f->height; }
    return f;
}
/* take up to max interleaved samples of decoded audio */
size_t sfd_take_audio(sfd_t *s, int16_t *dst, size_t max) {
    size_t n = s->npcm < max ? s->npcm : max;
    memcpy(dst, s->pcm, n * 2); memmove(s->pcm, s->pcm + n, (s->npcm - n) * 2); s->npcm -= n;
    return n;
}
int sfd_audio_channels(sfd_t *s) { return s->ch; }
int sfd_audio_rate(sfd_t *s) { return s->rate; }
double sfd_fps(sfd_t *s) { return s->fps; }
void sfd_close(sfd_t *s) {
    plm_video_destroy(s->video);
    free(s->abytes); free(s->pcm); free(s);
}
