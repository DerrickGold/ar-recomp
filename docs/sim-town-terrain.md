# SIM town terrain elevation

The SIM 3D terrain path is enabled in release builds. It uses one audited,
deterministic corner-height field for all six stock towns and one sampling
contract for every grounded presentation consumer. The compile-time flag
remains available only to produce a strict flat-terrain regression control.

## A/B builds

Player/release build (terrain enabled):

```sh
cmake --preset play
cmake --build --preset play
```

Flat-terrain control:

```sh
cmake --preset control
cmake --build --preset control
```

The terrain activates only with the cleaned background-voxel town canvas.
If that renderer is unavailable, the ordinary flat path remains the fallback.

In a release build, **Town 3D > Scene > Landscape height (%)** is a live
player-facing control. Its `0..150` range scales the audited terrain,
building-foundation grounding, bridge bank grounding, terrain depth occlusion,
terrain shading, and the stable flight datum as one system. It does **not**
resize authored mountain/volcano relief or voxel buildings: those are objects
standing on the scaled terrain and keep their own authored height strategies.
The separate Object height setting still controls a flying actor's clearance.
`100` preserves the audited profile, `50` gives a restrained half-height
landscape beneath full mountains, and `0` gives flat ground without flattening
the mountains themselves.

## Sampling contract

`SimTownTerrain_SampleCell` is the semantic API. Callers choose a cell and a
clamped local `u/v`, which preserves the selected side of a hard cliff instead
of averaging across it. The result contains height, analytic bilinear slopes,
a unit normal, and hard-edge flags. `SimTownTerrain_SamplePoint` is the
convenience API for ordinary map positions and selects their owning cell.

Hard edges are baked from the ROM face classifier; they are not guessed from
the size of a height mismatch. Every shared vertex outside that mask is welded
to one exact Q8 value. A hard skirt borrows the authored cliff-face cell's
material even when ordinary ground happens to be the higher owner. This keeps
smooth Marahna grades from becoming walls and prevents a grass wedge from
cutting across its grey plateau faces. The visible and depth-only passes
consume the same mask. When height ownership reverses between a hard edge's
two endpoints, each owner is clipped to its side of the exact equal-height
point. This avoids the self-crossing four-corner skirt whose two triangles
escaped across Aitos as long grass wedges. Bloodpool's `$72` cave mouth is a
face-topology subtype:
its centre texel is the black aperture, so the small north/south closure skirts
sample a neutral stone jamb instead. The entrance itself remains dark while
its aperture can no longer smear into horizontal black seams.

Grounded consumers use these rules:

- Foliage and rigid buildings sample the natural terrain height at the centre
  of their footprint and remain vertical. A building does not flatten its
  dynamic plot or rise to its highest corner. Instead, a footprint-aware stone
  foundation sits fractionally inside its base: uphill terrain buries it and
  only the downhill gap is filled. Large U-shaped landmarks retain separate
  foundation pieces rather than paving over their courtyards.
- Contact decals sample their own positions so they conform to a slope rather
  than hovering on a flat plane. The accumulated shadow mask is sampled only
  by depth-visible terrain-top geometry; cliff skirts and hidden ground do not
  receive it.
- Actors and map-plane effects sample their authentic foot/corner positions.
- Mountain faces sample every vertex, including skirts, so their bases meet
  the same terrain mesh.
- A live `$E1`/`$E2` bridge uses the higher of its two path-centre bank anchors
  as one horizontal visual datum, so its paving meets the authored path instead
  of perching at a lateral terrain corner. Its depth-only safety envelope uses
  the maximum under the complete rigid footprint. This prevents a sloped bank
  from erasing the paving without moving the bridge visibly upward. The deck,
  arches and parapets never twist across the gap or average through
  water/cliff walls.

## Dynamic stone bridges

The classifier groups adjacent bridge markers along their native axis and
walks through the water family to the first solid bank on both sides. One
continuous, horizontal model then covers the water opening with a one-pixel
masonry key into each bank. It does not extend to bank centres: doing so makes
perpendicular Bloodpool crossings collide inside their shared land cell. Its
visible datum follows the path-centre approaches; the maximum over that exact
footprint is used only for depth safety, so a steep bank cannot clip the rear
deck or force the whole bridge visibly above the path. Its
material contract follows the approved native graphic: cool gray-green
masonry and paving, pale stone edge courses, clearly raised stone parapets with
compact bank-end posts, and a shallow dark underside. It has no wood material
or speculative free-standing piers. The paving cap overlaps the slab, and the
parapet bodies/posts are mortised below that cap; no coplanar hairline can
separate a railing from the walking surface at an oblique camera pitch. End
posts do not place transverse stone bars across the path.

The entire live `$E1`/`$E2` metatile is removed from the captured ground and
replaced by its original river metatile (`$3A` under an east-west bridge, `$41`
under a north-south bridge), rendered through the current town's native
CHR/palette. The pale native rail extends outside the nominal deck band;
retaining those rows or borrowing water from the bridge cell itself leaves a
detached 2D rail floating above the modeled bridge. Restoring the real river
tile also avoids the grass or snow rectangle produced by general-biome
inpainting.

## Dynamic town stages

Terrain topology is static per town, while the rendered town artwork remains
live. This split is deliberate: the ROM-versus-live audit found 173–335 changed
cells per town, all gameplay/structure stamps, with no cliff or seam changes.
`SimTownCanvas` watches the live 64×64 BG1 tilemap, BG1 character data, and
palette, and increments its serial whenever any of them changes the rendered
town. The earthquake therefore publishes its new artwork regardless of which
native source carries the transition; the background voxel ground refreshes
from that same image serial. Its scene classifier follows the independent
tile-layout revision plus semantic cell/record inputs, so a water animation or
fade no longer rebuilds structures, bridges, foliage, or mountains. The baked
terrain API takes only town and position, so the event cannot accidentally
regenerate a different mesh.

For Marahna, every cell—including one currently covered by water—already has a
height. The initial still-water area is level at its audited water datum. When
the earthquake drains it and the game stamps the connecting land, those new
land pixels appear on the same pre-existing vertices. We therefore do not
apply the height map only after reveal, and we do not flatten the whole region
at the event boundary. The revealed basin/causeway is flat where the water-body
rule made it flat, then joins surrounding relief through the same welded edges.
This avoids a terrain pop while preserving the native before/after map change.

There is intentionally no separate "earthquake terrain visible" flag. The
pre-event water and post-event land are two live images of one immutable
surface. `sim_town_canvas_test` exercises a complete 16×16 Marahna-style
water-to-land redraw, including its exact dirty rectangle and serial advance;
`sim_background_voxels_test` proves that the cleaned enhanced ground retains
the old water image until that serial advances and then publishes the revealed
land. The offline terrain audit separately requires every Marahna still-water
body to remain level, so the hidden state cannot expose relief through the
water before the native event reveals its artwork.

## Depth and flight contracts

The visible textured terrain remains in the ground color pass. Its identical
tops and exposed cliff skirts are also submitted first to the shared D32 pass
with color writes disabled. That depth-only surface rejects mountains and
voxel buildings hidden behind nearer relief without repainting the ground or
covering the separately composed actor bands. The blurred screen-space shadow
mask is then sampled on those same top faces with depth testing enabled and
depth writes disabled. Shadows therefore stop at visible ridge/cliff lips and
remain behind solid buildings and bridge geometry.

Height classes decide altitude behavior. Grounded actors sample the terrain at
their feet. Flying actors and flying projectiles use the scaled town maximum as
one stable world-space datum, plus their independently scaled authored virtual
height. Their shadows still land on the local terrain and use the actual
clearance to the surface, so crossing a hill changes only the shadow
relationship—not the actor's flight plane. Ground strikes and fires sample
their local contact point; an effect attached to a flyer uses the flight datum.
The Aitos eruption uses the model renderer's live crater-mouth position and
authored mountain height above the local terrain, then follows local terrain
along its ballistic path so it both launches from a raised/cliff-backed volcano
and converges on its real landing cell. The cloud shroud is offset from the same
town maximum instead of the former flat map plane.

## Flat-datum audit

The elevation candidate has no production town-space consumer left silently
assuming that `z=0` means the visible town surface:

- terrain color, terrain depth, and terrain shadow receiving share the same
  corner field and hard-edge mask;
- rigid models, buried foundations, contact decals, and directional shadows
  sample their footprint or authored anchor contract;
- bridges use their horizontal bank datum and water-opening footprint;
- grounded actors sample their drawn feet, including the post-shear row when
  they stand on mountain relief; flying actors and attached effects use the
  stable town maximum;
- map-plane quads sample all four corners, while local ground effects sample
  their contact point;
- mountain and volcano vertices add terrain only as a base translation; their
  authored relief scale is independent of Landscape height;
- the Aitos crater anchor is published by the rendered model as height above
  local terrain, so a model retune or cliff placement cannot leave eruption
  arcs launching from the old flat-map coordinate; and
- clouds add their authored altitude to the town maximum.

The world underlay outside the finite 512×512 town, UI/screen-space effects,
and diagnostic-only cull markers are intentionally exempt. They are not
grounded objects inside the audited town height field.

## Lighting

Terrain cliffs use softly shaded skirts and short-range contact darkening.
Voxel material lighting has a higher ambient floor, corner AO is bounded, and
new profiles default to 45% shadow opacity with 50% softness. User settings
still control both values.

## Runtime performance contract

The audited maps and their cliff topology are immutable, so the render path
decodes them once per town rather than rediscovering them in each color,
shadow, and depth pass. Terrain corner lighting is cached by town and Landscape
height: a steady frame no longer performs 4,096 normal square roots or the
roughly 65,000 neighbouring height lookups used by contact shading. The
terrain's 1,024-cell painter order is cached across ordinary map panning and is
rebuilt only when the projection, dimensions, town, or height magnitude
changes. Its sort key is homogeneous clip depth, which is monotonic with GPU
normalized depth for the shared perspective matrix and avoids a divide per
cell; `scene3d_math_test` locks that relationship across the settable camera
range.

Point-height queries take a dedicated bilinear path. They do not construct
slopes/normals or call `sqrtf`, and their shared coordinate resolver avoids the
former duplicate validation and `floorf` work. Visible voxel entries similarly
resolve their terrain anchor and bridge depth envelope once per depth draw, and
the non-interleaved renderer does not calculate the unused 33x33 actor-band
depth range. Depth-pass buffers remain persistent and grow geometrically, so a
stable scene performs no per-frame geometry allocation. Quads stage their four
unique vertices and reuse a persistent 32-bit index pattern, avoiding the old
six-vertex expansion and reducing geometry conversion and upload volume by one
third.

Depth geometry asks one clip-space transform for both screen position and D32
depth instead of multiplying each vertex by the camera matrix twice. Clip-space
conversion stores per-target reciprocals rather than dividing every submitted
vertex. Material-aware color ramps are indexed directly from their named
brightness levels, and structure-grounding queries use a 32x32 height table
built with the voxel scene instead of scanning every scene object for each
actor sample.

Animated canvas revisions reuse the fixed 512x512 mountain texture and upload
transfer buffer; SDL GPU resource cycling provides a writable backing without
creating and retiring both objects on every revision. The seven-tap separable
shadow kernel uses one generated shader draw per axis on Metal, Vulkan and
D3D12, while retaining the fourteen-draw custom-blend implementation as the
backend-independent fallback. Smooth 2x automatically stays native when the
fourfold target would exceed 10 Mi pixels, avoiding the unbounded D32+RGBA
memory and fill-rate jump at 1440p and 4K.

Projected terrain depth quads are cached by camera, viewport, landscape scale,
and town. The early terrain-shadow pass and later object-depth composite reuse
the same projection instead of transforming every terrain corner twice, and a
still camera retains that projection across frames. Shadow masks are capped at
roughly 1440p working resolution: linear upsampling and a proportionally scaled
blur preserve their apparent size, while the two-target 4K footprint falls
from about 63 MiB to about 16 MiB.

## Maintenance contract

Runtime policy is named and shared rather than repeated at call sites. The
Landscape height minimum/default/maximum/step constants drive both the setting
descriptor and frame-value clamp. The visible and D32 terrain renderers call
one cliff-clipping policy wrapper, and face topology uses a typed
ordinary/cliff/cave enum instead of anonymous integer values. Town maxima are
derived by the terrain generator and committed beside the corner payload, so
queries are immutable constant-time reads rather than lazy mutable cache
initialization.

The render path is four translation units rather than one.
`sim_background_voxel_project` owns the camera transform every background
surface shares; `sim_background_mountain_render` owns projected relief,
silhouette skirts and the volcano's crater anchor;
`sim_background_voxel_terrain_depth` owns the D32 terrain surface and its
projection caches; and `sim_background_voxel_renderer` retains texture
ownership, object anchoring, foundations, model drawing and the shadow mask.
The two audited-height converters stay `static inline` in the projection
header because they are one-expression conversions called per vertex and per
terrain corner; the split should not put a call in that path. `D7-voxel-town`
renders the town through the real GPU depth path and is the checkpoint that
covers this code.

The stock SIM geography contract (six towns, 32×32 cells, 16 source pixels per
cell) is owned by `sim_world_map.h`. World-map composition, terrain sampling,
and voxel classification expose subsystem aliases derived from that contract
instead of repeating the numeric cardinalities.

Bridge source metatiles, travel axes, replacement-mask kinds, native river
tiles, bank embed, and cross-axis dimensions are named domain data. The stone
model's remaining measurements and brightnesses are deliberately localized
art-direction values; they do not control classification, scaling, terrain
height, or depth policy. Invalid bridge metadata now produces empty bounds and
no model instead of silently becoming a north-south bridge.

The generator's terrain-rule path has no Pillow dependency. Image libraries
are imported only when producing the optional audit render, keeping shipping
data regeneration deterministic and lightweight. GPU depth staging validates
row and total byte sizes before integer conversion, and its CPU/GPU growth
capacities are named independently.

## Audit status

All within-town structure, hydrology, flow, spike, and profile validators are
clean. The restrained inferred relief ranges from 0.72 units in towns without
contour art to 6.47 units in Marahna. Three cross-town world-space boundaries
remain intentionally open: Bloodpool–Fillmore, Northwall–Aitos, and part of
Aitos–Bloodpool. They require an underlay/world-base transition; flattening a
town's internally consistent relief to hide them would damage the town view.

The research source, editor, validators, deterministic renders, and generator
remain under `archive/handover/`. Regenerate the committed Q8 table with
`archive/handover/tools/gen_c_terrain.py` after changing the rules.
