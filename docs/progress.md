# ActRaiser Recomp — Progress Tracker

Tracks playability by **action level** and **simulation-mode town**, plus
cross-cutting **major functionality**. Replaces the old phase-checklist
version of this doc, which tracked recompiler-tooling setup milestones and
was accurate early on but stopped reflecting reality once the game itself
became the main focus (see `DEBUG.md` for that history if it's ever useful).

**How to keep this honest:** only mark something ✅/🟡 once it's actually
been played, not once the code "should" work. Everything below that hasn't
been personally exercised is marked ⬜ Untested — that's the accurate
starting state as of this doc's creation (2026-07-03), not a guess. Update
rows as you actually play through them, and note the date + what was
verified, same as the other status markers.

**Legend:** ✅ Confirmed working · 🟡 Playable with known issues (see note)
· 🔴 Broken/blocking · ⬜ Untested

## Action-widescreen milestone — 2026-07-12

Every action level in regions `$01-$06` has now been played through and is
confirmed fully playable with widescreen enabled. Across those passes:

- BG streaming, fast vertical traversal, sprites, old-edge activation, enemy and
  platform behavior, bosses, room transitions, and the observed HDMA/parallax
  effects render and behave correctly;
- the historical extra/partial sprite corruption did not recur after the BG
  refresher was isolated from emulated WRAM/OAM state;
- decorative 256px BG2 layers use stage-appropriate presentation padding:
  reflection where the art tolerates it, cyclic repeat for Aitos and Northwall
  cloud/snow raster effects, and an explicit clamp fallback;
- the handler-coverage batch restored the inert objects discovered by the wider
  playthroughs. Those were pre-existing recompilation gaps, not widescreen
  regressions.

One action-mode item remains outside this milestone. Death Heim (`$07`/`70X`),
the distinct boss-rush/final-boss flow, is done: user-verified end-to-end on
2026-07-14, playing through every boss to the end (see the region table).
Regions `$01-$06` still need a final presentation-aware camera/world-edge clamp
so the ends of finite background maps cannot scroll into the wider viewport.
That edge exposure is the only known gap between their current fully playable
state and complete widescreen presentation.

No generated ROM-derived source is committed; reproducible builds materialize
the registered handlers from `recomp/*.cfg`.

## Action stages — observed status

ActRaiser's 6 kingdoms each have two action (side-scrolling) stages, played
between rounds of that kingdom's sim-mode town. Region/act numbering below
follows the game's internal order (`$18`/`$19` in WRAM — see `docs/SEAMS.md`
"Game-state anchors"), region name order from the town-dispatch table at
`$03:F5ED`.

| Region | Act 1 | Act 2 |
|---|---|---|
| 1 — Fillmore | ✅ Full widescreen playthrough; sprites and activation clean (2026-07-12) | ✅ Full widescreen playthrough; fast vertical fall/row streaming confirmed repaired (2026-07-12) |
| 2 — Bloodpool | ✅ Full widescreen playthrough; narrow BG2 policy clean (2026-07-12) | ✅ Full widescreen playthrough; enemies, moving platforms, mirror padding, and boss confirmed (2026-07-12) |
| 3 — Kasandora | ✅ Full widescreen playthrough; generated runtime handlers and rendering confirmed (2026-07-12) | ✅ Full widescreen playthrough (2026-07-12) |
| 4 — Aitos | ✅ Full widescreen playthrough; cyclic BG2 cloud padding removes the parallax seam (2026-07-12) | ✅ Full widescreen playthrough (2026-07-12) |
| 5 — Marahna | ✅ Full widescreen playthrough (2026-07-12) | ✅ Full widescreen playthrough (2026-07-12) |
| 6 — Northwall | ✅ Full widescreen playthrough; cyclic BG2 cloud/snow padding confirmed across the affected maps (2026-07-12) | ✅ Full widescreen playthrough and boss completion (2026-07-12) |
| 7 — Death Heim | ✅ Full boss-rush playthrough to the end — entry, every boss fight, victory teleport-outs, hub warps, and the final boss all user-verified (2026-07-14) | — No Act 2 |

Static code confirms `$18=$07` selects its own handler table at `$00:F39A`.
The game structure was confirmed on 2026-07-12: Death Heim has no ordinary acts;
it teleports through all six act-2 bosses, then transitions to the final boss.
The 2026-07-14 repair (DEBUG.md §7.20 / docs/bug-ledger.md) fixed one crash and
one silent soft-lock, both unregistered yield-helper continuations; the `$19`
flow, derived from code and confirmed through the boss warps, is: `$19=1` hub →
bosses at `$19 = $0347+2` (progress counter `$0347 = beaten map - 1`, written by
`$00:FEEC`, which also stages the hub warp via `LDA #$0701; STA $1A`) → `$19=8`
final boss (sets `$0334=1`). The all-six-regions-complete path
(`$00:A343` checking `$7F:6B18`) is the separate post-rush exit.

### Remaining action-mode completion gate

The broad region `$01-$06` matrix is complete. Remaining action work is:

1. Map the native camera clamps and add widescreen-aware left/right limits so
   finite background edges never enter the visible margins. Exercise at least
   one wide streamed BG, one 256px padded BG2, and one vertical stage.
2. ~~Diagnose the `0701` crash after its first boss teleport~~ — done 2026-07-14
   (yield-helper continuations; docs/bug-ledger.md #20), and the full rush
   through the final boss (`$19=8`) is user-verified. There is no ordinary
   `0702`. Optional residue: the all-six-regions exit variant (`$00:A343` over
   `$7F:6B18`) fires only on a save with every kingdom's act 2 complete —
   exercise it during a full-game playthrough.
3. Re-run representative `$01-$06` boundaries with the feature-disable gates
   after the camera change. Add a lifted-limit A/B only after `NoSpriteLimits`
   is actually forwarded to the PPU render flags.

For Death Heim or any future anomaly:

- play from start through boss/transition, exercising both old 256px edges;
- take F2 snapshots at a dense encounter, a room/route transition, and the boss;
- exit/F9 while any object is inert or behavior is suspect so the 1,024-event
  dispatch ring retains the onset; use `AR_DISPMISSALL=1` only for a focused
  follow-up because ordinary `$896F` loop returns are benign noise;
- feed snapshots to `tools/find_handler_chain.py --snapshot ...`, and compare
  non-benign `found:0` roots against generated `bank_00_*` entries;
- record visual BG/sprite/activation results separately from handler coverage.
  A drawable or killable but inert object is usually a missing behavior handler,
  not proof of a widescreen rendering fault;
- current ActRaiser builds always use authentic scanline limits because the
  parsed `NoSpriteLimits` field is not forwarded to `PpuBeginDrawing`; wire that
  setting before attempting a lifted-limit A/B comparison;
- if a symptom might be caused by the action→action warp, reproduce with
  `AR_WS_ACTION=0` or natural progression before classifying it.

Testing caveats: recorded input needs the same cheat state used when it was
captured, and `AR_NO_KNOCKBACK=1` suppresses authentic water drag.

## Simulation mode — town verification matrix

| Town | Status |
|---|---|
| Fillmore | ✅ clean full round (act1 → sim → act2, 2026-07-07): development cycles, story events (rock zap/fire), lair sealing with all cutscene actors, reward grants (scroll persists), offerings |
| Bloodpool | 🟡 Entry and two-person lightning cutscene confirmed; full development/events/rewards/lair sealing and sim→act-2 transition remain |
| Kasandora | ⬜ Full authentic baseline needed before any widescreen town work |
| Aitos | ⬜ Full authentic baseline needed before any widescreen town work |
| Marahna | ⬜ Full authentic baseline needed before any widescreen town work |
| Northwall | ⬜ Full authentic baseline needed before any widescreen town work |

The sim-mode *engine* itself (town dispatch, spawn/behavior systems — see
`docs/SEAMS.md`'s "Sim-mode town architecture" section) is shared code across
all 6 towns, so a fix verified in Fillmore likely applies everywhere — but
"likely applies" isn't "confirmed," hence still ⬜ for the other five until
someone actually plays them.

For **each** incomplete town, the baseline pass must cover:

1. act-1→sim entry and initial population/state;
2. camera at left, center, and right map edges;
3. builders, people, monsters, lairs, and ordinary development cycles;
4. town-specific disaster/story events and their multi-actor cutscenes;
5. offerings/rewards and persistence of granted items/stat upgrades;
6. lair sealing, dialogs/temple, Sky Palace staging (margin decoder directly
   validated 2026-07-13 — byte-identical to the boot colonnade; re-check per
   town only if a palace state looks off), and sim→act-2 transition;
7. an exit dispatch ring plus F2 snapshots around any partial actor, silent
   event, frozen builder, or missing reward.

Run these authentic-geometry baselines before changing town rendering. The
existing partial-town-actor symptom must be captured as a baseline rather than
silently attributed to future widescreen code.

## Remaining proper-widescreen roadmap

The runtime settings/overlay plan in
[settings-system.md](settings-system.md) is complete through Phase 6, with one
manual save-acceptance matrix pending. Its 101-row registry
owns every wired application/game setting, resolves
`config.ini < settings.ini < environment`, persists atomically, and exposes
live/restart apply metadata. The SDL host overlay renders descriptor-driven
categories and rows, freezes game advancement while open, consumes SNES input,
saves accepted changes, and scales independently. Escape/F1 opens it globally
from every game state before emulated input dispatch. The authentic 2bpp font,
selector, and three-panel dialog frame are decoded from the user-supplied ROM
into full-window host atlases with independently scaled contents. Custom rows
support validated text entry, and Extras/Inspector actions reuse the pause,
turbo, save/load-state, warp, snapshot, complete scene-asset dump, restart, and
exit paths. Screen ratio is an explicit
4:3/16:9/16:10 cycle; ratio, pixel aspect, and renderer selection now rebind
preallocated video surfaces live. Enhanced music now adopts or releases the
currently playing song immediately, and audio frequency cycles through
32.04/44.1/48 kHz presets. Only audio format rows retain restart markers. A
native menu entry remains optional; gamepad support is tracked separately with
the general input system. The guarded Save editor can load/persist native or
lossless INI SRAM and stage Progress, Status, Magic, Items, and Scores pages,
including the six combined town states and the enabled fields represented by
the reference editor. Import/export does not change the active backend, and
session-only changes remain distinct from explicit writes. Automated codec/
backend tests pass; the outstanding manual gate is representative Apply and
save → Restart Game → Continue checks across those pages.
The remaining widescreen backlog is:

1. **Finish action presentation.** Implement the camera/world-edge clamp above.
   The separate Death Heim `70X` flow is already repaired and directly
   validated through its boss rush, final boss, and return transition.
2. **Freeze simulation baselines.** Complete Bloodpool and all four untested
   towns using the town matrix. The old `simdev.rec`/`lairseal.rec` files no
   longer reach a town viewport from the current SRAM, so new direct captures
   are required; use `AR_WS_SIM=0` for authentic geometry.
3. **Widen simulation backgrounds safely — implemented and directly validated.**
   Modes `$00:$01-$06` derive margins from `$01:B4C6` camera bounds, clamp at
   the 512px map edges, keep BG2/dialogs centered, clear framebuffer gaps, and
   provide the same-binary `AR_WS_SIM=0` switch. Fillmore direct BG testing on
   2026-07-14 confirmed clean clamped edges with no odd tile exposure.
   Bloodpool was captured as `$00:$02`, exposing and then repairing the original
   Fillmore-only gate. A faithful `$01:B4C6` HLE constrains the
   corrected-wide camera to `[extra,$0100-extra]`, eliminating cleared edge
   gaps instead of merely rendering them black. Direct testing on 2026-07-14
   confirmed the camera clamp works as expected in simulation mode. Bloodpool
   plus modes `$03-$06` remain full content-pass targets.
4. **Widen simulation world sprites — enemy composition and angel projectile
   validated.** The regenerated faithful `$01:ADAD/$01:AE6F` ports extend
   horizontal emission only for `$0A00-$1087` world records; the 2026-07-14
   direct run confirmed complete enemy sprites in both margins. The angel arrow
   is already world record `$0B0A`, but its state-2 update `$01:B44B` destroys
   it through `$01:B473` when `x+4` leaves the old `[cameraX,cameraX+$100)`
   window. The faithful `$B473` port widens only those horizontal camera
   comparisons, retaining the 512x512 hard bounds and authentic vertical rule.
   Direct testing on 2026-07-14 confirmed arrows remain and render correctly in
   both margins. `AR_WS_SIM_SPRITES=0` restores both native predicates and
   `AR_WS_SIM_SPRDBG=1` diagnoses both paths.
5. **Polish shared presentation.** Audit action HUD side panels, dialog staging,
   boss effects, intro/name-entry/ending screens, and framebuffer-gap clearing.
6. **Re-run the complete matrix.** Recheck every action stage, every town, and
   the major non-action screens with widescreen enabled and disabled before a
   release-quality milestone.

## Major functionality

| System | Status | Notes |
|---|---|---|
| Boot / title screen | ✅ | |
| Save / load (in-game state) | ✅ | Checksum-gated continue path confirmed (`AR_SAVECHECK`) |
| Action-stage combat | ✅ | Every ordinary action level plus the complete Death Heim boss rush, final boss, and return transition is fully playable. Open `$00:B8AB` spell-projectile garbage variant (`DEBUG.md` #19) remains a separate unconverted-code edge case. |
| Magic casting | 🟡 | WORKS as of 2026-07-07 (was dead — blocked by our own knockback cheat, `DEBUG.md` #18); Fire verified in-stage; other 3 spells' effects pending validation (`AR_ALL_MAGIC` cheat added for exactly this) |
| Sim-mode town simulation | 🟡 | Fillmore ✅ end-to-end; Bloodpool entry/lightning partial; full Bloodpool plus Kasandora/Aitos/Marahna/Northwall baselines pending. Reward web and multi-actor cutscenes fixed 2026-07-07 (`DEBUG.md` #18b/§7.17). |
| Scroll/MP persistence | ✅ | `$0295` persistent / `$21` working-copy model mapped + grant verified across modes (2026-07-07) |
| Audio (music/SPC) | 🟡 | SPC upload handshake and boss-music playback fixed; a narrower "voice/SFX key-on" gap was reported and its current status isn't confirmed — verify before marking ✅ |
| Music replacement (OGG streaming) | 🟡 | 2026-07-16: manifest-driven OGG streaming live (`[music:]` in `game-assets/manifest.ini`, all 17 song-table entries enumerated): port-0 play/halt protocol decoded, srcn>=0x0C DSP voice gate keeps SFX authentic, sample-accurate loops (LOOPSTART tags), `when =` variant gates. Title theme verified headless end-to-end (engage/loop/fallback/toggle). Pending: in-game listening pass, per-src identification of the 16 unnamed songs, driver fade capture. |
| Mode 7 (overworld/menus) | 🟡 | Frame-pacing bug (1/3 speed) fixed; not otherwise deeply verified |
| Input | 🟡 | Hardcoded keyboard mapping works (see README); no gamepad support yet. Consumer side fully mapped (SEAMS "Input" + "Magic system") |
| Runtime settings overlay | ✅ | Phase 5 complete: global Escape/F1 access, hierarchical category/direct-action navigation, independently scaled three-panel native dialog theme, ROM-decoded font/frame atlases, frozen-game input capture, validated editing/actions, and atomic `settings.ini` saves. Phase 6 includes the guarded Save editor category and codec actions. A native game-menu entry is optional; gamepad support belongs to the separate input backlog. 2026-07-21: new Graphics category added for the diorama GPU-shader effects below. |
| Diorama 3D presentation mode | 🟡 | `ar-recomp-threading-impl.md`'s full plan shipped 2026-07-20/21: action-stage layers render as a tilted 3D shadowbox (interactive camera, per-layer toggles), a dedicated present thread decouples rendering from game logic (vsync no longer blocks the game thread), a fixed 60.0988Hz game-tick loop, and optional GPU shader effects (rim lighting + depth-of-field/edge-AA, live-verified) reachable via Settings → Graphics. Two features are implemented but shipped OFF by default with a known bug each: scroll interpolation (vibrates the HDMA-driven BG2 parallax layer) and soft shadow blur (bleeds onto transparent gaps in the layer behind it, e.g. over the sky). Sim mode/world map (Mode 7) are explicitly out of scope for this feature. 2026-07-23: the act-title card and pause text (BG3 rows below the HUD split) now ride the composed flat HUD overlay instead of being buried behind the tilted scene planes — see rendering-engine.md §13.1. |
| Battery save codec/editor | 🟡 | 2026-07-16: exact 8 KiB native codec, checksum validation/repair, version-1 lossless INI, deterministic active backend, atomic writes, timestamped editor backup, auto-persist/shutdown shadow re-sync, five paged edit groups (town/Death Heim/Professional progress, player/Angel status, magic, items, and BCD scores), import/export/session/persistent actions, `tools/srm.py`, and transactional tests are implemented. All 9 repository saves validate and `.srm → .ini → .srm` is byte-identical. Pending final gate: manual Apply and save → Restart Game → Continue acceptance matrix in the game. |
| Cheats | 🟡 | Named cheat kit 2026-07-07: `AR_ALL_MAGIC`/`AR_RANGED_SWORD`/`AR_INF_MP`/`AR_INF_SP`/`AR_ANGEL_HP` + magic-safe `AR_NO_KNOCKBACK` + generic `AR_PIN`; real 8x turbo on `t`. `AR_FREEZE_TIMER` auto-backoff added, still unverified. `AR_NO_KNOCKBACK` is not physics-neutral: its pinned invulnerability suppresses water drag (confirmed 2026-07-12). |
| Bridge structure-cap fix (sim) | 🟡 | 2026-07-17: structure-record system fully mapped + SRAM-validated (SEAMS town §7, save-format §3.4: 128 × 4-byte records per town, allocator `$03:9D9F`, census `$03:C07F`, miracle damage `$03:B274`, bridge immunity row `$A435`; record format confirmed against real saves incl. both bridge orientation variants). v1 slot-reuse/lightning designs were withdrawn after they erased bridges on reconstruction. v2 uses a validated/deduplicated completed-bridge sidecar: `$9D9F` migrates, `$C07E` restores support, `$9CFB` restores `$E1/$E2` marks, and `$89F0` decodes the native rebuild program to restore the visible metatile after `$9D4D`. Sidecar-only checksum changes are shadowed until a normal ROM save transaction, with a persistence regression test. Marks-only visual capture correctly failed (black bridge), establishing the second render seam; generated build + replacement screenshot are the remaining acceptance gate. |
| Debug tooling | ✅ | 2026-07-07 toolkit: `dis65`/`romxref`/`wram`/`resolve_miss`/`cycle.sh` — anomaly capture → auto-triage → proposed cfg patch loop (`DEBUG.md` §1) |
| Action widescreen BG/sprites | 🟡 | All ordinary stages and Death Heim are fully playable and visually validated: wide streaming, fast vertical rows, sprites, activation, narrow-BG2 mirror/repeat policies, HDMA/parallax scenes, bosses, and post-final-boss transitions behave correctly. Remaining: general camera/world-edge clamp for full presentation coverage. |

## Codebase metrics (objective, automated — refreshed 2026-07-12)

These come from static analysis and reference-vector testing, not manual
play, so they're a different kind of signal than the tables above: they say
"how much of the recompiler's output is structurally sound / verified
correct," not "does the game actually work." Both matter. Re-run the
commands noted below periodically and update this section rather than let
it go stale — same discipline as the playability tables.

| Metric | Value | How to reproduce |
|---|---|---|
| Hand-authored recompiler directives (`recomp/*.cfg`) | 2,646 lines | `wc -l recomp/*.cfg` |
| → generated C output (`src/gen/*.c`) | 2,130,680 lines | `wc -l src/gen/*.c` (after `snesbuild regen`) |
| Hand-written game runtime (`src/*.c`/`*.h`, excl. shared engine) | 3,253 lines | `wc -l src/*.c src/*.h` |
| Bank coverage | 29 of 32 possible SNES banks | `ls recomp/bank*.cfg \| wc -l` |
| Recompiled functions (unique ROM addresses) | 2,480 | `grep -c "^    { 0x" src/gen/dispatch_v2.c` |
| Recompiled functions (× m/x width variants) | 4,657 | `go -C snesrecomp-go run ./cmd/v2regen link-audit --gen-dir ../src/gen --src-dir ../src --runtime-dir runtime/src` |
| Static reachability | 4,657/4,657 (100%) — 0 orphans, 0 unreferenced variants | same Go link-audit command |
| Unresolved trap sites | 74 logical sites / 165 variant emissions: 20 goto sites (53 variants) + 54 indirect-oob sites (112 variants) | `go -C snesrecomp-go run ./cmd/v2regen stub-census --gen-dir ../src/gen` |
| Opcode correctness vs. Tom Harte 65816 reference vectors | 227/227 opcodes clean, 14,528/14,528 vectors passed | `go -C snesrecomp-go run ./cmd/v2regen opcode-diff --cache-dir ../tools/oracle/harte_cache --runtime-dir runtime/src --all` |
| Go recompiler unit/regression suite | all packages passing | `go -C snesrecomp-go test ./...` |
| Generated-output layout | 83 generated C files in the current local build; generated files and comparison archives are ROM-derived and are not distributed | `find src/gen -maxdepth 1 -name '*.c' -type f \| wc -l` |

## What "done" looks like

Not attempting to define a precise finish line here since it'll shift as
things are found — but at minimum, "done" means every action stage and every
sim-mode town has actually been played through and marked ✅ or 🟡-with-a-
specific-known-issue, not left as an assumption. Update this doc as that
happens rather than batching it up for later — a stale progress doc is worse
than no progress doc, which is exactly why this one got rewritten.
