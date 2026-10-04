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
