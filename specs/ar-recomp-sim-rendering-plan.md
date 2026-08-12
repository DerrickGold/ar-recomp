# ActRaiser simulation-town 3D rendering plan

## Status and scope

This document is the implementation plan for enhanced rendering in the six town
simulation maps. Phase 0 evidence tooling is implemented; the enhanced renderer
itself begins in Phase 1.

**Current-status note (2026-08-12):** use the status table in `specs/README.md`
and the town matrix in `docs/progress.md` for what remains. The checkpoint
narrative below records the 2026-07-22 implementation state and is retained as
design evidence; later work has superseded several “what remains” paragraphs.

Implementation checkpoint (2026-07-22): Phase 3 / D3b is landed. D1's
frame-owned source/OAM metadata and 512x512 semantic atlas remain the required
integrity gate. D2 adds ten observational Mode-1 capture planes in exact SNES
painter order, reconstructs the pitch-zero frame on the CPU, compares every RGB
pixel with the same-frame authentic framebuffer, and publishes
`SeparatedComposite` only when that comparison and the atlas are both valid.
The renderer supports the enhanced profile and the diagnostic-layer views.
Unsupported PPU/color-math states,
pickers, renderer/capture conflicts, allocation failure, atlas failure, or any
pixel mismatch fail closed to A0 with a named inspector/trace status.

The `D2-flat-actions` checkpoint covers 10,072 valid town frames: 9,927 use A1,
13 intentionally fall back for unsupported PPU state, and 132 fall back for
unsupported color math. It reports zero mismatching pixels across 6,993 unique
separated images, all 10,072 authentic framebuffer hashes match the master-off
pass, and the sampled 256x224 A/B artifacts are byte-identical with an empty
difference image. All six picker exits—including all five targeted miracle
confirmations—publish enhanced capture on the first post-selection frame. The
canonical D1 object accounting remains 73,145 valid atlas fragments with no
overflow or accounting error.

D3a factors the action/SIM projection math into `scene3d_math`, projects the
BG1-low/high priority bands through an 8x6 ground mesh, and keeps BG2, BG3, and
all OBJ priority bands in the documented flat bypass. A dedicated Simulation
settings category exposes the master toggle, the per-stage toggles, camera
pitch/yaw/distance, the presentation tuning dials, and reset. Free-camera input mirrors diorama: right-drag
orbits, the wheel zooms, and middle-click resets; an active picker never owns
those inputs. The canonical `D3a-ground-projection` replay validates 1,884
metadata frames, 1,108 projected-capable frames, four picker intervals, zero
underlying framebuffer-hash changes, 272,524 changed output pixels in the A2
renderer screenshot, and a pixel-identical authentic/top-down picker screenshot.

D3b replaces each world OBJ priority plane with semantic-atlas draws at the
same hardware rank. Every priority fragment of a source shares one union-derived
bottom-centre foot, the foot is transformed by the same ground matrix, the
sprite remains screen-facing, and perspective scale comes from that anchor's
clip depth. Fixed OBJ remains flat. The canonical `D3b-object-billboards`
A2-versus-A3 replay validates all 1,884 metadata frames and 14,997 atlas-object
instances without accounting errors, changes 6,186 output pixels, preserves
every underlying framebuffer hash, and produces a pixel-identical top-down
picker. It also validates immediate enhanced frames at game frames 1102, 1368,
1914, and 2314; persistent `$D233-$D302` selector objects are projected onto
the map plane rather than billboarding. The companion `D3b-wide-hud-handoff`
checkpoint verifies 484 Wide Full
frames with zero underlying framebuffer-hash mismatches and no hourglass-shaped
cutout in projected BG1; its picker is also pixel-identical.

D3c adds the locked object-height policy as a pure data table. Every fragment
carries a classified presentation plane, a virtual height in authentic SNES
pixels, and its anchor/shadow traits; the presenter lifts a projected billboard
along the ground normal, leaving the ground anchor free for the D4 shadow pass.
Classified anchors are part of the descriptor, so the arrow and ground-targeted
lightning keep their record origin even when height resolves to zero — the
documented VirtualHeight bypass restores exactly the D3b image. The canonical
`D3c-virtual-height` A3-versus-A4 replay validates 1,884 metadata frames and
14,997 atlas objects without accounting errors, changes 2,353 output pixels,
preserves every underlying framebuffer hash, and produces a pixel-identical
top-down picker. The companion `D3c-height-variations` checkpoint runs the full
12,000-frame action replay and proves the arrow (219 fragments across all four
compositions), Napper ground-pluck (36 across `$E71B/$E73A/$E75E`), Blue Dragon
building lightning plus ground fire (913), grounded map objects, and map-plane
selectors all reach their documented planes, with no lifted object ever exceeding
the 24-pixel flight plane and no fixed record entering the height system.
Sailboats are the one classified family Fillmore never spawns; the water-plane
policy is asserted only by unit test until a coastal town replay exists.
`$D993` is the 64x64 hollow path/area selection square: a second map-plane
cursor outside the `$D233-$D302` family, on a class-`$09` record, which must
lie on the selected square rather than billboard.
Live thunder/rain/wind captures added the `$D9E5-$DCD2` miracle cloud family:
its bolt and rain frames are single compositions whose art spans cloud to
ground, and `$DA22` is the ROM's own shadow ellipse on a co-located record, so
the range stays on the map plane with a record-origin anchor and no synthetic
shadow. The same captures showed the angel's `$A627-$A792` pose frames borrowed
by a miracle effect record, so the angel is now selected by record and class
only.
`sim3d_height_scale_x100` (`AR_SIM3D_HEIGHT_SCALE`, default 100, range 0-400)
scales every classified plane for live tuning. It is resolved into the frame
snapshot beside the camera pose, never read by the present thread, and `0` is a
deliberate ground-everything value distinct from disabling the stage, so the
capture defaults the field to 100 rather than treating zero as unset. At `0`
the output is pixel-identical to the A3 billboard image.

Ground contact is preserved by two mechanisms, both confined to enhanced 3D
frames: contact-exact classes (`ground_effect`, `ground_strike`) land on their
first frame, and every other plane change eases at 4 px per frame per world
record, snapping instead when the record was absent from the preceding build so
a recycled slot never inherits the previous actor's plane. The Blue Dragon's
state-6 strike lowers the body onto its bolt's plane, because the ROM emits
both on the same record on alternating frames and repositions the record onto
the target itself. `D3c-height-variations` asserts this directly: 128 ramp
steps, zero slew violations, and `ground_strike` present. The height census over
the full replay is 25,007 flying, 16,535 grounded, 913 ground effect, 219 flying
projectile, 36 semi-grounded, 27 ground strike, and 6,183 map plane.

Phase 4 / D4a landed ground-only hard shadows on top of that classification.
Casters are pure data — `Sim3D_ObjectCastsShadow` selects a world-tier object
with usable atlas art that D3c did not mark `MapPlane` or `NoShadow` — so the
shadow pass never re-derives an identity or tests a height. Silhouettes
accumulate into a dedicated transparent target that is composited immediately
after the BG1-low ground draw, which is what keeps a shadow off sky, dialogs,
HUD, and settings, and what stops overlapping casters from double-darkening the
same ground pixel.

Four presentation decisions are worth recording, because in each case the
physically faithful choice reads worse than the shipped one.

**The silhouette is laid flat, not swept along the light.** A true shadow of a
camera-facing billboard — shearing each silhouette corner along the light and
projecting it onto z=0 — collapses to a two-or-three-pixel smear under this
shallow pitch, because the billboard has no depth for the light to sweep
through. The shipped pass lays the silhouette flat on the ground about the
caster's foot, scaled by `kSimShadowFootprintDepth` (0.6) along the ground's
depth axis, so it foreshortens with exactly the same projection as the ground
texture it sits on.

**The light is near-overhead** (`kSimShadowLightX` 0.10, `kSimShadowLightY` 0),
not the "above and slightly camera-left" the earlier draft of this document
specified. An angled light slides the shadow sideways, where the eye reads the
offset as lateral position rather than altitude. Overhead puts the ground point
directly beneath the lifted billboard on screen, so the vertical gap between
actor and shadow is unambiguous. The small lateral bias only keeps a grounded
sprite's shadow from hiding exactly behind its own feet.

**Height changes shadow size and sprite size in opposite directions.** A
directional light casts a constant-size shadow, which reads as no height at
all, so the footprint shrinks with height (`kSimShadowHeightShrink`, ~70% at
the 24px flight plane) while the billboard gains a deliberate scale pop on top
of the ~1.5% the lift genuinely produces. Both are driven by the resolved world
height, so the player's height-scale tuning feeds all of it. The pop is a
setting (`sim3d_height_pop_pct`, default 5%) rather than a constant, and is
normalized against the catalogue flight plane so the percentage means what it
says at any height scale: a first pass at a fixed ~+12% read as inflated
sprites rather than as altitude, which is a judgement call that belongs to the
player, not to a literal in the renderer.

The rejected alternative was shrinking the ground and grounded actors instead
of growing the flyers. Scaling the ground together with its actors is just a
camera zoom-out — identical relative effect, but it reframes the town and
fights the distance setting — and shrinking grounded actors alone breaks their
footprint against the map tiles they stand on. A flyer has no fixed size
reference in the art, so it is the cheap place to put the difference.

The resulting dials are orthogonal, which is the point: camera distance is a
global zoom, the height scale sets the Z separation between the flight and
ground planes (the screen offset and the true scale change both fall out of the
projection — there is no separate Y shift, and adding one would be the rejected
depth bias), and the pop scales lifted objects only. The billboard is scaled *in place* rather
than biased along the depth axis: pulling a flyer toward the camera moves it
back down-screen and closes the very gap to its own shadow that sells the
altitude.

**Billboards sort back-to-front within their priority band.** On the flat SNES
screen, OAM order alone decides overlap and is correct because everything
shares one plane. Once the map is projected, two actors on different map rows
really are at different distances, and honouring OAM order lets a far actor
paint over a near one. Sorting is confined to the band, so the hardware
priority bands still own the coarse layering, and reverse OAM order remains the
tiebreak so multi-part actors keep their authored overlap and the order stays
stable frame to frame. This subsumes the "separate flight and ground object
layers" idea: depth order gives flyers-over-ground actors for free, and also
gets near-ground actors right relative to each other.

`D4a-hard-shadows` (A=`0x000F`/B=`0x001F`) asserts
the caster census independently from the trait data: 5,003 casters over the
replay, 4,055 of them lifted, and no caster carrying `NoShadow`, `MapPlane`, a
fixed tier, or a record its own frame no longer lists.

Phase 4 / D4b landed the soft-shadow blur, plus player control of the light.

**The blur needs no shader, and the contract changed to say so.** This document
originally made `SoftShadows` depend on shader availability, with a hard-shadow
fallback when shaders are missing. In practice the only shader path in the
codebase is a hand-written MSL blur that requires the GPU renderer backend and
is off by default, so honouring that dependency would have shipped soft shadows
dark for almost everyone — the same failure mode that hid D3c and D4a. The
shadow mask is a render target we already own and is pure alpha, so a box blur
is exactly a weighted sum of alpha taps: `BlurSimShadowMask` does two separable
passes of ordinary blended draws through a scratch target, using a custom
blend mode that preserves the destination colour and *adds* source alpha, with
each tap's weight carried in the texture alpha mod. That works on every
renderer backend. `SoftShadows` therefore depends on `Shadows` alone; a missing
scratch target or custom blend mode degrades to D4a's hard silhouette at draw
time instead of clearing the bit.

**The light is now a pair of settings**, not constants: `sim3d_light_azimuth_deg`
(which way the shadow is thrown) and `sim3d_light_elevation_deg` (90 = straight
overhead, throwing no offset at all), resolved into shear as `cot(elevation)`
and clamped so a near-horizon light cannot throw a shadow to infinity. The
near-overhead default is retained for the reasons recorded under D4a, but it is
now a starting point rather than a constraint. `sim3d_shadow_softness_pct`
controls the blur radius, scaled with the viewport so the look is
resolution-independent.

`D4b-soft-shadows` (A5 vs A6) asserts the blur changes edge softness *and
nothing else*: the checkpoint carries both a minimum and a **maximum** on
changed output pixels, so a blur that shifted geometry or leaked past the ground
fails rather than passing for having changed something.

Phase 4 / D4c landed the optional rim light, which closes Phase 4.

Sprites have no normals, so the only physically meaningful lighting product
left is an edge. `DrawSimRimLight` builds one with two extra silhouette draws
rather than a shader: the billboard is drawn once in the light colour, offset
toward the light, and the sprite's own body is then erased out of that
silhouette, and that silhouette is then **intersected** with the sprite's own
body by a custom blend that multiplies destination alpha by source alpha. What
survives is a band just inside the lit edge, composited additively.

The intersect is load-bearing and was got wrong first: subtracting the body
instead leaves the band *outside* the silhouette, painting a halo onto the
background. That reads as the sprite glowing rather than being lit, and because
the halo scales with strength, lowering the strength only produces a fainter
version of the same wrong shape. The action-stage rim shader (`kRimLightMSL`,
`diorama.c`) avoids this by construction — its edge term is multiplied by the
pixel's own alpha — so both renderers now light only pixels the sprite owns.

Its restriction to billboard silhouettes is structural rather than a check:
the shared draw loop skips map-plane art, and each band's rim is composited
immediately after that band's own billboards, so the rim can never light the
ground, the HUD, or a sprite in a later priority band. The screen-space
direction is the opposite of the shadow shear plus a constant upward bias, so
the shipped near-overhead light — whose shear is nearly zero — still lights top
edges rather than nothing.

`sim3d_rim_strength_pct` (default 10) is the contribution; `0` restores
unmodified sprite colour with the stage still enabled. `D4c-rim-light` (A6 vs
A7) carries the same minimum/maximum pixel bounds as D4b, and its difference
image is confined to the sprite outlines.

One D2 fidelity defect surfaced during play and was fixed here rather than in
Phase 2, because it only appears once the enhanced view is running: an object
whose X wraps negative rasterizes into the widescreen margin columns, and when
the camera sits at a map edge the live margin has collapsed, so the hardware
shows black there. Both `ComposeFlatPixelsPolicy` and `RestoreTownHudPolicy`
applied the `live_x0/live_x1` rule to their base fill but not to their plane
compositing, so 4-8 pixels differed and D2's byte-equality gate correctly failed
the frame closed — one flat frame every time an arrow left the screen sideways.
`D2-margin-object-exit` pins it, and `[sim3d-view]` console lines plus
`AR_SIM3D_DUMP_ON_MISMATCH` now make any future capture drop self-reporting.
Full detail in `docs/bug-ledger.md` §23.

Phase 5 — camera, transitions, backdrop, and the shipped settings surface — is
next.

The target maps are exactly:

```text
$7E:0018 == $00
$7E:0019 == $01..$06
```

They are Fillmore, Bloodpool, Kasandora, Aitos, Marahna, and Northwall. Sky
Palace, the world map, temples, action stages, transitions, and other `$00`
submodes are out of scope. The existing flat renderer remains the fallback and
the fidelity reference.

The feature should be independently switchable from the action-stage diorama.
The action renderer's `diorama_mode` gate and behavior must not change.

## Findings that change the design

### Town simulation is not Mode 7

The earlier action-rendering plan describes simulation mode too broadly. The
actual town view is ordinary PPU Mode 1, not Mode 7. A traced Fillmore frame has:

```text
bgmode    = $09       (Mode 1 with BG3 priority)
BG1SC     = $63       (tilemap at VRAM $6000)
BG2SC     = $73       (tilemap at VRAM $7000)
BG3SC     = $58       (tilemap at VRAM $5800)
BG12NBA   = $0500
```

The useful semantic split is:

| Source | Town role | Enhanced presentation |
|---|---|---|
| BG1 | scrolling 512x512 town playfield | perspective ground plane, with its two hardware priority bands preserved |
| BG2 | centered dialog/frame and other UI staging | flat screen-space overlay unless the Phase 0 census proves a world use |
| BG3 | simulation HUD and dialog text | flat screen-space overlay; the top 32 rows keep the existing HUD anchoring |
| fixed OBJ records | screen-relative UI/effects from `$06A0-$09FF` | flat screen-space overlay |
| world OBJ records | camera-relative actors/effects from `$0A00-$1087` | individual feet-anchored billboards |

This is not the action diorama's stack of parallel full-screen planes. Treating
the whole town OBJ capture as one upright plane would pivot every actor around
one screen edge and would make correct ground anchors and per-actor shadows
impossible.

### The ROM has a semantic position-picker flag

Use `$7F:9215`, not menu text, input bits, OAM signatures, or the town-development
state, to decide when the view must be top-down.

ROM evidence:

- The still-unclassified type-`$0B` position-placement path at `$01:93DC` sets
  `$7F:9215 = 1` at `$01:93E4`; cancel and confirm clear it at `$01:9411` and
  `$01:942B`.
- Direct the People's on-screen `Building Direction` path reaches `$01:972F`,
  which sets the same flag at `$01:9737`, and clears it through
  `$01:9694/$01:9699`.
- The shared targeted-miracle picker `$01:9754` sets it at `$01:975C`; cancel
  clears it at `$01:9789` and confirm clears it at `$01:97CF` after copying the
  selected map cell to `$7F:90E1/$7F:90E5`.
- The ordinary town update, marker actors, and input code read this flag at
  `$01:9C01/$9C3C`, `$01:B0E6`, `$01:B282`, and `$01:C720/$C90A/$C94F`.
- `$03:80D5` clears it during simulation initialization.

The product predicate is therefore:

```c
town_map && ReadWram16(0x19215) != 0
```

Name this state `sim_map_picker_active` in host code. The implementation must
still replay every command and miracle in Phase 0 to prove that all positional
pickers use the flag and that no non-positional mode leaves it set.

Do not use these attractive but incorrect substitutes:

- `$7F:7CC9`: per-town development/construction state.
- `$7F:6BB7/$6BC3`: construction progress state used by `$03:8700`.
- `$033C/$033D`: spawn-list scratch and rotating subvariant.
- `$033E`: story/event dispatcher code.
- `$00:A0/$A1`: current input shadows.
- Miracle result fields `$7F:96E8/$96EA/$96EC`: post-selection effect state.

`$7F:7CA1` is also not a complete picker-mode enum. It is the pending
world/structure type: the unclassified `$01:93DC` path stages `$000B`, while
Direct the People / Building Direction and the shared targeted-miracle picker
both stage `$0009`. Log it as corroborating evidence, but use the ROM entry
routine when a fixture must distinguish the two `$0009` paths.

### Existing object code already exposes the right seam

`$01:ACD9` builds OAM in two ordered scans:

```text
fixed records: 48 x $12 bytes, $06A0-$09FF
world records: 44 x $26 bytes, $0A00-$1087
```

The two world composition leaves, `$01:ADAD` and `$01:AE6F`, are already HLE'd
as `ActRaiser_BuildSimSprites` and `ActRaiser_BuildSimSpritesAlt`. At those
leaves the host knows the source record, world coordinate, composition parts,
attributes, OAM cursor before and after emission, and whether the record is
fixed or world. Capture semantic render metadata there. Do not rediscover
object identity later from pixels.

## Required invariants

1. The game remains authoritative. The feature may read game/PPU state but may
   not alter gameplay WRAM, object records, camera decisions, collision, target
   selection, VRAM, CGRAM, or OAM.
2. The present thread reads one immutable `FrameSlot` plus uploaded textures.
   It must never read live `g_ram`, `g_ppu`, `g_settings`, OAM, or host producer
   metadata.
3. Pixel/atlas buffers follow the existing upload handshake: the present thread
   uploads them before releasing the game thread to draw the next frame.
4. UI, dialogs, settings, and the scene inspector are screen-space layers. They
   never inherit the ground projection.
5. Entering a map picker changes to the authentic flat view on the same rendered
   frame. Input is never delayed to wait for an animation.
   **Amended 2026-07-22:** this is now a build-time option,
   `AR_SIM3D_PICKER_TOPDOWN`, and is compiled **out** by default while the
   projected ground is evaluated for targeting. The path is retained, not
   deleted. What the invariant protected is unchanged either way: `$7F:9215`
   still selects the picker, D-pad targeting stays in original game
   coordinates, the ROM still chooses the cell, and the selector composition
   is still painted onto the map plane rather than billboarded. Only the
   presentation of those frames changes, and the switch must remain a
   single-definition change so the authentic view can be restored for a
   fidelity comparison at any time.
6. Unsupported or ambiguous frames use the authentic flat path. A cosmetic
   feature may fail closed; it may not guess through a fidelity hazard.
7. With the feature disabled, captures, OAM behavior, action diorama rendering,
   and the existing widescreen policies are byte-for-byte unchanged.
8. Every independently visible enhancement is controlled by one named runtime
   feature bit. The present thread receives the resolved bitmask in the frame
   snapshot; no render stage reads live settings or hides an additional implicit
   enable condition.

## Feature-switch contract

The implementation must make incremental visual comparisons possible without a
rebuild, savestate reload, or change to gameplay state.

| Feature bit | Enabled behavior | Disabled behavior |
|---|---|---|
| `kSimFeature_SeparatedComposite` | use the captured semantic layers and atlas | use the complete authentic composite |
| `kSimFeature_GroundProjection` | project BG1 through the oblique camera | render BG1 with the pitch-zero authentic mapping |
| `kSimFeature_ObjectBillboards` | draw isolated world records from the semantic atlas | draw world OBJ through the authentic flat overlay |
| `kSimFeature_VirtualHeight` | apply classified angel/enemy/projectile heights and dynamic overrides | use zero presentation height while retaining the selected anchor |
| `kSimFeature_Shadows` | draw the ground-only per-object shadow mask | omit the shadow pass entirely |
| `kSimFeature_SoftShadows` | blur the shadow mask | retain the hard-shadow fallback |
| `kSimFeature_RimLight` | apply the optional silhouette rim contribution | preserve the unlit sprite colors |
| `kSimFeature_Backdrop` | draw the selected atmospheric backdrop behind the finite ground | use the neutral/scene-derived clear |
| `kSimFeature_PickerExitEase` | ease only the return from a completed picker | cut directly back to the enhanced view |

Resolve dependencies once in `Sim3D_ResolveFeatureMask`, record both requested
and effective masks for diagnostics, and use the effective mask everywhere:

- all enhancement bits depend on `SeparatedComposite`;
- `VirtualHeight`, `Shadows`, and `RimLight` depend on `ObjectBillboards`;
- `SoftShadows` depends on `Shadows` only (see the D4b note: the blur needs no
  shader, and a missing blur target degrades at draw time);
- unsupported capabilities clear only their dependent bits, except a fidelity
  hazard, which selects the complete authentic fallback;
- picker entry, unsupported-state fallback, atlas-integrity checks, and the
  feature-off byte-equality path are safety rules, not optional feature bits.

Comparison is done by toggling stages between runs of the same deterministic
replay, not by composing two profiles inside one frame. The incremental
profiles below are therefore run labels, not renderer state:

```text
A0 authentic composite
A1 separated flat recomposition
A2 + ground projection
A3 + object billboards
A4 + virtual height
A5 + hard shadows
A6 + soft shadows
A7 + rim light
A8 + atmospheric backdrop
A9 + picker-exit ease
```

These internal profiles are test labels, not an ordering dependency beyond the
resolver rules above.

**Amended during D4a (2026-07-22).** This section originally kept every stage
bit out of the player menu, leaving a master toggle plus tuning values, and
retained two developer masks (A and B) with in-frame split/difference views. In
practice the hex masks were the only way to enable a landed stage, and because
the checkpoints pass their own explicit masks, both D3c virtual height and D4a
shadows shipped invisible in normal play without any test noticing.

The stages are now ordinary named toggles in the Simulation category, and the
shipped default profile is `kSim3DShippedFeatures` — one list, in
`sim_render_metadata.h`, that the per-stage defaults and the menu's greying of
unimplemented stages both derive from.

**The A/B mask pair, the comparison-view selector, and the in-frame
split/difference renderer were removed.** They duplicated what the toggles
already express, and no checkpoint ever exercised split or difference: every
one ran `view=B` and obtained its comparison from a second run of the same
replay. That second run now differs by named stage toggles (`baseline_env` in
the manifest), which is both what the harness always did in substance and what
a person would do by hand. One frame renders one profile. The
authentic-versus-separated difference *image* is unrelated machinery and
survives — it is D2's byte-exactness proof, not a view.

The diagnostic layer mask remains a developer-only control.

Also provide a diagnostic visibility mask for BG1 low/high, BG2 low/high, BG3,
world OBJ, fixed OBJ, shadow mask, and backdrop. Those controls deliberately can
produce incomplete images and exist only to inspect capture/painter-order bugs;
they are not enhancement features, persisted settings, or release presets.

## Target visual model

### Ground

BG1 is one finite ground surface. The view starts as an oblique/isometric-style
camera, not a shoebox:

- begin with yaw `0` so the game's cardinal directions remain visually obvious;
- begin with a tunable pitch around 35 degrees;
- keep a small perspective component so the far edge narrows and reads as a
  horizon;
- keep the full captured ground quad visible instead of zooming until its edges
  are clipped;
- use a neutral/scene-derived clear behind the far edge; no walls or ceiling.

Strict 45-degree isometric yaw is a tuning option, not the first implementation.
Rotating the already authored town art 45 degrees costs considerable capture
margin and can make roads/buildings less readable.

BG1's low and high tile-priority bands remain separate. The high band is drawn
at its authentic point in the Mode-1 painter order so trees, roofs, and other
foreground-marked tiles continue to cover the same sprite priority bands.

### World objects

Each world record becomes its own billboard, or up to four billboard fragments
when its OAM parts use different hardware priority bands. A billboard:

- is anchored to the record/composition's foot point;
- is positioned by the same `world - camera` relationship as the authentic
  OAM builder and then projected through the ground transform;
- remains parallel to the output screen;
- preserves the composition's internal part offsets, OAM order, palette,
  flips, and nearest-neighbor pixel sampling;
- scales only by the camera's perspective depth, not by an arbitrary per-type
  zoom.

The removed action `diorama_sprite_upright` experiment supplies reusable matrix
math, but not a reusable presentation strategy. It rotated the complete OBJ
sheet around one full-screen pivot. Simulation towns need per-composition pivots.

### Light and shadows

Use one directional light above and slightly camera-left by default. Sprites do
not have normals, so the useful lighting products are:

- a subtle optional edge/rim contribution on billboard silhouettes; and
- a projected silhouette shadow on the ground.

Build shadows through a dedicated transparent shadow-mask target:

1. Draw projected actor silhouettes into the mask.
2. Blur the mask horizontally and vertically when GPU shaders are available.
3. Composite the darkened mask immediately after the BG1-low ground draw.
4. Draw BG1-high, billboards, BG2/BG3, fixed OBJ, HUD, and settings afterward.

This explicitly avoids the current action shadow bug where a whole transparent
layer quad can read as a dark rectangular sheet. The shadow mask affects only
the ground pass, so it cannot darken the sky, dialogs, HUD, or settings.

Each render-object descriptor has data-driven traits:

```text
casts_shadow
virtual_height
foot_anchor_x/y
anchor = foot | record_origin | target_position
presentation = billboard | ground_effect | screen_effect
```

Default ordinary actors to billboard + shadow. Classify the angel, projectiles,
large effects, and cursor/marker types during Phase 0 rather than scattering
record-address checks through the renderer.

### Locked object-height policy

The ROM crawl and human visual classification in `docs/sim-object-catalog.md`
establish these defaults. They are data-table entries, not rendering heuristics:

| Identity | Height/anchor policy |
|---|---|
| Enemy record classes `$12-$15` | flying; `$13` Napper Bat may transition through a near-ground phase |
| Angel | flying at a fixed presentation height |
| Angel arrow record `$0B0A`; compositions `$D967/$D972/$D97D/$D988` | flying projectile at the angel's fixed height; billboard from the record origin; no shadow; preserve its original planar trajectory, collision, and lifetime/culling |
| People, people groups, horses, dogs, sheep, boats, and other non-enemy world graphics | anchored at map height; boats retain a water-plane presentation trait |
| `$E1BD/$E209/$E255` (visual IDs `$09-$0B`) | Blue Dragon building-zap lightning, anchored to the target building/ground tile rather than the flying enemy |
| `$E71B/$E73A/$E75E` (visual IDs `$3A-$3C`) | Napper ground-pluck frames, semi-grounded/near-ground |
| `$E676-$E6B5` (visual IDs `$30-$33`) | people groups, anchored at map height; `$E661`/visual `$2F` is the same grounded people-group family |
| `$E6CA/$E6D0/$E6D6` (visual IDs `$34-$36`) | ground fire, anchored at map height |

The default rule is therefore **record semantics first, composition override
second**. Enemy records use their flight plane until an explicitly classified
composition/state overrides it. Non-enemy world records use map height. UI and
screen effects never enter this height system. The angel arrow is an explicit
flying-projectile exception to the non-enemy default.

## Render data contract

Add a simulation-specific immutable payload to `FrameSlot`. Suggested shape:

```c
typedef enum SimViewKind {
  kSimView_None,
  kSimView_Enhanced,
  kSimView_AuthenticPicker,
  kSimView_AuthenticFallback,
} SimViewKind;

typedef uint32_t SimRenderFeatureMask;

typedef struct SimRenderObject {
  uint16_t record_address;
  uint16_t world_x, world_y;
  uint16_t oam_first;
  uint8_t  oam_count;
  uint8_t  priority;
  int16_t  foot_x, foot_y;
  int16_t  local_x0, local_y0, local_x1, local_y1;
  uint16_t atlas_x, atlas_y, atlas_w, atlas_h;
  uint8_t  traits;
} SimRenderObject;

typedef struct SimFrameData {
  SimViewKind view;
  SimRenderFeatureMask requested_features;
  SimRenderFeatureMask effective_features;
  uint32_t diagnostic_layer_mask;
  uint8_t town;
  uint16_t camera_x, camera_y;
  uint16_t angel_x, angel_y;
  uint16_t picker_flag;
  uint32_t build_serial;
  uint8_t world_oam_first, world_oam_count;
  uint8_t object_count;
  SimRenderObject objects[MAX_SIM_RENDER_OBJECTS];
  /* Resolved camera/light/effect settings follow. */
} SimFrameData;
```

Exact field packing can change, but the ownership cannot. `FrameSlot_Capture`
is the sole live-state reader and copies the completed producer metadata. Camera,
light, shadow, interpolation, and transition settings needed by presentation
are resolved and copied here too. Feature dependencies are also resolved before
publication for both A and B, and the A/B view is captured with them, so one
frame cannot mix old and new switch values or require a present-thread settings
read.

The existing `DioramaScrollSnapshot` should either become a renderer-neutral
previous-frame snapshot or gain a parallel `SimScrollSnapshot`. Do not make the
present thread retain a pointer to an old `FrameSlot`, because the game thread
will reuse it.

## Object isolation and atlas pipeline

Implement this as a semantic OAM atlas, not as rectangles cropped from the
already composited screen. Cropping screen pixels fails whenever two actors
overlap.

1. At the start of an `$01:ACD9` OAM build, reset a host-only metadata producer
   and increment `build_serial`. The preferred hook is the semantic OAM-build
   boundary. If the existing leaf HLEs are used to detect it, a fresh `$98 == 0`
   cursor is the validated reset condition and needs a unit test for empty and
   fully clipped compositions.
2. In `ws_sim_build_sprites`, record the OAM range emitted for every source
   record. Split a record descriptor when its parts cross OAM priority bands.
3. Immediately before PPU scanout, rasterize those exact OAM ranges from the
   current OAM/VRAM/CGRAM into a packed RGBA atlas. Share the PPU's sprite decode
   and color conversion; do not create a second interpretation of SNES tile,
   flip, size, palette, brightness, or OAM-high bits in game code.
4. Pack each `(record, priority)` image independently. Atlas overflow is a
   diagnosed fallback condition, never silent truncation.
5. Upload the atlas in `PresentUpload`. All later billboard draws reference only
   the slot's atlas rectangles and uploaded texture.

Because fixed records are emitted before world records, the visible world OAM
slots form a contiguous suffix. Capture/remove that suffix from the ordinary
PPU composite. Fixed records must also be available as a flat overlay, either
through the same semantic atlas or through a dedicated fixed-OBJ screen target.
Do not expand the current single OBJ capture range into an ambiguous all-purpose
surface.

The pitch-zero recompositor must preserve Mode-1 painter order. Use the existing
priority table as the reference, but make the simulation ordering explicit and
test it. Do not depth-sort across hardware priority bands. Within one band, begin
with OAM order; optional world-Y sorting is a later visual experiment and must
remain off until overlap captures prove it safe.

## Layer capture and composition

Add a separate `sim3d` capture family. Do not overload `g_diorama_layer_pixels`
or `Diorama_IsActiveThisFrame`, whose assumptions are action-specific.

For a supported enhanced town frame:

1. Capture BG1 low/high into dedicated ground buffers with RemoveFromGame.
2. Capture BG2 low/high into transparent flat-overlay buffers with
   RemoveFromGame.
3. Capture BG3 into a transparent flat-overlay buffer with RemoveFromGame.
4. Remove the world OBJ OAM suffix from the authentic composite and prepare the
   semantic atlas.
5. Preserve or separately reconstruct fixed OBJ as a flat overlay.
6. Keep the residual framebuffer only as the background/color reference; do not
   draw an opaque residual framebuffer over the projected ground.

Presentation order is conceptually:

```text
clear / supported residual backdrop
BG2 low at its hardware rank
BG1 low projected as ground
ground-only shadow mask
world billboard priority bands in hardware order
BG2 high as flat overlay at its hardware rank
BG1 high projected at its hardware rank
remaining world billboard priority bands
BG3 dialog text outside the HUD strip, flat
fixed OBJ, flat
anchored 32-row simulation HUD and hourglass
scene inspector / settings overlay
```

The exact interleave must come from the PPU's Mode-1 priority ranks, not this
abbreviated list. The zero-tilt reference gate below catches any mistake.

BG2 is classified as UI from current evidence, but Phase 0 must isolate it in
the canonical system fixture during day-cycle changes, development, dialogs,
and every miracle, plus one artwork smoke frame per remaining town.
If a town uses BG2 as world art, add an explicit per-scene role backed by evidence;
do not tilt dialogs or leave environmental art floating in screen space.

### HUD and dialogs

- Reuse the follow-up A7 flat/anchored HUD plumbing after it lands.
- BG3 rows `0..31` use the existing simulation HUD split and hourglass promotion.
- BG3 pixels below row 31 remain centered/authentic screen-space dialog text.
- BG2 dialog frames remain centered/authentic screen-space geometry.
- Settings and debug overlays render last at native output resolution.

This work overlaps `actraiser_rtl.c`, `present.c`, `present.h`, and settings files
with the current follow-up improvements. Integrate after A7's capture/present
contract is settled, or isolate work in new files until that point. Never resolve
the overlap by reverting the in-progress follow-up changes.

## Top-down interaction contract

`sim_map_picker_active` selects the authentic flat render path, not a ground
camera whose pitch merely happens to be zero. That gives a pixel-identical and
input-identical targeting view by construction.

**Build-time status (2026-07-22).** `AR_SIM3D_PICKER_TOPDOWN` defaults to `0`,
so pickers keep the projected view; playtesting found the tilted ground
accurate enough to aim on, and the flat cut was more disruptive than the
perspective. Everything below still describes the compiled-in behaviour and is
restored by the CMake option `-DAR_SIM3D_PICKER_TOPDOWN=ON`. The state table's
`$7F:9215` transitions remain the ROM contract in both builds; only the
"authentic flat view" outcome becomes "enhanced view" when the switch is off.
The `sim3d_demo` checkpoints read the compiled value back out of the D1 trace
and assert the matching contract, so neither build leaves picker frames
unverified: the top-down build must never render an enhanced frame while
`$7F:9215` is set, and the projected build must never force an authentic
picker frame and must keep painting the selector onto the map plane.

State transitions:

```text
normal town + feature on
    -> enhanced view

$7F:9215 changes 0 -> nonzero
    -> authentic flat view on this frame (no delayed input)

$7F:9215 remains nonzero
    -> authentic flat view

$7F:9215 changes nonzero -> 0
    -> enhanced capture resumes on this first post-selection frame

map/mode change, unsupported PPU state, atlas overflow, renderer loss
    -> authentic flat fallback
```

Ship both picker entry and exit as immediate cuts. In particular, a targeted
miracle confirmation must not hold or cross-fade the flat picker texture: its
first effect frame belongs to the enhanced renderer. Cross-fading also cannot
be allowed to make the cursor appear displaced while it is active.

SNES D-pad targeting remains entirely in original game coordinates. No inverse
projection is needed for gameplay. If mouse/touch targeting is added later, it
must be enabled only in authentic picker view or use a tested ground-plane ray
intersection.

## Fidelity and fallback gate

Before selecting the enhanced path, require all of:

- `ActRaiser_IsSimulationTown(...)`;
- the new PPU renderer/capture capability;
- PPU mode 1;
- feature setting enabled;
- `sim_map_picker_active == false`;
- complete atlas metadata with no overflow;
- a supported color/window state.

Global brightness/fade (`INIDISP`) is applied to the final enhanced composite.
Per-layer visibility windows are already represented in captured alpha. Color
math windows and cross-layer color math are not generally preserved by the
existing overlay captures. Phase 0 records `windowsel`, `cgwsel`, and `cgadsub`
for all test scenes. Unknown combinations fall back flat for that frame. Add
specific emulation only for combinations proven necessary and covered by a
capture test.

Losing an optional presentation resource disables that effect but never
geometry. The soft-shadow blur falls back to a hard alpha silhouette on the
same ground mask when its scratch target or custom blend mode is unavailable
(it needs no shader — see the D4b note); rim light is likewise built from ordinary
draws and needs no shader. Failure to build the
semantic atlas falls back to the complete authentic renderer; it must never fall
back to the rejected whole-screen upright OBJ plane.

## Implementation phases and hard gates

### Phase 0 — evidence lock and deterministic fixtures

Deliverables:

- Add named RAM constants and comments for `$7F:9215` and the confirmed picker
  entry/exit routines.
- Record one canonical-town fixture set for shared SIM systems: Direct the
  People (the on-screen Building Direction command), all targeted miracles,
  ordinary command menus, dialog, development, the angel projectile, map-edge
  camera movement, and camera shake. Town mechanics are shared and are not
  duplicated six times.
- Add variation-based fixtures for every enemy family and exceptional state,
  especially Napper ground-pluck, Blue Demon building lightning, grounded
  people, and fire.
- Keep one lightweight still/contact-sheet smoke check per town for background
  artwork, palette, layer-role, and backdrop tuning. These are art checks, not
  duplicate command/mechanics recordings.
- Capture isolated BG1 low/high, BG2 low/high, BG3, fixed OBJ, and world OBJ for
  the fixture matrix.
- Produce a state table showing `$7F:9215`, caller/routine, command, and expected
  view for every transition.
- Classify every observed world object type's billboard/shadow traits.

Current implementation status:

- the named full-mirror picker/pending-type constants, pure predicate, and unit
  tests are complete;
- `AR_SIM3D_TRACE` captures picker/view state, live SIM target, confirmed aimed
  cell, resolved miracle kind, camera, PPU layer/color state, and overlay
  descriptors without changing render output;
- `tools/sim3d_demo.py --all` runs four isolated, seed-hashed checkpoints and
  writes an aggregate coverage report;
- the fixtures prove Direct the People / Building Direction, five targeted-
  miracle picker calls and the exact resolved miracle-kind set `1-5`, Napper
  ground-pluck, Blue Demon lightning, grounded people, and ground fire;
- the separate `$01:93DC`/type-`$0B` picker still needs a gameplay-command name;
  it is no longer mislabeled as Direct the People. Other towns need art/layer
  smoke frames, not duplicate system recordings. See `docs/sim3d-phase0.md` for
  live evidence.

Gate: no ambiguous position-picker or layer role remains. If another positional
mode does not use `$7F:9215`, extend one named semantic predicate with ROM evidence;
do not add a visual heuristic.

### Phase 1 — metadata and capture plumbing, feature visually off

Deliverables:

- Add `SimFrameData` to `FrameSlot` and copy it at the game/present boundary.
- Add producer reset and per-record OAM-range metadata to the existing sim HLE
  leaves.
- Add atlas rasterization and overflow diagnostics.
- Allocate dedicated sim buffers/textures and include them in upload ownership.
- Add a debug inspector for object record, priority, OAM range, foot anchor, and
  atlas rectangle.
- Add the central feature-mask resolver and the requested vs. effective-mask
  diagnostics. At this phase every bit still resolves to the authentic
  composite.

Gate: ThreadSanitizer is clean; feature off is screenshot- and trace-identical;
metadata partitions all emitted OAM slots without overlap or loss.

Status (2026-07-22): complete. Focused tests cover shared OBJ decode/color,
flip/high-OAM/wrap behavior, priority mismatch, OAM rotation, successful packing,
descriptor overlap rejection, and forced overflow. The canonical D1 replay
published a valid atlas on all 10,072 town frames with zero invalid objects,
accounting errors, fallbacks, or authentic framebuffer mismatches.

### Phase 2 — flat separated recomposition

Render through the new capture/atlas pipeline with pitch=0, yaw=0, shadows off,
lighting off, and no transition. This is the first implementation of
`kSimFeature_SeparatedComposite`; switching A0/A1 must compare the authentic and
separated paths from the same captured frame.

Gate: for every supported fixture, output must match the authentic flat renderer.
Use exact screenshot comparison where color math is inactive. Any intentionally
unsupported color/window frame must select the flat fallback and match exactly.
Do not begin perspective work while objects, UI, priority, or transparency differ.

Status (2026-07-22): complete for the canonical shared-system fixture. The live
gate currently supports ordinary town Mode 1 with the observed no-op color-math
configuration. It deliberately selects authentic fallback for transition/fade
states it cannot prove and when another overlay policy owns a capture. In
particular, the standard widescreen town HUD capture now hands its surfaces to
the SIM capture and is restored as an anchored host overlay afterward;
unrelated overlay owners still fail closed. D2 uses the PPU-resolved full-screen
OBJ priority bands for its exact zero-pitch image while requiring the semantic
atlas to be valid; D3b replaces those screen-positioned bands with individual
atlas billboards, where feet anchoring and projected overlap become meaningful.

### Phase 3 — ground projection and billboards

Deliverables:

- Factor renderer-neutral projection helpers into a small tested module. If the
  follow-up GEO/`BuildQuadMesh` work has already landed, reuse it rather than
  making a second matrix library.
- Project BG1 low/high through the same ground transform.
- Anchor every billboard fragment to the projected record foot point.
- Keep BG2/BG3/fixed OBJ/HUD flat.
- Use stable WRAM camera `$22/$24` as the base camera source; do not interpolate
  end-of-frame PPU scroll residue.
- Land `GroundProjection`, `ObjectBillboards`, and `VirtualHeight` as separate
  gates with the disabled behavior defined in the switch table.

Gate: feet stay pinned to their ground coordinate while the angel moves, the
camera scrolls, the camera shakes, and the view changes aspect ratio. No sprite
leans with the ground or duplicates at an atlas edge. Map edges reveal neither
wrapped ground nor cleared widescreen margins.

### Phase 4 — directional light and shadows

Deliverables:

- Ground-only shadow mask target.
- Directional projection from billboard silhouette/virtual height to ground.
- Separable blur with a hard-shadow fallback (landed without a shader
  dependency; see the D4b note above).
- Data-driven exclusions/height overrides for effects and projectiles.
- Optional subtle rim light after the shadow path is correct (landed; built
  from two silhouette draws, no shader).
- Land hard shadows, soft-shadow blur, and rim light as separate gates; shader
  failure must clear `SoftShadows`/`RimLight` without disabling geometry.

Gate: no rectangular sheet shadows, no shadows on UI/sky, no dark halos from
straight-alpha linear sampling, and no shadow survives after its source record is
removed. Validate crowded towns and overlapping actors, not just the angel.

### Phase 5 — picker switching, settings, and rollout

Deliverables:

- Same-frame authentic picker switch from `$7F:9215`, when
  `AR_SIM3D_PICKER_TOPDOWN` is compiled in. With the default `0` build there is
  no view change to schedule and this deliverable reduces to keeping the
  selector on the map plane, which D3b already landed.
- Immediate enhanced-view restoration on every picker confirm/cancel; targeted
  miracle effects must never begin behind a held flat picker texture.
- A separate `sim_town_3d` setting, default off until the shared system/object
  matrix and all six town artwork smoke checks pass.
- A small set of resolved tuning settings: camera pitch, camera yaw, shadow
  strength/softness, and light azimuth. Keep engineering/debug knobs out of the
  player menu.
- Named per-stage toggles (see the amendment above). The removed A/B mask pair
  and split/difference display modes are not to be reintroduced: comparison is
  a two-run operation, not renderer state.
- Land `Backdrop` as a separate gate; the mandatory immediate switches into and
  out of an active picker remain outside the mask and cannot be disabled.
- Reset behavior and live setting changes through the existing present-thread
  quiesce/command contract.

**Camera deliverable, partially landed (2026-07-22).** `sim3d_camera_mode` is
Free or Dynamic, mutually exclusive, each owning its own pose — the same split
the diorama camera uses, and for the same reason: with one shared pose,
Dynamic sways around wherever the last manual drag left the camera. Dynamic
leans toward the angel's direction of travel and kicks on a hit, reusing the
diorama camera's construction wholesale (wall-clock damping, additive
impulses). See `docs/rendering-engine.md` §13g.

What remains under this deliverable is *transitions* — picker entry/exit, town
changes, savestate load — which is D5b/D5c work and is about when the camera is
allowed to move at all rather than how it moves.

**Shipped defaults are a captured baseline (2026-07-22).** The numeric sim3d
defaults are a snapshot of a tuned live session rather than derived values, so
a settings reset and a fresh save both land on a configuration that has been
looked at. Several differ from what the surrounding comment argues for on
first principles; where that happens the comment now says so and why the
looked-at value won. `settings.ini` beats compiled defaults, so changing one
affects resets and new installs only.

Gate: every picker confirm/cancel is as usable as the authentic view, ordinary
menus do not unnecessarily flatten the town, savestate/load/map transitions do
not retain the previous view state, and toggling the feature live cannot race the
present thread.

### Phase 6 — performance and default decision

Profile the worst observed 44-record scene, soft shadows, widescreen, 4K output,
and high-refresh presentation. Establish a reference-machine budget before
choosing the default. Record CPU atlas-build time, upload bytes/time, GPU scene
time, and p95/p99 present time separately.

Gate: no missed 60 Hz deadline on the supported baseline, no unbounded atlas or
per-frame allocations, and no regression with the feature off. Enable by default
only after visual approval and the performance gate; otherwise ship opt-in.

## Demo checkpoints

Every implementation slice ends in a runnable checkpoint. Do not defer the
first integrated demo until all phases are complete. Extend the checkpoint
manifest and `tools/sim3d_demo.py` runner (for example, `--checkpoint D3c`) to
load the named deterministic replay and run it twice — once with the stage under
test enabled, once with it switched off by name via `baseline_env` — writing
both screenshots, their difference image, and a short metrics/feature-mask
report. The scene inspector provides the equivalent interactive check by
toggling the stage on a paused frame.

| Checkpoint | Implementation slice | Demonstration | Pass condition |
|---|---|---|---|
| D0 | Phase 0 evidence | representative layer contact sheets, six-town artwork smoke checks, object-variation catalogue, and picker state table | every visible source has a role or an explicit authentic fallback; every tested position picker has the expected same-frame state; shared mechanics are proven once and enemy/effect variants are all represented |
| D1 | metadata and atlas, output still authentic | pause on angel/enemy/people/effect frames and inspect record, anchor, OAM range, atlas rectangle, requested/effective masks, and individual captured layers | inspector accounts for every emitted OAM part while the output hash remains authentic |
| D2 | A0 authentic versus A1 separated flat | toggle the separated stage and show the difference image for representative frames in all six towns | difference is empty where color math is supported; unsupported frames visibly and diagnostically choose authentic fallback |
| D3a | A1 versus A2 ground projection | tilt only BG1 while world OBJ and UI use their documented flat bypasses | map projection changes; dialogs, HUD, and fixed OBJ do not move or tilt |
| D3b | A2 versus A3 object billboards | enable isolated world billboards with zero virtual height while moving the camera and overlapping actors | every billboard stays screen-facing, its selected anchor stays pinned, and overlap order matches the captured priority policy |
| D3c | A3 versus A4 virtual height | enable flight policy with the angel, arrow, each enemy family, people, animals, boats, building lightning, ground fire, and a Napper dive/pluck fixture | angel/enemies/arrow lift to their classified planes; map objects remain anchored; dynamic Napper frames approach the ground without unrelated objects floating |
| D4a | A4 versus A5 hard shadows | toggle per-object hard shadows in a crowded town and during each special effect | only classified shadow casters contribute; flying shadows offset with height; UI, backdrop, arrow, lightning, fire, and removed actors leave no shadow |
| D4b | A5 versus A6 soft shadows | toggle the blur on the same shadow mask | only edge softness changes; shadow position, opacity ownership, and scene geometry remain fixed (asserted by a maximum on changed pixels, not only a minimum); a missing blur target returns to A5 |
| D4c | A6 versus A7 rim light | toggle rim lighting on moving and overlapping sprites | contribution is restricted to billboard silhouettes (structural: the shared loop skips map-plane art and each band composites after its own billboards) and disabling it restores unmodified sprite color |
| D5a | A7 versus A8 **ground extension** (LANDED 2026-07-22) | toggle the world-map underlay and full-town canvas at a town edge | ground continues past the captured window; the town's own composite and the D2 gate are untouched (`separated_mismatch_pixels_max: 0`) |
| D5a-3 | A8 versus A8+ **cull cues** (LANDED 2026-07-22) | toggle the ground fade, ground dim, focus falloff and cloud shroud while walking an actor to each edge of the sprite-drawable window, including a flying record at the near edge | every record the window takes away has cover over it at the moment it goes; no cue is visible inside the window; menus and HUD are untouched by any of them |
| D5a-2 | A8 versus A9 atmospheric backdrop (LANDED 2026-07-22) | toggle the graded sky in each town, at the pitch extremes as well as the default | only pixels behind the finite ground change; UI and captured town layers are untouched; strength 0 is pixel-identical to the previous flat clear |
| D5a-4 | A9 versus A9 + **dynamic camera** (LANDED 2026-07-22) | move the angel in each direction and take a hit, at 0%, 100% and 200% reactivity, across a town entry/exit, and switching Free/Dynamic both ways | lean follows the angel's own travel direction and settles; a hit jolts once and decays; 0% is the static pose exactly; entering a town never jolts on arrival; a mode switch snaps to that mode's own pose and the right-drag is inert in Dynamic |
| D5a-5 | **sun miracle colour math** (LANDED 2026-07-22) | cast every miracle in every town, watching the view-transition log | the view stays enhanced throughout with no `unsupported_color_math` and no `pixel_mismatch` fallback; the tint matches the authentic view |
| D5b | picker contract and A8 versus A9 exit ease | enter, move, confirm, and cancel Direct the People / Building Direction and every targeted miracle; compare cut versus exit ease | picker entry is always an immediate authentic frame, selected cells match gameplay, and the optional ease begins only after the picker clears |
| D5c | master/live switching | toggle enhanced rendering off/on during play, dialogs, map edges, savestate load, and town transitions | off is byte-identical authentic output; no stale atlas, prior-town frame, or present-thread race appears |
| D6 | six-town release candidate | run the full golden matrix and worst-case performance scene at 4:3, widescreen, 4K, and high refresh | all correctness gates pass and p95/p99 timings meet the recorded supported-platform budget |

**Phase 5 scope change (2026-07-22).** `kSimFeature_WorldUnderlay` (bit 9) was
added ahead of `Backdrop` and shipped first. The two are different layers and
were deliberately not merged: the backdrop sits *behind* the finite ground in
front of the sky, while the underlay lives *in* the ground plane, extending it
outward. Splitting them keeps each independently togglable and testable, and
let the underlay land without waiting on the sky's open design questions.

What shipped under that bit is two layers, not one — the half-resolution
Mode-7 world map for everything outside the town, and a full-resolution render
of the entire 512x512 town from the resident WRAM tilemap for the town itself.
See `docs/rendering-engine.md` §13c for the derivation and
`docs/SEAMS.md` for the seams. The same feature bit also gates the widened
sprite emit margin, since it is the view that justifies composing records the
authentic frame never shows.

Two results from that work belong in the plan rather than only in the ledger:

- **Metadata failures no longer drop the view** (ledger §24). The fallback
  contract in this document should be read as applying to the *separated
  capture* only. An unusable object list now costs the sprites for that frame
  and nothing else, because a whole-screen perspective change is a far louder
  artifact than a missing sprite.
- **Off-screen actors are recoverable horizontally only** (ledger §25). OAM's
  Y byte has no ninth bit and the 32 rows it appears to spare overlap the
  authentic viewport. Reaching actors above or below the camera needs a path
  that does not travel through OAM; that is Phase 5 work not yet started.

**Cull cues (2026-07-22).** `kSimFeature_CullHaze` (bit 11) joins
`CloudShroud` (bit 10). Both explain the same boundary and both resolve away
without `WorldUnderlay`, because with no extended ground there is nothing
out-of-range to mark. See `docs/rendering-engine.md` §13d for the design and
`docs/SEAMS.md` for the seams.

The result worth carrying into the plan is a correction to how this document
frames the problem. The plan treats the empty far field as a *decoration*
question — cover the ground that cannot hold actors. It is a **consistency**
question, and stating it as one changes the implementation:

> If a record is being taken away by the sprite window, something must be over
> it — per record, not on average.

A noise field dense enough that gaps are unlikely cannot satisfy that, because
a gap is exactly where a sprite vanishes over clear ground. The first attempt
did precisely this and the gaps were the only thing anyone noticed. Coverage by
probability cannot express a per-record guarantee, so the guarantee moved to
per-record cover driven by cull evidence the emitter now reports, and the
atmospheric layers were demoted to explaining the boundary rather than
enforcing it. Anything later in Phase 5 that hides state from the player should
be read the same way: name the invariant first, then pick the effect.

Two smaller results belong here too:

- **Cover timing and cover placement are different questions.** The emitter
  culls on a record's own y; the renderer draws lifted records up-screen. Cover
  timed and placed at the same point leaves a flying actor blinking out above
  its own cloud.
- **A cue's controls have to stay separable from what they are drawn over.**
  Out-of-range fade and out-of-range darkness began as one control, which
  worked only while the sky behind them was flat black. The graded sky turned
  the same control into a wash toward blue, because the layer it faded to
  hazes toward the backdrop. Splitting them into a structural term (alpha) and
  a photometric one (a colour multiplier) is what made "darker, not hazier"
  expressible at all. Expect any later cue tuned against a placeholder
  background to have the same latent coupling.
- **A pure predicate that feeds a renderer inherits the renderer's sampling.**
  `Sim3D_CullProximity` was correct while the mesh it was evaluated on was too
  coarse to show what it returned, which reads as the maths being wrong. Mesh
  density became a correctness constraint the moment a shape was sampled at
  the vertices.

**The tilted map has no visible horizon (2026-07-22).** Measured across the
whole settable pitch range (`sim3d_tilt_x_mrad`, -700..700), the ground plane's
vanishing line lands between 544 and 5619 destination pixels *outside* a
224-row viewport — closest at -700 mrad, and undefined at exactly 0. This
document's D5a-2 wording, "the atmospheric backdrop at the tilted map horizon",
describes something the camera cannot show.

What reads as sky in frame is where the ground *data* runs out — past the world
map extent, or the near-clip bound — which is a different edge in a different
place. In play that is the corners only, and only when fully zoomed out.

The backdrop therefore grades around a **synthetic** horizon at a settable
fraction of the viewport height, and uses the real one as an anchor only for
the case where it is visible. `Scene3D_GroundHorizonScreenY` exists and is
tested, including an assertion that sweeps the pitch range and fails if any
setting ever puts the horizon on screen, so widening that range forces the
backdrop to be revisited rather than silently producing sky below the horizon.

The general lesson for the rest of Phase 5: this document describes the
projected town in the vocabulary of a 3D scene — horizon, sky, distance — and
the camera it actually ships is close enough to top-down that several of those
words have no referent in frame. Check where a feature is *visible* before
designing what it anchors to.

**Colour math is part of the D2 contract (2026-07-22).** The separated capture
rebuilds the frame from layers, so any PPU colour math has to be reproduced or
the byte-exact gate rejects the frame. The gate fails closed and now accepts
three states: no-op, the targeted-miracle half-add, and a plain fixed-colour
add (the sun miracle). `docs/rendering-engine.md` §13e has the derivation.

Two results generalise beyond this one effect:

- **A state the gate rejects is a feature gap, not a defect.** The frame stays
  authentic and byte-exact. The transition log now prints the offending
  registers, so characterising the next one is a single run rather than an
  investigation.
- **Reproducing hardware maths means reproducing its *order*.** Colour math
  happens in 5-bit component space and the brightness mapping to 8 bits comes
  after it. Overlay surfaces only ever see the 8-bit result, so the obvious
  implementation — add the expanded colour to the expanded pixel — differs on
  168 of 1024 (component, add) pairs and the gate rejects it. Anything else
  added to this list should expect the same shape of problem.

**Not addressed by this work:** the Blue Dragon's building strike and the
Napper's dive/pluck land slightly off their targets under the height system.
That is D3c anchor policy (`kSimObjectTrait_RecordOriginAnchor`), not cull
cover, and wants its own pass against the catalogue and the canonical replay.


Intermediate demonstrations such as D3a intentionally isolate one stage and
may not be a desirable final presentation. Their value is that a regression can
be assigned to one stage before the next stage is introduced. A checkpoint is
complete only when its runner output and interactive demonstration both work;
the following checkpoint does not absorb an unresolved failure.

## Test plan

### Pure/unit tests

- town gate accepts only `$18=0,$19=1..6`;
- picker predicate handles word values and resets on mode change;
- floor projection maps all four source corners and remains invertible;
- billboard foot maps to the same projected point as its ground coordinate;
- pitch=0 projection reduces to the authentic screen mapping;
- fixed/world record range classification at both boundaries;
- OAM range groups are ordered, non-overlapping, and bounded by 128 slots;
- priority-band split retains every emitted component exactly once;
- atlas pack succeeds at the measured maximum and reports overflow safely;
- shadow projection remains finite at camera/light limits;
- feature-mask dependencies resolve deterministically for every bit combination;
- disabling a stage produces its documented bypass rather than a blank or stale
  intermediate target;
- A/B composition reuses one frame serial, atlas serial, and capture set;
- `FrameSlot_Capture` copies every presentation input needed by `sim3d`.

### Replay/golden matrix

Run the shared SIM-system matrix once in a canonical town:

- idle/day-cycle frames;
- movement in four directions and all four map edges;
- development/build animations and lair sealing;
- dialog and every command-menu depth;
- Direct the People / Building Direction enter/move/confirm/cancel;
- every targeted miracle enter/move/confirm/cancel and its resulting animation;
- projectile/effect lifetime in widescreen margins;
- camera shake and fade/transition frames;
- 4:3, corrected widescreen, and raw/fallback views.

Run the object-variation matrix wherever the relevant family naturally appears:

- every enemy family at left/center/right and overlapping the angel;
- Napper flight, dive, ground-pluck, carried-person, and departure states;
- Blue Demon flight plus ground-anchored building lightning;
- people, animals, boats, ground fire, and every other classified map object;
- the angel and arrow through movement, collision, culling, and map margins.

For each of the six towns, retain only lightweight background/palette/layer-role
and final-composite smoke frames unless ROM evidence reveals a town-specific
rendering path. This still catches artwork and backdrop mistakes without
pretending shared mechanics require six separate recordings.

Store four golden products per key frame:

1. authentic composite;
2. isolated semantic layers;
3. pitch-zero enhanced recomposition;
4. tuned enhanced output.

For representative frames, also render the A0-A9 incremental profiles from the
same captured frame. Attribute every changed pixel to the single newly enabled
stage; unexpected changes fail the comparison instead of being accepted as
general visual drift.

### Non-visual correctness

- Diff gameplay WRAM traces for feature off versus on under the same replay.
- Assert VRAM/CGRAM/OAM and save-state payloads are unchanged by presentation.
- Run present-thread stress, resize/fullscreen/live-setting changes, pause, turbo,
  screenshot capture, savestate load, and shutdown under ThreadSanitizer.
- Verify a renderer/shader allocation failure immediately selects a usable flat
  frame and logs once.

## Expected source layout

Prefer the following separation:

```text
src/sim/sim3d.c / src/sim/sim3d.h
    render gate, projection, composition, shadows, transitions

src/sim/sim_render_metadata.c / src/sim/sim_render_metadata.h
    producer metadata, atlas descriptors, pure classification

src/scene3d_math.c / src/scene3d_math.h
    shared projection/quad math if follow-up GEO has not already supplied it

src/actraiser/actraiser_widescreen_sprites.c
    existing semantic OAM leaf instrumentation only

src/actraiser/actraiser_rtl.c
    game-thread capture policy and atlas preparation

src/present.h, src/main.c, src/present.c
    immutable slot boundary, upload, and final branch

third_party/snesrecomp/runner/src/snes/ppu.c/.h
    reusable OAM-range rasterization/capture capability, with no ActRaiser rules
```

Keep ActRaiser record addresses and `$7F:9215` out of the generic PPU. Keep SDL
types and renderer objects out of the ROM/game-state classifier.

## Definition of done

The feature is complete only when:

- all six town maps use one proven implementation;
- ground, world billboards, flat UI, and hardware priority are separated without
  loss at zero tilt;
- actors remain screen-facing and feet-anchored under camera movement;
- shadows are per-object ground silhouettes with no sheet artifact;
- every positional picker uses the authentic flat view on its first active frame;
- confirm/cancel and selected cells are unchanged from the original game;
- unsupported states fail closed to the authentic renderer;
- every visual stage has a runtime A/B gate with a tested, useful bypass and
  centrally resolved dependencies;
- action diorama, Sky Palace, world map, and feature-off output do not regress;
- the threaded present contract and performance gates pass.

Phase 0 evidence, Phase 1 metadata/atlas plumbing, Phase 2 pitch-zero
recomposition, Phase 3 / D3a ground projection, Phase 3 / D3b object
billboards, and Phase 3 / D3c virtual height are complete, which closes
Phase 3. Phase 4 is complete as well: D4a hard shadows, D4b soft shadows, and
D4c rim light.

Phase 5 is under way: D5a ground extension, D5a-3 cull cues, D5a-2 atmospheric
backdrop and D5a-4 dynamic camera have landed. What remains is the picker and
transition contract (D5b), master/live switching and the shipped settings
surface (D5c), and the six-town release matrix (D6). None of the landed
Phase 5 work has been played through or checkpointed — the manifest still has
no entry for any of it, and every existing checkpoint needs the new stage bits
pinned before it can be trusted (`SIM3D_STAGE_ENV` raises on a partial pin,
which is what caught the same problem when soft shadows landed).
