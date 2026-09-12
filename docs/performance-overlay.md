# Performance overlay

In **Settings → Video → General**, set **Performance overlay** (next to the FPS
counter) to **Summary** or **Detailed**. It is off by default, changes live, and
persists with the other settings. This setting is independent of the 3D modes
and does not require developer settings. English, French, German and Japanese
setting labels/help are available; diagnostic stage names stay consistent with
the English run log.

Steam launch-option alternative: `AR_PERFORMANCE_OVERLAY=Detailed %command%`.
For logging without an on-screen panel, use `AR_PIPELINE_PERF=1`. Avoid adding
`AR_PERF` just to use this overlay: that separately enables the older, heavier
action coverage analysis.

## Reading it

- **FPS / ms / p95 / max** describe completed host presents and their intervals,
  not simulation speed or GPU execution. The interval p95 uses the latest 512
  intervals within the one-second sample; max covers the entire sample. Very
  fast output can exceed that percentile ring, without dropping stage totals.
- **Ticks / repeats** are counts per present. A steady 60-Hz simulation shown at
  120 Hz can have approximately 0.5 ticks and 0.5 re-presents. Headless replay
  measurements deliberately run one tick per present instead.
- **AVG** is CPU wall milliseconds per completed present, including driver/API
  blocking within that scope. **PEAK** is the longest individual invocation in
  that sample. Nested stages overlap: do not add PPU setup/scanout/finish to PPU
  total, or CRT/UI to presentation total. The SIM depth pass includes nested
  cull/mountain/projection/submit work in towns; the globe also records those
  stages outside that pass, so it is not a total of all globe depth work.
- **Present/wait** includes host submission and any wait inside the presentation
  API. **Pacing/sleep** is explicit host throttling/yielding. Neither is a GPU
  timestamp. The current portable renderer has no GPU timestamp query contract;
  **GPU execution: unavailable** is intentional, not zero GPU work.
- **Job owner / helpers / join wait** describe the render and town-pixel
  fork/join groups. Helpers are a sum of wall durations across parallel threads, not extra
  serial frame time. Jobs/helpers count dispatches/ranges, not CPU-core usage.
  Zero jobs in a quiet cached scene does not mean multicore support is disabled.
- **Scene batches/vertices/uploads** count the existing enhanced-scene and frame
  upload hooks. The two upload values are texture and depth-geometry bytes per
  present. They exclude host UI and uninstrumented backend-internal traffic;
  they are not total device draw counts or measured GPU bandwidth.
- **Scan MiB** counts bytes requested by exact change-detection comparisons,
  including both inputs and horizontal edge refinements. Skipped rows are not
  charged. This is not measured DRAM traffic: `memcmp` may stop at an early
  mismatch and the CPU may reuse cached lines. Before the conditional-bounds
  optimization, this counter charged the full image pair on every scan and
  omitted edge refinements; compare old/new logs with that distinction in mind.
- **Fallback / failed** count authentic-fallback selections and rejected host
  presents accumulated per successful present. Existing view-transition logs
  still carry fallback detail. A fatal run with no later successful present may
  end before these pending counters publish; its failure log remains authoritative.
- **CPU project / stage** count detailed geometry preparations and CPU buffer
  staging paths. **GPU reuse / publish** count retained range/sample uses and
  successful geometry publications. These are coarse events per present, not
  vertex counts or percentages: several batches can occur in one frame.
- **Atlas hit / copy** reports reused world-ground animation versions and
  GPU-only texture-copy MiB/calls per present. Copies occur when a new version
  is first published; warm hits avoid CPU art rebuilding and texture uploads.
  These bytes are separate from geometry copies and host uploads. They are
  submitted work, not measured GPU bandwidth or VRAM residency.
- **Opt / limit / reject** distinguish explicit optimization opt-outs, known
  caller-side draw-budget guards, and optional resource/API rejection. A reject
  does not identify a driver fault: it can include an allocation or adapter
  budget limit. These counters do **not** mean the view became authentic.
  Normal camera-dependent CPU work is counted as project/stage, not rejection.

The shared pipeline covers input/events, emulation, PPU preparation/scanout/
finish, world-map building, SIM metadata, canvas raster/enhancement, frame
snapshot, upload, presentation, CRT, host/settings UI, pacing and housekeeping.
The right column adds 19 SIM/world stages or 13 enhanced-action stages. Flat
action is labeled **Action 2D**, separately from **Native/menu** and **Action 3D**;
its scanout/upload/drawing costs are in the main pipeline, not an inactive 3D
compositor. Emulation includes
game/runner/APU work on the owner thread; it does not time the separate audio
callback. If emulation is dominant, the existing restart-time
`SNESRECOMP_APU_PROFILE` diagnostic is the next drill-down, not a guessed GPU fix.

The panel refreshes once per second and starts a fresh window when the scene,
map, host mode, output size or pacing policy changes. Detailed fits 1280×800 and
640×480. Narrow/tiny outputs use Summary. While the host settings menu is open,
a compact panel leaves most controls accessible; the log keeps full detail.
Native in-game menus still receive the normal detailed panel.

## What to send from Steam Deck

1. Let each problem view settle for about five seconds, then take a screenshot
   with **Detailed** selected: town, navigation, Sky Palace, action and any slow
   menu. Include the full panel rather than only the FPS number.
2. Note the Deck power limit, refresh/limiter settings, output resolution and
   graphics preset. Repeat with the same camera/content where practical.
3. Share that run's diagnostics ZIP. The once-per-second `[pipeline-perf]`,
   `[pipeline-stage]`, `[pipeline-work]`, `[pipeline-atlas]` and `[pipeline-path]`
   lines preserve more precision and history than a screenshot. The console prints the `runs/...` directory on
   exit. An overlay screenshot taken during entry can be paired with its log
   to distinguish a cold asset build from sustained per-frame work.

For stage-attributed geometry-path events, `AR_SIM3D_PERF=1` additionally logs
`[sim3d-path]` rows. The overlay is an aggregate for the current scene. Town
solids/depth/relief and ground shadows use `AR_SIM3D_TOWN_RETAINED=0` as a
restart-time diagnostic opt-out; quality/effect settings remain independent.

Do not conclude that a high present/wait time is wasted work: it can be normal
VSync/limiter behavior or GPU backpressure. Compare the same scene with its
limiter relaxed before choosing a CPU or GPU optimization.

## Ownership and cost

The collector stores only application-owned diagnostic values, uses a bounded
interval ring, and allocates nothing while recording. Disabling it avoids
diagnostic clock reads. Worker records are atomic; configuration, snapshots and
draining occur on the owner thread after fork/join completion. No runner ABI,
emulated state, shader contract or graphics-quality setting changes.

The panel receives a value snapshot and the frame-captured visibility setting.
It draws after CRT in physical output coordinates. Formatting and glyph
submission run at sample cadence; capable renderers retain the premultiplied
panel in one target and draw it once per warm frame. Unsupported/failed optional
targets use direct glyph batches. Failed target-state restoration propagates
instead of drawing into an unknown target. Off/reset/shutdown release retained
resources. The overlay's own wall time is measured and shown.

Regression coverage includes stale epochs, context switches, percentile ring
overflow, parallel recording, settings persistence, bounded layouts, actual
font rendering, warm panel reuse, resize/reset, optional allocation/bind/clear/
draw failures and failed target-state restoration.

See `pipeline-performance-audit.md` for repeated optimization measurements,
observer-cost caveats and the remaining CPU/GPU candidates.

## Automated comparisons

`tools/compare_pipeline_performance.py` runs eight interleaved ABBAABBA replays,
using copied SRAM/settings and the existing checkpoint manifest. It records
binary/input hashes, per-run logs, frame-weighted settled-window averages and
median/range summaries in a new output directory. Missing scene samples or
changed inputs fail the comparison instead of reporting a misleading zero.
The first matching window is discarded to exclude entry warmup.

For example, from the repository root, compare a saved control binary with the
current release in the populated Aitos town:

```sh
python3 tools/compare_pipeline_performance.py \
  --control /path/to/control --candidate build-release/ActRaiserRecomp \
  --set AR_SIM3D_CLOUDS=on
```

To isolate helper scaling, use the same binary for both arguments and add
`--control-workers 0 --candidate-workers 3`. No graphics settings are reduced.
Other replay fixtures can be selected with `--manifest`, `--checkpoint` and
`--scene`; use a long enough `--quit-frames` to reach three settled samples.
Paths inside manifests resolve from the repository root. A supplied output
directory must not already exist. Requires Python 3 and a working GPU backend.

Run comparisons one at a time without builds, tests, screen recording or other
benchmarks competing. The Unlimited policy does not override a driver-imposed
refresh limit. Review stage ranges as well as overall medians; a CPU scope win
does not establish an equal FPS gain. Screenshot comparisons are separate runs
so readback/encoding cannot contaminate timings.
