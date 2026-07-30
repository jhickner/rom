#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rom.h"
#include "rowcolumn_diacritics.h"

// The codepoint kitty reserves for image placeholder cells.
#define PLACEHOLDER_CP 0x10EEEEu

// Control data for dropping our image. The id has to reach the terminal in
// decimal, so it is formatted rather than stringified from the macro.
static void delete_head(char *out, size_t cap) {
    snprintf(out, cap, "a=d,d=I,i=%d,q=2", GFX_IMAGE_ID);
}

static bool   g_tmux;
static char   g_restore[96];
static size_t g_restore_len;

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

void gfx_init(void) {
    const char *t = getenv("TMUX");
    g_tmux = t && *t;

    char head[48], del[64];
    delete_head(head, sizeof head);
    size_t n = gfx_apc(del, head, NULL, 0);
    memcpy(g_restore, del, n);
    // Leave the keyboard protocol, show the cursor, drop the alt screen.
    const char *tail = "\x1b[<u\x1b[?25h\x1b[?1049l";
    size_t tl = strlen(tail);
    memcpy(g_restore + n, tail, tl);
    g_restore_len = n + tl;
}

bool gfx_tmux(void) { return g_tmux; }

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

void gfx_delete_image(int fd) {
    char head[48], buf[64];
    delete_head(head, sizeof head);
    size_t n = gfx_apc(buf, head, NULL, 0);
    (void)writeall(fd, buf, n);
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

    // 12 bytes a cell worst case, plus a cursor move and colour reset a row.
    size_t need = (size_t)rows * ((size_t)cols * 12 + 32) + 32;
    if (need > cap) {
        char *nb = realloc(buf, need);
        if (!nb) return;
        buf = nb;
        cap = need;
    }

    char cell[4], drow[4], dcol[4];
    size_t celln = utf8_encode(PLACEHOLDER_CP, cell);

    size_t o = 0;
    o += (size_t)sprintf(buf + o, "\x1b[38;2;%u;%u;%um",
                         (GFX_IMAGE_ID >> 16) & 0xFF,
                         (GFX_IMAGE_ID >> 8) & 0xFF, GFX_IMAGE_ID & 0xFF);
    for (int y = 0; y < rows; y++) {
        size_t rn = utf8_encode(rowcolumn_diacritics[y], drow);
        o += (size_t)sprintf(buf + o, "\x1b[%d;%dH", l->y + y, l->x);
        for (int x = 0; x < cols; x++) {
            size_t cn = utf8_encode(rowcolumn_diacritics[x], dcol);
            memcpy(buf + o, cell, celln); o += celln;
            memcpy(buf + o, drow, rn);    o += rn;
            memcpy(buf + o, dcol, cn);    o += cn;
        }
    }
    o += (size_t)sprintf(buf + o, "\x1b[39m");
    (void)writeall(fd, buf, o);
}
