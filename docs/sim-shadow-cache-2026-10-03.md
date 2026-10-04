# SIM shadow cost and Deck pacing — 2026-10-03

The preceding SIM audit work is committed as `b5beceed`. This experiment starts
from that commit. Evidence and pinned binaries are under
`runs/sim-pipeline-2026-10-03/`; `summarize_shadows.py` reproduces the tables.

**Final policy:** retain the existing per-pass shadow path by default. Set
`AR_SIM_SHADOW_HULL_CACHE=1` to opt into the tested two-layer experiment. Both
the smaller outline-only prototype and the two-layer version regress moving
Deck presentation; lower shadow CPU time alone does not justify changing the
shipping default. The new profiling scopes remain available in either mode.

## Attribution and implementation

New nested CPU wall-time scopes separate actor shadows, voxel shadows, canopy
hulls, blur and target transitions. In the instrumented baseline, shadows cost
3.162 ms/present, of which canopy hull construction costs 2.865 ms. Blur costs
0.084 ms. These are not GPU execution timestamps and nested scopes must not be
summed.

The renderer previously discarded its canopy-outline cache after each mask.
The first candidate retains exact model-local outlines by shape kind, variant
and floating-point light/camera shear. Its four-way cache is bounded at about
400 KiB, belongs to the render owner, and resets with the voxel renderer. Terrain
projection, dynamic actors, masks and blur still run. It does not quantize camera
angles, reduce shadow resolution, or retain whole rendered frames.

A second candidate also retains authored 3D canopy samples, so changing camera
shear does not repeat trigonometric crown/branch construction. Its additional
bounded cache is about 3 MiB. This saves CPU time but regresses moving-view
throughput in the measured synchronous presentation path; it is not a demonstrated
end-to-end optimization.

## Deck results

Runs use the exact prior Aitos seed/replay, Original SIM menu, Native text,
fullscreen Vulkan on the 1280×800 90 Hz Wayland desktop, default accumulator,
no interpolation, and isolated settings/saves. Paired controls use the same
binary with `AR_SIM_SHADOW_HULL_CACHE=0`. All completed without fatal sessions or
foreign game processes. Idle pairs and moving pairs were repeated in opposite
orders. The first moving enabled run captures one screenshot; the repeat does
not. The slowdown exists before that screenshot and repeats without it.

Presentation statistics cover source ticks 1100–3600. Shadow means weight the
final 30 Town 3D log windows by their present counts. These are present-return
measurements, not physical scanout or input latency.

| Candidate | FPS | p95 interval (ms) | Shadow wall time/present (ms) |
| --- | ---: | ---: | ---: |
| Instrumented baseline, idle | 85.04 | 23.14 | 3.162 |
| Outline cache only, idle | 86.59 | 20.93 | 0.800 |
| Both caches, idle 1 | 84.97 | 20.65 | 0.703 |
| Caches off, idle 1 | 84.30 | 23.34 | 3.276 |
| Caches off, idle 2 | 84.48 | 23.64 | 3.235 |
| Both caches, idle 2 | 85.00 | 20.24 | 0.681 |
| Both caches, moving 1 | 80.73 | 23.96 | 1.931 |
| Caches off, moving 1 | 85.30 | 20.55 | 2.560 |
| Caches off, moving 2 | 85.13 | 20.70 | 2.545 |
| Both caches, moving 2 | 79.46 | 24.84 | 1.958 |
| Outline cache only, moving | 81.21 | 21.60 | 2.454 |

Idle source-cadence mismatch also rises from roughly 4% to 7–8% with both caches,
despite the better completion-interval p95. Less shadow CPU work is established;
stable 90 FPS or uniformly better native animation cadence is not. The cause of
the moving-view regression is not established by the wall-time scopes.
The test binaries defaulted to caching; the final source requires explicit
opt-in. `AR_SIM_SHADOW_HULL_CACHE=0` controls and the final default execute the
same per-pass implementation.

## Validation

- Release model tests and AddressSanitizer model tests pass, including immutable
  sample projection across canopy families, towns, seeds and light angles.
- Focused Metal `--shadow-cache` checks pass in Release and AddressSanitizer:
  exact cold/warm/fresh pixel parity across six scene families, twelve camera,
  light and height variations, both mask paths, and a revisited prior view.
- The real captured-town Metal suite passes. The moving Deck screenshot was
  inspected with HUD, terrain, foliage and eruption effects intact.
- The older `--voxel-shadows` suite fails its forest-corner assertion on the
  unchanged `277e077e` baseline too. Its forest mask has the same SHA-256 in
  baseline and candidate: `58e0fc5820e4208f4589b09320329737886d981a76afed48e4e7f26073b6f5b7`.
  The assertion remains unchanged; the focused cache test has its own selector.
- Render-boundary, global-owner and performance-metric checks pass. The full
  repository style ratchet has existing failures; this diff adds no violations
  relative to committed HEAD. Mac and Linux game builds pass.
- The final default Deck build completes 2,400 frames cleanly. Its manifest has
  no cache opt-in and its hull-call counts verify the original per-pass path.
  Final binaries and benchmark hashes are in `shadows-sha256.txt`.

## Comparison with SIM packets

Shadow caching removes work; owned SIM packets would overlap producer/capture
work with presentation. The original steady capture scope is about 3 ms/present,
so overlap offers comparable potential critical-path headroom, not a guaranteed
3 ms gain. It requires immutable canvas/voxel/world resource publication and a
bounded queue, plus measurement of source age and owner-side submission pacing.
The live results make that scheduling work more important than further blur
quality reductions. No SIM streaming or cadence-default change is included here.
The shadow work is a validated experiment for that follow-up, not a claim of
improved default Deck FPS. New shadow changes are left separate from the
requested baseline commit for review.
