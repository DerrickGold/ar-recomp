# ActRaiser Recomp — Settings & Live-Config System (design)

Design for a single source of truth that captures every custom setting (cheats,
widescreen knobs, display/audio, QoL) and defines each one's **path to live
runtime updates**, so a future in-game overlay menu can flip them mid-run. This
is the "option B" refactor from the 2026-07-12 settings scan.

Status: **NEXT IMPLEMENTATION PHASE — design only, not yet implemented.** Companion docs:
[SEAMS.md](SEAMS.md) (gameplay/tunable seams the cheats hook),
[rendering-engine.md](rendering-engine.md) (widescreen policy internals),
[ram-map.md](ram-map.md) (the WRAM addresses the cheats pin).

Source sites this design replaces: `ActRaiser_ApplyCheats`
(`src/actraiser_rtl.c:607`), `ActRaiser_ApplyWidescreenPolicy`
(`src/actraiser_rtl.c:327`), the widescreen sprite/BG builders
(`src/actraiser_widescreen_sprites.c`, `src/actraiser_widescreen_bg.c`), the
`Config` struct + ini parser (`src/config.c`), and the boot flag init block in
`src/main.c` (~`:355`–`:470`).

---

## 1. Current state (what we're refactoring)

Two independent config surfaces exist today, and a setting's toggleability is
decided by **where its value is consumed**, not which surface it comes from.

1. **`Config g_config` struct** (`src/config.c`) — parsed once from `config.ini`
   at boot. Feeds window/renderer/audio init and the widescreen budget math.
2. **`AR_*` env vars** — read via `getenv()` throughout the runtime.
   `config.ini` also bridges any `AR_*` key into the environment via `setenv()`
   (`src/config.c:53`), so the two surfaces overlap.

The decisive pattern: **almost every cheat / widescreen flag is *enforced every
frame* but its env value is *read once and cached*** in a `static int en = -1`
gate. Example (`src/actraiser_rtl.c:665`):

```c
static int en = -1;
if (en < 0) { const char *e = getenv("AR_ALL_MAGIC"); en = ...; }  // read ONCE
if (en) { g_ram[0x0299] = 0x01; ... }                              // applied EVERY frame
```

Because enforcement is already per-frame, the machinery for live toggling is
*already present*. Converting a gate to a live toggle means only replacing the
cached `en` with a field the menu writes — no restructuring of the enforcement.
A few widescreen flags (`AR_WS_ACTION` `:414`, `AR_WS_CLAMP` `:463`,
`AR_WS_ONLYBG`, `AR_WS_LAYERS`) already read `getenv` uncached every frame, so
they are effectively live today.

---

## 2. Goals / non-goals

**Goals**
- One typed struct (`g_settings`) as the live source of truth.
- One descriptor table (`SettingDesc[]`) that drives env-seeding, ini load/save,
  menu rendering, and live-apply — no per-field duplication across those four.
- A host-side in-game overlay that renders settings without consuming SNES
  VRAM/OAM or changing emulated game state.
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
  bool ws_action, ws_bgrefresh, ws_sprites,
       ws_margin_objects, ws_margin_activation, ws_bg2_padding;
  int  ws_clamp_override;      // -1 = none, else per-layer clamp mask

  /* Aspect / render budget (restart-class) */
  uint8 aspect_x, aspect_y;    // 0:0 = off (authentic 4:3)
  bool  aspect_par_43;         // true = 4:3 PAR stretch, false = square pixels

  /* Display / audio (restart-class) */
  uint8  window_scale;
  bool   fullscreen, new_renderer, ignore_aspect_ratio, audio_enabled;
  uint16 audio_freq, audio_samples;

  /* QoL params (values behind the existing live hotkeys) */
  int    turbo_mult;           // T key multiplier (default 8)
  uint16 warp_target;          // F6 raw region/map; use README's verified table
} Settings;
extern Settings g_settings;
```

### 3.2 `SettingDesc[]` — the descriptor registry (**the capture/store answer**)

One row per setting captures everything: storage location, type, seed source
(env + ini key), category, default/range, availability, and — the key field —
its **live-update path** (`ApplyKind`). This one table is the input contract for
env-seeding, ini load/save, the menu, and live-apply.

```c
typedef enum { SET_BOOL, SET_INT, SET_ENUM, SET_MASK, SET_CUSTOM } SettingType;
typedef enum { APPLY_PASSIVE, APPLY_CALLBACK, APPLY_RESTART, APPLY_ACTION } ApplyKind;
typedef enum { CAT_CHEATS, CAT_WIDESCREEN, CAT_ASPECT, CAT_DISPLAY,
               CAT_AUDIO, CAT_QOL } SettingCat;

typedef struct SettingDesc {
  const char *key;        // "cheat_all_magic"  (ini key + menu id)
  const char *env;        // "AR_ALL_MAGIC"     (seed + back-compat; NULL if none)
  const char *label, *tooltip;
  SettingCat  cat;
  SettingType type;
  ApplyKind   apply;
  void       *field;      // &g_settings.cheat_all_magic
  long        imin, imax, istep, defval;
  const char **enum_labels;             // SET_ENUM
  bool (*available)(void);              // e.g. action-mode-only; NULL = always
  void (*on_change)(void);             // APPLY_CALLBACK/RESTART side-effect; NULL = PASSIVE
  bool (*parse)(const char *, void *); // SET_CUSTOM escape hatch (PIN, MOONJUMP, ...)
  int  (*format)(char *, size_t, const void *);
} SettingDesc;
extern const SettingDesc g_setting_descs[];
extern const int g_setting_desc_count;
```

A `static_assert` on the row count plus a unit test that every struct field has
exactly one descriptor guards against struct/table drift.

### 3.3 Value resolution (the seed path)

Precedence, applied **once at boot** by `Settings_Init()`, called immediately
after `ParseConfigFile()` (`src/main.c:358`):

```
built-in default  <  config.ini  <  env var  <  (runtime: live menu writes)
```

- `Settings_Init` walks `g_setting_descs[]`: `getenv(desc->env)` wins if set
  (this preserves every existing `AR_*` dev workflow *and* the `config.ini` AR_
  bridge, which merely `setenv`s); else the parsed ini value; else `defval`.
- After boot the **live struct is authoritative** — env/ini only seeded it. The
  menu mutates the struct and never touches the environment again.

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

---

## 4. Live-update paths (`ApplyKind` taxonomy)

Every setting resolves to exactly one path, which *is* how the menu updates it
live:

| Path | Mechanism | Settings | Cost |
|---|---|---|---|
| **PASSIVE** | Existing gate reads the field every frame. Menu writes struct → next frame reflects it. No extra code. | all 10 cheats, all 6 widescreen-behavior bools, `ws_clamp_override` | free |
| **CALLBACK** | `on_change()` fires one SDL/PPU call. `window_scale`→`SDL_SetWindowSize`; `fullscreen`→`SDL_SetWindowFullscreen`; `audio_enabled` mute→`SDL_PauseAudioDevice`; `ignore_aspect_ratio`→renderer logical-size. | window_scale, fullscreen, ignore_aspect_ratio, audio mute | small |
| **RESTART** | Needs realloc/reinit unsafe to do cheaply mid-frame. Per setting: attempt heavy live reinit, or set a "pending — applies on relaunch" badge (§10.2). | aspect on/off + ratio (window + PPU buffer realloc), `new_renderer`, audio freq/samples (device reopen; audio thread) | real work |
| **ACTION** | Not stored toggles — commands the menu invokes; only the *param* is stored. | warp (`warp_target`, F6), turbo (`turbo_mult`, T), savestate (F5/F7), snapshot (F2), pause (P) | reuse existing hotkey paths (`src/main.c:587`) |

The single change that lights up the whole gameplay/widescreen menu is
converting the ~17 PASSIVE gates:

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

---

## 5. Complete settings inventory (registry contents)

The full set of user-facing settings the menu should expose. Booleans note their
default polarity (cheats default **off** = `(e && e[0]!='0')`; widescreen
quality knobs default **on** = `!(e && e[0]=='0')`). "Avail." = the
`available()` predicate.

### Cheats (`CAT_CHEATS`) — all PASSIVE

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
| Wide action stages | `AR_WS_ACTION` | bool | on | master toggle; already uncached/live |
| BG margin refresh | `AR_WS_BGREFRESH` | bool | on | true-content margins vs stale/wrapped |
| Widen sprites | `AR_WS_SPRITES` | bool | on | emit sprites into margins |
| Draw margin objects | `AR_WS_MARGIN_OBJECTS` | bool | on | object draw coverage in margins |
| Extend activation | `AR_WS_MARGIN_ACTIVATION` | bool | on | `$0400` activation boundary |
| Decorative BG2 padding | `AR_WS_BG2_MIRROR` | bool | on | stage policy chooses reflection or cyclic repeat; off clamps the 256-wide BG2. Keep env name for compatibility |
| Clamp override | `AR_WS_CLAMP` | mask | none | manual per-layer mask; already uncached/live |

### Aspect / display / audio / QoL

| Setting | Source | Type | Default | Apply |
|---|---|---|---|---|
| Extended aspect | `ExtendedAspectRatio` (ini) | enum off/16:9/16:10 | off | RESTART (§10.2) |
| Pixel aspect | `AspectPAR` (ini) | enum 4:3/square | 4:3 | RESTART |
| Window scale | `WindowScale` (ini) | int | 3 | CALLBACK |
| Fullscreen | `Fullscreen` (ini) | bool | off | CALLBACK — *note: parsed but unused today (§11)* |
| New renderer | `NewRenderer` (ini) | bool | on | RESTART |
| Ignore aspect | `IgnoreAspectRatio` (ini) | bool | off | CALLBACK |
| Enable audio | `EnableAudio` (ini) | bool | on | CALLBACK (mute) / RESTART (reopen) |
| Audio freq | `AudioFreq` (ini) | int | 44100 | RESTART |
| Audio samples | `AudioSamples` (ini) | int | 2048 | RESTART |
| Turbo multiplier | `AR_TURBO_MULT` | int | 8 | ACTION param (T key) |
| Warp target | `AR_WARP` | custom | `0101` | ACTION raw region/map param (F6); populate choices from README's verified table, including broken `0701` status |

---

## 6. Persistence

- `Settings_Save(path)` / `Settings_Load(path)` iterate `g_setting_descs[]` using
  each row's `format`/`parse`, so adding a setting never touches the serializer.
- **Recommendation (see §10.1):** write a **separate `settings.ini`**, loaded
  *after* `config.ini` in the file tier, leaving the dev-authored `config.ini`
  and its comments untouched. The menu owns `settings.ini`; devs own
  `config.ini`; env still overrides both. Load order becomes:
  `defaults → config.ini → settings.ini → env → live`.

---

## 7. Threading / safety model

The game runs as a cooperative coroutine via `swapcontext`
(`RunOneFrameOfGame`, `src/actraiser_rtl.c:879`) on the **same thread** as the
SDL event loop (`src/main.c` main loop). The menu mutates `g_settings` *between*
frames; the game reads it on the *next* frame. **No locks or atomics are needed**
for PASSIVE/CALLBACK/ACTION settings.

The one exception: SDL's audio callback runs on a separate audio thread
(`SDL_OpenAudio`, `src/main.c:556`). This is precisely why audio-format settings
are RESTART (device close/reopen, which SDL serializes) rather than live-poked —
never mutate audio params the callback reads from the main thread.

---

## 8. Phased rollout

1. **Phase 1 — struct + seed, no menu.** Add `src/settings.{c,h}`, the `Settings`
   struct, descriptor rows for the 17 PASSIVE flags, and `Settings_Init` seeding
   from env. Convert those 17 gates to read the struct. *Verify byte-identical*
   cheat behavior vs the old getenv path (§9). Highest-value, highest-risk step.
2. **Phase 2 — prove live mutation headless.** A debug path (reuse the hotkey /
   a console command) that sets a field at frame N and confirms the effect at
   N+1. No UI yet; de-risks the live path before menu work.
3. **Phase 3 — descriptor metadata + apply kinds.** Fill labels/tooltips/ranges/
   `available()`/`on_change`. Wire CALLBACK settings; add the "pending restart"
   badge for RESTART.
4. **Phase 4 — persistence + Config merge.** `settings.ini` save/load; fold the
   `g_config` display/audio/aspect fields into `Settings` so there is one source
   of truth.
5. **Phase 5 — overlay UI.** Add the host overlay at the §3.4 present/input seam.
   Render rows from `SettingDesc[]`, write `g_settings`, intercept menu input,
   pause game-frame advancement while open, and expose ACTION commands without
   duplicating their existing hotkey implementations.

---

## 9. Verification (project bar)

- **Golden regression:** for each cheat, run the old env path vs the new seeded
  struct and assert `g_ram` effects are identical via the WRAM oracle / `F2`
  snapshot. Since seeding uses the same env vars, an `AR_ALL_MAGIC=1` run must
  match byte-for-byte.
- **Drift guard:** `static_assert(count == N)` + a test that every struct field
  maps to exactly one descriptor.
- **Live-path test:** the Phase-2 headless "set at N, observe at N+1."
- **Clean dev workflow:** `cycle.sh`, the `config.ini` AR_ bridge, and all debug
  flags (§11) stay on `getenv`. Migrate only the ~20 user-facing settings — do
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
`LinearFiltering`, `output_method`, `enable_gamepad[2]`, `gamepad_deadzone`,
`skip_launcher`, `autosave`, and `Fullscreen` (parsed, but no
`SDL_SetWindowFullscreen` call exists yet — the §5 CALLBACK wiring must be added
for it to work).
