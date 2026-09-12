# Shared GPU ocean source — 2026-09-12

Status: **default navigation and Sky Palace path**, extending the land/cliff
default established in `3fe56fd2`. No new experiment switch is introduced.
`AR_SIM3D_WORLD_GPU_GRID=0` still requests the complete compatibility renderer;
unsupported or failed source setup automatically uses that renderer too.

Follow-up: [shared GPU mountains](gpu-mountain-source.md) adds native cutouts
to the same source and retires the last CPU stream cache/toggle. The implementation
and measurements below describe the ocean checkpoint (`9b66d9e2`). Its retained
CPU ocean code is only the complete compatibility renderer, not a parallel
default path; the obsolete ocean cache and partial-receiver branches are gone.

## Implementation and retired branches

Ocean, land and registered cliffs share one retained source allocation. The
4,608 original ocean quads form a camera-local prefix; the land grid and cliff
tail stay in chart space. Two independently transformed ranges queue ocean
then land, with each range's shadows using the exact same positions, triangles,
hardware clipping and depth as its opaque draw. Land blur/haze exclude ocean.
The original ocean topology, color gradient, sea level and weather settings
are unchanged. Back-side faces reach the GPU and are hidden by normal depth.

The presentation owner supplies the ocean's camera-relative placement frame
and a separate source-to-chart shadow frame. This prevents weather from rotating
with the camera. Both are copied uniforms. The renderer knows no globe, town,
game clock or scene policy. One common shell-frame helper also supplies the
existing compatibility ocean and atmosphere/cloud shell math.

The default path no longer projects or clips ocean geometry on the CPU, builds
ocean screen-space shadow receivers, or uploads that geometry on moving/held
views. It retains only the shared immutable source until geography/relief source
changes; camera, lighting, viewport and wind use transforms/material parameters.
The existing bounded multicore land/cliff construction and mountain/model paths
remain intact. Ocean source construction is small serial setup work, not a new
thread pool or per-frame CPU projection.

Removed the now-dead ocean CPU stream cache, its allocation/reset/invalidation
branches, the partial-world receiver key, and partial GPU/CPU receiver branches.
`AR_SIM3D_WORLD_CLIP_CACHE` now controls only the remaining mountain CPU stream
cache. The earlier cliff experiment switch and hybrid land/cliff branch were
already removed in `3fe56fd2`. Complete CPU compatibility rendering, including
its held-view GPU retention and shadow handling, is deliberately preserved.
Cloud-body UV work also no longer prepares unused ground normals.

## Boundary and failure audit

- `Sim3DDepthPass_AppendSurfaceBatches` adds generic ranges to the project-private
  surface API, not a runner ABI or rendering vtable. A batch copies its transform,
  shadows and overlays. It addresses the currently selected source stream, so
  compacted subranges need no CPU vertex round trip or additional source handles.
- Every range, transform, layer and **combined** geometry/effect budget is checked
  before anything queues. Invalid final ranges/frames or exhausted combined budgets
  cannot strand only ocean or only land. Empty valid ranges draw nothing. Source
  updates/selections remain forbidden once queued; reset/republication rules remain.
- The optional shadow frame must be orthonormal, with the existing unit-vector
  tolerance; all zeros select identity and preserve existing source sampling.
  Invalid/nonfinite/parallel rows are rejected. Positions and lighting do not use
  that frame. Texture/overlay coordinate bounds and affine-overflow checks remain.
- The world still consumes one of the existing four opaque handles, with the
  existing 65,536-quad and 64 geometry/64 effect sample budgets. It submits two
  opaque range samples; source capacity includes the ocean prefix before allocation.
  Sixty-six ordered chunks merge into at most 33 selected runs. Ocean is retained
  conservatively rather than tested against the wrong chart-space land bounds.
- Packed source size stays 256 bytes/quad with sixteen attributes. Ocean adds
  1.125 MiB of logical source data before capacity rounding/cycling. Uniforms grow
  256→304 bytes to carry the shadow frame. No readback, fence, async compute queue,
  runtime shader compiler or platform type escapes the SDL adapter. Metal, SPIR-V
  and DXIL are generated offline; D3D12 runtime validation remains unavailable.
- Failure queues none of the surface group, latches source off until reset, and
  uses the full ordinary ocean/land/cliffs. Effects retain their existing controls
  and failure policy. The default never restages CPU shadows over GPU geometry.

## Correctness

All **165 app tests pass**. The real-Metal world fixture compares explicit enable,
unset/default, unculled/cold/warm views through 17 geography, relief, lighting,
weather, viewport and Advent states, three presentations each. Images are exact
within that comparison, actual compaction is required, and the CPU ocean stage
must never run. Existing compatibility, failure, layering and ABI tests pass.

The expanded depth fixture passes on both Metal and Deck/Vulkan. New tests compare
two independent reference meshes against ranges in one source, including selected
stream offsets, different transforms/shadow frames, masks/overlays, ordinary draw
ordering, empty ranges and eye-plane clipping. They test copied batch lifetime,
invalid-final-range/frame atomicity, nonfinite/nonorthogonal frames, combined
effect-budget overflow and the exact 64-geometry-sample limit. The independent
shadow coordinate oracle doubles from 28 to 56 states, retaining its existing
two-level texture/transcendental tolerance; exact geometry tests remain exact.

Full-game comparison against the previous GPU land/cliff renderer is **not
byte-identical**. At 2160x1344, Palace changes 86 of 2,903,040 pixels per capture
(12 captures), navigation changes 196–614 (16 captures). Every channel difference
is at most two 8-bit levels; none exceeds that. Two 1080x672 Deck Palace captures
each change 21 pixels, all by one level. Final WRAM matches in every comparison.
Full-frame inspection shows no visible geometry/weather seam. Strict comparison
failures and pixel histograms are retained, not waived or relabelled exact. These
are incremental ocean differences, not a resolution of older CPU/grid differences.

## Repeated results

Local: two cohorts of eight serial ABBAABBA runs, 2160x1344 Metal, unchanged Quality
and three helpers. Palace uses 1,800 ticks; navigation uses 2,000 through movement
and a final hold. Results weight the last five settled reporting windows by frames,
then take four-run medians. No own builds, GPU fixtures, captures or image analysis
overlap timing. Other background host work is not controlled; retain all runs.

Render CPU **wall** milliseconds, median [minimum–maximum]:

| View | CPU ocean | Shared GPU ocean | Median change |
| --- | --- | --- | --- |
| Local Palace | 2.759 [2.543–2.884] | 2.756 [2.677–2.834] | −0.1%: negligible |
| Local navigation | 2.353 [2.242–2.606] | 2.163 [2.110–2.366] | −8.1% |
| Deck Palace, short probe | 5.390 [5.346–5.435] | 5.273 [5.147–5.399] | −2.2%, directional |

Ranges overlap. Navigation presentation CPU falls 1.771→1.596 ms and depth submit
0.181→0.137 ms; its former 0.120-ms CPU ocean stage disappears. GPU surface group
preparation is included in the terrain stage, not an unmeasured CPU scope.
Local cadence stays approximately 8.34 ms (~120 Hz), with more time in present/wait.
These CPU savings are **not equivalent FPS gains**, GPU timings or power measures.

Per-presentation median traffic:

| View | CPU depth geometry upload (MiB) | Draws | Submitted vertices |
| --- | --- | --- | --- |
| Palace | 1.923 → 1.745 (−9.2%) | 26 → 26 | 767,931 → 934,519 (+21.7%) |
| Navigation | 4.242 → 2.779 (−34.5%) | 25 → 25 | 719,485 → 831,630 (+15.6%) |

This deliberately trades additional hardware-clipped/back-side ocean vertices for
CPU work and upload avoidance. It does not reduce geometry quality. GPU selection
copies stay zero in the Palace hold; navigation copies rise 0.100→0.145 MiB and
0.092→0.132 calls/presentation because of the additional prefix/ranges. The held
CPU-reuse counter changes two→one in Palace because ocean now resides on the GPU,
not because mountain reuse was removed. Weather/reporting-window variation remains.

Deck: separate control and candidate warmups precede four short ABBA timings,
1,000 ticks each, hidden 1080x672 output, bundled SDL/Wayland/Vulkan, same Quality
and three helpers. Warmups are retained separately, not selectively deleted
measured trials. Cadence median changes 6.631→6.479 ms (−2.3%, about +2.3% reciprocal
throughput), with overlapping ranges. This is encouraging directional evidence,
not a visible-game FPS estimate or a comparison with the original 22-FPS session.
Draws remain 26, depth upload falls 1.927→1.749 MiB. Peak incremental GPU allocation
is about 258 MiB control versus 242 MiB candidate, **about 16 MiB lower**, despite
the larger logical source. This includes driver allocation/cycling and retired
receiver storage; it is not a direct source-buffer size or total RAM measurement.

Every Deck run completed its full tick/present schedule with zero re-presents,
matching WRAM, no graphics rejection and no guard activation. Available RAM remains
over 11.6 GiB. The standalone Vulkan fixture passes in 0.61 s. The probes retain
8-GiB start/6-GiB abort RAM gates, a 2-GiB GPU-growth abort gate, 20/45-second
fixture/game timeouts and bounded process-group termination. No owned process
remains; Wayland is available. Installed Deck binaries/settings/saves are untouched.

## Evidence and next work

Local: `/private/tmp/actraiser-gpu-ocean.OYXLlw/`, with `tests-all.log`, shader/build
logs, `verify-*/pixel-diff.json`, `timing-*/{results,work-summary}.json`, and
`direction/analysis.json`, `warmup/`, `visual/pixel-diff.json` for Deck. Remote:
`/home/deck/argame/ocean-probe-20260912.VNS9hV/`.

- Mac control (`3fe56fd2`): `897bb540c02bb702f2844514a9646b4be388287144836b4cb61249c2fac5f9f0`.
- Mac candidate: `ac0922efe7b4fd839d0a14f1b613a56913bfa361a8480d04fa0ea91fccd889a6`.
- Linux control: `3fbd26c9bacd43e6cb1b1340df115ed47f41918afec996a81bb35da654fdc3f6`
  (`3e18ba68`, with its grid/cliff flags explicitly on: the same activated renderer).
- Linux candidate: `0751bbabe77ecd307d23ba00169582e14a2c3fe740e248cc3f5bdf817f4e489f`.
- Linux GPU fixture: `abffe9e3319ef6b7c9a30f446b998a202e1c2836e82187521858c136c3c09c9c`.

Local reproduction uses `tools/compare_pipeline_performance.py`, manifest
`/private/tmp/actraiser-world-surfaces.NDSzGz/checkpoints.json` and config
`/private/tmp/actraiser-deck-profile.2Y9hIv/benchmark-defaults.ini`, with both pinned
binaries using the default GPU path. Palace arguments: `--checkpoint palace
--scene 'Sky Palace' --quit-frames 1800 --set AR_REPLAY_NOSTOP=1`; navigation:
`--checkpoint navigation --scene 'World 3D' --quit-frames 2000`.

Next: native mountain cutout materials/held GPU residency and remaining uploads.
The generic source-range contract now supports different transforms in one handle.
Consider cheap conservative ocean visibility only if it beats its CPU overhead
and preserves exact near-plane/depth behavior. Avoid extra fine-grained CPU
visibility work or deletion of required compatibility rendering. Cold publication
and GPU elapsed time remain unmeasured; this is not async compute.
