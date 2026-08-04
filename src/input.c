#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "rom.h"

// Progressive enhancement flags: disambiguate (1) | report event types (2) |
// report all keys as escape codes (8). Flag 8 is what makes plain letters
// deliver release events, without which held keys never lift.
#define KITTY_KBD_FLAGS 11

static uint8_t pend[256];
static size_t  pendlen;
static double  esc_since;   // when a lone ESC started waiting for a sequence

static uint32_t map_tilde(int n) {
    switch (n) {
    case 1:  return KEY_HOME;      // the form tmux and screen send
    case 4:  return KEY_END;
    case 2:  return KEY_INSERT;
    case 3:  return KEY_DELETE;
    case 5:  return KEY_PGUP;
    case 6:  return KEY_PGDN;
    case 13: return KEY_F3;
    case 15: return KEY_F5;
    case 17: return KEY_F6;
    case 18: return KEY_F7;
    case 19: return KEY_F8;
    case 20: return KEY_F9;
    case 21: return KEY_F10;
    case 23: return KEY_F11;
    case 24: return KEY_F12;
    }
    return KEY_NONE;
}

static uint32_t map_final(char f) {
    switch (f) {
    case 'I': return KEY_FOCUS_IN;    // DECSET 1004 focus reporting
    case 'O': return KEY_FOCUS_OUT;
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case 'P': return KEY_F1;
    case 'Q': return KEY_F2;
    case 'S': return KEY_F4;
    }
    return KEY_NONE;
}

// kitty reports standalone modifier presses in the private-use area.
static uint32_t map_pua(uint32_t cp) {
    switch (cp) {
    case 57441: return KEY_LSHIFT;
    case 57447: return KEY_RSHIFT;
    case 57442: return KEY_LCTRL;
    case 57448: return KEY_RCTRL;
    case 57443: return KEY_LALT;
    case 57449: return KEY_RALT;
    }
    return cp;
}

// Keyboard-protocol escapes are for the terminal running tmux, not for tmux, so
// they go through the same DCS passthrough the graphics use. tmux does not
// recognise the replies or the key reports that come back and passes them
// through to us untouched, which is what makes this work at all.
static void kbd_seq(int fd, const char *seq) {
    char buf[64];
    size_t n = gfx_wrap(buf, seq, strlen(seq));
    (void)!write(fd, buf, n);
}

static bool probe_kitty_kbd(int fd) {
    kbd_seq(fd, "\x1b[?u");
    struct pollfd p = { .fd = fd, .events = POLLIN };
    char buf[64];
    size_t got = 0;
    double deadline = now_sec() + 0.3;
    while (got < sizeof buf - 1) {
        double left = deadline - now_sec();
        if (left <= 0) break;
        if (poll(&p, 1, (int)(left * 1000)) <= 0) break;
        ssize_t n = read(fd, buf + got, sizeof buf - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        buf[got] = 0;
        if (memchr(buf, 'u', got)) break;
    }
    buf[got] = 0;
    return got >= 3 && buf[0] == 0x1b && buf[1] == '[' && buf[2] == '?';
}

int input_init(Input *in, int ttyfd, bool allow_tmux_kbd) {
    memset(in, 0, sizeof *in);
    in->ttyfd = ttyfd;
    pendlen = 0;
    if (gfx_tmux() && !allow_tmux_kbd) {
        // Asked to leave the terminal's keyboard mode alone under tmux.
        in->kitty_kbd = false;
    } else {
        in->kitty_kbd = probe_kitty_kbd(ttyfd);
        if (in->kitty_kbd) input_kbd_push(in);
    }
    // Focus reporting is far more widely supported than the kitty keyboard
    // protocol, so enable it regardless. Terminals that ignore it simply never
    // send the events.
    (void)!write(ttyfd, "\x1b[?1004h", 8);
    in->focus_events = true;
    return in->kitty_kbd ? 0 : -1;
}

// Enabling the protocol pushes onto the terminal's mode stack, so pushes and
// pops have to stay paired: under tmux the mode belongs to the terminal, which
// every other pane shares.
void input_kbd_push(Input *in) {
    if (!in->kitty_kbd || in->kbd_active) return;
    char buf[32];
    snprintf(buf, sizeof buf, "\x1b[>%du", KITTY_KBD_FLAGS);
    kbd_seq(in->ttyfd, buf);
    in->kbd_active = true;
}

void input_kbd_pop(Input *in) {
    if (!in->kbd_active) return;
    kbd_seq(in->ttyfd, "\x1b[<u");
    in->kbd_active = false;
}

void input_shutdown(Input *in) {
    input_kbd_pop(in);
    if (in->focus_events) (void)!write(in->ttyfd, "\x1b[?1004l", 8);
    in->kitty_kbd = false;
    in->focus_events = false;
}

// While the terminal underneath is speaking the kitty protocol, tmux no longer
// recognises the reports its own bindings are made of, so an Alt chord that used
// to switch windows lands in the game instead. `send-keys -K` puts it back:
// unlike a plain send-keys, which types into a pane, -K hands the key to the
// client and it is looked up in the client's key table exactly as if it had been
// pressed at it. Whatever tmux binds is then tmux's business, blocks and all.
static const struct { uint32_t key; const char *name; } tmux_names[] = {
    { KEY_UP, "Up" }, { KEY_DOWN, "Down" }, { KEY_LEFT, "Left" },
    { KEY_RIGHT, "Right" }, { KEY_HOME, "Home" }, { KEY_END, "End" },
    { KEY_INSERT, "IC" }, { KEY_DELETE, "DC" }, { KEY_PGUP, "PPage" },
    { KEY_PGDN, "NPage" }, { KEY_ESC, "Escape" }, { KEY_TAB, "Tab" },
    { KEY_ENTER, "Enter" }, { KEY_BACKSPACE, "BSpace" }, { KEY_SPACE, "Space" },
    { KEY_F1, "F1" }, { KEY_F2, "F2" }, { KEY_F3, "F3" }, { KEY_F4, "F4" },
    { KEY_F5, "F5" }, { KEY_F6, "F6" }, { KEY_F7, "F7" }, { KEY_F8, "F8" },
    { KEY_F9, "F9" }, { KEY_F10, "F10" }, { KEY_F11, "F11" }, { KEY_F12, "F12" },
};

const char *tmux_key_name(uint32_t key) {
    static char buf[32];
    uint32_t base = key & KEY_BASE_MASK;
    const char *bn = NULL;
    char one[2];

    for (size_t i = 0; i < sizeof tmux_names / sizeof tmux_names[0]; i++)
        if (tmux_names[i].key == base) { bn = tmux_names[i].name; break; }
    if (!bn && base > 32 && base < 127) {
        one[0] = (char)base;
        one[1] = 0;
        bn = one;
    }
    if (!bn) return NULL;   // a modifier on its own, or nothing tmux can name

    snprintf(buf, sizeof buf, "%s%s%s",
             (key & MOD_CTRL) ? "C-" : "", (key & MOD_ALT) ? "M-" : "", bn);
    return buf;
}

void input_send_tmux_key(const char *name) {
    // Fork twice so the grandchild is reparented and never needs reaping; the
    // middle process exits at once, so the wait here does not stall a frame.
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        if (fork() == 0) {
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) {
                dup2(null, 0); dup2(null, 1); dup2(null, 2);
                if (null > 2) close(null);
            }
            execlp("tmux", "tmux", "send-keys", "-K", name, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

// Parses one CSI sequence starting at buf[0]=='\x1b'. Returns bytes consumed,
// 0 if the sequence is incomplete (caller should keep it buffered).
static size_t parse_csi(const uint8_t *buf, size_t len, KeyEvent *ev, bool *got) {
    *got = false;
    if (len < 2) return 0;
    if (buf[1] == 'O') {
        // SS3, which is how F1-F4 arrive from a terminal that is not speaking
        // the kitty protocol - tmux among them - and how the arrows arrive in
        // application cursor mode. F3 is the odd one: 'R' as a CSI final is the
        // cursor-position report, so map_final leaves it alone.
        if (len < 3) return 0;
        uint32_t key = buf[2] == 'R' ? (uint32_t)KEY_F3 : map_final((char)buf[2]);
        if (key != KEY_NONE) {
            *got = true;
            ev->key = key;
            ev->pressed = true;
            ev->repeat = false;
        }
        return 3;
    }
    if (buf[1] != '[') {
        // Bare ESC, or ESC+key with the protocol inactive.
        *got = true;
        ev->key = KEY_ESC;
        ev->pressed = true;
        ev->repeat = false;
        return 1;
    }
    size_t i = 2;
    while (i < len && ((buf[i] >= '0' && buf[i] <= '9') || buf[i] == ';' ||
                       buf[i] == ':' || buf[i] == '?' || buf[i] == '<' ||
                       buf[i] == '>'))
        i++;
    if (i >= len) return 0;                       // still accumulating
    char final = (char)buf[i];
    if (final < 0x40 || final > 0x7e) return i + 1;

    // groups[g][s]: semicolon-separated groups, colon-separated subparams
    int groups[4][3];
    for (int g = 0; g < 4; g++)
        for (int s = 0; s < 3; s++) groups[g][s] = -1;

    int gi = 0, si = 0;
    bool have = false;
    int acc = 0;
    for (size_t k = 2; k < i; k++) {
        uint8_t c = buf[k];
        if (c >= '0' && c <= '9') { acc = acc * 10 + (c - '0'); have = true; }
        else if (c == ':') {
            if (gi < 4 && si < 3 && have) groups[gi][si] = acc;
            si++; acc = 0; have = false;
        } else if (c == ';') {
            if (gi < 4 && si < 3 && have) groups[gi][si] = acc;
            gi++; si = 0; acc = 0; have = false;
        }
        // '?', '<', '>' are private markers; ignore
    }
    if (gi < 4 && si < 3 && have) groups[gi][si] = acc;

    int keycode = groups[0][0] >= 0 ? groups[0][0] : 1;
    int mods    = groups[1][0] >= 0 ? groups[1][0] : 1;
    int event   = groups[1][1] >= 0 ? groups[1][1] : 1;

    uint32_t key = KEY_NONE;
    if (final == 'u')      key = map_pua((uint32_t)keycode);
    else if (final == '~') key = map_tilde(keycode);
    else                   key = map_final(final);

    if (key != KEY_NONE) {
        // kitty encodes modifiers as a bitmask offset by one. Only ctrl and
        // alt are carried; shift and the locks would only surprise game input.
        unsigned bits = (unsigned)(mods > 0 ? mods - 1 : 0);
        if (key != KEY_FOCUS_IN && key != KEY_FOCUS_OUT) {
            if (bits & 4) key |= MOD_CTRL;
            if (bits & 2) key |= MOD_ALT;
        }
        *got = true;
        ev->key = key;
        ev->pressed = (event != 3);
        ev->repeat = (event == 2);
    }
    return i + 1;
}

int input_poll(Input *in, KeyEvent *ev, int max_ev) {
    uint8_t buf[512];
    size_t len = pendlen;
    if (len) memcpy(buf, pend, len);

    struct pollfd p = { .fd = in->ttyfd, .events = POLLIN };
    while (len < sizeof buf) {
        int pr = poll(&p, 1, 0);
        if (pr <= 0) break;
        ssize_t n = read(in->ttyfd, buf + len, sizeof buf - len);
        if (n <= 0) break;
        len += (size_t)n;
    }

    int nev = 0;
    size_t i = 0;
    while (i < len && nev < max_ev) {
        if (buf[i] == 0x1b) {
            bool got = false;
            size_t used = parse_csi(buf + i, len - i, &ev[nev], &got);
            if (used == 0) break;                 // incomplete; keep for next poll
            if (got) nev++;
            i += used;
        } else {
            // Fallback path for terminals without the kitty protocol: a
            // press with no matching release.
            uint32_t k = buf[i];
            // Control bytes carry no modifier info of their own. Tab (9),
            // LF (10) and CR (13) are excluded because they are indistinguishable
            // from Ctrl+I/J/M here and are wanted as themselves.
            if (k >= 1 && k <= 26 && k != 9 && k != 10 && k != 13)
                k = MOD_CTRL | (uint32_t)('a' + k - 1);
            ev[nev].key = k;
            ev[nev].pressed = true;
            ev[nev].repeat = false;
            nev++;
            i++;
        }
    }

    pendlen = len - i;
    if (pendlen > sizeof pend) pendlen = 0;
    else if (pendlen) memmove(pend, buf + i, pendlen);

    // A bare ESC is indistinguishable from the start of a sequence until
    // either more bytes arrive or enough time passes. Without this, Escape
    // never fires on terminals lacking the kitty protocol.
    if (pendlen == 1 && pend[0] == 0x1b) {
        double t = now_sec();
        if (esc_since == 0.0) {
            esc_since = t;
        } else if (t - esc_since > 0.05 && nev < max_ev) {
            ev[nev].key = KEY_ESC;
            ev[nev].pressed = true;
            ev[nev].repeat = false;
            nev++;
            pendlen = 0;
            esc_since = 0.0;
        }
    } else {
        esc_since = 0.0;
    }
    return nev;
}
