# Extended action view rendering audit

Audited 2026-09-27, against `32fee87d`, after the regional HLE closeout.
The optimization changes are in the runtime PPU; game simulation, object
activation, OAM allocation, HLE ownership, and authored scene data are unchanged.

## Findings and changes

Extra rows already enter the packed scanout/compositor. Two background-fetch
cases did unnecessary work inside that path:

1. **Margin-only virtual tilemaps.** If ROM/world tiles differ from the resident
   VRAM ring, the game correctly keeps native ownership of the authentic screen.
   The renderer also required `IncludeAuthentic` to batch virtual margins,
   sending every added-row pixel through `sample_bg`. The flag now controls
   source ownership without disabling batching. Synthetic rows use world tile
   spans; authentic rows retain native VRAM in the center, and mirror/repeat
   margins retain their native source. Raw horizontal margins use world spans.
2. **Mosaic transitions.** Every pixel in an extended-row mosaic group repeated
   the same fetch. The packed fallback now samples once per display-space group
   and replicates its packed result. It splits at native/world ownership edges,
   retains negative-coordinate phase, and applies windows per destination pixel.

Neither change substitutes different pixels or drops effects for performance.

## Path audit

| Stage | Extended-row behavior |
| --- | --- |
| Frame plan | `ActRaiser_ResolveVerticalMarginPolicy` clamps each side to 64 rows and clips each BG against its own camera/world extent. Maximum capture height is 352 versus 224 native rows. |
| Scanout | `run_ppu_scanout` calls `ppu_runMarginLine`; it reaches the same `render_native_fast_line` / `render_native_capture_line` as ordinary rows. Added rows do not run extra HDMA or simulation ticks. |
| World BG1/BG2 | `native_resolve_virtual_bg_span` uses `lookup_span`, decoded tile-row caches, and the existing whole-tile kernel where window/band policy permits. Margin-only bindings now qualify too. |
| VRAM layers | `native_resolve_vram_bg_span` walks tiles in added rows. BG3 width policy and per-layer clipping are retained. |
| Mosaic | The general fetch preserves mosaic semantics, once per group rather than once per pixel. Packed composition/capture remains active. |
| Sprites | `get_obj_sample_cache` and `native_obj_cache_pixel` reuse scanline sprite data. Added pixels do not scan all OAM entries individually. Native-height component priority remains intact. |
| Capture | Main/subscreen composition, priority bands, full-add exports, and captures share packed source buffers; height alone does not select the reference renderer. |
| Upload | `Diorama_Upload` uses the existing persistent plane textures and dirty-region mirrors. Unchanged planes skip uploads; sparse coverage scans only changed planes. Mirrors retain allocation capacity when height shrinks. |
| GPU composition | Taller captures use the same layer grid, shader/DOF/supersample paths, and sparse geometry filtering. The extra rows do not generate a separate draw per row. |

Correctness fallbacks remain for unsupported combinations such as visible
large-tile virtual maps, Mode-7 mosaic, and special combined capture policies.
Their eligibility depends on format/effects, not vertical extension. They were
not observed as whole-row fallbacks in the Aitos action replay below.

There is still ordinary work proportional to the visible area: 64 extra rows on
both sides can add 57% more source pixels. Upload payload and shader work can
grow accordingly. Compact supersample/DOF targets are recreated when their
required dimensions change, including near world edges; this is a possible
transition cost, not a per-row software fallback. Steady-state timing must be
kept separate from entry/resize spikes.

## Validation and evidence

Evidence is retained locally under `runs/extended-view-performance/` (ignored
builds, private replay saves, logs, captures, and audit scripts).

- Runtime suite: **41/41 passed**, including the expanded PPU parity matrix.
- Portable non-SIMD PPU implementation: parity suite passed as well.
- Game rendering integration checks: **31 passed**, including the GPU frame
  generation check rerun with display access after its sandbox-only skip.
- New coverage uses the maximum 64 rows per side, full and margin-only bindings,
  all fill policies, custom/hardware bands, windows, color math, live scroll,
  finite extents, explicit native tile fallback, and mosaic sizes 2 through 16
  (**440 virtual-capture comparisons**, in addition to existing PPU cases).
- Each eligible synthetic row must invoke the span provider. This assertion
  fails on the pre-change renderer for margin-only bindings.
- Aitos strict comparisons at **0, 32, and 64 rows**: **27 byte-identical final
  composite captures per setting** and identical final WRAM. Captures span
  entry, traversal, and the next-room transition. Instrumented and ordinary
  control builds were separately checked for pixel/WRAM identity.
- Fillmore Act 1 at **64 rows**: **17 byte-identical composite captures** and
  identical final WRAM. Both builds rendered all **383,429 added rows** through
  packed composition, with **zero** scalar background fetches or full-reference
  rows. This uses the retained local `walk7-centaur.rec` traversal, a private
  versioned SRAM seed, and the existing debug warp to enter Fillmore.

The scratch PPU census counts actual branches separately for native and added
rows, avoiding production per-pixel profiling overhead:

| Aitos, 64 rows, 3,000 presentation ticks | Before | After |
| --- | ---: | ---: |
| Added rows using packed composition | 160,598 | 160,598 |
| Added rows using full reference composition | 0 | 0 |
| Added-row mosaic sampler calls | 1,031,680 | 515,840 |
| Added-row non-mosaic scalar sampler calls | 0 | 0 |
| Added-row virtual span calls | 313,609 | 313,609 |

This route's preflight reported zero mismatches: it exercises full-world
bindings, not the margin-only case. Margin-only behavior is covered explicitly
by the runtime tests and isolated renderer benchmark, rather than claimed as a
speedup observed in this route.

## CPU timing

The production builds use the same ARM64 `-O2` / ThinLTO settings. Eight Aitos
64-row runs alternate control/candidate/candidate/control twice, with real GPU
presentation at 2160×1344, fixed replay inputs, unlimited refresh, no captures,
and no scratch PPU counters. Final WRAM matches across all eight runs.

| CPU stage, median ms/present | Before | After |
| --- | ---: | ---: |
| Total render CPU | 3.4113 | 3.4059 |
| PPU scanout | 2.8498 | 2.8507 |
| Texture upload | 0.4072 | 0.4053 |
| Presentation | 0.1026 | 0.1020 |
| Layer mesh (nested in presentation) | 0.00146 | 0.00144 |

The total ranges overlap: 3.3883–3.4242 ms before, 3.3123–3.4490 ms after.
There is no demonstrated steady-gameplay speedup or regression. PPU scanout is
the dominant CPU cost; changing the layer grid is not supported by these data.
The removed work concerns transitions and margin-only bindings, which the
settled full-world windows deliberately do not exercise.

The backend reported no whole-view fallback or failed presents in these trials.
Steady windows retained skipped uploads and zero upload-mirror reallocations.
`present/wait` was about 4.46 ms and includes driver submission/waiting; it is
neither CPU rasterization nor a GPU execution measurement.

An isolated PPU benchmark exercises the otherwise rare cases deliberately:
one world-bound BG, other native layers, windowing, main/subscreen composition,
and a capture surface 496 pixels wide. It measures 150 frames after 10 warmup
frames at 0/32/64 rows per side, with four alternating trials per build. These
are synthetic renderer timings, not game FPS or GPU measurements.

| Maximum extension, ms/frame median | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Full-world binding | 3.2382 | 3.1833 | within timing variability |
| Margin-only binding | 4.8897 | 3.1679 | 35.2% |
| Seven-pixel mosaic | 5.1334 | 3.2201 | 37.3% |

Margin-only trial ranges were 4.6838–4.9913 ms before and 3.0798–4.6167 ms
after; mosaic ranges were 4.9503–5.4600 and 3.1912–3.2981 ms. Host scheduling
noise remains visible; the medians are not a hardware-independent guarantee.
The 0- and 32-row cases also improved for both fallback scenarios. Corrected
benchmark source/results are `microbench.c`, `build_microbench.py`,
`measure_micro_valid.py`, and `micro-valid-results.json` in the evidence folder.
The initial `micro-*` trials are discarded: their setup reset the virtual
binding while configuring margins. The corrected benchmark installs the
binding afterward and asserts actual provider use.

## Reproduction

Build an unchanged control before editing the runtime, then build the candidate
with the same compiler/options. Use private copies of the binaries, an empty
config, the versioned SRAM/replay fixture, and identical presentation settings.
For the maximum-height comparison:

```sh
python3 tools/compare_pipeline_performance.py \
  --control runs/extended-view-performance/control-game \
  --candidate runs/extended-view-performance/candidate-game \
  --config runs/extended-view-performance/empty-config.ini \
  --manifest tests/fixtures/benchmark/action-vertical-checkpoints.json \
  --checkpoint aitos-vertical-64 \
  --replay tests/fixtures/benchmark/aitos-r4-natural-inputs.json \
  --scene 'Action 3D' --quit-frames 3000 \
  --output /tmp/actraiser-vertical-timing-new
```

Add `--verify-only --require-scene 'Action 3D' --capture-from 1330
--capture-to 2650 --capture-every 50` for exact image/memory comparisons. Use a
new output directory each time. GPU access is required even for headless-video.
The benchmark uses eight alternating runs and excludes the first matching
reporting window; timing scopes are CPU wall time, **not GPU timestamps**.

To run the independent runtime checks:

```sh
cmake -S snesrecomp-go/runtime -B /tmp/actraiser-runtime-audit \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DSNESRECOMP_ENABLE_SIMD=ON
cmake --build /tmp/actraiser-runtime-audit -j 6
ctest --test-dir /tmp/actraiser-runtime-audit --output-on-failure -j 6
```

The optional `[pipeline-work] fallback` metric concerns whole-view fallbacks;
it cannot establish tile-fetch path coverage. Use the PPU parity/performance
assertions or the scratch branch census for that question. See
[performance-overlay.md](performance-overlay.md) for the timing scopes.
