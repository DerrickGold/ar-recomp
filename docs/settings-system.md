# ActRaiser Recomp — Settings & Live-Config System

Architecture and implementation record for the single source of truth that
captures every custom setting (cheats,
widescreen knobs, display/audio, QoL) and defines each one's **path to live
runtime updates**, so the host overlay menu can flip them mid-run. This
is the "option B" refactor from the 2026-07-12 settings scan.

Status: **PHASES 1–6 IMPLEMENTED; FINAL IN-GAME SAVE-ACCEPTANCE CHECK PENDING.**
`src/settings.{c,h}` now own
the existing cheat fields, nine widescreen behavior gates, render profile,
host-output HUD/menu scales, application display/aspect/audio fields, and a
99-row descriptor registry (86 persistent settings plus 13 non-persistent host
actions) with lookup, formatting, availability, mutation,
callbacks, sticky/restart results, observer notification, layered INI loading,
and atomic persistence. The promoted game HUD proves the post-upscale
host-compositor seam; master audio gain, audio enable, fullscreen, window
sizing, and the Sky Palace dialogue blip prove the live callback paths. The
host settings menu renders registry categories/rows with the
ROM-decoded font and selector, consumes input, freezes game advancement while
remaining redrawable, applies live values, and atomically saves accepted
changes. The native dialog-frame theme is also ROM-decoded. Direct text editing
handles custom values such as PAR pins and warp targets, while screen ratio is
an explicit 4:3/16:9/16:10 enum; ACTION
rows reuse the pause, turbo, save/load-state, warp, snapshot, restart,
graceful-exit, and save-codec paths.
The optional native ActRaiser menu entry, broader gamepad input, and decoding
the still-opaque town-map payload remain outside the completed overlay/save
editor phases.
Companion docs:
[SEAMS.md](SEAMS.md) (gameplay/tunable seams the cheats hook),
[rendering-engine.md](rendering-engine.md) (widescreen policy internals),
[ram-map.md](ram-map.md) (the WRAM addresses the cheats pin),
[save-format.md](save-format.md) (the SRAM map + checksum behind `CAT_SAVE`).

Source sites this design replaces: `ActRaiser_ApplyCheats`
(`src/actraiser_rtl.c:607`), `ActRaiser_ApplyWidescreenPolicy`
(`src/actraiser_rtl.c:327`), the widescreen sprite/BG builders
(`src/actraiser_widescreen_sprites.c`, `src/actraiser_widescreen_bg.c`), the
`Config` struct + ini parser (`src/config.c`), and the boot flag init block in
`src/main.c` (~`:355`–`:470`).

---

## 1. Current state

User-facing settings now have one resolved runtime surface: `g_settings`.
`config.c` remains as a compatibility parser for the legacy CamelCase
`config.ini` keys, but it stages recognized values into the descriptor registry
instead of owning runtime video/audio behavior. `settings.ini` is loaded above
that layer, and real process-environment values remain the highest boot-time
override.

Diagnostic-only `AR_*`/`SNESREF_*` flags intentionally remain direct `getenv`
consumers. Unknown diagnostic keys in `config.ini` still use the environment
bridge; known registry keys do not, so they remain distinguishable from real
environment overrides.

Before Phase 1, almost every cheat/widescreen flag was *enforced every frame*
but its env value was *read once and cached* in a `static int en = -1` gate:

```c
static int en = -1;
if (en < 0) { const char *e = getenv("AR_ALL_MAGIC"); en = ...; }  // read ONCE
if (en) { g_ram[0x0299] = 0x01; ... }                              // applied EVERY frame
```

Phase 1 has now replaced those cached gates with descriptor-seeded fields for
the 11 cheat controls (including the moonjump sub-setting and PAR pin list) and
nine widescreen behaviors. Their existing enforcement seams read `g_settings`
every frame. Debug/diagnostic flags such as `AR_WS_ONLYBG`, `AR_WS_LAYERS`, and
the `*_DBG` family intentionally remain direct `getenv` consumers (§11).

---

## 2. Goals / non-goals

**Goals**
- One typed struct (`g_settings`) as the live source of truth.
- One descriptor table (`SettingDesc[]`) that drives env-seeding, ini load/save,
  menu rendering, and live-apply — no per-field duplication across those four.
- A host-side in-game overlay that renders settings without consuming SNES
  VRAM/OAM or changing emulated game state.
- Save persistence and editing (`CAT_SAVE`, §4/§5) as a first-class category:
  keep native `.srm` compatibility, optionally load/persist lossless `save.ini`
  files, and stage sim-mode progress states for testing — the gap `AR_WARP`
  cannot cover (it stages an act transition, not sim progress).
- Behavior-preserving: a normal run and every existing `AR_*` dev run stay
  byte-identical at boot.
- A defined **apply path** per setting (§4) so the menu knows what each control
  costs to change live.

**Non-goals (explicitly out of scope here)**
- Migrating the ~45 debug/diagnostic `AR_*` flags (§11). They stay on `getenv`.
- Live SDL audio-device reopen for the two remaining RESTART rows (§4, §10.2).

---

## 3. Architecture

### 3.1 `Settings` — the live runtime struct

Replaces the scattered `static int en` caches. One global; the menu writes it,
the per-frame gates read it.

```c
// settings.h
typedef struct Settings {
  /* Render-profile state. CUSTOM means the individual fields below no longer
   * exactly match one of the three deterministic capture presets. */
  DisplayMode display_mode;
  int hud_scale_percent;       // 0=match game; 100=native host-output 1x
  bool hd_replacements;

  /* Application presentation; video geometry rebinds live. */
  int extended_aspect;         // enum: 4:3, 16:9, or 16:10
  PixelAspect pixel_aspect;    // square or 4:3 CRT stretch
  int window_scale;
  bool fullscreen, new_renderer, ignore_aspect_ratio;

  /* Audio */
  bool audio_enabled;
  AudioFrequency audio_frequency; // enum: 32.04, 44.1, or 48 kHz
  int  audio_samples;             // restart-class device format
  int  audio_master_volume;    // 0..100; atomic callback mirror
  bool audio_dialog_blip;      // exact $01:902D COP #$07 site

  /* QoL/debug utility state */
  int turbo_multiplier;
  uint16 warp_target;
  bool scene_inspector;        // F3 + click, read-only PPU asset identity

  /* Cheats (action-gated unless marked all-mode) */
  bool   cheat_all_magic;      // all-mode  -> $0299-$029C
  bool   cheat_ranged_sword;   // all-mode  -> $00E4 = $80
  int    cheat_inf_mp;         // all-mode  0=off, else pinned scroll count $21
  bool   cheat_inf_sp;         // all-mode  -> $0282/83 = $0284/85 (self-cal)
  bool   cheat_angel_hp;       // all-mode  -> $0286 = $0287 (self-cal)
  int    cheat_inf_hp;         // action    0=off, 1=auto (high-water), n=literal $1D
  bool   cheat_freeze_timer;   // action    pin $E6/$E7 (auto-release on boss drain)
  int    cheat_moonjump_spd;   // action    0=off, else px/frame on $08A4
  uint16 cheat_moonjump_btn;   // button mask vs auto-joypad word (default $8000=B)
  int    cheat_no_knockback;   // action    0=off, 1=full invuln, else raw $08A0+off pin
  uint8  pin_count;            // AR_PIN generic PAR pinner
  struct { uint32 off; uint8 val; } pins[32];

  /* Widescreen behavior (per-frame policy) */
  bool ws_action, ws_sim, ws_bgrefresh, ws_skypalace_bg, ws_sprites,
       ws_margin_objects, ws_margin_activation, ws_bg2_padding,
       ws_sim_sprites;
} Settings;
extern Settings g_settings;
```

#### Deterministic render profiles (implemented)

`F9` cycles three preset actions over the live fields; `Shift+F9` retains the
diagnostic dump. Boot with `ExtendedAspectRatio` enabled so the maximum
framebuffer exists. Pausing with `P` supports exact-same-frame comparisons of
the crop and draw-time BG policies:

| Profile | Presentation | Widescreen HLE behavior |
|---|---|---|
| 4:3 authentic | crop the maximum framebuffer to its center 256 columns | force zero live margins and disable all repair policy |
| Widescreen RAW | present the complete framebuffer and force symmetric margins | disable BG refresh/source repair, clamp, mirror/repeat, finite-world margin correction, sprite widening/drawing, and activation widening |
| Widescreen FULL | present the complete framebuffer | enable every shipped widescreen correction and use the scene-specific policy table |

The enum also has a non-cycling `CUSTOM` state. Boot-time `AR_WS_*` values are
classified by their actual combination; the overlay marks the profile
`CUSTOM` whenever it edits one individual widescreen field. Selecting a preset
is an action that deliberately overwrites those fields. This prevents the UI
from claiming “FULL” while, for example, sprite widening is disabled.

Sprite emission and enemy activation are game-update seams, not scanout
effects. While paused, F9 can immediately redraw presentation/BG differences,
but OAM membership and activation reflect the new preset on the next game
frame. For a sprite-focused comparison, change modes while running (or briefly
unpause for one frame) before taking the F2 capture.

Phase 1 originally reserved only `save_region_progress[]`. Phase 6 has since
reconciled the reference editor's USA offset adjustment and expanded staging to
the paged status, magic, item, score, Death Heim, and Professional fields in
save-format.md §3. The raw town-map/city payload remains outside the descriptor
surface until it is independently decoded.

### 3.2 `SettingDesc[]` — the descriptor registry (**the capture/store answer**)

One row per setting captures its storage location, type, seed source, category,
default/range, and live-update path. The implemented Phase-1 shape is:

```c
typedef struct SettingDesc {
  const char *key;        // "cheat_all_magic"  (ini key + menu id)
  const char *env;        // "AR_ALL_MAGIC"     (seed + back-compat; NULL if none)
  const char *label, *tooltip;
  SettingType type;
  SettingApplyKind apply;
  SettingCategory category;
  void       *field;      // &g_settings.cheat_all_magic
  long        defval, minval, maxval, step;
  bool        sticky;
  const char *const *enum_labels;
  int         enum_count;
  bool (*available)(void);
  void (*on_change)(const SettingDesc *);
  bool (*parse)(const char *, void *);  // custom env encoding
  int  (*format)(char *, int, const void *);
} SettingDesc;
extern const SettingDesc g_setting_descs[];
extern const int g_setting_desc_count;
```

`Settings_Find`, `Settings_SetLong/Text`, `Settings_Reset`, and
`Settings_FormatValue` are the only mutation/formatting path the overlay and
persistence layer should use. They normalize ranges, report sticky disables or
pending restarts, invoke the row callback, and notify the host observer. The
CTest target verifies unique keys/fields and that every current user-facing
field has exactly one descriptor.

### 3.3 Value resolution (the seed path)

Precedence is applied once at boot by `Settings_InitWithFile()` after
`ParseConfigFile()` has staged the compatibility layer:

```
built-in default < config.ini < settings.ini < real env < live menu writes
```

- `config.c` maps wired CamelCase application keys to stable descriptor keys.
  Known registry `AR_*` values are staged with their historical parsing
  semantics instead of being exported into the environment.
- `settings.ini` uses stable `SettingDesc.key` names and ordinary formatted
  values. Unknown future keys are ignored; invalid known values are reported.
- A real `getenv(desc->env)` value is applied last, preserving every existing
  command-line/dev workflow and its bespoke legacy encodings.
- After boot the **live struct is authoritative** — env/ini only seeded it. The
  menu mutates the struct and never touches the environment again.

The earlier environment-identity hazard is resolved by
`Settings_StageConfigValue` / `Settings_StageConfigEnvironment`. Diagnostic-only
flags continue using the environment bridge, while known user settings retain
their correct config-tier rank. Display presets additionally track the highest
layer that touched an individual widescreen field: a lower-tier profile cannot
erase a higher-tier `ws_*` override, while `AR_DISPLAY_MODE` still wins over
same-tier individual flags as it historically did.

This is a drop-in: the gates get seeded from the same env vars they used to
read, so boot behavior is unchanged. The refactor is behavior-preserving by
construction (see the golden regression in §9).

The ~5 bespoke encodings — `AR_PIN` (code list), `AR_MOONJUMP` (`"1"`→6 else n,
plus `AR_MOONJUMP_BTN`), `AR_INF_HP` (`1`=auto vs literal), `AR_INF_MP`
(`"1"`→10 else n), `AR_NO_KNOCKBACK` (`1`=full vs hex offset) — do not fit
uniform int parsing and use the `SET_CUSTOM` `parse`/`format` hooks rather than
being forced into the generic path.

### 3.4 Host overlay integration seam

The settings UI should be a **host overlay**, not a new emulated SNES screen.
That keeps it independent of ActRaiser's tile/OAM budgets and prevents the menu
from perturbing WRAM, PPU registers, or input timing. The current present path
is `src/main.c:900`: copy the game texture, draw the overlay, then call
`SDL_RenderPresent`.

Required behavior:

1. An overlay hotkey is consumed before `HandleInput`; while open, navigation
   keys never reach `g_input_state`.
2. Freeze `RunOneFrameOfGame` while retaining and redrawing the last game
   texture behind the menu. Do not use the current early `g_paused` `continue`
   unchanged, because it skips rendering entirely.
3. Generate categories, rows, availability, values, tooltips, and restart
   badges from `SettingDesc[]`; hand-written UI code owns layout only.
4. Apply PASSIVE values between game frames. CALLBACK/ACTION operations also
   execute on the main SDL thread; RESTART values display their pending state.
5. Closing the overlay must release/restore captured input so a held menu key
   cannot become a stuck SNES button.

The Phase-5 implementation applies those lifecycle rules in
`src/settings_overlay.{c,h}`. Escape or F1 opens it globally before emulated
input dispatch, independent of `$18/$19` or any native menu state. Neither key
maps to the SNES controller, and opening clears the currently held emulated
buttons before freezing frame advancement. Escape/F1/B closes the overlay;
closing clears input again so no menu navigation can become a stuck game
button. Window-manager quit remains available while Escape is owned by the
menu.

Left/Right performs ordinary stepped edits. Pressing SNES A (`X`/Return) on a
CUSTOM row starts SDL text-input mode with the current formatted value; typed
text and Backspace edit a scratch buffer, Return validates it through
`Settings_SetText`, and Escape cancels without touching the live value.
Pressing the same button on an ACTION row calls `Settings_InvokeAction`.
Actions are descriptor-driven but intentionally absent from `settings.ini`;
the stored `turbo_multiplier` and `warp_target` parameters are ordinary
persistent rows.

The current ActRaiser target is C and links SDL2 only. A reusable RmlUi launcher
exists under `snesrecomp/runner/src/launcher/`, but it is not part of
`runner.cmake` and expects C++ plus an existing OpenGL 3.3 context; ActRaiser
currently presents with `SDL_Renderer`. The implemented overlay is therefore a
small SDL-rendered immediate UI with ROM-decoded assets and a host fallback
font. Reusing
RmlUi remains a later option if the runtime adopts its GL/C++ integration (see
§10.4).

That SDL frontend now renders directly in the renderer's physical output
coordinates. Its canvas is always the complete window resolution and therefore
inherits the window aspect ratio, not the letterboxed game viewport. Three
independent panels (category, settings, and help) leave transparent gutters
that expose the paused game. Font glyphs, selectors, borders, row spacing, and
column padding use an independent nearest-neighbor content scale.
`menu_scale_percent=0` selects the largest 0.25x step that retains a minimum
464×208 layout; `100` means one source-art pixel per host-output pixel, and the
current range reaches `800` for high-DPI displays. Manual values larger than
the available window are fit-clamped for rendering without rewriting the
saved preference.

Navigation is explicitly hierarchical. The overlay opens focused on the
left-hand primary list; Up/Down moves between categories and promoted direct
actions, and SNES A enters a category. Within a category, Up/Down selects rows,
Left/Right edits values, and SNES B returns focus to the primary list. SNES B
closes only when primary navigation already has focus. Inspector, Restart, and
Exit are direct leaves and therefore execute/toggle immediately on A without a
duplicate one-row settings panel. L/R no longer changes categories.

The font is the game's real 256-tile, 8×8, 2bpp dialog set. The title asset
script's second `$80` operation uploads it to BG3 VRAM `$5000`; its encoded
pointer `$0B:ECFB` normalizes to ROM `$17:ECFB`, file offset `$0BECFB`.
The first word is the exact `$1000`-byte output size. A host equivalent of
`$02:C5C9` decodes the continuously packed MSB-first stream once during overlay
initialization into small normal/dim/warning atlas textures. Pixel classes map
to transparent, black outline, pale-blue shadow, and white face, preserving the
font's baked treatment. ASCII letters, lowercase letters, and digits map
directly to tile indices; tile `$3E` supplies the selector. Host-authored
supplements replace the colon, percent, dollar, and restart-marker slots that
contain unrelated game symbols. If ROM validation or decoding fails, the
compact 5×7 host font supplies the atlas instead. An SDL texture-allocation
failure still aborts initialization normally. No ROM-derived bytes are
committed, and the overlay never samples scene-dependent live VRAM.

The panels use the game's native Sky Palace dialog frame as an 8×8 nine-slice.
Runtime snapshots `runs/20260716-072558/snapshots/snap_00_gf460` and
`snap_01_gf668` proved the character bank byte-identical to ROM `$0D:C000`
(file `$06C000`) and palette 7 at ROM `$1C:BF73` (file `$0E3F73`). The host
decodes `$CE/$CF` corners, `$DE/$DF` vertical edges, `$EE` horizontal edge, and
opaque-black `$FF` center directly from the supplied ROM. Vertical flips
produce the lower corners/top edge. This deliberately bypasses the 16×16
metatiles `$4E/$4F`: they mix the lower dialog corners with Sky Palace scenery
tile `$18`, which is not part of the reusable frame.

The promoted game HUD is a separate earlier validation of the same host
compositor seam. In widescreen-full scenes, the PPU extracts the promoted BG3 status pixels and
the exact selected-magic OAM signature into transparent ARGB surfaces. The SDL
present path first upscales the ordinary game texture, then switches to physical
renderer-output coordinates and composites the HUD chunks at
`hud_scale_percent`. `0` means **Match game**; `100` means one SNES HUD pixel is
one output pixel vertically. This operation never rewrites VRAM, OAM, WRAM, or
PPU registers. See `rendering-engine.md`, “Promoted HUD host overlay”.

---

## 4. Live-update paths (`ApplyKind` taxonomy)

Every setting resolves to exactly one path, which *is* how the menu updates it
live:

| Path | Mechanism | Settings | Cost |
|---|---|---|---|
| **PASSIVE** | Existing gate reads the field every frame. Menu writes struct → next frame reflects it. No extra code. | 11 cheat controls, 9 widescreen behaviors, host HUD scale, dialogue blip | free |
| **CALLBACK** | The host observer performs one live update: master gain copies to an atomic callback mirror; display callbacks resize/rebind the preallocated video surfaces; window/fullscreen/stretch controls update SDL; audio enable pauses/resumes the device. | master volume, screen/pixel aspect, renderer path, window scale, fullscreen, ignore-aspect, audio-device toggle | small |
| **RESTART** | Requires host-device reinitialization unsafe to perform from the current callback path. | audio frequency/samples (audio-device reopen; audio thread) | real work |
| **ACTION** | Not stored toggles — commands the menu invokes; only the *param* is stored. | warp (`warp_target`, F6), turbo (`turbo_mult`, T), savestate (F5/F7), snapshot (F2), pause (P) | reuse existing hotkey paths (`src/main.c:587`) |
| **SAVE** | Transactional mutation of the canonical `g_sram` image + mandatory checksum recompute; optional commit through the active `.srm` or `.ini` backend. Takes effect **the next time the game loads the save from its own title menu** — no app restart. See §4.1. | paged progress/status/magic/item/score staging, save import/export actions | small, but see the §4.1 hazard |

The core Phase-1 change is implemented across the original passive rows. Phase
4 adds one audio PASSIVE row and one audio CALLBACK row without changing those
gameplay gates:

```c
// before                                    // after
static int en = -1;                          if (g_settings.cheat_all_magic) { ... }
if (en < 0) { ...getenv("AR_ALL_MAGIC")... }
if (en) { ... }
```

Aspect now uses the renderer's compile-time maximum 448-pixel capacity while
retaining an active pitch of `256 + 2*extra`. Ratio or pixel-shape changes
recompute `extra`, rebind the PPU and overlay surfaces with that pitch, and
select the matching texture subrect. The emulated PPU/WRAM state is untouched,
so 4:3, 16:9, and 16:10 can switch live without reallocating game state.

### Audio control seams (Phase 4)

The current audio path has two useful control points, and they solve different
problems:

- **Device rate is a presentation boundary, not a clock.** The runner's S-DSP
  FIFO is always 32.04 kHz. `RtlSetAudioOutputRate()` receives the actual rate
  returned by SDL, and `dsp_getSamplesResampled()` retains a fractional native
  cursor across callbacks. Enhanced OGG and MSU-1 source consumption likewise
  derives from `out_frames / output_rate`, with fractional carry. Changing
  32.04/44.1/48 kHz or `AudioSamples` can therefore change latency/quality but
  never pitch, tempo, or the amount of emulated audio consumed per second.

- **Master volume is post-mix host gain.** `AudioCallback()` calls
  `RtlRenderAudio()` first, then scales the final interleaved signed-16-bit PCM
  block by `audio_master_volume` (`0..100`). This includes the DSP mix, sound
  effects, music, echo, and MSU-1 audio. The audio thread reads an
  `SDL_atomic_t` mirror; it never races on `g_settings`.
- **Dialogue blip is suppressed at its native request site.** The message
  composer calls `$01:901C` for each printable glyph. After the character delay
  (`$01:9278`), the non-space path at block `$01:902D` loads `#$07` and executes
  `COP`; the ROM vector writes that request to `$035A`. When
  `audio_dialog_blip` is off, `ActRaiser_CopHook()` skips only that exact
  `$01:902D` post. It must not suppress every COP request with ID `$07`, because
  the same ID is also used by unrelated events elsewhere in the game.

Independent music and SFX sliders are **not yet exposed**. By the time
`RtlRenderAudio()` returns, all eight DSP voices and echo are already summed;
scaling that PCM cannot separate categories. A correct implementation requires
one of these to be proven first:

1. identify stable ActRaiser music/SFX voice ownership across Sky Palace, sim,
   action, and bosses, then scale voices before DSP summation (including a
   defined echo policy); or
2. reverse-engineer the native SPC driver commands/state that control its music
   and SFX buses, and adjust those buses before mixing.

Use `AR_COPLOG=1` to correlate CPU sound requests and `AR_KONLOG=1` to correlate
DSP key-on/voice/source activity. Acceptance requires captures from every major
mode and an explicit echo test; a slider that merely changes DSP master volume
would be a mislabeled duplicate of the implemented master control.

- **Enhanced music (`music_replacements` / `AR_MUSIC_REPLACEMENTS`, default
  on, callback).** Master toggle for manifest-driven OGG music streaming
  (`[music:]` entries of `game-assets/manifest.ini`; see docs/SEAMS.md
  "Audio"). Inert without audio files. The registry callback calls
  `MusicReplacements_ApplySetting()` while game execution is paused: off stops
  the stream and clears `g_dsp_voice_mute_srcn_min`, while on reselects and
  starts a replacement for the remembered current `(src, song)` immediately.
  Native `$F2` pause and host-owned pause/settings-overlay state suspend the
  Vorbis decoder without closing it or advancing its cursor; the same-song
  resume command continues at the saved frame rather than restarting the OGG.
  The authentic SPC sequencer continues running beneath a replacement, so
  disabling resumes it in place. Note the srcn>=0x0C voice-gate
  discovery here is ALSO prerequisite work for the music/SFX slider problem
  above: if the srcn split proves stable across all modes, option 1 becomes
  "scale srcn>=0x0C voices" rather than full voice-ownership RE.

### 4.1 The save backend / `APPLY_SAVE` path

Save edits are structurally unlike cheats. A cheat pins WRAM every frame; a save
edit is a **transactional mutation of the battery-SRAM image**, consumed by the
game only when it next loads the save. Disk format is a separate concern.

**Implemented hook.** `src/main.c` attaches the canonical `g_sram` buffer to
`src/save_system.c` after power-on initialization. `SaveSystem_LoadActive()`
owns boot load and `SaveSystem_WriteActive()` provides unconditional explicit writes;
`SaveSystem_AutoPersistIfChanged()` owns the per-frame and clean-shutdown
shadow-aware persistence checks. The selected backend decodes either native 8 KiB SRAM or the lossless
INI schema in save-format.md §4 into the same canonical image. The game and HLE
code continue to see exactly the same buffer.

`SaveSystem_ApplyEdits()` can run after the active backend has loaded —
before the game boots — so a seeded `AR_SAVE_*` value can be live for the whole session
(the "easy testing" path). The overlay can also invoke it on demand: because
the game re-reads SRAM whenever the title menu loads a save, an edit applied
mid-session takes effect on **return-to-title → continue**. No app restart.

**Transactional operation (all steps are mandatory):**
1. Refuse field edits unless `save_edit_armed` is set. Import/export has its own
   explicit confirmation and does not bypass validation.
2. Decode/copy the current image into an 8 KiB scratch buffer. Never partially
   mutate live SRAM while parsing a file.
3. Validate and write only verified `SaveFieldDesc[]` fields; preserve every
   untouched byte, including unknown town data and power-on fill
   (save-format.md §1/§3.4).
4. Recompute the checksum over `[0x0000, 0x1fec)` and store it at `0x1fec`
   (save-format.md §2). **Skipping this makes the game reject the save.**
5. Take a timestamped backup of the active save if `save_autobackup` (default
   on) before the first persistent change.
6. Swap the validated scratch image into `g_sram` between frames. If the user
   selected **Apply and save**, atomically write it through the one active
   backend; **Apply for this session** leaves the disk file unchanged.
7. Re-sync the auto-persist shadow buffer so the editor's deliberate mutation
   is not mistaken for a game-originated save. This is implemented and covered
   by `actraiser_save_system`.

**Closed hazard: auto-persist versus deliberate editor changes.** The old main
loop directly compared `g_sram` and wrote `saves/save.srm`, which would have
made a session-only edit persistent. The implemented path delegates that check
to `SaveSystem_AutoPersistIfChanged()`, writes only the boot-selected backend,
and re-synchronizes its shadow after an editor swap. Consequently **Apply for
session** does not touch disk, while **Apply and save** performs the backup and
atomic write explicitly.

**Verification gate.** The field addresses and encodings are backed by the USA
offset adjustment in the external editor, the linear SRAM→WRAM status mapping,
repository fixtures, and transactional byte-level tests. The codec has also
proven every repository fixture checksum-valid plus an unedited
`.srm → .ini → .srm` byte-identical round trip. The remaining manual gate is to
apply representative persistent edits on every page, Restart Game, choose
Continue, and confirm the game displays/uses each intended value.

---

## 5. Complete settings inventory (registry contents)

The full set of user-facing settings the menu should expose. Booleans note their
default polarity (cheats default **off** = `(e && e[0]!='0')`; widescreen
quality knobs default **on** = `!(e && e[0]=='0')`). Cheats are always editable
and persist immediately; “Applies” identifies the engine in which their
per-frame enforcement becomes active.

### Cheats (`CAT_CHEATS`) — all PASSIVE

“Passive” means disabling stops future enforcement; it does not generally
rewind game history. All Magic is explicitly marked sticky because its unlock
bytes may already have been consumed as legitimate-looking progress. Infinite
HP's auto high-water and Freeze Timer's capture/drain latch reset when their
live values are disabled/re-enabled. Leaving full No Knockback clears only the
i-frame timer/flag that mode owns; experimental raw-offset pins cannot safely
restore an unknown prior byte.

| Setting | env | Type | Default | Applies | Effect / RAM |
|---|---|---|---|---|---|
| All Magic | `AR_ALL_MAGIC` | bool | off | all modes | unlock 4 spells `$0299-$029C` |
| Ranged Sword | `AR_RANGED_SWORD` | bool | off | all modes | `$00E4=$80` projectile |
| Infinite MP | `AR_INF_MP` | int | off (`1`→10) | all modes | pin scroll count `$21` |
| Infinite SP | `AR_INF_SP` | bool | off | simulation | `$0282/83 = $0284/85` (self-cal) |
| Angel HP | `AR_ANGEL_HP` | bool | off | simulation | `$0286 = $0287` (self-cal) |
| Infinite HP | `AR_INF_HP` | int | off (`1`=auto) | action | pin `$1D` |
| Freeze Timer | `AR_FREEZE_TIMER` | bool | off | action | pin `$E6/$E7`; auto-release on boss drain |
| Moonjump | `AR_MOONJUMP` | int px/f | off (`1`→6) | action | move `$08A4` up while btn held |
| — Moonjump button | `AR_MOONJUMP_BTN` | mask | `$8000` (B) | action | sub-setting of Moonjump |
| No Knockback | `AR_NO_KNOCKBACK` | int | off (`1`=full) | action | invuln `$08C6`/`$08D1` (magic-safe) |
| Custom codes | `AR_PIN` | custom | empty | all modes | up to 32 PAR `7Exxxxvv` pins/frame |

The menu does not dim or reject action-only cheats while simulation, Sky
Palace, a cutscene, or a title flow is active (and vice versa). A change is
saved immediately, then the existing mode-gated runtime hook begins enforcing
it naturally when the relevant engine becomes active.

### Widescreen behavior (`CAT_WIDESCREEN`) — all PASSIVE

| Setting | env | Type | Default | Note |
|---|---|---|---|---|
| Wide action stages | `AR_WS_ACTION` | bool | on | master action-geometry toggle |
| Wide simulation towns | `AR_WS_SIM` | bool | on | master toggle; applies `$01:B4C6` map-edge caps, keeps BG2/dialogs clamped, and gates the separate world-sprite setting |
| Wide simulation sprites | `AR_WS_SIM_SPRITES` | bool | on | widens ADAD/AE6F horizontal emission only for `$0A00-$1087` world records and the dedicated `$0B0A` angel-arrow lifetime leaf `$B473`. Fixed/UI records, hard world bounds, and vertical rules stay authentic |
| BG margin refresh | `AR_WS_BGREFRESH` | bool | on | true-content margins vs stale/wrapped |
| Sky Palace BG2 source repair | `AR_WS_SKYPALACE_BG` | bool | on | render-only ROM source-map margin decode; off restores raw-wide dialogue staging. Validated 2026-07-13 (byte-identical to the boot colonnade) |
| Widen sprites | `AR_WS_SPRITES` | bool | on | emit sprites into margins |
| Draw margin objects | `AR_WS_MARGIN_OBJECTS` | bool | on | object draw coverage in margins |
| Extend activation | `AR_WS_MARGIN_ACTIVATION` | bool | on | `$0400` activation boundary |
| Decorative BG2 padding | `AR_WS_BG2_MIRROR` | bool | on | stage policy chooses reflection or cyclic repeat; off clamps the 256-wide BG2. Keep env name for compatibility |
| Clamp override | `AR_WS_CLAMP` | mask | none | manual per-layer mask; already uncached/live |

### Aspect / display / audio / QoL

| Setting | Source | Type | Default | Apply |
|---|---|---|---|---|
| HUD output scale | `AR_HUD_SCALE` | int percent or `x` suffix | Match game (`0`) | PASSIVE; `100` = native output 1× |
| Menu output scale | `AR_MENU_SCALE` | int percent or `x` suffix | Auto (`0`) | PASSIVE; full-window content scale from 25–800, `100` = native 1× |
| HD replacements | `AR_HD_REPLACEMENTS` | bool | on | PASSIVE; inert when art is absent |
| Master volume | `AR_AUDIO_VOLUME` | int percent | 100 | CALLBACK; atomic post-mix gain, live `0..100` in steps of 5 |
| Dialogue text blip | `AR_DIALOG_BLIP` | bool | on | PASSIVE; exact `$01:902D` COP request only |
| Screen ratio | `ExtendedAspectRatio` / `AR_EXTENDED_ASPECT_RATIO` | enum 4:3/16:9/16:10 | 4:3 | CALLBACK; changes active PPU border/pitch and presentation live |
| Pixel aspect | `AspectPAR` / `AR_ASPECT_PAR` | enum 4:3/square | 4:3 | CALLBACK; recomputes active internal width live |
| Window scale | `WindowScale` / `AR_WINDOW_SCALE` | int 1..8 | 3 | CALLBACK |
| Fullscreen | `Fullscreen` / `AR_FULLSCREEN` | bool | off | CALLBACK; desktop-fullscreen |
| New renderer | `NewRenderer` / `AR_NEW_RENDERER` | bool | on | CALLBACK; live in 4:3, widescreen forces the new path while active |
| Ignore aspect | `IgnoreAspectRatio` / `AR_IGNORE_ASPECT_RATIO` | bool | off | CALLBACK |
| Enable audio | `EnableAudio` / `AR_ENABLE_AUDIO` | bool | on | CALLBACK; lazily opens then pauses/resumes the device |
| Audio freq | `AudioFreq` / `AR_AUDIO_FREQ` | enum 32.04/44.1/48 kHz | 44.1 kHz | RESTART; numeric `32040`/`44100`/`48000` remain accepted |
| Audio samples | `AudioSamples` / `AR_AUDIO_SAMPLES` | int | 2048 | RESTART |
| Turbo multiplier | `AR_TURBO_MULT` | int | 8 | Persistent ACTION parameter; consumed live by T and Toggle turbo |
| Warp target | `AR_WARP` | custom hex | `0101` | Persistent ACTION parameter; direct text editor accepts the raw region/map target used by F6 and Warp now |
| Scene inspector | `AR_SCENE_INSPECTOR` | bool | off | PASSIVE; F3/left-click freezes and identifies live BG/OAM/Mode-7 assets; panel auto-avoids the sample and is draggable; clear restores inspector-owned pause |

The six non-persistent QoL ACTION rows are Pause/resume, Toggle turbo, Save
state, Load state, Warp now, and Take snapshot; they dispatch to the same host
paths used by P/T/F5/F7/F6/F2. Scene Inspector, Restart Game, and Exit Desktop
remain ordinary registry descriptors but the overlay filters them out of the
QoL panel and promotes them as direct leaves in the primary left-hand
navigation. This is a presentation-only projection, so persistence, runtime
observers, and action dispatch still have one source of truth. The lifecycle pair saves settings,
takes the shared clean-shutdown/SRAM path, and either exits normally or
re-executes the same process arguments.

### Save storage and editing (`CAT_SAVE`)

The active disk backend changes serialization only; both formats decode to the
same 8 KiB `g_sram` image (save-format.md §4). The staged payload is projected
through five pages so the native-font panel stays readable; changing **Editor
page** only filters rows and never changes SRAM.

| Setting | env | Type / apply | Default | Note |
|---|---|---|---|---|
| Save storage format | `AR_SAVE_BACKEND` | enum / RESTART | `native-srm` | `native-srm` or `ini`; boot-selects exactly one authoritative backend/path |
| Allow save edits | `AR_SAVE_EDIT` | bool / APPLY_SAVE | **off** | explicit safety gate; Apply actions and next-boot staged overrides refuse all fields while Off |
| Auto-backup | `AR_SAVE_BACKUP` | bool / PASSIVE | **on** | timestamped backup of the active format before first persistent edit; see §4.1 |
| Editor page | — | enum / PASSIVE | Progress | Progress, Status, Magic, Items, or Scores presentation filter |
| Apply for session | — | ACTION | — | mutate live SRAM and re-sync the persistence shadow without writing disk |
| Apply and save | — | ACTION | — | back up once, atomically persist through the active backend, then swap live SRAM |
| Import save | `AR_SAVE_IMPORT` (path override) | ACTION | — | decode `saves/import.srm`, falling back to `saves/import.ini`, into scratch; validate, back up, convert to the active backend, and swap without changing backend selection |
| Export native SRAM | — | ACTION | — | write exact `saves/export.srm` without changing the active backend |
| Export structured INI | — | ACTION | — | write lossless `saves/export.ini` without changing the active backend |
| Six town State rows | `AR_SAVE_PROG_*` | enum / APPLY_SAVE | leave as-is | combined `$1200+r*2` base plus `$13B6+r*2` Act-2 flag; Act 1, Act 1 cleared, Act 2, Act 2 cleared |
| Death Heim State | — | enum / APPLY_SAVE | leave as-is | locked, unlocked, or cleared via `$120C/$1240` |
| Professional Mode | — | enum / APPLY_SAVE | leave as-is | locked/unlocked `ACT` marker at `$1FF0` |
| Player Name | — | text / APPLY_SAVE | leave as-is | 1–8 printable characters |
| Master status | — | numeric / APPLY_SAVE | leave as-is | level `1..17`, HP `1..24`, MP `0..10`, lives `1..9` |
| Angel status | — | numeric / APPLY_SAVE | leave as-is | current/max SP `0..999`, current HP `0..24`, max HP `1..24` |
| Message Speed | — | numeric / APPLY_SAVE | leave as-is | native saved value `0..9` |
| Equipped Magic | — | enum / APPLY_SAVE | leave as-is | none or Fire/Stardust/Aura/Light; writer selects the inventory slot containing that spell |
| Magic Slots 1–4 | — | enum / APPLY_SAVE | leave as-is | empty, Fire, Stardust, Aura, or Light |
| Item Slots 1–8 | — | enum / APPLY_SAVE | leave as-is | enumerated inventory values in save-format.md §3.3.1 |
| Twelve act scores | — | numeric / APPLY_SAVE | leave as-is | `0..99990` in increments of 10, encoded as packed BCD |

Every staged field defaults to **Leave as-is**. Page changes and ordinary
settings persistence can therefore never turn an unstaged value into an SRAM
write. All pages are collected into one `SaveEditRequest`, validated in scratch,
checksummed once, and committed transactionally by `SaveSystem_ApplyEdits()`.

---

## 6. Persistence

- There are two distinct INI domains. **`settings.ini` is application
  configuration** and may contain `save_backend = native-srm|ini`.
  **`saves/save.ini` is battery-save game data** and is owned exclusively by
  the Phase-6 save codec (save-format.md §4). Never pass save payload sections
  through `SettingDesc[]` or merge them into `settings.ini`.
- `Settings_Save(path)` / `Settings_Load(path)` iterate `g_setting_descs[]` using
  each row's `format`/`parse`, so adding a setting never touches the serializer.
- A separate **`settings.ini` is implemented and loaded after `config.ini`**,
  leaving the dev-authored file and its comments untouched. The writer uses a
  same-directory temporary file and atomic replacement. The menu owns
  `settings.ini`; devs own `config.ini`; env still overrides both. Load order:
  `defaults → config.ini → settings.ini → env → live`.
- The Phase-5 overlay invokes `Settings_Save("settings.ini")` after accepted
  menu mutations. Existing diagnostic hotkeys and `AR_SETTING_SET` remain
  session-only so automated probes do not silently rewrite user preferences.
- The two restart-class audio-format rows store the desired/persisted value in
  `g_settings`, while the open SDL audio device consumes a boot snapshot. This
  prevents a pending frequency/buffer change from half-applying through an
  unrelated live callback.
- `SaveSystem_LoadActive()` / `SaveSystem_WriteActive()` independently serialize
  the exact 8 KiB `g_sram` image using the backend selected by those resolved settings.
  Native `.srm` remains the default. The codec logs the chosen backend and path
  once at boot so users can always tell which save is authoritative.

---

## 7. Threading / safety model

The game runs as a cooperative coroutine via `swapcontext`
(`RunOneFrameOfGame`, `src/actraiser_rtl.c`) on the **same thread** as the SDL
event loop. The menu mutates `g_settings` *between* frames; ordinary game and
host code reads it on the next frame without locks.

SDL's audio callback is the exception: it runs on a separate audio thread.
Live callback inputs must use a synchronized mirror. Master volume therefore
copies the descriptor value into `SDL_atomic_t g_audio_master_percent`, which
the callback reads after rendering. Audio-format settings remain RESTART-class
because device close/reopen must be serialized by SDL; never have the callback
read mutable format fields directly from `g_settings`.

---

## 8. Phased rollout

1. **Phase 1 — struct + seed, no menu (implemented; gameplay golden runs
   remain).** The 20 legacy cheat/widescreen controls read live fields; special
   env encodings and default polarities are covered by `actraiser_settings`.
   The 4:3/RAW/FULL preset actions and paused render-only redraw are implemented.
   Remaining validation: §9 frame-level cheat comparisons in representative
   action/sim states.
2. **Phase 2 — prove live mutation headless (implemented).**
   `AR_SETTING_SET=key=value` plus optional `AR_SETTING_AT_GF=N` applies one
   descriptor mutation through the real main loop. The unit test covers the API;
   the headless probe has confirmed a live FULL→RAW profile mutation.
3. **Phase 3 — descriptor metadata + apply kinds (implemented for current
   rows).** Labels, tooltips, ranges, enum labels, availability, change
   callbacks, formatting, sticky warnings, restart results, and a host observer
   are present. New Config/audio/save rows must supply the same metadata as they
   migrate in Phases 4/6.
4. **Phase 4 — persistence + Config merge (implemented).** The registry
   owns every wired display/aspect/audio field. `config.ini` is a lower-priority
   compatibility layer, `settings.ini` load/save is descriptor-driven and
   atomic, real env values remain distinguishable and highest-priority, and
   audio-format resources use boot snapshots. Screen ratio, pixel aspect,
   renderer selection, window scale, fullscreen, ignore-aspect, audio enable,
   master volume, and dialogue blip have live apply paths. Independent
   music/SFX gain remains research-gated by the audio-seam
   criteria above; dead template config keys remain excluded rather than being
   promoted as fake settings.
5. **Phase 5 — overlay UI (implemented).** The SDL host
   overlay now renders rows from `SettingDesc[]`, writes `g_settings`, persists
   accepted mutations, intercepts menu input, freezes game-frame advancement,
   and scales independently. Escape/F1 opens it globally from every game
   state. The verified ROM font/selector asset is now decoded
   into host atlases; the same path builds the native three-panel dialog-frame
   theme, and the menu scales over the full window. Phase 5B adds validated
   direct text editing and eight descriptor-driven ACTION rows backed by the
   existing host commands. A native game-menu entry remains optional; gamepad
   navigation belongs to the broader runner input project rather than blocking
   the keyboard-complete overlay.
6. **Phase 6 — save codec, backends, and editor (`CAT_SAVE`; implemented).** It
   owns `g_sram` persistence, not the cheat gates. The delivered pieces are:

   - **6A — codec core (complete).** `tools/srm.py` and `src/save_system.c`
     implement exact-size native SRAM decode/encode,
     `SramChecksum()`, `SaveFieldDesc[]`, validation/errors, and fixture tests.
   - **6B — lossless INI (complete).** The version-1 chunked-hex schema from
     save-format.md §4.1. Require all raw chunks, apply verified readable-field
     overrides, and reject invalid input transactionally. Prove every fixture
     survives `.srm → .ini → .srm` byte-identically with no edits.
   - **6C — active backend integration (complete).** Boot load and per-frame
     auto-persist route through `SaveSystem_LoadActive()` /
     `SaveSystem_AutoPersistIfChanged()`; keep
     native `.srm` as default, make INI explicit, write atomically, log the
     selected target, take backups, and expose a real shadow-buffer re-sync API.
   - **6D — editing and conversion actions (code complete; manual game check
     pending).** The §4.1 safety sequence and all enabled fields represented by
     the reference editor are exposed on five pages: town/Death Heim/
     Professional progress, player and Angel status, magic, items, and scores.
     Import, Export `.srm`, Export `.ini`, Apply for session, and Apply and save
     remain explicit commands rather than implicit writes.
   - **6E — future field promotion.** Decode the raw town-map/city payload one
     field at a time only after WRAM correspondence, known-state diffing, and a
     game round trip. Reference-editor rows explicitly disabled or hidden
     (city internals, current city, derived next-level XP) remain unexposed.

   Automated Phase-6 acceptance now proves malformed/truncated INI never changes `g_sram`;
   an unedited cross-format round trip preserves all 8192 bytes including fill
   and unknown town state; a named-field edit changes only its verified bytes
   plus `0x1fec`–`0x1fef`; backend choice is deterministic when both files
   exist; and auto-persist writes only the active target. Acceptance of a
   host-edited save through the game's title-screen Continue flow remains the
   one manual check.

---

## 9. Verification (project bar)

- **Golden regression:** for each cheat, run the old env path vs the new seeded
  struct and assert `g_ram` effects are identical via the WRAM oracle / `F2`
  snapshot. Since seeding uses the same env vars, an `AR_ALL_MAGIC=1` run must
  match byte-for-byte.
- **Drift guard:** `tests/settings_test.c` asserts all 99 descriptor keys are
  unique, all 86 persistent rows have unique storage, ACTION rows have none,
  and lookup/formatting/persistence behave correctly.
- **Live-path test:** `AR_SETTING_SET=key=value AR_SETTING_AT_GF=N` applies via
  the same mutation API the overlay calls; observe enforcement at N+1.
- **Save-codec tests (Phase 6):** exact 8192-byte native round trip; fixture
  `.srm → .ini → .srm` identity; malformed/missing/duplicate INI chunks rejected
  without touching the destination; named-field override changes only its
  verified bytes plus checksum; session-only edits do not trip auto-persist;
  persistent edits and exports reload through their target codecs; and both
  backends must pass a real game load/save/load round trip.
- **Clean dev workflow:** `cycle.sh`, the `config.ini` AR_ bridge, and all debug
  flags (§11) stay on `getenv`. Migrate only user-facing settings — do
  not boil the ocean.

---

## 10. Design decisions and follow-ups

### 10.1 Persistence target

Implemented as a separate atomic `settings.ini`, keeping developer-authored
`config.ini` comments and structure untouched.

### 10.2 Remaining RESTART settings (audio format)

The video decision is implemented: maximum-capacity host textures plus live
active-pitch/PPU rebinding make screen ratio, pixel aspect, and renderer
selection callback rows. Audio frequency and sample-buffer size still show the
pending-restart badge; making those live requires serializing an SDL audio
device close/reopen against its callback thread.

### 10.3 Phase-1 scope

Implemented for user-facing settings. Diagnostic/debug flags remain direct
environment controls because wrapping them would add surface area without
improving the player menu.

### 10.4 Overlay frontend

Implemented as an SDL-rendered immediate UI using ROM-decoded ActRaiser font
and frame assets with host-authored fallbacks. RmlUi remains a possible future
frontend only if the runtime adopts its C++/OpenGL integration.

### 10.5 Default save-edit action
Implemented with **Apply for session** first: it swaps validated SRAM and
re-syncs the shadow without altering disk. **Apply and save** follows it and
backs up before atomically committing through the active backend. Merely
changing any editor row only stages a value; neither action fires implicitly.

---

## 11. Excluded from the menu

**Debug / diagnostic flags (~45 of the 69 `AR_*`)** — stay on `getenv`, most set
a global once in the boot init block. Not for end users; at most a hidden
"Developer" submenu: `AR_TRACE*`, `AR_*LOG`, `AR_MXCHECK/MXHIST/EXITMX/CALLMX`,
`AR_TRAPFN`, `AR_DUMP*`, `AR_SHOT*`, `AR_VRAMDUMP*`, `AR_WRAM*`, `AR_SRAM_FILL`,
`AR_FORCE18`, `AR_FORCE_INPUT*`, `AR_INPUT_RECORD/REPLAY`, `AR_REPLAY_NOSTOP`,
`AR_LOADSTATE`, `AR_HEADLESS`, `AR_WS_HEADLESS`, `AR_WS_SURVEY`, `AR_WS_*DBG`,
`AR_WS_LAYERS`, `AR_WS_ONLYBG`, `AR_NOPOP`, `AR_YIELDLOG`, `AR_GFLOG`,
`AR_PERF`, `AR_PACE`, `AR_QUIT_FRAMES`, `AR_NO_RUN_DIR`, `AR_CTACTION`,
`AR_RTSDISP_MISS`, `AR_OBJLOG`, `AR_PPULOG`, `AR_FRAMELOG`.

**Dead config fields — parsed but zero consumers.** Do not expose these as
menu settings; they currently do nothing (template leftovers from the
snesrecomp base). Either wire them up first or omit them: `NoSpriteLimits`,
`LinearFiltering`, `NoSpriteLimits`, `AudioChannels`, `output_method`,
`enable_gamepad[2]`, `gamepad_deadzone`, `skip_launcher`, and `autosave`.
Fullscreen is no longer in this list: Phase 4 wires both its boot flag and live
`SDL_SetWindowFullscreen` callback.
