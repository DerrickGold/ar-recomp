
/* ---- the docs panel ----------------------------------------------------
 * Kept in the tool on purpose. The reasoning behind the band model is the
 * part that is expensive to rediscover, and it is the part someone will need
 * when they come to wire the export into the engine. */
$('#docsBody').innerHTML = `
<h4>What a band is</h4>
<p>A band is a presentation surface anchored to the hardware BG it came from,
so it inherits that BG's tilemap, dimensions, camera, and live scroll deltas.
It is <b>not another hardware BG</b>: moving a tile to the other one would attach
it to the wrong scroll registers and make it drift or stop moving.</p>
<table><tr><th>#</th><th>band</th><th>where it draws</th><th>engine</th></tr>
<tr><td>0</td><td>Far virtual plane</td><td>dedicated captured surface</td><td>supported</td></tr>
<tr><td>1</td><td>Plane (default)</td><td>the anchor's priority-0 surface</td><td>supported</td></tr>
<tr><td>2</td><td>Priority band</td><td>the anchor's priority-1 surface</td><td>supported</td></tr></table>

<h4>Resetting</h4>
<p>Classification resets have three scopes &mdash; BG, room, and every room &mdash;
and are all undoable.
Because a tile's default <i>is</i> its own priority bit and an edit is only an
entry that overrides it, resetting is deletion rather than a rewrite: a reset
room is bit-for-bit what the cartridge says.</p>

<h4>The baseline is authentic, not flat</h4>
<p>A room already has a depth split: the priority bit. Every tile starts in the
band its own bit selects, so the editor's first render agrees with the game and
an edit is a <b>delta</b>. That is why the export carries only what you
changed, and why the outline overlay marks edited cells rather than all of
them.</p>

<h4>The INI is the source of truth</h4>
<p>This editor is the authoring surface. It imports the complete
<code>diorama-layers.ini</code> and owns <code>bg1</code>, <code>bg1hi</code>,
<code>bg2</code>, <code>bg2hi</code>, <code>bg1-virtual</code>, and
<code>bg2-virtual</code> records in base action-room sections. Export preserves
all unrelated planes, comments, scoped sections, and settings.
The game only consumes and renders the result.</p>
<pre>[layers:01:01]
bg1 = z:0.5 voxel:0.08 slices:12 dir:backward
bg1-virtual = z:0.35 order:4 alpha:255
bg1-virtual = metatile:23 band:0
bg1-virtual = cells:4,5-12,5 band:2</pre>
<p>Geometry, metatile rules, and cell rules are separate repeatable records.
Resolution is cell first, then metatile, then the ROM priority bit. Cell
coordinates name 16&times;16 metatile cells; rectangle endpoints are inclusive.
Sparse records keep hand-tuned files reviewable.</p>

<h4>Game composite parity</h4>
<p>The 2D view always paints the authentic Mode-1 background order: BG2 low,
BG1 low, the OBJ2 reference, BG2 high, then BG1 high. Virtual classification
can tint pixels and draw edit outlines, but it never changes this flat order.
That separation is intentional: the same configuration must leave ordinary
game presentation unchanged while enriching Diorama 3D.</p>

<h4>Geometry and paint order are independent</h4>
<p>The shipped defaults put BG2 at z 0.20, BG2 high at 0.21, BG1 at 0.50,
BG1 high at 0.51, and OBJ2 at 0.51. A new far plane defaults 0.15 behind its
anchor. Its Z, order, and alpha controls write directly to the room's virtual
record. The ordinary BG selector edits the four native low/high plane records:
Z, paint order, alpha, and the same Flat/Rake/Bow/Thickness/Stack/Voxel shape
strategies as the runtime.</p>
<p>Rake and bow use the runtime's six-row depth mesh. Thickness repeats the
captured bottom row across the shaded skirt. Stack and voxel use the same copy
limits, forward/backward/both placement, falloff, and redundant-copy rule as
the game. Imported records may compose multiple shape keys; selecting a new
strategy deliberately makes the shape exclusive, matching the game editor.</p>
<p>The renderer is a painter: <b>order</b> decides which surface covers another,
while <b>z</b> decides its projected location and depth effect. The ladder shows
both values. It warns only when surfaces from different BGs are nearly
co-planar; each BG's own low/high pair is intentionally close and is not a
warning.</p>

<h4>Sliding the other layer</h4>
<p>The two backgrounds are rarely the same size &mdash; <b>17 rooms</b> have a
BG2 of a single 256&times;256 page against a playfield up to 4096 long, and 16
more simply differ &mdash; and in play BG2 scrolls at its own rate or repeats.
So there is <b>no single full-level alignment</b> to inspect in the map editor,
and checking only the arrangement that happens to sit at the origin would miss
most of the relationship. Diorama 3D does not use this approximation: it uses
the native camera and each BG's exact resolved scroll.</p>
<p>The <b>X</b> and <b>Y</b> controls slide the other layer in level pixels, as
a fraction of its own extent so the same throw covers a 256px background and a
4096px one. <b>Repeat</b> tiles it across the active layer, which is what a
pinned background actually does; it is drawn as copies rather than a wrapped UV
because these textures are not power-of-two and WebGL1 will not REPEAT them.
Slide it end to end to scan the whole relationship for conflicts.</p>

<h4>The reference actor</h4>
<p>Background priority is judged against objects, so without an object in the
scene there is nothing to compare it with. The <b>Actor</b>
button puts a stand-in at the depth the engine composites sprites at, and it
drags in either view.</p>
<p>The stand-in uses the resolved OBJ2 z and order from the loaded INI. As in
the game, configured paint order decides whether a background appears before
or after it; the small z separation controls how that relationship reads when
the camera tilts.</p>
<p>It is a silhouette rather than real sprite art on purpose: the question is
one of ordering and footprint, and lifting the room's OBJ characters out of the
asset script would be a second reverse-engineering problem for no extra
answer.</p>

<h4>Why the preview is built this way</h4>
<p>The map editor keeps a full-level surface because it is an authoring view.
Native frame and Diorama 3D instead build the exact 256&times;224 capture for the
selected camera and frame: BG1/BG2 parallax, all ten persistent raster presets,
mosaic, CHR animation and the BG2 page cycle are resolved before pixels are
routed into virtual bands. Each band is then drawn as one textured quad at its
own z &mdash; deliberately the runtime shape, where a plane <i>is</i> a captured
surface composited at a depth.</p>
<p>The camera is a plain right-handed <code>perspective * lookAt</code> in the
same column-major convention as <code>scene3d_math.c</code>; the fragment stage
is <code>texel * vertex colour</code>, which the SIM 3D path already uses.
Changing the native camera controls changes the source capture instead of
sliding a decorative approximation.</p>

<h4>Extents differ, and that matters</h4>
<p>Across the 49 rooms: 10 have equal extents, 16 have a longer playfield, 6
have a longer or taller background, and <b>17 have a BG2 that is a single
256×256 page</b> &mdash; pinned or tiled behind everything. That last group is
what reads as a monolithic room. Nothing here assumes the two layers share a
grid.</p>

<h4>Export</h4>
<p><b>Export INI</b> downloads a complete merged configuration, not a sidecar.
The four ordinary BG and two virtual records in action-room base sections are
regenerated. Cell edits are coalesced into horizontal inclusive rectangles,
metatile records are sorted, and untouched rooms stay untouched. Load the
exported file again to continue authoring without any conversion step.</p>`;
