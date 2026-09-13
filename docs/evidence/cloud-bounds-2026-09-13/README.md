# Sky Palace: conservative cloud slice bounds

Implemented after committing the SIM GPU model-projection work as `fd73797e`.
The new optimization is enabled by default and affects **Sky Palace foreground
clouds**, not SIM's curved cloud shell or globe cloud-shadow sampling.

## Change

Each baked cloud slice now has a conservative rectangle around its nonzero
alpha, with a one-texel filtering margin. The presenter trims both geometry and
UVs to that rectangle and omits completely empty slices. Bounds are measured
once per atlas/light change, not per frame. The existing atlas pixels, lighting,
16-slice depth placement, global back-to-front order, drift, opacity, and
volumetric/single-layer setting are unchanged. No reduced-resolution cloud
target or density threshold is used.

Across the four baked banks, including their precomposited single-layer tiles,
the retained texel-centre rectangle area is **87,259 / 303,620 (28.7%)**. Seventeen
of those 68 atlas tiles are completely empty. This is an atlas support-area
measurement, not a claim that total screen overdraw or GPU time falls by 71%.

## Repeated all-effects benchmarks

Three same-binary pairs on each host, ordered full/bounded, bounded/full,
full/bounded. Each run plays the same 2,000-tick Palace replay with moving
clouds and the complete [all-effects profile](../all-effects-2026-09-12/profile.json),
including volumetric clouds, cloud shadows, atmosphere, lighting, high town
detail, interpolation, three helpers, and CRT/postprocessing. Refresh is
Unlimited and audio volume is zero in both variants. The first Palace
scene-entry window is excluded; all following 27 windows are retained.

| Host / output | Full rectangles | Bounded rectangles | Throughput gain |
| --- | ---: | ---: | ---: |
| Mac Metal, windowed 1440×896 | 226.77 FPS | 237.00 FPS | +4.5% |
| Steam Deck Vulkan, 1280×800 | 279.93 FPS | 334.13 FPS | +19.4% |

Values are medians of per-run presentation-cadence measurements, **not emulated
tick rates**. Mac ranges: 226.70–226.78 vs 236.95–237.14 FPS. Deck ranges:
279.41–280.03 vs 333.35–334.31 FPS. Both hosts' repeated results were stable in
this batch; the Mac had background indexing/media processes, recorded in the
raw evidence, and is not claimed to have been otherwise idle.

Deck cadence improved **3.572 → 2.993 ms** (16.2% shorter). The cloud CPU scope
fell 0.113 → 0.065 ms, and aggregate render CPU 1.662 → 1.353 ms. CPU scopes can
include driver blocking and change with the tick/re-present mix. These are not
per-pass GPU timer results, and nested scopes should not be added together.

All 12 timed runs completed without guard aborts or sampled enhanced-path
failure/fallback/rejection/opt-out/limit counters. Final emulated WRAM matched
across all variants. Legitimate mode-transition bridges are not interpreted as
failures. No installed game files or settings were modified on either host.

## Correctness and portability

- Three untimed image pairs per configuration: Metal volumetric and single-layer
  at 1440×896, plus Vulkan volumetric at 720×448. Clouds were frozen only for
  image comparisons, not timing. **Every changed channel differs by at most
  1/255**; maximum mean channel error is 0.00195/255. These small interpolation/
  blending round-off changes are not bit-exact image identity.
- The final default-enabled Mac replay, with no bounds override, produces three
  byte-identical screenshots compared with the tested opt-in path.
- Material tests cover empty slices, alpha=1, a bilinear support oracle, slice
  edges, the last atlas tile, padded pitch, invalid inputs with unchanged output,
  all 68 baked slice bounds, and lighting-independent support. Assertions are
  explicitly kept enabled in this test translation unit for Release builds;
  otherwise its assert-wrapped test operations would not execute.
- All three private/render backend boundary checks pass. The pure cloud module
  exposes only small integer support coordinates, with no renderer/device/frame
  dependencies or allocation. The presenter owns the cache and its existing
  reset/light-rebuild lifecycle. No SDK ABI, settings schema, shader, or platform
  backend contract changed. The existing layer uses bilinear filtering without
  mipmaps or anisotropy; those larger footprints would require wider bounds.

`AR_SIM3D_SKY_CLOUD_BOUNDS=0` keeps the full-rectangle diagnostic reference.
Windows/D3D12 runtime execution was not tested in this batch. The changes use
portable C and add no shader binary or runtime preparation.

## Reproduction / evidence

[Results](results.json), [runner](probe.py), [image comparator](compare.py), and
[validator/archive generator](summarize.py) are versioned. The generated
[raw archive](raw-evidence.zip) stays local under the repository ZIP ignore
policy; it contains logs, environment/guard/telemetry reports and input hashes,
not ROMs, saves, executables, or installed settings.

Builds use the preceding isolated model-projection source plus this scoped cloud
change, excluding unrelated dirty packaging/localization work. Mac: Release.
Deck: hermetic x86_64 Linux `-O2`, private test directory, fixtures copied and
hash-verified entirely on the Deck. Same binary on both sides of every pair:

- Mac A/B SHA-256: `6ef6f50a9d48ad2ce6753a38cef530f484bfcc146392e1665c1afab765fcf60d`
- Deck A/B SHA-256: `ceeec93e3855a5d82ad99fbc414d93c1234bd835b257fd01ebae515abd948afb`
- Mac default-check SHA-256: `b356670179668c4f85581a38ed572a1fdaad4fc1f144fbfe7410a72e882df448`

## Next target

Cloud-shadow receiver batching remains separate. Current soft shadows redraw
the same receiver geometry for each sample. Combining samples could reduce
vertex/raster work, but must preserve the exact opaque depth computation,
longitude seam handling, per-sample alpha cutoff, and layered transmittance.
Simply adding the weighted alphas is not equivalent to the existing blend.
No shadow batching prototype or changed shadow appearance is included here.
