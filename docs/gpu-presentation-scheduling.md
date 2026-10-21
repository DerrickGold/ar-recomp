# Presentation scheduling investigation

Measured 2026-10-19, after the [presentation timing](gpu-presentation-timing.md)
and [drawable-pool experiments](gpu-presentation-queue-experiments.md).

The current loop selects and renders content before waiting for a drawable.
An isolated prototype moves that wait before content selection. Reversing the
order repeatedly within a process removes approximately **one refresh of content
age** on this 60 Hz Mac. It does not remove the remaining compositor delay or
establish an improvement in rare display hitches. No scheduling default changes.

The retained repository changes are diagnostic instrumentation, correlation
tests and this report. Application experiments, executable copies and patches
are archived under `runs/gpu-present-scheduling-20261019/`. The application source
was restored and the release executable rebuilt after the experiments.

## Conditions and measurement boundaries

Apple M2, SDL 3.4.18, Metal, windowed VSync, 1920×1080 backing pixels in a
960×540 logical window on a 60 Hz display. The production two-drawable policy,
one allowed GPU frame in flight, native preparation worker, direct GPU layers,
resident effects and interpolation remain enabled. Camera-motion-bound and actor
rejection experiments remain disabled. Settings, saves, replay input and assets
are isolated from everyday play. CRT is disabled; other configured effects remain
enabled. No GPU benchmarks or builds ran concurrently with a replay.

Display intervals use the drawable's OS-reported `presentedTime`, not callback
delivery time or the duration of the present API. Selected captures are sampled
foreground/unlocked, have no diagnostic overflow, and have complete successful
command completion and unique nonzero display timestamps. Startup, loading and
shutdown are excluded. These are Mac 60 Hz measurements, not Deck 90 Hz results.

“Content age” below is OS display time minus the interpolation timeline selected
by the game, clamped to its available source pair. It is derived from the recorded
playout target, source timestamp and source interval. This measures the age of
the selected game timeline; it is **not input-to-photon latency**, nor a guarantee
that every independently animated visual element has that age. The upper-phase
clamp differs by less than 0.002 ms from the reported calculation. Endpoint age
is recorded separately; using only the newer endpoint would understate the age
of an interpolated image. WRAM equality checks game execution, not visual parity.

## The actual presentation request is prompt

`AR_METAL_TIMING_SCHEDULING=1` now records the scheduled-handler callback, the
actual public drawable presentation call, and thread CPU time inside drawable
acquisition. Native drawable identity prevents recycled pointers from merging
different frames. Nested `present`/timed-present calls count as one request.

Two observer-only 4200-tick captures select ticks 1600–4000:

| Scene | Actual drawable request after command commit, mean | p99 | Display interval p99 | Long display gaps |
| --- | ---: | ---: | ---: | ---: |
| Fillmore scrolling | 0.068 ms | 0.269 ms | 16.667 ms | Two at 33.33 ms |
| Marahna scrolling | 0.055 ms | 0.092 ms | 16.667 ms | One at 33.33 ms |

At Fillmore's two long gaps, the actual requests arrive 0.043 and 0.134 ms after
commit. The GPU finishes 47.90 and 48.80 ms before those images are displayed.
Delayed delivery of Metal's presentation request is therefore not the cause of
these particular hitches. The delay is downstream of that request and GPU work.

Fillmore's drawable wait uses only 0.024 ms of acquiring-thread CPU time on
average while its wall-clock wait is about 15 ms. That thread is largely blocked,
not spending that interval executing rendering math. Scheduled-handler delivery
is a CPU observation and must not be substituted for a GPU scheduling timestamp.

## Display-link pilot

A disposable observer variant replaces `nextDrawable` with drawables delivered
by `CAMetalDisplayLink` on a dedicated run-loop thread. It requests 60 Hz and
`preferredFrameLatency=1`, and retains at most one unconsumed drawable. The game,
source clock, rendering code and ordinary `presentDrawable:` call are unchanged.
This is an acquisition adapter, not a complete display-link-driven game loop.

In the 899-frame settled Fillmore sample, display intervals are regular at
16.667 ms, but present-return-to-display delay averages **49.929 ms**. The link
delivers a CPU target about 16.6 ms ahead and a presentation target about 49.9 ms
ahead. Actual display is approximately 0.068 ms after the advertised presentation
target. This pilot therefore does not demonstrate a latency improvement over
the existing path, which can settle near 32 ms or 49 ms across launches.

Apple documents that [preferred frame latency](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/preferredframelatency)
accepts 1 or 2 frames, and that final latency can be larger in macOS windowed
mode. A separate attempted explicit `presentAtTime:` on a display-link drawable
was rejected by QuartzCore with `CAMetalDrawableInvalidOperation`; it produced
no valid timing capture. Do not use that variant. The regular `presentDrawable:`
pilot exited normally. Neither method interception nor this single-window
diagnostic adapter is a shipping integration.

## Wait before selecting content

The second prototype changes only interpolated streaming on Metal/VSync:

```text
Current:     select source/phase → prepare + render → wait for drawable → copy/present
Prototype:   wait for drawable → select source/phase → prepare + render → copy/present
```

Simulation remains on its existing producer thread. Its timing, interpolation
delay, source-rate policy, shaders and drawable-pool size do not change. The
terminal command is reserved on the renderer owner thread, held within one
stream-loop iteration, and submitted after the offscreen writes. The experiment
explicitly retires an unexpected unconsumed reservation; no such event occurred
in accepted captures. It does not hold a reservation across event handling.

Separate 3200-tick Fillmore launches first showed a large content-age reduction,
but their compositor delays differed. To avoid attributing that launch effect
to scheduling, a 5100-tick executable alternates late/early acquisition every
600 eligible stream iterations. Analysis removes 1.25 seconds on each side of
every switch and excludes the initial phase.

The last complete late→early pair in each alternating run gives:

| Capture | Late/early samples | Content age, late | Content age, early | Present API return → display, late / early |
| --- | ---: | ---: | ---: | ---: |
| Fillmore | 449 / 451 | 77.162 ms | 60.593 ms | 31.928 / 31.082 ms |
| Marahna | 450 / 451 | 76.825 ms | 60.314 ms | 31.951 / 30.945 ms |
| Marahna repeat | 450 / 451 | 93.397 ms | 76.914 ms | 48.562 / 47.552 ms |

The approximately 16.5 ms content-age reduction survives both the 32 ms and
49 ms compositor-delay states. Most of the gain is fresher selection after the
wait, not a reduction in final presentation delay. Fillmore's earlier complete
phases also reverse consistently: early 60.408 → late 77.112 → early 60.596 →
late 77.162 → early 60.593 ms. Every settled phase has display-interval p99
16.667 ms. Full phase statistics and per-frame associations are retained in each
capture's `scheduling-summary.json` and `presentation.json`.

The two repeated 5100-tick Marahna runs have identical final WRAM. All three
3200-tick Fillmore early/control/repeat launches also have identical final WRAM.
These checks do not replace image comparisons or input-latency measurements.

## Tail behavior remains unresolved

The selected Fillmore alternating capture has no interval over 25 ms. The first
Marahna alternating capture has six: five around 33.33 ms and one around 100 ms.
The repeat has two around 33.33 ms and does not reproduce the 100 ms event.
Do not exclude those events from the result or claim that the new order fixes
pacing based on its unchanged p99.

At Marahna tick 2188 in the first capture, upload preparation takes 25.9 ms and
draw work takes 47.3 ms wall time, while measured draw-thread CPU time is only
0.23 ms. This is evidence of blocking/descheduling in preparation, not 47 ms of
CPU rendering arithmetic. The current scopes do not identify the blocked stack
or prove whether the cause is a GPU-resource dependency, driver scheduling or
system scheduling. It did not recur at that tick in the repeat.

Other gaps remain downstream: the repeat's next image finishes on the GPU 3.56
and 6.90 ms before the preceding image is displayed, yet the subsequent display
interval is 33.33 ms. Moving the acquisition wait cannot by itself account for
those gaps. The observations do not establish that the earlier order worsens
or improves rare-hitch frequency; longer controlled captures are needed.

## Production direction and gates

1. Split acquisition from terminal submission with an explicit frame lifetime
   owned by the presentation backend. Select the game timeline and interpolation
   phase after the pacing gate. Preserve the fixed simulation clock and existing
   producer buffer. The prototype's global reservation and environment switch
   are experiment scaffolding, not production interfaces.
2. Measure and preserve useful overlap for expensive endpoint preparation. Moving
   *all* preparation behind the wait can reduce deadline slack. A follow-up should
   distinguish reusable endpoint/motion preparation from the final phase-dependent
   draw, and instrument the preparation wait that produced the large Marahna tail.
   Earlier acquisition is a demonstrated content-freshness improvement here,
   not demonstrated extra GPU throughput.
3. Validate native, non-interpolated playback separately. Its current fixed
   content clock is based on the last completed output slot; this prototype
   intentionally does not change that policy. Include held/skipped source-frame
   counts, fractional source/display rates and transitions in that validation.
4. Exercise resize, hide/restore, fullscreen, display changes, acquisition failure
   and shutdown before enabling a default. Then repeat with Vulkan/DX12 and
   Steam Deck at 90 Hz. Do not copy Mac compositor assumptions or drawable counts
   into those backends. The present results validate neither their pacing nor
   their acquisition semantics.

A Mac Metal System Trace would help distinguish the remaining driver/compositor
and preparation waits. This machine has Command Line Tools, but no available
`xctrace` installation, so that attribution remains open. No system permissions
or persistent display settings were changed to work around it.

## Evidence and reproduction

Ignored evidence root: `runs/gpu-present-scheduling-20261019/`.

- `observe-fillmore`, `observe-marahna`: new observer on the frozen reference.
- `link-pilot`: regular display-link acquisition. `link-timed` is the rejected
  timed-present attempt, not a valid comparison.
- `early-pilot`, `early-control-a`, `early-repeat`: initial separate-launch
  controls; these demonstrate why a within-process reversal was necessary.
- `early-cycle-fillmore`, `early-cycle-marahna`, `early-cycle-marahna-repeat`:
  repeated ordering reversals, raw traces and phase summaries.
- `early-cycle.patch`, `prototype-*.c`, `prototype-render_sdl.h`, `game-early-cycle`:
  archived application experiment, never applied by the normal build.
- `profile-display-link.m`/`.dylib`: acquisition experiment; the same library
  with `AR_METAL_DISPLAY_LINK=0` observes the early/late experiment without
  creating a display link. Do not select its historical invalid mode 2.
- `analyze-early.py`: isolated analyzer extension for the early-acquisition
  scope and intervening owner-thread submissions. The normal tracked analyzer
  still enforces the current production terminal-acquisition contract.
- `summarize-scheduling.py`: phase filtering, interpolation-target age and gap
  statistics. `run-mac.py` isolates settings/saves and records binary, observer,
  replay, environment and final-WRAM hashes per run.

The frozen reference SHA-256 is
`806889afbc19355a280c864ca620cca836fb87d322e47e0ec3f3ea12ecc4a8f7`.
The alternating-order executable is
`296dec85392bb91e66729a0afff04f30b3fba7e5c99bdd2ab687b26bc15abe06`;
all three alternating runs use observer
`da171d81c8414c591c46fbe983e9b21d6cde09af5c28c7a1268cc983ccd44669`.

```sh
python3 runs/gpu-present-scheduling-20261019/run-mac.py NEW_NAME 0101 VSync 1 \
  runs/gpu-present-scheduling-20261019/early-cycle.json
PYTHONPATH=tools python3 runs/gpu-present-scheduling-20261019/analyze-early.py \
  runs/gpu-present-scheduling-20261019/NEW_NAME/metal.csv \
  runs/gpu-present-scheduling-20261019/NEW_NAME/pacing.csv \
  --start 1600 --end 5000 \
  --output runs/gpu-present-scheduling-20261019/NEW_NAME/presentation.json
python3 runs/gpu-present-scheduling-20261019/summarize-scheduling.py NEW_NAME
```

Use `0501` for Marahna. These commands require the archived executable/library,
local replay assets and a usable Metal window session. The configuration points
to the isolated executable, not the restored release binary.

Validation: strict-warning observer compilation; 14 Metal-correlation tests
including recycled drawable addresses, nested requests, missing callbacks and
unavailable CPU clocks; successful prototype and restored release builds; and
clean `git diff --check`. A final observer smoke replay validates the restored
game with the finalized callback-safe hook lookup and CPU-clock failure handling;
all selected requests, scheduled callbacks and CPU samples are available.
No runtime tests were performed on Windows or Deck
for this scheduling experiment. No commit or push was made.
