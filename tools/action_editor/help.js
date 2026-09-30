
/* ---- the docs panel ----------------------------------------------------
 * Kept in the tool on purpose. The reasoning behind the band model is the
 * part that is expensive to rediscover, and it is the part someone will need
 * when they come to wire the export into the engine. */
$('#docsBody').innerHTML = `
<h4>Edit from the map</h4>
<p>Choose <b>Select</b>, click a tile, or Shift-click another tile to select a
range. The bar above the map shows the selection count, its <b>applied band</b>
(including mixed bands), background, and regions that share these edits.
Selection alone makes no changes. <b>Far</b>, <b>Normal</b> and <b>Priority</b>
apply immediately to the selection. The paintbrush band in the sidebar only
sets the band for advanced painting tools.</p>
<p><b>Copy</b>, <b>Paste over</b>, <b>Stamp</b>, and <b>Fill transparency black</b>
work above the map or in its right-click menu. <b>Pixels…</b> opens a pixel
inspector beside the map for the focused tile; double-clicking a tile opens it
directly. Closing the inspector keeps the selection. <b>View selection in 3D</b>
centers the Diorama camera on that area. Advanced paint/depth and edge-space
controls are in expandable sidebar sections.</p>

<h4>See what changed and keep your work</h4>
<p><b>Blue</b> marks selected tiles; <b>gold</b> marks authored band edits,
pixel edits and pasted tiles, including edits loaded from your INI.
Choose <b>Highlight → Modified tiles</b> for these gold outlines, or choose
<b>Far band</b>, <b>Normal band</b> or <b>Priority band</b> to locate tiles by
their applied destination. Band highlights use purple, green and orange;
mixed tiles match any of their four 8px quadrants. Pasted tiles use their
current bands. <b>Show highlights</b> toggles the chosen overlay.</p>
<p>The matching count and <b>Previous edit</b>/<b>Next edit</b> (or
<b>Previous match</b>/<b>Next match</b>) review the current background and terrain
family. <b>Select edited tiles</b>/<b>Select highlighted tiles</b> selects all
matches so you can apply a bulk action. Choosing a highlight filter makes no
edits and keeps the selection. <b>Compare original tiles</b> temporarily hides black
masks, pasted scenery and band tint; your edits stay intact. Editing returns
to the edited view. <b>Original game frame</b> shows the native presentation.</p>
<p>Actions apply immediately to the preview; <b>Undo</b> reverses them.
<b>Unexported changes</b> means some room edits have not been copied or downloaded.
<b>Export level INI</b> opens a modal for the current room. Choose <b>Copy section</b>,
replace that room’s section in <code>diorama-layers.ini</code>, save, and restart.
Copying updates only this room’s savepoint; other rooms stay flagged.
Opening or closing the modal does not mark anything exported. Undo/Redo also
updates the badge. <b>Download full INI…</b> saves every room in one file.
Export before refreshing this page, then use <b>Load INI</b> to resume.</p>

<h4>Regional terrain</h4>
<p><b>Terrain variant</b> selects US, Japanese, or European platforms and
metatile definitions in every view. It uses the same room projection as the
game's <b>Platforms and terrain</b> setting. BG2 and other presentation choices
keep their base assets; terrain selection does not change mosaic patterns,
artwork, enemies, or items.</p>
<p>Tile edits share one set when the placed terrain matches. Different layouts
have separate bands, pixel masks, pasted scenery and edge bounds; unused tile
definition changes do not require another pass. BG1 and BG2 share independently.
The terrain hint lists which regions share the current background's edits.
Plane and depth geometry stay shared across the room.</p>
<p>Tile keys accept <code>:us</code>, <code>:jp</code>, <code>:eu</code> and
<code>:ge</code>. An untagged tile key means US; German shares European terrain.
Combined keys such as <code>bg1-pixels:us+eu</code> store shared edits once.
The game uses its active room terrain snapshot. Switching terrain preserves
all edits and history; undo returns to the affected terrain family. The saved
limits count distinct records across terrain families once.</p>

<h4>What a band is</h4>
<p>A band is a presentation surface anchored to the hardware BG it came from,
so it inherits that BG's tilemap, dimensions, camera, and live scroll deltas.
It is <b>not another hardware BG</b>: moving a tile to the other one would attach
it to the wrong scroll registers and make it drift or stop moving.</p>
<table><tr><th>#</th><th>band</th><th>where it draws</th><th>engine</th></tr>
<tr><td>0</td><td>Far virtual plane</td><td>dedicated captured surface</td><td>supported</td></tr>
<tr><td>1</td><td>Plane (default)</td><td>the anchor's priority-0 surface</td><td>supported</td></tr>
<tr><td>2</td><td>Priority band</td><td>the anchor's priority-1 surface</td><td>supported</td></tr></table>

<h4>Selection and partial transparency</h4>
<p><b>Right-click</b> a tile in the map for its action menu. Right-click inside
a selection to keep the whole range; right-click another tile to select that
tile. Priority on/off and Far plane change the selected tiles' Diorama bands.
Reset tile band removes local overrides, keeping any metatile rule.</p>
<p><b>Copy selected tiles</b> copies the range. <b>Paste over here</b> places the
copied rectangle with its top-left corner at the right-clicked cell, including
empty edge space. <b>Stamp copied tiles</b> starts repeated placement. The menu
also opens the pixel editor, fills selected transparency black, removes pasted
tiles, or clears the selection. Edits use the ordinary Undo/Redo history.</p>
<p>Click outside or press Esc to close the menu without clearing the selection.
With the map focused, Shift-F10 opens the same menu. Arrow keys choose an action;
Enter activates it. Paste requires a clipboard from the same room, background,
and matching terrain.</p>
<p><b>Select cells</b> highlights cells without changing their bands. Use
<b>Apply band to selection</b> to save a layer change. <b>Deselect</b>,
<kbd>Esc</kbd>, or <kbd>Ctrl/Cmd-D</kbd> clears highlights and keeps edits.
<b>Highlight → Modified tiles</b> separately shows authored band, pixel and pasted-tile edits.</p>
<p>With <b>Select cells</b> or <b>Select rectangle</b>, click a start tile and
<b>Shift-click</b> the opposite corner. All tiles in that rectangle are selected,
including both corners. Further Shift-clicks resize the range from the same
start tile. <b>Copy</b> and <b>Apply band</b> work on this selection. Deselect
clears the start point; changing room or BG starts a fresh selection.
<b>Shift-drag</b> still pans the map.</p>
<p>For a partially transparent tile in Kassandora Act 2, Room 4, choose
<b>Pixels…</b> after selecting that placed tile. Keep <b>This map cell
only</b> selected, then paint the desired pixels black on the magnified grid.
Checkerboard pixels remain transparent until painted. <b>Restore pixel</b>
returns a pixel to its original ROM art. <b>Fill transparent pixels black</b>
fills all transparent pixels of that tile; <b>Make whole tile black</b> also
covers its existing art. You can instead choose all instances of the metatile.</p>
<p>Pixel edits affect <b>Diorama 3D only</b>, appear in the authoring map, and
share Undo/Redo and INI export. Native frame keeps the original game pixels.
A cell mask overrides a metatile mask. Reset removes the selected scope's
mask, inheriting the remaining metatile edit when applicable.</p>
<p>For several tiles, select a range or individual cells, then click
<b>Fill transparency black</b> in the toolbar or <b>Fill selected transparency
black</b> in the sidebar. It fills only the highlighted placed tiles, including
pasted scenery, and preserves their artwork and existing black edits.
The single-tile scope dropdown does not expand this operation to unselected
metatile instances. One Undo restores the whole fill. Export level INI saves it.
If it would exceed 256 pixel records per BG, no pixels change and the status
asks you to select a smaller range.</p>

<h4>Copy and extend backgrounds</h4>
<p><b>Tiles…</b> opens the loaded artwork palette beside the map. The
<b>16×16 tiles</b> view includes every metatile definition, including ones
absent from the original scene. Gold dots mark unused definitions; filter by
<b>Unused in source</b> to find them. Click a tile, then click the map to stamp
it. Choices make no edits until placed. Thumbnails keep their original colors
and show transparency as checkerboard.</p>
<p><b>Artwork source</b> also offers other maps with identical graphics, color
palette and animation. Their tile arrangements can be borrowed safely.
Maps with different graphics banks require importing artwork into the game;
they do not appear here. The selected terrain variant supplies definitions.
The usage filter refers to the original source map, before your edits.</p>
<p>Choose <b>8×8 pieces</b> to assemble a 16×16 tile from four loaded characters.
Choose a color palette, click a quarter, and select a piece; the next quarter
becomes active automatically. <b>Use assembled tile</b> makes it a stamp.
Selecting a 16×16 tile seeds the four quarters, so you can replace just one
piece. <b>Mirror stamp H/V</b> reflects the current stamp before placement,
including a copied range. The preview shows the result.</p>
<p><b>Flip H</b>/<b>Flip V</b> immediately flip every selected tile in place.
<b>Mirror range H</b>/<b>Mirror range V</b> reverse tile order and flip the
artwork together: a horizontal row A, B, C becomes horizontally flipped C, B, A.
Vertical mirroring reverses rows and flips each tile vertically.
The right-click menu offers the same operations. Whole-range
mirroring requires a complete rectangle; individual flips work on scattered
selections. Existing horizontal/vertical flags are toggled, and quadrant
positions, depth bands and black pixel edits move with the artwork.
Copies and exports preserve those flags. One Undo reverses the whole operation.
Flipped tiles use the same 512-tile and 256-pixel-record budgets as pasted tiles;
an operation that exceeds a budget changes nothing.</p>
<p>Choose <b>Select rectangle</b> and drag over the source tiles. <b>Copy selected
 tiles</b> (<kbd>Ctrl/Cmd-C</kbd>) copies the whole rectangle, including art,
 depth bands, flip flags and black masks. <b>Add edge space</b> exposes empty cells on any
 side; its count uses 16px tiles. Pasting outside the original edge also grows
 the authoring bounds.</p>
<p>Choose <b>Stamp copied tiles</b> (<kbd>Ctrl/Cmd-V</kbd>), then click each new
 top-left corner. The ghost shows placement. Keep clicking to repeat;
 <kbd>Esc</kbd> ends stamping. X/Y and <b>Paste</b> place precisely, with negative
 coordinates extending left or up. Copy and paste stay within the same room/BG.
 An overlapping paste uses the frozen source rectangle.</p>
<p>Select pasted cells to change bands or paint black pixels. <b>Remove selected
 pasted tiles</b> restores their original scenery. <b>Reset this BG's pasted
 scenery</b> also removes added edge space. These actions are undoable.
 Saved bounds shrink to the native map plus remaining pasted tiles. Empty edge
 space is only a workspace; deleting the bottom extension lets the game anchor
 to the native bottom again.
 <b>Export level INI → Copy section</b> saves them; replace this room’s INI
 section, save the file, and restart the game.
 The additions affect Diorama only. There are 512 pasted tiles per BG; black
 masks share the 256 pixel-record budget. An oversized paste changes nothing.</p>
<p>New edge space can be selected like existing tiles. <b>Fill transparency
 black</b> creates black tiles there; <b>Pixels…</b> paints part of a blank tile.
 Selection alone makes no edits. Restoring pixels on a blank reveals transparency.</p>
<p><b>Trim unused edge space</b> under Scenery &amp; edge space fits the workspace
 to the original map plus remaining added tiles. With no added edge tiles,
 this restores the original bounds. It keeps all tile edits and supports Undo.</p>
<h4>Saved Diorama framing</h4>
<p>Click <b>Frame</b> above the map, or <b>Adjust frame on map</b> in the sidebar.
 The dashed white rectangle shows the native 256×224 view at the current
 preview scroll; the gold rectangle shows your saved offset. Drag the gold
 frame to adjust it, or use arrows for 1px steps and Shift + arrows for 16px.
 A drag is one Undo step. Esc cancels a drag and returns to Select.
 The guide uses BG1 coordinates; the saved offset applies to the whole scene.
 <b>Show viewport guides on BG1</b> toggles the guides, and <b>Diorama 3D</b> previews
 the result with depth and perspective.</p>
<p>Enable <b>16:9</b> (cyan) and <b>16:10</b> (purple) to compare wider viewports.
 All frames share the same center and move together; you can drag inside any
 visible frame. Match <b>Pixel aspect</b> to the game: CRT (4:3) shows 342×224
 and 308×224 map pixels, while Square pixels shows 400×224 and 360×224.
 <b>Flat viewport</b> keeps native height and uses the game's whole-column rounding.
 Select <b>Guide coverage → Diorama estimate</b> and match <b>Camera distance</b>
 to the game for the projected BG1 footprint. This uses the game's field of
 view and BG1 depth at zero tilt; dynamic floor alignment can shift it vertically.
 Resolution alone does not change coverage at the same aspect ratio.
 Guide options affect only this preview and do not add separate INI offsets.</p>
<p><b>View X / View Y</b> save a view offset relative to the existing scroll
 anchor. Negative X looks left; negative Y looks up. Leave Y at zero to keep
 normal floor anchoring. Adding or removing scenery never changes these offsets.
 The controls allow −64 to +64 pixels, support Undo/Redo and regional terrain
 sharing, and are included in <b>Export level INI</b>. <b>Reset framing</b>
 restores the default. Gameplay camera and collision are unchanged. Vertical
 offsets use the available capture, so large shifts can expose an edge.</p>
<p>Added scenery supplies horizontal edge padding in Diorama, allowing the
 camera to move farther toward the extended side within its native gameplay
 range. Empty workspace does not change the camera stop. Saved framing still
 applies at that stop.</p>
<p>The <b>Native frame controls</b> camera sliders are preview-only and do not save
 a room's framing.</p>
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
<code>bg2-virtual</code>, <code>bgN-pixels</code>, <code>bgN-stamp</code>, and <code>bgN-map</code> records in base action-room sections. Export preserves
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
<p><b>Export level INI</b> shows a complete replacement <code>[layers:GG:MM]</code>
block for this room, including both BGs, every terrain variant, and existing
settings and comments from the loaded base section. <b>Copy section</b>, then
replace the old block in your INI, stopping at the next section header. Replace
all duplicates with this single block; append it if missing. Keep camera-specific
sections. Replacing the block also removes any edits you reset in the editor.</p>
<p>If clipboard access is blocked, use <b>Select all</b> and <kbd>Ctrl/Cmd-C</kbd>.
<b>Download full INI…</b> keeps the full-file workflow available. Save your INI
and restart the game to test; load that file again to resume editing.</p>
<p>Overlapping pastes retain only the latest tile at each destination. Identical
regional records share a line, and band edits coalesce into horizontal runs.
Pasted art still needs one record per cell: the runtime has no rectangle/repeat
stamp format. Export preserves local scope and frozen tile art.</p>`;
