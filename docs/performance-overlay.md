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
- **Ticks / repeats** describe simulation ticks and repeated presentations per
  displayed frame. A 60-Hz simulation displayed at 120 Hz can have roughly
  0.5 ticks and 0.5 repeats.
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
