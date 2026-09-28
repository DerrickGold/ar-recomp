# Palm refinement — 2026-09-27

The palm now uses a natural palm silhouette within the game's faceted art
style. Following the user's direction, the original sprite remains a palette
and scale reference rather than an exact silhouette target.

See the [before/after and detail comparison](../runs/sim-palm-polish-2026-09-27/palm-comparison.png)
and the refreshed [regional art index](../runs/sim-palm-polish-2026-09-27/art-index/index.html).

## Shape and surfaces

- A slim, round-section stem tapers and leans gently into a small green growing
  point. The stem and crown share a ring, with no hidden cap between them.
- Eight arching, drooping fronds remain present at every detail level. Their
  directions, lengths and tips remain stable as quality changes. Seeded
  variation changes the lean, rotation and frond proportions.
- Higher detail adds curvature and feathered leaf margins. Each frond is a
  folded ribbon with two upper planes and a shaded underside; all segments
  share their bend vertices and converge to a closed tip. Narrow roots end
  inside the growing point. There are no internal bend caps or separate fins.
- Removed the cubic hub, wood blocks, projecting trunk collars and generic
  root cross. High and Ultra bark scars are narrow surface color bands.
- Brighter tropical greens distinguish sunlit leaf ridges from undersides.
  The trunk uses muted tan/olive-brown. Its ground contact is reduced from
  5.2×5.2 to 2.4×2.4 model units to follow the slimmer base.

New palm surfaces opt into outward winding. Lighting and axis-aligned surface
analysis respect it, allowing true downward-facing leaf normals while retaining
the established box/roof convention for existing models. The face flag occupies
existing structure padding; cached face size stays unchanged.

## Cost and checks

| Detail | Previous Varied faces | New faces | Existing budget |
|---|---:|---:|---:|
| Low | 54 | 63 | 64 |
| Balanced | 104 | 126 | 160 |
| High | 119 | 240 | 256 |
| Ultra | 129 | 368 | 384 |

All styles now use the same complete natural model at a given detail. Higher
settings deliberately spend more faces on curved, feathered foliage. These
counts are geometry costs, not a measured gameplay frame-time improvement.

Verification:

- Five rebuilt suites pass: models, palette, lighting, model cache and cache
  storage. New checks cover eight stable tips across detail levels, connected
  surfaces, slim grounded stems, downward leaf normals and overhead lighting.
  The existing steep-roof lighting regression still passes.
- A 768-input palm sweep found no overflows, duplicate faces, degenerate faces
  or vertices outside the tile footprint. A broader 15,360-input model sweep
  also found zero overflows.
- Reviewed all four detail levels, both lateral tilts and a lower camera
  through the production depth renderer. Reversed face order changed 91
  shared-edge pixels out of 3,496 opaque Ultra pixels, with no changed 3×3
  interior patches. The other 75 model images remain pixel-identical to the
  preceding audit captures.
- The regional index has 730 source-comparison rows and no missing image links.
- The full application builds successfully. The linker reports a common-data
  alignment reduction from `0x8000` to `0x4000`; no compilation errors occurred.

Evidence, render logs and the generated index are under ignored
`runs/sim-palm-polish-2026-09-27/`. This verifies isolated model rendering and
builder/cache behavior, not a dense-town gameplay performance benchmark.
