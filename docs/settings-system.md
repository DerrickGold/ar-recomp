# ActRaiser Recomp — Settings & Live-Config System

Design for a single source of truth that captures every custom setting (cheats,
widescreen knobs, display/audio, QoL) and defines each one's **path to live
runtime updates**, so a future in-game overlay menu can flip them mid-run. This
is the "option B" refactor from the 2026-07-12 settings scan.

Status: **PHASES 1–4 IMPLEMENTED; PHASE 5 NEXT.** `src/settings.{c,h}` now own
the existing cheat fields, nine widescreen behavior gates, render profile,
host-output HUD scale, application display/aspect/audio fields, and a 34-row
descriptor registry with lookup, formatting, availability, mutation,
callbacks, sticky/restart results, observer notification, layered INI loading,
and atomic persistence. The promoted game HUD proves the post-upscale
host-compositor seam; master audio gain, audio enable, fullscreen, window
sizing, and the Sky Palace dialogue blip prove the live callback paths. The
settings menu itself and the independent save codec/backends/editor remain.
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
- Live realloc for the RESTART-class settings on day one (§4, §10.2).

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

  /* Application presentation (restart rows retain desired value; host owns a
   * boot snapshot for already-created resources). */
  uint16 extended_aspect;      // packed (X << 8) | Y; 0 = authentic geometry
  PixelAspect pixel_aspect;    // square or 4:3 CRT stretch
  int window_scale;
  bool fullscreen, new_renderer, ignore_aspect_ratio;

  /* Audio */
  bool audio_enabled;
  int  audio_frequency, audio_samples; // restart-class device format
  int  audio_master_volume;    // 0..100; atomic callback mirror
  bool audio_dialog_blip;      // exact $01:902D COP #$07 site

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
classified by their actual combination; a future overlay must mark the profile
`CUSTOM` whenever it edits one individual widescreen field. Selecting a preset
is an action that deliberately overwrites those fields. This prevents the UI
from claiming “FULL” while, for example, sprite widening is disabled.

Sprite emission and enemy activation are game-update seams, not scanout
effects. While paused, F9 can immediately redraw presentation/BG differences,
but OAM membership and activation reflect the new preset on the next game
frame. For a sprite-focused comparison, change modes while running (or briefly
unpause for one frame) before taking the F2 capture.

`save_region_prog[]` is deliberately the *only* save field in Phase 1: it is the
one block verified against our own saves, and it alone delivers the sim-mode
testing goal. Every other offset in the third-party map is either untested or
contradicted for our ROM (save-format.md §3.2/§3.3) and must not be exposed
until a round-trip verifies it.

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

The current ActRaiser target is C and links SDL2 only. A reusable RmlUi launcher
exists under `snesrecomp/runner/src/launcher/`, but it is not part of
`runner.cmake` and expects C++ plus an existing OpenGL 3.3 context; ActRaiser
currently presents with `SDL_Renderer`. The lowest-risk first overlay is
therefore a small SDL-rendered immediate UI with an in-repo bitmap font. Reusing
RmlUi remains a later option if the runtime adopts its GL/C++ integration (see
§10.4).

The game HUD now validates this seam without implementing the settings menu.
In widescreen-full scenes, the PPU extracts the promoted BG3 status pixels and
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
| **CALLBACK** | `on_change()` fires one host update. Implemented audio master gain copies to an atomic callback mirror; future rows include `window_scale`→`SDL_SetWindowSize`, `fullscreen`→`SDL_SetWindowFullscreen`, `audio_enabled` mute→`SDL_PauseAudioDevice`, and `ignore_aspect_ratio`→renderer logical-size. | master volume now; window scale/fullscreen/ignore-aspect/audio-device toggle during Config migration | small |
| **RESTART** | Needs realloc/reinit unsafe to do cheaply mid-frame. Per setting: attempt heavy live reinit, or set a "pending — applies on relaunch" badge (§10.2). | aspect on/off + ratio (window + PPU buffer realloc), `new_renderer`, audio freq/samples (device reopen; audio thread) | real work |
| **ACTION** | Not stored toggles — commands the menu invokes; only the *param* is stored. | warp (`warp_target`, F6), turbo (`turbo_mult`, T), savestate (F5/F7), snapshot (F2), pause (P) | reuse existing hotkey paths (`src/main.c:587`) |
| **SAVE** | Transactional mutation of the canonical `g_sram` image + mandatory checksum recompute; optional commit through the active `.srm` or `.ini` backend. Takes effect **the next time the game loads the save from its own title menu** — no app restart. See §4.1. | `save_region_prog[]` (save-format.md §3.1), save import/export actions | small, but see the §4.1 hazard |

The core Phase-1 change is implemented across the original passive rows. Phase
4 adds one audio PASSIVE row and one audio CALLBACK row without changing those
gameplay gates:

```c
// before                                    // after
static int en = -1;                          if (g_settings.cheat_all_magic) { ... }
if (en < 0) { ...getenv("AR_ALL_MAGIC")... }
if (en) { ... }
```

For aspect specifically there is a strong intermediate that downgrades it from
RESTART to PASSIVE: the margin *amount* within an already-wide window is already
per-frame (`PpuSetExtraSpace` runs every frame in the policy,
`src/actraiser_rtl.c:469`). Allocate the window + PPU buffers at the **max**
budget on boot, then let the menu clamp visible columns live — no realloc. See
§10.2.

### Audio control seams (Phase 4)

The current audio path has two useful control points, and they solve different
problems:

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

### 4.1 The save backend / `APPLY_SAVE` path

Save edits are structurally unlike cheats. A cheat pins WRAM every frame; a save
edit is a **transactional mutation of the battery-SRAM image**, consumed by the
game only when it next loads the save. Disk format is a separate concern.

**Current hook and Phase-6 target.** Today `RtlReadSram()`
(`src/main.c:544`) directly loads `saves/save.srm` into `g_sram`, and
`RtlWriteSram()` writes that path. Phase 6 replaces direct file ownership with
`Save_LoadActive()` / `Save_WriteActive()`: the selected backend decodes either
native 8 KiB SRAM or the lossless INI schema in save-format.md §4 into the same
canonical `g_sram`. The game and HLE code continue to see exactly the same
buffer.

`Settings_ApplySaveEdits()` runs after the active backend has loaded — before
the game boots — so a seeded `AR_SAVE_*` value can be live for the whole session
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
   is not mistaken for a game-originated save — see the hazard below.

**⚠️ Hazard: auto-persist clobbers the user's real save.** `src/main.c:811-830`
diffs `g_sram` every frame and calls `RtlWriteSram()` the moment it changes,
overwriting `saves/save.srm` (deliberately — so progress survives a freeze). In
Phase 6 the same behavior must call `Save_WriteActive()` and persist only the
selected `.srm` or `.ini` target. A save edit otherwise trips this instantly and
silently replaces the user's active save. `Settings_ApplySaveEdits()` **must**
re-sync that shadow buffer after swapping the image (so the edit is not misread
as a game write), in addition to the backup/explicit commit choice above. This
is the single biggest implementation risk in the feature.

**Verification gate.** No save field ships to the menu until a round-trip proves
the game accepts our recomputed checksum (save-format.md §6.3), and only fields
marked ✅ there may be exposed at all. Separately, the INI codec must prove an
unedited `.srm → .ini → .srm` round trip is byte-identical before it can become
an active persistence backend.

---

## 5. Complete settings inventory (registry contents)

The full set of user-facing settings the menu should expose. Booleans note their
default polarity (cheats default **off** = `(e && e[0]!='0')`; widescreen
quality knobs default **on** = `!(e && e[0]=='0')`). "Avail." = the
`available()` predicate.

### Cheats (`CAT_CHEATS`) — all PASSIVE

“Passive” means disabling stops future enforcement; it does not generally
rewind game history. All Magic is explicitly marked sticky because its unlock
bytes may already have been consumed as legitimate-looking progress. Infinite
HP's auto high-water and Freeze Timer's capture/drain latch reset when their
live values are disabled/re-enabled. Leaving full No Knockback clears only the
i-frame timer/flag that mode owns; experimental raw-offset pins cannot safely
restore an unknown prior byte.

| Setting | env | Type | Default | Avail. | Effect / RAM |
|---|---|---|---|---|---|
| All Magic | `AR_ALL_MAGIC` | bool | off | all modes | unlock 4 spells `$0299-$029C` |
| Ranged Sword | `AR_RANGED_SWORD` | bool | off | all modes | `$00E4=$80` projectile |
| Infinite MP | `AR_INF_MP` | int | off (`1`→10) | all modes | pin scroll count `$21` |
| Infinite SP | `AR_INF_SP` | bool | off | all modes | `$0282/83 = $0284/85` (self-cal) |
| Angel HP | `AR_ANGEL_HP` | bool | off | all modes | `$0286 = $0287` (self-cal) |
| Infinite HP | `AR_INF_HP` | int | off (`1`=auto) | action | pin `$1D` |
| Freeze Timer | `AR_FREEZE_TIMER` | bool | off | action | pin `$E6/$E7`; auto-release on boss drain |
| Moonjump | `AR_MOONJUMP` | int px/f | off (`1`→6) | action | move `$08A4` up while btn held |
| — Moonjump button | `AR_MOONJUMP_BTN` | mask | `$8000` (B) | action | sub-setting of Moonjump |
| No Knockback | `AR_NO_KNOCKBACK` | int | off (`1`=full) | action | invuln `$08C6`/`$08D1` (magic-safe) |
| Custom codes | `AR_PIN` | custom | empty | all modes | up to 32 PAR `7Exxxxvv` pins/frame |

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
| HD replacements | `AR_HD_REPLACEMENTS` | bool | on | PASSIVE; inert when art is absent |
| Master volume | `AR_AUDIO_VOLUME` | int percent | 100 | CALLBACK; atomic post-mix gain, live `0..100` in steps of 5 |
| Dialogue text blip | `AR_DIALOG_BLIP` | bool | on | PASSIVE; exact `$01:902D` COP request only |
| Extended aspect | `ExtendedAspectRatio` / `AR_EXTENDED_ASPECT_RATIO` | custom `off` or X:Y | off | RESTART (§10.2) |
| Pixel aspect | `AspectPAR` / `AR_ASPECT_PAR` | enum 4:3/square | 4:3 | RESTART |
| Window scale | `WindowScale` / `AR_WINDOW_SCALE` | int 1..8 | 3 | CALLBACK |
| Fullscreen | `Fullscreen` / `AR_FULLSCREEN` | bool | off | CALLBACK; desktop-fullscreen |
| New renderer | `NewRenderer` / `AR_NEW_RENDERER` | bool | on | RESTART |
| Ignore aspect | `IgnoreAspectRatio` / `AR_IGNORE_ASPECT_RATIO` | bool | off | CALLBACK |
| Enable audio | `EnableAudio` / `AR_ENABLE_AUDIO` | bool | on | CALLBACK; lazily opens then pauses/resumes the device |
| Audio freq | `AudioFreq` / `AR_AUDIO_FREQ` | int | 44100 | RESTART |
| Audio samples | `AudioSamples` / `AR_AUDIO_SAMPLES` | int | 2048 | RESTART |
| Turbo multiplier | `AR_TURBO_MULT` | int | 8 | ACTION param (T key) |
| Warp target | `AR_WARP` | custom | `0101` | ACTION raw region/map param (F6); populate choices from README's verified table, including broken `0701` status |

### Save storage and editing (`CAT_SAVE`)

Only ✅-verified fields (save-format.md §3.1). Everything else in the
third-party map stays unexposed until promoted (save-format.md §6). The active
disk backend changes serialization only; both formats decode to the same 8 KiB
`g_sram` image (save-format.md §4).

| Setting | env | Type / apply | Default | Note |
|---|---|---|---|---|
| Save storage format | `AR_SAVE_BACKEND` | enum / RESTART | `native-srm` | `native-srm` or `ini`; boot-selects exactly one authoritative backend/path |
| Arm save editing | `AR_SAVE_EDIT` | bool / APPLY_SAVE | **off** | master gate; field edits cannot touch `g_sram` unless set |
| Auto-backup | `AR_SAVE_BACKUP` | bool / PASSIVE | **on** | timestamped backup of the active format before first persistent edit; see §4.1 |
| Import save | — | ACTION | — | decode `.srm`/`.ini` into scratch, validate, confirm, back up, then swap; does not silently change backend |
| Export native SRAM | — | ACTION | — | write an emulator-compatible exact 8 KiB `.srm` without changing the active backend |
| Export structured INI | — | ACTION | — | write the lossless versioned schema from save-format.md §4.1 without changing the active backend |
| Fillmore progress | `AR_SAVE_PROG_FILLMORE` | enum / APPLY_SAVE | leave as-is | SRAM `0x1200` |
| Bloodpool progress | `AR_SAVE_PROG_BLOODPOOL` | enum / APPLY_SAVE | leave as-is | SRAM `0x1202` |
| Kasandora progress | `AR_SAVE_PROG_KASANDORA` | enum / APPLY_SAVE | leave as-is | SRAM `0x1204` |
| Aitos progress | `AR_SAVE_PROG_AITOS` | enum / APPLY_SAVE | leave as-is | SRAM `0x1206` |
| Marahna progress | `AR_SAVE_PROG_MARAHNA` | enum / APPLY_SAVE | leave as-is | SRAM `0x1208` |
| Northwall progress | `AR_SAVE_PROG_NORTHWALL` | enum / APPLY_SAVE | leave as-is | SRAM `0x120a` |

Progress enum: `act1` (`0x00`) · `active` (`0x01`, observed in our saves but
absent from the third-party enum — see save-format.md §3.1) · `act1-cleared`
(`0x02`) · `act2` (`0x03`) · `act2-cleared` (`0x04`). A seventh region byte
(Death Heim, `0x120c`) is **not** exposed: it reads 0 in every save we have, so
its encoding is untested.

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
- Phase 5 will invoke `Settings_Save("settings.ini")` after accepted menu
  mutations. Existing diagnostic hotkeys and `AR_SETTING_SET` remain
  session-only so automated probes do not silently rewrite user preferences.
- Restart-class rows store the desired/persisted value in `g_settings`, while
  already-created renderer/PPU/audio resources consume a boot snapshot. This
  prevents a pending aspect/audio-format change from half-applying through an
  unrelated live callback.
- `Save_LoadActive()` / `Save_WriteActive()` independently serialize the exact
  8 KiB `g_sram` image using the backend selected by those resolved settings.
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
4. **Phase 4 — persistence + Config merge (implemented).** The 34-row registry
   owns every wired display/aspect/audio field. `config.ini` is a lower-priority
   compatibility layer, `settings.ini` load/save is descriptor-driven and
   atomic, real env values remain distinguishable and highest-priority, and
   restart-class host resources use boot snapshots. Window scale, fullscreen,
   ignore-aspect, audio enable, master volume, and dialogue blip have live apply
   paths. Independent music/SFX gain remains research-gated by the audio-seam
   criteria above; dead template config keys remain excluded rather than being
   promoted as fake settings.
5. **Phase 5 — overlay UI.** Add the host overlay at the §3.4 present/input seam.
   Render rows from `SettingDesc[]`, write `g_settings`, intercept menu input,
   pause game-frame advancement while open, and expose ACTION commands without
   duplicating their existing hotkey implementations.
6. **Phase 6 — save codec, backends, and editor (`CAT_SAVE`).** Independent of
   Phases 1–5 and schedulable after the current regeneration settles: it owns
   `g_sram` persistence, not the cheat gates. Split it into reviewable pieces:

   - **6A — codec core.** Promote the scratch analysis to `tools/srm.py`
     (save-format.md §6.4); implement exact-size native SRAM decode/encode,
     `SramChecksum()`, `SaveFieldDesc[]`, validation/errors, and fixture tests.
   - **6B — lossless INI.** Implement the version-1 chunked-hex schema from
     save-format.md §4.1. Require all raw chunks, apply verified readable-field
     overrides, and reject invalid input transactionally. Prove every fixture
     survives `.srm → .ini → .srm` byte-identically with no edits.
   - **6C — active backend integration.** Route boot load and per-frame
     auto-persist through `Save_LoadActive()` / `Save_WriteActive()`; keep
     native `.srm` as default, make INI explicit, write atomically, log the
     selected target, take backups, and expose a real shadow-buffer re-sync API.
   - **6D — editing and conversion actions.** Implement the §4.1 safety
     sequence; round-trip one region-progress edit through the game
     (save-format.md §6.3); expose the six verified region fields plus explicit
     Import, Export `.srm`, Export `.ini`, Apply for session, and Apply and save
     actions in the overlay/headless path.
   - **6E — field promotion.** Extend `SaveFieldDesc[]` one field at a time only
     after WRAM correspondence, known-state diffing, and a game round trip.

   Phase-6 acceptance requires: malformed/truncated INI never changes `g_sram`;
   an unedited cross-format round trip preserves all 8192 bytes including fill
   and unknown town state; a named-field edit changes only its verified bytes
   plus `0x1fec`–`0x1fef`; backend choice is deterministic when both files
   exist; auto-persist writes only the active target; and a save written by each
   backend is accepted by the game.

---

## 9. Verification (project bar)

- **Golden regression:** for each cheat, run the old env path vs the new seeded
  struct and assert `g_ram` effects are identical via the WRAM oracle / `F2`
  snapshot. Since seeding uses the same env vars, an `AR_ALL_MAGIC=1` run must
  match byte-for-byte.
- **Drift guard:** `tests/settings_test.c` asserts 21 unique keys and storage
  fields and exercises lookup/formatting for the current registry.
- **Live-path test:** `AR_SETTING_SET=key=value AR_SETTING_AT_GF=N` applies via
  the same mutation API the overlay will call; observe enforcement at N+1.
- **Save-codec tests (Phase 6):** exact 8192-byte native round trip; fixture
  `.srm → .ini → .srm` identity; malformed/missing/duplicate INI chunks rejected
  without touching the destination; named-field override changes only its
  verified bytes plus checksum; atomic-write failure leaves the previous save
  readable; and both backends pass a real game load/save/load round trip.
- **Clean dev workflow:** `cycle.sh`, the `config.ini` AR_ bridge, and all debug
  flags (§11) stay on `getenv`. Migrate only user-facing settings — do
  not boil the ocean.

---

## 10. Open design decisions

Marked pending; defaults recommended but not yet confirmed with the user.

### 10.1 Persistence target
Separate `settings.ini` (**recommended** — keeps dev `config.ini` clean) vs. a
single merged file (simpler, but menu writes clobber hand-authored comments).

### 10.2 RESTART settings (aspect / renderer / audio-format)
Start with a **"pending restart" badge** (**recommended** — trivial, safe) vs.
investing in live realloc up front. Intermediate for aspect specifically:
allocate at max budget on boot + clamp columns live (§4), which avoids realloc
entirely and makes aspect on/off effectively PASSIVE.

### 10.3 Phase-1 scope
Migrate only the user-facing ~20 (**recommended**) vs. also wrapping the debug
flags into the struct for uniformity (larger blast radius, low payoff).

### 10.4 Overlay frontend
Start with a minimal SDL-rendered immediate UI plus bundled bitmap font
(**recommended** — matches the current C/`SDL_Renderer` pipeline) vs. first
integrating the existing RmlUi launcher stack (richer styling, but requires its
C++/OpenGL/dependency build path).

### 10.5 Default save-edit action
Transactional scratch decoding is required by the Phase-6 codec. The remaining
UI decision is which explicit action gets primary placement: **Apply for this
session** (**recommended** — swaps validated SRAM and re-syncs the shadow but
does not alter disk) vs. **Apply and save** (backs up and atomically commits via
the active backend). Both actions should exist; neither should be triggered by
merely changing a row. Persistent edits require the §4.1 backup sequence and
the save-format.md §5.1 safeguards.

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
