# ActRaiser Recomp

A native, static-recompilation port of **ActRaiser (SNES, USA release)** to modern
desktop platforms.

![title screen](/assets/title.png)

## What is ActRaiser?

*ActRaiser* is a 1990 Quintet-developed, Enix-published SNES game that alternates
between side-scrolling action-platformer stages and a top-down "god game" town
simulation. This project targets the verified USA cartridge dump:

| | |
|---|---|
| Internal title | `ACTRAISER-USA` |
| Size | 1,048,576 bytes (1MB, no copier header) |
| Internal checksum | `0x83DB` |
| SHA-256 | `b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0` |
| SHA-1 | `e8365852cc20178d42c93cd188a7ae9af45369d7` |
| CRC32 | `0xEAC3358D` |

**The ROM is not included and never will be.** You must supply your own legally
obtained `ar.sfc` dump matching the checksums above, placed at the repo root. See
[What can (and can't) be committed here](#what-can-and-cant-be-committed-here).

## What is this project?

This is a **static recompilation ("recomp")**, not a decompilation ("decomp") —
the distinction matters:

- A **decompilation** is a hand-written, from-scratch reimplementation: someone
  reads the original binary (or its disassembly), understands *what* it does, and
  writes new, original source code that reproduces that behavior. The result is
  new expression of the same functionality.
- A **static recompilation** mechanically translates the ROM's actual 65816
  machine code into equivalent C, one function at a time, via an automated tool
  ([`snesrecomp-go`](snesrecomp-go/README.md), the bundled Go reimplementation
  of the historical SNES static recompiler). The output is a direct, literal translation
  of the original binary's logic — not new authorship. That generated code is
  copyrighted-ROM-derived and is **never committed to this repo** (see below);
  it's regenerated locally by everyone who builds this project, from their own
  ROM.

Layered on top of the recompiled game logic is a hand-written runtime — SDL3
windowing/input, a PPU/APU (video/audio) reimplementation, save-state handling,
and a growing set of "HLE" (high-level emulation) shims that replace timing- or
hardware-dependent ROM routines with equivalent native code. That runtime layer
*is* original engineering and *is* what's tracked in this repo.

### Why recomp instead of just running it in an emulator?

An emulator interprets or JIT-compiles the original ROM on the fly, forever
depending on an emulation core. A static recomp instead produces a real, native,
standalone executable — no core, no interpretation loop, no per-instruction
overhead — that can be profiled, debugged, and modernized (widescreen, higher
resolutions, better performance, native ports) like any other codebase, while
still requiring the end user to legally own the original game. That's the
long-term preservation case: as SNES hardware and even software emulators age
out, a native recompilation is far more portable and maintainable than either.

### Current status

**Actively in development — expect bugs.**
**[`docs/progress.md`](docs/progress.md) is the authoritative, kept-current
status tracker** — per-action-stage / per-sim-town playability tables plus
automated codebase metrics; update it whenever a stage/town is actually played.
Snapshot as of 2026-07-12:

- **Action stages, regions 1-6**: every ordinary action level has been played
  through and is fully playable. Widescreen BG streaming, sprites, activation,
  enemies/platforms, bosses, fast vertical traversal, and the observed
  HDMA/parallax effects all render and behave correctly.
- **Remaining action widescreen polish**: map and apply presentation-aware
  camera/world-edge clamping so the ends of finite backgrounds cannot scroll
  into the wider viewport.
- **Death Heim (region 7 / `70X`)**: the complete boss rush, final boss, and
  post-boss sky transition are playable and widescreen-validated.
- **Simulation mode**: Fillmore has one confirmed clean end-to-end town round;
  Bloodpool has partial entry/lightning coverage. The remaining towns and full
  simulation-mode widescreen behavior are TBD.
- Open bugs and investigation state live in [`DEBUG.md`](DEBUG.md).


Extended runtime features:

* Widescreen support (all action content validated; ordinary-stage camera-edge
  polish and simulation-mode BG/sprite validation remain)
* Global host settings overlay with live configuration, persistent user
  settings, and mode-independent cheat staging
  ([architecture and implementation](docs/settings-system.md))
* Click-driven scene inspector for locating live BG/OBJ/Mode-7 graphics,
  deriving asset-replacement manifest entries, and exporting complete resident
  BG/OAM/OBJ/palette data sets as PNG sheets

#### Screenshots

![ActRaiser's native in-game menu](/assets/menu.png)
![Mode 7 rendering](/assets/mode7.png)
![Bloodpool Act 2 in widescreen](/assets/bloodpool-act2.png)
![Fillmore simulation mode in widescreen](/assets/sim.png)
![Independently scaled simulation-mode HUD](/assets/hud-scaling.png)
![Host settings overlay using ActRaiser's native dialog font and frame graphics](/assets/overlay.png)

## AI disclosure

**This project is being built with substantial help from AI coding assistants**
(Claude / Claude Code). Large portions of the recompiler tooling, the runtime
code, the debugging infrastructure, and this documentation were written by AI
under my direction and review, not typed by me line-by-line.

This project is not intended to be the following:

* A showcase of technical skill or understanding
* Any form of creative expression (it's a literal translation of already written code from one platform to another)
* A pathway to fame or fortune

The entire goals of this project are:

* Another form of game preservation
* I can play the game sooner


If you're evaluating this codebase's provenance or reviewing a contribution, assume AI
involvement throughout unless told otherwise.

## Project layout

```
ActRaiserRecomp/
├── ar.sfc                  # (you supply this — gitignored)
├── config.ini               # default runtime config (player-facing)
├── dev-config.ini           # development config: debug flags + cheats enabled
├── nocheats-config.ini      # like dev-config.ini, cheats off
├── CMakeLists.txt            # developer build (CMake presets: dev/play/asan/trace)
├── snesbuild.ini             # game build manifest for the hermetic/bundled path
├── Makefile                  # `make release` (all platform bundles), `make clean`
├── DEBUG.md                  # ★ the debugging guide — every tool, every known
│                                bug class, and the full bug-hunt journal.
│                                Start here if something's broken.
├── docs/
│   ├── SEAMS.md               # ★ logic↔hardware boundary map + reverse-
│   │                             engineered game architecture (object systems,
│   │                             dispatch tables, subsystem roles) — the
│   │                             groundwork for a future full decompilation.
│   ├── research-symbol-map.md  # manually curated address → candidate semantic
│   │                             name index, with confidence/promotion status
│   ├── ram-map.md             # WRAM address reference
│   ├── rom-map.md             # ROM data-region reference
│   ├── rendering-engine.md    # rendering/streaming/OAM architecture
│   ├── widescreen-survey.md   # widescreen evidence + implementation record
│   ├── settings-system.md     # live settings + overlay architecture/record
│   ├── BUILD_TOOLING.md       # cross-platform driver + binary-bundle roadmap
│   └── progress.md            # ★ per-action-level / per-sim-mode-town /
│                                 major-functionality status tracker
├── recomp/
│   ├── bank*.cfg              # hand-authored per-bank recompiler directives:
│   │                             function addresses, entry m/x width pins,
│   │                             indirect-dispatch tables, HLE hooks. This is
│   │                             the actual authored "source" that drives
│   │                             regeneration — safe to commit (see below).
│   └── funcs.h                # generated from the *.cfg files — NOT committed
├── game-assets/
│   ├── manifest.ini           # ★ tracked asset-replacement manifest: every
│   │                             known HD-art + music hook, engaged by
│   │                             dropping the asset file it names
│   ├── hd/                    # your HD art (*.png) — gitignored
│   └── audio/                 # your music (*.ogg) — gitignored
├── src/
│   ├── main.c                 # SDL3 entry point, input, frame loop, config
│   ├── actraiser_rtl.c        # game-specific HLE/runtime glue + cheats
│   ├── actraiser_spc_player.c # SPC/audio upload handling
│   ├── hd_replacements.c      # HD-art manifest parsing + per-frame gates
│   ├── music_replacements.c   # music manifest + OGG loop streamer + triggers
│   ├── settings.c / settings_overlay.c  # live settings registry + host menu
│   ├── config.c               # .ini parsing
│   └── gen/                   # ★ regenerated C output — NOT committed (you
│                                 produce this locally via snesbuild regen)
├── third_party/
│   └── stb/                   # vendored single-file libs (stb_image,
│                                 stb_vorbis) — tracked, no install step
├── snesrecomp-go/             # standalone concurrent Go recomp toolchain
│   ├── docs/                  # per-project integration/config/runtime guides
│   ├── packaging/             # builds the self-contained per-platform bundles
│   └── runtime/               # bundled C runtime + SNES hardware model
├── tools/
│   ├── regen.sh                # compatibility launcher for Go snesbuild
│   ├── rom_info.py, lzss_decompress.py, ... — game/trace analysis tools
│   ├── compatibility launchers        # not used by build/regen; use cmd/v2regen
│   └── oracle/                 # differential-testing harness vs. real snes9x
├── tests/                      # golden-image + replay regression tests
├── release/                    # produced distribution bundles — NOT committed
└── saves/                      # runtime output only (dumps, snapshots,
                                   replays) — NOT committed, purely local
```

`DEBUG.md`, `docs/SEAMS.md`, `docs/research-symbol-map.md`, and
`docs/progress.md` are the documents worth reading before diving into the code:
`DEBUG.md` tells you *how to diagnose* a problem (which tool, which env var,
which known gotcha), `SEAMS.md` tells you *what the game's internal architecture
actually is* (object systems, dispatch
tables, subsystem boundaries) as reverse-engineered so far, and `progress.md`
tells you *what actually works today* (playability per stage/town + codebase
metrics). `docs/settings-system.md` records the architecture and implementation
of the live settings registry, persistent user settings, and host-side overlay
UI.

## Build instructions

There are two ways to build, aimed at different people:

- **Players — a prebuilt one-click bundle** (no developer tools at all). See
  [Prebuilt bundles](#prebuilt-one-click-bundles-for-players) just below.
- **Developers — from a source checkout** (CMake presets, or the hermetic
  no-install path). See [Building from source](#building-from-source).

### Prebuilt one-click bundles (for players)

The distribution bundles are the zero-setup path: a player downloads one
archive for their platform, drops in their own ROM, and runs one script — no
CMake, compiler, SDL, or Go required, and no repository checkout. Each bundle
carries the whole buildable project plus a pinned C toolchain (Zig) and, on
macOS/Windows, SDL3; the game's C is generated locally from the player's ROM
(never shipped), and the folder exposes only a `README.txt` and a `run-build`
script, with everything else tucked under `utils/`. Running `run-build` once
builds the game and writes a `run-game` script; the player opens `run-game` to
play, every time, with no rebuild.

To **produce** all six bundles from a source checkout (into `release/`, named
`actraiser-recomp-<platform>.{tar.xz,zip}` with SHA-256 sidecars):

```sh
make release
```

Go and CMake are the only host requirements — the C toolchain and SDL3 are
downloaded and bundled automatically. Full details, layout, and the current
signing/CI gaps are in [`docs/BUILD_TOOLING.md`](docs/BUILD_TOOLING.md).

### Building from source

#### Dependencies

- **Go 1.24+** — builds the recompiler/driver; required for both build paths
- **CMake** ≥ 3.16 — for the developer CMake presets (not needed for `--hermetic`)
- **A C11 compiler** (clang or gcc) — likewise (the hermetic path uses its own
  bundled Zig instead)
- **SDL3** (development package/headers) — the only external library this links
  against; auto-discovered by both build paths, and bundled outright in the
  distribution package (so end users need nothing)
- **git**

Python is optional for unrelated forensic/triage scripts; it is not a build,
regeneration, runtime, or opcode-validation dependency.

To reuse the bundled toolchain from another game project, start with
[`snesrecomp-go/README.md`](snesrecomp-go/README.md) and its
[`project integration guide`](snesrecomp-go/docs/PROJECT_INTEGRATION.md).
The native project-driver design, the hermetic build, and the self-contained
distribution bundles are documented in
[`docs/BUILD_TOOLING.md`](docs/BUILD_TOOLING.md).

The `brew`/`apt` lines below install the CMake-preset build's dependencies. The
hermetic path drops the CMake and C-compiler requirements (it uses its own
bundled Zig), so it needs only Go and SDL3 development files — and the
distribution bundle removes even the SDL3 requirement by carrying it inside.

**macOS** (verified — this is the only platform actually built/tested so far):

```sh
brew install cmake sdl3 go
```

**Linux** (Debian/Ubuntu — same CMake setup, untested by this project but
should work; the build has no OS-specific code beyond standard SDL3/POSIX):

```sh
sudo apt install build-essential cmake libsdl3-dev golang-go
```

**Windows**: the bundled runtime has MSVC-oriented support (see
`snesrecomp-go/runtime/runner.cmake`), but this specific project
hasn't been built on Windows yet — no `.vcxproj`/CI verifying it works here.
If you get it building, a PR documenting the steps would help.

#### Steps

1. **Clone this repo.** The Go recompiler and C runtime are included; there is
   no secondary toolchain checkout or symlink to create.

2. **Supply your own ROM.** Place a verified `ar.sfc` (see the checksums above)
   at the repo root.

3. **Regenerate the recompiled banks.** The cross-platform `snesbuild` driver
   runs the recompiler over your ROM, refreshes `src/gen/*.c`, `recomp/funcs.h`,
   metadata, RTS-web census, and the hard-stub report. From a source checkout:

   ```sh
   go -C snesrecomp-go run ./cmd/snesbuild regen \
     --root .. --rom ar.sfc --allow-stubs
   ```

   A downloaded `snesbuild`/`snesbuild.exe` can run the same operation directly
   without Go or Bash. `bash tools/regen.sh` remains a compatibility command.
   The inherited hard-stub backlog currently makes strict regeneration exit
   nonzero after writing complete output; see `DEBUG.md` §8.

4. **Compile the game.** Two options:

   - **Hermetic (no CMake/compiler/SDL install)** — compiles with a pinned Zig
     toolchain that `snesbuild` downloads on first use, discovering SDL3
     automatically:

     ```sh
     go -C snesrecomp-go run ./cmd/snesbuild toolchain fetch   # one time
     go -C snesrecomp-go run ./cmd/snesbuild build --hermetic --root ..
     ```

   - **CMake presets (the classic developer build)** — needs CMake, a C11
     compiler, and SDL3 development files:

     ```sh
     cmake --preset play && cmake --build --preset play
     # or, through the driver: go -C snesrecomp-go run ./cmd/snesbuild build --root ..
     ```

   Steps 2–4 are exactly what the player-facing `run-build` bundle script
   automates; producing those bundles is described under
   [Prebuilt bundles](#prebuilt-one-click-bundles-for-players) above.

## Running the game

```sh
./build/ActRaiserRecomp ar.sfc --config config.ini        # normal play
./build/ActRaiserRecomp ar.sfc --config dev-config.ini    # cheats + debug flags on
```

`--config <file>` **replaces** the default `config.ini` load entirely (the file
you pass carries its own `[Graphics]`/`[Sound]` sections too), so
`dev-config.ini` and `nocheats-config.ini` are complete, self-contained configs,
not overlays.

### What's actually wired up in the config file

Only what's listed below is read by `config.c`. **`config.ini`'s `[KeyMap]` and
`[GamepadMap]` sections, and the `Autosave`/`DisableFrameDelay`/`SkipLauncher`/
`EnableSnes9xOracle`/`WindowSize` keys, are currently placeholders — none of
them are parsed or have any effect.** `LinearFiltering`, `NoSpriteLimits`, and
`AudioChannels` are parsed compatibility leftovers without runtime consumers.
Gamepad input isn't implemented at all yet.

**Real config keys** (`[Graphics]`/`[Sound]`):

| Key | Effect |
|---|---|
| `WindowScale` | integer window scale factor |
| `Fullscreen` | desktop-fullscreen window mode; live changes are supported by the settings registry |
| `NewRenderer` | use the newer rendering path; live, though widescreen always forces it on |
| `ExtendedAspectRatio` | live screen ratio: `4:3`/legacy `off`, `16:9`, or `16:10` |
| `AspectPAR` | live pixel shape: `4:3` for SNES pixel-aspect correction or `square` |
| `IgnoreAspectRatio` | disable logical-size aspect correction and stretch to the window |
| `EnableAudio`, `AudioFreq`, `AudioSamples` | audio output settings; enable/disable is live, frequency cycles through `32040`/`44100`/`48000` Hz, and format changes apply on restart |

These legacy names are staged into the same descriptor registry used by the
menu and `settings.ini`; `g_config` is no longer consulted by runtime video or
audio code.

### Persistent user settings

`settings.ini` is loaded automatically after the selected `config.ini`. It is
menu/user-owned and uses stable descriptor keys such as `window_scale`,
`extended_aspect`, `pixel_aspect`, `audio_enabled`,
`audio_master_volume`, `menu_scale_percent`, and `ws_sprites`. The implemented atomic writer emits
every registry row without rewriting the developer-authored `config.ini`.

Resolution order is:

```text
built-in defaults < config.ini < settings.ini < real environment < live changes
```

Known `AR_*` settings inside `config.ini` are staged at the config tier rather
than disguised as real environment variables. Diagnostic-only `AR_*` and
`SNESREF_*` keys retain the old environment bridge. This means a command-line
environment value still reliably overrides both files.

**Keyboard controls are hardcoded** (not configurable via `.ini` yet) —
see `HandleInput()` in `src/main.c`:

| Key(s) | Function |
|---|---|
| Arrow keys | D-pad |
| `Z` | primary action button (SNES B) |
| `X`, `A`, `S` | SNES A, Y, X |
| `Q`, `W` | SNES L, R |
| Return, Right Shift | Start, Select |
| `Esc` / `F1` | open the host settings overlay from any game state; press again to close |
| `P` | pause |
| `T` | turbo — fast-forward at 8 game frames per rendered frame (`AR_TURBO_MULT` to change) |
| `F5` / `F7` | save / load state |
| `F6` | level warp (see `AR_WARP` below) |
| `F2` | full diagnostic snapshot (WRAM/VRAM/CGRAM/OAM + screenshot) |
| `F3` | toggle scene inspector; left-click pauses/inspects, drag its panel to move it, right-click clears/resumes |
| `F9` | cycle 4:3 authentic → widescreen raw → widescreen full (requires `ExtendedAspectRatio`; paused BG/crop changes redraw immediately, sprite/activation changes apply next game frame) |
| `Shift`+`F9` | dump diagnostic state |
| `-` / `+` | decrease/increase the promoted widescreen HUD by 0.25×; the first adjustment starts from the current game presentation scale |

The host settings overlay is available from every game state. It opens with
focus on the primary left-hand navigation: Up/Down selects a category or
direct action and `Z` (SNES B) or Return enters/runs it. Inside a category,
Up/Down selects a row, Left/Right changes ordinary values, and `Z` or Return
advances/toggles ordinary values, opens direct text editing for custom values,
or runs the selected command. `X` (SNES A) returns from a category to primary
navigation; from primary navigation it closes the overlay. During text entry,
Backspace edits, Return validates/applies, and Escape cancels. `A` restores a
setting's default; Escape or F1 closes the menu from either focus. F2 remains available for
a full snapshot while the overlay is open. Game-frame advancement and SNES
input are frozen until it closes; accepted setting changes are atomically
written to `settings.ini`. ACTION rows themselves are not persisted.

Display includes screen and pixel aspect so output geometry lives in one place.
Extras contains the bridge-limit enhancement plus turbo, pause, and snapshot
controls. Warp and quick-state commands remain developer hotkeys and are not
shown there. Inspector is a real submenu: its first row clearly shows/enables
the click inspector, its second row dumps the complete resident scene assets,
and the read-only area below shows the scene name, `$18/$19`, game/host frames,
pause/turbo state, camera/map dimensions, PPU mode/masks, and current music.
Restart Game and Exit Desktop remain direct primary-navigation leaves; pressing
B on either runs it immediately.
Restart and exit flush `settings.ini` and battery SRAM through the normal
shutdown path; restart then replaces the current process with the same
executable and command line.

### Save editor

The **Save editor** category stages battery-save changes without treating the
unknown town-map payload as disposable structured data. Use **Edit section** to
switch between Progress, Status, Magic, Items, and Scores. The active section is
repeated in the panel title and separated from global save controls and commands.
The editable set
includes all six town states, Death Heim and Professional-mode unlock state,
player name, Master/Angel level/health/SP/MP/lives, message speed, magic and
item slots, equipped magic, and both act scores for every town.

**Allow save edits** is an explicit safety switch, not an edit by itself. Leave
it Off while browsing; turn it On only when you are ready to apply changes.
With it Off, both Apply actions and next-boot staged overrides are refused and
cannot change live or stored SRAM. With it On, an explicit Apply works now;
staged values also become session-only boot overrides on the next launch.
Rows default to **Leave as-is**, so only values deliberately selected on any
page are written.

Then run one of these actions:

- **Apply for session** updates the live 8 KiB SRAM image but deliberately does
  not write disk. It remains session-only even if Restart/Exit is used; use it
  only when the current game flow can naturally return to the title screen and
  choose Continue.
- **Apply and save** first creates a timestamped backup (when Auto-backup is
  enabled), atomically writes the active backend, and updates live SRAM. This
  is the practical menu-testing path: run it, choose the top-level **Restart
  Game** action, then Continue.
- **Export native SRAM** and **Export structured INI** write
  `saves/export.srm` and `saves/export.ini` without changing the active save.
- **Import save** reads `saves/import.srm`, falls back to `saves/import.ini`
  (or uses `AR_SAVE_IMPORT=<path>`), fully validates it, backs up the active
  save, and converts it into the active backend without silently changing
  backend selection.

The storage-format row selects the authoritative `saves/save.srm` or lossless
`saves/save.ini` backend after restart. INI files retain all 8192 raw bytes in
128 required chunks and add readable verified region fields; unknown terrain,
town state, fill history, and the ending marker are therefore preserved.
Malformed/truncated files never partially replace live SRAM, and every editor
mutation recomputes the game's ADD/XOR checksum. The field map was reconciled
against the USA-region adjustment in the
[game-tools-collection ActRaiser editor](https://github.com/RyudoSynbios/game-tools-collection/tree/master/src/lib/templates/actraiser/saveEditor)
and then checked against this project's WRAM map and save fixtures.
`tools/srm.py` provides
`check`, `decode`, `diff`, `edit`, and cross-format `convert` commands for the
same format outside the game.

With the scene inspector enabled (`F3`, its Inspector-submenu toggle, or
`AR_SCENE_INSPECTOR=1`), left-click anywhere in the game viewport to freeze the
current frame. A clean, color-coded monospace panel reports game mode/submode,
camera and PPU
state, the BG tile(s) and OAM sprite(s) under the pointer, tile/frame numbers,
palette/priority/flips, and VRAM tilemap/character addresses. The console gets
the full report, including a manifest gate/draft and tile hashes compatible
with `AR_TILE_CENSUS`. The compact panel fits its natural width to the longest
visible report line and initially opens opposite the selected
point and can be moved by left-dragging its title strip. Dragging the cyan
lower-right grip uniformly scales the frame, font, and spacing so the report
can reveal more of the scene without truncating columns. Clicks elsewhere in
the report body pass through to scene selection. Right-click, F3, or P
clears the marker and releases the pause created by the inspector; an existing
manual pause is preserved. `screen` and `mode7`
manifest planes are live today; the inspector identifies hash-keyed `tiles`
assets but labels that replacement plane as reserved until the N-x renderer
path is implemented. When SDL logical rendering is active, its mouse events
already arrive in logical coordinates and are mapped directly through the
physical presentation viewport. The no-logical-size path converts window
coordinates to renderer output first. PAR/letterboxing, widescreen cropping,
and the independently scaled/anchored promoted HUD are then resolved. HUD
clicks are mapped back through the same presentation chunks used to render
them, so the marker, highlighted source tile/sprite, and pointer stay aligned.

**Dump scene assets** writes a frame-unique `scene_assets_*` directory beneath
the current run folder (`runs/latest/` points at it). It exports each complete
resident BG tilemap canvas as a transparent PNG, all 128 composed OAM entries as
a 16×8 sprite sheet, and a second OBJ sheet containing both name bases across
all eight palettes. That OBJ atlas includes animation cels loaded in VRAM even
when they are not the frame currently drawn. It also writes the complete CGRAM
palette sheet, raw VRAM/CGRAM/OAM/WRAM, and `metadata.json` with PPU registers,
layer bases/dimensions, and an index for every OAM slot. These are decoded from
resident PPU data, not cropped from the visible framebuffer.

Screen ratio is a normal three-choice row—4:3, 16:9, and 16:10—not a text
field. Screen ratio, pixel aspect, render profile, renderer path, window scale,
fullscreen, stretching, HUD/menu scale, and widescreen policy changes apply
live. Audio frequency is a three-choice 32.04/44.1/48 kHz row. It and audio
buffer size retain the restart marker because they require reopening SDL's
audio device. The actual opened device rate feeds a continuous resampling
boundary; SPC, enhanced OGG, and MSU-1 sources therefore retain the same pitch
and tempo at every preset and callback-buffer size.

`AR_MENU_SCALE=0` (the default, displayed as **Auto**) chooses the largest
quarter-step content scale that preserves the settings layout in the complete
window. The overlay always covers the renderer's real output resolution and
aspect ratio rather than inheriting the game's presentation viewport. `100`
makes one font-art pixel one host-output pixel; values through `800` enlarge
the font, selector, spacing, and panels independently from both the game
framebuffer and `AR_HUD_SCALE`.

The overlay decodes ActRaiser's 256-tile 2bpp dialog font and its native
Sky Palace dialog frame directly from the user-supplied ROM at startup.
Alphabetic and numeric tiles therefore retain the game's real artwork and
baked outline/shadow treatment. The frame is assembled as three independent
category/settings/help boxes with transparent gutters over the paused game.
Host-authored fallback glyphs and frames remain available if the expected ROM
assets cannot be decoded. No ROM-derived graphics data is committed or sampled
from scene-dependent VRAM.

In widescreen-full mode the action/simulation HUD is composited as a host
overlay after the game framebuffer is upscaled. `AR_HUD_SCALE=100` makes one
SNES HUD pixel one output pixel vertically, while `AR_HUD_SCALE=0` (the
default, displayed as **Match game**) preserves the normal game-sized HUD.
Values are percentages from 25 through 400 and can also use an `x` suffix,
such as `AR_HUD_SCALE=2.5x`. Authentic 4:3 and widescreen-raw remain untouched
comparison paths and keep the HUD inside the SNES framebuffer.

**Audio controls** are also live descriptor-backed settings. They can be set in
the config's `AR_*` bridge or changed through the settings overlay:

| Key | Effect |
|---|---|
| `AR_AUDIO_VOLUME=<0..100>` | master output volume (default 100); scales the final music/SFX/MSU-1 mix |
| `AR_DIALOG_BLIP=0` | mutes only the per-character Sky Palace dialogue sound; other uses of the same sound/event ID remain active |
| `AR_MUSIC_REPLACEMENTS=0` | disables enhanced manifest-driven music replacement (default on, inert without audio files); toggling live immediately hands the current song between OGG and the authentic SPC sequencer |

For an automated live probe without the overlay, use for example
`AR_SETTING_SET=audio_master_volume=25`; the scheduled settings mechanism
applies it through the same registry callback the menu uses.
Independent music and SFX levels are not exposed yet because the SPC/DSP mix
must first be separated or its voice ownership proven; see
`docs/settings-system.md`, “Audio control seams”.

Custom music (OGG streaming in place of SPC songs) is covered in
[Asset replacement](#asset-replacement-hd-art--music) below.

**Cheats** (`[Cheats]` section — registry-backed `AR_*` keys are staged as the
config layer; diagnostic-only keys are exported through the compatibility
environment bridge):

Every registry-backed cheat can be changed from the settings overlay in any
game mode. Mode-specific cheats are saved and staged immediately, then begin
applying when their action or simulation engine becomes active.

| Key | Effect |
|---|---|
| `AR_INF_HP=1` | pins player HP — infinite health |
| `AR_FREEZE_TIMER=1` | freezes the action-stage countdown timer. **Currently still buggy** — has an auto-release heuristic for the boss-defeat drain sequence that isn't fully reliable yet |
| `AR_MOONJUMP=1` | enable moonjump; hold the normal jump button to fly upward (`AR_MOONJUMP_SPEED`, default 6 px/frame) |
| `AR_NO_KNOCKBACK=1` | permanent invincibility — no damage, no hitstun. Magic-aware: invulnerability drops only for the 1-2 frames where a spell cast actually fires |
| `AR_ALL_MAGIC=1` | unlocks all four spells in the equip menu |
| `AR_MAGIC_CYCLE=1` | reserves SNES L (`Q`) during action mode to cycle Fire → Stardust → Aura → Light and reload each spell's resident OBJ tiles; `AR_MAGIC_CYCLE_BTN=<mask>` changes the button (`0x0020`=L, `0x0010`=R) |
| `AR_RANGED_SWORD=1` | sword fires a projectile |
| `AR_INF_MP=1` (or `=<n>`) | infinite magic scrolls (pins the working count; never written to the save file) |
| `AR_INF_SP=1` | sim mode: infinite SP (miracle points), self-calibrating to your max |
| `AR_ANGEL_HP=1` | sim mode: infinite angel health, self-calibrating to your max |
| `AR_PIN=<8-hex-PAR>[,...]` | generic Pro Action Replay code pinner (e.g. `7E00210A`); catalogue in `codes.txt` / `docs/ram-map.md` |
| `AR_WARP=<region_hex><map_hex>` | sets the raw `$18:$19` target used by `F6` (default `0101`); use the verified table below |
| `AR_WARP_AT=<gameframe>` | fires the `AR_WARP` target automatically once the game-frame counter reaches the value (headless runs can't press F6); same state caveats as F6 |
| `AR_TURBO_MULT=<n>` | game frames per rendered frame while `T` turbo is on (default 8) |

**Bridge-limit enhancement** (overlay category **Extras**; background in
`docs/SEAMS.md` town §7):

| Key | Effect |
|---|---|
| `AR_FIX_BRIDGE_LIMIT=1` | bridges stop counting toward a town's 128-structure population cap: completed bridges migrate to a validated sidecar in spare battery-save space while keeping their map mark, rendered metatile, river crossing, and 32-person support. Retroactive on existing towns; persisted only by the game's normal save transaction and sticky once saved. Replaces the withdrawn v1 slot-reuse/lightning designs, which broke town redraws |
| `AR_BRIDGEFIX_DEBUG=1` | `[bridgefix]` log from the structure-system hooks: migrations/cleanup, bridge allocations, table-full events, and sidecar mark/render passes; `=2` also logs every structure allocation |

#### Verified `AR_WARP` targets

The low byte is the game's raw map/sub-flow value, **not a uniform act number**.
For example, `0302` is not Kasandora Act 2; it loads invalid/garbage state, while
Kasandora Act 2 begins at `0303`.

| Region | Act 1 entry | Act 2 entry | Notes |
|---|---:|---:|---|
| Fillmore | `0101` | `0102` | |
| Bloodpool | `0201` | `0202` | |
| Kasandora | `0301` | `0303` | Do not use `0302` as an Act 2 shortcut |
| Aitos | `0401` | `0404` | |
| Marahna | `0501` | `0504` | |
| Northwall | `0601` | `0605` | `0608` directly enters the Act 2 boss arena for focused testing |
| Death Heim | `0701` | — | Boss-rush hub; verified end-to-end through every rematch and the final boss (2026-07-14) |

Set the target before launch, enter a transition-capable state, then press
`F6`. An action-to-action warp is not a naturally observed transition and may
inherit timing/object state; reproduce suspicious gameplay behavior through
natural progression before classifying it as a game or widescreen regression.

Everything else in `dev-config.ini`'s `[Debug]` section is diagnostic
instrumentation for active bug-hunting, documented in `DEBUG.md` — not
gameplay-relevant, off by default, and safe to ignore unless you're debugging.

## Asset replacement (HD art & music)

Both systems share one design: `game-assets/manifest.ini` is **tracked** and
ships every known replacement hook active, but an entry only engages when its
asset file exists at the path it names — the asset files themselves are
gitignored (bring your own). Drop a file with the matching name and it appears
on the next launch; no configuration editing needed. A missing asset is
silently inert (fully authentic rendering/audio), so a fresh clone behaves
exactly like the unmodified game.

### HD graphics (`[replace:<name>]` entries, files under `game-assets/hd/`)

Substitutes HD art for individual graphics via a declared plane: `screen`
(screen-locked elements like the title logo — any resolution, scales to the
output viewport) or `mode7` (canvas-space art rendered through the live Mode-7
matrix, so rotation/zoom/HDMA warps apply to the HD art). The title
logo/medallion ships as the worked example, engaged by dropping
`game-assets/hd/title-logo.png`. Toggled live by `hd_replacements` /
`AR_HD_REPLACEMENTS`. Full plane/key/gate reference: the manifest header and
`docs/rendering-engine.md` §13.

### Music (`[music:<name>]` entries, files under `game-assets/audio/`)

Streams OGG Vorbis files in place of the SPC driver's songs — sound effects
stay authentic (the SPC driver keeps running; only its per-song instrument
voices are muted at the DSP). All 17 songs of the ROM's song table ship as
inert manifest entries (`audio/title.ogg` is the title theme). Adding a track
needs no configuration: whenever a song starts without audio, the console
prints exactly what to provide, e.g.

```
[music] src=18:947F song=01 authentic — drop game-assets/audio/song-00.ogg to replace ([music:song-00])
```

so one normal play session identifies every track — rename the `song-NN`
entry/file in the manifest as you recognize each one. Loops are
sample-accurate: set `LOOPSTART`/`LOOPLENGTH` Vorbis comment tags in the file
(the RPG Maker convention, so existing tagging tools work), or
`loop_start`/`loop_end` keys in the manifest entry; untagged files loop whole.
Per-entry `when =` gates (same grammar as the HD art entries) select
game-state-dependent variants of the same song — first matching entry wins,
an ungated entry is the fallback. Toggled live by `music_replacements` /
`AR_MUSIC_REPLACEMENTS`; `AR_MUSICLOG=1` adds verbose tracing (uploads, event
ids, mix peaks). Full key reference: the manifest header and `docs/SEAMS.md`
“Audio”.

Note the licensing angle before sharing packs: files ripped from the original
game (its soundtrack, its art) are copyrighted content and belong in the
gitignored asset directories only — see the next section.

## What can (and can't) be committed here

This repo mixes original engineering (safe to commit) with content mechanically
derived from the copyrighted ActRaiser ROM (must never be committed). The
`.gitignore` enforces this, but the reasoning is worth understanding if you're
contributing:

**Never commit:**
- The ROM itself (`*.sfc`/`*.smc`), any save file derived from it (`*.srm`),
  or any raw memory/WRAM/SRAM dump captured while running it (`saves/`,
  `*.bin`) — these can contain decompressed copyrighted assets (graphics,
  text, audio) that were resident in memory at capture time.
- **`src/gen/*.c` and `recomp/funcs.h`** — the actual recompiled C output.
  This is a direct, literal translation of the copyrighted ROM's machine code.
  It's regenerated locally by `snesbuild regen` from your own ROM; there is
  nothing to commit here, ever.
- Audio/video recordings captured from a running instance (`*.wav`, replay
  recordings) — same reasoning as memory dumps, but for audio/video instead.
- Asset-pack files under `game-assets/` (`hd/*.png`, `audio/*.ogg`) — a rip of
  the original soundtrack or art is copyrighted content even after
  re-encoding, and even a fully original fan arrangement is yours, not this
  repo's, to distribute. The tracked `manifest.ini` (our own hook metadata) is
  the only file that belongs in git there.
- Trace/log files from debugging sessions (`*.jsonl`, `*.trace`, `*.log`,
  `*.cdl`) — these encode per-frame memory-state traces from actual gameplay.
- Prebuilt third-party binaries (e.g. the `snes9x_libretro.dylib` reference
  core used by the oracle test harness) — vendoring compiled binaries of
  someone else's project is both bad practice and, for GPL-licensed code like
  snes9x, a license-compliance problem on top of it. Fetch your own copy per
  `tools/oracle/README.md`.

**Safe to commit:**
- `recomp/*.cfg` — these are our own hand-written addresses, directives, and
  commentary that *tell* the recompiler what to do. They don't contain any
  translated ROM logic themselves, similar to how a symbol map or linker
  script isn't itself a copy of a binary.
- All hand-written project runtime/tooling source (`src/*.c` outside
  `src/gen/`, everything under `tools/`) — original engineering, not
  ROM-derived. `snesrecomp-go/` also contains no ROM-derived game data, but it
  has separate upstream provenance and licensing status documented in
  `snesrecomp-go/ATTRIBUTION.md`.
- `docs/`, `DEBUG.md` — our own analysis and documentation. Short illustrative
  disassembly snippets in service of explaining architecture are fine and
  normal for this kind of documentation; wholesale reproduction of ROM data
  tables is not, and none currently exists in these docs.

If you're ever unsure whether something is safe to commit, default to **not**
committing it and ask first — a `.gitignore` fix is trivial; scrubbing
something out of published git history is not.

## License

This repo's original source (runtime, tooling, `recomp/*.cfg`, docs) is
[MIT-licensed](LICENSE). That license explicitly does **not** cover the
ActRaiser ROM or anything derived from it — see the LICENSE file's Scope
section, and "What can (and can't) be committed here" above.

The bundled `snesrecomp-go/` is a Go reimplementation of the historical Python
recompiler and includes its copied C runtime and adapted documentation. That
source repository had not declared an overall license at the snapshot used for
the port. The module is therefore explicitly excluded from this repository's
MIT grant; see `snesrecomp-go/ATTRIBUTION.md`, its runtime provenance README,
and the LICENSE Scope section.
