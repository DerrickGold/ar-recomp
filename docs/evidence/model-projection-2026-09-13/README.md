# SIM model projection: GPU default

Camera-independent SIM town model geometry is now retained on the GPU. Camera
movement changes uniforms instead of reprojecting and uploading every model
vertex on the CPU. Authored models, adaptive LOD, lighting, contact geometry,
foundations, bridge depth envelopes, and pixel-clean rendering remain enabled.
Cloud rendering and graphics quality settings were not changed.

## Steam Deck results

Same executable, explicit CPU/GPU override, Aitos `D7-voxel-town` replay,
1280×800 Wayland/Vulkan, Unlimited refresh with interpolation, three helper
threads, and the complete [all-effects profile](../all-effects-2026-09-12/profile.json).
The profile includes High/Adaptive models, Materials + AO, shadows, particles,
curved clouds, globe underlay, haze, lighting, postprocessing, and CRT effects.
Audio volume is zero in both variants.

Three dynamic-camera pairs were ordered CPU/GPU, GPU/CPU, CPU/GPU. Each run
played 2,400 emulated ticks. Aggregates omit only the first Town 3D scene-entry
window and retain the following 24 windows, including subsequent expensive
work. Values below are medians of per-run frame-weighted measurements.

| Dynamic-camera metric | CPU projection | GPU projection | Change |
| --- | ---: | ---: | ---: |
| Presentation throughput | 120.56 FPS | 146.25 FPS | +21.3% |
| Render CPU per present | 7.248 ms | 5.807 ms | −19.9% |
| Model preparation/projection scope | 1.668 ms | 0.258 ms | −84.5% |
| Depth geometry uploaded per present | 2.488 MiB | 0.998 MiB | −59.9% |

CPU throughput ranged from 118.37–120.71 FPS; GPU throughput from
146.09–151.00 FPS. These are actual presentation-cadence measurements, not
headless emulation rates. FPS is not the emulated game tick rate. Nested timing
scopes must not be added together; the model scope includes cache/publication
work and some terrain collection, not just matrix multiplication. Depth upload
traffic includes remaining terrain and other geometry, not only town models.

Two fixed-camera pairs were effectively unchanged: 160.12 → 160.99 FPS
(+0.55%). This is expected because the CPU path already caches a settled
projection. The worst measured one-second window improved from a median
62.45 → 79.00 FPS for dynamic cameras, and 62.42 → 92.62 FPS for fixed cameras.
Those are window averages, **not** 1% lows; individual entry hitches can remain.

All ten Deck timed runs completed without a memory guard abort or sampled
enhanced-path failure, fallback, rejection, opt-out, or capacity-limit counter.
This does not assert that legitimate one-frame mode-transition bridges never
occur. Final emulated WRAM matched across all timed variants.

## Local tests and visual fidelity

Twelve additional Mac timed runs (six fullscreen and six windowed) also showed
large reductions in model CPU work and depth upload traffic. Both cohorts had
substantial throughput drift, including changing present waits. They are
preserved in the evidence but excluded from the headline FPS gain estimate.
The evidence does not establish a specific cause for that host drift.

Six separate untimed Metal replay runs captured CPU/GPU comparisons in Aitos,
low-angle Aitos, and the Fillmore effects fixture, at frames 1200/1500/1800.
Cloud drift and reactive camera movement were disabled only for these image
comparisons; they remained enabled in the dynamic performance runs. All nine
1440×896 image pairs retained matching final emulated state. At most 0.0865% of
pixels changed, with at most 0.0346% differing by more than 8/255 in any channel.
Differences are small model-edge/shading rasterization changes, not bit-exact
identity. Maximum mean absolute channel error was 0.00537/255. Inspected Aitos
images retain the town models, foundations, volcano, and actor composition.

The default-enabled build was then run **without** the model-path environment
override. Its three Aitos screenshots were byte-identical to the validated
opt-in GPU screenshots.

Validation completed:

- Metal and Steam Deck Vulkan depth suites, including 36 new synthetic
  reference comparisons for extrusion, offsets, pixel snapping, perspective,
  side/eye clipping, bridge alternate depth, and ordering against other solids.
- Viewport changes without source reupload; reset and republish; input-copy
  ownership; queued update/destroy safety; malformed and overflowing inputs.
- Metal shader loading and all three private/render-layer boundary checks.
- Generated MSL, SPIR-V, and DXIL consistency check. Windows/D3D12 runtime
  execution was not tested in this batch.

These fixtures do not cover every populated town at every LOD/quality level.
Bridge depth semantics have synthetic GPU coverage, not a separate full-game
Marahna visual fixture. Existing grey cliff triangles are unchanged.

## Implementation and contracts

The private depth adapter accepts generic source positions, displacement axes,
colors, and a transform. It does not know about towns, palettes, model kinds,
game clocks, or LOD policy. SIM owns source selection, placement, lighting,
revision keys, and cheap whole-object culling. The renderer maps its model
families onto numeric axis slots; no model identity crosses the adapter seam.

Publication is bounded to 64K source quads. Only the selected object/LOD set is
retained, not every LOD of a developed town. Camera-independent source keys
include scene, style, lighting, terrain height, and selected objects/LODs.
Camera and viewport changes normally update draw uniforms only. Changes in
visibility or LOD can still require publication. Backend resources remain
owned by the existing reset/lifetime contract. No SDK ABI, settings schema,
runner API, or platform handle was added to the SIM layer.

The new shader is prepared with the other model pipelines at video boot/reset;
there is no new game-time shader compilation path. Hardware clips partially
visible and behind-eye geometry. Clipping alone does not force CPU projection.

GPU projection is the default. `AR_SIM3D_TOWN_GPU_MODELS=0` retains an explicit
CPU reference for diagnostics. The shared CPU implementation also remains a
complete recovery path if optional bounded source publication fails. Such
rejections did not occur in the measured runs. This is not a new native-view
fallback policy. Existing helper-thread support is unchanged.

## Evidence and reproduction

- [Machine-readable results](results.json) include every run, ranges, image
  difference counts, input/log hashes, and the default-enabled check.
- [Raw evidence archive](raw-evidence.zip) contains logs, environment snapshots,
  guard/telemetry reports, input hashes, and GPU test results. No ROM, save,
  game executable, or user-installed settings are included. This generated
  archive remains local under the repository's existing ZIP ignore policy;
  the results and reproduction scripts are versioned.
- [Local runner](local_probe.py), [Deck runner](deck_probe.py),
  [image comparator](compare_images.py), and [summarizer](summarize.py) build on
  the previous [all-effects probe](../all-effects-2026-09-12/probe.py).

Builds use isolated source from `e1699c9e` with this model-projection change
overlaid; unrelated dirty packaging/localization work was excluded. macOS used
an isolated Release build; Deck used the hermetic x86_64 Linux `-O2` build.
Both sides of each timed comparison use the same executable. The Deck candidate
SHA-256 is `e61bf2be0c482aca9bb3b103719e69c7ac7edd3c16467c70d5815176872831f8`.
The final default-enabled Mac binary SHA-256 is
`b9f08877f89a7b1cbf7bdba6b1e19f856eb8b312f2152a9c5fc7920f42901263`.

Deck tests ran only in a private test directory, using hash-verified copies of
fixtures already on the Deck. Installed game files and settings were untouched.
The previous probe's `--visible` mode now honors an explicit window-mode/scale
override; its existing borderless/3× defaults remain unchanged.

## Next: clouds

The model path is complete. The previous component-isolation measurements
identified separate costs in cloud shadow receiver redraws and Sky Palace
foreground translucent overdraw. Those remain the next targets. This change
does not claim a cloud speedup or lower cloud quality to obtain the SIM gain.
