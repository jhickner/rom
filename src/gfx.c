#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rom.h"
#include "rowcolumn_diacritics.h"

// The codepoint kitty reserves for image placeholder cells.
#define PLACEHOLDER_CP 0x10EEEEu

static int      g_img_id;
static unsigned g_img_gen;

int gfx_image_id(void) {
    if (!g_img_id) {
        unsigned pid = (unsigned)getpid();
        // The three bytes travel as a foreground colour, so none may be zero.
        // The low byte starts at 1 and steps by 0x20, which leaves the band
        // offset room to run underneath it without carrying into the next.
        unsigned lo  = (((pid + g_img_gen) & 0x07u) << 5) | 1u;
        unsigned mid = ((((pid >> 8) & 0xFFu) + g_img_gen / 8u) & 0xFFu) | 1u;
        g_img_id = (int)(0x0A0000u | (mid << 8) | lo);
    }
    return g_img_id;
}

// Control data for dropping one of our images. The id has to reach the terminal
// in decimal, so it is formatted rather than stringified.
static void delete_head(char *out, size_t cap, int band) {
    snprintf(out, cap, "a=d,d=I,i=%d,q=2", gfx_image_id() + band);
}

static bool   g_tmux;
static bool   g_tmux_redraw;
static bool   g_sync;
static char   g_restore[2048];
static size_t g_restore_len;

// "tmux 3.7b" / "tmux next-3.8" -> 307 / 308. 0 when it cannot be parsed.
static int tmux_version(void) {
    FILE *p = popen("tmux -V 2>/dev/null", "r");
    if (!p) return 0;
    char buf[64] = "";
    bool got = fgets(buf, sizeof buf, p) != NULL;
    pclose(p);
    if (!got) return 0;
    const char *s = buf;
    while (*s && (*s < '0' || *s > '9')) s++;
    int maj = 0, min = 0;
    if (sscanf(s, "%d.%d", &maj, &min) < 2) return 0;
    return maj * 100 + min;
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

size_t gfx_apc(char *out, const char *head, const char *payload, size_t plen) {
    size_t o = 0;
    // tmux forwards an escape it does not understand only when it is wrapped in
    // a DCS with every ESC inside doubled. Base64 never contains one, so only
    // the two ESCs of the APC itself need it.
    if (g_tmux) {
        memcpy(out, "\x1bPtmux;", 7);
        o = 7;
        out[o++] = 0x1b;
    }
    out[o++] = 0x1b; out[o++] = '_'; out[o++] = 'G';
    size_t hl = strlen(head);
    memcpy(out + o, head, hl);
    o += hl;
    if (plen) {
        memcpy(out + o, payload, plen);
        o += plen;
    }
    if (g_tmux) out[o++] = 0x1b;
    out[o++] = 0x1b; out[o++] = '\\';
    if (g_tmux) { out[o++] = 0x1b; out[o++] = '\\'; }
    return o;
}

size_t gfx_apc_max(size_t plen) { return plen + 128; }

size_t gfx_wrap(char *out, const char *seq, size_t len) {
    if (!g_tmux) {
        memcpy(out, seq, len);
        return len;
    }
    memcpy(out, "\x1bPtmux;", 7);
    size_t o = 7;
    for (size_t i = 0; i < len; i++) {
        if (seq[i] == 0x1b) out[o++] = 0x1b;
        out[o++] = seq[i];
    }
    out[o++] = 0x1b; out[o++] = '\\';
    return o;
}

static void build_restore(void) {
    char head[48];
    size_t n = 0;
    for (int b = 0; b < GFX_MAX_BANDS; b++) {
        delete_head(head, sizeof head, b);
        n += gfx_apc(g_restore + n, head, NULL, 0);
    }
    // Popping the keyboard protocol has to reach the terminal itself, so it
    // needs wrapping too; the cursor and alt screen are tmux's own business.
    n += gfx_wrap(g_restore + n, "\x1b[<u", 4);
    // Mouse reporting is tmux's own mode, so it is turned off unwrapped along
    // with the cursor and the alt screen. Disabling a mode that was never on
    // costs nothing, which is why this needs no state to consult.
    const char *tail = "\x1b[?2026l\x1b[?1016l\x1b[?1006l\x1b[?1002l"
                       "\x1b[?25h\x1b[?1049l";
    size_t tl = strlen(tail);
    memcpy(g_restore + n, tail, tl);
    g_restore_len = n + tl;
}

void gfx_init(void) {
    const char *t = getenv("TMUX");
    g_tmux = t && *t;
    g_tmux_redraw = g_tmux && tmux_version() >= 307;
    build_restore();
}

// Gives up on the cells currently carrying our id and starts using another.
//
// Placeholder cells are ordinary text, so they stay where they were drawn and
// go on naming the id they were drawn with. Erasing them at the rectangle we
// recorded does not work: a pane resize makes tmux reflow its contents, so
// those rows no longer say where the cells ended up - it would clear whatever
// moved into them and leave the real cells behind, still naming a live image
// and so drawing a second copy of the picture.
//
// So the images are deleted and the id moved on instead. Whatever cells were
// left behind now name nothing, and a placeholder with no image draws blank.
void gfx_retire_image(int fd) {
    gfx_delete_image(fd);
    g_img_gen++;
    g_img_id = 0;                    // recomputed, from the new generation
    build_restore();
}

bool gfx_tmux(void) { return g_tmux; }

void gfx_set_sync(bool ok) { g_sync = ok; }
bool gfx_sync(void) { return g_sync; }

const char *gfx_restore_seq(size_t *len) {
    *len = g_restore_len;
    return g_restore;
}

// Passthrough is opt-in per server, and without it every graphics escape is
// silently eaten - which looks exactly like a terminal that cannot draw.
bool gfx_passthrough_ok(void) {
    if (!g_tmux) return true;
    FILE *p = popen("tmux show -gv allow-passthrough 2>/dev/null", "r");
    if (!p) return false;
    char buf[32] = "";
    bool got = fgets(buf, sizeof buf, p) != NULL;
    pclose(p);
    if (!got) return false;
    return strncmp(buf, "on", 2) == 0 || strncmp(buf, "all", 3) == 0;
}

// tmux only asks the terminal for mouse events while its own mouse mode is on,
// so without it a pane can enable reporting and never hear anything back.
bool gfx_tmux_mouse_ok(void) {
    if (!g_tmux) return true;
    FILE *p = popen("tmux show -gv mouse 2>/dev/null", "r");
    if (!p) return false;
    char buf[32] = "";
    bool got = fgets(buf, sizeof buf, p) != NULL;
    pclose(p);
    return got && strncmp(buf, "on", 2) == 0;
}

void gfx_delete_image(int fd) {
    char head[48], buf[64];
    for (int b = 0; b < GFX_MAX_BANDS; b++) {
        delete_head(head, sizeof head, b);
        size_t n = gfx_apc(buf, head, NULL, 0);
        (void)writeall(fd, buf, n);
    }
}

static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// One cell per grid position: the placeholder codepoint, then a diacritic for
// its row and one for its column. The image id is in the foreground colour.
void gfx_write_placeholders(int fd, const Layout *l) {
    static char  *buf;
    static size_t cap;

    int cols = l->cols, rows = l->rows;
    if (cols < 1 || rows < 1) return;
    if (cols > ROWCOLUMN_DIACRITICS_COUNT) cols = ROWCOLUMN_DIACRITICS_COUNT;
    if (rows > ROWCOLUMN_DIACRITICS_COUNT) rows = ROWCOLUMN_DIACRITICS_COUNT;

    // 12 bytes a cell worst case, a cursor move a row, a colour set a band,
    // and the synchronized-update pair below.
    size_t need = (size_t)rows * ((size_t)cols * 12 + 32) + GFX_MAX_BANDS * 32 + 64;
    if (need > cap) {
        char *nb = realloc(buf, need);
        if (!nb) return;
        buf = nb;
        cap = need;
    }

    char cell[4], drow[4], dcol[4];
    size_t celln = utf8_encode(PLACEHOLDER_CP, cell);

    // tmux 3.7 drops a cell's combining diacritics on the way to the terminal
    // whenever the pane is not at column 0: screen_write_combine() tests
    // visibility with a pane-relative x against window coordinates, decides the
    // cell is covered by whatever pane really sits there, and skips the write.
    // The cells still land in tmux's grid intact, so ending a synchronized
    // update - which tmux consumes itself, and answers with a full pane redraw
    // out of that grid - puts them on screen correctly. Unwrapped on purpose:
    // this one is addressed to tmux, not to the terminal underneath it.
    size_t o = 0;
    if (g_tmux_redraw) { memcpy(buf, "\x1b[?2026h", 8); o = 8; }

    // Row diacritics count from the top of the band, not of the picture.
    int nb = l->bands > 1 ? l->bands : 1;
    for (int b = 0; b < nb; b++) {
        int cy0, cy1;
        layout_band_cells(l, b, &cy0, &cy1);
        if (cy1 > rows) cy1 = rows;
        int id = gfx_image_id() + b;
        o += (size_t)sprintf(buf + o, "\x1b[38;2;%u;%u;%um",
                             (id >> 16) & 0xFF, (id >> 8) & 0xFF, id & 0xFF);
        for (int y = cy0; y < cy1; y++) {
            size_t rn = utf8_encode(rowcolumn_diacritics[y - cy0], drow);
            o += (size_t)sprintf(buf + o, "\x1b[%d;%dH", l->y + y, l->x);
            for (int x = 0; x < cols; x++) {
                size_t cn = utf8_encode(rowcolumn_diacritics[x], dcol);
                memcpy(buf + o, cell, celln); o += celln;
                memcpy(buf + o, drow, rn);    o += rn;
                memcpy(buf + o, dcol, cn);    o += cn;
            }
        }
    }
    o += (size_t)sprintf(buf + o, "\x1b[39m");
    if (g_tmux_redraw) { memcpy(buf + o, "\x1b[?2026l", 8); o += 8; }
    (void)writeall(fd, buf, o);
}
