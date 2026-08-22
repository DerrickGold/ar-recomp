# Action-background HLE evidence census

Evidence ledger for `SPEC-bg-hle.md` BH1–BH8 and the later role/extent work.
Sections preserve the dated evidence available at each checkpoint; later
acceptance sections supersede earlier open questions without rewriting the
historical measurements.

## Ordinary act-entry matrix — 2026-08-09

Command:

```sh
python3 tools/bg_hle_matrix.py
```

The runner uses the verified raw warp table from `docs/manual.md`, stages each
target from the replay's transition-capable world-map window at game frame 400,
and captures game frames 900 and 1200. Every child run uses a generated partial
`settings.ini` with Diorama off and vertical extension zero. This isolation is
load-bearing: an earlier probe accidentally inherited the developer's live
Diorama setting, whose headless PPM is only the residual/HUD plane even though
the tile-word comparator remains valid.

Inputs and output contract:

- ROM SHA-256:
  `b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0`;
- replay: `saves/fillmore-act-2.rec` (used to reach the transition-capable
  world-map window and provide deterministic post-entry input);
- runtime observer: `AR_ACTION_BG_HLE_COMPARE=1`, rendering still native;
- the current runner also enables the independent
  `AR_ACTION_ROOM_STAGE_COMPARE=1` command-4/5 WRAM oracle; historical
  manifests created before 2026-08-22 naturally do not contain that summary;
- the current runner requires the default guarded command-4/5 CPU HLE to report
  nonzero command and byte counts; `AR_ACTION_ROOM_LOAD_HLE=0` remains the
  separate exact native control;
- two full WRAM/VRAM/CGRAM/OAM/PPU snapshots per target;
- one authentic 256x224 flat framebuffer per target;
- machine-readable local manifest:
  `runs/bg-hle-matrix-20260809-114024.json`.

| Target | Scene | BG1 at entry | BG2 at entry | Runtime tile comparisons |
| --- | --- | --- | --- | ---: |
| `0101` | Fillmore act 1 | eligible | eligible | 2,177,811 |
| `0102` | Fillmore act 2 | eligible | eligible | 2,181,020 |
| `0201` | Bloodpool act 1 | eligible | native 32x32 decorative layer | 841,609 |
| `0202` | Bloodpool act 2 | eligible | eligible | 2,172,373 |
| `0301` | Kasandora act 1 | eligible | eligible | 2,200,655 |
| `0303` | Kasandora act 2 | eligible | disabled at both samples | 1,077,828 |
| `0401` | Aitos act 1 | eligible | native 32x32 decorative layer | 1,068,029 |
| `0404` | Aitos act 2 | eligible | eligible | 1,910,425 |
| `0501` | Marahna act 1 | eligible | eligible | 2,200,578 |
| `0504` | Marahna act 2 | eligible | disabled at both samples | 1,084,990 |
| `0601` | Northwall act 1 | eligible | native 32x32 decorative layer | 1,071,428 |
| `0605` | Northwall act 2 | eligible | native 32x32 decorative layer | 1,086,077 |

Totals:

- 12/12 target runs passed;
- 19,072,823 authentic runtime tile-word comparisons, zero mismatches and zero
  unexpected finite-world exits;
- 24 PPU-complete snapshots and 44,779 independent offline ring comparisons
  under the corrected PPU scanline interval, zero mismatches;
- zero wrong-mode, invalid-source, allocation, or comparison failures;
- expected fail-closed states: 7,752 layer-frames during load force-blank,
  2,370 disabled-layer frames, and 4,439 native-tilemap frames;
- every two-snapshot map/table/descriptor set is stable (one exact map hash,
  definition hash, and descriptor variant per target/layer);
- 12 valid 256x224 framebuffers, 12 distinct hashes. The 4x3 contact sheet was
  visually inspected in target order and every entry shows its intended stage.

Post-acceptance audit note (2026-08-09): the original manifest embeds 43,999
offline checks because that version of the census modeled output rows `0..223`.
Runtime scanout and the provider preflight correctly use PPU scanlines `1..224`.
Re-evaluating the same 24 immutable snapshots with the corrected tool produces
44,779 checks with the same zero mismatch/outside result. The captured artifacts
and 19,072,823 runtime comparisons are unchanged.

All twelve BG1 entry layers are world-provider eligible. Six BG2 layers are
eligible. Four entry BG2 layers explicitly expose a 32x32 PPU tilemap and remain
native/decorative policy, consistent with the existing mirror/repeat path. A
disabled BG2 sample is not a permanent native-only classification: Kasandora
act 2 and Marahna act 2 still require later-room/transition captures before the
scene plan can say the layer never activates.

## Command-4/5 background staging image — 2026-08-22

`ActRaiserActionBg_StageRoomSceneLayer` is the independent immutable oracle for
the guarded loader HLE. It reconstructs only the native outputs consumed by collision
and the resident-ring builder: dimensions `$2E-$35`, active map bytes in
`$8000-$FFFF`, and byte-swapped definitions in `$2100-$30FF`. It does not clear
inactive map tails or touch command-3 descriptors, CHR, CGRAM, actors, audio,
or any gameplay record.

Manifest `runs/action-room-stage-matrix-20260822-v3.json` records:

- 12/12 ordinary-entry targets passed;
- all 24 loaded BG layers and 166,496 staged bytes, zero mismatch, including
  the six BG2 assets disabled by their sampled PPU state;
- 19,315,975 native-ring comparisons, zero mismatch/outside;
- 19,522 immutable room-scene provider layers, zero live-WRAM fallback.

The gate is deliberately separate from `AR_ACTION_ROOM_SCENE_COMPARE` and runs
before layer-enable/provider eligibility. A dormant background is still loader
state and must match even when it contributes no pixels to the sampled frame.

## Guarded command-4/5 CPU handoff — 2026-08-22

`hle_func_if` preserves a decoded native body behind a read-only `CpuState`
predicate. The `$02:B363` and `$02:B3EB` guards accept only native-mode,
16-bit-index action-room commands with the audited compressed BG1/BG2 operand
shapes. Non-action raw map/definition staging, BG3, explicit
`AR_ACTION_ROOM_LOAD_HLE=0`, and unexpected shapes remain native.

Manifest `runs/action-room-load-hle-matrix-20260822-v2.json` records:

- 12/12 targets passed with 24 command-5 and 24 command-4 HLE invocations;
- 166,400 bytes produced by the CPU loaders, plus 96 dimension bytes in the
  independent 166,496-byte stage oracle, zero mismatch;
- zero native-ring mismatch/outside and zero immutable room-scene fallback;
- all 12 framebuffer hashes and every captured BG map/definition/dimension
  hash identical to the native-handler checkpoint
  `runs/action-room-stage-matrix-20260822-v3.json`.

The enclosing `$02:B1F7` interpreter and commands 7, 6, 3, 2, 1, and 0 remain
native. This checkpoint HLEs background staging, not complete level/gameplay
initialization.

## What this does not prove

This is an entry census, not the complete BH1 gate:

- the warp is a real game transition from non-action state, but deterministic
  replay input after entry was recorded for Fillmore; this is visual/source
  evidence, not a gameplay acceptance run for every target;
- each target samples the early room at two times. Mid-level BGSC handoffs,
  bosses, narrow mixed-policy scanline bands, and ending transitions remain;
- the native Death Heim route is now covered through `$0706`, but `$0707`,
  `$0708`, and the ending handoff still need the same continuous-run evidence;
  Northwall boss target `0608` still needs a natural act-2 transition;
- the framebuffers are authentic flat composites, but individual priority-plane
  captures and deliberate visual positive controls remain open;
- full-level rasterisation from resident WRAM still needs CHR-aware visual
  review outside Fillmore;
- these BH1 matrix runs predate BH4: they do not bind an HLE tile source or
  display a provider margin lookup.

These entry samples alone were not enough to close BH1 or promote/delete any
background path. BH7 later promoted the provider using the broader evidence
set below; this paragraph records the limits of the original BH1 matrix.

## Special action rooms — 2026-08-09

Death Heim needed earlier captures because `$0701` is a short hub and naturally
hands off to `$0702` before the ordinary matrix's game frame 900. The hub was
captured at frames 500/600; direct room selectors `$0702-$0708` were captured at
410/420, after the action load hold but before the borrowed replay could affect
the room transition.

| Target | Scene | BG1 | BG2 | Runtime comparisons | Visual result |
| --- | --- | --- | --- | ---: | --- |
| `0701` | Death Heim hub | native 32x32 | native 32x32 | 189,312 | coherent hub; naturally hands to `0702` |
| `0702` | rematch room 1 | eligible | native 32x32 | 171,680 | coherent |
| `0703` | rematch room 2 | eligible | native 32x32 | 172,576 | coherent |
| `0704` | rematch room 3 | eligible | native 32x32 | 171,680 | coherent |
| `0705` | rematch room 4 | eligible | native 32x32 | 172,212 | coherent |
| `0706` | rematch room 5 | eligible | native 32x32 | 172,576 | coherent |
| `0707` | rematch room 6 | eligible | native 32x32 | 171,680 | coherent |
| `0708` | final boss starfield | native 32x32 | native 32x32 | 0 | coherent native starfield |

The seven direct room frames are distinct and were visually inspected as one
contact sheet. Rooms `$0702-$0707` contribute 1,032,404 runtime comparisons
with zero mismatch; all their offline snapshot comparisons also match. `$0708`
is not a comparator omission: both displayed background layers explicitly use
32x32 native maps, so the world provider correctly declines them.

This establishes source eligibility for every raw Death Heim room, but direct
room selectors are still not a natural boss-rush playthrough. The complete hub
→ rematch → hub → final → ending handoff remains an acceptance and mutation
census gate.

### Native Death Heim handoffs

A second run started only at the real `$0701` hub, pinned the documented slot-50
boss HP field to one, and let the game's own victory driver, progress counter,
hub objects, fades, and action-to-action loader select every later room. It
progressed through this sequence without another map warp:

```text
0701 -> 0702 -> 0703 -> 0701 -> 0704 -> 0705 -> 0701 -> 0706
```

The observer rebuilt eight layer sources across those replacements and compared
6,646,861 in-world tile fetches with zero mismatch. Ten PPU-complete snapshots
in `runs/20260809-120758/` independently identify six maps, one exact source
variant per map/layer, and zero offline mismatch. The borrowed replay ends while
`0706` is in its post-boss coroutine, so this evidence does not promote the
uncaptured `0707`/`0708`/ending tail to verified.

The same run explains its only 364 out-of-world lookups. At GF 5293 in `0705`,
BG2 declares a finite 256x256 world while its camera is `(104,0)`. A 256px
authentic viewport therefore starts requesting tile X=32 beyond that decorative
layer. This is an expected narrow-layer presentation boundary, reported
separately from decoder failure and mismatch; it is exactly the input that the
existing isolated repeat/clamp policy must carry into the scene plan.

Finally, the authentic mid-fight savestate `saves/save0.sav` at `$0702` produces
the same map hash, definition hash, and decoder descriptor as the direct-room
capture while showing live boss graphics. This confirms the direct `0702`
source identity, but savestate execution itself is not used as handoff evidence
because the host coroutine stack cannot be resumed from that format.

## Scene-policy parity — 2026-08-09

BH3 moved the action-specific clamp/mirror/repeat decisions into the pure
`ActionBgPlan` without changing the tile source. Its ROM-free matrix classifies
all 49 known action maps and pins every current exception, including Bloodpool's
`136..224` repeat band and the two Death Heim hub presentations.

The pre-migration executable was retained as an oracle. Old and new builds were
run across the complete 12-entry census, five wide source/policy classes, and
three vertical/diorama cases. Framebuffers, full PPU snapshots, WRAM, SRAM,
dispatch logs, and final state dumps were byte-identical; emitted PPU policy
masks and bands also matched. Runtime diagnostics now add the resolved source
for each layer (`world`, `viewport`, or `native`). The plan is therefore the
production policy owner. BH4 now additionally consumes its world sources for
synthetic margins while the same setters still execute decorative policy.

## BH4 synthetic-margin provider — 2026-08-09

At the BH4 checkpoint, the generic PPU provider and ActRaiser binding adapter
were implemented behind default-off `AR_ACTION_BG_HLE=1`. Only plan layers
classified `world` bound, and only pixels outside the authentic 256x224
rectangle consumed them. The centre remained the native ring. BH5 expanded
ownership and BH7 later promoted the full provider; see below.

The focused evidence set is presentation-aware:

- wide `0101` world/world, `0201` world/mixed mirror+repeat viewport, and
  `0401` world/cyclic viewport have exact off/on screenshots, WRAM, SRAM,
  dispatch logs, and state dumps;
- `0101` with `AR_WS_BGREFRESH=0` plus HLE is byte-identical to the corrected
  reference screenshot and final state, while disabling both produces a
  different screenshot (provider-active positive control);
- Fillmore act 2 gf 2200 off/on matches all nine diorama layer/priority PNGs;
  the HLE arm remains identical with `AR_VEXT_BANDFIX=0`, proving the top band
  no longer relies on host-repaired ring rows;
- the real-PPU synthetic suite pins centre priority-word identity, finite
  transparency, palette swaps, priority, windows, fixed-color math, live
  scroll changes, signed scroll wrap, flips, mosaic, vertical bounds, and
  reset/fail-closed behavior.

This closes BH4. BH5's authentic-centre handoff is recorded below; the later-
room/natural-transition gaps in BH1 remain separate from renderer ownership.

## BH5 authentic-world provider — 2026-08-09

`kPpuVirtualTilemapFlag_IncludeAuthentic` extends the generic binding without
changing BH4's default margin-only contract. ActRaiser opts a world layer into
full ownership only when its full camera matches the live 10-bit scroll phase,
the exact displayed tile range has zero decoder/native-ring mismatches, and no
displayed coordinate is outside the finite world. Unknown flags, the legacy
PPU, raw-wide presentation, narrow/decorative sources, and every failed
precondition retain the native path. Diagnostics distinguish preflight layers,
tiles, mismatches, finite exits, eligible layers, successful bindings, and
runtime lookups.

The provider-enabled authentic-4:3 entry command is:

```sh
python3 tools/bg_hle_matrix.py --enable-provider
```

Manifest: `runs/bg-hle-matrix-20260809-145341.json`. Results:

- 12/12 targets passed; 19,522 eligible layer-frames all bound;
- 18,216,295 authentic preflight tile comparisons, zero mismatch/outside;
- 150,579,968 provider tile fetches, zero finite exits;
- zero scroll-phase, invalid-source, allocation, comparison, or bind-divergence
  fallbacks;
- all 204 compared artifacts are byte-identical to the earlier native matrix:
  12 final framebuffers, final WRAM/SRAM/dispatch/state per target, and two
  complete PPU snapshots per target.

The corrected comparator now follows the PPU's authentic 1-based scanline range
(`1..224`) rather than checking an unused row above fractional cameras. Its
same-run total is 19,315,975 matching native-ring words. The provider total is
lower by design because `ActionBgPlan` keeps narrow/authentic-viewport layers
native even when their raw 64x64 ring happens to be comparator-readable.

Presentation gates were repeated independently of the 4:3 matrix. Wide `0101`,
mixed Bloodpool `0201`, and cyclic Aitos `0401` match native screenshots plus
WRAM/SRAM/dispatch/state in `runs/20260809-140151`,
`runs/20260809-144615`, and `runs/20260809-144621`. Fillmore act-2 diorama gf
2200 in `runs/20260809-144640` matches all nine native layer/priority PNGs and
final state; `runs/20260809-144702` remains exact with the legacy vertical-band
repair disabled.

The maximum-span benchmark used the largest censused world, Aitos `0401` BG1
(4096x1024), at 496 pixels with the comparator disabled. Three release/headless
runs measured medians of 2.298703 s native and 2.426934 s HLE over 1,900 frames:
0.067 ms/frame added. This passes the explicit BH5 ceiling of 0.10 ms/frame at
the worst measured span (0.4% of a 60 Hz frame budget; 5.6% of unpaced headless
throughput).

This closed BH5's implementation and then-current evidence gate. It did not by
itself fill the natural Northwall boss transition or Death Heim ending-tail
gaps, promote the provider, or authorize deleting the native streamers/ring
oracle. BH7 promotion is recorded below.

## BH6 exact decorative/diorama handoff — 2026-08-09

BH6 removes the last lossy policy seam after scanout. The canonical action
`ActionBgPlan` is now latched with the pixels, copied into `FrameSlot`, and
consumed as exact BG1/BG2 source, edge and row-band metadata. Explicit 4:3,
Wide Raw and diagnostic overrides are projected at the producer where they are
applied; non-action scenes receive a native-source projection of their executed
policy. `present.c` does not inspect `g_ppu` or reverse-classify masks.

The old `DioramaBg2MarginSource` scalar could only call a banded layer
"clamped." Its replacement evaluates the plan in captured-row space, including
the vertical-extension origin, and builds up to `2*kActionBgMaxBands+1` exact
row/column spans. Equivalent spans coalesce: Bloodpool `0201` is full width in
both its mirrored upper rows and repeated water rows. Non-equivalent spans stay
separate: Death Heim's pre-ending hub stretches its clamped upper sky from the
authentic 256 columns while its repeated fog band samples the fully padded
capture. The skybox draws one quad per resulting span; ordinary one-span rooms
retain the legacy coordinates and draw count.

The live focused evidence is split according to real source ownership:

- `runs/bg-hle-matrix-20260809-151719.json`: provider-enabled Bloodpool `0201`,
  Aitos `0401`, and Northwall `0601`;
- `runs/bg-hle-matrix-20260809-151437.json`: provider-enabled Death Heim
  rematches `0702-$0707` at the documented early 410/420 capture window;
- `runs/bg-hle-matrix-20260809-151432.json`: native-only short hub `0701` at
  500/600, before its natural handoff to `0702`;
- `runs/bg-hle-matrix-20260809-151438.json`: native-only final starfield `0708`.

The nine provider-owned targets bind 10,364/10,364 eligible layer-frames,
perform 9,401,719 exact preflight comparisons and 74,602,976 provider lookups
with zero provider mismatch/outside result. Together with hub/final, the focused
set performs 10,129,970 runtime comparator checks with zero mismatch or
unexpected fallback. The `0705` comparator still records its already-explained
finite BG2 exits; provider preflight remains zero-outside and the plan keeps that
decorative layer native.

ROM-free tests pin native-plan initialization, policy override validation,
all 49 map classifications, Bloodpool's 136..224 band, Death Heim's 144..224
band and both ending-sky selectors, plus exact row-span behavior with a 16-row
vertical extension. At the BH6 checkpoint, debug and release builds succeeded;
40 sandbox-safe tests and the macOS display-dependent shader test passed
(41/41). The post-Marahna suite now contains 45 tests. BH6 is complete.

## BH7 default-on acceptance — 2026-08-09

The provider is now the default for eligible action world layers. Unset, empty,
or nonzero `AR_ACTION_BG_HLE` enables it; exact `AR_ACTION_BG_HLE=0` is the
native A/B. Wide Raw deliberately remains unbound, and every validation failure
in the accepted matrix fell back atomically per layer and frame. A later
leading-edge cadence case refined that rule: an in-world native-ring mismatch
now retains a
native authentic centre plus finite provider margins; phase/source/bounds
failures still fall back atomically (see ledger #45).

Five paired 12-entry matrices cover authentic 4:3, Wide Full, Wide Raw, and
diorama vertical extension 0 and 32:

- `runs/bg-hle-matrix-bh7-{default,native}.json`;
- `runs/bg-hle-matrix-bh7-full-{default,native}.json`;
- `runs/bg-hle-matrix-bh7-raw-{default,native}.json`;
- `runs/bg-hle-matrix-bh7-diorama0-{default,native}.json`;
- `runs/bg-hle-matrix-bh7-diorama32-{default,native}.json`.

All five pairs pass 12/12 targets. Each pair compares 204 framebuffer, state,
and PPU artifacts. Four are byte-exact throughout. Wide Full `0301` has one
intentional provider improvement: 30 pixels at x=25 in the synthetic left
margin become transparent because BG2 camera X is 0 while BG1 camera X is 1;
the provider honors BG2's independent finite bound instead of wrapping one sky
column from the resident ring. The centered authentic viewport and all state
artifacts remain exact. `tools/bg_hle_artifact_compare.py` with
`--framebuffer-policy authentic-center` accepts only this class: all non-image
artifacts and the centered 256 pixels must be exact, and any accepted margin
delta is counted and localized.

Long natural Fillmore act-2 replays run to game frame 9425 in Wide Full
(`runs/20260809-154717` / `154729`), Wide Raw (`154808` / `154816`), and
diorama-32 (`154845` / `154908`). Full and Raw compare 34 and 38 artifacts;
diorama compares 47 including nine late layer/priority planes. All are exact.
The Full provider completed 259,105,056 lookups and the diorama provider
372,401,760, with zero phase, mismatch, outside, invalid, allocation, or bind
defect. Raw correctly published no provider binding.

A continuous natural Death Heim route (`runs/20260809-154223` / `154230`) goes
`0701 -> 0702 -> 0703 -> 0701 -> 0704 -> 0705 -> 0701 -> 0706`. It produced
9,608 evaluated frames, eight source activations, 7,376 eligible/bound
layer-frames, and 53,189,472 successful provider lookups with zero provider
preflight defect. All 65 paired artifacts are exact. The comparator's 377
finite BG2 exits in `0705` remain the documented decorative-layer observation;
that layer is deliberately native. Historical human playthrough evidence covers
every boss and the ending; natural `0608` and automated `0707`/`0708` ending-tail
captures remain named BH1 archival gaps.

Lifecycle evidence includes same-game-frame paused redraw and policy rebind,
explicit provider reset, fresh-process startup/restart ownership, savestate load
(`runs/20260809-154310` / `154310-1`), and a live Wide Full to 4:3 settings/
geometry transition (`runs/20260809-154604` / `154606`). The savestate pair is
5/5 exact; the geometry pair is 25/25 exact. `AR_LOADSTATE` diagnostics must
redirect `AR_SAVE_NATIVE_PATH` to a disposable copy (or use replay protection),
because a loaded state may otherwise auto-persist its SRAM to the player's
native save.

Finally, a real non-headless Cocoa diorama-32 run
(`runs/20260809-153921` / `154008`) exercised the ordinary compositor and
matched all 14 framebuffer/state/layer artifacts. That host exposed no GPU
device, so GPU-only post-process effects were not part of this A/B; the existing
display-dependent shader test covers that code. Debug and release builds and
all 41 tests pass. BH7 is complete. BH8 owns behavior-neutral retirement of
duplicated host repair code; native streamers, ring, fallback, and oracle remain.

## BH8 legacy ring-repair retirement — 2026-08-09

The final consumer census separated the action-world transaction from the
unrelated Sky Palace repair in `actraiser_widescreen_bg.c`. BH8 removes
`ActRaiser_WidescreenMarginRefresh`, its 128 KiB WRAM/CPU/math snapshot,
`WsRefreshKey`, the `$B825/$B8A0` host-call trampolines, full/partial record
drains, visible/band row builders, and `AR_VEXT_BANDFIX`. The native game's
streamers and upload drains remain; HLE does not replace or mutate them.

`ws_bgrefresh` and `AR_WS_BGREFRESH` remain only as a hidden load-only alias so
old settings parse without warnings. They are excluded from display-preset
inference, runtime decisions, the menu, and newly saved settings. If a planned
world layer cannot bind, the pure
`ActionBgPlan_ClampUnboundWorldLayers` fallback reclassifies just that layer as
an authentic-viewport clamp. This avoids stale/wrapped synthetic margins while
preserving native center rendering. Wide Raw bypasses provider/fallback policy
and remains deliberately raw.

Three complete pre/post-cleanup comparisons pass:

- authentic 4:3: `runs/bg-hle-matrix-bh7-default.json` versus
  `runs/bg-hle-matrix-bh8-default.json`;
- Wide Full: `runs/bg-hle-matrix-bh7-full-default.json` versus
  `runs/bg-hle-matrix-bh8-full-default.json`;
- diorama-32: `runs/bg-hle-matrix-bh7-diorama32-default.json` versus
  `runs/bg-hle-matrix-bh8-diorama32-default.json`.

All 612 artifacts are accepted by the explicit BH8 comparison contract. Every
framebuffer, WRAM/SRAM/dispatch/state artifact, PPU register JSON, and
authentic-ring census is exact. Two Wide Full and five diorama full-VRAM files
contain 1,390 changed words; every address lies inside the provider-eligible
BG1/BG2 `$6000-$7FFF` tilemap ranges and both snapshots have zero authentic-ring
mismatch/outside result. These are the intended offscreen cells the deleted
transaction no longer writes. The reusable comparator option is
`--snapshot-vram-policy provider-owned`; it rejects any changed word outside an
eligible, zero-mismatch tilemap. Provider matrices still report expected
bindings. The comparator also requires identical manifest format, ROM hash,
replay, warp/capture/quit frames, and display/diorama fixture before inspecting
artifacts. Binary path and provider mode remain deliberate A/B dimensions. Each
requested `vd_gfN` snapshot must contain its own complete WRAM, VRAM, CGRAM,
OAM, high-OAM, and PPU-register set; a matching total assembled from incomplete
prefixes is rejected. The plan helper and load-only migration behavior have
ROM-free unit coverage. `PpuSetVerticalMarginLayerClip` is retained: it remains
the generic per-layer top/bottom bound for non-provider/native or decorative
layers in a shared vertical capture, not a duplicate world decoder.

### Final policy/setter census

`ActionBgPlan` remains the sole action-map classifier. The explicit map switch
in `ActRaiser_ApplyWidescreenPolicy` covers simulation, Sky Palace, Mode 7,
title, and other non-action scenes; it does not duplicate any of the 49 action
decisions. `ActionBgPresentationPolicy` remains a mechanical conversion between
the plan and generic PPU masks, plus the deliberate 4:3/Wide Raw/debug override
projection. It contains no map table.

Whole-layer clamp/mirror/repeat, repeat-band, and vertical per-layer clip all
have live consumers and remain. The scanline clamp-band prototype had no caller
at all; the margin-source-gap path had only a permanently-zero ActRaiser local.
BH8 removes both prototypes end to end from the PPU, raster loops, scene
inspector, frontend, and current docs. Sky Palace's ROM-source reconstruction
had already superseded the failed gap experiment.

The rebuilt-release post-census manifests are:

- `runs/bg-hle-matrix-bh8-final-43.json`;
- `runs/bg-hle-matrix-bh8-final-full.json`;
- `runs/bg-hle-matrix-bh8-final-diorama32.json`.

Each is 204/204 byte-exact against its accepted post-ring-repair BH8 baseline,
for 612/612 exact artifacts total. All 36 matrix targets report zero provider
mismatch, debug and release builds passed, and the then-current suite passed
41/41. The post-Marahna suite now passes 45/45. This closes BH8.

### Rejected Northwall `0608` shortcut

`0608` does enter map `$06/$08` for about 23 live frames. At frames 410/420 its
BG1 finite map matches the native ring (43,877 runtime comparisons, zero
mismatch) and BG2 is an explicit 32x32 native layer. The framebuffer is not a
valid boss-arena baseline: it shows patterned/garbage CHR, then the shortcut
self-exits to Sky Palace before frame 500. This is direct proof that tile-word
parity alone does not establish character-data residency or a coherent scene.

The earlier documentation calling `0608` a verified focused-test target was
too strong. Keep its tile-source evidence, reject its framebuffer, and obtain a
natural Northwall act-2 boss transition before closing BH1 or any pixel-parity
gate.

## Per-layer extent-role census — 2026-08-10

The layer-extent work adds a semantic role alongside source and edge policy.
This is intentionally not inferred in the presenter from a PPU layer number:
the immutable `ActionBgPlan` now states whether each plane is the scrolling
playfield, a special primary scene, a backdrop, or unclassified non-action
content. The role is metadata only at this checkpoint and does not change the
canvas or any pixel.

A fresh default-provider Wide Full matrix is recorded locally as
`runs/bg-layer-extents-census-full.json`. All twelve ordinary action entries
passed with zero tile mismatch and zero provider preflight failure. Visual
inspection of its contact sheet, the resident-world dimensions below, the
existing isolated-plane evidence, and the Death Heim fixtures establish this
classification:

| Entries | BG1 role/source | BG2 role/source | Extent consequence |
| --- | --- | --- | --- |
| `0101` | playfield / finite world | backdrop / finite world | BG1 remains available; the live-tuned BG2 backdrop is fixed to `128/128` |
| `0102`, `0303`, `0404`, `0504` | playfield / finite world | backdrop / finite world or disabled | both retain available caps; each finite source supplies its own natural bound |
| `0501`, `0502` | playfield / finite world | backdrop / 512px cyclic decoded world | BG2 wraps its decoded X when it shares the full camera with a wider BG1; subsection `$19` is not part of the rule |
| `0301`, natural-transition `0302` | playfield / finite world | backdrop / captured viewport | content-anchored sky rows Mirror within a tuned `128/128` cap while dune rows at BG2 world Y `256+` Repeat with an available extent; BG1 remains available |
| `0201` | playfield / 4096x512 world | backdrop / 256x256 viewport | moon/cloud rows use live-tuned fixed `76/100`; water `136..224` remains independently available |
| `0202` | playfield / 768x512 world | backdrop / 256x256 viewport | the whole BG2 span is live-tuned to `68/68`; water `136..224` repeats within that inherited limit |
| `0206` | playfield / finite world | backdrop / 256x256 viewport | BG1 remains available; the mirrored BG2 backdrop is live-tuned to `68/68` |
| `0207` | playfield / finite world | backdrop / 256x256 viewport | BG1 remains available; the mirrored BG2 backdrop is live-tuned to `92/92` |
| `0208` | playfield / finite world | backdrop / 256x256 viewport | both use Mirror/fill motion; BG1 exposes only `16/16` pixels beyond the authentic view and BG2 is fixed to `0/0` |
| `0401`, `0601`, `0605` | playfield / finite world | backdrop / 256x256 viewport | current cyclic backdrop strategy is repeat-safe and may use the available canvas |
| `0701` pre-ending | playfield / captured viewport | backdrop / captured viewport | bounded upper statue/face art and repeat-safe fog `144..224` remain separate policies |
| `0701` ending sky | playfield / captured viewport | backdrop / captured viewport | the page/state handoff replaces the fog band and remains a separately tuned backdrop |
| `0702`-`0707` | playfield / finite world | backdrop / 256x256 viewport | rematch parallax is cyclic; the playfield may grow independently |
| `0708` | primary scene / native raster | backdrop / native raster | no playfield owns finite-world growth; BG1 still anchors the native Diorama raster presentation |

The ordinary entry worlds observed in this fresh matrix are, respectively,
BG1/BG2: `4096x768/2304x512`, `2048x1280/2048x1280`,
`4096x512/256x256`, `768x512/256x256`, `4096x768/512x512`,
`2048x512/1024x512`, `4096x1024/256x256`,
`1280x1024/1024x1024`, `2048x512/512x512`,
`1024x1024/1024x512`, `2560x1024/256x256`, and
`768x768/256x256`. Death Heim `0702`-`0707` BG1 worlds range from
`256x256` to `512x512`; every BG2 is the intended 256x256 decorative plane.

`ActionBgPlan` assigns playfield/backdrop roles for all 49 recognized action
map IDs and tests the full matrix. `0708` is the explicit scene/backdrop
exception. `ActionBgPlan_InitNative` leaves roles unclassified, so simulation,
Sky Palace, Mode 7, title and other non-action projections cannot accidentally
opt into future action-canvas growth. The live BG Extents tuner displays and
prints the role but cannot edit it; a draft is therefore unable to silently
turn decorative art into a canvas owner.

### Marahna cyclic decoded-world evidence — 2026-08-11

Marahna corrects an assumption in the original role census: a declared world
width is not necessarily a finite endpoint. The two independently captured
subsections have different BG1 maps but the same topology:

| Snapshot | Map | BG1/BG2 camera X | BG2 declaration | Native-ring result |
| --- | --- | --- | --- | --- |
| `runs/20260811-115422/snapshots/snap_00_gf2331` | `0501` | `543/543` | 512px | all 924 authentic words match decoded X modulo 64 tiles |
| `runs/20260811-120243/snapshots/snap_00_gf9728` | `0502` | `503/503` | 512px | all 957 authentic words match decoded X modulo 64 tiles |

The canonical classifier therefore requires all of: Marahna map group, BG2
WorldMap source, BG2 width 512, independently wider BG1, and equal BG1/BG2
camera X. It sets `ActionBgLayerPlan.wrap_world_x`; lookup and native-ring
preflight both use the same modulo. No subsection allowlist is involved, so the
rule continues to later `$19` values whenever their live structure agrees and
fails closed when any structural input does not.

The snapshots also establish why this provider finding cannot be separated
from PPU screen ownership. Both record main `$06`, sub `$11`, CGWSEL `$02`, and
CGADSUB `$03`: BG2/BG3 form the main input, BG1/OBJ form the subscreen input,
and a full add combines their resolved winners. Provider eligibility and layer
census therefore use main-or-sub enablement rather than main alone. The detailed
capture/compositor contract is in rendering-engine.md §13.4 and SEAMS.md's
main/subscreen seam.

### Canonical layer limits

The first intentional policy promotes the conservative result of that census:
a narrow mirrored backdrop receives a fixed zero-pixel synthetic extension,
while a narrow cyclic backdrop remains available to the complete canvas. The
cap affects only pixels outside the authentic viewport; it neither clamps the
global canvas nor the playfield role. Bloodpool `0201` and `0202` additionally
resolve rows `136..224` as repeat-safe water. The promoted tuner export in
`runs/20260810-122509` gives `0201` a known-good reflected upper extent of 76px
left and 100px right while leaving its water available. The later `0202` tuning
uses 68px on each side for the whole backdrop; its Repeat band inherits that
extent. Death Heim `0701` applies the same banded principle at its established
fog boundary `144`.

Fillmore `0101` is the first tuned finite-world backdrop: BG1 remains available
to expose the playable level, while BG2's live-world plane stops after 128px of
additional canvas on either side. Its world source and LiveWorld edge remain
unchanged; the extent only bounds how much of that backdrop is presented.

Bloodpool `0206` and `0207` apply the same independently bounded composition
without row bands: their world-backed BG1 remains available, while their
viewport-backed Mirror BG2 stops after 68px and 92px per side, respectively.

Bloodpool boss room `0208` is the first tuned playfield whose finite world
source deliberately uses a non-World edge. BG1 retains its provider-backed
world source and playfield role, but Mirror/fill presentation is capped to 16px
per side; BG2 uses the captured viewport with the same Mirror/fill policy and a
0px cap. Source and edge are therefore independent: the provider still owns
verified world tile words, while the PPU edge policy decides how the small
synthetic span is filled. The fitted camera consumes the playfield's effective
16/16 cap rather than the larger display budget.

Kasandora `0301` and its natural-transition room `0302` use a dynamic hybrid
instead of one whole-layer edge. Isolated BG2 renders from
`runs/20260810-130310` confirm sparse sky/cloud art in the first 256px world
page and repeat-safe dunes beginning at world Y=256 in both rooms. The planner
stores one world-anchored `256..512` dune band and the common resolver projects
that content boundary into authentic rows as `255 - BG2 cameraY`, so
the upper rows Mirror and the lower rows Repeat while vertical parallax moves
the boundary. At the two captured states this resolves to row 82 (`0301`,
camera 173) and row 93 (`0302`, camera 162). BG2 uses its authentic viewport
for this synthesized presentation. The mirrored sky/backdrop is capped to
128px per side in both rooms, while the repeat-safe dune band remains available
to the complete canvas. BG1 remains the finite-world canvas owner.

Marahna `0505` keeps both layers world-backed. Its BG2 remains the structurally
identified 512px decoded-world cycle, but the promoted room policy presents
that cycle with Repeat/fill and a fixed 128px extension on each side. BG1 stays
available, so the cap bounds only the independently authored backdrop.

The same canonical table now supports up to four non-overlapping bands per BG.
Fill (Transparent/World/Clamp/Mirror/Repeat/Raw), apparent horizontal motion
(legacy fill-relative or compensated normal scrolling), screen/world anchoring,
and horizontal extent are independent fields. Compiled per-row PPU policy
removes the former one-Repeat-band restriction while preserving every zeroed
legacy initializer. The live BG Extents tab authors and exports this structure;
it remains session-only until a printed room policy is transcribed into the
map catalogue.

The promoted backdrop extents now live in one data table keyed by room plus the
required canonical source/fill classification. Adding another printed tuning
entry therefore does not extend a map-specific `if/else` chain. Mixed
screen/world bands are accepted only if their order remains disjoint across the
complete camera range; stale drafts are atomic and fall back to that canonical
catalogue.

An authentic band with `y0=0` or `y1=224` also governs the adjacent vertical
extension rows. This keeps the band's edge strategy and horizontal extent as
one content-family value: Bloodpool water and Death Heim fog continue cyclically
below the screen instead of falling back to the unique-art Mirror/Clamp default.
Bands that stop before an edge do not leak into either synthetic margin. The
rule is inferred from the existing bounds, so the census gains no second map
table or runtime source of truth.

The older upper-cloud cases remain whole-layer cyclic policies, not top bands:
Aitos `0401-0403`, Northwall `0601-0605`/`0608`, and Death Heim
`0702-0707`. Whole-layer Repeat naturally applies to synthetic vertical rows
on either side, retaining the established same-direction parallax behavior.
The all-map planner assertions and a real-PPU top-margin direction probe now pin
that distinction explicitly.

The initial conservative Wide Full captures stop the moon/cloud plane at the
authentic side boundaries while BG1 platforms and lower water continue. For
`0201`, that accepted baseline reports 1,628 changed margin pixels, zero center
pixels, zero provider-owned VRAM changes, and all 16 non-framebuffer artifacts
exact. `0202` likewise has an exact cropped 256x224 center and 16/16 exact
state/snapshot artifacts. The later live drafts replace `0201`'s upper cap with
`76/100` and give all of `0202` a `68/68` span; the latter's water changes edge
direction but inherits the same limit.
The pre-ending Death Heim hub remains 17/17 exact in both Wide Full and
Diorama-32 because its existing clamp/repeat edges already produced the
now-explicit limits.

This confirms the Bloodpool diagrammed behavior and the Death Heim policy seam.
The final gate covers every ordinary entry in 4:3, Wide Raw and Diorama-32,
plus all six Death Heim rematches in Wide Full and Diorama-32. The rematch Wide
Full control is 102/102 artifacts byte-exact to its frozen baseline. Native
`0708` is 17/17 exact in Wide Full and Diorama-32 after its special classifier
resets both raw raster planes to available extents.

That `0708` reset was found by the cross-mode gate. Generic narrow-BG2
classification had assigned a fixed `0/0` cap before the final-room override
changed the source and edge back to native `RawWrap`; retaining the orthogonal
cap emptied the starfield side margins. The special override now initializes
source, edge, horizontal/vertical extent, bands and role as one coherent state,
with a direct regression assertion.

A pinned `$0347=7` route reaches the real `0701` `$64/$74` page switch but
remains black, so it is not accepted as a visual shortcut for the natural
ending. The natural Death Heim ending tail and natural Northwall `0608`
transition remain useful archival/manual fixture gaps, not untested branches
of the implemented extent machinery.
