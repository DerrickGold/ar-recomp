# Particle and lighting effect hook investigation

Status: **static lifecycle, animation, geometry, and anchor mapping complete** for the four
action magics, the five simulation miracles, and the four simulation enemy classes. Simulation
Lightning, Blue Dragon lightning, and palette-selected ground fire are implemented; the blue
post-Lightning variant is replay-validated and the scripted red variant is live-capture-validated.
The distinct scripted burning-house family is mapped from run `20260803-130945` and implemented
with exact lifecycle, lighting, particle, ground-contact, and source-cadence handling.
Palette-1 Red Demon fire is implemented with ROM-semantic unit coverage; a live
Red Demon capture remains its visual acceptance gate. Magical Fire now has a host-side captured
lifecycle plus portable lighting/particle implementation for flat and diorama presentation; its
new visual pass and the other three action spells still need the short live acceptance sweep in
[Validation remaining](#validation-remaining).

The lifecycle mapping in this document is deliberately implementation-neutral. It identifies the
authentic 60 Hz game state that an enhanced renderer can observe without changing damage,
gameplay timing, or object allocation. The one explicit source-art exception is the
signature-checked burning-house frame hold documented below; it changes only that looping sprite's
visual cadence, not its actor-owned lifetime. This is the companion to [SEAMS.md](SEAMS.md),
[rendering-engine.md](rendering-engine.md), and [sim-object-catalog.md](sim-object-catalog.md).

## Conclusions that affect the visual design

1. **Magical Fire is not one fireball.** It is four cloned action objects, all created at the
   player, with every horizontal/vertical flip combination. Together they make a four-way,
   screen-filling sweep. Particle emitters must attach to the four object instances.
2. **Magical Light is not one ray.** It is a stationary centre flare plus two mirrored,
   16-by-224-pixel beam columns. The columns separate horizontally late in the spell. A light
   attached only to the player or cast event cannot follow the authored effect.
3. **Lightning and rain miracles are two co-located records.** One record owns the cloud and
   cloud-to-ground weather composition; the second owns the separate ground-shadow ellipse.
   The bolt/rain is already part of the cloud composition and must not be given a different
   anchor.
4. **Sunlight is a screen colour-math effect, not a sprite.** Its exact phase is a 140-tick
   counter and the ROM ramps fixed-colour red addition on BG1. It needs a scene light, not an
   object emitter.
5. **Blue Dragon lightning is not the miracle driver.** Class `$12`, state 6 is its own
   33-frame strike lifecycle. Lightning frames alternate with ordinary dragon frames every
   tick on the same record after that record is moved to the target plane.
6. **Composition address does not determine ground-fire colour.** Red Demon charge fire uses
   `$E340/$E35A/$E383` on the demon's 24-pixel flight plane. Ground fire uses the runtime-built
   `$E6CA/$E6D0/$E6D6` records at map height for both the scripted red blaze and post-Lightning
   blue fire. Their emitted OAM parts select palette 1 and 2 respectively while live CGRAM contains
   both colour ramps, so the producer must capture the selected OBJ palette.
7. **Burning houses are a third fire system.** Run `20260803-130945`, frame 19,950 contains three
   world records at `$0F0C/$0F32/$0F58`, all with packed identity `$0A01` and current composition
   `$DD33`. Their shared script `$01:A838` cycles `$DD2D/$DD33/$DD39`; each composition is one
   palette-1 16x16 part at local `(0,0)`. The record origin is the sprite top-left, so ground
   illumination semantics attach to contact `(8,16)`. Run `20260803-134746` shows that drawing the
   radial light and particle source at that same point biases both beneath the opaque flame;
   presentation therefore lifts both four authentic pixels to local `(8,12)`. This family is
   unrelated to `$E6CA/$E6D0/$E6D6`.
   The ROM authors every source frame at one 60 Hz tick, producing a 20-cycle-per-second flicker;
   the enhanced baseline deliberately holds each frame for four logic ticks (15 source fps).

These findings leave no ROM-decompilation blocker to choosing particle and light styles. They
also mean the first implementation should expose per-instance metadata, rather than a single
`magic_active` or `miracle_active` flag.

## Minimum presentation contract

Capture the following once per authentic 60 Hz logic tick. Interpolated presentation frames may
render between samples, but must never advance these lifecycles themselves.

| Field | Action mode source | Simulation source | Reason |
|---|---|---|---|
| instance key | action slot address | world-record address | clones and co-located records need stable, separate emitters |
| semantic kind | cast controller `$0860+$38` | miracle `$7F:90EB`, or record class/state | chooses an effect family without guessing from tiles |
| active/start/end | slot status plus controller lifetime | effect-driver flags plus actor/class state | creates and retires host emitters exactly once |
| world anchor | slot `+02/+04` | record `+0A/+0C` | authoritative position after game movement |
| current artwork | `+20` composition, `+22` visual | record `+08` composition | gates intensity to the frames that visibly contain fire/bolt/beam |
| transform | slot `+28` flip bits; current `+0A/+0C/+0E/+10` extents | authored part offsets/attributes | keeps mirrored and full-height effects aligned |
| camera | `$22/$24` | `$22/$24` | projects the world anchor to the authentic viewport |

Use slot/record lifetime as the instance lifetime, and composition identity as the per-frame
presentation phase. Sound requests are useful accents, but are neither unique instance IDs nor
reliable end signals.

### Action coordinate contract

The action animation interpreter is `$00:8E2F`; OAM emission is `$00:8D68`.

- Slot `+02/+04` is the object's world-space hot point. All player-centred spells receive the
  current player point through `$00:A061` from DP `$80/$82`.
- The current composition supplies four culling extents. `$8E2F` mirrors them according to slot
  `+28` and writes left/top/right/bottom to `+0A/+0C/+0E/+10`.
- Each seven-byte OAM part contains size, normal/flipped X bytes, normal/flipped Y bytes, tile,
  and attributes. The authentic local position is the selected unsigned part coordinate minus
  the selected left/top extent. Do not sign-extend the stored part coordinate first.
- The hot point projects approximately as `(slot.x - $22, slot.y - $24)`; use the same OAM bias
  as `$8D68` when pixel-exact sprite alignment matters. For a host light centred on the hot point,
  no composition-derived union or foot anchor is needed.
- `tools/action_magic_catalog.py` decodes both animation banks, all relevant state steps, both
  coordinate choices, culling extents, anchor-relative geometry, tile IDs, timing, and velocity.

### Magical Fire implementation slice (2026-08-03)

- `action_effects.c` positively identifies controller `$0860+$38 == 1` and exact cohort slots
  `$06A0/$06E0/$0720/$0760`. Each slot must retain animation table `$07:C000`, a Fire visual
  `5..43`, a nonzero composition, and its expected flip pair. Any ambiguous controller, bank,
  visual, composition, or transform fails closed instead of decorating a reused action slot.
- `FrameSlot_Capture` publishes one immutable instance per active slot with world hot point,
  velocity, unsigned culling extents, animation state/index, current composition/visual, flip flags,
  semantic geometry, authentic OBJ priority, actor/pulse generations, and separate actor/phase/pulse
  tick clocks. State 2 versus 3 resolves Fire visual 12's phase ambiguity. A paused host redraw
  passes zero elapsed ticks, so emitter clocks freeze with the game rather than advancing at display
  refresh rate. The observer is explicit state and is reset on every savestate load.
- `action_effect_render.c` owns spell style and geometry extraction; `present.c` only projects and
  submits its bounded batch. The renderer centres each glow on the published rectangle, then emits
  quadrant-aware embers from all four mirrored actors. Its pulse is an integer triangle wave rather
  than `libm` sine, so authored-tick output is repeatable across platforms. Unknown effect, phase,
  geometry, layer, or priority values fail closed.
- Flat mode maps through the resolved presentation viewport. Diorama mode consumes the compositor's
  exact resolved matrix, mesh dimensions, BG1 interpolation window, and the source OBJ priority
  plane's authored depth/rake/bow, keeping the overlay registered without reading live WRAM,
  duplicating camera auto-fit, or guessing a parallel depth. Enhanced light/embers intentionally
  remain a world overlay above the composed world and below HUD/HD UI; source priority is retained
  for projection now and priority-aware masking later.
- Both modes share the same capability-checked SDL additive/untextured-geometry path as simulation
  effects. No platform shader is required. Settings expose independent `action_effect_lighting`
  and `action_effect_particles` switches (`AR_ACTION_EFFECT_LIGHTING` /
  `AR_ACTION_EFFECT_PARTICLES`), both on by default.

### Simulation coordinate contract

World records are 38 bytes at `$0A00+`, rendered by `$01:ACD9` and `$01:ADAD/$AE6F`.

- Record `+0A/+0C` is world X/Y and record `+08` is the current five-byte-part composition.
  Project with the town camera `$22/$24` exactly as the original emitter does.
- Miracle clouds, their bolt/rain, and the separate shadow share one record origin. Keep the
  cloud-to-ground composition on the map plane; lifting it like a flying actor disconnects the
  strike from the terrain.
- The target pixels `$7F:96EA/$96EC` and derived aimed cells `$7F:90E1/$90E5` describe the
  gameplay target. They are not a substitute for the moving effect actor's current coordinates.
- Fixed/overlay simulation records at `$06A0-$09FF` are a distinct, screen-space format. Do not
  parse them as world records or as action-mode `$40`-byte slots.

## Action-mode magic lifecycle and artwork

### Shared cast lifecycle

The player update `$00:9832` tests held A/X at `$00:9843`. `$00:9DE1` accepts a cast only when no
cast is active, a spell is equipped, the player is not hurt/invulnerable, and working MP `$21` is
non-zero. Acceptance decrements MP, marks the player casting, disables input, and reaches the
semantic start below:

| Address | Meaning for an enhanced effect |
|---|---|
| `$00:9E89` | creates controller slot `$0860`; copies selected spell ID `$02AC` to controller `+38`; increments the player reference count |
| `$00:9F13` | dispatches ID 1/2/3/4 to `$9F25/$9F71/$9FBB/$9FFA` |
| `$00:A035-$A053` | yields while any spell cohort slot `$06A0-$0820` remains active |
| `$00:A054-$A060` | clears the player reference and frees the controller; semantic cast end |
| `$00:9EAB-$9F10` | player waits for that reference to clear, then restores input/cast state |

Controller `+38` is the stable spell kind for the cast. The individual cohort slots remain the
correct emitter instances. The controller ends only after every cohort slot reports free, so it
is a safe outer lifetime even for the staggered Stardust actors.

### Spell catalogue

Authored tick totals below sum `delay + 1` for animation entries and exclude small setup/free
edges in the handlers.

| ID | ROM setup and instances | Animation/geometry | Recommended hook |
|---:|---|---|---|
| 1 Magical Fire | `$9F25`; slots `$06A0/$06E0/$0720/$0760`, flip masks `0/$4000/$8000/$C000`, all born at player point | bank `$07:C000`; state 2 = 9 ticks, local union `x -39..8, y -8..23`; state 3 = 32 ticks, `x -44..8, y -29..30`, repeated twice. Total authored path 73 ticks per object | cast start from controller ID 1; one emitter per active slot; use current `+20/+22` and flip bits, not a single player-centred fireball |
| 2 Magical Stardust | `$9F71`; same four slots, staggered by 0/20/40/60 ticks; each actor launches four times | bank `$07:C000`; entry visual is 16x16 for 1 tick with velocity `(-8,+8)` before collision/motion handling; burst state is 23 ticks, visuals 1-4, grows to 32x32 | track four actor slots across all four launches (16 launch/burst opportunities); launch position is chosen at the viewport top/right edge, not retained at the player |
| 3 Magical Aura | `$9FBB`; four player-born slots with all flip combinations | bank `$07:C800`; state 3 = 116 ticks, 60 entries alternating visuals 10/11, each a four-part 32x32 orb; normal path velocity sum `(-76,-71)` before flip mirroring | one moving emitter per active slot; follow slot `+02/+04` every tick rather than treating this as a stationary halo |
| 4 Magical Light | `$9FFA`; centre `$07A0`, side columns `$07E0/$0820`; right/left side is horizontally mirrored | bank `$07:C800`; all three last 99 authored ticks. Centre visuals 5-9 grow to 9 parts with union `x -17..16, y -71..25`. Side visuals 1-4 are 14 stacked 16x16 parts: `16x224`, `y -112..112`; late velocities 1/2/4/6/8 separate the mirrored columns | centre flare and two column emitters. Gate beam lighting by side composition/visual so the 24-tick pre-beam visual does not receive full intensity |

The animation data is independent of the tile upload:

- `$02:BC9E` loads the common action atlas `$07:8000-$9FFF` and action palettes
  `$07:D040-$D09F`.
- It then loads 256 bytes from `$06:A400 + (selected_id-1)*$80` to VRAM `$2D40`.
  The overlapping `$80` source step is intentional.
- `$00:96C3-$96F5` is a separate conditional 128-byte dynamic overlay from
  `$06:A000 + (slot.+38 & $FF)*$80` to VRAM `$2D80`. Slot `+38` is polymorphic: the spell
  handlers above use it as repeat counts, so this path must not be treated as a universal spell
  identity lookup.

For particle/light alignment, the current composition and decoded tile/part geometry are enough.
Mapping the resident tile pixels back to every compressed character source is only required if
the original spell sprites themselves will be replaced.

## Simulation miracle lifecycle and artwork

### Shared driver

There are two entries into the same effect core:

| Address | Meaning |
|---|---|
| `$01:97E5` | user-selected miracle entry; writes kind to `$7F:90EB`, sets user-active `$90E9=1`, clears completion/actor flags, initializes, then pumps frames |
| `$01:9840` | posted/scripted effect driver; marks `$90F5=1`, clears completion/actor flags, then uses the same initializer/ticker. `$03:822E` reaches it for enemy/scripted posts |
| `$01:9898` | kind-specific initializer and semantic effect-start point |
| `$01:99BE/$99CC` | per-tick kind dispatcher |
| `$7F:90F1` | visible Lightning/Rain actor has finished; their next kind tick applies gameplay work and completes |
| `$7F:90F3` | overall effect complete |
| `$7F:90F7` | terrain/structure visual refresh requested |

For user miracles, `$90E9` brackets the full menu-owned operation. It is not sufficient for
posted enemy effects, so a shared host observer should also watch the `$9840` path or active
kind/record state. `$01:9460` is merely the common simulation frame pump and is too broad to use
as an effect hook by itself.

### Packed spawn identity and exact compositions

`$01:CFF2` stores A's **low byte** in `$033D` (variant), swaps bytes, and stores the **high byte**
in `$033C` (list). Therefore `$0500-$0503` mean spawn-list 5, variants 0-3—not list 0-3,
variant 5.

| Packed selector | Actor/use | Script | Exact composition sequence |
|---:|---|---|---|
| `$0500` | primary cloud, classes 2/3 | `$01:A893` | `$D9E5` cloud alone |
| `$0501` | co-located class-8 companion | `$01:A897` | `$DA22` ground-shadow ellipse |
| `$0502` | Lightning active phase | `$01:A89B` | `$DA4B/$DAA1/$DAF7/$D9E5/$DB5C/$D9E5`, authored durations 2/2/2/1/2/4, loop |
| `$0503` | Rain active phase | `$01:A8AE` | `$DBC1/$DC1C/$DC77/$DCD2`, four ticks each, loop |
| `$0504` | new-town two-person creation strikes | `$01:A8BB` | `$E9CC/$E527/$EA27/$E527/$EA82/$E527/$EAEC/$E527`, two ticks each, loop; `$E527` is an offscreen gap sentinel |

The decoded geometry is:

| Composition | Parts | Bounds at the shared origin | Palette | Meaning |
|---|---:|---|---:|---|
| `$D9E5` | 12 | 64x48, y `-16..32` | 2 | cloud |
| `$DA22` | 8 | 64x32, y `+40..72` | 7 | separate shadow record |
| `$DA4B/$DAA1/$DAF7/$DB5C` | 17-20 | 64x76-80, y `-16..64` | 2 | cloud and bolt in one composition |
| `$DBC1/$DC1C/$DC77/$DCD2` | 18 | 64x72, y `-16..56` | 2 | cloud and rain in one composition |
| `$E9CC/$EA27` | 18 | x `1..14` / `4..14`, y `-56..84` | 2 | town-creation bolt frames; terminal 8x8 tile is centred at local `(8,80)` |
| `$EA82/$EAEC` | 21 | x `0..16`, y `-56..88` | 2 | town-creation impact frames; terminal 16x16 block is also centred at local `(8,80)` |

Run `20260803-133014` resolves the town-creation pair as stride-`$26` world records, not the
fixed/process tier inferred from the initializer. Frame 750 contains record `$0E02`; frame 867
contains `$0E02` and `$0E28`. Both expose process identity `$000E` in `+$0E`, retain script base
`$A8BB` in polymorphic raw `+$06`, and place the strikes at `$0150/$0068` and `$0160/$0068`.
The combination `(world tier, process $000E, raw +$06 $A8BB, exact phase composition)` is the
hook. Including `$E527` only under that script identity preserves one lifecycle through the
authored blank gaps without misclassifying the same sentinel in cursor lists 40-48. The two
world slots are independent emitters, so simultaneous strikes receive separate generation/pulse
state while the scene flash still selects the strongest style instead of adding two full-screen
flashes.

### Miracle catalogue

| Kind | Initializer and lifecycle | Effect placement and hook |
|---:|---|---|
| 1 Lightning | `$98BF` allocates class 2 plus class 8. Class 2 moves the cloud into place, enters a 16-tick charge phase, runs `$0502`, spawns a 4x4 set of class-`$10` impact children at 10-tick intervals, then spends 40 ticks leaving/cleaning up. `$01:CC05` sets `$90F1`; `$99F3` completes and applies structure damage | visible light follows the class-2 record origin and only reaches strike intensity when `+08` is one of `$DA4B/$DAA1/$DAF7/$DB5C`; use the impact children/target for ground sparks |
| 2 Rain | `$98E5` allocates class 3 plus class 8. Class 3 moves in, holds the `$0503` rain loop for 60 ticks, moves out, then `$01:CCCF` sets `$90F1`; `$9A14` completes and applies terrain/structure work | same record-origin contract as Lightning. The undecided rain enhancement is therefore an aesthetic choice, not a missing hook problem |
| 3 Sunlight | `$990B`; no effect sprite. `$7F:923E` advances from 0 to 140 and indexes the `$9A6A` ramp through `$9B9D`; completion is `$9A57` | scene-wide light keyed directly to `kind=3, phase=$923E`; the authentic PPU effect is fixed-colour red add on BG1, not a point light at the aimed cell |
| 4 Earthquake | `$997B`; `$7F:921C=180`, with random town camera shake in `$9F65/$9F67`; completes at `$9A8D` and applies whole-map damage | useful for Skull Head and optional debris/dust; scene/terrain effect, not a sprite attachment |
| 5 Wind | `$998F`; `$7F:9218=120`, creates randomized class-4 effect actors, completes at `$9ADA` | attach any particles to the spawned records and their current compositions, with the global timer as outer lifetime |

## Simulation enemy attacks and shared effects

The record pair `(class = +0E, state = +12 & $7FFF)` provides the semantic lifecycle; record `+08`
provides the visible sub-phase.

| Enemy / opportunity | Lifecycle hook | Artwork and placement | Effect implication |
|---|---|---|---|
| Blue Dragon, class `$12` | state 6 in `$01:B9EC/$BB28`; 33-frame building strike | behavior 11 alternates bolt visual IDs 9/10/11 (`$E1BD/$E209/$E255`) with ordinary body frames every tick. Bolts are 15/15/18 parts, palette 2, bounds about `x -5..21, y -4..52/56`. The ROM moves the same dragon record from flight height to target height | outer lifetime `(class $12,state 6)`; emit light only on the three bolt compositions. Use current record X/Y and ground-strike plane; do not use the flying-body height or miracle target fields |
| Red Demon, class `$14` | states 7-9 in `$01:C237`; state 7 runs behavior 18 for 24 ticks, state 8 behavior 19 for 8 ticks, state 9 applies the map-cell hit | behavior 18 alternates body visual 15 with `$E340` (small attached flame) then `$E35A` (larger flame); behavior 19 alternates with `$E383` (large 2x2 flame). Bounds grow from 15x22 to 24x34, palette 1, on the same record | use states 7-9 as attack lifetime and the three exact compositions as intensity gates; this supports charge flame, embers, and impact lighting without lighting ordinary body frames |
| Skull Head, class `$15` | state 7 in `$01:C4E5/$C6F0`; after 24 wind-up ticks it posts miracle kind 4 through `$7F:96E8`, and `$03:820F` invokes `$01:9840` | no separate bolt sprite; the resulting global effect is the shared 180-tick Earthquake lifecycle | local charge particles may follow class/state; quake dust/shake should follow shared miracle kind 4 so scripted and enemy earthquakes match |
| Napper Bat, class `$13` | state 5 in `$01:BE4F` is the ground-pluck attack phase | `$E71B/$E73A/$E75E` reach toward the ground; no distinct emissive projectile or lightning family was found | no required light hook in the initial scope; composition/state remain available if ground dust is later desired |

Two shared enemy transitions are also identifiable:

- Class states 0 use behavior 22: ground fire `$E6CA/$E6D0/$E6D6` for 8 ticks each,
  followed by orb frames `$E6DC/$E6F1/$E706` for 5 ticks each.
- Common terminal state 15 changes into the effect path using behavior 39: burst
  `$E4E8/$E4FD/$E512`, 4 ticks each. Track the record/composition through the class change rather
  than assuming the original enemy class remains present.

The alternative composition family `$EC14-$EC35` remains unobserved in live miracle captures and
must not be used as the identity for Lightning or Rain.

## Implemented simulation lightning and fire slice

The enhanced simulation effects follow the minimum contract above without adding gameplay state:

- `SimFrameData` captures the user/posted miracle lifecycle words on the authentic logic tick and
  publishes a `lightning_miracle` effect instance for a valid world record of type `$02` throughout
  the complete kind-1 lifecycle. The cloud (`$D9E5`) and four visible bolt compositions
  (`$DA4B/$DAA1/$DAF7/$DB5C`) become explicit semantic phases rather than presentation-specific
  intensity values.
- A host-only generation identifies each lifecycle even when the same WRAM record is reused.
  Lifecycle age, phase age, visible-pulse generation/age, and terminal flags are captured once per
  authentic build tick, so replayed presentation frames cannot advance an emitter accidentally.
- The same producer publishes Blue Dragon class `$12`, state 6 for the full 33-frame attack while
  marking only `$E1BD/$E209/$E255` emissive. It publishes Red Demon class `$14`, states 7-9 while
  marking only `$E340/$E35A/$E383` emissive. Ground-fire lifecycle is composition-owned, so the
  same generation survives the observed `$10/$12/$13` record-class changes without guessing which
  system spawned it; its red/blue style comes from the captured palette mask, not that lifecycle.
- World-tier process `$000E` town-creation records with raw `+$06 == $A8BB` publish
  `town_creation_lightning` across the complete `$01:A8BB` loop. Four exact bolt phases are
  visible and `$E527` is a non-emissive gap inside the same record lifecycle, so particles pulse
  once per authored strike frame rather than restarting as unrelated effects. Two active records
  remain two independent emitters.
- Scripted burning-house records publish `house_fire` only for the exact combination of world tier,
  packed identity `$0A01`, and `$DD2D/$DD33/$DD39`. The three visible source phases share one
  record-owned pulse and use local `(8,16)` as their semantic ground contact. Renderer style lifts
  their glow and particle origins to `(8,12)`, leaving source classification unchanged.
  `sim_visual_patches.c`
  validates the complete `$01:A838` script signature before changing its three duration bytes from
  one to four; it runs on the live cart before the randomizer snapshots its restore baseline and
  leaves the image untouched on any mismatch. No renderer or metadata callback writes game state.
- The strike point comes from the decoded composition endpoint relative to that record's current
  world origin. Effect metadata carries a reusable point/segment/area/scene geometry contract and
  record-local/world-local/screen coordinate space. Projection shares the billboard's town camera,
  ground plane, and depth-scale helper, so camera movement and diorama pitch keep the light attached
  to the bolt.
- `Effect lighting` adds a brief scene flash and local lightning/fire glow. `Particles` adds
  deterministic strike sparks or continuously cycling rising embers keyed by record address,
  lifecycle generation, pulse generation, kind, and particle index. Style is mapped from semantic
  phase in the renderer; no brightness or particle-count policy leaks into the capture layer.
  Neither effect advances on presentation-only frames or reads live WRAM during rendering.
- All implemented action and simulation stages use SDL's renderer-provided additive blend and
  untextured geometry rather than a
  platform shader. That keeps the core path common to Metal, Vulkan, Direct3D, software, and
  headless renderers without claiming pixel-identical rasterization. Ground light, scene flash,
  and particles are separate ordered passes, and each geometry class is submitted as one batch.
  The renderer verifies the blend mode SDL actually applied, restores all state after each pass,
  and latches additive-blend and geometry failures as separate capabilities so both stages fail
  closed.
- Fixed direction vectors and lifecycle-relative integer hashes avoid platform math-library drift.
  Metadata capacity is explicit; overflow invalidates only effect metadata for that frame and
  suppresses both enhanced stages instead of drawing a partial, misleading result.
- Settings expose independent `sim3d_effect_lighting` and `sim3d_particles` switches, with
  `AR_SIM3D_EFFECT_LIGHTING` and `AR_SIM3D_PARTICLES` environment overrides for deterministic
  capture.

`D6a-lightning-miracle` replays the existing `sim-actions.rec` fixture at the first visible bolt
frame. It asserts the exact 240-tick lifecycle, 12 visible effect frames, semantic phase and
generation totals, zero overflow, and all four exact bolt compositions. It validates source/effect
accounting, captures the enhanced and disabled-stage images, and proves the authentic framebuffer
hash is unchanged because this remains a presentation-only pass.

`D6b-blue-ground-fire` captures four concurrent impact fires and asserts 1,123 lifecycle ticks,
29 generations, and the exact three-composition animation. Of those ticks, 635 emit palette-2 OAM
and are explicitly blue; 488 are outside the sprite window and deliberately retain no colour style.
`D6c-blue-dragon-lightning` captures
the first attack and asserts one 33-tick generation: 27 non-emissive attack frames plus two exact
three-frame bolt bursts. Both retain the strict feature-off comparison and authentic-framebuffer
non-interference checks.

## Suggested first hook boundary

The least invasive host boundary is a read-only metadata pass after authentic object updates and
before/alongside enhanced presentation:

1. Detect semantic start/end from the controller, miracle driver, or enemy class/state.
2. Enumerate the relevant live action slots or simulation records.
3. Publish instance address, lifecycle generation/ages, kind, semantic phase, geometry/space,
   current composition, transform, and completion flags.
4. Derive presentation style from those semantic fields and key any renderer-only emitter state by
   instance address plus lifecycle generation.
5. Retire emitters when the slot/record frees or the outer semantic lifecycle ends.

This preserves the original sprites as the synchronization authority. It also lets lighting gate
on exact visible frames, avoiding glow during Magical Light's pre-beam pause, Blue Dragon body
frames, or Red Demon body-only alternation.

## Validation remaining

No additional decompilation is required for the mapped effect families. Simulation Lightning,
Blue Dragon lightning, and blue ground fire now have strict replay coverage. The scripted red fire
has a frame-11,645 native snapshot proving palette-1 emission from the shared ground-fire pointers.
Run `20260803-130945` frame 19,950 separately proves the burning-house `$0A01/$A838/$DD33`
identity and placement; its post-implementation visual acceptance capture remains to be taken.
Before final visual tuning, finish these focused acceptance captures:

1. Enable `AR_ALL_MAGIC=1` and `AR_INF_MP=1`; cast Magical Fire with both flat and diorama action
   presentation and visually accept attachment, coverage, intensity, lifecycle end, and pause
   behavior. Then cast the other three spells in a scrollable action stage.
2. Log the controller kind and cohort slot `status, X/Y, +0A/+0C/+0E/+10, +20, +22, +28` each
   tick. Confirm the decoded timing, clone count, and slot reuse for IDs 2-4.
3. Cast simulation Rain and Sunlight once. Confirm the class-3 `$0503` transitions and the
   `$923E=0..140` sunlight ramp before implementing their distinct weather/scene-light styles.
4. Capture one Red Demon attack to visually accept its elevated red flame attachment, and one Skull
   Head attack to confirm the statically decoded Earthquake-posting window against live
   presentation.

This is an acceptance gate, not a discovery gate: all hook addresses, instance identities,
graphics compositions, positions, transforms, and lifecycle end conditions needed by the effect
layer are now mapped.

## Reproduction tools

```sh
python3 tools/action_magic_catalog.py > /tmp/action-magic-catalog.json
python3 tools/sim_object_catalog.py crawl --json /tmp/sim-object-catalog.json
```

The action tool is intentionally ROM-driven and reports geometry/timing rather than pixels. The
simulation catalogue supplies every behavior step, spawn-list script, composition part, tile,
palette, priority, and bound used above.
