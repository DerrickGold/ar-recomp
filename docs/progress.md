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

The immediate implementation focus moves to the runtime settings/overlay plan
in [settings-system.md](settings-system.md). The remaining widescreen backlog is:

1. **Finish action presentation.** Implement the camera/world-edge clamp above,
   then repair and validate the separate Death Heim `70X` flow.
2. **Freeze simulation baselines.** Complete Bloodpool and all four untested
   towns using the town matrix before widening simulation mode.
3. **Widen simulation backgrounds safely.** Derive margins from `$01:B4C6`
   camera bounds; clamp at map edges; preserve special policies for sky palace,
   temple, and world-map subflows; provide a same-binary disable switch.
4. **Widen simulation world sprites.** Extend horizontal emission only for
   `$0A00+` world records in `$01:ADAD/$01:AE6F`; retain authentic `$06A0`
   fixed/UI records and vertical clipping. Add dedicated sim sprite diagnostics.
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
| Action-stage combat | 🟡 | Every action level in regions `$01-$06`, including their bosses, is fully playable after the handler-coverage batch. Death Heim/`70X` reaches its first boss and crashes. Open `$00:B8AB` spell-projectile garbage variant (`DEBUG.md` #19). |
| Magic casting | 🟡 | WORKS as of 2026-07-07 (was dead — blocked by our own knockback cheat, `DEBUG.md` #18); Fire verified in-stage; other 3 spells' effects pending validation (`AR_ALL_MAGIC` cheat added for exactly this) |
| Sim-mode town simulation | 🟡 | Fillmore ✅ end-to-end; Bloodpool entry/lightning partial; full Bloodpool plus Kasandora/Aitos/Marahna/Northwall baselines pending. Reward web and multi-actor cutscenes fixed 2026-07-07 (`DEBUG.md` #18b/§7.17). |
| Scroll/MP persistence | ✅ | `$0295` persistent / `$21` working-copy model mapped + grant verified across modes (2026-07-07) |
| Audio (music/SPC) | 🟡 | SPC upload handshake and boss-music playback fixed; a narrower "voice/SFX key-on" gap was reported and its current status isn't confirmed — verify before marking ✅ |
| Mode 7 (overworld/menus) | 🟡 | Frame-pacing bug (1/3 speed) fixed; not otherwise deeply verified |
| Input | 🟡 | Hardcoded keyboard mapping works (see README); no gamepad support yet. Consumer side fully mapped (SEAMS "Input" + "Magic system") |
| Cheats | 🟡 | Named cheat kit 2026-07-07: `AR_ALL_MAGIC`/`AR_RANGED_SWORD`/`AR_INF_MP`/`AR_INF_SP`/`AR_ANGEL_HP` + magic-safe `AR_NO_KNOCKBACK` + generic `AR_PIN`; real 8x turbo on `t`. `AR_FREEZE_TIMER` auto-backoff added, still unverified. `AR_NO_KNOCKBACK` is not physics-neutral: its pinned invulnerability suppresses water drag (confirmed 2026-07-12). |
| Debug tooling | ✅ | 2026-07-07 toolkit: `dis65`/`romxref`/`wram`/`resolve_miss`/`cycle.sh` — anomaly capture → auto-triage → proposed cfg patch loop (`DEBUG.md` §1) |
| Action widescreen BG/sprites | 🟡 | Regions `$01-$06` are fully playable and visually validated: wide streaming, fast vertical rows, sprites, activation, narrow-BG2 mirror/repeat policies, HDMA/parallax scenes, and bosses all behave correctly. Remaining: camera/world-edge clamp for full presentation coverage; Death Heim/`70X` is blocked by its first-boss crash. |

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
| → generated C output (`src/gen/*.c`) | 2,130,680 lines | `wc -l src/gen/*.c` (after `tools/regen.sh`) |
| Hand-written game runtime (`src/*.c`/`*.h`, excl. shared engine) | 3,253 lines | `wc -l src/*.c src/*.h` |
| Bank coverage | 29 of 32 possible SNES banks | `ls recomp/bank*.cfg \| wc -l` |
| Recompiled functions (unique ROM addresses) | 2,467 | `grep -c "^    { 0x" src/gen/dispatch_v2.c` |
| Recompiled functions (× m/x width variants) | 4,634 | `python3 tools/link_audit.py` |
| Static reachability | 4,634/4,634 (100%) — 0 orphans, 0 unreferenced variants | `python3 tools/link_audit.py` |
| Unresolved trap sites | 75 logical sites / 167 variant emissions: 21 goto sites (55 variants) + 54 indirect-oob sites (112 variants) | `python3 tools/stub_census.py` |
| Opcode correctness vs. Tom Harte 65816 reference vectors | 227/227 opcodes clean, 14,528/14,528 vectors passed | `python3 tools/opcode_diff.py --all` |
| Framework regression suite (`tests/v2/`) | 189/201 passed; three split-bank tests added and passing, same 12 historical failures | `python3 third_party/snesrecomp/tests/v2/run_tests.py` |
| Framework regression suite (top-level) | still blocked by the `lint_codegen_widths` gate (5 violations, aborts the loop; last clean measure 57/58 on 2026-07-03) | `python3 third_party/snesrecomp/tests/run_tests.py` — see `DEBUG.md`/ask a fresh session to look at it |

## What "done" looks like

Not attempting to define a precise finish line here since it'll shift as
things are found — but at minimum, "done" means every action stage and every
sim-mode town has actually been played through and marked ✅ or 🟡-with-a-
specific-known-issue, not left as an assumption. Update this doc as that
happens rather than batching it up for later — a stale progress doc is worse
than no progress doc, which is exactly why this one got rewritten.
