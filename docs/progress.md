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

## Integration checkpoint — 2026-07-12

The action-mode widescreen implementation is ready to return to `main`:

- wide BG1/BG2 streaming, narrow-BG2 mirror/clamp, margin sprite drawing, and
  margin activation have survived the tested Fillmore, Bloodpool, and Kasandora
  paths without the historical sprite corruption;
- the fast vertical row-streaming and draw-performance fixes are source-only and
  need focused regression checks, but no current evidence ties them to sprite/OAM
  corruption;
- the remaining inert-object findings are pre-existing recompilation coverage
  gaps exposed by broader testing, not widescreen regressions;
- `recomp/bank00.cfg` contains **43 newly added handler entries**, and the current
  local generated build includes them. Static table, field-`$14`, literal-`$12`,
  recognized yield, and saved-snapshot scans now report zero unconverted results.
  Runtime-installed computed roots can still appear and require playthrough captures.

No generated ROM-derived source is committed; reproducible builds materialize
these entries from the cfg. Direct post-generation testing is the next gate.

## Action stages — observed status

ActRaiser's 6 kingdoms each have two action (side-scrolling) stages, played
between rounds of that kingdom's sim-mode town. Region/act numbering below
follows the game's internal order (`$18`/`$19` in WRAM — see `docs/SEAMS.md`
"Game-state anchors"), region name order from the town-dispatch table at
`$03:F5ED`.

| Region | Act 1 | Act 2 |
|---|---|---|
| 1 — Fillmore | ✅ Widescreen full pass; sprites and old-edge activation clean (2026-07-12) | 🟡 Full pass completed; fast-fall stale rows were fixed at the 16px vertical cadence and need one focused retest (2026-07-12) |
| 2 — Bloodpool | ✅ Widescreen full pass; narrow BG2 policy behaves (2026-07-12) | 🟡 Widescreen/mirror and boss completion confirmed. Inert enemy root `$BB25` is now generated; focused retest pending (2026-07-12) |
| 3 — Kasandora | 🟡 Instrumented widescreen pass remained visually successful; runtime roots `$C7FA/$C7FF/$C804/$C80A` are now generated and need focused retest (2026-07-12) | ⬜ Untested |
| 4 — Aitos | ⬜ Untested; static handler preflight complete | ⬜ Untested; same region table covered |
| 5 — Marahna | ⬜ Untested; static handler preflight complete | ⬜ Untested; same region table covered |
| 6 — Northwall | ⬜ Untested; static handler preflight complete | ⬜ Untested; same region table covered |
| 7 — Death Heim | ⬜ Single boss-rush/final-boss flow untested | — No Act 2 |

Static code confirms `$18=$07` selects its own handler table at `$00:F39A`.
The game structure was confirmed on 2026-07-12: Death Heim has no ordinary acts;
it teleports through all six act-2 bosses, then transitions to the final boss.
An instrumented pass still needs to record its `$19` sequence and validate `0701`.

### Next action-mode test gate

With the 43-handler batch now generated locally, test in this order:

1. Focused regressions: `0102` fast vertical fall; `0202` `$BB25`; `0301`
   `$C7FA/$C7FF/$C804/$C80A`.
2. New coverage: `0302`, `0401`, `0402`, `0501`, `0502`, `0601`, `0602`.
3. Final flow: one `0701` Death Heim boss-rush/final-boss pass; there is no
   ordinary `0702`.

For every new act/flow:

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
- use `NoSpriteLimits=1` for the main pass, then repeat at least one dense scene
  with `0` to distinguish authentic scanline pressure from OAM bugs;
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
6. lair sealing, dialogs/temple/sky-palace staging, and sim→act-2 transition;
7. an exit dispatch ring plus F2 snapshots around any partial actor, silent
   event, frozen builder, or missing reward.

Run these authentic-geometry baselines before changing town rendering. The
existing partial-town-actor symptom must be captured as a baseline rather than
silently attributed to future widescreen code.

## Remaining proper-widescreen roadmap

1. **Complete action verification.** Finish the post-regeneration matrix above,
   including authentic sprite limits, all magic/dynamic OBJ selectors, bosses,
   HDMA/window effects, iris/room transitions, and Death Heim.
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
| Action-stage combat | 🟡 | Core loop and bosses verified through Fillmore/Bloodpool plus an instrumented Kasandora-1 pass. The 43-handler batch is generated; direct retest remains. Open `$00:B8AB` spell-projectile garbage variant (`DEBUG.md` #19). |
| Magic casting | 🟡 | WORKS as of 2026-07-07 (was dead — blocked by our own knockback cheat, `DEBUG.md` #18); Fire verified in-stage; other 3 spells' effects pending validation (`AR_ALL_MAGIC` cheat added for exactly this) |
| Sim-mode town simulation | 🟡 | Fillmore ✅ end-to-end; Bloodpool entry/lightning partial; full Bloodpool plus Kasandora/Aitos/Marahna/Northwall baselines pending. Reward web and multi-actor cutscenes fixed 2026-07-07 (`DEBUG.md` #18b/§7.17). |
| Scroll/MP persistence | ✅ | `$0295` persistent / `$21` working-copy model mapped + grant verified across modes (2026-07-07) |
| Audio (music/SPC) | 🟡 | SPC upload handshake and boss-music playback fixed; a narrower "voice/SFX key-on" gap was reported and its current status isn't confirmed — verify before marking ✅ |
| Mode 7 (overworld/menus) | 🟡 | Frame-pacing bug (1/3 speed) fixed; not otherwise deeply verified |
| Input | 🟡 | Hardcoded keyboard mapping works (see README); no gamepad support yet. Consumer side fully mapped (SEAMS "Input" + "Magic system") |
| Cheats | 🟡 | Named cheat kit 2026-07-07: `AR_ALL_MAGIC`/`AR_RANGED_SWORD`/`AR_INF_MP`/`AR_INF_SP`/`AR_ANGEL_HP` + magic-safe `AR_NO_KNOCKBACK` + generic `AR_PIN`; real 8x turbo on `t`. `AR_FREEZE_TIMER` auto-backoff added, still unverified. `AR_NO_KNOCKBACK` is not physics-neutral: its pinned invulnerability suppresses water drag (confirmed 2026-07-12). |
| Debug tooling | ✅ | 2026-07-07 toolkit: `dis65`/`romxref`/`wram`/`resolve_miss`/`cycle.sh` — anomaly capture → auto-triage → proposed cfg patch loop (`DEBUG.md` §1) |
| Action widescreen BG/sprites | 🟡 | Fillmore 1/Bloodpool 1 clean; Bloodpool 2 mirror and boss completion confirmed. The generated 43-handler batch covers Bloodpool `$BB25`, Stage-3 runtime roots, all remaining table gaps, and final yield closure. Paired normal/slow F9 captures proved the earlier movement symptom was host slowdown (60→47 fps), fixed by keying margin refresh to the authentic 16px cadence; `[draw-perf]` covers that phase. Stage D2 remains clean; Kasandora retest, Aitos/Marahna/Northwall pairs, one Death Heim boss-rush pass, hardware-limit, and boss-effect testing remain. |

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
| Hand-written game runtime (`src/*.c`/`*.h`, excl. shared engine) | 3,230 lines | `wc -l src/*.c src/*.h` |
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
