# Evergreen lighting and rock shadows — 2026-09-27

The town screenshot exposed a mismatch between the new evergreen geometry and
its older shadow proxy. Trees and rocks were still extruding a full 16×16 tile
into the shadow mask. Adjacent forest cells consequently darkened almost all
the ground, including the open space between round crowns; scattered stones
left the same large rectangular patches.

The evergreen now casts a convex outline derived from the same crown profile
as its model, including its regional size, deterministic variation, canopy tip,
trunk foot, presentation lean and light direction. Twelve samples per outer
crown ring keep the shadow stable between detail levels without submitting all
foliage faces. Pairs of silhouette triangles share a quad, and batches flush
before capacity is exhausted. Both screen-space and native-town shadow masks
use this path. The existing mask opacity and blur controls still apply.

The later [model review follow-up](sim-model-review-followup-2026-09-27.md)
increases this to 24 samples per outer ring for scalloped evergreen branches
and reuses matching outlines within each shadow pass.

Evergreen green midtones and highlights are brighter, with fewer near-black
patches on upward-facing foliage. Dark branch undersides remain distinct; the
mesh's own lighting supplies depth rather than applying the native sprite's
baked darkness twice. Snow overrides remain unchanged.

All six rock layouts (boulder `$61`, scattered `$62/$63/$69/$6A/$6B`) now omit
both cast shadows and ground contact decals. Their facet lighting remains.

## Review

- [Before/after and GPU shadow previews](../runs/sim-tree-shadow-fix-2026-09-27/comparison.png)
- [Forest preview](../runs/sim-tree-shadow-fix-2026-09-27/forest-preview.png)
- [Refreshed regional art index](../runs/sim-tree-shadow-fix-2026-09-27/art-index/index.html)

The previews use the production model, depth renderer and shadow-mask entry
points. The focused scene captures composite a hard mask at 35% opacity over
plain ground so the outline is visible; they are diagnostic renders, not live
gameplay captures. The art-index thumbnails omit cast shadows, so the separate
shadow captures are necessary evidence. Generated assets remain in ignored
`runs/`.

## Validation

- Full `ActRaiserRecomp` build passes.
- Five focused CTests pass: models, palette, lighting, model cache and proportions.
- Model regression checks cover six towns, four variations, five light shears
  and all four detail levels. Projected model vertices remain within 0.4 authored
  pixels of the inexpensive outline; overhead outlines leave tile corners open.
- Metal shadow regression passes for both mask entry points, opposite light
  directions, detail-level stability and all six shadow-free rock variants.
- A 1,024-tree GPU mask confirms every caster survives multiple batch flushes
  and every tile corner stays clear.
- The broader world-navigation GPU suite still fails its existing animated
  windmill model-cache-count assertion (`TestAnimatedTownCache`, expected 3/72
  cache hits). It also fails when run before the new shadow tests. That broader
  cache issue is not resolved by this change.

Reproduce the focused GPU check with an existing output directory:

```sh
mkdir -p runs/sim-tree-shadow-check
build/actraiser_present_world_nav_gpu_test --voxel-shadows runs/sim-tree-shadow-check
```
