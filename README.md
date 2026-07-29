# emu

A libretro frontend that renders into the terminal using the kitty graphics
protocol, with real key-release input via the kitty keyboard protocol.

Built and tested against SNES (snes9x), but nothing in the frontend is
SNES-specific — geometry, pixel format, framerate, aspect ratio, and sample
rate all come from the core at runtime.

## Build

```sh
make
```

Requires macOS (CoreAudio for output) and a terminal implementing the kitty
graphics and keyboard protocols — Ghostty or kitty.

## Cores

Cores are plain libretro `.dylib` files. Build one from source:

```sh
git clone --depth 1 https://github.com/libretro/snes9x vendor/snes9x
make -C vendor/snes9x/libretro -j8
cp vendor/snes9x/libretro/snes9x_libretro.dylib cores/
```

Or install the RetroArch cask and use its core downloader; cores land in
`~/Library/Application Support/RetroArch/cores`.

`emu` picks a core from the ROM extension and searches, in order:
`~/.config/emu/cores/`, `./cores/`, then `<exedir>/../cores/`. Override with
`--core <path>`.

| Extension | Core |
|---|---|
| `.sfc` `.smc` `.fig` | `snes9x_libretro.dylib` |
| `.nes` | `fceumm_libretro.dylib` |
| `.gb` `.gbc` | `gambatte_libretro.dylib` |
| `.gba` | `mgba_libretro.dylib` |
| `.md` `.gen` `.smd` | `genesis_plus_gx_libretro.dylib` |
| `.pce` | `mednafen_pce_fast_libretro.dylib` |

## Run

```sh
./emu "path/to/game.sfc"
```

**Run it in a bare Ghostty or kitty window, not inside tmux.** tmux's graphics
passthrough is slow and unreliable for animation, and it interferes with the
keyboard protocol.

On startup `emu` queries the terminal for the graphics protocol. If there is no
answer it restores the terminal and exits with an explanation rather than
filling the screen with escape sequences it cannot render. tmux swallows the
reply even when the outer terminal supports the protocol, which is why running
under tmux trips this. Override with `--force` if you know better.

The keyboard protocol is a warning, not a hard stop: without key-release events
held keys never lift, which makes games unplayable but leaves Escape working.

### Display modes

**Inline (default).** The game plays in the normal terminal flow, at the
platform's native resolution — one emulated pixel to one terminal pixel, no
zooming. `emu` scrolls up just enough room at the cursor, draws there, and puts
the status line directly beneath. Your scrollback is never cleared, and on exit
the final frame stays on screen with the prompt below it, like any other
command's output.

Because the frame is transmitted with no `c=`/`r=` fields, the terminal draws it
at exactly its pixel size instead of stretching it to a cell rect. For SNES that
is 256×224 in 32×14 cells.

`--scale N` zooms by whole multiples (2, 3, …). Zooming is done in the frontend
with nearest-neighbour sampling so pixel art stays sharp rather than being
smoothed by the terminal's scaler. If the requested zoom does not fit the
window, `emu` steps it down to the largest one that does.

**Fullscreen (`--fullscreen`).** The original behaviour: alt screen, image
centred and zoomed to fill the window at the core's aspect ratio, status line
pinned to the bottom row. Restores your screen on exit.

### Options

| Flag | Effect |
|---|---|
| `--core <path>` | Use a specific core |
| `--slot <n>` | Initial save-state slot (0–9) |
| `--no-audio` | Disable audio output |
| `--inline` | Play inline at native resolution (default) |
| `--fullscreen` | Take over the screen and zoom to fit |
| `--scale <n>` | Integer zoom for inline mode (default 1, max 8) |
| `--recolor <mode>` | `off` \| `hue` \| `nearest` \| `duotone` \| `dither` |
| `--recolor-strength <0..1>` | Blend the recolour against the original (default 1) |
| `--keys` | Print the current keybinds and exit |
| `--force` | Run even if the terminal does not ack kitty graphics |
| `--selftest <n>` | Run `n` frames headlessly, check states and SRAM, exit |
| `--shot <file>` | With `--selftest`, write the final frame as a BMP |

`--selftest` needs no terminal, which makes it useful for verifying a new core:

```sh
./emu --selftest 900 --shot title.bmp game.sfc
```

## Default keys

Game (SNES layout):

| Key | Button |
|---|---|
| Arrows | D-pad |
| `z` / `x` | B / A |
| `a` / `s` | Y / X |
| `q` / `w` | L / R |
| Enter | Start |
| Right Shift | Select |

Hotkeys:

| Key | Action |
|---|---|
| Ctrl+C *or* Ctrl+Q | Quit |
| `p` | Pause |
| F1 | Toggle status line |
| F2 / F3 | Save / load state |
| F6 / F7 | Previous / next slot |
| F5 | Reset |
| Tab (hold) | Fast-forward |
| F8 | Cycle recolour mode |
| `m` | Mute |

Quit deliberately takes a modifier — Escape is far too easy to hit by reflex
mid-game, and it is unbound by default.

`emu --keys` prints the bindings currently in effect, including any you have
rebound, without needing a ROM.

## Recolouring to the terminal theme

`--recolor <mode>` remaps the game's colours to your terminal's palette, which
`emu` reads at startup via the OSC 4 / 10 / 11 queries (Ghostty and kitty both
answer). If the terminal does not reply, a built-in Tango palette stands in.

| Mode | Effect |
|---|---|
| `off` | No remapping (default) |
| `hue` | Keep each pixel's lightness and saturation, adopt the nearest theme hue. Structure and shading survive intact — the recommended starting point |
| `nearest` | Hard-map every pixel to its closest theme colour. Strongest "terminal" look, flattens gradients |
| `duotone` | Map lightness across the background-to-foreground ramp |
| `dither` | Ordered 4×4 dither between the two closest theme colours, recovering tones a hard map would flatten |

`--recolor-strength <0..1>` blends against the original, so you can dial the
effect down. **F8 cycles modes at runtime**, which is the fastest way to find a
look you like.

Set it permanently in the config:

```ini
[options]
recolor          = hue
recolor_strength = 1.00
```

### How it works

Cores hand out finished RGB frames, not palettes — snes9x exports SRAM, WRAM
and VRAM through libretro, but not CGRAM. That turns out not to matter: by the
time a frame exists it has already been through colour math, windowing, mosaic
and brightness, so the palette would not describe the pixels anyway. Working on
the output is both simpler and core-agnostic.

Since snes9x emits RGB565, a 65,536-entry table covers **every possible input
colour exactly** — no approximation and no per-frame colour analysis. Mapping
happens once when the table is built (9 ms), and per-pixel cost drops to a
single indexed load. Measured overhead is a few percent of total frame time,
which at ~45× realtime is irrelevant.

A static table also means the result is temporally stable. Anything that
re-derived a palette per frame would shimmer as colours entered and left the
scene.

Matching is done in Oklab rather than RGB, because RGB distance produces poor
perceptual pairings. Near-neutral colours are special-cased onto the
background-to-foreground ramp — greys have no meaningful hue, and letting them
snap to whichever accent happens to be nearest tints stonework at random.

## Auto-pause

`emu` enables focus reporting (DECSET 1004) and pauses when the terminal window
or pane loses focus, resuming when it comes back. Manual pause is tracked
separately, so refocusing will not un-pause a game you paused yourself.

Losing focus also drops every held key. The release event for anything held at
that moment would never arrive — you would come back to Link still walking into
a wall.

Terminals that do not implement focus reporting simply never send the events and
the feature stays inert. Under tmux it requires `set -g focus-events on`.

Disable with `pause_on_unfocus = false` in the config.

## Config

Written to `~/.config/emu/config` on first run; edit and restart. Key names are
single characters or one of: `Up Down Left Right Home End Insert Delete PageUp
PageDown F1`–`F12` `Escape Tab Enter Backspace Space LShift RShift LCtrl RCtrl
LAlt RAlt`.

Hotkeys accept the `Ctrl+` and `Alt+` prefixes, and a comma-separated list of
alternates:

```ini
[hotkeys]
quit = Ctrl+c, Ctrl+q

[options]
volume           = 100
integer_scale    = false   # snap the image to whole multiples of native size
show_stats       = false   # F1 toggles the status line at runtime
pause_on_unfocus = true    # pause when the terminal loses focus
```

Modifiers apply to hotkeys only. Game buttons ignore them, so holding Ctrl will
not stop your D-pad from working.

## Files

| Path | Contents |
|---|---|
| `~/.config/emu/config` | Keybinds and options |
| `~/.config/emu/saves/<rom>.srm` | Battery SRAM, autosaved every 5s when dirty and on exit |
| `~/.config/emu/states/<rom>.state<n>` | Save states, slots 0–9 |
| `~/.config/emu/emu.log` | Frontend and core logging |

Saves and states are written to a temp file and renamed, so an interrupted
write can't destroy a good save.

## Design notes

**Rendering runs on its own thread** behind a three-buffer mailbox. The
emulator thread always writes into a buffer nobody is reading, so a terminal
that can't keep up drops frames instead of stalling emulation and starving
audio. F1 shows a status line reporting emulated fps, displayed fps, dropped
frames per half-second, and audio latency; it is hidden by default and centred
under the image when shown.

**Bandwidth is bounded by output size, not console resolution.** Inline mode at
1× transmits exactly the core's native frame, which is the cheapest it can be —
about 14 MB/s at SNES resolution and 60fps. `--scale N` multiplies that by N²,
so 2× costs roughly 55 MB/s. Fullscreen sends native pixels and lets the GPU
scale via the `c=`/`r=` cell rect, so upscaling there is free; only when the
core outputs more pixels than the window can show (PSX hi-res, hi-res SNES
modes) is the frame box-downscaled first.

**Timing is paced by audio consumption**, holding roughly three frames of
buffer, so playback tracks real time without drift. With `--no-audio` it falls
back to a wall clock.

**Core stdout/stderr are redirected to the log** before the core is loaded, so
chatter during ROM load (snes9x prints its memory map) never reaches the
screen. The terminal is held on a private dup'd fd, and a dup of the original
stderr is kept so real startup errors still reach you.

**Teardown is defensive.** SIGINT/SIGTERM exit cleanly; SIGSEGV/SIGBUS/SIGABRT
restore the terminal and re-raise, so a crash never leaves a wedged terminal.

## Benchmark

`tools/kittybench.c` measures sustained kitty-graphics throughput across
platform resolutions, to see where a terminal tops out:

```sh
make kittybench && ./kittybench 400
```

## Limitations

- macOS only (CoreAudio).
- Software-rendered cores only. Cores requesting a GL/Vulkan context
  (N64, Dreamcast, hardware-renderer PSX) are not supported.
- Player 1 only; no gamepad input.
- Core options use their built-in defaults; there is no options UI.
