# Steam Deck prototype validation — October 20, 2026

The small excess in **host present-return p99 is not, by itself, a display
pacing failure**. A clear-only Vulkan window produced a 12.102 ms host p99
while Wayland reported a display p99 of 11.111 ms and no missed refreshes over
5,089 steady-state intervals. Bloodpool's effect-batching comparisons also
preserved a display p99 of 11.111 ms in both modes.

This changes how the earlier Deck batching result should be interpreted. Its
CPU saving is repeatable; a small increase in present-return p99 is insufficient
reason to reject it. It does not establish that every hitch on every platform
is harmless. The previously captured 33–66 ms **display** gaps on Metal remain
separate evidence.

## Setup and isolation

- Steam Deck OLED, active 1280×800 panel at 90 Hz, KDE desktop / KWin Wayland,
  Vulkan, RADV VANGOGH. Linux `6.18.50-valve2-1-neptune-618-gc7289a96b14d`;
  packaged SDL 3.4.16, verified by library hash.
- Same release executable for all game comparisons:
  `05624b0ce510f55a3121aa56af41f7725c83995d9822f71286167995d92721b1`.
  Current working-tree renderer and generated shaders; no default changes.
- Isolated settings, saves and outputs under
  `/home/deck/argame/prototypes-20261020`. The installed game and saves were not
  replaced. The panel stayed active; periodic lock checks remained unlocked.
- 16:10, square pixels, 64 extra rows, VSync, interpolation, direct GPU layers,
  native decode, environmental lighting/particles, CRT, rim, DOF and edge AA.
  Unrelated prototype switches were explicitly disabled for each timing test.
- Camera search: off/on/on/off in scrolling Marahna and on/off/off/on in
  Fillmore. Batching: off/on/on/off in stationary Bloodpool. Separate ABBA
  comparisons cover the preparation worker and stationary actor rejection.
  These are 20 uninstrumented game replays, excluding the setup pilot.
- Normal runs use ticks 1600–3100, about 25 seconds of settled samples per run.
  Batching uses ticks 1600–3700, about 35 seconds. Loading, warp, initial
  diorama entry and shader preparation are outside the measurement windows.
  Final WRAM hashes match within every equal-length scene comparison.

The results describe desktop Wayland. They do not establish Gamescope/Game
Mode behavior or D3D12 behavior. No Windows display-off result is used.

## Host timing and compute savings

Values below are milliseconds. Each cell is the median of the two run means
or p99s, not a pooled percentile. These host scopes are not GPU timers.

| Prototype / scene | Host draw mean off → on | Owner draw CPU mean off → on | Host present-return p99 off → on |
|---|---:|---:|---:|
| Camera search, Marahna | 0.4115 → 0.4181 | 0.4065 → 0.4124 | 11.7513 → 11.7551 |
| Camera search, Fillmore | 0.8251 → 0.8151 | 0.8042 → 0.7978 | 12.0941 → 12.0791 |
| Effect batching, Bloodpool | 2.6917 → 2.3800 | 2.6100 → 2.3139 | 11.9808 → 12.0861 |
| Preparation worker, Marahna | 0.4115 → 0.4794 | 0.4060 → 0.4251 | 11.8675 → 11.9754 |
| Actor rejection, Bloodpool | 2.7134 → 2.6839 | 2.6288 → 2.6058 | 11.7931 → 11.8834 |

All 20 runs averaged approximately 90 host presents/second. Batching saves
about **0.30 ms of owner CPU time** (11.3%) and 0.31 ms of draw wall time.
Under VSync most of the saving becomes swapchain waiting: mean acquisition
time increases from 5.7742 to 6.0941 ms. That is neither evidence of increased
GPU shader cost nor a new missed refresh.

The camera bound primarily reduces GPU search work, so owner CPU draw time is
not its useful metric. The isolated Vulkan global-search benchmark submits
eight analyses at a time, excludes uploads/downloads/compilation, warms each
mode, and reverses the mode order. Values are encoding-through-completion
time per analysis, **not hardware kernel timestamps or whole-frame latency**.

| Eight 640×352 synthetic planes | Hint off | Correct hint | Reversed hint |
|---|---:|---:|---:|
| Stationary | 1.3516 | 1.3151 | 1.3150 |
| Scrolling | 3.4029 | 1.8033 | 3.4101 |
| Scrolling with changed pixels | 3.4541 | 2.0933 | 3.4816 |
| Empty | 1.3143 | 1.3507 | 1.3507 |

Correct hints reduce these scrolling workloads by **47.0% and 39.4%**. Wrong
hints preserve the original search and its result. The small differences in
stationary/empty controls must not be extrapolated into a whole-game speedup.

The isolated actor shortcut remains workload-dependent: mixed stationary/moving
content improves from 1.187 to 0.806 ms, sparse stationary sprites from 0.312 to
0.251 ms, and dense moving content changes from 0.933 to 0.941 ms. The dense
stationary reference also drifts from 1.186 to 0.896 ms between its two rounds;
do not use that case's aggregate percentage as a stable performance promise.
These compute savings do not establish a gameplay pacing win in the actor
comparison above.

## Display feedback instead of assuming present return is scanout

An isolated `LD_PRELOAD` observer requests Wayland `wp_presentation` feedback
on the game's actual SDL Wayland surface before its existing terminal submit.
It records display timestamps, discarded requests, callback receipt, window
flags and CPU/wall time in existing acquisition/submission calls. It adds no
GPU readback, fence wait or render pass. Protocol round trips occur only during
initialization and teardown. The rendering executable is unchanged.

The compositor reports clock ID 1 and flags 7 (VSync, hardware clock and hardware
completion). These are compositor/driver reports, not a photodiode measurement.
The protocol describes the distinction between [display feedback and client
submission](https://raw.githubusercontent.com/wayland-mirror/wayland-protocols/main/stable/presentation-time/presentation-time.xml).
Selected samples have no duplicate or nonmonotonic display timestamps and
no hidden, minimized or unfocused window samples. The analyzer retains
intervals across discarded submissions so those cannot hide a display gap.

| Run | Host interval p99 | Display interval p99 | Largest display interval | Display gaps over 16.67 ms |
|---|---:|---:|---:|---:|
| Clear-only control | 12.1022 | 11.111 | 11.113 | 0 |
| Bloodpool, batching off, first | 12.0583 | 11.111 | 11.114 | 0 |
| Bloodpool, batching on, first | 12.0154 | 11.111 | 11.112 | 0 |
| Bloodpool, batching on, repeat | 11.6336 | 11.111 | 11.111 | 0 |
| Bloodpool, batching off, repeat | 11.9731 | 11.111 | 22.222 | 2 |
| Marahna, camera bound off | 11.9674 | 11.111 | 11.112 | 0 |
| Marahna, camera bound on | 11.5155 | 11.111 | 11.118 | 0 |
| Fillmore, camera bound on | 11.6389 | 11.111 | 11.112 | 0 |
| Fillmore, camera bound off | 12.1942 | 11.111 | 11.112 | 0 |

These eight instrumented game replays are additional to the 20 uninstrumented
comparisons. Camera-search feedback contains 8,987 display intervals with no
missed refreshes in either mode. Its host-p99 change is not a demonstrated
visible cadence improvement when both modes already deliver steady 90 Hz.

The second unbatched run includes two discarded submissions and two ~22.22 ms
display gaps; they are retained. The run is not evidence that batching prevents
rare hitches. Conversely, neither enabled sample shows a missed refresh, so
the earlier small host-p99 penalty is not reproduced as a display regression.

The empty control spends 10.859 ms on average in acquisition but only 0.161 ms
of CPU time there. Its return-to-display delay averages 38.94 ms even though
display cadence is steady. Throughput, CPU headroom, queueing latency and
visible cadence are different quantities.

Linux's per-task `schedstat` distinguishes [CPU execution from runnable queue
wait](https://docs.kernel.org/scheduler/sched-stats.html), but detailed scheduler
accounting is disabled on this Deck. The probe explicitly marks that field
unavailable. CPU/wall and context-switch counters do not identify every kernel,
driver or compositor dependency. No global tracing or scheduler policy was
changed. We cannot claim to have ruled out all operating-system scheduling
effects or identified the cause of each rare display gap.

Four initial game-feedback attempts are excluded from display analysis: the
launcher parent overwrote the renderer child's trace with an empty file at
exit. The corrected observer writes only from a process that actually observed
presentation; replacement runs require nonempty feedback. The standalone
control was unaffected. These exclusions are recorded in `feedback-excluded.json`.

## Correctness and readiness

- All **1,152 Vulkan CPU-oracle global-motion checks** passed.
- Vulkan actor-motion, frame-generation/batching and pipeline-preparation tests
  passed. Batching tests cover actual pixels and transfer/scratch-buffer reuse.
- **134/134 full-game frames match exactly**, comparing all four prototypes
  enabled together against all four disabled: Marahna phase 0.5 (48 frames),
  Fillmore phases 0.25 and 0.75 (43 each). BG1 travel is 1,024×147 and 1,044×188
  pixels respectively. These include horizontal scrolling and vertical drops,
  native capture, direct output, resident lighting and worker handoff.
- The capture tool now accepts `--effect-batching 0|1` and verifies its
  activation in resident-effects captures. Its existing 19 evidence tests pass;
  their mock provenance now includes the environment used by prototype checks.
- All 28 accepted game timing replays completed successfully with matching
  final WRAM within each scene/duration, no late shader builds, no motion
  metadata downloads and no resident-projection recovery. Standalone GPU tests
  compile their own pipelines before their timed regions; their initialization
  logs are not gameplay shader-build evidence.

Camera-guided search is now a supported candidate for Metal **and Deck Vulkan**
default promotion: exact pixels and a useful compute saving have been verified
on both. Effect batching is also a strong Vulkan promotion candidate: its CPU
saving repeats, and the old host-p99 objection is not a measured display-pacing
regression in the follow-up. These are recommendations; defaults remain unchanged.

Keep the preparation worker and actor rejection optional for now. These game
comparisons show no compelling broad pacing benefit. The earlier-acquisition /
later-frame-selection prototype is a separate latency improvement and still
needs its production lifecycle implementation and validation.

## Evidence

Local ignored evidence is in `runs/deck-prototype-validation-20261020/`:
source/binary hashes, cross-build logs, replay plans, exact environment records,
raw pacing/preparation/Wayland traces, telemetry, shader-search benchmarks,
pixel reports, the clear-only control, diagnostic observer sources, and the
host-versus-display timing graph. The diagnostic observer is not linked into
the game. No commit, push, installed-game replacement or default change was made.
