#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu.h"

struct named { const char *name; uint32_t key; };

static const struct named keynames[] = {
    { "Up", KEY_UP }, { "Down", KEY_DOWN }, { "Left", KEY_LEFT }, { "Right", KEY_RIGHT },
    { "Home", KEY_HOME }, { "End", KEY_END }, { "Insert", KEY_INSERT },
    { "Delete", KEY_DELETE }, { "PageUp", KEY_PGUP }, { "PageDown", KEY_PGDN },
    { "F1", KEY_F1 }, { "F2", KEY_F2 }, { "F3", KEY_F3 }, { "F4", KEY_F4 },
    { "F5", KEY_F5 }, { "F6", KEY_F6 }, { "F7", KEY_F7 }, { "F8", KEY_F8 },
    { "F9", KEY_F9 }, { "F10", KEY_F10 }, { "F11", KEY_F11 }, { "F12", KEY_F12 },
    { "Escape", KEY_ESC }, { "Tab", KEY_TAB }, { "Enter", KEY_ENTER },
    { "Backspace", KEY_BACKSPACE }, { "Space", KEY_SPACE },
    { "LShift", KEY_LSHIFT }, { "RShift", KEY_RSHIFT },
    { "LCtrl", KEY_LCTRL }, { "RCtrl", KEY_RCTRL },
    { "LAlt", KEY_LALT }, { "RAlt", KEY_RALT },
};

static const char *padnames[16] = {
    "b", "y", "select", "start", "up", "down", "left", "right",
    "a", "x", "l", "r", "l2", "r2", "l3", "r3"
};

static const char *hknames[HK_COUNT] = {
    "quit", "pause", "reset", "save_state", "load_state",
    "slot_next", "slot_prev", "fast_forward", "mute", "stats", "recolor",
    "volume_up", "volume_down"
};

const char *hotkey_name(int hk) {
    return (hk >= 0 && hk < HK_COUNT) ? hknames[hk] : "?";
}

const char *key_name(uint32_t key) {
    static char buf[40];
    uint32_t base = key & KEY_BASE_MASK;
    const char *bn = NULL;
    char one[2];

    for (size_t i = 0; i < sizeof keynames / sizeof keynames[0]; i++)
        if (keynames[i].key == base) { bn = keynames[i].name; break; }
    if (!bn && base >= 33 && base < 127) {
        one[0] = (char)base;
        one[1] = 0;
        bn = one;
    }
    if (!bn) return "?";

    snprintf(buf, sizeof buf, "%s%s%s",
             (key & MOD_CTRL) ? "Ctrl+" : "",
             (key & MOD_ALT) ? "Alt+" : "", bn);
    return buf;
}

uint32_t key_from_name(const char *name) {
    if (!name || !*name) return KEY_NONE;
    uint32_t mods = 0;

    // Strip any number of Ctrl+/Alt+ prefixes ('-' accepted as the separator).
    for (;;) {
        if (!strncasecmp(name, "ctrl", 4) && (name[4] == '+' || name[4] == '-')) {
            mods |= MOD_CTRL;
            name += 5;
        } else if (!strncasecmp(name, "alt", 3) && (name[3] == '+' || name[3] == '-')) {
            mods |= MOD_ALT;
            name += 4;
        } else {
            break;
        }
    }
    if (!*name) return KEY_NONE;

    for (size_t i = 0; i < sizeof keynames / sizeof keynames[0]; i++)
        if (strcasecmp(keynames[i].name, name) == 0) return keynames[i].key | mods;
    if (name[1] == 0)
        return (uint32_t)(unsigned char)tolower((unsigned char)name[0]) | mods;
    return KEY_NONE;
}

void config_defaults(Config *c) {
    memset(c, 0, sizeof *c);
    c->pad[RETRO_DEVICE_ID_JOYPAD_UP]     = KEY_UP;
    c->pad[RETRO_DEVICE_ID_JOYPAD_DOWN]   = KEY_DOWN;
    c->pad[RETRO_DEVICE_ID_JOYPAD_LEFT]   = KEY_LEFT;
    c->pad[RETRO_DEVICE_ID_JOYPAD_RIGHT]  = KEY_RIGHT;
    c->pad[RETRO_DEVICE_ID_JOYPAD_B]      = 'z';
    c->pad[RETRO_DEVICE_ID_JOYPAD_A]      = 'x';
    c->pad[RETRO_DEVICE_ID_JOYPAD_Y]      = 'a';
    c->pad[RETRO_DEVICE_ID_JOYPAD_X]      = 's';
    c->pad[RETRO_DEVICE_ID_JOYPAD_L]      = 'q';
    c->pad[RETRO_DEVICE_ID_JOYPAD_R]      = 'w';
    c->pad[RETRO_DEVICE_ID_JOYPAD_START]  = KEY_ENTER;
    c->pad[RETRO_DEVICE_ID_JOYPAD_SELECT] = KEY_RSHIFT;

    // Quit deliberately takes a modifier: Escape is far too easy to hit by
    // reflex mid-game.
    c->hotkey[HK_QUIT][0]         = MOD_CTRL | 'c';
    c->hotkey[HK_QUIT][1]         = MOD_CTRL | 'q';
    c->hotkey[HK_PAUSE][0]        = 'p';
    c->hotkey[HK_RESET][0]        = KEY_F5;
    c->hotkey[HK_SAVE_STATE][0]   = KEY_F2;
    c->hotkey[HK_LOAD_STATE][0]   = KEY_F3;
    c->hotkey[HK_SLOT_PREV][0]    = KEY_F6;
    c->hotkey[HK_SLOT_NEXT][0]    = KEY_F7;
    c->hotkey[HK_FAST_FORWARD][0] = KEY_TAB;
    c->hotkey[HK_MUTE][0]         = 'm';
    c->hotkey[HK_STATS][0]        = KEY_F1;
    c->hotkey[HK_RECOLOR][0]      = KEY_F8;
    // The kitty protocol reports the unshifted key, so shift+'=' arrives as
    // '='. Bind both so the shifted and unshifted spellings each work.
    c->hotkey[HK_VOL_UP][0]       = '=';
    c->hotkey[HK_VOL_UP][1]       = '+';
    c->hotkey[HK_VOL_DOWN][0]     = '-';
    c->hotkey[HK_VOL_DOWN][1]     = '_';

    c->volume = 100;
    c->integer_scale = false;
    c->show_stats = false;      // F1 toggles it on when you want the numbers
    c->pause_on_unfocus = true;
    c->recolor = RECOLOR_OFF;
    c->recolor_strength = 1.0;
    c->scale = 1;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static bool parse_bool(const char *v) {
    return strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 ||
           strcasecmp(v, "1") == 0 || strcasecmp(v, "on") == 0;
}

// One pass over the file. `want_suffixed` selects which half of the config is
// applied: the plain sections, or only those tagged for `system`. Running the
// plain pass first means a [options.gb] block overrides [options] no matter
// which order they appear in the file.
static int config_pass(Config *c, const char *path, const char *system,
                       bool want_suffixed) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512], section[64] = "";
    bool skip = true;
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) continue;
            *end = 0;
            snprintf(section, sizeof section, "%s", s + 1);
            char *dot = strchr(section, '.');
            bool suffixed = dot != NULL;
            skip = (suffixed != want_suffixed);
            if (suffixed) {
                *dot = 0;
                if (!system || strcasecmp(dot + 1, system) != 0) skip = true;
            }
            continue;
        }
        if (skip) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);

        if (strcasecmp(section, "pad") == 0) {
            for (int i = 0; i < 16; i++)
                if (strcasecmp(k, padnames[i]) == 0) {
                    uint32_t key = key_from_name(v);
                    if (key == KEY_NONE)
                        fprintf(stderr, "config:%d: unknown key '%s'\n", lineno, v);
                    else c->pad[i] = key;
                }
        } else if (strcasecmp(section, "hotkeys") == 0) {
            for (int i = 0; i < HK_COUNT; i++) {
                if (strcasecmp(k, hknames[i]) != 0) continue;
                // A hotkey may list alternates: "quit = Ctrl+c, Ctrl+q"
                for (int j = 0; j < MAX_HOTKEY_KEYS; j++) c->hotkey[i][j] = KEY_NONE;
                int j = 0;
                for (char *tok = strtok(v, ","); tok && j < MAX_HOTKEY_KEYS;
                     tok = strtok(NULL, ",")) {
                    char *name = trim(tok);
                    if (!*name) continue;
                    uint32_t key = key_from_name(name);
                    if (key == KEY_NONE)
                        fprintf(stderr, "config:%d: unknown key '%s'\n", lineno, name);
                    else c->hotkey[i][j++] = key;
                }
            }
        } else if (strcasecmp(section, "options") == 0) {
            if (strcasecmp(k, "volume") == 0) c->volume = atoi(v);
            else if (strcasecmp(k, "integer_scale") == 0) c->integer_scale = parse_bool(v);
            else if (strcasecmp(k, "show_stats") == 0) c->show_stats = parse_bool(v);
            else if (strcasecmp(k, "pause_on_unfocus") == 0) c->pause_on_unfocus = parse_bool(v);
            else if (strcasecmp(k, "recolor_strength") == 0) c->recolor_strength = atof(v);
            else if (strcasecmp(k, "scale") == 0) c->scale = atoi(v);
            else if (strcasecmp(k, "recolor") == 0) {
                int m = recolor_mode_from_name(v);
                if (m < 0) fprintf(stderr, "config:%d: unknown recolor mode '%s'\n", lineno, v);
                else c->recolor = m;
            }
        }
    }
    fclose(f);
    return 0;
}

int config_load(Config *c, const char *path, const char *system) {
    if (config_pass(c, path, NULL, false) != 0) return -1;
    if (system && *system) config_pass(c, path, system, true);

    if (c->volume < 0) c->volume = 0;
    if (c->volume > 100) c->volume = 100;
    if (c->scale < 1) c->scale = 1;
    if (c->scale > 8) c->scale = 8;
    if (c->recolor_strength < 0.0) c->recolor_strength = 0.0;
    if (c->recolor_strength > 1.0) c->recolor_strength = 1.0;
    return 0;
}

void config_print(const Config *c, const char *path) {
    // Display order follows the physical pad, not the libretro id order.
    static const struct { int id; const char *label; } order[] = {
        { RETRO_DEVICE_ID_JOYPAD_UP,     "D-pad up" },
        { RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-pad down" },
        { RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-pad left" },
        { RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-pad right" },
        { RETRO_DEVICE_ID_JOYPAD_A,      "A" },
        { RETRO_DEVICE_ID_JOYPAD_B,      "B" },
        { RETRO_DEVICE_ID_JOYPAD_X,      "X" },
        { RETRO_DEVICE_ID_JOYPAD_Y,      "Y" },
        { RETRO_DEVICE_ID_JOYPAD_L,      "L" },
        { RETRO_DEVICE_ID_JOYPAD_R,      "R" },
        { RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
        { RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
    };
    static const char *hkdesc[HK_COUNT] = {
        "Quit", "Pause", "Reset", "Save state", "Load state",
        "Next slot", "Previous slot", "Fast-forward (hold)", "Mute",
        "Toggle status line", "Cycle recolor mode", "Volume up", "Volume down"
    };

    printf("Game\n");
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        uint32_t k = c->pad[order[i].id];
        if (k) printf("  %-22s %s\n", order[i].label, key_name(k));
    }

    printf("\nHotkeys\n");
    for (int i = 0; i < HK_COUNT; i++) {
        char keys[96];
        size_t o = 0;
        keys[0] = 0;
        for (int j = 0; j < MAX_HOTKEY_KEYS; j++) {
            if (!c->hotkey[i][j]) continue;
            o += (size_t)snprintf(keys + o, sizeof keys - o, "%s%s",
                                  o ? " or " : "", key_name(c->hotkey[i][j]));
        }
        printf("  %-22s %s\n", hkdesc[i], keys[0] ? keys : "(unbound)");
    }

    printf("\nOptions\n");
    printf("  %-22s %d\n", "volume", c->volume);
    printf("  %-22s %d\n", "scale", c->scale);
    printf("  %-22s %s\n", "integer_scale", c->integer_scale ? "true" : "false");
    printf("  %-22s %s\n", "show_stats", c->show_stats ? "true" : "false");
    printf("  %-22s %s\n", "pause_on_unfocus", c->pause_on_unfocus ? "true" : "false");
    printf("  %-22s %s\n", "recolor", recolor_mode_name(c->recolor));
    printf("  %-22s %.2f\n", "recolor_strength", c->recolor_strength);

    if (path) printf("\nEdit %s to rebind.\n", path);
}

int config_write_default(const char *path) {
    Config c;
    config_defaults(&c);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# %s configuration\n"
               "# Key names: single characters (a, z, 1), or Up Down Left Right\n"
               "# Home End Insert Delete PageUp PageDown F1-F12 Escape Tab Enter\n"
               "# Backspace Space LShift RShift LCtrl RCtrl LAlt RAlt\n\n", APP_NAME);
    fprintf(f, "[pad]\n");
    for (int i = 0; i < 16; i++)
        if (c.pad[i]) fprintf(f, "%-8s = %s\n", padnames[i], key_name(c.pad[i]));
    fprintf(f, "\n[hotkeys]\n");
    fprintf(f, "# Modifiers: Ctrl+ and Alt+. Comma-separated alternates allowed.\n");
    for (int i = 0; i < HK_COUNT; i++) {
        fprintf(f, "%-13s =", hknames[i]);
        for (int j = 0; j < MAX_HOTKEY_KEYS; j++) {
            if (!c.hotkey[i][j]) continue;
            fprintf(f, "%s %s", j ? "," : "", key_name(c.hotkey[i][j]));
        }
        fputc('\n', f);
    }
    fprintf(f, "\n[options]\n");
    fprintf(f, "volume           = %d\n", c.volume);
    fprintf(f, "integer_scale    = %s\n", c.integer_scale ? "true" : "false");
    fprintf(f, "show_stats       = %s\n", c.show_stats ? "true" : "false");
    fprintf(f, "pause_on_unfocus = %s\n", c.pause_on_unfocus ? "true" : "false");
    fprintf(f, "scale            = %d          # inline-mode integer zoom, 1-8\n", c.scale);
    fprintf(f, "# recolor: off | hue | nearest | duotone | dither\n");
    fprintf(f, "recolor          = %s\n", recolor_mode_name(c.recolor));
    fprintf(f, "recolor_strength = %.2f\n", c.recolor_strength);
    fprintf(f,
        "\n"
        "# Per-platform overrides: suffix any section with a system name.\n"
        "# Systems: snes nes gb gba genesis pce\n"
        "# These are applied on top of the sections above, whatever the order.\n"
        "#\n"
        "# [options.gb]\n"
        "# scale = 4        # Game Boy is only 160x144, so zoom it further\n"
        "#\n"
        "# [pad.gb]\n"
        "# a = x            # the Game Boy has no X/Y/L/R to bind\n");
    fclose(f);
    return 0;
}
