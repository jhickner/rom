#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rom.h"

#define CHUNK 4096

static const char b64t[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>

// 48 input bytes to 64 output bytes a pass. vqtbl4q_u8 indexes the whole
// 64-byte alphabet in one instruction, so the 6-bit groups turn into ASCII
// without the usual branchless offset ladder.
static size_t b64enc_bulk(const unsigned char *in, size_t n, char *out,
                          size_t *io) {
    uint8x16x4_t tbl;
    tbl.val[0] = vld1q_u8((const uint8_t *)b64t);
    tbl.val[1] = vld1q_u8((const uint8_t *)b64t + 16);
    tbl.val[2] = vld1q_u8((const uint8_t *)b64t + 32);
    tbl.val[3] = vld1q_u8((const uint8_t *)b64t + 48);
    const uint8x16_t m3f = vdupq_n_u8(0x3f);

    size_t i = 0, o = 0;
    for (; i + 48 <= n; i += 48, o += 64) {
        uint8x16x3_t v = vld3q_u8(in + i);
        uint8x16x4_t q;
        q.val[0] = vshrq_n_u8(v.val[0], 2);
        q.val[1] = vandq_u8(vorrq_u8(vshlq_n_u8(v.val[0], 4),
                                     vshrq_n_u8(v.val[1], 4)), m3f);
        q.val[2] = vandq_u8(vorrq_u8(vshlq_n_u8(v.val[1], 2),
                                     vshrq_n_u8(v.val[2], 6)), m3f);
        q.val[3] = vandq_u8(v.val[2], m3f);

        uint8x16x4_t r;
        for (int k = 0; k < 4; k++) r.val[k] = vqtbl4q_u8(tbl, q.val[k]);
        vst4q_u8((uint8_t *)out + o, r);
    }
    *io = o;
    return i;
}
#else
static size_t b64enc_bulk(const unsigned char *in, size_t n, char *out,
                          size_t *io) {
    (void)in; (void)n; (void)out;
    *io = 0;
    return 0;
}
#endif

static size_t b64enc(const unsigned char *in, size_t n, char *out) {
    size_t o = 0, i = b64enc_bulk(in, n, out, &o);
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

static void draw_outline(uint8_t *rgb, int w, int h, int band) {
    static const uint8_t hue[4][3] = {
        { 255, 32, 32 }, { 32, 255, 32 }, { 64, 160, 255 }, { 255, 255, 32 }
    };
    const uint8_t *c = hue[band & 3];
    for (int x = 0; x < w; x++) {
        memcpy(rgb + (size_t)x * 3, c, 3);
        memcpy(rgb + ((size_t)(h - 1) * w + x) * 3, c, 3);
    }
    for (int y = 0; y < h; y++) {
        memcpy(rgb + (size_t)y * w * 3, c, 3);
        memcpy(rgb + ((size_t)y * w + w - 1) * 3, c, 3);
    }
}

// Assemble the chunked transmission for one band into `out`; a picture sent
// whole is band 0 of 1. `natural` omits c=/r=, which draws the image at its own
// pixel size rather than stretching it to a cell rect; bands always stretch.
//
// Under tmux the image is transmitted without a placement (a=t) and then given
// a virtual one (U=1), which draws nothing by itself: the placeholder cells
// already on screen are what the terminal fills in. The placement is re-sent
// every time because replacing an image id also drops its placements.
static size_t build_band(char *out, const char *b64, size_t blen,
                         int w, int bh, const Layout *l, int band) {
    size_t o = 0, off = 0;
    int first = 1;
    char head[96];
    bool tmux = gfx_tmux();
    int id = GFX_IMAGE_ID + band;
    int cy0, cy1;
    layout_band_cells(l, band, &cy0, &cy1);
    int brows = cy1 - cy0;
    bool natural = l->natural && l->bands <= 1;

    if (!tmux) o += (size_t)sprintf(out + o, "\x1b[%d;%dH", l->y + cy0, l->x);
    while (off < blen) {
        size_t n = blen - off;
        if (n > CHUNK) n = CHUNK;
        int more = (off + n < blen);
        if (first && tmux) {
            sprintf(head, "a=t,f=24,s=%d,v=%d,i=%d,q=2,m=%d;",
                    w, bh, id, more);
        } else if (first) {
            int p = sprintf(head, "a=T,f=24,s=%d,v=%d,i=%d,p=1,q=2,C=1",
                            w, bh, id);
            if (!natural)
                p += sprintf(head + p, ",c=%d,r=%d", l->cols, brows);
            sprintf(head + p, ",m=%d;", more);
        } else {
            sprintf(head, "m=%d;", more);
        }
        o += gfx_apc(out + o, head, b64 + off, n);
        off += n;
        first = 0;
    }
    if (tmux) {
        sprintf(head, "a=p,U=1,i=%d,p=1,c=%d,r=%d,q=2",
                id, l->cols, brows);
        o += gfx_apc(out + o, head, NULL, 0);
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

// Blanks the cells the picture sits in. A full clear would take the user's
// scrollback with it in inline mode, so there the rows are erased one by one.
static void clear_image_area(int fd, const Layout *l) {
    if (l->allow_clear) {
        (void)writeall(fd, "\x1b[2J", 4);
        return;
    }
    int last = l->status_row > 0 ? l->status_row : l->y + l->rows;
    for (int y = l->y; y <= last; y++) {
        char buf[32];
        int n = sprintf(buf, "\x1b[%d;1H\x1b[2K", y);
        (void)writeall(fd, buf, (size_t)n);
    }
}

// The main thread can hide the pane between a frame being picked up and its
// write going out; with the tty held, this is the last chance to notice.
static bool hidden_now(Renderer *r) {
    pthread_mutex_lock(&r->m);
    bool h = r->hidden;
    pthread_mutex_unlock(&r->m);
    return h;
}

static void *render_thread(void *arg) {
    Renderer *r = (Renderer *)arg;
    char *b64 = NULL, *msg = NULL;
    size_t b64cap = 0, msgcap = 0;
    char osd[OSD_MSG_MAX], status[256], sbuf[512];
    char bsu[64], esu[64];
    size_t bsulen = gfx_wrap(bsu, "\x1b[?2026h", 8);
    size_t esulen = gfx_wrap(esu, "\x1b[?2026l", 8);

    for (;;) {
        size_t wrote = 0;
        double wsec = 0.0;
        unsigned sent_bands = 0;
        int nb = 1;

        pthread_mutex_lock(&r->m);
        while (r->ready < 0 && !r->status_only && r->running)
            pthread_cond_wait(&r->cv, &r->m);
        if (!r->running) { pthread_mutex_unlock(&r->m); break; }

        // Hidden: drop whatever was queued rather than encoding a frame that
        // is not going anywhere. Showing again queues the last one afresh.
        if (r->hidden) {
            r->ready = -1;
            r->status_only = false;
            pthread_mutex_unlock(&r->m);
            continue;
        }

        // Nothing new to draw: refresh the text under the image and go back to
        // sleep, leaving the picture the terminal already has in place.
        if (r->ready < 0) {
            r->status_only = false;
            Layout l = r->layout;
            double now = now_sec();
            snprintf(osd, sizeof osd, "%s", r->osd_until > now ? r->osd : "");
            snprintf(status, sizeof status, "%s", r->show_status ? r->status : "");
            pthread_mutex_unlock(&r->m);

            pthread_mutex_lock(&r->wm);
            if (!hidden_now(r)) {
                draw_status(&l, sbuf, sizeof sbuf, osd, status);
                (void)writeall(r->ttyfd, sbuf, strlen(sbuf));
            }
            pthread_mutex_unlock(&r->wm);
            continue;
        }
        r->status_only = false;

        int idx = r->ready;
        r->ready = -1;
        r->busy = idx;
        int w = r->w[idx], h = r->h[idx];
        Layout l = r->layout;
        bool relayout = r->layout_dirty;
        bool clear = relayout && l.allow_clear;
        // The placeholder cells are the placement under tmux, so a new layout
        // means laying them down again over the new rectangle.
        bool place = relayout && gfx_tmux();
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
        // Worst case: every band changed, one wrapped escape per chunk plus a
        // placement and a cursor move each.
        size_t need_msg = need_b64 + (need_b64 / CHUNK + 2) * gfx_apc_max(0) +
                          (size_t)GFX_MAX_BANDS * (gfx_apc_max(0) + 64) + 256;
        if (need_msg > msgcap) {
            char *nm = realloc(msg, need_msg);
            if (!nm) goto release;
            msg = nm; msgcap = need_msg;
        }

        nb = l.bands > 1 ? l.bands : 1;
        // Stretched bands must divide evenly; an exact picture only has to
        // still fill the cell rect it was padded to.
        if (nb > 1) {
            if (l.exact) {
                if (h != l.rows * l.cell_h) nb = 1;
            } else if (h % nb || l.rows % nb) {
                nb = 1;
            }
        }
        l.bands = nb;
        bool all = relayout || !r->shown ||
                   r->shown_w != w || r->shown_h != h || r->shown_bands != nb;
        if (r->shown_cap < raw) {
            uint8_t *ns = realloc(r->shown, raw);
            if (!ns) goto release;
            r->shown = ns;
            r->shown_cap = raw;
            all = true;
        }

        pthread_mutex_lock(&r->wm);
        if (hidden_now(r)) { pthread_mutex_unlock(&r->wm); goto release; }
        // DECSET 2026, synchronized output.
        bool sync = nb > 1 && gfx_sync();
        if (sync) (void)writeall(r->ttyfd, bsu, bsulen);
        if (clear) (void)writeall(r->ttyfd, "\x1b[2J", 4);
        if (place) gfx_write_placeholders(r->ttyfd, &l);

        size_t n = 0;
        size_t stride = (size_t)w * 3;
        bool debug = l.debug && nb > 1;
        for (int b = 0; b < nb; b++) {
            int py0, py1;
            layout_band_rows(&l, b, h, &py0, &py1);
            size_t off = (size_t)py0 * stride, len = (size_t)(py1 - py0) * stride;
            bool changed = all || memcmp(r->shown + off, src + off, len) != 0;
            // An outline drawn last frame stays on screen until the band is
            // sent again, so an unchanged one goes out clean to erase it.
            bool erase = r->outlined[b] && !changed;
            if (!changed && !erase) continue;

            const uint8_t *payload = src + off;
            if (debug && changed) {
                if (r->band_cap < len) {
                    uint8_t *nbuf = realloc(r->band, len);
                    if (nbuf) { r->band = nbuf; r->band_cap = len; }
                }
                if (r->band_cap >= len) {
                    memcpy(r->band, src + off, len);
                    draw_outline(r->band, w, py1 - py0, b);
                    payload = r->band;
                }
            }
            size_t blen = b64enc(payload, len, b64);
            n += build_band(msg + n, b64, blen, w, py1 - py0, &l, b);
            if (changed) memcpy(r->shown + off, src + off, len);
            r->outlined[b] = debug && changed;
            sent_bands++;
        }
        r->shown_w = w;
        r->shown_h = h;
        r->shown_bands = nb;

        double w0 = now_sec();
        if (n == 0 || writeall(r->ttyfd, msg, n) == 0) {
            wrote = n;
            draw_status(&l, sbuf, sizeof sbuf, osd, status);
            size_t sn = strlen(sbuf);
            (void)writeall(r->ttyfd, sbuf, sn);
            wrote += sn;
        }
        if (sync) (void)writeall(r->ttyfd, esu, esulen);
        wsec = now_sec() - w0;
        pthread_mutex_unlock(&r->wm);

    release:
        pthread_mutex_lock(&r->m);
        r->busy = -1;
        r->sent++;
        r->bytes += wrote;
        r->write_sec += wsec;
        r->bands_sent += sent_bands;
        r->bands_total += (unsigned)nb;
        pthread_cond_broadcast(&r->cv);
        pthread_mutex_unlock(&r->m);
    }
    free(b64);
    free(msg);
    return NULL;
}

int renderer_start(Renderer *r, int ttyfd) {
    memset(r, 0, sizeof *r);
    r->ready = r->busy = r->writing = r->last = -1;
    r->ttyfd = ttyfd;
    r->running = true;
    r->layout_dirty = true;
    pthread_mutex_init(&r->m, NULL);
    pthread_mutex_init(&r->wm, NULL);
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
    free(r->shown);
    free(r->band);
    pthread_mutex_destroy(&r->wm);
    pthread_mutex_destroy(&r->m);
    pthread_cond_destroy(&r->cv);
    memset(r, 0, sizeof *r);
}

void renderer_submit(Renderer *r, const uint8_t *rgb, int w, int h) {
    size_t need = (size_t)w * h * 3;
    pthread_mutex_lock(&r->m);

    // The terminal keeps the placement, so an identical frame is already on
    // screen and re-sending it costs a full base64 pass and a ~200KB write for
    // nothing. The newest submission is always either displayed or still
    // queued, so it is the right thing to compare against. A layout change
    // invalidates that and has to go out in full.
    if (r->last >= 0 && !r->layout_dirty &&
        r->w[r->last] == w && r->h[r->last] == h) {
        const uint8_t *prev = r->buf[r->last];
        if (memcmp(prev, rgb, need) == 0) {
            r->skipped++;
            // The clock and the OSD still move while the picture holds still.
            r->status_only = true;
            pthread_cond_signal(&r->cv);
            pthread_mutex_unlock(&r->m);
            return;
        }
        // Something moved. Tally which rows, to size up what a damage-region
        // scheme could skip. Only frames already committed to a base64 pass and
        // a full write pay for this second comparison.
        size_t stride = (size_t)w * 3;
        unsigned changed = 0;
        for (int y = 0; y < h; y++)
            if (memcmp(prev + (size_t)y * stride, rgb + (size_t)y * stride,
                       stride) != 0)
                changed++;
        r->rows_changed += changed;
        r->rows_total += (unsigned)h;
    }

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
    r->last = idx;
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

void renderer_set_hidden(Renderer *r, bool hidden) {
    if (!r->running) return;
    // The tty is taken first and held across the whole change, so a frame the
    // render thread is already carrying cannot land after the image is dropped.
    pthread_mutex_lock(&r->wm);
    pthread_mutex_lock(&r->m);
    bool changed = r->hidden != hidden;
    r->hidden = hidden;
    Layout l = r->layout;
    if (changed && !hidden) {
        // Nothing of the picture is left on screen: the image, and under tmux
        // its placeholder cells, both have to go out again in full.
        r->layout_dirty = true;
        if (r->last >= 0) r->ready = r->last;
        pthread_cond_signal(&r->cv);
    }
    pthread_mutex_unlock(&r->m);
    if (changed && hidden) {
        gfx_delete_image(r->ttyfd);
        clear_image_area(r->ttyfd, &l);
    }
    pthread_mutex_unlock(&r->wm);
}

void renderer_lock_tty(Renderer *r) {
    if (r->running) pthread_mutex_lock(&r->wm);
}

void renderer_unlock_tty(Renderer *r) {
    if (r->running) pthread_mutex_unlock(&r->wm);
}
