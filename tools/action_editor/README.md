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

The merger owns only `bg1-virtual` and `bg2-virtual` lines in base action-room
sections. Existing plane overrides, camera-local sections, comments, unknown
settings, and unrelated rooms are preserved. This means the downloaded file is
the normal game configuration, not a sidecar or intermediate JSON document.

## Rendering views

**Game composite** reconstructs the ROM tilemaps with the game's Mode-1
background order:

1. BG2 priority 0
2. BG1 priority 0
3. OBJ priority 2 reference
4. BG2 priority 1
5. BG1 priority 1

Virtual classification never changes that order. Band tint and edit outlines
make authored changes visible without pretending they affect ordinary gameplay.
The native-frame control selects character animation phases and, for `0402`
and `0403`, the active BG2 page. **Play native phases** advances that clock at
60 Hz; pausing or moving the slider produces a deterministic authoring frame.

**Diorama 3D** rasterizes every band to its own full-level surface and uses the
same independent z and painter-order model as the runtime. Existing z, order,
and alpha plane overrides are read from the loaded INI. The depth ladder lists
resolved values and warns when surfaces from different BGs become nearly
co-planar; each hardware BG's intentional low/high pair is excluded.

The other BG can be shifted or repeated because the two layers often have
different extents and live scroll rates. The reference actor uses resolved
OBJ2 geometry/order and is draggable in either view.

## Controls

| input | Game composite | Diorama 3D |
|---|---|---|
| drag | paint | orbit |
| shift/middle drag | pan | pan |
| wheel | zoom | dolly |
| arrows | pan | walk the level window |
| Alt-drag | restore authentic classification | - |
| `F` | fit | - |
| `R` | - | reset camera |
| Ctrl/Cmd-Z | undo classification gesture | undo classification gesture |

Character phases from the native-frame slider apply to both views. The active
`0402`/`0403` BG2 page is cropped in the flat composite; 3D page framing will
move to the shared scanline compositor. The HUD reports the resolved video
profile, animation phase, BG2 page, and raster preset in either view.

Brush modes support all metatile instances, one map cell, or rectangles. Reset
actions delete sparse classification records and are undoable; they do not
rewrite a fake baseline. Geometry controls can be reset independently to their
built-in defaults.

## Rendering data

The ROM exporter links `src/action/action_room_scene.c`, the same immutable
room loader used by the game. Asset inheritance, video profiles, tile-word
masking, common priority, BG attribute merge, animation metadata, page cycling,
raster identity, palette, and CHR decode therefore have one C authority rather
than a JavaScript or tool-only interpretation. Identical asset blobs are pooled
across rooms to keep the generated HTML compact.

The 3D preview frames a gameplay-sized window and walks it along long levels;
rooms can be up to 4096 pixels wide, and presenting the entire room as one thin
quad would not represent what the player sees.

### Current parity boundary

The shared loader and editor currently agree on assets, map dimensions,
metatile expansion, character-bank attributes, profile common priority,
character animation, page-cycle identity, and raster-preset identity. The
flat view still needs the shared scanline compositor for profile TM/TS,
windowing, mosaic, colour math, camera/parallax, and the ten raster builders.
Until that lands, the editor is suitable for virtual-layer authoring and
priority/phase inspection, but its flat composite is not yet the final
pixel-exact game oracle.
