# Performance overlay

In **Settings → Video → General**, set **Performance overlay** to **Summary** or
**Detailed**. The setting changes live and is saved with your preferences.
It works in both authentic and enhanced modes.

On Steam, you can also use the launch option
`AR_PERFORMANCE_OVERLAY=Detailed %command%`. For logging without a visible panel,
use `AR_PIPELINE_PERF=1`.

## Reading the panel

- **FPS / ms / p95 / max** describe displayed frame intervals, not simulation
  speed. The percentile and maximum help identify uneven frame delivery.
- **Ticks / redraws** describe simulation ticks and retained-frame draw calls per
  presentation. Redraws are not held game frames: streaming uses that draw path
  for every presentation, even when the source tick changes.
- **Source holds / skips** replace that row during native streamed playback.
  They count actual repeated source ticks and omitted ticks per second.
  At 90 Hz, a 60 Hz source naturally holds about 30 presentations per second;
  at 60 Hz, the 60.0988 Hz source naturally omits roughly one tick every ten
  seconds. Those counts alone do not prove judder. Interpolated endpoints are
  excluded because they do not identify the synthesized image on screen.
- **AVG / PEAK** measure CPU wall time, including any driver calls that block.
  Nested stages overlap, so adding every row does not give total frame time.
- **Present/wait** includes submission and waiting inside the presentation API.
  **Pacing/sleep** is the host's explicit frame limiting. High values can be
  normal with VSync or a limiter; they do not necessarily indicate a slow game.
- **GPU execution: unavailable** means GPU execution time is not measured,
  not that the GPU is doing no work.
- **Job owner / helpers / join wait** describe parallel rendering work. Helper
  durations overlap and must not be added as serial frame time. Zero jobs can
  simply mean a quiet scene reused cached content.
- **Batches, vertices, uploads, and Scan MiB** describe submitted geometry,
  uploads, and requested change-detection comparisons. They are not measurements
  of total device bandwidth or memory traffic.
  Texture calls include both ordinary texture updates and individual GPU atlas
  regions. Texture bytes count their requested pixel payload, including full
  uploads and failed attempts, once at the backend. They exclude staging-row
  padding and hidden driver copies. Skipped uploads count unchanged mirrors in
  both SIM and action modes; textures omitted because they have no consumer do
  not count as mirror skips.
- **Fallback / failed** count authentic-view fallbacks and rejected presents.
  **Opt / limit / reject** distinguish optimization opt-outs, draw-budget limits,
  and optional resource rejection; those counters alone do not mean the view
  fell back to authentic rendering.

Detailed mode breaks time down by rendering stage and adds scene-specific rows.
The panel refreshes once per second and starts a fresh sample when the scene,
output size, or pacing policy changes. Small outputs use Summary, and the
settings menu uses a compact panel to leave controls accessible.

## Reporting a slow scene

1. Let the view settle for about five seconds, then take a screenshot with
   **Detailed** selected. Include the full panel, not just the FPS counter.
2. Note your hardware, power limit, display refresh rate, frame limiter, output
   resolution, and graphics preset. On Steam Deck, include its power settings.
3. Share that run's diagnostics ZIP. The console prints the `runs/...` directory
   on exit; its logs provide more detail than a screenshot.

If a slowdown only happens while entering a scene, capture that separately from
steady gameplay. When comparing settings, use the same location and camera.

## Frame-pacing diagnostics

`AR_PIPELINE_PERF=1` also reports these targeted stages in `[pipeline-stage]`
lines (not additional rows in the on-screen panel):

| Stage | What it measures |
| --- | --- |
| `settings write*` | Completed worker-owned settings writes, including durable flush/rename. Camera writes overlap frames; synchronous menu saves wait for this work. This is **not a separate main-thread stall measurement**. |
| `battery save` | Main-thread SRAM change check and, when changed, its durable save. |
| `music start` | Replacement decoder opening/session initialization, including audio-lock wait. Not steady-state decode time. |
| `terrain rebuild` | Cold world height preparation, including owner-side prior preparation and joined row workers. |
| `terrain samples` | Cold globe position/normal/height sampling, including worker join. |
| `globe grid bake` | SIM globe grid source construction and curved embedding, including worker join. |

Use `peak-call-ms` to find spikes. `mean-ms` is accumulated work divided by
presentations, not average duration per invocation. Background completions are
attributed to the reporting window in which the owner collects them; several
completed writes retain their individual maximum rather than creating one
artificially large peak. These stages overlap existing parent stages.

The existing per-scene window still resets on scene changes. It is not a
cross-transition hitch trace, and a short-lived scene may end before a report.


### Source cadence traces

`AR_FRAME_PACING_TRACE=/path/to/pacing.csv` adds source tick/epoch, source
interval, playout target/sample time, and producer thread CPU time to the
existing stage trace. CPU sampling is off outside trace runs. CPU time and
wall time refer to uploaded endpoints; skipped packets are not separate CSV
rows. Unaccounted wall time includes blocking and descheduling, and cannot
establish that a thread was runnable throughout a stall.

Analyze a settled native run with, for example:

```sh
python3 tools/analyze_frame_pacing.py pacing.csv --refresh 90 \
  --start 1825 --end 3000 --warmup-seconds 10 --max-irregular-percent 2
```

The `presentation_fps` result reports completed presents over elapsed time,
including held source ticks and epoch transitions. Its `one_percent_low` is
the reciprocal of the average slowest 1% of frame intervals, rounded up to a
whole interval. These are render presentation rates measured at backend
present return; they are separate from the SNES update rate and physical
display scanout. Use **Unlimited** for throughput comparisons: **Uncapped**
still applies a soft limit at twice the nominal display refresh rate.

The optional threshold fails the command if source cadence exceeds the stated
percentage. The analyzer fits one constant source/display phase per epoch and
reports the fraction of presents inconsistent with it. This accommodates
normal 1/2 holds at 90 Hz, source/display drift, and low-refresh skips. It is a
lower bound on irregular selection based on present-return timestamps, not
physical scanout or input-to-photon latency. Hold histograms omit the truncated
first and last hold of each epoch. Interpolated traces have no native cadence
percentage, and requesting a native threshold on them fails explicitly.
Use `--native` only for older traces known to have interpolation disabled;
legacy files without a source interval assume NTSC timing.

`[pipeline-cadence]` logs actual native holds and skipped ticks per completed
present. `[pipeline-work] repre` remains a count of retained-frame draw calls.

Native streamed presentation selects the newest source scheduled before its
presentation target, with a 0.75-source-period delay (about 12.48 ms at NTSC).
VSync uses a filtered refresh estimate from present returns; capped playback
uses its scheduled deadline. Future packets stay queued, and waiting for a due
packet is bounded. Game and audio clocks are unchanged. Unlimited presentation
retains newest-completed selection; unsupported scenes and below-source caps
keep their existing synchronous path. Interpolation retains its existing delay.

Diagnostic comparisons can set `AR_FRAME_STREAM_NATIVE_DELAY_US=0` to restore
newest-completed selection. Positive values up to 33000 override native delay.
On macOS, `AR_FRAME_PRODUCER_QOS=default|initiated|interactive` compares scheduling
classes; production defaults to interactive. Linux and Windows scheduling
priorities are unchanged. These are diagnostic overrides, not saved settings.
