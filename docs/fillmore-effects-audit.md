# Fillmore effects portability and performance audit

2026-09-28. Scope: Act 2 capture, landing history, cave/temple/tower geometry,
statue-orb accents, water sampling, and their flat/Diorama presentation paths.

**Post-audit decision:** the water-only sampler was removed after its controlled
On/Off replay showed negligible visual benefit (see
[water visibility verification](fillmore-cave-effects.md#water-visibility-verification)).
Its 880 KiB scratch and CPU resampling cost are gone. Remaining water glints,
ripples and mist are unchanged; actual submerged-scene refraction remains planned.
Sampler measurements below are historical evidence, not costs of the current build.

No platform-specific dependency, unbounded particle workload, or new gameplay
memory write was found. The implementation is suitable for the project's
Windows, macOS, Linux and Steam Deck targets at the source/API level. This is
not a claim that every target GPU or compiler was exercised locally.

## Changes from this audit

- Pool and waterfall coordinates now live in one immutable catalogue shared by
  glints, ripples and splash mist. Future placement changes cannot
  silently update just one of those paths.
- Capture uses explicit room/layer/projection descriptors instead of assigning
  behavior through array indices and nested ternaries.
- Dust grid origins now divide in floating point before `floorf`. Integer
  division had already truncated negative camera-relative coordinates toward zero.
- The water-only sampler was initially optimized and tested at 640×352, including
  unaligned input and winner masks. Those source files, upload hooks and dedicated
  tests are now removed following the visual review.
- The surviving cave-mask uploads retain regression coverage for bounded update
  calls, unchanged frames, resize reuse, unaligned/padded input and source immutability.

## Portability and ownership

The modules use C11, fixed-width state, unsigned clock/hash arithmetic and the
existing render-device API. Pixel loads use `memcpy`; packed ARGB is interpreted
as numeric `uint32_t` values, matching the runner/SDL pixel-format contract.
There are no SIMD intrinsics, variable-length arrays, new shaders, OS APIs or
per-frame heap allocations in the cave modules. Existing math-library linkage
handles MSVC separately from platforms requiring `libm`.

The producer only observes native objects and collision data. Captured records
contain values rather than WRAM pointers; native RNG and allocation pools remain
untouched. Contact history, settled patches and geometry have explicit limits.
Reserve checks fail closed if a cosmetic batch exceeds its capacity.

Large geometry workspaces are static and used by the presentation thread.
Before sampler removal, the largest individual optimized stack frame in the
eight audited modules was 2,912 bytes on arm64, in the now-removed sampler.
Upload, coverage analysis and frame-generation endpoint copies now consume native
pixels directly; no water scratch or borrowed sampler pointer remains.

Alpha/additive effects use existing geometry submissions. Surface illumination
uses the existing optional destination-color blend. A backend rejection disables
only that light pass, is not retried every frame, and is reset with the renderer.
Tests cover that fallback and preservation/restoration of rendering state.
Flat masking samples the existing winner-mask textures without a new full-screen
render target. Diorama submits effects at their owning layer depths.

## Measurements and validation

Optimized CPU-only probes on the local Apple Silicon Mac, before sampler removal:

| Work | Measured cost |
| --- | --- |
| Scene/environment capture at seven saved camera positions | 0.55–0.79 µs |
| All environmental geometry layers, 10,000 phases at each position | 4.7–15.3 µs; peak 1,011 vertices |
| Synthetic water-heavy flat sampling, 496×272 / 640×352 | 0.264 / 0.445 ms |
| Synthetic water-heavy isolated BG2 sampling, same extents | 0.318 / 0.531 ms |
| Completely transparent submerged BG2, same extents | 0.083 / 0.133 ms |
| No pool intersects the upload | Approximately 0.01 µs |

These measure separate CPU stages, not whole-frame or GPU duration. The saved
camera probe does not simulate a worst-case burst of simultaneous landings;
bounded saturation tests cover those geometry capacities. The water workload
uses changing synthetic colors over almost the entire upload, not a claim about
typical gameplay. Its cost scales with native capture pixels, not output-window
resolution. Water was the largest measured CPU enhancement cost in these probes;
the visual review subsequently justified removing it. These synthetic timings
are not a measured whole-frame speedup from removal.

The Release application builds. Eight focused capture, geometry, presenter,
render-device, SDL-state, projection and PPU-pipeline tests pass. Observer,
geometry and presenter suites also pass ASan/UBSan. All eight audited effect
translation units compile to arm64 and x86_64 macOS objects under strict C11,
pedantic warnings and a VLA prohibition. The existing missing-field-initializer
warnings in unrelated spell tables are suppressed in that strict probe.
Style, private-header, render-boundary and whitespace checks pass.

Before removal, every output pixel matched the pre-audit sampler across 2,048
synthetic frames spanning the complete 256-tick period, both tested extents,
flat/isolated coverage, transparent water and off-pool views. Local probe sources,
timings, comparison results and the audit-only diff are in
`runs/fillmore-effects-audit/` (ignored development evidence).

Remaining validation is native Windows/MSVC and D3D12, Linux/Steam Deck Vulkan,
and Windows/Linux ARM64 runs where supported. The Intel Mac check is object
compilation, not a linked application or GPU execution. GPU duration and
cross-device visual parity were not measured in this audit; prior local gameplay
captures are documented in `fillmore-cave-effects.md`.

## Steam Deck and SDL/D3D12 constraints

Source portability alone does not establish driver/library performance. The
following checks explicitly retain lessons from the project's earlier work:

| Prior constraint | Current effect path and guard |
| --- | --- |
| High texture-update call overhead | `upload_rect_run.h` records historical Deck/Vulkan measurements of 37.3 µs/call versus 4.6 µs on Mac/Metal. Commit `ebeb4e07` documented SDL 3.4.12/3.4.16 allocating a transfer buffer per update, backed by a D3D12 committed resource. Native cave-plane uploads and flat masks use `PresentationUploadMirror`: zero updates for unchanged pixels, otherwise one bounding rectangle per texture, regardless of scattered changed rows. These historical numbers are not timings of today's SDL on every device. |
| Transfer alignment and hidden copies | The existing manual SIM atlas uploader pads D3D12 rows to 256 bytes and offsets to 512 bytes. Cave uploads use `SDL_UpdateTexture`, so SDL owns their staging layout; the manual-atlas alignment helper does **not** remove that library cost. Avoid replacing one upload with many tiny rectangles merely to reduce visible payload bytes. |
| Optional GPU feature/resource limits | Output-device creation retains reduced D3D12 resource-slot support and does not request unused shader clip distances, depth clamping, indirect first-instance draws or anisotropy. Cave effects add no custom shaders or increased per-draw resource requirements. Their masks use one sampled texture per draw; other geometry is untextured. |
| Slow/invalid reads from upload memory | Mask conversion and dirty comparisons read ordinary CPU-owned pixels. They do not read back the GPU, read a write-only texture lock, or read mapped upload memory. |
| Platform runtime and pacing differences | The historical Deck fixes include GNU C/POSIX visibility, usable SDL video-driver diagnostics and gamescope pacing. The effect modules add no POSIX dependency, device creation, swapchain acquisition, fence wait or extra present. Existing packaged runtime and pacing choices remain in charge. |

`CaveMaskUploadBudget` exercises the real BG1/BG2 mask conversion and upload
mirror at the maximum flat-mask extent. Disjoint changed rows and regions cost
one update per texture; unchanged masks cost zero. Shrinking and restoring the
capture reuses each texture; separate upload-mirror tests cover allocation reuse.
The test also uses unaligned/padded source buffers and verifies immutability.
This replaces the deleted sampler's `WaterUploadBudget` test. Presenter,
upload-mirror, rectangle-coalescing, upload-metrics, SDL-state and D3D12-layout
coverage guards application calls and layout arithmetic, not undocumented
allocations inside an arbitrary SDL build.

Removing the sampler also removes its ability to dirty an otherwise unchanged
BG2 texture. Native water animation still incurs uploads; broad translucent
lighting can consume GPU fill bandwidth despite small geometry counts. These
remaining costs need device measurements.

For native acceptance, compare effects Off/On along the same cave waterfall,
temple-jump/orb and boss routes, in flat/Diorama views at 32/64 extension. Use
the shipped SDL build and record its version, GPU driver/device, output size,
refresh/limiter and Deck power limit. Test Deck through gamescope at 1280×800;
test Windows with D3D12 explicitly selected, including a supported integrated
GPU. Use the same settings in alternating runs and exclude entry/loading and
video capture from steady-state timings. Check:

- CPU capture, upload and presentation times, displayed p95/max frame intervals,
  upload calls/bytes/skips, mirror reallocations and failed/fallback counts.
- No growing upload-call count with waterfall count, particle density or added
  rows; stable view size should have zero steady-state mirror reallocations.
- Pause/re-present, room changes and resize/device reset, with separate timing
  for transition spikes and correct recovery of masks and optional lighting.
- GPU execution/overdraw with native GPU tools when available. The application's
  `present/wait` duration includes pacing/submission and is not a GPU timestamp.

The existing `deckbench-gpu` is useful for isolating library per-call/per-byte
costs with the same SDL version. It is an offscreen microbenchmark, not evidence
of gamescope frame pacing, and its default device request is stricter than the
game's compatibility request. Neither it nor Mac CPU timings replaces these
native acceptance runs, which remain pending.

Post-removal validation: Release build, 12 focused tests and presenter ASan/UBSan
pass. A 64-row Diorama replay matches all 240 frames and four WRAM snapshots from
the prior sampler-disabled control; a 64-row flat replay reports no capture or
session failure. See `runs/fillmore-water-removal/` and the cave-effects removal
section for review media and details. These local checks do not replace native
Deck/Vulkan and Windows/D3D12 performance acceptance.
