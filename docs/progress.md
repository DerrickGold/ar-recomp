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

## Action stages

ActRaiser's 6 kingdoms each have two action (side-scrolling) stages, played
between rounds of that kingdom's sim-mode town. Region/act numbering below
follows the game's internal order (`$18`/`$19` in WRAM — see `docs/SEAMS.md`
"Game-state anchors"), region name order from the town-dispatch table at
`$03:F5ED`.

| Region | Act 1 | Act 2 |
|---|---|---|
| 1 — Fillmore | ✅ clean full playthrough (2026-07-07) | ✅ clean full playthrough (2026-07-07) |
| 2 — Bloodpool | ✅ clean playthrough incl. act→sim transition (2026-07-07) | ⬜ Untested (sim→act-2 transition not yet reached) |
| 3 — Kasandora | ⬜ Untested | ⬜ Untested |
| 4 — Aitos | ⬜ Untested | ⬜ Untested |
| 5 — Marahna | ⬜ Untested | ⬜ Untested |
| 6 — Northwall | ⬜ Untested | ⬜ Untested |

There's a possible 7th, non-town action stage past the 6 kingdoms (ActRaiser's
final dungeon) — `$18` has room for a 7th region value, but this hasn't been
identified/confirmed in the recomp yet. Untested either way.

## Simulation mode (per town)

| Town | Status |
|---|---|
| Fillmore | ✅ clean full round (act1 → sim → act2, 2026-07-07): development cycles, story events (rock zap/fire), lair sealing with all cutscene actors, reward grants (scroll persists), offerings |
| Bloodpool | 🟡 enters cleanly from act 1 (2026-07-07); lightning cutscene shows both people; full sim playability + sim→act-2 transition NOT yet verified |
| Kasandora | ⬜ Untested |
| Aitos | ⬜ Untested |
| Marahna | ⬜ Untested |
| Northwall | ⬜ Untested |

The sim-mode *engine* itself (town dispatch, spawn/behavior systems — see
`docs/SEAMS.md`'s "Sim-mode town architecture" section) is shared code across
all 6 towns, so a fix verified in Fillmore likely applies everywhere — but
"likely applies" isn't "confirmed," hence still ⬜ for the other five until
someone actually plays them.

## Major functionality

| System | Status | Notes |
|---|---|---|
| Boot / title screen | ✅ | |
| Save / load (in-game state) | ✅ | Checksum-gated continue path confirmed (`AR_SAVECHECK`) |
| Action-stage combat | 🟡 | Core loop solid through Fillmore 1+2 / Bloodpool 1; open: `$00:B8AB` garbage-variant in the spell-projectile path (`DEBUG.md` #19) |
| Magic casting | 🟡 | WORKS as of 2026-07-07 (was dead — blocked by our own knockback cheat, `DEBUG.md` #18); Fire verified in-stage; other 3 spells' effects pending validation (`AR_ALL_MAGIC` cheat added for exactly this) |
| Sim-mode town simulation | 🟡 | Fillmore ✅ end-to-end (dev cycles, events, sealing, rewards); other towns pending. Reward-grant web + multi-actor cutscenes fixed 2026-07-07 (`DEBUG.md` #18b/§7.17) |
| Scroll/MP persistence | ✅ | `$0295` persistent / `$21` working-copy model mapped + grant verified across modes (2026-07-07) |
| Audio (music/SPC) | 🟡 | SPC upload handshake and boss-music playback fixed; a narrower "voice/SFX key-on" gap was reported and its current status isn't confirmed — verify before marking ✅ |
| Mode 7 (overworld/menus) | 🟡 | Frame-pacing bug (1/3 speed) fixed; not otherwise deeply verified |
| Input | 🟡 | Hardcoded keyboard mapping works (see README); no gamepad support yet. Consumer side fully mapped (SEAMS "Input" + "Magic system") |
| Cheats | 🟡 | Named cheat kit 2026-07-07: `AR_ALL_MAGIC`/`AR_RANGED_SWORD`/`AR_INF_MP`/`AR_INF_SP`/`AR_ANGEL_HP` + magic-safe `AR_NO_KNOCKBACK` + generic `AR_PIN`; real 8x turbo on `t`. `AR_FREEZE_TIMER` auto-backoff added, still unverified |
| Debug tooling | ✅ | 2026-07-07 toolkit: `dis65`/`romxref`/`wram`/`resolve_miss`/`cycle.sh` — anomaly capture → auto-triage → proposed cfg patch loop (`DEBUG.md` §1) |

## Codebase metrics (objective, automated — last measured 2026-07-07)

These come from static analysis and reference-vector testing, not manual
play, so they're a different kind of signal than the tables above: they say
"how much of the recompiler's output is structurally sound / verified
correct," not "does the game actually work." Both matter. Re-run the
commands noted below periodically and update this section rather than let
it go stale — same discipline as the playability tables.

| Metric | Value | How to reproduce |
|---|---|---|
| Hand-authored recompiler directives (`recomp/*.cfg`) | 2,532 lines | `wc -l recomp/*.cfg` |
| → generated C output (`src/gen/*.c`) | 2,111,037 lines | `wc -l src/gen/*.c` (after `tools/regen.sh`) |
| Hand-written game runtime (`src/*.c`/`*.h`, excl. shared engine) | 1,626 lines | `wc -l src/*.c src/*.h` |
| Bank coverage | 29 of 32 possible SNES banks | `ls recomp/bank*.cfg \| wc -l` |
| Recompiled functions (unique ROM addresses) | 2,394 | `grep -c "^    { 0x" src/gen/dispatch_v2.c` |
| Recompiled functions (× m/x width variants) | 4,557 | `python3 tools/link_audit.py` |
| Static reachability | 4,557/4,557 (100%) — 0 orphans, 0 unreferenced variants | `python3 tools/link_audit.py` |
| Unresolved trap sites | 75 logical sites / 167 variant emissions (~3.7% of all variants: 21 goto-trap + 54 indirect-oob; count GREW with coverage — newly decoded webs expose new frontier, same as the function counts) | `python3 tools/stub_census.py` |
| Opcode correctness vs. Tom Harte 65816 reference vectors | 227/227 opcodes clean, 14,528/14,528 vectors passed | `python3 tools/opcode_diff.py --all` |
| Framework regression suite (`tests/v2/`) | 186/198 passed | `python3 third_party/snesrecomp/tests/v2/run_tests.py` |
| Framework regression suite (top-level) | still blocked by the `lint_codegen_widths` gate (5 violations, aborts the loop; last clean measure 57/58 on 2026-07-03) | `python3 third_party/snesrecomp/tests/run_tests.py` — see `DEBUG.md`/ask a fresh session to look at it |

## What "done" looks like

Not attempting to define a precise finish line here since it'll shift as
things are found — but at minimum, "done" means every action stage and every
sim-mode town has actually been played through and marked ✅ or 🟡-with-a-
specific-known-issue, not left as an assumption. Update this doc as that
happens rather than batching it up for later — a stale progress doc is worse
than no progress doc, which is exactly why this one got rewritten.
