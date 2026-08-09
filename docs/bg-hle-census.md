# Action-background HLE evidence census

Living evidence ledger for `SPEC-bg-hle.md` BH1/BH2. This separates what the
provider has actually matched from what remains an architectural expectation.

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
- 24 PPU-complete snapshots and 43,999 independent offline ring comparisons,
  zero mismatches;
- zero wrong-mode, invalid-source, allocation, or comparison failures;
- expected fail-closed states: 7,752 layer-frames during load force-blank,
  2,370 disabled-layer frames, and 4,439 native-tilemap frames;
- every two-snapshot map/table/descriptor set is stable (one exact map hash,
  definition hash, and descriptor variant per target/layer);
- 12 valid 256x224 framebuffers, 12 distinct hashes. The 4x3 contact sheet was
  visually inspected in target order and every entry shows its intended stage.

All twelve BG1 entry layers are world-provider eligible. Six BG2 layers are
eligible. Four entry BG2 layers explicitly expose a 32x32 PPU tilemap and remain
native/decorative policy, consistent with the existing mirror/repeat path. A
disabled BG2 sample is not a permanent native-only classification: Kasandora
act 2 and Marahna act 2 still require later-room/transition captures before the
scene plan can say the layer never activates.

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

The later provider seam remains default-off, and these entry samples alone are
not enough to close BH1 or delete any legacy background path.

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

The generic PPU provider and ActRaiser binding adapter are implemented behind
default-off `AR_ACTION_BG_HLE=1`. Only plan layers classified `world` bind, and
only pixels outside the authentic 256x224 rectangle consume them. The centre
remains the native ring.

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

This closes BH5's implementation and current evidence gate. It does not fill
the natural Northwall boss transition or the Death Heim ending-tail gaps, make
the feature default-on, or authorize deleting the native streamers/ring oracle.

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
