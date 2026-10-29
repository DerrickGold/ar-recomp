# Action-mode layer editor

### Load and save a project

Use **Save project** above the map (or Ctrl/Cmd-S) to download one `action-project.zip` containing
`diorama-layers.ini` and `action-effects.ini`. It preserves all authored scenery
effects and background policies across every room and regional terrain. **Load project** restores
both together; its picker also accepts the two existing INI files selected at
once. ZIP files saved by the editor need no server, network or external library.
Ordinary compressed ZIP imports use the browser's `DecompressionStream` support.
Both INIs must be at the archive root. Missing, corrupt or invalid effect files
are rejected before replacing the active project.

The **Unsaved changes** badge covers scenery and effects, including Undo/Redo.
Save before refreshing or closing the editor. A save downloads a new file through
the browser; it does not overwrite the original file automatically. You can save
repeatedly, including an unchanged project; each ZIP contains the latest edits. Projects
store authoring data; ROM artwork and temporary preview camera/event controls
remain supplied by the editor. To use a project in the game, extract both INIs
beside `settings.ini` and restart the game.

Unapplied background policy drafts in any room block a project save. The editor
names every affected room/BG and opens a draft in the settings modal.
Choose **Apply policy** to keep that draft or **Discard draft** to retain the
already applied settings, then save again. Other rooms' tile edits stay intact.

The sidebar's **Individual INI files** section retains **Load scenery INI**,
**Export level INI**, **Load effects INI**, **Export effects INI** and the effects
document panel for working with one document at a time. Each individual import
keeps the other document loaded. **Export level INI → Download full INI…**
downloads scenery from every room; **Export effects INI** downloads effects.

### Layout and settings

**Hide sidebar** and **Hide tools** above the map independently collapse the left
panel and the extra toolbar rows. **Show sidebar** / **Show tools** restore them;
view switching, project load/save and **Settings…** remain available. Sidebar
sections are collapsible, with **Collapse sections** / **Expand sections** for all
sections together. Only the room picker is expanded initially.

Right-click the map and choose **Editor settings…** to open a modal containing
every sidebar section. Right-click other previews or the sidebar to choose a
section directly. **Settings…** in the top bar opens the same modal, even with
the sidebar hidden. The dialog uses the same live controls: ordinary edits apply
immediately; Background policy still requires **Apply policy** or **Discard
draft**. **Done** or Escape closes the dialog without discarding settings.
Map tools return to the canvas; effect configuration opens its existing inspector.
The context menus also support arrows, Home/End and Escape. Shift-F10 opens the
menu on a selected map tile, or opens settings from another focused preview.

### Background policy

Death Heim room 1 has separate **1 A — faces** and **1 B — completion scenery**
selections. Each displays only its own 256×256 background pages. Tile edits,
depth, framing and background policies save independently: A keeps the existing
`[layers:07:01]` section; B uses `[layers:07:01:completion]` and inherits no A
overrides. Both sections travel together in **Save project**. Effects settings
remain shared by raw room 1.

In game, choose **Diorama → Skybox → Skybox only** to enlarge A's faces around
the scene. Their live eye sprites are captured into the same skybox image;
the scrolling water stays on its foreground BG2 plane. The skybox uses the
authentic 256-pixel face region, independently of the water's repeat policy.
Its lower edge stays above the projected cloud band as the camera moves,
keeping the bottom faces clear without reducing their size.
In **Skybox only**, A's water plane is lowered until its bottom covers the
viewport, preserving the original water artwork and scrolling animation.
Authored far-band tiles and stamps feed that image too. B uses its own captured
completion background and never receives A's face/eye promotion.

B's enhanced completion scene surrounds the native platform, hero and text
with an ocean vista, clouds, light shafts, reflections, cherubs and feathers.
It preserves the original ending's text, music and timing.

The in-game switch lives under **Action 3D → Scene → Death Heim completion scene**. It
enables the vista independently of Town 3D and World navigation 3D; it requires
Action Diorama 3D and applies its own backdrop and enclosure with any skybox
preference. Turning it off restores B's captured artwork using the player's
unchanged Diorama settings. Waves, sun glint sparkles, pixel water, clouds,
shafts, cherubs, feathers, platform water effects and golden rim light default
to on; individual overrides remain available in
[`settings.ini`](../../docs/manual.md#death-heim-completion-scene).
Completion clouds and illumination use scene-owned tuning and do not inherit
SIM preferences. Room B's browser preview is labeled **Native artwork preview**
and shows native artwork and edits, with directions to the in-game switch.
Its enhanced backdrop cannot be edited in this tool.

In game, final-boss progress becomes 7 before the faces have faded away. The
completion scene and its overrides activate together when the native script
switches BG1SC/BG2SC to `$64/$74` at `$00:F5F0–F619`, after waiting for the
fade controller to reach black. The later song-3 selection at `$00:F650`
occurs after the returning fade-in and is not needed to time the policy switch.

Choose a room and BG1/BG2, then open **Background policy** near the top of the
sidebar. Each selector shows the room's C-planner default. Override edge fill,
scroll motion, horizontal/vertical extents, or up to four row bands, then select
**Apply policy** to commit one undoable edit. **Preview policy in shared renderer**
shows the resulting pixels. The map guides mark the authentic viewport in white,
extent caps in green, and row bands in purple. Available extents are drawn at the
renderer limit of 128 pixels; actual coverage also depends on world/scenery
bounds and the preview's horizontal and vertical budgets.

For Death Heim room 3, select **BG1 → Use painted scenery (disable fill)**.
This selects Live world and Available extents, and removes row-band fills so you
can supply the scenery with tiles. **Reset to room defaults** removes all authored
policy overrides and restores the room's built-in mirror policy.

Explicit painted tiles (including blank stamps) always take precedence at their
actual map coordinates, before mirror/repeat/clamp filling. The policy still
controls unpainted gaps. Use **Live world / painted scenery** when those gaps
should remain empty instead of being filled from the original room.

Screen bands use half-open rows within 0–224. World bands use this BG's world
rows and move with its camera. The native planner rejects overlapping bands,
including screen/world bands that would cross later during camera travel.
Layer source and semantic role are shown for context and stay owned by the
planner. Policies belong to the room and apply across its terrain variants.

**Save project** includes applied policies alongside tile edits in
`diorama-layers.ini`; a policy draft must be applied first. No third INI is needed.
The game loads these overrides with Diorama enabled, before its temporary debug
BG Extents draft. Older INIs without policy records retain their existing defaults.
For example:

```ini
[layers:07:03]
bg1-policy = edge:world motion:normal horizontal:available vertical:available bands:0
```

Omitted fields inherit defaults. `bands:0` explicitly removes bands;
`bands:1` through `bands:4` replace them with complete, ordered
`bg1-policy-band` / `bg2-policy-band` records. Fixed extents include the
corresponding `left`/`right` or `top`/`bottom` caps (0–128).

### Editing and preview

This standalone editor classifies action-room background tiles into virtual
depth bands and authors those changes directly in `diorama-layers.ini`. The
editor is the source of truth; the game only loads and renders the exported
configuration.

The effects workspace includes a shared WASM preview. **Shared renderer**
loads complete rooms directly into the production C PPU and Diorama compositor
via WASM/WebGL2. No game capture or gameplay session is required. The original
JavaScript view remains available as **Depth inspection**, a free authoring
camera rather than a gameplay-framing preview.
**Original game frame** remains a separate original-background comparison.

Select **Shared renderer**, choose a room/terrain, then use **Camera & animation**
to navigate and seek frames (0–65535) in either direction. Drag the preview or use
arrows to pan. **Aspect ratio** above the shared view offers 4:3, 16:9, 16:10
(Steam Deck), 21:9 and 32:9. **Auto (editor window)** follows the browser window
shape; showing tools or switching modes does not change it. **Stretch** uses
the game's stretched 16:9 crop. **Custom monitor…** accepts a
target width and height, such as 3440×1440 or 1080×1920. These control the actual
shared compositor projection; the image is fitted into the editor without
stretching. The preview caps its longest side at 960 pixels to keep rendering
cost bounded. Monitor dimensions set the aspect, not a resolution benchmark.
Changing output preserves player placement and authored data, and clamps the
camera to the new gameplay limits. Pixel aspect and capture margins remain
independent controls.

The view bar also controls square/CRT pixel aspect,
inspection zoom/tilt, capture width margin, 0/32/64 row budget, and all three backdrop
modes. Camera/clock/coverage changes regenerate visible art from the complete
room; orbit-only changes reuse rasterized surfaces. Existing tile, pixel, depth,
alpha, scenery and framing edits feed the production INI resolver.

Automatic distance fits within the captured vertical rows as well as the
requested width, including CRT pixel aspect. Painting farther above or below
the room does not increase that row budget. Fully captured short rooms keep
their framing; deliberate zoom-out or orbit can still reveal the scene edges.

This view covers **scenery and environmental sources**, including animated
tiles/raster phases, priority bands, copied edge tiles, independent vertical
clips, bounded and named skyboxes,
and Aitos's periodic waterfall page. The native forest, cave, marsh, castle and
Aitos environmental kernels, exposure and camera-local waterfall sections also
run in this view. Clock-based accent events and a placeable ROM player sprite
are available. The preview omits other actors and HUD,
gameplay-driven transitions, final CRT/heat and frame generation. The preview
camera uses the saved game baseline and production fit/clamping rules. **Reset
to game view** clears inspection zoom/tilt. It does not replay transient reactive
camera motion. Captures
remain developer comparison fixtures, not an authoring requirement.

### Environmental authoring

Right-click BG1's map and choose **Add effect…**. The modal starts at the exact
clicked map position. Choose a family or preset, then configure its appearance,
area, particles and receivers in the wider inspector. Unsupported controls are
hidden. **Add effect** / **Apply changes** saves one undoable operation;
**Cancel** or Escape discards the whole draft. **Apply & preview** commits and
opens the shared native renderer at the selected marker. **Preview here** from
the map menu centres the native camera on any clicked point, clamped to the room.

Markers include the whole room's native defaults, without visiting their camera
positions or capturing scenes. Click and drag a marker with ordinary tile
selection to move it; select it and drag a corner to resize its supported ranges.
Double-click or right-click a marker to configure it. When markers overlap, the
context menu offers **Effects at this point** to pick the intended source.
Deleting an authored effect removes it; deleting a default disables it, leaving
a grey marker available for re-enabling or **Restore source defaults**. Grouped
fields show their source regions or terrain contacts. **Apply and configure family
lighting…** opens the parent controls; individual rays and windows can also
override their inherited scenery/player/enemy receivers in their own inspector.

**Environmental sources** remains an optional source picker and import/export
panel. Select **Configure selected effect…** to open the same modal. Change a
source's enabled state, tint and intensity there. **Light reach** adjusts a wall torch's
spill radius independently of its flame, ember positions and peak brightness.
Compound sources also expose stable numbered catalogue members: forest openings,
castle windows and bounce lights, and Fillmore cave water, mist, drips and wet
surfaces. Select a numbered member to enable, tint, move or scale it independently.
**Offset X/Y** move its native anchor; **Width/Length scale** change its shape.
**Angle offset** rotates forest/window directions. Unsupported controls are
unavailable: drips keep their particle size, water keeps its surface depth, and
torches use **Light reach** for their spill instead of scaling their flame.
Ceiling drip and forest ray length handles keep the upper source anchor fixed.
Single wall torches and Aitos waterfall/splash sources also expose anchor offsets.
Members inherit receiver choices until **Override light receivers** is enabled.
Tint, intensity and position edits preserve receiver settings from a complete field.
Use placed fans, contours and particle fields for additional authored structures.

Region handles also edit castle floor mist/sky glow, temple dust, cave ambient
light, moon clouds and foreground blood-water/mist spans. These
edits update the existing complete field in `action-effects.ini`; they do not
add another rendering layer. Castle mist's Y coordinate stays on its supporting
floor: upper corners resize its height, up to 128 pixels, and width must remain
over a contiguous supported floor. Multiple region guides retain independent
selection; shared field parameters such as shoreline height remain linked.
Foreground water handles resize horizontally and remain on the native water
band. Shoreline mist height stays within the renderer's shallow mist apron.
Moon reflections expose glow, reflection-row spacing and wave shapes separately
in the complete moon field; a single glow rectangle would misrepresent that effect.

Resizing follows the source's meaning:

| Source | Size control |
| --- | --- |
| Placed lights, fans, haze, particles, free mist and clouds | Width/height handles and inspector. Room gradients, heat and large particle/exposure areas allow up to 16384 pixels per axis. |
| Wet contours, ground-cloud paths and particle routes | Resizing scales the actual local path points as well as the guide. |
| Forest rays, castle windows, cave mist and wet sheen | Individual member width/length scales; direction handles where supported. |
| Castle floor mist and other region fields listed above | Region handles or numeric bounds; edits retain source and collision validation. |
| Torches | Light reach changes spill radius independently of the flame. |
| Drips and water surfaces | Drip length or water span width; particle size/surface depth are separate properties. |
| Aitos waterfall/splash/mist | Source width/length scales. Mist scales its puff sizes and motion, not only its capture bounds. |
| Terrain-selected floor mist, wet timber, insects and lava; grouped leaves, tower rays and grit | Complete field controls expose search spans, dimensions and motion. Their terrain/source guides are not freely stretched copies of the detected art. |

**Add effect…** also works from the sidebar at the preview centre. It offers these families:

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

The inspector edits its layer coordinates, width/height, tint, intensity
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
expose **Override dimming receivers**. Select scenery, player and **Enemies &
objects** independently. Clearing an override restores inherited behavior. Explicit light
receivers move scenery illumination under actors and sample selected objects at
their native hot point; every OAM part and extended-apron part receives the same
object tint. This is not per-pixel shadow tracing. Native ownership identifies
the player and non-player action objects without guessing from artwork or
combat flags. The enemies/objects receiver includes pickups and sprite-based
scenery props; HUD icons, spell controller slots and unowned UI remain unchanged.

Fillmore's front forest rays and Bloodpool's moon rays receive the calibrated
light on scenery, player and enemies by default. Rear forest rays keep their
atmospheric volume without adding the same sunlight twice. The inspector shows
these inherited targets even when the override is off. Overrides still allow
each receiver group to be selected independently.
The native runtime checks the resolved scene, including built-in defaults;
an explicit INI receiver override is not required to activate sprite lighting.
The distant castle sky fan remains atmospheric behind masonry. Its window
sources own the configurable illumination entering the room.

On sprites, dimming sets the ambient illumination and nearby lights add colored
illumination before the result multiplies the original palette. For example,
25% ambient plus 75% white light restores the original colors. A warm torch
both brightens and tints the artwork without filling its dark details with a
flat color wash. Player, enemy and extended-apron sprites use the same rule in
the game and shared preview, with the existing strength, falloff and receiver
controls. Shared Diorama scenery uses the same illumination rule.

Authored dimming regions apply a BG1-plane wash; higher priority scenery is
painted afterward. Native cave/castle dimming retains its existing BG1 coverage
and distance ramp. Independent object dimming uses the same region or ramp.
Receiver effects require Environmental effects; moving accent lights also obey
Action lighting, and their trails retain Action particles ownership.

**Player & receiver preview** uses the actual idle player, extracted automatically
from the ROM when building the editor. The production PPU decodes both facings;
the normal OBJ layer then supplies depth, priority occlusion, filtering and the
game's rim shader. Lighting samples the native sprite hot point, with the same
integer color transform as gameplay. **Rim lighting** starts from the bundled
game setting and can be toggled independently. Turning **Environmental effects**
off provides an unlit comparison.

Right-click a BG1 map point and choose **Preview player here**, or choose **Place
player** above the shared view and click where its feet should go. Shift-click
or Shift-drag also positions the player directly in that view, respecting zoom,
tilt and aspect. **Player settings…** opens the modal for exact coordinates,
facing and visibility. The optional second player sprite uses **Enemies &
objects** receiver settings for an artwork-identical comparison; Alt+Shift-click
or drag places it. These are temporary stand-ins, with no collision, physics or
game AI. Stationary artwork is cached; only changed sprite rectangles upload.

**Actor accent** selects one of 19
recognized families, with event position, velocity, start frame, duration and
seed. **Start in current view** positions it and starts its clock. Seek or play
to inspect one representative native flight, charge, strike or landing phase.
It does not run enemy AI, spawn game actors, or show their sprite artwork.
Selecting the resulting source exposes enable/tint/intensity and, where present,
light receiver controls. Actor source ID zero targets that family throughout
the selected room/terrain, including future native generations. Event/player
positions are temporary; source edits persist in the effects document.

**Place on map** uses the selected BG and places the chosen preset at a click.
**Edit emitters on map** picks authored areas, moves them by dragging their
centre/body and resizes the selected area with corner handles. Hold Alt to snap
to 16 pixels. Dragging is a draft until release and creates one undo entry;
Esc, losing focus or switching layer/room/terrain cancels it. **Show effect markers on
map** controls their guides; only the selected marker shows its bounds. Numbered native members and movable single sources
also have handles. Their guides describe catalogue anchors and extents, rather
than tracing every animated ray or particle. Native offsets retain the original
source's coordinate space; forest openings belong to the intermediate light
layer and scroll at its existing parallax. Authored emitters use their selected
anchor layer.
Moving a member changes presentation only: it does not move window artwork,
change collision or bypass native source-art validation.

**Complete linked moon field:** select Bloodpool's moon, reflection or cloud
source and choose **Edit complete moon-field definition…**. The popup exposes
its fixed BG2 anchor, six low and five middle ray profiles, soft-shadow source
radius/depths, cloud veil and modulation, distant reflection wash/glints, and
wave caps tied to the game's actual water rows. Individual component checkboxes
turn each part on or off. Source/profile changes use the same native/WASM
kernels; foreground water/timber/insect lighting follows the shared profile.

Apply creates one ordinary `moon-field` definition in `action-effects.ini`,
replacing the bundled moon group. Its single marker on BG2 moves the complete
light source, cloud and reflections together. It does not move the moon artwork.
The palette also offers **Linked moon, cloud veil and water glints (BG2)** for
other rooms; BG2 placement uses the clicked point. From BG1 it starts at the BG2
map centre because a static background has no unique inverse foreground position.
If the room has no native water-row animation, only those wave caps are omitted.
Apply/Cancel, undo, disable/reset and export/reload work like other definitions.
**Complete marsh and castle fields:** the corresponding native sources now offer
**Edit complete marsh-field definition…** or **Edit complete castle-field definition…**.
The palette can add either group to another room. Marsh controls include water
spans and materials, shoreline mist, wet timber, drips, ripples and insects.
Castle controls include explicit opening/sill witnesses, arch samples, source
positions/reach, stacked-window blends, gallery highlights, torch bounce, dust,
floor haze, moat glints and an independently editable BG2 moon profile.
Component toggles keep the aggregate native batching; none creates native actors.
Material witnesses must match the room artwork before a source becomes active.
The source ID preserves variation when positions change. Floor haze still checks
collision support. Apply/Cancel, undo/redo and export/reload use ordinary field
records. Dimming strength, world-space depth ramps and scenery/player/enemy receiver
checkboxes are part of the atmosphere/castle definitions. Preserve original
receivers keeps the existing cave light composition; selecting explicit light
receivers routes it through the shared receiver path. Older version-1 complete
fields gain their previous dimming values at import. Torch-flame accents and
actor/trap responses use separate controls.

**Anchor coordinates** is separate from **Draw placement**. Choose **BG1** for
playfield pixels, **BG2 · fixed point** for a source such as Bloodpool's moon,
or **BG2 · surface raster** for effects riding the water bands. Fixed means
stationary in BG2's map, not pinned to the display: the source follows BG2's
camera and plane/skybox projection. The whole light or cloud uses the source
row's transform, so independent water bands cannot split or bend it. Surface
raster mode deliberately follows each band's mapping. Neither mode adds a
rendering pass or creates textures; both use the existing shared geometry path.

Right-clicking BG2 now adds BG2 effects (water ripples default to surface raster;
other free effects to a fixed point). Their drag/resize markers live on the BG2
map. **Apply and show anchor on map** locates the selected emitter on its owning
layer. Changing the anchor selector keeps the saved numeric X/Y values; it does
not guess a conversion between independent maps. Previewing a BG2 point keeps
the current foreground camera, since a static backdrop does not identify one
unique position along a level. Terrain-supported mist, traced playfield contours
and dimming regions still require BG1.

```ini
[emitter:02:01:0:light-fan:00000126]
anchor=bg2-point
placement=background
x=112
y=62
width=32
height=256
strands=12
```

`anchor` defaults to `bg1`, preserving existing documents; the other values are
`bg2-point` and `bg2-raster`. Parsing resolves this once into the retained source's
projection plane and fixed-point flag. Bounds, geometry limits and batching are
unchanged, and only required source/attachment masks are requested.

Light fans also expose **Cloud-break modulation** (`twinkle`, 0–1). It slowly
changes the transmission of the fan and its individual strands over **Cycle
frames**, while their positions and angles remain fixed. Zero keeps existing
fans steady. For distant storm god rays, use `bg2-point`, background placement,
disable scenery occlusion, and leave the foreground light receivers off.
Foreground artwork still covers the sky effect normally.
**Ray source feathering** (`fade-in`, 0–0.5) fades the start of each strand over
a fraction of its length. Zero preserves crisp window/doorway attachments.

For Aitos-style backlit cloud edges, choose **Cloud-break lighting** on BG2.
This material combines a cream-colored edge, amber halo, broad angled shafts
and a lower-cloud reflection. Its rectangle covers the whole composition;
edge/reflection heights are fractions within it. Drag corners to resize or
use the rotation handle to aim the shafts. Opening count, pattern seed, moving
cloud cover, cycle time, softness and four component strengths are editable.
Openings have independently seeded positions, widths, brightness and spill
lengths, allowing clusters and dark gaps. Adding openings preserves the earlier
ones; changing the seed reshuffles the composition. Map guides use the shared
renderer layout to show source widths, widening beam envelopes and directions.
Each opening casts one continuous shaft across its full width, spreading a
soft wash into the sky. **Sky beam strength** controls the light in the air
independently of the cloud's surface color; **Sky beam falloff** controls how
quickly it fades below the opening. These retain the existing `streak-strength`
and `falloff` INI keys. Shafts keep a fixed source and direction while cloud
cover varies their brightness. The cream
edge and amber spill colors also determine their warm scattering color.
Both colors are configurable. The same transmission warms rolling BG2 clouds
and lets the bright openings show through them. All controls support Shift-reset,
copy/paste, undo and project export. Sparse INI definitions use the native defaults.

**Background base brightness** (`background-brightness`, 0–1; default 1) dims
the BG2 artwork before this sunlight is applied. It can deepen a waterfall
crevice while preserving bright, warm illumination through the opening.
The dimming follows the world-space rectangle with a 10% feather on each edge;
overlaps use the strongest dimming rather than stacking. This happens in the
existing sky/plane material, with no extra pass or texture. Mist, foreground
scenery and actors keep their own lighting controls. The neutral default leaves
existing rooms unchanged; disabling the light also disables its background dimming.

This is atmospheric BG2 illumination, so it does not illuminate actors or
foreground scenery. It supports live captured BG2 planes and skyboxes; named
ROM replacement skies and dedicated periodic skyboxes retain their own material.
Up to four enabled regions per room/terrain are accepted. All effects remain
under **Environmental effects**. Aitos 04:01 contains four editable examples,
with two softer depth bands of **Distant storm lightning**. The lightning
**Distant bolt softness** control feathers the bolt without a new blur pass.
Each strike has an irregular main channel and thinner forks. Sustained return
strokes retrace the lingering channel and reveal additional forks; some strikes
have a third pulse. **Bolt paths** controls the main channel plus up to three
forks; **Channel bends** controls the irregularity. **Storm pattern seed**
changes both shapes and timing. **Mean strike interval** controls the quiet
spacing, while **Active fraction** stretches the whole sequence: bright holds,
spacing between discharges and the fading afterglow. The channel remains visible
between pulses.
The dashed **Cloud attachment** guide marks the possible strike origins, 12%
below the region's top. Place this guide inside the cloud artwork, not in the
clear sky below it. The flash halo is centred on the same origin. The guide
moves and scales with the region, including copied and pasted effects.
**Horizontal parallax** sets camera travel relative to BG1 (Aitos uses 0.25
for the source cloud band); leave it blank to inherit BG2. **Ground palette**
and **Ground colour indices** select the distant terrain to strike. Aitos uses
palette 1 and indices `1,2,3,4,5,6,7,8,9,10`. The region height becomes a maximum
reach: the main bolt and forks stop at their first terrain contact, including
native raster scroll and tile edits. A missing contact hides the strike. Set
indices to `none` for airborne lightning. These controls require a BG2 point
anchor; changing to another anchor clears them. Copy/paste, Shift-reset and
project export retain these settings.
All of this uses the shared renderer, including deterministic reverse scrubbing.

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

Edits use the normal Undo/Redo controls. **Save project** keeps scenery and
effects together. Under **Individual INI files**, **Review / paste effects INI** provides
a copyable document and an atomic Apply operation with line diagnostics.
**Load effects INI** and **Export effects INI** use the same document. Save the
export as `action-effects.ini` beside the game's `settings.ini`, then restart
the game. It is independent of `diorama-layers.ini`; export both when sharing
scenery and effects separately, or save one project ZIP. The editor build embeds an effects file found beside its
input layers file. A missing effects file preserves the approved native defaults.

The shared parser supports schema version 1, sparse source overrides and twelve
emitter kinds. Scope uses hexadecimal map group/room, decimal terrain profile
(0 US, 1 JP, 2 EU), a named kind and hexadecimal stable source identity:

```ini
[effects]
version=1

; Obtain native source and numbered member IDs from the source inspector.
[member:01:01:0:forest-forward:00000004]
offset-x=48
width-scale=1.5
angle=6

; The parent family still controls receiver selection.
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

`enabled` is 0/1; intensity is 0–16 for lights and 0–4 for particles;
torch reach is 0.25–4. Above intensity 4, light radiance increases while the
falloff stays intact. Torches (native sconces and placed emitters) default to
**intensity 4, reach 2**; explicit values still override these defaults.

`illumination` (0–16) controls light received by scenery, the player and enemies,
independently of visible rays, additive attack glow and particles. When absent,
it inherits the response in `assets/effects/light-response.json`, shared by the
native renderer and editor. Torch response is 1 so its 4/2 tuning is applied
once. Other material lights use family-specific gains: local glows 3, broad
daylight/window light 2, lava 2.5, wet reflections 1.5. Actor glows use 3 and
lightning/sword beams 2 when receiver lighting is enabled; their default
additive overlays keep the original brightness. The four native player spells
also retain their additive glow and phase envelopes. Alpha waterfall mist
keeps a neutral response. Forest and moon material light use a 2x response;
their atmospheric volumes keep their authored intensity. The skybox projection
is published before building the shared material light map, so BG2-anchored moon
light also illuminates foreground scenery in skybox-only mode. Matching scenery,
player and enemy targets share their projected illumination map. Sprite lighting
samples that map per pixel after projection, so a narrow angled ray lights the
part of a sprite actually crossing it. The PPU exports owner/ambient attributes
beside each OBJ band; interpolation transports them with the artwork in the same
GPU pass and motion field. Flat mode retains object-space sampling. There is no
extra motion search or GPU readback. Shift-click the
Illumination control to restore the family default (or the loaded project value).

Emitter positions
are integer pixels (−2048 to 16384), width/height 4–512 (up to 16384 for particle
and dimming regions), mote count 1–128 (region density 1–16),
and lifetime 16–4096 frames. Other defaults are enabled, intensity/reach 1, white tint,
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

Each room/terrain permits 96 enabled emitters and at most 4096 added vertices per render batch;
the file holds at most 1024 records/512 KiB across all rooms and regional profiles.
The per-room instance and geometry limits are independent of that document capacity.
Conservative costs are 97 vertices
per light, four per mote and 296 per free-mist volume. Ground mist reserves
`(ceil(width / 16) + 1) × 45` vertices for the worst support fragmentation,
independent of the current camera. Each span uses three density slices and at
most 144 indices. Over-budget documents are rejected as a whole, with the
affected room, required count and limit shown beside the map. The separate
authored list cannot consume the 16 native actor accents or 27 native decoration
slots. Geometry shares retained pass
batches; emitters create no textures or per-frame heap allocations. The status
line reports actual submitted geometry/draws/uploads/allocations and CPU time,
which is not a GPU timing or Steam Deck/D3D12 performance measurement. New family
costs are bounded at export too: regions reserve 64 cells, fans reserve clipped
triangles per strand, clouds reserve 37 vertices per lobe and contours reserve
14 per segment. Smaller actual visible costs appear in the status line.

The forest default is a complete editable grouped `ray-field` recipe.
Select a forest family and choose **Edit complete ray-field definition…** to
extract its entire definition into the effects document. This replaces its
bundled capture, preserving aggregate rendering. The modal exposes openings,
origin groups, rear/front profiles, light-responsive mote clusters, leaves,
colors, motion, bounds and source-art witnesses. Existing member handles follow
the configured base positions/count; sparse member and receiver overrides still
apply. Untouched decimal values keep their exact round-trip representation.
**Disable field** keeps the replacement disabled; **Restore source defaults**
removes it and resolves the bundled definition. Apply/Cancel and Undo work as
for other effects. The grouped field is also available in the Add effect palette.
Its positions use the independent light layer; placement converts the map's
scroll ratio, as native member handles do.

`assets/effects/forest-ray-field.ini` is the single source of bundled visual data.
After editing it, run `python3 tools/generate_effect_defaults.py` and the native
recipe tests. Editor builds verify that its checked-in embedding is current.
Complete field records are version 1 additions; existing source/member/emitter
documents remain supported. They require every defining property, allow one field
per room/terrain, and retain the twelve-opening/four-origin/two-profile bounds.

Complete definitions are also available for cave water/atmosphere, Bloodpool
moon/marsh/castle lighting, torch/temple glows, trap/wizard/centaur lightning,
four projectile families and Aitos lava/splash/waterfalls. Select a native source
and use **Edit complete … definition** to extract its recipe. The same field
families appear in **Add effect**. Their source rules, geometry, palettes,
clocks and component controls are retained data shared with the native renderer.
Aitos fields offer material recognition or manual source lists. Add/remove source
buttons preserve valid bounds and unique identities. Lava-lake heat controls
operate the existing flat game refraction pass; the shared diorama preview shows
its glow/sparks but does **not** preview that flat-only distortion.

#### Actor attachments and reusable fire/clouds

A placed emitter's modal offers **Attach to an actor or attack**. Choose the
player or a family from the room picker; custom family IDs and optional parent,
state, visual-frame, animation and handler filters identify runtime attacks.
This works for actors without an existing native accent. **Bloodpool boss fire
preset** selects the Act 1 boss's fire children. Family IDs describe native game
identities, independent of regional art.

The optional `actor-parent` filter matches the parent's source captured when the
child is first observed. That association remains fixed for the child's lifetime,
even if the parent dies or its slot is reused. It resets with the child's identity
or room/observer reset. If the parent source is already unavailable on first
observation, a parent-specific filter cannot match; a later occupant of that slot
does not retroactively become the child's parent. State, visual-frame and handler
filters still follow the child's current facts. See the native
[projectile identity and lifetime](../../docs/ram-map.md#projectile-identity-and-lifetime)
reference.

Attached X/Y are offsets from the actor. The map marker uses the reference actor
position; dragging edits its offset. **Preview attached effect** or **Apply &
preview** supplies one matching visual actor sample and lets you scrub its age.
This does not simulate AI, spawn game objects or display actual actor artwork.
A maximum-visible-instances setting reserves authored capacity at import time.
Terrain effects and large particle regions stay attached to terrain rather than
actors. Receiver checkboxes independently control scenery, player and enemy
lighting; excluding scenery light does not suppress companion particles.

In the shared Diorama renderer, scenery and actor receivers apply ambient
brightness plus incoming colored light to the original artwork. A white light
can restore the original palette in a dimmed area; a colored torch both reveals
detail and tints it. BG1 normal, priority and far tiles share a resident GPU
illumination map. Filtering and lighting run in the same material draw, while
explicit ray volumes and particles retain their depth passes. No CPU pixel
readback is required. Renderers without custom shaders keep the legacy overlays.

Light sources accept **Intensity 0–16** in the effect modal and exported INI.
Values through 4 retain their established opacity curve; above 4, brightness
increases while the falloff keeps its shape. Scenery, player and enemy receivers
allow up to 4x illumination before output colors saturate. Try 4–8 for stronger
torch spill or chamber lighting, then tune the tint and reach independently.
Particle-only effects keep their 0–4 range. Raising intensity does not add
geometry, passes, texture memory or readbacks.

Use **Flame / fire with embers** for a reusable warm fire, or **Torch** for the
shared native flame profile with independent spill reach. **Cloud bank** supplies
layered moving clouds. All can be configured without new C code. Later-stage
compositions add ordinary editable halo, gradient, mist, ray and particle markers;
they are authoring starting points, not final art approval for those stages.

#### Copy and repeat placement

Select an effect marker; Ctrl/Cmd-click adds more markers. Ctrl/Cmd-C copies the
selection. Ctrl/Cmd-V enters repeat placement: click to paste, Escape to finish.
The right-click menu also offers copy/paste. The first copied marker lands at
the clicked world position; the remaining markers retain their relative spacing.
Copy, placement and validation feedback appears beside the map. Pasted markers
are selected and their guides made visible. Each paste gets fresh identities,
preserves settings and is one undoable operation. Individual native
torches copy into authored torches; their flame profile comes from the destination
room's shared glow field. Actor attachments copy their selector and local offset.
BG1/BG2 effects stay in their owning coordinate system. Whole compound fields are
edited through their source lists rather than duplicated as map markers. The
internal effect clipboard is separate from the exported INI clipboard.

Native actor phases, landing dust and spell-controller visuals use sparse
overrides here. Exporting such an override is not a complete definition of a
native actor effect.

#### Native members and edited light occlusion

Member identities are catalogue ordinal + 1, with append-only catalogue order.
Transforms are sparse `[member:GG:RR:T:kind:ID]` records in the same version-1
file. Each room/terrain allows 64 member overrides separately from its 16
additional emitters. Offsets are limited to ±512 game pixels, shape scales to
0.25–2, and supported angle offsets to ±30 degrees. Reset removes the record
and restores its native defaults. Tint and intensity combine with the parent.
Large transforms remain subject to existing room/source clipping and geometry
budgets; they do not make an invisible or unrecognized native source valid.

Directional lights now read the renderer's edited BG1 silhouette, including
stamped/copied tiles, flips, depth bands, painted black and transparent pixels.
Only the playfield band casts these atmospheric shadows; far/high bands do not
silently become playfield occluders. Bloodpool Act 1's moon sampler uses this
same edited opacity, while forest front rays, castle windows and authored fans
share a bounded projected coverage field. The three finite-depth samples soften
and attenuate rays under platforms without suppressing all light below them.
This is an atmospheric approximation applied to mesh vertices, not full
volumetric or per-pixel shadow tracing. Changing scenery never creates gameplay
collision, mist support or source-art witnesses.

The capture examines a 768 × 352 game-pixel window, merges opaque scanline runs
into at most 2048 rectangles and reuses each retained batch's coverage scratch.
The generic coverage field is 400 × 176 bytes. Overflow disables this optional
capture instead of overrunning its capacity. No extra GPU targets, texture
uploads, readbacks or per-frame heap allocations were added. Receiver sampling
and presentation use caller-owned retained batches; actor light meshes append
directly, without a shared global scratch buffer or an intermediate copy.

#### Whole-room default markers and inventory cost

The map inventory scans immutable room assets once per loaded room using the
native source producers and deduplicates stable kind/source IDs. The scan has a
1024-source bound and makes no PPU render, GPU texture, upload or draw calls. It
preserves the current preview frame and recipes; edits use sparse overrides.
Numbered members come from the shared native catalogue. Parallax source handles
convert between source-layer coordinates and BG1 map pixels when moving them.
Actor events are previewed separately; moving game objects are not static map
markers. Source-art checks still apply, so removing a source's supporting tile
can stop that native effect even though its default marker remains available.

#### Effects storage

Keep `action-effects.ini` beside `settings.ini`. It stores semantic effect
recipes and sparse overrides; `diorama-layers.ini` stores the room's layer,
framing and scenery edits. Preserve both files when moving an authored room.
Kassandora rooms `03/01`–`03/06` ship ordinary emitter and glow-field definitions
in this document for all three terrain profiles. Their sand areas, haze banks,
light boxes, bounced light, overhead fans, dust and brazier sources appear on the
map and use the same modal/drag/copy controls as newly placed effects. Act 2
(`03/03`–`03/06`) lights and dimming include the player and enemies/objects by
default in every terrain profile, as do the temple and Pharaoh presets. Act 1
keeps its scenery-only lighting. Receiver controls remain independently editable.
The stage presets also provide desert, temple and Pharaoh compositions.

Marahna Act 1 (`05/01`–`05/03`) ships ordinary editable emitters for all
three terrain profiles: cool ambient light, warm broken canopy light, light
boxes, textured swamp mist, firefly clusters, drifting leaves/spores, wet glints
and small bioluminescent moss points. Dimming and canopy illumination include
scenery, the player and enemies; their receiver controls remain independent.
Room 1 alternates wider warm glades with smaller, sheltered openings. Room 2
has cooler, more enclosed patches around its ruins; the boss room uses one broad
warm clearing. The fields vary in spacing, size, strength, tint and sway period,
using the same editable controls and emitter count as the earlier uniform layout.
Leaves and spores concentrate around the walking/jumping corridor, with wider
firefly clusters near the foliage and vine tips reaching into the normal view.
Their bounds remain fixed in BG1 world space; climbing reveals the quieter upper
canopy rather than moving emitters with the camera. Particle-region density is
per 256×256 world cell, so a room-sized field with density 1 is intentionally sparse.

**Settings → Scenery palette** adjusts the BG1 source artwork before dimming
and illumination. Strength blends the grade, saturation enriches or mutes the
existing colours, and shadow/highlight tones add cool/warm colour separation.
Actors, HUD and particles retain their own palettes. BG1-derived skyboxes use
the same grade in their cached source filter. This room-wide setting is saved
in `diorama-layers.ini` by Save project or Export level INI:

```ini
scenery-grade = strength:0.48 saturation:1.25 shadows:34675e highlights:d9d38b
```

It applies in the native/shared 3D renderer with Environmental effects enabled.
Apply palette validates the draft; reset and undo/redo work like other scenery
edits. Shift-click restores a value's neutral default. Grading is fused into
the existing scenery material, before exposure and incoming light. A room with
only a palette grade uses a retained black texel instead of a full light map.

The **Canopy light** emitter (`canopy-light`) uses a seeded leaf-gap mask.
Width sets the opening at the top of the field, height its vertical reach, and
angle leans the rays (±70°). Its map outline shows that same slant. BG1 collision
surfaces stop the light; decorative background pixels do not. **Solid terrain
shadows** toggles this, **Surface spill** controls the short warm return at a
hit, and **Air scattering** adjusts visible haze separately from illumination.
**Light / shadow softness** blends the field's sides and terrain contacts;
overlap neighbouring fields across those soft margins to avoid unlit seams.
Leaf gaps use equal world-pixel scales on both axes, independent of ray slant
or collision clipping. The full seed selects a mask, square rotation/reflection,
sample region and size, so seeds eight apart no longer repeat the same pattern.
Broad shaded crowns break up the fine gaps. These patterns reuse the fixed mask
atlas and existing draw batch; the shared mapping also lights sprites and mist.
Sway and cycle animate them around fixed world positions. Shadow capture is
bounded and independent of the camera. This is an artistic approximation of
surface bounce, rather than a full indirect-light simulation. Painted scenery
that has no collision does not become a blocker.

Free **Cloud banks** can enable **Textured wisps** for layered density and small
clear gaps. They use the same fixed atlas as canopy light, with no per-frame
texture generation or GPU readback. Place several low, overlapping banks with
slightly different tints, drift and lifetimes for rolling mist. Textured wisps
are separate from traced ground dust; clear the contour to use them.

The `foliage-shadow` emitter adds hanging vines and feathered leaf silhouettes.
**Textured vine silhouettes** selects the shared atlas, reducing per-stem geometry.
On BG1, **Solid terrain shadows** clips these stencils above collision surfaces,
using the full field rather than the visible camera window. Start the area above
open canopy: non-solid decorative masonry does not block this collision mask.
Marahna's shipped fields occupy open foliage pockets, with roofs clipping their
lower ends. Background placement draws behind BG1; playfield placement shows
silhouettes over the native backdrop.

**Fireflies / bioluminescent points** (`fireflies`) is a bounded region with 1–32
insects total. Each has a feathered bright core, a soft halo, an independent slow
pulse and wandering motion. Size controls the core/halo; tint and end tint color
the brighter/dimmer phases. Lifetime controls motion/pulse speed, travel adds a
slow excursion, and wander adds smaller loops. **Firefly replies** (`chorus`, 0–1)
blends occasional staggered responses into the independent pulses. **Glow style**
selects insects, will-o’-wisps, or luminous moss. Wisps have slower breathing light;
**Wisp tail** (`trail`, 0–1) adds three soft masks along their recent path. Moss
uses four irregular porous masks, stays fixed, and ignores travel/wander. Its
seed, count, size, region and tint remain editable. Light receivers select scenery,
player and enemies; illumination sets their faint local spill. Rolling mist catches
nearby firefly light through its existing canopy-response control. No particle
simulation, per-insect draw, dynamic texture upload or additional light pass is used.
The Add effect modal includes **Will-o’-wisps**, **Luminous moss patches** and
**Occasional vine dew** presets; all remain ordinary copyable emitter records.
Marahna uses one or two jade wisps per room, near the walking route, with warmer
golden fireflies and small moss patches on selected roots and damp stones.

**Occasional vine dew** uses `drips` with `activity` below 1, leaving quiet gaps
between falls. The top edge is the source and the bottom edge is the landing;
keep the source width narrow for a vine tip. **Water landing** (`ripple=1`)
adds a small expanding ring at that bottom edge after impact. This is explicit,
not automatic water detection: enable it only above verified open water and
keep the fall clear of platforms. The preset starts with ripples disabled.
BG1 **Light response** lets dew catch the same canopy light as nearby scenery.
The drop and ripple share one deterministic clock; panning or reverse scrubbing
does not restart either. Moss and ripple masks use the atlas's remaining cells,
without enlarging it. The editor and native game use the same kernels and budgets.

**Particle region → Leaves → Feathered leaf silhouettes** uses a rotating leaf
mask rather than a hard diamond. Particle regions and paths expose **Canopy
response** (BG1 only): 0 keeps their normal appearance; 1 makes glints/spores catch
the current canopy light and disappear in its shadows.

Marahna's damp temple ledges reuse Fillmore Act 2's `water-field` wet-sheen
component. A continuous shallow patch sits inside the measured stone contour,
with a thin animated reflection along its lip. Its green-blue body and pale
sage highlights suit the jungle lighting. These are surface patches rather than
floating glint particles; the distant foliage and open boss clearing receive no
wet highlights. Each patch has an editable marker at its landing surface.
The additions use saved emitters and complete fields with editable bounds,
colors and animation controls; Save project includes them in `action-effects.ini`.

Light fans expose **Scenery occlusion**, enabled by default for existing fans;
this uses visible BG1 opacity rather than collision. Cloud banks and canopy
fields support room-sized areas up to 16384 pixels. The catalogue holds 512
recipes; per-room limits remain 96 instances and 4096 authored vertices in any
single render batch. Validation reserves each entire field in its draw pass and
all possible material-light passes, including actor copies. Independent alpha
foliage and additive lighting no longer consume each other's batch allowance;
camera culling is never used to admit an over-budget pass. The shared immutable
1024×768 atlas is built/uploaded at renderer startup and uses existing shaders.

**Cloud volume** is available on textured, rolling `cloud-bank` emitters.
A positive `volume` selects rounded cloud billows; its 0–1 value deepens the
contrast between the main crest tint and **Shadowed body tint**. Zero preserves
existing mist. The editor enables rolling when volume is first enabled and
clears volume when texturing/rolling is disabled or a ground path is drawn.
Drift, billow, cycle and seed move and distribute fixed shapes without stretching
their density or shading. All lobes stay inside the saved region as it is resized.
BG1 and BG2 anchors, copy/paste, reset and project ZIP export use the same controls.
The Aitos sky uses BG2 fixed-point anchoring so raster bands cannot bend a cloud.

Eight density/lighting pairs are baked from small 3D cloud volumes at startup.
They use the same two masked quads per lobe and alpha batch as rolling mist;
there is no per-frame volume integration, new shader or additional render pass.
Cloud opening light tints the baked illumination without erasing its shadows.
The atlas grows by 1 MiB on the GPU, with 256 KiB of additional cached alpha and
500 KiB of shared startup scratch on the CPU. Existing mist and canopy cells
retain their pixel content.

**Spray rainbow response** (`rainbow`, 0–1) is available on textured rolling
cloud banks. Moving spray reveals fragments of a shared directional spectrum,
using direct **Cloud-break lighting**, droplet density and the current viewing
ray. The spectrum is centred opposite the sunlight; it is not an arch fitted to
each bank. Resizing a bank changes the spray coverage, not the optical curvature.

On the Cloud-break light, **Sun direction in depth** (`sun-depth`, −85° to 85°;
default 48°) supplies the out-of-plane direction missing from the 2D beam angle.
Positive values shine from behind the viewer into the scene; negative values
shine toward the viewer and generally hide the rainbow. This control changes
the spray's optical response without moving the sky openings. Nearby banks
under the same sunlight show pieces of the same spectrum. Camera rotation and
zoom change it smoothly; output resolution does not set its curvature.

This is a broadened RGB approximation to a primary rainbow, intended for the
source artwork and fine spray, not a droplet-size simulation. BG1 mist samples
BG2 sunlight using the captured camera translation. Ambient halo and reflected
light do not generate rainbows. Zero sunlight, zero density or disabled lighting
also suppress the spectrum. Both controls support copy/paste, Shift-reset and
project/INI export. A ground contour or disabled texturing/rolling clears the
bank's response.

The bounded mesh uses the existing alpha batch and atlas. Native rendering
retains its spray/illumination samples and evaluates the view-dependent colour
in the existing GPU projection shader; the editor/reference renderer uses the
same optical math. No additional shader pipeline, texture, pass or readback is
needed. The existing effect projection pipeline is prewarmed at game startup.
Aitos's local waterfall spray banks use a response of 0.9. The main lower mist
uses 0.65 in 04:02 and 1 in 04:03, in all regional profiles. Incident light,
viewing angle and mist density attenuate the result. The foreground wisps keep
their response off so overlapping near/far banks do not duplicate the spectrum.

BG1 rolling mist's **Light response** (`canopy-response`, 0–1) also catches
Cloud-break sunlight. Its existing lobe colours sample the same world field,
translated from BG1 to BG2, while retaining alpha, shape and shaded undersides.
This is independent of the rainbow response and adds no geometry or pass.
The Aitos boss funnel uses one centred vertical opening, a 41.5° sun depth,
and responsive lower/airborne mist and platform spray. The crossing keeps its
angled light. All of these controls remain ordinary editable effect properties.

**Spray sparkle response** (`sparkle`, 0–1; default 0) adds fine droplets to
textured rolling cloud banks. They follow the moving billows, sample the same
density mask and catch direct Cloud-break sunlight. Empty air and unlit mist
do not sparkle; the sun's colour warms the highlights. Ambient halo alone does
not illuminate them. There are at most four candidates per body lobe, with
independent clocks, using the existing particle geometry and alpha batch.
The inspector enables rolling when needed and clears the response when rolling
or texturing is disabled or a ground contour is added. Copy/paste, Shift-reset
and project/INI export preserve the control. Budget validation reserves the
maximum added geometry before admitting a bank.

Particle regions also offer **Water glints (flowing)** (`pattern=water-glints`).
These small reflections follow the configured travel and twinkle, unlike the
existing stationary **Surface glints**. For distant waterfalls, use BG2 fixed
point anchoring, background placement and **Light response** (`canopy-response`)
of 1. This makes glints follow BG2 scrolling and appear only in direct sunlight;
foreground scenery covers them. They use the existing additive BG2 decoration
pass. A response below 1 allows some glints outside the light. BG1 regions and
particle paths retain their canopy-light response and also sample sky light.
The Aitos defaults add flowing glints over both waterfall backgrounds and
sparkles on local spray and lower foreground mist, leaving broad clouds soft.

**Rolling ground dust** is a cloud bank with an optional `points` contour.
Trace the ground in its effect modal, or edit up to 16 local X,Y pairs in
increasing X order. The map shows the path and the maximum lift above it.
Gusts build on rising slopes and carry across crests before settling. `carry`
controls the downwind settling distance (0 hugs the ground); drift controls
wind direction and travel, and amplitude controls lift. Optional `particles`
lift small grains from this same path with a mix of hops and longer flights.
Their count, size, wander, end tint and seed are editable; zero disables grains.
Lifetime, strand count, tint and intensity control the cloud underneath them.
Resizing scales the path, and size reset restores the loaded path and bounds.
These are authored BG1 guides, so redraw them when changing the terrain.
Clearing the points also clears carry and grains, returning the bank to its
original airborne cloud behavior.
The shared renderer uses a bounded mesh without terrain scans or GPU readbacks.

**Torch motes** are optional particles on an ordinary placed `torch`. They
float within its spill-light ellipse and fade toward the edge. Set the mote
count, size, end tint, travel, wander and lifetime in the emitter modal; use
Reach and the shared glow definition to change the light pool. The mote area
follows that radius (up to 256 pixels per axis). Existing torch embers remain
independent. Older torch recipes have zero ambient motes and keep their look.
Kassandora sconces use wider amber spill profiles and slow golden motes.
Act 1's soft sun glare uses an ordinary BG2 light gradient with clipped edges.

**Particle paths** trace ordered local X,Y points from source to destination.
Use them for crest sand, downhill slides and ceiling grit. Unlike ground-cloud
contours, routes may travel in either direction or vertically. Dust accelerates
along the route; the `glints` pattern stays at fixed points on the traced art.
`activity` controls the active fraction of each cycle, leaving quiet intervals.
Count, lifetime, grain size, wander, seed and start/end tint remain editable.
Trace on the map, drag or resize its bounds, copy/paste, and reset like other
emitters. Attached paths use actor-local points; map tracing is disabled while
attached. `gust` links path density and ground-cloud density to a shared smooth
room wind envelope without changing particle positions when scrubbing.
`twinkle` (Grain glint strength, 0–1) briefly brightens the existing moving
grains on paths, particle areas and ground-cloud contours. It adds no geometry
or draw passes; zero preserves the previous appearance. Clearing a cloud's
contour also clears its glints. This differs from the stationary `glints`
pattern used on temple decorations.

The **Antlion emergence dust** preset attaches an ordinary cloud bank to
family `C66F`, parent `C66F`, animation `5000`, state 5 and visuals 4–6. These
identify the native sand-mound child, not the moving boss body or its shots.
The native mound origin stays at ground Y720 and retires when its parent enters
buried state 2 (`$00:C7A0–C7D6`). The cloud therefore follows the mound's
position and visibility without writing game memory or allocating game objects.
Its offset, area, tint, density and billow use the existing actor attachment
controls and preview; the shipped definition covers all three terrain profiles.

**Scenery heat shimmer** is a `heat-haze` region placed on either map. BG1
anchoring distorts playfield scenery above lava (low/high/far priority bands
share the same displacement). BG2 anchoring distorts the distant background,
including skybox-only mode. Actors and HUD remain sharp. Amplitude and intensity
control displacement (capped at four native pixels), lifetime sets the cycle,
and the rectangle feathers its edges. Up to four regions are supported per
room; overlapping envelopes take the strongest displacement. Move/resize the
map marker or switch its BG1/BG2 anchor in the modal to edit the heated air.

BG1 heat runs per fragment in the existing prewarmed scenery material, before
ordinary/DOF/priority filtering. This avoids camera-dependent coarse mesh
waves and adds no draw pass, scene capture or GPU readback. BG2 retains its
fixed mesh/resident skybox path. These authored regions use the shared diorama
renderer; flat mode retains its separate native lava-lake heat effect.

Placed torches expose **Moving torch shadows**. Their spill uses the shared
scenery coverage field with a slight source sway. The direct spill is softly
occluded while bounced light and the flame core stay bright. Kassandora also
ships ordinary cloud-bank layers, ceiling-grit paths and sparse gold-glint paths. These are available through the stage presets;
paths should be retraced if their supporting artwork changes.

Release packages carry `defaults/action-effects.ini`. It follows the same
backed-up content upgrade policy as `diorama-layers.ini` below. Source checkouts
use the root document directly.

The game reads these files on launch. Exported recipes contain settings and
source identities, rather than rendered vertices or particle states.

```sh
sh tools/action_editor/build.sh
sh tools/action_editor/build.sh ar.sfc out.html path/to/diorama-layers.ini
```

The default build needs a C compiler, Python 3 and Emscripten **5.0.7 or 6.0.12**. Its
output is a self-contained HTML file that can open from `file://`. The default output is
`build/action-editor/ar-action-layer-editor.html`, keeping the generated editor
out of the source tree and Git. By default it embeds the repository ROM and
`diorama-layers.ini`; **Load project** replaces scenery and effects together.
**Load scenery INI** under **Individual INI files** replaces scenery alone, and
**Export level INI** opens a copyable replacement section for the current room.
The dialog also offers **Download full INI…** for a complete merged file.
The build also reads `settings.ini` for the Diorama coverage guide's aspect,
pixel aspect, distance, camera tilt/mode, vertical budget and backdrop mode.
An optional fourth build argument selects
another settings file. These preferences are embedded for offline use; the editor
does not write game settings.

### Shared original-background comparison

The build defaults to `ACTION_EDITOR_WASM=on`, requiring Emscripten **5.0.7 or 6.0.12**
and embedding both the original-background comparison and shared renderer WASM
modules directly in the HTML. Opening the editor requires no compiler, server,
adjacent files, or network access. `ACTION_EDITOR_WASM=off` builds the existing
JavaScript-only editor with just a C compiler and Python 3.
`ACTION_EDITOR_WASM=auto` builds shared previews when `emcc` is available;
a different compiler version reports an error. Homebrew's `6.0.12-git` release
banner is supported. `EMCC` selects the compiler and
`EM_CACHE` can override the workspace-local build cache.

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

See [the snapshot contract](scene-snapshot.md) for the format and comparison
scope. Original-background snapshots do not include enhanced Diorama scenes.

Scheduled game captures (`AR_SHOT_AT_GF` / `AR_SHOT_EVERY`), F2 snapshots,
scene-asset dumps and native shared-renderer captures now write compressed,
lossless PNG images. This changes image storage only; memory/PPU dumps keep their
existing binary formats. `AR_SHOT_FORMAT=ppm` explicitly restores the legacy
scheduled-capture format for byte-contract tools. Existing PPM evidence remains
readable by the comparison tools. PNG pixel comparison requires Pillow; encoding
uses the already-required SDL 3.4+ API and introduces no new runtime library.
Capture readback/compression runs only when requested, never in ordinary rendering
or the measured room-sweep frames.

### Standalone native room sweeps

`actraiser_action_room_benchmark` runs the same complete-room renderer without a
browser, emulation or controller replay. It loads `.arscene` room assets and the
two authored INIs directly. It supports scrolling, animation, environmental
receivers, extended scenery and native Metal/Vulkan/DX12. No personal settings or
saves are written. The fixture export is local ROM-derived data; do not distribute it.

```sh
cmake -S . -B build-render-perf -DCMAKE_BUILD_TYPE=Release -DAR_TESTS_ONLY=ON -DBUILD_TESTING=ON
cmake --build build-render-perf --target actraiser_action_room_benchmark
node tools/action_editor/export_gpu_rooms.mjs build/action-editor/ar-action-layer-editor.html runs/sweep-assets 0
python3 tools/action_editor/benchmark_rooms.py \
  --binary build-render-perf/actraiser_action_room_benchmark \
  --rooms runs/sweep-assets --layers diorama-layers.ini --effects action-effects.ini \
  --output runs/room-benchmark --drivers metal
```

The supplied route plan covers Marahna 05:01–08 and Aitos 04:01–03, including
high/low sections and lava pits. Use `--only` to select named routes or `--plan`
for another JSON plan. Review captures to verify that each route exposes the
intended effects; the CSV coordinates are **requested** camera positions, subject
to the normal room clamps. Actor stand-ins follow the route; this is not a
benchmark of scripted enemies, game logic, interpolation, CRT or presentation pacing.

Each execution warms the **entire camera route and animation interval**, seeks
back and then records 240 frames. The runner repeats three times, alternates
effects off/on and backend order, checks the backend and rejects late shader
compilation. `--baseline` adds a paired executable comparison. `--wake-deck`
wakes/checks the desktop display before every execution and rejects runs where
the internal backlight turns off; Linux telemetry records clocks, temperature
and GPU load. Stage the binary, SDL library, scripts, room assets and INIs in an
isolated Deck directory and set `LD_LIBRARY_PATH` there. Under WSL, use `--wsl`
and `--drivers direct3d12 vulkan` to launch native Windows executables. With a
locked/off Windows display, use those paired results only for compatibility and
gross backend differences, not gameplay pacing.

`prepare_ms` includes editor room rasterization and texture preparation;
`compose_submit_ms` covers effect/compositor preparation and submission.
`gpu_wait_ms` is the remaining queued work at a deliberate GPU-idle wait, **not a
GPU timestamp**. Their sum is serial completion time, not the production game's
overlapped frame budget. `submit_elapsed_ms` is the combined preparation and
submission **wall time**, not thread CPU usage (older traces called this
redundant column `cpu_ms`). Actual thread CPU times require `AR_BENCH_PROFILE`.
Warmup has no CSV rows. No images/readbacks occur in
measured frames. `summary.json` records per-run means, medians, p95/p99; keep the
three individual results when comparing variation. Every run retains its raw
CSV, log, display status and telemetry, with input hashes in `inputs.json`.
Malformed numeric arguments and route plans are rejected before rendering.
Failure/timeout telemetry is retained, and incomplete or inconsistent timing
rows cannot become accepted summary statistics.

For room `07:08`, use `--final-battle` (or `AR_BENCH_FINAL_BATTLE=1` for a
direct invocation) to preview the optional native-nebula enhancement. It is
independent of the environmental-effects toggle and recorded in `inputs.json`.

Vulkan and DX12 retain upload staging per texture by default. This matters for
the native editor's CPU-rendered tile images: allocating and mapping fresh
staging for every upload caused repeated minor page faults on the Deck.
`AR_SDL_GPU_UPLOAD_REUSE=0` remains available for direct executable diagnostics;
the repeat runner clears inherited rendering overrides to keep its default
measurements reproducible. Metal keeps its existing upload policy.

For videos, capture separately at a smaller resolution:

```sh
python3 tools/action_editor/benchmark_rooms.py \
  --binary build-render-perf/actraiser_action_room_benchmark \
  --rooms runs/sweep-assets --layers diorama-layers.ini --effects action-effects.ini \
  --output runs/cloud-preview --drivers metal --only aitos-clouds \
  --capture --size 640 400 --frames 240 --repeats 1
# Run for either the off/on capture directory produced above:
ffmpeg -framerate 30 -i frames/frame-%06d.png -c:v libx264 -crf 25 -pix_fmt yuv420p -movflags +faststart preview.mp4
```

Capture-only mode writes lossless compressed PNGs and a camera trace, deliberately
omitting performance numbers. The animation step is two game ticks at 60 Hz per
output frame (30 fps). Use identical routes, settings and frame counts for
before/after captures. `AR_BENCH_SHOT=/path/final.png` optionally saves just the
last frame after a timed run. `--mode on` or `off` selects one configuration.

#### Diagnosing a slow route

For a direct benchmark invocation, `AR_BENCH_PROFILE=cpu.csv` adds owner-thread
CPU times for room preparation, composition and completion waiting, plus source
effect/skybox callback times and source packet counters. This is opt-in and does
not change the ordinary sweep CSV. The source draw/submission counters cover
the effect backend only, not all renderer or driver calls. Callback times are
nested inside composition; do not add them to the outer totals. Thread-clock
sampling has a cost, so retain ordinary runs for before/after comparisons.

`tools/profile_vulkan_sweep.c` is a **Linux diagnostic proxy**, not part of the
game or its portable renderer. It records actual Vulkan submissions and
timestamp-query intervals, and correlates GPU time with `CLOCK_MONOTONIC` using
`VK_EXT_calibrated_timestamps`. Build with Vulkan headers and a Linux compiler
(or the same Linux cross compiler used for the benchmark):

```sh
cc -O2 -Wall -Wextra -shared -fPIC tools/profile_vulkan_sweep.c -ldl -o profile-vulkan.so
SDL_GPU_DRIVER=vulkan \
SDL_VULKAN_LIBRARY="$PWD/profile-vulkan.so" \
AR_BENCH_FRAME_PROBE="$PWD/profile-vulkan.so" \
AR_VK_PROFILE=gpu.csv AR_BENCH_PROFILE=cpu.csv AR_BENCH_WARMUP=240 \
  build-render-perf/actraiser_action_room_benchmark \
  sweep.csv diorama-layers.ini action-effects.ini rooms/0401.arscene \
  0 712 3800 712 128 888 240
```

Use an awake display and the same full-route warmup/repeats as ordinary sweeps.
The repeat runner deliberately clears inherited rendering overrides; the above
is a direct diagnostic invocation. Do not run file transfers or other profiling
processes during measurement. The proxy requires one device/owner thread,
64-bit graphics/compute timestamps and the calibrated-timestamp extension. It
supports the harness's `vkQueueSubmit` / device-idle path, **not arbitrary game
execution**, and fails rather than silently overflowing its fixed capacities.
It uses no pixel readback and reads timestamp results only after the harness's
existing idle wait. Its query collection still adds overhead to the instrumented
wall-clock wait; compare ordinary runs separately. `AR_BENCH_SHOT` captures after
the probe closes; per-frame capture mode cannot be combined with the probe.
`benchmark_probe.h` defines the frame/close protocol used by the optional
library. Failed benchmark runs also collect pending work before closing it;
neither the protocol nor the probe is linked into the game.

The GPU CSV includes `command_span`, draw/compute/upload events, CPU
`queue_submit` calls and the CPU `device_idle` interval. `start_ns`/`end_ns` use
the calibrated host clock; `deviation_ns` records calibration uncertainty.
Shader IDs are FNV-1a-64 hashes of each module's SPIR-V bytes. Draw timestamps
span top-to-bottom pipeline stages and include overlapping work/dependencies:
**their durations are not additive or exclusive shader costs**. Command spans
also include dependencies and scheduling gaps, not pure GPU utilization.
Compare first-submit return to first GPU timestamp, the GPU span, and last GPU
timestamp to idle return separately. A delayed first timestamp does not itself
identify the OS, compositor or driver as the cause.

The measured Aitos/Marahna hot-path findings and next experiments are recorded
in [the 2026-10-29 audit](../../docs/environment-hotpaths-20261029.md).

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
# Build the browser compositor alone (Emscripten 5.0.7 or 6.0.12).
python3 tools/action_editor/build_compositor.py build/action-editor/ar-renderer-preview.html

# Capture a real scene with isolated copies of the seed/settings/input replay.
python3 tools/action_editor/capture_compositor.py --room 0101 --output runs/compositor-fillmore

# Optionally embed a small compressed example into the offline HTML.
python3 tools/action_editor/build_compositor.py build/action-editor/ar-renderer-preview.html \
  --scene runs/compositor-fillmore/scene.ardi

# Replay precisely those pixels/settings using the native GPU backend.
cmake --build build --target actraiser_diorama_replay actraiser_diorama_snapshot_test
build/actraiser_diorama_replay runs/compositor-fillmore/scene.ardi runs/compositor-fillmore/native.png
```

The native capture environment controls are `AR_DIORAMA_SNAPSHOT=/path/scene.ardi`
and optional `AR_DIORAMA_SNAPSHOT_AFTER=120` (uploaded Diorama frames). Disable
frame generation. The diagnostic copies only once and frees its temporary pixels
after writing. Some interior rooms require a natural-entry seed/replay; the helper
accepts `--seed`, `--replay`, `--warp-at`, `--diorama-at` and `--quit-frames`.
For the existing Aitos waterfall route use `saves/legacy/aitos-save.srm`,
`saves/legacy/aitos-waterfall.rec`, warp 1320, Diorama 1700 and quit 3000.

The repository INI is also the release's authored scenery source. Packaging
ships it as `defaults/diorama-layers.ini`; the first game launch after those
bundled bytes change backs up and replaces the installed live copy, including
corrections to existing rooms and removed tile patches. Backups are named
`diorama-layers.ini.pre-update-N`, and `diorama-layers.ini.installed` tracks the
last applied release. Local edits survive ordinary launches and identical
reinstalls. A source checkout without `defaults/` uses the edited INI directly.

## Regional terrain

**Terrain variant** selects US, Japanese, or European terrain for all 49 rooms (50 scenery selections)
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
updates only that room's scenery savepoint; other unsaved scenery or effects
keep the **Unsaved changes** badge.
When every room matches its loaded, copied or downloaded state, the badge shows
**Matches last copy** or **Matches last export**. Undo/Redo updates the badge.
**Select all** and Ctrl/Cmd-C work when browser clipboard access is blocked.
**Download full INI…** saves all rooms. Export before refreshing the editor;
use **Load scenery INI** with the updated file afterward, or **Load project**
with your saved ZIP to resume scenery and effects together.

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

**Coverage** asks the shared C compositor for the selected BG's viewport
footprint, using the same camera stops, aspect fit, saved framing, base-plane
depth/rake/bow and inspection pose as **Shared renderer**. There is no reference
room calibration or separate JavaScript projection. The query runs without PPU
scanout, texture uploads or drawing and rounds outward to complete 16px cells.

Green means existing scenery covers the footprint's outer bounds. Red marks
visible finite-room borders and reports optional columns/rows to cover them.
Short rooms can intentionally show their borders; red is not an invalid-map
error. Skybox backgrounds require no extra painted tiles. Mirrored, repeated
or capped backgrounds instead report that their policy controls coverage.

**Fit guide on map** includes this footprint in the authoring view. **Add edge
workspace** grows the workspace in one Undo step; paint the empty cells afterward.
Empty workspace never changes gameplay travel. Real painted extensions can
change horizontal camera stops, within the original gameplay range.

The build seeds controls from `settings.ini`, including the selected camera
mode's baseline. **Preview aspect** is linked to the shared renderer. Camera X/Y
show its allowed travel and cannot overscroll; the painting canvas still pans
freely. Controls are preview-only and do not export game settings. Review other
positions in scrolling levels. This checks outer bounds of the base plane, not
transparent holes, depth stacks, raster bands or transient reactive camera
motion. Zooming/orbiting can reveal capture limits that painting alone cannot fix.

### Saved framing for small rooms

Click **Frame** above the map (or **Adjust frame on map** in the sidebar) to
focus a draggable native 256×224 viewport. Dashed white marks the unshifted
view at the current preview scroll; gold marks the adjusted view. Drag the
gold rectangle to set the saved offsets, or use arrow keys for 1px nudges and
Shift + arrows for 16px. Each drag is one Undo step; Esc cancels the current
drag and returns to Select. The guide uses BG1 coordinates; the offset moves
the whole scene. **Show viewport guides on BG1** toggles the overlay. Switch to
**Shared renderer** to check the result with the game camera.

The **16:9** (cyan) and **16:10** (purple) guides show the shared compositor's
footprint at each aspect, including that aspect's camera stop. Toggle either
independently; drag inside a visible viewport with the Frame tool. **Guide
coverage → Game camera (shared renderer)** uses production projection, while
**Flat viewport** shows the native 224-row crop only. **Pixel aspect** and
**Camera distance** start from the game's settings; distance zero selects
automatic fit. Resolution alone does not change coverage at a fixed aspect.
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

In **Shared renderer** and game-camera map guides, horizontal panning uses the
game's baseline camera limits; vertical panning stops at the native room bounds.
Further dragging or arrow input cannot zoom the scene. Aspect ratio, saved
baseline, framing and actual pasted tiles determine the horizontal limits;
inspection zoom/tilt and empty workspace do not. Camera X/Y report the stopped
position. Small rooms retain their existing framing.

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

### Complete cave water definitions

The cave water default is also a complete editable `water-field` recipe.
Choose a cave water, drip, splash-mist or wet-sheen family and **Edit complete
water-field definition…**. Pools and falls use their owning BG2 world pixels;
wet contacts and rock contours use BG1. The field includes exact material IDs,
33 measured contour columns (127 denotes a gap), linked contact/ripple timing,
water colors, glows and layered splash-mist parameters. The bounds and witnesses
are explicit; a missing wet material disables that contact rather than attaching
water to decorative rock. Existing native member controls remain relative to
these configured bases. Complete water fields can be placed in other rooms;
their member markers follow the configured source counts. Wet-sheen handles
sit at the landing surface, while drip handles remain at the ceiling source.

**Linked water, wet rock and splash field** in the placement palette starts a
single pool at the clicked location, converted into BG2 coordinates. Configure
its contacts and components in the popup; counts and sampling rates are bounded
by the native geometry budget. Every scalar and vector is included in exported
`action-effects.ini`. Disable retains an explicit disabled definition; restoring
defaults removes that override. The canonical bundled definition is
`assets/effects/cave-water-field.ini`; regenerate the embedding with
`python3 tools/generate_effect_defaults.py` after editing it.

### Complete cave and temple atmosphere definitions

Choose a native `temple-dust`, `cave-light`, `temple-grit`, `floor-mist` or
`tower-window` source, open its configuration modal, and choose **Edit complete
atmosphere definition…**. This extracts the current room's entire atmosphere
into an ordinary `atmosphere-field` record. It replaces the native field while
using the same geometry kernels. Apply and Cancel remain transactional; resetting
the complete definition restores the bundled default. Source-level receiver
masks and sparse appearance overrides remain independent.

The three defaults are in `assets/effects/{cave,temple,tower}-atmosphere-field.ini`.
Regenerate embeddings with `python3 tools/generate_effect_defaults.py` after
editing these files. No C lighting/source table must be edited as well.

All positions use BG1 world pixels. `counts` selects active ambient sources,
dust rectangles, grit contacts, tower origins and map witnesses, respectively.
`components` is a sum of dust=1, tower light=2, ambient light=4, grit=8 and floor
mist=16. Keep inactive slots in the file: complete fields are strict, bounded
records. Dust coverage is the union of `area-N` rectangles. The modal labels
source radii, exposure versus surface gain, particle timing, RGB/opacity and
profile values separately. The **Atmosphere: light, dust, mist and grit** palette
entry starts a generic light pool and dust region at the clicked point, with
room-art witnesses disabled so it can work in other rooms.

`floor-area` is a 16-pixel-grid search rectangle: each column uses its first
exposed supported floor. Spikes with no collision do not stop mist above their
base. The region is limited to 880 pixels wide and a 128-pixel vertical search;
fragmentation beyond the existing native mist-span budget omits only floor mist.
`floor-style` controls volume height and ambient-light response. The `mist-*`
properties describe the same layered volume kernel used by splash mist.
Native **floor-mist** markers sit at the centre of each collision-supported
span, with an outline extending up from its floor by the configured mist height.
These guides refresh after floor-area/height changes and Undo; they do not use
the generic room-family marker row. Click a span to edit its appearance or open
the complete atmosphere definition to change its floor search area.
`grit-*` and `grain-*` configure falling grit and its landing burst; actor jump
and landing event scheduling is still a separate native family.

Atmosphere sub-sources are edited through labeled vectors in their configuration
modal; they have no individual drag handles.

Other native regions also have spatial guides: cave light pools, dust areas,
falling-grit paths, tower-window light, forest leaf coverage, castle floor mist
and water, and Aitos lava surfaces. Castle sky-light guides live on BG2. Guides
use the active definition and native layer-to-map scale; lava capture refreshes
when its surface definition changes. Castle-window members likewise use the
active source positions and count. Group controls without their own spatial
region stay in the source selector instead of adding placeholder map markers.
Clicking a linked region keeps **Apply & Preview** centred on that region.

### Light boxes and native dimming

**Draw light box** lets you drag a rectangular area on BG1 and configure its
brightness (0–200%, with 100% unchanged), four inward edge fades in map pixels,
and scenery/player/enemy receivers. Boxes can be moved, resized, copied, pasted,
and reset with the existing map controls. The dotted inner guide shows where
full brightness is reached. Opposing fades scale down together on small boxes.
Boxes with the same brightness merge using their strongest coverage, including
feathers; overlapping them never darkens or brightens the overlap twice.
Different brightness values multiply. Flat rendering uses bounded, world-anchored
union geometry in the existing alpha/light batches, including deferred GPU
submission. Diorama samples exposure on its existing BG1 low/high/far scenery
meshes, preserving texture alpha and avoiding extra draws. Like native dimming,
its fades are interpolated at the scenery mesh resolution.
The C parser checks the combined geometry budget before accepting an edit.

Native atmosphere/castle dimming now has a selectable **Light box** guide over
its actual area. Opening it drafts an ordinary exposure emitter and removes
only the parent field's dimming, preserving all other atmosphere properties.
Apply commits both together; Cancel and Undo restore the original definition.
Fillmore Act 2's lower cave/palace zone spans (800,640) to (2048,1280), at 55%
brightness, fading in over 320 pixels from the left and 448 from the top.

Exposure INI records use `brightness=0.55` and `fade-left`, `fade-top`,
`fade-right`, `fade-bottom`. Old `intensity`-based exposure records retain their
hard-edged dimming when the new keys are absent; an explicit brightness takes
precedence over intensity.

Rolling floor mist uses `cloud-bank` with `textured=1` and `rolling` (0–1).
The main color tints its thin crests; `color-end` tints the denser shadowed body.
`amplitude`, `drift`, `lifetime` and `strands` control billow, travel, cycle and bank count.
`wake` (0–1) adds short trailing wisps from moving player/enemy bodies within the area;
`canopy-response` (0–1) catches authored canopy sunlight using its existing mask and terrain stops.
These optional controls use BG1 world coordinates, without an actor attachment or traced dust path.
Move the player/enemy stand-ins while the shared preview plays to check wakes. Pausing holds them;
warping, changing rooms and seeking time clear their history. Native projectile children do not stir fog.
The renderer reuses the shared mask atlas and alpha batch (two quads per bank, at most two wake quads
per area); there is no fluid solver, per-frame texture generation or additional full-screen pass.

### Shared preview scanout reuse

The native and WASM editors retain the previous tile image when only the
environmental-effects clock changes. Effects, receiver motion, lighting and
projection still update normally. The reuse key includes resolved camera and
capture bounds, every raster-scroll/mosaic row, color-math state, character
animation phase and BG2 page phase. Successful scenery edits invalidate it;
invalid edits leave the last accepted image intact. Animated waterfalls and
wave backgrounds redraw whenever their resolved tile inputs change.

`AR_EDITOR_RASTER_CACHE=0` disables reuse in native comparison tools. The WASM
module always uses it and gains no process-environment import. The
`EditorRoomScene_RasterCount` diagnostic counts actual PPU scanouts, and the
native room oracle prints it when `AR_EDITOR_RASTER_STATS=1` is set.

October 27, 2026 validation compared 5,400 timeline/camera/capture samples across
all 150 regional room variants: tile and effect hashes matched the uncached
path, with scanouts reduced from 5,506 to 3,449 including additional regression
checks. Still-camera previews with animated effects at 1920×1080 on Apple M2
Metal gave these median run-mean CPU submission times (two runs per mode in
off/on/on/off order, 600 frames each, excluding the first 30):

| Preview | Uncached (ms) | Reused (ms) | Reduction |
| --- | ---: | ---: | ---: |
| Marahna shrine | 2.302 | 1.489 | 35% |
| Marahna gallery | 3.139 | 1.955 | 38% |
| Viper chamber | 3.369 | 2.007 | 40% |

Frames that change the tile image still pay the normal scanout cost; this is
an average editor CPU reduction, not evidence of improved native-game p99 or
display pacing. Scrolling runs also benefited when consecutive frames shared
the same resolved tile camera/animation state, but CPU/system variation makes
their exact percentages less useful. Local scripts and raw results are in
`runs/effect-packing-20261027/benchmark-cache*.py` and `cache-parity.py`.

### Shared cloud materials

Native rendering and the WASM preview use the cloud material, motion and mesh
kernels in `src/render/cloud_material.*`, `cloud_noise.h`, `cloud_motion.h` and
`cloud_mesh.h`. The existing cloud-bank, mist, spray, smoke and complete-field
controls retain their profiles and serialized properties. This does not change
saved regions, BG1/BG2 anchors, timeline behaviour, palette or layer ordering.
New cloud styles should extend these material/shape profiles instead of adding
another private baker. Scene-specific floor support, actor emission and
projection stay in their adapters. See `docs/aitos-act1-effects.md` for the
consolidation parity checks and render-path boundaries.

### Native final-battle nebula

The optional room `07:08` enhancement grades and blooms the original projected
background. **Native nebula glow (07:08)** in Shared renderer previews the game
preference; **Settings → Native nebula glow** edits the material. These are
independent, so disabling a preview does not erase authored values.

`native-bloom` in the room's `diorama-layers.ini` section stores `strength`,
`radius` (native image pixels), `threshold`, `saturation`, `contrast`, `light`
and `receivers` (player 2, enemies 4, both 6, neither 0). Save project, level INI,
undo/redo and Shift-click defaults use the same values. The controls are disabled
outside the final battle. Reset removes the override and restores engine defaults.

The same section also controls drifting space dust: `dust` (density, 0–1),
`dust-scale` (spread, 0.5–2), `dust-speed` (integer 0–4), and six-digit RGB
`dust-blue` / `dust-rose` colours. Density zero disables the additional clouds;
speed zero freezes them. Old material lines load the new default cloud settings.
The sparse wisps use the shared cloud density, mesh and motion kernels and sit
behind fighters. Their gameplay clock freezes when paused; editor seeking is
deterministic, including the scene-clock wrap.

Six cloud variants share one 576×96 procedural atlas (216 KiB) and one geometry
draw into the existing background target, before light extraction. The atlas is
prepared with boot resources. No additional shader, filter pass or per-frame
texture upload is needed. Native resident skyboxes publish their already-resolved
source bands for matching CPU detail placement; this requires no GPU readback.

Native and WASM renderers share the original raster-wave input, colour shader
and projected radiance map. Original art stays at output resolution; the two
filter targets are bounded to 640×360. The original geometry is submitted once,
then actors and foreground priority bands retain their painter slots. Authored
lights, dimming and atmospheric overlays remain available. There are no new
bitmap assets, CPU readbacks or effect image uploads during animation. Shaders
are included in the existing boot preparation for Metal, Vulkan and DirectX 12.
The default halo is broader than the bright filament cores, with restrained
saturation and separate fighter illumination. Glow strength supports the full
0–3 range without exceeding normalized texture-tint limits or disabling the
enhancement. Existing authored scenery illumination is reused for extraction
and the normal scene; it is not rendered twice.
