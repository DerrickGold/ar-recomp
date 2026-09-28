# SIM shelters and alternate-house refinement — 2026-09-27

The early straw shelters and Kasandora canvas tents now use grounded coverings and their own construction frames. Alternate-facing houses preserve their visible crown, awning, dormer and roof-access features. The window cleanup removes projecting dark boxes and the generic trim that could cross a canvas entrance or extend beyond a regional roof.

Review the [native/front/alternate comparison](../runs/sim-shelter-refinement-2026-09-27/alternate-details.png), [before/after overview](../runs/sim-shelter-refinement-2026-09-27/focus.png), [construction stages](../runs/sim-shelter-refinement-2026-09-27/construction-review.png), or [complete art index](../runs/sim-shelter-refinement-2026-09-27/art-index/index.html). Generated images and ROM-derived references remain in ignored `runs/`.

## Changes

| Family or issue | Result |
|---|---|
| Shared early straw shelter, including Aitos, Marahna and Northwall | Steep straw covering reaches the ground, with a bound doorway, supported pole tips and surface-following woven detail. The alternate has a broader crown, four exposed pole ends and a horizontal binding. The shared native family still receives the same geometry across towns. |
| Kasandora middle tent | Canvas reaches the ground over A-frames, with a sagging ridge, split entrance flaps, pegs and guy lines. The alternate lifts its entrance flap into an awning on two grounded poles. The old solid wall box, rectangular foundation plinth, fascia crossbeam and invented side wing are removed. |
| Timber-family alternates | Small roof dormers replace generic ground-level wings on timber, Bloodpool, Marahna stilt and log-cabin families. Their windows and sills sit on the dormer walls above the supporting roof. The stilt model keeps four grounded supports and its open undercroft. These are interpretations of the projections visible in the tiny native sprites. |
| Flat masonry alternates | Adobe/stone houses have a raised roof-access structure and window; Aitos has its small roof block and opening. Each joins the existing terrace. Alternate authored-height bounds include these additions. |
| Fine roof details | Timber roofs use a straw palette, with staggered short surface patches at High/Ultra. Fillmore's chimney emerges from the right roof slope. Detail patterns remain attached to the roof planes. |
| Window artifacts | Timber, adobe/stone and Aitos doors/windows are shallow facade inlays. Their former projecting boxes, duplicate posts and raised masonry courses are removed or replaced by attached bands. Generic house fascia is no longer appended after the regional builder. |
| Snow windmill | Steep roof planes receive snow, including the front of the tapered main roof. Roof snow applies at every detail level; vertical gables stay clear. The separate High+ foliage rule is retained. |
| Construction | Straw shelters show a lashed radial frame followed by partial straw coverage. Canvas tents show paired A-frames and ridge cord followed by partial canvas. Ordinary houses retain their wall-and-gable construction. Buried canvas ridge supports are omitted from the finished mesh. |

The native construction images are shared; construction architecture is selected from the existing town/development identity rather than inferred from the shared scaffold metatile. The two construction stages follow the resolved family without introducing another setting or cache-key dimension.

## Verification

- Built `ActRaiserRecomp` and the ten targeted SIM model, biome, cache, LOD, preset, palette, lighting, proportions and region test targets. All ten tests pass. The linker retains its existing `__DATA,__common` alignment warning.
- Checked 15,360 finished configurations and 3,456 construction configurations across towns, house tiers, alternate flags, detail levels and styles: **18,816 configurations, zero overflows**. [Geometry summary](../runs/sim-shelter-refinement-2026-09-27/geometry-summary.json).
- Maximum authored house face counts are **61 / 76 / 166 / 248** at Low / Balanced / High / Ultra, within the unchanged **64 / 160 / 256 / 384** budgets. No increased shelter budget is needed.
- Added regression checks for grounded shelter skins, distinct family construction stages, alternate identity at every detail level, facade-plane windows, alternate roof bounds, canopy contacts, and steep-roof snow without snow on vertical gables or steep foliage.
- Rendered 83 panels at all four detail levels, another camera tilt and lower lighting. The set contains 73 historical entries, three regional renders and seven supplemental construction states; the native index retains 730 provenance rows.
- Compared 166 normal/reversed submission pairs. There are 5,273 changed boundary pixels across 161 images and **zero fully changed 3×3 interior patches**. This is not pixel-identical output or proof for every possible camera. [Draw-order results](../runs/sim-shelter-refinement-2026-09-27/draw-order.json).

Verification uses the production model/cache, palettes, material lighting, projection and D32 renderer on a common flat terrain datum. A new live-town gameplay session was not run for this pass. The index continues to display native early/late banks separately; these references are not a claim of newly implemented runtime bank-specific house variants.

## Reproduce the gallery

Run `tools/make-sim-voxel-model-sheet.sh` to render the production sheet, then `tools/make_sim_voxel_regional_index.py --renders <render-directory> --out <index-directory>` with the local retail ROMs available. The sheet's `AR_AUDIT_DETAIL`, `AR_AUDIT_TILT_X/Y`, `AR_AUDIT_LIGHT_ELEVATION` and `AR_AUDIT_REVERSE` controls reproduce the diagnostic views. `state-manifest.tsv` now includes both straw and canvas construction stages alongside the second ordinary-house stage and both bridge axes.
