#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "rom.h"

static struct termios saved_tio;
static bool have_saved;

// Reads an escape-sequence reply terminated by any of `finals`. Returns bytes
// read, 0 on timeout.
static size_t read_reply(int fd, char *buf, size_t cap, const char *finals,
                         double timeout) {
    size_t got = 0;
    double deadline = now_sec() + timeout;
    struct pollfd p = { .fd = fd, .events = POLLIN };
    while (got < cap - 1) {
        double left = deadline - now_sec();
        if (left <= 0) break;
        if (poll(&p, 1, (int)(left * 1000)) <= 0) break;
        ssize_t n = read(fd, buf + got, cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        buf[got] = 0;
        if (strpbrk(buf, finals)) break;
    }
    buf[got] = 0;
    return got;
}

int term_enter(Term *t) {
    if (tcgetattr(t->fd, &saved_tio) == 0) {
        have_saved = true;
        struct termios raw = saved_tio;
        cfmakeraw(&raw);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(t->fd, TCSANOW, &raw) != 0) return -1;
        t->raw = true;
    }
    if (t->inline_mode) {
        (void)!write(t->fd, "\x1b[?25l", 6);            // hide cursor only
    } else {
        (void)!write(t->fd, "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H", 20);
        t->alt = true;
    }
    return 0;
}

int term_reserve_inline(Term *t, int rows) {
    if (rows < 1) rows = 1;
    // Emit newlines so the terminal scrolls up room for us, then climb back to
    // the top of the block we just made.
    for (int i = 0; i < rows; i++) (void)!write(t->fd, "\r\n", 2);
    char buf[32];
    int n = snprintf(buf, sizeof buf, "\x1b[%dA", rows);
    (void)!write(t->fd, buf, (size_t)n);

    // Wherever that landed is our origin; ask rather than assume, since the
    // block may or may not have caused a scroll.
    int row = 0, col = 0;
    if (write(t->fd, "\x1b[6n", 4) == 4) {
        char rep[64];
        if (read_reply(t->fd, rep, sizeof rep, "R", 0.3) > 0)
            sscanf(rep, "\x1b[%d;%dR", &row, &col);
    }
    t->inline_origin = row > 0 ? row : 1;
    t->inline_rows = rows;
    return t->inline_origin;
}

void term_leave(Term *t) {
    if (t->inline_mode) {
        // Leave the final frame on screen and park the cursor below it.
        char buf[64];
        int n = snprintf(buf, sizeof buf, "\x1b[%d;1H\x1b[?25h\r\n",
                         t->inline_origin + t->inline_rows);
        (void)!write(t->fd, buf, (size_t)n);
    } else if (t->alt) {
        // Drop the image, leave alt screen, restore cursor.
        (void)!write(t->fd, "\x1b_Ga=d,d=I,i=1\x1b\\\x1b[?25h\x1b[?1049l", 30);
        t->alt = false;
    }
    if (t->raw && have_saved) {
        tcsetattr(t->fd, TCSANOW, &saved_tio);
        t->raw = false;
    }
}

int term_size(int fd, int *cols, int *rows, int *cell_w, int *cell_h) {
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    if (ioctl(fd, TIOCGWINSZ, &ws) != 0) memset(&ws, 0, sizeof ws);
    // A pty that reports nothing still needs a usable layout.
    *cols = ws.ws_col ? ws.ws_col : 80;
    *rows = ws.ws_row ? ws.ws_row : 24;

    if (ws.ws_xpixel && ws.ws_ypixel) {
        *cell_w = ws.ws_xpixel / ws.ws_col;
        *cell_h = ws.ws_ypixel / ws.ws_row;
        return 0;
    }

    // Fall back to the CSI 16 t cell-size query, but only ever once: its read
    // window swallows anything else the user types, and this runs on every
    // relayout.
    static int cached_w, cached_h;
    if (!cached_w) {
        cached_w = 8;       // conservative guess; only affects aspect correction
        cached_h = 16;
        if (write(fd, "\x1b[16t", 5) == 5) {
            char buf[64];
            if (read_reply(fd, buf, sizeof buf, "t", 0.2) > 0) {
                int a = 0, h = 0, w = 0;
                if (sscanf(buf, "\x1b[%d;%d;%dt", &a, &h, &w) == 3 && a == 6 && w > 0) {
                    cached_w = w;
                    cached_h = h;
                }
            }
        }
    }
    *cell_w = cached_w;
    *cell_h = cached_h;
    return 0;
}

bool term_probe_graphics(int fd) {
    if (write(fd, "\x1b_Ga=q,i=31,s=1,v=1,f=24;AAAA\x1b\\", 31) < 0) return false;
    char buf[128];
    size_t n = read_reply(fd, buf, sizeof buf, "\\", 0.5);
    if (n == 0) return false;
    return strstr(buf, ";OK") != NULL;
}
