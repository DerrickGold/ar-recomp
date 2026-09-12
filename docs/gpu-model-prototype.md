# Retained geometry experiments — 2026-09-11

Checkpoint: `2a539c3b` on `builder-desktop-shell`, including pending performance,
audio and packaging work. The existing CPU/multicore renderer remains the
reference and fallback. Held-view geometry retention is now the default;
The affine/hardware-clipped mesh harness below remains a separate experiment.
A later [resident radial globe path](gpu-world-models.md) integrates GPU
placement/projection into navigation and Palace views, enabled by default for
offline testing. `AR_SIM3D_WORLD_GPU_MODELS=0` restores the CPU model path.

## Two distinct paths

1. **GPU model projection (standalone harness only).** Retain world-space
   positions/colors and change an 80-byte camera/viewport uniform. No game view
   uses this shader. This measures potential savings, not Deck FPS.
2. **Held-view retention (default real-game integration).** Retain existing
   CPU-projected, clipped, colored geometry with its existing shader:
   static model ranges and the complete ocean/mainland/cliff Ground batch.
   `AR_SIM3D_RETAINED_SOLIDS=0` and `AR_SIM3D_RETAINED_GROUND=0` independently
   disable them for diagnosis. Unset (or `1`) enables them, subject to normal
   eligibility/fallback rules. Read once per presentation-resource generation.
   No saved settings or runner ABI changes. Moving cameras still use the
   unchanged workers, culling, LOD, clipping and authored models.

Neither implements instancing, async compute, GPU culling or a new compositor.
Held-view retention avoids repeated collection/upload; its initial projection
still runs on the CPU.

## Ownership, ordering and invalidation

The project-private depth contract accepts plain values and opaque handles.
The adapter owns GPU layouts, buffers and submission. No WRAM, FrameSlot,
game camera policy, compiler or town identity crosses that boundary.

Updates copy inputs; model appends copy transforms. Queued handles cannot be
updated. Destroying queued geometry safely fails the pass. Reset invalidates
GPU contents, preserving handles for republication. Projected meshes require
the same viewport; world-space meshes do not. Optional allocation/publication/
range rejection falls back to ordinary geometry, not authentic graphics.
Actual render submission failure retains the existing failure contract.

Ordinary geometry and retained ranges interleave in exact per-material order,
preserving equal-depth alpha and windmills between static ranges. Each sample
records its ordinary-stream insertion point; adjacent ordinary calls coalesce.
Transparent-material contracts are unchanged. Ground retention adds no draws;
splitting models around animated objects can add draws.

`CaptureGeometryMesh` copies a complete ordinary material batch into owned
CPU staging—not GPU readback. It leaves current draws untouched. Empty,
sampled, queued, failed and over-budget batches reject. The presenter captures
ocean before mainland/cliffs. Current atlas textures remain live, so retaining
geometry does not freeze water animation.

Models use the existing projected-model key/revision. Ground uses the full
projection/viewport, geography serial, cliff revision, source layout and light
key. Existing CPU terrain/ocean caches must remain valid for weather/haze.
Changed views draw normally; the second identical view captures ground, then
warm views reuse it. Unavailable optimizations are not retried every frame.
Presentation reset releases both handles and resets enablement/publication state.

The default-enablement audit found a shared-budget risk: many interleaved
model ranges could exhaust the sample slots used later by cloud shadows.
Budgets are now isolated: 16 effect handles and 64 effect samples remain
available, independently of four opaque handles and 64 opaque ranges. Both
domains remain bounded; opaque cache exhaustion cannot omit weather. Each
mesh remains limited to 262,144 vertices.
Each full-vertex mesh reserves at most 10 MiB vertex + 10 MiB transfer storage,
excluding driver cycling. These two new caches add at most 40 MiB explicitly
allocated backend storage, in addition to existing CPU/batch caches. Existing
multicore fallback and its caches are preserved. Four opaque handles permit
up to 80 MiB explicitly allocated backend storage in that domain, but the
current presenter uses only two (40 MiB maximum, normally much less).

Static-model retention also requires at most **16 nonempty static spans**.
More fragmented streams keep their existing single ordinary model batch;
this bounds additional Solid draws to 32 without removing any models or
changing LOD. A 144-object/72-windmill stress fixture originally grew to 137
total draws with both caches. The guard keeps it at 11, matching the ordinary
renderer, while ground retention reduces uploads from 5,267,344 to 1,186,720
bytes/present. All 778,384 submitted vertices (including repeated weather
samples) and every image pixel are preserved.

The GPU adapter tests exhaust both sample budgets in either order, then
submit all 128 retained commands plus ordinary fallback. They also queue
192 frames of changing geometry/atlases with only periodic readbacks, to
exercise in-flight buffer/transfer cycling. Presenter tests exhaust all opaque
handles and confirm ordinary image/draw/upload parity with weather intact.
Unset/default behavior is checked against explicit enablement, not inferred
only from opt-in runs.

## Projection precision and clipping

The initial shader differed at 21 pixels across eight 1280×800 comparisons,
including two silhouette pixels. Explicit fused operations matching the tested
CPU expression plus reciprocal refinement resolved this without relaxing the
oracle. The shader retains legacy affine interpolation and viewport/depth
mapping (`W=1` after projection).

The expanded suite compares **192 camera/viewport combinations**: 32 poses at
1280×800, 800×600, 1792×1344, 1279×799, 853×641 and 1920×1080.
Zero- and three-helper references both have **zero differing pixels**, including
coverage, across 241,751,744 pixels per sweep. Debug and RelWithDebInfo passed
on Metal. This does not prove Vulkan/D3D12 or x86 compiler parity.

Retained AABBs are conservatively checked against all six planes, including
float overflow/cancellation bounds—not just an ideal double transform.
Clipped, behind-eye or uncertain transforms reject before queuing. Ordered
CPU fallback ranges are supported. Hardware-clipped edges, pixel-clean snapping
and town-facing lean are not approved. No Mac-specific CPU math policy was added.

### Whole-object hardware clipping follow-up

The interior-only guard is a limitation of the **legacy-parity experiment**,
not a requirement to render partially visible objects on the CPU. Clipping
keeps the visible part of a primitive; culling removes an invisible object.
The scene owner should still coarsely cull whole invisible objects/chunks.

`CreateHardwareClippedModelMesh` now provides a separate experimental policy,
using the same owned model vertices, Update/Append calls, 80-byte uniform,
depth attachment, Solid ordering and opaque budgets. Its vertex shader keeps
homogeneous W, maps Scene3D's -W..W depth to SDL GPU's 0..W, and lets hardware
clip before division. Zero/negative-W source corners and partially off-screen
meshes are accepted without CPU projection or polygon clipping. The adapter
still rejects nonfinite/overflow-prone transforms. Camera changes do not
re-upload vertices. No per-range CPU index or per-object GPU draw was added.

This policy has its own lazy vertex/fragment shaders, generated for MSL,
SPIR-V and DXIL. Screen-linear (`noperspective`) color interpolation avoids
silently switching the existing authored shading to perspective interpolation.
However, the legacy CPU clipper interpolates newly generated edge colors in
homogeneous space and then rasterizes them affinely. Its clipped-edge colors,
triangulation and projection/depth rounding need not match hardware clipping
bit-for-bit. Differences are evidence to inspect, not evidence that GPU
clipping is impossible or permission to lower the image oracle's threshold.

The old exact-parity shader and default game paths are unchanged. No shipping
view uses either model-projection experiment yet; globe placement/deformation,
source-cache ownership, LOD and town-facing/pixel-clean policies still need
real-view integration. Multicore projection remains available as the reference
and fallback. This does not add async compute or GPU-side visibility selection.

GPU tests exercise **54 flat-color cases** against the production CPU clipper,
covering every frustum plane, partially and wholly invisible geometry, negative
and zero W, a viewport-covering quad with all corners outside, degeneracy,
depth occlusion, viewport changes and reset/republication. They also verify
unchanged warm uploads, copied inputs/transforms, overflow rejection, budget
exhaustion, queued destruction and equal-depth translucent interleaving with
ordinary/retained geometry. These tests pass exactly on Metal in Debug and
RelWithDebInfo. Vulkan/D3D12 blobs compile offline but have not been run here.

The authored-model comparison has explicit `cpu-clipped`, `gpu-clipped` and
`verify-clipped` modes. It sweeps retained Low town models through the viewport
edges and uses the production CPU clipping code as the reference. The repeated
runner's `--hardware-clipping` option runs moving cameras only: the existing
harness caches projected vertices but not post-clipped polygons, so a held-view
comparison would misrepresent the production cache. Verification/readback is
separate from serial ABBAABBA timings with zero and three requested helpers.
Strict mismatches remain a failed verification; timing results cannot promote
the prototype by themselves.

```sh
python3 tools/compare_model_projection.py \
  build-tests/actraiser_model_projection_benchmark --hardware-clipping \
  --output /path/to/new-clipping-evidence --frames 480
```

Final follow-up evidence: `/private/tmp/actraiser-hardware-clipping.fjH8zZ/`.
Two serial batches total 32 timed runs; each batch has four runs per variant
and helper count, 120 warmup + 480 measured frames. The final batch includes
the conservative float-overflow rounding margin. Final binary SHA-256:
`050e11f19c7b9d0be0e99924a6540db64ac598c9a45a3e784572644f47016c5c`.
Mac/Metal model-only CPU wall time, median [min–max] milliseconds:

| Requested helpers | CPU projection + clipping + submission | GPU-resident whole mesh |
| --- | ---: | ---: |
| 0 | 2.655 [2.489–2.786] | 0.085 [0.069–0.094] |
| 3 | 2.171 [2.069–2.261] | 0.091 [0.084–0.093] |

The preceding batch measured 2.577 → 0.088 ms (0 helpers) and 2.158 → 0.087 ms
(3 helpers). Both paths issue one model draw; warm vertex uploads fall from
2,846,167 bytes/frame to zero. These are ~96% reductions in this isolated
stage, **not whole-game or Steam Deck FPS gains**. Frames remain paced near
120 Hz. The CPU reference still uses its existing worker pool; visibility/LOD
selection and initial globe/model construction are excluded from timing.

Each final 192-pose/viewport authored-model sweep covers 6,452,980 CPU-painted
pixels, 8,136 partially clipped quads and 214,057 wholly outside quads. Both
helper configurations produce the same mismatch totals: 3,683 pixels differ,
including 105 coverage changes. That is approximately 0.057% relative to the
painted-pixel count, but not all are tiny channel-rounding changes: 1,775
pixels have a maximum channel difference above 8/255. The strict oracle
therefore **fails**, and default enablement is not approved. Worst-pose images
are retained for review; the small count does not establish motion stability
or prove the higher-delta pixels are harmless. The legacy model shader's
192-case sweep remains byte-identical. Application tests pass 162/162; after
the final extreme-float guard, focused GPU tests pass again in both build types.

See [GPU offload audit](gpu-offload-audit.md) for the other CPU visual work and
the distinction between expensive fallbacks and useful coarse culling.

## Standalone measurements

Apple M2 / macOS 15.7.7 / Metal, RelWithDebInfo (`-O2 -g -DNDEBUG`), 1280×800.
512 actual Low/Architectural town models: 76,960 vertices. Compilation, globe
placement, visibility and LOD selection are outside both timed paths. Terrain,
weather, UI, simulation and animated models are absent. Held CPU views already
cache projection, but still collect/upload ordinary geometry.

Final 32-run ABBAABBA batch: four runs/variant, 480 measured frames after 120
warmup frames. Median scene CPU ms; brackets show min–max run average:

| Camera / helpers | CPU reference | GPU-resident model path |
| --- | ---: | ---: |
| Moving / 0 | 2.438 [2.224–2.657] | 0.092 [0.092–0.093] |
| Moving / 3 | 1.978 [1.864–2.004] | 0.092 [0.092–0.094] |
| Held / 0 | 1.237 [1.230–1.264] | 0.093 [0.092–0.094] |
| Held / 3 | 1.256 [1.241–1.285] | 0.094 [0.087–0.106] |

Both issue one draw. Warm geometry uploads: **3,078,400 bytes/frame → zero**;
uniforms/commands remain. Savings combine retention, avoided CPU projection,
collection and upload—not just matrix multiplication. The earlier batch had
lower CPU times (moving/3: 1.646 ms); report repeated ranges, not best samples.

End-to-end frames remain near 8.26–8.28 ms, consistent with compositor pacing
near 120 Hz. These are CPU wall times, not GPU timestamps. **The isolated ~95%
model-stage reduction is not a whole-game FPS gain.**

Evidence: `/private/tmp/actraiser-model-followup.dshVmP/final-model-timing/`.
Binary SHA-256: `7f76bc22fa4b87483881f463f8109306803452643866560877583e2647f70b77`.

## Full-game held-view measurements

Same optimized binary, Performance-quality models, three requested helpers.
The runner copies saves/settings, checks input hashes, runs serial ABBAABBA
trials, and excludes the first three reporting windows. Captures/readback are
separate. These replays also remain presentation-paced near 120 Hz.

Static models alone, final 16-run batch:

| View | Presentation CPU ms, off → on | Depth upload MiB/present | Depth draws/present |
| --- | ---: | ---: | ---: |
| Sky Palace | 2.732 → 2.586 (5.3% less) | 8.735 → 6.241 | 16 → 22 |
| Navigation tour | 3.093 → 3.057 (1.2% less) | 9.139 → 8.663 | 15 → ~16.3 |

A preceding 16-run batch showed 6.3% and 0.3%, respectively. Palace is a
repeatable benefit; navigation's small reduction overlaps noise and depends
on time spent stationary. Moving-camera GPU projection is still needed.

Evidence: `/private/tmp/actraiser-model-followup.dshVmP/final-timing/` and
`timing/`. Both caches together pass **33 byte-identical full-game composites**
(14 Palace + 19 navigation) and identical final WRAM hashes; see
`/private/tmp/actraiser-model-followup.dshVmP/ground-composites/`.
Both caches together, separate 16-run ABBAABBA batch:

| View | Presentation CPU ms, off → on | Depth upload MiB/present | Depth draws/present |
| --- | ---: | ---: | ---: |
| Sky Palace | 2.759 → 2.314 (16.1% less) | 8.735 → 4.563 (47.8% less) | 16 → 22 |
| Navigation tour | 3.037 → 2.963 (2.4% less) | 9.139 → 8.120 (11.2% less) | 15 → ~16.3 |

Palace run averages: off 2.718–2.859 ms, on 2.309–2.336 ms. Navigation:
off 2.927–3.073 ms, on 2.893–3.020 ms. Navigation ranges overlap; its small
gain needs more evidence before treating it as a reliable whole-view gain.
Dynamic weather/reporting-window boundaries cause slight average vertex-count
variation; fixed-state comparisons preserve exact pixels and vertex counts.

Evidence: `/private/tmp/actraiser-model-followup.dshVmP/combined-timing/`.
Application SHA-256: `3462f80777ffdf3b23015cc196a228bdc6abb634af4e141e231f4741eaf265de`.
The ROM-free animated-factory test independently checks ground retention adds
no draws, retains all 51,176 vertices, and lowers warm depth uploads from
1,995,840 to 52,320 bytes/present after model retention is already enabled.

## Default-enablement measurements

An additional **48 serial full-game runs** compare both caches off/on: four
runs per variant and view, ABBAABBA, across three preset/helper configurations.
Each pair uses the same executable and immutable input hashes, isolated saves
and settings, no captures, and the same replay. All final WRAM hashes match.
No render failures/fallbacks were counted in the pipeline windows. Helper
activity remains present with three requested workers and absent with zero.

The legacy presentation logger rounds to 0.1 ms. This batch additionally uses
the existing fine-grained pipeline reducer, filtered to the requested scene,
excluding its first matching window and weighting remaining windows by their
present count. Its **drawing scope is not the same scope as the legacy overall
presentation timer**. Render CPU below sums nonoverlapping top-level render
pipeline scopes, not simulation, whole-frame latency or GPU timestamps.
Medians of run averages, milliseconds off → on:

| Preset / helpers | View | Drawing CPU | Render-pipeline CPU | Depth upload MiB/present |
| --- | --- | ---: | ---: | ---: |
| Performance / 3 | Palace | 2.562 → 2.138 (16.5% less) | 3.544 → 3.223 (9.0% less) | 8.735 → 4.563 |
| Performance / 3 | Navigation | 3.069 → 3.013 (1.8% less) | 3.432 → 3.381 (1.5% less) | 9.138 → 8.126 |
| Balanced / 0 | Palace | 2.777 → 2.367 (14.8% less) | 3.717 → 3.385 (8.9% less) | 8.749 → 4.563 |
| Balanced / 0 | Navigation | 3.753 → 3.670 (2.2% less) | 4.075 → 3.994 (2.0% less) | 9.206 → 8.323 |
| Quality / 3 | Palace | 2.565 → 2.145 (16.4% less) | 3.548 → 3.234 (8.9% less) | 8.749 → 4.563 |
| Quality / 3 | Navigation | 3.042 → 2.985 (1.9% less) | 3.406 → 3.357 (1.4% less) | 9.205 → 8.189 |

Every configuration's on/off ranges are disjoint for both fine-grained scopes.
Navigation render CPU ranges, off versus on: Performance 3.411–3.442 versus
3.379–3.383 ms; Balanced 4.070–4.146 versus 3.988–4.010 ms; Quality 3.394–3.417
versus 3.339–3.378 ms. This is stronger evidence than the earlier coarsely
rounded/overlapping samples, but still a small moving-tour improvement, not
evidence that moving geometry now projects on the GPU. Upload reduction is
about 48% in Palace and 10–11% in navigation; normal scene draws remain
16 → 22 and 15 → about 16–16.3, respectively.

Evidence: `/private/tmp/actraiser-retained-default.9RfSlD/`, subdirectories
`performance-3`, `balanced-0` and `quality-3`. Performance was measured after
budget isolation but before the static-span guard/default switch (binary
`f16e8259ca3ea384acc41b4d00941452c154f3e0d837e108ca57fc8a9f5e3ab7`).
Balanced and Quality use the final guarded/default candidate
`b8e263613d32a6a01017f8e57e8277780401b13a36b81e3b7f81fea0e92c8999`.
Both variants explicitly set their overrides for timing; separate pixel tests
exercise the unset/default policy. The tested replay scenes do not hit the
fragmentation limit. Preset and helper counts vary together here, so this is
not a controlled benchmark of helper scaling.

## Reproduction and checks

```sh
cmake --build build-tests --target actraiser_model_projection_benchmark
python3 tools/compare_model_projection.py \
  build-tests/actraiser_model_projection_benchmark \
  --output /path/to/new-evidence-directory --frames 480
python3 tools/compare_retained_solids.py --help
```

Game runner: `--feature solids` toggles models with ground off;
`--feature ground` toggles ground with models fixed on; `--feature all` toggles
both. `--verify-only` compares exact composites/state rather than timings.
`--default-candidate --feature all` compares forced-off against unset/default
policy instead of explicit `1`. `--preset` and `--workers` select test coverage.
Supply private ROM, save, settings and replay paths explicitly. Do not build
or run other GPU work during timing batches.

All **162 Debug tests** pass with the final default-on policy, including runner/render
boundary guards. The final adapter also passes AddressSanitizer and
UndefinedBehaviorSanitizer on Metal (driver leak detection disabled), shader
blob validation, and both 192-case strict model-image sweeps. New GPU tests
cover copying/lifetime, all-plane/overflow/cancellation rejection, mixed
ordering, sample exhaustion fallback, live atlases and warm-upload counters.
A 12-state cold/repeated/warm sequence checks live geography edits and restore,
lighting, relief, detail, orbit and Palace/navigation transitions against the
ordinary path. These comparisons remain byte-exact.

The optional real-art all-town/effect/weather matrix also passes with both
caches off and on: **254 matching PPM files, zero differences**. It includes
all six towns, near/middle/wide/HD views, effect and model-quality toggles,
animated weather, Palace views and Advent assertions. Read-only snapshot:
`runs/20260910-214041/snapshots/vd_gf610.wram.bin`; evidence:
`/private/tmp/actraiser-retained-matrix.CnOApU/{off,on}` and sibling logs.
The final guarded/default candidate repeats this matrix with both overrides
**unset**, against forced-off: **254 byte-identical images** in
`/private/tmp/actraiser-retained-default.9RfSlD/{matrix-off,matrix-default}`.
The final optimized candidate also passes **33 full-game Quality-preset
composites** (14 Palace + 19 navigation), forced-off versus unset/default,
with identical final WRAM hashes. Evidence:
`/private/tmp/actraiser-retained-default.9RfSlD/quality-default-composites/`.

MSL, SPIR-V and DXIL ship together; only Metal executed here. Three pre-existing
assert-based setup patterns in `actraiser_present_world_nav`, its profiled
variant and the model-cache storage fixture fail under NDEBUG. Use Debug for
the full regression suite, optimized builds for benchmarks. Final default-suite,
sanitizer and strict shader-sweep logs are in
`/private/tmp/actraiser-retained-default.9RfSlD/`; the two shader sweeps again
report zero differing pixels across all 192 cases each. The shader source/blob
regeneration check also passes.

## Remaining architecture work

Next: **camera-independent globe geometry**, not another screen-coordinate
cache. Globe rotation and radial displacement currently precede projection.
Collapsing them into one matrix reassociates float math and can change depth/
silhouettes. Preserve ordered math under a plain-value deformation contract,
bounded source chunks, explicit revisions and unchanged LOD.

Keep ordered CPU ranges for clipped/uncertain chunks and animated models.
Compare tiny silhouettes, terrain intersections, Advent near-plane cases and
town pixel-clean modes. Do not pass game state into the adapter or discard
workers because of Mac single-core results.

Before enabling **GPU model projection**, validate Vulkan/D3D12 and CPU compiler
differences, then whole-view GPU time, memory and power on Deck. The held-view
cache default has a narrower justification: it uses the same packed vertices,
shader, clipping, textures and depth/blend state as ordinary submission; it
adds no new device feature requirement. Existing weather already uses retained
buffers through this adapter. Bounded fallback, budget isolation, draw-call
limits and opt-outs address its added lifetime/memory/fragmentation risks.

Native tests here execute Metal only; no usable Vulkan/D3D12 test runtime was
found/configured on this host. An x86_64 C11 compile checks adapter syntax/layout, not
native driver execution. Deck smoke testing and GPU/power measurements remain
valuable and are not claimed by the Mac results. Higher GPU utilization alone
is not the goal: lower frame time at unchanged quality is.
