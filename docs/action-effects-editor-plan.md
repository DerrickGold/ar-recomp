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

Priority update, 2026-10-01: finish the remaining native-default migrations and
planned editor effect capabilities before implementing camera-aligned editing
or a detached live shared-renderer window. Both view improvements are queued in
phase 6a below; they must not displace the current effects work.

### Native defaults must be reconstructible from configuration

Required by the user on 2026-10-01: bundled level effects must be ordinary,
editable definitions in the same effects suite used for authoring. They must be
reconstructible from configuration with the native default definitions disabled,
without losing appearance, behavior or rendering performance. Editable overrides
of a C-owned effect do not satisfy this requirement.

The current implementation does **not** yet meet this gate for all defaults.
Complete configuration definitions now cover forest rays; cave water and
atmosphere; Bloodpool moon, marsh and castle groups; torch/temple glows;
trap, wizard and centaur lightning; fireball, orb, jungle-fire and lava-fire
responses; and Aitos lava, splash, waterfall and waterfall mist. These families
have independent pre-migration geometry oracles as well as native/WASM checks.
Other actor phases, landing dust and spell-controller visuals still contain
C-owned recipes. Their sparse overrides and representative preview events do
not count as complete reconstruction. Sixteen freely placed emitter families,
generic actor bindings and later-stage composition presets extend authoring;
matching a family name alone is not evidence of native fidelity.

The implementation contract is:

- Bundled defaults and user-created effects use the same versioned semantic
  recipe format, resolver, reusable kernels and editor controls. Exporting a
  default must preserve its complete definition and dependencies; importing it
  into a room with that default disabled must reproduce it. Reset resolves the
  bundled recipe, rather than re-entering a separate hard-coded visual path.
- Extend the suite with reusable capabilities when a default cannot be expressed:
  compound sources, sampled opening/falloff profiles, shared-origin ray groups,
  linked light/cloud/particle/water responses, explicit layer/priority attachment,
  contextual animation clocks and configurable event responses. Do not implement
  an opaque "native preset" or a room-name switch that secretly calls the old
  effect. Specialized optimized kernels are allowed when their defining data is
  available to arbitrary authored recipes and editable with the same controls.
- Keep source discovery and gameplay observation separate from visual recipes.
  Existing map-material validation, floor support, raster-row offsets, actor
  bindings and event facts remain read-only inputs. Recipes need explicit,
  reusable bindings to those inputs, including source witnesses and dependency
  IDs where needed. Configuration never spawns actors or changes collision.
- Compile and validate configuration at load/edit time into bounded retained
  data. Preserve aggregation, culling, deterministic seeds, gameplay-clock
  ownership, source-relative transforms, blend modes and receiver gates. Moving
  twelve native rays into twelve separately submitted emitters, raising pool
  limits, or adding per-frame parsing/allocations is not an acceptable migration.
- Sparse user overrides may remain layered over bundled definitions for upgrades;
  a complete editable definition must also be exportable. Store semantic data,
  not captured meshes, C struct dumps or per-frame vertex recordings.

Migration proceeds family by family while preserving the working renderer as a
comparison oracle. First capture matched unmodified baselines, then extract the
missing reusable data/controls and compile bundled defaults through that path.
Remove each legacy visual default only after its independent reconstruction
passes. Do not claim the editor complete while any approved default relies on
an inaccessible visual definition.

For each migrated family, compare the legacy baseline against a fresh config-only
load with native defaults disabled, then export/reload, move, duplicate, retune
and reset it. Exercise complete rooms, camera extremes, forward/reverse time,
pause, regional terrain, edited source art, plane/skybox modes and receiver
selection. Compare source identities, deterministic geometry, colors, indices,
pass/blend/depth structure and motion before using GPU image tolerances. Verify
that dependencies survive duplication without pointing back to native defaults.

Performance acceptance requires unchanged visible geometry and draw/submission
counts, uploads, scene resolves, allocations and retained resource bounds for
equivalent definitions. Measure capture/resolution, mesh generation, receiver
sampling and GPU time separately with identical scenes and settings. Timings
must show no reproducible regression beyond measurement noise; equal counts
alone do not prove equal performance. Native Metal, Deck/Vulkan and Windows/D3D12
checks remain required, including dense authored combinations. Browser parity
cannot substitute for target-device measurements.

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
| 3: data and resolver | Versioned recipes and sparse overrides, strict codec, stable IDs, bounded retained data, full definitions for migrated scenery/arc/projectile families | Remaining actor/contact/spell recipes, complete dependency exports, explicit reload and migration fixtures |
| 4: authoring workflow | Context add/edit/delete/preview, popup inspector, native markers, drag/resize, multi-effect copy/paste, repeat placement, mist/particle region tools, actor family/attack selectors, undo and INI transport | Compound group handles, material/slope tools, combined project and native/browser review clip |
| 5: established families | Config-only forest/cave/Bloodpool/castle/glow/Aitos surface families, three arc and four projectile families, generic actor attachments and independent receivers | Remaining native actor/contact/spell defaults, target rendering-cost acceptance and gameplay visual review |
| 6: later treatments | Snow, sand, leaf, insect, scarab, spark, cloud, flame, torch, halo and gradient authoring; ten stage compositions | Per-stage artistic rollout; diorama refraction/heat and full volumetric scattering remain experiments |
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

| Established family | Editable coverage | Remaining limit |
| --- | --- | --- |
| Forest canopy/front rays, leaves and motes | Complete `ray-field`; openings, profiles, clocks, colors, particles and witnesses; member handles | Target-device timing and final image review. |
| Cave/temple water and atmosphere | Complete `water-field` and `atmosphere-field`; contacts, contours, dust regions, grit, mist support, lighting and exposure | Landing dust event recipe is separate and still C-owned. |
| Bloodpool moon, water, timber, air and clouds | Complete linked `moon-field` / `marsh-field`; fixed BG2 point or raster-row attachment | Target-device and full edited-room visual acceptance. |
| Castle windows, gallery, boss, moat and torches | Complete `castle-field` and `glow-field`; opening joins, source witnesses, material response, glow profiles, independent receivers and exposure | Compound group handles remain less direct than ordinary emitter handles. |
| Aitos lava, heat, waterfall/mist and splash | Complete five surface fields; map rules or manual sources, glow/particle/cloud profiles and heat coefficients | Existing heat pass is flat-mode only; the shared diorama preview does not show refraction. |
| Actor, projectile, electrical and boss accents | Complete trap/bolt/centaur and four projectile fields; generic attachment of authored effects to any visible family/player, including previously unrecognized attacks | Remaining native phase recipes, landing events and spells still need extraction. Preview uses a synthetic actor sample, not AI or sprite artwork. |
| Reusable cloud/fire and later-stage atmosphere | Cloud bank, flame, native-profile torch, halo and gradient; ten editable stage compositions | Per-stage art rollout and gameplay validation remain separate. |

### Native-default reconstruction audit — 2026-10-01

| Native behavior | Current data ownership | Remaining acceptance work |
| --- | --- | --- |
| Forest canopy/front light and boss clearing | Complete `ray-field` configuration, unchanged aggregate passes | Native target timing and image review. |
| Cave pools, waterfall contacts, wet rock and drips | Complete `water-field`, including 33-column contours and material witnesses | Native target timing and image review. |
| Cave/temple atmosphere and tower light | Complete atmosphere source, motion, floor-support and exposure profiles | Extract actor landing/contact response; hardware acceptance. |
| Bloodpool moon/cloud/distant lake | Complete `moon-field` with linked shadow, cloud, reflection and row-motion response | Hardware acceptance. |
| Bloodpool foreground water, timber, shoreline and insects | Complete `marsh-field`, preserving moon linkage and material selectors | Hardware acceptance. |
| Castle windows, gallery and boss | Complete room-specific `castle-field`, including arches, joins, columns, mist, water, dust and exposure | Hardware acceptance and compound handle UX. |
| Torches and temple glows | Complete `glow-field` source rules/manual points and multi-component profiles; placed `torch` uses the same kernel | Copied torches use the destination room's shared glow profile. |
| Aitos lava, heat, splash and waterfalls | Complete `lava-pit-field`, `lava-lake-field`, `splash-field`, `waterfall-field`, `waterfall-mist-field` | Heat is flat-only; retain that limit until a measured diorama implementation exists. |
| Trap, wizard and centaur lightning | Complete `trap-field`, `bolt-field`, `centaur-field` with phase/profile/path data | Hardware acceptance and native gameplay visual review. |
| Fireball, statue orb, jungle fire and lava fire | Complete four projectile response fields | Other native actor responses and spell phases remain C-owned. |
| Landing dust and other actor/spell responses | Generic actor selectors are available; the remaining per-phase recipes, event variation and contact cooldowns still need extraction | Preserve read-only observation, detached bursts and native actor budgets. |
| Scene dimming and receivers | Migrated atmosphere/castle exposure and per-field/emitter receiver controls are data-driven | Audit any remaining stage-specific treatment during that stage's migration. |

The migration inventory includes defaults outside the decoration list, such as
scenery exposure and actor/contact responses. A complete room export must include
every required visual dependency, not merely the currently visible markers.
Additional effect families must meet this same contract when introduced.

### Actor authoring, Aitos surfaces and repeat placement — 2026-10-01

Authored emitters can bind to the player or a native actor family, with optional
parent, animation, state, visual-frame and handler filters. Observation covers
previously unrecognized families, without allocating native actors or modifying
WRAM. A room actor picker is generated from the canonical descriptor tables;
custom family IDs cover runtime-created attacks. Bloodpool Act 1 boss fire has
an audited selector preset (`B786`, parent `B786`, animation `5000`, states 0–1).
Raw-memory and selector tests cover this identity; actual boss gameplay visual
review is still pending. Attachment preview supplies one synthetic actor sample.
Its marker/drag uses actor-local offsets and requests the required object plane,
even when the scene has no sprite at that depth.

The reusable `flame` supplies a warm body, spill and independently seeded embers;
`torch` reuses the configured native flame kernel. Cloud banks remain freely
placeable/attachable. Shift-click selects several effect markers; Ctrl/Cmd-C and
Ctrl/Cmd-V support repeated click placement, with fresh IDs and one undo per
paste. Native torch points can be copied into ordinary authored torches. Copying
whole compound definitions is not yet a map gesture; edit their source lists.
BG1/BG2 selections remain in their owning coordinates. Actor copies retain their
selector/offset; placement does not turn them into map sources.

Aitos source selection now resolves from complete bounded surface definitions,
using material rules or explicitly placed sources. Rendering retains the native
fourteen-splash limit, twelve lava glow segments and existing cloud/particle caps.
Heat coefficients feed the existing retained flat refraction mesh and target;
edits invalidate the mesh without allocating another target. Glow/spark and heat
enables are independent. No new render passes or per-emitter textures were added.
Receiver preparation skips frame copies and receiver meshes when no actor lights
are enabled. Config parsing and style decoding remain outside frame rendering.

Validation includes 19 focused native suites, editor model/build tests, strict
WASM compilation and sanitized whole-room checks across 147 regional rooms:
882 native/WASM surface/source matches, 147 authored round trips and 30 stage
preset reconstructions. The Aitos legacy oracle matches 1,200 camera/shape/clock
and component cases exactly (`957be29d`, 559,800 vertices / 2,220,480 indices).
Seven alternating host CPU trials of geometry plus hashing measured medians
0.011624 ms/case before and 0.011782 after, with overlapping ranges
(0.011519–0.012504 / 0.011542–0.013073). These exclude GPU work and do not close
Metal, Steam Deck/Vulkan or Windows/D3D12 acceptance.

### Grouped ray-field reconstruction increment — 2026-10-01

The first migrated family uses complete `[field:GG:RR:T:ray-field:ID]` records.
The canonical forest document supplies all opening/fan/profile data, particle
regions, counts, colors, motion, clipping, source dimensions and material
witnesses. The embedded default is generated from this document and parsed once
through the same strict codec. No forest visual table remains in C. Room selection
chooses the default definition; generic capture/rendering also work in another
room with explicit bindings. Captured frames own copied data, so subsequent
imports cannot change retained frames. A configured field suppresses the native
capture and keeps the original three aggregate decorations and passes.

The inspector extracts the complete field, exposes its numeric/vector controls,
and preserves untouched round-trip values. Existing member handles follow its
ray positions and count; sparse enable/tint/transform/receiver overrides remain
independent. Apply, Cancel, Undo, reset and disable use the existing transactional
workflow. New placements convert both axes through the room's light-layer
scroll ratio. One aggregate field per room/terrain is allowed, with up to twelve
openings, four origin groups and two profiles. Adding another ray uses that
field rather than introducing another draw.

Validation pins pre-migration vertex/color/index digests for 210 combinations
of camera, time, projection and lighting/particle gates. Tests also cover a fresh
empty-frame reconstruction, another room, full export/reload, mutable controls,
source-art rejection, zero components, retained frames and invalid definitions.
All 147 regional rooms and 882 native/WASM surface/source comparisons pass under
ASan/UBSan; all three regional forest definitions reconstruct identically, with
22,298 valid recorded draws across the suite. Browser review confirms the complete
definition affects the shared renderer without warm uploads or allocations.

An optimized Mac CPU comparison against the earlier forest kernel measured
approximately 0.0156 ms/frame before and 0.0158 ms/frame after across seven camera
positions and five clock values. The geometry digest and total vertices/indices
were identical. Shared-origin sway is computed once per fan. The roughly 0.0002 ms CPU increase is recorded; equal GPU resource
and draw counts are not evidence of identical timings. Metal, Deck/Vulkan and
Windows/D3D12 timing/image acceptance remains open. This earlier increment did not close the full native-default gate. Later
scenery migrations are listed in the current audit above; remaining actor/contact
and spell responses must be completed before the queued view work.

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

### Linked wet-surface field reconstruction increment — 2026-10-01

`assets/effects/cave-water-field.ini` now owns the complete cave water definition:
three pools, four waterfall splash contacts, eight wet contacts with exact material
IDs, 33-column rock contours including open gaps, mandatory map witnesses and
all glint/ripple/drop/sheen/glow/layered-spray visual parameters. There is no native
pool or contour catalogue left in C. The canonical document is embedded by the
same reproducible generator as the forest field, then parsed once into typed,
owned data. Contacts and contours are prepared at load time; retained frames
perform no parsing or float-to-contour conversion.

The reusable `water-field` captures four aggregate records through the shared
production kernels. Read-only material validation suppresses a removed wet
contact and its linked drip/patch; mandatory room witnesses fail closed for the
whole field. Config selection suppresses native capture before construction.
Replacement retains the native layer submission order relative to dust/grit,
contact bursts and floor mist. The same definition works in another room without
a room-name visual switch. Existing source/member receiver controls remain
separate from the complete definition.

The editor can extract the complete default from any cave-water/drips/mist/sheen
family. The modal exposes each contact, contour column, palette, timing profile
and mist slice parameter. Native marker bases follow edited contacts, and existing
member offsets remain relative to those bases. Disable keeps the field disabled;
reset restores the bundled definition. The placement palette can start a simple
pool field in any room, using the owning background's scroll conversion. Numeric
sampling/count limits preserve the existing geometry budgets, including member
scale and offset bounds; other wet contacts can be enabled within those limits.

Validation pins the original vertex/color/index digests for 288 combinations of
camera, time, projection and lighting/particle gates. Fresh empty-frame loading,
other-room reconstruction, export/reload, actual visual parameter changes,
zero components, missing artwork, retained frames and atomic malformed/incomplete
imports pass. All three regional cave definitions reconstruct exactly in the
native/WASM whole-room suite: 147 rooms, 882 comparisons, 147 authored round trips
and 22,451 valid recorded draws, with ASan/UBSan. The transactional editor tests
cover extraction, linked marker bases, apply/cancel/undo, disable/reset and
owning-layer placement. Browser inspection covers an edited pool definition and
the native waterfall splash with no console errors.

An optimized Mac geometry microbenchmark against the immediately preceding
kernel produces identical geometry digests and 26,341,196 vertices / 56,169,414
indices over seven rounds. Across repeated runs, warm samples ranged around 0.0048–0.0058 ms/frame before
and 0.0049–0.0055 ms/frame after. Some runs measured a roughly 0.0002 ms increase
and others a small decrease; this short CPU sample does not establish equal
target-hardware timings. The field adds no render passes, GPU resources, warm allocations or
uploads; complete native Metal, Deck/Vulkan and Windows/D3D12 image/timing
acceptance remains open. Cave atmosphere was migrated in the following increment;
exposure and actor/contact response remain ahead of the other editor polish.

### Cave/temple atmosphere reconstruction increment — 2026-10-01

Three canonical documents (`assets/effects/cave-atmosphere-field.ini`,
`temple-atmosphere-field.ini`, and `tower-atmosphere-field.ini`) now own every
visual parameter of their five atmosphere families. Ambient pool positions,
radii, lean, exposure and surface gain are explicit. Dust covers a union of up
to three rectangles with editable grid density, motion, size, color and a lower
hall density band. Grit specifies individual ceiling/landing contacts and seeds,
fall timing, and the grain burst's color, shape, diffusion and lifetime. Floor
mist specifies its collision-search region, height and all three density slices;
the search still sinks through non-colliding spikes to supported ground. Tower
light owns its three origins, five-row width/intensity profile and cloud motion.

Native selection loads the same complete bundled text used by the editor.
The render kernels contain no room-specific light/dust/grit/profile tables.
Loaded definitions cache contiguous source vectors once and retained frames
own their data. Atmosphere and splash mist share one mesh kernel through explicit
style views, without falling back to another field's visual defaults. Water and
atmosphere share the strict vector codec. Counts and region sizes retain fixed
record/geometry ceilings; malformed or incomplete documents fail atomically.
Flat-mode mask selection now follows enabled field components: atmosphere only
needs BG1 for floor mist, forest uses BG2, and water uses its actual owning
layers. Exporting a native field no longer requests both masks indiscriminately.

The editor exposes **Edit complete atmosphere definition…** from temple dust,
cave light, grit, floor mist and tower sources. The add-effect palette can create
a generic atmosphere field in any room; the modal exposes all contacts and
coverage vectors with labeled units. Existing source overrides still control
receiver masks and tuning. Definition edits, Apply/Cancel, disable/reset and
Undo preserve the complete data. Individual atmosphere sub-source drag handles
remain a later usability improvement; current editing uses the complete modal.

Validation: 1,008 fixed room/camera/clock/toggle/projection combinations match the
independent pre-migration vertex/color/index digests (`161e87d6`, `628197c5`).
Fresh config-only capture, reconstruction in a different room, export/reload,
actual parameter edits, zero components, retained-frame ownership, invalid
inputs, missing room witnesses and mist following edited support all pass.
All 147 regional room fixtures pass ASan/UBSan with 882 native/WASM surface/source
comparisons and 22,847 valid draws, including complete definitions in the three
Fillmore rooms for all terrain variants. Browser testing confirms the temple
modal extracts and applies an ambient radius edit without console errors.

A Mac `-O2` geometry microbenchmark over seven rounds of 7,000 frames preserves
11,381,167 vertices / 27,319,929 indices and digest `0179ee3d`. Source lookup in
particle loops initially added measurable CPU cost; load-time decoding removed
that overhead. Latest warmed samples are roughly 0.0095–0.0100 ms/frame before
and 0.0095–0.0098 after (first configured sample 0.0110). This is a CPU kernel
check, not GPU or full-frame acceptance. No new draws, textures, render targets,
warm allocations or uploads are introduced. Metal, Deck/Vulkan and Windows/D3D12
capture/timing acceptance remains open.

Next after this increment: Bloodpool moon/cloud/water/timber definitions, then castle opening/gallery
profiles, compound glow/flow families, actor/contact responses and exposure.
The full native-default reconstruction gate remains open.

### Marsh and castle reconstruction increment — 2026-10-01

Bloodpool's foreground water/shoreline mist, timber lighting/drips, post ripples
and insects now use `assets/effects/marsh-field.ini`. The seven Act 2 rooms use
`assets/effects/castle-{2..8}-field.ini` for their window sources and sampled
arch contours, stacked joins, soft/broken fan profiles, opening/sill/pillar/floor
highlights, torch bounce, drifting dust, floor haze, exterior moon and moat water.
Each castle definition owns a complete sky-ray profile. Neither family dispatches
visual styles from an authored room number or borrows C-owned visual defaults.
Native source-art/material/collision checks remain read-only observations.
Scenery dimming, world-space depth ramps and scenery/player/enemy receiver
choices now live in the atmosphere/castle definitions too. The original cave
light receiver behavior is preserved until explicitly changed. Version-1 fields
saved before those controls are upgraded at load, then export the complete data.
Native flame accents and actor/trap responses remain pending migrations; this
does not close the whole native-default gate.

The editor offers complete-definition extraction and new placement in its modal,
with component toggles and named vector controls. Apply/Cancel, reopen, undo/redo,
serialization and generic-room capture are covered. The marsh definition keeps
its water/material dependencies; castle sources include stable variation IDs,
arch profiles, source/sill witnesses and clip bounds. A real browser smoke passed
marsh/castle extraction, a component edit, apply, reopen and undo. Existing map
member handles still edit sparse native member overrides; full compound handles
remain in the subsequent usability work.

The saved pre-migration renderer oracles match exactly: marsh 720 cases,
828,519 vertices / 3,063,978 indices (`004603af` optimized); castle 840 cases,
260,256 vertices / 906,489 indices (`d6a30baa` optimized). Full-room comparison
caught a cloud-mask ordering dependency when marsh capture followed moon capture;
the linked witness mask is now refreshed in either ordering. Seven castle defaults
also pass independent config-only reconstruction and owned-data round trips.
Fourteen focused C tests, editor interaction/build tests and the full native build
pass. ASan/UBSan regional checks cover all 147 room variants, native/WASM surfaces
and source definitions, including complete castle definitions in 21 variants.

No draw/texture/target limits were raised. Arch samples and ray profiles prepare
at load time; frames retain owned values, with no configuration parsing or heap
allocation in capture/render. Alternating local Mac CPU geometry-plus-hash probes
measured marsh medians 0.11067 ms before / 0.11111 ms after, and castle 0.01303 /
0.01341 ms, with overlapping timing ranges. These are small CPU fixtures, not
GPU/frame-time acceptance or proof about Deck/D3D12. Target hardware and broader
capture/presentation timings remain open.

### Linked moon-field reconstruction increment — 2026-10-01

`assets/effects/moon-field.ini` is now the canonical definition for Bloodpool's
BG2 moon, cloud veil and distant reflections/wave caps. Native defaults and
exported definitions use the same kernels. An override suppresses the bundled
visual capture; material detection for the remaining marsh effects continues.
The capture owns its prepared data and preserves native alpha submission order.
Definitions can also be placed in other rooms without a Bloodpool room gate.

The definition exposes the BG2 source, six low and five middle ray profiles,
haze gains/palette, source radius and shadow depths, cloud motion/density/shape,
reflection rows/glow/color, and native-raster wave rows/glints/seeds. Source and
cloud retain a fixed BG2-point projection; water follows the retained HDMA rows.
Foreground water, timber and insects consume the edited projected moon profile.
Their own receiver-material/particle recipes remain a separate migration.
Shadow coverage dimensions, mesh resolution, maximum ray/crest counts and
submission budgets stay fixed. Parameter bounds prevent expanding those budgets.

The popup has individual component toggles, labeled source/profile controls and
transactional Apply/Cancel. Extract via **Edit complete moon-field definition…**,
or add **Linked moon, cloud veil and water glints (BG2)**. The linked field has
one draggable BG2 source marker; new placement uses the clicked BG2 coordinate.
Drag, popup edits, disable/reset, export/reload and undo are tested. Moving this
source changes the illumination, not the moon's painted artwork.

Validation: 12 focused C tests; editor interaction/build tests; ASan/UBSan across
147 regional rooms with 882 native/WASM surface/source matches and 23,502 valid
draws, including full moon reconstruction in all three terrain variants.
The independent pre-migration projection fixture retains exactly 5,716,240
vertices / 22,331,610 indices: optimized digest `4b12f297`, unoptimized digest
`ec229576` (different compiler floating-point evaluation, each pinned before
migration). Generic-room reconstruction, retained ownership, HDMA changes,
component gates, missing witnesses and atomic rejection are covered.

Clock-independent radial/angular profiles are prepared once at load, avoiding
extra math in the mesh loop. Three alternating Mac `-O2` CPU runs (five rounds
of 360 frames each) measured about 0.600–0.613 ms/frame before versus
0.564–0.571 warmed after, with identical geometry/digest. This measures geometry
plus hashing, not capture or GPU frame time. No new draws, render targets,
textures, per-frame parsing, allocations or uploads were added. Native Metal,
Steam Deck/Vulkan and Windows/D3D12 image/timing acceptance remains open.

Next: compound glow/flow families, actor/contact responses and exposure. Compound source handles and the remaining editor UX follow complete definitions; the view improvements remain queued in phase 6a.

### BG2 source anchoring increment — 2026-10-01

Authored effects now specify `anchor=bg1|bg2-point|bg2-raster`, independently of
draw placement. A BG2 point remains fixed in layer coordinates and follows its
camera/projection. In skybox mode its entire mesh uses the source row's transform;
water raster bands cannot bend an airborne beam or cloud. Raster binding instead
preserves native row motion for surface effects. The projection code uses an
explicit retained-source flag, replacing its special moon/cloud kind check.
Native Bloodpool capture opts its moon and cloud into that same generic policy.

The editor supports right-click placement, selection, dragging, resizing and
transactional modal configuration on BG2, plus an apply-and-locate action. BG2
preview preserves the foreground camera because a static background point has
no unique inverse foreground position. Collision-supported mist, dimming regions
and traced playfield contours remain BG1-bound. Native moon/cloud catalogue
markers appear at `(112,62)` on BG2. The following moon-field increment adds
their full editable coupled definition; this anchoring increment alone did not.

An audit found two related authored-only gaps: BG2 alpha submission ignored
authored records, and the requested Diorama callbacks could omit an effect's
draw attachment when it differed from its source plane. Both now include the
existing authored list/attachment. Required-mask queries no longer request both
background masks indiscriminately for every placed emitter.

Validation covers 81 kind/anchor/placement combinations, invalid bindings,
retained ownership, independent BG1/BG2 camera movement, flat/plane/skybox/both
projection, two output aspects, extended rows and raster seams. Presenter tests
verify authored-only clouds on plane and skybox paths, alpha-correct flat masks,
and no added textures, resolves or intermediate targets. Browser verification
uses the real WASM validator and BG2 modal/preview.

A Mac `-O2` comparison over 1,800 native moon/cloud/reflection frames preserves
5,716,240 vertices, 22,331,610 indices and geometry/color digest `4b12f297` against
the pre-change projection. Five 360-frame CPU samples (including digest work)
measure 0.591–0.612 ms before and 0.590–0.630 ms after; warmed medians are about
0.60 ms. This is not a GPU or target-device benchmark. No new parser work occurs
per frame, geometry budgets are unchanged, and Deck/Vulkan, Windows/D3D12 and
native full-frame timing acceptance remains open.

### Native member and edited silhouette increment — 2026-10-01

The previous increment is committed as `9e807853`. Items 3 and 4 now provide
stable numbered native members for forest openings, castle windows/bounce
lights and cave water/mist/drip/sheen sources, plus movable single torch and
Aitos waterfall/splash anchors. Sparse member records expose enable/tint/intensity,
offsets, shape scales and applicable direction controls. Source validation and
native pool bounds remain in force; reset restores catalogue defaults. Parent
families own receiver selection. Map handles and inspector changes share atomic
validation and one-step undo. Native coordinate spaces remain unchanged.

Native and browser capture now resolve edited tile silhouettes from the same
stamps, depth bands, flips and black/transparent pixel masks as rendering.
Gameplay collision and source-art witnesses remain independent. Bloodpool's
moonlight reads edited opacity; forest front rays, castle windows and authored
fans use a bounded projected field with finite-depth attenuation. This mesh-based
atmospheric approximation does not claim full volumetric shadow tracing. It
reuses retained batch scratch without new GPU textures, resolves or readbacks.
Actor lighting now appends to caller-owned retained geometry, removing a global
scratch race and an intermediate geometry copy.

Validation extends the 147 regional room / 882 scanout-and-source comparisons
and all authored round trips to member records, edited opacity, reverse seeking,
atomic invalid imports and finite geometry under ASan/UBSan. Native tests cover
flips/transparency/depth, caster removal restoring light, bounded overlapping
forest members, unsupported controls and terrain isolation. Browser inspection
covers a numbered forest ray, offsets/scales/angle, live rendering and undo/redo.

Mac CPU samples at 128 horizontal / 64 vertical extension measured 2.442–3.871 ms
for animated room PPU/capture/effect meshes, with 0.035–0.804 ms spent on meshes
and 0.113–0.203 ms on separately sampled opacity capture. Static native Metal
compositor checks measured 1.081 ms at 960 × 600 and 1.758 ms at 1440 × 900 on
Apple M2. These are separate workloads, not complete gameplay frame times.
See the editor guide for room samples, methodology, limits and reproduction.

The point-and-click editor now loads default-source markers for the entire room
without captures or preview scrubbing. BG1 context menus add effects at the click,
edit or disable native defaults, delete authored effects, choose overlapping
markers and preview any map point through the shared renderer. Ordinary tile
selection also moves effect markers and resizes supported dimensions. Modal
controls replace the crowded effect settings in the sidebar, hide unsupported
fields and offer one-step Apply/Cancel/Undo; member controls link to parent-family
receiver settings. Native catalogue guides show source anchors and reference
extents, not every animated ray or particle boundary. Room families stay fixed;
actor events are configured through their separate preview workflow.

Whole-room default inventory is cached, bounded to 1024 sources and uses no
renderer/GPU calls. Four native Apple M2 inventory samples took 0.106–4.293 ms
once per room. UI regression tests exercise exact-point placement, modal rollback,
undo/redo, drag/resize, overlap choice, disabled-default reset and shared preview
positioning. The 147-room parity gate also verifies inventory identity, finite
coordinates, cache reuse and unchanged preview/GPU state under ASan/UBSan.

Next close native-default reconstruction, matched native visual comparisons,
long-session/context recovery, combined scenery/effects transport and target-platform measurements before
marking the whole editor plan complete. Deck/Vulkan and Windows/D3D12 hardware
acceptance is still outstanding.

The [editor guide](../tools/action_editor/README.md#environmental-authoring)
documents the file format, controls, limits and current omissions.

### Steam Deck performance gate — 2026-10-01

Actor migration is paused for this hardware check. **The initial performance
gate was not closed:** the Deck run found a severe existing flat-mode fallback
and a measurable cost from the new edited-scenery shadows. The initial
measurements below precede the fixes and remeasurement recorded at the end of
this section.

The current working tree and committed baseline `9e807853` were cross-built with
Zig 0.16.0, `x86_64-linux-gnu`, `-O2`, the portable runner source fallback and the
same generated CPU sources. Both use SDL 3.4.14 and SDL_ttf 3.2.2 from Steam
Runtime packages. The baseline needed one build-manifest repair: restoring the
missing `source =` prefix for `action_effect_preview.c`; its C sources were
unchanged. This is a matched comparison, not the Mac ThinLTO release build.
Binary identities, source hashes, hardware details, per-window samples and the
isolated runner are in
[Deck evidence](evidence/action-effects-deck-2026-10-01/measurements.json),
[CSV](evidence/action-effects-deck-2026-10-01/measurements.csv) and
[probe.py](evidence/action-effects-deck-2026-10-01/probe.py).

The actual device was a Steam Deck OLED, AMD APU 0932 / RADV VANGOGH,
Mesa 26.2.0-devel (`035ae2f854`), on AC power with the existing 15 W cap and
`powersave` governor. Tests used Desktop Mode, X11 through XWayland, Vulkan,
1280 × 800 borderless output, 90 Hz Vsync, 16:10 square pixels, dynamic Diorama
camera with zero tilt and distance 325, skybox-only, 64 extra vertical rows,
CRT/DOF/rim/edge-AA/interpolation enabled and three render workers. Action
lighting and particles stayed enabled; only Environmental effects changed in
the main on/off pairs. Audio used the dummy device and replacement music was
disabled, so this is not audio-inclusive release acceptance. Temperatures in
the 28 accepted timing runs peaked at 53°C; power/display settings were not
changed. Every run used private settings, input and save copies in
`~/argame/effects-2026-10-01`, leaving the prior installation intact.

Five scene-specific reporting windows are discarded for warm-up. FPS below is
the median reporting-window FPS; p95 is the **median of window p95s**, not a
pooled percentile. Stage times are frame-weighted CPU wall time per present,
including any driver blocking. Nested stages must not be added together.
GPU timestamps remain unavailable. The normal draw/vertex counters do not count
environmental callback geometry, so their small values do not establish a total
effects submission budget.

| Diorama sample | Effects off FPS | Effects on FPS | Off / on window p95 (ms) |
| --- | ---: | ---: | ---: |
| Fillmore Act 1 entry `0101` | 89.75 | 88.85 | 14.16 / 15.17 |
| Fillmore Act 2 cave entry `0102` | 90.00 | 90.00 | 12.89 / 13.08 |
| Bloodpool Act 1 entry `0201` | 89.75 | 88.00 | 14.17 / 15.15 |
| Bloodpool Act 2 exterior entry `0202` | 90.00 | 90.00 | 11.96 / 11.87 |
| Aitos Act 1 entry `0401` | 83.40 | 81.35 | 15.87 / 15.99 |
| Aitos Act 2 lava entry `0404` | 90.05 | 89.85 | 13.85 / 13.91 |
| Aitos Act 1 waterfall route `0402` | 75.50 | 77.20 | 17.63 / 17.58 |

The six entry pairs are stationary samples with native animation/enemies; the
waterfall uses its existing seed/replay setup and moves within the room. An
on-result above an off-result is not evidence that effects improve performance.
The Aitos `0401` and waterfall `0402` samples had about 0.001 ms effects callback
time and were already limited with effects off: they expose base scene/capture
cost, not a measurement of dense visible waterfall effect geometry. Castle
interior/gallery, lower temple, boss and densely authored scenes remain open.

Findings to address before resuming actor migration:

1. **Flat cave masks select the reference PPU renderer.** Fillmore `0102` flat
   mode measured 90.0 FPS with Environmental effects off and 30.9 FPS on
   (profiled runs); the committed baseline with effects on measured 31.9 FPS
   without profiling. The current on-run spends 29.59 ms/present in PPU scanout.
   `ActRaiser_PrepareSceneMasks` requests
   `SR_PPU_OVERLAY_MARK_OWNING_SCREEN_WINNER`, and
   `capture_needs_reference_sampler` in `snesrecomp-go/runtime/src/snes/ppu.c`
   rejects the packed scanline path for that policy. The
   [CPU profile](evidence/action-effects-deck-2026-10-01/flat-cave-profile.txt)
   is dominated by `sample_bg`, `render_line_to` and screen resolution. Preserve
   main/subscreen ownership and color-math semantics while adding an equivalent
   fast mask path; do not merely switch flags without parity checks. This
   predates the migration. A zero view-fallback counter does not detect this
   internal PPU fallback.
2. **Edited-scenery shadow work is repeated.** In the matched forest sample,
   baseline/current callback CPU time is 0.065 / 0.458 ms and frame snapshot time
   is 0.055 / 0.464 ms. Current timing repeats at essentially the same values.
   [CPU sampling](evidence/action-effects-deck-2026-10-01/forest-profile.txt)
   confirms `ActionEnvironmentScene_CaptureScenery`,
   `ActionSceneryShadow_Prepare` and their projection calls. Capture scans a
   768 × 352 opacity window and presentation rebuilds the projected coverage.
   Cache/reuse must track camera/projection, animated tile opacity and edits;
   retained or interpolated presents must not blindly rebuild unchanged work.
   This is added shadow functionality in the current migration, not evidence
   that parsing the new configuration files is expensive every frame.
3. **Bloodpool remains a heavier effects workload.** Its current callback costs
   about 2.38 ms/present at 90 Hz; the baseline measured 2.62 ms. It did not show
   the forest's callback regression, but its minimum reporting window fell to
   78.1 FPS in the main on-run. Profile coverage/mesh work and library submission
   costs before raising authored capacities. Scene texture upload volume stayed
   close between off/on at the same camera (about 0.116 / 0.119 MiB per present);
   this is not a measurement of all GPU geometry or transparent overdraw.

Reducing vertical extension to 32 improved forest/Aitos entry median FPS to
90.0/89.8, but Bloodpool measured 76.2. It changes the presented coverage and
timing mix, so these single comparisons do not support a blanket lower-row
recommendation. At 64 rows with the app's 60 FPS limiter, forest/Bloodpool/Aitos
entry medians were 60.0/59.6/60.0, with window p95s 17.16/17.90/16.97 ms and
worst intervals 23.9/27.1/25.5 ms. Those are not locked 60 FPS results. The panel
remained 90 Hz; Gamescope's limiter and a 60 Hz display mode were not tested.

All seven equal-final-tick Diorama on/off pairs produced identical final WRAM.
Baseline/current forest and Bloodpool comparisons also matched final WRAM.
Accepted timed runs reported no fatal/MX/dispatch/Vulkan errors or failed
presents; settled main entry pairs had no upload-mirror reallocations. This is
bounded replay evidence, not a long-session memory-leak or full traversal test.
Separate 1280 × 800 composite captures were inspected for forest, cave,
Bloodpool exterior/castle entry and lava; captures are excluded from timings.
Traversal assists were enabled, so player flash frames are not visual sign-off.

Excluded/limited attempts remain recorded in the local raw evidence:

- Forced visible Wayland initially stalled at frame zero in a DRM sync-object
  wait. Automatic SDL selection later chose X11 and completed successfully.
  Do not generalize the forced-driver failure to Gaming Mode or all Wayland;
  the native Wayland path needs a separate bounded investigation.
- The initial hidden smoke used 1080 × 672, so it is excluded from the native
  1280 × 800 timing table. Initial capture attempts before the requested game
  frame were superseded by verified composite captures.
- Direct `0103` warp lacked prerequisite terrain state and failed at the native
  asset-load boundary. It is an invalid fixture, not a regression finding.
- The moving Bloodpool 60 FPS attempt left the action scene before collecting
  sufficient settled windows. Its later menu frames are excluded; the reported
  60 FPS numbers are from the subsequent stationary tests.
- The profiled flat cave on-run ended at tick 1901 versus the requested 1900.
  Its valid timing windows are retained, but it is excluded from equal-tick
  gameplay-memory claims and is not used to infer a 1 FPS baseline regression.

Raw logs, private fixtures, captures, build logs and perf recordings remain in
`runs/deck-effects-2026-10-01` locally and the isolated Deck folder. To repeat,
copy the evidence `probe.py` beside those staged assets and run, for example,
`python3 probe.py --driver x11 --tag repeat64 --rooms 0101 0102 0201 0202 0401 0404 --order Off On`.
Use a fresh tag; the runner refuses to overwrite a previous case. `--mode 2d`
selects flat presentation, `--profile` records CPU samples and
`--refresh Limit --fps 60` selects the app limiter. No production code was
changed or committed during this pass. Windows/D3D12, Gaming Mode, full-room
routes, dense authoring stress, GPU timings and long-session acceptance remain
open; this evidence must not be used to check off those gates.

### Steam Deck fixes and remeasurement — 2026-10-01

The three identified effects hot spots now have fixes. The broader hardware
gate remains open, particularly moving-scene cadence and Windows/D3D12.

- Pure owning-screen winner masks now stay in the packed PPU renderer. Main
  and subscreen masks resolve the complete pre-extraction competition, including
  subscreen-owned backgrounds when output color math is disabled. Combined
  policies still use the reference renderer. Expanded oracle tests also caught
  an existing reference Mode 7 bug: disabled BG2 and nonexistent BG3/BG4 must
  not sample the affine map simply because their TM/TS bits are set.
- Scenery capture retains exact opacity bytes and extracted runs. It still
  observes VRAM animation, stamps, transparency, depth and camera movement;
  unchanged opacity skips run reconstruction. Resolved tile edits avoid a
  duplicate base metatile lookup. Native and editor callers own their caches.
- Forest/castle/authored-fan coverage retains its own buffer and compares the
  actual occluders and projection values. It never relies on recycled frame
  pointers or on the moonlight workspace remaining untouched.
- Moon visibility retains the transport result separately from animated ray
  geometry. A static skybox anchor keys its exact source band and BG1 caster
  projection, so other water bands and repeated strip callbacks cannot force
  a rebuild. Source movement, caster changes, field edits and actual projection
  changes still invalidate it. Custom projection callbacks bypass both render
  caches. Each present still updates light pulses, cloud transmission and rays.

These are portable CPU changes with no added GPU passes, textures, readbacks or
per-frame allocations. Retained storage adds approximately 117 KiB per render
batch and 82 KiB per scenery-capture owner; batches/caches must be zero-initialized
once. Geometry limits, blend order and the SDL rendering backend are unchanged.

The same isolated Deck installation, SDK, compiler options, 1280 × 800 output,
64 rows and 90 Hz display were used. Final binary `optimized3` has SHA-256
`04f52ce654cc266fe9cc2c09d697030b2d4260476c06aa86501907c593ee77f9`.
The evidence records the intermediate cache variants as well as the final one:
[optimization measurements](evidence/action-effects-deck-2026-10-01/optimization-measurements.json),
[runner](evidence/action-effects-deck-2026-10-01/optimization-probe.py),
[source hashes](evidence/action-effects-deck-2026-10-01/optimization-source-hashes.json)
and [Bloodpool CPU profile](evidence/action-effects-deck-2026-10-01/moon-optimization-profile.txt).
As above, stage values are CPU wall milliseconds per present and nest; FPS/p95
are reporting-window medians, not GPU measurements or pooled percentiles.

| Sample / cost | Before | After |
| --- | ---: | ---: |
| Flat cave, effects on: FPS | 30.9 profiled; 31.9 committed baseline unprofiled | 90.0 |
| Flat cave: PPU scanout | 29.59 ms | 2.71 ms |
| Forest stationary: effects callback | 0.458 ms | 0.074 ms |
| Forest stationary: frame snapshot | 0.464 ms | 0.350 ms |
| Bloodpool stationary: effects callback | 2.38–2.40 ms | 1.25 ms |
| Bloodpool stationary: frame snapshot | 0.464–0.475 ms | 0.449 ms |

The packed-mask measurements precede the later moon/scenery-only refinements;
their PPU implementation is the same. Final forest and Bloodpool stationary
medians were 89.5 and 87.95 FPS. Bloodpool's refined-cache repeats measured
1.233/1.234 ms callbacks before the final tile-lookup change. Aitos entry was
86.75 FPS with approximately 0.001 ms effects callback time; its base rendering
cost remains outside these effects fixes. Accepted follow-up runs peaked at 47°C.

Moving-camera verification used a back-and-forth/jump replay, not a stationary
cache-hit workload. Final forest callback cost was 0.428 ms versus 0.407–0.417 ms
before, with snapshot cost 0.423 ms versus 0.497–0.502 ms. Median FPS did **not**
improve: final 70.15 versus prior repeats 72.9–73.8, while median window p95
improved to 17.36 ms from 17.94–17.95 ms. The mix of presentations changed:
re-presents fell from roughly 20% to 17%; PPU cost per actual scanout call stayed
at 9.22 ms versus 9.18–9.25 ms. Thus the per-present scanout increase is largely
the changed presentation mix, not a slower PPU invocation. Effects-off moving samples were also
limited (69.95 prior / 71.7 optimized), and the app's 60 FPS limiter measured
59.05 with 17.70 ms window p95 on an intermediate build. These results do not
establish a moving-scene FPS win or locked 60/90 FPS. Base PPU/capture cost,
presentation cadence and Gaming Mode/refresh combinations need separate work;
the effects-only savings must not conceal that remaining gap.

Validation:

- Full runtime PPU oracle tests and capture-tile parity pass under ASan/UBSan.
  Modes 0–7, main/sub ownership changes, windows, extraction, authentic output,
  extended rows and classified virtual tiles are covered. Margin tests assert
  actual batched provider use, preventing a slow-path-only parity success.
- 56 action/render/Diorama CTest cases pass; the GPU frame-generation test is
  explicitly skipped by this test configuration. Native/WASM whole-room checks
  pass under ASan/UBSan: 147 regional rooms, 882 surface/source comparisons,
  147 authored round trips, 30 preset reconstructions and 28,263 valid draws.
  The standalone parity runner's missing depth-shape link dependency was fixed.
- Cached and uncached geometry match through animation, camera, clipping,
  viewport, field, source-band and occluder changes. The final forest and
  Bloodpool Deck captures are byte-identical to the prior build at game frame
  1000. Cave captures and final WRAM also match when both enter Diorama before
  room entry. The usual flat lead-in instead shifts one native enemy animation
  by a tick on the slow prior build; disabling interpolation alone does not
  remove that timing difference. Cave scenery/effect pixels were unchanged in
  those earlier comparisons too. Equal-tick forest/Bloodpool stationary and
  moving comparisons preserve final WRAM; the optimized flat cave on/off pair
  does as well.
- The first follow-up timed out because the panel was asleep, with roughly
  980 ms spent waiting to present; it is excluded. Waking the panel restored
  normal cadence without changing persistent settings. Native Wayland then
  completed both a visible capture and a timed 1280 × 800 cave run at 90.05 FPS.
  This supersedes the earlier startup-only failure as evidence of a general
  Wayland incompatibility, but is not Gaming Mode or long-session acceptance.

No commits, player-save changes or installed-game replacement were performed.
The isolated Deck folder retains the prior and optimized binaries for follow-up.

### Bloodpool regression follow-up (2026-10-01)

The field migration had two gaps that the native/WASM parity checks did not
cover. The editor placed non-movable family markers in a synthetic top-left
row. Bloodpool now publishes semantic guides from the shared C room adapter:
eight water spans, eight mist spans, sixteen bank insect regions, continuous
exposed timber edges, and BG2 moon/cloud/reflection regions. The inventory is
cached per room; field edits refresh guide positions without rendering or
allocating textures. Terrain guides explicitly edit linked field settings;
they do not pretend to be freely movable individual emitters. BG2 effects no
longer appear as false foreground anchors. Apply/cancel/undo and component
enablement retain those semantics.

The migration also applied foreground occlusion to the castle's own receiving
surfaces. This dimmed the opening, arch/sill spill, column edges and gallery
floor highlights against the masonry they should illuminate. That extra pass
and its unnecessary castle-only opacity capture are removed. Rear moon shafts
still pass behind the native wall/pillar silhouettes through normal composition.
The original geometry oracle now runs with populated opaque scenery as well as
an empty mask, so this error cannot pass on empty synthetic fixtures again.

The current WebGL/shared-C output matches a separately compiled HEAD `9e807853`
renderer pixel-for-pixel for gallery, large stacked windows, narrow windows and
boss views at frame 743 (960×600, 64 extra rows). Plane-only, skybox/plane and
skybox-only option selections were checked. The broken gallery differed on
45,184 pixels; the restored capture differs on zero. Review fixture and original
module are retained under `runs/bloodpool-regressions/`. These are browser
comparisons of production C, not a new Deck or D3D12 hardware qualification.

Validation: 56 action/render/Diorama tests pass with the GPU frame-generation
test skipped; the editor gesture suite and six bundle tests pass. ASan/UBSan
whole-room checks pass for all 147 regional rooms, including semantic guide
positions, 882 native/WASM surface/source matches, 147 authored round trips,
30 preset reconstructions and 28,263 draws. No new runtime GPU resources,
passes, particles or allocations were added by these fixes.


### Effect footprints, rotation and per-value resets (2026-10-01)

The map now draws static, unoccluded footprints rather than treating every
source as an axis-aligned box. Forest ray slope/fan growth and castle sill/lean
metadata come from the active C recipe. Layer scale is applied independently in
X and Y. Authored fans show several strands and their spread; soft lights,
halos, mist and clouds use elliptical area guides. Rectangular emitters retain
area bounds. These are layout guides, not a second effects renderer: the shared
preview remains authoritative for sway, fading, particles and terrain occlusion.

Selected directional lights have a round rotation handle. Native member offsets
retain their validated ±30-degree range; authored fans/gradients use ±180.
Symmetric effects and terrain-dependent mist/water do not expose a rotation
control that the renderer would ignore. Ray size handles follow the footprint;
rotation and resize drafts submit one validated undo operation on release.

Shift-click resets an effect value or a position/size/rotation handle. Native
source/member overrides return to inherited game defaults. Placed or loaded
complete definitions retain their initial session values, including placement,
so resetting a parameter does not unexpectedly send a custom effect to (0,0).
Vector controls reset only the selected component; count resets restore missing
ray records. Focused, uncommitted text resets as well. Modal cancel and undo
remain atomic. Ctrl/Cmd-click now selects several effect markers.

Validation: source/built-editor tests cover directional hit testing, nonuniform
layer scale, rotation drafts/undo, reset no-ops, vector/count defaults and modal
cancel. The browser confirmed a native ray-handle drag and Shift-click reset.
The shared room oracle passes all 147 regional rooms / 882 surface-source
comparisons under ASan/UBSan. This adds no game-renderer passes or hot-path work.
Screenshot: `runs/bloodpool-regressions/editor-ray-guides.png`.



### Moving-forest Steam Deck cost audit (2026-10-01)

Seven isolated runs reproduced and narrowed the ~70 FPS issue. This uses the
previously verified `optimized3` binary (SHA-256
`04f52ce654cc266fe9cc2c09d697030b2d4260476c06aa86501907c593ee77f9`),
whose forest rendering is unchanged by the subsequent castle/editor fixes.
Deck OLED, Desktop Mode, Vulkan, 1280×800, 90 Hz, skybox only, 64 extended rows,
three configured render workers; deterministic right/left/jump replay through
game frame 2400. Each run excludes the first five reporting windows after
entering Diorama. All seven final WRAM hashes match. Temperatures stayed ≤53°C.

| Moving forest variant | Median reporting-window FPS | Median window p95 ms |
| --- | ---: | ---: |
| 64 rows, effects/interpolation/CRT on, unprofiled X11 | 71.75 | 17.344 |
| Same, CPU profiler attached | 70.35 | 17.924 |
| Environmental effects off, profiler attached | 74.40 | 16.681 |
| Interpolation off | 88.00 | 15.080 |
| CRT off | 69.30 | 17.355 |
| 32 rows, interpolation/effects on | 87.90 | 15.338 |
| 64 rows, native Wayland | 73.60 | 17.496 |

These are presentation rates, including repeated frames. The interpolation-off
88 FPS result does not mean 88 distinct game updates or smooth generated frames.
The game remains near its normal 60 Hz simulation. None of these short Desktop
Mode runs establishes locked 90 FPS, Gaming Mode behavior or D3D12 performance.

The unprofiled baseline's top-level mean wall scopes per presentation are PPU
scanout 7.437 ms, upload/preparation 2.600 ms, frame presentation 0.70 ms or less,
and present/wait 1.206 ms. Nested interpolation analysis accounts for 1.536 ms
of upload/preparation; it must not be added to upload again. The equivalent
profiled run spends about 9.25 ms per actual PPU scanout (after excluding retained
presentations) and about 1.9 ms of interpolation analysis per new image. Effects
geometry callback is only ~0.43 ms per presentation. Larger surfaces plus these
serial costs cross the 11.11 ms 90 Hz budget; reducing rows or removing analysis
moves the same workload back near the panel limit. Switching X11/Wayland or
removing CRT does not resolve the bottleneck.

Moving-only user-cycle samples (22 seconds after first sample through one second
before the last; no lost samples) put ~21% in `render_native_fast_line` including
inlined capture/composition, ~7.5% in `native_capture_tiles_line`, ~6% in
`native_overlay_line_plan`, ~6% in `native_write_overlay_packed`, and ~5% in
`native_resolve_virtual_bg_span`. `FindGlobalMotion` and `AnalyzeDirection`
contribute another ~7%. The hot paths are packed/native paths, not evidence of
an extra-row reference-sampler fallback. PPU scanout remains serial to preserve
HDMA/IRQ ordering; configured render helpers do not parallelize this path
(the benchmark reports zero helper jobs). Interpolation analyzes changed
background images with global image search and object planes with block search.
It still incurs this work when scrolling, when stationary reuse cannot apply.

Prioritized follow-up:

1. Reduce duplicate palette/overlay-plan construction, capture export and tile
   edit passes in the packed scanout. Preserve line-local HDMA/CGRAM/window and
   main/subscreen ownership behavior; extend oracle coverage before changing
   any state reuse. Do not solve this by silently reducing extended rows.
2. Add validated known-motion hints for suitable background/skybox planes, with
   image-search fallback for raster motion, animation, edits and discontinuities.
   Audit endpoint/motion preparation before changing interpolation quality.
3. Recheck remaining upload/mirror/coverage memory passes (about 6 MiB scanned
   per presentation), then repeat the moving 64-row test in Gaming Mode and on
   D3D12 hardware. GPU timestamps were not collected; the CPU measurements and
   controlled toggles identify the current critical path, not every GPU cost.

No runtime performance algorithm or persistent player setting was changed in
this audit. Evidence: `runs/deck-effects-2026-10-01/moving-audit/` contains all
seven raw run summaries, moving-only profiles and the summarization script.


### Steam Deck capture/interpolation optimization — 2026-10-02

Implemented the moving-forest audit's CPU priorities without changing extended
rows, effect density, interpolation quality, CRT, resolution or player settings:

- Ordinary capture shares the current RGB palette instead of constructing a
  256-color ARGB table for every source/scanline. Fixed-color subtraction and
  object color-math metadata retain their specialized tables. All decisions
  remain line-local, preserving HDMA, brightness and CGRAM changes.
- Capture destinations, priority/semantic-band routing and object-color policy
  are resolved once per bound scanline. Unbound/aliased bands retain the prior
  primary-surface fallback and content-mask behavior.
- Background capture export is separate from packed main/subscreen composition.
  Removed capture regions skip composition; the remaining max-merge loops use
  the existing vectorizable portable implementation. Authentic/winner buffers,
  authored tiles, windowing and the reference renderer remain intact.
- Global interpolation search uses complete small-motion scores as an upper
  bound. It discards a candidate only when its nonnegative partial cost already
  exceeds that bound; ties, refinement and inverse validation remain unchanged.
  A compile-time bound proves costs cannot overflow. A rejected forward search
  no longer triggers a redundant backward search. No camera-motion assumption,
  reduced search radius, new worker synchronization or GPU pass was introduced.

The exhaustive motion oracle now also covers maximum-size RGBA surfaces, sparse
transparency, ties and partially animated surfaces. A separate per-pixel
"already exported" shortcut was tried and discarded after the Deck measurement
showed no benefit.

Matched Deck OLED / RADV Vulkan / X11 Desktop Mode runs use 1280×800 at 90 Hz,
64 extended rows, skybox only, interpolation/effects/CRT enabled and a deterministic
right/left/jump replay through runtime frame 2400. As before, the first five
reporting windows are excluded. The control build contains all current editor
and castle changes and replaces only the two optimized implementation files
with their pre-pass snapshots.

| Scene/run | Median window FPS | Median window p95 ms |
| --- | ---: | ---: |
| Moving forest, freshly built matching control | 71.25 | 17.632 |
| Moving forest, optimized | 88.60 | 15.235 |
| Moving forest, warmer repeat | 84.45 | 16.395 |
| Moving forest, final source-matched build | 87.40 | 16.014 |
| Fillmore cave, optimized | 90.00 | 11.460 |
| Bloodpool exterior, optimized | 89.80 | 14.061 |
| Bloodpool castle, optimized | 90.00 | 11.361 |
| Aitos lava, optimized | 89.00 | 14.498 |

The previous optimized3 control independently repeated at 70.7–71.6 FPS. Forest
PPU scanout falls from 9.27 to 8.38–8.43 ms per new image; interpolation analysis
falls from 1.93 to 1.50–1.64 ms per new image. Analysis is nested inside upload
and must not be added to that scope again. All moving-forest runs finish with
identical WRAM. Device temperatures reached 56°C during these short tests.
Presentation FPS includes interpolated/repeated frames, not extra game updates;
this is an approximately 19–24% throughput improvement, not a locked-90 claim.
Other room rows are coverage measurements, not before/after speedup claims.

Validation: full PPU/reference and capture-tile tests pass, including ASan/UBSan;
58 selected native tests pass (one additional headless GPU test skips); the exhaustive motion
oracle passes in release and sanitizer builds. The macOS build and editor WASM
build pass, as do 147 regional-room / 882 native-WASM surface/source comparisons
and 30 preset reconstructions. The CPU code adds no backend-specific dependency.
All five matched Deck final-composite captures (1280×800, game frame 1000,
interpolation disabled for deterministic endpoint comparison) are byte-for-byte
identical, with identical final WRAM as well. Motion decisions are checked
separately against the exhaustive oracle. The stress microbenchmark also keeps
large-shift cases near the old search cost while improving small shifts and
unrelated-frame rejection. On the Deck's synthetic maximum-size RGBA fixture,
small shifts take ~0.21 ms instead of ~1.55 ms; large shifts take ~1.66 ms instead
of ~1.53 ms (an 8–9% search-only tradeoff), and unrelated pairs take ~0.82 ms
instead of ~1.54 ms. This is a microbenchmark, not an in-game FPS measurement. Raw measurements, image hashes, profiles, source/
binary hashes and the stress harness live in `runs/deck-effects-2026-10-02/`.

Remaining platform acceptance: sustained Gaming Mode and Windows/D3D12 hardware
measurements. Forest frame pacing still has room to improve; these runs do not
establish every-room worst-case or a GPU timing budget. The remaining packed
scanout, capture-tile lookup and upload/coverage passes are the next measured
candidates if a stricter 90 Hz target is needed. Known-motion hints are deferred:
exact search pruning delivered a gain without weakening image-based validation.


### Scanout/presentation overlap prototype — 2026-10-01

Checkpoint `60f3fb93` contains the pending editor/effects work and the preceding
packed-scanout/motion optimizations. The follow-up prototype is **off by default**:
`AR_SCANOUT_OVERLAP=1` enables it only for interactive Fillmore Act 1 (`01/01`).
Headless runs and other rooms retain synchronous execution. No player setting,
installed Deck executable, resolution, row count, effect density or CRT setting
was changed for this experiment.

This is a bounded first experiment, **not the complete independent game-frame
producer**. A persistent SDL worker runs the existing ordered PPU scanout,
including HDMA and IRQ callbacks. The owner presents the previous uploaded frame
concurrently and then joins before completing the capture. Game coroutines,
input, frame setup/finish, metadata capture, uploads, settings, device resets and
event processing stay on the main thread. Failure to create the worker falls
back synchronously; failed presentation still joins. There is one outstanding
job, no growing queue, and teardown joins before releasing the worker resources.

The first version copied all retained CPU surfaces and was slower. The retained
compositor only needs uploaded-plane presence: texture pixels, coverage masks,
motion endpoints and diagnostic snapshot pixels are already presentation-owned.
The retained forest slot now clears its borrowed PPU/skybox/HUD/SIM views after
upload, and drawing uses the successful-upload mask. It cannot read a producer
buffer while scanout overwrites it. The extra copies were removed, rather than
trading frame coherence for throughput. Session-fatal publication is atomic so
an IRQ error and a renderer error cannot corrupt the first-failure diagnostic.

Matched Deck OLED / RADV Vulkan / X11 Desktop Mode, 1280×800 at 90 Hz, 64 extra
rows, skybox-only, interpolation, effects and CRT enabled. All runs use the
same moving right/left/jump replay through runtime tick 2400 and exclude the
first five reporting windows. FPS counts completed presentations, not game
updates; p95 below is the median of each reporting window's p95 interval.

| Experiment | Median window FPS | Median window p95 ms |
| --- | ---: | ---: |
| Fresh synchronous control | 87.85 | 15.425 |
| Overlap with extra CPU surface copies (discarded) | 86.35 | 16.224 |
| Overlap using existing presentation resources | 89.45 | 14.936 |
| Matching final-binary synchronous control | 88.20 | 15.256 |
| Final prototype repeat | 89.70 | 14.500 |

The final runs performed 1,186–1,187 worker scanouts. Actual worker scanout cost
averaged 8.80–8.90 ms; the owner still waited 2.97–2.98 ms after presenting, with
8.85–8.98 ms maximum joins. Minimum reporting-window FPS improved from
80.0–82.8 to 87.0–87.1, but worst window p95 remained 15.86–16.01 ms and maximum
intervals were about 22.4 ms. This does **not** establish reliable 90 Hz.
The whole-PPU wall scope includes overlapped presentation/join time; it must not
be added to presentation as if both were serial. The PPU-scanout scope itself
runs on the worker and measures actual scanout work. GPU timestamps were not
collected.

Validation: all moving runs have identical final WRAM at tick 2400. A separate
interpolation-disabled game-frame-1000 final-composite capture is byte-for-byte
identical (1280×800, zero differing bytes); both capture runs also end with
identical WRAM at tick 1550. macOS and Linux cross-builds pass. Of 29 selected
native tests, 28 pass and one headless GPU test skips. Worker reuse, owner-thread
presentation, synchronous fallback, failed-present joins and repeated shutdown
pass release, Address/UndefinedBehaviorSanitizer and ThreadSanitizer tests.
These sanitizer results cover the worker harness, not an instrumented full-game
session. Windows/D3D12 and sustained Gaming Mode are still unmeasured.

The pending endpoint is not extrapolated: an overlap present clamps to the last
completed image; subsequent re-presents can interpolate the newly uploaded pair.
Thus display FPS alone is insufficient acceptance. End-to-end input latency and
interpolated motion quality have not been measured; deterministic endpoint
identity does not prove those properties. Keep the prototype disabled by default.

**Decision / next experiment:** removing the copy is worthwhile, but fork/join
scanout overlap alone does not close the 11.11 ms deadline. A full producer must
run on its own source clock and publish completed, coherent frames through a
bounded handoff, allowing presentation to continue without a same-iteration
join. Move runner/coroutine ownership, capture buffers and metadata together;
keep SDL rendering on the main thread. Latch input/settings at safe boundaries,
use source timestamps for interpolation, drain on pause/reset/resize, and measure
queue age/input latency along with deadline misses. Prefer binding a small pool
of producer-owned buffers to copying the entire framebuffer again. No additional
HLE conversion or image-quality reduction is justified by this experiment.

Raw summaries (including all reporting windows), exact environment, image hashes,
WRAM hashes, collection script and final binary provenance are in
`runs/deck-scanout-overlap-2026-10-01/`. The isolated Deck binary is
`~/argame/effects-2026-10-01/scanout-overlap-03`, SHA-256
`59d8496e1859281580f3f96402bc78f1bb32bd6d569d9b92f3d08f6e13301cd8`.


### Independent forest producer experiment — 2026-10-01

Follow-up to the fork/join experiment above. `AR_FRAME_PRODUCER=1` is a separate,
**default-off diagnostic**, not a player setting or a production replacement.
It takes precedence over `AR_SCANOUT_OVERLAP`. Asynchronous work is limited to
interactive Fillmore Act 1 (`01/01`), software-paced refresh modes, no turbo,
no scene inspector and no authentic comparison. Headless stays unchanged.
For the measured 90 Hz configuration, use `AR_REFRESH_MODE=Limit` and
`AR_FRAME_LIMIT_FPS=90`; the experiment never changes the user's refresh setting.

One persistent SDL worker owns every game-coroutine resume from the first boot
tick. Other rooms and unaudited modes dispatch ticks synchronously to that same
owner; a Windows fiber/POSIX context never migrates between threads. In the
forest, a job runs the game tick(s) and the complete ordered CPU PPU transaction.
Its source deadline runs independently of presentation. There is at most one
outstanding job; completion explicitly returns runner/buffer ownership before
capture, upload, input, settings or event processing. The main thread presents
only retained value metadata and already-uploaded resources while work runs.
No additional full-frame copies, unbounded queue or per-frame allocations were
introduced. SDL rendering/uploads remain on main. The worker uses an explicit
4 MiB stack and destroys the game coroutine on its owner before exiting; the
Windows host-fiber conversion is released as well.

A completed frame is captured/uploaded before host housekeeping can change its
settings. Housekeeping still runs before the next game tick. Input and turbo
values are latched before handing a job to the worker. While it is pending,
the main thread does not pump input, change settings or read live game state.
A room transition returns without an asynchronous PPU draw, allowing the normal
host path to handle that frame. Pause/reset/resource work remains serialized.
This is a bounded prototype, **not a general detached simulation architecture**.

The first implementation achieved 90 completed presents/s under VSync but
produced only about **45 images/s**, despite retaining 60 game ticks/s through
catch-up. Blocking present prevented timely completion/upload/release of the
single producer buffer. This result was rejected. The final version uses the
synchronous path for VSync; it does not hide a source-rate reduction behind the
FPS counter. True concurrent VSync needs a bounded set of independently owned
completed endpoints (or an equivalent nonblocking backend handoff).

For software pacing, deadline checks now happen **before** a retained draw.
Waiting yields back to the loop instead of sleeping inside `CompletePresent`,
so completed producer work can be serviced between presentations. No backend
VSync override or persistent setting change is made. The main loop still yields
when it does not present. The source-clock timestamp travels with each uploaded
endpoint; interpolation does not restart from upload completion or extrapolate
past the newest image. Catch-up is bounded and image captures are logged
separately from game ticks.

Matched Deck OLED / RADV Vulkan / X11 Desktop Mode, 1280×800 at 90 Hz, 64 extra
rows, skybox-only, effects, CRT and interpolation enabled; moving oscillation/jump
replay through runtime tick 2400. The table uses median reporting-window FPS
and median reporting-window p95 interval, excluding five warm-up windows.
These are CPU/present-completion measurements, **not GPU timestamps or proof of
what the panel displayed**.

| Software-paced Limit=90 run | Median FPS | Median window p95 ms |
| --- | ---: | ---: |
| First matched synchronous control | 82.20 | 15.029 |
| Independent producer | 90.00 | 13.994 |
| Repeat synchronous control | 79.95 | 15.311 |
| Repeat independent producer | 90.00 | 13.641 |
| Final guarded producer | 90.00 | 14.026 |

The producer sustains approximately **60.1 images and game ticks/s** on this
path, instead of the discarded VSync variant's 45-image compromise. A separate
Uncapped-policy pair completed about 180 presents/s versus 80.45 synchronously
while preserving approximately 60.1 source frames/s. That policy targets twice
nominal refresh here: it is a throughput experiment, not 180 visible panel
updates. It must not be compared directly with VSync's pacing or queue depth.

Despite the throughput improvement, the 90-limit runs still have uneven
intervals (about 13.6–14.0 ms median window p95, with longer outliers). This does
**not** establish reliably spaced 11.11 ms presentations. Source work averaged
roughly 10 ms per capture. Initial 90-limit samples showed approximately
14–15 ms from source deadline to upload and 16.5 ms from sampled input to upload.
Those software ages exclude event arrival, display queuing, interpolation delay
and scanout: they are not end-to-end input-latency measurements.

Correctness evidence: matching tick-2400 WRAM for the measured moving runs;
paired interpolation-disabled frame-1000 final-composite screenshots are
byte-identical (1280×800), with identical tick-1550 WRAM. On Deck the producer
survived pause/resume, settings open/close, diorama off/on and scheduled vertical
extent changes 64→32→64, then exited normally. The final VSync fallback also matches the same reference
screenshot/WRAM and issues no asynchronous jobs. macOS and Linux builds pass;
29 selected tests pass, with one headless GPU test skipped. The producer harness
passes ThreadSanitizer and Address/UndefinedBehaviorSanitizer, including bounded
submission, nonblocking completion, deadlines, persistent ownership, shutdown
with a pending job, owner-thread cleanup and repeated lifetimes. Sanitizers cover
the harness, not an instrumented full-game run. Audio audition, actual
input-to-display latency, sustained Gaming Mode, Windows/D3D12 and full-game
thread-sanitizer coverage remain open.

**Next:** keep the experiment disabled by default. Reduce and schedule the
remaining main-thread capture/upload/motion-analysis handoff, and introduce
bounded owned endpoints before enabling concurrent VSync. Validate a monotonic
interpolation/playout timeline, source cadence, latency and sustained Gaming Mode
together; do not accept display FPS alone or compensate by reducing effects,
resolution or extra rows. The source transaction fits within the approximately
16.64 ms game interval, so these measurements support fixing pipeline scheduling
before using additional HLE as the next performance intervention.

Evidence and provenance are under ignored
`runs/deck-frame-producer-2026-10-01/`: `report.json` retains per-window data,
source-cadence/age samples, environments, binary hashes and screenshot hashes;
the probe, collector, build/test/sanitizer logs and complete prototype patch are
saved alongside it. Deck binaries and test saves/settings remain isolated under
`~/argame/effects-2026-10-01`; the installed game was not replaced.


### Buffered forest producer validation — 2026-10-02

`AR_FRAME_STREAM=1` extends the experiment with three independently owned,
preallocated frame packets. It remains **default off and forest-only**, with the
same inspector/authentic-comparison/turbo exclusions and persistent coroutine
owner. It takes precedence over the earlier one-job producer. No player-facing
setting, installed Deck binary, resolution, effects or extra-row budget changed.

The worker now owns tick, ordered PPU scanout, metadata capture and pixel
snapshotting. Main owns SDL events, upload and presentation. Each packet copies
only requested/content-bearing action planes plus HUD/skybox sources, validates
surface bounds and severs unsupported borrowed views. The queue uses C11
acquire/release ownership; a held reader packet cannot be overwritten. No
per-frame allocation, unbounded producer queue or cross-thread coroutine resume
is introduced. The measured copy is 5.107 MiB/frame (about 307 MiB/s at source
rate), typically 0.7–0.8 ms. This is a measurable prototype cost, not zero-copy.

Ordinary controller state is sampled on main and published atomically. Host
commands, camera changes, keyboard events, settings, resource resets and exact
scheduled captures require an acknowledged pause. SDL polling markers and raw
joystick notifications ignored by the normal event loop are not pause requests.
An initial all-events pause policy repeatedly stopped production on the Deck's
continuous axis noise: it reported 90 presents but only 47–51 source images/s.
That implementation was rejected. The accepted input path preserves meaningful
axis/button changes and does not dispatch host actions concurrently with game
state. Housekeeping obtains an ownership boundary periodically as well.

Presentation keeps future packets queued until their endpoints bracket a target
approximately two source periods in the past. Upload jitter no longer restarts
the interpolation phase. `AR_FRAME_STREAM_TRACE=<path>` records submission start,
completion, endpoint, phase, target and source tick for every successful stream
present. Source cadence, holds, backwards steps and delayed completions are
checked separately from the FPS counter. The measured source-to-present software
age is about **44.3 ms**, including this playout delay. This is not physical
input-to-photon latency; panel scanout and compositor queueing remain unmeasured.

Matched Deck OLED, native RADV Vulkan, X11 under KDE Wayland Desktop Mode,
physical 1280×800/90 Hz, 64 extra rows, skybox-only, effects/CRT/interpolation on:

| Test | Median reporting-window FPS | Median window p95 ms |
| --- | ---: | ---: |
| Synchronous control, 2400 moving ticks | 88.50 | 15.132 |
| Buffered producer, 2400 moving ticks | 90.00 | 11.363 |
| Synchronous control, 9000 moving ticks, real audio backend | 88.85 | 15.595 |
| Buffered producer, 9000 moving ticks, real audio backend | 90.00 | 11.416 |
| Buffered producer, live input and 64→32→64 changes | 90.00 | 11.405 |
| Final buffered build, native Wayland / 2400 ticks | 90.00 | 11.542 |
| Nested Gamescope, buffered producer | 89.95 | 18.867 |
| Nested Gamescope, extra submission scheduling experiment | 89.70 | 17.673 |

The sustained run's complete trace (excluding five initial seconds) measured
**60.099 source ticks/s**, 11.445 ms p95 / 11.608 ms p99 completed-present
interval, and nine intervals over 16.67 ms out of 11,211 intervals. Worst was
22.652 ms; the slowest one-second reporting window was 89.0 FPS. There were zero
backwards interpolation timestamps, three held timestamps, and 0.874% endpoint
clamps. Temperature peaked at 58°C. Thus the desktop result is substantially
better, but does not claim every single frame met 11.11 ms or that no source
endpoint was briefly late. These are CPU wall/completion diagnostics, not GPU
execution timestamps or panel presentation feedback.

The extra scheduling option did not solve nested Gamescope's uneven completions
and was removed after preserving its binary/patch/evidence. Nested Gamescope is
not the Deck's direct Gaming Mode compositor: these results cannot prove or
refute final Gaming Mode pacing. Its statistics confirm roughly 90 compositor
FPS, which alone still does not establish evenly spaced game content.

Correctness and portability checks so far:

- All matched 2400-tick runs preserve the reference WRAM hash
  `f8f6f7b1afe041b5446563e5383a124741885f734f93bafc566d3059bbd6abc4`.
- The paired 9000-tick sustained runs also finish with identical WRAM
  (`db096e764ce28afd13a8a459cdb7fbc9326e9f67c3b464019b97641f7723c4de`).
- The buffered, interpolation-disabled gf=1000 final composite is byte-identical
  to the reference 1280×800 image (SHA-256
  `6c269c88db16d5b156ad486e3f41835bc7844ce2bf040dc89f6f1a1c6d13faae`);
  tick-1550 WRAM also matches. Exact diagnostic capture boundaries are retained.
- Live keyboard churn and scheduled vertical-extent changes complete cleanly.
  Replay owns canonical inputs in that stress test, so it verifies host-event
  boundaries rather than asserting that real controller input altered the replay.
- macOS and Linux cross-builds pass. 33 selected tests pass, one GPU test skips
  without a display. Queue tests cover pixel ownership, unsupported/invalid views,
  arena exhaustion, no overwrite of held packets and 100,000 concurrent handoffs.
  Queue and producer harnesses pass ThreadSanitizer and ASan/UBSan; this is not
  full-game sanitizer coverage. The input test preserves host-action/camera
  barriers while accepting gameplay input and resting stick noise.

**Decision:** the producer/buffer architecture supports 90 Hz on the tested
Desktop Mode workload without lowering source cadence or visual settings.
The entire target-platform premise is **not yet signed off**. Direct Gaming Mode
validation is pending permission to close the active KDE session; broader room
coverage, physical input/display latency, Windows/D3D12 and full-game race
instrumentation remain production follow-ups. Do not enable the experiment by
default or reinterpret the nested-compositor result as a production pass.

Evidence: `runs/deck-frame-stream-2026-10-02/report.json` retains every measured
window, source sample, phase-trace summary, environment, binary/image hash and
lifecycle message. Raw traces, captures, scripts, build/test output and the
complete prototype patch are preserved in that directory. The final isolated
Deck binary is `frame-stream-07`, SHA-256
`919061ad28a04e2bc34ff8663142fd2d999f0a5b681c4499a9843608127f645a`; remote runs remain
under the isolated `~/argame/effects-2026-10-01/results/stream-*` paths.

#### Interpolation-disabled comparison — 2026-10-02

A matched pair using `frame-stream-07`, native Wayland in Desktop Mode, real
audio backend, 1280×800/90 Hz, 64 extra rows, all other effects enabled and the
same 2400-tick moving replay also improves completed-present consistency with
interpolation disabled:

| Path | Median window presents/s | Median window p95 ms | Worst settled interval ms |
| --- | ---: | ---: | ---: |
| Synchronous | 89.95 | 13.295 | 21.297 |
| Buffered producer | 90.00 | 11.454 | 12.490 |

Both final WRAM hashes match the 2400-tick reference above. The buffered trace
measures 60.093 distinct source ticks/s, with no skipped source endpoints or
backwards steps after warmup. Its 441 repeated endpoints among 1328 presents
are expected with interpolation disabled: roughly 60 source images on a 90 Hz
display alternate between one and two refreshes. Distinct-image intervals thus
still reach about 22.2 ms (p95 22.253 ms), despite regular presentation calls.
This is a short Desktop Mode pacing result, not a 90-distinct-frame result or
an input-latency improvement.

The prototype currently keeps the same two-source-period queue policy even
when interpolation is disabled. Rendering uses the current endpoint rather
than the interpolated phase; its measured endpoint age at present completion
is 36.386 ms median, not the interpolated timeline's 44.3 ms. Neither quantity
is button-to-photon latency or a measured difference from the synchronous path.
A production non-interpolated path should separately test earlier endpoint
delivery and a display cadence suited to native ~60 Hz content, rather than
inherit this interpolation buffer unconditionally. The overlap improves
scheduling; it does not reduce total rendering work and retains the ~0.77 ms
packet copy cost.

Evidence is in the same report, under `stream-nointerp-control-awake-07` and
`stream-nointerp-buffered-07`. The collector now reads each run's interpolation
setting and distinguishes endpoint timing from interpolated timing. The first
control attempt (`stream-nointerp-control-07`) timed out before gameplay with
the display asleep and is excluded; the display was awakened before both valid
runs. No game code or player settings were changed for this comparison.

#### Old versus buffered interpolation age — 2026-10-02

`AR_FRAME_SYNC_TRACE=<path>` adds an optional synchronous-loop timing trace:
accumulator sample clock, present completion, source period/remainder, phase,
tick and feature state. It changes no phase, capture timestamp or scheduling.
With the normal unclamped accumulator, interpolation between the previous and
current endpoint represents `sample_ns - source_period`; catch-up pair phase
preserves that one-period offset. This is the nominal interpolation timeline,
not an assertion that every effect, HUD element or fallback pixel has that age.

Matched `frame-stream-08` runs used native Wayland in Desktop Mode, real audio
backend, 1280×800/90 Hz, 64 extra rows, interpolation and effects enabled, and
the same 2400-tick moving replay. Traces exclude the first five seconds; the
synchronous trace also excludes the final stop's accumulator reset. No settled
sample gap reached the accumulator's three-period cap.

| Path | Median software timeline age ms | p95 software age ms | p95 completed-present interval ms |
| --- | ---: | ---: | ---: |
| Existing synchronous interpolation | 30.194 | 31.982 | 15.518 |
| Buffered producer interpolation | 44.312 | 44.773 | 11.558 |

The difference between median software ages is **14.118 ms**. The configured
target moves from one source period behind the synchronous loop's sampled
clock to two periods behind the producer presentation clock, but the measured
difference is not exactly 16.639 ms because source work and presentation waits
occur differently in the two paths. This demonstrates a latency/pacing tradeoff;
it does not establish button-to-photon latency or its exact incremental change.
The compositor, panel and real input arrival remain unmeasured. Both traces
retain ~60.1 source ticks/s and final WRAM matches the 2400-tick reference.

The trace-only change builds on macOS and Linux; no graphics quality, installed
Deck binary or player setting changed. Isolated binary `frame-stream-08` has
SHA-256 `42cfed1c4ffa1da675d8e9eb90c667ae00a8f8767789b89834e1a986643beed5`.
Raw traces/logs/settings and the collector are preserved under
`runs/deck-frame-stream-2026-10-02/stream-latency-{control,buffered}-08-00-0101-On/`,
with summaries in `report.json` and provenance in `provenance.json`.


#### Buffered pipeline production hardening — 2026-10-02

The buffered producer was hardened and measured as an opt-in first. Following
the user's rollout decision on 2026-10-02, it is now enabled by default for
ordinary play. `AR_FRAME_STREAM=0` selects the synchronous diagnostic fallback;
`AR_FRAME_STREAM=1` remains accepted. Headless replay/oracle runs stay synchronous.
No player configuration migration is required. The installed Deck game has not
been replaced; the default change is in the source and newly built binaries.

Implementation now has one bounded producer path. The earlier fork/join scanout
and one-job producer experiments have been removed. The coroutine stays on one
SDL worker for its whole lifetime, including synchronous scenes and destruction;
SDL events, settings, uploads and GPU presentation remain on the main thread.
Three reusable packets own their action plane/HUD/skybox pixels until upload.
Retained presentation strips borrowed CPU surfaces after upload. Unsupported
SIM/Mode 7/inspector views and room transitions return to synchronous rendering.

Playback policy is separate from ownership. Interpolation defaults to a target
1.75 source periods behind the presentation clock; non-interpolated playback
uploads the latest completed capture and discards stale queued images before
upload, without dropping their game ticks. Explicit frame limits below the source
rate retain synchronous tick coalescing so the bounded queue cannot slow the game
to the requested render rate. Pause, room changes, interpolation/source-rate
changes and long host interruptions invalidate old interpolation history. Host
commands pause and acknowledge the producer before touching game state; ordinary
keyboard/gamepad input updates a coherent atomic input snapshot without draining
the pipeline. Queue allocation failure falls back before a coroutine is created.

The delay sweep used the same moving forest replay, Desktop Mode Wayland/Vulkan,
VSync at 90 Hz, 1280×800, 64 extra rows and all effects, with live audio backend:

| Delay in source periods | Median window p95 ms | Median software image age ms | Clamped phases |
| --- | ---: | ---: | ---: |
| 2.00 | 11.592 | 44.283 | 0.75% |
| **1.75** | **11.542** | **40.105** | **0.90%** |
| 1.50 | 11.508 | 36.037 | **20.87%** |

All three average 90 presents/s and preserve identical final WRAM. The 1.5-period
setting is rejected: its attractive average/pacing conceals frequent missing
interpolation brackets. The 1.75-period policy removes about 4.2 ms of software
image age from the initial prototype, while retaining comparable phase coverage.
This is still about 9.9 ms older than the earlier synchronous median of 30.194 ms;
these are separate matched workload measurements, not physical input latency.
Non-interpolated latest-frame delivery measures 29.485 ms, down from the old
buffered policy's 36.386 ms, at 90 presents/s and native ~60.1 source ticks/s.

Broader matched 2400-tick VSync room samples (pipeline-10):

| Scene | Synchronous presents/s / window p95 ms | Buffered presents/s / window p95 ms |
| --- | ---: | ---: |
| Fillmore Act 2 cave, 01/02 | 90 / 12.092 | 90 / 11.455 |
| Bloodpool Act 1 exterior, 02/01 | 79.8 / 17.265 | 90 / 11.933 |
| Bloodpool Act 2 castle, 02/02 | 90 / 11.440 | 90 / 11.484 |
| Aitos lava, 04/04 | 90 / 14.688 | 90 / 11.592 |

Each pair preserves identical final WRAM. Buffered source cadence is 60.09–60.14
ticks/s, with no backward timeline steps. This is representative workload coverage,
not complete traversal of those levels or validation of all rooms/regions.

Timing traces distinguish clock epochs and untimestamped startup images. Those
images remain in presentation-gap statistics but cannot supply a meaningful
stream timeline age; comparisons across clock resets are excluded from timeline
continuity metrics. Scheduled pauses/rate changes are lifecycle stress, not steady
state benchmarks. CPU completed-present timing does not measure panel scanout.

Final pipeline-13 VSync sustained validation (9000 ticks, same full-quality
forest workload) measures 90.00 median window presents/s, 60.097 source ticks/s,
11.635 ms global p95 completed-present interval and 40.101 ms median software
image age. Four of 11,211 measured intervals exceed 16.67 ms, with a 20.769 ms
maximum; the timeline has no backward steps and one hold. Clamped phases are
1.18%; peak reported temperature is 58°C. Final WRAM matches the earlier
9000-tick reference exactly. This supports the reduced-delay policy on this
workload, not a zero-hitch or physical display-latency claim.

The same binary's software `Limit=90` sustained test also preserves that WRAM
and ~60.102 source ticks/s, but its global p95 is 15.120 ms, with 211 intervals
over 16.67 ms. Its lower 30.743 ms software age reflects different presentation
waiting, not a validated low-latency display path. Keep this pacing limitation
open and do not combine the two refresh modes into one acceptance result.

Lifecycle tests cover interpolation toggles, Test 30/Native 60 source-rate
changes, renderer toggles, host pause, settings menus, 32/64-row changes, and
targeted keyboard churn. Final gameplay memory matches between the 2600-tick
stress runs. Source-rate and host tests have no timeline backsteps within clock
epochs; pause gaps remain visible in the raw pacing statistics. Input churn
uses a replay for canonical gameplay, so it validates routing/ownership rather
than physical input responsiveness. macOS and Linux builds pass; 34 selected
tests pass and one GPU test skips without a display. New combined queue/worker
lifecycle tests pass narrow ThreadSanitizer and ASan/UBSan harnesses.

An explicit 20 FPS software limit exercises the synchronous fallback with and
without the worker enabled. Both retain ~60.068 game ticks/s and 20 presents/s,
finish on tick 2402 (the final catch-up group), and have identical WRAM
`293ccdaa2c0f176efc1ae7853a0d4b2a3c0ccb913bd2af6eed54d4850f1ef1a7`.
The buffered trace contains no streaming presents in that test, as intended.

Matched forest-to-Bloodpool-castle warp runs also preserve identical tick-2600
WRAM (`03449ddc3c895c8723b4c424d56d5f6f1545843fce78e06efe33203f22f83bc1`).
Dynamic-camera captures differ slightly because presentation-time smoothing is
not replay-clock deterministic; disabling interpolation alone does not remove
that difference. With Free Cam and interpolation disabled, both gf=1600 composite
captures are byte-identical (SHA-256
`bf806dc725f9cef504df29085c0ce030de1d2b81b91ce80d0b62c4c942ae4d8e`).
The first shortened fixed-camera attempt ended before the requested game-frame
capture and is excluded; the completed pair uses the full 2600-tick replay.

Evidence for this follow-up is under
`runs/frame-pipeline-production-2026-10-02/`, including original dirty-work backup,
build/test logs, harness, collector and per-run measurements. Original prototype
results above remain historical evidence rather than the new policy's results.
The hardening validation used isolated Deck binary `frame-pipeline-13`, SHA-256
`5886a078236fcdd40084bde8d4ff52ce0d30a68caa654ec7a0b9039f431e5464`.

Remaining validation follow-ups: direct Deck Gaming Mode, Windows/D3D12 runtime
and fiber lifecycle, full-game race
coverage, physical input/display latency, and broader transitions/visual acceptance.
Desktop Mode evidence supports this default change; it does not establish that
Gaming Mode has identical compositor pacing. These checks are follow-up work,
not blockers to the user-authorized rollout. The unit sanitizer harnesses
validate queue/worker ownership, not the entire renderer.
Diagnostic framebuffer captures recapture live state while the worker is stopped;
they must not be presented as exact pixel verification of the queued GPU path.

Default-rollout verification uses `frame-pipeline-14`, SHA-256
`072310cb94ab6a7c19ba1a68ada8d4b8d0505128bf280112e1c0897ebd5a1440`.
macOS and Linux builds pass. A 2400-tick Deck VSync replay with **no**
`AR_FRAME_STREAM` variable selects the buffered path and measures 90.0 median
window presents/s and 11.611 ms global p95, with no timeline backsteps/holds.
The matched `AR_FRAME_STREAM=0` run selects synchronous rendering. Both preserve
the exact 2400-tick reference WRAM. Evidence and the harness (which now explicitly
disables the pipeline for control runs) are in
`runs/frame-pipeline-default-2026-10-02/`. These are rollout smoke checks;
the longer performance and latency measurements remain documented above.

### PPU GPU-offload investigation — 2026-10-02

The buffered default is committed as `0cc29867`. The next substantial CPU target
is **Mode 1 background rasterization and capture export**, not more game-logic
HLE or an isolated GPU tile decoder. The offline prototype below now validates
that narrower background-capture premise on Metal and Deck Vulkan. No live-game
PPU GPU path or whole-game speedup has been implemented or measured yet.

Existing unprofiled Deck Desktop Wayland/Vulkan VSync measurements give the
following costs. PPU and emulation scopes are normalized from milliseconds per
presentation by the measured game ticks per presentation; packet-copy figures
are the separate source-trace medians. These are per native game frame, **not**
per 90 Hz presentation, and are overlapping pipeline work rather than a list to
sum into display latency.

| Workload, effects on | CPU PPU scanout ms/game frame | CPU emulation ms/game frame | Owned packet copy ms/game frame |
| --- | ---: | ---: | ---: |
| Fillmore Act 1 forest, sustained | 8.96 | 0.37 | 0.77 |
| Fillmore Act 2 cave | 7.42 | 0.56 | 0.62 |
| Bloodpool Act 1 exterior | 7.83 | 0.50 | 0.84 |
| Bloodpool Act 2 castle | 5.68 | 0.54 | 0.78 |
| Aitos lava | 7.23 | 0.38 | 0.88 |

Source: `runs/frame-pipeline-production-2026-10-02/report.json`, pipeline-13
forest sustained VSync and pipeline-10 room runs. This establishes the scale of
the CPU work, not how much a GPU implementation can remove. Forest packets copy
5.107 MiB per game frame, but actual GPU upload traffic is already reduced by the
upload mirror; packet size must not be reported as current GPU transfer volume.

A fresh 2600-tick moving forest replay on the committed default, using isolated
Deck binary `frame-pipeline-14`, confirms where CPU cycles go. The steady profile
excludes the first 22 seconds and final second, has approximately 2,000 samples
and no lost samples. The following are **exclusive user-cycle shares across all
process threads**, not GPU durations or percentages of the PPU scope:

| Symbol / work | Sampled CPU cycles |
| --- | ---: |
| `render_native_fast_line` (includes inlined capture/composition) | 21.26% |
| `native_capture_tiles_line` | 6.67% |
| `native_resolve_virtual_bg_span` | 5.26% |
| `decoded_4bpp_row` | 1.18% |
| `FindGlobalMotion` / `AnalyzeDirection` | 4.09% / 3.32% |
| `ActionSceneryShadow_Prepare` | 2.47% |

The decoder already caches expanded rows by VRAM contents. Moving that function
alone would address a small portion of the workload. The expensive path also
walks world tiles, resolves scroll/priority/edits, expands palette colors, writes
separate layer/band captures, and performs main/subscreen composition and masks.
The existing extended-row path uses the native capture renderer; this profile
does not indicate a slow extra-row fallback. Scenery-shadow preparation is a
separate CPU effect cost and would not disappear with a PPU shader.

The new profile retains 90.0 median window presents/s, 11.665 ms median window
p95 and the prior 2600-tick WRAM hash
`3d4530acb0674abd8c2c9b309300dbec8caf1ea56000c3cea2fa7af3e9b99599`.
Use the unprofiled runs for absolute timings; sampling changes the workload.
Raw profile, time window, thread/symbol reports, replay, settings and logs are
saved under `runs/ppu-gpu-audit-2026-10-02/`. `audit.json` records provenance and
the normalized costs. The installed Deck executable was not changed.

**Required split and dependencies:**

- Keep game execution, ordered HDMA/IRQ callbacks, beam/VBlank progression and
  OAM evaluation/status on the CPU. `runner_ppu_services.c:run_ppu_scanout` renders
  each row before that row's HDMA and handles IRQ-driven state changes. An
  end-of-frame register/VRAM snapshot cannot reproduce every row. Preserve the
  synthetic top/bottom-row policy as well as native rows.
- Export immutable, bounded tile/span commands with resolved capture edits,
  semantic bands, window/scroll/color state and referenced VRAM/CGRAM versions.
  Virtual-world providers contain CPU callbacks, so shaders cannot consume the
  current bindings directly. The scanout ABI and editor `SrSceneFrame` are useful
  starting points, but neither is a complete live GPU frame program. Raw VRAM
  and palette total roughly 64.5 KiB; row versions and world/capture metadata add
  to that. Measure actual command volume instead of promising a fixed reduction.
- Rasterize supported BG1/BG2 captures into GPU textures, including extended
  rows, priorities, transparency and semantic bands. The first prototype may
  retain CPU OBJ/HUD and packed priority/color-math work; count that remaining
  work when assessing savings. Later move composition/winner-mask generation
  only after exact CPU comparisons pass. Unsupported cases retain the CPU path.
- Interpolation currently reads full CPU previous/current images in
  `diorama_frame_generation_sdl.c`. It already copies GPU endpoint textures
  without uploading them twice. Avoid a new full-frame GPU readback: either move
  the required background motion analysis to GPU or validate a narrowly eligible
  motion-metadata path, with explicit handling of animated tiles, row scroll,
  edits and discontinuities. Camera delta alone is not a general replacement.
  CPU OBJ block analysis can remain while OBJ remains CPU-rendered.
- Audit every remaining pixel consumer: upload mirrors/content masks,
  authentic/winner masks, diagnostic captures and interpolation. GPU plane alpha
  can serve some lighting passes, but it is not equivalent to the native winning
  pixel mask. Diagnostics may read back on demand; normal playback must not
  synchronize just to recreate discarded CPU images.
- Reuse the owned frame queue for immutable render commands. Create/submit GPU
  work on the renderer thread, retain interpolation endpoints, and bound GPU
  resources until their in-flight uses complete. A released CPU packet does not
  establish GPU completion. Render source endpoints at the native game rate and
  reuse them across higher-rate presentations.

**Portable implementation and library costs:** start with a batched fragment
tile rasterizer through the existing GPU backend. It can share tile/state rules
with the editor's WebGL2 backend, while native shaders use the established
MSL/SPIR-V/DXIL build pipeline. SDL's custom fragment render states are available
in the pinned 3.4 runtime and have renderer-thread affinity
([SDL reference](https://wiki.libsdl.org/SDL3/SDL_CreateGPURenderState)).
Do not add one draw/update per tile or scanline. Upload dirty atlas/state ranges
in batches and reuse resources; the previously audited SDL texture-update path
can allocate a transfer resource per call, including committed resources on
D3D12. Compute is an alternative if measurements justify it, not a requirement
for pixel parallelism or a promise of asynchronous GPU execution. A custom
command-buffer path also needs the ordering contract documented in
`sim3d_depth_pass_sdl.c` and `ArSdlRenderBackend_SubmitPending`; a renderer flush
alone is not a GPU completion fence. Neither present/wait time nor a CPU timer
around a GPU draw proves available GPU headroom.

**Recommended prototype sequence:**

1. Define the immutable command contract and validate replay against the CPU
   renderer. A native 256×256 BG2 page is a useful first shader fixture, but its
   small scope alone cannot validate the performance premise. Retain the CPU
   renderer as the oracle and headless/unsupported backend fallback.
2. Expand the experimental path to the forest's Mode 1 world BG1/BG2 captures
   with 64 extra rows, then Bloodpool's skybox/water-band views. Compare exported
   RGBA, content/priority/semantic masks and placement before removing CPU export.
   Include the existing plain, skybox-only and plane-plus-skybox policies.
3. Complete the interpolation/pixel-consumer bridge and remove redundant CPU
   export for supported captures. A non-interpolated microbenchmark is useful,
   but it is not acceptance for the current default workload.
4. Measure end-to-end producer and presentation costs, GPU execution, transfer
   bytes/calls, allocations, sustained pacing and power/temperature. A suggested
   go/no-go target is at least **2 ms less producer work per forest game frame**
   with interpolation/effects/64 rows unchanged and no presentation-tail
   regression. This is an engineering target, not a predicted speedup. Additional
   GPU work competes with effects and synthesis, even if CPU time improves.
5. Validate fades, HDMA palette/scroll changes, water, windows, sub-only ownership
   such as Marahna, edited maps, room transitions, region variants and alternate
   capture widths/heights. Check gameplay memory and status as well as pixels.
   Exercise Metal, Deck Vulkan, Windows/D3D12 and shared editor fixtures before
   broadening defaults. Revisit interpolation buffering only after sustained
   completion times improve; faster PPU work does not automatically reduce the
   current configured software image age.

#### Offline GPU background prototype — 2026-10-02

**Decision: proceed to a bounded live-integration experiment.** The first
experiment is an opt-in executable, `actraiser_ppu_gpu_probe`, outside the game
and runner ABI. `tools/ppu_gpu/packet.c` builds an owned 273,408-byte RGBA8 command
texture with VRAM, palettes, resolved world tiles, per-row scroll/extents and
band selection. `src/shaders/ppu_bg_probe.frag.glsl` rasterizes six captures
(ordinary/high/far for each background) in one offscreen draw. There are no
CPU tile-provider callbacks or borrowed VRAM pointers across the GPU boundary.

The probe uses direct SDL_GPU with persistent upload storage and a maximum of
three fenced submissions. It uploads once and draws once per job, rather than
using per-tile or per-row renderer updates. GPU resources may cycle within that
bounded queue. Timed streaming work includes CPU packet construction, upload,
rendering, submission and waiting for all work to complete; validation readback
is outside the timed loops. This measures **completed wall-clock throughput**,
not hardware GPU timestamps, single-job latency or available GPU headroom in
the running game. CPU and GPU parts can overlap. Resident-packet replay is a
separate diagnostic, not a value to subtract from streaming time: clocks and
queue behavior differ between those workloads.

Final `-04` runs, 500 jobs per case, Release/O3, SIMD enabled on both CPU
architectures, production batched `ActionBgWorld` lookups for real rooms:

| Room artwork | Deck CPU scene ms | Deck packet build ms | Deck GPU streaming ms | Mac CPU scene ms | Mac GPU streaming ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fillmore Act 1 forest, 01:01 | 4.391 | 0.186 | 0.448 | 1.579 | 0.547 |
| Fillmore Act 2 cave, 01:02 | 4.683 | 0.185 | 0.449 | 1.725 | 0.551 |
| Bloodpool Act 1 exterior, 02:01 | 3.274 | 0.213 | 0.438 | 1.128 | 0.501 |
| Bloodpool Act 2 castle, 02:02 | 3.039 | 0.195 | 0.435 | 1.073 | 0.489 |
| Aitos lava, 04:04 | 4.337 | 0.185 | 0.450 | 1.567 | 0.572 |

Three further 1,000-job Deck forest runs measured CPU scene 3.833–3.881 ms,
packet build 0.185–0.186 ms and GPU streaming 0.4046–0.4047 ms. The synthetic
fixture measured 6.373 ms CPU / 0.551 ms streaming on Deck and 2.277 / 0.599 ms
on Mac. SDL versions were 3.4.14 (Deck Vulkan) and 3.4.12 (Mac Metal). These are
short standalone throughput experiments with normal dynamic device clocks,
not sustained power-controlled game benchmarks. Earlier `-01` window-backed
GPU timings were contaminated by presentation pacing and are not valid GPU
work measurements; `-02` also lacked the final SIMD/batched CPU baseline.

**Correctness:** all six fixtures pass 12 camera/time/policy phases on both
devices with zero RGBA mismatches against the production PPU capture surfaces.
The final phase also matches the slower reference pixel renderer. Replaying an
owned packet after changing source VRAM, CGRAM and camera still gives the
original pixels. Post-benchmark readback validates that queued output completed.
Logs count nontransparent pixels so blank captures cannot masquerade as useful
room coverage. Synthetic coverage includes horizontal extras 0/112/128, 128
total extra vertical rows split 0/128 or 64/64, flips, transparency, tile
priorities, semantic bands, clipping/extents, fixed subtract, half-add alpha,
fill colors, sub-only capture ownership and changing row scroll. Real fixtures
use immutable exported room artwork, rebuilt animated characters and native
frame-state scroll values. They do not need manual gameplay captures.

**Scope limits:** these real-art fixtures intentionally use finite live-world
capture rules and do not reproduce each room's full presentation policy. They
do not validate skybox pages, plane projection, effects, actors/HUD, main/subscreen
composition, native winning-pixel masks or live HDMA/IRQ execution. The packet
rejects edits, authentic fallback tiles, non-live-world edge policies,
fill-relative motion, mosaic greater than one, native-page/background-view
requests and winner-dependent/full-add exports. It does not encode per-row
VRAM or CGRAM versions. The CPU timing includes clearing all seven scene
surfaces and backdrop rendering; the GPU output is only the six BG capture
surfaces. Their difference is **not** a measured amount removed from live PPU
scanout, and cannot be subtracted from the earlier game-frame table.

Build/run from the repository root:

```sh
cmake -S . -B build-ppu-gpu -DAR_TESTS_ONLY=ON -DAR_PPU_GPU_PROBE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-ppu-gpu --target actraiser_ppu_gpu_probe
build-ppu-gpu/actraiser_ppu_gpu_probe --iterations 500
node tools/ppu_gpu/export_fixtures.mjs build/action-editor/ar-action-layer-editor.html runs/ppu-fixtures
build-ppu-gpu/actraiser_ppu_gpu_probe --scene runs/ppu-fixtures/room-0101.arscene --iterations 500
```

The optional CTest skips with code 77 if no GPU device is available; a skip is
not parity acceptance. The fixture exporter reuses the existing editor encoder
and an already built bundle; generated game artwork stays in ignored evidence.
MSL, SPIR-V and DXIL generation/checks pass; Windows/D3D12 runtime and browser
execution remain untested. The probe is portable C11 with strict warnings clean;
AddressSanitizer/UBSan synthetic and real cave runs pass. Existing PPU pipeline,
scene snapshot and runner-private-boundary tests pass. At this initial milestone the render boundary gate still referenced the absent
`src/action/action_forest_effect_render.c`. The live-integration follow-up below
updates that inventory to the consolidated authored/ray modules and passes both
its positive and negative checks; this is not a claim of a full-suite run.
Evidence, binary/source hashes, platform metadata and raw logs are in
`runs/ppu-gpu-prototype-2026-10-02/`. No installed game executable was replaced.

**Follow-up experiment (results below):** implement an eligible live Mode 1 capture job,
retaining ordered CPU scanout/status and explicit CPU fallback. First compare
owned commands and output against live CPU captures at matching ticks, including
row palette/VRAM changes. Then bridge interpolation and other CPU pixel consumers
without per-frame readback before removing CPU BG export. Retain GPU endpoint
textures across presentations, measure the remaining CPU composition work and
test contention with effects/frame synthesis. Only then evaluate the ≥2 ms
producer reduction and sustained 90 Hz pacing gate above. The prototype supports
proceeding to that test; it does not establish production readiness or reduced
input latency.

#### GPU-resident interpolation path

The user asked whether interpolation can also remain on the GPU. Yes: CPU
pixels are a requirement of the current implementation, not of interpolation
itself. Today `DioramaFrameGeneration_CaptureWithSkybox` copies already available
CPU capture pixels for `PresentationFrameGeneration_Analyze`, while endpoint
copies and synthesis already run on the GPU. There is no current GPU-to-CPU
readback in that path. Offloading PPU rendering without changing this dependency
would introduce a new readback, which the production design should avoid.

Keep previous/current capture textures resident and produce motion vectors and
confidence on the GPU, then consume them directly in synthesis and compositing.
Retain analysis once per new source pair, reuse it for all intermediate presents,
and keep discontinuity/pair timestamps as small CPU control metadata. A GPU
confidence flag must select the exact endpoint in the shader when unreliable;
reading that flag synchronously to the CPU would recreate a stall even though
it contains only a few bytes. Preserve alpha, priority separation, bidirectional
consistency and nearest-endpoint pose ownership to avoid sprite trails.

- Start with the background global-motion search. Its bounded integer candidate
  scoring and deterministic tie-breaking can be evaluated in parallel and reduced
  on GPU, with the CPU implementation as an exact motion-vector oracle. This is
  the direct replacement for the CPU-pixel dependency introduced by GPU BGs.
- Use known per-layer/per-row camera/scroll motion as an optimization only for
  validated stable regions. Animation, edited tiles, palette changes, visibility
  and newly revealed pixels require validity masks or image-based checks. A
  single camera delta cannot describe water raster bands or all skybox motion.
- Move OBJ block analysis afterward. The present CPU algorithm seeds blocks from
  already computed left/upper neighbours; a naive parallel translation changes
  its answers or races. Use explicitly staged seeds/refinement and confidence
  passes, or a separately validated parallel search. Actor/object identities may
  help where stable, but raw OAM slot reuse is not reliable object identity.
- Remove secondary CPU motion consumers too. `DioramaFrameGeneration_PlaneOffset`
  supplies offsets used by plane/skybox and effect projection. Those consumers
  must use known metadata or the resident GPU field so lighting stays attached
  without downloading search results. CPU-produced OBJ can initially upload once
  and participate in GPU analysis; this does not require an OBJ rasterizer port.

SDL_GPU supports compute pipelines for SPIR-V, MSL and DXIL
([SDL compute pipeline contract](https://wiki.libsdl.org/SDL3/SDL_CreateGPUComputePipeline)).
Dependent dispatches require separate compute passes under its synchronization
contract ([SDL compute pass rules](https://wiki.libsdl.org/SDL3/SDL_BeginGPUComputePass)).
The offline shader builder now supports compute with verified resource bindings;
the global-motion experiment is described below. Keep the portable CPU fallback and separately address browser
WebGL2, which is not covered by a native SDL compute implementation. This path
does not require a vendor optical-flow API, but backend execution and performance
must still be verified. Measure the combined rasterization, analysis, effects
and synthesis workload on Deck: GPU work competes for execution and memory
bandwidth. Fewer CPU copies and waits are promising, not proof of a faster full
frame or of reduced interpolation buffering. Global background motion and warp
are now prototyped and exercised in the live compositor below; removal of CPU
pixel consumers and actor block analysis remain open.

#### GPU motion and interpolation prototype — 2026-10-02

The opt-in probe now accepts `--motion`. It renders directly into alternating
GPU endpoint textures, estimates bidirectional global motion, validates its
confidence and synthesizes the nearer endpoint entirely on the GPU. Neither
pixels nor motion/confidence values are downloaded during the timed path. The
CPU still builds/uploads the compact background command packet, chooses phase
and schedules bounded GPU work. Validation readback is explicitly outside timing.

`src/platform/sdl/gpu_global_motion_sdl.c` records five compute passes: nearby-candidate bounds,
coarse displacement scoring, fine refinement, inverse/confidence validation,
and synthesis. The first version searched all candidates and was too expensive
(Deck forest analysis+warp 2.953 ms, combined background+analysis+warp 3.019 ms).
Exact nonnegative-cost pruning now skips candidates that cannot beat the known
bound, retaining ties and the CPU's deterministic selection. This improves the
cost without replacing image analysis with a camera-only assumption. Unreliable
motion selects current pixels on GPU; no CPU confidence flag download is needed.

For new source pairs, endpoint textures swap rather than copying the previous
full image. Additional presentations reuse the resident motion field. Scratch
buffers and staging resources persist, resource cycling has at most three
fenced submissions in flight, and dependent dispatches use separate passes.
The warp clamps sampling inside each layer's own atlas band and preserves exact
endpoint/rejected-pair pixels and nearest-pose ownership.

**Correctness:** zero motion-vector or acceptance mismatches against the
production CPU global analyzer on both Metal and Deck Vulkan. Synthesized pixel
checks at phase 0, .25, .5, .75 and 1 allow at most one 8-bit channel value of
linear-filter rounding for fractional phases; original endpoints and rejected
pairs must be exact. Fixtures cover transparent, flat, repeating, random-alpha
and decorrelated patterns, signed motion including radius limits, 96×64 and
640×352 textures, 13×9 and 1×1 degenerate grids, and eight camera/time pairs for
each of the five real room fixtures. The final vectors and synthesized images
are checked again after the timed changing-frame sequence and after motion-field
reuse, without recomputing GPU analysis in the checker. Odd/even sequence lengths
are exercised. This validates global motion only, not the actor block algorithm.

The five-room `-04` Deck measurements below use 500 source jobs per scope.
The CPU combined scope renders the scene, retains its pixels and analyzes all
six background bands. The GPU scope builds/uploads commands, renders the six
bands and runs analysis/warp. The last column schedules three warps per two
source frames, reusing analysis for the extra presentation:

| Fixture | CPU scene + global analysis ms/source | GPU BG + analysis + one warp ms/source | GPU 60→90 work ratio ms/source |
| --- | ---: | ---: | ---: |
| Fillmore forest | 5.904 | 1.388 | 1.352 |
| Fillmore cave | 6.327 | 1.254 | 1.189 |
| Bloodpool exterior | 5.094 | 1.271 | 1.244 |
| Bloodpool castle | 4.792 | 1.198 | 1.165 |
| Aitos lava | 6.063 | 1.466 | 1.408 |

Three 1,200-source forest repeats measure CPU combined 5.222–5.239 ms, GPU
combined 1.271–1.275 ms and the 60→90 work ratio 1.289–1.297 ms/source. A reused
warp alone costs 0.099–0.100 ms in those repeats. Mac Metal room runs measure
CPU combined 1.504–2.056 ms and GPU 60→90 ratio 0.918–1.112 ms/source. Standalone
GPU analysis need not beat CPU analysis on Mac; removing its CPU-image dependency
and combining it with GPU background rendering is the relevant result.

These are completed wall-clock throughput scopes, not GPU timestamp durations,
physical latency, live 90 Hz pacing or a promise of these CPU savings in game.
Scope order and normal dynamic clocks can make a short 90-ratio run slightly
faster than its preceding one-warp run despite doing more work; use the longer
repeats for that comparison. CPU scene rendering still includes backdrop/seven
surface clears, and neither path applies the game's changed-plane skipping.
No actors, effects, main/subscreen composition, presentation or live PPU event
recording compete for resources in this benchmark. Room/capture eligibility
limits from the original background probe still apply.

**Portability:** `tools/build_shaders.py` now compiles `.comp.glsl` to SPIR-V,
MSL and DXIL. It verifies SDL's compute resource order and normalizes named MSL
buffer slots (uniforms before storage), with tests for bad/missing bindings.
Shader freshness, strict C11 warnings, existing CPU motion/oracle/PPU/snapshot
tests, both opt-in GPU CTests, and ASan/UBSan real-cave runs pass. Windows/D3D12
runtime and browser execution are not verified. Browser WebGL2 needs a separate
implementation/fallback and is not changed by these native compute kernels.

The final upload tests exposed a library-specific trap:
[SDL 3.4.12 Metal's upload implementation](https://github.com/libsdl-org/SDL/blob/release-3.4.12/src/gpu/metal/SDL_gpu_metal.m#L1668)
uses destination width rather than the declared source row pitch. The test
harness therefore uses packed Metal uploads and 256-byte-aligned Vulkan/D3D12
uploads, avoiding D3D12's repacking fallback. Readback uses aligned pitch on all
backends. The background command packet is naturally aligned and unaffected.
Revision `-06` has this workaround; the timed kernels and workload are unchanged
from `-04`. The failed `-05` upload experiment remains in the evidence logs.

Reproduce with the existing opt-in build and exported fixtures:

```sh
build-ppu-gpu/actraiser_ppu_gpu_probe --motion --iterations 500
build-ppu-gpu/actraiser_ppu_gpu_probe --motion --scene runs/ppu-fixtures/room-0101.arscene --iterations 1200
```

Evidence and hashes: `runs/gpu-motion-prototype-2026-10-02/`. No shipped renderer
or installed Deck game executable changed in the offline experiment. The next
section records the live ownership/interpolation bridge and its performance gate.
Actor block analysis remains a separate algorithm/quality step.

#### Live GPU replay and global motion — 2026-10-02

Historical indexed-bridge results; the subsequent ownership experiment below
supersedes this section's implementation status and next gate.

**Decision: retain the live bridge as an opt-in experiment. Do not enable it by
default.** It passes the exercised correctness cases on Metal and Deck Vulkan,
but does **not** pass the ≥2 ms producer reduction gate. This is a completed
live integration experiment, not completion of the GPU PPU migration.

Two independent development switches now exercise the ordinary action renderer:

- `AR_GPU_BG_CAPTURE=1`: record an owned indexed BG1/BG2 capture during ordered
  scanout and resolve its ordinary/high/far colors on the GPU. `validate` also
  compares the indexed replay to every matching CPU capture pixel and rejects
  mismatched/unsupported sources. It does not read GPU pixels during gameplay.
- `AR_GPU_BG_MOTION=1`: capture GPU endpoints, run global motion/confidence on
  compute and reuse that resident field for intermediate background images.
  `validate` additionally downloads the 192-byte field and compares it to the
  CPU oracle. That explicitly fenced validation mode is excluded from timings.
- Both are off unless explicitly set. The regular renderer and existing
  buffered producer remain the defaults. The retained packet allocation is
  lazy; ordinary frame-queue slots do not acquire the additional 2.417 MiB.

**What this bridge proves:** `SrPpuBgPacket` records the palette/color policy at
each row's actual scanout time. Palette, brightness, scroll, VRAM changes and
capture edits therefore cannot be replaced accidentally by an end-of-frame
snapshot. It is owned by the existing bounded producer queue, with no borrowed
VRAM/provider pointers surviving scanout. The V2 scanout request remains valid;
the optional tail is size-gated and its borrowed pointer is cleared at return.
The packet's 2,534,416 bytes are deliberately an indexed replay of CPU-resolved
pixels, **not** the earlier compact tile-command packet and **not** a removal
of CPU tile decoding or CPU capture writes. This distinction is essential when
interpreting the cost below.

The SDL fragment adapter batches the packet upload, retains six plane targets
and skips unchanged sources. Byte texture rows are naturally 1024-byte aligned;
words are explicitly converted to little endian. Unsupported rows, reference
padding, winner-dependent color policies and post-scanout hub edits retain CPU
textures. Failed GPU setup also falls back. A castle transition exposed mirror
padding appended by the reference sampler; that source now fails eligibility
instead of presenting a partly exported row. Default-fill and authored
transparent/black/replacement behavior is covered by the packet oracle.

The compute module is shared with the standalone probe. Six atlas bands have
independent capture widths, including the different BG apron policies. Unused
or unchanged bands skip searching and invalidate old confidence. The adapter
uses `ArSdlRenderBackend_SubmitPending` at SDL/raw-GPU dependency boundaries;
`SDL_FlushRenderer` alone would not establish them. Legacy/external renderers
retain their existing path. Target/viewport/clip state is restored, allocation
failure cannot reuse partly configured targets, and room/size/discontinuity
changes invalidate endpoint pairs. Normal execution downloads neither images
nor vectors.

**Remaining duplicate CPU work:** the existing CPU global analyzer still supplies
plane/skybox/effect projection offsets. Dropping that oracle now would detach
lights and mist from the interpolated background. Actor block motion, independent
skybox views, coverage masks, snapshots and all CPU PPU rasterization are also
retained. No claim is made that interpolation as a whole is now GPU-only.

**Deck live comparison:** identical isolated `-03` executable, 3,000 emulation
ticks per run, Fillmore 01:01 right/jump/attack replay, 1280×800 fullscreen desktop
Wayland, 64 extra rows, effects/CRT/DOF/interpolation and the buffered producer,
90 Hz cap. Validation readbacks are off. Each run takes about 50.6 seconds;
roughly 28 seconds of the settled action trace are measured after discarding
its first 180 presentations. The producer column is the last ten one-second
work samples (ms/source); presentation work is trace start-to-completion CPU
wall time (ms/present). The two overlap and must not be added as one frame cost.

| Path | Producer work ms/source | Presentation work ms/present | Cadence p95 ms | Queue copy MiB/source |
| --- | ---: | ---: | ---: | ---: |
| Existing CPU path | 12.136 | 2.278 | 14.824 | 5.107 |
| Indexed GPU replay only | 13.449 | 2.847 | 15.612 | 7.524 |
| GPU global motion only, CPU offset oracle retained | 12.113 | 2.399 | 15.275 | 5.107 |
| Both experiments | 13.521 | 3.048 | 15.789 | 7.524 |

All four average approximately 90 presentations/s and 60.1 source frames/s;
each trace contains over 2,300 fractional presentations. This capped throughput
does not establish spare GPU capacity or uniformly paced 90 Hz. Compared with
the CPU path, the combined experiment adds about 1.39 ms/source and 0.77
ms/present; an earlier independent pair showed the same regression. The indexed
packet costs additional recording, copies and uploads; global motion currently
adds duplicate analysis and atlas work. Average selected-endpoint age at
presentation start rises from 20.48 to 21.04 ms. These are host timestamp ages, not physical
input-to-display latency, GPU timestamps, or changes to the configured buffering.
Normal dynamic clocks and this short replay do not establish a long-session,
Game Mode, power or thermal result.

**Validation:** exact six-band GPU color/padding tests, motion-vector/confidence
comparisons, endpoint and fractional warp checks (≤1 channel-value filter
rounding), independent widths, skipped-band invalidation, target-state restoration,
resizing, stationary pairs, room discontinuities and CPU fallback pass on Metal
and Deck Vulkan. Live capture validation passes forest/cave/castle/lava scenes;
actual buffered interpolation is exercised in the forest, castle and lava runs.
The Deck forest run reports over 1,100 checked plane pairs without a mismatch;
Mac castle reports over 300. The live lava sequence exercises only a few accepted
pairs and is not equivalent to the broader offline motion fixture coverage.
Focused PPU, frame queue, CPU motion/oracle, scene snapshot and renderer tests,
ASan/UBSan checks, shader freshness and strict C11 warnings pass. SPIR-V/MSL/DXIL
blobs are generated; Windows/D3D12 execution and browser compute remain unverified.
The browser's existing CPU/shared renderer path is unchanged.

Evidence, exact environment/settings, replay harness, traces, validation logs
and hashes: `runs/live-gpu-capture-2026-10-02/`. Test executables use separate
names on Deck; the installed game binary was not replaced.

**Next implementation gate:** replace this indexed diagnostic bridge with a
bounded live tile/span program that preserves row-state versions and authored
capture policies, then move the CPU projection/mask/skybox consumers before
removing CPU BG export. Benchmark the removal, not an extrapolation from the
standalone timings. GPU actor block analysis can follow once the background
path meets exactness and whole-game timing gates. Keep the current defaults
until the duplicate work is actually gone and measured pacing improves.

#### GPU-owned backgrounds and interpolation — 2026-10-02

**Status: real CPU work has been removed, but the complete PPU migration is
still open. Keep the experiment opt-in.** The indexed replay comparison above
measured duplicate work. This stage replaces supported background capture with
raw tile commands and gives the GPU ownership of motion analysis and synthesis.

Use `AR_GPU_BG_CAPTURE=owned AR_GPU_BG_MOTION=owned` for the ownership path.
`AR_GPU_BG_CAPTURE=tiles-validate AR_GPU_BG_MOTION=validate` deliberately retains
CPU output/analysis and checks the GPU algorithm against it; exclude that mode
from performance comparisons. The older indexed switches remain diagnostics.

Completed ownership changes:

- Supported Mode-1 BG1/BG2 ordinary/high/far captures record raw 4-bpp tile rows,
  palette and scanline policy. CPU tile decoding and capture pixel stores are
  skipped. Compact native rows store two words per tile. Revision `-15` also
  uses compact native rows when edits/aprons exist: a separate lazy mask stream
  is composed in the shader, removing CPU realignment of ordinary tiles.
  Palettes are shared until CGRAM/brightness/policy changes.
- Independent skybox output is also GPU-owned, including native periodic tile
  pages and captured-world intervals. Coverage comes from raw command bits.
  The queue copies only the used command prefix and omits owned CPU images.
- Global background/skybox/residual motion and four OBJ block-motion fields are
  computed on the GPU. Owned mode performs neither CPU image copies for motion
  nor CPU motion analysis. Actor candidate costs run in parallel; ordered seed
  propagation, tie-breaking, confidence rejection and halo handling match the
  existing CPU algorithm. Synthesis uses GPU-resident fields and textures.
- There is **no gameplay image readback**. A **256-byte global-vector download
  remains** for CPU effect/skybox projection. It is submitted before actor search
  and consumed at presentation preparation, allowing intervening CPU/GPU work
  instead of waiting immediately. The eventual fence wait is included in the
  measured presentation cost. It is not a fully asynchronous, readback-free
  renderer yet. Actor vectors are downloaded only in validation mode.
- Unsupported source policies, a provider switching to authentic tiles midway
  through a row, GPU capability/setup failure, snapshots and post-scanout hub
  edits retain or materialize CPU output. A source is published as owned only
  when all of its required rows are represented. Diagnostic consumers also use
  current commands instead of reading stale owned CPU surfaces.

**Same-build Deck result:** isolated `ActRaiserRecomp-owned-14`, SHA-256
`30398cf8547ec7e2af78165cc193fc333fa768d33482bb75dab4ce8dce4e67ec`, the same
3,000-tick moving Fillmore 01:01 replay and 1280×800/64-extra-row/all-effects
workload as above. Both use the production bounded producer and 90 Hz cap.
The settled trace contains 2,496 presentations per run; cadence is measured
from consecutive presentation starts, not the fixed source-interval CSV field.

| Path | Producer ms/source | Presentation work ms/present | Cadence p95 / p99 ms | Queue copy MiB/source |
| --- | ---: | ---: | ---: | ---: |
| CPU capture and CPU interpolation | 12.315 | 2.388 | 15.044 / 17.127 | 5.107 |
| Owned BG/skybox and GPU interpolation | 10.265 | 2.567 | 13.431 / 17.071 | 3.985 |

This run removes **2.05 ms/source (16.6%)** of producer work and **22%** of queue
copy traffic. Presentation work increases by 0.18 ms/present. These overlapping
CPU wall-time columns must not be summed. Average cadence is 11.111 ms in both,
but p95/p99 remain above the 11.111 ms budget: this does **not** establish reliably
paced 90 Hz. Selected-endpoint age is 20.49 versus 20.71 ms on average, not
input-to-display latency. Earlier revision `-13`, before deferring the vector
fence, measured 2.29 versus 3.30 ms/present and worse GPU-path p95. The shorter
wait in `-14` is promising; thermal/clocks/run variance still require repetitions
and longer representative workloads before promotion.

**Final compact-edit follow-up (`-15`):** profiling `-14` found 9.80% of settled
user-cycle samples in tile command packing, 11.25% in virtual background span
preparation and 5.96% in capture-edit traversal. These are whole-process CPU
sample shares, not GPU timings. That evidence prioritized moving native tile
alignment/edit composition to the shader before the more invasive OBJ migration.

The matched `-15` executable has SHA-256
`8b3e3f691c93038d7b7fcbb4796dc4c36d21dc4bb2c906f919d945b9da3dc4a8`:

| Path | Producer ms/source | Presentation work ms/present | Cadence p95 / p99 ms | Queue copy MiB/source |
| --- | ---: | ---: | ---: | ---: |
| CPU | 12.301 | 2.317 | 14.792 / 17.260 | 5.107 |
| GPU-owned, run 1 | 9.985 | 2.645 | 13.718 / 17.890 | 4.203 |
| GPU-owned, repeat | 10.021 | 2.783 | 13.639 / 16.290 | 4.203 |

The repeat supports a **2.28–2.32 ms/source (about 19%) CPU reduction** in this
workload. GPU-path presentation CPU work remains 0.33–0.47 ms/present higher.
The edit-overlay stream increases copy traffic relative to `-14`, but it is still
18% below the original CPU path; producer work improves by about 0.25 ms/source.
Pacing remains above the 90 Hz budget at p95; tail variation is material. This is
still a migration milestone, not grounds to change the default. The final compact
edit format passes CPU ownership/apron/window/fallback tests, ASan/UBSan and
all-six-band Metal/Vulkan GPU pixel parity, including edit-only cache invalidation
and malformed edit offsets. Final live forest, cave and castle comparisons also pass.

**Without interpolation:** the matched `-13` pair measured producer work
12.248 → 10.317 ms/source and presentation work 1.647 → 1.675 ms/present.
Cadence p95 was 12.722 → 12.809 ms and p99 18.128 → 16.778 ms. Thus ownership
saves producer work independently of interpolation, without evidence of a large
presentation-cost improvement in that mode.

**Correctness and portability:** Metal GPU color tests cover all six bands,
raw/compact/edited formats, palettes, brightness, flips, windows, padding,
independent and aliased skyboxes, periodic dimensions and malformed bounds.
GPU actor tests compare all vectors/flags against the CPU across 18 dense,
sparse, independent-motion, disappearing and disabled cases, including maximum
640×352 captures, exact endpoints and fractional meshes. Deck Vulkan passes the
same actor and raw-background parity checks. Frame-generation tests cover owned
NULL CPU surfaces, resizes, discontinuities, preserved renderer state, and four
queued frames drained without presenting (only the latest download is consumed).
Live forest/cave/castle/lava validation reports no capture or motion mismatch;
unsupported castle transition rows correctly fall back. The attempted Aitos
04:02 harness warp fails its terrain-load contract before GPU capture and is
not counted as validated. Aitos 04:04 passes.

The shader tool now normalizes **named** MSL sampler and texture bindings:
SPIRV-Cross enumeration order can otherwise silently swap previous/current
endpoints despite containing slots 0 and 1. Compute buffer bindings and generated
SPIR-V/MSL/DXIL freshness pass. C11 strict warnings, runtime ABI/contracts,
PPU render-pipeline ownership sentinels, queue/snapshot tests, renderer boundary
checks and focused ASan/UBSan runs pass. Windows/D3D12 runtime and browser compute
remain unverified; browser CPU/shared rendering is unchanged. Separate Deck test
binaries leave the installed game untouched.

**Remaining ownership work, in dependency order:**

1. Export native OBJ tile commands with per-line OAM priority/rotation, window
   policy, VRAM/CGRAM versions and receiver color transforms. Include the apron
   channel's cross-priority first-writer rule and the HUD icon's punch/restore
   semantics before omitting CPU OBJ rasterization. GPU actor *motion* above
   does not remove this sprite-raster work.
2. Move residual main/subscreen composition and the remaining supported capture
   policies onto the GPU. CPU sprite rasterization, HUD capture/repair and final
   residual composition still run today. CPU world/provider lookups also remain.
3. Move effect/skybox projection consumers to GPU-resident transforms, removing
   the small vector transfer/fence without detaching environmental effects.
   CPU effect geometry generation is also still present.
4. Repeat exactness and whole-game timing gates across rooms, settings, supported
   backends and longer Deck runs. Promote only after pacing and fidelity pass;
   do not equate an average 90 Hz cap with meeting every 11.1 ms deadline.

Evidence: `runs/live-gpu-capture-2026-10-02/ownership-comparison-15.json`,
`ownership-comparison-14.json`, `ownership-comparison-13.json`,
`deck-owned-profile-14/`, paired logs/traces, validation captures and the
replay harness in that directory. No default has been changed.

### Scrolling whole-composite parity gate (2026-10-02)

`tools/compare_action_gpu_frames.py` now replays Fillmore 1 with actual walking,
jumps, attacks and camera scrolling, capturing every game frame from 1000–1599.
The primary route traverses 778 horizontal / 188 vertical BG1 pixels and
389 / 63 BG2 pixels. Both paths use the same binary, recorded input, copied SRAM,
room config and settings. Final WRAM hashes and per-frame room/camera coordinates
match. The settings include 64 extra vertical rows, square pixels, 16:10,
skybox-only, environmental lighting/particles, DOF, rim lighting, edge AA and CRT.
The free-camera pose stays fixed to remove wall-clock camera easing from this
pixel test; the level itself scrolls in both directions.

- **Metal (Apple M2):** all 600 source frames match byte-for-byte at 720×448.
  Each of the 25%, 50% and 75% interpolation phases adds 600 compared frames.
  Maximum final-composite channel difference is 4/255; mean absolute error
  per phase is below 0.00012 on the 0–255 scale. At most eight pixels in any
  frame differ by more than two channel levels.
- **Steam Deck Vulkan (RADV VANGOGH 26.1.99):** all 600 source frames match
  byte-for-byte at 360×224. The same 1,800 interpolated frames have a maximum
  channel difference of 3/255. Phase mean absolute errors are 0.0361–0.0434;
  at most nineteen pixels in a frame differ by more than two levels. Many more
  pixels can differ by one level on this backend: interpolation is visually
  close, not bit-exact. Reviewed differences follow filtered texture detail,
  without displaced silhouettes or missing geometry, consistent with sampling
  and quantization differences between the SDL geometry and compute paths.
- A repeated CPU midpoint run matches all 600 frames exactly. The capture clock
  is deterministic, rather than differences being hidden by timing noise.
- An additional 600 midpoint frames per backend disable the no-knockback cheat
  to cover normal player colors and hit reactions. The route still scrolls
  538 / 156 BG1 pixels; results remain within the small interpolation color
  differences. Infinite HP stays enabled for traversal. The primary route's
  white player silhouette is the existing invulnerability cheat, not GPU damage.
- The diagnostic generated-plane masks confirm real synthesis. CPU skips an
  unchanged actor plane at frame 1416 where GPU generates it; final pixels are
  identical at all three phases. GPU background ownership is confirmed for all
  three sources with no rejected sources. These are CPU-vs-GPU comparisons
  *within* each backend, not claims of bit-identical cross-backend output.

Scheduled screenshots now reuse the uploaded FrameSlot rather than recapturing
and breaking interpolation history. `AR_SHOT_PHASE=0..1` selects a fixed phase;
normal screenshots still use the current endpoint. For offline headless-video
comparisons only, `AR_FRAME_CAPTURE_TICK_CLOCK=1` timestamps captures at a fixed
60 Hz so readback/file IO cannot expire motion pairs. It does not change the
game clock or live pacing. Logging records camera coordinates and generated/GPU
plane masks so stationary captures or skipped synthesis cannot masquerade as
coverage. The targeted frame-generation variants and renderer boundary tests
pass. This diagnostic work makes no rendering default change.

Evidence is in `runs/gpu-scroll-parity-2026-10-02/`: per-frame comparison JSON,
input/build hashes, local captures, compact Deck reports, worst-frame difference
images, and a 10-second 720×248 H.264 side-by-side clip under 1 MB. Large raw Deck
frames remain in the isolated `parity-16-*` directories on Deck. These capture
runs deliberately read back every frame and are **not performance measurements**.

This increases confidence for scrolling Fillmore 1 on Metal/Vulkan. It does not
close whole-level/boss/transition coverage, live threaded scheduling,
reactive-camera timing, other room
families, the aspect/plane-policy matrix, Windows D3D12 runtime testing, or the
remaining CPU ownership work above. Keep the GPU path opt-in while broadening
those gates. No visual mismatch in this sample warrants removing the measured
producer-time improvement.

#### Presentation pacing and input stalls — 2026-10-02

The next pass separated input pumping, endpoint preparation/upload, motion
projection fence waits, drawing and backend present return. Enable
`AR_FRAME_PACING_TRACE=<csv>` and analyze with `tools/analyze_frame_pacing.py`.
Upload-only iterations are retained, source work is counted separately, and
epoch changes do not become fake frame intervals. These are CPU wall timings;
backend completion is **not physical scanout**, and producer/presenter durations
overlap. The trace is dormant by default.

Two scheduling changes are now enabled:

- Prepare the interpolation pair for the next presentation deadline during
  available idle time. Compute the displayed phase immediately before drawing.
  This preserves the 1.75-source-period playback delay and three-slot capacity;
  it does not add buffering or extrapolation. `AR_FRAME_STREAM_PREPARE_AHEAD=0`
  retains the previous timing for diagnostic comparisons. Interpolation-off
  still consumes the newest completed endpoint.
- On Linux, poll SDL gamepads on a separate worker at a 4 ms interval. SDL's
  Deck HID watchdog was making three synchronous feature-report calls during
  the main-thread event pump, producing recurring ~8 ms stalls. A child-process
  `strace` confirmed blocking `HIDIOCSFEATURE`/`HIDIOCGFEATURE` on `/dev/hidraw3`;
  this matches the watchdog in the tested
  [SDL 3.4.14 Deck driver](https://raw.githubusercontent.com/libsdl-org/SDL/release-3.4.14/src/joystick/hidapi/SDL_hidapi_steamdeck.c).
  Window/keyboard pumping, event dispatch, bindings and gameplay state remain
  on main. Keyboard/gamepad arbitration uses an atomic physical-activity
  snapshot so per-frame getters cannot block on the worker's joystick lock.
  Hotplug is still serviced; shutdown joins before closing pads and restores
  the previous SDL auto-update hint. Initialization failure keeps synchronous
  polling. `AR_GAMEPAD_POLL_THREAD=0` disables the worker; `=1` also permits
  testing it on other platforms, whose defaults remain unchanged pending
  physical-device validation. The implementation uses portable SDL/C11 APIs.

Matched Deck binary `pacing-19` (`56173c75…0322c`), SDL 3.4.14/Vulkan,
90 Hz fullscreen, 64 extra rows, all effects/CRT and the moving Fillmore replay:
each run lasts 3,600 ticks; analyze settled ticks 1,200–3,300. GPU background and
motion ownership remain opt-in. Times below are milliseconds between backend
present **returns**, with ~3,100 presents per interpolated run.

| Path | p95 interval | p99 interval |
| --- | ---: | ---: |
| GPU owned, previous scheduling/input | 16.104 | 18.879 |
| GPU owned, input worker only | 15.603 | 16.627 |
| GPU owned, input worker + early preparation | 13.329 | 16.597 |
| Same combined change, repeat | 13.772 | 16.632 |
| GPU owned, interpolation off, previous input | 13.084 | 17.338 |
| GPU owned, interpolation off, input worker | 12.453 | 13.479 |

Input-pump p99 falls from 7.899 to 0.039–0.043 ms. Average projection-fence
wait falls from 0.541 to 0.256–0.286 ms; its remaining p99 is ~2.8 ms.
Preparation crossing a draw deadline drops from 372 to 92–134 occurrences.
All observed queue depths remain at most one. The 4 ms device polling cadence
is separate from playback delay; these replay measurements do not measure
physical controller-to-photon latency.

The existing CPU-analysis renderer also passes: with the input worker enabled,
early preparation changes p95/p99 from 13.610/15.835 to 13.581/15.985 ms, within
the observed variation. A shorter paired Mac Metal run (1,800 ticks, settled
1,200–1,700, same executable) improves p95 from 14.404 to 12.089 ms; p99 remains
~15.6 ms. Do not present either backend as stable 90 Hz yet.

Validation includes a blocked virtual-device update while main continues pumping,
button/axis transitions, physical arbitration state, hot removal, repeated
start/stop and hint restoration/failure fallback. These pass on Mac and Deck.
The poller also compiles for Windows x64 and Linux ARM64; that is not runtime
device coverage. Playout tests prove the same endpoint pair/phase across 900
90 Hz deadlines. Existing frame-pipeline, input/settings and Metal motion
oracle tests pass. No shader, image or motion-estimation algorithm changed in
this pacing pass; the scrolling parity evidence above remains applicable.

Evidence: `runs/deck-pacing-2026-10-02/` contains the isolated run harness,
binary/environment provenance, raw traces, logs and `analysis.json`. Syscall
and sampling-profiler runs are diagnostic evidence only, not timing baselines.
Remaining work is the projection readback dependency, upload/submission cost,
and CPU PPU work (including OBJ and residual composition). The new trace makes
their effect on deadline misses measurable independently of the input stall.

The rebuilt defaults (`pacing-20`, SHA-256 `1eb7d074…ee3f3`) reproduce the
result: p95/p99 completion intervals 13.647/16.600 ms, input-pump p99 0.037 ms,
projection-wait p99 2.798 ms. A separate user-cycle sampling run starts after
20 seconds to exclude boot. About 75.8% of sampled CPU cycles are on the frame
producer and 23.0% on presentation; the gamepad worker accounts for ~0.5%.
The leading producer symbols are `render_native_fast_line` (9.9% of all sampled
cycles), `native_resolve_virtual_bg_span` (6.7%), `native_capture_tiles_line`
(4.1%) and `background_packet_tile_to` (4.0%). Scenery capture adds 3.3%; main
thread scenery-shadow preparation adds 2.6%. These percentages exclude blocked
time and GPU execution, so they cannot be read as percentages of frame latency.
They prioritize reducing CPU tile-command/PPU preparation alongside removing
the projection fence, rather than assuming more game-logic HLE is the next win.
The full sample stays on Deck; the report and compact hotspot table are saved
with the local evidence. Stable 90 Hz, D3D12 runtime and broader room/transition
acceptance remain open.

#### Scanline packet preparation — 2026-10-02

Committed the preceding GPU/pacing work as `d6caf024`, then measured two follow-ups.
Parallelizing the ten fine global-motion searches into separate GPU workgroups
preserved the Metal motion oracle and scrolling comparison, but did not produce
a consistent overall Deck pacing improvement. Its p95 completion interval was
13.665/13.269 ms against the intervening control's 13.224 ms. The prototype is
discarded; its patch and comparison remain in `runs/gpu-refine-2026-10-02/` and
`runs/deck-pacing-2026-10-02/refine-analysis.json`. No changed shader is retained.

The retained change prepares the compact packet row once for an unwindowed
virtual BG span. Individual tiles reuse its metadata, pixel pointer and surface
origin instead of repeating row lookup, allocation checks and per-pixel window
iteration. The compact tile encoder is shared with the general writer. VRAM,
palette, provider lookup and scroll still follow each scanline; there is no
cross-scanline cache or added allocation. Window splits and other packet formats
retain the general path. A provider fallback still cancels ownership and
reconstructs the complete CPU row.

Matched Deck Vulkan binary `pacing-22` (`c6c50205…13c3d0`) versus the committed
`pacing-20`, using the same 3,600-tick moving Fillmore replay, 64 extra rows,
effects/CRT, 90 Hz fullscreen and settled ticks 1,200–3,300:

| Interpolated GPU-owned path | Control | Prepared row | Repeat |
| --- | ---: | ---: | ---: |
| Producer mean, ms | 10.075 | 9.139 | 9.219 |
| Producer p95, ms | 10.564 | 9.716 | 10.008 |
| Completion interval p95, ms | 13.378 | 12.568 | 12.718 |
| Completion interval p99, ms | 16.663 | 16.695 | 16.620 |
| Projection wait mean, ms | 0.247 | 0.161 | 0.179 |
| Uploads crossing a deadline | 88 | 15 | 36 |

This removes 0.86–0.94 ms (8.5–9.3%) of average CPU production work. It does
not establish stable 90 Hz: the completion tail remains about 16.6 ms. Queue
depth stays at most one and playback delay is unchanged. These are backend
present-return intervals, not physical scanout measurements. A paired Mac Metal
check (3,000 ticks; analyze 1,200–2,700) reduces producer mean from 4.501 to
4.248 ms; completion p95/p99 changes from 12.133/15.623 to 12.270/15.827 ms,
so no Mac pacing improvement is claimed.

Interpolation-off Deck repeats reduce producer mean from 10.183/10.165 to
9.062/9.049 ms (about 11%). Captured-source age at drawing also falls from
18.854/18.707 to 17.674/17.701 ms; this is not an input-latency measurement.
There is a small pacing tradeoff: completion p95 rises from 12.480/12.494 to
12.787/12.711 ms, while p99 is 13.399/13.579 versus 13.673/13.584 ms. Retain the
CPU saving, but do not claim universal pacing improvement. Endpoint uploads
still cross some deadlines; addressing that without delaying fresh input is a
separate scheduling/upload task. Full results are in `row-nointerp-analysis.json`.

Validation adds main/subscreen ownership and window splits to the packet/CPU
oracle, including every fine-scroll phase, asymmetric flipped tiles, per-line
VRAM/CGRAM writes, authored edits, aprons and fallback partway through a row.
Release and ASan/UBSan tests pass. The PPU translation unit also compiles for
Windows x64 and Linux ARM64; these are compile checks, not runtime coverage.
On Metal, all 600 scrolling source-frame comparisons are byte-exact. Another
600 midpoint comparisons preserve the existing tiny filtering differences
(worst reviewed frame: 89 of 322,560 pixels differ; maximum channel difference
3/255, mean 0.000155/255). A 1,800-tick live Deck validation run also exercises
GPU synthesis with the CPU oracle: zero packet mismatches and zero motion-vector
errors across all 42 reported check batches. Evidence lives in
`runs/ppu-row-2026-10-02/`, with
Deck traces and provenance in `runs/deck-pacing-2026-10-02/row-analysis.json`.

The next architectural target remains the CPU dependency on GPU projection
results: skybox mapping and effect-plane offsets consume those results before
drawing. Removing the fence requires moving those consumers to the GPU while
preserving confidence rejection and endpoint selection. Upload/submission cost,
OBJ work and residual composition remain separate targets. Keep the owned GPU
path opt-in until broader room/transition and D3D12 runtime gates pass.

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
- Extend the format to complete bundled definitions and reusable source/dependency
  bindings; sparse overrides alone cannot close native-default reconstruction.
- Migrate representative existing torch/window light and supported-floor mist
  into recipes while retaining source validation and output.
- Introduce coherent preview event inputs instead of relying on fabricated WRAM
  or linking the entire game just to place a light.
- **Exit:** unchanged defaults match native baselines; edits survive round trips,
  reload and default upgrades; malformed or oversized changes are rejected
  atomically without corrupting the active scene. A complete exported definition
  reproduces its baseline with the corresponding native visual default disabled.

### 4. Deliver the first useful authoring workflow

- Add effect picking/inspector, light handles, floor/free fog brush, overlays,
  presets, duplicate/reset/disable, undo/redo and explicit export/reload feedback.
- **Exit, user exercise:** select a Bloodpool torch and widen its reach without
  increasing peak brightness; add a soft light; paint cool Fillmore floor mist
  across ledges and spike bottoms; undo, export, reload and compare in game.
  Verify skybox-only and plane presentation, retained-frame stability and settings
  ownership. Supply a small native/browser comparison video.

### 5. Cover every established effect family

- Finish actor/contact/spell recipe extraction after the migrated forest, cave,
  marsh, castle, glow and Aitos surface groups. Add missing compound handles
  without changing source ownership or aggregation.
- Replace C-owned visual defaults with bundled configurations through the same
  authoring suite. Add missing reusable capabilities from the reconstruction audit;
  opaque native preset wrappers and approximate substitutions do not count.
- Expose recognized actor/event accent parameters and deterministic preview
  actions. Preserve material recognition, regional applicability and native clocks.
- **Exit:** every existing level-effect family has a coverage-matrix entry with
  editable defaults, working export/reload, native/browser tests and review
  evidence. Existing compound effects are selectable and retain their variation.
  Each family also passes config-only reconstruction and equal-cost acceptance
  from the native-default contract, including every required dependency.

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

### 6a. Camera-aligned editing and detached live preview

Queued by the user on 2026-10-01, **after** the remaining default migrations and
planned effect additions in phases 5–6. These improvements are not implemented.
The current full-map canvas offsets/repeats the other background for inspection;
its combined view is not aligned through the room's camera/parallax rules.

- Add effect picking and drag/resize handles over the shared renderer. Resolve
  their positions and inverse dragging through the same BG1/BG2 and plane/skybox
  projections as their effects. Keep the full-layer map for artwork editing.
  BG2 fixed-point and surface-raster anchors must remain in their owning map's
  coordinates, including with static BG2, independent X/Y parallax and water
  bands. Camera scrubbing must show their changing relationship to BG1.
- Allow the shared renderer to open in a separate popup window while the main
  editor remains available for map edits and effect configuration. Stream current
  in-memory edits, selected room/terrain, camera, animation time and view settings
  to that preview without export/reload or scene captures. Maintain one document
  and undo history; expose how navigation/time controls are linked.
- Reuse the existing shared rendering path. Coalesce edit updates, retain assets
  and update only changed resources. Avoid running a redundant hidden embedded
  renderer while the popup is active; bound any additional renderer resources.
  Closing/reopening the popup must preserve edits, and a blocked popup must leave
  the embedded preview usable. Handle resize and graphics-context recovery.
- **Exit:** place/move BG2 sources while observing their actual relationship to
  BG1, then scrub both axes and time in static, parallax and raster-band rooms.
  Check flat/plane/skybox presentation, aspect/zoom/extended rows, supported effect
  handles, live tile/effect edits and undo/redo in the detached preview. Verify
  open/close/reopen synchronization and measure edit latency, memory and frame
  cost with only the intended preview rendering.

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
- [ ] Every native default can be completely exported and reconstructed with its
      C-owned visual default disabled, using ordinary authoring controls and the
      same optimized kernels, aggregation and resource budgets.
- [ ] Config-only reconstructions preserve appearance/motion and show no measured
      performance regression on the supported native targets.
- [ ] All established families pass the coverage matrix and persistence checks.
- [ ] Later-stage baseline treatments are authorable; optional experiments have
      explicit status and extension points.
- [ ] After effect coverage is complete, camera-aligned effect editing and a
      detached shared-renderer window reflect live edits with shared undo/state
      and verified synchronization, resource bounds and performance.
- [ ] Flat, plane and skybox compatibility is verified beyond the initial rooms.
- [ ] User overrides survive upgrades and projects export/reload reproducibly.
- [ ] Authoring limits prevent native-pool interference and rendering overload.
- [ ] Browser packaging, target-platform measurements and user visual review pass.

Continue with remaining actor/contact/spell recipe extraction, compound
source handles, material/slope overlays and visual acceptance. Close phase 2's
representative live-scene image/edit comparisons before replacing the legacy
Diorama view, and keep native target-platform acceptance separate from browser
and CPU test results.
