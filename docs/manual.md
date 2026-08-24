# Manual — running, configuring, and tuning the game

This is the player and power-user reference for launching, configuration,
controls, settings, developer tools, cheats, and asset replacement. For a first
run, use the [quick start](../README.md#quick-start); the in-game settings
overlay (`Esc`/`F1`) also explains each selected row.

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
- [Manual page-turn geometry](#manual-page-turn-geometry)

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

### `config.ini`

`config.c` reads only the keys below. The `[KeyMap]` and `[GamepadMap]` sections
are placeholders; configure input from the settings overlay. These keys also
have no effect: `Autosave`, `DisableFrameDelay`, `SkipLauncher`,
`EnableSnes9xOracle`, and `WindowSize`. `LinearFiltering`, `NoSpriteLimits`, and
`AudioChannels` are parsed only for compatibility.

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

These legacy names feed the same settings registry as the menu and
`settings.ini`; runtime code reads the resolved `g_settings` values.

### Persistent user settings (`settings.ini`)

`settings.ini` loads after the selected `config.ini`. It contains user-owned
keys such as `window_scale`, `extended_aspect`, `pixel_aspect`, `audio_enabled`,
`audio_master_volume`, `menu_scale_percent`, and `ws_sprites`. Saving settings
rewrites this file atomically and leaves `config.ini` untouched.

Resolution order is:

```text
built-in defaults < config.ini < settings.ini < real environment < live changes
```

Known `AR_*` settings in `config.ini` use the config tier. Diagnostic-only
`AR_*` and `SNESREF_*` keys retain the environment bridge, so command-line
environment values override both files.

## Controls

**Settings → Controls** stores separate keyboard and gamepad bindings. *Input
device* selects which set feeds the game; *Auto* keeps both active. *Configure
bindings for* selects the set being edited. To rebind a row, press Return (or
SNES B), then press the new key or button. `A` (SNES Y) restores the default;
Escape cancels. Changes are saved immediately to `settings.ini` as
`bind_key_*` or `bind_pad_*`.

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

The defaults follow the standard SNES-on-Xbox layout: South/East/West/North map
to B/A/Y/X, shoulders to L/R, Menu/View to Start/Select, and D-pad to D-pad.
The left stick also acts as the D-pad by default, with an adjustable deadzone.
The right stick and triggers control the camera.

Set the *Gamepad* row to pick between several connected controllers; it names
each one, and *First connected* follows hotplug. If SDL does not recognise a
pad, drop a `gamecontrollerdb.txt` next to the executable (or in `assets/`)
and it is loaded at startup.

### Camera control

In diorama mode and 3D town Free Cam, the right stick orbits, the triggers zoom,
and R3 recentres. The mouse equivalents are right-drag, wheel, and middle-click.
All seven pad inputs are re-bindable; sensitivity, deadzone, and invert-Y are
configurable. Camera bindings do nothing outside these modes.

### Host actions on the pad

Seven host actions are gamepad-bindable: open settings (default L3), reset
camera (default R3), pause, fast forward, save state, load state, and compare
rendering. The comparison action also has a bindable keyboard row; it is
unbound by default on both devices. The pad also drives the settings menu using
your SNES bindings. Keyboard hotkeys for the other host actions remain fixed so
a bad rebind cannot lock a desktop player out of the menu.

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

The overlay is available from every game state. Its navigation column contains
Video, Diorama, Town 3D, Audio, Controls, Cheats, Save, and System. Enabling
*Show debug settings* adds the developer-only Layers section.

| Context | Controls |
|---|---|
| Navigation | Up/Down selects; `Z` (SNES B) or Return enters or runs |
| Settings | Up/Down selects; Left/Right changes; `Z` or Return toggles, edits, or runs |
| Text entry | Backspace edits; Return applies; Escape cancels |
| Defaults | `A` (SNES Y) restores the selected setting |
| Back/close | `X` (SNES A) returns to navigation, then closes; Escape or F1 closes anywhere |

Every tab also ends with `Reset <section> defaults`; pressing it twice restores
every tab in that top-level section, including hidden developer controls,
without changing other sections. For example, resetting Town 3D restores Scene,
Camera, Light, and Weather together.

F2 remains available for a full snapshot while the overlay is open. The game and
SNES input stay frozen until the overlay closes. Accepted setting changes are
written atomically to `settings.ini`; action rows are not persisted.

### Authentic/enhanced comparison control

Bind **Compare rendering** in **Settings → Controls**. A tap toggles between
the player's current enhanced presentation and an authentic view; a hold opens
an enhanced-priority comparison with the authentic view inset as
picture-in-picture. Releasing the hold returns to the tap-selected view.

The authentic view is the ROM's complete 256×224 PPU composition and native SPC
audio. It bypasses widescreen, diorama/town 3D, HD replacements, other enhanced
host graphics effects, enhanced music replacements, and the dialogue-blip mute.
Action stages render it as a second native-geometry PPU pass with independent
BG1/BG2 raster camera phases and native world-sprite framing; fixed HUD sprites
remain screen-relative. The scanout is armed while either comparison binding is
configured or a comparison view needs it, and otherwise remains dormant.
CRT is an independent output treatment rather than part of the enhanced game
renderer, so the player's current CRT configuration continues to apply in
authentic and side-by-side views. The side-by-side view keeps the enhanced
audio. Host master volume and output device settings still apply in every view.

This is a session-only presentation override: it does not edit any video or
audio setting, it carries between action and simulation modes, and it resets to
enhanced on every launch. Gameplay QoL settings, cheats, and all other
non-graphics/audio options continue to apply. The transition briefly freezes
gameplay and audio, using the same host-owned pause behavior as the settings
overlay. In widescreen action stages the authentic view has its own native
camera framing so the player stays tracked independently of the extended view.

Restart Game and Exit Desktop live at the end of System → Tools. Both flush
`settings.ini` and battery SRAM through the normal shutdown path; restart then
replaces the current process with the same executable and command line.

**Show debug settings** (System → Tools) reveals the developer-only rows: the
diorama and Town 3D numeric tuning dials, their per-layer and per-stage A/B
toggles, the granular widescreen flags, and the scene inspector. With it off,
the menu keeps to the master toggles and the major on/off effects.

The developer-only **Layers → BG Extents** tab authors action-stage background
policies. Select BG1 or BG2, then edit its role, source, edge strategy, motion,
or per-side caps. Each layer supports up to four non-overlapping row bands with
independent anchoring, row range, fill strategy, motion, and horizontal cap.
Open a band before editing it.

- **Ignore side bounds** and **Ignore vertical bounds** temporarily bypass the
  selected layer's Diorama guides. Canvas, finite-world, and edge-strategy limits
  still apply.
- **Apply draft** enables the live A/B. **Extent guides** draws BG1 in cyan and
  BG2 in orange. **Print draft** writes the resolved plan to the run log.
- `Y` restores the selected row's canonical value.
- Drafts start disabled, reset on room changes, and never modify
  `settings.ini` or `diorama-layers.ini`.

Mixed screen/world bands must remain disjoint throughout the room's camera
travel. Invalid edits report normal limit feedback and leave the canonical room
policy active.

Corrected action-wide modes automatically keep the requested view within finite
BG1 dimensions when it fits. The correction is disabled in 4:3, Wide Raw,
`AR_ACTION_BG_HLE=0`, and scenes without a finite playfield canvas. If an axis
cannot fit safely, it keeps the game's native bounds. `AR_WS_ACTION_CAMDBG=1`
logs the decision.

### The overlay's artwork

At startup, the overlay decodes ActRaiser's dialog font and Sky Palace frame
from the user-supplied ROM. Host-authored fallbacks are used if those assets
cannot be decoded. No ROM-derived graphics are committed.

## Display and scaling

Screen ratio offers 4:3, 16:9, and 16:10. Ratio, pixel aspect, render profile,
renderer path, render scale, window mode, stretching, HUD/menu scale, and
widescreen policy changes apply live.

*Render scale* is the internal render/upscale multiple of the SNES output
(1–8, default 3). Higher values render more actual detail in the 3D town and
Mode 7 paths and downsample into the window. *Refresh rate* is Vsync (timed by
the SDL renderer), Uncapped (vsync off with a soft 2× nominal-display-refresh
cap), Limit with a chosen target FPS, or Unlimited (vsync and host presentation
throttling off). The optional *FPS counter* reports completed host presents in
the top-right; use it with Unlimited to measure maximum rendering throughput
rather than emulation ticks. Presentation cadence is independent of the optional frame-interpolation
effect, so disabling interpolation does not force Vsync, Uncapped, Limit, or Unlimited
back to the game's native tick rate. In 3D action mode, interpolation estimates
motion between consecutive captured layer images and generates intermediate
pixel frames; it does not alter the game's 60 Hz logic or require reconstructed
SNES object state. The dependent *Interpolation source test* row normally stays
at **Native 60 Hz**. **Test 30 -> 60 Hz** is a diagnostic slow-motion mode: in
3D action stages it deliberately advances game logic at 30 Hz while the host
continues presenting at the selected refresh rate, making one generated
midpoint visible between every source pair on a 60 Hz Vsync display. It is not
a gameplay mode and does not attempt to request an unsupported half-rate Vsync
from SDL.

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
| `AR_MUSIC_VOLUME=<0..100>` | music volume (default 100); scales authentic SPC songs and enhanced OGG music without changing SFX |
| `AR_SFX_VOLUME=<0..100>` | sound-effects volume (default 100); scales native event and ordinary effects without changing music |
| `AR_EXTENDED_AUDIO_CHANNELS=1` | enables the restart-class ten-voice native mode; song voices 6/7 remain active while the two original effect tracks render through virtual voices 8/9 |
| `AR_DIALOG_BLIP=0` | mutes only the per-character Sky Palace dialogue sound; other uses of the same sound/event ID remain active |
| `AR_MUSIC_REPLACEMENTS=0` | disables enhanced manifest-driven music replacement (default on, inert without audio files); toggling live immediately hands the current song between OGG and the authentic SPC sequencer |

For an automated live probe without the overlay, use for example
`AR_SETTING_SET=audio_master_volume=25`; the scheduled settings mechanism
applies it through the same registry callback the menu uses.

Music/SFX separation follows the SPC driver's logical track provenance, not
sample number: song tracks `$00-$0E` feed Music and effect tracks `$10/$12`
feed SFX before DSP summation. Both controls preserve sequencing and the shared
hardware echo; an existing echo tail decays naturally after a live change. At
100%/100% the native DSP uses its legacy integer mix path. Extended sound
channels retain that same classification and mixer:
the original SPC700 effect sequencer still runs, but its `$10/$12` DSP writes
are bridged to voices 8/9 and the three proven ownership branches no longer
discard song-track updates for voices 6/7. This first extension prevents music
voice stealing; it intentionally retains the original one-instance effect
lanes until the later queued/polyphonic phase. See
[`settings-system.md`](settings-system.md), "Audio control seams".

Custom music (OGG streaming in place of SPC songs) is covered in
[Asset replacement](#asset-replacement-hd-art--music) below.

## Save editor

The **Save editor** stages changes without discarding unknown town-map data.
**Edit section** switches among Progress, Status, Magic, Items, and Scores. You
can edit town states, unlocks, player name, Master and Angel stats, message
speed, magic, items, and act scores.

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

Left-click the game viewport to freeze and inspect a frame. The panel reports
game mode, camera and PPU state, BG tiles and OAM sprites under the pointer,
palette and priority data, and VRAM addresses. The console receives the full
report, including manifest gates and `AR_TILE_CENSUS`-compatible hashes.

Drag the title strip to move the panel or the cyan lower-right grip to resize it.
Clicks in the report body pass through to scene selection. Right-click, F3, or P
clears the marker and releases an inspector-created pause; a manual pause is
preserved.

`screen` and `mode7` manifest planes are live today; the inspector identifies
hash-keyed `tiles` assets but labels that replacement plane as reserved until
the N-x renderer path is implemented.

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
For example, `0302` is not Kasandora Act 2. It is a valid room only after the
natural `0301` transition supplies its setup; using it as a standalone warp
loads invalid/garbage state. Kasandora Act 2 begins at `0303`.

| Region | Act 1 entry | Act 2 entry | Notes |
|---|---:|---:|---|
| Fillmore | `0101` | `0102` | |
| Bloodpool | `0201` | `0202` | |
| Kasandora | `0301` | `0303` | `0302` is a natural-transition room, not a standalone warp or Act 2 shortcut |
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

Both systems use the tracked `game-assets/manifest.ini`. A replacement activates
only when its named file exists; asset files are gitignored. Missing files are
inert, so a fresh clone keeps the authentic graphics and audio.

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

## Manual page-turn geometry

The in-game manual uses painter-ordered geometry: SDL provides no depth test or
backface culling for the turning sheet. The bow is therefore constrained by two
invariants tested by the geometry suite: its depth is never negative, and depth
is monotonically non-decreasing from the hinge (`u = 0`) to the free edge
(`u = 1`). The renderer must emit the mesh column-major so later triangles are
also nearer triangles when the bowed sheet overlaps itself on screen.

For hinge angle `a` and normalized span `u`, the depth term is

```text
z(u) = sin(a) * [u/2 + A*sin(pi*u)*cos(a)]
```

The worst case approaches the hinge when `cos(a) = -1`. Since
`sin(pi*u) ~ pi*u` there, non-negative and monotonic depth require
`A <= 0.5/pi`. `kManualCurlLimitPermille` is consequently
`floor(1000 * 0.5/pi) = 159`; it is a derived bound, not a tuning value.
`kManualCurlPermille` remains at 70 for comfortable headroom. A freely rolling
curl would violate monotonic depth and would require silhouette-aware mesh
splitting rather than the current fixed draw order.
