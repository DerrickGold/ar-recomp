# SIM model recognition refinements — 2026-09-27

Implemented the six groups of changes from the [recognition audit](sim-model-recognition-audit-2026-09-27.md), including the ancient tree's separate landmark allowance requested during implementation.

Start with the [before/after overview](../runs/sim-model-polish-2026-09-27/polish-overview.png) or the refreshed [regional art index](../runs/sim-model-polish-2026-09-27/art-index/index.html). The index retains 730 native comparisons, the original 73 numbers and three regional renders. Three supplementary construction renders do not change those numbers. Generated references require the local ROMs and remain under ignored `runs/`.

## Implemented changes

| Audit finding | Result |
|---|---|
| Mechanical tree crowns and collapsed Low silhouettes | Surface-authored evergreen skirts and three broad leaf clusters on a visible forked trunk. Every detail level keeps the main shape. Evergreen material groups remain distinct under overhead lighting. |
| Ancient tree reads as a column of blocks | Broad snow-covered clusters, a taller branching trunk and four spreading roots. Its logical 2×2 plot is unchanged; geometry remains within 32×32 and reaches about 29.7 units high. |
| Weak regional house identities | Pale straw starter shelters, pale Kasandora masonry, stone/timber Aitos terraces, and log/timber cues retained at Low. Roof courses follow the actual supporting surfaces. Rectangular house depth is reduced to expose the facade at the production camera angle; global height scaling is unchanged. The round starter hut and white canvas tent retain their depth. |
| Stilt-house floor and supports obscured | Raised floor, taller visible posts, an open alternate-facing veranda, and individual post contacts. Slope foundations no longer fill the whole undercroft. Applies to the Japanese developed-house variant too. |
| Generic construction | Two peaked house frames with partial walls, open factory wings with supported rafters, and three progressively taller round windmill stages. The last mill stage has its roof and projecting entrance before the rotor appears. |
| Low loses important features | Four cathedral columns flank the entrance. Low factories retain two chimneys within the existing 64-face limit by simplifying their cap surfaces. |
| Floating/overlapping decoration | Closed the starter roof's tip; replaced floating accents with material variation and surface-following bands. Cathedral relief lies within the main gable; the extra portico/window ornaments were removed. Fillmore window and door frame pieces now meet without overlapping corner caps. |
| Bush underside lighting | The existing crown keeps its shape and uses explicit outward normals. Snow conversion also respects downward foliage normals and authored dark needle pockets. |

Completed windmills, castle, Marahna palace, pyramids, palm and rock silhouettes retain their prior designs. Bridge construction was added to the diagnostic coverage; its existing geometry is retained.

The original cathedral diagnosis overstated the detached crest: a secondary portico supported part of it. The genuinely unsupported diamond and inconsistent relief depths still warranted cleanup. The implementation mounts the complete relief on one verified backing triangle.

## Budgets and surface cost

Values below are maximum **authored** face counts in the completed-model sweep, before removing buried box faces. Budgets apply at that stage, so final counts cannot conceal an overflow.

| Family | Low | Balanced | High | Ultra |
|---|---:|---:|---:|---:|
| Ordinary houses | 62 | 88 | 133 | 175 |
| Evergreen | 53 | 86 | 200 | 264 |
| Broad tree | 64 | 134 | 232 | 352 |
| Ancient tree | 122 | 180 | 320 | 368 |
| Ancient-tree allowance | 128 | 192 | 320 | 384 |
| Cathedral | 48 | 136 | 176 | 231 |
| Factory | 58 | 111 | 199 | 202 |

Ordinary allowances remain 64/160/256/384. Broad and ancient trees deliberately spend more faces on their branch-supported contours. Evergreen Ultra drops from the previous sweep's maximum 309 to 264 authored faces. No global face allowance was increased.

The box-metadata sweep now peaks at 45 records, previously 322. Removing occupancy-grid foliage permits reducing temporary model capacity from 336 to 96 boxes, saving 5,760 bytes per scratch model. Cached face layout is unchanged.

## Verification

- Full `ActRaiserRecomp` build passed. The existing macOS common-section alignment warning remains.
- All ten targeted suites passed: models, biome, model cache, cache storage, LOD, preset, palette, lighting, proportions and region.
- **17,664 builder inputs, zero overflows**: 15,360 completed combinations plus 2,304 construction combinations, including both bridge axes and construction phases.
- Regression checks cover closed foliage shells and their pole triangles, bush underside lighting, ancient-tree bounds/budgets, open stilt contacts, supported facade relief, window-frame joints, factory chimneys/courtyard, and construction progression.
- **79 renders per view**: 76 indexed entries plus three supplementary construction states. Rendered Low, Balanced, High and Ultra, oblique and lower-camera/45° light views, and reversed submission order at the default and oblique cameras.
- The 158 reversed-order comparisons retain 5,272 changed pixels along small raster boundaries across 154 images, with **zero fully changed 3×3 interior patches**. These are not pixel-identical results or proof of every possible camera; the remaining boundary differences are recorded in [draw-order.json](../runs/sim-model-polish-2026-09-27/draw-order.json).

Evidence: [test log](../runs/sim-model-polish-2026-09-27/tests.log), [completed sweep](../runs/sim-model-polish-2026-09-27/geometry.csv), [construction sweep](../runs/sim-model-polish-2026-09-27/construction-geometry.csv).

These are isolated renders through production model, palette, lighting, projection and D32 paths, at the production audit camera and additional diagnostic views. This pass did not run a new live-town gameplay session. Native early/late character banks remain separately indexed references; bank-dependent runtime variants were not inferred from unverified resource combinations.

## Visual review

- [Fillmore and Bloodpool houses](../runs/sim-model-polish-2026-09-27/review-1.png)
- [Kasandora and Aitos houses](../runs/sim-model-polish-2026-09-27/review-2.png)
- [Marahna and Northwall houses](../runs/sim-model-polish-2026-09-27/review-3.png)
- [Infrastructure and landmarks](../runs/sim-model-polish-2026-09-27/review-4.png)
- [Vegetation](../runs/sim-model-polish-2026-09-27/review-5.png)
- [Construction, bridges, rocks and regional variants](../runs/sim-model-polish-2026-09-27/review-6.png)

Native previews are enlarged independently. Within each before/after card, model crops share a scale. The regional gallery preserves the model sheet's common world scale.

`tools/sim_voxel_model_sheet.c` now supports optional `AR_AUDIT_DETAIL=0..3`, `AR_AUDIT_TILT_X`, `AR_AUDIT_TILT_Y`, `AR_AUDIT_LIGHT_ELEVATION`, and `AR_AUDIT_REVERSE=1` diagnostic controls. Defaults remain Ultra/Varied with the existing camera. Follow the [art-index reproduction instructions](sim-model-art-index.md) with defaults to regenerate the native comparison gallery.
