# Spec — WN1: 3D sim-mode inter-town navigation (map `$09`)

**Status:** Steps 1-4d and visual-tuning Steps 5a/5b/5d/5e implemented and
fixture/live-replay-proven 2026-07-27. Step 4e still awaits a complete
movement/action-entry replay; Step 5c remains an explicit research checkpoint.

## 1. Intent

The game has an inter-town world-navigation screen in the simulation family,
reachable from the Sky Palace main menu and from town simulation:

```text
$18 = $00
$19 = $09
```

Today it presents the developed world as an authentic flat Mode-7 map. The goal
is to construct and render this as a host 3D scene using the same world texture,
frame-metadata boundary, scene construction, effects, and presentation
infrastructure as the enhanced simulation-town view.

This view has one deliberate camera restriction:

> **World navigation is always top-down.**

The Sky Palace is authentic top-down sprite art positioned as a fixed screen
centre object while the world moves beneath it. It cannot survive an oblique
ground camera: pitching the map while leaving that composition flat makes the
Palace appear detached from the ground, while pitching the sprite itself turns
top-down art into a card. Free, dynamic, and orbit cameras therefore do not
apply on `$09`. The renderer may still use the 3D scene pipeline for lighting,
weather, depth-aware effects, scaling, and composition, but the resolved camera
looks straight down at the world plane.

Top-down constrains camera **pitch**, not scripted rotation within the world
plane. When the player enters an action level, the authentic `$09` flow removes
the Palace/UI sprites and performs a Mode-7 zoom-and-spin around the selected
location before handing off to action mode. The enhanced view must preserve
that event with a top-down camera whose in-plane orientation and scale follow
the game's animation.

There is no town canvas, town object atlas, or separated town layer stack in
this view. The complete developed world map is the scene's ground.

## 2. Verified fixtures

Two captures pin both sides of the design:

- `runs/20260727-202157/snapshots/snap_00_gf9461.wram.bin` — direct
  act-to-town transition whose live `$7E:C000` shadow has structured action
  garbage in rows 0-79.
- `runs/20260727-204432/snapshots/snap_00_gf782.*` — authentic world
  navigation at `$18=00/$19=09`.
- `runs/20260727-205459/snapshots/snap_00_gf764.*` — midway through the
  `$09` zoom-and-rotation event that enters an action level.

The `$09` fixture establishes:

- `$7E:C000-$FFFF` matches the low byte of Mode-7 VRAM word-for-word: 0
  mismatches over all 16,384 tiles.
- It matches the owned ROM-builder output produced from the direct act-to-town
  fixture: 0 mismatches over all 16,384 tiles.
- It differs from the pristine ROM baseline in 447 tile bytes, all genuine
  current development.
- Fillmore's `$7F:6B18` word is `1` in the `$09` capture and `2` in the
  act-to-town capture, yet their developed maps are identical. `$02:865C`
  merely tests each of the six words for zero/nonzero; development detail lives
  in the six town cell maps, not in the word's numeric value.
- The steady navigation frame has `$92=00`, so Mode-7 is not being rewritten by
  HDMA in this state.

The action-entry fixture establishes:

- `$18=00/$19=09` remains active during the animation. View classification
  cannot assume every `$09` frame is steady navigation.
- The developed `$7E:C000-$FFFF` tilemap is unchanged and still matches the
  owned-builder result byte-for-byte. This is a camera/presentation event, not
  another map build.
- World focus is `$0300/$0302 = $0348/$0238`; authentic scroll is
  `$22/$24 = $02C8/$01C8`. Their differences remain exactly 128 and 112.
- The current and staged matrices both read
  `[A,B,C,D] = [$00BC,$026C,$FD93,$00BC]`.
- `$0314=$0034`, corresponding to the observed in-plane rotation, while
  `$0316=$0516` is animating toward `$0318=$040A`.
- `$92=00` here too. The rotation is one global matrix, not an HDMA
  per-scanline warp.
- Every OAM entry is hidden (`00 E0 00 E0`). The Palace marker and location
  label deliberately disappear before the zoom-and-spin begins.

These fixtures are the byte-exact acceptance oracles for Step 1.

## 3. Delivery sequence

### Step 1 — replace the transactional ROM call with a pure HLE world builder

**Implemented 2026-07-27.** `SimWorldMap_ComposeDeveloped` is the state-free
host implementation, while `SimWorldMap_BuildIfNeeded` coordinates town and
`$09` consumers. The ROM-call bridge remains only behind the diagnostic
`AR_WORLDMAP_HLE_COMPARE=1` differential switch.

This is the prerequisite shared by the simulation-town underlay and the `$09`
3D scene.

The game's full map presentation routine is `$02:B475`:

1. copy/decompress the 16 KiB base tilemap into `$7E:C000`;
2. `JSL $02:865C` to stamp current development;
3. upload `$7E:C000-$FFFF` through `$2118` to Mode-7 VRAM.

Only phase 2 is dynamic map construction. It is separable, bounded, and
yield-free. The former host bridge invoked its recompiled CPU function
transactionally; Step 1 replaced that production path with the pure HLE below.

Implement a pure host equivalent with explicit immutable ROM data and explicit
simulation inputs:

```c
bool SimWorldMap_ComposeDeveloped(
    uint8_t out[16384],
    const uint8_t baseline[16384],
    const uint8_t town_maps[6][1024],
    const uint16_t town_enabled[6],
    uint8_t world_flags,
    const SimWorldMapRomTables *tables);
```

Immutable ROM inputs:

- base tilemap: `$06:B341`, 16 KiB;
- ordinary cell-to-world-tile translation: `$02:8000`, 256 bytes;
- special `$E3-$EF` 2x2 expansions: `$02:8100`, 13 records × 4 bytes;
- town destination offsets: `$02:87A5`, six words. These are the already-pinned
  town origins used by `SimWorldMap_OriginForTown`.

Mutable simulation inputs:

- six quadrant-paged 32x32 town cell maps at `$7F:2000-$37FF`;
- six enable words at `$7F:6B18-$6B23`, interpreted only as zero/nonzero;
- `$7F:9101` bit 0, which controls the small 8x8 clearing at map offset `$0660`.

The HLE must:

1. copy the pristine base to `out`;
2. apply the flag-controlled 8x8 clearing exactly;
3. for each enabled town, perform `$02:86D1`'s quadrant-paged 32x32 translation
   pass, preserving the base wherever the translation table returns zero;
4. perform `$02:8726`'s special `$E3-$EF` 2x2 expansion pass;
5. mutate no CPU, WRAM, PPU, or other emulator state.

The current recompiled `$02:865C` transaction remains as an opt-in differential
oracle. Required parity cases:

- both captured fixtures above;
- all six towns individually enabled and disabled;
- both values of `$7F:9101` bit 0;
- ordinary cells, zero translations, and every special value `$E3-$EF`;
- repeated composition into dirty output memory.

Production no longer performs the synthetic CPU call, fake `$19/$AA`
preconditions, 128 KiB WRAM snapshot, or restore. The ROM-call path remains
behind the diagnostic-only comparison switch.

The build coordinator must serve both consumers:

- build on every simulation-town entry and when construction inputs change;
- build explicitly on `$09` entry and when construction inputs change;
- publish one shared complete tilemap through `SimWorldMap`;
- never observe or adopt `$7E:C000`.

`SimWorldMap_BuildIfNeeded()` now treats town and `$09` as distinct consumers,
so transition into either forces a build even if the construction inputs are
unchanged. A navigation view cannot inherit whichever host texture happened to
be published previously.

### Step 2 — add the world-navigation frame and camera contract

**Implemented 2026-07-27.** `AR_SIM3D_WORLD_NAV` is an independent
off-by-default setting. `kSimView_WorldNavigation` is selected only for
`$18/$19=00/09` with that setting enabled and a complete HLE map available;
otherwise the authentic path remains selected. `SimFrameData.town` stays zero,
town record/atlas metadata is cleared, and the immutable
`SimWorldNavigationFrame` carries every field below. Scene-specific
menu/forced-blank eligibility fails closed at the Steps 3-4 presentation gate;
partial master-brightness fades are owned by the host composition. Eligibility
is not inferred from nonzero rotation or an in-progress zoom.

Both supplied WRAM fixtures pass the exact field assertions. A headless live
replay entered `$09` directly at gf380 with focus `(768,512)`, scroll
`(640,400)`, current/staged matrix `[512,0,0,512]`, and zoom
`$040A/$040A`. The trace contained zero stale town sources or objects.

Add `kSimView_WorldNavigation`, selected only when:

- `$18=00/$19=09`;
- the enhanced world-navigation setting is enabled;
- no menu, forced blank, or other unsupported authentic-only substate owns the
  frame. Partial INIDISP brightness is explicitly supported.

The supported view includes both steady navigation and the action-entry
zoom-and-rotation event while `$18/$19` remains `00/09`. Do not fall back merely
because `$0314 != 0` or `$0316 != $0318`; those values are the animation inputs.

`SimFrameData.town` remains zero. Every downstream site must use `view`, not
`town != 0`, to decide whether a 3D scene exists.

Snapshot the navigation state into immutable frame metadata on the game thread.
The `$09` capture and ROM code identify the canonical fields:

| WRAM | steady gf782 | action-entry gf764 | Meaning |
|---|---:|---:|---|
| `$0300` | `$0300` | `$0348` | world focus X in source pixels |
| `$0302` | `$0200` | `$0238` | world focus Y in source pixels |
| `$0022` | `$0280` | `$02C8` | authentic Mode-7 horizontal scroll |
| `$0024` | `$0190` | `$01C8` | authentic Mode-7 vertical scroll |
| `$0304-$030A` | `$0200,0,0,$0200` | `$00BC,$026C,$FD93,$00BC` | current A/B/C/D matrix |
| `$030C-$0312` | `$0200,0,0,$0200` | `$00BC,$026C,$FD93,$00BC` | staged next matrix |
| `$0314` | `0` | `$0034` | scripted in-plane rotation |
| `$0316` | `$040A` | `$0516` | current zoom state |
| `$0318` | `$040A` | `$040A` | target zoom state |

At `$02:8213`, directional input changes `$0300/$0302` and `$22/$24` together
by two source pixels. In the fixture:

```text
$0300 - $22 = 128
$0302 - $24 = 112
```

That is exactly the authentic 256x224 half-screen. `$0300/$0302` is therefore
the canonical 3D world focus; it does not need to be inferred from end-of-frame
PPU state. `$02:8384` uploads the WRAM matrix and centre directly to
`$211B-$2120`.

The 3D camera contract is:

- target/focus comes from `$0300/$0302`;
- the camera pitch is always perpendicular to the world plane;
- during steady navigation, in-plane orientation is zero and authentic zoom
  controls top-down scale or distance;
- during the action-entry event, the current A/B/C/D matrix drives in-plane
  rotation and scale around the same `$0300/$0302` focus. `$0314/$0316` are
  useful semantic state, but the matrix is the exact transform the game
  presents;
- host free-camera input is ignored on this view, leaving the game's
  navigation controls authoritative;
- when `$18/$19` leaves `00/09`, the action renderer owns the next frame.

Manual yaw remains disabled. The scripted in-plane spin is not a user camera
mode and does not relax the forced top-down rule.

### Step 3 — construct the full-world scene

**Implemented 2026-07-27.** `SimWorldNavigationScene_Build` converts the
captured current A/B/C/D matrix into a source-to-authentic-screen affine
transform, binds the complete developed-map serial as one 1024x1024 texture,
and publishes one four-corner plane covering `[0,128) x [0,128)` through
`SimFrameData.world_navigation_scene`. The steady and animated fixtures both
round-trip through the captured Mode-7 transform; a zero/singular matrix fails
closed to `kSimView_AuthenticFallback`.

This step does not yet select the host presentation path. Step 4 must first
capture and separate the authentic Palace and location UI composition, so the
master switch cannot expose a ground-only half-frame. Until that lands,
`kSimView_WorldNavigation` deliberately resolves zero effective presentation
features and `PresentComposite` keeps authentic Mode 7.

Construct a scene through the same immutable-frame and render-thread boundary
as simulation-town 3D, but with a world-navigation-specific layer set:

1. one full 1024x1024 developed world texture from `SimWorldMap`;
2. one world ground plane covering tile coordinates `[0,128) × [0,128)`;
3. optional lighting, weather, colour treatment, and depth-aware effects;
4. a reserved layer for the authentic Sky Palace marker and location UI
   composition, populated by Step 4;
5. no town ground canvas, object atlas, town OAM records, or separated BG
   planes.

Do not route this through the town underlay's “extension around a visible
window” assumptions. World navigation has no 32x32 town window and no
`kSimUnderlayMarginPixels`: the full map is the primary plane, not distant
fallback terrain.

Because the view is perpendicular to a flat plane, the tilted-underlay
near-plane and horizon problems do not apply. A simple full-plane mesh is
sufficient unless a later effect genuinely requires denser subdivision. Any
reuse of the current 64x48 underlay mesh must still re-derive its static array
bounds rather than silently exceeding them.

### Step 4 — render top-down and preserve navigation composition

Deliver Step 4 as five explicit checkpoints:

#### Step 4a — full-world ground presentation

**Implemented 2026-07-27.**

- Upload the Step-3 1024x1024 developed texture through the existing
  render-thread cache.
- Draw the one four-corner plane with
  `world_navigation_scene.source_to_screen`.
- Preserve the authentic frame whenever the texture, affine transform, or
  renderer resource is unavailable.

#### Step 4b — Palace and location-UI ownership

**Implemented 2026-07-27.**

- Capture navigation OAM through the immutable frame boundary.
- Identify and rasterize the Palace and label/frame as separate compositions;
  do not route either through simulation-town record classification.
- Draw the Palace after world-space effects and the label/frame in screen
  space after the Palace.
- Treat the action-entry frame's all-hidden OAM as a valid empty composition,
  not as a capture failure.

#### Step 4c — navigation lighting and colour

**Implemented 2026-07-27.**

- Apply whole-scene colour treatment to the developed ground without changing
  the host-owned tilemap.
- Reuse compatible light direction/elevation and strength controls, but do not
  require the Simulation town 3D master.
- Keep town-only billboard rim lights and actor shadows out of `$09`; they have
  no valid caster until a real 3D Palace asset exists.

#### Step 4d — whole-world clouds and weather

**Implemented 2026-07-27.**

- Add a navigation-specific, world-anchored cloud/weather layer between the
  lit ground and the Palace.
- Reuse the town renderer's proven procedural cloud field. Upload a padded
  2x2 repetition for navigation so each drifting layer is one affine quad:
  this keeps the field seamless without relying on renderer texture-wrap
  support or joining separately rasterized tiles.
- Give navigation cloud cover its own off-by-default enable switch while
  sharing compatible density, altitude, drift, softness, and lighting tuning.
- Do not reuse the town cloud shroud's sprite-window hole, cull coverage,
  underlay margin, or focus falloff. The full world is intentional content.
- During action entry, transform world-anchored weather with the same scripted
  rotation/zoom as the ground; do not synthesize hidden Palace/UI art.

#### Step 4e — transitions and replay gate

**Partially verified 2026-07-27.** The steady gf782 OAM fixture and a live
Fillmore replay classify/rasterize the 20-sprite label plus 9-sprite Palace.
The gf764 mid-animation fixture classifies all-hidden OAM as a valid empty
composition, and its rotated affine matrix passes the projection oracle. The
current replay enters `$09` but does not perform the action-entry event or
exercise four-direction movement, so the full per-frame presentation gate
still needs a user recording. The same replay now proves continuous INIDISP
ownership: enhanced composition begins at brightness 0 on gf381, ramps through
1-15 on gf383-397, remains enhanced while fading 14-0 on gf436-450, and hands
off only after the fully black gf451 frame.

- Validate movement in all four directions, every steady zoom, location-label
  changes, and the complete action-entry animation.
- Keep partial-brightness fade frames enhanced. Composite backdrop, ground,
  lighting, haze, and weather at full intensity, then apply INIDISP master
  brightness once; Palace/UI captures already contain that same PPU brightness.
- Fall back for forced blank, unclassified OAM, or unsupported navigation
  substates. Forced blank is fully black, so this handoff is invisible.
- `AR_SIM3D_WORLD_NAV=1` now selects the host presentation. Unsupported
  composition/layout states retain the authentic Mode-7 fail-closed path.

The raw `$09` OAM fixture has 29 active sprites:

- 20 compose the `FILLMORE` label and its frame (slots 0-19);
- 9 compose the Sky Palace marker.

The Palace occupies slots 20-28; slot 29 is already hidden. The earlier
30/21+9 count was an off-by-one transcription and is superseded by the raw
512-byte OAM fixture.

The label/frame is screen-space UI and remains flat. The Palace marker is also
authentic top-down art at the screen centre while the map scrolls underneath.
The forced top-down ground camera lets that composition remain visually
coherent without inventing an oblique 3D model.

During the action-entry event, all of those entries are already hidden by the
game. Render only the developed world plane, transformed by the scripted
top-down matrix, plus fades/effects that remain active. Do not synthesize a
Palace marker during the spin.

The render order is:

1. full-world ground and its scene effects;
2. world-space/top-down effects that belong beneath the Palace;
3. INIDISP master-brightness fade over those full-intensity host layers;
4. authentic Palace OAM composition, already brightness-adjusted by the PPU
   rasterizer;
5. location label and other screen-space navigation UI, adjusted likewise.

For action entry, steps 3-4 naturally contribute nothing because their
authentic OAM entries are hidden. The same scene therefore covers steady
navigation and the transition without a second camera model.

Do not pass the navigation OAM through simulation-town object classification:
its records have different semantics. Initially preserve the authentic OAM
composition as a unit. A later replacement with a true 3D Palace asset is a
separate feature and would be the only reason to relax the forced top-down
camera.

The town cloud shroud is not used to hide actor-free territory here—the full
world is intentional content. Navigation's independent off-by-default weather
pass now supplies the weaker whole-map cloud treatment without inheriting the
town underlay shroud's hole or cull geometry.

### Step 5 — world-navigation visual tuning

**Implemented 2026-07-27, except the Step 5c research checkpoint.**

#### Step 5a — atmospheric backdrop beyond the finite world

- Draw the same authored horizon-to-zenith gradient used by Simulation town
  3D before the world plane. Navigation has no perspective horizon, so use the
  existing synthetic horizon control rather than inventing one from the
  top-down affine matrix.
- The backdrop must cover every pixel exposed past the finite 1024x1024 map
  during wide output, far zoom, and the action-entry spin.
- Reuse the shared backdrop strength and horizon tuning without requiring the
  Simulation town 3D master.

#### Step 5b — active-location haze

- Capture `$0341` as the authoritative active world location. `$01:B6CA`
  writes it while testing the focus against the seven 256x256 source regions
  at `$01:B73C`; it is the same selection consumed by the location label and
  destination handoff.
- Keep that region sharp and fully lit. Blend the existing downsampled
  world-map mip and atmospheric dimming over the other regions with a soft
  world-space boundary.
- Cover locations 1-7: Fillmore, Bloodpool, Kasandora, Aitos, Marahna,
  Northwall, and Death Heim. `$01:B6CA` explicitly clears `$0341` before its
  scan; zero means the Palace is outside every town border. In that state the
  entire world stays dimmed—there is no clear-region cutout.
- Keep the mask attached to world source coordinates so movement and the
  scripted action-entry rotation/zoom transform it exactly with the map.

#### Step 5c — high-fidelity 2048x2048 world composition research

- Investigate a 2x world canvas. One current world tile represents one
  simulation cell, so a 2048x2048 map gives every cell its native 16x16 town
  footprint and every 32x32 town region a 512x512 replacement window.
- Determine how to reconstruct all six native town terrains without depending
  on the currently resident town VRAM: identify each town's ROM tileset,
  palette, metatile translation, and any animated/development-dependent cells.
- Define the seam policy between native town windows and the 2x-scaled
  world-map terrain, plus mip/downsample policy at far navigation zoom.
- Do not increase the production texture size until all six towns can be
  rebuilt deterministically from explicit inputs. This is research, not a
  prerequisite for Steps 5a, 5b, or 5d.

#### Step 5d — zoom-relative cloud ceiling

- Treat `$0316`'s three steady values—near `$0206`, middle `$040A`, and far
  `$0562`—as the navigation camera-height axis.
- Use the shared cloud-altitude setting as a world-space ceiling. At the
  closest zoom the default camera is below that ceiling and cloud bodies are
  hidden; zooming outward crosses it smoothly and reveals the clouds.
- Continue drawing cloud shadows on the ground while below the cloud deck:
  the bodies are above the camera, but still block the directional light.
- Apply the visibility factor continuously during zoom and action-entry
  animation; never switch clouds abruptly at one of the three steady values.

#### Step 5e — animated world water

- The static world CHR at ROM `$0E:8000` is not the complete presentation.
  `$02:AF86` replaces Mode-7 tiles `$00` and `$AA` from four 64-byte frames at
  ROM `$0A:B000/$B040/$B080/$B0C0`.
- Preserve that animation in the owned texture without observing PPU VRAM.
  Copy the selected immutable ROM frame into both host tile slots, dirty only
  tilemap cells that reference `$00/$AA`, and advance the texture serial only
  when the visible phase changes.
- During `$09`, synchronize to the source address retained at `$7E:00D7` after
  the authentic DMA. Consecutive captures pin the order and cadence:
  `$B000 -> $B040 -> $B080 -> $B0C0`, eight game frames per phase.
- The simulation-town outer underlay is the same owned world texture, so it
  must not freeze after leaving `$09` or after a direct act-to-town transition.
  In town mode continue the pinned cycle from the global game-frame clock;
  do not read `$00D7`, because that descriptor then belongs to the town's
  unrelated WRAM-backed tile animation.

## 4. Existing reusable pieces

- `kActRaiserNonActionMap_WorldMap = 0x09` already identifies the scene.
- `$09` is already fully wide with no ordinary UI layers in
  `src/actraiser/actraiser_rtl.c`.
- `SimWorldMap` already stores and bakes the complete 128x128 map, not a town
  chunk.
- `SimWorldMap_Downsample` provides a safe CPU-side blur mip.
- The simulation-town pipeline already provides the immutable `SimFrameData`
  handoff, render-thread ownership, camera resolution, world-map texture cache,
  effects, and final composition stages.

The obsolete live-shadow design is not reusable. `SimWorldMap_Refresh`,
volatile-row adoption, fingerprints, coherence policy, staleness fallback, and
`worldmap_top_rows` have been removed and must not reappear in this work.

## 5. Settings and fallback

The off-by-default enhanced world-navigation master owns two subordinate
effect switches. When the master is disabled:

- `$09` remains the authentic flat Mode-7 scene;
- the rendered output must remain byte-identical;
- the HLE may keep the host-owned `SimWorldMap` current, but it must not alter
  the authentic CPU, WRAM, PPU, OAM, or VRAM path.

`AR_SIM3D_WORLD_NAV` is an independent master, not a child of
`AR_SIM3D`. Enabling world navigation must therefore work while Simulation
town 3D is off. Shared implementation and compatible numeric treatment
(lighting, weather, and colour controls where their semantics genuinely
match) do not imply two-master gating. Town camera controls, the town canvas,
object billboards/atlas, the town cull shape/darkening, and the underlay cloud
shroud remain town-only. The shared backdrop and local-area haze toggles also
gate their compatible navigation passes, without requiring the town master.
The full-world plane and the game's scripted matrix are mandatory parts of
`$09`, not optional town stages.

The current town cloud **shroud geometry/policy** is specifically not reused:
it encodes the sprite-drawable town window and would invent a false boundary
on the full world. The procedural cloud texel generator is reused, with the
padded navigation texture described in Step 4d. `AR_SIM3D_WORLD_NAV_LIGHTING`
defaults on and applies only the compatible top-down colour/cloud-light
treatment. `AR_SIM3D_WORLD_NAV_CLOUDS` defaults off and enables a separate
whole-map weather pass; it shares density, altitude, drift, light
direction/elevation, shadow darkness, and shadow softness tuning without
sharing the town shroud's geometry or cull policy.

When enabled but the HLE texture or scene capture is unavailable, fall back for
that frame to authentic Mode 7. Never render a partially configured 3D view.

## 6. Testing

### Step 1: HLE construction

- Byte-compare all 16,384 output bytes against the current ROM-call oracle.
- Assert 447 differences from the pristine baseline for both captured states.
- Assert exact equality between the `$09` authentic shadow and the direct
  act-to-town HLE output.
- Assert no emulator-visible state changes.
- Verify act-to-town without a world-map interlude still publishes the
  developed map.
- Verify direct entry to `$09` publishes the developed map even if no town view
  has rendered during the current process.

### Steps 2-4: scene and rendering

- Snapshot `$0300/$0302`, zoom, and relevant transition fields into
  `SimFrameData`; the render thread must not read live WRAM or `g_ppu`.
- Verify the gf782 fixture resolves world focus `(768,512)` and centres
  Fillmore.
- Verify the gf764 action-entry fixture resolves focus `(840,568)`, matrix
  `[$00BC,$026C,$FD93,$00BC]`, rotation `$0034`, current/target zoom
  `$0516/$040A`, and no active navigation OAM.
- Record/replay movement in all four directions and each authentic zoom state;
  ensure the host ground and authentic Palace remain locked with no one-frame
  drift.
- Record/replay the complete action-entry event; compare the host map's
  per-frame centre, rotation, and scale against the authentic Mode-7 transform,
  and verify the handoff occurs on the first non-`$09` frame.
- Confirm the location label changes normally and remains screen-aligned.
- Verify every partial-brightness transition frame remains
  `world_navigation`, with no full-brightness effects gap at either endpoint;
  forced blank must remain an indistinguishable black fallback.
- Keep the feature off-by-default and run a PPM byte-identity gate on the
  authentic path.

### Step 5: visual tuning

- Assert `$0341=1` selects Fillmore `(640,384,256,256)` and `$0341=7`
  selects Death Heim `(640,0,256,256)` in world-source pixels.
- Assert zero and out-of-range locations leave the scene valid, publish no
  clear-region cutout, and therefore keep the complete world hazed.
- Assert the default cloud deck is hidden at near zoom `$0206`, visible at
  middle/far `$040A/$0562`, and crosses continuously between them.
- Assert all four world-water ROM frames replace both tile `$00` and tile
  `$AA`, reject misaligned/out-of-range sources, and bump the texture serial
  only when a valid phase changes.
- Replay consecutive navigation frames and assert serial changes at
  gf381/385/393/401/409, matching the retained DMA sources. Remain in a town
  for at least 32 frames and confirm the outer world underlay completes the
  same four-frame cycle without a preceding `$09` visit.
- Replay at far zoom and through action entry to inspect backdrop coverage,
  haze attachment during rotation, and cloud-body continuity by eye.

## 7. Risks

- **WN-R1 — `town` versus `view` gates.** A missed `sim.town != 0` condition can
  produce a half-configured scene. Audit every scene, input, picker, and
  presentation gate.
- **WN-R2 — HLE semantic drift.** The algorithm is simple but quadrant paging,
  zero-translation preservation, and the special 2x2 pass are easy to transpose.
  Differential parity with `$02:865C` is mandatory before removing the oracle.
- **WN-R3 — `$09` build timing.** The six town maps must be read after the game
  has initialized/restored simulation state and before immutable frame capture.
- **WN-R4 — Palace/UI separation.** Treating all 30 OAM entries as one class
  either lights the label like world art or detaches the Palace from the map.
  Preserve their two observed compositions explicitly.
- **WN-R5 — animation-frame timing.** `$0304-$030A` is uploaded before
  `$030C-$0312` is promoted for the next frame. Both pairs match in the captured
  mid-animation frame, but a replay is needed to prove whether they ever differ
  and pin which snapshot belongs to the displayed frame.
- **WN-R6 — forced camera ownership.** Host orbit/zoom input must not steal
  controls from the game's navigation UI.
- **WN-R7 — inert fallback.** When disabled or unavailable, authentic Mode 7
  must remain byte-identical.

The previous HDMA-stability risk is substantially resolved: the `$09` fixture
has `$92=00`, the focus is explicit in WRAM, and `$02:8384` uploads it directly.
Dynamic replay still verifies temporal alignment, but end-of-frame inference
from a possibly warped PPU matrix is no longer the design.

## 8. Effort

- **Step 1 — pure HLE builder and differential tests:** complete.
- **Step 2 — view classification and immutable camera metadata:** complete.
- **Step 3 — full-world top-down scene:** complete.
- **Step 4 — Palace/UI composition, transitions, tuning, and replay tests:**
  implemented except the full movement/action-entry replay.
- **Step 5a/5b/5d/5e — backdrop, location haze, cloud ceiling, animated
  world water:** complete.
- **Step 5c — 2048x2048 high-fidelity composition:** research pending.

## 9. Remaining audit questions

1. Record one `$09` replay covering movement, every available steady zoom
   state, location-label changes, and the complete action-entry zoom-and-spin.
2. Pin the runtime OAM boundary between Palace marker and location UI instead
   of relying solely on the gf782 entry grouping.
3. Verify the earliest possible `$09` entry, including a new/empty save, has all
   six `$7F:2000-$37FF` town maps initialized before the HLE build point.
4. Identify deterministic ROM inputs and seam/mip policy for Step 5c's
   2048x2048 high-fidelity world composition.

The former `town`-gate audit is resolved: generic diagnostics and view
transition tracking use `view`; the remaining `town` checks are deliberately
town-only resources (object-atlas upload, full-town canvas build/draw, and the
free/dynamic camera).
