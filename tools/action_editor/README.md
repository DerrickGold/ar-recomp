# Action-mode layer editor

This standalone editor classifies action-room background tiles into virtual
depth bands and authors those changes directly in `diorama-layers.ini`. The
editor is the source of truth; the game only loads and renders the exported
configuration.

```sh
sh tools/action_editor/build.sh
sh tools/action_editor/build.sh ar.sfc out.html path/to/diorama-layers.ini
```

The build needs a C compiler and Python 3. Its output is a self-contained HTML
file that can open from `file://`. The default output is
`build/action-editor/ar-action-layer-editor.html`, keeping the generated editor
out of the source tree and Git. By default it embeds the repository ROM and
`diorama-layers.ini`; **Load INI** can replace the configuration at runtime and
**Export INI** downloads a complete merged file.

## Regional terrain

**Terrain variant** selects US, Japanese, or European terrain for all 49 rooms
in Map editor, Native frame, and Diorama 3D. The build projects BG1's map and
metatile definitions with `ActionRoomTerrain_Project`, the same authority used
by the game's **Platforms and terrain** setting. Each variant carries its own
C golden frame, and rendering caches distinguish the selected terrain.

The default US ROM supplies the base assets; Japanese and European terrain
come from the game's measured patches, so no additional ROM is needed. This
selector changes terrain only. BG2, artwork, mosaic patterns, enemies, and
items keep their base presentation. The room hint counts map cells and
metatile definitions that differ from US, including definitions not used in
the current room.

Tile edits share an authoring set only when a background's placed metatile IDs,
resolved tile words and dimensions match. Definitions that never occur in the
map do not split that set. BG1 and BG2 are compared independently, so BG2 stays
shared when only the platform layout changes. The room hint lists the regions
that share your current background's tile edits. Pixel masks, bands, pasted
scenery and expanded edges follow that set; plane and depth geometry stay
shared across the room. Undo switches to the affected terrain family when needed.

Tile keys accept `:us`, `:jp`, `:eu`, and `:ge` region suffixes. Untagged tile
keys mean US. German is an alias of the game's European terrain profile. A
combined key such as `bg1-pixels:us+eu` stores a shared edit once. Loading an old
untagged INI seeds the US terrain family, including regions that match it;
export writes explicit combined keys where sharing is needed. A different
layout receives its own records. Export still writes one section per room.

For example, these masks apply to different regional terrain:

```ini
[layers:03:04]
bg1-pixels = cell:4,5 black:8000000000000000000000000000000000000000000000000000000000000000
bg1-pixels:jp = cell:4,5 black:4000000000000000000000000000000000000000000000000000000000000000
```

The game resolves tile edits against the terrain snapshot active for the room,
independently of language, artwork, difficulty, or pending settings changes.
The saved limits (256 pixel records, 512 pasted cells, 512 cell spans per BG)
count distinct records across all terrain families, with shared records counted
once. Copy/stamp stays within the same room, background and terrain family.

## Authoring model

Every BG retains its own tilemap, dimensions, scroll, and authentic priority.
Moving scenery to the other hardware BG would attach it to the wrong camera, so
the editor instead classifies tiles into three surfaces anchored to their
source BG:

| band | surface | default source |
|---|---|---|
| 0 | far virtual plane | newly captured Diorama surface |
| 1 | ordinary plane | ROM priority bit clear |
| 2 | priority plane | ROM priority bit set |

Unedited tiles always fall back to their ROM priority bit. A metatile rule
changes every instance of a 16x16 metatile; a cell rule refines one location.
Resolution is cell, then metatile, then ROM priority.

The far plane has independent Z, paint order, and alpha controls. Z controls
its 3D geometry; order controls overlap because the Diorama renderer is a
painter. The defaults are z 0.35/order 4 for BG1 and z 0.05/order 3 for BG2,
placing each far plane 0.15 behind its anchor and immediately before it in
paint order.

## Selection and tile pixels

Start with **Select** above the map. Click a tile or Shift-click another tile
to select a range. The selection bar reports its applied band (or mixed bands),
BG, and the regions sharing its edits. **Far**, **Normal**, and **Priority**
apply immediately to that selection. **Copy**, **Paste over**, **Stamp**,
**Fill transparency black**, and **Pixels…** are available in the same bar.
**View selection in 3D** centers the preview on the selected area.

Blue outlines mark selections; gold outlines mark authored band edits, pixel
edits and pasted tiles, including those loaded from the INI. Choose
**Highlight → Modified tiles** for these gold outlines, or **Far band**,
**Normal band**, or **Priority band** for tiles with that applied destination.
Band highlights use purple, green and orange. Mixed tiles match any of their
four 8px quadrants; pasted tiles use their current bands. **Show highlights**
toggles the chosen overlay. Filtering makes no edits and keeps the selection.

**Previous edit**/**Next edit** (or **Previous match**/**Next match**) center and
select each matching tile in the current BG/terrain family. **Select edited
tiles**/**Select highlighted tiles** selects all matches for a bulk action.
**Compare original tiles** hides authored pixel masks, pasted scenery
and band tint temporarily. Editing returns to the edited preview.

Actions apply immediately in the preview. The **Unexported changes** badge
compares the complete INI against the loaded file or last export, including
Undo/Redo. **Export changes** downloads that INI and changes the badge to
**Matches last export**. Replace the game's `diorama-layers.ini` with the
download and restart to use it. Export before refreshing the editor; use
**Load INI** afterward to resume your work.

Advanced paint/depth, native-frame and edge-space controls live in expandable
sidebar sections. The sidebar's **Paintbrush band** sets the advanced brushes;
it does not change the selected tiles until you paint or apply it.

Right-click a tile in the map for its action menu. A click inside the existing
selection keeps the whole range; a click on another tile selects that tile.
Opening the menu never paints or pans, regardless of the active brush. Mac
Control-click opens it too; use Cmd-click for additive selection on a Mac.

The menu offers priority on/off, Far plane, Reset tile band, pixel editing,
transparency fill, Copy, Paste over here, repeated stamping, removal of pasted
tiles, and Deselect. Priority changes use Diorama bands and keep authentic tile
words unchanged. Reset tile band removes local overrides and inherits any
metatile rule. Mutating actions use the ordinary Undo/Redo history.

**Paste over here** places the copied rectangle with its top-left corner at the
clicked cell. Empty edge space is a valid destination, and pasting expands the
map bounds as needed. Copies stay within the same room, BG and terrain family.
The menu disables Paste when that clipboard is unavailable. Feedback appears
at the top of the map, including any capacity error.

Click outside, scroll the map, or press Esc to dismiss the menu while keeping the
selection. With the map focused, **Shift-F10** or the keyboard menu key opens
it; arrows, Home/End, Enter and Tab provide keyboard navigation. Changing room,
BG, terrain or view closes it.

**Select cells** highlights without changing bands. **Apply band to selection**
saves a classification. **Deselect** (Esc or Ctrl/Cmd-D) clears highlights and
keeps saved edits; **Highlight → Modified tiles** shows the authored-edit overlay.

With **Select cells** or **Select rectangle**, click a start tile, then
**Shift-click** the opposite corner to select the inclusive rectangle between
those two points. Further Shift-clicks resize it from the same start tile.
**Copy** and **Apply band** use that range. Selection does not save edits;
Deselect clears the start point, and switching room or BG starts fresh.
**Shift-drag** still pans the map.

For a partial transparency fix in **Kassandora · Act 2 · Room 4 (3:4)**:

1. Double-click the placed tile, or select it and choose **Pixels…**.
2. Keep **This map cell only** selected in the pixel inspector beside the map.
3. Paint the desired subsection black on the magnified 16×16 grid. Unpainted
   checkerboard pixels retain transparency. **Restore pixel** restores ROM art.
4. Check **Diorama 3D**, then **Export INI** and replace the game's
   `diorama-layers.ini` with the downloaded file. Restart the game to load it.

**Fill transparent pixels black** fills only colour-zero pixels of the selected
cell. **Make whole tile black** also covers its original art. Both can instead
apply to all instances of the selected metatile. Pixel strokes and bulk edits
are undoable. Cell masks take precedence over metatile masks; reset removes the
selected scope's mask. Regional layouts share these rules just like bands.

To fill several placed tiles at once, select them with Shift-click, a dragged
rectangle, Select cells, or Select by, then choose **Fill transparency black**
in the toolbar or **Fill selected transparency black** in the sidebar. Only
their transparent pixels become opaque black; original artwork, existing
black pixel edits, depth bands, and unselected tiles stay as they were. The
operation includes pasted tiles and always targets the selected placed cells,
regardless of the single-tile metatile scope. It creates one Undo step and is
saved by Export INI. If the whole operation would exceed the 256 pixel-record
limit per BG, the editor reports it and changes no pixels.

Pixel edits are opaque-black presentation masks, not changes to ROM CHR or
palette data. The normal game and **Native frame** remain authentic. Map editor
shows the edits for authoring; Diorama captures route them through the same BG,
camera, live raster/mosaic, windows and depth bands as their source tiles.

Each repeatable `bgN-pixels` line names `cell:x,y` or `metatile:HH`, followed by
`black:` and 64 hexadecimal digits: sixteen rows of sixteen mask bits, with the
highest bit on the left. A cell's explicit zero mask restores original pixels
under a metatile mask. The bounded runtime accepts 256 pixel records per BG;
the editor checks that limit before exporting. Existing whole-plane
`transparent:off|black|cgram-HH` settings are preserved during export.

## Copy, paste, and extend scenery

1. In **Map editor**, choose the room and BG, then **Select rectangle** and drag
   across the tiles to copy, or click the start tile and Shift-click the opposite
   corner. **Copy selected tiles** (Ctrl/Cmd-C) captures the
   complete rectangle, including its art, four depth bands, and pixel edits.
2. Use **Add edge space** to expose empty cells at the left, right, top, or
   bottom. The count is in 16px tiles; four adds 64px. Pasting beyond an edge
   also expands the authoring bounds automatically.
3. Choose **Stamp copied tiles** (Ctrl/Cmd-V). Its translucent preview follows
   the pointer; click the destination's top-left cell. Click again to repeat,
   then press Esc to finish. The X/Y fields and **Paste** provide exact placement.
4. Check **Diorama 3D** and **Export INI**. Replace the game's
   `diorama-layers.ini` with the export and restart the rebuilt game.

Copies stay attached to their original room, BG and terrain family so their
graphics bank, scroll, palette and layout remain valid. An overlapping paste
uses the original clipboard snapshot. Matching regional terrain shares that
clipboard; a different layout requires copying its own source art. Characters
still follow the selected artwork and animation. Paste overwrites the
destination's presentation art, including
transparent pixels. It does not alter gameplay maps or collision.

**Remove selected pasted tiles** restores the underlying ROM scenery;
**Reset this BG's pasted scenery** also removes added edge space. Paste,
removal, edge expansion, bands, and pixel painting share Undo/Redo. Pasted cells
can be selected and painted black individually in the pixel editor.

There are at most 512 pasted cells per BG. Bounds can span 512 tiles on either
axis, within signed coordinates −512 through 511. Rectangles are limited to
512 cells. Copies carrying black masks also use the shared 256 pixel-record
budget; an oversized paste leaves the map unchanged and reports the limit.

The export uses frozen displayed SNES character words and one band per 8px
quadrant (top-left, top-right, bottom-left, bottom-right):

```ini
bg1-map = bounds:-4,0,32,16
bg1-stamp = cell:-1,0 metatile:23 words:0010,4011,8012,E013 bands:0,1,2,1
```

Map bounds have exclusive upper edges. Stamp positions and pixel masks accept
negative cell coordinates. Pixel masks use the existing `bgN-pixels` records.
The game samples the copied characters from live VRAM/CGRAM during capture,
following flips, animation, brightness, mosaic, windows, and raster scroll.
Only Diorama background surfaces receive these presentation additions.

## INI format

Virtual records live beside existing plane overrides in a room's base section:

```ini
[layers:01:01]
bg1-virtual = z:0.35 order:4 alpha:255
bg1-virtual = metatile:23 band:0
bg1-virtual = cells:4,5-12,5 band:2
```

Geometry, metatile mappings, and cell rectangles are separate repeatable
records. Metatile IDs are hexadecimal. Cell coordinates name 16x16 metatile
cells and rectangle endpoints are inclusive. Later cell records win when they
overlap; export writes non-overlapping canonical runs.

The merger owns `bg1`, `bg1hi`, `bg2`, `bg2hi`, `bg1-virtual`, and
`bg2-virtual`, `bgN-pixels`, `bgN-stamp`, and `bgN-map` lines in base action-room sections. Other planes, camera-local
sections, comments, unknown settings, and unrelated rooms are preserved. This
means the downloaded file is the normal game configuration, not a sidecar or
intermediate JSON document.

## Rendering views

**Map editor** reconstructs the complete ROM tilemaps with the game's Mode-1
background order for room-wide painting:

1. BG2 priority 0
2. BG1 priority 0
3. OBJ priority 2 reference
4. BG2 priority 1
5. BG1 priority 1

Virtual classification never changes that order. Band tint and edit outlines
make authored changes visible without pretending they affect ordinary gameplay.
**Native frame** is the exact stable 256x224 BG1/BG2 reference. Camera and
frame controls resolve native parallax, character animation, the `0402`/`0403`
BG2 page cycle, all ten persistent raster presets, mosaic, TM/TS priority, and
colour math. **Play native phases** advances the frame clock at 60 Hz; pausing
or moving the slider produces a deterministic authoring frame.

**Diorama 3D** splits those same camera-local, raster-resolved BG captures into
authored bands and uses the same independent z and painter-order model as the
runtime. Ordinary BG planes expose z, order, alpha, and the Flat/Rake/Bow/
Thickness/Stack/Voxel strategies. The preview uses the runtime's six-row mesh,
skirt, stack direction, copy falloff, and solid-voxel formulas. The depth ladder
lists resolved values and warns when surfaces from different BGs become nearly
co-planar; each hardware BG's intentional low/high pair is excluded.

The map editor can shift or repeat the other BG while inspecting full-room
relationships. Diorama 3D uses the exact native scroll instead. The reference
actor uses resolved OBJ2 geometry/order and is draggable in either view.

## Controls

| input | Map editor | Native frame | Diorama 3D |
|---|---|---|---|
| drag | paint | - | orbit |
| shift/middle drag | pan | - | move native camera |
| Shift-click | select range from start tile | - | - |
| wheel | zoom | - | dolly |
| arrows | pan | move camera | move camera |
| Alt-drag | restore authentic classification | - | - |
| `F` | fit | - | - |
| `R` | - | - | reset orbit |
| Ctrl/Cmd-C / V | copy rectangle / stamp | - | switch to stamp |
| Esc / Ctrl/Cmd-D | clear selection, end stamp | clear selection | clear selection |
| Ctrl/Cmd-Z | undo authoring gesture | undo authoring gesture | undo authoring gesture |

Character phases from the native-frame slider apply to every view. The HUD
reports the resolved video profile, animation phase, BG2 page, raster preset,
and C/JavaScript golden-frame parity result.

Brush modes support all metatile instances, one map cell, or rectangles. Reset
actions delete sparse classification records and are undoable; they do not
rewrite a fake baseline. Geometry controls can be reset independently to their
built-in defaults.

## Rendering data

The ROM exporter links `src/action/action_room_scene.c`, the same immutable
room loader used by the game. Asset inheritance, video profiles, tile-word
masking, common priority, BG attribute merge, animation metadata, page cycling,
raster identity, palette, CHR decode, and the final 8 KiB decompression
workspace therefore have one C authority rather than a JavaScript or tool-only
interpretation. The workspace matters because six ROM raster families leave
selected Mode-2 HDMA bytes untouched and inherit the bytes last staged there.
R4 also exports the 256-byte ROM window immediately after the nominal waveform:
its native routine shifts only the low frame byte, writes only the low byte of
a 16-bit scratch index, and inherits a high byte of one in settled action mode.
The resulting MOSAIC pattern therefore samples adjacent ROM bytes rather than
the clean waveform page. The shared C and JavaScript builders both preserve
that quirk; a separate request flag models R4's flat first visible entry table.
Identical asset blobs are pooled across rooms to keep the generated HTML
compact.

The exporter renders a fixed C golden frame for every room and terrain profile.
Opening a room in the editor hashes the JavaScript compositor at that same camera/frame and
reports whether it matches, guarding the self-contained port against drift.
The additive schema-v4 `rasterEntryCameraX` field records the natural bootstrap
camera used by the first visible R6/R9 table where it differs from the settled
room camera. The editor deliberately previews the settled authoring state;
this metadata lets the game-side acceptance oracle reproduce the transient.
Visible frame N normally uses the persistent table built at action tick N-1;
the editor advances that clock deterministically. The game may retain an entire
table during hit-stop, which is action-update cadence rather than a different
raster formula and does not require gameplay simulation in the editor.

### Current parity boundary

The shared C authority and editor cover the stable two-background Mode-1 room:
assets, camera/parallax, priority/transparency/flips, animation/page cycling,
R1-R10 raster state, mosaic, TM/TS, stable screen-window masks, and supported
colour math. BG3 HUD, real OBJ streams, fades, and gameplay-object-driven
window timelines are deliberately outside the standalone room contract. Use
the reference actor for ordering work; use the game-side differential observer
when validating a transient gameplay moment.

Virtual-band records and the ordinary action-background planes (`bg1`,
`bg1hi`, `bg2`, `bg2hi`) are fully authorable here. OBJ, BG3, and backdrop
records are loaded for the relationships the preview can represent and remain
preserved; they are not background-tile authoring targets. The in-game debug
editor is therefore optional for diagnosis, while this standalone editor owns
the action-background configuration consumed by the game.

## Source layout and checks

`build.sh` compiles/runs the shared ROM exporter; `build.py` bundles the result.
`editor.head.html` owns styles and `editor.body.html` owns markup and script order.
The browser code is authored in ordinary JavaScript files, checked by the local
`make check-quality` gate:

- `native_frame.js`: room decoding, raster state and native-frame parity.
- `layer_editor.js`: sparse classification/INI model and the 2D map view.
- `regional_editor.js`: terrain identity, shared authoring families, and regional keys.
- `painting.js`: undo/redo and brush gestures.
- `pixel_editor.js`: partial-cell black masks and magnified pixel painting.
- `stamp_editor.js`: rectangular clipboard, signed scenery placement, and edge bounds.
- `editor_feedback.js`: selection actions, applied-state inspection, edit review and export savepoints.
- `tile_menu.js`: context actions, paste destination, and keyboard menu navigation.
- `diorama_view.js`: WebGL preview and orbit interaction.
- `editor.js`: reference actor, UI controls and startup.
- `help.js`: the embedded help content.

These are ordered classic scripts with shared bindings. The builder embeds them
verbatim so the exported editor still opens offline as one file. The Python
bundle tests check script order, missing optional INI input and lossless data
embedding, including text containing HTML script delimiters.

`python3 tests/action_editor_build_test.py` also checks regional switching,
shared INI export, selection, pixel undo/round trips, render-cache identity,
and cross-room undo without a ROM.
After building with a local ROM, check all 147 C/JavaScript reference frames
and a single-pixel transparency edit in Kassandora Room 4:

```sh
node tests/action_editor_terrain.test.mjs build/action-editor/ar-action-layer-editor.html
```

`make check-quality` checks these classic scripts in their shared browser scope.
`editor.body.html` is the load-order manifest for both the builder and ESLint;
diagnostics point back to the individual source file and line. New script files
must appear in that manifest.
