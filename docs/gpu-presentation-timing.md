# GPU execution and presentation timing

Measured 2026-10-19. The Mac captures locate the main owner-thread wait inside
Core Animation's `nextDrawable`, and show a substantial delay between the
terminal presentation submission and OS-reported display. They do not establish
a whole-scene GPU bottleneck or a Steam Deck 90 Hz limit.

The initial investigation leaves production presentation ordering, queue depths
and defaults unchanged. The startup preparation follow-up below moves remaining
pipeline creation into boot; it does not change frame scheduling. The Metal
callback probe is a separately loaded diagnostic library, never linked into the
application.

## Results

Apple M2, SDL 3.4.18, Metal, windowed, **1920×1080 backing pixels** (960×540
logical window points on this Retina display). Interpolation, environmental
effects, lighting, particles, rim lighting, edge AA and DOF enabled; CRT disabled.
Both `AR_GPU_DIRECT_LAYERS` and `AR_GPU_EARLY_SUBMIT` disabled. The OS-reported
presentation timestamps showed a 60 Hz cadence in every measured case.

Each capture ran to game tick 2600; the table selects ticks 1600–2500 after the
room and streamed renderer were established. Marahna uses the scrolling replay;
Bloodpool is a stationary effects-heavy scene. Each row has 899–900 presented
frames over about 15 seconds, with no missing display callbacks or zero
presentation timestamps in that interval. All sampled foreground/session states
were active and unlocked. The two Marahna capped repetitions agree closely.

| Measurement, ms | Marahna, cap 90 (two runs) | Marahna, VSync | Bloodpool, cap 90 |
| --- | ---: | ---: | ---: |
| CPU upload, mean | 0.76–0.79 | 0.85 | 1.20 |
| CPU draw preparation, mean | 0.154–0.155 | 0.153 | 1.044 |
| CPU offscreen flush, mean | 0.016 | 0.016 | 0.017 |
| CPU swapchain acquisition, mean | 15.65–15.68 | 15.60 | 14.31 |
| Of that: `nextDrawable`, mean | 15.65–15.67 | 15.59 | 14.30 |
| CPU blit encoding + terminal submit, mean | 0.038–0.039 | 0.035 | 0.073 |
| Terminal commit → GPU start, mean | 15.79–15.81 | 0.21 | 15.80 |
| Terminal GPU command duration, mean | 0.264–0.278 | 0.238 | 0.335 |
| Terminal GPU end → OS display, mean | 32.65–32.68 | 48.25 | 32.56 |
| Present API return → OS display, mean | 48.71–48.72 | 48.67 | 48.64 |
| CPU present-return interval, p99 | 17.13–17.16 | 17.00 | 17.15 |
| OS display interval, p99 | 16.667 | 16.667 | 16.667 |

Do not sum this table: CPU and GPU work overlap, the acquisition rows are
nested, and the post-submission rows describe the same asynchronous timeline.
The terminal command is the final output-to-swapchain blit, **not all scene
rendering**. Its short duration does not prove every effect shader is cheap.
The approximately 49 ms is a measured present-return-to-display delay, not
input-to-photon latency or an estimate derived from buffer count.

In the capped runs the layer reported `displaySyncEnabled=0`,
`maximumDrawableCount=3`, and `presentsWithTransaction=0`. VSync changed
`displaySyncEnabled` to 1. Our VSync policy also reduces SDL's allowed frames
in flight from two to one, so this is a policy comparison, not an experiment
isolating either setting. It moved roughly one refresh period from the
commit-to-GPU-start interval to the GPU-end-to-display interval without reducing
total presentation delay.

SDL's [Metal implementation at release 3.4.18](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/gpu/metal/SDL_gpu_metal.m)
waits for its in-flight fence before requesting `nextDrawable`. The separate
callback trace shows that only about 0.002–0.006 ms of the mean acquisition
scope was outside `nextDrawable` here. This identifies where the CPU blocks;
it does not establish which compositor/resource dependency ultimately releases
that drawable.

## Measurement safeguards and limitations

- A run without the injected library still averaged 16.667 ms between present
  returns, with p99 17.23 ms. A separate run with only command-completion timing
  and no drawable presentation callbacks also retained the 60 Hz ceiling.
  These are observer-overhead controls, not independent display measurements.
- An initial probe queried the macOS session synchronously on the render thread
  every 60 acquisitions. That query caused periodic millisecond stalls and
  inflated CPU p99 to 22.33 ms. Those pilot p99 values are excluded. Session
  polling now runs on a utility queue; render-thread sampling only reads cached
  flags and application activity. The OS display cadence in that pilot also
  remained 60 Hz, demonstrating why API-return p99 alone is insufficient.
- The initial oversized window produced 3420×2082 backing pixels, despite a
  request for 1920×1080 logical points. It is excluded from this table.
- Command buffers overlap and may contain dependency stalls. Summing their GPU
  intervals overcounts time; their interval union is only coverage, **not GPU
  engine utilization**. Even the near-100% capped-run coverage cannot establish
  that shader execution saturates the GPU.
- Apple exposes command [GPU start](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime)
  and [GPU end](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpuendtime)
  after completion. We correlate their host clock with SDL's nanosecond clock.
  Recorded clock-offset spread in the final runs was below one microsecond.
  Callback delivery time is retained separately and is never substituted for
  GPU execution or display time.
- A drawable's [presented time](https://developer.apple.com/documentation/metal/mtldrawable/presentedtime)
  is OS-reported presentation, not a photodiode measurement. Zero timestamps and
  missing callbacks are reported separately. We do not prove distinct *content*
  on every refresh from drawable identities alone, or eliminate interpolation
  artifacts from these measurements.
- Active/unlocked state is sampled, not continuous. Only one display setup,
  two scenes and short steady intervals were tested. This does not validate
  90 Hz on Deck, long-session p99, or DX12/Vulkan presentation.
- Windows PresentMon is staged, but the SSH session lacks ETW capture privileges.
  No permissions were changed; Windows capture is deferred at the user's request.

## Follow-up: output reuse and drawable pool

The subsequent [controlled queue experiments](gpu-presentation-queue-experiments.md)
reject extra application output textures as a latency fix on this Mac, and
identify the Metal drawable-pool size as a useful presentation-latency control.
That investigation also found that immediate-mode presentation callbacks can
report updates faster than the monitor refresh; those callbacks must not be
called physical full-screen scanouts.

## Original next experiment

Prioritize output-resource lifetime and drawable backpressure over further
reductions to the final blit. Trace reuse of the offscreen output texture
against terminal completion, and test a bounded scheduling/resource-lifetime
change against this same display timeline. Require improvements in both actual
display cadence and presentation delay, with pixel parity and native-mode
cadence checks, before changing defaults. Do not infer success from a shorter
CPU acquisition scope or a higher submission rate alone.

Keep the earlier early-submit and direct-layer prototypes separate. These
captures are a baseline, not validation of either prototype. More precise
per-pass GPU counters or Xcode Metal System Trace are still needed to divide
the entire scene's GPU cost into shader work, bandwidth and dependencies.

## Reproducing the diagnostic

Build with the platform SDK and the same SDL3 library used by the game. For
Homebrew on Apple Silicon:

```sh
clang -dynamiclib -O2 -g -std=c11 -fobjc-arc -Wall -Wextra -Werror \
  -I/opt/homebrew/include tools/profile_metal.m -L/opt/homebrew/lib -lSDL3 \
  -framework Foundation -framework Metal -framework QuartzCore \
  -framework AppKit -framework ApplicationServices -o /tmp/ar-profile-metal.dylib
```

Run a local development binary with absolute trace paths, while the desktop is
unlocked and the game is visible. Keep replay, resolution and settings fixed
between captures; isolate save/config paths from everyday play. Prefix the
normal replay command with:

```sh
env DYLD_INSERT_LIBRARIES=/tmp/ar-profile-metal.dylib \
  AR_METAL_TIMING_TRACE=/tmp/ar-metal.csv \
  AR_FRAME_PACING_TRACE=/tmp/ar-pacing.csv \
  SDL_GPU_DRIVER=metal ./build-release/ActRaiserRecomp /path/to/rom.sfc
```

The probe intercepts public `commit` and `nextDrawable` methods on the active
Metal classes. It appends bounded in-memory records, adds asynchronous completion
and presentation handlers, and writes CSV at process teardown. It does not
change the drawable pool, command ordering, scheduling, or add GPU waits or
readbacks. It is intended for a disposable development process, not shipping or
in-process unloading. Check the exit summary for zero overflow and for complete
records; late teardown callbacks are not waited on. Analyze a settled interior
interval, not the final draining frames.

```sh
python3 tools/analyze_metal_presentation.py /tmp/ar-metal.csv /tmp/ar-pacing.csv \
  --start 1600 --end 2500 --output /tmp/ar-presentation.json
```

The analyzer requires one drawable acquisition and one matching owner-thread
terminal submission per selected present, successful GPU completion, consistent
CPU scopes and a stable clock mapping. Ambiguous associations fail rather than
invent a latency. `timing_eligible` records sampled active/unlocked status;
inspect missing/zero presentation counts separately. Set
`AR_METAL_TIMING_DISPLAY=0` for the callback-overhead control. Remove
`DYLD_INSERT_LIBRARIES` entirely for the CPU-only control.

`AR_METAL_TIMING_RESOURCES=1` additionally records actual Metal color attachments,
fragment samplers and presented surfaces. The analyzer resolves command/encoder
address reuse and checks that the terminal blit samples the preceding output
writer. This distinguishes a single SDL texture container from cycling native
backings. It reports output backing counts and CPU reuse overlap with the
previous read. Overlap is legal and ordered by Metal; it is not evidence of a
data race. Writer timing covers the whole command, not just its output pass.
The resource trace expands the bounded record pool from 262,144 to 1,048,576
entries. Compare runs with the same observer configuration, check overflow,
and use the smaller timing-only probe as an overhead control.

`AR_METAL_TIMING_SCHEDULING=1` adds command scheduled-handler delivery,
actual drawable `present`/timed-present calls, native drawable identities, and
the acquiring thread's CPU time. This distinguishes a late presentation request
from a prompt request that waits downstream. Scheduled-handler timestamps measure
CPU callback delivery, not GPU scheduling. The analyzer combines nested public
present calls, rejects separate requests for one drawable, and reports missing
callbacks, requests and unavailable thread-CPU samples rather than inventing zero
cost. Its `scheduling.frames` and `scheduling.metrics` are written to the JSON
report. See the [presentation scheduling experiment](gpu-presentation-scheduling.md)
for the resulting measurements and isolated policy prototypes.

The report counts duplicate OS display timestamps explicitly. When native-frame
rows have unique nonzero timestamps it also evaluates source-frame cadence at
OS-reported display, using the same phase-fitting method as the CPU analyzer.
Source age at display is a timestamp-age measurement, not input-to-photon latency.

## Shader creation versus presentation stalls

`AR_METAL_TIMING_PIPELINES=1` additionally times the four synchronous Metal
library/pipeline creation entrypoints used by SDL 3.4.18, including SDL's
internally cached render pipelines, and records render/compute pipeline binds.
This expands the bounded event pool to 1,048,576 entries. No GPU waits or
readbacks are added by this observer. The probe targets the default Metal
device; the report rejects unscoped devices and reports unmatched bindings.
It matches bindings to creation generations, so a recycled native pointer does
not hide a new pipeline's first use. A `pipeline_scope` marker distinguishes a
capture with no creations in the selected interval from one without tracing.

These are **API wall times, not driver cache-miss or compiler-busy counters**.
Creating a shader or pipeline can hit the driver cache; its first binding does
not imply compilation. Hidden driver work cannot be excluded by this probe
alone. Existing captures without this tracing cannot retrospectively rule out
compilation around their CPU stalls.

Two 4,200-tick Marahna scrolling captures used the same frozen reference binary,
replay, settings and observer, with a fresh process for each. VSync was enabled;
the display remained at 60 Hz. These shader-audit runs use the production
direct-layer path, keep early submission disabled, and disable the experimental
camera-bound/stationary-actor rejection shortcuts. Driver caches were **not** cleared, so the first
run is not a controlled cold-cache measurement. Both runs ended with the same
WRAM hash. In ticks 1,600–4,000, both had complete native binding coverage, zero
new libraries/pipelines, zero first bindings, no binding call above 1 ms, zero
missing display callbacks and sampled foreground/unlocked status throughout.

| Reference run | Game-wrapper preparation total | Long OS display gaps in selected interval | Shader/pipeline creations in interval |
| --- | ---: | --- | ---: |
| First process | 508.2 ms | 33.33 ms at tick 2166 | 0 |
| Warm repeat | 14.1 ms | 50.00 ms at tick 3247; 33.33 ms at tick 3307 | 0 |

At all three gaps, the next image's terminal GPU command had finished
14.7–14.8 ms **before the previous image's display timestamp**. These observed
gaps are downstream of image rendering, rather than waiting for a new shader
or its first draw. This does not explain every earlier hitch or prove that all
compositor stalls share one cause.

## Complete startup pipeline preparation

The audit nevertheless found boot coverage gaps: interpolation was prepared
only when enabled in settings, custom fragment warmup covered only four blend
modes, the PPU fragment fallback did not warm its drawing pipelines, and SDL's
built-in RGB/RGBA/solid drawing and GPU blit variants could be created on room
entry. A cached shader object does not cover every render-pipeline combination.

Full boot and device-reset preparation now:

- Prepares all current supported game shader families, including interpolation
  when disabled, without allocating full-size motion/capture textures. The
  existing preparation includes action effects, lighting, PPU decode, SIM/world
  geometry, clouds, shadows, CRT and ending materials.
- Exercises custom fragment shaders with all ten portable blend modes against
  ARGB8888 and ABGR8888 scratch targets, including the PPU fragment fallback.
- Exercises SDL's built-in RGB, RGBA and untextured triangle combinations, plus
  its separate GPU blit cache, for both target formats. These scratch draws are
  submitted without presenting their pixels or changing the caller's target,
  viewport or clip. Material uniforms and texture bindings do not introduce
  new pipeline variants.
- Uses a portable `ArRenderDevice_FinishPreparation` boundary. The SDL GPU
  backend waits for queued preparation to finish before gameplay can start;
  software/synchronous backends can complete without a GPU wait. A failed
  finish fails startup preparation. This wait is never added to normal frames.

Process-static diagnostic overrides still exclude unreachable GPU paths. Device
reset repeats preparation for the new device; ordinary settings changes reuse
the prepared pipelines. The game log records exact boot start/end timestamps
for correlation with the native trace.

| Prepared capture | Interpolation | Boot preparation | Native creation calls after boot |
| --- | --- | ---: | ---: |
| Marahna, 2,600 ticks | On | 94 ms | 0 |
| Fillmore, 3,200 ticks | On | 35 ms | 0 |
| Marahna, 3,200 ticks | Off | 23 ms | 0 |

Each capture observed all 363 successful native creation calls before boot
completed: 52 source libraries, 6 binary libraries, 291 render pipelines and
14 compute pipelines. There were no unmatched bindings or event overflows.
Later first bindings reused those pipelines; no binding exceeded 1 ms. These
are cache-populated Mac startup measurements, not cold-cache promises or
cross-platform timings. The interpolation-off capture still had a 33.33 ms
display gap at tick 1848, with its next image ready 15.05 ms before the previous
display, independently confirming that startup preparation does not resolve
all presentation stalls.

Validation: 19 analyzer tests, three portable/render-ordering tests and four
native Metal tests pass. The native tests cover repeated preparation preserving
target pixels/viewport/clip, both PPU implementations, and SIM depth rendering.
The release game and editor WebAssembly compositor build successfully. The
production changes use portable C and SDL GPU APIs; new runtime validation on
DX12, Vulkan and Steam Deck remains outstanding.

Evidence is under ignored `runs/gpu-shader-stalls-20261019/`: frozen binaries,
isolated replay harness/configurations, logs, raw Metal/pacing traces,
`presentation.json` reports and `after-boot.json` reports. The reference binary
SHA-256 is `d60b5dcaf4d48bba83082f221ba04627e17fac39a028febc77233cfa63d1fae2`;
the prepared binary is
`6c65fd7e8f998eb22a72b5b756e3b87c32c7c2c7623bd665d8bb6f779ed0f6a5`.
The failed `prepared-marahna` pilot is excluded: its temporary render-device
facade omitted the scoped-target capability and stopped during startup. The
corrected run is `prepared-marahna-fixed`.

## Evidence and validation

Local ignored evidence is in `runs/gpu-present-timing-20261019/`, including
`summary.json`, `source-manifest.json`, the isolated replay harness, per-run
settings/environment/binary hashes, raw CPU/Metal traces and game logs.
Accepted captures are:

- `marahna-1080-cap-final-1`
- `marahna-1080-cap-final-2`
- `marahna-1080-vsync-final`
- `bloodpool-1080-cap-final`

All use game executable SHA-256
`10b5074744227bc2de8e3533aa693f18b67ad189ee146ddace415970222b5b72`.
The three Marahna captures and the uninstrumented control have identical final
WRAM hashes. The newer probe's post-capture changes initialize atomic record
flags explicitly and handle inherited command methods; its separately named
verification binary is not substituted for the captured binary.

Validation includes the release build, strict-warning probe compilation,
CPU presentation ordering/scope tests, pacing analyzer tests, Metal correlation
tests, and pixel-exact real-Metal normal/prepared presentation checks. These
validate instrumentation and presentation integrity; they are not a new visual
parity audit of the entire game.

## Windows observation without privileged tracing

On 2026-10-19, the existing staged Windows build completed a 3,200-tick Marahna
replay using DX12, 1920x1080, VSync and interpolation without PresentMon or
administrator privileges. All 49 foreground checks passed. This executable
predates the complete startup-preparation work above; this is an observability
check, not Windows validation of that change or the new acquisition-scheduling
prototype. The test used isolated settings and saves.

The settled ticks 1,600–3,100 contain 1,479 presentation returns:

| CPU wall-time measurement | Mean | p99 | Maximum |
| --- | ---: | ---: | ---: |
| Interval between presentation API returns | 16.880 ms | 31.756 ms | 32.114 ms |
| Backend acquisition stage | 14.353 ms | 29.300 ms | 31.382 ms |
| Final backend submission stage | 0.368 ms | 0.815 ms | 1.731 ms |
| Draw preparation | 0.225 ms | 0.295 ms | 3.617 ms |
| Upload preparation | 1.982 ms | 3.755 ms | 8.243 ms |
| Producer work | 8.517 ms | 9.123 ms | 9.805 ms |

There were 117 return intervals above 30 ms. These establish irregular
application-side pacing, not 117 missed display refreshes: this capture has
neither GPU execution timestamps nor actual display feedback. The user later
confirmed that the Windows display was off or its desktop locked during this
investigation. Foreground-PID checks alone do not establish an active visible
desktop. This run therefore cannot establish visible-gameplay pacing or justify
a shipping performance change. Its coarse acquisition scope also predates
separate command-buffer/swapchain timing.
VSync acquisition normally waits, so the mean acquisition time is not work
that can simply be removed to recover throughput. Producer work overlaps
presentation; the rows must not be summed into a frame budget.

Two Vulkan runs completed rendering with the same final WRAM hash and no
reported errors, but both failed every foreground check (43/43 each). Their
timings are excluded. This was a focus limitation, not an ETW permission error
or a game execution failure.

The next attribution steps are to split acquisition into lock, GPU-fence and
cleanup intervals, then correlate submitted frames with asynchronous GPU
timestamps and DXGI flip-model frame statistics. SDL 3.4.18's DX12 acquisition
path waits on an in-flight fence; its fence-wait implementation holds the
submission lock through waiting and completed-command cleanup. That identifies
places to instrument, not a proven cause. DXGI statistics can report late
refreshes without privileged ETW; unsupported, stale and disjoint observations
must be excluded. Do not add a synchronous readback or compositor flush to
obtain timing samples.

Evidence is under ignored `runs/windows-unprivileged-stalls-20261019/`, including
raw CPU traces, replay plans, environment metadata, foreground checks and
`summary.json`. Executable SHA-256:
`777b3db8d804c3693988727aaca32957b5db6133d2efd049d257a42594217ef7`.


## Isolating the presentation stalls

The 2026-10-19 follow-up uses an isolated SDL 3.4.18 DX12 diagnostic build and
Mac clear-only controls. No production presentation policy is changed by these
experiments. Raw evidence, diagnostic sources and executable hashes are under
ignored `runs/gpu-stall-attribution-20261019/`.

### Windows: display state invalidates the apparent regression

The DX12 observer separates lock acquisition, fence-event setup, the event wait,
completed-command cleanup, queue submission and `Present`. Optional GPU timestamp
queries are read only when the existing command fence has already completed;
there is no additional readback wait. DXGI frame statistics and present counts
are captured without ETW, administrator privileges or `DwmFlush`.

The original, isolated `dx12-observe-a` capture's selected interval has advancing
DXGI statistics and regular application returns: maximum return interval
19.03 ms, fence-wait p99 14.71 ms, lock-wait p99 0.0013 ms and cleanup p99
0.0565 ms. Its terminal GPU copies take approximately 0.03 ms. The CPU is waiting
for queued work to become eligible, rather than executing 14 ms of copy work.

However, the longer no-query control reports successful DXGI calls with **zero
statistics throughout**. Another run initially advances and then freezes its
statistics; that run also loses foreground focus. Those periods exhibit roughly
15.6/31.2 ms application return intervals. A foreground PID can still match while
the monitor is off or the desktop is locked. The user confirmed this display
condition, so these runs cannot compare visible pacing, query overhead or the
experimental DXGI frame-latency waitable object. Neither successful `Present`
returns nor successful-but-empty/stale statistics validate actual display.

An additional power-notification observer registered successfully but received
no state notification; its state remains **unknown**, not "on". The media
statistics interface is unsupported on this swapchain. Do not infer the exact
monitor/lock state from either absence. Further Windows timing tests require an
on, unlocked display with the game visible, advancing display statistics and
foreground validation. The normal installation and persistent system settings
were not changed.

### Mac: the display gaps also occur without the game or SDL GPU

Two controls each submit 6,000 frames, at the same 960x540 logical / 1920x1080
backing resolution, VSync and two Metal drawables. They only clear a color;
there is no game, shader creation, interpolation, texture upload, worker
preparation or environmental rendering. One uses SDL GPU; the other uses an
SDL window/event loop with native `MTLCommandQueue`/`CAMetalLayer` rendering and
presentation, bypassing the SDL GPU backend. Both wait for the preceding command
to complete, matching the one-frame SDL GPU fence limit.

| Clear-only control | Selected displayed intervals | GPU command mean / p99 | Gaps over 25 ms | Largest gap |
| --- | ---: | ---: | ---: | ---: |
| SDL GPU | 5,699 | 0.0230 / 0.0278 ms | 3 | 33.333 ms |
| Native Metal | 5,701 | 0.0232 / 0.0282 ms | 2 | 33.333 ms |

Selection excludes the first 299 acquisitions and zero display timestamps.
Every acquisition has a display callback; three SDL and two native startup/
teardown callbacks report zero presentation time and are not treated as displays.
There are no duplicate nonzero timestamps or ambiguous command/request matches.
All selected state samples report foreground, unlocked console use. Clock-offset
variation is under 0.0002 ms. Native requests and GPU completions use matching
command/drawable generations, not recycled object pointers alone.

For native drawable 3231, using the previous displayed image as time zero:

```text
-14.864 ms   Native presentation requested
-14.601 ms   This image's GPU clear is complete
  0.000 ms   Previous image displayed
 16.667 ms   No new image reported at this refresh
 33.333 ms   This image displayed
```

Both native gaps have their next image complete approximately 14.6 ms before the
previous image is displayed. The SDL gaps have 14.6–31.5 ms of the same margin.
This reproduces the game's downstream-gap signature without its workload or
SDL's GPU presentation implementation. It localizes this class of gap to native
presentation/display scheduling after rendering, rather than a lack of game CPU
or GPU headroom. It does **not** identify a private WindowServer/driver function,
prove that every hitch shares this cause, or establish an unavoidable macOS bug.
The timestamps are OS-reported presentation, not a physical scanout measurement.

Evidence: `mac-clear-control`, `mac-native-control`, `minimal-present.c`,
`minimal-native.m` and `analyze-clear.py` in the attribution evidence root. The
analysis keys distinguish absent callbacks from zero (not displayed) timestamps.
Neither control is linked into the game.

### Separating native preparation waits

`AR_METAL_TIMING_WAITS=1` extends the standalone observer with wall and calling-
thread CPU times for existing `waitUntilCompleted`, command-buffer creation and
render-encoder creation calls. It never adds a GPU wait. The JSON analyzer reports
these under `native_waits`; unavailable CPU samples stay explicit. Off-CPU time
includes blocking and descheduling, not just GPU dependency waits.

Optional `AR_METAL_TIMING_MEMORY=1` also enables wait observation and snapshots
`getrusage(RUSAGE_SELF)` around those calls. Calls lasting at least 1 ms record
fault and context-switch deltas. These counters cover the **entire process**;
they cannot attribute a fault to the measured thread by themselves. This heavier
diagnostic is disabled unless explicitly requested.

A 6,200-tick Marahna replay with the prepared game and the explicitly enabled
experimental preparation worker (`AR_GPU_PREPARE_WORKER=1`) caught a larger
hitch. This is not the shipping inline-preparation default:

- At tick 5267, upload/preparation spans 87.27 ms and a subsequent OS display
  interval reaches 66.67 ms.
- The preparation worker spends 40.43 ms inside native `commandBuffer`, with
  only 0.044 ms of thread CPU time. An earlier encoder creation takes 7.13 ms,
  with 0.039 ms CPU time.
- At the start of the 40.43 ms call, all previously submitted game commands
  have finished GPU execution. No game GPU command executes during the call.
  Therefore this is not 40 ms of game GPU arithmetic. Its thread CPU measurement
  also rules out 40 ms of CPU encoding work.
- The selected interval contains zero shader/library/pipeline creations, zero
  first pipeline bindings and no binding above 1 ms. All 363 native creation
  calls finish during startup. Native fence waits peak at 2.96 ms; their p99
  is 0.000208 ms. The ordinary approximately 15.5 ms acquisition waits are
  almost entirely `nextDrawable`, a separate presentation throttle.

Apple documents that [`commandBuffer`](https://developer.apple.com/documentation/metal/mtlcommandqueue/makecommandbuffer%28%29?language=objc)
can block when its queue has no free command buffers, and that the
[default queue](https://developer.apple.com/documentation/metal/mtldevice/makecommandqueue%28%29?language=objc)
permits 64 uncompleted commands. A long call alone is not proof of reaching that
limit. This capture has no submitted GPU backlog during the long call; it does
not justify increasing that limit or blaming shader compilation.

The memory-counter repeat has identical final WRAM and no command-buffer or
encoder call above 1 ms in the selected interval. Its three longer explicit
fence waits report zero process major/minor faults. Because the large allocation
hitch did not recur, this repeat neither proves nor disproves a memory-fault
cause for it. The larger preparation hitch must not be folded into the separate
ready-image display-gap explanation.

Evidence: `attribution-marahna-waits` and `attribution-marahna-memory` under
`runs/gpu-present-scheduling-20261019/`, with observer binaries/configurations
in the attribution root. Both use prepared executable SHA-256
`6c65fd7e8f998eb22a72b5b756e3b87c32c7c2c7623bd665d8bb6f779ed0f6a5`.
The selected intervals have complete display/GPU coverage, no buffer overflow,
and foreground/unlocked state throughout.


The diagnostic stack follow-up uses a separate ARM64-only observer copy, never
linked into the game. It inspects the calling thread's state and a bounded stack
only after command-buffer/encoder creation is already slow. It reads its own
process with Mach APIs; it neither suspends threads nor reads another process.
Symbol names are resolved at exit. A deliberate 30 ms sleep in the standalone
control verified that the sampler correctly reports `TH_STATE_WAITING` and the
`nanosleep` stack. That injected delay is absent from all game captures.

The 8,000-tick `attribution-marahna-watch` replay caught an 11.47 ms encoder
creation call on `NativeCaptureWorker`. CPU time is 0.0805 ms and process fault
deltas are zero. The sampled thread state is `TH_STATE_RUNNING` (runnable),
with this native stack:

```text
free
AGX::ResourceGroupUsage::reset
AGX::BlitDispatchContext::commit
-[AGXG14GFamilyBlitContext deferredEndEncoding]
-[AGXG14GFamilyCommandBuffer commitEncoder]
-[AGXG14GFamilyCommandBuffer renderCommandEncoderWithDescriptor:]
METAL_BeginRenderPass
ArGpuActionScenePass_Encode
ArGpuFrameHandoff_EncodePack
RunNativeCapture
NativeCaptureWorker
```

This sample locates the delay in driver-side encoder setup/deferred cleanup,
with the worker runnable at the sampled instant. It is not a shader compiler
stack or an explicit GPU fence wait. A single sampled stack cannot prove what
occupied the entire off-CPU interval, and this was not the earlier 40.43 ms
command-allocation event. Do not label it allocator lock contention, disk
paging or QoS starvation without further evidence. The process context-switch
counter is not specific to this worker. Native-request display gaps also remain
in this capture, independently of the preparation call.

The normal inline-preparation control, `attribution-marahna-owner`, completes
6,200 ticks with the same final WRAM as the two 6,200-tick worker captures. In
selected ticks 1,600–6,100, command-buffer creation peaks at 0.040 ms, encoder
creation at 1.145 ms and explicit command-completion waits at 0.738 ms. No large
preparation stall recurs. It still has two 33.333 ms display gaps, with next-image
GPU completion 14.64 and 14.80 ms before the previous image's display. Pipeline
creation/binding coverage is complete with zero creations or first bindings in
that interval, no event overflow, and foreground/unlocked status throughout.

This confirms that the ready-image display gaps do not require the experimental
worker. One quiet inline run does not establish a worker regression or prove
that inline preparation cannot stall. Preserve the current inline default while
investigating the worker's rare tail; do not promote it based solely on average
CPU savings. No renderer defaults were changed in this investigation.

A separate room-entry check (`attribution-worker-qos-ready`) measures requested
Darwin QoS with `pthread_get_qos_class_np`: the owner is user-interactive (33),
while `GPU preparation` requests default (21). The producer explicitly requests
user-interactive. The earlier short `attribution-worker-qos` check ended before
the worker was created and provides no worker-priority evidence. Requested QoS
does not reveal temporary effective-priority overrides; these values identify
a policy mismatch, not proof of scheduler starvation during a particular hitch.

The worker supplies work on which the render owner synchronously waits. Apple's
[task-priority guidance](https://developer.apple.com/library/archive/documentation/Performance/Conceptual/power_efficiency_guidelines_osx/PrioritizeWorkAtTheTaskLevel.html)
classifies immediate animation/UI work as user-interactive. Matching the worker's
requested class is a justified prototype to test before productionizing it.
The priority experiment changes only that named thread inside an isolated
observer; it does not change process-wide priority or persistent system settings.

The first matched-QoS prototype, `attribution-marahna-qos`, successfully changes
only `GPU preparation` from requested default (21) to user-interactive (33).
It completes 6,200 ticks with the same final WRAM as the preceding controls.
Selected ticks 1,600–6,100 contain 4,492 displayed images / 4,491 intervals;
command-buffer creation peaks at 0.0373 ms, encoder creation at 0.0570 ms, and
no display interval exceeds 16.6668 ms. Explicit completion waits peak at
2.342 ms. There are no shader creations, first pipeline bindings, trace
overflows or missing/duplicate display timestamps; foreground/unlocked state
is valid throughout.

This is a promising single-launch prototype, **not a proven rare-hitch fix**.
The runs are not randomized/repeated QoS reversals under controlled system load,
and the memory-counter worker control also avoided a large allocation hitch.
In particular, do not attribute the disappearance of native display gaps in
this launch to QoS: those gaps independently occur in a native clear-only loop.
The practical follow-up is a within-process priority comparison before enabling
worker preparation by default, keeping the native presentation investigation
separate. The isolated QoS source/library/configuration are
`profile-metal-qos.m`, `profile-metal-qos.dylib` and `marahna-qos.json`.

Validation for the additions in this follow-up: strict-warning compilation of
the passive Metal observer, 20 analyzer tests including missing CPU measurements
and process-fault associations, both clear-only controls, the recorded game
replays, and `git diff --check`. Stack observation and the QoS mutation remain
isolated diagnostics. No production rendering code, default policy, commit or
push was introduced by this follow-up.

### Mac priority reversals after excluding Windows display-off captures

Two further 8,000-tick Marahna replays alternate the preparation worker's
requested QoS every 16 seconds **within the same process**. The second launch
starts with the opposite priority. The executable, observer, replay and rendering
configuration are identical; both finish with identical WRAM. The observer
changes only the named worker's requested class and verifies every change. It
does not sample stacks or inject delays. Changes remain isolated diagnostics;
the normal inline-preparation default is unchanged.

Analysis selects ticks 1,600–7,900 and removes 0.5 seconds on both sides of each
priority boundary. The two captures together provide about 98.83 selected seconds
per priority, with 5,937 default-priority and 5,941 interactive-priority jobs.
All selected intervals have complete command/display coverage, foreground and
unlocked status, no overflow, and no shader creation or first pipeline binding.

| Reversal order | Owner waiting for worker, mean: default / interactive | Preparation job, mean: default / interactive | Display gaps over 25 ms: default / interactive |
| --- | ---: | ---: | ---: |
| Default first | 0.0298 / 0.0113 ms | 0.0707 / 0.0619 ms | 1 / 1 |
| Interactive first | 0.0324 / 0.0104 ms | 0.0718 / 0.0600 ms | 0 / 1 |

The small preparation improvement reverses with priority in both launches, but
it is approximately **0.02 ms of owner wait**, not a multi-millisecond pacing fix.
Neither priority reproduces the earlier large allocation/encoder hitch. This
does not establish that either prevents that rare event. All three display gaps
are about 33.333 ms; their next image finishes on the GPU 14.54–30.55 ms before
the preceding image is displayed. Two occur at interactive priority. Matching
worker priority therefore does not remove the separate ready-image display-gap
signature, and the earlier single all-interactive launch's zero gaps were not
sufficient evidence to claim otherwise.

Evidence: `attribution-marahna-cycle-default` and
`attribution-marahna-cycle-interactive` under the scheduling evidence root, each
with `prepare.csv`, native/pacing traces, `presentation.json`, `qos-analysis.json`
and hashed metadata. The attribution root contains `build-qos-cycle.py`,
`profile-metal-qos-cycle.m`/`.dylib`, the two JSON configurations and
`analyze-qos-cycle.py`. The experiment library SHA-256 is
`1f9bb55336bed5c7a7cc5f2cb5613250c3e5a8dd67c76ca6919f56cffaac90cb`.
No Windows measurements were added: monitor-off/locked-desktop results remain
excluded from visible-pacing comparisons.

### Independent display-clock observation

The next native clear-only control observes a `CVDisplayLink` tied to the window's
actual display. It records callback arrival and the OS-provided output timestamps
without using those callbacks to acquire, render, submit or present anything.
The existing native Metal loop is unchanged. This deprecated API is isolated in
the diagnostic; it is not a proposed production dependency. Its
[output timestamp](https://developer.apple.com/documentation/corevideo/cvdisplaylinkoutputcallback)
is a display-clock schedule, **not proof of physical scanout**.

`mac-native-clock` completes 6,000 clear-only frames. The selected interval has
5,701 displayed-image intervals, 5,705 display-clock callbacks, 95 valid foreground/
unlocked samples, no trace overflow, no missing display callbacks and no duplicate
nonzero display timestamps. The first two drawable timestamps are zero. CV and Metal
clock mappings agree within 53 ns in their median; the largest CV mapping spread
is 0.031 ms.

It catches one **66.666 ms** image-presentation gap. Relative to the preceding
image's OS-reported presentation:

| Time | Observation |
| ---: | --- |
| −15.268 ms | Next image's native presentation request starts; returns 0.0245 ms later |
| −15.162 ms | Its 0.021 ms GPU clear has finished |
| 0 ms | Preceding image is reported displayed |
| +16.667, +33.333, +50.000 ms | Display-clock output slots advance; no new drawable presentation is reported |
| +66.666 ms | The already-ready image is reported displayed |

The independent callbacks continue across the gap, arriving at −8.282, +10.188,
+27.016 and +42.803 ms for those four future output slots. Across the entire
selected interval, the output video clock advances exactly one refresh per
callback; callback-arrival intervals peak at 18.73 ms, not 66.7 ms. This rules out
a simultaneous process-wide callback pause as the explanation for this event.
It does not identify a private WindowServer/driver wait or prove which physical
refreshes reached the panel.

The event also separates the apparent acquisition stall from its cause: the
following `nextDrawable` waits 58.40 ms, although the delayed image's request and
GPU work had already completed. Mean request-to-display latency shifts from
31.69 ms in drawables 300–1,299 to 48.34 ms in 1,500–2,499 and remains near that
level later. The observed layer policy stays at two drawables, VSync on and
`presentsWithTransaction` off throughout. This is evidence of a changed downstream
presentation-delay state without an application drawable-policy change, not a
new rendering workload or shader compilation.

Evidence: `minimal-native-clock.m`, `display-clock.inc`, `run-native-clock.py`,
`analyze-display-clock.py`, and `mac-native-clock/{metal.csv,refresh.csv,analysis.json,
refresh-analysis.json,metadata.json}` in the attribution root. The native control
SHA-256 is `d784865507115866714c78052d7118b1555bd7b46a7f9d26a5a869aa5156227f`.

### Explicit presentation-target pilot

`mac-present-cycle` alternates ordinary `presentDrawable:` with
[`presentDrawable:atTime:`](https://developer.apple.com/documentation/metal/mtlcommandbuffer/present(_:attime:)?language=objc)
every 16 seconds in the same native clear-only process. Timed requests use the
latest independent display-clock output slot, advance stale/reused targets and
leave at least 1 ms before the requested time. This is an explicit requested
presentation time, not a guarantee that the compositor meets a deadline. Unlike
the earlier rejected display-link-drawable experiment, these are ordinary
`nextDrawable` drawables; Metal accepts the requests and the run exits normally.

The native observer adds the timed command-buffer entrypoint and correlates both
immediate and timed native requests with each drawable generation. Selection
removes 0.5 seconds on either side of policy changes and the first 299 drawables.
Every selected state sample is foreground/unlocked, with no overflow, missing
display callbacks or ambiguous request matches. Only the first drawable has a
zero display timestamp.

| Phase | Request policy | Request → display, mean | Display after requested target, mean |
| --- | --- | ---: | ---: |
| 0 | Immediate | 31.67 ms | — |
| 1 | Timed | 31.67 ms | 16.67 ms |
| 2 | Immediate | 48.11 ms | — |
| 3 | Timed | 46.34 ms | 31.33 ms |
| 4 | Immediate | 31.66 ms | — |
| 5 | Timed | 31.71 ms | 16.67 ms |

The partial final immediate phase is also approximately 31.68 ms. Explicit
targets do not consistently reverse the delay: timed phases occur in both delay
states, and returning to immediate presentation can retain the lower delay.
Phase 3 changes states during the phase, which is why its means lie between
refresh multiples. This pilot provides no basis for adding a production timed-
presentation policy. One 33.33 ms gap occurs shortly after the second policy
switch; a short switching experiment does not establish a rare-hitch rate or
show that timed presentation fixes the independently reproduced display gaps.

Evidence in the attribution root: `present-cycle.inc`, `minimal-present-cycle.m`,
`profile-present-cycle.m`/`.dylib`, `build-present-cycle.py`, `run-present-cycle.py`,
`analyze-clear-timed.py`, `analyze-present-cycle.py`, and `mac-present-cycle/`.
The control SHA-256 is `0c66a77017da4168d69b16a05fd2c3ad46c8e737b424a87c7f3533a8c807db06`;
observer SHA-256 is `3e36ae4018a3e4e429b27176dc1f557a1ca5ebc77bf4d6de9bc43f2813532f6c`.

The remaining attribution gap is the compositor's acceptance/latching of a ready
surface and the responsible WindowServer/driver scheduling or wait. Public
application callbacks identify the boundary, not that private dependency. The
earlier acquisition-before-selection prototype remains a measured content-age
improvement; these experiments do not turn it, extra GPU headroom, or a worker
priority change into a demonstrated fix for downstream presentation gaps.

Validation: both 8,000-tick priority reversals, both 6,000-frame native controls,
strict-warning compilation of the isolated Objective-C experiments, all 20 Metal
timing analyzer tests, and `git diff --check`. Only this report was changed in
tracked files during this continuation; experiment sources/data remain isolated
under ignored `runs/`. No production default, commit, push or persistent system
setting was changed.

## Steam Deck cross-check, October 20

The [Deck prototype validation](steam-deck-prototype-validation.md) separates
host API return intervals from Wayland presentation feedback. A clear-only
Vulkan control has host p99 12.102 ms but display p99 11.111 ms and no missed
refreshes over 5,089 intervals. Effect batching saves about 0.30 ms of owner CPU
time; both modes retain a display p99 of 11.111 ms. Two isolated ~22.22 ms gaps
and discarded submissions in one unbatched repeat remain in the analysis.
Camera-bound on/off feedback windows are also steady at 90 Hz.

This invalidates using a small host-return p99 excess alone as a GPU bottleneck
or a batching-regression diagnosis. It does not explain away this report's
measured Metal display gaps, identify the cause of every missed refresh, or
establish Game Mode behavior. Linux detailed scheduler accounting was disabled;
the experiment did not change system tracing or scheduling policy. Defaults
remain unchanged.
