# Environment rendering prototypes — 2026-10-29

Follow-up to [the hot-path audit](environment-hotpaths-20261029.md).
Keep the sprite shader's filtered-alpha early return. Do not keep the scenery
early-return variants or the upload-batching scope: their performance gains
were inconsistent. All rejected implementation changes were restored from
pre-experiment source copies, preserving the existing unrelated work.

## Method

- Steam Deck / RADV VANGOGH / Vulkan, 1280×800. Four routes: Aitos clouds and
  west lava, Marahna swamp doorways and temple gallery.
- Each execution warms the complete 240-frame camera/animation route, then
  repeats it for 240 timed frames. Three alternating control/candidate pairs
  per route. Four experiments, 96 timed executions total.
- Wake and verify the Deck display before every execution; check brightness
  and power throughout. No accepted run had a sleeping display, crash or late
  shader compilation. No captures or result transfers during Deck timing.
- Tables use the median of three per-run means/p99s. They are not pooled
  percentiles. Values below are composition/submission plus remaining GPU
  completion, unless explicitly labeled as whole-harness completion.
- Same room assets, effects and `review-layers.ini` fixture as the audit.
  Exact hashes, CSVs, logs, sensor samples and binaries are retained under
  `runs/environment-prototypes-20261029/`.

The harness serializes the editor's CPU tile rasterizer and GPU completion.
It does not measure the game's GPU PPU producer, interpolation, CRT, gameplay
logic or swapchain pacing. These results do not certify 90 Hz gameplay.

## Retained: skip lighting for fully transparent sprite fragments

`actor_light.frag.glsl` now returns transparent black after the artwork's
filtered alpha is confirmed to be zero. It avoids two receiver-light samples,
up to four rim samples and their lighting math on empty fragments. The art
and receiver-attribute filtering preceding that decision is unchanged.

Testing the unfiltered center texel would incorrectly cut off depth-of-field
fringes, so the return happens after filtering. Remaining texture samples use
explicit LOD 0, appropriate for these single-level captures, rather than
depending on implicit derivatives after a varying early return. The WebGL
export includes the same orientation shim for these explicit-LOD art samples.

No pass, texture, readback, binding, uniform layout or new shader variant was
added. The existing actor-material preparation/warmup path compiles it with
the other shaders. Vulkan SPIR-V, Metal and DXIL headers were regenerated.

| Route | Control mean / p99 | Sprite fast path mean / p99 | Mean change |
| --- | ---: | ---: | ---: |
| Aitos clouds | 5.137 / 7.038 ms | 4.961 / 6.869 ms | −0.176 ms |
| Aitos west lava | 6.197 / 7.534 ms | 6.045 / 7.328 ms | −0.152 ms |
| Marahna swamp doorways | 6.035 / 8.243 ms | 6.168 / 8.098 ms | +0.133 ms |
| Marahna temple gallery | 3.045 / 3.577 ms | 2.837 / 3.422 ms | −0.208 ms |

The repeatable Aitos/gallery savings are roughly 2.5–6.8% of this measured
renderer interval, not of the entire game frame. Swamp results remain noisy:
its three paired mean changes are −0.500, +0.281 and +0.211 ms. Do not claim a
swamp speedup or that this fixes the audit's pre-execution delay. Sprite-plane
coverage and interpolation can change the saving in the game.

## Rejected: early return for transparent scenery

The unconditional version tested filtered priority/DOF coverage before color
grading and cloud-light evaluation, using explicit LOD for the subsequent
light-map sample. It was paired against the retained sprite-only version.

| Route | Sprite-only mean | Sprite + scenery mean | Mean change |
| --- | ---: | ---: | ---: |
| Aitos clouds | 4.998 ms | 4.910 ms | −0.088 ms |
| Aitos west lava | 6.006 ms | 5.825 ms | −0.181 ms |
| Marahna swamp doorways | 5.871 ms | 5.354 ms | −0.517 ms |
| Marahna temple gallery | 2.822 ms | 2.943 ms | +0.121 ms |

A second version restricted the return to materials with cloud lighting or
color grading. It did not rescue the tradeoff: means changed by −0.041 ms for
clouds, +0.071 ms for lava, −0.273 ms for swamp and +0.149 ms for gallery.
These experiments do not isolate whether branch divergence, instruction
layout, register pressure or sampling caused the different outcomes.

Both scenery variants were removed. Their patches and generated shader
artifacts remain with the run evidence; there is no room-specific switch in
production. The conditional version also had eleven one-channel-step pixel
differences across the two Deck swamp comparison captures. The retained
sprite-only version had none.

## Rejected: upload-only batching scope

This experiment collected independent CPU texture uploads before room
composition into one GPU copy pass/command buffer. It preserved ordering by
submitting previous SDL draws first, flushing before a repeated destination
texture or capacity limit, and closing the scope before drawing/readback.
It retained the existing write-only staging and DX12 alignment behavior.

Both arms used the same executable with the same sprite/scenery shaders;
only `AR_BENCH_BATCH_UPLOADS` differed. Because batching changes overlap, judge
the whole-harness interval rather than just the smaller CPU upload timer.

| Route | CPU preparation off → on | Whole-harness mean off → on | Whole-harness p99 off → on |
| --- | ---: | ---: | ---: |
| Aitos clouds | 9.099 → 8.954 ms | 14.030 → 13.981 ms | 15.922 → 15.855 ms |
| Aitos west lava | 8.712 → 8.618 ms | 14.529 → 14.569 ms | 16.040 → 15.975 ms |
| Marahna swamp doorways | 11.682 → 11.560 ms | 17.700 → 17.814 ms | 20.853 → 21.103 ms |
| Marahna temple gallery | 8.398 → 8.291 ms | 11.340 → 11.240 ms | 12.698 → 12.427 ms |

The scope saved preparation time but also delayed GPU work; completion waiting
increased on the Aitos/swamp routes. It did not yield a consistent end-to-end
benefit or resolve swamp's tail. Remove the API/backend state and benchmark
flag rather than leave an unused production experiment. The complete patch,
including ordering tests, is archived as `upload-batch-prototype.patch`.
This does not rule out batching other render commands or eliminating genuinely
empty submissions; those were not measured by this scope.

## Correctness and portability

- Retained sprite change: **zero changed pixels in 32 full-frame comparisons**.
  Four poses, 1280×800 and 1920×1080, on Deck Vulkan, Windows Vulkan, Windows
  DX12 and Mac Metal. Captures were separate from performance measurements.
- Native material oracles pass on those four backend/device combinations,
  including receiver isolation, moving artwork, dimming, partial alpha,
  depth-of-field fringes, rim enhancement and priority composition.
- The batching experiment passed padded odd-width ARGB/ABGR, partial/repeated
  upload and interleaved draw/update ordering tests on all four combinations.
  Correctness alone was not sufficient to retain it.
- Actual WebKit WebGL2 compilation: all seven exported fragment shaders and
  the baseline actor shader compile/link. Eight actor comparisons covering
  blur, rim strength and source orientation are byte-identical. The explicit
  source flip also matches the expected vertically mirrored result.
- Five compositor/export tests pass; final generated actor/scenery headers
  pass `build_shaders.py --check`. The native material test also passes after
  restoring the rejected scenery/backend changes.

The images demonstrate parity at the sampled poses, not an exhaustive proof
of every gameplay/interpolation state. No effect intensity, density or visual
quality was reduced to obtain the retained saving.

## Cleanup after prototype review

The benchmark now has one teardown path, checked numeric/route inputs,
overflow-safe camera interpolation, checked output writes and shared preview
declarations. The optional Vulkan probe uses a named frame-close protocol and
resets its state on device destruction. Its diagnostics remain outside the game.
The Python runner retains logs and telemetry on failure/timeout and rejects
incomplete or inconsistent timing CSVs. The misleading `cpu_ms` column is now
`submit_elapsed_ms`: it measures elapsed preparation/submission time, not CPU
usage. Existing timing summaries remain readable.

Validation: seven benchmark-tool test cases and five compositor/export tests
pass; the native benchmark builds for macOS, Linux and Windows. Eight additional
captures (Aitos clouds and Marahna swamp on all four backend/device combinations)
match the preceding build pixel for pixel. A complete 240-frame Deck sweep
preserves camera positions, animation clocks, primitive and vertex counts, with
no late shader compilation. The probe-enabled capture also matches the
unprofiled capture exactly. Controlled asset, receiver, trace and PNG failures,
including a buffered disk-full write, exit cleanly with the probe finalized.
All 132 previous timing traces pass the
stricter CSV validation. Evidence is in `runs/environment-cleanup-20261029/`.

This cleanup adds no render passes or production shader changes and makes no
new performance claim.

## Desktop regression checks

The retained sprite-only change also received three warmed alternating pairs
on two routes per backend: 12 additional Metal runs and 24 Windows runs, with
DX12/Vulkan order alternating too. No late compilation or run failure occurred.
These are the same composition-completion statistic used in the Deck table.

| Backend / route | Control mean / p99 | Sprite fast path mean / p99 |
| --- | ---: | ---: |
| Windows DX12 / Aitos clouds | 2.056 / 3.022 ms | 2.054 / 2.895 ms |
| Windows DX12 / temple gallery | 1.177 / 1.383 ms | 1.153 / 1.363 ms |
| Windows Vulkan / Aitos clouds | 1.581 / 2.684 ms | 1.585 / 2.742 ms |
| Windows Vulkan / temple gallery | 1.014 / 2.436 ms | 1.016 / 2.440 ms |
| Mac Metal / Aitos clouds | 4.365 / 7.720 ms | 3.718 / 5.814 ms |
| Mac Metal / temple gallery | 1.965 / 3.468 ms | 1.874 / 3.661 ms |

Windows's display is off; use these paired results only to check for a large
backend-specific regression, not as display pacing or Deck performance
predictions. The candidate is essentially neutral there. Metal's mean improves
on both routes, but gallery p99 worsens by 0.193 ms; do not claim a universal
tail-latency improvement. No other local GPU test ran during the Metal sweeps.

## Next experiment

Target Aitos's cloud-light field evaluation, the larger remaining repeatable
cost from the audit. Prototype computing its four spatial weights on the GPU
and reusing them across consumers. Keep the original-art luminance/gray-cloud
response in the consuming material: baking a final generic RGB light map would
change the rim/halo/streak behavior. Preserve region coordinates, parallax,
animation phase and source-dependent filtering.

Measure the added field pass, bandwidth and synchronization against the saved
per-fragment math before accepting it; compare sharp cloud edges and waterfall
lighting at both resolutions. The earlier CPU-precomputed-opening approach
was already rejected and should not be repeated as a new idea.
