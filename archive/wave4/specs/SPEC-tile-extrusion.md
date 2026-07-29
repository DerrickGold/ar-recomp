# Spec — WN2: extruded 3D geometry for background tiles (sim + overworld)

**Status:** authored, not implemented, not audited. Sequenced after the Wave-4
fixes; **independent of WN1** (see §2).

> **SCOPE — read this before §9b.** The focus of this spec is **object height**:
> buildings, trees, rocks standing up off a flat ground plane (§4–§6, Stages
> 0–E). That is the deliverable.
>
> **Terrain elevation (§9b, Stage F) is a SEPARATE PHASE and is not part of the
> initial work.** It is documented here only because cliff tiles will look
> increasingly wrong as everything around them becomes 3D — so the eventual
> answer wants recording while the reasoning is fresh, not because it belongs in
> the first implementation. §9b grew long during design discussion; its length
> reflects how much got settled, not its priority.
>
> If you are implementing from this spec: do Stages 0–C and stop. Ignore §9b.

## 1. Intent

In sim-town 3D and (once WN1 lands) the overworld, promote selected background
tiles — buildings, trees, rocks — from flat ground texels to **extruded prisms**:
the tile art on top, a side treatment on the vertical faces, standing on the
existing ground plane. The result reads as 2.5D dioramic depth without authored
3D assets.

**Chosen scope: extruded prisms, generated from the tilemap.** Not authored
meshes. A model format, loader, and per-tile asset pipeline is a content project;
prisms need none of that and cover "buildings, trees, rocks" convincingly.

**A note on the reference.** A similar-looking project was suggested
(`bryanthaboi/pokemon-gen1-recomp-project`). Its README and `docs/architecture.md`
both describe a purely 2D renderer — 160×144 canvas, integer nearest scaling,
SpriteBatch 8×8 quads, a 2D follow camera. It exposes a `TILT` hotkey whose
mechanism is documented nowhere I could find. **So it is not a source for this
design**, and nothing here is derived from it. If its tilt does turn out to be
relevant, the place to look is its `src/` tree, not its docs.

## 2. It does NOT collide with existing systems (verified)

I initially claimed a collision with WN1 and was wrong. The data flow says
otherwise:

- `sim_town_canvas.c` reads the **town** BG1 tilemap from WRAM `$7F:0000`
  (64×64 tiles, quadrant-paged) and renders a 512² pixel buffer.
- `sim_world_map.c` reads a **separate** 128×128 tilemap and renders a 1024²
  buffer.

Two independent tilemaps, two independent textures, two independent consumers.
Extrusion is a **new draw pass that reads a tilemap and emits prisms** — it
modifies neither existing path. WN1 changes *which* map is the primary layer;
WN2 adds geometry on top of whichever is active. They compose.

It is also a natural third draw class alongside what already exists: flat tilted
layers, and projected billboards (`DrawSimObjectPriority`) which already draw
tile-sourced art as camera-facing quads with depth sorting and virtual height.

## 3. The actual first task: publish tile identity

Both modules currently **discard the tile IDs** — they consume the tilemap and
expose only pixels (`SimTownCanvas_Pixels()`, `SimWorldMap_Bake()`). There is no
`SimTownCanvas_TileAt(x, y)`.

So step one is not geometry. It is a pure accessor over data already read every
frame, which makes it **fully unit-testable ROM-free**: feed a synthetic
tilemap, assert the classifier picks the expected cells — the same fixture shape
`tests/sim_world_map_test.c` already uses.

## 4. Classification: ROM-derived base + manifest override

**Decided (author):** a hybrid, explicitly so the same seam serves a future
N\*X/HD graphics pack.

```
tile class = manifest override for this tile index, if present
           : else ROM-derived class from the tile-index range table
           : else kTileClass_Flat   (no extrusion — the default for everything)
```

- **ROM-derived base.** Which tile-index ranges are buildings / trees / rocks,
  derived from the ROM and recorded in `docs/rendering-engine.md` with its
  evidence, the way the world-map blobs (§13c) and the town bounds were. Works
  for every town and the overworld with zero per-map authoring.
- **Manifest override.** A data file mapping tile index (or range) → class,
  loaded the way `game-assets/manifest.ini` already is for HD replacements.
  Lets a graphics pack declare "this tile is now a building with this shape"
  without touching code or re-deriving anything.

**Why this seam matters beyond WN2:** `hd_replacements.h` already reserves
`kHdPlane_Tiles` — "hash-keyed HD tile pack (parsed + reserved; needs the N-x
RGBA-sideband renderer path)". A tile-class table keyed by tile index is the
same key space that plane will use, so the two features share one manifest
concept instead of inventing a second.

**Default must be Flat.** An unclassified tile extrudes nothing, so a missing or
partial table degrades to exactly today's rendering. This is also what keeps the
feature provably inert when disabled.

## 5. Height: per-class constant, settings-tunable

**Decided (author).** Each class carries one height, exposed as a settings row so
it can be tuned live — matching how the diorama layer Z values and the existing
sim3d tuning knobs already work.

```
sim3d_extrude_building_height   (default TBD by eye)
sim3d_extrude_tree_height
sim3d_extrude_rock_height
```

Rejected: deriving height from the tile art's opaque extent (couples geometry to
art unpredictably, hard to unit-test) and per-tile heights in the manifest (only
viable if identity were manifest-only, and the most authoring work). Per-tile
height can be added to the manifest later without changing the model — the
override chain in §4 already has the shape for it.

## 6. Geometry

Per extruded cell: a top face carrying the tile's art UVs, and side faces.
Precedent to follow rather than reinvent — `DrawSimGroundExtension`
(`present.c`) already builds a subdivided mesh over a tilemap-derived region and
projects it through the same MVP, and `BuildQuadMesh` (`diorama.c`) already
builds arbitrary axis-pair quads.

Open choices, all cheap to try:
- **Side treatment.** Flat shade of a colour sampled from the tile's edge row is
  the cheapest and reads acceptably; alternatively reuse the top texels
  vertically stretched. Start with flat shade.
- **Face culling.** No depth buffer (`SDL_RenderGeometry` has none), so back
  faces must be omitted by camera-facing sign, exactly as `DrawDioramaShoebox`
  picks its far wall from `tilt_y`'s sign.
- **Draw order.** Prisms must sort with the existing billboards, not in a
  separate pass, or actors will paint through buildings. `SimObjectSortsAfter`
  is the existing band-local back-to-front comparator to extend.

## 7. Risks

- **WN2-R1 — vertex budget. The most serious risk.** A previous audit confirmed
  the underlay mesh arrays are *exactly* tight
  (`kSimUnderlayVertexCount/IndexCount`, with only +2 rows/cols of insertion
  headroom). Per-cell prisms across a 64×64 town — let alone a 128×128 world map
  — multiply vertex counts substantially. Bounds must be re-derived, not nudged,
  and the count must be capped with the cap *logged* rather than silently
  truncating.
- **WN2-R2 — no profiling data exists.** The ROM has never run on the authoring
  machine, so every performance claim in this spec is unmeasured. This is the one
  queued item where I expect a real ceiling. `HANDOVER.md` §4c is the profiling
  checklist to run first.
- **WN2-R3 — painter's algorithm without depth.** Tall geometry intersecting
  actor billboards has no correct painter ordering in general; a building and an
  actor at overlapping depth ranges will pick one wrong. Expect artifacts at
  cell boundaries and decide how much to tolerate.
- **WN2-R4 — near-plane clamp.** Tall prisms near the camera will cross the
  camera plane sooner than flat ground does. `a9599e6`'s `w<=0` rejection drops
  such primitives cleanly (so no smearing), but a dropped building **pops out of
  existence**, which is worse than a smear visually. May need per-frame clamping
  via `Scene3D_DepthBoundaryY`.
- **WN2-R5 — the overworld tilemap is a different key space.** The town BG1
  tilemap and the world map's 128×128 byte tilemap index different character
  sets. The class table must be per-source, or one table will misclassify the
  other map.

## 8. ROM-free testability (the design driver)

Deliberately structured so most of it is unit-testable on a machine with no ROM:

1. **Tile classification** — pure function `(tile_index, source, manifest) →
   class`. Assert the override chain: manifest beats ROM range, ROM range beats
   Flat, unknown is Flat. Table-driven.
2. **Manifest parsing** — same shape as the existing `hd_manifest_test`.
   Malformed lines, out-of-range indices, duplicate keys, ranges.
3. **Prism geometry** — pure function `(cell, class, height, mvp) → vertices`.
   Assert vertex/index counts against the declared array bounds for the worst
   reachable case (WN2-R1), that the top face's UVs match the cell's tile, and
   that the emitted count is zero for `kTileClass_Flat`.
4. **Face culling** — pure predicate `(face_normal, camera_yaw) → visible`.
   Assert exactly the expected faces at yaw extremes and that the count never
   exceeds 3 of 5.
5. **Draw-order comparator** — extend the existing `SimObjectSortsAfter` tests
   to cover a prism-vs-billboard pair.

Not testable ROM-free, and must be stated as such: whether it *looks* right,
the height constants, side-treatment choice, and all of §7's artifacts.

**Per the wave rule:** every new test gets probed against a deliberately-broken
build to prove it is not tautological.

## 9. Research vehicle: the town cathedral (author's choice)

**Start with exactly one structure — the cathedral — and get it right before any
general classifier exists.** This is the best possible first case, for a reason
that is already load-bearing elsewhere in the codebase rather than convenient:

The cathedral's position is **already derived and documented with evidence**.
`docs/rendering-engine.md:981` records that each town's world-map window origin
is "the world cathedral icon minus the town's own cathedral cell", and that this
is what *pins* the whole town-window table — "no other assignment of towns to
icons has that property". `kTownWindows[6]` in `sim_world_map.c:33-40` is that
result. So:

- Cathedral cell positions per town are established fact, not a guess.
- They are corroborated: every origin lands on a multiple of 16 and the six
  windows tile the map disjointly.
- The cathedral is also the structure whose *live* state the underlay already
  tracks (`$7E:C000`, so "cathedrals and roads appear as they currently stand"),
  which means a built-vs-unbuilt distinction is available without new research.

This collapses §4's open question for the first milestone: no ROM-derived range
table and no manifest are needed yet. One known *position*, one prism, one
height. Everything in §4/§5 becomes the *generalisation* step, informed by having
seen a prism render correctly.

It also gives §7 a cheap first answer: one cathedral is ~1 prism, so WN2-R1's
vertex budget and WN2-R3's painter-ordering artifacts can be observed in
isolation before they are multiplied by a whole tilemap.

### 9a. BUT: the cathedral is already fake-3D, and that changes the model

**Author, 2026-07-26 — this invalidates the naive "one cell → one prism"
assumption and is the single most important constraint in this spec.**

The cathedral is one of a small number of structures that the original game
*already* draws with forced perspective, by stacking tiles vertically to imply
height. Its upper tiles are not ground-plane content hidden behind the bottom
row — they are **elevation already expressed in 2D**. Factories are the same.
Most other town buildings are single-cell and behave the way §6 assumes.

So a multi-tile structure like the cathedral has two distinct kinds of tile:

- **Footprint tiles** — the cells the structure actually occupies on the ground.
- **Elevation tiles** — cells *above* the footprint in tilemap space that
  represent the structure's height, not ground at that map position.

Consequences, all of which the naive model gets wrong:

1. **Extruding every occupied cell double-counts the height.** The upper tiles
   already *are* the height; standing them up as their own prisms would produce a
   staircase of stacked boxes rather than one tall building.
2. **Extruding only the footprint discards the art.** The upper tiles carry the
   building's actual facade — roof, windows, spire. Those texels have to end up
   on the prism's *sides and top*, not left lying on the ground.
3. **The ground beneath the elevation tiles is unknown.** Those cells contain
   building art, not terrain, so once the structure is lifted there is a hole
   where they were. Something must fill it — most likely the surrounding
   terrain tile, which is a guess the current data does not resolve.
4. **Granularity mismatch.** A town cell is 16 authentic pixels
   (`kSimTownCellPixels`) while a canvas/world tile is 8
   (`kSimWorldMapTilePixels`, `kSimTownCanvasTiles = 64` over 512px), so "the
   cell above" is two tile rows up, and a structure's footprint and elevation
   may not align to cell boundaries at all.

**Therefore the unit of extrusion is a STRUCTURE, not a cell.** A structure
descriptor needs, at minimum: footprint cells, which tile rows are elevation,
how the elevation art maps onto the prism's faces, and what fills the vacated
ground. That is strictly more than a tile→class table can express, which means
§4's classifier is *necessary but not sufficient* — it can find single-cell
buildings, and it cannot describe a cathedral.

**Revised plan.** Stage 0 stays the cathedral, but its goal changes from "prove a
prism renders" to **"determine the cathedral's real tile composition"** — which
tiles are footprint, which are elevation, and how tall the implied structure is.
That is ROM/WRAM research on the tilemap, and it is the prerequisite for any
geometry. A single-cell building is now the better *rendering* first case, since
it isolates the geometry question from the composition question.

**Open, and needs the ROM:** are the elevation tiles distinguishable from ground
tiles by index alone? If yes, the classifier extends to a
`kTileClass_Elevation` and structures can be assembled automatically. If no,
multi-tile structures need explicit descriptors — which is exactly what the §4
manifest is for, and a strong argument for building the manifest before
attempting any multi-tile structure.

### 9b. Terrain elevation — DEFERRED TO STAGE F, not initial scope

> Recorded during design because cliff tiles will read as increasingly wrong as
> the objects around them gain height — an unresolved question worth capturing,
> NOT part of Stages 0-C. Everything below is settled design for a later phase.

**Author, 2026-07-26. This is a SECOND mechanism, not a variant of extrusion.**

Extrusion (§6) stands objects up on a flat ground plane. Cliffs are different:
the *ground itself* has elevation, and cliff-edge/cliff-face tiles are the game's
2D notation for it. So WN2 actually contains two features:

- **Object extrusion** — buildings, trees, rocks. Ground stays flat; discrete
  prisms stand on it. (§4–§6.)
- **Terrain elevation** — a per-cell height field the ground mesh itself follows,
  derived from cliff tiles. Everything else — objects, actors, shadows, the
  underlay — then sits on that surface rather than on y=0.

The second is the larger change, because *everything currently assumes a flat
ground plane*: `DrawSimGroundExtension` builds its mesh at a constant world y,
billboards are placed from a ground projection, and the shadow/rim passes assume
one ground height.

**Proposed derivation (author's sketch, to be validated):**

1. Scan each horizontal strip (tilemap row) for cliff-edge / cliff-face tiles.
2. A strip containing them is an **incline/decline** — height ramps across it.
3. A strip with none is **flat** — height holds at the running value.
4. **Water clamps the running height.** On reaching a large body of water (ocean
   or lake) the height is pinned — sea level is an absolute datum, so
   accumulated error cannot carry past it.

The water clamp is the load-bearing part and worth stating plainly: a purely
incremental scan accumulates error, and a per-row running height with no
absolute reference will drift arbitrarily far over 64 rows. Water gives
resynchronisation points, exactly like the `>=50ms` sanity guard in the scroll
math gives the interpolator a reference it cannot drift past.

**East/west cliff tiles are NOTATION, not geometry.** (Author, 2026-07-26,
clarified twice — the second correction matters.) The game presents exactly ONE
fixed view, so an east/west-facing cliff was never drawn as a receding slope.
But it also does **not** become a drawn vertical face in our model: it acts as a
**clamp/reset of the running height for the remainder of the row**.

So an east/west cliff tile is a *control signal* to the height-field derivation,
not a thing that emits triangles. Concretely, scanning a row left to right:

- a north/south cliff tile ramps the running height (incline/decline);
- an **east/west** cliff tile **resets the running height to 0 (base level)**,
  and the rest of the row continues from there;
- water clamps it to the sea-level datum (the absolute reference);
- everything else holds the current value.

**The reset is ABSOLUTE (to 0), not relative.** (Author, 2026-07-26.) The
evidence is that there is usually only ONE tile marking an east/west cliff edge,
and a single tile cannot encode a magnitude — it has no room to say "drop by
three". A lone edge marker can only mean "ground level returns to base here". A
relative step would additionally require the derivation to know how far down to
step, which nothing in the notation supplies.

This is worth stating because it also makes the scan **self-correcting**: every
east/west edge is an absolute reference point, so accumulated error from a long
run of N/S ramps cannot propagate past one. Together with water (§9b's other
absolute datum) that gives the derivation two independent resynchronisation
mechanisms, which is what makes a single-pass running-value scan trustworthy over
64 rows rather than merely plausible.

Why this is the right call and not a shortcut:

- **No new geometry at all for east/west boundaries.** I previously wrote that
  they would reuse the prism side-face builder. That was wrong: nothing is drawn
  there. The ground mesh simply has a height discontinuity between adjacent
  columns, and whatever the tilted ground quad does across that step is the
  whole visual result. Strictly less code than I estimated.
- **It keeps the derivation one-dimensional per row.** The output is still a
  per-cell height field produced by a single left-to-right pass with a running
  value plus reset points — which is exactly the shape that is cheap to
  unit-test (§9b's test list applies unchanged).
- **It matches what the art means.** A vertical face in a fixed-perspective
  tileset is the artist saying "the ground level changes here", not "there is a
  wall surface to be shaded". Drawing it as a lit wall would invent detail the
  source does not have.
- **The flood-fill alternative is definitively unnecessary.** With resets as
  in-row control signals, a per-row scan can express plateaus and terraces that
  I thought needed a 2D fill. Flood fill stays rejected.

**Consequence for Stage F:** the height field is a pure 1-D-per-row scan, and the
only renderer change is that the ground mesh samples a height per cell instead of
a constant. No cliff-wall geometry, no cliff-face texturing decision, no extra
culling rule.

**Open questions still needing the ROM:**

- **Is the north/south sign recoverable from the tile index?** An incline needs
  to know whether the ground rises or falls across the strip. If edge tiles
  distinguish "top of cliff" from "bottom of cliff" the sign follows directly;
  if not, it must be inferred from which side the water/lower ground is on, and
  will sometimes be wrong. This is now the ONLY direction question.
- **What is the height step?** One cliff tile = how many world units? Probably a
  settings-tunable constant like §5's, but the *authentic* implied step (if the
  art is drawn to a consistent scale) would be better.
- **What is "a large body of water"?** Distinguishing ocean/lake from a pond,
  river, or decorative water needs either a tile class or a connected-component
  size threshold.
- (No longer open: the reset is ABSOLUTE to 0 — a single edge tile cannot encode
  a magnitude. See §9b.)
- (No longer open: whether a vertical drop needs texturing or flat shade —
  nothing is drawn at an east/west boundary at all.)

**Risk WN2-R6 — the flat-ground assumption is load-bearing today.** Adding a
height field means auditing every consumer that assumes y=0: the ground mesh,
billboard placement, `virtual_height`, shadow projection, the rim pass, the
underlay, and the cloud shroud's altitude. This is the largest single item in
this spec and should be **its own stage after object extrusion ships**, not
bundled with it.

**ROM-free testability — good, actually.** The height-field derivation is a pure
function `(tilemap, tile classes) → height per cell`, so it is entirely
unit-testable with synthetic tilemaps:

- a flat map yields uniform height;
- one cliff row yields exactly one step, in the right direction;
- water clamps the running height and resynchronises after drift;
- an east/west edge tile resets the running height to exactly 0, whatever it had
  climbed to beforehand (the absolute-reset rule);
- two ramps separated by an east/west edge do NOT accumulate — the second starts
  from 0, which is the property that makes the scan self-correcting;
- a map with no water still terminates with bounded height;
- the same tilemap always yields the same field (determinism);
- adjacent cells never differ by more than one step (no cliffs from nothing).

That is the part worth building first, because it can be proven correct here
before any geometry consumes it.

## 10. Effort and staging

- **Stage 0 — cathedral COMPOSITION research (no rendering).** Per §9a the
  cathedral is already fake-3D, so the first question is data, not geometry:
  read the town BG1 tilemap around the known cathedral cell and determine which
  tiles are footprint and which are elevation, whether elevation tiles are
  distinguishable by index, and the implied height in cells. Deliverable is
  evidence recorded in `docs/rendering-engine.md`, in the style of the existing
  world-map derivation. **Needs the ROM; cannot be done on the authoring
  machine.**
- **Stage 0b — one single-cell building, hardcoded.** The rendering first case,
  chosen to isolate geometry from composition. Extrude one known single-cell
  building behind an off-by-default setting with a settings-tunable height.
  Answers: does a prism on the tilted ground read correctly, how does it sort
  against actor billboards, and what does the near-plane clamp do up close
  (WN2-R4 — note a dropped building POPS rather than smears). **~half a day.
  Stop and look at it.**
- **Stage A** — tile-identity accessors + classification + manifest (§4), no new
  rendering. Fully unit-tested, zero visual change. ~1 day.
- **Stage B** — prisms for single-cell buildings generally, sim-town only. ~1 day.
- **Stage C** — trees/rocks, side treatment, culling, sort integration. ~1–2 days.
- **Stage D** — MULTI-TILE structures (cathedral, factories). Needs Stage 0's
  evidence and the §4 manifest, since a structure descriptor is more than a
  tile→class table can express. Elevation art must land on the prism's faces and
  the vacated ground must be filled. **Largest and least certain stage; scope it
  only after Stage 0.** ~2–3 days.
- **Stage E** — overworld (needs WN1 and WN2-R5). ~1 day.
- **Stage F — TERRAIN ELEVATION (§9b).** The height-field derivation first, as a
  pure unit-tested function with no renderer attached; then the ground mesh
  follows it; then every flat-ground consumer is audited (WN2-R6). Deliberately
  LAST: it changes an assumption the whole 3D path is built on, so it wants a
  stable object-extrusion baseline underneath it. **~2-4 days** — revised down
  twice: east/west boundaries emit no geometry at all (they are height-field
  reset signals), so the only renderer change is the ground mesh sampling a
  per-cell height instead of a constant. The bulk of the remaining cost is
  WN2-R6, auditing every consumer that assumes flat ground.

Stage 0 deliberately front-loads the visual and artifact questions — the ones
that cannot be answered ROM-free — so the unit-testable infrastructure in Stage A
is only built once we know the rendering approach is worth building it for.

Stage A remains worth doing regardless of the outcome: publishing tile identity
is useful for the N\*X tile pack, for diagnostics, and for any future per-tile
feature, and it cannot regress rendering because nothing draws differently.

### What Stage 0 can and cannot prove here

Testable ROM-free: the prism geometry for a known cell (vertex/index counts, top
UVs, zero output when disabled), and that the feature is inert when the setting
is off. Not testable: whether it looks right, the height value, and every §7
artifact — all of which is precisely why Stage 0 is small and comes first.

## 11. Open questions

1. **WN2-R1 bounds:** what is the maximum extruded cell count we will support,
   and is it a hard cap or view-frustum-driven?
2. **SUPERSEDED by §9a** — the answer is "both, and multi-cell ones are already
   fake-3D". The real questions are now: are elevation tiles distinguishable by
   index alone, and what fills the ground they vacate?
3. Should extrusion apply in the *authentic* flat view at all (no — it must be
   diorama/3D-mode only, and provably inert otherwise), and does it interact
   with the picker's pixel-identical requirement?
4. WN2-R5: is one class table enough, or one per tilemap source?
