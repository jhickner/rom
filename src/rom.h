#ifndef EMU_H
#define EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include "libretro.h"

#define APP_NAME "rom"            // executable and config-directory name
#if defined(__APPLE__)
#define CORE_EXT ".dylib"
#elif defined(__linux__)
#define CORE_EXT ".so"
#else
#error "unsupported platform"
#endif
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
    HK_RECOLOR, HK_VOL_UP, HK_VOL_DOWN, HK_SCALE_UP, HK_SCALE_DOWN,
    HK_TERM_SCALE,
    HK_COUNT
};

// Core options are passed straight through to the core, so the names and
// values are whatever that core documents. rom never interprets them.
#define MAX_CORE_OPTIONS 32
#define CORE_OPT_KEY_MAX 64
#define CORE_OPT_VAL_MAX 64

typedef struct {
    char key[CORE_OPT_KEY_MAX];
    char val[CORE_OPT_VAL_MAX];
} CoreOption;

// The DS touchscreen, driven from the keyboard: four directions walk a cursor
// and one key taps it down. The mouse drives the same touchscreen directly.
enum { STY_UP, STY_DOWN, STY_LEFT, STY_RIGHT, STY_TOUCH, STY_COUNT };

typedef struct {
    uint32_t pad[16];        // indexed by RETRO_DEVICE_ID_JOYPAD_*
    uint32_t stylus[STY_COUNT];
    int      stylus_speed;     // cursor pixels a frame
    bool     mouse;            // touch the DS screen with the mouse
    uint32_t hotkey[HK_COUNT][MAX_HOTKEY_KEYS];
    int      volume;           // 0..100
    bool     integer_scale;    // snap image to whole multiples of native size
    bool     show_stats;
    bool     pause_on_unfocus; // pause when the terminal loses focus
    bool     hide_on_blur;     // erase the image while the pane is unfocused
    int      recolor;          // RECOLOR_*
    double   recolor_strength; // 0..1 blend against the original colours
    int      scale;            // default integer zoom for inline mode
    bool     term_scale;       // terminal stretches the image; false upscales
                               // here instead, at scale^2 the bytes on the wire
    bool     async_readback;   // hw cores: overlap GPU readback with emulation
                               // at the cost of one frame of input latency
    bool     tmux_keyboard;    // under tmux, put the terminal into the kitty
                               // keyboard protocol for real key releases
    bool     tmux_alt_keys;    // hand Alt chords to tmux's own key table
                               // instead of the game
    CoreOption core_opt[MAX_CORE_OPTIONS];
    int        n_core_opt;
} Config;

void config_defaults(Config *c);
// Adds or replaces a core option. Returns -1 when the table is full.
int  config_set_core_option(Config *c, const char *key, const char *val);
// Applies a "key=value key=value" string, as carried by a CoreSpec.
void config_apply_core_options(Config *c, const char *spec);
// Value for `key`, or NULL when unset.
const char *config_core_option(const Config *c, const char *key);
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

// One table entry drives core selection, the per-platform config slug, and
// fetching the core when it is not installed yet.
typedef struct {
    const char *exts;      // space separated
    const char *sys;       // slug for [options.<sys>] sections
    const char *core;      // shared library filename, also the build artifact
    const char *repo;      // github path
    const char *subdir;    // directory within the repo to build in, or NULL
    const char *makefile;  // -f argument, or NULL for the default Makefile
    const char *makeargs[3];  // extra make arguments, NULL terminated
    const char *defopts;   // "key=value key=value" core options, or NULL
    const char *datafile;  // build-dir file the core needs in the system
                           // directory (prboom.wad), or NULL
} CoreSpec;

// Clones the core's repo under the config dir, builds it, and installs the
// result into <config>/cores. Writes the installed path to `out`.
int core_fetch(const CoreSpec *spec, char *out, size_t cap);

// ---------------------------------------------------------------- theme

typedef struct {
    uint8_t pal[16][3];      // ANSI 0-15
    uint8_t fg[3], bg[3];
    bool    from_terminal;   // false means the built-in fallback is in use
    bool    bg_from_terminal;// the palette can answer while fg/bg does not, and
                             // recolouring is built around the background
} Theme;

void theme_fallback(Theme *t);
// Queries the terminal's palette via OSC 4/10/11. Returns 0 if it answered.
int  theme_query(Theme *t, int ttyfd);

// -------------------------------------------------------------- recolor

enum {
    RECOLOR_OFF, RECOLOR_HUE, RECOLOR_NEAREST, RECOLOR_DUOTONE, RECOLOR_TINT,
    RECOLOR_DITHER, RECOLOR_COUNT
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
    pthread_mutex_t wm;         // serializes tty writes with the main thread
    pthread_cond_t  cv;
    pthread_t       thread;
    bool            running;

    uint8_t *buf[RB_COUNT];
    size_t   cap[RB_COUNT];
    int      w[RB_COUNT], h[RB_COUNT];
    int      ready, busy, writing;
    int      last;              // newest submitted buffer, -1 before the first

    Layout layout;
    bool   layout_dirty;
    bool   status_only;         // image unchanged; only the text needs redrawing
    bool   hidden;              // nothing goes to the tty until it is shown again

    char status[256];
    char osd[OSD_MSG_MAX];
    double osd_until;
    bool  show_status;

    int  ttyfd;
    unsigned long long sent, dropped;
    unsigned long long skipped;    // identical frames never retransmitted
    unsigned long long bytes;      // total transmitted, for the stats line
    double write_sec;              // cumulative time blocked in write()
    // Rows that actually differ from the frame before, summed over transmitted
    // frames. Measures the headroom a damage-region update scheme would have.
    unsigned long long rows_changed, rows_total;
} Renderer;

int  renderer_start(Renderer *r, int ttyfd);
void renderer_stop(Renderer *r);
// Called from the emulator thread with a native-resolution RGB24 frame.
void renderer_submit(Renderer *r, const uint8_t *rgb, int w, int h);
void renderer_set_layout(Renderer *r, const Layout *l);
void renderer_set_status(Renderer *r, const char *s);
void renderer_set_osd(Renderer *r, const char *s, double seconds);
// Drops the image and blanks its cells, holding every write off the tty until
// it is called again with false, which redraws the last frame in full.
void renderer_set_hidden(Renderer *r, bool hidden);
// Take/release the tty so the main thread can emit escape codes without
// landing in the middle of a frame transmission.
void renderer_lock_tty(Renderer *r);
void renderer_unlock_tty(Renderer *r);

// ---------------------------------------------------------------- graphics

// Under tmux the image cannot be placed at the cursor: tmux neither tracks nor
// redraws it. The way through is the kitty protocol's Unicode placeholders -
// ordinary text cells carrying the image id, which tmux moves and redraws
// like any other text - with every graphics escape wrapped in a tmux DCS.

// Under 2^24 with no zero byte, so a truecolor foreground carries it whole.
#define GFX_IMAGE_ID 0x0A5F31

// Reads $TMUX once. Call before anything is written to the terminal.
void gfx_init(void);
// True when escapes need tmux wrapping and the image needs placeholder cells.
bool gfx_tmux(void);
// True when tmux will forward our escapes to the terminal it is running in.
bool gfx_passthrough_ok(void);
// True when tmux will forward mouse events to the pane. Always true outside it.
bool gfx_tmux_mouse_ok(void);
// Appends one graphics APC escape: `head` is the control data, `payload` the
// base64 body (omitted when `plen` is 0). Returns bytes written.
size_t gfx_apc(char *out, const char *head, const char *payload, size_t plen);
// Copies an escape sequence meant for the terminal rather than for tmux,
// wrapping it when needed. `out` needs room for len * 2 + 16.
size_t gfx_wrap(char *out, const char *seq, size_t len);
// Upper bound on what gfx_apc writes for a `plen`-byte payload.
size_t gfx_apc_max(size_t plen);
// Drops our image from the terminal.
void gfx_delete_image(int fd);
// Lays down the placeholder cells for `l`. Render thread only: it reuses one
// internal buffer.
void gfx_write_placeholders(int fd, const Layout *l);
// Prebuilt terminal-restoring escapes, for the fatal signal handler to write.
const char *gfx_restore_seq(size_t *len);

// ---------------------------------------------------------------- input

typedef struct {
    bool     held[KEY_MAX_ - 0xE000 + 0x200];  // unused; see input.c
    int      ttyfd;
    bool     kitty_kbd;
    bool     kbd_active;    // the protocol is currently pushed on the terminal
    bool     focus_events;
    // Mouse reports are state, not events: input_poll leaves the latest here
    // rather than in the queue.
    bool     mouse;         // tracking is on
    bool     mouse_pixels;  // positions are pixels rather than cells
    bool     mouse_down;    // left button, as of the last report
    int      mouse_x, mouse_y;   // 1-based, in whichever unit mouse_pixels says
    unsigned mouse_presses; // button-down edges seen, for taps inside one frame
} Input;

typedef struct {
    uint32_t key;
    bool     pressed;   // false = release
    bool     repeat;
} KeyEvent;

// `allow_tmux_kbd` permits reaching past tmux to put the terminal itself into
// the kitty keyboard protocol, which is the only way to get real key releases
// there. Ignored outside tmux, where the protocol is always used if available.
int  input_init(Input *in, int ttyfd, bool allow_tmux_kbd);
void input_shutdown(Input *in);
// Release the terminal's keyboard mode while another pane has the focus, and
// take it back on return. No-ops when the protocol is not in use.
void input_kbd_push(Input *in);
void input_kbd_pop(Input *in);
// Starts mouse reporting, in pixel coordinates where the terminal offers them.
// Costs the terminal's own click-drag selection for as long as it is on, so it
// is only turned on for a system that has something to point at.
void input_mouse_enable(Input *in);
// Forgets a button that is still down, for when its release will never arrive.
void input_mouse_reset(Input *in);
// Drains pending input. Returns number of events written to `ev`.
int  input_poll(Input *in, KeyEvent *ev, int max_ev);
// The key in tmux's own spelling ("M-Left"), or NULL for one tmux cannot name.
const char *tmux_key_name(uint32_t key);
// Feeds `name` to tmux's key table as if it had been typed at the client, so
// whatever is bound to it runs. Unbound keys do nothing.
void input_send_tmux_key(const char *name);

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
// Filename without directory or extension, the name saves are keyed on.
void rom_base_name(const char *rom_path, char *out, size_t cap);
// Settings the player changes mid-game and expects back next time, kept per
// ROM under <config>/games. `key` names the setting, e.g. "volume" or "scale";
// a stored value outside lo..hi is ignored in favour of `fallback`.
#define SCALE_MIN 1
#define SCALE_MAX 8
int  game_setting_load(const char *key, int fallback, int lo, int hi);
int  game_setting_save(const char *key, int value, int lo, int hi);
int  state_save(Core *c, int slot, char *err, size_t errlen);
int  state_load(Core *c, int slot, char *err, size_t errlen);
bool state_slot_exists(int slot);
// Slot holding the most recently written state for this game, -1 if none.
int  state_newest_slot(void);
// Bit N set means slot N holds a state for `rom_path`. Independent of the
// paths state_paths_init established, so it can be asked about other games.
unsigned state_slot_mask(const char *rom_path);
int  sram_load(Core *c);
int  sram_save(Core *c);
bool sram_dirty(Core *c);

// ---------------------------------------------------------------- recent

#define MAX_RECENT 40
#define RECENT_PATH_MAX 1024

typedef struct {
    char   path[RECENT_PATH_MAX];
    char   base[256];       // display name: filename without the extension
    time_t played;
} RecentEntry;

// Moves `rom_path` to the head of the played-games list.
void recent_record(const char *rom_path);
// Newest first. Entries whose file has since disappeared are left out.
int  recent_load(RecentEntry *out, int cap);
// Interactive picker on the terminal. Writes the chosen path to `out` and
// returns 0; 1 if the user cancelled, -1 if there is nothing to resume.
int  recent_pick(char *out, size_t cap);

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
// Drops the current image, wipes the reserved block, and reserves `rows`
// again from the same origin. Returns the new origin. Inline mode only.
int  term_resize_inline(Term *t, int rows);
int  term_size(int fd, int *cols, int *rows, int *cell_w, int *cell_h);
bool term_probe_graphics(int fd);

// ---------------------------------------------------------------- util

double now_sec(void);
const char *config_dir(void);
int  ensure_dir(const char *path);
int  write_file_atomic(const char *path, const void *data, size_t size);
// System slug for a ROM's extension, "" when unrecognised. Lives in main.c
// alongside the core table.
const char *rom_system_slug(const char *rom_path);

#endif
