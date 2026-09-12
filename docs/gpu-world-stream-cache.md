# GPU-world CPU stream reuse — 2026-09-12

Status: **retired**. [GPU ocean](gpu-ocean-source.md) and
[GPU mountain source](gpu-mountain-source.md) replace both CPU stream caches.
Their allocations, capture/replay helpers, `AR_SIM3D_WORLD_CLIP_CACHE` toggle and
unused `cpu-reuse` counter/overlay column have been removed. The complete
compatibility renderer still retains its held-view **GPU** surface cache.
The measurements and implementation account below describe the historical
intermediate optimization, not the current shipping path.

## What changes

The land-grid prototype uses the existing world-surface opaque handle, so it
cannot simultaneously retain ocean/mountains there. Their projected vertices
were already cached, but gathering and clipping them was repeated every frame.
Presentation now copies the exact post-clip ocean and mountain draw streams on
a second matching view, then submits each with one bulk append on later holds.
Moving-camera projection still uses the existing bounded multicore worker pool.

This is **CPU work avoidance, not GPU residency**: backend vertex conversion,
staging and uploads remain. It does not add a fifth opaque handle or claim to
solve moving-camera source transforms. The detailed overlay and logs distinguish
`cpu-reuse` from `gpu-reuse`; the new counter reports two reused streams per
settled Palace presentation, not two eliminated GPU uploads.

## Ownership and failure audit

- Two presentation-owned caches copy public `Sim3DDepthVertex` values. They
  neither borrow renderer storage nor expose a platform type, native handle,
  town policy or live game state across a boundary. There is no runner ABI,
  renderer vtable, shader, GPU readback, fence or compute-queue change.
- Each cache grows in bounded powers of two, at most 32,768 quads / 4.5 MiB
  with the current 36-byte vertex layout: **9 MiB maximum additional CPU
  storage**, not a free-memory optimization. Existing GPU budgets are unchanged.
- The ocean owner invalidates before a changed projection/viewport is built.
  Mountain source/geography/relief changes already invalidate its projection;
  the draw cache follows that invalidation and the full projection, viewport
  size and lighting-enable key. Empty clipped streams are valid cached results.
- A moving view invalidates readiness, retaining only capacity. It does not
  copy a never-reused stream on the first frame. Resource reset frees both
  streams; mountain-owner destruction also frees its stream.
- Recording occurs only after ordinary batch submission succeeds. Allocation
  or capacity failure discards the optional cache and latches it unavailable
  until owner reset, while continuing the complete ordinary draw. A partial
  or failed capture is never marked ready. No new authentic-view fallback or
  per-frame allocation retry is introduced.
- Colors, UVs, primitive order and clipping results are copied exactly. Ocean
  normal/front/depth arrays remain available to its existing shadow receiver.
  Animated atlases, wind and shadows remain live, outside the immutable cache.

## Correctness

All 165 local app tests pass, including actual Metal and layering/ABI tests.
The final expanded GPU world fixture also passes separately after adding
viewport resize/restore and lighting disable/re-enable coverage. It compares
uncached/full-source, default cold caches/culling and warm caches/culling
through 17 source, geography, lighting, relief, view-family and Advent states,
three presentations per state. Reuse and actual GPU compaction are required,
with zero optional-geometry rejection and exact images, not a wider tolerance.

Unit tests compare the entire captured/replayed vertex stream, test caller
mutation, batch/capacity growth, invalidation/reset, empty output, budget
exhaustion and ordinary-submit failure. Budget exhaustion still draws every
ordinary face and does not retry allocation. The optional allocation-failure
branch shares that discard path; allocation failure itself is not injected.

Whole-game validation against the previous culled GPU-grid build: all **12
Palace and 16 navigation captures are byte-identical**, and final WRAM matches.
Two separate Deck/Vulkan Palace captures at GF600/GF900 also match cache-off
exactly. This verifies this cache, not resolution of the GPU-grid prototype's
older isolated coverage differences versus the original CPU transform.

## Repeated measurements

Local: eight serial ABBAABBA runs per view, 2160x1344 Metal, Quality and three
helpers in both variants. Culling is explicitly on in both pinned binaries to
isolate the cache. Palace runs 1,800 ticks; navigation runs 2,000 ticks through
movement and a final hold. The existing harness weights the last five settled
reporting windows by presentations, then takes four-run medians per variant.
Our builds/tests/captures were outside timing. Background macOS indexing/media
analysis was present; these are not controlled power or utilization measures.

Render CPU **wall** milliseconds, median [minimum–maximum]:

| View | Cache off | Cache on | Median change |
| --- | --- | --- | --- |
| Local Palace | 2.588 [2.575–2.647] | 2.434 [2.392–2.464] | −6.0% |
| Local navigation | 2.581 [2.472–2.610] | 2.590 [2.536–2.602] | +0.4% |
| Deck Palace, short probe | 6.149 [6.050–6.248] | 5.437 [5.328–5.547] | −11.6% |

Local Palace mountain staging falls 0.178→0.050 ms and ocean preparation
0.102→0.011 ms. Navigation falls 0.183→0.161 and 0.149→0.134 ms respectively,
but its overall ranges overlap and its median is slightly worse: **no overall
navigation gain is established**. Local cadence stays about 8.34 ms (~120 Hz).
This preserves multicore support; it is not a worker-count comparison.

Deck: four short ABBA runs with the same fresh Linux executable, changing only
`AR_SIM3D_WORLD_CLIP_CACHE=0/1`, bundled SDL/Wayland/Vulkan, hidden 1080x672
output, Quality and three helpers, 1,000 ticks per run. Mountain staging falls
0.512→0.140 ms and ocean preparation 0.140→0.027 ms. Median cadence improves
7.369→6.678 ms (−9.4%, about +10.3% reciprocal throughput). This is encouraging
**directional evidence from a short hidden-output probe**, not a visible-game
FPS estimate or a comparison with the user's original 22-FPS session. Reporting
windows cover different tick ranges as throughput changes; all four runs are
retained, and two untimed captures separately establish exact geometry.

Draws stay 29 in Palace and 28 in navigation. CPU geometry upload stays about
2.04 MiB/presentation in Palace and 4.39 MiB in navigation; this cache removes
no uploads. Timed weather/reporting windows cause small vertex/traffic variation
(Deck vertices differ 0.007%); no topology/quality reduction was introduced.
GPU timestamps, power and thermal improvement are not measured.

Every Deck process completed normally with matching WRAM, its full tick/present
schedule, zero re-presents, no graphics failure or guard activation, at least
11.6 GiB RAM available and under 176 MiB peak incremental GPU allocation. The
45-second timeout, 8-GiB start/6-GiB abort RAM gates and 2-GiB GPU-growth gate
remained active. No probe process remains; Wayland is healthy. The installed
Deck executable, settings and saves were not replaced.

## Evidence and next work

Local: `/private/tmp/actraiser-surface-streams.1w81tb/`. `timing-{palace,navigation}`
contains complete run inputs, hashes, results and `work-summary.json`.
`verify-*` contains exact capture hashes, `tests-final.log` the final full suite and
`resize-test.log` the final extended world fixture. `direction/analysis.json`
and `visual/results.json` contain the Deck measurements and exact image hashes.
Deck: `/home/deck/argame/surface-stream-probe-20260912.ZyDcUN/`.

- Mac control (`b38415f4`): `2125fac78bfafcc372d243bee6a3d5682324adde5b60216139d8c42b37aa05ff`.
- Mac candidate: `66f17d3dc978eb11acbfcdc6e85642052d0294f1c7f3989cc9722091ca22627f`.
- Linux same-binary A/B: `b8f24a8af1d5da9be9027b832a5dc4d7d50b36860ef947e9a6263766bf29266c`.

Reproduction uses `tools/compare_pipeline_performance.py`, the manifest
`/private/tmp/actraiser-world-surfaces.NDSzGz/checkpoints.json` and config
`/private/tmp/actraiser-deck-profile.2Y9hIv/benchmark-defaults.ini`. Both local
variants set `AR_SIM3D_WORLD_GPU_GRID=1` and `AR_SIM3D_WORLD_GPU_GRID_CULL=1`.
The Deck script, pinned binary and guards are in the evidence directory.

Subsequent source offloads retired this cache in stages: [cliffs](gpu-cliff-source.md),
[ocean](gpu-ocean-source.md), then [mountains](gpu-mountain-source.md). Current
limitations and remaining uploads are recorded in those follow-up reports.
