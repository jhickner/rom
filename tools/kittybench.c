// Measures sustained kitty-graphics-protocol frame throughput at SNES resolution.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

#define SNES_W 256
#define SNES_H 224

static const char b64t[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64enc(const unsigned char *in, size_t n, char *out) {
    size_t o = 0, i = 0;
    for (; i + 2 < n; i += 3) {
        unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[o++] = b64t[(v >> 18) & 63]; out[o++] = b64t[(v >> 12) & 63];
        out[o++] = b64t[(v >> 6) & 63];  out[o++] = b64t[v & 63];
    }
    if (i < n) {
        int rem = (int)(n - i);
        unsigned v = in[i] << 16;
        if (rem == 2) v |= in[i + 1] << 8;
        out[o++] = b64t[(v >> 18) & 63]; out[o++] = b64t[(v >> 12) & 63];
        out[o++] = rem == 2 ? b64t[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    return o;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int writeall(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

// Cheap animated pattern so the eye can see whether frames are actually landing.
static void gen_frame(unsigned char *fb, int w, int h, int t) {
    for (int y = 0; y < h; y++) {
        unsigned char *row = fb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            row[x * 3 + 0] = (unsigned char)(x + t);
            row[x * 3 + 1] = (unsigned char)(y - t);
            row[x * 3 + 2] = (unsigned char)((x ^ y) + t * 2);
        }
    }
}

// Assemble one complete kitty frame (all chunks) into buf; returns bytes used.
static size_t build_frame(char *buf, const char *ctrl, const char *b64, size_t blen) {
    size_t o = 0, off = 0;
    int first = 1;
    o += (size_t)sprintf(buf + o, "\x1b[H");
    while (off < blen) {
        size_t chunk = blen - off;
        if (chunk > 4096) chunk = 4096;
        int more = (off + chunk < blen);
        if (first) o += (size_t)sprintf(buf + o, "\x1b_G%s,m=%d;", ctrl, more);
        else       o += (size_t)sprintf(buf + o, "\x1b_Gm=%d;", more);
        memcpy(buf + o, b64 + off, chunk); o += chunk;
        buf[o++] = 0x1b; buf[o++] = '\\';
        off += chunk; first = 0;
    }
    return o;
}

struct result { const char *name; double fps; double mbs; int ok; };

static struct result run_direct(int fd, int w, int h, const char *ctrl,
                                const char *name, int frames) {
    size_t raw = (size_t)w * h * 3;
    unsigned char *fb = malloc(raw);
    char *b64 = malloc(raw * 4 / 3 + 8);
    char *buf = malloc(raw * 4 / 3 + 65536);
    struct result r = { name, 0, 0, 0 };
    double t0 = now();
    size_t bytes = 0;
    for (int f = 0; f < frames; f++) {
        gen_frame(fb, w, h, f);
        size_t blen = b64enc(fb, raw, b64);
        size_t n = build_frame(buf, ctrl, b64, blen);
        if (writeall(fd, buf, n) < 0) goto done;
        bytes += n;
    }
    r.ok = 1;
done: {
        double dt = now() - t0;
        r.fps = frames / dt;
        r.mbs = bytes / dt / 1e6;
    }
    free(fb); free(b64); free(buf);
    return r;
}

// Probe with q=0 so the terminal must reply OK / error; tells us if a
// transfer mode is actually supported before we benchmark it.
static int probe_ok(int fd, const char *ctrl, const char *payload) {
    char msg[512];
    int n = snprintf(msg, sizeof msg, "\x1b_G%s;%s\x1b\\", ctrl, payload);
    if (writeall(fd, msg, (size_t)n) < 0) return 0;
    struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
    char resp[512]; size_t got = 0;
    double deadline = now() + 0.5;
    while (now() < deadline && got < sizeof resp - 1) {
        int ms = (int)((deadline - now()) * 1000);
        if (ms < 0) ms = 0;
        if (poll(&p, 1, ms) <= 0) break;
        ssize_t k = read(STDIN_FILENO, resp + got, sizeof resp - 1 - got);
        if (k <= 0) break;
        got += (size_t)k;
        resp[got] = 0;
        if (strstr(resp, "\x1b\\")) break;
    }
    resp[got] = 0;
    return strstr(resp, ";OK") != NULL;
}

static struct result run_shm(int fd, int w, int h, int frames) {
    struct result r = { "shared memory  256x224", 0, 0, 0 };
    size_t raw = (size_t)w * h * 3;
    unsigned char *fb = malloc(raw);
    double t0 = now(); size_t bytes = 0;
    char name[32], b64name[64], ctrl[128], msg[512];
    for (int f = 0; f < frames; f++) {
        snprintf(name, sizeof name, "/kbench%d", f & 1);
        shm_unlink(name);
        int sfd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
        if (sfd < 0) goto done;
        if (ftruncate(sfd, (off_t)raw) < 0) { close(sfd); goto done; }
        void *m = mmap(NULL, raw, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
        close(sfd);
        if (m == MAP_FAILED) goto done;
        gen_frame(m, w, h, f);
        munmap(m, raw);
        b64enc((const unsigned char *)name, strlen(name), b64name);
        b64name[(strlen(name) + 2) / 3 * 4] = 0;
        snprintf(ctrl, sizeof ctrl,
                 "a=T,t=s,f=24,s=%d,v=%d,i=1,p=1,q=2,C=1", w, h);
        int n = snprintf(msg, sizeof msg, "\x1b[H\x1b_G%s;%s\x1b\\", ctrl, b64name);
        if (writeall(fd, msg, (size_t)n) < 0) goto done;
        bytes += (size_t)n;
    }
    r.ok = 1;
done: {
        double dt = now() - t0;
        r.fps = frames / dt;
        r.mbs = bytes / dt / 1e6;
    }
    shm_unlink("/kbench0"); shm_unlink("/kbench1");
    free(fb);
    return r;
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    int fd = STDOUT_FILENO;

    if (getenv("TMUX")) {
        fprintf(stderr,
            "WARNING: running inside tmux. Graphics passthrough is slow and\n"
            "         often broken for animation. Run this in a bare Ghostty\n"
            "         window for meaningful numbers.\n\n");
    }

    struct winsize ws = {0};
    ioctl(fd, TIOCGWINSZ, &ws);
    int cw = ws.ws_xpixel && ws.ws_col ? ws.ws_xpixel / ws.ws_col : 0;
    int ch = ws.ws_ypixel && ws.ws_row ? ws.ws_ypixel / ws.ws_row : 0;
    fprintf(stderr, "terminal: %dx%d cells, %dx%d px, cell=%dx%d px\n",
            ws.ws_col, ws.ws_row, ws.ws_xpixel, ws.ws_ypixel, cw, ch);

    // Cells needed to show 256x224 scaled up to roughly fill the window.
    int cols = ws.ws_col > 4 ? ws.ws_col - 2 : 40;
    int rows = ws.ws_row > 4 ? ws.ws_row - 3 : 20;

    struct termios old, raw_t;
    int have_tty = tcgetattr(STDIN_FILENO, &old) == 0;
    if (have_tty) {
        raw_t = old;
        cfmakeraw(&raw_t);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_t);
    }

    // Does this terminal speak the protocol at all?
    int supported = probe_ok(fd, "a=q,i=31,s=1,v=1,f=24", "AAAA");
    fprintf(stderr, "kitty graphics protocol: %s\n\n",
            supported ? "supported" : "NO RESPONSE (unsupported or swallowed)");

    // Native resolutions of the platforms a multi-system frontend would carry.
    // Everything is GPU-scaled to the same on-screen cell rect, so the only
    // variable is transmitted pixel count.
    struct plat { int w, h; const char *sys; } plats[] = {
        { 160, 144, "Game Boy / GBC" },
        { 240, 160, "Game Boy Advance" },
        { 256, 224, "SNES / NES" },
        { 320, 224, "Genesis" },
        { 320, 240, "PSX (typical)" },
        { 640, 480, "PSX hi-res / Saturn" },
        { 0,   0,   "your window, 1:1 px" },
    };
    int nplat = (int)(sizeof plats / sizeof plats[0]);
    if (ws.ws_xpixel && ws.ws_ypixel) {
        plats[nplat - 1].w = ws.ws_xpixel;
        plats[nplat - 1].h = ws.ws_ypixel;
    } else {
        nplat--;  // no pixel geometry reported; skip that row
    }

    struct result res[16]; int nres = 0;
    static char names[16][64];
    char ctrl[160];

    fprintf(stderr, "\x1b[2J");

    for (int i = 0; i < nplat && nres < 15; i++) {
        snprintf(names[nres], sizeof names[0], "%-20s %4dx%-4d",
                 plats[i].sys, plats[i].w, plats[i].h);
        snprintf(ctrl, sizeof ctrl, "a=T,f=24,s=%d,v=%d,i=1,p=1,q=2,C=1,c=%d,r=%d",
                 plats[i].w, plats[i].h, cols, rows);
        res[nres] = run_direct(fd, plats[i].w, plats[i].h, ctrl, names[nres], frames);
        nres++;
    }

    int shm_supported = probe_ok(fd, "a=q,t=s,i=32,s=1,v=1,f=24", "L2ticHJvYmU=");
    if (shm_supported) {
        snprintf(names[nres], sizeof names[0], "%-20s %4dx%-4d", "shm transfer", SNES_W, SNES_H);
        res[nres] = run_shm(fd, SNES_W, SNES_H, frames);
        res[nres].name = names[nres];
        nres++;
    }

    // Clean up placements.
    dprintf(fd, "\x1b_Ga=d,d=I,i=1\x1b\\\x1b_Ga=d,d=I,i=31\x1b\\\x1b_Ga=d,d=I,i=32\x1b\\");
    dprintf(fd, "\x1b[2J\x1b[H");

    if (have_tty) tcsetattr(STDIN_FILENO, TCSANOW, &old);

    fprintf(stderr, "\n%-32s %9s %10s   %s\n", "platform / transmitted size",
            "fps", "MB/s", "vs 60fps");
    fprintf(stderr, "%-32s %9s %10s   %s\n",
            "--------------------------------", "--------", "---------", "--------");
    for (int i = 0; i < nres; i++)
        fprintf(stderr, "%-32s %9.1f %10.1f   %.1fx%s\n",
                res[i].name, res[i].fps, res[i].mbs, res[i].fps / 60.0,
                res[i].fps < 60 ? "  <-- BELOW 60" : "");
    if (!shm_supported)
        fprintf(stderr, "\n(shared-memory transfer not supported by this terminal)\n");
    fprintf(stderr, "\n%d frames per test.\n", frames);
    return 0;
}
