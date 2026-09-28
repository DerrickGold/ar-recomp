# Model review follow-up — 2026-09-27

This pass addresses the nine notes from the regional art-index review. Native
references remain unchanged; updated models preserve their regional palettes,
tile footprints and faceted rendering.

The alternate straw crown in this pass was still too flat. The subsequent
[straw-hut correction](sim-straw-crown-2026-09-27.md) restores its broad raised
crest; the comparison images below preserve this earlier pass.

## Changes

| Model | Correction |
|---|---|
| #11 Bloodpool developed house, front | Removed the intersecting left extension and shed roof that produced the broken protrusion. |
| Alternate early straw houses | Replaced the exposed wooden crown pegs with a broader, lower straw cap and short ochre bands following its surface. The front-facing tied peak remains distinct. |
| #15–16 Kasandora middle houses | Rebuilt as standing canvas pavilions: upright side walls, peaked cloth roofs, split entrance curtains, attached guy lines and a supported alternate awning. Construction stages now use upright posts and rafters. |
| #24 Aitos developed house, alternate | Replaced the rooftop room-like structure with an open chimney, including a recessed flue and rim. |
| Marahna stilt houses, including Japanese developed variants | Added stairs ascending left-to-right along the front face, with stringers and a landing connected to the doorway. |
| Windmill | Sails widen toward their tips, sweep slightly, and carry a curved purple pinstripe on both faces. Snow remains on the entrance roof. |
| Factory | Rebuilt the joined U-shaped roof with continuous pitched planes, two open chimneys on the right connector, and three peaked window dormers on the rear/front arms. Exterior walls are joined and survive hidden-face cleanup. |
| Permanent trees | Taller pointed leaders, separated branch tiers and scalloped bough edges make the crown read as an evergreen. The brighter foliage palette and canopy-shaped shadows remain. |
| #58 Northwall ancient tree | Added a central snow/foliage crown to fill the gap between the three surrounding masses, within its existing landmark budget and footprint. |

Tree shadow outlines now sample each outer crown ring at 24 points to follow
the scalloped branches. A small cache within each shadow pass reuses outlines
for matching regional tree shapes. Both mask paths still omit all cast and
contact shadows for the six rock layouts.

## Visual review

- [Bloodpool, straw and canvas houses: native / before / updated](../runs/sim-model-review-followup-2026-09-27/review-1.png)
- [Aitos chimney and stilt stairs: native / before / updated](../runs/sim-model-review-followup-2026-09-27/review-2.png)
- [Windmill, factory and trees: native / before / updated](../runs/sim-model-review-followup-2026-09-27/review-3.png)
- [Updated regional art index](../runs/sim-model-review-followup-2026-09-27/art-index/index.html)

These are production model/depth-renderer diagnostics, not live gameplay
captures. Comparison panels are independently enlarged for inspection; they
do not establish relative world scale. Generated assets require local ROMs
and remain in ignored `runs/`.

The visual pass covered all four detail levels and an oblique camera. Reversed
face submission was compared at normal and oblique angles across 166 image
pairs: 4,858 pixels changed at surface boundaries, with no solid 3×3 changed
interiors. This supports the overlap check but is not proof against artifacts
at every possible camera or light angle.

## Geometry and validation

The sweep covered 15,360 finished and 3,456 construction configurations, with
zero face-buffer overflows. Maximum authored face counts across the swept
variants are below; hidden-face cleanup can reduce the final counts.

| Family | Low | Balanced | High | Ultra |
|---|---:|---:|---:|---:|
| Houses | 61 | 83 | 166 | 248 |
| Windmill | 62 | 115 | 117 | 149 |
| Factory | 61 | 81 | 87 | 93 |
| Permanent evergreen | 61 | 86 | 232 | 328 |
| Ancient tree | 122 | 190 | 272 | 368 |

The ancient tree retains its enhanced 128/192/320/384 limits. Low-detail
windmills omit a separate base trim ring so the round tower and striped rotor
fit their existing budget.

- Full `ActRaiserRecomp` build passes.
- All ten targeted CTests pass: models, biome, model cache, cache storage, LOD,
  preset, palette, lighting, proportions and region.
- New geometry regressions check the removed house protrusion, attached straw
  decoration, upright tent walls, surviving open chimney flues, ascending
  stairs, sail width/stripe, pitched factory roofs/dormers/exterior walls and
  the ancient tree's center.
- The focused Metal shadow regression passes for both mask paths, opposite
  light directions, LOD stability, all six shadow-free rock layouts and a
  1,024-tree forest with clear tile corners.
- The broader world-navigation GPU suite was not rerun in this pass; its
  previously observed animated model-cache assertion is documented in the
  [shadow correction report](sim-tree-shadow-fix-2026-09-27.md).

```sh
ctest --test-dir build --output-on-failure \
  -R '^actraiser_sim_background_voxel_(models|biome|model_cache|model_cache_storage|lod|preset|palette|lighting|proportions|region)$'
mkdir -p runs/sim-model-review-shadow-check
build/actraiser_present_world_nav_gpu_test --voxel-shadows runs/sim-model-review-shadow-check
```
