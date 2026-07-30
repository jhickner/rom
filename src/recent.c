#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include "rom.h"

// One "<unix time>\t<absolute path>" line per game, newest first.
static void list_path(char *out, size_t cap) {
    snprintf(out, cap, "%s/recent", config_dir());
}

static int load_entries(RecentEntry *out, int cap, bool require_file) {
    char path[512];
    list_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    int n = 0;
    char line[RECENT_PATH_MAX + 64];
    while (n < cap && fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *p = tab + 1;
        size_t len = strlen(p);
        while (len && (p[len - 1] == '\n' || p[len - 1] == '\r')) p[--len] = 0;
        if (!len || len >= RECENT_PATH_MAX) continue;

        struct stat st;
        if (require_file && stat(p, &st) != 0) continue;

        bool dup = false;
        for (int i = 0; i < n && !dup; i++)
            if (strcmp(out[i].path, p) == 0) dup = true;
        if (dup) continue;

        RecentEntry *e = &out[n++];
        snprintf(e->path, sizeof e->path, "%s", p);
        rom_base_name(p, e->base, sizeof e->base);
        e->played = (time_t)strtoll(line, NULL, 10);
    }
    fclose(f);
    return n;
}

int recent_load(RecentEntry *out, int cap) {
    return load_entries(out, cap, true);
}

void recent_record(const char *rom_path) {
    char resolved[PATH_MAX];
    const char *abs = realpath(rom_path, resolved) ? resolved : rom_path;
    if (strlen(abs) >= RECENT_PATH_MAX) return;

    // Entries whose file is currently unreachable are kept: an unmounted drive
    // should not silently empty the list.
    RecentEntry *old = malloc(sizeof *old * MAX_RECENT);
    if (!old) return;
    int n = load_entries(old, MAX_RECENT, false);

    size_t cap = (size_t)MAX_RECENT * (RECENT_PATH_MAX + 32);
    char *buf = malloc(cap);
    if (!buf) { free(old); return; }

    size_t len = (size_t)snprintf(buf, cap, "%lld\t%s\n",
                                  (long long)time(NULL), abs);
    int written = 1;
    for (int i = 0; i < n && written < MAX_RECENT; i++) {
        if (strcmp(old[i].path, abs) == 0) continue;
        len += (size_t)snprintf(buf + len, cap - len, "%lld\t%s\n",
                                (long long)old[i].played, old[i].path);
        written++;
    }

    char path[512];
    list_path(path, sizeof path);
    ensure_dir(config_dir());
    write_file_atomic(path, buf, len);
    free(buf);
    free(old);
}

// ------------------------------------------------------------------- picker

static void ago(char *out, size_t cap, time_t then, time_t now) {
    long d = (long)(now - then);
    if (d < 0) d = 0;
    if (d < 60)          snprintf(out, cap, "just now");
    else if (d < 3600)   snprintf(out, cap, "%ldm ago", d / 60);
    else if (d < 86400)  snprintf(out, cap, "%ldh ago", d / 3600);
    else if (d < 172800) snprintf(out, cap, "yesterday");
    else if (d < 7 * 86400)  snprintf(out, cap, "%ldd ago", d / 86400);
    else if (d < 60 * 86400) snprintf(out, cap, "%ldw ago", d / (7 * 86400));
    else {
        struct tm tm;
        localtime_r(&then, &tm);
        strftime(out, cap, "%b %d", &tm);
    }
}

// Copies `s` into `out`, cut to `max` columns with a trailing ellipsis. The cut
// backs up off UTF-8 continuation bytes so a multi-byte name is never split.
static void fit(char *out, size_t cap, const char *s, int max) {
    if ((int)strlen(s) <= max) {
        snprintf(out, cap, "%s", s);
        return;
    }
    int keep = max > 1 ? max - 1 : 1;
    while (keep > 0 && ((unsigned char)s[keep] & 0xC0) == 0x80) keep--;
    snprintf(out, cap, "%.*s\xe2\x80\xa6", keep, s);
}

static void slot_summary(char *out, size_t cap, const char *rom_path) {
    unsigned mask = state_slot_mask(rom_path);
    if (!mask) { *out = 0; return; }
    size_t o = (size_t)snprintf(out, cap, "state ");
    int shown = 0;
    for (int s = 0; s < MAX_STATE_SLOTS; s++) {
        if (!(mask & (1u << s))) continue;
        if (shown == 3) { snprintf(out + o, cap - o, "+"); break; }
        o += (size_t)snprintf(out + o, cap - o, "%s%d", shown ? "," : "", s);
        shown++;
    }
}

static void draw_row(const RecentEntry *e, int num, bool selected,
                     const char *slots, int namew, int linew, time_t now) {
    char name[400], when[24], line[600], shown[700];
    fit(name, sizeof name, e->base, namew);
    ago(when, sizeof when, e->played, now);

    snprintf(line, sizeof line, "%2d  %-*s  %-7s  %-10s  %s",
             num + 1, namew, name, rom_system_slug(e->path), when, slots);
    fit(shown, sizeof shown, line, linew);
    if (selected) printf("\x1b[7m \xe2\x96\xb8 %-*s\x1b[0m", linew, shown);
    else          printf("   %-*s", linew, shown);
}

// Returns a KEY_* code, a raw byte, or 0 at end of input.
static uint32_t read_key(void) {
    unsigned char b;
    if (read(STDIN_FILENO, &b, 1) != 1) return 0;
    if (b != 0x1b) return b;

    // A bare Escape has nothing behind it; anything else is a sequence whose
    // final byte identifies the key.
    struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
    if (poll(&p, 1, 30) <= 0) return KEY_ESC;
    char s[16];
    ssize_t n = read(STDIN_FILENO, s, sizeof s);
    if (n < 2 || (s[0] != '[' && s[0] != 'O')) return KEY_ESC;
    switch (s[n - 1]) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '~':
        switch (s[1]) {
        case '1': return KEY_HOME;
        case '4': return KEY_END;
        case '5': return KEY_PGUP;
        case '6': return KEY_PGDN;
        }
        return KEY_ESC;
    }
    return KEY_ESC;
}

int recent_pick(char *out, size_t cap) {
    RecentEntry *e = malloc(sizeof *e * MAX_RECENT);
    if (!e) return -1;
    int n = recent_load(e, MAX_RECENT);
    if (n == 0) { free(e); return -1; }

    // Without a keyboard to drive the menu, resuming means the newest game.
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        snprintf(out, cap, "%s", e[0].path);
        free(e);
        return 0;
    }

    char (*slots)[24] = malloc(sizeof *slots * (size_t)n);
    if (!slots) { free(e); return -1; }
    for (int i = 0; i < n; i++)
        slot_summary(slots[i], sizeof slots[i], e[i].path);

    int cols, rows, cw, ch;
    term_size(STDOUT_FILENO, &cols, &rows, &cw, &ch);
    int visible = rows - 2;
    if (visible > n) visible = n;
    if (visible < 1) visible = 1;
    int linew = cols - 4;
    if (linew < 20) linew = 20;
    int namew = linew - 39;
    if (namew < 8) namew = 8;
    if (namew > 48) namew = 48;
    int lines = visible + 1;             // header plus the visible rows

    struct termios saved, raw;
    bool restore = tcgetattr(STDIN_FILENO, &saved) == 0;
    if (restore) {
        raw = saved;
        // ISIG off as well, so Ctrl+C arrives as a byte to cancel on rather
        // than killing us with the cursor hidden and echo off.
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    fputs("\x1b[?25l", stdout);

    time_t now = time(NULL);
    int sel = 0, first = 0, chosen = -1;
    for (bool initial = true;; initial = false) {
        if (!initial && lines > 1) printf("\r\x1b[%dA", lines - 1);
        printf("\r\x1b[2K\x1b[1mResume\x1b[0m  \x1b[2m"
               "\xe2\x86\x91/\xe2\x86\x93 move \xc2\xb7 enter play \xc2\xb7 q cancel");
        if (n > visible) printf("  \xc2\xb7  %d of %d", sel + 1, n);
        fputs("\x1b[0m", stdout);
        for (int i = 0; i < visible; i++) {
            printf("\r\n\x1b[2K");
            int idx = first + i;
            draw_row(&e[idx], idx, idx == sel, slots[idx], namew, linew, now);
        }
        fflush(stdout);

        uint32_t k = read_key();
        if (k == KEY_UP || k == 'k') sel--;
        else if (k == KEY_DOWN || k == 'j') sel++;
        else if (k == KEY_PGUP) sel -= visible;
        else if (k == KEY_PGDN) sel += visible;
        else if (k == KEY_HOME || k == 'g') sel = 0;
        else if (k == KEY_END || k == 'G') sel = n - 1;
        else if (k == '\r' || k == '\n' || k == ' ') { chosen = sel; break; }
        else if (k >= '1' && k <= '9' && (int)k - '1' < n) { chosen = (int)k - '1'; break; }
        else if (k == '0' && n > 9) { chosen = 9; break; }
        else if (k == 0 || k == 'q' || k == KEY_ESC || k == 3 || k == 4) break;

        if (sel < 0) sel = 0;
        if (sel > n - 1) sel = n - 1;
        if (sel < first) first = sel;
        if (sel >= first + visible) first = sel - visible + 1;
    }

    // Wipe the menu and leave the cursor where it started, so the game's own
    // block lands here instead of below a stale list.
    if (lines > 1) printf("\r\x1b[%dA", lines - 1);
    for (int i = 0; i < lines; i++) printf("\r\x1b[2K%s", i + 1 < lines ? "\r\n" : "");
    if (lines > 1) printf("\r\x1b[%dA", lines - 1);
    fputs("\x1b[?25h", stdout);
    fflush(stdout);
    if (restore) tcsetattr(STDIN_FILENO, TCSANOW, &saved);

    if (chosen >= 0) snprintf(out, cap, "%s", e[chosen].path);
    free(slots);
    free(e);
    return chosen >= 0 ? 0 : 1;
}
