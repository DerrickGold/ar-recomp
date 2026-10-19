# GPU execution and presentation timing

Measured 2026-10-19. The Mac captures locate the main owner-thread wait inside
Core Animation's `nextDrawable`, and show a substantial delay between the
terminal presentation submission and OS-reported display. They do not establish
a whole-scene GPU bottleneck or a Steam Deck 90 Hz limit.

Production presentation ordering, queue depths and defaults are unchanged.
Only opt-in CPU trace scopes were added to the game. The Metal callback probe
is a separately loaded diagnostic library, never linked into the application.

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

The report counts duplicate OS display timestamps explicitly. When native-frame
rows have unique nonzero timestamps it also evaluates source-frame cadence at
OS-reported display, using the same phase-fitting method as the CPU analyzer.
Source age at display is a timestamp-age measurement, not input-to-photon latency.

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
