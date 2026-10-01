# Aspect ratio audit

Audit date: September 30, 2026. Required targets are **4:3, 16:9, and
16:10**, with both square pixels and CRT pixel correction. **21:9 is
exploratory**. Arbitrary Stretch output is outside this acceptance matrix.

The initial audit covers display sizing, tile capture, horizontal and vertical
scroll limits, finite background bounds, synthetic background padding, HUD
placement, town rendering, and world navigation. The follow-up fixes finite
BG2 bounds in the captured-texture skybox path and adds display acceptance
checks. The sampled scenes are not a certification of every room or platform.

## Findings

### Captured parallax skybox bounds now follow BG2

**Fixed for the captured-texture path.** Its valid horizontal span previously
used only the primary playfield's remaining margins. A finite parallax layer
can reach its own edge sooner. Captured frames now publish BG2's actual finite
source interval, and
[`DioramaBgValidSpanPlan_Build`](../src/diorama/diorama_skybox_uv.c) intersects
live-world spans with that interval. BG1 still owns gameplay camera travel and
the shared capture budget; BG2 keeps its own parallax.

The Aitos 04/04 replay reproduces the mismatch at game frame 2200:

| Captured value | BG1 | BG2 |
| --- | ---: | ---: |
| Camera X | 120 | 60 |
| World width | 1280 | 1024 |
| Available native terrain to the left | 120 | 60 |

The PPU snapshot publishes a 120-pixel left margin. In the separate BG2 dump,
surface columns 64–123 are black padding; real terrain begins at column 124.
The old captured-texture span nevertheless began at column 64. At 16:9 with
square pixels, the aspect crop could retain roughly 12 pixels of that invalid
strip; the narrower 16:10 crop concealed it. This is separate from
ordinary perspective revealing the physical edge of a finite diorama plane.

The first audit's raw BG2 dump established the span mismatch, but did not
prove that the default room presentation sampled that strip. Follow-up GPU
checks established that ordinary finite skies already use a separate clamped
background view; named ROM skies use their own source too. Those paths are
unchanged. Virtual depth classification can require the ordinary captured
BG2 texture instead, exposing the missing source bound. An isolated Aitos
04/04 classification reproduces that path and confirms the correction.

The producer derives bounds from the verified BG binding and authored scenery
extent, intersects the raster offsets observed during scanout, and accounts
for display-anchored mosaic groups. The immutable frame carries the result to
the presenter. Rejected bindings and room transitions clear the metadata.
Explicit mirror/repeat/clamp policies, fixed caps, and cyclic worlds retain
their existing behavior. Bounds observation runs only when the skybox margin
fix can use it.

### The original terrain capture gap has targeted coverage

The preceding fix lets verified finite world terrain fill the existing
64-pixel guard strips for widescreen diorama ratios wider than 16:10. The
ordinary scanout remains capped at 120 pixels per side. Provider tests cover
finite boundaries, raster offsets, rejected bindings, and eligibility reset.
Fixed, mirrored, and banded layers retain their separate policies.

Earlier isolated Fillmore, Bloodpool, and Kasandora captures exercised the
16:9 fix. The fixed-camera 16:10 control comparison retained identical pixels
and WRAM. Evidence is in the local `runs/widescreen-columns/` directory.

### Authored tiles now use native background capture

Stamped BG1/BG2 tiles, black masks, and guard terrain now enter the PPU's
capture export in 8×8 tile runs. They share the native character-row cache,
scanline palette, brightness, scroll, mosaic, window handling, and capture
blend policy. Capture writes reserve their destination bands before ordinary
export; the completed background planes no longer need a second painting
pass. Gameplay source buffers and authentic output remain independent.

The manifest caches its terrain-resolved stamp index and occupied-cell bounds.
Reloads, mutable editor access, saves, and room/profile changes invalidate that
view. Ordinary frames neither rebuild the entire stamp index nor rescan every
cell to calculate camera bounds. Work during capture follows the captured tile
runs rather than the number of authored cells in the level.

Permanent native-renderer tests compare edited output with an independently
merged tile provider for both BGs, including blank tiles, masks, all depth bands,
negative coordinates, raster scroll/palette/brightness changes, mosaic, windows,
subscreen ownership, mirror mapping, and the scalar reference renderer. They
also verify unchanged gameplay/authentic output and bound callback counts.
ABI tests cover invalid capacity, stale lifetime, atomic rejection, and clearing.
Manifest tests cover cached 15,000-cell lookup and edit/profile invalidation.

GPU comparisons retain identical composite pixels and WRAM at 4:3, 16:9, and
16:10. The 16:9 stress fixture adds 15,000 off-screen cells to the authored Aitos
04/01 map. Evidence and binary hashes are under `runs/stamp-native-capture/`;
the fixture leaves the shipped map unchanged. Runtime API usage is documented
in [the capture tile contract](../snesrecomp-go/runtime/docs/API_REFERENCE.md).

On the local macOS host, eight alternating stress runs (four per executable,
2,400 presents each) used that same 16:9 map, settings, and replay. All eight
finished with identical WRAM. Settled CPU wall-time results were:

| Scope | Baseline median | Native capture median |
| --- | ---: | ---: |
| PPU and capture | 14.12 ms | 3.89 ms |
| PPU setup | 0.607 ms | 0.015 ms |
| PPU scanout | 12.44 ms | 3.85 ms |
| PPU finish | 0.430 ms | 0.010 ms |
| Total measured render CPU | 14.64 ms | 4.29 ms |

PPU/capture CPU time decreased by about 72%. Baseline run means ranged from
10.66–22.64 ms, versus 3.83–4.09 ms for native capture. These are local CPU
measurements, not GPU timings or cross-platform frame-rate guarantees. The
full inputs, binary hashes, individual runs, and ranges are recorded in
`runs/stamp-native-capture/stress-timing/results.json`.

The 47-test runtime suite, dedicated BG capture parity test, and 11 focused
application tests passed. Five application sanitizer targets and the runtime
ABI/capture-tile sanitizer tests also passed. `make check` again stopped at the
pre-existing style violations listed below; private-header, global-owner,
and source-manifest checks passed separately. The full PPU sanitizer suite was
replaced with the focused capture-tile test after its long debug run; no full
runtime sanitizer-suite result is claimed.

### Ratio coverage was concentrated on 16 by 10

**Coverage improved.** Existing action render checkpoint defaults selected
16:10. This pass adds permanent tests for supported geometry, centering and
letterboxing, settings transitions, capture origins, scroll limits, and
background span policies at each supported capture budget. Follow-up cases
exercise independent BG1/BG2 left and right edges, narrow rooms, authored
extensions, raster shifts, mosaic, cyclic sources, and rejected bindings.

The sizing calculation now lives in
[`DisplayGeometry_CalculateHorizontal`](../src/present/display_geometry.c),
called by the host and directly exercised by
[`aspect_ratio_test.c`](../tests/aspect_ratio_test.c). This extraction preserves
the existing sizing policy. Settings and skybox tests cover additional
transitions and bounds without requiring a ROM or GPU.

### Ultrawide requires capacity work

**Deferred.** Square-pixel 21:9 requires 524 source pixels, or 134 extra pixels
per side after symmetric rounding. That exceeds both the game's 120-pixel
streaming cap and the runtime's 128-pixel horizontal margin. SIM capture also
has a 512-pixel limit. CRT-corrected 21:9 would fit in 448 source pixels, but
supporting only that pixel mode would leave an inconsistent feature.

The aspect setting currently has no 21:9 option. Do not advertise support by
only adding a selector value: allocation, streaming, projection, sprites, and
HUD paths need a separate capacity audit and acceptance captures.

## Supported geometry

Widths are source pixels before host pixel correction. Vertical native capture
is 224 pixels. Widescreen diorama uses a wider capture than its visible crop.

| Aspect setting | Pixel mode | Visible width | Flat margin per side | Diorama margin per side |
| --- | --- | ---: | ---: | ---: |
| 4:3 | Square | 256 | 0 | 0 |
| 4:3 | CRT | 256 | 0 | 0 |
| 16:9 | Square | 400 | 72 | 120 |
| 16:9 | CRT | 342 | 43 | 120 |
| 16:10 | Square | 360 | 52 | 120 |
| 16:10 | CRT | 308 | 26 | 120 |

The 4:3 setting preserves the native viewport. With square pixels its actual
shape is 256:224, or 8:7; CRT correction supplies the intended 4:3 shape.
Integer rounding produces small differences from an exact nominal widescreen
ratio. Neither behavior is an arbitrary missing tile column.

## Scrolling and edge policy

The action camera resolves horizontal travel using the render budget and any
tuned extent. Rooms too narrow for those margins fall back to native bounds.
Authored scenery can relax a presentation stop but cannot extend gameplay
camera travel beyond the native world. Zero-motion native transition state is
deliberately preserved until a native tracking update.

Native vertical travel uses a 225-row camera viewport, independent of extra
diorama capture rows. Shrinking that travel to fit extra rows would move the
floor stop and can affect actors near the native screen edge.

The Aitos captures reach both fitted horizontal stops: 72/952 in flat 16:9
square-pixel output, 52/972 in flat 16:10, and 120/904 in both wide diorama
modes for its 1280-pixel world. The Death Heim replay crosses 07/01 and 07/02,
including the 512-pixel boss room's narrow 120–136 fitted interval. Ratio
changes can therefore change native camera state; cross-ratio WRAM equality
is not an appropriate acceptance criterion.

Mirror/repeat padding and fixed asymmetric caps remain explicit per-layer and
per-band choices. The added skybox tests check those independently from live
world margins, including a transition from a wide span to clamped art.

## Verification

The isolated GPU matrix completed **36 runs and 204 final-composite captures**:
six ratio/pixel-mode combinations across Aitos flat, Aitos diorama, Death Heim
diorama, town actions and menus, voxel town, and world navigation. The diorama
fixtures in this initial matrix leave the skybox setting off. Captures
required a real composite; native-framebuffer fallback was rejected. World
navigation was checked against the active view at each capture. Representative
contact sheets were inspected for composition and HUD placement.

Local evidence, input hashes, settings, WRAM/PPU snapshots, logs, and contact
sheets are under `runs/aspect-ratio-audit/`; `matrix.json` indexes the 36 runs.
`aitos-bg2edge-169-square.json` indexes the additional background-layer probe.
Evidence is intentionally ignored by Git and depends on a locally supplied ROM.

The aspect, settings, background-provider, and skybox tests pass in optimized
and AddressSanitizer/UndefinedBehaviorSanitizer builds. The focused optimized
suite covers 41 camera, background, diorama, settings, input, HUD, and rendering
tests, including GPU frame generation. The optimized game builds successfully.
A control/candidate
replay switches from 16:10 to 16:9 at frame 1600 and to 4:3 at frame 2200.
All six captured composites and WRAM snapshots are byte-identical after the
sizing extraction, as is final WRAM. This verifies live buffer/crop changes
without claiming equal gameplay state between different fixed ratios.

The follow-up evidence lives in `runs/aspect-ratio-fix/`:

- `matrix.json`: 12 skybox-off runs / 72 composites match the initial Aitos
  and Death Heim controls byte for byte, including sampled and final WRAM.
- `skybox-matrix.json`: 24 control/candidate skybox-on runs / 144 composites
  verify the existing clamped/ROM skybox and Death Heim band policies remain
  byte-identical across all six aspect/pixel-mode combinations.
- `captured-skybox-matrix.json`: 12 control/candidate runs / 72 composites
  exercise a virtual-classified Aitos BG2. Only the wide-ratio left-edge
  checkpoint changes; 4:3 and the other checkpoints remain byte-identical.
  Sampled and final WRAM remain identical. The corrected sampling also shifts
  the skybox crop at 16:10 in this specific path. The comparison image is
  `captured-bg2-comparison.png`.
- `display-acceptance.log`: 84 native macOS GPU checks cover all six
  combinations at 1× and 2× density, two window resizes, borderless fullscreen,
  exclusive desktop-mode fullscreen, and restoration to a window. All pass
  viewport, pixel readback, letterbox, and pointer-coordinate checks using
  production presentation/mapping functions in a dedicated SDL harness.
- `camera-matrix.json`: 12 runs / 72 composites exercise configured pitch/yaw
  limits of ±700 mrad and a live distance change from 2.0 to 20.0 across all
  six combinations. The skybox fills the output and HUD placement remains
  stable in the inspected captures. Finite foreground-plane edges remain
  visible at extreme poses by design. Sampled and final WRAM match the
  corresponding baseline; `camera-extremes.png` shows representative poses.
- `policy-matrix.json`: eight control/candidate runs / 32 composites check
  Fillmore 01/01, Bloodpool 02/01, Kasandora 03/01, and Marahna 05/01 at 16:9
  with skyboxes enabled. All images and sampled WRAM match. Combined with
  the Death Heim replay and unit cases, these cover finite sources, authored
  scenery, mirror/repeat bands, fixed caps, and cyclic-world policy.
- `final-captured-matrix.json`: the final aspect-fix build repeats the 16:9
  and 16:10 square-pixel captured-BG2 cases and matches the corrected images,
  sampled WRAM, and final WRAM exactly.

These results describe the tested builds, whose hashes are recorded in the
replay indexes. Concurrent terrain-stamp storage changes appeared afterward in
the shared workspace, including `diorama_layer_order.h/.c` and
`actraiser_action_bg.c`. Before committing the aspect work, the combined test
targets were rebuilt and all 41 focused tests passed again, including GPU
frame generation. The separate terrain/editor changes remain outside this
commit. The earlier GPU replay evidence describes the aspect-fix build before
that storage refactor.

Pre-commit `make check` was also attempted with the existing quality-tooling
Python environment. Constant checks, all three Go vet targets, and the
Python/JavaScript/shell/Go tooling checks passed. The gate then stopped at the
previously identified style violations outside this change. Logs for this
pass are under `runs/aspect-ratio-commit/`; no clean full-suite result is
claimed.

Permanent ROM-free resize coverage adds 24 actual SDL software-renderer
resize/readback cases. Existing input tests cover fractional pixel-density
mapping and out-of-window pointers; HUD/menu tests cover narrow outputs and
automatic scale limits. Physical display hot-plug and moving a live window
between displays with different densities were not exercised. Native Windows
and Linux acceptance still requires those platforms.

The full 311-test optimized suite initially reported three failures and four
GPU skips. Shader-blob validation passed when rerun with host display access;
GPU effect, diorama frame-generation, and SIM depth-pass tests then passed too.
The remaining failures are the style ratchet, localization save/name
persistence assertions, and the world-navigation GPU test's assertion that
lowering a voxel detail tier changes pixels (`present_world_nav_gpu_test.c:888`).
The latter two test targets do not link the sizing code changed here. Style
violations occur in unmodified effect, voxel, and rendering files. The full
repository gate is therefore not clean; these failures are not claimed fixed.

The ROM-free checks can be repeated with:

```sh
cmake --build build-tests-release --parallel 3
ctest --test-dir build-tests-release -R '(diorama|action_bg|aspect_ratio|camera|render_pipeline|settings|host_input|host_display|hud)' --output-on-failure
```
