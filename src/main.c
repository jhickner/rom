#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include "emu.h"

static Core       core;
static Config     cfg;
static Renderer   rend;
static Input      input;
static AudioCtx  *audio;
static Term       term;
static Theme      theme;
static Recolor    recolor;
static double     g_recolor_strength = 1.0;

static volatile sig_atomic_t g_quit;
static volatile sig_atomic_t g_winch = 1;

static unsigned  g_pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint8_t  *g_rgb;
static size_t    g_rgb_cap;
static uint8_t  *g_scaled;
static size_t    g_scaled_cap;
static int       g_frame_w, g_frame_h;
static bool      g_have_frame;
static bool      g_geom_dirty;

static bool held_ascii[256];
static bool held_spec[KEY_MAX_ - 0xE000];

static bool user_paused, focus_paused, muted, fast_forward, running = true;
static int  slot;
static int  max_send_w, max_send_h;      // cap transmitted size to the window
static int  g_scale = 1;                 // requested integer zoom (inline mode)
static int  g_eff_scale = 1;             // zoom actually in use after fitting
static uint8_t *g_zoom;                  // nearest-neighbour upscale buffer
static size_t   g_zoom_cap;
static FILE *logf;
static FILE *g_err;              // the real stderr, kept across the log redirect
static char  g_sysdir[512];

// ------------------------------------------------------------------ helpers

static void logmsg(const char *fmt, ...) {
    if (!logf) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    fflush(logf);
}

// User-facing message that must reach the terminal even after stdout and
// stderr have been pointed at the log file.
static void errmsg(const char *fmt, ...) {
    FILE *f = g_err ? g_err : stderr;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
}

static bool *held_slot(uint32_t k) {
    if (k < 256) return &held_ascii[k];
    if (k >= 0xE000 && k < KEY_MAX_) return &held_spec[k - 0xE000];
    return NULL;
}

static bool key_held(uint32_t k) {
    bool *p = held_slot(k & KEY_BASE_MASK);
    return p && *p;
}

static bool hotkey_held(int hk) {
    for (int j = 0; j < MAX_HOTKEY_KEYS; j++)
        if (cfg.hotkey[hk][j] && key_held(cfg.hotkey[hk][j])) return true;
    return false;
}

static int hotkey_for(uint32_t key) {
    for (int hk = 0; hk < HK_COUNT; hk++) {
        if (hk == HK_FAST_FORWARD) continue;      // held, not toggled
        for (int j = 0; j < MAX_HOTKEY_KEYS; j++)
            if (cfg.hotkey[hk][j] && cfg.hotkey[hk][j] == key) return hk;
    }
    return -1;
}

static void osd(const char *fmt, ...) {
    char buf[OSD_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    renderer_set_osd(&rend, buf, 1.6);
}

// ------------------------------------------------------------ libretro glue

static void log_cb(enum retro_log_level level, const char *fmt, ...) {
    if (!logf) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(logf, "[core:%d] ", (int)level);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fflush(logf);
}

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        unsigned fmt = *(const enum retro_pixel_format *)data;
        if (fmt != RETRO_PIXEL_FORMAT_0RGB1555 &&
            fmt != RETRO_PIXEL_FORMAT_XRGB8888 &&
            fmt != RETRO_PIXEL_FORMAT_RGB565)
            return false;
        g_pixfmt = fmt;
        logmsg("pixel format: %u", fmt);
        return true;
    }

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = g_sysdir;
        return true;

    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        struct retro_log_callback *cb = data;
        cb->log = log_cb;
        return true;
    }

    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        const struct retro_game_geometry *g = data;
        core.av.geometry = *g;
        g_geom_dirty = true;
        return true;
    }

    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        const struct retro_system_av_info *av = data;
        core.av = *av;
        g_geom_dirty = true;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        v->value = NULL;     // no overrides: cores fall back to their defaults
        return false;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;

    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0;
        return true;

    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
        return true;

    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
        return true;

    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;                                  // duped frame
    if (data == RETRO_HW_FRAME_BUFFER_VALID) return;    // hw render unsupported

    size_t need = (size_t)w * h * 3;
    if (need > g_rgb_cap) {
        uint8_t *nb = realloc(g_rgb, need);
        if (!nb) return;
        g_rgb = nb;
        g_rgb_cap = need;
    }
    if (video_convert(g_rgb, data, w, h, pitch, g_pixfmt, &recolor) == 0) return;
    g_frame_w = (int)w;
    g_frame_h = (int)h;
    g_have_frame = true;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames) {
    if (audio && !fast_forward) audio_write(audio, data, frames);
    return frames;
}

static void audio_sample_cb(int16_t l, int16_t r) {
    int16_t f[2] = { l, r };
    if (audio && !fast_forward) audio_write(audio, f, 1);
}

static void input_poll_cb(void) { /* polled in the main loop */ }

static int16_t input_state_cb(unsigned port, unsigned device,
                              unsigned index, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || index != 0) return 0;
    if (id >= 16) return 0;
    uint32_t key = cfg.pad[id];
    return (key && key_held(key)) ? 1 : 0;
}

// ------------------------------------------------------------------- layout

static void base_size(int *bw, int *bh, double *dar) {
    *bw = (int)core.av.geometry.base_width;
    *bh = (int)core.av.geometry.base_height;
    if (*bw <= 0) *bw = 256;
    if (*bh <= 0) *bh = 224;
    *dar = core.av.geometry.aspect_ratio;
    if (*dar <= 0.0) *dar = (double)*bw / (double)*bh;
}

// Largest integer zoom (capped by --scale) whose image still fits the window.
static int fit_scale(int cols, int rows, int cw, int ch) {
    int bw, bh;
    double dar;
    base_size(&bw, &bh, &dar);
    int s = g_scale;
    while (s > 1 && (bw * s > cols * cw || bh * s > (rows - 1) * ch)) s--;
    return s < 1 ? 1 : s;
}

static int inline_rows_needed(int cols, int rows, int cw, int ch) {
    int bw, bh;
    double dar;
    base_size(&bw, &bh, &dar);
    int s = fit_scale(cols, rows, cw, ch);
    int ih = bh * s;
    if (ih > (rows - 1) * ch) ih = (rows - 1) * ch;
    return (ih + ch - 1) / ch + 1;          // image rows plus the status line
}

static void recompute_layout(void) {
    int cols, rows, cw, ch;
    if (term_size(term.fd, &cols, &rows, &cw, &ch) != 0) return;
    if (cw <= 0) cw = 8;
    if (ch <= 0) ch = 16;

    int base_w, base_h;
    double dar;
    base_size(&base_w, &base_h, &dar);

    Layout l;
    memset(&l, 0, sizeof l);
    l.term_cols = cols;
    l.term_rows = rows;

    if (term.inline_mode) {
        // Stay at native resolution (times an integer zoom) and sit in the
        // normal terminal flow rather than filling the window.
        g_eff_scale = fit_scale(cols, rows, cw, ch);
        int iw = base_w * g_eff_scale, ih = base_h * g_eff_scale;

        int max_w = cols * cw, max_h = (rows - 1) * ch;
        bool shrunk = false;
        if (iw > max_w || ih > max_h) {
            // Native does not even fit at 1x; fall back to a scaled placement.
            double s = (double)max_w / iw;
            double s2 = (double)max_h / ih;
            if (s2 < s) s = s2;
            iw = (int)(iw * s);
            ih = (int)(ih * s);
            shrunk = true;
        }
        l.cols = (iw + cw - 1) / cw;
        l.rows = (ih + ch - 1) / ch;
        l.natural = !shrunk;         // exact pixel size unless we had to shrink
        l.x = 1;
        // A resize can reflow the block off the bottom; keep it on screen.
        l.y = term.inline_origin;
        if (l.y + l.rows > rows) l.y = rows - l.rows;
        if (l.y < 1) l.y = 1;
        l.status_row = l.y + l.rows;
        l.allow_clear = false;       // never wipe the user's scrollback
        max_send_w = iw;
        max_send_h = ih;
    } else {
        int avail_rows = rows - 1;         // bottom row is the status line
        if (avail_rows < 1) avail_rows = 1;
        double aw = (double)cols * cw;
        double ah = (double)avail_rows * ch;

        double tw, th;
        if (cfg.integer_scale) {
            int n = (int)(aw / base_w);
            int m = (int)(ah / base_h);
            int s = n < m ? n : m;
            if (s < 1) s = 1;
            tw = (double)base_w * s;
            th = (double)base_h * s;
        } else {
            tw = aw;
            th = aw / dar;
            if (th > ah) { th = ah; tw = ah * dar; }
        }

        l.cols = (int)(tw / cw);
        l.rows = (int)(th / ch);
        if (l.cols < 1) l.cols = 1;
        if (l.rows < 1) l.rows = 1;
        if (l.cols > cols) l.cols = cols;
        if (l.rows > avail_rows) l.rows = avail_rows;
        l.x = (cols - l.cols) / 2 + 1;
        l.y = (avail_rows - l.rows) / 2 + 1;
        l.natural = false;
        l.status_row = rows;
        l.allow_clear = true;
        g_eff_scale = 1;
        max_send_w = l.cols * cw;
        max_send_h = l.rows * ch;
    }

    renderer_set_layout(&rend, &l);
    logmsg("layout: %s %dx%d cells at (%d,%d) natural=%d scale=%d send<=%dx%d",
           term.inline_mode ? "inline" : "fit", l.cols, l.rows, l.x, l.y,
           (int)l.natural, g_eff_scale, max_send_w, max_send_h);
}

// Hands the frame to the renderer, downscaling first if the core's output is
// larger than the on-screen area.
static void submit_frame(void) {
    if (!g_have_frame) return;
    int w = g_frame_w, h = g_frame_h;
    const uint8_t *src = g_rgb;

    // Integer zoom is done here rather than by the terminal, so pixel art
    // stays sharp instead of being smoothed by the GPU scaler.
    if (g_eff_scale > 1) {
        size_t need = (size_t)w * h * 3 * g_eff_scale * g_eff_scale;
        if (need > g_zoom_cap) {
            uint8_t *nb = realloc(g_zoom, need);
            if (nb) { g_zoom = nb; g_zoom_cap = need; }
        }
        if (g_zoom_cap >= need) {
            video_upscale(g_zoom, g_rgb, w, h, g_eff_scale);
            src = g_zoom;
            w *= g_eff_scale;
            h *= g_eff_scale;
        }
    }

    if (max_send_w > 0 && max_send_h > 0 && (w > max_send_w || h > max_send_h)) {
        double s = (double)max_send_w / w;
        double s2 = (double)max_send_h / h;
        if (s2 < s) s = s2;
        int dw = (int)(w * s), dh = (int)(h * s);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        size_t need = (size_t)dw * dh * 3;
        if (need > g_scaled_cap) {
            uint8_t *nb = realloc(g_scaled, need);
            if (!nb) { renderer_submit(&rend, src, w, h); return; }
            g_scaled = nb;
            g_scaled_cap = need;
        }
        video_downscale(g_scaled, dw, dh, src, w, h);
        renderer_submit(&rend, g_scaled, dw, dh);
    } else {
        renderer_submit(&rend, src, w, h);
    }
}

// ------------------------------------------------------------------ hotkeys

static void do_hotkey(int hk) {
    char err[128];
    switch (hk) {
    case HK_QUIT:
        running = false;
        break;
    case HK_PAUSE:
        user_paused = !user_paused;
        osd(user_paused ? "paused" : "resumed");
        if (audio) audio_flush(audio);
        break;
    case HK_RESET:
        core.reset();
        osd("reset");
        break;
    case HK_SAVE_STATE:
        if (state_save(&core, slot, err, sizeof err) == 0) osd("saved slot %d", slot);
        else osd("save failed: %s", err);
        break;
    case HK_LOAD_STATE:
        if (state_load(&core, slot, err, sizeof err) == 0) {
            osd("loaded slot %d", slot);
            if (audio) audio_flush(audio);
        } else {
            osd("load failed: %s", err);
        }
        break;
    case HK_SLOT_NEXT:
        slot = (slot + 1) % MAX_STATE_SLOTS;
        osd("slot %d%s", slot, state_slot_exists(slot) ? "" : " (empty)");
        break;
    case HK_SLOT_PREV:
        slot = (slot + MAX_STATE_SLOTS - 1) % MAX_STATE_SLOTS;
        osd("slot %d%s", slot, state_slot_exists(slot) ? "" : " (empty)");
        break;
    case HK_MUTE:
        muted = !muted;
        audio_set_volume(audio, muted ? 0 : cfg.volume);
        osd(muted ? "muted" : "unmuted");
        break;
    case HK_VOL_UP:
    case HK_VOL_DOWN: {
        cfg.volume += (hk == HK_VOL_UP) ? 10 : -10;
        if (cfg.volume < 0) cfg.volume = 0;
        if (cfg.volume > 100) cfg.volume = 100;
        muted = false;              // reaching for the volume means you want to hear it
        audio_set_volume(audio, cfg.volume);
        osd("volume %d%%", cfg.volume);
        logmsg("volume -> %d%%", cfg.volume);
        break;
    }
    case HK_RECOLOR: {
        int m = (recolor.mode + 1) % RECOLOR_COUNT;
        if (recolor_build(&recolor, m, &theme, g_recolor_strength) != 0) {
            osd("recolor: out of memory");
        } else {
            osd("recolor: %s%s", recolor_mode_name(m),
                theme.from_terminal ? "" : " (built-in palette)");
            logmsg("recolor mode -> %s", recolor_mode_name(m));
        }
        break;
    }
    case HK_STATS:
        cfg.show_stats = !cfg.show_stats;
        pthread_mutex_lock(&rend.m);
        rend.show_status = cfg.show_stats;
        pthread_mutex_unlock(&rend.m);
        break;
    default:
        break;
    }
}

static void handle_input(void) {
    KeyEvent ev[64];
    int n = input_poll(&input, ev, 64);
    for (int i = 0; i < n; i++) {
        if (getenv("EMU_DEBUG_INPUT"))
            logmsg("key ev: 0x%08x pressed=%d repeat=%d", ev[i].key,
                   (int)ev[i].pressed, (int)ev[i].repeat);
        if (ev[i].key == KEY_FOCUS_OUT || ev[i].key == KEY_FOCUS_IN) {
            bool out = (ev[i].key == KEY_FOCUS_OUT);
            logmsg("focus %s (pause_on_unfocus=%d)", out ? "out" : "in",
                   (int)cfg.pause_on_unfocus);
            if (out) {
                // The release for anything held now will never arrive, so drop
                // every key rather than leaving Link walking into a wall.
                memset(held_ascii, 0, sizeof held_ascii);
                memset(held_spec, 0, sizeof held_spec);
            }
            if (cfg.pause_on_unfocus) {
                focus_paused = out;
                if (audio) audio_flush(audio);
                if (out)
                    renderer_set_osd(&rend, "paused (unfocused)", 1e9);
                else
                    osd(user_paused ? "paused" : "resumed");
            }
            continue;
        }

        // Held state ignores modifiers so game input is unaffected by them.
        bool *slotp = held_slot(ev[i].key & KEY_BASE_MASK);
        if (slotp) *slotp = ev[i].pressed;

        if (ev[i].pressed && !ev[i].repeat) {
            int hk = hotkey_for(ev[i].key);
            if (hk >= 0) do_hotkey(hk);
        }
    }
    fast_forward = hotkey_held(HK_FAST_FORWARD);
}

// ------------------------------------------------------------------ signals

static void on_signal(int sig) {
    (void)sig;
    g_quit = 1;
}

static void on_winch(int sig) {
    (void)sig;
    g_winch = 1;
}

// Restores the terminal without touching anything unsafe, then re-raises so
// the real signal disposition still applies.
static void on_fatal(int sig) {
    const char *restore = "\x1b_Ga=d,d=I,i=1\x1b\\\x1b[<u\x1b[?25h\x1b[?1049l";
    (void)!write(term.fd, restore, strlen(restore));
    struct termios t;
    if (tcgetattr(term.fd, &t) == 0) {
        t.c_lflag |= (ECHO | ICANON | ISIG);
        tcsetattr(term.fd, TCSANOW, &t);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

// ------------------------------------------------------------- core lookup

// Slug used to select per-platform config sections, e.g. [options.gb].
static const char *system_for_ext(const char *rom) {
    const char *dot = rom ? strrchr(rom, '.') : NULL;
    if (!dot) return "";
    dot++;
    if (!strcasecmp(dot, "sfc") || !strcasecmp(dot, "smc") || !strcasecmp(dot, "fig"))
        return "snes";
    if (!strcasecmp(dot, "nes")) return "nes";
    if (!strcasecmp(dot, "gb") || !strcasecmp(dot, "gbc")) return "gb";
    if (!strcasecmp(dot, "gba")) return "gba";
    if (!strcasecmp(dot, "md") || !strcasecmp(dot, "gen") || !strcasecmp(dot, "smd"))
        return "genesis";
    if (!strcasecmp(dot, "pce")) return "pce";
    return "";
}

static const char *core_for_ext(const char *rom) {
    const char *dot = strrchr(rom, '.');
    if (!dot) return NULL;
    dot++;
    if (!strcasecmp(dot, "sfc") || !strcasecmp(dot, "smc") || !strcasecmp(dot, "fig"))
        return "snes9x_libretro.dylib";
    if (!strcasecmp(dot, "nes")) return "fceumm_libretro.dylib";
    if (!strcasecmp(dot, "gb") || !strcasecmp(dot, "gbc"))
        return "gambatte_libretro.dylib";
    if (!strcasecmp(dot, "gba")) return "mgba_libretro.dylib";
    if (!strcasecmp(dot, "md") || !strcasecmp(dot, "gen") || !strcasecmp(dot, "smd"))
        return "genesis_plus_gx_libretro.dylib";
    if (!strcasecmp(dot, "pce")) return "mednafen_pce_fast_libretro.dylib";
    return NULL;
}

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int find_core(char *out, size_t cap, const char *rom, const char *exedir) {
    const char *name = core_for_ext(rom);
    if (!name) return -1;
    const char *dirs[3];
    char cfgdir[512], reldir[512];
    snprintf(cfgdir, sizeof cfgdir, "%s/cores", config_dir());
    snprintf(reldir, sizeof reldir, "%s/../cores", exedir);
    dirs[0] = cfgdir;
    dirs[1] = "cores";
    dirs[2] = reldir;
    for (int i = 0; i < 3; i++) {
        snprintf(out, cap, "%s/%s", dirs[i], name);
        if (file_exists(out)) return 0;
    }
    snprintf(out, cap, "%s", name);
    return -1;
}

static void usage(void) {
    fprintf(stderr,
        "usage: " APP_NAME " [options] <rom>\n"
        "  --core <path>   libretro core to use (default: chosen by ROM extension)\n"
        "  --slot <n>      initial save-state slot (0-%d)\n"
        "  --no-audio      disable audio output\n"
        "  --inline        play inline at native resolution (default)\n"
        "  --fullscreen    take over the screen and zoom to fit\n"
        "  --scale <n>     integer zoom for inline mode (overrides config)\n"
        "  --recolor <m>   remap colours to the terminal theme:\n"
        "                  off | hue | nearest | duotone | dither\n"
        "  --recolor-strength <0..1>   blend against the original (default 1)\n"
        "  --keys          print the current keybinds and exit (pass a ROM for\n"
        "                  that platform's overrides)\n"
        "  --force         run even if the terminal does not ack kitty graphics\n"
        "  --selftest <n>  run <n> frames headlessly and exit (no terminal)\n"
        "  --shot <file>   with --selftest, write the final frame as a BMP\n"
        "  --help          this message\n",
        MAX_STATE_SLOTS - 1);
}

static int write_bmp(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int rowbytes = w * 3;
    int pad = (4 - (rowbytes % 4)) % 4;
    uint32_t datasize = (uint32_t)((rowbytes + pad) * h);
    uint32_t filesize = 54 + datasize;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesize, 4);
    uint32_t off = 54;
    memcpy(hdr + 10, &off, 4);
    uint32_t ihsize = 40;
    memcpy(hdr + 14, &ihsize, 4);
    int32_t iw = w, ih = h;
    memcpy(hdr + 18, &iw, 4);
    memcpy(hdr + 22, &ih, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &datasize, 4);
    fwrite(hdr, 1, sizeof hdr, f);

    uint8_t zero[3] = { 0, 0, 0 };
    for (int y = h - 1; y >= 0; y--) {          // BMP rows run bottom-up
        const uint8_t *row = rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; x++) {
            uint8_t bgr[3] = { row[x * 3 + 2], row[x * 3 + 1], row[x * 3] };
            fwrite(bgr, 1, 3, f);
        }
        if (pad) fwrite(zero, 1, (size_t)pad, f);
    }
    fclose(f);
    return 0;
}

static int run_selftest(const char *core_path, const char *rom, int frames,
                        const char *shot) {
    if (core_load(&core, core_path) != 0) return 1;
    core.set_environment(env_cb);
    core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);
    core.set_input_state(input_state_cb);
    core.init();
    if (core_load_game(&core, rom) != 0) { core_unload(&core); return 1; }
    core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    printf("core:   %s %s\n", core.sys.library_name, core.sys.library_version);
    printf("geom:   %ux%u (max %ux%u) dar=%.4f\n",
           core.av.geometry.base_width, core.av.geometry.base_height,
           core.av.geometry.max_width, core.av.geometry.max_height,
           core.av.geometry.aspect_ratio);
    printf("timing: %.4f fps, %.1f Hz audio\n",
           core.av.timing.fps, core.av.timing.sample_rate);
    printf("pixfmt: %u (%s)\n", g_pixfmt,
           g_pixfmt == RETRO_PIXEL_FORMAT_0RGB1555 ? "0RGB1555" :
           g_pixfmt == RETRO_PIXEL_FORMAT_RGB565 ? "RGB565" : "XRGB8888");
    printf("state:  %zu bytes\n", core.serialize_size());
    printf("sram:   %zu bytes\n", core.get_memory_size(RETRO_MEMORY_SAVE_RAM));

    double t0 = now_sec();
    for (int i = 0; i < frames; i++) core.run();
    double dt = now_sec() - t0;
    printf("ran %d frames in %.2fs = %.0f fps (%.1fx realtime)\n",
           frames, dt, frames / dt, frames / dt / 60.0);

    if (!g_have_frame) { printf("NO FRAME PRODUCED\n"); core_unload(&core); return 1; }
    printf("frame:  %dx%d\n", g_frame_w, g_frame_h);

    // Non-black check: a black screen means it booted but never drew.
    unsigned long sum = 0;
    size_t n = (size_t)g_frame_w * g_frame_h * 3;
    for (size_t i = 0; i < n; i++) sum += g_rgb[i];
    printf("mean pixel value: %.1f\n", (double)sum / n);

    if (shot && write_bmp(shot, g_rgb, g_frame_w, g_frame_h) == 0)
        printf("wrote %s\n", shot);

    // Save-state round trip: the same input from the same state must produce
    // the same frame, or states are silently lossy.
    size_t ssize = core.serialize_size();
    void *sbuf = malloc(ssize);
    int rc = 0;
    if (sbuf && core.serialize(sbuf, ssize)) {
        for (int i = 0; i < 60; i++) core.run();
        unsigned long ha = 5381;
        for (size_t i = 0; i < n; i++) ha = ha * 33 + g_rgb[i];

        if (!core.unserialize(sbuf, ssize)) {
            printf("state: unserialize FAILED\n");
            rc = 1;
        } else {
            for (int i = 0; i < 60; i++) core.run();
            unsigned long hb = 5381;
            for (size_t i = 0; i < n; i++) hb = hb * 33 + g_rgb[i];
            printf("state: round trip %s (%lx vs %lx)\n",
                   ha == hb ? "OK" : "MISMATCH", ha, hb);
            if (ha != hb) rc = 1;
        }
    } else {
        printf("state: serialize FAILED\n");
        rc = 1;
    }
    free(sbuf);

    // SRAM path: write, clobber memory, read back.
    state_paths_init(rom);
    size_t srsize = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    uint8_t *mem = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (mem && srsize) {
        sram_load(&core);
        for (size_t i = 0; i < srsize; i++) mem[i] = (uint8_t)(i * 7 + 1);
        if (sram_save(&core) != 0) { printf("sram: save FAILED\n"); rc = 1; }
        memset(mem, 0, srsize);
        sram_load(&core);
        bool ok = true;
        for (size_t i = 0; i < srsize; i++)
            if (mem[i] != (uint8_t)(i * 7 + 1)) { ok = false; break; }
        printf("sram: round trip %s\n", ok ? "OK" : "MISMATCH");
        if (!ok) rc = 1;
        memset(mem, 0, srsize);
        sram_save(&core);       // don't leave the test pattern on disk
    }

    core_unload(&core);
    return rc;
}

// --------------------------------------------------------------------- main

int main(int argc, char **argv) {
    const char *rom = NULL, *core_path = NULL, *shot = NULL, *recolor_arg = NULL;
    bool want_audio = true, force = false, inline_mode = true, keys_only = false;
    int selftest = 0, scale_arg = 0;
    double strength_arg = -1.0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--core") && i + 1 < argc) core_path = argv[++i];
        else if (!strcmp(argv[i], "--slot") && i + 1 < argc) slot = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-audio")) want_audio = false;
        else if (!strcmp(argv[i], "--force")) force = true;
        else if (!strcmp(argv[i], "--inline")) inline_mode = true;
        else if (!strcmp(argv[i], "--fullscreen")) inline_mode = false;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale_arg = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--recolor") && i + 1 < argc) recolor_arg = argv[++i];
        else if (!strcmp(argv[i], "--recolor-strength") && i + 1 < argc)
            strength_arg = atof(argv[++i]);
        else if (!strcmp(argv[i], "--selftest") && i + 1 < argc) selftest = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--keys")) keys_only = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (argv[i][0] == '-') { usage(); return 2; }
        else rom = argv[i];
    }

    if (keys_only) {
        char p[512];
        ensure_dir(config_dir());
        snprintf(p, sizeof p, "%s/config", config_dir());
        config_defaults(&cfg);
        if (!file_exists(p)) config_write_default(p);
        // Naming a ROM shows the bindings that ROM's platform would actually use.
        const char *sys = system_for_ext(rom);
        config_load(&cfg, p, sys);
        if (*sys) printf("Platform overrides applied: %s\n\n", sys);
        config_print(&cfg, p);
        return 0;
    }

    if (!rom) { usage(); return 2; }
    if (slot < 0 || slot >= MAX_STATE_SLOTS) slot = 0;

    int recolor_mode = -1;
    if (recolor_arg) {
        recolor_mode = recolor_mode_from_name(recolor_arg);
        if (recolor_mode < 0) {
            fprintf(stderr, APP_NAME ": unknown recolor mode '%s'\n"
                            "  expected: off hue nearest duotone dither\n",
                    recolor_arg);
            return 2;
        }
    }

    char exedir0[512];
    snprintf(exedir0, sizeof exedir0, "%s", argv[0]);
    if (selftest > 0) {
        char cb[600];
        if (!core_path) {
            if (find_core(cb, sizeof cb, rom, dirname(exedir0)) != 0) {
                fprintf(stderr, APP_NAME ": no core found for %s\n", rom);
                return 1;
            }
            core_path = cb;
        }
        // No terminal to ask, so the built-in palette stands in.
        if (recolor_mode > RECOLOR_OFF) {
            theme_fallback(&theme);
            double s = strength_arg >= 0.0 ? strength_arg : 1.0;
            recolor_build(&recolor, recolor_mode, &theme, s);
            printf("recolor: %s (built-in palette, strength %.2f)\n",
                   recolor_mode_name(recolor_mode), s);
        }
        int rc = run_selftest(core_path, rom, selftest, shot);
        recolor_free(&recolor);
        return rc;
    }

    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr, APP_NAME ": stdout is not a terminal\n");
        return 1;
    }

    ensure_dir(config_dir());
    snprintf(g_sysdir, sizeof g_sysdir, "%s", config_dir());

    char cfgpath[512];
    snprintf(cfgpath, sizeof cfgpath, "%s/config", config_dir());
    config_defaults(&cfg);
    if (!file_exists(cfgpath)) config_write_default(cfgpath);
    const char *system = system_for_ext(rom);
    config_load(&cfg, cfgpath, system);
    if (recolor_mode >= 0) cfg.recolor = recolor_mode;
    if (strength_arg >= 0.0) cfg.recolor_strength = strength_arg;
    g_recolor_strength = cfg.recolor_strength;
    // CLI --scale wins over the config default.
    if (scale_arg > 0) g_scale = scale_arg;
    else g_scale = cfg.scale;
    logmsg("system: '%s', scale %d", system, g_scale);

    char exedir[512];
    snprintf(exedir, sizeof exedir, "%s", argv[0]);
    char *ed = dirname(exedir);
    char corebuf[600];
    if (!core_path) {
        if (find_core(corebuf, sizeof corebuf, rom, ed) != 0) {
            fprintf(stderr, APP_NAME ": no core found for %s (looked for %s)\n"
                            "  place it in %s/cores/ or pass --core\n",
                    rom, corebuf, config_dir());
            return 1;
        }
        core_path = corebuf;
    }

    // A private handle to the terminal, so core logging on stdout/stderr can
    // never corrupt the display.
    int ttyfd = dup(STDOUT_FILENO);
    if (ttyfd < 0) { perror("dup"); return 1; }

    char logpath[512];
    snprintf(logpath, sizeof logpath, "%s/%s.log", config_dir(), APP_NAME);
    logf = fopen(logpath, "w");

    // Redirect before the core is touched at all: cores chatter on stdout
    // during load (snes9x prints its memory map), which would otherwise land
    // on the terminal above our image.
    int errfd = dup(STDERR_FILENO);
    if (errfd >= 0) g_err = fdopen(errfd, "w");
    if (logf) {
        fflush(stdout);
        fflush(stderr);
        dup2(fileno(logf), STDOUT_FILENO);
        dup2(fileno(logf), STDERR_FILENO);
    }

    if (core_load(&core, core_path) != 0) {
        errmsg(APP_NAME ": failed to load core %s\n  see %s\n", core_path, logpath);
        return 1;
    }

    core.set_environment(env_cb);
    core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);
    core.set_input_state(input_state_cb);
    core.init();

    if (core_load_game(&core, rom) != 0) {
        errmsg(APP_NAME ": failed to load %s\n  see %s\n", rom, logpath);
        core_unload(&core);
        return 1;
    }
    core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    logmsg("core: %s %s", core.sys.library_name, core.sys.library_version);
    logmsg("av: %ux%u dar=%.4f fps=%.4f rate=%.1f",
           core.av.geometry.base_width, core.av.geometry.base_height,
           core.av.geometry.aspect_ratio, core.av.timing.fps,
           core.av.timing.sample_rate);

    state_paths_init(rom);
    sram_load(&core);

    term.fd = ttyfd;
    term.inline_mode = inline_mode;
    if (g_scale < 1) g_scale = 1;
    if (g_scale > 8) g_scale = 8;
    if (term_enter(&term) != 0) {
        errmsg(APP_NAME ": failed to set raw mode\n");
        core_unload(&core);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGWINCH, on_winch);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, on_fatal);
    signal(SIGBUS, on_fatal);
    signal(SIGABRT, on_fatal);

    // Nothing can be drawn without the graphics protocol, so bail before we
    // start writing image data at a terminal that would only render it as
    // garbage. Detection relies on the terminal answering a query, which tmux
    // swallows even when the outer terminal supports it.
    if (!term_probe_graphics(ttyfd) && !force) {
        term_leave(&term);
        errmsg(
            APP_NAME ": this terminal did not acknowledge the kitty graphics protocol.\n"
            "\n"
            "  * Use Ghostty or kitty.\n"
            "  * Run outside tmux, which does not forward the protocol's replies,\n"
            "    so detection fails even when the outer terminal supports it.\n"
            "  * If you are certain your terminal supports it, pass --force.\n"
            "\n"
            "To exercise emulation without a terminal:\n"
            "  " APP_NAME " --selftest 900 --shot out.bmp <rom>\n");
        close(ttyfd);
        core_unload(&core);
        if (logf) fclose(logf);
        return 1;
    }

    // Claim our block of the scrollback before anything else draws, so the
    // origin we compute stays valid.
    if (term.inline_mode) {
        int c, r, cw, ch;
        if (term_size(ttyfd, &c, &r, &cw, &ch) == 0) {
            if (cw <= 0) cw = 8;
            if (ch <= 0) ch = 16;
            term_reserve_inline(&term, inline_rows_needed(c, r, cw, ch));
        } else {
            term_reserve_inline(&term, 16);
        }
    }

    int kbd = input_init(&input, ttyfd);

    // Grouped with the other startup probes: these queries have read windows
    // that would swallow keystrokes if run once the game is underway.
    theme_fallback(&theme);
    if (cfg.recolor != RECOLOR_OFF) {
        theme_query(&theme, ttyfd);
        logmsg("theme: %s, bg #%02x%02x%02x fg #%02x%02x%02x",
               theme.from_terminal ? "from terminal" : "built-in fallback",
               theme.bg[0], theme.bg[1], theme.bg[2],
               theme.fg[0], theme.fg[1], theme.fg[2]);
        double t0 = now_sec();
        if (recolor_build(&recolor, cfg.recolor, &theme, cfg.recolor_strength) != 0)
            logmsg("recolor: failed to build LUT");
        else
            logmsg("recolor: %s LUT built in %.1f ms",
                   recolor_mode_name(cfg.recolor), (now_sec() - t0) * 1000.0);
    }

    if (renderer_start(&rend, ttyfd) != 0) {
        term_leave(&term);
        core_unload(&core);
        errmsg(APP_NAME ": failed to start renderer\n");
        return 1;
    }
    pthread_mutex_lock(&rend.m);
    rend.show_status = cfg.show_stats;
    pthread_mutex_unlock(&rend.m);

    if (kbd != 0)
        renderer_set_osd(&rend,
            "no kitty keyboard protocol: keys will stick - run outside tmux", 6.0);
    else
        osd("%s  |  ^C quit  F2 save  F3 load  Tab fast-forward",
            core.sys.library_name ? core.sys.library_name : "core");

    if (want_audio) {
        if (audio_start(&audio, core.av.timing.sample_rate) != 0) {
            audio = NULL;
            logmsg("audio: failed to start");
        } else {
            audio_set_volume(audio, cfg.volume);
        }
    }

    double fps = core.av.timing.fps > 0 ? core.av.timing.fps : 60.0;
    double rate = core.av.timing.sample_rate > 0 ? core.av.timing.sample_rate : 32040.0;
    size_t target_queued = (size_t)(rate / fps * 3.0);   // ~3 frames of latency

    double next_frame = now_sec();
    double stat_t = now_sec();
    double paused_draw = 0;
    int frames_this_window = 0;
    double shown_fps = 0;
    unsigned long long last_sent = 0, last_dropped = 0;
    double sram_check = now_sec();

    while (running && !g_quit) {
        if (g_winch) {
            g_winch = 0;
            recompute_layout();
        }
        if (g_geom_dirty) {
            g_geom_dirty = false;
            recompute_layout();
        }

        handle_input();

        double t = now_sec();

        if (user_paused || focus_paused) {
            if (t - paused_draw > 0.1) {
                paused_draw = t;
                submit_frame();
            }
            usleep(5000);
            continue;
        }

        // Pace on audio consumption when we have it, wall clock otherwise.
        if (!fast_forward) {
            if (audio) {
                if (audio_queued_frames(audio) > target_queued) {
                    usleep(1000);
                    continue;
                }
            } else {
                if (t < next_frame) { usleep(500); continue; }
                if (next_frame < t - 0.25) next_frame = t;   // resync after a stall
                next_frame += 1.0 / fps;
            }
        }

        core.run();
        submit_frame();
        frames_this_window++;

        if (t - stat_t >= 0.5) {
            shown_fps = frames_this_window / (t - stat_t);
            frames_this_window = 0;
            stat_t = t;

            pthread_mutex_lock(&rend.m);
            unsigned long long sent = rend.sent, dropped = rend.dropped;
            pthread_mutex_unlock(&rend.m);
            double disp_fps = (sent - last_sent) / 0.5;
            unsigned long long drop_delta = dropped - last_dropped;
            last_sent = sent;
            last_dropped = dropped;

            char status[256];
            int aud_ms = audio ? (int)(audio_queued_frames(audio) * 1000.0 / rate) : -1;
            snprintf(status, sizeof status,
                     "slot %d  emu %.0f  disp %.0f%s  drop %llu  aud %dms%s",
                     slot, shown_fps, disp_fps, fast_forward ? " FF" : "",
                     drop_delta, aud_ms, muted ? "  MUTE" : "");
            renderer_set_status(&rend, status);
        }

        if (t - sram_check > 5.0) {
            sram_check = t;
            if (sram_dirty(&core)) {
                sram_save(&core);
                logmsg("sram autosaved");
            }
        }
    }

    sram_save(&core);
    renderer_stop(&rend);
    input_shutdown(&input);
    term_leave(&term);
    audio_stop(audio);
    core_unload(&core);

    recolor_free(&recolor);
    free(g_rgb);
    free(g_scaled);
    free(g_zoom);
    if (logf) fclose(logf);
    if (g_err) fclose(g_err);
    return 0;
}
