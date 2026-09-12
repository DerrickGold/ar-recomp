# Action-inclusive rendering audit — 2026-09-11

Baseline: `8ccaab56`, including default held-view globe geometry retention.
Action is not a user of that geometry cache: it captures PPU priority planes,
then uses either flat composition or the enhanced diorama compositor. Moving
GPU model projection remains a separate experiment, not an action optimization.

## Measurement coverage

`tests/fixtures/benchmark/action-render-checkpoints.json` pins wide Aitos and
Death Heim replays in flat and enhanced presentations. Both retain lighting
and particle effects. Each replay uses an isolated native SRAM/settings copy;
the source save is never staged or overwritten. The movement cheats reproduce
the recordings, not a shipping graphics preset. Enhanced action is enabled
after native entry through the existing deterministic trigger.

The pipeline reducer now supports exact map filters. Previously flat action
shared the `Native/menu` label with boot and menus; it is now `Action 2D`.
Filtering scene AND map avoids mixing startup, room transitions or another
room's costs into a settled action comparison. `--control-scene` permits the
old instrumentation label only when a room filter is supplied. The overlay
explains where flat scanout/upload/drawing costs appear instead of showing
irrelevant SIM/3D stage rows.

The reducer drops the first matching reporting window, not the first window
of every later visit. Death Heim returns from 07/02 to 07/01; its hub-filtered
results include that return window and describe a traversal/return workload,
not an uninterrupted idle hub. The 07/02 visit can be reduced separately from
the same logs using `--map 07/02` for future runs. Do not average the two rooms
or treat their different capture/HLE policies as interchangeable.

Initial single-run survey (diagnostic, not optimization gain evidence), Apple
M2, Metal, optimized build, three requested helpers, wide 2160×1344 output:

| Scene | Render CPU ms | PPU scanout ms | Upload ms | Drawing ms |
| --- | ---: | ---: | ---: | ---: |
| Aitos flat, 04/04 | 2.086 | 1.886 | 0.119 | 0.038 |
| Aitos enhanced, 04/04 | 3.196 | 2.709 | 0.333 | 0.109 |
| Death Heim flat, 07/01 | 1.634 | 1.362 | 0.201 | 0.016 |
| Death Heim enhanced, 07/01 | 2.752 | 2.263 | 0.209 | 0.112 |

Render CPU is the existing sum of nonoverlapping preparation, snapshot,
upload and presentation scopes. It excludes emulation and explicit waits;
nested scanout and diorama stages are not added again. The compositor remains
approximately 120-Hz paced. These are CPU wall times, not GPU timestamps,
unconstrained FPS or Steam Deck forecasts. Headless replay deliberately does
one game tick per presentation, unlike a live between-ticks re-present.

A separate call-stack sample identifies packed capture composition,
virtual-background span resolution and overlay pixel export as substantial
scanout work. That profiled run is not timing evidence. Death Heim's additional
post-scanout face/eye promotion also shows up separately in PPU finish.

## Retained changes and boundaries

- **Capture palette lookup:** prepare capture colors once per palette entry
  per source per scanline. Pixel export keeps the same priority/semantic band,
  mask, color-math marker and content-bit behavior. The line-local table sees
  all intervening HDMA/IRQ/brightness/CGRAM/fixed-color changes. It adds 5 KiB
  bounded stack storage per capture line, not a retained frame cache. It stays
  entirely inside the runner's PPU implementation; no public ABI, game-specific
  rendering policy, GPU dependency or emulated-state layout changes.
- **Action coverage early-out:** search each of the existing 8×6 cells only
  until its first alpha-bearing texel. Exact ceil boundaries invert the old
  pixel-to-cell rule, including uneven/tiny images. Dilation, indices, LOD and
  actual visual coverage are unchanged. Byte-addressed reads also support
  unaligned buffers/pitches, with explicit input/overflow validation. No new
  allocation or worker group.

Multicore paths remain intact. Raw PPU scanlines cannot simply be dispatched
concurrently: HDMA/IRQ sequencing, live registers, sprite evaluation and decode
caches are shared. Parallel work would need immutable raster inputs and
disjoint output ownership, not host workers reading mutable runner state.
GPU plane composition is a possible architectural direction, but must retain
native comparison, capture masks, hardware color math and same-frame fallback
without GPU readback stalls. Async compute is not an automatic solution to
this predominantly CPU scanout bottleneck.

### Remaining measured leads (not implemented by this change)

- The virtual-background tile-span loop is still prominent in the sampled
  stack. Its complete-tile packed/SIMD path currently excludes semantic-band
  output, which enhanced action needs. A band-preserving complete-tile path
  deserves a separate prototype, including transparent pixels, horizontal
  flips, mirror seams, window boundaries, main/sub ownership and scalar parity.
- Death Heim's native-only hub promotes statue faces/eyes after scanout.
  It is a distinct game-side cost; optimizing it must preserve priority winners
  and eye blinking, not move room knowledge into the runner.
- Changed-plane dirty-rectangle analysis is another candidate for immutable,
  per-plane jobs. GPU calls and upload-mirror mutation must remain owner-thread
  operations, with jobs joined before publication. Measure dispatch overhead
  against the now-cheaper coverage scan before adding workers here.
- A GPU tile/plane compositor could remove more scanout CPU work, but would be
  a new renderer contract. It needs raster-state snapshots and a validated
  native/fallback strategy; moving existing CPU-projected town models into a
  shader does not address this action bottleneck.

## Reproduction

### Vertical-extension coverage

The base action fixtures use zero vertical extension. The separate
`action-vertical-checkpoints.json` adds 32 and 64 rows per side. Actual margins
are independently clamped to the primary layer's camera/world bounds, and each
background is clipped to its own bounds. The setting applies to enhanced action,
not flat action, towns or globe navigation.

These vertical fixtures are prepared but their additional real-game runs are
deferred at the user's request to return to the unfinished 3D GPU paths. The
vertical observations below are source-audit findings, not measured gains.

At the maximum, 224 rows become 352 (+57.1% pixels per active plane at a fixed
width); at 32 per side the maximum is 288 (+28.6%). This is a work-volume bound,
not an FPS prediction. Horizontal expansion multiplies it. Synthetic rows use
held first/last raster state and are real CPU scanout, not simply stretched UVs.
The line-local capture palette also applies to these packed margin scanlines.

The CPU-heavy stages to watch are scanout, dirty-rectangle/coverage analysis,
plane copies and uploads. Changing actual margins changes upload-mirror height:
`EnsureStorage` currently frees/reallocates on any dimension change, invalidates
the mirror and causes a full upload. Even a constant total height with a moving
top origin can shift much of the content. This is a potential spike source,
distinct from the unavoidable steady-state cost of drawing more world.
The ordinary GPU plane textures already reserve the ABI maximum height.
Optional frame interpolation is different: it copies changed planes, uploads
private capture-sized endpoints and runs CPU motion analysis over the taller
images. `EnsurePlaneTextures` destroys/recreates its previous/current/generated
textures when capture dimensions change; interpolation continuity is also
invalidated. This adds a second potential allocation/upload spike source when
interpolation is enabled. Capacity reuse would need explicit valid-region/edge
clamping to preserve the current protection against sampling stale padding.
Geometry
uses the authentic 224-row scale, so extension adds world rather than shrinking
the native art. No bounds or sprite-ownership checks should be removed to save
work. The existing `bg_hle_matrix.py` fixture validator only admits 0..32;
the new performance fixtures explicitly cover the UI's 64-row limit.

### Commands

```sh
python3 tools/compare_pipeline_performance.py \
  --control /path/to/pinned-control --candidate build-release/ActRaiserRecomp \
  --manifest tests/fixtures/benchmark/action-render-checkpoints.json \
  --checkpoint aitos-diorama --scene 'Action 3D' --quit-frames 3000 \
  --output /path/to/new-evidence-directory
```

For the flat checkpoints use `--scene 'Action 2D'`, adding
`--control-scene 'Native/menu'` only when the control predates the label fix.
The checkpoint provides the exact room filter. Comparisons use serial
ABBAABBA trials (four runs per variant), frame-weighted settled windows,
unchanged input hashes, and no overlapping build/test/capture work. Captures
and sanitizer runs are separate from timing.

`--verify-only --capture-from 1200 --capture-to 2600 --capture-every 100`
runs one control and one candidate without timing instrumentation. It requires
every scheduled final-composite PPM to be byte-identical and checks final WRAM.
The frame range uses the game's 16-bit frame counter, not the runner tick
counter used by `--quit-frames`; boot/transition ticks can differ. Both timing
and verification reject input mutations, missing diagnostics and differing
final WRAM. Screenshot readbacks are never included in CPU gain estimates.

Working evidence: `/private/tmp/actraiser-action-render.WdJ6mU/`.

## Final cross-mode comparison

The final optimized binary completed **56 serial timed runs**, four per
variant across seven replay/presentation configurations. All final WRAM hashes
match within each comparison. The earlier 16 exploratory timing runs predate
the removal of redundant palette-table clearing; they are not pooled into
these results. Town/globe use Quality models; action retains its lighting and
particle settings. Mac CPU scope medians, milliseconds:

| Workload | Control | Candidate | Interpretation |
| --- | ---: | ---: | --- |
| Aitos enhanced, 04/04 | 3.0511 | 2.7844 | 8.7% lower; disjoint ranges |
| Aitos flat, 04/04 | 2.0273 | 2.0300 | Neutral; overlapping ranges |
| Death Heim enhanced hub return, 07/01 | 2.7468 | 2.7006 | Inconclusive; overlapping ranges |
| Death Heim flat hub return, 07/01 | 1.7047 | 1.6661 | Inconclusive; overlapping ranges |
| SIM town | 2.8730 | 2.8080 | 2.3% lower median; ranges nearly touch |
| Sky Palace | 3.2110 | 3.2156 | Neutral; overlapping ranges |
| World navigation | 3.4553 | 3.3156 | 4.0% lower; disjoint ranges |

The same Death Heim logs also cover the uninterrupted 07/02 visit. A secondary
room-specific reduction gives enhanced 2.9336 → 2.7885 ms (4.9% lower), with
control range 2.8845–2.9662 and candidate 2.7581–2.8163. Flat 07/02 remains
inconclusive (1.8409 → 1.8124; overlapping ranges). Both room results are
reported; the favorable 07/02 result does not replace the hub result. No new
runs or different binaries were selected for this secondary reduction.

Aitos enhanced scanout falls 2.5499 → 2.3100 ms and action upload
0.3364 → 0.2948 ms. All stage/whole-render percentages are local Mac CPU work,
not Steam Deck FPS, total frame latency or GPU-time claims. The existing
multicore and fallback renderers are unchanged.

Separate control/final replays pass **102 byte-identical final-composite
captures** with matching final WRAM: 15 each for four action configurations,
9 SIM town, 14 Palace and 19 navigation. These include entry/room transitions;
not every capture is an already-settled enhanced frame. Cloud drift is frozen
only for image verification, never during timing. The final binary is the
same in both matrices. Final results and input hashes are in the `*-final`
and `*-parity` directories (Aitos enhanced timing is `aitos-final`).

## Final validation

The normal application suite passes 162/162 tests, and the standalone runtime
passes 36/36. The coverage oracle also passes under AddressSanitizer and
UndefinedBehaviorSanitizer. A separate Debug runtime with SIMD disabled passes
35/36 tests under both sanitizers, including the modified PPU suite. Its sole
failure is the installed-consumer smoke test: the separately configured consumer
does not inherit sanitizer link flags for the instrumented static library.
That consumer test passes in the normal runtime build. This is not a clean
36/36 sanitizer result, nor evidence of a PPU sanitizer failure.
