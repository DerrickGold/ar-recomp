# Shared GPU cliff source — 2026-09-12

Status: **enabled by default**, including the enclosing shared GPU land renderer,
following the user's acceptance of the measured performance/memory tradeoff.
`AR_SIM3D_WORLD_GPU_GRID=0` requests the complete compatibility path; unsupported
or failed GPU source setup also falls back there automatically. The hybrid
GPU-land/CPU-cliff branch and `AR_SIM3D_WORLD_GPU_CLIFFS` switch are removed.
Grid culling remains on by default. No saved quality
setting, authored art, model LOD or worker-count default changes. The measurements
below record the earlier opt-in checkpoint, including its isolated pixel changes;
promotion does not claim those old CPU/GPU differences disappeared.

The subsequent [shared ocean source](gpu-ocean-source.md) and
[mountain source](gpu-mountain-source.md) are also default and remove both CPU
stream caches, their obsolete toggle and partial shadow-receiver branches. The
implementation/measurement sections below describe the earlier cliff checkpoint.

Default/cleanup validation: all 165 app tests pass. The Metal world fixture now
compares an explicitly enabled source against the unset/default path across all
17 states. The declining-adapter test verifies that the unset/default path attempts
GPU source setup once, then preserves complete compatibility output. All 12
Palace and 16 navigation captures are byte-identical to `3e18ba68` with its two
old opt-ins enabled, with matching final WRAM; no new image difference is introduced
by this cleanup. Two further full-game Palace captures also match when the
candidate launcher explicitly removes both old experiment flags from its
environment. Evidence: `/private/tmp/actraiser-world-default.fnjNJ4/`.

## What changes

Registered cliff faces now follow the land grid in the same retained GPU source.
Ground, blur, active-town haze and spherical cloud shadows reuse their exact
homogeneous positions and triangulation. Camera movement, lighting and weather
change copied uniforms, not CPU cliff projection, clipping or geometry uploads.
The ordinary cliff overlay and shadow-receiver paths are skipped only after the
complete GPU group queues successfully. Ocean and mountains retain their existing
CPU geometry path and the previously committed held-view stream cache.

Cliff texture coordinates cannot also locate the active-town mask: skirt faces
can deliberately sample one rock texel while spanning different chart positions.
`Sim3DDepthPass_UpdateSurfaceMeshWithMask` therefore accepts an optional copied
array of generic mask coordinates, independently of texture UVs. The existing
update entry point still uses texture UVs as its mask. The renderer knows neither
town identity nor chart policy; the presentation owner supplies both arrays.

## Contract, lifecycle and cost audit

- No runner ABI, rendering vtable, native handle, game-state access in the SDL
  adapter, runtime shader compiler, readback, fence or compute-queue change.
  This extends the project-private depth surface API. Metal, SPIR-V and DXIL
  shader blobs are generated offline.
- Explicit mask coordinates must be finite and within `[-16,16]`. The complete
  input is validated before retained data is changed. Both arrays are copied;
  rejected last-corner input preserves the preceding publication. Queued source
  updates are forbidden. Existing layer append validation remains atomic.
- Source construction uses the existing bounded fork/join worker pool, including
  cliff work in batches of 256 faces. Terrain arrays are prepared by their owner
  before workers read them; all workers join before source publication or mutation.
  Source validity follows geography, cliff revision, chart radius and relief ratio,
  not camera or weather. Resource reset destroys the mesh and diagnostic latch.
- The existing world opaque handle is replaced, not supplemented. The four-handle,
  64-sample and 65,536-source-quad limits are unchanged. The owner rejects excessive
  cliff counts before allocating or queuing the source. Failure latches the GPU
  group off until reset and uses the ordinary complete world.
- Sixty-four grid chunks plus one ordered cliff tail produce at most 33 disjoint
  selected ranges after adjacent ranges merge. The tail's conservative radial
  bounds include every cliff height. It can retain extra offscreen faces, but
  does not add per-cliff draw calls or change primitive order.
- Independent mask coordinates grow every packed surface quad from 224 to 256
  bytes, fourteen to sixteen float4 attributes, even with the cliff flag disabled.
  At the unchanged maximum, source and transfer storage are each 16 MiB, plus
  up to 16 MiB for selected geometry, before backend cycling. Adding cliffs can
  also cross a power-of-two capacity boundary. CPU construction arrays are bounded
  and freed after publication; the ordinary projection allocation remains available
  for fallback. This is not a free-memory optimization.
- Effects remain independently toggleable. Texture UVs, face elevations and authored
  face shade are unchanged; lighting normals are constructed from the same terrain
  samples in camera-independent source space. The new path submits some additional
  GPU-clipped cliff faces that the CPU path previously culled.

## Correctness

All 165 local app tests pass, including real Metal, layering and ABI tests.
The expanded depth fixture passes on Deck/Vulkan too. Independent-mask tests cover
analytic overlay coverage, copied lifetimes, invalid-final-coordinate atomicity,
selection/compaction, mixed ordering, queued-update rejection and reset. Existing
clipping, texture interpolation, depth and budget oracles retain their thresholds.
D3D12 blobs compile, but there is no D3D12 runtime test here.

The real-Metal world fixture exercises authored synthetic Bloodpool cliffs with
the new flag both off and on. Within each path, unculled/uncached, default cold
and warm images match exactly through 17 geography, lighting, relief, weather,
view-family, viewport and Advent states, three presentations per state. Actual
compaction and CPU stream reuse are required, with zero geometry rejection.
Warm depth upload falls 5,656,544 to 5,602,112 bytes in the authored-cliff fixture.
The declining-adapter unit fixture preserves complete ordinary land/ocean shadows
with either cliff setting and makes only one failed attempt per resource lifetime.

Full-game comparisons against the committed GPU grid plus CPU cliffs are **not
pixel-identical**. At 2160x1344, each of 12 Palace captures differs in 53 of
2,903,040 pixels; only one pixel exceeds two channel levels, with maximum error
12. Sixteen navigation captures differ in 14–174 pixels, with 0–2 pixels per
capture exceeding two levels and isolated maximum error 85. Most differences
are one 8-bit level. Full-frame inspection shows no broad lighting or mask seam;
that inspection is not an exact-parity claim. Both final WRAM dumps match.
Strict comparison failures and per-pixel evidence are retained; no tolerance
was increased. These incremental differences do not resolve the earlier grid
versus CPU geometry acceptance gate.

Two separate Deck captures at GF600/GF900 differ in 14 of 725,760 pixels each,
all by exactly one 8-bit channel level (17 changed channels), with matching
WRAM. Their strict comparator likewise fails; `visual/pixel-diff.json` records
the quantified result. With the cliff flag off, the fresh binary's two image
hashes match the previous stream-cache probe exactly, checking compatibility of
the new mask layout on this full-game Vulkan view.

## Measurements and evidence

Local evidence: `/private/tmp/actraiser-gpu-cliffs.G2nrc7/`. Pinned binary hashes:

- Mac control (`9fa39d21`): `66f17d3dc978eb11acbfcdc6e85642052d0294f1c7f3989cc9722091ca22627f`.
- Mac candidate: `609ad3c291ccb306668d4b7c6acb5adfc0460f84bca511e5f259276de8477ec8`.
- Linux game: `3fbd26c9bacd43e6cb1b1340df115ed47f41918afec996a81bb35da654fdc3f6`.
- Linux depth fixture: `8e7f284b92f228ae6a965b67c4565def7f2f524d72855ad7b1d27b6281faea93`.

Reproduce local comparisons with `tools/compare_pipeline_performance.py`, manifest
`/private/tmp/actraiser-world-surfaces.NDSzGz/checkpoints.json`, config
`/private/tmp/actraiser-deck-profile.2Y9hIv/benchmark-defaults.ini`, and both variants
setting `AR_SIM3D_WORLD_GPU_GRID=1`, `AR_SIM3D_WORLD_GPU_GRID_CULL=1` and
`AR_SIM3D_WORLD_GPU_CLIFFS=1` (the old control ignores the last flag). Palace uses
`--checkpoint palace --scene 'Sky Palace' --quit-frames 1800 --set AR_REPLAY_NOSTOP=1`;
navigation uses `--checkpoint navigation --scene 'World 3D' --quit-frames 2000`.
The harness runs eight serial ABBAABBA trials, isolated inputs, Quality and three
helpers, and reports frame-weighted last-five settled windows then four-run
medians. Timed weather remains live. Builds, captures and GPU tests are outside
local timing intervals. Exact environment, input hashes and individual runs are
preserved in `timing-*/results.json` and traffic in `work-summary.json`.

Local render CPU **wall** milliseconds, median [minimum–maximum]:

| View | GPU grid + CPU cliffs | GPU grid + GPU cliffs | Median change |
| --- | --- | --- | --- |
| Palace | 2.795 [2.465–2.872] | 2.633 [2.332–2.909] | −5.8% |
| Navigation | 2.485 [2.361–2.566] | 2.267 [2.200–2.391] | −8.8% |

Ranges overlap; Palace in particular drifts substantially across the cohort.
Treat these as measured cohorts, not a precise quiet-machine estimate. Navigation
terrain CPU falls 0.189→0.124 ms; Palace terrain falls 0.139→0.121 ms. Presentation
CPU medians fall 1.895→1.717 ms and 1.272→1.200 ms respectively. Median cadence
stays approximately 8.34 ms (~120 Hz) in both views, with more time in present/wait.
These are CPU/traffic reductions, **not equivalent FPS gains**. GPU timestamps,
power and thermal improvements have not been measured.

Per-presentation median traffic:

| View | Draws | CPU depth geometry upload (MiB) | Submitted vertices |
| --- | --- | --- | --- |
| Palace | 29 → 26 | 2.036 → 1.923 (−5.6%) | 765,148 → 767,931 (+0.4%) |
| Navigation | 28 → 25 | 4.392 → 4.241 (−3.4%) | 713,704 → 719,919 (+0.9%) |

The three removed draws are split cliff ground/blur/haze submissions. GPU-side
selection copies remain zero during the settled Palace hold. Navigation copies
increase 0.085→0.100 MiB/presentation with the wider source and extra cliff tail;
copy-call counts are essentially unchanged. CPU ocean/mountain reuse remains live.
Timed weather and slightly different reporting windows cause small traffic
variation; topology, quality and weather settings were not reduced.

The isolated Deck probe directory is
`/home/deck/argame/gpu-cliff-probe-20260912.g4DHSw/`; the installed executable,
settings and saves are untouched. Probes use bundled SDL, Wayland/Vulkan and
hidden 1080x672 output with copied inputs. Guarded fixture and game probes use
20/45-second timeouts respectively, 8-GiB available-RAM start and 6-GiB abort
gates, a 2-GiB incremental GPU-allocation abort gate and bounded process-group
termination. These are compatibility/directional probes, not visible-game FPS
or comparisons with the user's original 22-FPS Palace session.

Eight short Deck timings (two separate ABBA cohorts, 1,000 ticks each, same
candidate binary and only the cliff flag changed) reproduce a strong run-order
effect. Render CPU control sequences are `[6.852, 5.500]` and `[6.875, 5.554]` ms;
candidate sequences are `[5.364, 5.427]` and `[5.311, 5.389]` ms. All results are
retained. First controls in both cohorts are much slower; their cause is not
established. The later controls versus candidates suggest a modest improvement,
but the unqualified aggregate 13–14% CPU reduction would overstate confidence.
Cadence has the same ordering: first controls 8.413/8.446 ms, later controls
6.711/6.772 ms, candidates 6.544–6.629 ms. This is directional support, not a
reliable Deck percentage or a worker-count experiment.

Deck terrain is 0.341 ms in both later controls versus 0.308–0.315 ms for candidates.
Draws fall 29→26 and CPU depth uploads about 2.039→1.927 MiB (−5.5%). Incremental
peak GPU allocation is about 173–174 MiB with cliffs off versus 257–258 MiB on:
**about 84 MiB more**, with the same new binary/layout. This measurement includes
driver allocation/cycling and is not a direct source-buffer size measurement.
The wider layout's separate cost against the older binary is not isolated by this
same-binary Deck comparison. Cold source publication remains a measurement gate.

All eight timings complete 1,000 tick presentations, zero re-presents, identical
final WRAM, no graphics rejection and no guard activation; minimum available RAM
exceeds 11.6 GiB. The standalone Vulkan fixture completes in 0.61 seconds with
over 11.7 GiB available RAM and about 168 MiB incremental GPU allocation. No
owned probe process remains, and the Wayland socket is present. Logs, guards and
individual results are in `direction/`, `direction-repeat/`, `visual/` and
`fixture-guard.json` in the local evidence directory.

## Next gates

Finish ocean and its shadow receivers in the shared source without adding opaque
handles, then address mountain materials and remaining repeated uploads. Measure
cold publication and GPU allocation costs, and investigate isolated coverage/lighting
differences as the combined source is completed. Preserve the multicore
construction and complete compatibility path throughout.
