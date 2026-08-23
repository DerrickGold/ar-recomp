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
`bg2-virtual` lines in base action-room sections. Other planes, camera-local
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
| wheel | zoom | - | dolly |
| arrows | pan | move camera | move camera |
| Alt-drag | restore authentic classification | - | - |
| `F` | fit | - | - |
| `R` | - | - | reset orbit |
| Ctrl/Cmd-Z | undo classification gesture | undo classification gesture | undo classification gesture |

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

The exporter renders a fixed C golden frame for every room. Opening a room in
the editor hashes the JavaScript compositor at that same camera/frame and
reports whether it matches, guarding the self-contained port against drift.
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
