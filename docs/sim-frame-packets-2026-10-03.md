# Enhanced-town frame packets — 2026-10-03

Enhanced SIM towns now use the bounded producer/presentation queue by default.
`AR_SIM_FRAME_STREAM=0` restores synchronous town rendering;
`AR_FRAME_STREAM=0` disables streaming for both action and SIM.

## Ownership and supported scenes

A packet owns its `FrameSlot`, required separated plane/HUD pixels, sprite atlas,
optional flat composite, and a complete WRAM/VRAM/CGRAM/PPU-state snapshot.
The three-slot queue retains each snapshot until upload completes. Releasing or
skipping a packet never skips an emulated tick. Whole source snapshots allow the
consumer to skip stale captures without losing intermediate dirty-region updates.

The presentation owner prepares the developed world map, town canvas and voxel
scene from those inputs. Their textures, dirty-region cursors and geometry caches
stay with that owner. This avoids copying the derived multi-megabyte canvases on
every source frame. Terrain preparation uses the packet's capture status and
backdrop instead of live SIM capture globals. Metadata takes an explicit entry
underlay generation; preparation replaces it with that packet's completed map
generation before upload.

Atlas and flat-composite upload now read slot descriptors instead of global pixel
buffers. Retention clears source pointers, including the SIM input snapshot;
retained draws use uploaded resources and copied metadata. Copy selection keeps
fallback ground planes but omits replaced OBJ planes, the unused native composite,
and duplicate HUD sources. At the end of the Aitos run, packet traffic fell from
5.662 to 3.342 MiB/capture, with pixel-copy time around 0.5–0.6 ms on Deck.

Camera motion remains producer-owned. Camera controls use the displayed scene's
metadata and run on the presentation owner, including held analog input and mouse
orbit. Screenshots fill the same presentation controls. Eruption crater feedback
is a single atomic publication tagged with its town; a producer capture cannot
mix coordinates from different presents or consume another town's anchor.

Only eligible enhanced towns stream. World navigation, Sky Palace, authentic
fallbacks, inspection, comparison, modern menu ownership and scene changes use
the existing synchronized handoff. The CPU world-map oracle and D1 metadata trace
also keep synchronous preparation. Headless runs remain synchronous. SIM uses its
native source clock even when action frame generation is enabled; it does not
inherit action interpolation delay or a reduced action source rate.

## Deck measurements

Isolated KDE/Wayland runs on the existing 1280×800, 90 Hz Deck setup. Original SIM
menus, native text, identical SRAM seed and canonical replay, fullscreen, audio
off. No shadow-cache override. Each run executes 3,600 ticks; the table analyzes
host ticks 1,100–3,600. Moving runs replace later input with the same 150-tick-leg
D-pad square. Values are application present-completion intervals, not measured
panel scanout or GPU timestamps.

All four primary runs use the identical `packets-v2-deck` executable, SHA-256
`2283331ae80d5b8a03bf6260c4bcf34802c532387c6278edf2e131ef6377743f`.

| Workload | Path | Presents/sec | p95 interval | p99 interval | Cadence mismatch |
|---|---|---:|---:|---:|---:|
| Idle town | Synchronous | 84.06 | 23.63 ms | 25.58 ms | 4.43% |
| Idle town | Packets | 88.38 | 19.88 ms | 26.16 ms | 2.86% |
| Moving town | Synchronous | 84.68 | 20.84 ms | 23.67 ms | 4.23% |
| Moving town | Packets | 87.39 | 20.14 ms | 26.28 ms | 3.96% |

This is about 5.1% more presents in the idle case and 3.2% in the moving case.
The first packet implementation also measured 88.24 versus 84.44 presents/sec
idle, with p95 19.20 versus 23.25 ms. These are individual paired samples, not
confidence intervals. Tail stalls remain: p99 worsened in the primary pairs,
and neither workload demonstrates a locked 90 Hz. The changes improve overlap,
mean throughput and p95 in these samples; they do not remove shadow cost or
solve every pacing outlier. Cadence mismatch is the analyzer's fitted native
source-phase metric, not the normal fraction of held 60 Hz frames on a 90 Hz panel.

Final WRAM hashes are byte-identical between packet and synchronous runs:

- Idle: `43cbb8e8bb94ce1703588016f7c98dc47f90498ae1dfe1f5f7260557230ad5cd`
- Moving: `d6f23f4b1cac27f06025b473c63758665332f78ccfb4a864e472f9c3441ba57c`

## Validation and evidence

- Release: 12 targeted tests covering queue publication/reuse/pause, producer,
  capture, camera, input, metadata, world map, textures, screenshots and PPU pipeline.
- Sanitizers: all 9 targeted address/undefined-behavior checks passed; results are in
  `runs/sim-pipeline-2026-10-03/packets-final-asan.log`.
- New ownership tests overwrite live WRAM, VRAM, CGRAM and pixels before consuming
  a packet. The threaded queue test alternates action/town packets through full
  queues, held readers, maintenance pauses and recycled slots. The atlas test
  includes its last 512th row. Camera tests change live WRAM after scene capture.
- Captured-town GPU regression passed on Metal, including held/cold pixel parity,
  camera limits, settings variants and motion captures.
- Deck replay traversed Palace/navigation/town boundaries and the town interlude.
  A separate live-settings run disabled SIM at gf1400, restored it at gf1600 and
  captured the final composite at gf1800. Action interpolation was enabled, while
  all town stream trace rows correctly remained native (`interpolation=0`).
- Visual inspection of that screenshot showed the town, sprites, HUD, terrain,
  shadows and eruption effects after the handoff. It is a visual smoke check,
  not a claim of byte-exact streamed-versus-synchronous screenshot parity.

Raw traces, run logs, summaries, pinned binaries and screenshots live under
`runs/sim-pipeline-2026-10-03/`, including `packets-summary.json`,
`deck-packets-v2-{idle-a,sync-a,move-a,sync-move-a,handoff}`,
`gpu-packets-town/` and `packets-sha256.txt`. The final default-enabled binary has
an additional isolated Deck smoke run, `deck-packets-final-default` (2,400 ticks) and `deck-packets-final-default-b`
(1,800 ticks after avoiding an extra FrameSlot copy on the action path).
The installed Deck game was not overwritten.

## Follow-up: locating the remaining stalls

Reviewing individual intervals changes the interpretation of the tail results:
the largest stalls occur in both packet and synchronous runs at nearly identical
host ticks. Packetization did not introduce this family of stalls.

| Host tick | Idle packets interval | Idle synchronous interval |
|---|---:|---:|
| 1,997 | 48.98 ms | 48.97 ms |
| 2,004 | 45.98 ms | 48.43 ms |
| 3,270 | 55.77 ms | 60.20 ms |
| 3,299 | 60.58 ms | 58.92 ms |

In the analyzed 1,100–3,600 window, each packet workload has ten intervals over
33.33 ms and each synchronous workload has nine. The excess packet interval is
a smaller follow-on interval, not a new collection of 40–60 ms stalls. Idle
packet draw scopes reach 40–53 ms while final backend swap waits are about
0.3–0.4 ms. The corresponding rolling reports put the peak inside SIM underlay
rendering; shadow calls remain roughly 3–4 ms. These are inclusive wall-time
scopes, not independent GPU execution measurements.

The world-map logs explain the repeated timing: development changes at game
frames 1,994 and 2,001 modify four cells and one cell respectively. Their
presentation rebuilds report about 7.7–7.9 ms in cliff setup, 6.4–6.5 ms in ground
art setup, and up to 7.3 ms in globe-surface preparation. Later single-cell
changes reach 10.7–11.9 ms in cliff setup. Small source changes still fan out
into large derived-resource rebuilds on the presentation owner.

Code paths worth addressing next:

- `EnsureWorldNavigationCliffs` destroys/rebuilds the full cliff collection when
  the global terrain serial changes. Globe surface publication then depends on
  that cliff generation. Determine the affected terrain/edge regions and retain
  unchanged mesh chunks; global height inference and boundary blending must
  remain correct.
- Detailed town terrain, curved mountains, and model source style use the broad
  geography serial. Audit their actual height/source dependencies, compare
  resulting geometry on rebuilds, and narrow invalidation only where equivalent
  output is established. Simply suppressing the revision risks stale terrain.
- `EnsureWorldNavigationArt` discards all GPU animation-atlas versions when
  geography or town sources change. The CPU art update is already incremental,
  but this cache has to populate again over subsequent animation phases. Patch
  affected regions across retained versions or otherwise bound that work.

A fresh 2,400-tick Deck `perf record -F 99 --call-graph dwarf,4096` replay using
`packets-final-deck` reproduced draw stalls at ticks 1,997, 2,004, 2,113,
2,125, 2,127 and 2,130. Samples include terrain-floor sampling, globe mapping,
mountain embedding and mesh updates; foliage shadow projection remains the
largest individual sampled function across the run. Stack unwinding is
incomplete, and the sample includes startup/scenes before the analysis window,
so its percentages are not steady-town stage costs. The display initially
slept and was awakened; this profiling run is attribution evidence, not another
FPS comparison. It exited cleanly with no detected foreign game process.
The text report is `runs/sim-pipeline-2026-10-03/packets-stall-profile-report.txt`,
with traces under `deck-packets-stall-profile`; raw perf data remains on Deck.

The p99 difference remains a separate, unresolved question. Increased overlap
can change CPU/cache contention and how work lands relative to refresh, but
these traces do not prove either explanation. Repeated opposite-order pairs,
presentation-thread CPU time, and controlled worker-count comparisons would
separate those effects. Prioritize the deterministic rebuild spikes for hitch
reduction, and shadow work for sustained 90 Hz headroom.

## Steady-state follow-up: native windmill motion invalidation

Prioritizing the intervals between world-map development events exposed a
separate bug. `WorldNavigationModelMesh_DrawFacingTown` already retained the
static town separately from captured windmill poses. Its motion-only comparison
normalized `animation_phase`, but omitted `visual_metatile`. Native finished
windmills cycle through metatiles `$24/$26/$16` along with phases 0/1/2. Those
ordinary animation changes therefore invalidated the entire static town stream.

The fix normalizes both windmill motion fields for that comparison. The actual
incoming phase is still compiled into the animated stream; construction state,
flags, other model identities, heights and style remain in the comparison.
No shadow policy, source timing, quality setting or simulation state changes.

An opt-in `AR_SIM_GLOBE_TRACE` splits slow connected-town draws into view setup,
mountains, terrain, shadows, surfaces, models, actors and depth submission. The
new `draw_cpu_ns` pacing CSV column measures presentation-owner CPU time. Before
the fix, construction-free examples spend about 10 ms in model preparation;
slow 18–20 ms draw scopes have almost as much owner CPU time as wall time.
This identifies real CPU work rather than a long final swap wait. After the fix,
sampled slow-frame model preparation is around 0.1 ms. CPU time excludes helper
threads; wall minus owner CPU is not a GPU timer.

### Deck comparison

Same 1280×800 90 Hz KDE/Wayland replay setup and settings as above. Runs execute
3,200 ticks and compare identical host-tick windows without logged world-map
development changes. This does not exclude ordinary visual animations or
camera-dependent mountain work. The baseline and fixed binaries both include
the same diagnostics, with `AR_SIM_GLOBE_TRACE=1`:

- `steady-trace-deck`: `1fa79ceefae73693a7e4a9af909a1f876fdf1614cb6c8695d73918eb6c9e8376`
- `steady-fix-deck`: `b5a6194e53ec05f068aa0c9cf5ea6abfae41c612f3325e02dbaec861da9a5df0`

| Window | Workload | FPS before → after | p99 interval before → after | Fitted cadence mismatch before → after |
|---|---|---:|---:|---:|
| 1,100–1,900 | Idle | 88.31 → 90.10 | 23.21 → 16.25 ms | 3.65% → 0.50% |
| 2,200–3,200 | Idle | 89.95 → 90.08 | 24.45 → 13.64 ms | 1.54% → 0.40% |
| 1,100–1,900 | Moving | 87.87 → 90.10 | 26.48 → 16.53 ms | 3.93% → 0.92% |
| 2,200–3,200 | Moving | 87.78 → 90.12 | 26.04 → 16.57 ms | 2.88% → 0.60% |

These are application present-completion intervals, not physical scanout.
About 90 FPS average still does not establish uniformly spaced 11.1 ms presents.
Each workload has one fixed sample; an additional baseline was repeated after
the fixed runs to check order/warmup. Data is in `steady-summary.json` and
`deck-steady-{trace-a,fix-idle-a,base-idle-b,base-move-a,fix-move-a}` under the
existing run directory. All runs exited cleanly without detected foreign games.

Two source-delay experiments were rejected as defaults. Removing the delay
reduced some long completion intervals but worsened fitted native cadence to
7–9% mismatch. With the model fix, increasing delay from about 12.48 to 16.64 ms
reduced idle p99 further (13.36/12.18 ms in the two windows), but worsened fitted
cadence to 1.33%/2.67%, versus 0.50%/0.40% at the original delay. It also adds
about 4.16 ms of selection delay. Existing timing remains unchanged.

### Regression coverage

The prior captured-motion GPU test changed only `animation_phase`, so it did not
model a real native spin. It now changes both phase and the corresponding native
metatile. A negative-control build without the fix fails the bounded-upload
assertion. With the fix, rotor updates upload 29,920 bytes versus 740,320 bytes
for the synthetic cold town, held frames upload zero, and warm/cold pixels match
exactly through the complete rotor cycle and an independently held scaffold.
The focused `--captured-motion` suite passes in Release and ASan/UBSan; the real
captured-town GPU suite also passes. Seventeen targeted CTests and eleven pacing
analyzer tests pass. Mac Release and Deck cross builds succeed, and changed C
files add no style violations relative to HEAD.

Paired 3,200-tick final WRAM hashes match exactly:

- Idle: `5211a57db8dfc8e30a3e2866d16d05b63df484a379b3a78f9d416fefd3f0f8d4`
- Moving: `4abbae59dacedd72e9f4e676ee6641e60d62a24e7bbc750deee5871154224c71`

This fixes a repeatable steady-state pacing defect without adding latency. The
remaining smaller variation includes endpoint waiting and moving-view mountain
preparation; construction-triggered world rebuilds remain separate work.

## Moving-view follow-up: shared mountain ground samples

The frame-packet and windmill work above was committed as `5e7dc3db`. The next
steady-state cost is curved mountain preparation when camera facing changes.
Relief tiles and skirts repeatedly evaluate the same exact ground XY: native
terrain height, town/world floor registration, globe placement and local metric.
Rotation legitimately changes the relief's stack direction, so retaining the
entire old mesh would change its appearance.

`PresentSimGlobeMountains_Prepare` now uses a bounded 4,096-entry lookup during
each source emission. Entries store exact float-bit XY keys and the resolved
ground point/metric; elevation, facing, UVs and color still follow the existing
recipe. Collisions replace entries. The 112 KiB temporary cache is discarded at
the end of emission, with uncached fallback on allocation failure. Nothing
survives to become stale across a camera, town, landscape or geography change.
There is no quality reduction or source-delay change.

`AR_SIM_MOUNTAIN_GROUND_CACHE=0` selects the uncached calculation for profiling
and parity tests. `AR_SIM_GLOBE_TRACE=all` records every stage sample; the existing
`=1` mode continues logging only draws above 8 ms. The pacing analyzer also stops
reporting an absolute clock value as deadline lateness in its worst-frame list
when the trace has no software deadline, as in the VSync runs below.

### Deck results

Same isolated Aitos replay, 1280×800 90 Hz KDE/Wayland, 3,200 ticks and unchanged
presentation settings. All six runs use one binary with the cache enabled or
disabled, SHA-256:
`ae69a6648fddad67468b39145b9d3fcf3902b7cf41e4a7d373765b8af6fe323d`.
Both variants use `AR_SIM_GLOBE_TRACE=all`. The measured windows contain no logged
world-map development changes. Average rates remain approximately 90 FPS.

| Window | Workload | Mean mountain stage before → after | p99 present interval before → after | Fitted cadence mismatch before → after |
|---|---|---:|---:|---:|
| 1,100–1,900 | Moving, pair A | 2.084 → 1.092 ms | 16.64 → 15.36 ms | 0.33% → 0.00% |
| 2,200–3,200 | Moving, pair A | 2.029 → 1.098 ms | 16.13 → 15.13 ms | 0.40% → 0.00% |
| 1,100–1,900 | Moving, pair B | 2.052 → 1.061 ms | 16.43 → 15.34 ms | 1.00% → 0.00% |
| 2,200–3,200 | Moving, pair B | 2.044 → 1.070 ms | 16.08 → 15.13 ms | 0.60% → 0.00% |
| 1,100–1,900 | Idle | 2.100 → 1.130 ms | 16.25 → 14.98 ms | 1.16% → 0.00% |
| 2,200–3,200 | Idle | 0.006 → 0.004 ms | 13.71 → 13.22 ms | 0.20% → 0.33% |

Moving pair A runs baseline then optimized; pair B reverses that order. The idle
pair also runs optimized first. The first idle window still has changing camera
facing. In the later idle window the camera has settled and only two mountain
rebuilds occur, so the small pacing differences there should not be attributed
to this optimization. Both repeated moving runs cut mountain preparation by
roughly half. Whole-draw CPU savings are smaller than that stage saving; this
does not remove the other draw, upload or scheduling costs.

These are application present-completion intervals, not scanout timestamps.
Zero fitted source-cadence mismatch does not mean every present is 11.1 ms apart.
For example, optimized moving pair A still has a 17.26 ms interval at tick 2,207:
8.94 ms before the presenting loop, 1.70 ms upload, 6.25 ms drawing and 0.33 ms
final present. Source waiting before upload/draw remains a separate investigation;
the existing playback policy was retained.

All six runs exited cleanly with no detected foreign game process. Paired final
WRAM hashes exactly match the idle and moving hashes recorded above. Data is in
`runs/sim-pipeline-2026-10-03/deck-mountain-{base,fix}-{idle-a,move-a,move-b}`;
`analyze_mountains.py` regenerates `mountain-summary.json` and verifies the selected
windows exclude logged development changes.

### Regression coverage

Captured Fillmore and Aitos GPU suites compare cache-enabled and uncached images
across 12 camera/detail/landscape combinations, including all four detail levels
and 0/75/150% landscape height. Pixels and curved crater anchors match exactly.
Ground evaluations fall from 61,452 to 19,553 for Fillmore and 85,500 to 29,439 for
Aitos, reductions of 68% and 66%. Existing camera-limit, held/cold, live crater
and immutable-capture checks also pass. Both town suites pass in Release and
ASan/UBSan. Seven related terrain/mountain/playout CTests and twelve pacing
analyzer tests pass. Mac Release and Deck cross builds succeed; changed C files
add no style violations relative to HEAD.

## Priority audit after the mountain fix

The mountain optimization was committed as `9b39dbf1`. This follow-up tests
whether source-frame waiting is actually the highest-payoff pacing target.
The objective is lower whole-frame p99 with correct native source cadence, not
just lower average CPU time. A stage's own p99 need not occur on a slow present.

### Match the work to the slow presents

The opt-in globe trace now includes start/end timestamps from the same SDL
clock as the pacing CSV. Native game-frame IDs can lag host ticks and repeat,
so the audit joins each stage record to its enclosing draw interval rather than
matching those IDs. Every selected draw in the timestamped runs has a verified
enclosed stage record. `analyze_priority.py` reproduces `priority-summary.json`
and `priority-summary.txt` in the existing run directory.

For the default moving run, host ticks 2,200–3,200, whole-frame p99 is 15.54 ms.
The following compares each component's own distribution with its cost on the
slowest approximately 1% of complete present intervals:

| Component | Median | Component p99 | Mean on slow presents |
|---|---:|---:|---:|
| Gap before presenting loop | 0.008 ms | 6.704 ms | 6.748 ms |
| Owner upload/preparation scope | 1.440 ms | 2.902 ms | 2.222 ms |
| Drawing, including the stages below | 6.382 ms | 8.293 ms | 6.606 ms |
| Shadow preparation | 3.564 ms | 4.635 ms | 3.595 ms |
| Mountain preparation | 1.054 ms | 1.619 ms | 1.160 ms |
| Model preparation | 0.088 ms | 0.377 ms | 0.198 ms |

Shadows remain expensive, but are nearly typical on these slow frames. The
pre-loop gap dominates interval variation. All of the strict slowest-1% samples
in both moving windows consume a new source packet which was not ready at the
previous present's completion. In the later window, the producer's associated
work is 6.78 ms at the median and 6.67 ms on slow presents: these are generally
not producer-work spikes either. Arrival phase and the remaining owner work
matter. On slow presents, only 6.42 ms remains between source completion and the
estimated presentation sample, while upload plus drawing costs 8.83 ms.

The gap is not 6.75 ms of freely removable sleep. Source completion to the start
of the presenting loop averages 0.745 ms on those slow frames; most of the gap
occurs before the needed source exists. Shorter polling could recover some of
that pickup delay, but removing waits wholesale would change which native
frame gets presented. The earlier zero-delay cadence regression still applies.
Producer and owner work overlap and must not be added as average frame costs.

### Current CPU profile and shadow retest

A fresh `perf` sample covers twelve seconds starting at native game frame 2,252,
after the construction window, with 3,092 user-cycle samples and no lost samples.
Presentation owns 60.91% of sampled user cycles; the frame producer owns 36.28%.
`SimBackgroundVoxelModel_ProjectFoliageShadow` alone accounts for 15.66% of all
sampled cycles and `AppendTreeShadow` another 4.17%. Producer PPU rendering and
overlay writes are the next significant family. These are CPU sample shares,
not elapsed frame costs. Raw data remains on Deck in
`deck-priority-profile/steady.data`; text reports and metadata are saved locally.
The profiled run is attribution evidence, not an FPS comparison.

The retained shadow experiment was previously rejected on the synchronous SIM
path. Repeating it after packets, windmill invalidation and mountain reuse gives
a different result. Two moving pairs, in opposite orders, now hold about 90 FPS
and reduce whole-frame p99. Each pair uses one binary and changes only
`AR_SIM_SHADOW_HULL_CACHE`:

| Workload | Window | p99 cache off → on | Fitted source-cadence mismatch off → on |
|---|---|---:|---:|
| Moving A, on then off | 1,100–1,900 | 15.29 → 13.93 ms | 0.08% → 0.42% |
| Moving A, on then off | 2,200–3,200 | 15.33 → 13.74 ms | 0.07% → 0.60% |
| Moving B, off then on | 1,100–1,900 | 15.24 → 13.81 ms | 0.00% → 0.08% |
| Moving B, off then on | 2,200–3,200 | 15.54 → 13.91 ms | 0.20% → 0.07% |
| Idle, off then on | 1,100–1,900 | 15.44 → 13.78 ms | 0.00% → 0.17% |
| Idle, off then on | 2,200–3,200 | 14.46 → 11.93 ms | 0.40% → 1.20% |

Moving shadow median drops from 3.56 to 2.28 ms. With a settled camera, retained
shadow outlines reduce the median to 0.29 ms. This demonstrates tail improvement
from removing steady work, even though shadows are not the original spike.
The prior moving-throughput regression is absent in these streamed runs.
Cadence does not improve consistently, and the idle result trades some fitted
source-cadence accuracy for tighter completion intervals. Keep the experiment
opt-in until that behavior is resolved; a p99 win alone is insufficient.

### Revised priorities and limits

1. The largest demonstrated next p99 gain is the remaining shadow CPU work:
   1.3–1.6 ms moving and 2.5 ms in the settled idle comparison. Finish evaluating
   the retained path with native cadence as an acceptance criterion. Its old
   synchronous failure should not be treated as proof it fails on SIM packets.
2. Investigate source arrival versus presentation deadlines together with that
   headroom. The fixed wait policy reserves only 2 ms, while current upload/draw
   needs considerably more. Treat producer completion, packet pickup, source
   selection and actual presentation as separate events. A shorter sleep alone
   does not reclaim the full waiting gap.
3. Owner upload/preparation tails are a smaller direct target (roughly 1.4 ms
   median versus 2.9 ms p99). Producer PPU/overlay work is worth reducing if it
   advances the required source's availability. Further model, blur or settled
   mountain optimization is low priority in these windows.

These measurements still describe application present completion. At 90 FPS,
16 ms and 6.22 ms intervals can average 11.11 ms; buffering can also separate
return timing from display timing. Verify actual presentation feedback before
claiming remaining return-time variation is visible stutter. Deck advertises
Wayland `wp_presentation` v2 and Vulkan present-timing/wait extensions, but this
audit does not collect their timestamps. Zero fitted cadence mismatch is also
a diagnostic lower bound, not a scanout guarantee.

All seven new runs exited cleanly without detected foreign games. Final WRAM
hashes match the previously recorded moving and idle hashes. Pair A and the
profile use `mountain-cache-deck` (SHA-256 recorded above); timestamped pair B
and idle use `priority-trace-deck`, SHA-256
`cb290f044f213c8a153fd4c6c832f5e21678926bfa2fffc66ad7f4f22b52bc0e`.
The only production-source change in this audit is opt-in trace timestamps.
Mac Release and Deck cross builds pass; style/diff checks are clean for that
change. No rendering, source-delay or shadow-cache default was changed.


## Physical presentation feedback and rejected timing experiments

This follow-up checks whether the app's completion-time tails correspond to
uneven display delivery, before promoting retained shadows or changing packet
waits. The important result is that the steady-state **display interval p99 is
11.111 ms**, even where app completion p99 is 15–16 ms. The experiments did not
qualify a new default. Shadow reuse remains opt-in; the timing experiments were
removed from production source and saved as reproducible artifacts.

### Measurement and frame association

An isolated Linux `LD_PRELOAD` diagnostic requests Wayland `wp_presentation`
feedback for the SDL GPU window. It records swapchain-bearing command-buffer
submissions, matches their FIFO buffer-bearing `wl_surface.commit` requests,
and attaches feedback immediately before each actual commit. It recognizes
Wayland surface wrappers by object ID. A private event queue prevents the
presentation thread from racing listener registration on the SDL event thread.
The original acquire-time probe is retained separately, but its source-frame
association is treated as unverified and excluded from cadence conclusions.

The compositor reports clock ID 1 (`CLOCK_MONOTONIC`) and feedback flags 7:
vsync, hardware clock, and hardware completion. See the
[Wayland presentation protocol](https://raw.githubusercontent.com/wayland-mirror/wayland-protocols/main/stable/presentation-time/presentation-time.xml).
These are compositor-reported display timestamps, not a camera or photodiode
measurement. Sequence counters are zero on this compositor, so missed refreshes
are inferred from timestamp gaps and the reported refresh period. The initial
clock offset maps timestamps into the game's SDL clock. A separate ten-second
clock check found 26.8 microseconds of relative drift (2.68 ppm), much smaller
than the millisecond variation being investigated.

`analyze_feedback.py` joins the original acquisition timestamp to its enclosing
pacing-CSV present, verifies preceding submissions and FIFO buffer commits,
rejects missing internal feedback, and excludes only the final unresolved
shutdown suffix (four buffers per run). All six qualified runs have one buffer
commit per swapchain submission and no unmatched commits. Previous buffer
commits had completed before the next acquisition in the measured windows.
When a refresh is missed, the analysis includes the previous source frame's
additional hold. Discarded buffers are not counted as displayed frames.

Cadence mismatch below means the percentage of displayed source selections
incompatible with the best constant source-to-display delay in that window.
It permits normal 60.0988 Hz to 90 Hz one/two-refresh holds and fits one phase
per source epoch. It is an optimistic diagnostic of variable source timing,
not a percentage of visibly perceived stutters or a count of missed refreshes.

### What the timing experiment changed

The existing refresh estimator accepts individual return intervals only when
they are within ten percent of nominal. Asymmetric CPU completion jitter can
bias that selected subset: a deterministic 90 Hz regression fixture produces
an estimated rate of about 95 Hz. A candidate instead measures complete
64-present spans, so early and late returns cancel. The candidate retains the
existing phase filter and stall recovery. A slower phase-filter variant was
also explored but did not establish consistent improvement.

The span candidate passes a new regression test for asymmetric return jitter
and fractional refresh, including a rate error below 10 microseconds and phase
variation below 0.5 ms after warm-up. The original estimator fails this test.
That mathematical improvement did **not** translate into a consistent Deck
cadence improvement, so the candidate was not kept in the default path.

### Qualified Deck comparisons

All runs use the same 3,200-tick Aitos replay, original SIM menus, native text,
Fullscreen 1280×800 at 90 Hz, Wayland/KDE, and the existing native source delay.
The moving replay begins at tick 1,100. The second measurement window ends at
the last delivered frame before shutdown, rather than assuming unresolved
buffers were displayed. The table compares the existing default, span estimator
alone, and span estimator plus retained shadows:

| Workload / host ticks | App p99: default / span / span + shadows | Display-source mismatch: default / span / span + shadows |
|---|---:|---:|
| Moving, 1,100–1,900 | 15.61 / 15.27 / 13.69 ms | 6.25% / 3.75% / 5.08% |
| Moving, 2,200–end | 15.18 / 15.49 / 14.04 ms | 4.22% / 4.01% / 4.82% |
| Idle, 1,100–1,900 | 15.57 / 16.55 / 14.04 ms | 3.75% / 9.42% / 3.17% |
| Idle, 2,200–end | 13.46 / 13.38 / 12.33 ms | 3.68% / 5.69% / 3.82% |

Display interval p99 is 11.111 ms in every cell. Individual windows still have
zero to four missed refreshes and the same number of discarded buffers; this
is not perfectly uniform delivery, and p99 alone hides those rarer events.
The shadow path consistently reduces app completion tails, but it does not
consistently improve source cadence. The clock-only candidate worsens idle
cadence. These samples do not justify promoting either change, and they do not
establish a statistically reliable change in rare missed-refresh frequency.

### Packet pickup versus the presentation clock

For the existing default, inspect the irregular delivered frames themselves:

| Window | Irregular delivered frames | Correct source for the app's estimated sample | Due source missing when drawing began |
|---|---:|---:|---:|
| Moving, 1,100–1,900 | 75 | 75 | 0 |
| Moving, 2,200–end | 63 | 63 | 0 |
| Idle, 1,100–1,900 | 45 | 45 | 0 |
| Idle, 2,200–end | 53 | 53 | 0 |

All 236 actual irregular deliveries selected the correct native source for
`sample_ns - delay_ns`. The two reconstructed missed-refresh holds in the late
idle window are separate from those app decisions. Across all measured
baseline presents there were zero overdue endpoints at draw time. The predicted
sample was also always in the future at source selection; none of the 5,394
baseline presents clamped its predicted sample to the current time. The delay
between the app's estimated sample and actual delivery is roughly 40 ms and
varies by several milliseconds; that includes buffering and is not a direct
input-latency measurement.

This changes the priority: first make the source-selection clock predict
actual delivery consistently, separating CPU return phase from queued display
work. Validate it against per-buffer presentation feedback and actual source
holds. Simply shortening source polling does not correct the wrong sampling
time, and reducing shadows does not by itself align those clocks. Preserve the
native source delay and current defaults until an end-to-end comparison passes.
Owner preparation costs and construction rebuilds remain later targets.

### Artifacts and validation

All artifacts are under `runs/sim-pipeline-2026-10-03/`:

- `deck-feedback-commit-{base,span-base,span-cache}-{move,idle}` contains the six
  qualified runs; `feedback-summary.json`/`.txt` and `analyze_feedback.py`
  reproduce the comparisons and source-selection attribution.
- `presentation-probe/probe-commit.c` and `probe-commit.so` are the exact commit
  probe. The older `probe-acquire.*` and `deck-feedback-*` exploratory runs are
  retained for auditability, not mixed into qualified source-cadence results.
- `refresh-span-experiment.patch` preserves the rejected candidate and its
  regression test without changing playback defaults. `span-refresh-deck` is
  its pinned test binary; `stable-refresh-deck` is the slower-phase experiment.
- Probe SHA-256:
  `b822d9b3e83d49c6426e90a09ff0d612b1a69a72f9f53d70f67781590b1f54ac`.
- Span binary SHA-256:
  `09dcb6b26280f5abce87557c0102b2fc967593775760bed8ecc79c5125e49432`.
- Default binary remains `priority-trace-deck`, hash recorded above.

All six qualified runs exit cleanly with no detected foreign game and matching
final gameplay hashes for their moving/idle replay. The candidate builds on Mac
and Deck; its new test passes in Release and ASan/UBSan, five related pacing,
refresh, producer and queue CTests pass, and twelve pacing-analyzer tests pass.
Changed files add no style violations. The candidate's failed Deck acceptance
check takes precedence over those unit/build results: only diagnostic trace
and documentation changes remain in the working tree.

## Follow-up: fixed native content timeline

Native Vsync selection now advances a content timestamp once per successful
present, using the display mode's precise rational refresh interval. The old
CPU-return estimator is retained behind `AR_FRAME_REFRESH_TIMELINE=0`. The Deck
reports 11,110,617 ns per refresh; rounding to 90 Hz would introduce avoidable
phase drift. The per-display cache preserves the precise interval alongside the
rounded UI rate across transient query failures and invalidates both on removal.
Software pacing, source delay, gameplay tick rate and action interpolation are
unchanged. This is native frame selection, not SIM interpolation.

The content timestamp may precede CPU wall time while queued output recovers.
It is not a measured scanout time or a CPU deadline. Ordinary return jitter does
not change it; a refresh-rate change, backward clock, gap over three refreshes,
or accumulated phase error over three refreshes restarts it. The phase bound
also prevents sustained slow rendering from accumulating unlimited source lag.
An earlier one-refresh reset threshold restarted during normal queue recovery
and regressed a late moving window to 18.33% mismatch; that version was rejected.

### Display measurements and their limits

The same exact-commit presentation probe is used throughout. Pinned artifacts:

- `timeline-refresh-deck`: rejected one-refresh reset/clamped candidate,
  `918b3dfa83015e679e8392360638d61f7f8d72d42ce37136381e62dbcb0e8b5b`.
- `content-timeline-deck`: initial unclamped candidate,
  `d49dbcb40c0b4a8a96c3e36e4060b0b30c9d8e3a859ab91dfc476dd21d48ea9e`.
- `bounded-content-deck`: adds cumulative lag bound,
  `911e78ee29e1a4717609de7dfdd6302dc2ef7f29579b7767ee2c9735f37c9734`.
- `default-content-deck`: default enabled and precise per-display cache,
  `f12378318885b37097ab84ad18b3047fa7d1e7b5d24cd57b148acdb3ed2f6259`.

For the final same-binary moving comparison, `AR_FRAME_REFRESH_TIMELINE=0`
selects the control. The initial idle pair uses the bounded control and initial
unclamped candidate; the added lag bound did not fire in those steady windows.

| Workload / ticks | Control app p99 | Timeline app p99 | Control mismatch | Timeline mismatch |
|---|---:|---:|---:|---:|
| Final moving, 1,100–1,900 | 15.05 ms | 17.73 ms | 3.33% | 0.25% |
| Final moving, 2,200–end | 14.48 ms | 17.72 ms | 5.15% | 0.34% |
| Initial idle, 1,100–1,900 | 16.23 ms | 17.77 ms | 7.75% | 0.33% |
| Initial idle, 2,200–end | 13.48 ms | 18.00 ms | 3.75% | 0.94% |

**Display interval p99 remains 11.111 ms.** Higher app-return p99 is not proof of
slower display delivery. The final moving candidate misses zero refreshes in
both windows; its control misses zero/one. Initial idle misses one/five versus
one/zero for its control. These small counts do not establish a reliable change
in missed-refresh frequency. Typical steady sample-to-display variation falls
from several milliseconds to roughly 0.1–0.2 ms in the short windows, though its
absolute offset varies between runs and after stalls. Source age is not measured
input latency.

The final idle repeat has 0.17% mismatch in the early window but **29.45%** over
the entire late window. There is one real 22.222 ms display interval at tick
2,762, without a discarded buffer or content-clock reset. It changes the output
queue's delay by one refresh. A single fixed-phase fit across both sides counts
many later selections as mismatches, even though ordinary hold lengths remain
one/two refreshes. Separately, ticks 2,200–2,700 and 2,800–end have 0.13%/0.17%
mismatch and no missed refreshes. This split explains the metric; it does not
remove the missed refresh from the result or claim perfectly smooth output.

The 9,000-tick moving run has 0.33%/0.67% mismatch in the original windows.
Later construction stalls force clock resets at host ticks 4,440, 5,585 and
7,857. Windows spanning them have 1.53%, 8.36% and 17.98% fixed-phase mismatch.
Construction-free intervals 3,500–4,300, 4,600–5,500, 5,700–6,500, 7,000–7,800,
and 8,000–end measure 0.25%, 0.30%, 0.33%, 0.17% and 0.54%. Actual missed
refreshes and their held images remain included in all those measurements.

Retained shadows with this clock measure 0.33%/0.27% moving and 0.33%/0.34% idle,
with one/zero and zero/zero missed refreshes. This is promising but one pair does
not qualify the shadow cache across other rendering paths; it remains opt-in.
The next priority is construction and cache recovery, before SIM interpolation.

### Validation and reproduction

Release and ASan/UBSan tests cover asymmetric return jitter over 12,000 samples
at fractional 60/90 Hz and 120 Hz, native source choices, queue recovery,
sustained slow rendering, stalls, backward clocks, disabling and mode changes.
Precise refresh-cache tests cover failed queries, fractional periods, per-display
isolation, removal and capacity eviction. Five related pacing/producer/queue
CTests and twelve analyzer tests pass. Mac Release and Deck cross builds pass.

An attempted action regression using the Aitos fixture warped to 01/01 fails in
the **old-clock control** at game frame 1,003 with a PPU scanout ABI service error.
It was stopped, subsequent batch cases were cancelled, and none is counted as
successful action coverage. This does not establish an action regression from
the clock change; live action validation of this cross-build remains incomplete.

The confirmed 3,200-tick moving and idle experiment hashes remain respectively
`4abbae59dacedd72e9f4e676ee6641e60d62a24e7bbc750deee5871154224c71` and
`5211a57db8dfc8e30a3e2866d16d05b63df484a379b3a78f9d416fefd3f0f8d4`.
Data is under `runs/sim-pipeline-2026-10-03/deck-feedback-{content-*,bounded-*,final-*}`.
The analyzer accepts `--run`, repeatable `--window LO:HI` and `--output`;
`feedback-final-summary.json`, `feedback-final-idle-segments.json`,
`feedback-long-summary.json`, `feedback-long-steady-summary.json` and
`feedback-cache-summary.json` retain the full and partitioned comparisons.

## Construction follow-up: model invalidation and cache recovery

The 9,000-tick trace confirms two separate costs. Development changes cause a
large rebuild in their own draw, then ground-art animation phases repopulate
on later draws. For example, the baseline event at game frame 4,432 takes
51.10 ms, followed by view-setup costs of 6.72, 6.04 and 5.44 ms at frames
4,435, 4,441 and 4,449. Other later events show the same three-phase recovery
pattern over roughly 0.3–0.4 seconds. This is measurable follow-up work; it is
not evidence of permanently elevated per-frame cost after every construction.
Separately, a long construction stall can restart the native content timeline
and change its delivery offset, as recorded above.

### Retained fix

`AppendSimGlobeModels` no longer puts the broad geography revision in the model
source style. Every source already carries its resolved floor and bridge-depth
heights, object identity, detail and position; the style carries the full globe
mapping, scale and lighting. Both model compilers use those complete values.
A revision alone cannot change their vertices. Removing that extra invalidation
retains unchanged neighbours and static town models, while real source/mapping
changes still rebuild. `AR_SIM_MODEL_SOURCE_KEY=0` restores the previous key.
No new geometry approximation, memory cache or quality reduction is introduced.

The initial long baseline, the model-key candidate, and a later old-key control
all run the same moving Aitos replay for 9,000 ticks. Later one-cell events often
leave the model inputs unchanged:

| Game frame | Initial baseline draw | Model-key draw | Later old-key control draw | Model preparation, baseline → fix |
|---|---:|---:|---:|---:|
| 4,432 | 51.10 ms | 33.54 ms | 55.59 ms | 17.03 → 0.09 ms |
| 5,576 | 50.20 ms | 29.80 ms | 56.81 ms | 17.78 → 0.09 ms |
| 6,711 | 49.84 ms | 33.39 ms | 50.96 ms | 17.26 → 0.10 ms |
| 7,846 | 51.89 ms | 36.00 ms | 50.68 ms | 18.11 → 0.09 ms |

These are inclusive CPU wall scopes, not GPU durations. Display feedback also
improves in these event windows: the initial baseline's maximum gaps of
44.44/44.44/44.44/55.55 ms become 33.33/22.22/33.33/33.33 ms. Missed refreshes
fall from 5/5/5/6 to 2/2/2/4. The early clustered construction window still has
a 55.55 ms maximum gap, and changes that actually alter models still incur
model compilation. The fix reduces construction cost; it does not eliminate
construction stalls or qualify a universal 90 Hz lock.

### Rejected allocation-reuse experiment

The ground atlas currently discards its animated GPU snapshots after changed
geography/native sources. A separate experiment invalidated their identities
while retaining their texture allocations for complete recapture. It preserved
atomic replacement of valid versions, prohibited stale selection, and passed
real-GPU invalidation/recapture/queued-selection tests in Release and ASan/UBSan.
However, the Deck still showed approximately 5–7 ms follow-up view-setup costs.
It did not establish a material recovery improvement and was removed.

`atlas-storage-experiment.patch` preserves that candidate and tests;
`construction-reuse-deck` and `deck-feedback-construction-reuse-long` preserve
its run. The opposite-order `deck-feedback-construction-control-long` uses the
same binary with both `AR_SIM_MODEL_SOURCE_KEY=0` and
`AR_SIM_ATLAS_STORAGE_REUSE=0`. That latter diagnostic exists only in the saved
experiment, not the final source.

Remaining targets, ahead of SIM interpolation, are the animation-content work
needed to repopulate those phases and the roughly 8–11 ms cliff / 8–10 ms globe
surface rebuilds. Retaining allocation alone did not address the recovery cost.
Narrowing their dependencies or patching affected regions requires output parity;
merely ignoring a geography revision is safe for the fully resolved model
sources above, not automatically safe for terrain or textured surfaces.

### Validation and artifacts

- A new GPU regression advances geography using identical terrain artwork and
  asserts zero model compilation, then changes a model and requires rebuilding.
  Retained and cold pixels match exactly in both cases. The old-key negative
  control fails the zero-compilation assertion, demonstrating regression coverage.
- Focused town/model GPU tests pass in Release and ASan/UBSan. Related pacing,
  refresh, producer and queue tests pass. Mac Release and Deck cross builds pass;
  changed C files add no style violations versus HEAD.
- The broader world GPU suite is not green: a cloud-bound assertion
  `Differences(frames[i], capped) > 0` fails before the new model test, and also
  fails with the old model-key control. It is recorded in
  `construction-world-gpu-{full,control}.log`, not counted as successful coverage.
  The action ABI limitation from the previous section also remains open.
- All four long Deck runs exit cleanly, detect no foreign game and have identical
  final WRAM SHA-256:
  `d2c147d0fc639328a835842cbe1ab34476f062b5ee4fae14957febac68d7903c`.
- Final retained-code Deck binary: `model-source-key-deck`, SHA-256
  `16036212d7427b82708bb9e927cf6c736f1a512d9ab28b13a7c7179d54d5b0ec`.
- Rejected allocation experiment/control binary: `construction-reuse-deck`,
  `2439efab259605495c8bcf24b277b7a050865fc1b11ee49398501aa09c50c89a`.
- Logs/traces: `deck-feedback-model-key-long-move`,
  `deck-feedback-construction-{reuse,control}-long`,
  `construction-model-key-summary.json`, `feedback-construction-model-key.json`,
  and `feedback-construction-final.json` in the existing run directory.
