# Action-background audit: 32 versus 64 extra rows

Audited 2026-09-27, after `4407b525` (`Keep extended action rows on efficient
rendering paths`).

## Recommendation

Use **64 as the action diorama's requested vertical budget**, retaining the
existing world-boundary clipping, independent BG1/BG2 clipping, and room-specific
limits. The audit found no new background defect in the 64-row captures that
would require another room-specific cap. This is a maximum per side, not a
promise that every background can supply 64 rows above and below the viewport.

The source's factory default is actually **0**, in the `diorama_vertical_extend`
descriptor in `src/app/settings.c`; 32 is the comparison baseline, not the
current factory default. The local `settings.ini` already saves 64. Neither
setting was changed by this audit. If the recommendation is adopted, change the
descriptor default and its outdated streaming-seam comment, preserve explicit
saved preferences, and cover fresh/reset settings in the settings test.

Of 49 action rooms, **36 have a primary background taller than 256 pixels** and
can reveal more terrain at 64. The other **13 have only 31 extra rows available
in total**, because the native camera uses a 225-pixel viewport bound. Their
32/64 composite captures are byte-identical. Increasing the requested budget
does not stretch these arenas or repeat their art to fill the requested space.

| Region | Rooms gaining capacity | Unchanged short rooms | Main consideration |
| --- | ---: | ---: | --- |
| Fillmore | 3 | 1 | Forest and caves benefit; the castle's independent BG2 must retain its own clipping. |
| Bloodpool | 6 | 2 | Keep the water/sky raster backdrop and its finite extents; taller castle geometry benefits. |
| Kasandora | 5 | 1 | Preserve cloud/dune bands and native interior backdrops. |
| Aitos | 5 | 2 | Keep room 02's **BG2 top/bottom cap of 24** and page-cycle handling. |
| Marahna | 7 | 1 | Temple geometry benefits; horizontal jungle cycles remain independently bounded vertically. |
| Northwall | 8 | 0 | All primary maps are tall; short mountain backdrops and tower BG2 still have finite limits. |
| Death Heim | 2 | 6 | Only rooms 06/07 gain capacity; hub, other rematches, and final boss remain unchanged. |

## Evidence

All locally generated evidence is under the ignored
`runs/all-action-vertical-audit/` directory. ROM-derived images and data were not
added to the repository.

- The official `tools/action_editor/action_bg_export.c` exporter loaded **49/49
  rooms without failure**, including cumulative inherited assets. The asset
  inspection helper decoded both background maps for every room: **98 maps**.
- Fourteen GPU-presented capture runs visited each region's rooms in order at
  both 32 and 64, yielding **98 composite images and 98 WRAM/VRAM/PPU snapshots**.
  The expected room identity was checked in every snapshot. All runs completed
  without fatal-session, missing-width-variant, or capture-schedule failures.
- Visually reviewed all seven region comparison sheets. No new tile garbage,
  wrapped strip, or background seam attributable to 64 appeared at these sampled
  views. Extra space is sometimes more sky, solid wall, or authored black space;
  it is not additional useful scenery in every room.
- The live provider compared **47,319,134 authentic-view tile samples with zero
  mismatches** over 39,102 provider frames. Northwall 06/07 has an existing
  finite-edge rejection, detailed below; these are not all successful bindings.
  The immutable room-source diagnostic reported zero fallbacks to the live WRAM
  decoder.
- All 49 paired snapshots have identical BG camera coordinates, map hashes, and
  metatile-definition hashes. The 13 short-room image pairs are byte-identical.
  Full WRAM is identical in 38 pairs; the other pairs differ, so this audit does
  **not** claim simulation equivalence between different visibility budgets.
- Existing `actraiser_action_bg_plan`, `actraiser_action_bg`, and
  `actraiser_diorama_rom_backdrop` tests pass. Running
  `build-tests-release/actraiser_diorama_rom_backdrop_test ar.sfc` also passes its
  complete stock-ROM catalogue checks: all 49 scene profiles/native frames and
  all 98 background decodes, including the two page-cycle rooms and 17 rooms
  using raster effects.

`summary.json` contains room dimensions, paired camera/margin state, snapshot
comparisons, and provider summaries. `census.json` contains the full snapshot
census. `gallery.html` and the seven `*-comparison.jpg` sheets show the images.
`asset-audit.json`, `rooms.json`, and `assets/` contain the immutable-source
inventory. `build_sequence.py`, `capture_sequence.py`, `run_sequences.py`, and
`summarize.py` record the local harness and analysis.

## Limits that should remain

`ActRaiser_ResolveVerticalMarginPolicy` selects capture height from the primary
layer, then clips each BG independently. The native camera bound uses 225;
available rows are `min(budget, camera_y)` above and
`min(budget, max(0, world_height - 225 - camera_y))` below. Keep this behavior.
For example, a stationary 256-pixel-high BG2 at Y=0 supplies no upper extension
and at most 31 lower rows, even when BG1 can supply 64 on both sides.

The Aitos 04/02 waterfall uses a native 32x32 PPU map and a page cycle despite
its 512-pixel decoder dimensions. Its independently tuned BG2 cap is **24 above
and 24 below** in `kTunedLayerPolicies`. Increasing the global budget must not
raise that cap or reclassify the layer as an ordinary world map. The primary
terrain can still use the larger budget. Room 04/03 also cycles pages but has a
256-pixel primary map, so its captured framing is unchanged.

Kasandora 03/01–02 retain their cloud/dune band policy. Bloodpool, Northwall, and
Death Heim retain their viewport/native raster backgrounds rather than being
forced into the finite world provider just to support a larger budget.

Northwall **06/07 BG2** reaches camera Y=318 with a 512-pixel map. Its authentic
view already crosses the finite map boundary. Both settings report exactly
51,200 outside-world tile samples over 400 frames; the guard keeps the native
source instead of binding a finite provider across that boundary. This is
present at 32 as well as 64. At the saved checkpoint, all 256 tiles sampled by
the 64-row upper band match the live decoded map and VRAM ring; the 32-row check
also matches (128 tiles). The existing fallback is not evidence that the added
rows selected a slower renderer, and was not bypassed for this audit.

## Cost

With both sides fully available, capture height increases from **288 to 352**
rows: **22.2% more source pixels at the same width**. This is a workload estimate,
not a claim of a 22.2% frame-rate reduction. At the 49 sampled views, summed
capture heights increase from 13,273 to 15,014 rows (**13.1%**); that is an
unweighted room sample, not a gameplay-time average.

The committed [rendering audit](extended-view-rendering-audit.md) establishes
that extra rows use the ordinary packed capture and tile-span paths. Height
alone does not enable a scalar/reference renderer, add HDMA ticks, or issue a
draw call per row. The corrected isolated PPU benchmark's candidate medians
increase from 2.598 to 3.183 ms for full-world bindings, 2.553 to 3.168 ms for
margin-only bindings, and 2.658 to 3.220 ms for mosaic: about **21–24%** from 32
to 64, consistent with the larger raster area. These are synthetic CPU timings,
not whole-game or GPU timings.

Keep 32 available as a lower-cost preference. The earlier audit measured the
optimized game's Aitos 64-row render CPU at about 3.406 ms on this machine, but
did not establish GPU headroom across supported hardware. The present capture
runs were concurrent and instrumented and are deliberately not used for
performance comparisons.

## Positioning of short rooms

There is **no reserved top offset for rows that do not exist**. Presentation
receives `height = 224 + actual_top + actual_bottom` and
`authentic_y0 = actual_top` in `src/diorama/present_diorama.c`. A short room with
31 available rows therefore has a 255-row capture at either requested budget,
not a 352-row capture with empty padding.

`PrepareDioramaView` in `src/diorama/diorama.c` uses that actual capture height
for UVs and geometry. Dynamic camera mode centers the projected bounds of the
actual BG1 scene. If that scene exceeds the viewport, the centering routine
constrains the shift to keep the original 224-row gameplay band visible, or
centers that band when even it cannot fit. Free camera mode uses a bounded
tilt-dependent shift from `(actual_top - actual_bottom) / (2 * 224)`; it does
not use the requested budget. These camera modes need not center every distant
background independently, but neither compensates for nonexistent rows.

The 13 byte-identical short-room comparison pairs verify that raising 32 to 64
did not alter their sampled screen positions. Existing
`actraiser_diorama_projection` tests cover centering and protection of the native
band with asymmetric margins.

## Coverage boundaries

This is a source/policy audit plus sampled background inspection of every room,
not a complete playthrough at every camera position or animation phase. It used
the local US ROM and private save/settings/replay copies, square pixels, a 16:10
extended aspect, and diorama presentation with existing layer definitions.
It does not separately certify every regional ROM, aspect ratio, custom camera,
boss transition, or object-pool behavior.

Directly warping from startup into an intermediate room is insufficient: the
native asset VM can depend on assets inherited from preceding rooms. The first
Aitos 04/02 pilot correctly failed its asset-boundary guard at **both** settings.
Those runs were discarded. The counted runs enter rooms cumulatively using an
audit-only automation object linked into a private binary; production sources
were not modified. This establishes valid background inheritance, but those
debug warps are not a substitute for natural route/transition tests. The white
player silhouette in some images is the private invulnerability setting.

## Room inventory

Dimensions below are complete decoded BG1/BG2 maps. The last column is the
**actual primary top/bottom capture margin at the sampled view**, not each BG's
independent allowance and not a full-route guarantee. Room numbers are internal
group/map IDs; several rooms can belong to one act.
| Region / room | BG1 dimensions | BG2 dimensions | Top/bottom: 32 → 64 | Note |
| --- | --- | --- | --- | --- |
| Fillmore 01/01 | 4096×768 | 2304×512 | 32/32 → 64/64 | More primary terrain available |
| Fillmore 01/02 | 2048×1280 | 2048×1280 | 32/32 → 64/64 | More primary terrain available |
| Fillmore 01/03 | 1024×1792 | 256×512 | 32/32 → 64/55 | BG2 camera/clipping independent |
| Fillmore 01/04 | 512×256 | 256×256 | 31/0 → 31/0 | Short primary; unchanged |
| Bloodpool 02/01 | 4096×512 | 256×256 | 32/0 → 64/0 | More primary terrain available |
| Bloodpool 02/02 | 768×512 | 256×256 | 32/0 → 64/0 | More primary terrain available |
| Bloodpool 02/03 | 1024×1024 | 256×256 | 32/32 → 64/64 | More primary terrain available |
| Bloodpool 02/04 | 512×512 | 256×256 | 0/32 → 0/64 | More primary terrain available |
| Bloodpool 02/05 | 1792×1024 | 1792×1024 | 32/32 → 64/64 | More primary terrain available |
| Bloodpool 02/06 | 768×256 | 256×256 | 31/0 → 31/0 | Short primary; unchanged |
| Bloodpool 02/07 | 1024×1024 | 256×256 | 32/0 → 64/0 | More primary terrain available |
| Bloodpool 02/08 | 256×256 | 256×256 | 31/0 → 31/0 | Short primary; unchanged |
| Kasandora 03/01 | 4096×768 | 512×512 | 32/23 → 64/23 | Cloud/dune bands |
| Kasandora 03/02 | 3072×768 | 512×512 | 32/32 → 64/55 | Cloud/dune bands |
| Kasandora 03/03 | 2048×512 | 1024×512 | 32/32 → 64/64 | More primary terrain available |
| Kasandora 03/04 | 1536×768 | 1024×512 | 0/32 → 0/64 | More primary terrain available |
| Kasandora 03/05 | 1024×1536 | 1024×512 | 32/0 → 64/0 | More primary terrain available |
| Kasandora 03/06 | 512×256 | 1024×512 | 31/0 → 31/0 | Short primary; unchanged |
| Aitos 04/01 | 4096×1024 | 256×256 | 32/32 → 64/64 | More primary terrain available |
| Aitos 04/02 | 1792×768 | 512×512 | 24/32 → 24/64 | Native BG2 capped 24/24; page cycle |
| Aitos 04/03 | 512×256 | 512×512 | 0/31 → 0/31 | Page cycle; short primary |
| Aitos 04/04 | 1280×1024 | 1024×1024 | 32/32 → 64/64 | More primary terrain available |
| Aitos 04/05 | 512×512 | 256×256 | 24/32 → 24/64 | More primary terrain available |
| Aitos 04/06 | 1536×1024 | 1024×1024 | 32/32 → 64/64 | More primary terrain available |
| Aitos 04/07 | 512×256 | 512×256 | 31/0 → 31/0 | Short primary; unchanged |
| Marahna 05/01 | 2048×512 | 512×512 | 32/0 → 64/0 | BG2 horizontal cycle |
| Marahna 05/02 | 1536×512 | 512×512 | 32/7 → 64/7 | BG2 horizontal cycle |
| Marahna 05/03 | 512×256 | 512×256 | 8/23 → 8/23 | Short primary; unchanged |
| Marahna 05/04 | 1024×1024 | 1024×512 | 32/32 → 64/64 | More primary terrain available |
| Marahna 05/05 | 768×512 | 512×512 | 24/32 → 24/64 | More primary terrain available |
| Marahna 05/06 | 2304×1792 | 1280×1024 | 32/32 → 64/64 | More primary terrain available |
| Marahna 05/07 | 2304×1792 | 1280×1024 | 32/32 → 64/64 | More primary terrain available |
| Marahna 05/08 | 512×512 | 512×256 | 32/0 → 64/0 | More primary terrain available |
| Northwall 06/01 | 2560×1024 | 256×256 | 32/32 → 64/64 | More primary terrain available |
| Northwall 06/02 | 1792×768 | 1792×768 | 32/32 → 56/64 | More primary terrain available |
| Northwall 06/03 | 1536×1536 | 1792×768 | 32/32 → 64/64 | More primary terrain available |
| Northwall 06/04 | 512×512 | 512×512 | 32/32 → 64/64 | More primary terrain available |
| Northwall 06/05 | 768×768 | 256×256 | 32/32 → 64/64 | More primary terrain available |
| Northwall 06/06 | 1024×2048 | 512×768 | 32/32 → 64/64 | More primary terrain available |
| Northwall 06/07 | 1024×1536 | 512×512 | 32/32 → 64/39 | BG2 finite-edge native fallback |
| Northwall 06/08 | 512×512 | 256×256 | 32/0 → 64/0 | More primary terrain available |
| Death Heim 07/01 | 512×256 | 512×256 | 31/0 → 31/0 | Native hub scene |
| Death Heim 07/02 | 512×256 | 256×256 | 31/0 → 31/0 | Short primary; unchanged |
| Death Heim 07/03 | 256×256 | 256×256 | 31/0 → 31/0 | Short primary; unchanged |
| Death Heim 07/04 | 512×256 | 256×256 | 31/0 → 31/0 | Short primary; unchanged |
| Death Heim 07/05 | 512×256 | 256×256 | 31/0 → 31/0 | Short primary; unchanged |
| Death Heim 07/06 | 512×512 | 256×256 | 32/0 → 64/0 | More primary terrain available |
| Death Heim 07/07 | 512×512 | 256×256 | 32/0 → 64/0 | More primary terrain available |
| Death Heim 07/08 | 256×256 | 256×256 | 31/0 → 31/0 | Native final-boss scene |
