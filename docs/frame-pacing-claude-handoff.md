# Frame pacing investigation — handover for Claude

Prepared October 20, 2026. This is a request for an independent investigation,
including alternative explanations and architectural options. Treat the
interpretations below as hypotheses to verify against the code and traces.

## Objective

Make the enhanced action renderer sustain smooth 90 Hz on Steam Deck, with
reasonable latency, exact visual output, and portable Metal/Vulkan/D3D12 behavior.
Understand the remaining stalls rather than improving an average metric while
leaving visible hitches unexplained. Native, non-interpolated presentation also
matters. Do not change the simulation rate to meet the display rate.

The user's latest question was whether freeing CPU headroom without improving
frame times implies a GPU bottleneck. It does not establish that by itself:
the remaining limit could be GPU work, a serialized CPU dependency, resource
waiting, presentation backpressure, or the display's refresh cap. Recent Deck
measurements show that some apparent over-budget host intervals are compatible
with perfectly regular compositor-reported display intervals.

The user wants new angles, not another unqualified conclusion that everything
is an OS problem. There is evidence of downstream presentation delay on Mac;
there is not a complete attribution of every hitch on every platform.

## Workspace and preservation

- Repository: `/Users/derrick/Documents/Programming/ActRaiserRecomp`.
- Branch at handover: `main`; HEAD
  `7baac894aa23aeb805cf9641ad70dd6d5799d9db`.
- The working tree is intentionally dirty. It contains startup pipeline
  preparation, motion prototypes, tracing, tests and reports from this ongoing
  investigation. Preserve those changes and any unrelated user work. The latest
  Deck validation did not change renderer defaults, commit, or push.
- Important recent renderer commits: `f7fb6436` (direct GPU layer output / Metal
  queue integration), `4c6f7ed3` (GPU full-add capture / packet preparation).
  Later HEAD commits also include unrelated SIM/controller work.
- Evidence is under ignored `runs/`; it is **not included in an ordinary Git
  clone**. This handover assumes access to the existing Mac workspace. Copy
  the referenced evidence separately for another machine; do not distribute
  ROM/game assets with a public report.

## Read these first

1. [Latest Steam Deck validation](steam-deck-prototype-validation.md): final
   comparison tables, limitations, correctness, and default-promotion candidates.
2. [GPU execution and presentation timing](gpu-presentation-timing.md): long
   chronological investigation. Start with “Isolating the presentation stalls,”
   “Independent display-clock observation,” and the October 20 Deck cross-check.
3. [Presentation scheduling](gpu-presentation-scheduling.md): the
   acquisition-before-selection prototype, its latency result, and production gaps.
4. [Integrated renderer paths](gpu-rendering-integration.md) and
   [drawable/output queue experiments](gpu-presentation-queue-experiments.md).
5. [Effect batching](gpu-effect-command-batching.md),
   [camera-guided search](gpu-camera-motion-bound.md), and
   [actor rejection](gpu-actor-motion-rejection.md).

Older documents preserve the decisions made at the time. Read their dated
follow-ups before treating “not tested on Deck” or “keep off because of p99” as
the latest conclusion. The standalone benchmarks are not whole-game timings.

## Architecture and code map

The producer executes game/PPU work independently and publishes immutable frame
slots. The render owner chooses a source pair and interpolation phase, prepares
GPU endpoints/motion, composites effects and postprocessing into an offscreen
output, then waits for a swapchain image and performs the terminal copy/present.
The optional GPU-preparation worker is separate from the simulation producer.

```text
Producer: game/PPU capture → immutable frame slots ────────────────┐
                                                                ↓
Owner: select source/phase → endpoint preparation + GPU commands → offscreen image
       → wait/acquire drawable → terminal copy + presentation request
                                                                ↓
GPU/driver/compositor: execute dependencies → latch/display an image
```

This diagram shows dependencies, not a promise that the stages run serially or
that their measured durations can be added. GPU commands may overlap or include
dependency waits. Source frames and display updates run at different rates.

| Responsibility | Starting points |
|---|---|
| Producer and streaming loop, pacing CSV | `src/app/game_loop.c` (`ProduceFrameStream`, `GameLoop_Run`) |
| Slots, source selection, playout clock | `src/host/frame_queue.*`, `frame_playout.h`; producer API in `frame_producer.h`, implementation in `src/platform/sdl/frame_producer_sdl.c` |
| Host presentation policy / early submission | `src/host/host_display.c`, `host_display_pacing.*`, `host_video.c` |
| Final output and swapchain wait | `src/platform/sdl/render_sdl.c` (`PresentWindow`, `PreparePresent`, `PresentPrepared`) |
| Endpoint capture, motion, direct outputs, optional worker | `src/platform/sdl/diorama_frame_generation_sdl.c` |
| Global and actor motion | `src/platform/sdl/gpu_global_motion_sdl.*`, `gpu_block_motion_sdl.*`; `src/shaders/motion_*` |
| Effect command batching | `src/platform/sdl/action_effect_source_sdl.c` |
| Shared compositor | `src/diorama/`, `src/present/`, render-device interfaces |
| Startup preparation and diagnostics | `src/present/render_preparation.c`, `src/render/gpu_compile_log.h` |
| Mac observer / reduction | `tools/profile_metal.m`, `tools/analyze_metal_presentation.py` |
| Host timing / exact capture tools | `tools/analyze_frame_pacing.py`, `tools/compare_action_gpu_frames.py` |

Production uses SDL GPU plus custom raster/compute commands; the cost is not
just SDL's 2D raster API. Direct interpolation output, GPU background capture,
resident environmental projection and removal of normal motion readbacks are
already integrated. Do not propose those again as if they are still absent.

## Current prototype/default status

| Mechanism | Status at handover | What the evidence says |
|---|---|---|
| Direct layer synthesis / lazy fallback atlases | Default; `AR_GPU_DIRECT_LAYERS=0` selects reference | Exact output; avoids intermediate writes/copies and ~13.58 MiB of fallback atlas allocation in the measured case |
| Metal two-drawable VSync pool | Default; `AR_METAL_DRAWABLE_COUNT=2/3` overrides | Within-process reversal removed ~one refresh of presentation delay; not a complete stall fix |
| Startup pipeline preparation | Present in dirty tree | 28 accepted Deck game timing runs reported no late shader builds |
| Camera-guided search bound | Opt-in `AR_GPU_CAMERA_MOTION_BOUND=1` | Strong promotion candidate on tested Metal/Deck Vulkan; exact pruning, useful scrolling compute savings |
| Scoped effect batching | Opt-in `AR_GPU_EFFECT_BATCHING=1` | Strong Vulkan candidate; repeatable owner CPU saving; prior host-p99 concern did not become a display regression in follow-up |
| Stationary actor rejection | Opt-in `AR_GPU_ACTOR_STATIONARY_REJECT=1` | Synthetic savings depend on content; no compelling broad game-pacing win |
| Native preparation worker | Opt-in `AR_GPU_PREPARE_WORKER=1` | No compelling Deck game win; Mac priority mismatch was explored separately |
| Early command submission | Opt-in `AR_GPU_EARLY_SUBMIT=1` | No reliable foreground pacing win; different from acquiring before source selection |
| Acquire drawable before selecting source/phase | Archived application prototype, not a production switch | Repeated ~16.5 ms reduction in selected content age on Mac; still needs explicit lifetime/error/transition handling |
| Multiple application output textures | Rejected/removed prototype | Removed a resource-reuse dependency without relieving drawable backpressure |

Do not silently turn on every prototype when comparing one of them. The Deck
harness explicitly disables unrelated switches. Current evidence does not
validate all optional switches for D3D12 or Gamescope.

## Strongest observations

### Steam Deck: host return jitter and display cadence are different

Steam Deck OLED, 1280×800 at 90 Hz, KDE/KWin **desktop Wayland**, Vulkan RADV
VANGOGH, packaged SDL 3.4.16. Not a Game Mode measurement. The display stayed
on and unlocked. Device policy was observed, not changed: 15 W cap, `powersave`
governor; detailed scheduler accounting disabled. Telemetry is retained.

The same release binary was used for 20 uninstrumented comparisons plus eight
accepted presentation-feedback runs. Settled windows exclude loading/warp/
initial diorama entry. Final WRAM matches within scene/duration comparisons.

| Observation | Result |
|---|---|
| Clear-only Vulkan control | Host interval p99 **12.102 ms**, display p99 **11.111 ms**, **0** missed refreshes in 5,089 intervals |
| Bloodpool effect batching | Owner draw CPU **2.610 → 2.314 ms**; host draw wall **2.692 → 2.380 ms** |
| Saved batching CPU time under VSync | Mostly becomes acquisition waiting: **5.774 → 6.094 ms** mean |
| Four Bloodpool feedback runs | Display p99 **11.111 ms** in every run; two ~22.22 ms gaps in one unbatched repeat, none in either batched run |
| Four camera-bound feedback runs | 8,987 display intervals, **0** missed refreshes with either mode |
| Isolated camera-guided search | Scrolling **3.403 → 1.803 ms**; changed scrolling **3.454 → 2.093 ms** per analysis |
| Correctness | **1,152** global CPU-oracle checks; **134/134** full-game frames exactly match with all four prototypes on vs off |

The two unbatched gaps and two discarded presentation requests were retained,
including display intervals that span discarded requests. These short windows
do not prove batching prevents rare hitches. The camera microbenchmark measures
encoding-through-completion of batched analyses, not a complete GPU frame.

Wayland reports hardware-clock/completion/VSync feedback flags. This is stronger
than an API-return timestamp but is still compositor reporting, not a physical
scanout measurement. The Deck observer has **no per-command GPU completion
timestamps**. It therefore cannot prove why those two individual gaps occurred.

### Mac: some real display gaps occur after GPU completion

Apple M2, Metal, normally 960×540 logical / 1920×1080 backing pixels at 60 Hz.
SDL 3.4.18. Active/unlocked foreground tests, settled windows, matched replays.
Do not translate a 60 Hz cap into a measured Deck 90 Hz throughput ceiling.

- Both an SDL GPU clear-only program and a native Metal clear-only program
  reproduced ~33.33 ms display gaps. Native Metal bypasses the game and SDL GPU.
  Their GPU commands took about **0.023 ms** on average.
- An independent display-clock diagnostic caught a **66.666 ms** displayed-image
  gap. The next image's request completed about **15.24 ms before the preceding
  image displayed**, and its GPU clear finished **15.162 ms before it**.
  Display-clock callbacks kept advancing during the gap. The following drawable
  acquisition waited **58.40 ms**: that wait was a consequence of delayed
  presentation in this event, not proof of 58 ms of rendering work.
- The same clear-only run changed from roughly **31.69 to 48.34 ms**
  request-to-display delay without changing its two-drawable policy. A changed
  private compositor/driver dependency is plausible but not identified.
- Cold/warm shader tests and startup preparation did not eliminate display
  gaps. Measured GPU completion precedes the affected display slots in specific
  captures. Do not blame those events on a hypothetical uncached shader.
- A separate large preparation wait also exists in some worker experiments.
  It must not be merged into the downstream presentation explanation. A sampled
  Metal command-buffer-creation stack and low thread CPU time narrow it, but do
  not identify every blocking/descheduling cause.
- Matching preparation-worker QoS to the owner was tested with within-process
  reversals. The repeatable owner-wait improvement was only about **0.02 ms**;
  downstream display gaps remained. Normal preparation remains inline.

GPU command start/end intervals may include dependencies. Do not sum overlapping
intervals into utilization or call a scheduled-handler callback a GPU timestamp.

### Freshness is independently improvable

Acquire-before-select changed the ordering from:

```text
select content → render → wait for drawable → present
```

to:

```text
wait for drawable → select content → render → present
```

Repeated within-process late/early reversals reduced content age by about
**16.5 ms**, including launches with different downstream delay states. Example:
Fillmore **77.162 → 60.593 ms** selected-timeline age at display, while
API-return-to-display remained **31.928 → 31.082 ms**. This is not measured
input-to-photon latency and does not remove the producer interpolation buffer.
The prototype did not establish a reduction in rare display-gap frequency.

## Fresh questions worth investigating

Please challenge these priorities if the evidence suggests a better experiment.

1. **Find the deadline that was actually missed on Deck.** Correlate source
   availability, owner selection, GPU completion, Wayland request/display and
   discarded feedback on the same time base. Add bounded asynchronous Vulkan
   timestamp/fence observation if feasible; do not add a per-frame readback or
   blocking wait that creates the phenomenon being measured. Record whether
   the next frame was GPU-ready before the relevant display opportunity.
2. **Separate compositor policy from application scheduling on Mac.** The
   minimal native reproducer is the best starting point. Windowed/fullscreen,
   display attachment and supported presentation policies are possible axes.
   Keep each reversal controlled and preserve source/GPU-ready timestamps.
   A real Metal System Trace could identify waits that public callbacks cannot;
   only Command Line Tools were available, with no `xctrace` installation.
   Native immediate/timed presentation and a CAMetalDisplayLink acquisition
   pilot already exist; neither demonstrated a general latency/stall cure.
3. **Prepare reusable work before the pacing gate; select phase after it.**
   The early-acquisition result may be productionized without delaying all
   expensive endpoint/motion preparation until after acquisition. Investigate
   explicit prepared-frame ownership and cancellation, rather than the
   prototype's global drawable reservation. Measure overlap, selected content
   age and deadline slack together. Native playback needs separate cadence tests.
4. **Audit the critical dependency, not total CPU utilization.** A mostly idle
   process can still wait for one producer task, worker, lock or resource.
   Correlate producer publication, wakeup/join and terminal ownership. Check
   whether batching changes the timing of submissions relative to refresh
   deadlines rather than their GPU cost. Deck detailed runqueue accounting was
   unavailable; zeros must not be interpreted as no scheduling delay.
5. **Measure scene and tail coverage.** Longer sessions and the animated Aitos
   waterfall may exercise different dependencies than stationary Bloodpool.
   Compare Desktop Wayland and Gamescope at the same active 90 Hz settings when
   available. Report miss counts and worst events alongside p99; a rare held
   frame can disappear below a percentile without being fixed. Track distinct
   content/phase progress as well as distinct presentation requests.

Avoid increasing queues solely to make a throughput graph smoother without
reporting the resulting latency. Ray tracing is deliberately deferred because
of portability complexity; no evidence makes it relevant to these waits.

## Evidence locations and reproduction

All local paths below are relative to the repository unless absolute.

| Evidence | Location / useful files |
|---|---|
| Latest Deck results and raw timing | `runs/deck-prototype-validation-20261020/deck/`: `summary.json`, `feedback-summary.json`, `feedback-excluded.json`, `runs/*/{pacing,presented,waits}.csv`, `analysis.json`, `telemetry.json`, `environment.json`, logs |
| Deck pixel evidence | Same root: `parity-summary.json`, `parity-comparison-*/comparison.json`, `parity-*/capture.json` and provenance. Large raw PPMs remain on Deck |
| Deck diagnostic sources | `runs/deck-prototype-validation-20261020/{profile-waits.c,minimal-present.c,analyze-feedback.py,run-deck.py}`; `wayland/` protocol headers/XML |
| Host versus display graph | `runs/deck-prototype-validation-20261020/deck-host-vs-display.png` |
| Frozen source/binary identity | `runs/deck-prototype-validation-20261020/source-manifest.json`; per-plan metadata and per-run environment |
| Mac independent clock event | `runs/gpu-stall-attribution-20261019/mac-native-clock/{metal.csv,refresh.csv,refresh-analysis.json,metadata.json}` |
| Mac native control sources | `runs/gpu-stall-attribution-20261019/minimal-native*.m`, `analyze-display-clock.py`, `analyze-clear.py` |
| Mac acquire-before-select reversals | `runs/gpu-present-scheduling-20261019/early-cycle-{fillmore,marahna,marahna-repeat}/`; `early-cycle.patch`, `prototype-*.c`, `analyze-early.py`, `summarize-scheduling.py` |
| Prior algorithm and queue experiments | `runs/gpu-camera-bound-20261019/`, `gpu-motion-pruning-20261019/`, `gpu-present-queue-20261019/`, `gpu-command-batching-20261018/` |

Deck staging directory: `/home/deck/argame/prototypes-20261020` via `ssh steamdeck`.
Installed game/saves were not replaced. The tested executable SHA-256 is
`05624b0ce510f55a3121aa56af41f7725c83995d9822f71286167995d92721b1`.
The fixed Wayland observer SHA-256 is
`dfc447e219821295b45776ebfed6ccbd2eec29e826bbfca226ffa6ff6a9fbc8d`.

**Observer pitfall:** the first game-feedback attempts produced empty files
because the launcher parent overwrote the renderer child's trace during exit.
Use `profile-waits-fixed.so`, not `profile-waits.so`, for game tests. The fixed
observer writes only from a process that observed presentation. The original
standalone control was unaffected. Require nonempty, complete steady-state
feedback; never treat absent data as “zero stalls.”

To run a fresh matched Deck batching comparison, first confirm the display is
active/unlocked and there is no other GPU workload. In an SSH shell on Deck:

```sh
cd /home/deck/argame/prototypes-20261020
python3 - <<'PY'
import datetime, json, pathlib, subprocess
stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S')
plan = pathlib.Path(f'feedback-claude-batch-{stamp}.json')
plan.write_text(json.dumps([
    {'name': f'feedback-claude-batch-{stamp}-{i}-{mode}', 'room': '0201',
     'observer': 'profile-waits-fixed.so',
     'env': {'AR_GPU_EFFECT_BATCHING': str(mode)}}
    for i, mode in enumerate([0, 1, 1, 0])
], indent=2))
subprocess.run(['python3', 'run-deck.py', str(plan)], check=True)
PY
python3 analyze-feedback.py
```

The runner isolates saves/settings, bounds execution, verifies activation and
records telemetry. It refuses existing run directories. Its default settled
window is ticks 1600–3100. For camera tests use rooms `0101` and `0501` with
`AR_GPU_CAMERA_MOTION_BOUND`; existing plans show exact controls. Keep the
`feedback-*` run-name prefix for the observer analyzer's automatic discovery.
Do not overwrite old evidence or silently omit newly named runs.

The downloaded Deck reports can also be regenerated locally without a replay:

```sh
python3 runs/deck-prototype-validation-20261020/deck/analyze-feedback.py
```

For Mac, the scheduling report contains working replay/analysis commands.
`run-mac.py` accepts a unique run name, room, refresh policy, observer mode and
optional JSON environment overrides. Inspect the override file: some select
an archived experimental executable rather than today's build. Some standalone
control runners also hardcode their original output directory; copy/adapt them
to a new name rather than deleting evidence to rerun them.

Tracked tooling checks:

```sh
python3 tests/pipeline_performance_tool_test.py
python3 tests/metal_presentation_tool_test.py
git diff --check
```

The Deck focused binaries already staged are `actraiser_gpu_global_motion_test`,
`actraiser_gpu_block_motion_test`, `actraiser_diorama_frame_generation_test` and
`actraiser_render_sdl_preparation_test`. Run with the staged SDL library and
active Wayland environment. Their initialization-time shader messages do not
mean gameplay compiled a shader late.

## Windows and validation constraints

`ssh windows` was available, but the user confirmed its display was off or
desktop locked. Those pacing captures are unsuitable for normal display
conclusions. Do not resurrect them as evidence of a renderer regression.
PresentMon event capture also lacked tracing privileges; a script was staged,
but the user chose to continue on Mac. Do not assume permission to change
machine security or tracing policy. Retest Windows when the desktop is active.

Keep native GPU APIs and diagnostics behind platform boundaries. Preserve the
shared renderer/editor behavior and receiver/material alignment. Preserve
generic, game-agnostic boundaries in `snesrecomp-go` if touching it. Establish
pixel equivalence separately from game-state hashes. No observer/interception
library or diagnostic global reservation should become shipping architecture.

## Desired outcome from Claude

Return a ranked set of explanations, each distinguishing observed facts from
inference, with the smallest experiment that would confirm or reject it.
Identify any flaw in our instrumentation or reduction before trusting the
headline numbers. If an optimization is implemented, compare the same workload
with reversed run order and report CPU work, GPU readiness, actual display
intervals, content age, tail-event counts and visual correctness separately.
Keep useful throughput improvements even if a separate compositor issue
survives, but do not claim those improvements fixed that issue.
