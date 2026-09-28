# SIM environment models — 2026-09-27

The first environment pass covers the burnable `$01` bush and all six native
rock metatiles. The completed building audit was committed as `56649524`.

## Source and models

- **Bush `$01`:** replaced the cube-grid crown with a closed, rounded surface,
  a low shoulder, short stem and clustered leaf colors. The palette retains
  dark green between highlights instead of saturating the entire crown.
  Permanent forest, palms and the ancient tree retain their existing models.
- **Boulder `$61`:** two joined, angular stone masses follow the native split
  profile. This includes the lightning boulder in Fillmore and the same art
  used among Aitos' obstacles.
- **Scattered rocks:** `$62/$63/$69/$6A/$6B` preserve their respective
  one/two/three/three/four-stone layouts. The small stones remain separate,
  with ground and individual contact shadows between them.

The source images are decoded from each supplied ROM's terrain atlas, through
the existing regional index generator. All six rock images match across the
five supplied releases and both character banks. The US obstacle overlays at
file offset `0x51800 + (town - 1) * 0x400` contain one `$61` cell in Fillmore;
Aitos contains 15 boulders plus 12/9/8/4/8 cells of the five scattered layouts.
These are obstacle-layer counts, not a claim that every cell is visible at
every point in play. The geography classifier independently identifies these
six entries as rocks, separate from cliff, sand and mountain families.

The town classifier follows the displayed terrain metatile, preserving the
model after a miracle updates the logical cell but before the final BG redraw.
The ordinary ground replacement removes its original flat image. Retained
world towns read their own cell maps and publish the same model identities.
Rock metatile identity is part of the model cache key, so changing a layout
at the same coordinate cannot reuse the previous arrangement.

## Cost and verification

| Model | Low | Balanced | High | Ultra |
|---|---:|---:|---:|---:|
| Bush faces | 45 | 89 | 149 | 225 |
| Boulder faces | 32 | 32 | 32 | 32 |
| Scattered-rock faces | 16–64 | 16–64 | 16–64 | 16–64 |

No budget increases are needed. The rocks fit Low with their complete native
composition; higher settings do not add speculative stones. Bush geometry
contains only its crown surface and stem rather than a volume of foliage cubes.

Evidence under `runs/sim-environment-models-2026-09-27/` includes:

- `environment-comparison.png`: original art, previous bush, Ultra, Low and
  oblique renders, enlarged independently for inspection.
- `art-index/`: 73 model entries and three regional variants, with 730 original
  comparisons and verified local image links. The original 67 numbers remain
  stable; the six rock entries are appended.
- `mesh-counts.csv`: 15,360 builder inputs across kinds, towns, seeds, detail and
  style, with zero overflows, including every scattered-rock layout.
- `test-results.log`: geometry, cache, palette, classification, clearing,
  ground cleanup, retained world ownership and style checks.
- `build-app.log`: the full application build.

Normal, Low, oblique and reversed-face-order renders use the production depth
renderer. Reversing submission changes only isolated shared-edge pixels in
these models; no changed 3×3 interiors were found. This is an isolated model
and capture-contract review, not a full gameplay playthrough of each miracle.
