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

Priority update, 2026-10-02: **GPU-owned action composition comes first**, ahead
of the remaining actor/default migrations and editor UX work. The user explicitly
prioritized replacing the CPU-projected SDL_Renderer scene contract so that
previously constrained solutions can be reconsidered. Follow the
[compositor migration and acceptance gates](#next-architectural-milestone-gpu-owned-action-composition)
below. Keep those deferred editor tasks in the queue and preserve the shared
native/browser rendering goal; this priority change does not close their gates.

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

#### Projection submission follow-up — 2026-10-02

Committed the scanline packet change as `aae8fc07`. The next small change appends
the 256-byte projection download to global analysis instead of submitting a
separate command buffer for it. Its fence still precedes actor search. Presentation
queues motion synthesis and destination copies before consuming projection
metadata, so independent preparation can overlap the remaining wait. GPU confidence
rejection still selects the current endpoint; only accepted planes and their
offsets are exposed. No motion algorithm, playback delay or queue limit changes.
`AR_GPU_PROJECTION_PREPARE_FIRST=0` retains the earlier wait-before-preparation
order for diagnostic comparisons, with the merged submission still enabled.

Matched Deck Vulkan runs use the preceding 3,600-tick replay/settings and settled
ticks 1,200–3,300. Candidate `pacing-23` is SHA-256 `1dc7e1c2…458e5c`:

| Metric, ms | Control | Candidate | Control repeat | Candidate repeat |
| --- | ---: | ---: | ---: | ---: |
| Upload preparation mean | 2.130 | 2.003 | 2.033 | 1.967 |
| Projection wait mean | 0.188 | 0.139 | 0.140 | 0.141 |
| Projection wait p99 | 2.684 | 2.523 | 2.639 | 2.570 |
| Completion interval p95 | 12.816 | 12.511 | 12.377 | 12.643 |
| Completion interval p99 | 16.757 | 16.337 | 16.485 | 16.437 |

Retain the simpler submission path and modest upload-preparation saving, but do
not claim a consistent overall pacing improvement: the repeat reverses the p95
change. The main-thread projection dependency remains, and 90 Hz is not closed.
The diagnostic early-wait run gives upload mean 1.994 ms and completion p95/p99
12.626/16.659 ms; that also argues against treating late consumption alone as a
large win. CPU scopes overlap and are not GPU timer measurements. Completion is
the backend present return, not physical scanout or controller-to-photon latency.

All four Metal frame-generation test modes pass, including changed uniform
endpoints whose rejected motion must not expose a prepared image or stale offset.
The 600 scrolling midpoint comparisons retain exactly the previous filtering
difference statistics (worst reviewed frame: 89 pixels, maximum channel difference
3/255). A live Deck CPU-oracle run exercises synthesis with zero packet mismatches
and zero vector errors across 43 reported check batches. Evidence is in
`runs/gpu-stall-2026-10-02/` and
`runs/deck-pacing-2026-10-02/{stall-analysis,stall-repeat-analysis}.json`.
D3D12 runtime and broader room/transition acceptance remain open.

#### Next architectural milestone: GPU-owned action composition

The limitation is the current rendering contract, not merely SDL call overhead.
`ArRenderBackendOps.draw_geometry` accepts already projected CPU vertices and
individual textures. SDL_Renderer's GPU render-state extension supplies custom
fragment shaders and GPU resource bindings, but no custom vertex shader. Our
CPU skybox mapping, effect clipping and plane projection therefore consume GPU
motion results before drawing. The current path also packs endpoint textures
into motion atlases and copies warped atlas slices back into per-plane textures.
Those round trips between representations and submission boundaries remain even
when every individual shader is fast.

Keep SDL platform services and SDL_GPU's portable Metal/Vulkan/D3D12 backend.
Use direct SDL_GPU passes for action composition, following the native GPU pass
pattern already used by SIM3D and actor warping. Replacing SDL with raw Vulkan
would retain the dependency if the CPU-projected rendering contract stayed intact.
The prioritized migration is:

1. Define a backend-neutral scene-pass contract carrying source-space geometry,
   plane/camera parameters, endpoint resources and GPU motion bindings. Give the
   action compositor explicit resource lifetime and pass ownership, with one
   deliberate handoff to any retained SDL_Renderer UI/presentation work. Do not
   multiply Renderer/GPU flush boundaries for individual effects.
2. Draw BG/skybox planes directly from resident endpoints and motion resources.
   Move projection and confidence/endpoint selection into shaders, and fuse
   synthesis with sampling where parity and measured cost permit. Remove redundant
   atlas-to-plane copies as their consumers migrate; do not assume all passes
   can safely be fused.
3. Migrate attached effects' projection, finite clipping and visibility alongside
   those planes. Preserve skybox mapping, bow/rake/folds, masks and occlusion;
   applying a 2D offset after CPU projection is not equivalent. Keep gameplay,
   emitter/event simulation and coherent frame snapshots on the CPU where useful.
   Remove production projection readback only when every live consumer has moved;
   retain explicit oracle readback for diagnostics.
4. Preserve the CPU/browser fallback and shared projection semantics. Compare
   scrolling, actor movement, confidence rejection, fades and room transitions
   across plane/skybox combinations, aspect ratios, zoom and extended rows before
   making the new path the default. GPU projection must not change authoring data
   or weaken the editor's native-preview parity contract.
5. Accept using repeated full-frame Deck 90 Hz and non-interpolated measurements,
   plus Metal and D3D12 runtime coverage. Record CPU waits, GPU pass times,
   submissions, copy traffic and memory; require unchanged playback buffering.
   An isolated shader win or lower average CPU cost does not close pacing.

The first source-space pass is implemented and exercised by a standalone GPU
test, and now backs an opt-in live endpoint packing pass (see the handoff
measurements below). **It does not yet replace final scene composition or remove
the game's projection download**. `DioramaSceneDraw` carries capture-space vertices, a plane
or skybox-band mapping, finite bounds, blend and a motion-result binding.
`ArGpuActionScenePass_Encode` consumes the GPU result directly in its vertex
shader. Its caller records analysis/warp, scene rendering and final presentation
in order; the pass never submits, waits, downloads, or owns a swapchain. Existing
affine triangle interpolation is retained deliberately. Rake, bow, world Y offset
and overflow-fold equations follow the CPU projection oracle.

One retained, cycled upload covers the bounded vertex/index list and one render
pass preserves draw order. Native textures/atlas rectangles bind separately from
the portable scene description. The proof draws directly from the resident warp
atlas, without copying its slices into per-plane textures. It retains the warp
itself: fusing its filtering with final sampling is a separate parity/performance
experiment, not an assumed free optimization. Complete-list preflight rejects
unsupported materials, invalid indices/mappings, capacity overruns and target
feedback before recording the pass. This first adapter supports opaque, alpha
and additive materials only.

The GPU fixture compares **35 scene/phase images per backend** against CPU motion
analysis, the shipped CPU projection routines and SDL_RenderGeometry. Metal and
Steam Deck Vulkan pass independent X/Y motion, phases 0/.25/.5/.75/1, rejected
uniform pairs, viewport/aspect changes, skybox cropping, hidden capture margins,
352/224 height scaling, rake/bow/fold geometry, attached additive light and alpha
particles, finite skybox clipping and retained-resource reuse. No GPU result is
downloaded until the final comparison image. Interior channel error is at most
2/255 in these runs. Each backend has one image with one different boundary pixel
whose centre is within .01 output pixel of a CPU-projected edge. The test allows
up to four such pixels and 3/255 interior error, with a separate mean-error bound;
it must not mask interior displacement as a generic image tolerance.

The shader tool now verifies/remaps named graphics storage-buffer bindings as
well as compute bindings: Metal places uniforms before storage, while SPIR-V and
DXIL retain their separate resource sets. MSL/SPIR-V/DXIL blobs are generated
offline. Strict C11 checks and Windows x64/Linux ARM64 compile checks pass;
D3D12 runtime remains open. Existing frame-generation and GPU block-motion tests
pass on Metal, as do ASan/UBSan checks, render-boundary checks and the release
build. This is functional evidence, **not** a live 90 Hz or end-to-end
performance result. Evidence is under `runs/action-compositor-2026-10-02/`.

**Live handoff checkpoint — 2026-10-02.** `AR_GPU_FRAME_HANDOFF=all`
now routes endpoint packing through the native source-space pass, records global
analysis in that same command buffer, and copies warped planes in one native
copy pass. `unpack` isolates the output-copy change; absent/`0` retains the
reference handoff. RGBA8 generated targets match the compute atlas format, while
packing samples source textures to preserve RGBA/BGRA conversion and transparent
RGB. Padding is cleared at allocation/extent changes, retained without cycling,
and never reused across an invalidated capture region. Pair timestamps,
confidence rejection, unchanged sources and renderer state restoration remain
owned by the existing frame-generation transaction. Command recording/submission
failures invalidate the endpoint instead of exposing partially packed data.

This is an intermediate resource migration, **not the architectural performance
comparison**. It still warps into atlases, copies them into per-plane textures,
projects effects on the CPU and downloads the 256-byte global motion result.
The expected architectural gains require removing those dependencies; repeating
or tuning this bridge alone is not the next performance priority.

Moving Fillmore with all established effects, CRT, 64 extra rows, GPU-owned
capture/motion and a 90 Hz cap was replayed for 3,600 ticks per Deck run. Settled
ticks 1,200–3,300 contain 3,126 presents per run. Native and reference use the
same executable/settings/replay; the second pair reverses their order.

| CPU/presentation metric (ms) | Reference | Native handoff | Reference repeat | Native repeat |
| --- | ---: | ---: | ---: | ---: |
| Upload/preparation mean | 2.002 | 1.988 | 1.995 | 2.025 |
| Draw work mean (includes projection wait) | 1.289 | 1.305 | 1.284 | 1.280 |
| Projection wait mean | 0.134 | 0.134 | 0.126 | 0.118 |
| Completion interval p95 | 12.573 | 12.530 | 12.466 | 12.316 |
| Completion interval p99 | 16.542 | 16.468 | 16.511 | 16.371 |

No material speedup is established. In particular, tail completion intervals
remain above the 11.11 ms refresh budget. These are CPU wall scopes and backend
present-return intervals, not GPU timestamps, physical scanout or input latency.
The unpack-only sample is likewise flat (draw mean 1.321 ms, completion p95/p99
12.639/16.480 ms). The bridge stays opt-in.

Visual evidence: **600/600 moving Metal frames and 250/250 moving Vulkan frames
are byte exact** at midpoint, including matching generated-plane masks, camera
coordinates and final WRAM. The twelve-plane native handoff fixture is byte exact
on both backends for RGBA/BGRA sources, transparent RGB, crops, repeated writes
and smaller replacement extents with cleared padding. Eight Metal scene-pass /
frame-generation tests pass, including production-adapter native-all/native-unpack
modes, rejected pairs and restored renderer state. ASan/UBSan, strict C11 and
Windows x64/Linux ARM64 handoff compile checks pass. D3D12 runtime remains open.
Bloodpool acceptance is not claimed: the generic Fillmore route left that room,
and its stationary retry did not exercise GPU synthesis. An initial Metal timing
pair rebuilt its executable between variants and is excluded. Recordings with
incomplete capture schedules were also excluded.

[Measurements and provenance](evidence/action-gpu-handoff-2026-10-02/measurements.json)
and the [isolated timing probe](evidence/action-gpu-handoff-2026-10-02/probe.py)
are retained; full local traces/captures are under `runs/gpu-handoff-2026-10-02/`.
The next performance gate is **live GPU projection plus direct resident sampling
with real materials/effects**, removing readback and the obsolete intermediate
work before interpreting the full architecture's speed. Keep the handoff as a
migration/regression baseline, not evidence for or against that design.

**Immediate integration gates, in order:**

- Replace the live scene's per-plane texture handoff with coherent resident
  endpoint/atlas and motion bindings, tagged to the same frame epoch/pair. Preserve
  drained-source handling, rejected/unchanged pairs, resets and device recreation.
- Migrate actual source-space BG/skybox meshes and attached-effect construction.
  Preserve skybox camera-follow/crop/band selection and static-source semantics.
  Effect kernels currently mix per-point projection with screen-space offsets,
  local derivative scales and CPU clipping: passing their final screen vertices
  through an inverse transform is not an exact migration. Port those operations
  explicitly, including near-camera rejection and curved finite boundaries.
- Cover all existing materials and passes: priority/color-math surfaces, exposure,
  actor/scenery receivers, shadow grouping, edge AA, DOF/rim, overflow/skirts,
  heat/refraction, HUD/CRT handoff and authored blend modes. Unsupported scenes
  retain the complete existing path, rather than silently dropping an effect.
- Run moving Fillmore and Bloodpool image sequences with every established effect
  active, then representative Aitos/water/band/transition cases. Exercise the
  browser/shared-renderer contract before removing production readback and before
  accepting any default switch. Finally repeat Deck 90 Hz/non-interpolated and
  desktop performance measurements with the CPU work actually removed.

**Reopened options and limits:**

| Option | Why revisit it / remaining constraint |
| --- | --- |
| GPU projection and effect placement | Custom vertex stage can read the resident motion result; first pass proves the dependency can stay GPU-side. |
| Direct atlas sampling and combined scene passes | Per-plane SDL textures and renderer flush boundaries are no longer required by the new contract. Preserve blend/pass order and measure bandwidth. |
| Retained source meshes / GPU particle expansion | Source geometry no longer depends on each camera projection. Reuse world/capture geometry or instances where topology and effect clocks permit; avoid replacing small cheap CPU tasks with costly dispatches. |
| Compact masks and explicit depth/occlusion passes | Native GPU resource formats and depth targets can bypass Renderer format/depth restrictions. Check format support and port mask semantics; a depth buffer cannot replace SNES priority/color math or transparent ordering wholesale. |
| Fused interpolation, shading and final sampling | New pass ownership makes this testable. Intermediate UNORM rounding, edge sampling, DOF and nonlinear projection must be compared before removing resolves. |
| Worker-owned native GPU preparation | SDL_GPU permits offscreen command buffers acquired, encoded and submitted by a worker. Replace remaining Renderer calls and shared mutable resources first; preserve the existing frame delay and ordered dependent submissions. See the threading audit below. |
| Parallel fine-motion search | The earlier prototype was rejected for inconsistent measured gains, not an SDL_Renderer prohibition. Keep its evidence; retest only when changed workload or scheduling gives a specific reason. |
| Explicit async-compute queues / GPU timestamps | Direct SDL_GPU command buffers do not by themselves prove concurrent queues or provide measured GPU execution time. Do not relabel CPU timers or promise overlap from a new pass layout. |

Assess lower-level API limits only if profiling the migrated path identifies an
actual remaining SDL_GPU restriction. Keep SDL platform services and the portable
GPU backend unless that evidence justifies a separate backend experiment.

**Threading re-evaluation — 2026-10-02.** The old
[SDL_RenderGeometry](https://wiki.libsdl.org/SDL3/SDL_RenderGeometry) contract
requires main-thread rendering. Native
[GPU command buffers](https://wiki.libsdl.org/SDL3/SDL_AcquireGPUCommandBuffer)
may be prepared on workers, but each must be acquired, used and submitted on
the same thread. [Swapchain acquisition](https://wiki.libsdl.org/SDL3/SDL_WaitAndAcquireGPUSwapchainTexture)
still belongs to the window-creating thread. Keep input/window/presentation on
main; there is no reason to replace SDL platform services to use this split.
Ordered submissions and CPU parallel recording do not establish simultaneous
GPU compute/graphics execution or expose an explicit async-compute queue.

The `actraiser_gpu_frame_handoff_worker` fixture now checks main-thread upload,
worker-owned native packing, and main-thread consumption. A persistent worker
owns command acquisition/encoding/submission. A semaphore publishes submission
before the consumer records dependent work; **there is no GPU fence wait at
that handoff**. The final readback oracle alone waits for completion. Sixty-four
updates of twelve RGBA/BGRA planes, changing content/crops/padding, are byte exact
on Metal and Steam Deck Vulkan. Main acquires its consumer command buffer while
the worker prepares its producer command. This validates ownership and backend
ordering, not live scheduling speed, concurrent frame-resource reuse, swapchain
thread migration or D3D12 runtime behavior. No production worker/default changes
are made by this fixture.

Re-analysis of the four existing handoff timing traces (same settled ticks
1,200–3,300; no new performance runs) narrows the opportunity:

| Deck run | Endpoint preparations between presents / total | Completion intervals >15 ms | Of those, motion wait >1 ms | Mean wait in those intervals |
| --- | ---: | ---: | ---: | ---: |
| Reference | 1,942 / 2,068 | 80 | 80 | 2.376 ms |
| Native handoff | 1,945 / 2,067 | 82 | 82 | 2.383 ms |
| Reference repeat | 1,941 / 2,067 | 82 | 82 | 2.318 ms |
| Native repeat | 1,963 / 2,065 | 70 | 70 | 2.364 ms |

Endpoint preparation averages ~2 ms, but most already happens between presents;
that whole duration is **not** an available per-present saving. All 314 >15 ms
intervals include >1 ms projection-result waiting; 313 also upload in the same
iteration. This is an association in CPU/present-return traces, not a GPU-time
measurement or proof that threading alone eliminates the tail. Remove the CPU
motion-result consumer and prototype overlapping preparation together.

| Candidate | Decision and ownership requirement |
| --- | --- |
| Native upload/decode and endpoint-analysis worker | Highest-priority threading experiment after resident resources are integrated. Consume an immutable owned frame; keep its upload buffers, pass scratch and endpoint resources exclusively owned until safely published/reused. Main can draw the previous eligible pair while preparation runs. |
| Parallel CPU scenery/effect preparation | Possible, but it was not prohibited by SDL when it used only CPU data. `CaptureScenery` currently borrows WRAM/VRAM/edit callbacks; shadow rasterization has overlapping coverage writes; effect presentation owns shared static scratch/caches. Snapshot or join borrowed data, partition output, and assemble in deterministic blend order. Re-profile after GPU projection/mask migration before adding work that the migration will remove. |
| Threading motion search / fence polling | Search already runs on GPU. Moving encoding or polling to another CPU thread does not remove the CPU projection dependency. Keep the rejected GPU fine-search experiment separate from CPU threading proposals. |
| Parallel live PPU scanlines | Not a safe drop-in optimization. HDMA, per-line VRAM/CGRAM and shared packet/cache state impose ordering. Freeze raster inputs before any parallel decoding; prefer the planned GPU packet decode rather than duplicate legacy CPU work. |
| More generic render helpers | `HostParallelWork` currently serves SIM/world navigation, not the action presentation path. `AR_RENDER_WORKERS=3` does not mean action rendering is already split across three helpers. Test one coarse native preparation worker first; extra workers can compete with the game/audio threads and GPU for Deck resources. |

The game/PPU producer and Linux gamepad polling are already on persistent
workers. Do not count those existing gains a second time. The ~9.1–9.2 ms
producer scope overlaps presentation; it cannot be added to ~2 ms upload and
~1.3 ms draw work to estimate a serial frame cost.

**Worker integration acceptance:**

1. Give the native preparation stage a single owner and a bounded immutable job
   tagged with epoch, source tick and frame-pair identity. Existing SPSC packets
   cannot be released while a worker still borrows their contents; introduce
   explicit ownership transfer or hold them until preparation acknowledges the
   borrow. Snapshot settings, never read live runner globals on this worker.
2. Keep in-flight GPU resources separate from the pair being displayed. Explicitly
   manage upload-buffer cycling and reuse after last GPU use; CPU submission is
   not GPU completion. Keep control metadata on main or publish immutable
   results, rather than concurrently touching `s_gpu`, Renderer texture wrappers
   or effect caches. Join/cancel obsolete jobs on resets, room changes, device
   recreation and shutdown; reject stale epochs before display.
3. Acquire/submit each offscreen command buffer on its worker. Publish successful
   submission before main records/submits dependent passes. Final swapchain
   acquisition/presentation stays main. Failure must invalidate that endpoint
   and preserve the supported fallback, never expose partially prepared work.
4. Compare the **same fully migrated native path** with preparation inline versus
   one worker. Separately retain the old renderer as a visual oracle. Preserve
   the 1.75-source-period interpolated playout policy and bounded queue; do not
   add a queued frame to hide work. For non-interpolated output, preserve its
   current source-age policy too.
5. Measure worker wake/record/submit time, endpoint-ready time, main waits,
   producer duration, queue depth and input/source age alongside present p95/p99.
   Repeat reversed-order moving Fillmore/Bloodpool comparisons, interpolation on
   and off, on Deck and Mac. Verify frame masks/pixels and transitions; D3D12
   runtime remains a separate portability gate. Accept only measured pacing or
   latency improvement without visual or producer regressions.

**Live preparation-worker experiment — 2026-10-02.** The narrow first threading
slice is now available with `AR_GPU_PREPARE_WORKER=1`, alongside
`AR_GPU_FRAME_HANDOFF=all` and `AR_GPU_BG_MOTION=owned`. One persistent worker
records/submits native endpoint packing, global analysis/download and actor
analysis. Inline and worker variants execute the same native job and preserve
the projection-fence-before-actor-search order. Packet validation/decode, raw
texture uploads, CPU projection and its 256-byte result download are still in
the existing path. This experiment isolates command-preparation overlap; it is
not the full worker-owned upload/scene architecture described above.

The job contains copied descriptors/extents and native resources only. It reads
no FrameSlot/WRAM/settings. Main joins CPU submission before source texture
overwrite/destruction, next capture, preparation or reset. Reset stops the worker
before resource release. Jobs are single-flight; there is no new frame queue,
playout delay or GPU-completion wait at the thread handoff. Failed work cannot
publish an endpoint. `AR_GPU_PREPARE_TRACE` records queue/start/submit/join times
without moving shared presentation metrics onto the worker.

Four 3,600-tick Deck runs use one pinned binary, moving Fillmore, all established
effects, CRT, 64 extra rows and the 90 Hz cap. Order is inline, worker, worker,
inline; settled ticks 1,200–3,300 contain 3,126 presents per run.

| CPU/presentation metric (ms) | Inline | Worker | Worker repeat | Inline repeat |
| --- | ---: | ---: | ---: | ---: |
| Upload/preparation mean | 1.985 | 1.842 | 1.807 | 2.006 |
| Draw work mean, including projection wait | 1.303 | 1.307 | 1.308 | 1.343 |
| Projection wait p99 | 2.620 | 2.579 | 2.558 | 2.644 |
| Completion interval p95 | 12.699 | 12.531 | 12.598 | 12.724 |
| Completion interval p99 | 16.531 | 16.536 | 16.551 | 16.632 |
| Game/PPU producer mean (overlapping, not additive) | 9.353 | 9.281 | 9.335 | 9.351 |

Main preparation drops by roughly 0.14–0.20 ms, but **no material p99 improvement
is established**. Native packing/analysis command work is only 0.169–0.171 ms
inline. On the worker, its wall scope takes 0.268–0.269 ms plus 0.045–0.047 ms
wake-up; the main submission join averages 0.012–0.013 ms. The latter is not the
GPU projection fence wait. These wall scopes do not distinguish CPU execution
from driver contention or preemption. About 6% of jobs are unfinished when main
reaches the join. Upload savings therefore cannot be counted as a matching
reduction in total work or endpoint-ready latency. Queue p95/p99 remains one;
input-sample age at draw remains roughly 21.7 ms mean with no physical input
latency claim.

One matched Mac pair likewise does not support enabling this by default: upload
mean 0.653 → 0.605 ms, draw mean 0.559 → 0.555 ms, completion p95 12.286 → 12.378
ms and p99 15.305 → 15.546 ms. The Mac sample has no reversed-order repeat and
does not establish a small regression either. All figures are CPU scopes and
backend present-return intervals, not GPU timestamps or scanout measurements.

Keep the worker **opt-in**. This is a small overlap opportunity, not a solution
to 90 Hz pacing. Do not add workers to chase the remaining projection stall:
migrate its consumers to resident GPU projection and migrate the larger
remaining upload/decode stages. Revisit whole-frame preparation concurrency
with separate in-flight resources once those stages are available. Interpolation
off does not execute this worker, so no non-interpolated improvement is claimed.

Release Mac/hermetic Deck builds and strict C11 checks pass. Metal frame-generation
tests cover seven modes; the native inline/worker tests additionally cover reset
immediately after dispatch and repeated lifetimes. ASan/UBSan passes for the
frame-generation/lifecycle fixture (leak detection disabled; linked backend/SDL
support libraries are not instrumented). Moving Fillmore midpoint replays match
all 250 frames exactly on Metal and all 250 on Vulkan, with matching camera,
generated-plane masks and WRAM. D3D12 runtime remains untested. Full
trace/provenance and comparison data are under `runs/gpu-worker-2026-10-02/`.

**Large-stall priority — 2026-10-02.** Stop optimizing command-worker wake-up
as the primary pacing effort. The remaining production dependency is the CPU
consumer of resident GPU motion: `ApplyGpuResults` waits before updating accepted
plane masks, effect capture offsets and finite-skybox placement. Port those
consumers to source-space GPU composition together; moving only the fence poll
to a worker does not remove the dependency. Keep the current frame delay and
bounded queue, and reject a shortcut that simply omits attached effect motion.

The next measured slice is `AR_GPU_BG_DECODE=native`: upload integer packet words
directly to a cycled GPU storage buffer, then decode requested priority bands in
one compute dispatch per source (BG1, BG2, independent skybox). This replaces the
RGBA packet staging texture, texture-lock/repacking loop and per-band Renderer
passes. Background decoding was already GPU-based; the experiment changes its
resource and submission architecture, not ownership of the game/PPU producer.
It also runs without interpolation. The existing fragment decoder remains the
default and unsupported native setups fall back. Runtime native failure latches
the caller's CPU reconstruction until renderer reset, with a diagnostic.

Packet validation, changed-source detection, band semantics and alpha/palette
policies are shared with the reference. Upload/compute commands are ordered
after prior Renderer consumers. Native output wrappers are borrowed only until
reset; teardown submits pending consumers before releasing wrappers/resources.
This slice does not add a worker, buffering, frame delay or CPU fence wait.
It makes the decode stage usable from an eventual coarse preparation worker;
source lifetime and endpoint resource ownership must still be separated before
that integration. D3D12 shaders are built offline, but runtime testing there is
still outstanding.

The final decoder's four-run order is fragment, native, native, fragment on each
host, using a pinned binary per host and settled ticks 1,200–3,300 (3,126 presents
per run). The preparation worker is disabled to isolate this resource-path
change. CPU/present-return timings:

| Deck metric (ms) | Fragment | Native | Native repeat | Fragment repeat |
| --- | ---: | ---: | ---: | ---: |
| Upload/preparation mean | 2.000 | 1.559 | 1.567 | 2.045 |
| Projection wait p99 | 2.547 | 2.131 | 2.261 | 2.535 |
| Completion interval p95 | 12.329 | 12.292 | 12.312 | 12.367 |
| Completion interval p99 | 16.432 | 14.963 | 15.310 | 16.409 |

This saves about 0.44–0.48 ms of main preparation and improves the tail by about
1.1–1.5 ms. Queue p95/p99 remains one; mean input-sample age at draw is roughly
21.5 ms native versus 21.7–21.8 ms reference. These are not physical input-latency
or GPU execution measurements. Producer means remain around 9.0–9.1 ms and
already overlap presentation on their own worker.

Metal also repeats the improvement: preparation 0.662/0.670 → 0.491/0.500 ms;
completion p99 15.632/15.631 → 13.893/14.119 ms. Its p95 does not consistently
improve, and producer wall time varies across runs, so do not generalize the
entire distribution from the tail improvement alone.

One non-interpolated Deck pair saves preparation time too: 1.686 → 1.245 ms;
completion p95 12.731 → 12.263 ms and p99 13.742 → 12.727 ms. Draw means are
0.930 → 0.948 ms; no motion-result waits occur in either variant. This pair has
no reversed repeat, so it establishes a promising broader benefit rather than
a final default decision.

Keep native decode opt-in pending wider room/backend qualification. It advances
the architecture and measurably improves this workload, but it does not yet
provide reliable 11.1 ms completion intervals. The remaining 2.1–2.3 ms p99
projection wait is the next primary target. The packet oracle covers all four
row formats, alpha, palettes, edits, skybox aliases, padding, dimensions and bad
inputs. Final Metal/Vulkan fixtures also queue changing texture revisions with
no intermediate readbacks; every saved revision remains exact. Metal's legacy
Renderer fallback and fully instrumented project-side ASan/UBSan fixture pass
(SDL itself is not instrumented; leak checking disabled). Final-binary moving midpoint
image comparisons match all 250 frames on Metal and all 250 on Vulkan exactly,
including camera positions, generation masks and final WRAM. Full hashes, timings, validation scope and
per-frame reports are under `runs/gpu-bg-decode-2026-10-02/`.

#### Resident effect projection without motion readback — 2026-10-02

The complete moving Fillmore Act 1 workload now has an opt-in path that removes
the motion-metadata download and its CPU fence wait. Select
`AR_GPU_EFFECT_PROJECTION=resident` with `AR_GPU_BG_CAPTURE=owned`,
`AR_GPU_BG_MOTION=owned`, and `AR_GPU_FRAME_HANDOFF=all`; the measurements also use
`AR_GPU_BG_DECODE=native`. Defaults remain unchanged.

Existing recipes still produce bounded source-space primitives on the CPU.
Compute shaders perform finite clipping, projection, particle sizing, leaf
geometry and scenery-shadow sampling directly from resident motion/confidence.
The finite skybox follows the camera and fits its aspect on the GPU. OBJ-only
accents keep their existing CPU projection, which does not consume background
motion metadata. Effects, shadows and particles are not omitted to obtain the
result, and neither frame delay nor queue capacity was increased.

The source packet has fixed bounds (4,096 primitives, at most 15 output vertices
per primitive); uploads and output buffers are retained/cycled. Renderer/native
submissions preserve ordering without a result fence. CPU masks now identify
candidate planes on this path; the shaders apply confidence rejection and use
zero displacement/current-endpoint pixels when motion is rejected.

Qualification is deliberately limited to the complete forest recipes in 0101,
OBJ accents and a single finite captured skybox band. Authored additions, other
recipe families, ROM/periodic skyboxes and unqualified rooms retain the reference
readback path. Unsupported primitive construction or failed native submission
fails explicitly rather than silently losing an effect. This is a prototype
qualification boundary, not completion of all-room GPU migration.

Final matched scrolling comparisons cover 250 frames per pair, with BG1 moving
638 x 188 pixels and BG2 319 x 63. Metal covers phases .25/.5/.75 (the .25/.75
runs use camera tilts 120/-100 mrad), and Vulkan covers .25/.5 with the same
tilted-camera check at .25. Every frame is within one 8-bit color unit; exact
counts are 215/219/211 on Metal and 228/227 on Vulkan. Camera positions, replay,
settings and final WRAM match. The largest changed-pixel count is 135 of 322,560
on Metal; Vulkan's worst is 17 of 80,640. These image-capture dimensions are
separate from the 1280x800 fullscreen Deck performance runs.

The image comparison caught two actual implementation differences: a linear
sampler used on a nearest-filtered skybox, and GPU division/UV operation order
crossing nearest-sampling boundaries. The adapter now honors texture filtering,
and skybox fitting corrects the division residual and preserves endpoint
arithmetic. The original band-sized differences are gone; the remaining tiny
differences are color/raster rounding, not displaced background rows.

Four 3,600-tick runs per host use a pinned executable in reference, resident,
resident, reference order. Settled ticks 1,200–3,300 contain 3,125–3,126 presents
per run at a 90 Hz cap, with effects, CRT and 64 extra rows. The preparation
worker is disabled. These are CPU scopes and backend present-return intervals,
not GPU execution timestamps, physical scanout or input latency.

| Deck metric (ms) | Reference | Resident | Resident repeat | Reference repeat |
| --- | ---: | ---: | ---: | ---: |
| Upload/preparation mean | 1.500 | 1.492 | 1.493 | 1.494 |
| CPU drawing mean | 1.239 | 1.004 | 1.008 | 1.251 |
| CPU drawing p99 | 3.416 | 1.339 | 1.355 | 3.368 |
| Projection wait p99 | 2.294 | 0 | 0 | 2.270 |
| Completion interval p95 | 12.302 | 12.101 | 12.114 | 12.276 |
| Completion interval p99 | 15.043 | 12.828 | 13.111 | 15.153 |

Each resident run records 1,998 resident captures, **zero metadata downloads and
zero downloaded bytes**, plus 10,728 source draws and about 1.339 million
primitives. Reference runs perform 1,998 metadata downloads (511,488 bytes).
The resident projection wait is zero throughout, not merely at a percentile.
Queue p99 remains one and mean input-sample age at drawing stays near 21.4 ms.
This is a repeated 2.0–2.2 ms improvement to the completion tail, but still not
reliable 11.1 ms completion intervals.

Metal repeats the benefit: completion p99 13.975/14.097 → 12.522/12.482 ms;
projection-wait p99 1.803/1.854 → zero. CPU drawing mean rises slightly
(0.551/0.554 → 0.570/0.576 ms), while its p99 falls from 2.201/2.242 to
0.905/0.877 ms. Do not equate eliminating the wait with eliminating all work.

Release Metal and hermetic Deck builds pass. CPU recipe/bounds, compositor,
projection and skybox tests pass. GPU fixtures cover triangle/particle/leaf
projection, asymmetric forward/backward motion at five phases, confidence
rejection, reference/resident transitions and repeated teardown. Committed
SPIR-V, MSL and DXIL outputs pass freshness checks; D3D12 runtime remains
untested. Evidence, scripts, hashes and measurements are under
`runs/gpu-effect-resident-2026-10-02/`; the Deck executable is isolated under
`/home/deck/argame/effects-2026-10-01/ActRaiserRecomp-resident-effects-final`.

Next, broaden recipe/skybox qualification with the same image gates. For further
pacing work, profile the remaining main-thread upload/preparation (about 1.49 ms
mean, 2.1 ms p99 on Deck) and late draw deadlines: several remaining outliers
coincide with 2–3 ms uploads. Separate in-flight endpoint ownership before moving
whole preparation jobs off main; do not reintroduce a readback dependency or
increase buffering to hide it. This path is interpolation-specific, so no new
non-interpolated improvement is claimed here.

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


### Full action-scene GPU migration — 2026-10-02 implementation ledger

The production target is all 49 action rooms and every native/authored effect,
not a Fillmore rollout. The earlier measurements above remain the pinned
prototype baseline; they are not performance or parity claims for this larger
migration. The reference renderer remains an oracle and a recovery path for
unsupported hardware or resource failures.

Implemented and enabled automatically for action Diorama scenes:

- Shared producer/presenter automatic policy for every valid action room.
  `AR_GPU_PIPELINE=reference` selects the reference pipeline; stage overrides
  remain diagnostic controls. Backend initialization and full-frame recovery
  determine availability, independently of room identity.
- Deferred triangles, quads, glows/cloud billboards, stars, particles/leaves,
  embers, lightning ribbons, sword trails and authored floor mist. CPU recipe
  fixtures exercise construction without CPU projection. Actual Metal checks
  cover projection, clipping, OBJ priorities, folds and lighting transport.
- Captured finite skyboxes share raw-band mapping and aspect fitting with their
  attached GPU effects. Periodic Aitos art follows BG1 motion; static ROM art
  uses no generated motion. A source band entering the sampled window is kept
  available on GPU instead of being culled using stale CPU motion.
- Forest scenery shadows and Bloodpool moon transport, water highlights, wet
  timber and illuminated insects consume captured occluders on GPU. No normal
  motion/pixel readback is required by these consumers.
- Actor and spell submission uses the same primitive packet path as decorations.
  All source packets retain bounded allocation and explicit failure reporting.

Validation and retained evidence (2026-10-02):

- All 49 action rooms render through the reference and resident consumers at
  three camera poses, all three plane/skybox modes, and zero/.25/.75 motion:
  **1,323 paired images on Metal and 1,323 on Steam Deck/Vulkan**. The fractional
  cases inject matched forward/backward motion into both consumers; they test
  projection, not motion estimation or a complete gameplay traversal.
- Metal: 1,027 pairs are exact. All but three pairs stay within 2/255 per
  channel; those three Aitos 0403 images each differ above that threshold at
  one particle-edge pixel (maximum 36/255). Vulkan: 964 exact pairs; 29 pairs
  have one or two pixels above 2/255 (maximum 106/255, two torch-edge pixels in
  Marahna 0504). Reviewed the largest outlier as enlarged reference/resident
  crops. No missing room geometry or effect family was observed. The strict
  comparison harness **returns failure for these outliers**; do not describe
  this as bit-exact parity or silently increase its tolerance.
- Final-build Metal gameplay comparisons retain 40 matched frames each for
  moving Fillmore 0101 (interpolation off), Fillmore cave 0102 (phase .25),
  Bloodpool water 0201 (off), and castle 0202 (.25). Exact pairs: 38/40, 40/40,
  6/40 and 38/40 respectively. Only one Bloodpool frame exceeds 2/255: nine
  glint-edge pixels, maximum 34/255; other changed pixels stay within 1/255.
  The stationary Bloodpool recording does not produce accepted reference motion
  vectors, so it is deliberately **not** claimed as interpolated gameplay
  evidence. Earlier moving forest phase-.5 and Aitos lava comparisons remain
  pinned separately; all-room injected motion covers Bloodpool projection.
- The whole-room native/WASM suite passed 147 regional rooms, 882 surface/source
  comparisons, 147 authored round trips and 30 preset reconstructions, including
  reverse time, edit/load atomicity and teardown. This is regional/browser
  contract evidence, not 147 rooms tested on each native GPU.
- Final renderer suite: 38/38 checks, including all deferred recipe fixtures,
  shader bindings, backend boundaries, raw/no-interpolation rendering,
  interpolation, packet lifetime/fades, failed source submission, recovery
  latching and reset. Actual Metal gameplay also verified
  `AR_GPU_PIPELINE=reference` disables the automatic pipeline.

The complete migration exposed repeated Bloodpool recipe construction and
primitive uploads for skybox bands/repaints. The presenter now retains bounded
CPU packets per captured frame, and the GPU retains their primitive buffers.
New captures, in-place edits, toggles and resets invalidate those packets;
projection, motion, shadows and brightness still resolve for each presentation.
Failed construction/allocation/submission requests a complete reference redraw,
never a partially missing effect. This reduced Bloodpool resident CPU draw mean
from 3.61 ms before retention to 2.65 ms in the final run.

Final Deck timings use the isolated directory
`/home/deck/argame/effects-2026-10-01`, the production bounded frame pipeline,
90 Hz cap, 64 extra rows, 16:10 square pixels, skybox-only, effects and CRT on,
no preparation worker, 3,600 ticks, and the settled 1,200–3,300 interval.
Automatic runs remove all GPU stage overrides.

| Scene / path | CPU draw mean | CPU draw p99 | Present-completion interval p99 | Motion metadata waits |
| --- | ---: | ---: | ---: | ---: |
| Fillmore scrolling, reference projection (candidate B, two controls) | 1.21–1.23 ms | 3.38–3.44 ms | 14.82 ms | p99 2.26–2.28 ms |
| Fillmore scrolling, final automatic | 1.00 ms | 1.37 ms | 12.88 ms | zero |
| Bloodpool, final reference projection | 2.28 ms | 3.86 ms | 14.57 ms | present |
| Bloodpool, final automatic | 2.65 ms | 3.71 ms | 13.66 ms | zero |
| Fillmore scrolling, final automatic, interpolation off | 0.84 ms | 1.22 ms | 12.97 ms | zero |

Bloodpool retains a **0.37 ms higher average CPU draw cost** than reference
projection while improving the measured tail and presentation pacing. Do not
hide this tradeoff, sum overlapping producer/presenter timings, call these GPU
timestamps, or claim locked 90 Hz/physical scanout latency. Further work should
measure producer PPU work and per-band GPU submission/lighting cost. The final
Bloodpool process high-water RSS was 204.9 MiB versus 178.2 MiB for reference;
scrolling Fillmore was 184.0 MiB (178.5 MiB with interpolation off). The complete Vulkan room sweep peaked at
69.0 MiB in its standalone harness. These are process measurements, not VRAM
allocation measurements or proof that every driver has the same memory use.

Evidence lives under `runs/gpu-production-2026-10-02/` (ignored local artifacts):

- `room-oracle-final/results.jsonl`, `deck-room-oracle-final.jsonl`,
  `deck-room-oracle-final-run.json`, and `rooms-repro/manifest.json`.
- `metal-d-*-compare/comparison.json`, matched captures and magnified differences;
  `metal-d-reference-policy/` records the escape-hatch run.
- `production-d-pacing.json`, `production-d-nointerp-pacing.json` and
  `deck-final-evidence/` preserve timings, full
  stage configuration, clean-shutdown/readback counters and binary hashes.
- Final Metal binary SHA-256:
  `d241b21ab40fdd6e36b463a845c6f52de1b6fad7e54e8e1b5d4671533d4f8669`.
- Final Deck binary SHA-256:
  `d1b1fcb1699a10c280a2955a3d8d039ce377f5a261dc88bd78d2fa43c9877d67`.

To regenerate the GPU room oracle from a locally built editor (no scene capture
required):

```sh
node tools/action_editor/export_gpu_rooms.mjs build/action-editor/ar-action-layer-editor.html runs/gpu-rooms
cmake --build build-tests-release --target actraiser_action_room_gpu_compare
mkdir -p runs/gpu-room-comparison
build-tests-release/actraiser_action_room_gpu_compare runs/gpu-room-comparison diorama-layers.ini runs/gpu-rooms/*.arscene > runs/gpu-room-comparison/results.jsonl
```

The exporter accepts terrain profile 0/1/2 as its third argument and pins the
editor/input hashes in its manifest. The measured GPU sweeps used profile 0.
The oracle writes image pairs for every pixel-threshold failure; retain those
with its JSONL instead of treating a nonzero exit as an unspecified GPU failure.

Remaining platform/coverage limits are explicit rather than room whitelists:

- Windows/D3D12 shaders compile, but this change has no Windows hardware timing
  or visual qualification. Steam Deck tests ran on the available desktop
  Wayland/Vulkan session, not a separate Gamescope/game-mode qualification.
- CPU game logic, recipe evaluation, and special PPU capture sources still
  exist. Non-virtual/colour-math BG sources and Death Heim's post-scanout statue
  promotion can publish CPU-decoded pixels into the new pipeline. For example,
  Bloodpool legitimately reports available BG mask 3 with GPU-owned mask 1.
  This does not require downloading GPU motion/pixels, but it is not a claim
  that every PPU operation has moved to the GPU.
- Flat and simulation rendering keep their existing owners. All action
  Diorama rooms/effects share the new path; the reference implementation remains
  for unsupported hardware, diagnostics, and exceptional full-frame recovery.
- Image outliers above remain recorded. Gameplay traversal of every room,
  every authored extreme, and every camera/aspect/extension combination is not
  implied by the three-pose room matrix or the recipe fixtures.

### VRAM background packets and authored floor recovery — 2026-10-03

The completed migration was committed first as `f484a0f4`. The next profile
identified ordinary VRAM-backed BG capture as remaining CPU pixel work.
Mode-1 BG1/BG2 with 8x8 tiles now publish scanline bitplanes directly, using
the existing GPU decoder. Repeated 256px pages retain compact tile alignment;
mirrored margins use the existing aligned bitplane cells. Windows, H/V flips,
HDMA scroll, live VRAM/CGRAM changes, brightness and supported capture colour
policies remain scanline state. Unsupported mosaic/16x16 and winner-dependent
captures retain CPU ownership. No shader variant, thread or GPU readback was
added. This supersedes the earlier Bloodpool ownership limitation above:
Bloodpool now reports available mask 3 / owned mask 3.

The first candidate deliberately left mirror/repeat on CPU and still reported
owned mask 1 in Bloodpool; those timings are not evidence of BG2 offload. The
revised candidate exercised ownership 3, passed the tile validation oracle,
and reproduced all 40 committed Bloodpool source images exactly on Metal.

The reported gap below Bloodpool's platform was also reproduced with Dynamic
Cam. The camera correctly framed the authored 528px scenery, including the
extra painted row beneath the native 512px map, but `native_capture_tiles_line`
discarded that row at the native vertical limit. The overbroad guard originated
in `9e807853` (2026-10-01), before this iteration. Explicit authored tiles now
survive native clipping, while the automatic apron lookup remains clipped so
short/repeating backgrounds cannot wrap into the margins. Both CPU and GPU
capture share the correction. Live Dynamic Cam images confirm the gap is
filled; manual orbit/zoom and the camera clamp were not changed.

Validation:

- 10/10 PPU, action-background and Diorama camera/compositor checks pass. New
  CPU-oracle comparisons cover both BG layers, main/sub owners, every fine
  scroll phase, map wrapping, window splits, scanline palette/brightness/tile
  writes, extended rows, partial bounds, both mirror directions, repeat bands,
  and CPU/validation/GPU ownership. Unsupported tile sizes/mosaic stay on CPU.
- A separate authored-margin regression compares the reference pixel renderer,
  native CPU capture and GPU packet output, checking explicit stamps inside
  and outside the ordinary horizontal capture while unedited margins stay
  empty. This would fail with the old clipping guard.
- Steam Deck Bloodpool `tiles-validate` reports zero rejected sources and zero
  mismatching frames at both 300- and 600-frame checkpoints, including the
  corrected floor row. The ordinary candidate uses GPU ownership 3.
- 60 scrolling Fillmore images at interpolation phase .5 are exact against
  the committed build: BG1 moves 760px horizontally/188px vertically, BG2
  380px/63px. Inputs, settings, camera state and final WRAM hashes match.
- For the corrected Bloodpool floor, the full CPU reference versus resident
  GPU comparison retains the prior small effect-rasterization differences:
  39/40 frames have no channel difference above 2/255; one frame has one pixel
  above that threshold (maximum 5/255). The floor is present in both paths.
- The comparison tool now offers explicit Dynamic Cam capture and opt-in
  cross-build comparison. Different executable hashes are rejected by default;
  opting in still requires identical assets, settings, replay, camera state
  and final gameplay memory, and records both executable hashes.

Deck measurements retain the preceding production settings and the original
1,200–3,300 tick interval (see the warmup correction below). Three baseline Bloodpool runs bracket the candidates;
two repeated candidate-C runs include the restored floor. Times below are
CPU scopes and backend-present completion intervals, not GPU timestamps or
physical display scanout. Producer work overlaps presentation.

| Scene / build | Producer mean | Upload mean | CPU draw mean | Completion interval p99 |
| --- | ---: | ---: | ---: | ---: |
| Bloodpool committed baseline, three runs | 11.02–11.09 ms | 2.20–2.21 ms | 2.62–2.65 ms | 13.44–13.53 ms |
| Bloodpool tile offload, before floor fix | 10.14 ms | 2.03 ms | 2.67 ms | 13.80 ms |
| Bloodpool tile offload + restored floor, two runs | 10.27 ms | 2.06–2.08 ms | 2.65–2.66 ms | 13.69–13.71 ms |
| Bloodpool final reviewed build D | 10.08 ms | 2.06 ms | 2.64 ms | 13.57 ms |
| Fillmore scrolling baseline | 9.20 ms | 1.46 ms | 0.99 ms | 12.99 ms |
| Fillmore scrolling candidate | 9.32 ms | 1.48 ms | 0.99 ms | 12.85 ms |

This saves about 0.8 ms (7%) of Bloodpool producer work after restoring the
missing scenery. It is **not a pacing win**: the measured Bloodpool completion
tail is about 0.2–0.3 ms worse, with 821–828 intervals over the 90Hz budget plus
1 ms versus 743–768 in the controls (3,125 presents per run). Fillmore is
essentially unchanged; its BG sources already used tile ownership. All runs
retain zero motion-metadata waits. Keep the CPU headroom evidence distinct
from the unresolved 90Hz pacing target. The next investigation should isolate
presentation deadline scheduling and GPU submission/completion variance,
instead of assuming another producer saving will fix the presentation tail.

Final review kept the aligned-mirror allocation restricted to eligible VRAM
sources, leaving unsupported virtual/mosaic/16x16 allocation unchanged. Build D
passes the same 10 checks and all 40 Dynamic Cam images match candidate C
exactly. Its separate Deck confirmation saves 0.99 ms (9%) of producer work,
with 769 intervals over budget plus 1 ms and completion p99 13.57 ms. This is
closer to the baseline pacing than the two preceding candidate runs, but still
does not establish a presentation improvement. Final pinned SHA-256 hashes:

- Metal: `fefdadf94c9be7e9a5946b92bc368dec3a61d2eda8e035f01a62dd4ebc60895c`.
- Deck: `60f8ac4efc05a04b3d2d65e7d4f45354492f618a4f752bdaceb2a1506882491a`.

Evidence: `runs/gpu-vram-2026-10-03/` contains pinned binaries, image comparisons,
`vram-final-pacing.json`, `vram-d-pacing.json`, and `deck-evidence/` with raw
traces and provenance.
The diagnostic Free Cam images intentionally bypass framing; the separately
labeled Dynamic Cam images establish the actual floor correction. Failed
early harness attempts that demanded resident projection with CPU capture
are retained but are not counted as passing runs. Metal and Deck/Vulkan were
exercised; there is no new D3D12 hardware or Gamescope qualification here.

### Presentation tail and warmup audit — 2026-10-03

The 1,200-tick cutoff excluded level entry (tick 502) and the requested Diorama
activation (tick 900), but did **not** exclude streaming-path startup: the first
nonzero source tick in all three Bloodpool controls and final build D is 1,214.
Calling that entire interval settled was too strong. Re-analysis starts at
tick 1,800, approximately ten seconds after streaming begins, and retains about
25 seconds / 2,248 presentations in the same room and stream epoch. No room
transition or epoch boundary is being excluded within this later interval.

| Bloodpool timing | Original ticks 1,200–3,300 | Later ticks 1,800–3,300 |
| --- | ---: | ---: |
| Baseline completion p99, three runs | 13.44–13.53 ms | 13.43–13.48 ms |
| Final D completion p99 | 13.57 ms | 13.45 ms |
| Final D maximum completion interval | 18.42 ms | 15.46 ms |
| Final D producer work p99 | 11.38 ms | 11.36 ms |

The later D interval still contains 95/2,247 completion intervals above 13 ms;
the three controls contain 67, 110 and 113. Disjoint D windows 1,800–2,399 and
2,400–2,999 retain p99 values of 13.25 and 13.52 ms. Early samples inflate the
maximum and modestly affect p99, but cannot explain the recurring presentation
tail. D's later p99 falls within the baseline spread: these traces establish
neither a pacing improvement nor a meaningful p99 regression from tile offload.

Of D's 95 later intervals above 13 ms, 29 upload an endpoint in the presenting
iteration (median draw deadline lateness 2.98 ms); 66 do not (median 0.89 ms).
This supports inspecting both upload/deadline overlap and varying draw/present
cost. It does not identify the exact cause or establish GPU execution time.
The trace measures host-side backend-present return, not physical scanout.
Periodic pipeline logging and CSV tracing remain enabled, so their contribution
and desktop scheduling/driver noise still need a controlled measurement.

Next pacing comparisons should report startup separately, use a warmup relative
to actual streaming activation, and retain late-window p99/max plus repeated
runs. Prioritize presentation scheduling/submission tails over treating another
producer mean reduction as proof of stable 90Hz. Re-analysis evidence is saved
in `runs/gpu-vram-2026-10-03/tail-window-audit.json` alongside the raw traces.

### Presentation scheduling experiments — 2026-10-03

The follow-up isolates presentation tails with 4,200-tick Deck runs. Analysis
excludes ten seconds after the first actual streamed endpoint and stops at
tick 3,900: about 35 seconds / 3,124 presents per run. The analyzer now supports
`--warmup-seconds`, records actual measured ticks, and exposes both same-epoch
and all-interval statistics so a clock reset cannot silently hide a hitch.
These runs have no epoch boundary in the measured window. Settings, source
clock, effects, and resolution remain those of the preceding Deck comparison.

The main findings are scheduling costs, not loading contamination:

- Disabling pipeline metrics and the auxiliary capture/phase traces did not
  remove the tail: completion p99 was 13.32 ms with diagnostics and 13.64 ms
  with only the pacing trace. This does not measure the cost of the pacing
  trace itself, but rules out the other diagnostics as the main explanation.
- A precise final deadline wait reduced draw-start lateness p99 from about
  2.94 to 0.79 ms, but completion p99 only improved to 13.12 ms. The original
  presenter starts drawing at the deadline, so varying draw cost still shifts
  the final submission even when waking accurately.
- Preparing the draw early, submitting offscreen work, and waiting for the
  output deadline absorbs that variation. Sampling interpolation at the
  intended output time is required. An initial ungated prototype reached
  11.53 ms p99 but lacked the required interpolation endpoint on 559 measured
  frames; it is rejected as a pacing success. A readiness check now waits for
  the correct pair while time remains, with the ordinary bounded hold retained
  if the source is genuinely late at the output deadline.

| Deck case | Completion p99 | Intervals >12.11 ms | Mean animation age at present |
| --- | ---: | ---: | ---: |
| Bloodpool control, phase trace enabled | 13.46 ms | 811 | 32.26 ms |
| Bloodpool early draw, readiness check, existing 1.75-source-frame delay | 13.22 ms | 186 | 29.88 ms |
| Bloodpool early draw, 1.9-source-frame delay, two runs | 11.53 / 11.53 ms | 4 / 4 | 32.26 / 32.24 ms |
| Bloodpool early draw, 2-source-frame delay, two runs | 11.55 / 11.54 ms | 3 / 4 | 33.90 / 33.91 ms |
| Scrolling Fillmore control | 12.83 ms | 152 | 30.51 ms |
| Scrolling Fillmore early draw, existing 1.75-source-frame delay | 11.41 ms | 3 | 29.64 ms |

All readiness-aware cases above have zero targets beyond their available
interpolation endpoint and no backward reconstructed timeline. Animation age
is backend-present completion minus the reconstructed interpolated source time,
not measured input latency or physical scanout. The 1.9-frame policy adds about
2.50 ms to the nominal buffer (29.12 to 31.61 ms), but earlier drawing removes
approximately that much downstream delay in Bloodpool. Two-frame buffering
adds real age without improving p99 here; it is not the preferred candidate.

The largest remaining misses are separate from the broad p99 improvement:

- In the final instrumented Bloodpool run, p99 is 11.56 ms and five intervals
  exceed 12.11 ms. The four largest (14.25–15.72 ms) occur on the first source
  after periodic owner service. Their producer starts are 4.82–6.06 ms late,
  while backend work on those frames is only 0.44–0.66 ms. The current owner
  handoff waits for queued endpoints to drain before running housekeeping and
  restarting the producer. That interruption is the next concrete target;
  another general draw-throughput change does not address it directly.
- A diagnostic run disabling only the periodic timer, preserving event and
  scheduled service, reduced the worst interval from 15.12 to 13.01 ms and
  intervals over 12.11 ms from four to two; p99 remained 11.52 ms. This is an
  isolation experiment, not a proposal to stop servicing saves/settings.
- Earlier runs contain rare backend-scope spikes around 3 ms. New opt-in
  scopes separate offscreen flush, swapchain acquisition, and final blit/submit.
  Those 3 ms spikes did not recur in the instrumented run: their respective
  maxima were 0.21, 0.51 and 1.07 ms. Attribution to Vulkan, compositor or OS
  scheduling is still unproved; do not label these as a confirmed GPU stall.

The prototype remains opt-in: `AR_PRESENT_PRECISE_YIELD=1` and
`AR_PRESENT_PREPARE_LEAD_US=5500`; the lead is capped to half the refresh period.
`AR_FRAME_STREAM_DELAY_PERMILLE=1900` exercises the Bloodpool buffer candidate.
`AR_FRAME_STREAM_SERVICE_MS` defaults to 1,000; zero is diagnostic only. Ordinary
presentation and the default 1.75-frame buffer remain unchanged. No shader,
framebuffer queue expansion, or readback was introduced. The SDL adapter now
exposes offscreen submission without leaking its internal struct to the host.

Validation: six pacing/playout/queue/producer/backend tests pass, including a
60/90 Hz readiness simulation across relative phase alignments and the existing
hidden-swapchain/error protocol checks. Metal builds and scrolling smoke tests
pass; the warmed Metal p99 comparison is 12.45 to 11.19 ms. Final gameplay WRAM
matches between the corresponding controls/candidates: Bloodpool hash
`e0f49116efd6da3e4bada8e7b8e09137cbc8208d08c9dbb004572f3172efd1a7`,
Fillmore hash `55a18e330a5743beb1b2a0c1755c9c7886d95b2325bcc39674951b3104d4c42f`.
These are timing/continuity checks, not a new frame-by-frame visual oracle or
physical scanout qualification. Non-interpolated play, all rooms, D3D12 hardware,
and Gamescope remain to be qualified before making the policy the default.

Evidence is in `runs/pacing-tail-2026-10-03/`: pinned binaries A–F, the runner,
raw Deck traces/logs/provenance, `summarize_tail.py`, `tail-results.json`, and the
two Metal runs. Final instrumented Deck binary F SHA-256:
`c40c723ab08e6ff49b217b49f5741524d2ee1141e6b4141469e905323520015e`.


### Cooperative maintenance and event handoffs — 2026-10-03

The next pacing investigation found and corrected two avoidable interruptions
of the independent producer:

- Routine persistence previously stopped the producer at an arbitrary point
  in its source interval, drained future frame packets at their display times,
  then resumed production. Keeping those owned packets removes the drain delay,
  but alone still allowed 3.4–3.7 ms late restarts. The producer now offers the
  maintenance boundary immediately after publishing a source, giving main the
  idle time before the next source tick to service persistence and resume.
  No source-clock reset, additional buffer, thread, or GPU readback is involved.
- The streaming event loop classified/consumed game-only and ignored events,
  then peeked again to decide whether to pause. The asynchronous gamepad poller
  could post a notification between those two operations. A trace caught an
  ignored `SDL_EVENT_GAMEPAD_UPDATE_COMPLETE` causing a full handoff and a
  6.04 ms late producer restart / 15.29 ms presentation interval. Only an event
  actually classified as requiring host ownership may now request the pause.
  Newly arriving events are classified on the next loop iteration.

Main still waits for the producer acknowledgement before reading the runner
or doing persistence. Warps, settings application, screenshots/dumps, room
changes, errors, and real host events retain the full drained boundary.
Developer automation now reports due warp/diorama actions through the same
host-service predicate as captures, so the persistence-only path cannot skip
those actions. Audio completion remains in each producer tick. Persisting an
actual changed save may still involve disk work; replay timing does not test
save-write latency because replays deliberately protect save data.

The matched Deck runs retain the prior 90 Hz, 1280×800, Vulkan/Wayland desktop,
64-row, effects/CRT, skybox-only settings. Each runs 4,200 ticks, discards ten
seconds after actual streaming starts, and measures through tick 3,900 (about
3,124 presents). Ordinary game state and resources are unchanged.

| Bloodpool interpolated case | Completion p99 | Worst interval | Intervals >12.11 ms | Worst periodic restart lateness |
| --- | ---: | ---: | ---: | ---: |
| Previous instrumented baseline F | 11.56 ms | 15.72 ms | 5 | 6.06 ms |
| Interleaved baseline F repeat | 11.51 ms | 15.12 ms | 3 | 4.62 ms |
| Combined handoff/event fix I, run 1 | 11.56 ms | 22.73 ms | 2 | 0.11 ms |
| Combined handoff/event fix I, run 2 | 11.55 ms | 12.11 ms | 0 | 0.10 ms |

The two fixed runs have zero spurious event handoffs, missing interpolation
endpoints, backward reconstructed source motion, or measured epoch changes.
Animation age remains 32.25 / 32.22 ms versus 32.25 / 32.22 ms in the controls.
The broad p99 improvement belongs to the earlier early-draw experiment; this
change removes identifiable maintenance/input tails, not another throughput
bottleneck. The 22.73 ms outlier must not be discarded: its source was ready,
backend work was small, and the main loop resumed late before drawing. It was
not a periodic-service restart. Build J adds opt-in long-yield logging and
records slow idle event-loop iterations, closing the gap in the earlier trace.
A separate 13.90 ms interval spent about 2.97 ms in swapchain acquisition.
These observations do not establish physical scanout timing or a 90 Hz
worst-case guarantee.

Scrolling Fillmore with I remains at 11.41 ms p99 (previous prototype 11.41),
29.66 ms animation age (29.64), and zero missing endpoints/backward motion.
The two measured intervals above 12.11 ms remain recorded (worst 13.50 ms).

Validation includes an actual persistent producer plus bounded queue regression:
64 maintenance resumptions with three future packets retained, alternating
explicit pause and producer completion, preserve owned pixels, tick order,
source timestamps, and thread ownership. Eight focused queue/producer/playout,
pacing, input/replay and backend checks pass. Metal builds and the boundary
exercise pass: extension changes at game frames 1,300/1,500 and composite
captures at 1,350/1,500/1,650 still execute at the full safe boundary.

The final J diagnostic repeats the restart result (35 periodic services,
maximum 0.10 ms late, zero event handoffs). Its reported p99 is 11.55 ms and
one interval is 13.96 ms. That last outlier exposes a measurement issue: the
pacing CSV sampled completion after writing the phase CSV, about 2.6 ms beyond
the measured draw/present scopes on this frame. Build K timestamps completion
before either trace write, and uses 1 MiB / 4 MiB diagnostic buffers to reduce
file-flush interference. Earlier tables retain their raw results rather than
retroactively subtracting unmeasured logging costs. They should not be read
as exact backend-return or physical-scanout intervals. J also catches one
1 ms idle yield lasting 3.36 ms; the earlier 22.73 ms outlier did not recur,
so its specific cause remains unproved.

With interpolation and the early-draw prototype disabled, the ordinary
90 Hz non-interpolated comparison is effectively unchanged in broad pacing:
p99 13.39 ms control / 13.50 ms fixed, mean displayed-source age 21.59 / 21.84 ms.
The periodic restart maximum improves from 2.52 to 0.09 ms, but the existing
start-drawing-at-deadline policy still dominates this case. Do not claim a
non-interpolated p99 win from maintenance alone. Final WRAM is identical across
the Bloodpool control, both interpolated candidates, and both non-interpolated
runs (`e0f49116efd6da3e4bada8e7b8e09137cbc8208d08c9dbb004572f3172efd1a7`).
The scrolling Fillmore control/candidate also match
(`55a18e330a5743beb1b2a0c1755c9c7886d95b2325bcc39674951b3104d4c42f`).

The handoff/event fixes apply to the ordinary bounded stream. The earlier
precise/early-draw and 1.9-frame timing settings remain diagnostic opt-ins;
this work does not silently promote them to defaults. Gamescope and D3D12
hardware qualification remain outstanding.

Evidence: `runs/pacing-tail-2026-10-03/`, `run_handoff.py`,
`service_tails.py` / `service-results.json`, and the `tail-maintenance-*`,
`tail-cooperative-*`, `tail-eventfix-*` logs/traces. Tested Deck build I SHA-256:
`d106edb4b5af406865680bd5c078b43cb79a6b99515fd7eeb4a71cca22883d26`.
Diagnostic build J SHA-256:
`84e6df112ac7b1faa97496a15786f7d0861833670926ab16990248b7b18515d9`.


With corrected completion timestamps and buffered traces, final build K measures
11.504 ms completion p99 / 13.270 ms maximum, with three intervals >12.11 ms
among 3,123 warmed intervals. All 35 periodic restarts are within 0.113 ms;
there are zero spurious event handoffs, missing interpolation endpoints,
backward timeline steps or measured epoch changes. Mean animation age is
32.227 ms. The largest interval includes a 2.60 ms swapchain acquire, and the
second includes 1.77 ms final blit/submit; these remaining measured tails are
not the fixed maintenance boundary. No >2 ms idle-yield overshoot was logged
in K. The earlier isolated 22.73 ms gap remains an unresolved observation,
not evidence that every tail is eliminated. No extra render work or memory
is added in ordinary playback by the diagnostic buffering.

Final Deck build K SHA-256:
`a7264d269ee68defe3c3251bda7e475cd6866380b77b94650b18cfbd88fcaee7`.
Evidence is `tail-eventfix-timestamp`; its final WRAM matches the Bloodpool
controls. Mac release build also succeeds. The next narrow investigation is
swapchain acquisition/final submission and occasional host wakeup gaps, with
completion sampled before logging. Do not increase source buffering to hide
these downstream tails without measuring its latency cost.

### Prepare the final swapchain blit before the deadline — 2026-10-03

The remaining backend tail can be reduced without another buffered frame.
`AR_PRESENT_PREPARE_SWAPCHAIN=1`, used with the existing early-draw experiment,
submits the finished offscreen draw, acquires the window image and records its
blit before waiting for the same scheduled output deadline. Only the final
command-buffer submission remains at that deadline. It also removes an empty
SDL renderer submission that the previous early-flush path made after waiting.
There is no new worker, GPU readback, source delay, or rendering change.

The window owner acquires and submits the command buffer on the same thread.
Preparation is idempotent; no drawing or output-setting changes may occur until
Present consumes it. A successful hidden-window acquire with no image still
submits the cleanup buffer. Acquisition failure cancels the unacquired buffer;
submission failure consumes it. Teardown retires any prepared buffer before
releasing its source/window. These paths use the common SDL GPU API rather than
Vulkan-specific calls. The existing completion trace now distinguishes work
performed before the deadline from the remaining final Present call; backend
flush/acquire/blit scopes must not be added to that final-call duration.

Eight matched Deck runs use one pinned binary L and the established 1280×800,
Vulkan/Wayland desktop, 90 Hz cap, 64-row, effects/CRT and skybox-only workload.
All enable precise/early drawing with a 5,500 µs lead and use a 1.9-period buffer
when interpolating. Only early swapchain preparation changes within each pair.
Bloodpool runs in off/on/on/off order; scrolling Fillmore runs off/on, and
Bloodpool without interpolation runs on/off. Each excludes ten seconds after
streaming begins and measures through tick 3,900, about 3,123–3,124 presents.

| Case | Completion p99 | Worst interval | Intervals >12.11 ms | Mean displayed-source age |
| --- | ---: | ---: | ---: | ---: |
| Bloodpool, acquire at deadline, run 1 | 11.551 ms | 13.673 ms | 2 | 32.265 ms |
| Bloodpool, acquire early, run 1 | 11.434 ms | 11.952 ms | 0 | 31.937 ms |
| Bloodpool, acquire early, run 2 | 11.432 ms | 12.557 ms | 3 | 31.930 ms |
| Bloodpool, acquire at deadline, run 2 | 11.537 ms | 14.228 ms | 4 | 32.239 ms |
| Scrolling Fillmore, acquire at deadline | 11.479 ms | 13.849 ms | 1 | 32.171 ms |
| Scrolling Fillmore, acquire early | 11.449 ms | 12.693 ms | 1 | 31.958 ms |
| Bloodpool, interpolation off, acquire at deadline | 11.537 ms | 13.223 ms | 1 | 24.512 ms |
| Bloodpool, interpolation off, acquire early | 11.366 ms | 12.840 ms | 1 | 24.272 ms |

Bloodpool's final Present call averages 0.610–0.638 ms in the controls and
0.298–0.308 ms with preparation. Producer work stays around 10.0–10.1 ms and
upload/preparation around 2.16–2.18 ms; this is scheduling improvement, not a
claim that those costs disappeared. The three >12.11 ms intervals in the
second candidate coincide with late source preparation (3.17/4.29 ms uploads,
or a 12.06 ms producer tick), while final Present remains only 0.20–0.28 ms.
Submission variability remains in other frames. A sampling profile follows to
identify the next preparation target instead of increasing buffering again.

Every matched run has zero backwards source-time steps and measured epoch
changes; interpolated runs have zero missing endpoints. All six Bloodpool
WRAM hashes match the existing `e0f49116…efd1a7` control, and both scrolling
Fillmore hashes match `55a18e33…c42f`. CPU time/RSS show no material increase in
the repeated Bloodpool comparison. Eight focused pipeline/backend/input tests
pass, including delayed prepare/present, hidden windows and failure cleanup.
The pacing analyzer also preserves backend-stage evidence for individual worst
intervals and tests that early acquire work is not double counted. Metal builds
and the scrolling geometry-change/capture exercise pass; the captured final
composition was inspected. D3D12 and Gamescope runtime tests remain outstanding.

Keep the switch opt-in with the other early-draw settings for now. These are
CPU/backend-return intervals, not physical scanout or input-to-photon latency.
The rare tails and limited run lengths do not establish a worst-case 90 Hz
guarantee; the scrolling p99 difference in particular is small.

Evidence: `runs/pacing-tail-2026-10-03/run_swapchain.py`, the eight
`tail-swapchain-*` control/early directories and their `late-analysis.json`.
Deck binary L SHA-256:
`49607bcb53968118a9b692d5bdbe59f8c6382723b4b404c8786abebcccc55ec0`.
Metal binary L SHA-256:
`1ae0acf8005f5c28c9c48a2943ad9a0378ee0d3b06ab43899c3e46ef591e11e7`.

The next profile identified useful steady-work reduction independently of
pacing: `ResolveNative` accounted for 3.84% of all sampled user cycles, about
11.8% of the main thread's samples. Instruction annotation attributes over
98% of that function's samples to the scalar packet-copy loop, including its
per-word reload of the packet size. Little-endian hosts now `memcpy` the
non-aliasing packet into the mapped transfer allocation; big-endian hosts
retain explicit conversion with a cached word count. Packet bytes, resource
ownership, GPU commands and shaders are unchanged.

An interleaved L/M/M/L Deck comparison with early swapchain preparation enabled
reduces source preparation/upload mean from 2.186–2.188 ms to 1.960–1.963 ms
(about 10%). Its p99 falls from 2.573–2.641 ms to 2.335–2.341 ms. Completion
p99 is effectively unchanged: 11.442/11.447 ms in controls, 11.426/11.438 ms
with the copy fix. Do not count this as another established pacing win.
Final Bloodpool WRAM matches in both candidates. Metal's ordinary/native GPU
packet fixtures pass, and 20 deterministic scrolling composites at phase 0.5
are pixel-exact between L and M, with identical generated-plane masks and
camera ranges. Both release builds succeed. The first short image run was
rejected because it ended before all requested game-frame captures and the
ownership report; the completed 2,000-tick repeat supplies the comparison.

One M run records a 20.525 ms interval. There is a 14.633 ms gap between the
previous completed present and the next loop entry; its source was already
ready, and drawing/submission do not account for that gap. The previous pacing
CSV row crosses a 4 KiB flush boundary. A direct probe on the Deck establishes
that `setvbuf(file, NULL, _IOFBF, 4194304)` actually creates a **4,096-byte**
buffer in its C library. The 1 MiB request likewise becomes 4 KiB. Supplying
owned storage yields the full requested sizes. Thus the previous "buffered"
K/L/M captures still permitted frequent diagnostic writes; the corrected
completion timestamp is valid, but logging can disturb the following frame.
This is a strong specific observer-effect lead, not proof that every long gap
was a disk write or that OS scheduling noise is absent.

Build N owns the diagnostic buffers through `fclose`, disables a trace with a
clear diagnostic if allocation/setup fails, and reports trace-write wall
scopes exceeding 1 ms. Ordinary untraced playback allocates none of this
storage. Longer traces still flush when their real buffers fill. A longer
hardware capture follows with these corrections before assigning isolated
maxima to renderer work or system noise. Earlier raw measurements remain
preserved; they are not silently corrected or filtered.

Evidence: `tail-swapchain-profile` (sampling only, not a timing control),
`tail-packet-*`, `metal-packet-parity-r2-*`, and `metal-packet-parity-r2-report`.
Deck M SHA-256:
`a57b5a37875b49240430f89fd1e058025d2ea815a69bfb85a7b863d86ab55fe1`.
Deck N SHA-256:
`27396ec329bf72e7db96b6486c1a2e92b2a9035b7d799c8bb36b0a0a493dda7a`.

**Corrected longer capture and focus table.** N completes 9,000 ticks / 150.45
seconds. After ten seconds of actual-stream warmup, ticks 1,815–8,700 contain
10,312 presents (about 114.6 seconds). The pacing and phase files are 3,622,773
and 916,136 bytes, below their real 4 MiB/1 MiB buffers. There are no logged
>1 ms trace-write scopes. Completion mean is 11.111 ms, median 11.121 ms,
p95 11.297 ms and p99 11.408 ms: p99 is 0.287 ms / 2.58% above the median.
Ten intervals exceed 12.11 ms; the worst is 13.740 ms. The 20+ ms gap does not
recur, but that alone does not prove its earlier cause. There are zero missing
interpolation endpoints, backwards timeline steps or measured epoch changes.
Mean displayed-source age is 31.940 ms.

| CPU wall scope | Mean | Median | p99 | Focus |
| --- | ---: | ---: | ---: | --- |
| Completed-frame cadence | 11.111 ms | 11.121 ms | 11.408 ms | Outcome, not active rendering cost |
| Game/PPU producer | 10.063 ms | 10.001 ms | 11.086 ms | Overlaps main; roughly 60 Hz source budget |
| Main source preparation/upload | 1.953 ms | 1.951 ms | 2.297 ms | Keep bulk-copy win; inspect remaining copies/encoding |
| Main drawing/command preparation | 2.597 ms | 2.594 ms | 3.648 ms | Highest remaining active-work target |
| Early swapchain acquisition | 0.227 ms | 0.221 ms | 0.313 ms | Usually hidden by lead; one 3.162 ms tail remains |
| Final Present call | 0.312 ms | 0.293 ms | 0.636 ms | Driver/submission variability still visible |
| Event processing and owner polling | 0.020 ms | 0.017 ms | 0.054 ms | Low priority after handoff fixes |

Rows overlap or run at different rates and must not be summed as one frame.
These are still CPU wall scopes, not GPU execution timestamps or physical
scanout. The table uses the early-presentation prototype, not ordinary pacing
settings. The bigger outliers need finer attribution: a logged 0.200 ms idle
yield lasts 2.638 ms near tick 7,046; four tails contain 4.87–6.44 ms draw scopes;
one includes a 4.63 ms upload, another a 3.16 ms acquire, and another a 1.54 ms
final Present. The largest interval at tick 8,112 includes roughly 2.57 ms not
accounted for by measured backend work and scheduled wait. Do not mislabel
that residual as GPU execution or as a proven timer oversleep. Thread CPU-time
and scheduling/wait attribution are the next diagnostic step for these tails.
The steady-work priority is effect/draw-command construction followed by the
remaining source-preparation costs; event polling and small worker wake costs
are no longer the promising targets. Evidence: `tail-owned-trace-long` and
its `long-analysis.json`.

### Producer breakdown on Deck (2026-10-03)

The broad producer number hid a larger CPU scanout cost than the earlier
whole-process cycle samples suggested. Prioritize this source-side work along
with presentation construction; it is not primarily recompiled game logic or
scheduler noise. Source frames still have a 60.0988 Hz deadline, but completing
them earlier gives 90 Hz presentation more preparation headroom.

Measured with the same Bloodpool 0201 workload, 64-pixel vertical extension,
624×352 background packet, resident GPU ownership, and the early-presentation
prototype (5.5 ms preparation lead, 1900 permille stream delay, precise yield,
early swapchain acquisition). Each 4,200-tick run measures ticks 1,815–3,900
(2,086 source frames) after excluding ten seconds from stream startup. Fillmore
0101 also uses the scrolling replay. Per-tick monotonic wall scopes and Linux
thread CPU time are recorded independently on the producer, with an owned
8 MiB CSV buffer; complete trace files fit without mid-run buffer flushes.
These are CPU timings, not GPU execution timestamps.

Bloodpool broad scopes, milliseconds per source frame:

| Stage | Mean | Median | p99 |
| --- | ---: | ---: | ---: |
| Entire producer | 10.098 | 10.035 | 11.273 |
| PPU scanout | 6.962 | 6.886 | 8.106 |
| PPU policy/setup/finish, excluding scanout | 0.122 | — | — |
| APU timeline advancement (nested in emulation) | 1.177 | 1.160 | 1.623 |
| Recompiled game execution (nested in emulation) | 0.214 | 0.206 | 0.701 |
| Emulation total | 1.397 | 1.380 | 1.858 |
| Frame snapshot/effects | 0.714 | 0.702 | 0.922 |
| Owned packet/surface copying | 0.839 | 0.819 | 1.107 |
| SIM capture | 0.055 | 0.052 | 0.096 |
| Audio post-tick service | 0.002 | 0.001 | 0.004 |

Nested rows are not additive; neither are marginal percentiles. Effects account
for 0.613 ms of snapshot capture: actor observation 0.050 ms, environmental
observation 0.203 ms, and recipe application 0.360 ms. Background packet copying
is 0.339 ms of the 0.839 ms ownership copy; remaining surfaces/housekeeping cost
0.500 ms. Total owned payload is 4.730 MiB per Bloodpool source frame.

Thread CPU time averages 9.958 ms versus 10.098 ms wall time. The remaining
0.140 ms includes off-CPU waits/descheduling and measurement boundary skew; it
is not enough to explain the steady cost. In the slowest 1% of source frames,
mean total rises to 11.551 ms, thread CPU to 10.972 ms, and the wall-minus-CPU
residual to 0.579 ms. PPU work accounts for 1.190 ms of that cohort's 1.453 ms
wall-time increase. There is both execution variation and off-CPU noise.

Fine scanout scopes (nested in scanout above; means in milliseconds):

| Scanout operation | Bloodpool 0201 | Scrolling Fillmore 0101 |
| --- | ---: | ---: |
| Authored tile/apron lookup and packet capture | 1.613 | 1.548 |
| BG1 native/world tile resolution and packet construction | 0.394 | 0.458 |
| BG2 native/world tile resolution and packet construction | 0.881 | 0.465 |
| BG3/HUD tile resolution | 0.266 | 0.259 |
| Sprite evaluation, margins and range capture | 0.215 | 0.224 |
| Sprite pixel export/composition | 1.115 | 1.089 |
| Overlay row clearing/cache reset/packet row initialization | 0.707 | 0.551 |
| Final RGB output/color math | 0.680 | 0.660 |
| BG export/composition | 0.191 | 0.185 |
| Capture-line setup | 0.131 | 0.125 |
| Winner masks | 0.015 | 0.014 |
| Remaining scanout, including probe overhead | 1.045 | 1.012 |
| Full scanout with these probes | 7.253 | 6.590 |

Every capture-line stage runs 352 times per source frame, including the extra
vertical rows. GPU-owned BG sources avoid CPU pixel decoding, but the CPU still
walks map/edit providers and builds their scanline programs. That distinction
matters: “GPU-owned” does not mean all background preparation is off the CPU.
The existing sampled profile independently identifies
`native_capture_tiles_line`, tile providers and packet construction as hot work.

A second diagnostic layout split authored capture by layer: BG1 takes 1.619 ms,
BG2 just 0.014 ms. It also measures HDMA at 0.014 ms and confirms there are no
scanline observer callbacks or separate background-view calls in this Bloodpool
run. The post-raster scanline block accounts for 0.747 ms of the earlier residual.
Its sprite-export scope measures 0.854 ms instead of 1.115 ms: instrumentation
changes code generation/layout as well as reading clocks, so treat fine-stage
figures as a targeting range rather than exact promised savings.

A final, shorter Q run (3,000 total ticks, warmed ticks 1,815–2,700;
886 samples) splits the post-raster block: padding 0.016 ms, authentic-surface
check 0.014 ms, **Mode 7 overlay cleanup 0.759 ms**. This is a concrete stale-dirty
flag bug, not useful Mode 7 rendering in Bloodpool:

- `HdReplacementHost_BindSurfaces`/`RebindSurfaces` binds a Mode 7 surface with
  height `224 * scale`; binding sets `m7OverlayMaybeDirty`.
- `render_mode7_override_line` returns when `output_row * scale` reaches that
  surface's height. It resets the dirty flag only on
  `screen_y == 223 + extraBottomCur`, after that bounds return.
- With 64 top and bottom rows, the intended reset is output row 351, outside
  the 224-row overlay. It never runs. The inactive high-resolution buffer is
  therefore cleared on every frame instead of once after invalidation.

Fix retirement at the end of the actually covered output surface, preserving
proper invalidation on bind/reset, active Mode 7 drawing, geometry changes, and
scene transitions. This should precede larger refactoring. The ~0.75 ms is the
measured cleanup scope, not yet a validated net saving. No fix was applied as
part of this measurement pass.

Controls and reproducibility:

- Uninstrumented binary N: mean producer 10.012 ms, p99 11.033 ms. Broad-scope
  binary O: 10.098 ms, p99 11.273 ms. Fine O: 10.394 ms, p99 11.567 ms. Deep P:
  10.256 ms, p99 11.296 ms. Separate runs cannot distinguish all code-generation,
  timer, thermal, and scheduling effects; no timing overhead has been subtracted
  from individual scopes.
- Scrolling Fillmore fine O: mean producer 9.407 ms, p99 10.509 ms, thread CPU
  9.305 ms. The leading scanout work persists during scrolling.
- All four full Bloodpool runs preserve WRAM SHA256
  `e0f49116efd6da3e4bada8e7b8e09137cbc8208d08c9dbb004572f3172efd1a7`.
  Fillmore preserves `55a18e330a5743beb1b2a0c1755c9c7886d95b2325bcc39674951b3104d4c42f`.
  GPU ownership remains 3 for Bloodpool and 7 for Fillmore, with no rejected
  sources/fatal errors or resident-effect metadata downloads. These ownership
  logs are not a new pixel-parity comparison.
- Evidence, scripts, source patches and original source hashes are under
  `runs/producer-breakdown-2026-10-03/`. Each `instrument*.py` restores every
  touched source byte in `finally`; diagnostic binaries are explicitly named
  `ActRaiserRecomp-producer-*`. No timer probes or Linux-only timing dependencies
  were left in production source. All measured windows and stage percentiles
  are retained in each run's `producer-analysis.json`; use `run_producer.py`
  with `--producer-trace` and optionally `--fine` for diagnostic binaries only.

Next optimization order:

1. Fix the inactive Mode 7 surface dirty-flag retirement described above and
   test extended-height/short-surface transitions.
2. Reduce repeated BG1 edit/apron/provider walks. Cache tile-level edit and band
   decisions by map/edit generation, or publish compact resident tile/map data
   and per-row state for GPU consumption. Preserve HDMA scroll/palette changes,
   VRAM animation and provider fallback semantics; do not simply cache a whole
   rendered scanline across frames.
3. Specialize sprite export for empty rows/spans and common capture policies;
   eventually evaluate a resident sprite command path. Sprite evaluation itself
   is small compared with scanning/exporting/compositing its destination pixels.
4. Remove unnecessary clear/RGB work where ownership proves there is no CPU
   consumer, and reduce the remaining packet/surface copy. Keep CPU HUD, native
   presentation, comparison, capture, and unusual PPU policy fallbacks correct.
5. Revisit effect-scene/occlusion snapshot invalidation, then APU scheduling.
   APU timeline advancement is worthwhile (~1.18 ms), but moving it off-owner
   requires preserving deterministic SPC-port synchronization. More gameplay
   HLE is low priority with game execution averaging only ~0.21 ms.

These are measured cost targets, not projected recoverable time. Validate each
change with scrolling pixel parity and the same producer/presentation traces.

### 2026-10-03 — Portable producer optimizations, pass R

Implemented the first three producer targets above. Runtime changes are based
only on PPU state, surface bounds and explicit capture contracts:

- Retire an inactive Mode 7 surface's dirty flag at the bound surface end,
  including a partial scaled row. Extended scanout can continue past that end.
  The previous final-scanline-only check repeatedly cleared an already-clean
  high-resolution surface. Rebinding, reset and active artwork retain their
  invalidation behavior.
- Add an optional, versioned `SR_RUNNER_CAP_PPU_CAPTURE_TILE_CACHE` contract.
  Hosts select stable authored lookup layers in the appended request field;
  the runtime caches hits and misses by full tile coordinates for one scanout
  frame. Cache storage is bounded, and frame start/rebinding invalidate it.
  Legacy request prefixes remain live. VRAM, palette, scroll and virtual-world
  callbacks remain live even when authored metadata is cached. ActRaiser's
  adapter opts in for its immutable published edits; no room IDs, game memory
  addresses or environment rules enter `snesrecomp-go`.
- Track occupied sprite spans, including synthetic horizontal margins. For
  whole-OAM extraction with uniform visibility, export only occupied pixels
  and merge the remaining packed source spans separately. Partial OAM ranges,
  variable windows and winner-dependent captures keep the general path.

Deck results below use normal binaries, not the instrumented build. Same
4200-tick workload, 64 extended rows, resident background/effect rendering,
90 Hz presenter, early swapchain preparation, 5.5 ms lead and 1.9-frame delay.
The first ten seconds of actual streamed output are excluded; the measurement
ends at tick 3900 (2085–2086 producer samples per run). Bloodpool was repeated
in A/B/B/A order. Times are milliseconds of **producer work**, not GPU time
or end-to-end input latency; producer and presenter overlap.

| Workload | Before mean | After mean | Before p99 | After p99 |
|---|---:|---:|---:|---:|
| Bloodpool, interpolation, pair 1 | 10.066 | 8.130 | 11.098 | 9.173 |
| Bloodpool, interpolation, pair 2 | 10.028 | 8.100 | 10.902 | 9.136 |
| Scrolling Fillmore, interpolation | 9.267 | 7.365 | 10.553 | 8.332 |
| Bloodpool, interpolation disabled | 10.020 | 8.127 | 11.058 | 9.305 |

This recovers about 1.9 ms (19–21%) of producer work in these workloads.
Bloodpool presentation interval p99 remains roughly 11.41 ms in both builds;
this gives the producer headroom, rather than proving every presentation tail
is fixed. Its deadline-lateness p99 improved from 2.472 to 1.107 ms in pair 1.
The interpolation-disabled run still uses the 90 Hz presenter to repeat native
endpoints; it is not a claim of 90 Hz game simulation.

A separate fine-scope build attributes the changes. The earlier fine O and new
fine R measurements include probe/code-generation overhead, so their deltas
must not replace the uninstrumented totals above:

| PPU scope | Fine O mean | Fine R mean |
|---|---:|---:|
| Total scanout | 7.253 | 5.148 |
| Authored tile/apron capture | 1.613 | 1.389 |
| OBJ resolve | 0.215 | 0.213 |
| OBJ export/composition | 1.115 | 0.024 |
| Row clear/setup | 0.838 | 0.825 |
| RGB output | 0.680 | 0.653 |
| Remaining scanout scopes, including Mode 7 cleanup | 1.045 | 0.313 |

Validation:

- Generic PPU oracle tests, ABI/C/C++ header tests and Mode 7 tests pass.
  The new Mode 7 test failed before the fix. Cached/uncached authored capture
  agrees with live scroll, brightness, palette and VRAM changes, masks,
  mosaic, mirror policies and frame invalidation. Sprite tests cover empty
  rows, margins, movement, visibility, partial rectangles and OAM ranges.
- ASan/UBSan capture/ABI/Mode 7 checks pass. The PPU suite also passes with
  32-bit mask words. macOS ARM64 and Linux x86-64 builds succeed; no new
  platform-specific intrinsics or timing dependencies ship in the runtime.
- Forty controlled Metal frames match byte-for-byte: twenty scrolling Fillmore
  composites at interpolation phase 0.5, and twenty stationary Bloodpool source
  composites. Fillmore's BG1 moves 546 pixels horizontally and 188 vertically.
  GPU background parity tests also pass. These are sampled comparisons, not
  exhaustive visual coverage of every platform/room.
- Dynamic Cam initially produced small differences even between repeated runs
  of the same executable: its presentation damping uses wall time. Fixed
  presentation-camera orientation removes that variable while game/level
  scrolling continues. Those dynamic runs are retained as diagnostics, not
  counted as exact parity. A Bloodpool walking capture fell into a pit and
  failed room/ownership validation; the valid Bloodpool comparison is stationary.
- The shared WASM renderer builds; whole-room native/WASM tests pass across
  147 regional rooms, 882 surface/source comparisons, 147 authored round trips,
  30 preset reconstructions and 28263 draws. Host queue, producer, playout,
  input, pacing and PPU integration tests pass.
- All matching Deck replay pairs preserve final WRAM hashes: Bloodpool
  `e0f49116efd6da3e4bada8e7b8e09137cbc8208d08c9dbb004572f3172efd1a7`,
  Fillmore `55a18e330a5743beb1b2a0c1755c9c7886d95b2325bcc39674951b3104d4c42f`.
  No fatal sessions, rejected background sources or effect metadata downloads.

Evidence is in `runs/producer-opt-2026-10-03/`: pinned before/after executables,
capture manifests, comparison reports, A/B traces, state hashes and a separate
instrumentation script/patch. Production Deck R SHA256 is
`818ea31aa0f8cc0551017ab1a718d5b2bc864f1e502f8824e3136feae3fcada1`.
Every instrumented source was restored byte-for-byte after the diagnostic
build; probes are not left in shipping code.

Remaining producer priorities: authored tile/apron packet generation (1.389 ms),
BG2 resolution (0.872 ms), packet/surface copying (0.849 ms), row clears
(0.691 ms), RGB conversion (0.653 ms), and effect snapshot capture (0.730 ms).
Further clear/RGB/copy elision needs explicit CPU-consumer and buffer-lifetime
proof; those ownership changes are still pending. APU timeline work remains
1.182 ms, with deterministic port synchronization required before threading it.
Sprite export is no longer a leading target. Presenter tail work remains a
separate investigation from these producer savings.
### 2026-10-03 — Packet ownership and tile capture optimizations, pass U

The next producer pass removes the per-frame background packet copy. Scanout
writes into its reserved queue slot; the slot remains immutable until released
by the presenter. The ordinary copy path remains available for external packet
storage. When the producer yields ownership, it copies its last publication
into the synchronous snapshot once, so paused redraws and diagnostics can
recapture safely without another scanout or a pointer into recycled storage.
Queue allocation remains lazy and bounded; no new thread, GPU readback, packet
format, shader or graphics-backend dependency is introduced.

Portable `snesrecomp-go` changes:

- Uniform-visibility authored tiles are emitted as a whole run without walking
  individual pixels to rediscover that there is no window split.
- Complete authored replacements clear their three destination bands together
  and write only the selected band. Partial masks and skybox cells retain the
  general writer, preserving black, transparent and backing semantics.
- Main/authentic RGB rows clear only guard columns that scanout will not
  overwrite. Forced blank still clears the whole row.
- A new dirty-buffer/colour-math test exposed a pre-existing native/reference
  mismatch: clipping the main colour could skip an actor's colour transform
  even though subsequent colour maths produced visible output. The native
  path now applies the transform after maths, matching the reference sampler.

A uniform-RGB-span experiment did not produce a useful measured saving and was
removed. The implementation retains the ordinary per-pixel RGB path, avoiding
an extra uniformity scan on dense scenes. Runtime decisions remain based on
PPU state and capture contracts; game-specific orchestration stays in the host.

Normal Deck binaries, with the same pass-R workload and settings: 4200 ticks,
64 extended rows, 90 Hz presentation, resident rendering, 5.5 ms prepare lead
and 1.9-frame playout delay. Exclude ten seconds of streamed warmup and stop
measurement at tick 3900 (2085–2086 source samples). These are producer work
costs, not input latency or total serial CPU+GPU time.

| Workload | R mean | U mean | R p99 | U p99 |
|---|---:|---:|---:|---:|
| Bloodpool, interpolation, pair 1 | 8.085 | 7.724 | 9.140 | 8.906 |
| Bloodpool, interpolation, pair 2 | 8.145 | 7.698 | 9.247 | 8.837 |
| Scrolling Fillmore, interpolation | 7.411 | 7.064 | 8.454 | 8.066 |
| Bloodpool, interpolation disabled | 8.110 | 7.673 | 9.590 | 8.721 |

This saves another 0.35–0.45 ms of average producer work, approximately 5%.
Presentation interval p99 remains 11.3–11.4 ms; more producer headroom is not
proof that every 90 Hz presentation tail is solved. The interpolation-disabled
case still presents repeated native 60 Hz endpoints on the 90 Hz schedule.

Validation:

- Forty controlled Metal composites match pass R byte-for-byte: twenty
  scrolling Fillmore phase-0.5 frames (BG1 traverses 546×188 pixels), and twenty
  Bloodpool source frames. Three additional streamed Bloodpool captures across
  vertical-view changes 64→32→64 also match exactly, with identical final WRAM.
- That transition test caught and corrected an intermediate ownership bug:
  clearing the live publication after each capture removed GPU-owned scenery
  from later host recaptures. U preserves the publication when yielding, before
  borrowed queue storage can be freed. Intermediate S/T binaries are diagnostic
  evidence only and must not be distributed.
- Queue tests cover direct capture, ordinary copied packets, slot reuse and
  retaining a frame while production resumes. Native/reference PPU tests cover
  dirty guards, blank/resume, colour-window/maths combinations, per-actor tints,
  authored tile masks, provider/raster changes and fallback paths. ASan/UBSan
  focused checks, C/C++ API checks, 32-bit-mask PPU tests, host queue/producer/
  input/presentation tests and GPU background parity checks pass.
- Shared WASM rendering builds and matches all 147 regional rooms: 882 surface/
  source comparisons, 147 authored round trips, 30 presets and 28263 draws.
  macOS ARM64/Metal and Linux x86-64/Vulkan are exercised. Windows/D3D12 is not
  run here; these changes introduce no backend-specific code or intrinsics.

A separate diagnostic build attributes the improvement; probe overhead means
these figures should not replace the normal-binary totals above:

| Producer scope | Fine R mean (ms) | Fine U mean (ms) |
|---|---:|---:|
| Packet/surface copies | 0.849 | 0.494 |
| Background packet copy per frame | 0.343 | 0.000 |
| PPU scanout | 5.148 | 5.072 |
| Authored tile/apron capture | 1.389 | 1.285 |

Copied bytes fall from 4.730 to 2.766 MiB per source frame.
The snapshot copy at an ownership handoff (normally about once a second for
maintenance) is outside the per-frame copy scope. Presentation traces include
those handoffs; no claim is made that all copying has disappeared.

All matching Deck replay pairs preserve WRAM hashes: Bloodpool
`e0f49116efd6da3e4bada8e7b8e09137cbc8208d08c9dbb004572f3172efd1a7`, Fillmore
`55a18e330a5743beb1b2a0c1755c9c7886d95b2325bcc39674951b3104d4c42f`. No rejected background sources, fatal sessions, or resident
effect metadata downloads occurred. Remaining measured work includes roughly
1.29 ms authored tile capture, 1.19 ms APU advancement, 0.73 ms frame/effect
capture and 0.49 ms CPU surface copies. Those need separate changes and
validation; game-specific APU shortcuts have not been introduced.

Evidence is under `runs/producer-opt2-2026-10-03/`, including pinned binaries,
before-source snapshots, normal traces, capture comparisons, transition
regression evidence and `profile-u/`'s reproducible diagnostic patch. Every
instrumented production source was restored byte-for-byte. The final Deck U
binary SHA256 is
`3e008df0ff53824074027e9b9a9059a47814ec83d2d6c97a55eb17a73a3e14f0`.
Changes are left uncommitted for review.

### 2026-10-03 — D3D12 rendering audit after producer commit

The pending producer, pacing and validation work above was committed as
`084af97f` (`Optimize PPU frame production and stabilize streamed presentation
pacing`). This audit reviews that commit; it does not change production code
or claim native Windows performance acceptance.

Scope: the ordered SDL_GPU/SDL_Renderer adapter, action background decoding,
GPU motion and endpoint handoff, resident effect and skybox projection,
CPU-produced plane uploads, SIM retained geometry/material uploads, shader
generation, resource retirement and presentation. SDK references below are
Microsoft's D3D12/DXGI documentation. Library behavior was checked against SDL
release **3.4.12 and 3.4.16** source, not inferred from the SDL API names.
The local Metal test installation reports 3.4.12. The two versions have identical
`SDL_render_gpu.c`; the D3D12 backend has a few intervening fixes. Packaging
resolves an SDL version at bundle creation, so a Windows test must record the
actual DLL/SDK lock rather than assuming it matches the Mac.

#### Findings and proposed order

| Order | Finding | Evidence and consequence | Next change / acceptance |
|---|---|---|---|
| 1 | **P2: CPU reads from mapped upload memory** | `action_effect_source_sdl.c:314–339` copies primitives into upload storage, then multiplies their mapped colors in place. It also accumulates occluder bounds in the mapped buffer. Brightness changes and shadowed batches can therefore read write-combined memory. | Compute each adjusted primitive and bounds in ordinary CPU memory/registers, then write once to staging. Check fade/color and shadow parity. Do not add another GPU wait. |
| 2 | **P2: the old per-update allocation/repacking path remains for CPU planes** | `diorama_upload.c:53` → `presentation_upload_mirror.c:195` → `render_sdl.c:160` still reaches `SDL_UpdateTexture` for changed non-GPU-owned planes, including actors. SDL creates/releases an upload buffer per call. Its D3D12 implementation uses a committed resource, and on adapters lacking unrestricted buffer/texture copy pitch it creates another upload buffer when a tightly packed dirty width is not 256-byte aligned. | Add a reusable, cycled staging arena for these uploads in the ordered adapter; align rows/offsets, batch copies and preserve SDL/native queue ordering. Keep dirty detection and unchanged-texture skipping. Compare native and interpolated output, partial dirty rectangles, resize and repeated in-flight uploads. |
| 3 | **P2: submission boundaries scale with effect batches** | Each resident effect/skybox batch calls `SubmitPending`, acquires its own command buffer, records work and submits. `SubmitPending` unconditionally calls offscreen `SDL_RenderPresent`, which also submits even when no SDL commands were added. SDL D3D12 closes/executes a command list and signals a fence per submission; descriptor pools and pass state also have to be rebound. | First count submissions/empty submissions and their CPU costs by scene. Then record adjacent native batches into a shared command buffer and submit at actual SDL/native dependency boundaries. Preserve transparent draw order and all required compute dependencies. |
| 4 | **P2: new action PSOs are created during first capture/draw** | `EnsureGpu`, `EnsureSourceProjection`, `ArGpuActionScenePass_Init` and background `EnsureNative` lazily create motion, effect, projection and decode pipelines. `RenderPreparation_Prepare` warms older postprocessing/SIM paths but does not prepare this newer action pipeline. | Prepare pipelines before interactive action rendering and preallocate known bounded resources. Treat room-entry/resize costs separately from steady-state p99. Cached DXIL removes frontend shader compilation, not driver PSO creation. |
| 5 | **P2: offscreen SDL submission failure is not propagated** | `render_sdl.c:595–596` checks flush/present, but SDL 3.4.12/3.4.16 `GPU_RenderPresent` ignores the return of `SDL_SubmitGPUCommandBuffer` and returns true. A successful flush only establishes successful recording, not successful submission. This affects device/resource-failure handling, not measured normal-frame performance. | Use a checked submission boundary through an SDL fix/API or a fully owned command path; add failure-injection coverage. Checking a stale `SDL_GetError` string is not a reliable substitute. |

Microsoft explicitly warns against CPU reads from UPLOAD/write-combined memory
in [ID3D12Resource::Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map).
Our code contains the read/modify/write pattern; the size of the Windows penalty
is unmeasured. Likewise, upload allocation and extra command submissions are
confirmed source paths, not measured D3D12 millisecond savings.

For finding 2, the relevant library chain is
[`GPU_UpdateTextureInternal`](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/render/gpu/SDL_render_gpu.c#L436)
→ [`D3D12_CreateTransferBuffer`](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/gpu/d3d12/SDL_gpu_d3d12.c#L4040)
→ `D3D12_INTERNAL_CreateBuffer` / `CreateCommittedResource`, followed by
[`D3D12_UploadToTexture`](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/gpu/d3d12/SDL_gpu_d3d12.c#L5971).
Microsoft's [texture upload guidance](https://learn.microsoft.com/en-us/windows/win32/direct3d12/upload-and-readback-of-texture-data)
specifies the 256-byte row / 512-byte placement alignment baseline. The existing
SIM atlas uploader already handles it with `ArSdlTextureUploadLayout_Append`;
the ordinary SDL texture-update path does not use that helper. Our upload-byte
counters measure texel payload, so they omit SDL's extra allocations and copies.

For finding 3, see
[`D3D12_Submit`](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/gpu/d3d12/SDL_gpu_d3d12.c#L7964)
and Microsoft's [command queue guidance](https://learn.microsoft.com/en-us/windows/win32/direct3d12/executing-and-synchronizing-command-lists).
Reducing submissions must not remove the real ordering boundary between queued
SDL work and a native consumer. `SDL_FlushRenderer` alone does not submit an
offscreen renderer's command buffer. Separate dependent compute passes remain
necessary under SDL's contract; merging command buffers and merging dependent
dispatches into one pass are different changes.

For finding 4, Microsoft's
[PSO cache sample](https://learn.microsoft.com/en-us/samples/microsoft/directx-graphics-samples/d3d12-pipeline-state-cache-sample-win32/)
explains first-use compilation hitches. Existing pipelines are retained after
creation; this finding does not allege per-frame shader recompilation.

For finding 5, see
[`GPU_RenderPresent`](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/render/gpu/SDL_render_gpu.c#L1473).
The native final swapchain submission already checks its own return value;
that does not validate earlier offscreen submissions.

#### Contracts that the inspected paths preserve

- **No normal resident motion readback:** `CaptureOwnedNative` passes a null
  download target while resident, and analysis, actor motion and projection stay
  on the GPU. Downloads/fence waits remain for validation, a deliberately
  nonresident path and exceptional recovery. Those must be identified separately
  in performance results rather than described as zero readbacks for every mode.
- **Thread ownership and lifetime:** the optional preparation worker acquires
  and submits its own command buffer; its semaphore publishes submission, not
  GPU completion. Consumers are ordered after submission, and teardown joins
  the worker before releasing resources. SDL's resource tracking/deferred release
  and transfer-buffer cycling cover in-flight GPU use. Do not replace cycling
  with a fixed modulo reuse rule without fences. See Microsoft's
  [fence-based resource management](https://learn.microsoft.com/en-us/windows/win32/direct3d12/fence-based-resource-management).
- **Compute dependencies:** bound/cost/refine/validate and effect-mask stages
  use distinct passes. SDL D3D12 performs UAV/resource transitions at the pass
  boundaries. The native BG decoder binds distinct skybox placeholder outputs;
  handoff validation rejects source/destination aliasing. No explicit app-level
  device-idle wait was found in the normal resident rendering path. See
  [D3D12 resource barriers](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12).
- **Resource reuse:** background packets, native vertex/index buffers, effect
  buffers, shaders, samplers and retained SIM meshes are reused; growth is bounded
  or geometric. This is distinct from finding 2's residual SDL uploads. SDL itself
  owns descriptor heaps and command allocator retirement; the game must not
  reset or mutate those native objects behind SDL's tracking.
- **Portability:** current shaders use generated SM 6.0 DXIL, explicit register
  spaces and checked C/GLSL layouts. Reviewed kernels use 64-thread or 8×8
  workgroups without a wave-size assumption. Optional device features remain
  disabled and the reduced D3D12 resource-slot option remains enabled. This is
  source-level evidence, not an integrated-GPU or Windows qualification.
- **Presentation:** the ordered adapter has one window present; offscreen
  `SDL_RenderPresent` calls are submissions, not extra window presents. SDL uses
  `DXGI_SWAP_EFFECT_FLIP_DISCARD` and transitions the swapchain image for present.
  Our offscreen work precedes swapchain acquisition, and a prepared swapchain
  command buffer is submitted during teardown rather than cancelled.

#### Remaining backend limits and validation gaps

SDL 3.4.12/3.4.16 D3D12 uses the single direct queue for the inspected GPU work.
The preparation worker enables CPU recording overlap, not a separate asynchronous
compute queue. Also, its swapchain code clamps buffer count to 2–3 and advances
its in-flight fence ring using that count: requesting one frame in flight does
not reproduce a DXGI one-frame latency limit. It does not create a
`FRAME_LATENCY_WAITABLE_OBJECT` swapchain. We request one in Vsync mode, so our
log currently reports a requested policy, not proof of the effective Windows
display queue depth. Microsoft discusses flip-model latency controls in
[DXGI guidance](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model).
Do not infer actual input/display latency from that setting or assume Deck
prepare-lead timings transfer unchanged to Windows.

The full shader consistency check was run using the existing local DXC 1.9
build. **41 of 44 shader headers pass; three fail the strict DXIL program
comparison:** `action_effect_moon_comp.h`, `action_effect_project_comp.h` and
`action_effect_shadow_comp.h`. Their regenerated MSL and SPIR-V are byte exact.
DXIL differences include local allocation/SSA ordering beyond the build-ID
exception; no changed visual behavior or invalid DXIL has been demonstrated.
Three repeat compilations of the same shadow HLSL with the local tool are stable.
This remains a reproducibility gate: pin/recover the generating DXC build,
compare its output and validate native Windows images before choosing new blobs.
Do not silently weaken the check or overwrite the committed shader arrays just
to make this audit pass.

Seven focused non-GPU tests pass: shader tools, upload mirror, upload rectangle
coalescing, SDL state, SDL presentation, upload metrics and upload alignment.
Eight Metal tests pass with no skips: shader blobs, main/worker handoff, action
scene projection, block motion, native BG decode, resident frame generation and
worker frame generation. The first sandboxed shader-blob attempt could not
access the display; the explicit GPU-enabled rerun passes. The full shader
consistency command is **not** reported as passing.

No Windows D3D12 device, PIX capture, GPU-based validation or D3D12 timings were
available/executed in this audit. Normal SDL debug mode is not proof of GPU-based
validation. Microsoft documents its distinct shader/resource checks and overhead
in [GPU-based validation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-d3d12-debug-layer-gpu-based-validation).
Run correctness with the debug layer/GBV first, then performance without GBV.
Use [PIX timing captures](https://devblogs.microsoft.com/pix/timing-captures-new/)
to separate CPU submission stalls, GPU pass cost, queue bubbles and presentation.

Windows acceptance should explicitly select `SDL_GPU_DRIVER=direct3d12`, reject
GPU-test skips, record the SDL/DXC/driver versions, and cover moving Fillmore,
Bloodpool water/gallery, Aitos waterfall rooms `0402`/`0403`, dense effects and
SIM retained geometry. Include interpolation on/off, maximum extended view,
resize, minimize/restore and scene transitions. Report cold startup separately
from warm producer/GPU/presentation distributions. This also closes the Aitos
coverage gap; the old 75–77 FPS waterfall result cannot qualify this renderer.

Audit evidence: `runs/d3d12-audit-2026-10-03/` holds the versioned upstream source
snapshots, Metal test log, full shader check and per-format shader comparisons.
The findings above remain open. Only this audit record is changed after the
user-requested commit.

### 2026-10-03 — Remove mapped upload-memory reads

Implemented finding 1 from the D3D12 audit in `action_effect_source_sdl.c`.
Faded primitives are adjusted one at a time in an ordinary CPU local and copied
once into upload storage. Full-brightness batches retain their bulk `memcpy`.
Occluder rectangles and aggregate bounds are calculated in CPU locals before
copying to upload storage, including the empty-set infinity bounds. Mapped
upload storage is no longer read by the effect upload path.

Packet revision/brightness caching, transfer-buffer cycling, GPU submissions,
shader layouts and shader blobs are unchanged. There are no new heap allocations,
GPU waits or whole-batch CPU scratch buffers.

Expanded the existing resident GPU oracle to cover RGB and alpha preservation,
quad fourth-corner colors, fading to black and recovery, unchanged source
primitives, repeated faded packet reuse, camera/apron-translated occluder bounds,
actual shadow attenuation and removal of all casters after a populated upload.
The Release test target builds; the resident and native-worker frame-generation
tests pass on Metal with no skips. `git diff --check` passes.

Finding 1 is fixed at source level. Native Windows D3D12 timing and correctness
qualification remain outstanding; no Windows performance gain is claimed.
Findings 2–5 and the shader reproducibility gate remain open.

### 2026-10-03 — Widescreen sprite edge coverage

Committed the preceding mapped-upload fix as `6bf2601a`. Investigated
`saves/snapshots/snap_01_gf2437`: Aitos `0401`, camera `(190,488)`, 120 extra
columns per side and 64 extra rows above/below. The right bamboo includes a
16-pixel part at X=370 crossing the ordinary scanout boundary at X=376.

Two omissions caused the premature clip. The host apron channel recorded only
X-rejected parts, losing the outer pixels of accepted straddlers. The compositor
then cropped OBJ meshes to the ordinary window even though verified BG meshes
could draw their guards. Capture now records both types in component order;
the apron rasterizer still writes only outside ordinary scanout. OBJ meshes
draw the complete existing capture, with matching sparse-coverage masks. UVs
and mesh width expand together to preserve sprite positions and attached-effect
projection. OAM admission, object activation and allocation limits are unchanged.

No new GPU submissions, readbacks, textures or shader variants are introduced.
The extra CPU work is limited to rasterizing accepted parts that overlap the
existing guard and including guard columns in cached OBJ coverage scans. This
is not a new performance benchmark or Windows/Deck qualification.

Validation: Release build and five sprite/compositor tests pass. The compositor
also passes native ASan/UBSan and 108 native/WASM command/projection comparisons
covering 4:3, 16:9, 16:10, extended rows, zoom, tilt and skybox modes. Sprite cases
cover both edges, 8/16-pixel parts, finite guard limits and the final OAM slot.
Matched Metal captures at camera `(190,488)` restore the right bamboo; WRAM,
OAM, high OAM, VRAM and CGRAM are byte-identical before/after. Evidence and the
isolated capture script are under `runs/sprite-edge-2026-10-03/` (`before-wide`
and `after-wide`). Comments and the release notes now describe the visible
guard behavior consistently.
