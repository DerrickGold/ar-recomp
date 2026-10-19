# Metal output reuse and drawable-pool experiments

Measured 2026-10-19, following the [presentation timing investigation](gpu-presentation-timing.md).
Reducing the Mac drawable pool from three textures to two removes approximately
one refresh of presentation delay in a reversible, within-process experiment.
Extra application output textures did not help. No production queue policy or
rendering default was changed by these experiments.

The later [production integration](gpu-rendering-integration.md) replaces the
experimental policy hook with explicit native-window access and enables two
drawables under Mac VSync. The results below retain their original test setup.

## Controlled result

Apple M2, SDL 3.4.18, Metal, a 1920×1080 backing surface in a 960×540 logical
window on the internal 60 Hz display. The attached ASUS display also reports
60 Hz. Environmental effects, interpolation, lighting, particles, rim lighting,
edge AA and DOF were enabled; CRT, early submission and direct-layer prototypes
were disabled. Configuration and saves were isolated from everyday play.

In one Bloodpool replay, a diagnostic library alternated
`CAMetalLayer.maximumDrawableCount` between 2, 3, 2 and 3 before acquiring
drawables. VSync and SDL's allowed frames in flight remained unchanged. Each
measurement below excludes the transition and at least 60 subsequent drawable
acquisitions, as well as startup and shutdown.

| Settled phase | Drawable pool | Samples | Present API return → OS display, mean | OS display interval, p99 |
| --- | ---: | ---: | ---: | ---: |
| A | 2 | 191 | 31.963 ms | 16.667 ms |
| B | 3 | 381 | 48.630 ms | 16.667 ms |
| A again | 2 | 381 | 31.979 ms | 16.667 ms |
| B again | 3 | 481 | 48.594 ms | 16.667 ms |

All selected presents had nonzero OS display timestamps. Sampled application
and console states were active and unlocked. This reversal is stronger evidence
than comparing separate application launches: the delay follows the pool size,
without changing game content, producer behavior, shaders or synchronization mode.

The measurement is presentation delay after the API returns, **not total input
latency**. Source age at display is recorded separately. These results do not
measure photons, all GPU shader cost, or a Steam Deck 90 Hz budget.

## Additional controls

Separate 2- and 3-drawable VSync captures used identical executable, replay,
settings and observer, selecting game ticks 1600–2500. They reached identical
final WRAM for equal replay lengths. WRAM equality checks deterministic game
execution, not visual parity.

| Scene and mode | Pool 2 delay, mean | Pool 3 delay, mean | OS display interval p99, both |
| --- | ---: | ---: | ---: |
| Marahna scrolling, interpolation | 31.999 ms | 64.510 ms | 16.667 ms |
| Marahna scrolling, native frames | 32.007 ms | 65.301 ms | 16.667 ms |
| Bloodpool effects, interpolation | 31.971 ms | 65.244 ms | 16.667 ms |

The 3-drawable path sometimes settled near 65 ms across launches rather than
49 ms. Its interpolated Marahna capture includes one 33.33 ms display interval;
the 99th percentile remains 16.67 ms. Do not promise a two-refresh reduction
based on these launches: the reversible experiment supports a repeatable
**one-refresh** improvement. The reason for the additional launch-dependent
refresh remains unresolved.

Native source cadence was also checked at OS display timestamps: phase-fit
mismatches were 2/899 with two drawables and 0/900 with three. Neither capture
held a source frame; source-tick skips were 2 and 1 respectively. Native game
and display rates differ slightly, so these small counts are not a claim of
perfect long-session cadence or a meaningful regression.

Disabling detailed resource tracing reproduced the result in two more Marahna
VSync captures: **31.975 ms with two drawables, 48.637 ms with three**, both with
16.667 ms display-interval p99. All 899 selected presents per capture had unique,
nonzero display timestamps, and all 7,058 selected commands completed. This
rules out the additional resource tracing as a necessary cause of the gain;
the smaller asynchronous timing observer remains active in these controls.

The immediate-mode/cap-90 control is a different result. With two drawables it
submitted about 90 times per second and reduced mean presentation delay to
7.66 ms, but OS update intervals had p99 20.64 ms and maximum 31.95 ms. The
physical display is 60 Hz. Faster presentation callbacks in immediate mode do
not establish 90 complete physical scanouts, regular pacing, or tear-free output.
The three-drawable control remained near 60 submissions per second and 48.56 ms
delay; it also reported one zero presentation timestamp and one duplicate
timestamp among 901 selected presents. Keep VSync and immediate-mode conclusions
separate; do not equate submitted drawables with distinct displayed frames.

## Rejected application output ring

Optional resource tracing follows native Metal color attachments, fragment
samplers, presentation surfaces and command identities. It confirms that the
normal terminal copy reads one application output backing, even though SDL can
cycle other texture backings. The CPU starts writing the next output about
15.27 ms before the previous terminal read completes; Metal orders this legal
resource dependency.

A three-output prototype moves that reuse to about 18.26 ms *after* the previous
read completes and confirms three distinct native backings. Nevertheless,
Marahna's cap-90 presentation delay stays at 48.73 ms versus 48.89 ms with one
output, while `nextDrawable` still waits about 15.6 ms and OS display p99 remains
16.67 ms. The immediate output-reuse dependency was removed without relieving
drawable backpressure. The prototype was archived in the ignored evidence
directory and removed from application source; its extra memory and lifetime
complexity have no demonstrated payoff here.

Output-writer GPU timestamps cover the entire command and may include waits.
They are not per-pass shader cost. Overlapping command intervals must not be
summed as GPU utilization.

## Implementation boundary and next steps

Apple documents a supported [maximum drawable count](https://developer.apple.com/documentation/quartzcore/cametallayer/maximumdrawablecount)
of two or three, with three as the default. SDL 3.4.18's public API does not
expose this property. Its [Metal backend](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/gpu/metal/SDL_gpu_metal.m)
tracks allowed frames in flight separately; changing that setting does not
change the layer's drawable pool.

The experiment intercepts `nextDrawable` in a disposable process and changes
only this public layer property. **Do not ship method interception.** A
production implementation should use an explicit Mac backend/SDL integration,
apply the setting when the relevant layer is created or replaced, and verify
it after resize, fullscreen and display changes. Preserve independent Vulkan
and DX12 policy; this result does not justify changing their swapchain depths.

Before enabling a Mac default, validate longer sessions, hidden/restored windows,
resize and fullscreen transitions, and a native high-refresh/variable-refresh
display. Two drawables still leave about 32 ms of presentation delay under
VSync here. Further compositor/scheduling investigation should explain the
remaining delay and launch-dependent extra refresh rather than assume a
shorter API wait represents faster GPU execution.

## Evidence and reproduction

Ignored local evidence: `runs/gpu-present-queue-20261019/`. It contains raw
CPU/Metal traces, per-run environment and binary hashes, source snapshots,
isolated `run-mac.py`, `bloodpool-pool-abab/phases.json`, the rejected output-ring
patch and binary, display inventory, and validation logs. Replays use:

```sh
python3 runs/gpu-present-queue-20261019/run-mac.py NAME 0501 VSync 1 CONFIG.json
```

Use room `0201` for Bloodpool. Configuration JSON selects the diagnostic policy
library and `AR_METAL_DRAWABLE_POOL=2` or `3`; `AR_INTERP_ENABLE=0` selects native
frames. `pool-abab.json` selects the captured alternating-policy library and a
3400-tick replay, switching at drawable sequences 1700, 2200 and 2700. Settled
ranges are 1450–1640, 1760–2140, 2260–2640 and 2760–3240. The normal game executable
for these comparisons is SHA-256
`10b5074744227bc2de8e3533aa693f18b67ad189ee146ddace415970222b5b72`.

The tracked `tools/profile_metal.m` remains an **observer only**. Its resource
trace is opt-in. Reproduction of policy experiments uses the separately named
captured diagnostic source/library, never silently replacing the observer.
The analyzer rejects ambiguous command/output associations, reports missing
or zero display timestamps and duplicate timestamps, and distinguishes GPU
completion from callback delivery and OS display.

Validation passed: release rebuild after removing the output-ring prototype;
strict-warning Metal probe compilation; presentation-ordering and pacing-tool
tests; ten Metal correlation tests; and a real-Metal two-drawable upload and
presentation test with 32 pixel-exact normal/prepared pairs across visible and
hidden windows. The GPU test reads pixels only after terminal submission, so
it does not serialize away the ordering being tested. It checks output pixels
and command integrity, not whole-game visual parity or physical display latency.
All accepted replay traces report zero event-buffer overflow.
