# Environment renderer hot paths — 2026-10-29

The strongest repeatable effect cost is Aitos's cloud-opening lighting.
Marahna Act 1 has a different tail: much of its variation occurs between
submission and the first GPU timestamp, rather than during the measured GPU
commands. Neither result supports indiscriminately reducing mist or particles.
No production rendering behavior or effect quality changed in this audit.

The subsequent [prototype report](environment-prototypes-20261029.md) records
the measured sprite/scenery early-return and upload-batching experiments.

## Method and scope

- Steam Deck, RADV VANGOGH, Vulkan, 1280×800, desktop display awakened and
  checked throughout each execution. No display sleep, crash or late shader
  compilation occurred in the accepted timed runs.
- Four selected hot routes: Aitos clouds and west lava, Marahna swamp doorways
  (05:02), and Marahna temple gallery (05:06).
- Every execution traversed the entire 240-frame route/animation interval as
  warmup, then repeated it for 240 measured frames. Three alternating repeats.
- 45 ordinary effect-family isolation runs; 24 additional CPU-profile / GPU-
  timestamp runs. No captures, transfers or external profilers during those
  accepted runs. One separate CPU `perf` sampling run is attribution evidence,
  not a performance comparison.
- Isolation disables one emitter family in a diagnostic INI copy, retaining
  all other scene definitions. Differences include CPU, GPU and pipeline
  interactions; they are not exclusive shader timings or promised speedups.
- Tables report the median of three per-run statistics. p99 uses the nearest
  rank, as in `benchmark_rooms.py`, rather than pooling all frames.

The fixture is the previously validated `review-layers.ini`, not subsequent
unrelated edits to the repository's root layer file. Input hashes and exact
diagnostic configs accompany the evidence. The standalone room renderer uses
the editor's CPU tile preparation and an offscreen serial GPU-idle wait. It
does not measure the game's GPU background producer, interpolation, CRT,
gameplay logic or swapchain pacing. These results do not certify 90 Hz gameplay.

## Repeatable effect cost

All numbers below are composition/submission **plus remaining GPU completion**,
excluding editor CPU tile preparation. These runs have no timestamp probe.

| Route | All effects mean / p99 | Selected family disabled mean / p99 | Mean difference |
| --- | ---: | ---: | ---: |
| Aitos clouds | 5.135 / 7.105 ms | Cloud lighting: 3.753 / 5.008 ms | 1.382 ms |
| Aitos west lava | 6.200 / 7.428 ms | Cloud lighting: 4.369 / 5.428 ms | 1.831 ms |
| Aitos west lava | 6.200 / 7.428 ms | Heat haze: 5.672 / 7.093 ms | 0.528 ms |
| Marahna swamp doorways | 5.670 / 8.013 ms | Cloud banks: 5.673 / 8.151 ms | No benefit |
| Marahna temple gallery | 3.034 / 3.590 ms | Soft lights: 2.807 / 3.532 ms | 0.227 ms |

Disabling Aitos cloud banks saves about 0.39–0.51 ms overall. CPU composition
decreases by about 0.66–0.78 ms, while completion waiting increases: the two
measurements overlap in execution and should not be interpreted independently.
Disabling storm lightning saves about 0.26 ms on the cloud route. Removing
Marahna canopy lighting actually worsens total mean from 5.670 to 6.153 ms;
foliage-shadow removal also fails to improve it. Their changed shader/pipeline
mix and queue behavior need a separate experiment before assigning the cause.

The cloud-light family accounts for about 27–30% of Aitos composition completion
in this experiment. Removing it also changes submission and material paths, so
1.4–1.8 ms is an attribution bound, not the gain expected from optimizing its math.
The previously rejected CPU-precomputed-opening prototype should not be repeated
as if it were an untested optimization.

## Where slow frames spend time

The separate timestamp proxy correlates CPU and GPU clocks. Values below are
instrumented diagnostics, not the ordinary benchmark numbers above.

| Route | GPU command envelope mean / p99 | First submit return → first GPU timestamp mean / p99 | Last GPU timestamp → idle return mean |
| --- | ---: | ---: | ---: |
| Aitos clouds | 4.701 / 6.487 ms | 1.396 / 1.717 ms | 0.115 ms |
| Aitos west lava | 5.701 / 6.899 ms | 1.513 / 1.735 ms | 0.103 ms |
| Marahna swamp doorways | 3.468 / 4.095 ms | 2.967 / 5.183 ms | 0.111 ms |
| Marahna temple gallery | 2.629 / 3.392 ms | 1.349 / 1.696 ms | 0.083 ms |

These columns overlap other CPU work and are not additive frame-budget slices.
The GPU envelope includes gaps and dependencies between command buffers.
Draw-level top/bottom timestamps also overlap, so summing draw durations would
overstate GPU work. Typical clock calibration uncertainty is about 0.008 ms;
the largest individual bound is 0.130 ms. Avoid microsecond conclusions.
This follows the timestamp scope and uncertainty described in the
[Vulkan query specification](https://docs.vulkan.org/spec/latest/chapters/queries.html)
and [calibrated timestamp reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetCalibratedTimestampsKHR.html).

Aitos cloud-route slow frames recur at requested x≈1907–2082 in all three runs.
For each run's eight slowest frames, the GPU envelope averages 6.437 ms versus
the route's ≈4.701 ms; pre-execution delay averages 1.549 ms versus ≈1.396 ms.
That points toward scene workload as the main tail contributor here.

Marahna's slow frames move around the route between repeats. Its slowest eight
per run average 3.697 ms in the GPU envelope versus ≈3.468 ms normally, but
4.984 ms before the first GPU timestamp versus ≈2.967 ms normally. Completion
wakeup stays around 0.13 ms. This is evidence of a start-delay problem, **not
proof that the OS or compositor caused it**. Driver submission, dependencies,
GPU scheduling/power behavior and competing work remain candidates. This
harness makes no swapchain presents, so these waits cannot simply be labeled
swapchain-acquisition stalls.

![Representative calibrated traces](../runs/environment-hotpaths-20261029/hotpath-timeline.png)

## CPU and material findings

The CPU-only profile measures about 8.3–11.7 ms of owner-thread room preparation
on these routes. A separate Aitos `perf` sample attributes 26.5% of cycles to
`render_native_capture_line`, 9.0% to `native_capture_tiles_line`, 7.3% to
`native_resolve_virtual_bg_span`, and 6.8% to `native_write_overlay_packed`.
This is primarily the editor's tile rasterizer, not gameplay emulation or the
game's GPU PPU producer. Optimizing it would help native editor sweeps; it must
not be advertised as the same saving in the shipping game.

Composition owner-thread CPU means are about 2.14 ms for clouds, 2.44 ms for
lava, 1.02 ms for swamp and 1.30 ms for gallery. `DioramaCloudLight_Sample`
accounts for about 2.1% of the whole Aitos CPU sample. CPU callback timers are
nested, not extra work to add on top of composition.

The GPU trace's long draw intervals occur around skybox lighting and lit
scenery/actor materials. Source inspection identifies these concrete candidates:

1. **Cloud-light field reuse.** `src/render/cloud_light_math.inc` evaluates
   opening geometry, trigonometry and exponential falloffs for each sampled
   fragment. `src/shaders/cloud_light.glsl` applies up to four regions in both
   skybox and scenery materials. Prototype a GPU-generated spatial light field
   with explicit refresh dependencies and reuse across materials. Preserve
   layer coordinates, parallax, cloud-edge contrast and mist/rainbow sampling;
   compare at multiple resolutions before considering any approximation safe.
2. **Transparent sprite/scenery work.** `actor_light.frag.glsl` evaluates receiver
   lighting and rim samples even after its filtered art is fully transparent.
   Start with a filtered-alpha fast path, then measure conservative sprite
   coverage. `present_diorama.c` intentionally disables existing coverage masks
   when interpolation is enabled; simply enabling those stale masks risks
   clipped motion. Any interpolated solution must cover both source frames,
   displacement, filtering and rim extent without introducing pixel readbacks.
   The shared editor's stand-in path also lacks those coverage masks, so its
   actor-plane cost is not representative of every production mode.
3. **Batch compatible uploads and eliminate empty submits.** Aitos averages
   32.7–33.7 Vulkan submits/frame, with about 0.95–0.99 ms inside submit calls.
   Swamp averages 14.8 calls / 0.51 ms; gallery 17.4 / 0.59 ms. These are actual
   driver calls, larger than the source-effect counters. `UpdateGpuTexture`
   still flushes/submits the renderer and then submits a separate copy for
   each texture. Group only updates whose ordering is compatible; preserve
   interleaved draw/update semantics and retained staging. This may help the
   editor and CPU-upload fallbacks more than the game's GPU-native producer.
   It is also a useful controlled test of the start-delay issue, not a promise
   to eliminate that delay.

Recommended order: measure the alpha fast path first as a narrow parity-tested
change; prototype cloud-field reuse for the largest Aitos-specific opportunity;
test upload/empty-submit batching against the calibrated start-delay trace.
Do not reduce Marahna fog detail without new evidence. Preserve CPU rasterizer
optimization as a separate editor project.

## Instrumentation, validation and evidence

`AR_BENCH_PROFILE` enables the portable thread-clock diagnostics in the native
benchmark. The Linux Vulkan proxy is a separate developer tool, never linked
into the game. Query reads occur only after the existing harness idle wait,
with availability checked; no extra per-draw waits or pixel readbacks are
inserted. The probe itself increases measured composition completion by roughly
0.06–0.18 ms in the median paired runs. Its ordinary CSV includes collection
overhead; the calibrated `device_idle` interval stops before query collection.

The initial capture smoke exposed the probe's unsupported screenshot-fence
path. The probe now closes after the final measured idle, before PNG readback
or teardown. Two 1280×800 Aitos/Marahna captures with and without instrumentation
have **zero changed pixels**. Those capture runs are not timing evidence.
Benchmark builds pass for Mac/Metal, Linux/Deck and Windows. Mac CPU-profile
runtime smoke passes; this audit adds no new Windows timing or DX12 runtime
claim beyond the preceding backend validation.

Accepted raw data, input hashes, configs, logs and telemetry:
`runs/environment-hotpaths-20261029/ablations/` and `profiles-final/`.
`hotpath-analysis.json` and `analyze.py` preserve the statistics and per-frame
decomposition; `shader-hashes.json` maps shader modules. `probe-parity/` contains
the PNG comparisons. Earlier `profiles/` and `profiles-v2/` are exploratory and
excluded (concurrent result retrieval / earlier frame-tagging diagnostics).
All production configuration and shaders are unchanged by this audit. Existing
unrelated work remains uncommitted and untouched.
