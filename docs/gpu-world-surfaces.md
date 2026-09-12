# Retained globe ground and mountain surfaces

Status: enabled by default in world navigation and Sky Palace. This removes
repeated **held-camera** mountain clipping/staging/uploads, not moving-camera
terrain projection. It uses the existing screen-space geometry contract and
shaders; no art, tessellation, filtering, clipping, or depth policy changes.
`AR_SIM3D_RETAINED_GROUND=0` selects ordinary ground **and mountain** submission.
The saved graphics settings, including mountain/effect toggles, still apply.

## Ownership and failure audit

Previously the ground mesh was retained, but native mountain cutouts were
copied, clipped and uploaded again every frame, even with a cached projection.
`DrawWorldNavigationSurfaceLayers` now captures the complete Ground and
WorldMountain materials into the **same existing opaque handle**, after their
ordinary submission succeeds on a second matching view. Subsequent held
frames atomically append both ranges. The adapter retains material/depth order,
including ocean-before-mainland order within Ground. Moving frames retain the
existing CPU/multicore projection and do not publish never-reused buffers.

- The copied key includes the full ground projection/viewport/light/geography
  key, cliff revision and an explicit mountain-geometry revision. Relief,
  mountain-source and view-family changes invalidate it. Reset destroys the
  presentation-owned handle and clears the key/ranges.
- Mountain atlas animation (including lava), water animation, weather time and
  shadow samples remain live. They do not invalidate immutable geometry.
- Ground CPU arrays stay valid for haze and exact-depth cloud-shadow receivers.
  No weather mesh is approximated or independently reprojected.
- Atomic range rejection queues nothing; ordinary rendering remains available.
  If the combined capture exceeds its budget or fails, the old ground-only
  capture is attempted, leaving mountains on their original CPU path. Failure
  of that optional cache is latched until resource reset, not an authentic-view
  fallback or a per-frame allocation retry.
- Still four opaque handles globally, 64 opaque samples and 256 Ki retained
  vertices per handle. This adds no handle, unbounded cache, new runner ABI,
  render vtable entry, platform import, shader, readback or fence wait. The two
  materials need two ranges/draws, exactly as their ordinary material batches.

## Validation

All 165 app tests pass, including actual Metal tests and the layer/ABI checks.
The expanded GPU fixtures cover:

- Exact ordinary/cold/warm images through light/relief changes, visible wind
  motion, mountain disable/re-enable, camera motion, navigation/Palace changes,
  source edits/restoration and output resizing. Forced opaque-handle exhaustion
  also reproduces the ordinary images and traffic.
- Independent live Ground/WorldMountain atlases, cutout transparency against
  shared depth, unchanged draw/vertex counts and zero warm surface uploads.
- A combined 256-Ki-vertex-budget overflow: rejection leaves ranges untouched,
  ground-only capture still succeeds, and ordinary mountains plus retained
  ground reproduce the complete ordinary image.
- Existing atomic append-budget, reset, and portable CPU-fallback tests remain
  enabled. The stronger synthetic wind/source assertions were rerun after the
  full suite.

Separate full-game comparisons: 12 Palace and 16 navigation composite captures
are byte-identical, with identical final WRAM. Navigation enters at GF339,
moves in four directions, then holds; it does not leave the enhanced view
outside the expected one-frame forced blank at entry.

## Repeated local measurements

Mac/Metal, pinned release binaries, isolated saves/settings,
eight serial ABBAABBA runs per scene. Capture/build/test activity is separate
from timing. Values are render **CPU wall** milliseconds, median [min–max],
using the existing frame-weighted settled-window harness.

| Scene / helpers | Control | Retained surfaces | Median reduction |
| --- | --- | --- | --- |
| Palace / 3 | 3.385 [3.358–3.409] | 3.275 [3.226–3.287] | 3.3% |
| Navigation movement/hold route / 3 | 3.047 [3.033–3.051] | 2.939 [2.921–2.950] | 3.6% |
| Palace / 0 | 3.570 [3.564–3.589] | 3.454 [3.358–3.463] | 3.3% |

Palace mountain staging falls from about 0.198 ms to no recurring calls; depth
submission falls 0.161 → 0.141 ms. Settled depth upload drops approximately
5.03 → 4.02 MiB/present. Draw count stays 16. Live weather can slightly change
vertex counts between timed runs; the frozen GPU oracle requires equal counts.
Palace present/wait rises 4.381 → 4.462 ms, so CPU savings are not an equivalent
FPS gain. Navigation's sampled windows include held frames; this is **not** a
claim that continuous camera deformation has moved to the GPU.

## Short Steam Deck verification

Four 1000-tick ABBA runs, using bundled SDL/Vulkan and the previously fixed
NULL-swapchain lifecycle in **both** binaries. Each run guards available RAM,
combined GPU VRAM+GTT growth and elapsed time. Two additional untimed runs
compare GF600/GF900 composites: both images and final WRAM match exactly.

Render CPU: control 8.112 / 7.453 ms, candidate 7.074 / 7.054 ms. The first
control is noticeably slower than the last; treat this as directional evidence,
not a robust Deck percentage or visible-game FPS result. Mountain staging
0.478–0.556 ms disappears from settled frames, and depth submission falls
0.960–1.011 → 0.898–0.900 ms. All probes complete without graphics failure or
memory guard activation, with over 11.6 GiB RAM available. No game process is
left running and the Wayland session stays healthy. The installed Deck binary
was **not replaced**.

## Evidence and reproduction inputs

Local evidence: `/private/tmp/actraiser-world-surfaces.NDSzGz/`, including
`timing-palace`, `timing-palace-zero`, `timing-navigation`, `verify-palace`, `verify-navigation`,
`deck-direction`, `deck-visual`, `tests.log` and the guarded probe script.
Deck evidence: `/home/deck/argame/surface-probe-20260912.jBNEZ4/`.

| Binary | SHA-256 |
| --- | --- |
| Mac control | `0b3bbdf37fcf94ccd457ecac8546767bf7dbc31f69e4b89eb340afab396ab48b` |
| Mac candidate | `d612621d3e37b1fc01badc680bf38d408a00b796a0bf7074a365eeccf102988b` |
| Linux fixed control | `158d05f653180ad265a111ff58baa4913860aa695bb54d3f3103c677781cdfc6` |
| Linux candidate | `0ddbc1d88cfcdd8a12e67e9be701df3f54d4ea5244e31c6b50bdfd99f1417c56` |

Both local routes use `tests/fixtures/sim3d/world-navigation-settings.ini`,
Quality preset, 16:10 square pixels, display mode 2 and the populated private
seed SHA-256 `480a8375b6255ad681202986640c5b06889458125ec0f82641a0e4fb425c0d45`.
The comparison tool records complete hashes/environments in each results file.

The old temporary navigation recording was missing. Its replacement is a
deterministic legacy `<uint32 frame, uint32 buttons>` stream for every frame
0..2200, initially zero, with the following half-open input intervals:

```
B (mask 1):  [134,140), [178,185), [198,206), [218,225), [305,312), [318,325)
Right 128:  [430,700)
Down 32:    [800,1050)
Left 64:    [1150,1400)
Up 16:      [1500,1700)
```

This regenerates `navigation.rec`, SHA-256
`bdf54e78a5061a7c640aef598f4530c7b2413fdd829fc20b1e3157e9443a5c4f`.
Local navigation stops at 2000 host ticks, safely before replay EOF. Palace
uses the existing Palace-only prefix and 1800 ticks with `AR_REPLAY_NOSTOP=1`
(its final held input is zero). Deck probes stop at 1000 ticks without that flag.

## Next architectural step

Ground/ocean and their shadow receivers still need an explicit shared textured
source-surface transform contract for moving cameras. Offload their geometry
together so hardware clipping cannot introduce receiver/surface depth mismatch.
Retain cheap CPU visibility/LOD and multicore source preparation. This exact
cache is a measured improvement and fallback, not a substitute for that work.

The [shared surface renderer](gpu-surface-prototype.md) now defaults to live
land/cliff integration, sharing Ground, blur, haze and cloud-shadow transforms.
Ocean/mountains remain on the old path.
Its local benchmark results and increased GPU work are documented there;
the retained surface path described above remains the complete compatibility path
(`AR_SIM3D_WORLD_GPU_GRID=0`, or automatic fallback on source setup failure).
