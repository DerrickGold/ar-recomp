# Steam Deck action scanout optimization plan

Date: 2026-09-12. Camera changes committed as `27df9aab`; finite-skybox fix
committed separately as `7f3f218b`.

Status: source analysis and local baseline survey, followed by the first
capture-eligibility implementation and a native Deck A/B comparison. See the
[capture fast-path audit](capture-fast-path-audit.md) for changes, verification
and measurement results: Aitos scanout median fell 10.9% (0.432 ms), with exact
composite parity. A subsequent visible fullscreen/interpolated comparison
measured 387 → 433 uncapped host FPS and 5.66 → 5.01 ms per scanout call;
both builds held 90 Hz Vsync. These are separate desktop operating points,
not Game Mode measurements or an upload speedup claim.
The user then identified Fillmore as the source of the older ~7 ms readings:
the recovered skybox/CRT/32-extra-row preset measured 7.38 → 6.64 ms per action
scanout in four visible Unlimited trials, and 7.08 → 6.45 ms in a separate
90 Hz pair. Upload was unchanged; this preset has interpolation off.
The older local survey below remains Mac-only. No user graphics settings changed.
Unrelated in-progress installer and world-navigation work is outside its scope.

## Recommendation

Keep the verified no-classifier fast-path change. Attribute the remaining
scanout cost on that candidate, then test **overlay color-table reuse** and
**clamped-skybox row work**, followed by bounded clear/fill reductions. The
Fillmore interpolation-on results below keep PPU work ahead of motion analysis.
Band-aware whole-tile processing remains a conditional other-room follow-up,
not the next Fillmore optimization. Do not begin with a threaded scanline
renderer, a GPU PPU rewrite, or reduced image quality.

The [reviewed field guide](steam-deck-field-guide-reviewed.html) is useful
hardware and historical measurement evidence, not a mandate to adopt every
technique it discusses. Its qualifications supersede stronger claims in the
[older handover](steam-deck-optimization-handover.md).

## Starting-code findings after the skybox commit

The first experiment can be smaller than a new SIMD kernel:

1. `render_native_capture_line` initializes both band arrays to `0xff`, then
   requests band output from every eligible virtual BG, even when that BG has
   no classification callback. The source resolver writes the same `0xff`
   values back and bypasses its explicit eight-pixel/SIMD path because the
   band pointer is non-null. Ordinary hardware priority splits do not require
   custom band data: `0xff` already means use the native priority.
2. The public virtual-map adapter preserves a null band callback, so the runner
   can make this decision from its own binding. First test passing no band
   destination when classification is absent, retaining the initialized array
   for downstream export. No room IDs, renderer types or new ABI are needed.
3. The current `diorama-layers.ini` authors custom BG classification only for
   Death Heim's `07/01` BG2. This is not a proof of per-frame eligibility, but
   it makes the no-classification case broadly relevant. Both new replays
   report zero band-cache builds/hits; merely visiting the authored Death Heim
   room does not prove the virtual classified path executed. Add a fixture
   that actually binds and exercises classification before testing the later
   band-preserving whole-tile path.
4. Add no-callback variants to the independent reference-renderer capture
   oracle: the existing virtual-capture fixture always supplies a callback.
   Also explicitly exercise full unwindowed tiles, partial tiles, mirror seams,
   transparent texels, main-only/sub-only sources, raster palette/brightness
   changes and hardware priority splits. Test SIMD on and off, then x86/SSE2
   on the Deck; passing only ARM/NEON is insufficient.

The second experiment remains eliminating duplicate primary clear/fill stores.
Do **not** change band-clear ordering casually: existing tests deliberately
alias a priority surface to its primary, then unbind and rebind it. Filling the
primary before clearing an aliased band would erase that fill. Preserve this
ordering and all content-mask semantics.

The current optimized local build is `RelWithDebInfo`, `-O2`, thin LTO,
ARM64, and `SNESRECOMP_ENABLE_SIMD=1`, with deep instrumentation/recorder off.
Its figures cannot establish the benefit of a different x86 compiler or ISA.
The new fixed skybox adds its own view resolve/upload/history work; keep it
enabled consistently in later A/B runs rather than comparing against a build
that still has the clipped backdrop.

### Fresh local baseline survey

Six serial, bounded runs of the post-skybox optimized build: one per flat
checkpoint and two per diorama checkpoint. All completed 3,000 presentations.
No image captures, profiler or build ran alongside them. Input hashes, isolated
saves/settings, logs and the harness are retained under
`/private/tmp/actraiser-action-scanout-survey.9KtiZY/` (`results.json` and
`repeat-diorama/results.json`). This is a baseline survey, not an optimization
comparison. Repeated diorama runs have byte-identical final WRAM within each
checkpoint; flat and diorama end states differ and are not paired oracles.

Apple M2 / SDL GPU Metal, 2160×1344 output, 16:10, three requested helpers,
default interpolation off and zero vertical extension. The reducer filters
scene and map, discards entry warm-up, and frame-weights the last settled
reporting windows. Values are CPU wall time per presentation, not GPU time or
Deck predictions. Each headless presentation contains one emulation tick.

| Checkpoint | Settled windows | PPU scanout ms | Render CPU ms |
| --- | ---: | ---: | ---: |
| Aitos flat, 04/04 (one run) | 5 | 1.451 | 1.666 |
| Aitos diorama, 04/04 (two runs) | 5 each | 2.196–2.372 | 2.768–2.901 |
| Death Heim flat, 07/01 (one run) | 5 | 1.192 | 1.315 |
| Death Heim diorama, 07/01 (two runs) | 3 each | 1.715–1.900 | 2.150–2.367 |

Scanout accounts for about 79–82% of these diorama render-CPU totals, excluding
emulation and explicit present waits. The unchanged reruns differ by 8.0% and
10.7% in scanout time: these samples locate work but do not establish a stable
noise floor or a speedup threshold. Use alternating repeated trials, exact
same-mode WRAM/image checks, and target verification for each candidate.

The Death Heim log explicitly selects `viewport/viewport`, `hle=00` in 07/01;
its world-backed BG1 appears in 07/02 and 07/03 instead. Use hub 07/01 as a
native-path regression/control case, not evidence for a virtual-tile win.
Reduce boss-room windows separately. Both surveyed routes report zero compiled
band-cache builds/hits, so a classified synthetic oracle remains necessary.

The first implementation removes the no-classification band request and narrows
the disabled-large-tile eligibility gate. Verification includes an independent
with/without-callback pixel oracle; replay evidence is recorded in the linked
audit. The native hermetic Linux/SSE2 comparison now supports keeping the
no-classifier change. Keep band-aware whole-tile kernels and clear/fill changes
as separately measured experiments; their benefit is still unmeasured.

## What the evidence actually establishes

- Historical Deck action measurements put scanout at **3.65–3.72 ms**, nested
  inside PPU/capture at 3.72–3.79 ms. These are not two additive costs. That run
  used an older build, interpolation, a hidden 1080×672 Wayland window and
  competing Steam activity; it is not the current Game Mode baseline.
- The current `kPerformance_PpuScanout` scope wraps `run_ppu_scanout`, including
  CPU rasterization, layer exports, HDMA and IRQ callbacks. It does not directly
  time GPU blending. See [the application call](../src/actraiser/actraiser_rtl.c)
  and [the runner service](../snesrecomp-go/runtime/src/runner/runner_ppu_services.c).
- Twelve output textures do **not** imply twelve complete independent PPU
  scanouts. The packed capture path resolves sources into line scratch and
  exports multiple views from them. Extra planes still increase clearing,
  output writes and later capture/upload work.
- Fixed capture dimensions should produce similar scanout work at different
  diorama zooms. A zoom-only change that improves presentation but not scanout
  points to a separate screen-coverage cost. Record dimensions and path counts
  to check this instead of conflating the two bottlenecks.
- GPU execution time is still unavailable in the portable performance overlay;
  present/wait or texture-update CPU time is not a substitute. See
  [performance diagnostics](performance-overlay.md).

Deck has four Zen 2 physical cores, eight logical threads, an eight-CU RDNA 2 GPU
and shared system/graphics memory. These favor reducing serial work and traffic
before adding workers. [Valve specifications](https://www.steamdeck.com/en/tech).
The field guide's reference OLED reports 32 KiB L1D and 512 KiB L2 per core,
4 MiB shared L3, and AVX2 support. Neither the cache capacity nor theoretical
memory bandwidth defines a universal application-performance threshold.

## 1. Establish attribution without crossing the runner boundary

Use a symbolized optimized Linux build and a bounded CPU sampling profile on
settled action frames. Confirm the shipped build enables
`SNESRECOMP_ENABLE_SIMD`; the existing x86 backend is SSE2. Keep profiling runs
separate from timing and image-readback runs.

Add opt-in, runner-owned aggregate diagnostics only where sampling cannot
explain the mechanism. Record these once per scanout, not as per-pixel logs:

| Area | Useful attribution/counters |
| --- | --- |
| BG resolution | Virtual/VRAM/fallback spans; full-tile, partial-tile and windowed pixels; full tiles excluded only by band output |
| Layer export | Primary/band bytes cleared, nonzero-fill bytes, exported pixels, bound/active planes |
| Composition | Main/sub passes, ordinary exports, main-winner masks, full-add masks, simple/complex color conversion |
| OBJ | Cache builds/hits by coordinate space and include/exclude range |
| Authentic output | Shared-source rows versus separate authentic renders, with mismatch reasons |
| Geometry | Native rows, actual top/bottom rows, live width, extra capture-budget padding |
| Raster scheduling | HDMA/IRQ/callback work versus raster work |

Prefer sampling plus cheap mechanism counters before adding many clock reads.
If timings are necessary, gate them completely when disabled and measure their
overhead. The runtime must not include application `PerformanceMetrics`, SDL or
private application state. Start with runner-owned diagnostics; if the overlay
needs these values, design an additive size/capability-checked public result or
query with unavailable/older-runner handling. Do not expose `Ppu *` to the app.

Report cost per emulated tick as well as per presentation: interpolation can
present more often than the PPU runs. Track exclusive versus nested scopes.

## 2. First implementation and conditional band-path follow-up

Source: `native_resolve_virtual_bg_span`, `native_apply_virtual_4bpp_tile` and
`render_native_capture_line` in
[runtime ppu.c](../snesrecomp-go/runtime/src/snes/ppu.c).

The virtual BG resolver already batches tilemap lookup and decodes complete
4-bpp tile rows. Its eight-pixel fast path requires `bands == NULL`. Before the
verified patch, the packed capture caller passed band scratch for BG1/BG2 even
without a provider band callback. The patch removes that redundant destination;
custom-classified tiles still take their original path.

Proposed experiments, separately measured:

1. Completed and measured: avoid requesting band output when classification is
   provably absent and the existing `0xff` fallback metadata is preserved.
2. Extend the existing complete-tile path to carry band metadata. Preserve
   transparent pixels, horizontal flip/mirror direction, main/sub visibility,
   priority comparisons, and fallback-to-authentic behavior. Band stores have
   their own existing nontransparent-pixel condition: do not incorrectly use
   the main/sub priority-replacement mask as the band-store mask.
3. Retain scalar handling for partial tiles, window intersections and unusual
   policies. Keep the portable scalar and ARM paths equivalent.

This is a generic runtime improvement, not a Marahna special case. It need not
change layer ordering, capture formats, palette semantics or the public ABI.
Eight packed 16-bit pixels already fit a 128-bit vector: restore useful work
to the current SIMD path before assuming an AVX2 rewrite will help. Measure
eligible pixels and actual Deck timings before estimating benefit.

## 3. Second candidate: avoid redundant initialization and output writes

`clear_overlay_row` clears the complete primary pitch and each bound band
pitch, then may overwrite the primary capture interval with a nonzero fill.
Inactive clean sources already have a skip; that is not a new optimization.

Start with the bounded change: initialize the primary row's outside regions to
zero and its capture interval to its required fill without first zeroing that
same interval. Validate clipping, pitch and row origins before splitting spans.
Keep the required fill evaluated from the current scanline's PPU state.

Only if clearing remains significant, investigate clearing the union of prior
and current written intervals for sparse band surfaces. This requires reliable
tracking across capture movement, inactive frames, source disable, resize,
reset, savestate load, aliasing and live unbind/rebind. All bytes consumers can
read must retain their existing values, including transparent RGB. Do not
remove clears based only on the final image appearing transparent.

Also measure clearing all 512 entries of active source scratch versus only the
proven read span. `PpuNativeLineScratch` reserves 15 KiB; decoded row arrays
reserve another 384 KiB, before OBJ caches, palette tables and output buffers.
These are allocated sizes, not measured simultaneously hot working sets. They
motivate cache/load-store profiling, not a claim that scanout is DRAM-bound.

## 4. Conditional follow-ups after attribution

- **Repeated winner composition:** main-winner masks and the dual-authentic
  path can reconstruct pre-removal winners from the same resolved sources.
  Investigate reuse where the requested sets and coordinates match. Full-add
  exports have different exclusions; they are not interchangeable winners.
- **Authentic-render reuse:** reuse already exists when scroll and OBJ sampling
  agree. Count why separate rendering is needed before changing it. An enhanced
  crop is not automatically the authentic frame, especially with camera offsets
  and source removal.
- **Fallback coverage:** determine whether tiled mosaic, provider eligibility,
  mixed padding or extended rows dominate scalar work. Improve only the local
  unsupported span/effect while retaining the reference sampler as the oracle.
- **Color conversion:** if this is hot after tile/export work, compare integer
  SIMD kernels for the observed color-math cases, preserving exact saturation,
  halving and palette/brightness timing. Plain RGBA math is not a replacement
  for SNES main/sub semantics.

Do not reintroduce the [discarded line-local export experiment](scanout-export-experiment.md)
as a proven win: its local action improvement overlapped measurement noise,
and no Deck gain was established. Existing palette lookup tables, OBJ caches,
tile-span callbacks, conditional subscreen resolution, GPU interpolation copies,
upload-mirror reuse and coverage early-outs also should not be proposed as new.

## 5. Larger changes to defer

- **Parallel scanlines:** the current loop mutates raster state through HDMA,
  IRQs and callbacks, and uses shared caches. A correct design first records
  immutable per-line inputs, including versioned tile/palette/OAM resources;
  then independent jobs need private scratch, ordered output and a join. The
  snapshot, synchronization and cache costs must be included. Four physical
  cores do not imply that eight workers are useful.
- **Producer dirty tracking:** potentially useful for post-scanout comparisons
  and uploads, but unchanged tile IDs do not prove unchanged output. Palette,
  scroll, windows, math, OAM, provider data and resets can change pixels. This
  needs an explicit producer contract, not app access to runner internals.
- **Paletted/GPU scanout:** may remove ARGB traffic and CPU color work, but must
  represent palette changes by row, priority, coverage, half/full-add markers,
  authentic output and interpolation. A single R8 texture plus one frame palette
  is not a drop-in replacement. Any new path belongs behind the runner/render
  capabilities with a tested CPU fallback and proper resource lifetimes.
- **Render scale, AA or blending changes:** these address a demonstrated GPU
  hotspot, not CPU tile scanout. Do not lower Deck image quality to hide this
  bottleneck. Shared memory also does not make mapped uploads automatically
  zero-copy or suitable for the current read-modify-write producer.

## Validation and decision gates

Use the existing [action checkpoints](../tests/fixtures/benchmark/action-render-checkpoints.json)
and [vertical checkpoints](../tests/fixtures/benchmark/action-vertical-checkpoints.json),
with the [comparison tool](../tools/compare_pipeline_performance.py). Add a
reproducible Marahna checkpoint from `saves/subscreen-access.rec`, pinning its
SRAM/settings, map and settled frame interval; do not assume the legacy recording
alone encodes that complete context.

- Timing minimum: Aitos and Death Heim, Action 2D/3D, 16:10; Marahna's subscreen
  color math; and 0/32/64 requested vertical extension, recording actual rows.
  At maximum extension, 224 native rows become 352 rows, 57% more raster rows
  at the same width, not a promised 57% timing increase.
- Correctness matrix: 4:3/16:10, HUD match-game/scaled, diorama off/on,
  interpolation off/on, tight/wide camera, partial/mirrored tiles, windows,
  HDMA palette/brightness changes, full/half-add, subtract, scene transitions,
  savestate/reset and surface alias/unbind/rebind. Explicitly inspect Marahna's
  transparent BG2-over-BG1 edges and black/light-border regressions.
- Run the independent reference-pixel-renderer oracle against color, authentic,
  primary/band/range outputs and content masks, plus application/runner boundary
  tests. Require deterministic input completion and matching final WRAM.
- Run serial ABBAABBA trials, four per variant, with no concurrent build,
  profiling or image capture. Compare exact scene/map windows, drop warm-up,
  inspect frame-weighted medians/ranges and slow-frame behavior. Reject incomplete
  runs and improvements indistinguishable from drift.
- First prove correctness/mechanism locally; verify promising variants on Deck
  with a pinned build, compiler/SIMD flags, SDL/backend, output size, refresh,
  limiter, power setting and temperature. Confirm sustained play in shipping
  Game Mode at 1280×800, not only a hidden-window probe. Preserve simulation
  cadence; a 90 Hz OLED panel does not require 90 emulation ticks per second.

Retain a candidate only for a reproducible Deck improvement in scanout and/or
whole-frame critical-path cost, with no material regression in unaffected
scenes, visuals, audio or pacing. The next work is the candidate-side PPU
attribution and small reuse experiments below, not the larger architectural
changes.

## Updated priorities after visible Fillmore interpolation testing

The same skybox/CRT/dynamic-camera/32-extra-row preset now has a four-run visible
ABBA comparison with interpolation **on**. At 1280×800/Unlimited, candidate
scanout is 6.643 ms/call, the top-level upload scope 1.320 ms/call, and its nested
action-analysis scope 0.382 ms/call. Ordinary action-plane upload is 0.573
ms/call. Analysis includes CPU endpoint copies, GPU endpoint-copy recording
and motion estimation; it is not an exclusive motion-search measurement.
Presentation averages 0.300 ms/host present, which has a different denominator
because generation/retained frames present between authentic ticks. No GPU
timestamp result is implied by these CPU scopes.

The PPU patch lowers scanout from 7.413 to 6.643 ms/call and raises host FPS
from 349.50 to 378.46. Both builds also hold 90 Hz Vsync. Upload is effectively
unchanged by this patch (1.305 → 1.320 ms/call). Enabling interpolation raises
candidate upload from the earlier 0.940 to 1.320 ms/call, but does not reproduce
the user's historical 4.74 ms value. Do not attribute that entire old gap to
either interpolation or the PPU change. See the linked audit for raw evidence.

The following are **source-backed hypotheses, not implemented/measured wins**:

1. **Reuse overlay color tables for identical live raster state.**
   `native_overlay_line_plan` currently prepares 256 ARGB entries for every
   bound source row; the skybox's exposed-edge path prepares another such plan.
   The base RGB palette cache already exists, but these adjusted export tables
   are still rebuilt. First count table builds, source policies and raster
   invalidations. Test sharing an equivalent prepared table or a private cache
   keyed/invalidation-guarded by palette, brightness and relevant capture/math
   policy. Preserve HDMA/IRQ palette changes, OBJ alpha markers, half-add and
   fixed subtract, reset and savestate behavior. This stays inside the runtime;
   it does not give the app PPU pointers or add a renderer dependency.

2. **Trim and batch the finite-skybox view's remaining work.**
   `PpuRenderBackgroundViewLine` already copies the overlapping primary capture
   and only decodes newly exposed edges. Do not propose a second full scanout
   elimination that is already implemented. Remaining work includes clearing
   the row before a later complete overlap copy, individual provider lookups
   for edge tiles, and per-pixel edge palette expansion. Count overlap/copied
   pixels versus decoded edge pixels; then test moving initialization behind
   validated overwrite cases and using the existing provider span contract.
   Preserve forced blank, transparent fills, priority-1 exclusion and all
   failure/partial-write behavior. Keep the independent clamped world origin:
   reusing BG2 at its gameplay coordinates would bring back the clipped skybox.

3. **Reduce scratch/overlay stores where the exact readable span is known.**
   The existing clear/fill section remains applicable, but Fillmore 01/01 has
   no authored black/CGRAM BG fill in `diorama-layers.ini`, so a nonzero-fill-only
   optimization should not be sold as its main win. Broader sparse clearing
   needs aliasing, stale-region and transparent-RGB correctness. Likewise,
   packed export/color-conversion specialization needs current attribution;
   the rejected line-local destination/content-bit experiment is not a win.

4. **Retain interpolation quality while reducing analysis overhead.**
   Whole unchanged planes are already skipped, endpoints already use GPU
   copies, and global search already uses cache-friendly candidate accumulation.
   Profile copy, global search, OBJ block search and endpoint setup separately.
   If scoring matters, test exact integer/SIMD channel differences while keeping
   the same candidate set, sample locations, alpha weight and tie-breaking.
   Do not revive the earlier hill-climb search: it could choose incorrect motion
   for high-frequency pixel art. At ~0.382 ms for this combined stage, even
   removing it entirely would not rival a large reduction in 6.643 ms scanout.

Partial-window whole-tile classification and custom-band SIMD can benefit other
rooms, but this Fillmore run reports unclassified `hle=03` sources and zero band
cache activity. Measure actual exclusions before prioritizing them. Similarly,
the original zoom-dependent blending issue needs a separate GPU/presentation
comparison at fixed PPU capture dimensions; it cannot be inferred from these
scanout timers. General dirty-region producer metadata or paletted GPU outputs
remain larger explicit-contract projects, not shortcuts around the runtime ABI.
