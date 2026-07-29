#ifndef EMU_H
#define EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "libretro.h"

#define APP_NAME "rom"            // executable and config-directory name
#define MAX_STATE_SLOTS 10
#define OSD_MSG_MAX 128
#define MAX_HOTKEY_KEYS 3        // alternates per action, e.g. Ctrl+c / Ctrl+q

// Modifiers ride in the high bits of a key code; the low bits stay a plain
// codepoint so held-key tracking can ignore them.
#define MOD_CTRL      0x40000000u
#define MOD_ALT       0x20000000u
#define KEY_BASE_MASK 0x001FFFFFu

// ---------------------------------------------------------------- keys

// Regular keys are their Unicode codepoint. Functional keys live in the
// private-use area so both kinds share one namespace.
enum {
    KEY_NONE = 0,
    KEY_ESC = 27,
    KEY_TAB = 9,
    KEY_ENTER = 13,
    KEY_BACKSPACE = 127,
    KEY_SPACE = 32,

    KEY_UP = 0xE000, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_INSERT, KEY_DELETE, KEY_PGUP, KEY_PGDN,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT,
    KEY_FOCUS_IN, KEY_FOCUS_OUT,   // synthesized from CSI I / CSI O, not bindable
    KEY_MAX_
};

const char *key_name(uint32_t key);
uint32_t key_from_name(const char *name);

// ---------------------------------------------------------------- config

enum {
    HK_QUIT, HK_PAUSE, HK_RESET, HK_SAVE_STATE, HK_LOAD_STATE,
    HK_SLOT_NEXT, HK_SLOT_PREV, HK_FAST_FORWARD, HK_MUTE, HK_STATS,
    HK_RECOLOR, HK_VOL_UP, HK_VOL_DOWN,
    HK_COUNT
};

typedef struct {
    uint32_t pad[16];        // indexed by RETRO_DEVICE_ID_JOYPAD_*
    uint32_t hotkey[HK_COUNT][MAX_HOTKEY_KEYS];
    int      volume;           // 0..100
    bool     integer_scale;    // snap image to whole multiples of native size
    bool     show_stats;
    bool     pause_on_unfocus; // pause when the terminal loses focus
    int      recolor;          // RECOLOR_*
    double   recolor_strength; // 0..1 blend against the original colours
    int      scale;            // default integer zoom for inline mode
} Config;

void config_defaults(Config *c);
// `system` is a slug like "snes" or "gb"; sections suffixed with it (e.g.
// [options.gb]) are applied on top of the unsuffixed ones. NULL for none.
int  config_load(Config *c, const char *path, const char *system);
int  config_write_default(const char *path);
void config_print(const Config *c, const char *path);
const char *hotkey_name(int hk);

// ---------------------------------------------------------------- core

typedef struct {
    void *handle;
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*api_version)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*reset)(void);
    void (*run)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);

    struct retro_system_info sys;
    struct retro_system_av_info av;
    bool loaded;
} Core;

int  core_load(Core *c, const char *path);
int  core_load_game(Core *c, const char *rom_path);
void core_unload(Core *c);

// ---------------------------------------------------------------- theme

typedef struct {
    uint8_t pal[16][3];      // ANSI 0-15
    uint8_t fg[3], bg[3];
    bool    from_terminal;   // false means the built-in fallback is in use
} Theme;

void theme_fallback(Theme *t);
// Queries the terminal's palette via OSC 4/10/11. Returns 0 if it answered.
int  theme_query(Theme *t, int ttyfd);

// -------------------------------------------------------------- recolor

enum {
    RECOLOR_OFF, RECOLOR_HUE, RECOLOR_NEAREST, RECOLOR_DUOTONE, RECOLOR_DITHER,
    RECOLOR_COUNT
};

typedef struct {
    int      mode;
    uint8_t *lut;    // 65536 * 3, one RGB24 result per RGB565 input
    uint8_t *pair;   // 65536 * 7, dither: rgbA, rgbB, bayer threshold 0-15
} Recolor;

int  recolor_build(Recolor *rc, int mode, const Theme *t, double strength);
void recolor_free(Recolor *rc);
int  recolor_mode_from_name(const char *s);
const char *recolor_mode_name(int mode);

// ---------------------------------------------------------------- video

typedef struct {
    int      src_w, src_h;      // native frame size from the core
    unsigned pixfmt;            // RETRO_PIXEL_FORMAT_*
    double   dar;               // display aspect ratio
} VideoInfo;

// Convert a core framebuffer to packed RGB24, honoring pitch. Returns bytes
// written, or 0 on unsupported format.
size_t video_convert(uint8_t *dst, const void *src, unsigned w, unsigned h,
                     size_t pitch, unsigned pixfmt, const Recolor *rc);

// Box-downscale RGB24. Only used when the source is larger than the target.
void video_downscale(uint8_t *dst, int dw, int dh,
                     const uint8_t *src, int sw, int sh);

// Nearest-neighbour integer upscale, so pixel art stays crisp rather than
// being smoothed by the terminal's scaler.
void video_upscale(uint8_t *dst, const uint8_t *src, int w, int h, int scale);

// ---------------------------------------------------------------- renderer

#define RB_COUNT 3

typedef struct {
    int  term_cols, term_rows;
    int  x, y;             // 1-based cell origin of the image
    int  cols, rows;       // cell extent of the image
    bool natural;          // transmit at natural pixel size (omit c=/r=)
    int  status_row;       // 1-based row for the status line
    bool allow_clear;      // may we clear the whole screen on relayout
} Layout;

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  cv;
    pthread_t       thread;
    bool            running;

    uint8_t *buf[RB_COUNT];
    size_t   cap[RB_COUNT];
    int      w[RB_COUNT], h[RB_COUNT];
    int      ready, busy, writing;

    Layout layout;
    bool   layout_dirty;

    char status[256];
    char osd[OSD_MSG_MAX];
    double osd_until;
    bool  show_status;

    int  ttyfd;
    unsigned long long sent, dropped;
} Renderer;

int  renderer_start(Renderer *r, int ttyfd);
void renderer_stop(Renderer *r);
// Called from the emulator thread with a native-resolution RGB24 frame.
void renderer_submit(Renderer *r, const uint8_t *rgb, int w, int h);
void renderer_set_layout(Renderer *r, const Layout *l);
void renderer_set_status(Renderer *r, const char *s);
void renderer_set_osd(Renderer *r, const char *s, double seconds);

// ---------------------------------------------------------------- input

typedef struct {
    bool     held[KEY_MAX_ - 0xE000 + 0x200];  // unused; see input.c
    int      ttyfd;
    bool     kitty_kbd;
    bool     focus_events;
} Input;

typedef struct {
    uint32_t key;
    bool     pressed;   // false = release
    bool     repeat;
} KeyEvent;

int  input_init(Input *in, int ttyfd);
void input_shutdown(Input *in);
// Drains pending input. Returns number of events written to `ev`.
int  input_poll(Input *in, KeyEvent *ev, int max_ev);

// ---------------------------------------------------------------- audio

typedef struct AudioCtx AudioCtx;

int    audio_start(AudioCtx **out, double sample_rate);
void   audio_stop(AudioCtx *a);
// Returns frames accepted (may be fewer than `frames` if the buffer is full).
size_t audio_write(AudioCtx *a, const int16_t *pcm, size_t frames);
size_t audio_free_frames(AudioCtx *a);
size_t audio_queued_frames(AudioCtx *a);
void   audio_set_volume(AudioCtx *a, int vol_0_100);
void   audio_flush(AudioCtx *a);

// ---------------------------------------------------------------- state

void state_paths_init(const char *rom_path);
const char *state_rom_base(void);
int  state_save(Core *c, int slot, char *err, size_t errlen);
int  state_load(Core *c, int slot, char *err, size_t errlen);
bool state_slot_exists(int slot);
int  sram_load(Core *c);
int  sram_save(Core *c);
bool sram_dirty(Core *c);

// ---------------------------------------------------------------- terminal

typedef struct {
    int  fd;
    bool raw;
    bool alt;
    bool inline_mode;      // draw in the normal flow instead of the alt screen
    int  inline_origin;    // 1-based row where the reserved block starts
    int  inline_rows;      // rows reserved, including the status line
} Term;

int  term_enter(Term *t);
void term_leave(Term *t);
// Scrolls up `rows` lines of room at the cursor and reports the absolute row
// the block now starts on. Inline mode only.
int  term_reserve_inline(Term *t, int rows);
int  term_size(int fd, int *cols, int *rows, int *cell_w, int *cell_h);
bool term_probe_graphics(int fd);

// ---------------------------------------------------------------- util

double now_sec(void);
const char *config_dir(void);
int  ensure_dir(const char *path);

#endif
