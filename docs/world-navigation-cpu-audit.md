# Globe CPU bottlenecks and navigation fallback

Town follow-up: [SIM cloud GPU composition and demand-driven underlay](sim-town-cpu-gpu-audit.md).

2026-09-10. Follow-up to the Steam Deck report (~40 FPS navigation, ~22 FPS
Sky Palace, one busy CPU core and low reported GPU utilization). Diagnosis
uses `20260910-212514.zip`, the local populated save and reproducible Mac
replays. The supplied archive has no per-stage performance capture or OAM
snapshot at the failing frames, so it cannot establish a Deck stage budget.
The results below are local relative measurements, not promised Deck FPS.

## Reproduced fallback

The native location label does not always traverse glyphs left-to-right in
OAM. Kasandora's measured X sequence is
`156,164,172,180,188,196,212,220,204`. The classifier incorrectly rejected
the last glyph, despite its valid row, palette, bounds and ownership.
Marahna also has a non-monotonic sequence.

Two original-binary navigation replays reproduced the same fallback windows:
gf614–761, gf1682–1809 and gf1974–2101. A probe using those actual OAM snapshots
confirmed that changing only traversal order made classification succeed.
The fix accepts distinct, in-bounds glyph anchors without sorting or mutating
OAM, preserving native sprite priority. Malformed rows, palettes, duplicate
anchors, unrelated objects and incomplete Palace/plaque shapes remain rejected.
The fixed flight stays enhanced after its one forced-blank entry frame.

Producer-side `world-navigation-capture` diagnostics now identify the failed
gate: API, borrowed memory, mode, blanking, OAM layout or a particular sprite
raster. They log reason changes rather than every failed frame. No diagnostic
field crosses the frame ABI. Existing `inactive/integrity=0/mismatch=0` text
describes the separated town capture, not a navigation GPU downgrade reason.
There is no FPS threshold driving this fallback.

## Retained changes and ownership

1. **Reuse packed PPU winners for observational masks.** The Palace BG1
   foreground mask forced the slow reference renderer, including repeated
   per-pixel source sampling. Its simultaneous BG3/text and top-row OBJ
   extraction means a mask must observe the *pre-extraction* screen. The
   runtime now derives pure main-screen-winner masks from its already
   resolved source arrays, including the original winners when another
   source is removed. Mask white remains independent of display brightness.
   Native scanout, authentic capture, windows, priority planes, source
   removal and full-add capture keep their existing contracts. Owning-screen
   and combined winner policies retain the reference fallback. This is a
   generic runtime implementation change, with no ActRaiser knowledge or
   public ABI modification.
2. **Parallelize cloud-coordinate math.** Up to three sleeping helpers plus
   the presentation owner process disjoint ranges of immutable normals with
   the same rotation/arithmetic as the serial path. The portable opaque
   `HostParallelWork` contract lives in `src/host`; SDL threads/semaphores
   remain in `src/platform/sdl`. Jobs complete synchronously before cache
   publication. Workers do not read live WRAM/FrameSlot state, choose caches,
   allocate render resources, call the renderer, or update profiler globals.
   Small jobs run serially; thread/setup failure also uses the identical
   serial callback. No per-job allocation or detached work. Resource reset
   joins and destroys the helpers. `AR_RENDER_WORKERS=0..3` is a diagnostic
   startup cap, not a game/quality setting.

Both build manifests include the adapter, and the render-boundary guard and
its negative tests cover the new portable header. Models, LOD, terrain,
materials, cloud samples, shaders and effect toggles are unchanged.

## Repeated measurements

RelWithDebInfo (`-O2`) binaries on the same Mac/Metal host at 1792x1344; Performance voxel
preset, populated isolated save, full globe effects and moving clouds.
Each comparison uses four runs per variant in **ABBAABBA** order, with no
concurrent build/test/encode. CPU means are presentation-count weighted after
discarding three initial reporting windows; table values are medians across
the four runs. The performance baseline includes the classifier fix, so a
fallback to cheap native drawing cannot make the old renderer appear faster.

| CPU scope | Baseline median (range), ms | Updated median (range), ms |
| --- | --- | --- |
| Palace native drawing | 7.230 (7.200–7.255) | 0.708 (0.700–0.733) |
| Palace enhanced presentation | 5.291 (5.266–5.335) | 5.213 (5.175–5.233) |
| Navigation enhanced presentation | 5.790 (5.766–5.824) | 5.665 (5.611–5.678) |
| Navigation cloud work, included above | 1.626 (1.620–1.631) | 1.473 (1.466–1.479) |

The primary Palace improvement is **90.2% less native drawing CPU time**,
not a large reduction in its 3D draw cost. Navigation presentation improves
**2.2%**, with **9.4% less cloud CPU work**. These are different scopes and
must not be added together. Navigation does not inherit the Palace's mask
bottleneck or its speedup.

Another eight-run comparison uses the *same updated binary*, changing only
`AR_RENDER_WORKERS=0` versus `3`: Palace cloud work **2.119 -> 1.942 ms**
(8.3% lower), total enhanced presentation **5.375 -> 5.196 ms** (3.3% lower).
Total presentation ranges are 5.350–5.383 versus 5.175–5.233 ms.

The complete 1800-frame Palace replay takes a median **22.55 -> 16.13 s**
(28.5% shorter), including boot and fades. Although headless video requests
vsync off and Unlimited refresh, this Mac's hidden swapchain still waits in
presentation. That wall-clock result is not an unconstrained renderer-FPS
measurement; the CPU scopes above are the better optimization evidence.

Local evidence: `/private/tmp/actraiser-globe-cpu.7PSuil/` contains the pinned
baseline/optimized binaries, `benchmark.py`, replay, `final-palace.json`,
`final-navigation.json`, `workers-final-palace.json` and per-run logs.
`benchmark.py full palace`, `full navigation`, and `workers palace` reproduce
the comparisons with the existing isolated save and navigation replay paths
recorded in that script. Initial unsuccessful mask experiments are retained
separately, not presented as successful measurements.

## Validation

- All **156 application CTests** and **35 runtime CTests** pass, including
  runner ABI, rendering boundaries and negative boundary-injection cases.
- The winner-mask oracle covers **192 combinations**: all eight PPU modes,
  native/full-margin widths, six mask/extraction variants, and authentic
  capture on/off. It byte-compares scanout, original capture, z-buffer,
  content masks and priority planes against the reference pixel renderer.
  Variants include brightness, mosaic/windows, source-only subscreen
  ownership, filtered OBJ ranges, text/HUD removal and full-add capture.
  The runtime PPU suite also passes **AddressSanitizer/UndefinedBehaviorSanitizer**.
- Cloud worker tests compare exact-once outputs with serial execution, verify
  helpers actually participate, exercise small/uneven ranges and repeated
  create/join/destroy, and pass **ThreadSanitizer**.
- **254 deterministic GPU images** are byte-identical to the baseline,
  including all towns, injected-clock moving weather, effect/quality toggles
  and resource resets. **14 full-game Palace final-composite captures** at
  gf400–1700 also match exactly with frozen cloud drift. The latter checks
  the actual native PPU foreground, not just the isolated globe renderer.
  Evidence is in `gpu-A`, `gpu-B`, `validate.py` and `visual-palace-*.log`
  under the evidence root above. Actual run directories are
  `runs/20260910-221902` and `runs/20260910-221924`.

## Initial remaining CPU/GPU work (before the following passes)

The GPU is used, but much geometry processing remains CPU-side: projection,
lighting, clipping and cloud/shadow preparation. The vertex shader accepts
preprojected positions, and the depth backend stages and uploads all submitted
vertices each frame. The local flight submits roughly 0.38–0.57 million
vertices per frame; at 40 bytes each, that is about 14–22 MiB of vertex data
before index/texture traffic. That architecture is consistent with a CPU-fed
bottleneck; GPU utilization percentage alone does not measure its stage cost.

The larger next step is retained GPU geometry plus camera/weather uniforms:
start with globe/ocean/cloud receiver meshes, then GPU projection/lighting
and reusable town meshes/instances. It needs a deliberate portable render
contract and equivalent shaders for all backends, not SDL handles in the
game/frame layer. Preserve clipping, wrap seams, transparent ordering and
depth occlusion with image comparisons. Cloud shadows currently reuse nine
CPU receiver sample streams but batch them into one material draw; they are
not nine GPU render passes. Moving that coordinate/sample work to GPU may
reduce both CPU preparation and repeated vertex traffic.

Other safe multicore candidates are immutable terrain/model transform
batches with per-job output buffers merged in deterministic order. Shared
model-cache lookup, renderer submission and live SNES/PPU progression must
not simply be dispatched across cores. The new helper is an initial narrow
implementation, not a threaded simulation or a retained-mesh renderer.

## Second pass: retain shadow receiver positions on the GPU

The follow-up keeps all three cloud banks and all softness samples, but stops
resending complete position/color/UV vertices for every sample. One opaque,
presentation-owned quad mesh retains receiver positions in the depth backend;
only sample UV arrays and a constant RGBA color stream each frame. Camera,
terrain, clipping, radius and viewport changes republish positions using the
existing exact receiver key. Held Palace views reuse the position buffer.

The backend uses a split-input vertex layout with the **existing unchanged
Metal, SPIR-V and DXIL shaders**: position and UV are vertex-rate inputs, color
is an instance-rate input with one instance per draw. There is no new shader
compiler/runtime dependency. Sample draw order, texture coordinates, source
geometry, depth tests, alpha blending and clipping are unchanged. Cloud
shadows change from one aggregate draw to nine sample draws at ordinary
softness, increasing total scene draws 7 -> 15 (navigation) and 8 -> 16
(Palace). Measurements below include that extra command cost.

The new API is project-private, not a runner ABI extension. Callers see no GPU
handles or native structs. It copies input arrays and rejects update-after-
queue and mixed ordinary/retained data within a material. Reset invalidates
GPU payloads but leaves opaque handles for their caller to republish/destroy.
Destroying a queued mesh fails the pass closed. Creation/publication failure
selects the existing exact ordinary-geometry path before any retained sample
is queued; final transfer failures use the existing submission-error policy.
The ordinary test adapter deliberately declines retained storage to exercise
that fallback.

Storage is bounded: at most 16 mesh handles, 262,144 vertices per mesh,
64 samples per pass and 2,097,152 streamed UV vertices. The globe uses one
mesh and at most nine samples. Its scratch arrays inherit the existing
4 MiB receiver-directory bound. GPU/CPU buffers grow geometrically and reuse
capacity; resets release platform allocations, while the caller releases
its handle and scratch arrays. No background thread touches these resources.
The new `vertex-upload-MiB/present` counter measures actual vertex-stream
transfers separately from atlas uploads and effective draw vertex counts.

Another **16 interleaved runs**, four per variant per view, use the same
1792x1344 Mac/Metal host, Performance voxel preset, moving clouds and all
effects. Both binaries include the first pass and use three helper threads.
No builds, tests or captures run alongside the benchmarks.

| CPU scope | First-pass median (range), ms | Retained median (range), ms |
| --- | --- | --- |
| Navigation presentation | 5.570 (5.554–5.622) | 5.104 (5.063–5.138) |
| Navigation cloud preparation | 1.461 (1.452–1.468) | 1.145 (1.141–1.166) |
| Palace presentation | 5.237 (5.183–5.258) | 4.358 (4.342–4.375) |
| Palace cloud preparation | 1.948 (1.940–1.950) | 1.411 (1.404–1.422) |

Presentation CPU cost falls a further **8.4% in navigation** and **16.8% in
Palace**. Included cloud preparation falls **21.7% / 27.6%**, respectively;
do not add those percentages to the total. Native PPU drawing is unchanged
by this pass. Hidden-window swapchain waits still limit wall-clock throughput,
so these are CPU improvements, not a Deck FPS prediction.

In representative B runs, measured vertex streams average **10.45 MiB/frame**
for navigation and **11.55 MiB/frame** for Palace. Using the same runs' actual
submitted vertex counts, the former 40-byte full-vertex representation would
transfer **17.31 / 22.80 MiB/frame**, respectively: approximately **40% / 49%**
less traffic. Index and texture transfers are excluded from this comparison.

The controlled 254-image town/weather/quality/reset sweep remains byte-identical
to the first-pass baseline. Focused GPU tests also compare retained/ordinary
sample pixels and exact transfer byte counts; exercise copied input lifetime,
held views, viewport changes, grow/shrink/reuse, reset with live handles,
unsupported layers, invalid sizes and queued destruction. The CPU clipping
oracle compares streamed UVs with the independent original full-quad clipper.
All **156 application CTests** pass. The focused GPU lifetime/transfer tests
and the complete populated-town GPU sweep also pass **ASan/UBSan**. A final
rebuilt-code comparison repeats all **254 byte-identical GPU images**, and
**14 full-game Palace final-composite captures** match the first-pass binary
with frozen drift. Sanitizer captures are checked for errors/internal
invariants, not compared bytewise across different compiler flags.

Evidence: `/private/tmp/actraiser-retained-receivers.CPw6CL/`, including pinned
binaries, `benchmark.py`, `retained-navigation.json`, `retained-palace.json`,
per-run logs, `compare.py`, `gpu-candidate`, `gpu-final`, `asan-gpu` and
`validate.py`. The first pass's `gpu-B` captures
are the image baseline. Source snapshots preserve the pre-experiment presenter
and depth backend. Broader retained world-space meshes and shader projection/
weather math remain future work; this pass only retains screen-space shadow
receiver positions and reduces their attribute traffic.

## Third pass: GPU spherical shadow coordinates

Retain one instanced quad containing the exact clipped positions, original
texture-sphere normals and clipping weights. Nine small wind/atlas/color
uniforms replace nine CPU coordinate streams. The vertex shader rotates
normals, maps spherical UVs, unwraps all four original corners, clamps
latitude, selects the atlas bank, then applies the original difference-form
clip interpolation. It does **not** alter positions, depth, the affine `w=1`
interpolation convention, material order, shadow softness or effect settings.
Cloud bodies retain their separate pole/seam splitting path and CPU helpers.

The optional project-private mesh API copies caller data and shares the
previous handle/reset/viewport/queued-destruction contract. Only the SDL
adapter knows GPU resources, instance attributes or uniform packing. Mesh
counts, storage growth and sample commands remain bounded. No FrameSlot,
runner ABI, public settings or game/runtime boundary changed. Unsupported
shader/pipeline initialization falls back to the existing retained-UV path,
then ordinary geometry if necessary, at the same quality.

The authored GLSL and generated **MSL, SPIR-V and DXIL** ship together.
`tools/build_shaders.py` now validates vertex uniforms in register space 1,
not the fragment stage's space 3; MSL buffer 0 is also checked. Regeneration
and `--check sim3d_spherical` pass. The missing offline DXC was built in the
temporary evidence directory from Microsoft's tagged `v1.9.2607` source
(`0d3ee6b551b8fa768fbf825300ebab81047ef6a8`) with its pinned DirectX-Headers.
Only the local compiler build's forced SPIR-V backend was disabled (glslc
already supplies SPIR-V). This adds no shipping/build-time dependency.
Metal execution is tested locally; D3D12/Vulkan runtime execution still
requires their respective hosts.

Again, **16 ABBAABBA runs**, four per variant per view, isolated from builds,
tests and captures, use Performance models, all effects, moving weather and
three helpers at 1792x1344. The baseline includes passes one and two.

| CPU scope | Retained-UV median (range), ms | GPU-mapped median (range), ms |
| --- | --- | --- |
| Navigation presentation | 5.094 (5.082–5.107) | 4.469 (4.460–4.510) |
| Navigation cloud preparation | 1.143 (1.142–1.146) | 0.516 (0.514–0.517) |
| Palace presentation | 4.317 (4.300–4.325) | 3.404 (3.392–3.433) |
| Palace cloud preparation | 1.419 (1.414–1.423) | 0.416 (0.415–0.417) |

Total presentation CPU time improves **12.3% / 21.1%**. Cloud preparation,
already included in that scope, falls **54.8% / 70.7%**. Native draw time
stays ~0.291 ms in navigation but rises ~0.713 to ~0.783 ms in Palace in this
comparison; the combined CPU saving remains substantial. Representative
vertex traffic falls **10.46 to 9.14 MiB/frame** in navigation and **11.55 to
8.74 MiB/frame** in Palace. Effective geometry and draw counts are unchanged.
Hidden-swapchain wall time remains cadence-limited: these are not GPU timing
or Steam Deck FPS claims.

All **156 CTests** pass. An independent CPU oracle covers 24 wind phases,
three atlas banks, both clipped triangles, wrap seams, latitude clamping,
an exact pole, copied inputs, depth rejection and reset/reuse. Across its
12,288 pixels only one differs, by one channel level. Additional tests cover
growth/shrink, malformed provenance, nonfinite inputs, viewport invalidation
and queued destruction. Shader blobs compile on the live Metal backend.
The focused mesh tests and complete 254-image populated-world sweep also
pass **ASan/UBSan** (`asan-depth.log`, `asan-globe-full.log`). An initial
sanitizer capture invocation omitted its required output directory and
stopped at file creation; the corrected full run is the validation result.

The **254-image** town/weather/quality/reset sweep differs from the preceding
pass only by at most **1/255 per channel**, with at most **59 of 2,408,448
pixels** changed in any image. **14 actual full-game Palace composites**
differ at only 2–4 pixels each, again by one channel level. These are tiny
transcendental/UNORM rounding differences, not reduced geometry or sampling.

Evidence: `/private/tmp/actraiser-gpu-weather.n9dHJB/`; `baseline` SHA-256
`471000cff33f72933b993e2afbbb35e76152bd54eed9812dae0f4b0d1bb87b7e`,
`optimized` SHA-256
`ece4de08618e88029adab0ab6f49bc14a7c22eb3dad62dd7b1163308bb26f371`.
See `spherical-{navigation,palace}.json`, per-run logs, `compare.py`,
`image-differences-candidate.json`, `validate.py`,
`fullgame-image-differences.json`, `ctest-all.log` and the source snapshots.

## Rejected experiment: GPU-retained town vertices

A complete colored-vertex retained mesh eliminated repeated conversions and
uploads for held town-model views. All 254 images were byte-identical and the
opaque/transparent/reset/ownership tests passed, but **16 interleaved runs**
did not justify retaining it: navigation **4.481 -> 4.524 ms** (+1.0% cost),
Palace **3.400 -> 3.333 ms** (-2.0% cost). Frequent camera/animation-key changes
limit amortization. The mixed result and extra API/cache complexity are not
being shipped. The candidate implementation and tests were removed; this
does not remove the retained shadow receiver implementation above.

Pinned candidate/baseline binaries, source-before snapshots, image comparison
and `towns-{navigation,palace}.json` remain in
`/private/tmp/actraiser-retained-towns.bzJOZ3/`. A future larger model-instancing
design should separate static geometry from animation instead of repeatedly
replacing an entire projected town mesh.

## Fourth retained pass: cached terrain samples and fork/join projection

The presenter retains the 16,769 ground samples' exact chart normals, height
taps and coastline opacity (~0.83 MiB). Geography, cliff registration, chart
radius or relief enablement invalidates the cache. Camera-dependent work
transforms those immutable samples, reuses the centre point for lighting,
and partitions disjoint output ranges over the existing host helper pool.
The pool is now presentation-owned and shared sequentially by terrain and
weather; it is not tied to cloud enablement. Publication and cliff/cache
access stay on the owner, and reset joins/destroys the helpers.

Sixteen isolated ABBAABBA full-view runs compare against GPU spherical
mapping: navigation **4.465 -> 3.731 ms (16.4% less presentation CPU)**;
Palace **3.400 -> 3.388 ms (0.4%, effectively unchanged)**. The latter's
stationary terrain was already cached. Eight additional interleaved runs
of the same new binary with zero versus three helpers measure navigation
**3.887 -> 3.740 ms (3.8%)**: the multicore contribution is smaller than the
total win, but independently measurable. This does not move live simulation,
texture access or rendering to workers, and creates no runner ABI fields.

All 254 GPU images are byte-identical to the preceding retained version.
A presenter regression compares serial/parallel ground submission hashes
over 16 navigation/Palace, viewport, orbit, lighting and relief combinations,
including held-frame reuse. It passes normally and under ThreadSanitizer.
Evidence: `/private/tmp/actraiser-ground-workers.bH0h9O/`, including
`ground-{navigation,palace}.json`, `ground-workers-navigation.json`,
`image-differences-candidate.json`, `parity-test.log` and
`tsan-parity-test.log`.

## Fifth retained pass: camera-keyed sphere shells

Ocean, atmosphere and cloud bodies now retain separate projected shell
snapshots keyed by the complete initialized camera projection and viewport.
Only camera-dependent geometry is reused: moving weather coordinates,
opacity and all effect switches retain their existing behavior. Failed
preparation never publishes a key; resource reset invalidates all snapshots.
This reuses the existing ocean/cloud storage, moves shared scratch into its
own atmosphere snapshot, and adds no backend or ABI contract.

Sixteen isolated ABBAABBA runs measure navigation **3.734 -> 3.691 ms
(1.2%)**, Palace **3.387 -> 3.187 ms (5.9%)**. All 254 GPU images are
byte-identical, including weather phases, view/quality switches and resets.
CPU presenter tests pass. Evidence:
`/private/tmp/actraiser-shell-cache.2cV7WI/`, `shells-{navigation,palace}.json`,
`image-differences-candidate.json` and pinned executables/source-before.

## Sixth retained pass: shared decoded source tiles

The world-map owner now expands its 256 source tiles once at ROM load, and
updates only the two animated water tiles on a phase change. Repeated map
placements copy those exact texels. The additional cache is 64 KiB; no API
or ownership changed. Bake and BakeBaseline still overwrite every output
pixel on every call and respect arbitrary valid row pitch. Classification
continues using source material indices, not decoded colors.

Sixteen ABBAABBA runs measure navigation **3.686 -> 3.662 ms (0.7%)** and
Palace **3.204 -> 3.167 ms (1.2%)**. This is a small remaining gain, just
above the requested cutoff in Palace, not a large animation rewrite.
All 254 images match exactly. An independent nonuniform tile/palette oracle
also checks every output pixel, untouched pitch padding, dirty publication,
borrowed/copy outputs, both water destinations, all four phases and palette
replacement on ROM reload. Evidence:
`/private/tmp/actraiser-world-tiles.QKeMOz/`, `tiles-{navigation,palace}.json`,
`image-differences-candidate.json`, and `world-map-test.log`.

## Seventh retained pass: static town spans around animated windmills

Windmill pose changes no longer invalidate every static building's projected
vertices. The existing 8 MiB cache retains static spans; a bounded ~55 KiB
table records the current capture's windmill indexes/detail between them.
Replay submits those spans and animated models in the original order. No
FrameSlot or compiler-cache pointer is retained, no model/LOD is removed,
and failed cache capture still falls back to ordinary drawing. Moving
cameras keep the previous no-copy path; only a repeated view warms the cache.

Sixteen ABBAABBA runs measure navigation **3.671 -> 3.609 ms (1.7%)** and
Palace **3.163 -> 2.996 ms (5.3%)**. All 254 images match exactly. The added
GPU regression mixes three windmills with two factories, compares cold and
cached renders across forward/backward pose changes, and verifies that only
the three animated objects revisit the compiler cache. Its initial 800x600
fixture could not distinguish the top-down blade poses; the corrected
1792x1344 fixture proves distinct poses and exact cache parity.
Evidence: `/private/tmp/actraiser-static-towns.thFbm8/`,
`static-{navigation,palace}.json`, `image-differences-validated.json`,
`gpu-fixed-synthetic.log` and `gpu-validated.log`.

## Eighth retained pass: geometry-loop work reduction

Ground, blur and haze quads use the existing 64-quad batch seam while
retaining every original corner, clipping result, material and submission
order. Shell rings reuse the same 96 exact longitude sine/cosine values;
the haze and defocus passes share their identical per-vertex distance
calculation. Extra scratch is ~66 KiB and no interface changes are needed.

Sixteen ABBAABBA runs measure navigation **3.625 -> 3.500 ms (3.4%)**, Palace
**2.992 -> 2.950 ms (1.4%)** for the group. All 254 images are byte-identical
and the CPU presenter tests pass. Evidence:
`/private/tmp/actraiser-ground-batch.Nlq9ha/`, `loops-{navigation,palace}.json`,
`image-differences-candidate.json`, `cpu-test.log` and pinned executables.

## Ninth retained pass: clipped atmospheric draw stream

On a repeated camera/viewport, the atmospheric rim retains the exact clipped
2D quad stream and submits it through the existing renderer seam. Camera
motion uses the original scratch path, so continuous movement does not pay
an extra capture copy. The cache is bounded to 32,768 quads (4.75 MiB maximum,
including indexes), never borrows renderer storage, and drops partial
allocations on failure while continuing ordinary drawing. Reset frees it;
projection changes invalidate it. No sampling, alpha or triangle order changes.

Sixteen ABBAABBA runs show navigation **3.499 -> 3.506 ms (0.2%, noise-level)**,
Palace **2.875 -> 2.808 ms (2.3%)**. Palace runs have more system variance here
(A 2.833–2.967, B 2.725–2.850 ms), but both four-run ABBA groups favor the
candidate. The atmospheric stage itself drops approximately 0.200 to 0.057 ms.
All 254 GPU images match exactly. A CPU regression hashes the complete indexed
triangle stream across cold/warm/cached frames, verifies fewer submissions,
and exercises viewport/altitude changes, failed cached draws, retry and reset.
Evidence: `/private/tmp/actraiser-atmosphere-cache.nQ0LlK/`,
`atmosphere-{navigation,palace}.json`, `image-differences-candidate.json`
and `cpu-test.log`.

## Diminishing-returns cutoff: finer cloud jobs rejected

Reducing the cloud-coordinate minimum partition from 2,048 to 1,024 vertices
let all three helpers share the 4,609-vertex shells. Sixteen ABBAABBA runs
measure navigation **3.461 -> 3.436 ms (0.74%)**, Palace **2.796 -> 2.775 ms
(0.75%)**. Cloud work itself improves ~0.015–0.017 ms, but that is not a 1%
whole-presentation gain. This candidate is removed: the default remains
2,048 with fewer wakeups for small jobs. Ground projection still uses all
three helpers where its larger workload supports it.

This is the stopping point for the current incremental optimization sweep,
not a claim that the renderer is globally optimal. Larger GPU-model or art
pipeline redesigns remain possible but would be separate architecture work.
Evidence: `/private/tmp/actraiser-cloud-grain.ayWBhH/`,
`grain-{navigation,palace}.json`, pinned binaries and `cpu-test.log`.

## Final validation checkpoint (2026-09-11)

Both normal build configurations succeed. All **156 application CTests**
and **35 runtime CTests** pass again, including ABI and positive/negative
layering guards. The final presenter and helper tests pass ThreadSanitizer;
the presenter, decoded-world oracle and GPU depth tests pass ASan/UBSan.
The complete **254-image** GPU sweep also passes under ASan/UBSan. Both its
images and the normal final sweep are byte-identical to the GPU-spherical
reference. The separate spherical mapping oracle retains its expected
one-channel-level difference at only one of 12,288 pixels.

Fourteen actual release-build Palace composites, compared against the
pre-spherical retained-UV version, differ by at most **1/255**, at only
**2–4 pixels per image**, consistent with the already-audited GPU mapping
rounding. These include native foreground, fades and animated windmill poses.
The runs are `runs/20260911-011216` and `runs/20260911-011232`.
MSL/SPIR-V/DXIL regeneration checks still pass; only Metal execution is
available locally. Address/UB tests disable SDL process-global leak detection
(`ASAN_OPTIONS=detect_leaks=0`), so these are not a whole-process leak audit.

Evidence: `/private/tmp/actraiser-globe-final.dHeAQs/`, including build logs,
`ctest-all.log`, `runtime-tests.log`, `tsan-{present,workers}.log`,
`asan-{present,world-map,depth}.log`, `gpu-asan.log`,
`image-differences-{final,asan}.json`, `fullgame-image-differences.json`,
`shader-check.log` and reproducible scripts. Final release SHA-256:
`380553194d17141fb1252c0c293ba600e92f457d973a45c7d12e1babd27e8003`.

## Final end-to-end remeasurement

After validation finished, another **16 isolated ABBAABBA runs** compare the
final binary against the original optimization baseline, which already
contains the location-label classifier fix. Neither version can win by
falling back to cheaper authentic navigation. All effects, the Performance
model ceiling, populated save, replay, viewport and three-helper override
match the earlier protocol. No builds, tests or encodes ran alongside these.

| CPU scope | Original median (range), ms | Final median (range), ms | Reduction |
| --- | --- | --- | --- |
| Navigation presentation | 5.753 (5.741–5.766) | 3.500 (3.499–3.513) | 39.2% |
| Palace presentation | 5.250 (5.228–5.272) | 2.862 (2.850–2.867) | 45.5% |
| Palace native drawing | 7.181 (7.156–7.211) | 0.858 (0.850–0.883) | 88.0% |
| Navigation clouds, included in presentation | 1.620 | 0.494 | 69.5% |
| Palace clouds, included in presentation | 2.042 | 0.343 | 83.2% |

Navigation native drawing is effectively unchanged (~0.342 -> 0.337 ms).
Do not sum overlapping scopes or add their percentage reductions. The hidden
swapchain still has cadence waits; these measurements are CPU-side elapsed
scopes, not hardware GPU timings or predictions of Steam Deck FPS. A fresh
Deck run remains useful for platform-specific confirmation, not necessary
to establish the local improvements above.

The final evidence root contains `end-to-end-{navigation,palace}.json` and
all 16 logs. `benchmark.py` uses `baseline-original`, SHA-256
`3784838a7b89acefe02fdade4e4fbf03d41bf12b9a68f435158e09f70fe9df0f`.
The separate `baseline` executable remains the pre-spherical retained-UV
version (`471000cf...`), used by `validate.py` for full-game image comparisons.
The repository changes remain uncommitted on `builder-desktop-shell`.

## Follow-up: more CPU work on the helper pool (2026-09-11)

The subsequent user-authorized experiments extend the same synchronous
presentation-owned helper pool; they do not thread live emulation or add
renderer calls to workers. The benchmark protocol remains four runs per
variant per view, ABBAABBA, Performance models, all effects and moving weather.

### Building projection batches

The owner resolves compiled models, palettes, shading, terrain anchors and
visibility. Bounded staging copies up to 128 objects / 8,192 faces before the
next cache lookup can evict any borrowed geometry. Helpers project independent
objects using local column caches and write disjoint output ranges. The owner
then clips/submits original object/face order and publishes static spans around
animated windmills. Hidden windmill poses retain an empty animated span so a
different pose can become visible without moving the camera. Small/held views,
unavailable helpers and allocation failure use the same scalar math directly.
Staging is compile-time bounded below 3 MiB, reused, and freed on reset.

Sixteen runs measure navigation **3.509 -> 3.290 ms (6.2%)**, Palace
**2.867 -> 2.867 ms (unchanged)**. Navigation's separately measured native draw
scope varies 0.309 -> 0.347 ms; this does not erase the presentation saving.
All 254 reference images match exactly. A new CPU regression compares worker
and scalar model streams for 256 mixed objects across eight view/lighting/LOD/
relief/orbit cases and four cold/warm/animated/rewound presentations each.
It checks at least 128 initial model lookups, exercising multiple batches.
Evidence: `/private/tmp/actraiser-model-workers.4in0jp/`, including
`models-{navigation,palace}.json`, pinned binaries and the image sweep.

### Animated map reconstruction rows

The portable art module prepares a fixed ~280 KiB pixel-work value containing
explicit source image descriptors, change mask, pre-resolved read-only atlas
tiles and feather weights. Separate row execution has no allocation, lookup,
thread dependency, renderer type or global mutation. The presenter reuses one
plan and dispatches disjoint world-cell row ranges; source halos only read
immutable input images, never adjacent jobs' output. After joining, the owner
applies mountain patches and performs the existing transactional texture upload.
The old synchronous entry point remains a wrapper around the same pixel work;
allocation failure selects it. Source failure does not modify the image/mask.
No complete animation-phase image cache, art approximation or ABI field is added.

Sixteen runs measure navigation **3.300 -> 3.140 ms (4.8%)**, Palace
**2.862 -> 2.668 ms (6.8%)**. Palace's separate native draw scope changes
0.850 -> 0.891 ms; the combined saving remains positive. All 254 images match
exactly. Full-bake pixel oracles now also cover reordered, uneven row ranges,
transparent animation, all town/style gates, unchanged phases and pitch padding.
Evidence: `/private/tmp/actraiser-animation-workers.9j7tJQ/`, including
`animation-{navigation,palace}.json`, `art-final.log` and the image sweep.

### Mountain projection batches

The owner still resolves terrain heights and prepares retained face samples.
The pure projection function now accepts explicit lighting and geometry values,
not a FrameSlot or a terrain-lookup fallback. On a changing camera, disjoint
face ranges update the existing projected storage through the same helper pool.
Clipping, submission and key publication follow the join in original order.
Held views skip the jobs entirely. Failed sample allocation keeps the scalar
path, with the owner resolving each temporary input before projection. No
additional retained allocation or interface change is needed.

Sixteen runs measure navigation **3.163 -> 3.020 ms (4.5%)**. Palace is
**2.645 -> 2.659 ms** (0.5% higher, overlapping run ranges; its held projection
was already cached). All 254 GPU images match exactly, and CPU presenter tests
pass. Evidence: `/private/tmp/actraiser-mountain-workers.KqalGP/`, including
`mountains-{navigation,palace}.json`, pinned binaries and the image sweep.

The `depth-project` per-call maxima now refer to model batches rather than
individual buildings; compare per-presentation totals, not those differently
grouped maxima. These gains still describe Mac CPU-side elapsed scopes, not
Steam Deck FPS or GPU timings. Full world-space GPU model instancing remains
a separate architectural option; it is not part of this CPU batching change.

### Follow-up validation

Both builds succeed and the final GPU-enabled CTest run passes **156/156**;
the unchanged runtime suite passes **35/35**. The initial sandboxed CTest run
could not initialize video for the shader test; `ctest-sandbox.log` preserves
that environment failure. Re-running with display/GPU access passes all tests.
The final presenter passes **ThreadSanitizer** with three helpers; presenter,
art oracles and helper lifetime tests pass **ASan/UBSan**. SDL process-global
leak detection remains disabled for those address/UB checks; this is not a
whole-process leak audit. The final normal 254-image GPU sweep matches the
reference byte-for-byte. No backend shader/resource contract or frame ABI was
changed by this follow-up. Pure geometry helpers also shed unused FrameSlot/
viewport parameters rather than forwarding irrelevant game metadata.
All **14 actual full-game Palace composites** also match the prior build
byte-for-byte (`runs/20260911-044840` and `runs/20260911-044857`). They include
native foreground, fades and animated poses, not just the isolated globe.

Final evidence: `/private/tmp/actraiser-multicore-final.Y2jRmS/`. The prior
build (`baseline`) SHA-256 is
`380553194d17141fb1252c0c293ba600e92f457d973a45c7d12e1babd27e8003`;
the final release (`optimized`) SHA-256 is
`1d9eeeab2a9285aafed320faffcda34b1655d097302330a2933b066144a74faa`.

### Multicore retention policy

Per the user's follow-up, a sub-1% Mac result alone is not a reason to remove
a safe portable multicore path. Relative scalar throughput, thread wakeup cost
and memory contention can differ on Steam Deck. Retain the bounded helper
implementation and scalar fallback, and use `AR_RENDER_WORKERS=0..3` for
controlled hardware comparisons. Mac timings establish local behavior, not
the optimal Deck worker count or partition size.

Terrain, model, mountain, cloud and animation-row workers are all retained.
The earlier discarded 1,024-element cloud-grain experiment changed scheduling,
not the available computation path; its evidence remains available above for
Deck-specific retuning. The 2,048 default uses fewer helpers on small cloud
shells, while larger jobs can use all three. No current multicore path was
removed because a stationary Palace view already cached its output.

### Follow-up cumulative remeasurement

After final validation, 16 isolated ABBAABBA runs compare the pinned prior
release with the final release, both using three helpers. These measure the
combined building, animation-row and mountain changes above, not the earlier
optimization series again. All effects, animated weather, populated save,
Performance models and the 1,792 x 1,344 viewport match. No builds, tests or
encodes ran alongside these benchmarks.

| CPU scope | Prior median (range), ms | Final median (range), ms | Reduction |
| --- | --- | --- | --- |
| Navigation presentation | 3.513 (3.512–3.519) | 3.030 (3.007–3.033) | 13.7% |
| Palace presentation | 2.846 (2.842–2.892) | 2.668 (2.645–2.673) | 6.2% |

The separate native drawing scopes rise slightly: navigation 0.341 -> 0.363 ms,
Palace 0.862 -> 0.895 ms. Cloud time, already included in presentation, also
rises (navigation 0.495 -> 0.544 ms; Palace 0.343 -> 0.357 ms). The overall
presentation saving outweighs these regressions; not every scope gets faster.
The hidden swapchain remains cadence-limited, so these are CPU-side elapsed
scopes, not GPU utilization measurements, whole-frame speedups or Deck FPS
predictions. Evidence: `final-{navigation,palace}.json` and all 16 logs under
the final evidence root above.

A separate eight-run navigation ABBAABBA comparison uses the **same final
binary**, changing only `AR_RENDER_WORKERS=0` versus `3`. Presentation falls
from **3.712 ms (3.687–3.718)** to **3.033 ms (3.027–3.047)**: **18.3%** lower
CPU-side elapsed time with helpers enabled. This isolates current multicore
support from the other caching and scalar improvements. Evidence:
`workers-navigation.json`. Zero helpers remains a supported diagnostic and
failure-fallback path, not the new default.

The same-binary Palace worker comparison was repeated because its first
ABBAABBA batch showed a within-batch timing shift:

| Palace presentation CPU | No helpers median (range), ms | Three helpers median (range), ms | Reduction |
| --- | --- | --- | --- |
| Initial eight runs | 2.854 (2.808–2.900) | 2.591 (2.482–2.682) | 9.2% |
| Independent eight-run repeat | 2.800 (2.783–2.825) | 2.609 (2.582–2.618) | 6.8% |

Both batches favor workers, but their different magnitudes are evidence
against presenting one exact universal speedup. The repeat's separate native
drawing scope is 0.804 -> 0.827 ms and its included cloud scope is effectively
unchanged (0.358 -> 0.357 ms). Evidence: `workers-palace.json`,
`workers-repeat-palace.json` and all 16 logs. Across navigation and Palace,
the final cumulative and worker-only comparisons total 40 isolated runs.
The next hardware-specific check is the same populated replay on Deck with
0, 1, 2 and 3 helpers, keeping graphics and power settings fixed. The Mac data
does not establish Deck's best helper count or justify removing multicore
support or more aggressively shrinking its jobs.
