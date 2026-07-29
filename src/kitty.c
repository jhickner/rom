#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "emu.h"

#define CHUNK 4096

static const char b64t[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64enc(const unsigned char *in, size_t n, char *out) {
    size_t o = 0, i = 0;
    for (; i + 2 < n; i += 3) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        out[o++] = b64t[(v >> 18) & 63]; out[o++] = b64t[(v >> 12) & 63];
        out[o++] = b64t[(v >> 6) & 63];  out[o++] = b64t[v & 63];
    }
    if (i < n) {
        int rem = (int)(n - i);
        unsigned v = (unsigned)in[i] << 16;
        if (rem == 2) v |= (unsigned)in[i + 1] << 8;
        out[o++] = b64t[(v >> 18) & 63]; out[o++] = b64t[(v >> 12) & 63];
        out[o++] = rem == 2 ? b64t[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    return o;
}

static int writeall(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w; n -= (size_t)w;
    }
    return 0;
}

// Assemble the full chunked transmission for one frame into `out`. When
// `natural` is set the c=/r= fields are omitted, which tells the terminal to
// draw the image at exactly its pixel size instead of stretching it to a cell
// rect.
static size_t build_frame(char *out, const char *b64, size_t blen,
                          int w, int h, const Layout *l) {
    size_t o = 0, off = 0;
    int first = 1;
    o += (size_t)sprintf(out + o, "\x1b[%d;%dH", l->y, l->x);
    while (off < blen) {
        size_t n = blen - off;
        if (n > CHUNK) n = CHUNK;
        int more = (off + n < blen);
        if (first) {
            o += (size_t)sprintf(out + o,
                    "\x1b_Ga=T,f=24,s=%d,v=%d,i=1,p=1,q=2,C=1", w, h);
            if (!l->natural)
                o += (size_t)sprintf(out + o, ",c=%d,r=%d", l->cols, l->rows);
            o += (size_t)sprintf(out + o, ",m=%d;", more);
        } else {
            o += (size_t)sprintf(out + o, "\x1b_Gm=%d;", more);
        }
        memcpy(out + o, b64 + off, n);
        o += n;
        out[o++] = 0x1b; out[o++] = '\\';
        off += n;
        first = 0;
    }
    return o;
}

// Text under the image is aligned to the image, not the terminal, so it stays
// visually attached to the picture rather than drifting to the window edge.
static void draw_status(const Layout *l, char *buf, size_t cap,
                        const char *osd, const char *status) {
    size_t o = 0;
    int termw = l->term_cols > 0 ? l->term_cols : 80;
    int row = l->status_row;
    if (row < 1) row = 1;

    int sx = l->x > 0 ? l->x : 1;
    int sw = l->cols > 0 ? l->cols : termw;
    if (sx + sw - 1 > termw) sw = termw - sx + 1;
    if (sw < 1) sw = 1;

    o += (size_t)snprintf(buf + o, cap - o, "\x1b[%d;1H\x1b[2K", row);

    bool has_osd = osd && *osd;
    bool has_stat = status && *status;
    int olen = has_osd ? (int)strlen(osd) : 0;
    int slen = has_stat ? (int)strlen(status) : 0;

    if (has_osd && has_stat && olen + slen + 2 <= sw) {
        // Both fit: message on the left edge of the image, stats on the right.
        o += (size_t)snprintf(buf + o, cap - o, "\x1b[%d;%dH\x1b[1;97m%s\x1b[0m",
                              row, sx, osd);
        o += (size_t)snprintf(buf + o, cap - o, "\x1b[%d;%dH\x1b[2;37m%s\x1b[0m",
                              row, sx + sw - slen, status);
    } else if (has_osd) {
        if (olen > sw) olen = sw;
        int col = sx + (sw - olen) / 2;
        if (col < 1) col = 1;
        o += (size_t)snprintf(buf + o, cap - o, "\x1b[%d;%dH\x1b[1;97m%.*s\x1b[0m",
                              row, col, olen, osd);
    } else if (has_stat) {
        if (slen > sw) slen = sw;
        int col = sx + (sw - slen) / 2;
        if (col < 1) col = 1;
        o += (size_t)snprintf(buf + o, cap - o, "\x1b[%d;%dH\x1b[2;37m%.*s\x1b[0m",
                              row, col, slen, status);
    }
    (void)o;
}

static void *render_thread(void *arg) {
    Renderer *r = (Renderer *)arg;
    char *b64 = NULL, *msg = NULL;
    size_t b64cap = 0, msgcap = 0;
    char osd[OSD_MSG_MAX], status[256], sbuf[512];

    for (;;) {
        pthread_mutex_lock(&r->m);
        while (r->ready < 0 && r->running)
            pthread_cond_wait(&r->cv, &r->m);
        if (!r->running) { pthread_mutex_unlock(&r->m); break; }

        int idx = r->ready;
        r->ready = -1;
        r->busy = idx;
        int w = r->w[idx], h = r->h[idx];
        Layout l = r->layout;
        bool clear = r->layout_dirty && l.allow_clear;
        r->layout_dirty = false;
        double t = now_sec();
        snprintf(osd, sizeof osd, "%s", r->osd_until > t ? r->osd : "");
        snprintf(status, sizeof status, "%s", r->show_status ? r->status : "");
        uint8_t *src = r->buf[idx];
        pthread_mutex_unlock(&r->m);

        size_t raw = (size_t)w * h * 3;
        size_t need_b64 = raw * 4 / 3 + 8;
        if (need_b64 > b64cap) {
            char *nb = realloc(b64, need_b64);
            if (!nb) goto release;
            b64 = nb; b64cap = need_b64;
        }
        // Worst case: one 12-byte escape wrapper per chunk, plus cursor moves.
        size_t need_msg = need_b64 + (need_b64 / CHUNK + 2) * 64 + 256;
        if (need_msg > msgcap) {
            char *nm = realloc(msg, need_msg);
            if (!nm) goto release;
            msg = nm; msgcap = need_msg;
        }

        if (clear) (void)writeall(r->ttyfd, "\x1b[2J", 4);

        size_t blen = b64enc(src, raw, b64);
        size_t n = build_frame(msg, b64, blen, w, h, &l);
        if (writeall(r->ttyfd, msg, n) == 0) {
            draw_status(&l, sbuf, sizeof sbuf, osd, status);
            (void)writeall(r->ttyfd, sbuf, strlen(sbuf));
        }

    release:
        pthread_mutex_lock(&r->m);
        r->busy = -1;
        r->sent++;
        pthread_cond_broadcast(&r->cv);
        pthread_mutex_unlock(&r->m);
    }
    free(b64);
    free(msg);
    return NULL;
}

int renderer_start(Renderer *r, int ttyfd) {
    memset(r, 0, sizeof *r);
    r->ready = r->busy = r->writing = -1;
    r->ttyfd = ttyfd;
    r->running = true;
    r->layout_dirty = true;
    pthread_mutex_init(&r->m, NULL);
    pthread_cond_init(&r->cv, NULL);
    if (pthread_create(&r->thread, NULL, render_thread, r) != 0) {
        r->running = false;
        return -1;
    }
    return 0;
}

void renderer_stop(Renderer *r) {
    if (!r->running) return;
    pthread_mutex_lock(&r->m);
    r->running = false;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->m);
    pthread_join(r->thread, NULL);
    for (int i = 0; i < RB_COUNT; i++) free(r->buf[i]);
    pthread_mutex_destroy(&r->m);
    pthread_cond_destroy(&r->cv);
    memset(r, 0, sizeof *r);
}

void renderer_submit(Renderer *r, const uint8_t *rgb, int w, int h) {
    size_t need = (size_t)w * h * 3;
    pthread_mutex_lock(&r->m);

    int idx = -1;
    for (int i = 0; i < RB_COUNT; i++)
        if (i != r->ready && i != r->busy) { idx = i; break; }
    if (idx < 0) { pthread_mutex_unlock(&r->m); return; }

    if (r->cap[idx] < need) {
        uint8_t *nb = realloc(r->buf[idx], need);
        if (!nb) { pthread_mutex_unlock(&r->m); return; }
        r->buf[idx] = nb;
        r->cap[idx] = need;
    }
    memcpy(r->buf[idx], rgb, need);
    r->w[idx] = w;
    r->h[idx] = h;
    if (r->ready >= 0) r->dropped++;   // replacing a frame the terminal never got
    r->ready = idx;
    pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->m);
}

void renderer_set_layout(Renderer *r, const Layout *l) {
    pthread_mutex_lock(&r->m);
    r->layout = *l;
    r->layout_dirty = true;
    pthread_mutex_unlock(&r->m);
}

void renderer_set_status(Renderer *r, const char *s) {
    pthread_mutex_lock(&r->m);
    snprintf(r->status, sizeof r->status, "%s", s);
    pthread_mutex_unlock(&r->m);
}

void renderer_set_osd(Renderer *r, const char *s, double seconds) {
    pthread_mutex_lock(&r->m);
    snprintf(r->osd, sizeof r->osd, "%s", s);
    r->osd_until = now_sec() + seconds;
    pthread_mutex_unlock(&r->m);
}
