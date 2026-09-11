# SIM town CPU/GPU follow-up — 2026-09-11

Follow-up to `world-navigation-cpu-audit.md`, starting at commit `91b82425`.
These changes affect **SIM towns**, not the globe's separate spherical-cloud
renderer. Existing 0–3-helper world rendering paths remain intact. No runner
ABI, render-device vtable, quality preset, model detail or cloud density changed.

## Retained work

### Cloud cache and GPU bank composition

The town shroud regenerated a 64×48 grid and projected/covered each vertex
separately for three cloud banks on every presentation. Drift does not change
that geometry. A camera/coverage key now retains it, initializes indices once,
and leaves only UV drift updates in the portable three-draw fallback. The key
explicitly excludes structure padding. The fixed presentation-owned cache is
558,004 bytes on this build, with a 576-KiB compile-time ceiling and no per-frame
allocation.

Custom-shader-capable renderers now sample and composite all three original
banks in one fragment-shader draw. Per-bank CPU UV updates and two geometry
submissions disappear. This is **graphics-shader offloading, not async compute**:
no readback, extra render target, worker graphics calls or reduced sample count.
Reducing CPU submission work matters; filling the GPU for its own sake does not.

`sim_cloud_effect_backend.h` carries portable sampling values. The SDL adapter
owns formats, device/state lifetime and copied uniforms; all calls stay on the
renderer owner thread. The presenter unbinds after every bind attempt. Missing
shaders or failed binds with successful restoration use the original draw path.
There is no fallback draw after a possibly partial submitted draw; failed state
restoration is a core failure. Reset permits recovery from optional failures.
`AR_SIM3D_CLOUD_GPU=0` selects the cached CPU fallback as a startup diagnostic,
not a new quality setting. Existing cloud controls retain their meaning.

### Demand-driven town underlay blur

The underlay refreshed a four-by-four CPU box downsample and uploaded it on
every map revision even when cull haze/defocus was disabled. Its extracted
presentation-owned resource module accepts an explicit `want_blur` decision.
Sharp-only rendering neither allocates nor refreshes the blur. Re-enabling it
catches up even if sharp is already current. Only successfully uploaded,
matching-revision textures escape.

The original filter, composition order and CPU-owned source image remain
unchanged; no write-only texture mapping is read. A partially failed sharp
upload invalidates its old serial too, requiring a full retry. Optional blur
failures preserve sharp rendering and remain bounded until reset. Navigation
already requests its separate blur on demand and was left alone.

## Measurements

Mac release build, real hidden GPU compositor, copied immutable Aitos fixture
SRAM, `D7-voxel-town` replay with clouds enabled, three helpers and unlimited
refresh. Each comparison uses **ABBAABBA**, four runs per variant. The first
three `sim3d-perf` windows and first eleven draw/present windows are dropped to
measure the settled town. Remaining means are frame-count-weighted, then
medians/ranges are calculated across runs. No competing builds or test suites
run during benchmarks.

These are **CPU scope times**, not GPU timestamps or uncapped FPS: the hidden
swapchain still settles at 120-Hz cadence. The broad `draw-perf` scope includes
PPU rendering, world/metadata/canvas preparation and diagnostics, not just PPU
rendering. Do not double-count nested depth sub-stages.

| Comparison | CPU scope | Before median (range), ms | After median (range), ms |
| --- | --- | --- | --- |
| Cached CPU clouds → GPU banks | Cloud | 0.1844 (0.1822–0.1860) | 0.0609 (0.0605–0.0628) |
| Committed baseline → both cloud changes | Cloud | 0.3103 (0.3075–0.3122) | 0.0608 (0.0605–0.0610) |
| Both cloud changes → demand-driven blur | Underlay, blur disabled | 0.2420 (0.2413–0.2435) | 0.1469 (0.1442–0.1485) |

Cloud work saves **80.4% of that stage's CPU time**. The unused-blur cleanup
saves **39.3% of the tested sharp-only underlay stage**. Neither is a whole-game
FPS improvement. The cloud-only baseline comparison's broader presentation
mean falls from 1.400 to 1.150 ms; its separate draw/preparation mean varies from
2.433 to 2.517 ms. Those broader logs have coarse rounding.

The final baseline-versus-combined ABBAABBA batch (`final.json`) brings the
total to **32 interleaved runs** across the four comparisons. Presentation CPU
time is 1.400 → 1.100 ms; the separate draw/preparation scope is
2.467 → 2.583 ms. Summing those two non-overlapping scope means within each run
before taking the median gives **3.867 → 3.650 ms (5.6% less measured CPU work)**.
That excludes other emulation/event-loop work and still is not an FPS claim.
Cloud and underlay stage medians in this final batch are 0.311 → 0.062 ms and
0.237 → 0.147 ms respectively. This pass has measurable headroom; it does not
establish that all remaining candidates are below a 1% diminishing-return cutoff.

## Validation

- All **158 application CTests passed**, including real-GPU rendering,
  lifecycle, ABI/layering and negative boundary-injection tests.
- Address/undefined-behavior sanitizer checks passed for cloud cache, underlay
  lifecycle and the GPU effect backend. Leak detection was disabled for these
  SDL/driver runs; this is not a leak-check claim.
- MSL, SPIR-V and DXIL blobs reproduce with
  `tools/build_shaders.py --check sim_cloud`. Metal ran here; Vulkan/D3D12
  execution still needs target-host confirmation.
- Eighteen full-game close/wide cloud comparisons: CPU cache byte-identical;
  GPU differs by **at most one channel step out of 255**. Compositing rounds
  once instead of after each bank's 8-bit target write. No coverage/geometry
  was removed. Six synthetic GPU phases separately test UV wrapping, ordered
  samples, varying coverage and exactly transparent coverage.
- Eighteen full-game underlay comparisons, blur off/on including a wider tilted
  camera, are byte-identical to the cloud-only candidate. Nine forced CPU
  fallback frames are byte-identical to the committed baseline.
- Cloud tests cover warm reuse, drift advance/rewind, 24 invalidating inputs,
  disabled clouds, projection failures, bind/draw/unbind/allocation failures
  and reset. Underlay tests cover revisions, off/on toggles, stale textures,
  partial uploads, optional failures and recovery. New portable files and the
  effect contract are included in the rendering boundary audit.

## Remaining candidates

The settled replay still spends roughly 2.5 ms in combined draw/capture/metadata
preparation, 0.35 ms in presentation uploads, and 0.11–0.12 ms in town model
projection (more when geometry/camera changes). Split that broad preparation
scope before assigning its whole cost to the PPU or choosing another offload.

For models, retained **model-space** meshes/instances and GPU projection are
more promising than caching a whole already-projected town buffer, previously
tested and rejected in the globe audit. Preserve per-model lean, separate
bridge contact/depth heights, clipping and pixel-clean rounding. CPU projection
jobs need immutable model/shading inputs: cache views can be evicted by later
lookups, so dispatching the current `DrawModel` loop directly is unsafe. Neither
shortcut is introduced here.

Steam Deck confirmation remains necessary: Metal CPU savings do not establish
Mesa/Vulkan shader cost, power behavior or final FPS. Keep multicore support
and compare both shader modes on Deck before drawing hardware conclusions.

## Evidence

Temporary root: `/private/tmp/actraiser-town-gpu.Ma3JEp/`, with pinned binaries,
source snapshots, `benchmark.py`, `validate.py`, per-run logs,
`gpu-banks.json`, `combined.json`, `demand-underlay.json`, `final.json`, image-comparison JSON,
shader/build logs, `ctest-all.log` and `asan-tests.log`. Raw composites live in
the run directories named in the comparison JSON. Host cleanup may remove this
temporary evidence; the regression tests are checked in.

SRAM SHA-256: `26ec2474882a69dff576f518f614a094f58f1428c807faf83230d72fe4c13568`.

| Binary | SHA-256 |
| --- | --- |
| `baseline` (`91b82425`) | `1d9eeeab2a9285aafed320faffcda34b1655d097302330a2933b066144a74faa` |
| `cloud-cached` | `ca021ca2ed6917a9b1bb713dabc04a85784a1b2a647192abf609ca54f0cc58ec` |
| `cloud-gpu` | `1b68228396d36aa0361adc5dc4545cff4aaca9604c67d4253b2561f572c613cc` |
| `demand-underlay` | `988de06c862ddbf6da48f889acb93e24f31b32fdacc4c83124c688765ba96300` |
