# Fillmore Act 1 effects implementation audit

2026-09-27. Scope: the approved forest/boss lighting, leaves and motes,
including capture, projection, geometry, flat/Diorama presentation, resource
lifetime and renderer portability. The authored appearance is preserved.

## Corrections

1. **Removed unnecessary flat-view render targets.** The old path accumulated
   the rays and foliage into a viewport-sized target, then masked and composited
   each layer. The existing binary BG2 winner mask now textures the geometry
   directly, using standard additive and alpha blending. Native mask UVs account
   for the visible crop and output viewport. Two geometry draws replace seven
   draws, two target binds/clears and their restores. Including foreground light,
   the forest requires three draws in either view. Flat Fillmore no longer needs
   an effect render target or a custom alpha-mask blend. Other rooms retain their
   existing composition paths.
2. **Isolated optional surface-light blending.** A renderer rejecting the
   destination-color blend previously tripped the process-wide host-effect
   failure latch. Now only surface lighting is disabled, once per device lifetime;
   ordinary rays, foliage, spells and actor effects remain available. Resetting
   presentation allows another renderer to try again. No incorrect additive
   substitute is used to brighten black scenery.
3. **Removed repeated ray calculations.** Motes and leaves use a small local
   snapshot of the twelve ray slopes/sway values, instead of recomputing sine
   for each particle/light pair. Dust and culling reuse those values. There is no
   persistent animation cache to invalidate on pause, map changes or clock wrap.
4. **Do not draw with a stale BG2 mask.** A failed upload now suppresses the
   masked passes until an upload succeeds. A successful unchanged-image upload
   still marks the retained mask ready. This prevents old terrain occlusion from
   being applied to a newly scrolled frame.
5. Reused the existing finite-rectangle validation helper in projection and
   restored constness of the screen X coordinate.

## Ownership and bounds

- Capture reads canonical room dimensions/map data and WRAM through bounded
  accessors. It publishes three decoration records; it does not allocate native
  actors, consume enemy slots, or write WRAM. Presentation reads captured frames,
  not live emulation state. The independent Environmental effects setting gates
  capture and presentation; the native scene clock governs animation and pause.
- The source table has twelve authored openings. At most six intersect the
  capture field. The table's spacing, height/slope envelope, runtime count guard,
  clipping tests and compile-time scratch assertions bound work and memory.
  Ray triangles are clipped with interpolated colors, never vertex-clamped.
- Conservative per-layer limits remain 2,536 vertices / 4,884 indices for
  background rays plus particles, 1,680 / 3,600 for foreground light, and
  144 / 240 for leaves. The fine-dust budget is 192 candidates, with 22 larger
  motes and 16 leaves. Offscreen rays are culled before their dust is generated.
- Builders reuse the existing static geometry workspace; the new ray snapshot
  is 96 bytes. There is no per-frame heap allocation, room-size scan, GPU
  readback, new shader, or draw per particle. Scratch tails are not cleared.
- Flat mode converts at most 640 × 224 packed mask pixels into a reusable
  560 KiB scratch buffer. The existing upload mirror sends changed regions and
  retains its allocation. This mask is not generated for Diorama, which uses
  the existing depth-ordered plane callbacks. Extra rows therefore do not
  introduce the flat masking path, target copies, or additional effect passes.
- Resources are owned by the presentation thread and released on its reset.
  Authored recipes remain with the other scene-effect families; no new global
  animation state or cross-module dependency was needed.

## Portability

The geometry and capture code are C11, using fixed-width clock/hash fields,
unsigned deterministic hashing, bounded arrays, standard math and the portable
renderer interface. There are no new platform intrinsics or native graphics
types in action code. Packed ARGB mask words are read with `memcpy`, avoiding
alignment/aliasing assumptions; the format contract defines the word value
independently of host byte order. All large scratch storage stays off the stack.

Ordinary forest layers now need only textured geometry with standard alpha/add
blending. Foreground lighting uses a custom blend whose acceptance is checked
at submission. SDL's software renderer does not generally support custom blend
modes; rejecting this one no longer disables ordinary effects. The SDL state
test exercises rejection/success reporting and draw-state restoration using
the real software renderer, while presenter tests inject an isolated rejection
and verify continued rendering and reset recovery.

Runtime validation here is macOS/Apple M2/Metal plus SDL software tests. Windows,
Linux and other GPUs were not run, and cross-backend bit-identical rasterization
is not promised. GPU duration is unavailable in the current metrics; a 4K or
low-power GPU fill-rate assessment remains a separate hardware check.

## Validation and local evidence

Artifacts/scripts are retained in the ignored `runs/fillmore-effects-audit/`
directory, alongside a copy of the pre-audit executable and geometry source.
No user save or preferences were used by the captures/benchmarks.

- Release build and focused observer, capture, geometry, presenter,
  render-device and SDL-state tests pass. Presenter coverage includes cropped
  mask UVs, a nonzero viewport origin, no render-target capability, failed-upload
  recovery, custom-blend rejection isolation, resource reset and settings Off.
- Geometry and presenter tests pass AddressSanitizer and UndefinedBehaviorSanitizer,
  compiled as C11. Compiler stack-usage output reports a largest scene-render
  function frame of 2,160 bytes (`AppendForestRays` on this build); large batch
  arrays are static. The camera/phase sweep compares 12,960 builds with the pre-audit source:
  used vertices and indices are byte-identical (`geometry-comparison.json`).
- Paired early-forest captures cover flat/Diorama, 32/64 extra rows, and effects
  On/Off. All native WRAM snapshots match the approved version. All Off images
  and all Diorama images match exactly. Flat On differs by at most 1/255 in a
  color channel at a small number of pixels, consistent with eliminating an
  intermediate 8-bit target (`visual-comparison.json`, `flat-before-after.png`).
- An isolated geometry probe, run in control/current/current/control order,
  measures about 21.84 → 20.86 microseconds for all three boss layers, a roughly
  4.5% CPU reduction in that probe. This is a small geometry-only improvement,
  not a whole-frame or GPU speedup claim.
- Touched C/header files satisfy the style ratchet; scoped `git diff --check`
  passes. The release linker retains the existing macOS `__DATA` alignment
  warning.

Full presentation timing results are recorded in `perf-summary.json`; the raw
per-window samples and replay driver are `perf-results.json` and `perf.py`.
These sequential runs cover traversal and the Centaur arena at 64 extra rows,
without screenshots/video encoding. They exclude the first two action windows
and the window containing the single native snapshot used to mark the arena.
Previous late-replay audio scheduling differences remain an observational
limit; these timings do not establish native lockstep determinism.

Median CPU presentation time on Apple M2/Metal, 720 × 448 output (milliseconds):

| View / section | Before audit, effects On | After audit, effects On | Effects Off |
| --- | ---: | ---: | ---: |
| Flat traversal | 0.0774 | 0.0324 | 0.0055 |
| Flat boss | 0.1061 | 0.0417 | 0.0067 |
| Diorama traversal | 0.1139 | 0.1133 | 0.0977 |
| Diorama boss | 0.1365 | 0.1218 | 0.0825 |

There are 24–25 traversal and 10–11 boss windows per run. All 213 retained
windows report zero presentation fallbacks/failures and zero upload-mirror
reallocations. The separate PPU/capture medians vary appreciably between runs:
flat On/Off is 1.3399/0.7674 ms during traversal and 1.2296/1.0044 ms at the boss;
the extra flat winner-mask capture has a cost even though the compositing is
now cheaper. Diorama On sometimes measures faster than Off in PPU/capture,
so no whole-frame speedup is inferred. Raw metrics' draw counters do not cover
all flat/effect submissions; the five eliminated draws are established by the
presenter regression test and implementation, not those counters.
