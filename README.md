# rom

Play ROMs directly in your terminal.

`rom` is a small libretro frontend for macOS and Linux. It renders native
pixels with the kitty graphics protocol, supports real key-release events,
save states, audio, fast-forward, and live terminal-theme recoloring.

![Metroid Fusion running inline in the terminal with hue recoloring](docs/screenshots/metroid-fusion-hue.png)

`rom` does not include games, BIOS files, or emulator cores. Only run software
you are legally entitled to use.

## Quick start

You need [Ghostty](https://ghostty.org/) or
[kitty](https://sw.kovidgoyal.net/kitty/) and a C compiler.

On macOS:

```sh
xcode-select --install
```

On Ubuntu or Debian:

```sh
sudo apt install build-essential libasound2-dev
```

Then:

```sh
git clone https://github.com/jhickner/rom
cd rom
make
./rom "path/to/game.sfc"
```

The first time you open a ROM for a platform you have no core for, `rom` offers
to fetch and build that core.

Ghostty or kitty has to be the terminal underneath, but it can be running tmux
— see [tmux](#tmux) for the one setting that needs.

Optional installation:

```sh
make install PREFIX="$HOME/.local"
```

## Cores

`rom` uses libretro cores: `.dylib` on macOS and `.so` on Linux.

Open a ROM without its core and `rom` asks before doing anything:

```
rom: no core installed for roms/Link's Awakening.gb

  gambatte_libretro.dylib can be built from https://github.com/libretro/gambatte-libretro
  and installed into /Users/you/.config/rom/cores.
  Needs git, make, and a C/C++ toolchain, plus a few minutes.

Fetch and build it now? [Y/n]
```

Say yes and it shallow-clones the repository into `~/.config/rom/src/`, builds
it, installs the core, and starts the game. Say no and it prints the filename
and repository so you can build it yourself. Runs without a terminal on stdin
skip the prompt and print the same instructions.

Cores are loaded from `~/.config/rom/cores/`, wherever you run `rom` from. Use
`--core <path>` to select one directly.

Cores you build yourself in this repository's `cores/` are copied there by
`make install`, or on their own with `make install-cores`.

| ROM | Core |
|---|---|
| `.sfc` `.smc` `.fig` | `snes9x_libretro` |
| `.nes` | `fceumm_libretro` |
| `.gb` `.gbc` | `gambatte_libretro` |
| `.gba` | `mgba_libretro` |
| `.md` `.gen` `.smd` | `genesis_plus_gx_libretro` |
| `.pce` | `mednafen_pce_fast_libretro` |
| `.n64` `.z64` `.v64` | `mupen64plus_next_libretro` |
| `.nds` | `desmume_libretro` |
| `.wad` | `prboom_libretro` |
| `.wl6` `.wl1` `.sod` `.sdm` `.n3d` | `ecwolf_libretro` |

Cores that render with OpenGL are supported on macOS: `rom` creates an
offscreen 4.1 core-profile context through CGL (Metal-backed on Apple
silicon), lets the core draw into an FBO, and reads each frame back into the
normal video path. Nothing is ever presented to a window.

N64 defaults to Mupen64Plus-Next's Angrylion renderer rather than its GL one.
GLideN64 corrupts its texture cache and aborts a few seconds into a game on
macOS/arm64, and at terminal resolutions the software renderer looks the same.
Set `mupen64plus-rdp-plugin = gliden64` under `[core.n64]` to try it anyway.

DS games arrive as both screens stacked, 256x384. DeSmuME boots them with its
own HLE BIOS, so no DS BIOS or firmware files are needed. Its other screen
layouts are core options: `desmume_screens_layout = top` under `[core.nds]`
drops the bottom screen.

The touchscreen takes the mouse: click and drag on the bottom screen to touch
it. Where the terminal reports pixel coordinates a tap lands on the pixel it
was aimed at; where it reports cells — inside tmux, which speaks nothing else —
it lands within half a cell, so raise `scale` for finer work. Reporting is only
turned on for a DS game, and while it is on the terminal's own click-drag
selection belongs to `rom`. Hold Shift to get it back for a drag, or turn it
off:

```ini
[options]
mouse = false
```

The touchscreen is also on the keyboard: `i` `j` `k` `l` walk a cursor over the
bottom screen and Space taps it. Rebind them in `[pad]`, and set
`stylus_speed` for how many pixels a frame the cursor moves.

DeSmuME exposes no save RAM, so DS battery saves are the core's own: it writes
`~/.config/rom/<game>.dsv` rather than using `~/.config/rom/saves/`. Save
states are unaffected.

This fork of prboom plays Heretic and Hexen as well as Doom, detecting which
from the IWAD's lump table. Verified working: `doom.wad`, `doom1.wad`,
`doom2.wad`, `doom2f.wad`, `doomu.wad`, `tnt.wad`, `plutonia.wad`,
`heretic.wad`, `heretic1.wad`, `hexen.wad`, `hexendemo.wad`.

Not working: Strife (`strife0.wad`, `strife1.wad`) hangs the core, and
`hexdd.wad` and `voices.wad` crash it - the first is a Hexen expansion and the
second is Strife's speech pack, neither of which is a standalone game. Chex
Quest (`chex.wad`, `chex3.wad`) ships as PWADs, which are patches that need a
base IWAD loaded underneath; rom passes one file to the core, so they are
rejected.

Fetching the core also installs `prboom.wad`, the core's own data file, into
`~/.config/rom/`.

Wolfenstein takes any of the game's data files, e.g. `VSWAP.WL1`; ECWolf finds
its siblings in the same directory and carries its `ecwolf.pk3` internally.

## Usage

```sh
./rom [options] <rom>
./rom --resume
```

| Option | Description |
|---|---|
| `--resume` | Pick from the games you have been playing, at the newest save state |
| `--core <path>` | Use a specific core |
| `--fullscreen` | Fill the terminal window |
| `--scale <n>` | Integer zoom in inline mode, 1–8 (default 2); change live with `[` / `]` |
| `--slot <n>` | Initial save-state slot, 0–9 |
| `--no-audio` | Disable audio |
| `--recolor <mode>` | `off`, `hue`, `nearest`, `duotone`, `tint`, or `dither` |
| `--recolor-strength <0..1>` | Blend recoloring with the original; for `tint`, how faint |
| `--keys` | Print current key bindings |
| `--selftest <n>` | Run `n` frames without a terminal |
| `--shot <file>` | Save the final self-test frame as BMP |
| `--force` | Skip terminal graphics detection |

Inline mode is the default. It leaves scrollback intact and removes its Kitty
image when the game exits.

### Resuming

`rom --resume` lists the last 40 games you played, newest first, with when you
last played and which save-state slots are filled:

```
Resume  ↑/↓ move · enter play · q cancel

    1  Super Mario World      snes     2h ago      state 0
    2  Links Awakening        gb       yesterday   state 0,3
    3  doom2                  doom     4d ago
```

Arrows or `j`/`k` move, Enter plays, `1`–`9` jump straight to a game, `q`
cancels. Games are matched by absolute path, so entries survive a change of
directory; anything that has been moved or deleted drops off the list.

Resuming means resuming: the most recently written save state is loaded and
becomes the live slot, so F2 keeps saving where you already were, and the game
returns at the volume and scale you left it. Add `--slot <n>` to resume a
particular slot instead, and launch by path rather than `--resume` to start from
the battery save alone. A state the core rejects — one written by a different
core version, say — is reported and skipped rather than being fatal.

### Controls

| Key | Action |
|---|---|
| Arrows | D-pad |
| `z` / `x` | B / A |
| `a` / `s` | Y / X |
| `q` / `w` | L / R |
| Enter / Right Shift | Start / Select |
| Click / drag | DS stylus |
| `i` `j` `k` `l` | DS stylus cursor |
| Space | DS stylus tap |
| Ctrl+C or Ctrl+Q | Quit |
| `p` | Pause |
| F2 / F3 | Save / load state |
| F6 / F7 | Previous / next slot |
| F5 | Reset |
| Tab | Fast-forward while held |
| F8 | Cycle recolor mode |
| `[` / `]` | Scale down / up (inline mode) |
| F9 | Toggle terminal scaling (see below) |
| F10 | Toggle damage updates (see below) |
| F4 | Outline each band as it updates (damage debugging) |
| `-` / `=` | Volume down / up |
| `m` | Mute |

Alt chords are left to tmux — see [tmux](#tmux).

## tmux

`rom` runs inside tmux. Enable passthrough once:

```sh
tmux set -g allow-passthrough all      # add to ~/.tmux.conf to keep it
```

Without it every graphics escape is swallowed, and `rom` says so rather than
drawing nothing.

The DS stylus needs a second setting:

```sh
tmux set -g mouse on
```

tmux only asks the terminal for mouse events while its own mouse mode is on,
and it forwards them to whichever pane has enabled reporting. It parses the
reports itself and speaks only cell coordinates, so a tap inside tmux lands
within half a cell of where it was aimed rather than on the pixel.

tmux cannot be shown an image at the cursor — it neither tracks nor redraws one,
so the picture would survive until the first redraw and then vanish. Instead the
frame is transmitted with no placement and given a *virtual* one, and the cells
it occupies are filled with placeholder characters carrying the image id in
their foreground color and their row and column in combining diacritics. Those
are ordinary text, so tmux moves, scrolls, and repaints them without
understanding any of it, and the terminal underneath substitutes pixels. Scaling
and resizing work as usual.

Input goes through the same trick. tmux itself has no notion of a key release,
but it does not have to: the keyboard protocol is enabled on the terminal
underneath by passing the escape through, and tmux hands the key reports it does
not recognize — releases and modifier-only keys among them — straight to the
pane. So key handling inside tmux is the same as outside it, Right Shift for
Select included.

Reading the theme is the one place where passthrough is *not* wanted for
everything. tmux never answers `OSC 4`, so the palette query has to be passed
through to the terminal. But tmux asks the terminal for its default foreground
and background itself, and swallows every `OSC 10`/`OSC 11` reply that comes
back — it cannot tell its own query from a pane's — so a passed-through query
for those is never answered. `rom` sends them plain instead and lets tmux answer from
the values it cached. When only the palette answers, the log line says
`bg #000000 (fallback)`, and the modes built around the background — `tint` and
`duotone` — will look wrong.

If the terminal does not answer the capability query, `rom` falls back to
inferring releases from auto-repeat: a key lifts once its repeats stop, so
holding a direction hiccups once at the start while the keyboard waits out its
delay-until-repeat.

There is one thing tmux does lose that way: its own bindings. The key reports
the terminal sends in this mode are not the ones tmux matches against, so
`Alt+Left` stops switching windows and arrives as a plain Left in the game.
`rom` hands those back — an Alt chord it has no hotkey for is dispatched to tmux
with `send-keys -K`, which looks it up in the client's key table exactly as if
it had been pressed there, so window switching, `Alt+1`, popups and the rest
keep working while a game is running. Releases still reach the game, so a
direction held when Alt is tapped does not stick.

```ini
[options]
tmux_alt_keys = false     # keep Alt chords in the game instead
```

The keyboard mode belongs to the terminal, which every pane shares, so `rom`
hands it back whenever another pane takes the focus and takes it again on
return. If it upsets your tmux bindings anyway, turn it off and take the
inferred releases instead:

```ini
[options]
tmux_keyboard = false
```

Expect a lower frame rate than a bare terminal: every frame is copied through
tmux on its way out.

## Scaling

By default the terminal stretches the image to its cell rect, so only one
native-resolution frame goes down the pipe no matter what zoom you pick. Set
`term_scale = false` to zoom locally instead, which keeps pixel art blocky
rather than letting the terminal's scaler smooth it — at the cost of
transmitting scale² more bytes per frame. Press F9 to compare while playing.

The write to the terminal dominates the render path, so the difference is
large: at 3x zoom local zooming sends 2020 KB/frame against 225 KB, and at 4x
it alone caps the render thread near 60fps.

## Damage updates

The picture can be split into horizontal bands, each its own image on its own
cell rect, with only the bands that changed since the last frame retransmitted.
Press F10 to toggle it while playing, or set it in the config:

```ini
[options]
damage = false
```

Toggling it never changes how the picture is drawn, only what is retransmitted.
How finely it splits depends on which scaling mode is in use.

With `term_scale = false` the picture is already at its final pixel size, so it
is padded out to the cell rect and each band placed at its own size. Nothing is
stretched or filtered, every cell boundary is a legal cut, and bands may be
uneven — 16 of them at any window size. This is where damage saves the most,
since sharp mode transmits scale² bytes: at 4x a GBA frame drops from 1.8 MB to
around 500 KB.

With `term_scale = true` the terminal stretches each band to its cell rect, and
a band only lands where the whole picture would have put it if it covers the
same pixels-per-cell. That limits the split to `gcd(cells, pixel rows)` equal
bands — 2 for a DS at 2x, often 1, in which case F10 does nothing.

A frame the window is too small to show at native size is downscaled and goes
out whole.

With `show_stats` on, `dmg` reports bands sent against bands offered.

F4 outlines each band in colour on the frame it is retransmitted, cycling
through four colours so neighbouring bands are distinguishable:

```ini
[options]
damage_debug = false
```

## GPU readback

Cores that render on the GPU (N64) have their frame read back off the GPU each
frame. By default that transfer is queued and collected on the next frame, so it
overlaps emulation instead of stalling it — roughly 3x cheaper at 640x480, more
at higher internal resolutions. The cost is one frame of input latency; set
`async_readback = false` to read synchronously instead. Software-rendered cores
are unaffected either way.

## Terminal colors

`rom` reads the terminal's ANSI palette, foreground, and background at
startup. Colors are matched perceptually in Oklab through a prebuilt lookup
table, so the result is stable and fast.

| Mode | Result |
|---|---|
| `hue` | Keeps shading while adopting theme hues |
| `nearest` | Maps directly to the terminal palette |
| `duotone` | Uses the background-to-foreground ramp |
| `tint` | Collapses everything to tones of the background color |
| `dither` | Mixes nearby palette colors to recover shades |

Start with `--recolor hue`. Press F8 to compare modes while playing — the
palette is read at startup either way, so F8 works even when recoloring starts
off.

`tint` ramps from the background colour itself up to a tone above it, holding
one hue throughout. Black lands exactly on the background, so unused areas of
the picture disappear into the terminal, and the brightest pixels rise to a
clearly lighter tone of the same colour.

How far it rises is a fixed step in perceptual lightness, not a fraction of the
distance to the foreground. A theme with a dim foreground would otherwise pull
the whole ramp down with it and leave every game looking nearly black.

It borrows only the background's *hue*, not its saturation: terminal backgrounds
are nearly neutral, and a literal reading of one would come out grey. A
background with no hue at all — pure black or grey — can only give greyscale.

For `tint`, `--recolor-strength` sets how much of that hue the ramp carries, and
leaves the lightness alone: 1.0 is a full wash of the background's colour, lower
values fade towards a plain tonal ramp, and no setting is darker than another.
For the other modes it blends against the original colours.

## Config and saves

The config is created at `~/.config/rom/config`. Edit it to change keys,
volume, scale, recoloring, or focus behavior.

```ini
[options]
scale            = 2
term_scale       = true
damage           = false
async_readback   = true
recolor          = hue
recolor_strength = 1.00
pause_on_unfocus = true
hide_on_blur     = false
mouse            = true

[options.gb]
scale = 4
```

Sections may be suffixed with `snes`, `nes`, `gb`, `gba`, `genesis`, `pce`,
`n64`, `nds`, `doom`, or `wolf3d` for system-specific overrides. Run
`rom --keys <rom>` to see the effective settings.

A `[core]` section passes libretro core options through untouched. The names
and values belong to the core, not to `rom`; anything you leave out keeps the
core's own default.

```ini
[core.n64]
mupen64plus-rdp-plugin = angrylion
```

Volume and scale changes are remembered separately for each ROM, so a game
comes back the size and loudness you left it — the configured values are just
the initial defaults. Passing `--scale` overrides what was remembered for that
run without replacing it.

| Path | Contents |
|---|---|
| `~/.config/rom/config` | Settings and bindings |
| `~/.config/rom/saves/` | Battery saves |
| `~/.config/rom/states/` | Save states |
| `~/.config/rom/games/` | Per-ROM volume and scale |
| `~/.config/rom/recent` | Games played, for `--resume` |
| `~/.config/rom/rom.log` | Frontend and core log |

Games pause when the terminal loses focus and resume when it returns. With
`hide_on_blur = true` the picture is erased from the pane while another pane
has the focus and drawn again on return. Saves are written atomically and SRAM
is autosaved every five seconds when changed.

## Limitations

- macOS (CoreAudio) or Linux (ALSA)
- Ghostty or kitty required, optionally with tmux on top
- In tmux, key releases need a terminal that speaks the kitty keyboard protocol
- GL cores on macOS only; Linux is software-rendered until an EGL backend lands
- No Vulkan cores
- Player 1 only, from the keyboard and mouse

Licensed under the [MIT License](LICENSE).
