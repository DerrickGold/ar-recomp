# Manual — running, configuring, and tuning the game

Everything the player-facing side of ActRaiser Recomp exposes: how to launch it,
every real config key, the full settings overlay, controls and hotkeys, the save
editor, the scene inspector, cheats, and asset replacement.

If you just want to play, you don't need this file — the
[quick start](../README.md#quick-start) is three steps and the in-game settings
overlay (`Esc`/`F1`) explains every row as you select it. This is the reference
for when you want to know exactly what a control does.

**Contents**

- [Launching the game](#launching-the-game)
- [Configuration files](#configuration-files)
- [Controls](#controls)
- [Host hotkeys](#host-hotkeys)
- [The settings overlay](#the-settings-overlay)
- [Display and scaling](#display-and-scaling)
- [Audio](#audio)
- [Save editor](#save-editor)
- [Scene inspector](#scene-inspector)
- [Cheats](#cheats)
- [Enhancements](#enhancements)
- [Level warp](#level-warp)
- [Asset replacement (HD art & music)](#asset-replacement-hd-art--music)

## Launching the game

A bundle-built game is launched by its `run-game` script (`run-game.command` on
macOS, `run-game.bat` on Windows, `run-game.sh` on Linux) and needs no
arguments. A source build is launched directly:

```sh
./build-release/ActRaiserRecomp ar.sfc --config config.ini        # normal play
./build-release/ActRaiserRecomp ar.sfc --config dev-config.ini    # cheats + debug flags on
```

(The `play` preset builds into `build-release/`; the `dev` preset builds into
`build/`.)

`--config <file>` **replaces** the default `config.ini` load entirely (the file
you pass carries its own `[Graphics]`/`[Sound]` sections too), so
`dev-config.ini` and `nocheats-config.ini` are complete, self-contained configs,
not overlays.

## Configuration files

### What's actually wired up in `config.ini`

Only what's listed below is read by `config.c`. **`config.ini`'s `[KeyMap]` and
`[GamepadMap]` sections, and the `Autosave`/`DisableFrameDelay`/`SkipLauncher`/
`EnableSnes9xOracle`/`WindowSize` keys, are currently placeholders — none of
them are parsed or have any effect.** `LinearFiltering`, `NoSpriteLimits`, and
`AudioChannels` are parsed compatibility leftovers without runtime consumers.
Input binding is done from the settings overlay, not from this file.

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

These legacy names are staged directly into the same descriptor registry used
by the menu and `settings.ini`; the config parser keeps no state of its own, and
runtime video and audio code reads only `g_settings`.

### Persistent user settings (`settings.ini`)

`settings.ini` is loaded automatically after the selected `config.ini`. It is
menu/user-owned and uses stable descriptor keys such as `window_scale`,
`extended_aspect`, `pixel_aspect`, `audio_enabled`, `audio_master_volume`,
`menu_scale_percent`, and `ws_sprites`. The atomic writer emits every registry
row without rewriting the developer-authored `config.ini`.

Resolution order is:

```text
built-in defaults < config.ini < settings.ini < real environment < live changes
```

Known `AR_*` settings inside `config.ini` are staged at the config tier rather
than disguised as real environment variables. Diagnostic-only `AR_*` and
`SNESREF_*` keys retain the old environment bridge. This means a command-line
environment value still reliably overrides both files.

## Controls

Every joypad button is re-bindable from **Settings → Controls**, for the
keyboard and for a gamepad independently. Both binding sets are always stored;
the *Input device* row decides which one feeds the game (*Auto* keeps both
live), and *Configure bindings for* switches which set the rows below show.
To rebind, select a row, press Return (or SNES B), then press the key or
button you want; `A` (SNES Y) restores that row's default and Escape cancels.
Bindings live in `settings.ini` as `bind_key_*` / `bind_pad_*` and are saved
the moment they change.

Keyboard bindings are stored by **physical key position** (SDL scancode), so a
layout change moves with the keys rather than the letters. The defaults are:

| Key(s) | Function |
|---|---|
| Arrow keys | D-pad |
| `Z` | primary action button (SNES B) |
| `X`, `A`, `S` | SNES A, Y, X |
| `Q`, `W` | SNES L, R |
| Return, Right Shift | Start, Select |

### Gamepad

The gamepad defaults follow the standard SNES-on-Xbox-layout mapping: South =
B, East = A, West = Y, North = X, shoulders = L/R, Menu/View = Start/Select,
D-pad = D-pad. The left analog stick doubles as the D-pad (*Left stick as
D-Pad*, on by default, with an adjustable deadzone); the right stick and
triggers are reserved for the camera.

Set the *Gamepad* row to pick between several connected controllers; it names
each one, and *First connected* follows hotplug. If SDL does not recognise a
pad, drop a `gamecontrollerdb.txt` next to the executable (or in `assets/`)
and it is loaded at startup.

### Camera control

Camera control (diorama and 3D sim town, Free Cam only) is on the pad as well
as the mouse. The right stick orbits (yaw/pitch), the triggers zoom in/out, and
clicking the right stick recentres the camera; all seven are re-bindable, and
*Camera sensitivity*, *Camera stick deadzone*, and *Invert camera Y* tune the
feel. Stick input is integrated over real elapsed time, so orbit speed does not
change with frame rate. Outside those two modes the camera bindings do nothing.
The mouse path is right-drag orbit, wheel zoom, middle-click reset.

### Host actions on the pad

Six host actions are gamepad-bindable too, because a Steam Deck has no
keyboard: open settings menu (default L3), reset camera (default R3), pause,
fast forward, save state, and load state. The pad also drives the settings menu
itself — using your own bindings, so menu confirm is whatever you bound to SNES
B — and that includes rebinding, so the whole feature is reachable with no
keyboard attached. The keyboard hotkeys below stay hard-wired on purpose: a bad
rebind can never lock a desktop player out of the menu.

### Steam Deck

Launched through Steam, Steam Input presents a standard pad and everything
works with the defaults. Launched from desktop mode, SDL's HIDAPI Steam driver
is enabled at startup so the built-in sticks, D-pad, and face buttons are
picked up directly. Either way, L3 opens the settings menu.

## Host hotkeys

Keyboard only, not re-bindable:

| Key(s) | Function |
|---|---|
| `Esc` / `F1` | open the host settings overlay from any game state; press again to close |
| `P` | pause |
| `T` | turbo — fast-forward at 8 game frames per rendered frame (`AR_TURBO_MULT` to change) |
| `F5` / `F7` | save / load state |
| `F6` | level warp (see [Level warp](#level-warp)) |
| `F2` or `C` | full diagnostic snapshot (WRAM/VRAM/CGRAM/OAM/high-OAM + screenshot). `C` is the one-hand alias and repeats while held — one snapshot per key repeat — for sweeping a glitch that only lasts a frame. Each is ~21 MB, so a long hold writes GBs; on a replay prefer `AR_SHOT_EVERY` / `AR_VRAMDUMP_GF` to capture exact frames instead |
| `F3` | toggle scene inspector; left-click pauses/inspects, drag its panel to move it, right-click clears/resumes |
| `F9` | cycle 4:3 authentic → widescreen raw → widescreen full (requires `ExtendedAspectRatio`; paused BG/crop changes redraw immediately, sprite/activation changes apply next game frame) |
| `Shift`+`F9` | dump diagnostic state |
| `-` / `+` | decrease/increase the promoted widescreen HUD by 0.25×; the first adjustment starts from the current game presentation scale |

## The settings overlay

The host settings overlay is available from every game state. Its left-hand
navigation column lists eight sections — Video, Diorama, Town 3D, Audio,
Controls, Cheats, Save, System — each of which holds one or more tabs. (A ninth
developer section, Layers, appears only with *Show debug settings* on.)

It opens with focus on the primary left-hand navigation: Up/Down selects a
category or direct action and `Z` (SNES B) or Return enters/runs it. Inside a
category, Up/Down selects a row, Left/Right changes ordinary values, and `Z` or
Return advances/toggles ordinary values, opens direct text editing for custom
values, or runs the selected command. `X` (SNES A) returns from a category to
primary navigation; from primary navigation it closes the overlay. During text
entry, Backspace edits, Return validates/applies, and Escape cancels. `A`
restores a setting's default.

Every tab also ends with `Reset <section> defaults`; pressing it twice restores
every tab in that top-level section, including hidden developer controls,
without changing other sections. For example, resetting Town 3D restores Scene,
Camera, Light, and Weather together.

Escape or F1 closes the menu from either focus. F2 remains available for a full
snapshot while the overlay is open. Game-frame advancement and SNES input are
frozen until it closes; accepted setting changes are atomically written to
`settings.ini`. ACTION rows themselves are not persisted.

Restart Game and Exit Desktop live at the end of System → Tools. Both flush
`settings.ini` and battery SRAM through the normal shutdown path; restart then
replaces the current process with the same executable and command line.

**Show debug settings** (System → Tools) reveals the developer-only rows: the
diorama and Town 3D numeric tuning dials, their per-layer and per-stage A/B
toggles, the granular widescreen flags, and the scene inspector. With it off,
the menu keeps to the master toggles and the major on/off effects.

### The overlay's artwork

The overlay decodes ActRaiser's 256-tile 2bpp dialog font and its native
Sky Palace dialog frame directly from the user-supplied ROM at startup.
Alphabetic and numeric tiles therefore retain the game's real artwork and
baked outline/shadow treatment. The frame is assembled as three independent
category/settings/help boxes with transparent gutters over the paused game.
Host-authored fallback glyphs and frames remain available if the expected ROM
assets cannot be decoded. No ROM-derived graphics data is committed or sampled
from scene-dependent VRAM.

## Display and scaling

Screen ratio is a normal three-choice row — 4:3, 16:9, and 16:10 — not a text
field. Screen ratio, pixel aspect, render profile, renderer path, render scale,
window mode, stretching, HUD/menu scale, and widescreen policy changes apply
live.

*Render scale* is the internal render/upscale multiple of the SNES output
(1–8, default 3). Higher values render more actual detail in the 3D town and
Mode 7 paths and downsample into the window. *Refresh rate* is Vsync, Unlimited
(vsync off, for lower input-to-photon latency), or Limit with a chosen target
FPS.

`AR_MENU_SCALE=0` (the default, displayed as **Auto**) chooses the largest
quarter-step content scale that preserves the settings layout in the complete
window. The overlay always covers the renderer's real output resolution and
aspect ratio rather than inheriting the game's presentation viewport. `100`
makes one font-art pixel one host-output pixel; values through `800` enlarge
the font, selector, spacing, and panels independently from both the game
framebuffer and `AR_HUD_SCALE`.

In widescreen-full mode the action/simulation HUD is composited as a host
overlay after the game framebuffer is upscaled. `AR_HUD_SCALE=100` makes one
SNES HUD pixel one output pixel vertically, while `AR_HUD_SCALE=0` (the
default, displayed as **Match game**) preserves the normal game-sized HUD.
Values are percentages from 25 through 400 and can also use an `x` suffix,
such as `AR_HUD_SCALE=2.5x`. Authentic 4:3 and widescreen-raw remain untouched
comparison paths and keep the HUD inside the SNES framebuffer.

## Audio

Audio frequency is a three-choice 32.04/44.1/48 kHz row. It and audio buffer
size retain the restart marker because they require reopening SDL's audio
device. The actual opened device rate feeds a continuous resampling boundary;
SPC, enhanced OGG, and MSU-1 sources therefore retain the same pitch and tempo
at every preset and callback-buffer size.

Audio controls are live descriptor-backed settings. They can be set in the
config's `AR_*` bridge or changed through the settings overlay:

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
[`settings-system.md`](settings-system.md), "Audio control seams".

Custom music (OGG streaming in place of SPC songs) is covered in
[Asset replacement](#asset-replacement-hd-art--music) below.

## Save editor

The **Save editor** category stages battery-save changes without treating the
unknown town-map payload as disposable structured data. Use **Edit section** to
switch between Progress, Status, Magic, Items, and Scores. The active section is
repeated in the panel title and separated from global save controls and
commands. The editable set includes all six town states, Death Heim and
Professional-mode unlock state, player name, Master/Angel level/health/SP/MP/
lives, message speed, magic and item slots, equipped magic, and both act scores
for every town.

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
`tools/srm.py` provides `check`, `decode`, `diff`, `edit`, and cross-format
`convert` commands for the same format outside the game.

## Scene inspector

A developer tool: enable it with `F3`, its Inspector-submenu toggle, or
`AR_SCENE_INSPECTOR=1` (the Inspector tab itself is hidden unless *Show debug
settings* is on).

Left-click anywhere in the game viewport to freeze the current frame. A clean,
color-coded monospace panel reports game mode/submode, camera and PPU state, the
BG tile(s) and OAM sprite(s) under the pointer, tile/frame numbers,
palette/priority/flips, and VRAM tilemap/character addresses. The console gets
the full report, including a manifest gate/draft and tile hashes compatible with
`AR_TILE_CENSUS`.

The compact panel fits its natural width to the longest visible report line and
initially opens opposite the selected point and can be moved by left-dragging
its title strip. Dragging the cyan lower-right grip uniformly scales the frame,
font, and spacing so the report can reveal more of the scene without truncating
columns. Clicks elsewhere in the report body pass through to scene selection.
Right-click, F3, or P clears the marker and releases the pause created by the
inspector; an existing manual pause is preserved.

`screen` and `mode7` manifest planes are live today; the inspector identifies
hash-keyed `tiles` assets but labels that replacement plane as reserved until
the N-x renderer path is implemented.

When SDL logical rendering is active, its mouse events already arrive in logical
coordinates and are mapped directly through the physical presentation viewport.
The no-logical-size path converts window coordinates to renderer output first.
PAR/letterboxing, widescreen cropping, and the independently scaled/anchored
promoted HUD are then resolved. HUD clicks are mapped back through the same
presentation chunks used to render them, so the marker, highlighted source
tile/sprite, and pointer stay aligned.

**Dump scene assets** writes a frame-unique `scene_assets_*` directory beneath
the current run folder (`runs/latest/` points at it). It exports each complete
resident BG tilemap canvas as a transparent PNG, all 128 composed OAM entries as
a 16×8 sprite sheet, and a second OBJ sheet containing both name bases across
all eight palettes. That OBJ atlas includes animation cels loaded in VRAM even
when they are not the frame currently drawn. It also writes the complete CGRAM
palette sheet, raw VRAM/CGRAM/OAM/WRAM, and `metadata.json` with PPU registers,
layer bases/dimensions, and an index for every OAM slot. These are decoded from
resident PPU data, not cropped from the visible framebuffer.

## Cheats

`[Cheats]` section — registry-backed `AR_*` keys are staged as the config layer;
diagnostic-only keys are exported through the compatibility environment bridge.

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
| `AR_MAGIC_CYCLE=1` | legacy seed for the **Cheats > Cycle magic spell** toggle. Arms the rebindable *Cycle magic spell* control (Settings > Controls; default keyboard `M`, no pad default) which steps through the spells the save has UNLOCKED during an action stage and reloads each one's resident OBJ tiles. No longer reserves a SNES button, so `AR_MAGIC_CYCLE_BTN` is gone. A cheat badge is drawn while it is armed. |
| `AR_RANGED_SWORD=1` | sword fires a projectile |
| `AR_INF_MP=1` (or `=<n>`) | infinite magic scrolls (pins the working count; never written to the save file) |
| `AR_INF_SP=1` | sim mode: infinite SP (miracle points), self-calibrating to your max |
| `AR_ANGEL_HP=1` | sim mode: infinite angel health, self-calibrating to your max |
| `AR_PIN=<8-hex-PAR>[,...]` | generic Pro Action Replay code pinner (e.g. `7E00210A`); catalogue in `codes.txt` / [`ram-map.md`](ram-map.md) |
| `AR_WARP=<region_hex><map_hex>` | sets the raw `$18:$19` target used by `F6` (default `0101`); use the verified table below |
| `AR_WARP_AT=<gameframe>` | fires the `AR_WARP` target automatically once the initialized game-frame counter reaches the value (the power-on `$5555` sentinel is ignored; headless runs can't press F6); same state caveats as F6 |
| `AR_TURBO_MULT=<n>` | game frames per rendered frame while `T` turbo is on (default 8) |

## Enhancements

Overlay section **System → Game**. Background in [`SEAMS.md`](SEAMS.md) town §7.

| Key | Effect |
|---|---|
| `AR_FIX_BRIDGE_LIMIT=1` | bridges stop counting toward a town's 128-structure population cap: completed bridges migrate to a validated sidecar in spare battery-save space while keeping their map mark, rendered metatile, river crossing, and 32-person support. Retroactive on existing towns; persisted only by the game's normal save transaction and sticky once saved. Replaces the withdrawn v1 slot-reuse/lightning designs, which broke town redraws |
| `AR_BRIDGEFIX_DEBUG=1` | `[bridgefix]` log from the structure-system hooks: migrations/cleanup, bridge allocations, table-full events, and sidecar mark/render passes; `=2` also logs every structure allocation |
| `AR_TURBO_MULT=<n>` | turbo multiplier, 2–64 (default 8) |

## Level warp

### Verified `AR_WARP` targets

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
| Northwall | `0601` | `0605` | `0608` selects the boss map but is **not** a valid standalone visual baseline: from a non-action warp it leaves patterned/garbage CHR and self-exits. Reach the boss naturally from `0605` for acceptance. |
| Death Heim | `0701` | — | Boss-rush hub; verified end-to-end through every rematch and the final boss (2026-07-14) |

Set the target before launch, enter a transition-capable state, then press
`F6`. An action-to-action warp is not a naturally observed transition and may
inherit timing/object state; reproduce suspicious gameplay behavior through
natural progression before classifying it as a game or widescreen regression.

Everything else in `dev-config.ini`'s `[Debug]` section is diagnostic
instrumentation for active bug-hunting, documented in
[`DEBUG.md`](../DEBUG.md) — not gameplay-relevant, off by default, and safe to
ignore unless you're debugging.

## Asset replacement (HD art & music)

Both systems share one design: `game-assets/manifest.ini` is **tracked** and
ships every known replacement hook active, but an entry only engages when its
asset file exists at the path it names — the asset files themselves are
gitignored (bring your own). Drop a file with the matching name and it appears
on the next launch; no configuration editing needed. A missing asset is
silently inert (fully authentic rendering/audio), so a fresh clone behaves
exactly like the unmodified game.

In a downloaded bundle, the same folder is `utils/game-assets/`.

### HD graphics (`[replace:<name>]` entries, files under `game-assets/hd/`)

Substitutes HD art for individual graphics via a declared plane: `screen`
(screen-locked elements like the title logo — any resolution, scales to the
output viewport) or `mode7` (canvas-space art rendered through the live Mode-7
matrix, so rotation/zoom/HDMA warps apply to the HD art). The title
logo/medallion ships as the worked example, engaged by dropping
`game-assets/hd/title-logo.png`. Toggled live by `hd_replacements` /
`AR_HD_REPLACEMENTS`. Full plane/key/gate reference: the manifest header and
[`rendering-engine.md`](rendering-engine.md) §13.

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
ids, mix peaks). Full key reference: the manifest header and
[`SEAMS.md`](SEAMS.md) "Audio".

Note the licensing angle before sharing packs: files ripped from the original
game (its soundtrack, its art) are copyrighted content and belong in the
gitignored asset directories only — see
[what can and can't be committed](contributing.md#what-can-and-cant-be-committed-here).
