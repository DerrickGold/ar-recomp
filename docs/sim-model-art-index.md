# SIM 3D model art index

The current review index compares the production model sheet with original
pixels from each supplied retail ROM. It retains the existing 67 model numbers,
appends six rock layouts as 68–73, and records release, town, native house family, early/late character bank, metatile
composition, asset offsets, and pixel hashes. An auxiliary manifest adds three
regional renders (pyramid eye and both Japanese developed-house views) without
renumbering the baseline.

Open the locally generated [regional gallery](../runs/sim-straw-huts-2026-09-27/art-index/index.html)
or [provenance TSV](../runs/sim-straw-huts-2026-09-27/art-index/art-index.tsv).
See the [original audit](sim-model-audit-2026-09-27.md) and the
[implemented corrections](sim-model-fixes-2026-09-27.md) and
[environment models](sim-environment-models-2026-09-27.md). Generated images require local ROMs and remain in ignored
`runs/`; the generator and documentation are the reproducible repository artifacts.

The focused [palm audit](sim-palm-audit-2026-09-27.md) and subsequent
[palm refinement](sim-palm-polish-2026-09-27.md) cover entry 56. Its current
model uses a natural palm interpretation with curved, feathered fronds and
a slim tapered stem while preserving the game's faceted style.

The subsequent [full-set recognition and style review](sim-model-recognition-audit-2026-09-27.md)
records which existing models should be retained, refined or reworked, including
Low-detail identity and construction-stage coverage. The resulting
[recognition refinements](sim-model-polish-2026-09-27.md) are now reflected in
this gallery, including the ancient tree's landmark budget. The subsequent
[shelter and alternate-house refinement](sim-shelter-refinement-2026-09-27.md)
adds grounded straw/canvas shelters, their distinct construction stages,
alternate crowns, awnings, dormers and roof-access details, cleaner windows and
consistent steep-roof snow. Seven supplemental panels cover the second ordinary
house stage, both bridge axes and both stages of straw/canvas construction,
without changing the native comparison count or existing numbers.
The [tree-lighting and ground-shadow correction](sim-tree-shadow-fix-2026-09-27.md)
brightens evergreen surfaces, replaces their tile-sized shadow proxies with
canopy outlines, and removes cast/contact shadows from all six rock layouts.
The latest [model review follow-up](sim-model-review-followup-2026-09-27.md)
corrects Bloodpool's protruding house extension, alternate straw crowns,
standing canvas tents, Aitos's chimney, stilt stairs, striped windmill sails,
factory roofs and chimneys, evergreen silhouettes, and the ancient tree's
central crown. It includes native/before/after comparison images.
The [alternate straw-hut correction](sim-straw-crown-2026-09-27.md) replaces
the overly flat cap with the native broad raised crest, including its outer
tips and five dark straw bands. Two more supplemental panels show this hut's
crest frame and partial covering, bringing the supplemental count to nine.
The [shared straw-hut refinement](sim-straw-huts-2026-09-27.md) gives both
facings upright reed walls and rounded, overhanging thatch roofs. The front
has a bundled straw peak; the alternate retains its broad crest.

## Regional coverage

| Model numbers | Original variant | Current 3D coverage |
|---|---|---|
| 46, Kasandora pyramid | Japan has an eye in both character banks; US, Europe, Germany and France have plain casing | Plain and eye variants, selected from resolved town artwork and donor availability at every detail level |
| 29–30, developed Marahna house | Japan selects stilt family `$0E`; the four Western releases select timber family `$02` | Finished native metatile selects the stilt or log-cabin model; both are rendered in the index (no new house-art setting) |
| 47, Marahna temple | Separate `$EF` landmark, structure tiles `$E4/$E5/$EC/$ED`; distinct from the ordinary `$C2` cathedral | Central prang, rounded side mounds, entrance pillars, gateposts and connected enclosure walls; ordinary `$C2` cathedral stays separate |
| 68–73, town rocks | Boulder `$61` and scattered-stone `$62/$63/$69/$6A/$6B` match across supplied releases and banks | Native placement/count preserved in both town and world models; cleared tiles release their models |
| All remaining entries | The decoded references match between releases within each art bank | Town and early/late differences remain visible in the index |

These statements cover the 73 indexed models, not all regional game artwork.
Follower symbols, lairs and other sprites have separate inventories.

The native house selector table is at file offset `0x01D7CB` in Japan and
`0x01DCC6` in the other four supplied ROMs. The generator reads each table
instead of applying the US family map to every release. Japanese native code
at `$03:9E5C` loads `$03:D7CB,X`; the developed Marahna entry is the only
difference in these 18-entry tables.

## Reading the comparisons

Select a release and an art bank, then search by model name. “Early/late” means
the town's act-completion character bank; it is independent of the early,
middle and developed house tiers. All 730 source combinations are decoded for
inspection, including combinations whose in-game reachability was not verified.

The original panels preserve ROM pixels. They are enlarged independently to
make small details readable; panel size is not evidence of relative world size.
Production panels share the model-sheet world scale and use Ultra detail,
Varied style and the production depth renderer. Vegetation references are
representative metatiles, not exhaustive forest adjacency configurations.

Release filtering shows the native donor artwork. It does not emulate every
combination of the game's independent regional settings. The Japanese pyramid
eye, for example, can be selected without selecting Japanese gameplay.

The new index supplements the archived single-source
[comparison index](../development/documentation-archive/docs/research/sim-voxel-model-audit/original-vs-voxel-audit-index.tsv).
That historical index is preserved rather than relabeled as multi-release data.

## Regenerate

Use a Python environment with Pillow, plus the existing SDL3/ImageMagick model
sheet prerequisites. Supply headerless `ar.sfc` and, optionally, `ar-jp.sfc`,
`ar-eu.sfc`, `ar-ger.sfc`, and `ar-fra.sfc` in the repository root. These are
reviewed retail layouts; arbitrary ROM hacks are not supported. US is required
for shared composition identities. The generator verifies that those composition
bytes also exist in each supplied donor and records the input ROM hashes.

```sh
bash tools/make-sim-voxel-model-sheet.sh runs/sim-straw-huts-2026-09-27/model-sheet.png
python3 tools/make_sim_voxel_regional_index.py \
  --renders "${TMPDIR:-/tmp}/actraiser-sim-voxel-model-sheet/renders" \
  --out runs/sim-straw-huts-2026-09-27/art-index
```

Use `--rom-dir` for a different ROM directory. Open `index.html` locally; it
requires no external services. Re-render the production sheet after geometry
changes before regenerating the gallery.
