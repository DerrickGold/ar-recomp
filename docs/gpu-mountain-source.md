# Shared GPU mountain source — 2026-09-12

Status: **default navigation and Sky Palace path**, building on GPU ocean commit
`9b66d9e2`. No new experiment switch or saved quality setting. The complete
compatibility renderer remains available via `AR_SIM3D_WORLD_GPU_GRID=0` and
automatic source-setup failure. This is graphics offload, not new mountain art,
reduced LOD, a runner-ABI change or async compute.

## Implementation and cleanup

The original native mountain faces form the final material range in the world's
existing source allocation, after ocean, chunked land and registered cliffs.
Presentation supplies immutable chart normals, registered floor height, native
relief times the chart metric, original atlas UVs and authored brightness. A
separate transform applies the existing tile-world rise scale and 0.90/1.0 lighting
gain. Camera, viewport and lighting changes update uniforms, not source vertices.
Animated lava continues to use the existing small atlas-region upload.

Source construction uses the existing bounded fork/join pool. The owner prepares
terrain first; helpers only read frozen faces/height/cap arrays and write disjoint
output spans. No worker touches renderer resources or publishes a partial source.
The source key includes mountain geometry revision in addition to geography,
chart radius, cliffs and relief ratio. Enable/disable, town/source changes and
Palace/navigation radius changes therefore republish the appropriate complete set.

The default no longer projects, clips, gathers, caches, converts or uploads
mountain screen-space vertices on each presentation. Removed the final CPU
stream cache, capture/replay helpers, `AR_SIM3D_WORLD_CLIP_CACHE` flag and unused
`cpu-reuse` metric/overlay column. The remaining CPU mountain projection code is
used by the **complete** compatibility renderer and its existing held GPU cache.
The old ocean cache and partial-receiver paths were already removed in the ocean
commit; its compatibility shell also remains needed.

## Contract and failure audit

- `Sim3DDepthSurfaceBatch.layer` explicitly selects Ground, Mountain or
  WorldMountain. Cutouts keep independent atlases, nearest sampling, the original
  alpha discard and opaque depth-write pipeline, plus per-material ordinary call
  ordering. No platform handle, map identity, game clock or live state crosses
  the project-private value contract.
- Only Ground accepts shadows/blur/haze. Applying those samples to cutouts would
  paint across their transparent holes; invalid combinations reject before any
  batch is queued. The entire ocean/land/cliff/mountain group is atomic.
- No extra source handle, sample budget, vertex format, shader blob or uniform
  layout is added. One of four opaque handles retains at most 65,536 quads;
  subtraction-based guards include both authored tails before allocation. The
  group uses three opaque samples (empty mountains are skipped).
- Sixty-seven ordered chunks merge into at most 34 selected runs, below 64.
  Mountains are conservatively retained: ground-only bounds omit their separate
  native rise scale. The GPU handles clipping and back-side depth, with no new
  CPU per-face culling or clipped-object fallback. Land culling is unchanged.
- Source publication/selection and copied sample lifetimes retain the existing
  ready/reset/queued-update contracts. Failure queues none of the source group,
  latches that path unavailable until reset and uses complete compatibility.
  Driver submission failure retains the pass's existing failure policy.
- No readback, new fence/queue, runtime compilation, render vtable or runner ABI
  changes. Metal and actual Deck/Vulkan are exercised; D3D12 runtime is unavailable.
  Existing generated shader formats are unchanged by this work.

## Correctness

All **165 local tests pass**, including render/runner boundary checks. The world
fixture now compares 21 source, lighting, viewport, geography, mountain and Advent
states through three presentations, with exact images for serial versus three
helpers, explicit/default source, full/culled source and cold/warm caches. The
default must execute neither CPU ocean nor CPU mountain stages and must exercise
actual GPU compaction without optional-geometry rejection.

The expanded depth fixture passes on Metal and Deck/Vulkan. Eight new cutout
states compare against independently CPU-placed ordinary geometry, exactly:
both mountain atlases, sharp alpha holes, lighting gain, perspective W, source
range/selection offsets, equal-material order and foreground/background depth.
It also rejects unsupported material/effect combinations atomically. Existing
range, eye-plane clipping, reset, mask, sample-budget and copied-lifetime tests
remain unchanged. The retained compatibility clipping test still checks complete
ordered batches, empty output and submission failure after deleting the cache.

Local full-game comparison against the ocean build is **not byte-identical**.
At 2160x1344, Palace changes 2,061–2,082 of 2,903,040 pixels per capture;
only two pixels exceed two channel levels (maximum 12). Navigation changes
224–11,398 pixels; 0–36 pixels exceed two levels (maximum 78). Most changes are
one-level rounding, with sparse sharp-texture/coverage boundary differences.
Full-frame inspection shows no visible change, but the strict comparison failures
and complete histograms are retained, not relabelled exact or hidden by widening
the tests. Twelve Palace and sixteen moving/held navigation frames have matching
final WRAM. The captures precede the logging-only removal of the obsolete metric;
their pinned executable is retained separately.
Two further local Palace captures confirm the final metric cleanup is
byte-identical to that pinned mountain-renderer executable, with matching WRAM.

Two full-game Deck Palace captures at GF600/GF900 are **byte-identical** to the
ocean build, with matching WRAM. These use the final candidate and do not imply
all Metal/Vulkan views are bit-exact.

## Repeated measurements

Local uses the existing serial ABBAABBA harness, pinned release binaries,
2160x1344 Metal, Quality and three helpers in both variants. Palace runs 1,800
ticks; navigation runs 2,000 through movement and a final hold. Last-five settled
windows are frame-weighted, then run medians are compared. No own build, GPU
fixture, capture or image analysis overlaps local timing. Other host load is not
controlled. Every run completes the full presentation schedule with matching WRAM.

Render CPU wall milliseconds, median [minimum–maximum]:

| Cohort | Ocean build | GPU mountains | Median change |
| --- | --- | --- | --- |
| Local Palace, eight runs | 2.570 [2.495–2.634] | 2.451 [2.432–2.588] | −4.6% |
| Local navigation, first eight runs | 1.991 [1.933–3.560] | 3.397 [1.791–7.512] | **+70.6%** |
| Local navigation, repeated eight runs | 2.319 [2.131–3.047] | 2.126 [2.021–2.548] | −8.4% |
| Deck Palace, first short ABBA | 5.885 [5.191–6.579] | 4.939 [4.834–5.044] | −16.1% |
| Deck Palace, second short ABBA | 5.859 [5.353–6.366] | 4.944 [4.943–4.945] | −15.6% |
| Deck navigation, short ABBA | 4.895 [4.889–4.902] | 4.143 [4.126–4.160] | −15.4% |

**Do not select only the favorable local navigation cohort.** Late runs in the
first cohort slow dramatically, including the last control; some intervals exceed
400 ms. A read-only process snapshot observes WindowServer and several Spotlight
workers, but does not prove they caused the slowdown. Repeated navigation and
Deck results support keeping the path; the noisy first cohort remains evidence
against claiming a precise/reliable local navigation speedup. Window timings also
cover different portions of movement/hold as cadence changes, so vertex/copy
averages are not identical-work GPU cost estimates.

Per-presentation traffic:

| Cohort | Depth geometry upload, MiB | Draws | GPU selection copy, MiB |
| --- | --- | --- | --- |
| Local Palace | 1.745 → 0.740 (−57.6%) | 26 → 26 | 0 → 0 |
| Local navigation, first | 2.778 → 1.400 (−49.6%) | 25 → 25 | 0.144 → 0.181 |
| Local navigation, repeat | 2.777 → 1.486 (−46.5%) | 25 → 25 | 0.140 → 0.229 |
| Deck Palace, first | 1.748 → 0.744 (−57.4%) | 26 → 26 | 0 → 0 |
| Deck navigation | 2.725 → 1.397 (−48.7%) | 25 → 25 | changed selection/window mix |

Local Palace submitted vertices rise 934,519→942,171 (+0.82%): unchanged authored
geometry, with more clipping/depth rejection performed on the GPU. Its depth
submission falls 0.106→0.079 ms and CPU mountain staging 0.055→0. The latter is
removed work, not uninstrumented work: source-group setup/submission is measured
under terrain. Moving navigation copies a larger selected source stream on the
GPU; these copies are separately counted, not called upload savings. Native/2D
texture uploads remain around 1.97 MiB in the Palace run.

Local Palace cadence stays 8.336 ms (about 120 Hz), with more present/wait time.
Deck Palace cadence medians are 7.270→6.217 and 7.224→6.218 ms; Deck navigation
6.136→5.419 ms. These are **short hidden-output directional probes**, not visible
game FPS predictions, GPU timestamps, power measurements or a comparison with
the original 22-FPS session. Both Palace cohorts retain a slower first control
than final control even after explicit control/candidate warmup runs. Device
contention and power-state order effects remain possible; no run is discarded.

Deck uses bundled SDL/Wayland/Vulkan at 1080x672, Quality and three helpers.
Palace has two warmup runs then two four-run 1,000-tick ABBA cohorts; navigation
uses the same warmed pipelines for four 2,000-tick runs. All guards remain active:
8-GiB minimum available RAM at start, 6-GiB abort floor, 2-GiB GPU-growth limit,
20-second fixture/45-second game timeout and bounded process-group termination.
No guard fires, no graphics source rejection occurs, no re-presents are emitted,
and every final WRAM matches its control. Available RAM stays above 11.6 GiB.
Peak incremental GPU allocation is roughly 242 MiB for both Palace variants and
under 254 MiB in navigation; this includes driver cycling, not just source size.
The Vulkan fixture passes in under one second. No probe process remains and
the Wayland session is available.

## Evidence

Local scratch: `/private/tmp/actraiser-gpu-mountains.nR9jOn/`. Remote scratch:
`/home/deck/argame/mountain-probe-20260912.AAh8XJ/`. Installed Deck game, settings
and saves remain untouched. Probe copies use bundled SDL/Wayland/Vulkan and
per-process timeout, RAM and GPU-allocation guards.

Pinned executables:

- Mac ocean control: `ac0922efe7b4fd839d0a14f1b613a56913bfa361a8480d04fa0ea91fccd889a6`.
- Mac final candidate: `266cd89cf194941321c30daa2054f9def2731eea1b88e6adf6fc1f18e9bc73c7`.
- Mac capture candidate before logging-only cleanup: `9dac01f4fb6c4b31d98baa346106e668f3b42fc23e345c3f99e093e72c503904`.
- Linux ocean control: `0751bbabe77ecd307d23ba00169582e14a2c3fe740e248cc3f5bdf817f4e489f`.
- Linux candidate: `66c7d103f4a93e64c80176e3adc0a44da282d8a948a0b986541004b9de62d81c`.
- Linux depth fixture: `10407c4ade01e86660792a760c7b96ef96926e2dac9eb860f0a8809650750cdf`.

Reproduction: `tools/compare_pipeline_performance.py`, config
`/private/tmp/actraiser-deck-profile.2Y9hIv/benchmark-defaults.ini`, manifest
`/private/tmp/actraiser-world-surfaces.NDSzGz/checkpoints.json`, pinned `control`
and `candidate` above. Palace: `--checkpoint palace --scene 'Sky Palace'
--quit-frames 1800 --set AR_REPLAY_NOSTOP=1`. Navigation: `--checkpoint navigation
--scene 'World 3D' --quit-frames 2000`. Each output directory retains all inputs,
hashes, run logs and weighted metrics. `verify-*` holds strict capture results
and pixel histograms; `direction`, `direction-repeat`, `navigation`, `visual`
and `warmup` retain the guarded Deck evidence. `tests-final.log` records all 165
tests and `tests-workers.log` the further serial/helper world-fixture check.

## Remaining uploads

Follow-up: [continuous GPU cloud bodies](gpu-cloud-bodies.md) now removes the
spherical body stream described below from the default path. The paragraph
records the remaining work at this mountain commit, not the current default.

Spherical cloud bodies still calculate moving atlas coordinates/splits on the
CPU and upload the resulting vertices; Palace volume slices also submit ordinary
geometry. Preserve their wrap/pole splits, translucent ordering, alpha and
hardware depth behavior in a later source offload. Do not replace them with an
unsplit spherical UV approximation merely to eliminate upload bytes.

The native/composite texture uploads are distinct from these depth geometry
uploads and remain present. Their ownership and producer-side dirty-tracking
constraints are recorded in the Steam Deck handover. This change does not claim
zero total uploads or measured GPU elapsed time, power or thermal improvement.
