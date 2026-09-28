# SIM model corrections — 2026-09-27

Implemented the [model audit](sim-model-audit-2026-09-27.md) findings and the
additional architectural corrections from the original-art review. The
[regional art index](sim-model-art-index.md) now selects the corresponding 3D
variant beside each original reference.

## Architecture and source identity

- **Windmills:** tapered octagonal towers, projecting gabled entrances, curved
  trim and attached facade markings. Construction stages also build the rounded
  tower. All three rotor poses retain clearance from the building.
- **Bloodpool castle:** separate curtain walls, four corner towers and two
  taller towers attached to the central keep, with visible courtyard ground.
  A hipped keep roof with a short ridge and pointed spires replace the
  dome and disconnected finial. The keep facade rises to 20 units, with its
  roof ridge at 26, reducing the roof rise from 11 to 6 units to match the
  original's roof-to-wall balance. Windows and stone courses follow the taller
  facade. Low detail retains closed roofs.
  The front corner shafts reach the ground in front of the wall ends; curtain
  trim stops between them. A projecting gatehouse adds thick stone piers, a
  heavy coping and an open arched passage. The curtain panels sit 2.5 units
  behind the gate and corner tower fronts; their ground supports preserve
  those recessed bays. Shorter front towers, a narrower keep, and restrained
  overall height preserve the native proportions; invented crenellations and
  blue windows are removed. Contact shadows and terrain foundations follow
  separate masses, so slopes cannot put the old full-plot slab back into the
  courtyard.
- **Marahna palace:** a taller Khmer-inspired central prang, conical side
  mounds with rounded bellies and decorated tips, doorway supports, and two
  freestanding pillars in the front courts. Continuous outer walls connect the mounds, front edges,
  and gateposts, with narrow foundations and visible garden ground. Openings
  beside the entrance lead into the courts rather than being blocked by rails.
  The side mounds have no invented windows. The original pale-gold palette and
  three-tower composition guide the interpretation.
  Both town and world classifiers use the native `$EF` landmark, independently
  of Marahna's ordinary `$C2` cathedral. The incorrect `$C0` signature is removed.
- **Kasandora pyramid:** four continuous Egyptian slopes, flat staggered masonry
  markings and smooth upper casing. No stepped platforms or invented doorway.
  The Japanese eye survives every detail level. The active town uses its accepted
  artwork snapshot and donor availability; retained world models use the most
  recent accepted town-art generation. Pending choices and missing donors do not
  produce an eye absent from the corresponding active artwork policy.
- **Houses:** shared first-tier family `$00` uses rounded yurts in all towns.
  Northwall's developed stone family keeps the native flat roof, including its
  alternate wing. A decoded finished house metatile selects the source family,
  so Japanese Marahna stilt houses do not alias Western log cabins in the cache.
  This adds source-driven model support, not a new regional house-art setting.

## Structure, lighting and capacity

Factory arms now meet the spine without overlapping different wall materials.
Courtyard trim is separated from its backing surface, and the previously buried
vents are authored directly as visible wall insets. Castle windows no longer
cover the doorway.

Steep roof normals previously used the vertical-wall winding correction when
`nz < 0.5`. That inverted the pyramid casing under overhead light and made its
mortar resolve to the same palette color. The correction now distinguishes
vertical walls from sloped roofs; a lighting regression covers this case.

The largest Ultra tropical crown requires 322 temporary boxes after trunk and
root trim. Capacity is now 336, with coverage of every crown profile, town,
detail and style. Visible foliage and the 384-face Ultra limit are unchanged.
This metadata is discarded when a compact mesh enters the cache.

The follow-up landmark review retains all six castle spires, temple pillars,
and complete enclosure walls even at Low detail. The unique buildings have
explicit object budgets; repeated town buildings keep the original limits:

| Model | Low | Balanced | High | Ultra |
|---|---:|---:|---:|---:|
| Regular model budget | 64 | 160 | 256 | 384 |
| Bloodpool castle budget | 128 | 160 | 256 | 384 |
| Castle final faces | 124 | 132 | 140 | 146 |
| Marahna temple budget | 144 | 256 | 320 | 384 |
| Temple final faces | 140 | 249 | 297 | 345 |

Budgets are enforced before surface optimization. Buried curtain end caps and
pillar joints are omitted, while the visible wall thickness and closed roofs
survive every detail level. Castle spire caps retain their eight sides and the
same silhouette bounds even at Low detail. The castle's front cap overhang is
included in the measured scene bounds. Both landmarks now use the original sprite's pale
gold/stone colors; Marahna's architecture no longer borrows the ground greens.

Basic houses now share cached meshes across placements when their source family,
orientation and other geometry inputs match. Varied houses, foliage, animation
and bridge endpoints retain their existing identity. The audit's speculative
foliage-quad merging was not applied, and no frame-time improvement is claimed.

## Validation

The local evidence is in `runs/sim-model-fixes-2026-09-27/`:

- `mesh-counts.csv`: 13,824 synthetic builder inputs, **zero overflows**; the
  original sweep had six. This samples eight seeds and all details/styles, not
  every possible live town configuration. Eye variants and construction poses
  have additional regression coverage.
- `test-results.log`: model, cache, classification, parallel capture, frame
  metadata, lighting, regional runtime and repository checks.
- `build-app.log`: full `ActRaiserRecomp` build.
- `native-classification.txt`: verified Marahna WRAM now returns one `$EF`
  landmark while retaining its `$C2` sanctuary.
- `normal/`, `oblique/`, `low/`, `reversed/`: 70 production-depth renders per
  view (67 baseline models and three regional variants).
- `model-corrections.png`: original art beside normal, oblique and Low renders
  of the four remodeled families, including both pyramid variants.
- `art-index/`: 670 original comparisons across five releases and two art banks,
  with separate Japanese eye and developed-house renders. Every image link was
  checked. The maintained model sheet also has a regional-variants section.

The subsequent castle/temple refinement is recorded separately in
`runs/sim-model-refinements-2026-09-27/`, preserving the earlier images for
comparison. `landmark-refinements.png` shows the original art, previous model,
revised model, oblique view and Low detail. The refreshed `art-index/` includes
the same regional coverage with the latest geometry. `castle-gatehouse.png`
shows the entrance refinement in normal, oblique and Low views.
`castle-keep-proportions.png` compares the subsequent shorter keep roof and
taller front facade against the original art and preceding model. The regression checks
cover the six castle peaks, short roof ridge, grounded front shafts, open arch
passage, projecting gatehouse, recessed curtain bays, temple
pillars and gateposts, rounded side mounds without windows, connected wall
runs, open court entrances, courtyard foundation clearance, and the landmark
budget exceptions. The final 13,824-case sweep has zero overflows; the full app
build and model/cache/palette/lighting/LOD/proportion/region checks pass. Normal, both oblique directions, all four details, and reversed submission
order are rendered. These checks establish capacity and rendering behavior;
source likeness is reviewed separately in the visual comparison.

Geometry regressions check the castle courtyard and foundation clearance, roof
closure, round windmill walls and entrance, three temple peaks, continuous
pyramid planes, source-family selection, artwork invalidation and the confirmed
coplanar conflicts. Reversed-order pyramid renders match exactly. Other models
still have shared-edge raster differences; those alone are not evidence of a
surface conflict.

This verification uses native data and isolated production rendering. It does
not claim a complete campaign playthrough across every terrain, camera and
lighting combination. Early/late art-bank variants remain indexed references;
unverified bank/tier combinations were not turned into new runtime variants.
