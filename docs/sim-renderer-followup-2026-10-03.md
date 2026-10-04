# SIM rendering audit: implementation and live validation — 2026-10-03

This follows `sim-renderer-pacing-audit-2026-10-03.md`. Changes and experiments
start from `277e077e`. Evidence, pinned binaries, exact settings, replay hashes,
CSV, logs, and a summary script are in `runs/sim-pipeline-2026-10-03/`.

## Decisions

| Topic | Outcome |
| --- | --- |
| SIM diagnostics | Implemented. Buffered traces cover every synchronous present; source timestamps, epochs, tick deltas, scene and actual pacing source are recorded. The analyzer accepts both paths. Overlay holds/skips include SIM. |
| Actual pacing clock | Fixed native sampling during software VSync fallback. A bounded nine-present probe every five seconds can restore renderer pacing after startup/occlusion. No permanent setting changes. |
| Refresh phase | Implemented a 1/8 phase correction with discontinuity resets. `AR_FRAME_REFRESH_PHASE=0` selects the raw-phase control. Live improvement is small; the offline multi-fold claim is not established. |
| Scheduled synchronous ticks | Implemented and tested, **experimental and off by default**. Opt in with `AR_FRAME_SYNC_SCHEDULE=1`. The Deck comparison favored the accumulator. Low limits, hidden windows, turbo and interpolation retain accumulator behavior. |
| Development rebuilds | Mountain invalidation now follows actual rock art and mountain-relevant native inputs. Terrain caches use consumed water/rock/vegetation coverage. World art patches changed cells and filter neighbours using its own snapshot. Water-only changes no longer republish the globe surface. |
| BG1 transfers | Skip both BG1 texture uploads only after this frame has a ready voxel replacement. Menus and the inspector retain them; failed/unavailable replacements retain their upload path. Shadow dispatch remains intact. |
| BG1 scanout reduction | Deferred. CPU HUD reconstruction, object half-add and diagnostic composition still read BG1. Upload-time readiness cannot safely promise a replacement before scanout. A request/consumer contract is required before narrowing capture. |
| Full SIM streaming | Not included. Deck measurements justify a follow-up, but SIM still reads shared canvas, voxel, world-map and capture state. An owned publication boundary must precede producer/presenter overlap; reusing the action packet switch would race these readers. |

No image interpolation, GPU decoding of replaced BG1, or full-surface per-packet
copies were added. Rebuilds that change actual terrain remain synchronous.

## Corrections to the third-party attribution

The supplied audit runs logged software VSync fallback, including action.
The previous sampler used that software deadline only in producer-pacing mode,
which is disabled when VSync is requested. Consequently those action rows had
`sample_ns == prepare_ns`; an offline phase-only replay was not a faithful replay
of the clock actually selecting frames. The new trace labels distinguish
immediate (0), renderer (1), software deadline (2), and recovery probe (3).

SIM emulation already dispatches to the producer and waits; the entire measured
serial path is not main-thread CPU time. Also, the broad SIM upload scope includes
world navigation preparation. Narrow measurements showed recurring mountain,
cliff and world-art work, rather than a 21–25 ms voxel palette or atlas upload.
On the Mac, voxel palettes were below 0.01 ms and mountain-atlas upload below
0.1 ms in the initial profile.

The world map's existing dirty flags are consumed by image baking. Incremental
world art therefore owns a separate 16 KiB tilemap snapshot. Publication failures
invalidate both CPU/GPU certification, and recovery still requires a full upload.

## Measurements

The town workload uses the exact audit seed (SHA-256
`26ec2474882a69dff576f518f614a094f58f1428c807faf83230d72fe4c13568`) and
`aitos-eruption.rec`, Original SIM menu, Native text, isolated saves, and the
same presentation settings. Town measurements cover ticks 1100–3600. The Deck
runs fullscreen at 1280×800 on its 90 Hz Wayland desktop; 60 and 120 are software
presentation targets, not physical display modes. The harness reported no foreign game processes, and benchmarks on each device were run sequentially.

| Run | Completed FPS | p95 interval, ms | Maximum interval, ms | Best-phase mismatch |
| --- | ---: | ---: | ---: | ---: |
| Mac before cache changes | 59.83 | 16.98 | 53.41 | 1.73% |
| Mac after cache changes | 59.93 | 17.01 | 38.14 | 1.12% |
| Deck before cache changes, scheduled | 84.57 | 22.89 | 90.98 | 7.35% |
| Deck after cache changes, scheduled | 84.54 | 23.77 | 60.36 | 8.13% |
| Deck after cache changes, accumulator | 84.15 | 23.71 | 61.52 | 4.88% |
| Deck moving town, scheduled | 84.60 | 20.82 | 52.69 | 7.98% |
| Deck software 60 target, accumulator | 56.96 | 25.58 | 61.72 | 10.60% |
| Deck software 120 target, accumulator | 87.55 | 22.42 | 62.40 | 4.22% |

These are individual runs, not statistical confidence intervals. Before-cache
binaries already include the initial pacing/diagnostic work. The scheduled
implementation also received a catch-up/reset correction between the initial
profile and final candidate. The source metric fits a constant delay per epoch;
it is not raw hold percentage, GPU execution time, physical scanout or measured
input latency. Normal 90 Hz repeats of 60.0988 Hz source frames are expected.

The clearer gain is recurring growth work. Mac mountain preparation fell from
roughly 8–14 ms to 0.04–0.07 ms; most world-art updates fell from 7–16 ms to 2–5 ms.
Deck growth mountain work fell from 15–22 ms to about 0.2 ms, and world art from
12–16 ms to 5–7 ms. Cliffs still cost 7–11 ms when changed coverage affects terrain.
Average Deck throughput did not materially improve. Settled per-present means
include about 3 ms PPU/capture and 5.6 ms presentation; nested scopes must not be
summed. These costs and remaining growth spikes still limit 90 Hz presentation.

On the same pinned final Mac binary, streamed action at native 60 Hz scored
0.85% with raw phase versus 0.77% with filtered phase (ticks 1200–2400). Town phase
A/B results were 1.04% versus 1.36%, with different growth-frame maxima. The live
runs do not demonstrate the audit's projected 2–4× improvement. Both action runs
reported zero GPU background capture mismatches and no session failures.

The initial Deck Wayland attempt blocked with the display asleep; an Xwayland
retry completed at approximately 1 Hz. Those runs are excluded. Waking DPMS
restored normal 90 Hz operation. A further exploratory run used a different
existing Deck seed and reached Sky Palace; it too is excluded from town results.
Renderer pacing was observed in the valid VSync runs. Recovery-probe behavior is
covered by deterministic tests; these runs did not trigger recovery themselves.

## Validation and follow-up

- Release tests cover clocks, pacing, metrics/overlay, world-map revisions,
  incremental art, mountains, terrain, water, texture ownership and PPU capture.
- AddressSanitizer tests passed for the modified cache, capture-upload and pacing
  paths. Python analyzer tests passed.
- Incremental native/overview art is compared pixel-for-pixel with full rebuilds
  across model/detail/cliff gates, cell boundaries, tiers, occupancy, native
  ownership, town enable/disable, animation and padded pitches.
- Captured-town Metal GPU tests passed: cold/warm parity, camera limits, settings
  changes, immutable capture, terrain/model registration and continuous globe.
- The general synthetic Metal GPU suite fails its model-detail image assertion
  at `present_world_nav_gpu_test.c:888`. An isolated build of unchanged HEAD fails
  identically. This existing failure was not bypassed or changed.
- Live Deck idle, moving, software 60/120, and Mac native-action runs completed
  without fatal sessions. The moving Deck composite was inspected, including HUD,
  water, shadows and eruption effects.

Remaining priorities are an owned SIM publication contract and the measured
PPU/shadow/presentation costs. The results do not justify enabling scheduled SIM
ticks by default or claiming stable 90/120 FPS. Capture elision should follow a
complete BG1 consumer inventory and a pre-scanout replacement guarantee.

`AR_FRAME_SYNC_TRACE=/path/sync.csv` enables the new trace. `sample_ns` preserves
the old iteration-start meaning; `presentation_sample_ns` records the predictor's
sample explicitly. `target_ns` records the release target. Analyze with:

```sh
python3 tools/analyze_frame_pacing.py /path/sync.csv --start 1100 --end 3600 --refresh 90
python3 runs/sim-pipeline-2026-10-03/summarize_results.py
```

Benchmark binaries were pinned before unrelated concurrent action-effect edits
appeared in the shared checkout; those edits were left untouched. Final Mac and
Deck builds also include the independently committed action change `a09429c2`.
The final Deck default-policy smoke ran 2,400 frames cleanly; its CSV verifies
that the accumulator is the default and the presentation sample is recorded.
Final verification: 11 focused Release tests, 9 ASan tests (plus the updated
water/clock cases), 10 Python analyzer tests, and the captured-town GPU suite.
Binary hashes are in `shipping-sha256.txt`; final smoke evidence is in
`deck-shipping-default/`.
