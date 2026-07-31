#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "rom.h"

// Tango, used when the terminal will not tell us its palette.
static const uint8_t fallback_pal[16][3] = {
    { 0x00, 0x00, 0x00 }, { 0xcc, 0x00, 0x00 }, { 0x4e, 0x9a, 0x06 },
    { 0xc4, 0xa0, 0x00 }, { 0x34, 0x65, 0xa4 }, { 0x75, 0x50, 0x7b },
    { 0x06, 0x98, 0x9a }, { 0xd3, 0xd7, 0xcf }, { 0x55, 0x57, 0x53 },
    { 0xef, 0x29, 0x29 }, { 0x8a, 0xe2, 0x34 }, { 0xfc, 0xe9, 0x4f },
    { 0x72, 0x9f, 0xcf }, { 0xad, 0x7f, 0xa8 }, { 0x34, 0xe2, 0xe2 },
    { 0xee, 0xee, 0xec },
};

void theme_fallback(Theme *t) {
    memset(t, 0, sizeof *t);
    memcpy(t->pal, fallback_pal, sizeof fallback_pal);
    t->fg[0] = 0xd3; t->fg[1] = 0xd7; t->fg[2] = 0xcf;
    t->bg[0] = 0x00; t->bg[1] = 0x00; t->bg[2] = 0x00;
    t->from_terminal = false;
}

// Reads 1-4 hex digits and scales to 0-255, so rgb:f/f/f and rgb:ffff/../..
// both work. Returns digits consumed.
static int hex_component(const char *s, uint8_t *out) {
    unsigned v = 0;
    int n = 0;
    while (n < 4) {
        char c = s[n];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else break;
        v = v * 16 + d;
        n++;
    }
    if (n == 0) return 0;
    unsigned max = (1u << (4 * n)) - 1u;
    *out = (uint8_t)((v * 255 + max / 2) / max);
    return n;
}

static bool parse_rgb(const char *s, uint8_t out[3]) {
    if (strncmp(s, "rgb:", 4) != 0) return false;
    s += 4;
    for (int i = 0; i < 3; i++) {
        int n = hex_component(s, &out[i]);
        if (n == 0) return false;
        s += n;
        if (i < 2) {
            if (*s != '/') return false;
            s++;
        }
    }
    return true;
}

int theme_query(Theme *t, int ttyfd) {
    theme_fallback(t);

    // Ask for everything in one write; terminals answer in order and this
    // avoids paying a timeout per colour.
    char q[1024];
    size_t o = 0;
    for (int i = 0; i < 16; i++)
        o += (size_t)snprintf(q + o, sizeof q - o, "\x1b]4;%d;?\x1b\\", i);
    o += (size_t)snprintf(q + o, sizeof q - o, "\x1b]10;?\x1b\\");
    o += (size_t)snprintf(q + o, sizeof q - o, "\x1b]11;?\x1b\\");

    // tmux answers these itself, out of its own idea of the palette rather than
    // the terminal's, which is how a themed background gets reported as black.
    // Wrapped, the query is opaque to tmux and only the terminal replies.
    char wrapped[2 * sizeof q + 16];
    size_t n = gfx_wrap(wrapped, q, o);
    if (write(ttyfd, wrapped, n) < 0) return -1;

    char buf[8192];
    size_t got = 0;
    int seen = 0;
    double deadline = now_sec() + 0.4;
    struct pollfd p = { .fd = ttyfd, .events = POLLIN };

    while (got < sizeof buf - 1 && seen < 18) {
        double left = deadline - now_sec();
        if (left <= 0) break;
        if (poll(&p, 1, (int)(left * 1000)) <= 0) break;
        ssize_t n = read(ttyfd, buf + got, sizeof buf - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        buf[got] = 0;
        // Count complete replies so we can stop as soon as all have arrived.
        seen = 0;
        for (const char *c = buf; (c = strstr(c, "rgb:")) != NULL; c++) seen++;
    }
    buf[got] = 0;
    if (got == 0) return -1;

    int found = 0;
    for (const char *c = buf; (c = strchr(c, 0x1b)) != NULL; c++) {
        if (c[1] != ']') continue;
        const char *body = c + 2;
        if (!strncmp(body, "4;", 2)) {
            int idx = 0;
            const char *d = body + 2;
            if (*d < '0' || *d > '9') continue;
            while (*d >= '0' && *d <= '9') idx = idx * 10 + (*d++ - '0');
            if (*d != ';') continue;
            if (idx >= 0 && idx < 16 && parse_rgb(d + 1, t->pal[idx])) found++;
        } else if (!strncmp(body, "10;", 3)) {
            if (parse_rgb(body + 3, t->fg)) found++;
        } else if (!strncmp(body, "11;", 3)) {
            if (parse_rgb(body + 3, t->bg)) found++;
        }
    }

    if (found == 0) return -1;
    t->from_terminal = true;
    return 0;
}
