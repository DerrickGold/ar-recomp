# SIM 3D model audit — 2026-09-27

Historical findings against working-tree model code at base commit `4407b525`.
The audit itself did not change runtime code. The subsequent authorized fixes
and additional architectural corrections are recorded in
[SIM model corrections](sim-model-fixes-2026-09-27.md); the evidence below retains
the original failing results for comparison.

The models generally share a coherent material palette and stepped vegetation
language. The largest problems are missing native identities, a capacity failure,
and inconsistent construction of a few surfaces. Fix those before adding detail.

## Findings, in recommended order

### 1. High: some Ultra tropical trees exceed box capacity and are skipped

The evergreen crown uses a 9×9×9 occupancy grid at Ultra. Tropical profile 2
fills the 320-box metadata capacity once its two trunk boxes are included.
Trim, Architectural and Varied then add two root-flare boxes, setting
`overflow`. This is a box limit, despite the finalized model having only 293
faces. The renderer skips an overflowing model entirely.

Evidence: `sim_background_voxel_models.c:72–85`, `:1277–1323`, `:2241–2247`;
`sim_background_voxel_models.h:15–22`;
`sim_background_voxel_renderer.c:1025` under [src/sim/voxels](../src/sim/voxels/).
The diagnostic sweep found six failing inputs out of 13,824: town 5, tree,
Ultra, styles 1/2/3, diagnostic seeds 2/6. One exact input is
`cell_x=2, cell_y=14, group=2, record_slot=2`; another is `(6,10,6,6)`.
The remaining probe fields and all results are in
[mesh_probe.c](../runs/sim-model-audit-2026-09-27/mesh_probe.c) and
[mesh-counts.csv](../runs/sim-model-audit-2026-09-27/mesh-counts.csv).

The input failure is reproduced through the production builder and the renderer
skip is verified in code. A disappearing tree was not captured in a live
campaign. The regular model test passes because its sampled identities do not
cover this failure.

Recommended fix: reserve capacity for mandatory trim, increase/prove the metadata
bound, or compact occupancy metadata without changing exposed surfaces. Add
coverage of all crown profiles across town, detail and style. Do not reduce the
visible canopy merely to work around build metadata.

### 2. High: the Marahna temple model is not connected to its native landmark

The model sheet displays a Marahna temple, but
[the landmark table](../src/sim/voxels/sim_background_voxel_landmarks.c)
at lines 20–29 explicitly excludes Marahna. Instead,
[the sanctuary table](../src/sim/voxels/sim_background_voxels.c) at lines 130–142
maps terrain `$C0` to the temple.

The verified native Marahna WRAM has an ordinary `$C2` cathedral at `(17,17)`
and four `$EF` landmark marks at `(6,20)`–`(7,21)`. Running the real landmark
classifier against that snapshot returns zero Marahna landmarks. Native special
plot data selects picture 16, which expands structure `$E4/$E5/$EC/$ED`.
This is separate from the ordinary cathedral.

Evidence: [classifier probe](../runs/sim-model-audit-2026-09-27/classification_probe.c),
[verified WRAM](../development/research/regional-maintenance-live-2026-09-21/verified-v1/usa-marahna/wram.bin),
US special-plot pointer table at file `0x01BBF8`, and index model **47**.
The synthetic `$C0` fixture in `tests/sim_background_voxels_test.c:1282–1292`
validates the incorrect identity. The empty-town checks in
`tests/sim_background_voxel_landmarks_test.c:66–70` describe Marahna as having
no reserved plot and never exercise its native `$EF` block.

The model also replaces the original's three gold peaked/tiered towers with a
single gabled shrine. Recommended fix: classify the `$EF` plot, keep the `$C2`
cathedral independent, then remodel the landmark around its verified silhouette.
Validate both identities together in the native snapshot.

### 3. Medium: regional model identity is missing

The Japanese pyramid eye is present in both native banks, but
`BuildPyramid` in [the builder](../src/sim/voxels/sim_background_voxel_models.c)
at line 1695 receives only detail. Neither the object identity nor the model
cache key carries the selected pyramid decoration. This is an already
[documented limitation](regional-media.md#install-japanese-town-artwork), not
evidence of a new regression.

The updated index also exposes another difference: developed Marahna houses
select family `$0E` in Japan and `$02` in all four Western releases. The
[current town/level table](../src/sim/voxels/sim_background_voxel_region.c)
at lines 19–46 always chooses the Western log cabin. The Japanese house variant
is a native-reference coverage gap; this audit does not establish an existing
user-facing regional house-art option.

Recommended fix: represent resolved artwork identity explicitly where a variant
is supported, propagate it through classification and caching, and render the
appropriate mesh/decorative layer. Respect the independent pyramid-art setting;
do not infer it from language or a global gameplay region.

See [regional comparison](../runs/sim-model-audit-2026-09-27/art-index/regional-differences.png)
and [index provenance](sim-model-art-index.md). Model numbers **29, 30, 46**.

### 4. Medium: the Low castle has an open keep and detached finial

At Low, `BuildBloodpoolCastle` calls `AddRoofedBox` for the central keep
(`sim_background_voxel_models.c:1637–1638`). This helper deliberately omits a
top, expecting a separate roof. No roof closes that Low branch. The keep ends
at `z=24`, while the gold finial starts at `z=26.6`, leaving a 2.6-unit gap.
Low corner towers also use top-less 5×5 shafts capped by smaller 3.8×3.8 boxes,
leaving exposed openings around the caps.

Evidence: [Low versus Ultra render](../runs/sim-model-audit-2026-09-27/castle-lod.png),
builder lines 359–367 and 1630–1657. Model **45**.
Recommended fix: close the coarse silhouette and connect the finial. Low should
simplify a complete building. Its current 53 finalized faces leave room below
the 64-face budget, though authored capacity must also be checked.

### 5. Medium: overlapping coplanar details make surfaces depend on draw order

The castle's dark doorway ends at front plane `y=31.4`, spanning
`x=12.5…19.5, z=3…8.5` (builder lines 1666–1667). Two later glass windows
overlap that door at exactly the same plane, `z=3.4…7.4` (lines 1676–1682).
They partly replace the intended dark opening. The factory similarly overlaps
its arms and spine across `x=21…22` on shared front/rear planes, using different
wall materials (lines 1111–1116). Its courtyard trim and roof also share a west
plane at `x=20.8` (lines 1117–1118 and 2230–2231).

The [D32 model pipeline](../src/platform/sdl/sim3d_depth_pass_pipelines_sdl.c)
uses `LESS_OR_EQUAL` at line 292. Reversing face submission in an isolated audit
renderer changes 213 castle pixels and 479 factory pixels in the default view.
See [normal/reversed/difference panels](../runs/sim-model-audit-2026-09-27/face-order-evidence.png).
These totals include shared raster edges; they are not counts of defective
pixels. Small order differences alone do not prove a modeling error. The
overlapping rectangles above are confirmed geometrically.

Recommended fix: split/union the primary surfaces and avoid windows over the
entrance. Give intended relief a deliberate separation from its backing surface.
A global depth bias would hide the modeling ambiguity rather than resolve it.

### 6. Medium: some house silhouettes depart from their source families

Northwall's developed source is family `$0A`, the same flat-topped family used
by Kasandora. The current Northwall mapping chooses `Stone`, whose
`BuildStoneHouse` adds a pitched gable (`sim_background_voxel_models.c:677–688`).
Snow coloring makes it regionally recognizable but does not preserve the source
silhouette. Compare models **17–18** with **35–36**.

All towns select the same rounded first-tier family `$00` in the native table.
The models instead use gabled tents in four towns and rounded yurts in two.
That is an artistic reinterpretation, not a runtime defect. The generic
alternate-house transform also compresses the main structure and adds a side
wing; it should be judged per source family rather than assumed to represent
every alternate source view.

The early/late original banks visibly change house and infrastructure treatment.
For example, Aitos's developed house changes from a flat striped mass to a much
barer slatted appearance. One fixed model does not retain that contrast. Resource
differences are verified; which bank/tier combinations occur during normal play
needs a separate reachability check before adding variants.

Recommended art direction: retain the native roof shape, facade bands, door
placement and distinguishing regional marks first. Keep new trim subordinate to
those features. Treat added snow coverage and town-specific reinterpretations as
explicit art choices. These are fidelity recommendations, not automated failures.

## Optimization while retaining quality

| Opportunity | Evidence | Recommendation |
|---|---|---|
| Share identical geometry across placements | 144 Basic/Ultra front houses, eight placements for each of 18 town/tier groups, produced one distinct serialized face/box set per group. The cache key still includes cell, group and slot for all kinds. | Normalize keys only where those fields cannot affect geometry, or intern compiled meshes. Keep placement, palette and shading identity separate. Recheck Varied, foliage, bridges and animation rather than dropping fields globally. |
| Avoid spending authoring capacity on buried details | Factory Ultra vents at builder lines 1163–1166 sit inside the spine wall. Twelve of their 15 box faces are culled; the remaining three west faces are coplanar with the wall. | Author the intended visible inset/relief directly. This saves build work/capacity; culled faces already cost no GPU triangles. |
| Merge compatible exposed foliage quads | The crown emits each exposed voxel side independently; current finalization removes buried/duplicate faces but does not merge coplanar surfaces. | Merge only when material, brightness and corner occlusion interpolation remain equivalent. Preserve the stepped boundary and color patches. Measure visible output before claiming a saving. |
| Simplify pyramid upper casing | An isolated Ultra prototype reduced 61 faces to 41 by replacing six coplanar strips per side with one. It changed 108 pixels in the default view and 55 obliquely, including boundary coverage. | Promising 20-face/40-triangle reduction, but **not validated as quality-neutral**. Resolve shading interpolation/raster boundaries before adoption. |

The [geometry-sharing result](../runs/sim-model-audit-2026-09-27/basic-house-geometry-sharing.json)
compares exported face coordinates, materials, brightness and box metadata; it
is not a cache/FPS benchmark. The [pyramid experiment](../runs/sim-model-audit-2026-09-27/pyramid-simplification.json)
was temporary and did not modify production sources.

The existing compiler already removes fully buried axis-aligned faces and exact
duplicate rectangles before corner-occlusion computation. Blanket removal of
“hidden geometry” would overstate the available GPU saving. Sloped/partially
overlapping surfaces and metadata capacity are the more useful next targets.

Observed finalized quad ranges in the synthetic sweep:

| Family | Low | Balanced | High | Ultra |
|---|---:|---:|---:|---:|
| Houses | 16–46 | 26–80 | 32–100 | 32–137 |
| Cathedral | 40 | 81–112 | 121–182 | 143–208 |
| Windmill | 52 | 61–87 | 78–109 | 118–139 |
| Factory | 51 | 72–97 | 121–165 | 124–167 |
| Evergreen | 29–41 | 66–105 | 124–191 | 209–303 |
| Broad tree | 41–43 | 117–127 | 148–168 | 189–223 |
| Palm | 54 | 94–104 | 109–119 | 119–129 |
| Shrub | 29–41 | 102–119 | 122–156 | 170–198 |
| Story tree | 42 | 117 | 162 | 212 |
| Castle | 53 | 93 | 121 | 139 |
| Temple | 26 | 62 | 88 | 108 |
| Pyramid | 33 | 37 | 50 | 61 |
| Bridge | 25 | 45 | 46 | 48 |

These are builder coverage ranges, including town/kind combinations not proven
reachable in the campaign. The evergreen range includes flagged failures; face
count alone does not establish a valid model. No frame-time claim is made.

## Coverage and evidence

- Rendered all 67 production-sheet entries with production geometry, cache,
  lighting and D32 rendering. Reviewed original/model contact sheets across all
  families. Retained [the current model sheet](../runs/sim-model-audit-2026-09-27/model-sheet.png).
- Decoded 670 source references: five retail releases × two character banks ×
  67 entries. All image references resolve and are nonempty. ROM/asset provenance
  is in the generated [index](sim-model-art-index.md).
- Compared regional pixel hashes: six Japanese rows differ from US (two house
  views plus pyramid, in both banks); Europe/Germany/France match US throughout
  this indexed set.
- Swept 13,824 builder inputs across 13 kinds, six towns, all four details and
  four styles, eight placement seeds, and house tiers/facings. Six overflow cases
  were reproduced. This is broad synthetic coverage, not an exhaustive seed or
  complete in-game scene sweep.
- Rendered all entries at Low, plus Ultra default/oblique views with normal and
  reversed face submission. Confirmed the reported defects against coordinates;
  did not classify every shared-edge pixel difference as z-fighting.
- Rebuilt and ran the existing `sim_background_voxel_models_test`: passed.
  Ran the native-WRAM Marahna classifier probe: zero landmarks, `$EF` plot,
  `$C2` sanctuary.
- Verified the gallery's release/search filters and Japanese eye comparison in
  the browser. Low-to-High transitions, terrain joins, actor occlusion and every
  camera/light setting still need campaign-level acceptance checks after fixes.

Recommended implementation order: tree capacity and native landmark dispatch;
castle closure and overlapping surfaces; artwork identity propagation; source
silhouette corrections; then measured geometry/cache optimization.
