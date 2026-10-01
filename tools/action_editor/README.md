# Action-mode layer editor

This standalone editor classifies action-room background tiles into virtual
depth bands and authors those changes directly in `diorama-layers.ini`. The
editor is the source of truth; the game only loads and renders the exported
configuration.

The [Effects workspace and shared WASM preview](../../docs/action-effects-editor-plan.md)
is being implemented in stages. **Shared renderer** now
loads complete rooms directly into the production C PPU and Diorama compositor
via WASM/WebGL2. No game capture or gameplay session is required. The original
JavaScript **Diorama 3D** remains available while the shared view is validated.
**Original game frame** remains a separate original-background comparison.

Select **Shared renderer**, choose a room/terrain, then use **Camera & animation**
to navigate and seek frames (0–65535) in either direction. Drag the preview or use
arrows to pan. Its view bar controls 4:3/16:9/16:10 output, square/CRT pixel aspect,
zoom/tilt, horizontal coverage, 0/32/64 vertical budget, and all three backdrop
modes. Camera/clock/coverage changes regenerate visible art from the complete
room; orbit-only changes reuse rasterized surfaces. Existing tile, pixel, depth,
alpha, scenery and framing edits feed the production INI resolver.

This increment covers **scenery and environmental sources**, including animated tiles/raster phases, priority
bands, copied edge tiles, independent vertical clips, bounded and named skyboxes,
and Aitos's periodic waterfall page. The native forest, cave, marsh, castle and
Aitos environmental kernels, exposure and camera-local waterfall sections also
run in this view. Clock-based accent events and optional player/enemy reference
silhouettes are available. Actual actor artwork/HUD, gameplay-driven transitions, final
CRT/heat and frame generation remain pending. The preview camera uses auto-fit
plus its own controls; it does not replay the game's reactive camera. Captures
remain developer comparison fixtures, not an authoring requirement.

The dedicated whole-room gate uses a built editor containing local assets:

```sh
python3 tools/action_editor/check_room.py --sanitize
```

It compares 882 full-room scanouts across all 49 rooms × 3 regional terrains
between native C and WASM, including backwards seeking, varying camera/coverage,
authored configuration, finite skybox views and animated periodic pages. It also
checks pixel-mask and band edits, per-layer top clipping, atomic rejection,
resource reuse and complete teardown. The check also compares resolved native
effect sources and 147 authored-effect round trips. The browser GPU view still needs matched
live-scene image review across the remaining room families and target platforms.

### Environmental authoring

Open **Environmental sources** in Shared renderer. Select a source to change
its enabled state, tint and intensity. **Light reach** adjusts a wall torch's
spill radius independently of its flame, ember positions and peak brightness.
Compound sources such as canopy light or gallery windows expose the whole group.
Use placed fans, contours and particle fields for additional authored structures;
these do not move individual members of an existing native compound group.

**Add at view centre** places any of the following families:

| Family | Controls and behavior |
| --- | --- |
| Soft light / halo | Area, tint, intensity and independent light receivers; halo is a color/size preset. |
| Motes / particle region | Count or density, radius, motion, seed, age tint and depth. Region patterns include dust, leaves, snow, sand, insects, scarabs and sparks. |
| Free mist / ground mist | Airborne soft volume or collision-supported floor slices. |
| Light fan | Opening width, length, direction, fan angle, 1–32 strands and a soft backdrop. Zero degrees points down; the origin is the opening's center. |
| Water surface | Animated crests and glints, amplitude, phase offset, cycle, particle radii and age tint. This is surface geometry, not distortion of submerged artwork. |
| Drips / waterfall spray | Analytic falling drops or a splash arc, area, count, size, cycle, seed, tint and depth. Place spray at the native splash footprint. |
| Cloud bank | Layered soft lobes with horizontal drift and vertical billow. This suggests volume; it is not a 3D volumetric ray marcher. |
| Dimming region | A black alpha wash behind actors, plus separately selected player/enemy dimming. Native cave/castle dimming also exposes receiver choices. |
| Wet rock contour | Up to 16 local path points. **Trace wet contour** draws a bounded stroke following a verified visible edge. |

The palette also provides temple dust, falling leaves, snow, blowing sand,
insects, scarabs, lava embers and halo presets for later-stage authoring. They
reuse the shared kernels; this does not install new art across those stages.

The inspector edits its playfield coordinates, width/height, tint, intensity
and animation cycle. Motes additionally expose particle count, radius range,
travel per cycle, horizontal sway/spread, a pattern seed and optional end tint.
All distances are in game pixels. Negative travel Y rises, positive falls, zero
retains each mote's starting height; a blank value rises by the area height.
Spread 0 emits from the centre and 1 uses the full width. **New pattern** changes
the seed without changing source identity. Particle positions/colors are analytic,
so pause and backward time seeking reproduce the same frame. **Duplicate
emitter** copies its settings with a new stable identity and a 16-pixel offset.
Changing field dimensions does not enlarge individual motes. Free mist is
explicitly airborne. Existing native floor mist remains collision-supported and
can be tinted or dimmed.

**Draw particle region** creates one record for a large rectangle, up to
16384 × 16384 game pixels. It does not require placing point emitters throughout
the room. Density is particles per 256 × 256 world cell (1–16, default 4), with
at most 64 nearby cells generated per view. Motion envelopes determine which
birth cells need evaluation. Cell seeds stay fixed while panning or seeking;
offscreen cells do not consume the native actor/decoration pools. Increase
density and radius independently, or change the pattern/age tint in the inspector.

Lights expose **Override light receivers**, and exposure/cave/castle sources
expose **Override dimming receivers**. Select scenery, player and enemies
independently. Clearing an override restores inherited behavior. Explicit light
receivers move scenery illumination under actors and sample selected objects at
their native hot point; every OAM part and extended-apron part receives the same
object tint. This is not per-pixel shadow tracing. Native ownership identifies
players and attacking enemy objects without guessing from artwork. Unowned
decorations, pickups and other objects keep their original color.

Authored dimming regions apply a BG1-plane wash; higher priority scenery is
painted afterward. Native cave/castle dimming retains its existing BG1 coverage
and distance ramp. Independent object dimming uses the same region or ramp.
Receiver effects require Environmental effects; moving accent lights also obey
Action lighting, and their trails retain Action particles ownership.

**Receiver & event preview** offers movable blue player/orange enemy reference
silhouettes for testing these choices. **Actor accent** selects one of 19
recognized families, with event position, velocity, start frame, duration and
seed. **Start in current view** positions it and starts its clock. Seek or play
to inspect one representative native flight, charge, strike or landing phase.
It does not run enemy AI, spawn game actors, or show their sprite artwork.
Selecting the resulting source exposes enable/tint/intensity and, where present,
light receiver controls. Actor source ID zero targets that family throughout
the selected room/terrain, including future native generations. Event/probe
positions are temporary; source edits persist in the effects document.

**Place on map** switches to BG1 and places the chosen preset at a click.
**Edit emitters on map** picks authored areas, moves them by dragging their
centre/body and resizes the selected area with corner handles. Hold Alt to snap
to 16 pixels. Dragging is a draft until release and creates one undo entry;
Esc, losing focus or switching room/terrain cancels it. **Show emitter areas on
map** controls their guides. Native compound groups remain in the source picker;
these handles do not guess positions for their individual members.

**Paint ground mist** switches to BG1's map and enables the shared collision
overlay. Drag a rectangle around the floors you want to cover (up to 512 × 512
pixels). Each column searches downward for its first supported floor; spikes
and non-solid background rocks do not catch the mist. Its **Mist height** (4–64
pixels, default 26) controls height above the floor, independently of the search
area. Solid ceilings limit available headroom. **Preview selected area** moves
the shared camera there and reports the number of supported spans, or explains
when no floor was found. **Erase mist areas** removes intersecting authored
regions in one undoable operation; it leaves native sources alone. Select or Esc
leaves the brush. Middle-drag pans and the wheel zooms.

The overlay shows solid cells in green, partial collision in amber and supported
top edges in cyan. It uses original gameplay collision from the same C resolver
as the game; scenery stamps and pixel edits do not create collision. This first
resolver supports flat solid surfaces and verified Fillmore temple capitals.
Other partial/sloped shapes are shown but are not treated as flat supports.
Mist splits at gaps and steps instead of bridging them, renders behind actors,
and clips before projection. Region outlines appear only on BG1's map.

Edits use the normal Undo/Redo controls. **Review / paste effects INI** provides
a copyable document and an atomic Apply operation with line diagnostics.
**Load effects INI** and **Export effects INI** use the same document. Save the
export as `action-effects.ini` beside the game's `settings.ini`, then restart
the game. It is independent of `diorama-layers.ini`; export both when sharing
scenery and effects. The editor build embeds an effects file found beside its
input layers file. A missing effects file preserves the approved native defaults.

The shared parser supports schema version 1, sparse source overrides and twelve
emitter kinds. Scope uses hexadecimal map group/room, decimal terrain profile
(0 US, 1 JP, 2 EU), a named kind and hexadecimal stable source identity:

```ini
[effects]
version=1

; Obtain native source IDs from the source inspector.
[source:02:03:0:wall-torch:54000101]
reach=2.5
intensity=0.75
color=ffe6bb

[emitter:01:02:0:motes:00000123]
x=1280
y=1056
width=96
height=64
particles=48
lifetime=300
intensity=1
color=ffe6bb
size-min=0.65
size-max=1.25
travel-x=60
travel-y=-96
wander=5
spread=0.6
seed=42
color-end=91bedf

[emitter:01:02:0:ground-mist:00000124]
x=1280
y=1168
width=480
height=160
mist-height=26
color=91bedf

[emitter:01:02:0:particle-area:00000125]
x=1024
y=1136
width=2048
height=256
particles=8
pattern=motes
placement=playfield
color=ffe6bb
travel-y=-96

[source:01:02:0:cave-light:c200021f]
light-scenery=1
light-player=0
light-enemies=1
dim-scenery=1
dim-player=1
dim-enemies=1
```

`enabled` is 0/1; intensity is 0–4; torch reach is 0.25–4. Emitter positions
are integer pixels (−2048 to 16384), width/height 4–512 (up to 16384 for particle
and dimming regions), mote count 1–128 (region density 1–16),
and lifetime 16–4096 frames. Defaults are enabled, intensity/reach 1, white tint,
position 0,0, area 96×64, 24 motes and a 240-frame cycle. Unknown versions,
properties, kinds, duplicate records and non-finite values are rejected.
Optional mote fields preserve those defaults when absent: `size-min`/`size-max`
0.35/0.75 (allowed 0.1–4, minimum ≤ maximum), `travel-x` 0, `travel-y` −height
(allowed −512 to 512), `wander` 2 (0–32), `spread` 1 (0–1), `seed` the source ID
(decimal 0–4294967295), and `color-end` the primary tint (six hexadecimal digits).
Drifting motes are culled by their motion envelope, independently of their birth
area. These controls keep the four-vertex particle cost and existing pass batching.
Source changes are sparse; removing a source record restores native defaults.
Actor/projectile accents retain their existing settings and native bindings.
Overrides are terrain-specific even where scenery edits share
equivalent US/EU layouts.

Each room/terrain permits 16 enabled emitters and at most 4096 added vertices;
the file holds at most 256 records/128 KiB. Conservative costs are 97 vertices
per light, four per mote and 296 per free-mist volume. Ground mist reserves
`(ceil(width / 16) + 1) × 45` vertices for the worst support fragmentation,
independent of the current camera. Each span uses three density slices and at
most 144 indices. Over-budget documents are
rejected as a whole. The separate authored list cannot consume the 16 native
actor accents or 27 native decoration slots. Geometry shares retained pass
batches; emitters create no textures or per-frame heap allocations. The status
line reports actual submitted geometry/draws/uploads/allocations and CPU time,
which is not a GPU timing or Steam Deck/D3D12 performance measurement. New family
costs are bounded at export too: regions reserve 64 cells, fans reserve clipped
triangles per strand, clouds reserve 37 vertices per lobe and contours reserve
14 per segment. Smaller actual visible costs appear in the status line.
An additional development-machine CPU stress fixture (three eight-strand fans,
two retained receiver batches, 16 object samples) averaged 0.221 ms per frame
over 1000 iterations. Reproduce with
`build/actraiser_action_authored_fields_test --benchmark`; it excludes GPU work,
native scene observation, PPU tinting and end-to-end frame time.

Validation includes native pause-clock ownership, retained frames, malformed
imports, budgets, unchanged torch flame/embers when reach changes, and native
presentation of authored-only scenes. Browser smoke tests cover import, invalid
version rejection, undo/redo and effect geometry. Download completion through
the automation browser remains unverified; the copyable document is available.

Still pending: automatic contour/material recognition, sloped mist supports,
individual native compound-member handles, a combined scenery/effects project,
native reload without restarting, edited-silhouette occlusion, actual actor/HUD
art, final image parity and target-hardware acceptance. Water refraction/heat
and full volumetric scattering remain separate experiments.

#### Text versus binary storage

Keep `action-effects.ini` as the canonical effects sidecar beside `settings.ini`.
It stores small semantic recipes, not rendered vertices or particle states.
Load/import parses it once into bounded retained records; rendering never parses
text. A full 256-record receiver-heavy fixture was 41,492 text bytes and 63,492
retained bytes, averaging 0.729 ms per parse over 500 runs on the development
machine. Reproduce with `build/actraiser_action_effect_recipes_test --benchmark`.
These are CPU measurements, not target-device load benchmarks.

Text remains readable, reviewable in Git and tolerant of compiler/architecture
changes. A raw dump of C structs would encode padding, enum sizes, endianness and
pointers, and would be unsafe as a portable project format. A binary effects
cache would not improve per-frame rendering or reduce retained table size.

`diorama-layers.ini` currently contains about 631 KiB, mostly dense scenery edits;
that is a different workload from effects recipes. If larger pixel masks,
painted density grids, meshes or imported artwork make this data unwieldy, keep
semantic settings in text and add a versioned binary asset payload with stable
IDs, explicit little-endian lengths/counts, bounds validation and a content hash.
Both pieces would need atomic export/import and a documented migration. An
optional compiled cache should be disposable and keyed by text/schema hash.
There is no measured need to introduce that format for today's effects.

```sh
sh tools/action_editor/build.sh
sh tools/action_editor/build.sh ar.sfc out.html path/to/diorama-layers.ini
```

The build needs a C compiler and Python 3. Its output is a self-contained HTML
file that can open from `file://`. The default output is
`build/action-editor/ar-action-layer-editor.html`, keeping the generated editor
out of the source tree and Git. By default it embeds the repository ROM and
`diorama-layers.ini`; **Load INI** can replace the configuration at runtime and
**Export level INI** opens a copyable replacement section for the current room.
The dialog also offers **Download full INI…** for a complete merged file.
The build also reads `settings.ini` for the Diorama coverage guide's aspect,
pixel aspect, distance and camera tilt. An optional fourth build argument selects
another settings file. These preferences are embedded for offline use; the editor
does not write game settings.

### Shared original-background comparison

If Emscripten **5.0.7** is installed, the build embeds the WASM module directly in
the HTML. Opening the editor requires no compiler, server, adjacent files, or
network access. `ACTION_EDITOR_WASM=on` requires this module;
`ACTION_EDITOR_WASM=off` builds the existing JavaScript-only editor. The default
`auto` builds it when `emcc` is available; a different compiler version reports
an error instead of silently changing the comparison toolchain. `EMCC` selects
the compiler and `EM_CACHE` can override the workspace-local build cache.

**Original game frame** uses shared C for original BG1/BG2 decoding, regional
terrain, animation, raster scrolling, mosaic and color math. Room, camera and
frame controls drive it. **Band tint** intentionally selects the JavaScript
diagnostic view. A status line identifies the active path or a module failure;
ordinary map tools continue to work without WASM.

**Download comparison scene** saves the current original-background assets,
terrain, camera and clock as a versioned `.arscene` fixture. It excludes tile
edits, actors, effects, Diorama settings and final post-processing. It is a
renderer comparison fixture, not a level project or game save. Exporting one
does not clear unsaved tile edits. The native replay tool needs no ROM:

```sh
cmake --build build --target actraiser_action_scene_replay
build/actraiser_action_scene_replay scene.arscene
build/actraiser_action_scene_replay --ppm frame.ppm scene.arscene
```

Run the shared-renderer gate separately from ordinary editor tests (requires
the pinned Emscripten compiler, C compiler, Python and Node):

```sh
python3 tools/action_editor/check_preview.py          # synthetic, no ROM required
python3 tools/action_editor/check_preview.py ar.sfc   # 49 rooms x 3 terrain variants
```

The larger gate checks 1,176 frames, native export goldens, RGBA upload conversion,
clock boundaries, phase overrides, invalid input, reset/reload and fixed memory
across repeated room changes. See [the snapshot contract](scene-snapshot.md) for
format and comparison scope. Passing this gate does not establish enhanced
Diorama, browser GPU, Steam Deck or D3D12 parity.

### Shared Diorama compositor validation

The production compositor now receives explicit `DioramaRenderOptions`, captured
planes, camera/view and scene callbacks. Desktop controls/settings are isolated
in `diorama_controls.c`; the compositor has no live settings, manifest, WRAM,
environment or host-clock reads. The desktop build uses this same entry point.

Run its separate **command/projection** gate without a ROM:

```sh
python3 tools/action_editor/check_compositor.py
python3 tools/action_editor/check_compositor.py --sanitize
cmake --build build --target actraiser_diorama_compositor_test
ctest --test-dir build -R '^actraiser_diorama_compositor$' --output-on-failure
```

`EMCC` selects pinned Emscripten 5.0.7; `CC` selects the native C compiler. The
native and WASM harnesses share `compositor_sources.txt`. Their recording backend
validates draw/resource ownership, geometry, shader parameters and projection
across 108 aspect/zoom/extension/skybox configurations. It also checks bounded
scratch-target reuse/eviction, repeated reset, invalid input and failed draw/target
restoration. WASM runs in fixed 32 MiB memory. Only failure diagnostics use WASI;
unexpected host imports fail the test. Native/WASM float agreement allows 0.001
output pixels for math-library rounding (observed maximum below 0.000184).

This command-recording harness does **not** draw pixels. The UI still
uses the rendering modes described above, and `.arscene` remains the original
background-only snapshot format. The separate captured-scene viewer below now
covers base-compositor pixels; matched live-scene image review and environmental
effects remain gates before replacing Diorama 3D.

### Captured scenes through WebGL2

WASM-enabled `build.sh` also builds `ar-renderer-preview.html` and adds a link near
the top of the editor. Both HTML documents embed their modules and need no runtime
network dependencies. The captured viewer runs the real C compositor through a
WebGL2 `ArRenderDevice` adapter, with generated production blur/rim/DOF/priority
shaders. It supports output aspect/size, camera distance/tilt, skybox off/only/both,
and PNG export. A capture keeps its original extension rows and horizontal art;
resizing does not regenerate world pixels beyond that capture.

The viewer is deliberately separate from live editing. It includes native
captured actors/priority art but does not yet render environmental effects,
a flat HUD or CRT processing. It cannot apply unsaved map edits. See the
[compositor snapshot contract](compositor-snapshot.md) for supported inputs.

```sh
# Build the browser compositor alone (pinned Emscripten 5.0.7).
python3 tools/action_editor/build_compositor.py build/action-editor/ar-renderer-preview.html

# Capture a real scene with isolated copies of the seed/settings/input replay.
python3 tools/action_editor/capture_compositor.py --room 0101 --output runs/compositor-fillmore

# Optionally embed a small compressed example into the offline HTML.
python3 tools/action_editor/build_compositor.py build/action-editor/ar-renderer-preview.html \
  --scene runs/compositor-fillmore/scene.ardi

# Replay precisely those pixels/settings using the native GPU backend.
cmake --build build --target actraiser_diorama_replay actraiser_diorama_snapshot_test
build/actraiser_diorama_replay runs/compositor-fillmore/scene.ardi runs/compositor-fillmore/native.bmp

# Export the browser frame using its Export frame / Download PNG controls.
# Requires Pillow; allows tiny edge-rasterization differences across backends.
python3 tools/action_editor/compare_compositor.py runs/compositor-fillmore/native.bmp browser.png

# ABI, resource reuse, shader fallback and malformed-load checks with actual captures.
node tests/diorama_captured_wasm.test.mjs build/action-editor/ar-renderer-preview.wasm \
  runs/compositor-fillmore/scene.ardi
python3 tests/action_editor_compositor_build_test.py
```

The native capture environment controls are `AR_DIORAMA_SNAPSHOT=/path/scene.ardi`
and optional `AR_DIORAMA_SNAPSHOT_AFTER=120` (uploaded Diorama frames). Disable
frame generation. The diagnostic copies only once and frees its temporary pixels
after writing. Some interior rooms require a natural-entry seed/replay; the helper
accepts `--seed`, `--replay`, `--warp-at`, `--diorama-at` and `--quit-frames`.
For the existing Aitos waterfall route use `saves/legacy/aitos-save.srm`,
`saves/legacy/aitos-waterfall.rec`, warp 1320, Diorama 1700 and quit 3000.

Verified locally: native Metal/WebGL2 pixels in Fillmore, Bloodpool and Aitos,
changed Bloodpool aspect/camera/skybox policy, all four shader programs, and no
new texture allocations on repeated frames. These are base-composition fixtures,
not a claim of complete environmental parity or native Deck/D3D12 validation.
The compositor's scratch cache belongs to one render device per module; call
`Diorama_ResetCompositorResources` against that device before destroying it or
binding a replacement. Native `Diorama_ResetRendererResources` also resets its
desktop upload and named-skybox caches.

The repository INI is also the release's authored scenery source. Packaging
ships it as `defaults/diorama-layers.ini`; the first game launch after those
bundled bytes change backs up and replaces the installed live copy, including
corrections to existing rooms and removed tile patches. Backups are named
`diorama-layers.ini.pre-update-N`, and `diorama-layers.ini.installed` tracks the
last applied release. Local edits survive ordinary launches and identical
reinstalls. A source checkout without `defaults/` uses the edited INI directly.

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
The saved limits (256 pixel records and 512 classification spans per BG)
count distinct records across all terrain families, with shared records counted
once. Pasted tile storage grows as needed, without a fixed count budget.
Copy/stamp stays within the same room, background and terrain family.

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

**Tiles…** opens a palette of all 256 loaded 16×16 metatile definitions,
including artwork absent from the original scene. Gold dots mark unused
definitions; **Unused in source** and **Used in source** filter by the original
source map. Click a tile, then click the map to stamp it. Selection and stamp
preparation make no edits until placement. Thumbnails retain original colors
even with band tint enabled, and checkerboard identifies transparency.
Hover a tile or focus it with the keyboard for a crisp 128×128 preview beside
the palette, including its ID and source usage. This also works for 8×8 pieces.

**Artwork source** includes other maps when their CHR banks, extra graphics,
color palette and animation descriptor match the destination. Their metatile
arrangements can then be borrowed without changing the game's loaded assets.
Maps with different graphics require an asset-import feature and are excluded.
The current terrain variant supplies the palette's definitions. Placed tiles
follow the destination background's existing regional sharing rules.

Switch to **8×8 pieces** to assemble a 16×16 tile from four loaded characters.
Choose a palette row, click the desired quarter, and choose a piece. The next
quarter becomes active automatically. **Use assembled tile** starts stamping.
Choosing a 16×16 tile seeds the composer so individual quarters can be replaced.
The current stamp preview also shows copied rectangles. **Mirror stamp H/V**
reflects that stamp before placement, without modifying existing scenery.

**Flip H/V** flip each selected tile in place, keeping its position.
**Mirror range H/V** reverse tile order within a complete selected rectangle
and flip each tile's artwork on the same axis. For example, **Mirror range H**
turns `A, B, C` into horizontally flipped `C, B, A`; **Mirror range V** reverses
rows and flips each tile vertically. Both actions are also in the right-click
menu. Mirroring requires a complete rectangle; scattered selections support
individual flips. **Mirror stamp H/V** perform the same order-and-artwork
reflection on a copied rectangle before you place it.
Mirroring swaps 8×8 quadrants, toggles native horizontal/vertical flags, and
moves black pixel edits and depth bands with the artwork. Copy, paste, export
and reload preserve these flags. One Undo reverses an entire selection flip.
The resulting edits use ordinary stamps without a fixed tile-count limit.
Black masks retain the shared limit of 256 pixel records per BG. Capacity checks
happen before mutation, so a rejected operation leaves the scene unchanged.

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

Actions apply immediately in the preview. **Export level INI** opens a modal
with the current room's complete `[layers:GG:MM]` section: both BGs, all terrain
variants, and existing settings and comments from the loaded section. Choose
**Copy section**, replace that section in the game's `diorama-layers.ini`, save,
and restart. Replace old tile lines too; appending a patch cannot remove old
edits. Leave camera-specific sections in place. If the base section does not
exist, append the copied block. Repeated base sections are consolidated in the
copied output and should be replaced with that single block.

Opening or closing the modal does not mark edits exported. Successful copying
updates only that room's savepoint; other rooms keep **Unexported changes**.
When every room matches its loaded, copied or downloaded state, the badge shows
**Matches last copy** or **Matches last export**. Undo/Redo updates the badge.
**Select all** and Ctrl/Cmd-C work when browser clipboard access is blocked.
**Download full INI…** saves all rooms. Export before refreshing the editor;
use **Load INI** with the updated file afterward to resume your work.

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
4. Check **Diorama 3D**, then **Export level INI → Copy section** and replace
   this room's section in `diorama-layers.ini`. Save and restart the game.

**Paint transparent** cuts holes in opaque artwork or black pixel edits. Drag
across the grid for a continuous stroke; checkerboard shows the transparent
result. **Paint black** makes a pixel opaque again. **Restore pixel** clears
either edit and reveals the source artwork. Copies, flips and range mirroring
preserve the transparent mask.

**Fill transparent pixels black** fills colour-zero pixels and painted holes of the selected
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
saved by Export level INI. If the whole operation would exceed the 256 pixel-record
limit per BG, the editor reports it and changes no pixels.

Pixel edits are black/transparent presentation masks, not changes to ROM CHR or
palette data. The normal game and **Native frame** remain authentic. Map editor
shows the edits for authoring; Diorama captures route them through the same BG,
camera, live raster/mosaic, windows and depth bands as their source tiles.

Each repeatable `bgN-pixels` line names `cell:x,y` or `metatile:HH`, followed by
`black:` and 64 hexadecimal digits: sixteen rows of sixteen mask bits, with the
highest bit on the left. An optional `transparent:` mask uses the same layout
to remove source pixels. Black and transparent bits cannot overlap. Either
mask may be supplied alone; the editor includes black zeros for transparent-only
edits, and old black-only records keep their meaning. Both masks share one
pixel record and the same cell/metatile scope. A cell's explicit zero mask restores original pixels
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
4. Check **Diorama 3D**, then **Export level INI → Copy section**. Replace this
   room's section in `diorama-layers.ini`, save, and restart the game.

Copies stay attached to their original room, BG and terrain family so their
graphics bank, scroll, palette and layout remain valid. An overlapping paste
uses the original clipboard snapshot. Matching regional terrain shares that
clipboard; a different layout requires copying its own source art. Characters
still follow the selected artwork and animation. Paste overwrites the
destination's presentation art, including
transparent pixels. It does not alter gameplay maps or collision.

The right-click **Delete tile / Delete selected tiles** action clears artwork
from the selection in one Undo step. Tiles inside the original map become
transparent replacements; added edge tiles are removed so saved bounds shrink.
Gameplay collision and the native game view remain unchanged. Already empty
edge space stays empty without creating saved records.

Right-click **Reset tile to default / Reset selected tiles to default** to
restore the selected cells' authentic regional artwork, flip flags, priority
bands and pixels. Added cells without an original are removed, including their
pixel masks, so saved scenery bounds can shrink. The whole reset is one Undo
step. Unselected tiles, framing and authoring edge space are preserved.
If a metatile-wide rule still affects unselected cells, reset writes local
exceptions for the selection; these remain authored records in the highlights.
Selecting every original instance removes the shared rule instead. Reset is
independent of the pixel inspector's scope and applies to the selected tiles.
Export the room section after resetting to remove old edits from the game INI.

**Remove selected pasted tiles** restores the underlying ROM scenery;
**Reset this BG's pasted scenery** also removes added edge space. Paste,
removal, edge expansion, bands, and pixel painting share Undo/Redo. Pasted cells
can be selected and painted black individually in the pixel editor.

Saved bounds automatically fit the native map plus the remaining pasted tiles.
Deleting a bottom extension restores the native bottom edge and lets Diorama
anchor there again. Empty **Add edge space** remains available while editing,
but is omitted from export and does not reserve space in the game. The game
also ignores stale empty bounds in older INI files when calculating its edges.

Empty edge cells are selectable, including Shift-click ranges and right-click
actions. **Fill transparency black** creates solid black tiles in those cells;
**Pixels…** lets you paint only part of a blank cell. Copy/paste can also include
blank cells. Selecting empty space does not save tiles until you edit or paste
them. Newly painted blanks use transparent source art (`words:blank`), so
restoring a pixel reveals transparency instead of an unrelated ROM character.
The same tile and pixel-edit limits apply, with an atomic capacity check.

### Diorama coverage and missing edge tiles

**Coverage** shows a tile-aligned minimum envelope for the selected BG. Red
shading shows where saved scenery falls short; green means its outer bounds
cover the current baseline. **Diorama map coverage** reports the exact extra
columns needed on the left/right and rows above/below. **Fit** and **Fit coverage**
include that envelope. **Add required edge space** grows the workspace in one
Undo step; stamp or paint those empty cells afterward. Workspace alone never
counts as saved scenery, and this check does not prove that all interior pixels
are opaque. Added transparent areas may be intentional.

The **Tested minimum** calibration is `[layers:02:08]` / Bloodpool Act 2 room 8:
`bounds:-8,-2,24,18`, or **32×20 tiles / 512×320 pixels**. At 16:10, square
pixels, and Dynamic Cam baseline distance 3.25, this is the author's observed
minimum at native display resolution. The original map is 16×16 tiles; it needs
eight columns on each side and two rows above/below. This reference stays fixed
even when editing or trimming that room later.

The guide inverse-projects the game's 0.4-radian camera frustum onto each used
BG depth. It uses the runtime's camera rotation order, includes shaped-row and
stack depth ranges, and scales the envelope by the measured reference factor
(about 1.084). It rounds outward to complete 16px cells. **Projection only**
omits the calibration and shows the smaller geometric estimate. Aspect, pixel
aspect, camera distance, tilt, selected BG depth, preview scroll and saved
framing update the counts. Small rooms keep their native vertical center;
taller rooms anchor at finite vertical scroll edges. Resolution at a fixed
aspect ratio does not change the source footprint.

The build seeds controls from `settings.ini`, using Dynamic Cam's baseline
instead of the inactive Free Cam pose. Controls and visibility are preview-only
and do not export game settings. This is a baseline extent guide; free orbit,
reactive camera movement, raster scroll effects and transparent holes can reveal
more than the displayed area. Review other preview scroll positions for large
scrolling levels. Other BGs can require more area because they sit farther away.

### Saved framing for small rooms

Click **Frame** above the map (or **Adjust frame on map** in the sidebar) to
focus a draggable native 256×224 viewport. Dashed white marks the unshifted
view at the current preview scroll; gold marks the adjusted view. Drag the
gold rectangle to set the saved offsets, or use arrow keys for 1px nudges and
Shift + arrows for 16px. Each drag is one Undo step; Esc cancels the current
drag and returns to Select. The guide uses BG1 coordinates; the offset moves
the whole scene. **Show viewport guides on BG1** toggles the overlay. Switch to
**Diorama 3D** to check the result with depth and perspective.

The **16:9** (cyan) and **16:10** (purple) guides show wider coverage around
the same center and move with the native frame. Toggle either independently;
you can drag inside any visible viewport with the Frame tool. **Pixel aspect**
should match the game's setting: CRT (4:3) covers 342×224 and 308×224 map pixels,
while Square pixels covers 400×224 and 360×224, respectively. These widths use
the game's rounding to equal whole-pixel margins. **Guide coverage → Diorama
estimate** uses the game's 0.4-radian field of view, the BG1 plane depth, and
**Camera distance** to show the wider projected footprint. Match the distance
and pixel aspect from your game settings. At distance 3.25 with square pixels
and BG1 depth 0.50, 16:10 covers roughly 473×296 map pixels. This estimate assumes
zero tilt; dynamic floor alignment can shift its vertical position. Resolution
alone does not change coverage at the same aspect ratio.
Guide visibility, coverage, distance and pixel aspect are preview options; export still saves one
shared framing offset for the room's terrain family.

Use **Saved Diorama framing → View X / View Y** to adjust the view relative to
the existing scroll anchor. Negative X looks left; negative Y looks up. These
offsets default to zero and do not change when you extend or trim the map. New
graphics grow outward around the original room. In Diorama, added horizontal
scenery supplies widescreen edge padding so the camera can travel farther
toward that side within its original gameplay range. Empty workspace does not
change the stop. The native world dimensions and vertical gameplay camera stay
unchanged; saved framing is applied after automatic presentation alignment.

Keep Y at zero to retain the normal floor anchoring, and adjust X to balance a
small boss arena. Each control allows −64 to +64 native pixels; a tile is 16px.
The game captures nearby native and added artwork in extra horizontal columns
when an X offset is set or that BG has pasted scenery. Vertical offsets pan within the available capture;
large adjustments can expose an edge. **Reset framing** restores the default.
Framing changes use Undo/Redo and appear in **Export level INI**. Matching BG1
terrain shares the setting; different regional layouts can be tuned separately.
The existing **Native frame controls → Camera X / Y** only move the preview and
are not exported.

**Scenery & edge space → Trim unused edge space** removes unused authoring
margins while preserving the original map and all added tiles. If no tiles
remain outside the native map, the workspace returns to its original bounds.
It supports Undo/Redo and leaves saved framing and tile edits intact.

```ini
framing:us+jp+eu = x:-40 y:0
```

Pasted cells and copied rectangles have no fixed tile-count limit. Bounds and
rectangles can span 512 tiles on either axis, within signed coordinates −512
through 511. Copies carrying black masks still use the shared 256 pixel-record
budget; a paste exceeding those limits leaves the map unchanged and reports why.

The export uses frozen displayed SNES character words and one band per 8px
quadrant (top-left, top-right, bottom-left, bottom-right):

```ini
bg1-map = bounds:-1,0,32,16
bg1-stamp = cell:-1,0 metatile:23 words:0010,4011,8012,E013 bands:0,1,2,1
```

Map bounds have exclusive upper edges. Stamp positions and pixel masks accept
negative cell coordinates. Pixel masks use the existing `bgN-pixels` records.
The game samples the copied characters from live VRAM/CGRAM during capture,
following flips, animation, brightness, mosaic, windows, and raster scroll.
Only Diorama background surfaces receive these presentation additions.

Export already consolidates overlapping pastes by destination (the latest
paste wins), sorts their coordinates, and merges identical records across
terrain variants into combined regional keys. Band edits are emitted as
non-overlapping horizontal runs. Pasted artwork still uses one stamp record per
unique destination cell: the game has no rectangle/repeat stamp syntax.
Export does not turn a local paste into
a metatile-wide replacement or drop frozen art that happens to match the
current source; those changes could affect other instances or later edits.

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
`bg2-virtual`, `bgN-pixels`, `bgN-stamp`, and `bgN-map` lines in base action-room
sections. Room export preserves the current base section's other planes,
comments and unknown settings. Full INI download also preserves camera-local
sections and unrelated rooms. Both outputs use the game's normal INI format.

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
- `export_editor.js`: room-section modal, clipboard fallback and full INI download.
- `framing_editor.js`: saved offsets relative to the room's scroll anchor.
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
