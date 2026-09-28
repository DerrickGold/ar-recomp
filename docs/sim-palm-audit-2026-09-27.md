# SIM palm audit — 2026-09-27

**Follow-up:** the [palm refinement](sim-palm-polish-2026-09-27.md) addresses
these findings with a natural palm interpretation, as requested after this
audit. The observations and measurements below describe the earlier model.

**Verdict: needs a focused art and surface pass.** The model is connected and
comfortably within budget at Balanced and above, but its foliage colors, trunk
and detail-level silhouettes do not yet match the original's character.
This audit changes documentation only; the palm implementation is unchanged.

## Reference and evidence

Compare the [original and seven model views](../runs/sim-palm-audit-2026-09-27/palm-audit.png).
Model panels share one scale; the source sprite is enlarged independently.
The reference is art-index entry **56, Marahna palm**, terrain metatile `$09`.
Its decoded pixels match across all five supplied releases and both character
banks: ten index rows, one pixel hash. US provenance is definitions `0x0C881A`,
characters `0x060000`, palette `0x0E3B93`; pixel SHA-256 is
`c67b626899a3e30470b69f036ea856604d64642391c2e64eb8bde57a0c38dbdf`.

Views cover Low, Balanced, High and Ultra, both lateral tilts and a lower camera.
They use Varied style, MaterialAware shading and the production SDL depth pass.

## Findings

### 1. High art priority: the palm inherits the dark evergreen palette

`sim_background_voxel_palette.c:316–327, 439–483` supplies the common leaf
ramps to palms without a palm-specific override. Their brightest green is
approximately `(16,106,0)`, apart from small seeded variation. The original
contains 27 pixels of `(32,148,0)` and 23 of `(57,189,0)` in a 16×16 tile.
Those bright greens create distinct light and dark frond strips; the current
model cannot reproduce them and reads as a nearly uniform dark green crown.

Give palms their own source-derived leaf and trunk ramps, then distribute
highlight/shadow materials along the fronds. The source's visible stem is
muted gold/olive-brown, while the rendered trunk and added bands are orange-brown.
This correction does not require more faces.

### 2. High art priority: trunk and crown details change the native silhouette

`BuildPalm` in `sim_background_voxel_models.c:1705–1771` uses a thick square
trunk, a box-shaped crown hub and straight, two-segment leaf strips. High adds
a tall square bud and two wood blocks under the crown. Ultra adds projecting
square trunk collars. `BuildSilhouetteTrim` at line 2594 also adds a broad
cross-shaped root base in Trim and higher styles.

The source reads as an arched, drooping leaf fan over a slim stem. Its tiny
sprite does not support a precise anatomical leaf count or definite coconut
interpretation, but it does support a slimmer stem, less visible box-shaped
crown mass and more varied frond curvature. The enlarged roots, collars and
central bud make the added detail more conspicuous than the source features.

Low also drops all four diagonal fronds (`models.c:1736`), changing the crown
from eight directions to a four-arm cross. Preserve the fan's important
directions across detail levels and simplify the surfaces instead. Spend
High/Ultra geometry on curvature, taper and restrained leaf color divisions.

### 3. Medium correctness priority: frond undersides have upward normals

`AddPalmFrondSegment` (`models.c:1668–1673`) authors separate top and underside
faces. `SimBackgroundVoxelSurface_OutwardNormal`
(`sim_background_voxel_surface.c:24–32`) forces every nonvertical normal's Z
component upward. Consequently **all 16 frond underside faces at Balanced and
above have upward-facing normals**; Low has eight such faces.

A probe of the first segment resolves both normals to
`(-0.079116, 0, 0.996865)`. Under overhead MaterialAware lighting, its top and
underside receive brightness 253 and 244 respectively. The underside's authored
brightness of 178 therefore does not restore appropriate directional shading.
The second segment exhibits the same problem, with brightness 242 versus 233.

Introduce an explicit convention for downward surfaces or consistently outward
winding before changing the shared normal resolver. Preserve existing steep
roof/pyramid lighting when fixing this. Add a regression that verifies opposing
normals and overhead-light response for an actual palm segment.

### 4. Low optimization priority: bends overlap and retain internal caps

`AddPalmFrond` (`models.c:1682–1702`) starts its second segment 0.1 units before
the first segment ends, 0.02 units higher, with a different width. Each segment
also receives an end cap. There are four intermediate caps at Low and eight
at higher detail, mostly inside the overlapping joint, with a small exposed
step. These are not exact duplicate faces.

The general cleanup (`models.c:2275–2329`) only removes qualifying axis-aligned
faces buried in tracked boxes; it cannot merge these frond joints. Author
shared bend vertices and only cap the tip. That would remove four/eight
intermediate faces and the overlapping seam while retaining the silhouette.
This is a modest cleanup, not evidence of a large performance problem.

## Budget and structural checks

| Detail | Basic final faces | Trim/Architectural/Varied final faces | Maximum authored | Budget |
|---|---:|---:|---:|---:|
| Low | 54 | 54 | 55 | 64 |
| Balanced | 94 | 104 | 105 | 160 |
| High | 109 | 119 | 120 | 256 |
| Ultra | 119 | 129 | 130 | 384 |

The authoring limit applies before cleanup. High and Ultra have ample room for
better silhouettes; increasing their budgets is unnecessary. Low needs careful
surface allocation if it is to retain more of the crown.

- A fresh sweep of **768 inputs** (six towns, four details, four styles, eight
  coordinate/group seeds) found zero overflows, exact duplicate surfaces,
  degenerate quads or faces outside the 16×16 footprint. Town combinations
  here are builder robustness checks, not claims about native palm placement.
- Inspected views show no obvious detached fronds, open trunk connections or
  broad depth-order failures. Reversed face submission changed 25 pixels out
  of 5,680 opaque Ultra pixels, with no changed 3×3 interior patches. Small
  shared-edge differences remain; this is not proof of perfect ordering at
  every camera setting or dense in-game overlap.
- The model, palette and lighting test targets were rebuilt and their three
  existing test suites passed. Palm-specific coverage currently checks basic
  bounds/materials/sloped foliage, not source fidelity or underside normals.

Audit artifacts are in `runs/sim-palm-audit-2026-09-27/`: `palm-audit.png`,
`mesh-counts.csv`, `mesh-summary.json`, `normal-check.txt`, `test-results.log`,
and the local diagnostic scripts. Generated images and probes remain ignored.
This is an isolated model audit, not a gameplay performance benchmark or a
full review of every palm-to-neighbor overlap.

Recommended correction order: source palette and overall silhouette; stable
Low crown; shared frond bends and correct underside normals; then additional
High/Ultra shaping only where it improves the reference match.
