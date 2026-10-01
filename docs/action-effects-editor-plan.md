# Action effects editor and shared browser renderer

Planned 2026-09-30. This document owns the editor/runtime implementation plan.
[Action environmental effects](action-environmental-effects-plan.md) owns the
per-level art direction and stage order. Implementation, visual acceptance, and
target-platform performance are separate completion gates.

## End goal

The level editor must let the user select, add, move, shape, tune, disable,
duplicate and save all established level-effect families, and author the effects
planned for later stages. The preview must use the game's rendering code and
matching scene inputs. Routine placement and tuning must not require editing C
or rebuilding either the game or the browser module.

The first usable release is lights and floor mist, but that is an intermediate
milestone. Completion includes rays, particles, water and wet surfaces,
waterfalls, clouds, exposure regions, and supported event/actor-bound accents.
The editor must expose the existing approved effects, not just new empty scenes.

The normal workflow loads a complete level from the project's room assets and
authored configuration. The user selects a stage/act and its rooms, navigates the
full map, and scrubs animation/effect time in the shared renderer. No gameplay
session, save state, screenshot or manual scene capture is required to open,
explore or edit a level. `.arscene` and `.ardi` captures are optional developer
comparison fixtures, never the source of truth for an editable level.

The renderer generates the visible surfaces from the full room at the requested
camera position, time and view settings. A captured rectangle must not limit
camera travel, horizontal/vertical extension, zoom or the locations where effects
can be placed. Loading a whole level does not require rasterizing its whole map
into a single texture: retain the assets and authored data, then resolve/cull and
update the visible region using the shared native rules.

New renderer behavior may still require a new shared C effect implementation.
Existing behaviors and their combinations should be reusable through presets and
authored parameters. Future effects are not considered implemented merely because
their category appears in the palette.

## Current foundation and verified limits

- The standalone editor already has room/BG selection, regional terrain families,
  tile/depth/pixel edits, scenery stamping, framing, undo/redo, native animation
  controls, INI export and a JavaScript/WebGL Diorama preview.
- That preview independently implements rendering. It is useful for editing,
  but does not establish agreement with the complete production compositor.
- The game provides `Diorama_Composite`, `ArRenderDevice`, separate effect-backend
  contracts, immutable presentation inputs, and shared C room decoding. The
  environmental capture/render modules and common pass policy already exist.
- A September 30 feasibility probe compiled `diorama.c`, `action_room_scene.c`,
  `action_scene_effect_render.c`, `action_bloodpool_effect_render.c` and
  `action_cave_effect_render.c` into WASM objects using Emscripten. This was
  object compilation only: no linked browser compositor, shader parity or
  interactive editor was validated. Local evidence is in
  `runs/wasm-renderer-feasibility/`.
- Production shaders currently generate Vulkan, Metal and D3D12 outputs. A
  browser backend and matching shader variants remain necessary.
- The static room editor does not simulate native enemies or gameplay events.
  Live event effects need explicit preview events or captured/replayed state.

### First implementation increment — 2026-09-30

The original-background comparison foundation is implemented. A versioned
`.arscene` codec, native replay tool and pinned Emscripten module now use the
production `ActionRoomScene` rasterizer in **Original game frame**. The editor
embeds WASM offline, exposes snapshot download and reports fallback status.
Native/WASM frame hashes agree for 1,176 frames across all 49 rooms and three
terrain variants, including camera/time/phase changes. Malformed imports are
atomic; the native codec passes address/undefined-behavior sanitizers.

This is a bounded part of phase 0, **not** completion of phases 0 or 1. The
enhanced compositor, edited surfaces, actors and environmental sources are not
in snapshot v1. Diorama and effect-authoring controls remain pending. Browser
smoke checks subsequently passed over localhost: WASM startup, original-frame
display, Fillmore cave X/Y scrolling, terrain selection, animation playback and
band-tint switching, plus Bloodpool's water/raster view. Browser console checks
reported no warnings or errors. Direct `file://` navigation remains blocked by
the browser tool; its download-event check timed out, so browser snapshot
download completion is still unverified. Automated snapshot export/replay tests pass.
Build/replay commands and format details live in the
[editor guide](../tools/action_editor/README.md#shared-original-background-comparison)
and [snapshot contract](../tools/action_editor/scene-snapshot.md).

### Compositor extraction increment — 2026-09-30

The production `Diorama_Composite` now takes explicit, immutable render options
through `DioramaScene`: visibility, skybox policy, shading, GPU-effect choices,
implementation gates, a borrowed layer table and a named-skybox resolver. Desktop
camera input, settings persistence, environment gates and resource ownership live
in `diorama_controls.c`. Auto-fit distance is returned with the projection rather
than written into the desktop controller from the compositor. A boundary check
rejects reintroducing live settings, WRAM, manifest, environment or clock reads.

The **linked compositor** now runs natively and in a separate WASM validation
module. Its validating command recorder covers 108 synthetic configurations:
4:3/16:9/16:10, square/CRT pixels, zoom, symmetric and redistributed extension rows
up to the 640×352 capture allocation, guard columns, skybox off/only/both, periodic
and band-mapped skyboxes, authored depth, visibility, shader availability, effect
callback ordering and resource lifetime. Native/WASM agree across 2,954,081 recorded
values with a maximum delta of 0.000184 (tolerance 0.001); native ASan/UBSan passes.
Before/after native comparison also preserves drawing geometry, draw order and
shader parameters across the same 108 configurations, allowing for changed
temporary texture identities and allocation commands.

These checks exposed existing target churn when BG guard columns made background
captures wider than sprites. Supersample and compact DOF scratch targets now retain
two exact sizes each, with bounded eviction and reset. Repeated unchanged frames
allocate no new targets in the fixtures, including mixed capture widths. Invalid
capture dimensions/non-finite camera inputs fail before submission; failed frames
clear their published projection, and a failed target restore stops drawing.

This is a **geometry/command agreement gate**, not browser image or shader parity.
The test backend records shader parameters; it does not shade pixels. It uses
synthetic plane content-presence markers, not captured enhanced room art. Snapshot
v1 and the editor's active rendering modes are unchanged: Original game frame uses
WASM, while Diorama 3D still uses JavaScript/WebGL. No environmental authoring UI
or additional effects are claimed complete. Native Deck/Vulkan and Windows/D3D12
GPU validation remains outstanding.

The captured compositor increment below supplies the subsequent surface and
WebGL2 comparison gate. The shared compositor currently owns one device's scratch
resources per module; reset against the old device before switching contexts.

### Captured compositor and WebGL2 increment — 2026-09-30

A separate captured-scene viewer now runs the production compositor against real
native plane pixels. `.ardi` snapshots carry owned upload-time BG/OBJ/priority
surfaces, extension rows and aprons, resolved layer order/shapes, camera/bounds,
coverage/row masks, transparent fills, exposure and captured/periodic skybox art.
The codec uses explicit little-endian fields and RGBA bytes, not structure dumps.
It validates and atomically rejects malformed inputs; browser replacement uploads
a candidate before releasing the previous scene. Normal native play does not
allocate snapshot storage. Frame-generated and named-replacement captures fail
closed until their exact inputs are supported.

The WebGL2 adapter implements texture/target operations, scoped viewport/clip,
batched geometry, blend/address modes and semantic shader bindings. GLSL ES
variants come from production GLSL with a render-target orientation shim and a
checked uniform layout. WASM memory is fixed at 64 MiB; streamed buffers and the
existing bounded target caches are reused. The offline viewer can adjust output
aspect/size, camera distance/tilt and skybox mode, and export PNG comparisons.
WASM-enabled editor builds link to this separate viewer.

Native Metal and browser WebGL2 comparisons passed for real Fillmore 01/01,
Bloodpool 02/01 and Aitos 04/02 captures. Bloodpool's changed camera, 4:3 output
and skybox-plus-plane mode also passed. At the original Bloodpool view, maximum
channel error was 1/255. Aitos had one edge pixel above the 2/255 tolerance;
the tilted Bloodpool comparison had 24 pixels above 8/255 in 480,000 pixels.
These small raster-edge differences are recorded, not described as bit-exact.
Local evidence is in `runs/action-editor-captured-scene/`.

The real-packet WASM harness exercises shader availability/fallback, all skybox
modes, three aspects, zoom/tilt, atomic failed loads and resource teardown across
all three scenes (601 draws). Repeated frames allocate no new targets. The
108-case native/WASM command gate remains passing under native ASan/UBSan, and
the packet codec has separate malformed-input and sanitizer checks.

The captured viewer remains supplemental validation tooling. Its capture format
still excludes effect inputs, a flat HUD, heat/CRT and named replacements. The
following live-room increment supersedes the previous capture-only workflow.

### Live whole-room scenery increment — 2026-09-30

The main editor now has an opt-in **Shared renderer**. It loads the embedded full
room assets automatically and feeds a small isolated runtime API backed by the
production PPU. The same C compositor then renders its regenerated surfaces with
the current editor INI. No runner, emulated gameplay state or manual capture is
involved. The legacy JS Diorama tab stays available until parity acceptance.

Implemented: all 49 room choices and US/JP/EU terrain, free native-room camera
travel, explicit frame seeking/playback, 0/32/64 redistributed rows and horizontal
coverage, square/CRT pixels, 4:3/16:9/16:10 outputs, zoom/tilt, all backdrop modes,
priority/far bands, pixel masks, stamps, authored scenery extents and framing.
Named ROM backdrops reuse the native immutable-page rasterizer. Finite skyboxes
use the production background-view sampler; Aitos uses its periodic native page.
Independent layer clips keep captured extra rows within each layer's native bounds.

The sanitizer-backed whole-room gate compares 882 native/WASM scanouts across
147 regional scenes and validates draw geometry/resources, reverse time, malformed
edits/loads, classification/mask changes and teardown. The original-background
gate still matches all 1,176 frames. The desktop build and relevant C tests pass.
Tile classification is cached on edits; unchanged frames allocate no textures;
orbit/aspect changes do not resample or upload room pixels. Browser timings are
CPU submission measurements, not GPU or Steam Deck/D3D12 performance evidence.

**Phase 2 remains in review.** Live-scene native/browser image comparisons and
representative edited-scene motion review remain before replacing the legacy
Diorama tab. The Bloodpool 2:1 combined-backdrop corner artifact was subsequently
traced to capture aprons bypassing vertical layer clips. The shared PPU now
clips the guard columns and edited tiles consistently; the regression checks
the full surface pitch, and the browser reproduction no longer shows the water
blocks above the sky. This fix does not establish final compositor parity.
Actors/HUD, reactive camera behavior, gameplay-driven transitions, final CRT/heat
and frame generation are not yet connected. Native Deck/Vulkan and Windows/D3D12
checks remain open. Environmental integration is described below.

### Shared environmental authoring increment — 2026-10-01

The prior whole-room foundation was committed as `e5918817`. Environmental
capture now has a borrowed immutable scene contract with separate native-WRAM
and full-room adapters. Forest, cave, marsh, castle, torch and Aitos source
recognition share the same C kernels. The editor calls no gameplay interpreter
and manufactures no WRAM. Native actor/event observation remains in its existing
owner. Native and browser also share the decoration pass table, exposure policy
and effect geometry; seeking the room clock now seeks ambient animation.

Implemented authoring:

- Version-1 `action-effects.ini`, stable room/terrain/kind/source identities,
  sparse enable/tint/intensity overrides and independent wall-torch spill reach.
- A native source picker and inspector, default reset, and placed soft-light,
  mote and free-mist emitters. Coordinates, field dimensions, tint, intensity,
  cycle and mote count are editable. Emitters can be duplicated or removed.
- Authored emitter map picking, placement, move and corner resize handles with
  one-step undo, grid snapping, cancelled drafts and room/terrain isolation.
  Mote fields expose independent radius, drift, sway/spread, seed and age tint;
  deterministic analytic animation preserves backward seeking and fixed cost.
- Native session-start loading, shared atomic validation, import/export and a
  review/paste dialog. Changes participate in the map editor's undo history.
- Separate authored storage: 16 enabled emitters and a conservative 4096-vertex
  budget per room/terrain. The 16 native actor and 27 decoration records retain
  their budgets. Geometry reuses the existing pass batches and retained scratch.
- Fixed-memory offline WASM packaging, source/geometry/draw/upload/allocation
  diagnostics and a context-restoration handler that rebuilds resources while
  retaining in-memory documents. Recovery itself still needs browser testing.

Validation: 147 regional rooms, 882 exact native/WASM surface and resolved-source
matches, 147 authored-file round trips, finite submitted geometry, reverse seeking,
unchanged-frame resource reuse and atomic invalid imports under the native
ASan/UBSan oracle. Native effect/capture/presentation, PPU and Diorama tests pass
(the optional frame-generation GPU test skips without its environment gate).
Torch reach tests verify changed radius with unchanged peak, flame and embers.
An authored-only presentation test verifies additive light, alpha mist and the
Environmental effects setting independently of actor lighting/particle settings.
Browser import, rejected version, undo/redo and source/geometry changes were
reviewed over localhost. Export download completion remains unverified; copyable
INI is available. No authored-file live-game visual comparison is claimed yet.

These are parts of phases 3–5 and 7, **not completion of every remaining phase**.
Current phase status and concrete remaining work:

| Phase | Delivered | Remaining acceptance / implementation |
| --- | --- | --- |
| 0–2: shared preview | Full-room scenery and ambient sources, native kernels, deterministic camera/time, aspect/extension controls | Matched edited-scene images and motion, actor/HUD inputs, final post-processing, legacy renderer replacement after acceptance |
| 3: data and resolver | Versioned sparse overrides, stable IDs, native loader, budgets, atomic parse, bounded field/receiver parameters and deterministic preview events | Source-group member records, explicit reload and migration fixtures |
| 4: first authoring workflow | Source inspector, torch reach, placed light/motes/free mist, emitter canvas picking/move/resize, supported-floor mist rectangle/erase tools, shared collision overlay, duplicate/reset, undo, INI transport | Native group member handles, richer material/slope support, combined scenery/effects project, small native/browser comparison clip |
| 5: established families | Shared native source/geometry paths; group tint/intensity/enable; authored fans, water/spray/drips/contours/clouds/exposure; particle regions; 19 recognized event families; separate scenery/player/enemy receivers | Individual native compound-member tuning, edited-silhouette occlusion, automatic material/contour recognition and family-by-family native visual acceptance |
| 6: later treatments | Bounded snow, sand, leaf, insect, scarab, spark, cloud and halo authoring behaviors/presets | Per-stage artistic rollout and native visual review; full refraction/heat/volumetric scattering experiments remain deferred |
| 7: hardening | Embedded WASM, fixed memory, resource reuse/teardown and invalid-input checks | Context recovery exercise, long-session/resize/DPR checks, offline browser acceptance, native Metal live-edit comparison, Deck/Vulkan and Windows/D3D12 measurements |

Supported-floor authoring now uses a shared exposed-floor predicate for native
cave defaults, authored recipe capture and the editor collision overlay. Painted
regions split at steps/gaps, settle at spike-pocket bottoms and respect ceiling
headroom. The renderer uses three clipped density slices behind actors, with a
conservative whole-room geometry budget and no additional textures. Native
flat/diorama tests include an authored-only alpha-mask regression. Browser paint, erase, undo/redo, INI import, live height editing and
regional native/WASM source comparison cover the lower temple. Redraws retain
uncommitted inspector input; room/terrain changes isolate source pickers.
Flat solid surfaces and verified Fillmore capitals are supported; other partial
and sloped shapes remain explicitly unsupported rather than being guessed.

Mote/handle validation additionally covers atomic parameter rejection, independent
emission/particle scale, the full cycle's alpha bounds, drift beyond birth-area
culling, stable identities during seed changes, typed inspector edits, map
move/resize undo and cancelled drafts. All 147 authored-file native/WASM round
trips include the new parameters. Browser image review at frames 37 → 111 → 37
restores identical preview pixels, with unchanged pass count and no warm-frame
uploads/allocations. Review examples are in `runs/action-editor-effects/`;
they are browser-only examples, not installed game overrides. These checks do
not replace native Metal, Deck/Vulkan or D3D12 measurements.

### Field, receiver and event authoring increment — 2026-10-01

Twelve authored families now cover lights, motes, free/floor mist, large particle
regions, fans, surface waves/glints, drips, waterfall spray, cloud banks, dimming
regions and wet contours. The inspector supplies validated family-specific
controls, depth placement and deterministic seed/age palettes. Region drawing
creates one record for up to 16384 × 16384 pixels, with density per 256-pixel
world cell and a fixed 64-cell visible-motion bound. It does not allocate native
actors or generate the entire room each frame. Wet contours are artist-authored
paths, not automatic slope or material recognition.

The eight region patterns include motes, dust, leaves, snow, sand, insects,
scarabs and sparks. Halo is a soft-light preset; cloud banks layer soft lobes to
suggest volume. Water surfaces animate crest geometry and glints without an
extra scene resolve; actual submerged-artwork refraction is deferred. This
delivers authoring behaviors for the later-stage roadmap, not a completed
artistic treatment of every room.

Light and dimming receivers independently select scenery, player and enemies.
Existing source records retain inherited behavior until explicitly overridden.
Native object ownership supplies one coherent hot-point tint for all sprite/OAM
and extended-apron parts. Color transforms are an atomic, optional runner ABI
facet; native/reference PPU output agrees and authentic comparisons remain
untinted. Moving accent lights retain their Action lighting gate and particle
trails remain independent. Native receiver preparation reads a copy of the
pending observation, without consuming ticks or modifying identity history.

The browser provides movable reference silhouettes and clock-based preview
events for all 19 registered actor accent families. These use the established
native geometry/phase validators; they do not simulate enemy AI or draw actual
actor artwork. Source ID zero edits the recognized family for that room/terrain,
including subsequent native generations. Event/probe positions are preview-only.

Coverage matrix:

| Established family | Editable coverage | Deliberate limit |
| --- | --- | --- |
| Forest canopy/front rays, leaves and motes | Native group enable/tint/intensity and light receivers; placed fans and leaf/mote regions | Native individual canopy openings retain their procedural layout. |
| Cave/temple water, sheen, drips, dust, mist, grit, ambient and tower light | Native group overrides/receivers; placed waves, spray, drips, contours, particle/cloud/mist/dimming regions | Artist paths and explicit spray anchors require visual review; no guessed collision changes. |
| Bloodpool moonlight, reflection, water, timber, air and clouds | Native group overrides/lighting receivers; authored fans, crests/glints and cloud/particle regions | Existing native occlusion and water-band recognition remain authoritative. |
| Castle windows, sky, mist, water and wall torches | Native group overrides/receivers, independent torch spill reach; additional opening fans/water/mist | Moving an authored fan does not move a native window or alter its source recognition. |
| Aitos lava, waterfall/mist, splash and torch families | Native group overrides/receivers; cloud/spray/water/spark presets | Native fourteen-splash and other family budgets remain intact. |
| Recognized projectile, electrical and boss accents plus landing dust | Nineteen native representative preview phases; enable/tint/intensity, light receivers where present | No new gameplay spawn, full boss AI, phase authoring or spell-controller replacement. |

Validation includes 147 regional rooms, 882 native/WASM surface/source matches,
147 twelve-family authored round trips, 22,172 valid recorded draws, reverse
seeking, retained resource reuse and atomic malformed-load rejection under
ASan/UBSan. Native tests exercise receiver isolation, glow-center ownership,
moving-light/trail separation, pause/peek clock ownership, ABI atomicity and
negative-margin sprite tinting. Editor tests cover one-record large-region and
contour strokes, one-step undo, independent receiver masks and unchanged scenery.
Browser review of an authored palace region, actor accent and player-only light
at frames 37 → 111 → 37 restores identical preview image bytes with zero warm
uploads/allocations. Target-device timing and matched live-game images remain
acceptance gates.
The retained receiver preparation/sampling stress fixture (three eight-strand
fans and 16 object samples) measured 0.221 ms CPU per frame over 1000 runs here.
It excludes PPU rasterization, observation and GPU work; it is not a target
hardware frame-time measurement.

Storage decision: retain semantic recipes in `action-effects.ini`, a text sidecar
next to settings. A 256-record fixture measured 41,492 text bytes, 63,492 retained
bytes and 0.729 ms CPU parse time averaged over 500 development-machine runs.
The parser runs only at load/import. A binary struct dump would reduce neither
rendering work nor retained memory, while introducing platform/layout and
migration hazards. Consider a separately versioned binary payload only for
future dense masks/grids/meshes/artwork, or a disposable hash-keyed cache after a
measured loading bottleneck. See the editor guide's storage discussion.

Next preserve source validation while making edited visible silhouettes available
to light occlusion; scenery stamps must not silently become gameplay collision.
Close native visual comparisons and target-platform measurements before marking
the whole editor plan complete.

The [editor guide](../tools/action_editor/README.md#environmental-authoring)
documents the file format, controls, limits and current omissions.

### Extraction inventory and remaining comparison inputs

These are the implementation owners and comparison inputs for the editable
coverage described above:

| Family | Existing capture / rendering owners | Inputs the enhanced fixture must preserve |
| --- | --- | --- |
| Forest canopy, leaves, motes, front/boss light | `action_forest_effect_capture.c`, `action_forest_effect_render.c` | Stable world field/seed, gameplay clock, front/rear attachment, room clip and projection |
| Cave pools/drips/sheen, waterfall mist, temple dust/bounce/mist/grit, tower light | `action_cave_effect_capture.c`, `action_cave_effect_render.c` | Validated wet-source mask, collision-supported floor spans including spikes, splash contacts, field bounds and independent time |
| Bloodpool water/moon/clouds/air/timber/mist | `action_bloodpool_effect_capture.c`, `action_bloodpool_effect_render.c` | Native water-row scroll, moon source/projection, pixel-silhouette occluders, timber/post details, lake/foreground distinction |
| Castle windows/gallery/exterior, moat and mist | `action_bloodpool_effect_capture.c`, `action_castle_effect_render.c` | Validated opening/sill source masks and groups, moon exposure, water strips, receiver masks and room clipping |
| Actor/projectile/torch/trap/boss and Aitos waterfall/lava accents | `action_effects.c`, `action_scene_effect_render.c` | Observed identities/phases, motion, tick history, source geometry and explicit preview events |
| Jump/landing dust | Observer/contact state plus cave/common render modules | Actor/contact identity, settled patch cooldown, emitted-puff age/seed and floor placement |
| Pass/blend/exposure/heat/projection | `present_action_effects.c`, `diorama.c`, effect backends | Explicit settings, ordered passes, masks, target sizes, GPU capability behavior and coherent projection state |

Capture and pass settings remain separate: Environmental effects owns scenery;
Action lighting/particles owns recognized actor accents. The existing pass table
distinguishes additive plane light from alpha atmosphere and supported-floor
mist, with separate foreground/world passes. Preserve those blend/order rules.

Current bounds include 16 actor accents, 27 decoration records, six landing
puffs, fourteen temple-mist spans, 2,048 moonlight occluder rectangles, 64 timber
and 32 post details. The geometry capacities in `action_effect_render.h` account
for larger waterfall/lava/lightning families explicitly. These are storage and
geometry bounds, **not** measured browser/GPU cost budgets. The browser port must
retain batching and add measured upload/submission/overdraw diagnostics before
accepting arbitrary authored emitter counts.

As each effect family joins the live level resolver, extend the native/browser
fixtures with its resolved sources, masks and clocks. Use the existing compositor
and WebGL2 backend for those comparisons. Fixture expansion supports the live
authoring work; it must not turn manual capture into a prerequisite for opening a
room or defer whole-level navigation until every effect has a recorded example.

## Authoring experience

Add an **Effects** workspace beside the existing map tools. It shares the room,
terrain, selection, camera and undo history. Keep common controls visible and
advanced attachment, occlusion and timing controls in an inspector.

The opening workflow is **load project → select stage/act/room → navigate and
scrub → edit → export**. Provide full-map navigation/minimap positioning and free
camera movement independent of a player or recorded frame. Changing aspect,
pixel aspect, zoom or extra tile ranges regenerates the needed view from room
data. Pause, play, single-step and a time slider operate on deterministic tile,
water-band and effect animation. Seeking to the same time with the same edits,
seed and preview events must reproduce the same scene, including when seeking
backwards. Stateful particle/event effects need deterministic reconstruction or
bounded checkpoints; they must not depend on the order of camera/time scrubs.

| Tool | Interaction and controls | Required behavior |
| --- | --- | --- |
| Lights | Click to place; drag elliptical reach handles; set color, intensity, falloff, flicker and receiver strength. | Select existing torches, window bounce and other sources. Radius and peak brightness are independent. Scenery and actor contribution can be tuned separately where the renderer supports them. |
| Rays and fans | Place an on/offscreen source, drag aim/reach/spread, edit an opening or grouped fan; choose soft wash or defined shafts. | Support canopy groups, front/rear depth, moon rays, arch/sill scattering, stacked windows and boss presets without camera-edge bending. |
| Mist and fog | Brush/erase coverage or edit bounded areas; set height, density, tint, softness, drift and variation. | Floor-following mist settles into spike pockets; free haze is an explicit alternative. Layered cloud shapes provide the existing volume approximation. |
| Particles | Paint emission areas or place emitters; set size, density, motion, lifetime, palette, seed and light response. | Cover dust, motes, leaves, grit, insects, snow, sand and supported sparks. Keep particle scale separate from emission-area size. |
| Water and wet surfaces | Mark surfaces/contours and splash contacts; set reflection strength, cap/glint density, ripples, drips and spray. | Follow owning layers and native water-band motion. Distinguish pools, distant lakes, foreground water, rock contours and waterfall bases. |
| Exposure and atmosphere | Paint feathered scenery-dimming regions; place cloud/veil fields and supported gradients or halos. | Preserve palette identity, readable actors and HUD. Bind cloud modulation to the intended light source. |
| Event accents | Select a recognized actor/event binding; adjust its existing preset, reach, intensity, particles and cooldown. | Preview jump/landing dust, projectile light, traps and boss accents without placing or modifying gameplay actors. |

Every effect has a readable name, stable ID, preset, enabled state, solo/hide,
reset-to-default, duplicate/delete and an indicator of its origin: bundled,
automatically derived, or user-authored. Bulk edits and each brush/drag gesture
are atomic undo steps. Changing room or terrain must retain unsaved work.

A floor brush stores bounded regions or simplified paths, not thousands of
individual puffs. A source inspector shows its origin, extent, source layer,
receivers and relevant blockers. Picking an effect remains possible when its
visible alpha is low or another layer covers it.

## Shared rendering architecture

The browser keeps the existing HTML/JavaScript controls. A narrow WASM API owns
scene resolution, effect resolution, animation and rendering. Native and browser
builds compile the same production modules; they differ at the host/GPU boundary.

1. **Scene inputs:** room assets, regional terrain, tile edits, camera, pixel
   aspect, extended-view settings, animation time and optional captured actors.
2. **Shared resolution:** native layer/raster/color-math rules, edited surfaces,
   framing, source validation, material/occluder information and effect recipes.
3. **Shared presentation:** capture metadata, camera and skybox mapping,
   compositor, pass policy, clipping, dimming and effect geometry.
4. **Backend:** existing SDL/native implementation or a new WebGL2 render-device
   and effect backend. Generate browser shader variants from the maintained
   shader sources; share calculations and test bindings/uniform layouts.

Reuse the production background capture/resolution functions where possible;
extract shared pure helpers where host/runner ownership prevents direct reuse.
Feeding the C compositor differently generated JavaScript textures would leave
an important source of preview disagreement and is not the final architecture.

Extract only the needed settings, clock, diagnostics, file and device ownership
from desktop dependencies. Keep a rendering library target separate from the
game loop, audio, installer, settings overlay and recompiled gameplay. Use explicit
inputs and module lifecycle/reset operations rather than a growing set of dummy
game globals. Native builds must consume these same extracted paths.

The editor interface passes commands and changed data, not individual pixels or
vertices. Keep textures, buffers, masks and reusable scratch resident; invalidate
only affected resources. Advance a deterministic preview clock, freeze it during
pause, and render at the same time after a retained-frame redraw.

WASM/native exchange uses a versioned fixed-width representation with checked
lengths and IDs. Do not serialize `FrameSlot`, native pointers, borrowed surface
addresses or GPU handles directly. Copy snapshot-owned bytes and rebuild resource
handles in the receiving backend. Validate memory growth/view lifetimes.

## Editable effect data

Use a versioned `action-effects.ini` alongside `diorama-layers.ini`, with a shared
C parser, validator and resolver used by native and WASM builds. Existing tile
configuration remains authoritative for scenery edits. Exact record syntax is
finalized with the first light/mist round-trip implementation.

Each authored record needs:

- Stable ID, room scope, terrain applicability, effect/preset version and an
  explicit add/override/disable relationship to a default source.
- Point, ellipse, segment, bounded polygon/path or surface-span geometry, with
  defined units and finite limits; bounds may include legitimate offscreen lights.
- Source attachment and coordinate space independently of render order:
  playfield BG, background/high/far band, midpoint-parallax field, sky/moon
  anchor, native water raster row, or validated actor/contact binding.
- Style values, color, intensity, receiver response, falloff, motion, stable
  seed and optional shared fan/phase group. Source radius and light reach are
  independent from particle count and peak exposure.
- Surface/material requirements, light blockers, receiver masks, optional
  clipping regions, and supported event conditions/cooldowns.
- Existing settings ownership and declared resource costs/capabilities.

Separate **authored recipes**, **resolved sources** and **captured render data**.
For example, a temple-floor recipe produces collision-supported spans; changing
terrain re-resolves those spans rather than leaving saved floating fog. Repeated
windows can be an editable group with member overrides. Native actor identities
remain in their current validated observer, not in an arbitrary user script.

Build a small typed preset registry with control metadata, validation, defaults,
attachment capabilities and conservative cost bounds. Generate editor controls
from that metadata where practical. Keep stage-specific source recognition and
artistic rules explicit; adding a preset should not require editing every
presenter or duplicating its renderer in JavaScript.

Missing files preserve current behavior. Migration first reproduces approved
defaults, then permits sparse user overrides; visual retuning is a separate change.
Derived sources get IDs from stable authored identities, not frame/array order.
An edited or deleted source must not regenerate as a duplicate on the next frame.

Keep bundled defaults separate from user override storage. Game upgrades must
not replace the user's authored effects. Provide deterministic import/export,
schema migration, reset inheritance, and a project export carrying both scenery
and effect configuration. Unsupported versions/records produce actionable
diagnostics and cannot silently disappear in a save round trip. Reload validates
a complete configuration before atomically replacing the active one.

## Attachment, occlusion and preview rules

- Expose material/contact and light-blocker overlays. Native priority, visual
  depth, physical collision and surface material are distinct facts. Decorative
  cave boulders must not become drip anchors merely because they share a BG.
- Default authored light blocking can use resolved visible pixel silhouettes,
  including transparent cutouts and editor changes. Physical floor/contact
  rules continue to use validated collision/material data. Stamped decorative
  scenery does not automatically acquire gameplay collision.
- Show invalidated anchors after terrain edits, with snap/rebind controls.
  Never silently move them to another regional layout. Share authoring only
  across compatible terrain families; previews identify the selected family.
- Keep source attachment separate from receivers and submission order. Airborne
  moon rays/clouds use continuous source mapping; distant wave caps follow
  native water rows. Foreground water remains when BG2-low is replaced by a skybox.
  Preserve layered light depth: a platform must not erase all light below it,
  and a foreground object covering the moon must not black out the whole scene.
- Offer shared-renderer flat and Diorama views, plane-only, skybox-only and both,
  plus matching zoom, pixel aspect, extended rows and camera controls. Preserve
  the original/native comparison independently of the authored preview.
- The compatibility/status panel reports unsupported or omitted effects. A
  missing shader or capability must not silently produce an apparently complete
  in-game preview. The map tools remain usable if the enhanced renderer fails.
- Add pause, single-step, time scrub, repeatable camera routes, reference actor,
  scripted jump/landing/projectile events and captured-scene playback. Clearly
  distinguish simulated editor events from a real gameplay replay.
- Actual boss states, fades and transient native windows require coherent game
  snapshots or replay evidence. A static room preview cannot prove those paths.

## Effect coverage and stage expansion

Track each family through default import, editing, native export/reload, browser
parity and visual approval. Grouped implementations still expose their meaningful
sources and parameters. No established family is silently left as an uneditable
C-only exception at project completion.

| Coverage | Existing effects to expose | Planned presets/capabilities |
| --- | --- | --- |
| Fillmore forest | Varied canopy rays, common-origin fans, front surface light, falling leaves, ambient/clustered motes, boss-clearing layout. | Reusable canopy/light-pocket groups for bamboo and jungle. |
| Fillmore caves, temple and tower | Pools/glints/ripples, waterfall flow/splash mist, drips and wet contours, mineral dust/grit, contact dust, floor mist, diffuse bounce, lower-temple dimming and tower arches. | General supported-surface authoring and enclosed-room atmosphere. |
| Bloodpool marsh | Layered moon fans, pixel-silhouette shadows, distant/foreground water light, native-row wave caps, timber highlights/drips, post ripples, insects, shoreline mist and cloud modulation. | Other source-linked reflection/veil treatments with explicit water palettes. |
| Bloodpool castle | Soft window wash and broken boss shafts, arch/sill/column/floor highlights, gallery/exterior moon rays, torch bounce, drifting dust, floor haze, moat shimmer and trap-light overrides. | Reusable window groups, stacked joins and palace lighting. |
| Aitos | Existing lava pit/reservoir light, heat treatment, waterfall/splash/veil and mist, plus existing recognized fire/rock accents. | Bamboo canopy, leaves, improved waterfall/lava spill; measured Diorama heat experiment. |
| Kasandora | Existing recognized torch, fire and projectile accents, after source-coverage audit. | Sun/sky gradient, distant heat, sand wisps, pit/contact dust, optional cloud/scarab fields, darker interiors, statue light, warm shafts and boss overhead dust/light. |
| Marahna | Existing temple/actor/projectile/boss accents and their bindings. | Darker green jungle, varied canopy light, humid mist, distant haze, arena fog and richer green light; optional foreground silhouettes. |
| Northwall | Existing recognized ice and boss accents. | Room-scoped snow, spindrift and cool surface highlights. |
| Death Heim | Existing rematch/boss accents without duplicating gameplay behavior. | Portal/eclipse halos and slow distant wisps, with arena/ending-specific scope. |
| Optional water experiments | Existing surface glints/ripples remain supported. | Actual distortion of submerged scenery and plausible caustics only after visible-benefit and performance approval; the removed water-artwork sampler stays removed. |

Environmental scenery retains **Environmental effects** ownership. Enemy,
projectile and boss accents retain existing Action lighting/particles controls.
Authoring their visual responses does not spawn enemies, change attack timing,
alter damage/collision, or consume native object slots. A new unrecognized actor
requires an audited native binding before it can be used as an emitter.

## Delivery sequence and exit criteria

Each phase is a reviewable implementation increment. The phase list is the work
order, not a claim that the feature already exists. Do not rewrite the full
renderer or change approved art during extraction.

### 0. Establish the comparison contract

- Inventory existing presets, source validators, effect/pass settings and costs.
- Record reproducible native scene fixtures with pixels/assets, resolved metadata,
  camera/settings, clocks and dynamic sources; define a portable snapshot format.
- Start with Fillmore cave/temple, Bloodpool marsh and castle windows, then forest
  and Aitos. Record known visual defects separately from migration regressions.
- **Exit:** fixtures replay in the native renderer with stable output and capture
  provenance; geometry, mask and image comparison rules are documented.

### 1. Run the real compositor in the browser

- Add a pinned, separate Emscripten build target and small host adapter.
- Compile the production compositor/projection/geometry and implement the
  required render-device operations and shader backends in WebGL2.
- Support actual blend semantics, texture filtering/addressing, priority-surface
  coverage, clipping, render targets, DoF, rim and skybox behavior. Audit any
  additional final post-processing used by the chosen comparison profile.
  Final output scaling/filtering and optional CRT need shared behavior before
  advertising agreement with those game settings; frame-generation comparisons
  must also use the same captured offsets and timing.
- **Exit:** identical recorded scenes render in native and browser paths with
  exact structural agreement and reviewed bounded pixel differences. Missing
  capabilities are visible. Object compilation alone does not close this phase.

### 2. Load complete levels and connect live map edits

- Load all stage/act/room choices from the project's full room assets and authored
  configuration without a scene capture or gameplay session. Feed the selected
  room, regional variant and in-memory INI edits through shared C resolution;
  retain reusable assets and rebuild only changed/visible scene resources.
- Add free X/Y camera positioning across the complete room, map navigation and
  deterministic animation-time seeking. Resolve native raster phases and the
  requested extended surfaces for each view rather than stretching or panning a
  previously captured frame. Keep data/loading boundaries ready for effect
  recipes and deterministic preview events in subsequent phases.
- Match camera/framing, native raster timing, aspect, extended rows, short rooms,
  copied scenery, pixel edits and far/normal/high classification.
- Replace the independent JavaScript Diorama renderer after parity acceptance;
  retain the ordinary map UI and original/native comparison.
- **Exit:** every exported room opens without a capture, the user can navigate
  its full extent and seek animation forwards/backwards repeatably, and changes
  to view coverage regenerate the required art. Tile/depth/framing edits update
  the shared preview immediately and match the exported game's view in flat and
  all three Diorama backdrop modes. Exercise all 49 rooms, with detailed scrolling,
  seeking and edited-surface comparisons on representative room families.

### 3. Establish editable effects and preserve defaults

- Implement the versioned schema, shared resolver, stable IDs, registry,
  validation, persistence, reload and default/override semantics.
- Migrate representative existing torch/window light and supported-floor mist
  into recipes while retaining source validation and output.
- Introduce coherent preview event inputs instead of relying on fabricated WRAM
  or linking the entire game just to place a light.
- **Exit:** unchanged defaults match native baselines; edits survive round trips,
  reload and default upgrades; malformed or oversized changes are rejected
  atomically without corrupting the active scene.

### 4. Deliver the first useful authoring workflow

- Add effect picking/inspector, light handles, floor/free fog brush, overlays,
  presets, duplicate/reset/disable, undo/redo and explicit export/reload feedback.
- **Exit, user exercise:** select a Bloodpool torch and widen its reach without
  increasing peak brightness; add a soft light; paint cool Fillmore floor mist
  across ledges and spike bottoms; undo, export, reload and compare in game.
  Verify skybox-only and plane presentation, retained-frame stability and settings
  ownership. Supply a small native/browser comparison video.

### 5. Cover every established effect family

- Migrate the remaining forest, cave, marsh, castle and Aitos source groups in
  small stages, adding ray/opening, water/contour, particle and exposure tools.
- Expose recognized actor/event accent parameters and deterministic preview
  actions. Preserve material recognition, regional applicability and native clocks.
- **Exit:** every existing level-effect family has a coverage-matrix entry with
  editable defaults, working export/reload, native/browser tests and review
  evidence. Existing compound effects are selectable and retain their variation.

### 6. Support later-stage treatments through the editor

- Follow the level roadmap: Kasandora, Marahna jungle/boss, Aitos refinements,
  Marahna temple, Northwall, then Death Heim. Reuse the catalog and extend shared
  kernels only when a required behavior is missing.
- Add snow/sand/scarab/cloud/halo presets and optional scenery placement using
  the same data, bounds, undo and rendering contracts.
- **Exit:** every planned baseline family has a working preset/tool demonstrated
  in a representative intended room and verified in the native game. The complete
  artistic rollout across all rooms remains in the stage roadmap; it is not a
  prerequisite for completing the authoring tools. Optional heat/refraction/
  silhouette experiments have explicit accept/defer decisions with evidence;
  do not silently count them as shipped or make unproven experiments prerequisites
  for basic authoring.

### 7. Harden and package

- Bundle the WASM module, shader outputs and version metadata with the editor;
  normal users do not install a compiler. Prefer a single-threaded preview first.
- Preserve offline use. Prove an embedded single-file package works, or provide
  a self-contained local launcher with a documented migration from `file://`.
  Avoid external runtime downloads and undocumented serving/header requirements.
- Check browser context loss/recovery, resizing, device-pixel ratio, memory growth,
  long sessions, repeated room switching and invalid/old project imports.
- **Exit:** supported browser and native-target checks pass; authoring/export
  documentation, diagnostic information, reproducible captures and user review
  are complete. Native hardware performance cannot be closed by browser tests.

## Performance and correctness acceptance

Use measured baseline scenes and dense authored stress scenes. Decide numerical
CPU/GPU/interaction budgets from those measurements and record them before
raising capacities or accepting new full-screen effects.

- Reuse batches by pass/material and existing retained scratch. Avoid per-emitter
  texture uploads, geometry submissions, scene resolves or shader compilation.
  Cull offscreen sources before expensive construction; account for raster-band
  repeats, clipping expansion and transparent overdraw.
- Editor diagnostics show visible sources, particles, geometry, uploads/calls,
  target switches and estimated transparent coverage. Label measured timings
  separately from estimates; a particle count is not a GPU-cost measurement.
- Current host decoration capacity is 27 records, actor accents 16, and the Aitos
  splash family remains capped at fourteen. Authoring recipes may describe a
  whole room, but visible output needs a deliberate bounded aggregation/storage
  design. Do not merely increase every list or evict active actor effects.
- Validate worst visible-window cost at compile/export where possible and enforce
  runtime bounds. Over-budget edits give a useful error or an explicitly previewed
  deterministic quality reduction; no silent flickering or partial scene loss.
- Preserve shader/blend capability fallbacks. Retained frames, pause, clock wrap,
  load/room transitions and effects toggles must not advance or corrupt state.
- Require exact comparison for decoded surfaces/masks/ownership and stable draw
  structure where applicable. Use documented tolerances for GPU filtering/color
  differences, plus motion review; never promise bit-identical pixels across GPUs.
- Cover US/JP/EU terrain applicability; 32/64 and redistributed rows; square/CRT
  pixels; common aspect ratios and camera extremes; plane-only, skybox-only, both,
  hidden/alpha-zero plane fallback and flat mode. Include transparency, fades,
  short-room bounds and waterfall continuations.
  Run automated room smoke coverage across all 49 exported rooms; exercise the
  larger setting matrix on representative raster, terrain and effect families.
- Run native Metal, Steam Deck/Vulkan and Windows/D3D12 measurements using the
  shipped SDL version, fixed power/pacing and matched settings. Include frame-time
  spikes, resource growth and upload/submission counts. Browser WebGL results do
  not predict those backends. Preserve generic Linux and supported architecture
  builds, and record actual hardware/browser coverage honestly.
- Gameplay-memory comparisons and observer tests must prove effects never allocate
  native actors or mutate collision/gameplay. Use controlled replay clocks/audio
  scheduling so an unrelated asynchronous timing difference is not misreported
  as rendering corruption.

Release evidence includes small, download-friendly clips (about 480px wide),
before/after or browser/native comparisons with identical camera/time, and labeled
preview events or traversal assists. Larger diagnostic captures remain optional.

## Completion checklist

- [ ] Shared production scene/render code drives the browser preview.
- [ ] Complete levels load without captures; full-room navigation, regenerated
      view coverage and deterministic time scrubbing work with authored edits.
- [ ] Baseline captures and edited scenes meet native/browser parity gates.
- [ ] Existing effects are discoverable, editable and resettable without C edits.
- [ ] All established families pass the coverage matrix and persistence checks.
- [ ] Later-stage baseline treatments are authorable; optional experiments have
      explicit status and extension points.
- [ ] Flat, plane and skybox compatibility is verified beyond the initial rooms.
- [ ] User overrides survive upgrades and projects export/reload reproducibly.
- [ ] Authoring limits prevent native-pool interference and rendering overload.
- [ ] Browser packaging, target-platform measurements and user visual review pass.

Continue from the October 1 status table: native compound-member tools,
material/slope overlays and visual acceptance. Close phase 2's
representative live-scene image/edit comparisons before replacing the legacy
Diorama view, and keep native target-platform acceptance separate from browser
and CPU test results.
