# SIM model recognition and style review — 2026-09-27

**Status at the time of this review:** the set is not yet uniformly at the standard of the refined landmarks and
palm. The main remaining art work is forest canopies, the ancient tree,
house material/structure cues, and construction stages. Several smaller
attachment, lighting and Low-detail issues also remain.

The findings were subsequently addressed in the [recognition refinement pass](sim-model-polish-2026-09-27.md). The evidence below remains the pre-change review. The implementation report also corrects the cathedral crest's partial support by a secondary portico.

This review uses the user's updated direction: a model should communicate
what it represents naturally, while keeping the native art's palette,
proportions and distinguishing features. Pixel-for-pixel reconstruction is
not the goal. A round tower, branching tree or readable timber frame matters
more than adding small decorative boxes.

## Coverage

Reviewed all **76 indexed render entries**: 73 baseline entries and three
regional variants. These are presentation variants, not 76 independent model
builders. The complete
[per-entry checklist](../runs/sim-model-recognition-audit-2026-09-27/index.html)
links each decision to a comparison sheet; a
[TSV inventory](../runs/sim-model-recognition-audit-2026-09-27/review-inventory.tsv)
is also available.

- [Fillmore and Bloodpool houses](../runs/sim-model-recognition-audit-2026-09-27/review-1.png)
- [Kasandora and Aitos houses](../runs/sim-model-recognition-audit-2026-09-27/review-2.png)
- [Marahna and Northwall houses](../runs/sim-model-recognition-audit-2026-09-27/review-3.png)
- [Infrastructure and landmarks](../runs/sim-model-recognition-audit-2026-09-27/review-4.png)
- [Vegetation](../runs/sim-model-recognition-audit-2026-09-27/review-5.png)
- [Bridges, construction, rocks and regional variants](../runs/sim-model-recognition-audit-2026-09-27/review-6.png)

Each sheet pairs early/late native references with Ultra, Low and oblique
production renders from the just-completed palm pass. Western entries use
US references; the three Japanese variants use their corresponding Japanese
art. The existing 730-row index supplies provenance across five releases.
Original previews are enlarged independently; model crops share a scale only
within each card. Panel sizes are not evidence of relative world size.

Additional renders test 45° lighting and a lower camera. The
[focused camera/light comparison](../runs/sim-model-recognition-audit-2026-09-27/shape-vs-presentation.png)
helps separate modeling choices from presentation. Runtime models were not
edited during this review.

## Findings, in recommended order

### 1. High art priority: remaining tree crowns need the same care as the palm and bush

**Entries 48–55 and 58.** Ultra evergreens retain a pointed tree silhouette,
but their rectangular tiers and sparse color patches look mechanically stacked
beside the newer foliage. Broad trees conceal most of their trunk and roots;
their large, nearly uniform upper surface reads more as a green mass than a
branching tree. The lower camera helps expose some trunk but does not resolve
the crown construction.

Low is the clearest failure: evergreens become squares or crosses, broad trees
become rectangular green blocks, and the ancient tree becomes a narrow
blue-white column. The ancient tree's Ultra silhouette also needs a broader,
spreading-tree read rather than a tall pile of snow blocks.

The cause is explicit in `BuildVoxelCrown`: Low samples every crown on a 3×3×3
grid; higher tiers only refine that same occupancy approach
(`sim_background_voxel_models.c:1354–1559, 1779–1816`). The ancient tree uses
a 23-unit crown span and 22.5-unit crown height, so it inherits the broad-tree
sampling problem at landmark scale.

**Recommendation:** author a complete coarse crown for each family—layered
needle clusters for evergreens, several broad leaf masses with visible
branching for broad trees, and a wide snow-laden crown for the ancient tree.
Keep these masses at Low, and spend higher detail on contour and leaf/snow
grouping. Preserve the chunky game language without making voxel-grid cells
the defining visual feature.

### 2. Medium art priority: house silhouettes read as houses, but family identity is weak

**Most house entries 1–36, plus Japanese 29–30.** Large roof planes dominate
the inspected views and leave narrow strips for timber, logs, stilts and
masonry. A lower camera improves the façade, so this is partly presentation,
not evidence that every roof needs rebuilding. However, lower lighting alone
does not recover the missing material cues.

The clearest family-specific problems are:

- Starter shelters read as rounded huts, but their orange-brown roofs and
  broad planar surfaces lose the source's pale straw/canvas character.
- Kasandora's developed family `$0A` has the right flat roof but a tan/orange
  treatment where the native art is predominantly pale masonry. Northwall's
  version of the same family preserves that material character better.
- Aitos's large orange roof suppresses the source's stone/timber banding;
  the late-bank lattice appearance is particularly distinct.
- Marahna's stilt houses need a more obvious raised floor, open undercroft
  and supports. The roof and enclosed front currently dominate. Log cabins
  need their courses to remain recognizable at Low as well as Ultra.

Evidence: `BuildYurtHouse`, `BuildTimberHouse`, `BuildAdobeHouse`,
`BuildAitosHouse`, `BuildMarahnaStiltHouse` and `BuildMarahnaLogCabin`
(`models.c:735–943`); `SetYurtHouse`, `SetAdobeHouse`, `SetAitosHouse`
(`sim_background_voxel_palette.c:107–207`); house height scale `0.68` in
`sim_background_voxel_proportions.c:3–6` and per-model facing in
`sim_background_voxel_project.c:150–180`.

**Recommendation:** review façade visibility in the actual town camera before
changing global proportions. Then restore each family's defining materials
and supports using restrained surface bands, clear openings and modest depth.
Do not substitute regional stereotypes for the native palette. The white
canvas tent and Northwall flat stone house already have sound main identities.

### 3. Medium art priority: construction stages do not communicate the building being made

**Entries 63–67.** House and factory construction are largely flat platforms
with posts and crossbars. The native house scaffold has a peaked structural
frame; the factory already shows its partial wings and roof framing. Those
cues are missing from the common rectangular scaffold.

Windmill construction grows a round stump inside the same frame, but even its
last construction phase has no roof or projecting entrance. The paired native
last frame already shows both. This creates a larger visual jump to completion
than the original sequence.

Evidence: `BuildConstructionFrame` (`models.c:527–550`), the windmill
construction branch (`1164–1175`), and factory construction (`1252–1257`).
The final windmill construction body reaches height 12, versus 22 for the
completed tower before its roof.

**Recommendation:** preserve each building's future outline as it develops:
house rafters, partial factory wings, and a progressively completed windmill
tower/entrance/roof. A generic work-site frame can supplement those masses.

### 4. Medium refinement: Low should retain the cathedral and factory's strongest cues

**Entries 37–38 and 43–44.** The columned sanctuary and courtyard factory are
recognizable at Ultra. Low makes them more generic than necessary.

The cathedral uses three Low columns. Its middle column occupies `x=15…17`,
while the nearer dark door occupies `x=14…18` and covers most of its height;
only the flanking columns read clearly (`models.c:1017–1031`). The factory
falls from four chimneys to one (`models.c:1292–1309`).

**Recommendation:** distribute a small set of columns around the doorway,
retain a clear pediment, and preserve a stronger industrial silhouette at
Low. Reallocate surface cost first: the factory authors 58 faces against a
64-face Low allowance, so blindly adding another complete chimney would
overflow even though its finalized count is only 51.

### 5. Confirmed surface defects: some decorative elements are detached

The starter hut's Ultra Varied accent at `y=14.12…14.38, z=4.8…6.2`
(`models.c:2682–2685`) is outside its sloping roof. At `z=4.8`, even the
roof's furthest forward extent is `y=13.84`; it recedes further with height.
The accent therefore has no backing surface. This is a geometric attachment
defect, independent of whether it happens to overlap the roof in the image.

The cathedral's winged crest also starts at `y=31.2`, while its front gable
ends at `y=30.5`. Its diamond window is farther forward at `y=31.82`.
The high ornament floats in front of the gable rather than being mounted on it
(`models.c:1013–1015, 2456–2519`).

**Recommendation:** place relief and inlays on the actual supporting planes;
remove or reshape details that do not contribute to the building's identity.

### 6. Confirmed lighting defect: the bush's lower surface still uses the legacy normal rule

**Entry 57.** Its revised shape and palette communicate a bush well. It does
not need another silhouette replacement. However, `BuildShrub` still emits
its closed curved surface with the legacy winding mode (`models.c:1570–1605`).
The normal resolver forces its downward normals upward. A fresh probe found
16/36/64/100 affected faces at Low/Balanced/High/Ultra respectively.

**Recommendation:** apply the explicit outward-winding convention introduced
for the palm to the shrub's curved surface, verifying the pole triangles and
lower hemisphere. This is a small lighting correction; retain the current
bush design. Evidence is in
[normal-check.txt](../runs/sim-model-recognition-audit-2026-09-27/normal-check.txt).

## Models whose main direction should be retained

| Family | Assessment |
|---|---|
| Finished windmills | Round towers, projecting entrances and rotors communicate windmills at every inspected detail. |
| Bloodpool castle | Gate, recessed curtain walls, six tower profiles and central keep preserve a compact fortress. |
| Marahna palace | Three principal masses, bulbous side towers, entrance pillars and enclosure retain its distinctive temple identity. |
| Egyptian pyramid, plain and eye variants | Continuous casing, broad base and masonry are appropriate; the regional eye survives Low. |
| Palm | Curved fronds, visible stem, stable Low silhouette and brighter greens meet the revised direction. |
| Bush | Shape and leaf grouping are sound; fix the underside lighting described above. |
| Boulders and scattered rocks | Read as angular stone, with distinct native counts and arrangements at all details. |
| Bridges | Decks, parapets and stone/ice treatment read as crossings; judge them with water/banks before adding structural detail. |
| White canvas tent and Northwall stone house | Main family silhouettes and material identities are sound; avoid unnecessary redesign. |

Snow added to some architecture is an artistic interpretation, not automatically
a defect. Conversely, some native early/late art changes—especially Aitos's
lattice appearance and the cathedral crest—are not represented as independent
bank-dependent models. Confirm which combinations occur during play before
expanding that variant work; the gallery contains resource combinations, not
proof of their in-game reachability.

## Cost, scope and next pass

The current post-palm sweep already covers 15,360 builder inputs with zero
overflows. It is reused here because runtime model sources were unchanged
during the review. Low crown authoring peaks at 47 faces for evergreens,
44 for broad trees and 47 for the ancient tree, against a 64-face allowance.
Their visual shortcomings therefore cannot be explained by a universal lack
of face capacity. Reauthoring the major surfaces is more useful than another
global budget increase.

The checklist records 23 entries to retain, 38 to refine, nine crown reworks,
five construction-stage reworks and one lighting correction. Repeated towns,
orientations and regional variants inflate those entry counts; they represent
a much smaller number of shared builders.

Recommended implementation order: fix the proven attachment/normal defects;
refine the shared forest and ancient-tree crowns; improve house material and
façade cues; then make construction and Low infrastructure preserve identity.
Use native-size town views as well as enlarged isolated renders for acceptance.

Evidence is under `runs/sim-model-recognition-audit-2026-09-27/`, including
review sheets, additional lighting renders, diagnostic source and a hash of
the inspected runtime sources. This is an art/recognition review backed by
targeted geometry checks. It does not certify every seed, intermediate detail,
animation, construction subtype, bridge span, terrain slope or live-town
overlap. Bridge construction is not present in the current 76-entry art index.
