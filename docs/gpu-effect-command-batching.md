# GPU effect command batching

The resident action renderer groups consecutive effect packets into one SDL GPU
command buffer (default on for Metal and Vulkan since October 20, 2026). Scenery illumination, actor illumination, attached layer effects,
and final actor/atmosphere overlays each define their own scope. This changes
submission granularity, not geometry, shaders, blend order, or interpolation.

## Ownership and ordering

- `DioramaRenderOptions.source_effect_batch` brackets effect-only callbacks.
  Begin is lazy: an empty scope acquires or submits nothing.
- The first nonempty draw submits preceding SDL renderer work and acquires a
  native command buffer. Later packets append their copy, compute, and render
  passes in the original order. End submits before SDL rendering or restoration
  of a target/viewport. A scope cannot span frames or nest.
- Existing transfer-buffer, input-buffer, and scratch-buffer cycling remains
  enabled. Earlier packets retain their own bound storage generations while
  later packets upload data and produce vertices.
- Retained input cache keys are speculative until submission. Recording failure
  cancels the pending buffer and invalidates those keys; failed submission also
  invalidates them. The presenter retains its full-frame reference recovery.
- The compositor closes a started scope even if its callback fails. Destruction
  cancels any outstanding command buffer before releasing resources. Snapshot
  descriptions remove native callbacks before becoming portable replay data.

The implementation uses SDL GPU on Metal, Vulkan, and D3D12. It introduces no
native API dependency, new shaders, readback, fence wait, or cross-frame queue.
The WebAssembly editor retains its existing WebGL submission path; its shared
compositor has a null batching callback.

## Diagnostics and validation

`AR_GPU_EFFECT_BATCHING` follows `gpu_prototype_policy.h`: unset, empty or
`auto` batches on Metal and Vulkan and keeps immediate submission on D3D12;
`1` forces batching and `0` forces immediate per-packet submission for A/B
testing. This is an initialization-time override, not a saved setting. `[gpu-effect-commands]` reports draws, submissions,
and largest batch at shutdown/reset. These are effect/skybox submission counts,
not total renderer command counts or GPU execution time.

The October 18, 2026 experiment and raw traces are in the local, ignored
`runs/gpu-command-batching-20261018/` directory. The comparison uses the same
binary with batching disabled/enabled, Deck Vulkan at 1280x800, and identical
replays/settings. Forest and Bloodpool capped comparisons use off/on/on/off
order, excluding loading and the first five seconds. Native presentation and
the animated Aitos waterfall have additional paired checks. VSync is measured
separately from the 90 FPS cap, including a longer on/off pair with approximately
62 seconds of steady-state samples per mode.

The original two smoke measurements are excluded because DPMS was off; waking
the display during the second caused an epoch boundary. An initial waterfall
attempt used a nonexistent input filename and produced no measurements. The
valid waterfall test uses `input-waterfall.rec`, `aitos-seed.srm`, warp frame
1320, diorama frame 1700, and measurement frames 2625–3500.

Validation covers complete forest, moonlight, castle, and temple rooms across
camera poses, skybox policies, interpolation phases, faded/retained packets,
invalid cache slots, and player/enemy receivers. GPU frames are compared
exactly between submission modes on both Metal and Vulkan. Existing small
CPU-reference differences are checked for equality between modes rather than
misrepresented as newly introduced errors. Focused GPU tests cover scratch and
transfer cycling, blend order, slot replacement, cancellation, and empty/nested
scopes. The compositor recorder checks submission before state restoration and
scope closure on callback/submission failures. The rebuilt WASM receiver suite
checks the unchanged editor path.

Windows/D3D12 execution was not tested in this experiment. No D3D12-specific
code or shader interfaces changed.

## Results and decision

Eighteen valid Deck replays completed with identical game-state hashes within
each comparison, zero projection recoveries, zero metadata downloads, and zero
synchronous motion waits. Values below are milliseconds; repeated short tests
use the median of run means/p99s, rather than a pooled percentile.

| Scene and presentation | Host draw, immediate → batched | Completed-present p99, immediate → batched |
| --- | --- | --- |
| Bloodpool 1, interpolated, 90 FPS cap (2 runs/mode) | 2.784 → 2.471 | 13.458 → 13.222 |
| Fillmore 1, interpolated, 90 FPS cap (2 runs/mode) | 1.057 → 1.031 | 11.558 → 11.535 |
| Fillmore 1, native, 90 FPS cap (1 run/mode) | 0.798 → 0.754 | 11.471 → 11.509 |
| Aitos waterfall, interpolated, 90 FPS cap (1 run/mode) | 0.826 → 0.809 | 11.537 → 11.531 |
| Bloodpool 1, VSync (2 short runs/mode) | 2.732 → 2.388 | 11.653 → 11.850 |
| Bloodpool 1, VSync (1 long run/mode) | 2.782 → 2.405 | 11.642 → 12.015 |

Bloodpool's effect/skybox command submissions decreased by 28.6%; Fillmore's
decreased by 25%. Aitos's tested view had no consecutive nonempty effect
packets to combine, so its small timing differences are control variation,
not evidence of a batching speedup. All valid comparisons averaged about
90 completed presents/second under their respective cap/VSync policies.

In the long VSync pair, owner-thread CPU draw time fell from 2.711 to 2.325 ms,
while mean swapchain acquisition wait rose from 5.730 to 6.118 ms. The saved
CPU time mostly became waiting under this presentation policy. Intervals over
the 11.11 ms budget plus 1 ms increased from 16 to 36. Mean source age at
completion stayed near 31.9 ms in both modes. These are host completion metrics,
not physical scanout latency or GPU execution timestamps. Process RSS did not
grow (long-pair peak approximately 165 MiB immediate, 162 MiB batched); this is
not a measurement of GPU-local allocations.

Pixel validation: 135 Metal cases matched the preceding committed renderer
exactly; another 135 cases including player/enemy receivers matched between
submission modes on both Metal and Vulkan. Retained/fade/invalid-slot checks
also matched. All 120 compositor-recorder scenarios, the snapshot tests, source
GPU tests, and rebuilt WASM receiver checks passed.

**Keep batching opt-in.** It reduces CPU submission work, but the VSync tail
regression repeated in the longer comparison. It does not yet establish a
stable 90 Hz improvement. The next experiment should correlate per-frame batch
submission, new-capture versus repeated-frame work, and swapchain readiness
before changing presentation scheduling or extending scopes across targets.
Raster/compute pass counts and math remain unchanged in this prototype; GPU
event timings are still needed before attributing any cost to a particular
shader. Ray tracing remains deferred.

## October 20 presentation-feedback follow-up

The [new Deck validation](steam-deck-prototype-validation.md) changes the
interpretation of that host-p99 objection. Batching again saves about 0.30 ms
of owner CPU time. Four additional game replays with Wayland presentation
feedback all retain an 11.111 ms display p99; neither enabled run misses a
refresh in its selected window. One unbatched repeat has two ~22.22 ms gaps
and two discarded submissions, retained in the analysis. A clear-only Vulkan
control also has a 12.102 ms host p99 while displaying every 11.111 ms.

The small host-p99 excess is therefore insufficient evidence of a display
pacing regression. Batching is now a strong Vulkan default-promotion candidate,
with existing Metal/Vulkan pixel validation and repeated CPU savings. This
does not establish a rare-hitch cure or D3D12 runtime behavior.

**Promoted October 20, 2026** for Metal and Vulkan after the 25-minute Deck
arms in [the Deck review](frame-pacing-deck-review-20261020.md) showed no
display miss attributable to the game in either mode; D3D12 keeps immediate
submission until a Windows runtime pixel comparison exists.
