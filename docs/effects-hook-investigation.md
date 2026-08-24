# Particle and lighting effect hook investigation

**Status:** Lifecycle, animation, geometry, and anchor mapping is complete for
the four action magics, five simulation miracles, and four simulation enemy
classes. Implemented effects cover simulation lightning and fire families,
burning houses, Magical Fire, and the mapped Bloodpool, Marahna, Aitos, and
Death Heim scene accents. Enhanced boss families cover each original room as
well as its Death Heim rematch. Snapshot hooks and rendering have ROM-free
regression coverage.

The remaining acceptance work is the focused Bloodpool, sword-beam, Marahna,
and simulation-effect sweep described below. Current project acceptance lives
only in [progress.md](progress.md); all four action spells, Aitos, and Death
Heim are confirmed. See
[Validation remaining](#validation-remaining).

The mapping is implementation-neutral: it identifies authentic 60 Hz state an
enhanced renderer can observe without changing damage, timing, or object
allocation. The signature-checked burning-house frame hold is the one documented
source-art exception; it changes visual cadence, not actor lifetime. Related
references: [SEAMS.md](SEAMS.md), [rendering-engine.md](rendering-engine.md), and
[sim-object-catalog.md](sim-object-catalog.md).

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
8. **The Bloodpool wall torches are BG1 map objects, not action records.** Maps `$02/$03` and
   `$02/$05` both use the exact metatile pair `$47` over `$4F`; the light anchor is the flame at
   local `(8,15)` within that 16x32 pair. Capture therefore matches that bounded pair throughout
   the Bloodpool group rather than maintaining a room allowlist. A colour-key or screen-space
   pixel search is unnecessary.
9. **The observed enemy fireballs and lightning are independent Bloodpool Act-2 action objects.**
   The room family is `$02/$02-$08`: the ordinary-enemy blob is shared across the act, so exact
   discovery-room gates would reject legal randomized placements. Fireball flight retains source
   `$BD76/$BD84` and is handler `$BDF0`, resume `$BDD9`, state `$23`, animation `$7E:4000`, with exact
   visual/composition pairs `$17/$45EF` and `$18/$4610`. The lightning-trap bolt is source `$BD2A`,
   resume `$BD69`, state `$14`, animation `$7E:4000`, handler `$BD36`/`$8683`, with pairs
   `$1F/$46FE` and `$20/$479D`. Their live slot positions and culling extents are the anchors.
   Both source families fail closed outside the scoped act, and a stage/map handoff retires their
   observer generations even if the next room immediately reuses the same action slots.
10. **The Bloodpool boss attack is a third lightning family.** In map `$02/$08`, source `$BDFF`
    creates linked `$7E:5000` children through `$BFDF/$BFF9`. States `$02-$04` are long/medium/
    short VERTICAL strikes using visuals/compositions `$00/$5346`, `$01/$5401`, `$02/$5492`;
    states `$05-$07` are the corresponding DIAGONAL strikes using `$03/$54F2`, `$04/$55C2`,
    `$05/$5661`. Horizontal flip mirrors every path. Shared `$20/$5D2B` is the blank half-cycle,
    not a telegraph; saved resumes `$C02B/$C04B/$C051` vary across repetitions and are not phase
    identity. State `$09` is the linked floor impact. Slot `+$3A` validates the parent and supplies
    stable continuity across those control-flow changes.
11. **The sword beam is a player-linked action projectile, not a Bloodpool object.** Creator
    `$00:9CF2` installs handler `$9D1C`, animation `$06:8000`, attacker flag `$0001`, and a
    backlink to player slot `$08A0`. State/visual/composition tuples `$13/$30/$99E8` and
    `$14/$31/$9A17` are its two authored cycles. Both compositions emit the same six 8x8 crescent
    parts, but their signed byte origins place them differently relative to the object hot point.
    State `$13` normal bounds are `(32,-33)..(48,-1)`, centred at `(40,-17)`; state `$14` normal
    bounds are `(40,-9)..(56,23)`, centred at `(48,7)`. H-flip produces `(-48,-33)..(-32,-1)` and
    `(-56,-9)..(-40,23)`. Run `20260810-184935` confirms state `$13` exactly: world `(232,456)`
    minus camera `(120,255)` gives hot point `(112,201)`, while captured OAM spans
    `(144,168)..(160,200)`. Vertical flip is not an authored beam state and fails closed; horizontal
    flip selects the measured mirrored rectangles. The measured velocity, normally `+8` or `-8`,
    is the authoritative path direction.
    Aitos's Act-2 dragon has a second, separately validated producer in map `$04/$03`.
    `$00:D785` leaves an inactive state-0 `$D793` controller linked to the active `$D646`
    boss root and allocates two `$A655` children. Once visible they are handler/resume
    `$8661/$A65D`, animation `$7E:5000`, index 1, flags `$0020`, and exact
    state/visual/composition/local-counter/velocity tuples
    `$01/$21/$56D8/$01/(-3,+1)` and `$02/$20/$56BE/$02/(-3,-1)`.
    Snapshot `snap_05_gf21056` proves both 24×24 crescents at OBJ priority 2. They reuse
    the sword comet presentation through their measured diagonal headings; they do not
    relax the player identity rule or match arbitrary boss-bank artwork. Run
    `20260812-224123/snap_01_gf15666` measures the other facing: controller and child both
    carry H+V flip `$C000`, velocity becomes `(+3,-1)` for state 1, and all four extents
    swap sides. Applying that exact 180-degree lifecycle relation to both states covers
    the two rightward diagonals without admitting independent flip/velocity mixtures.
12. **Marahna's requested accents are a separate map/object family.** In maps `$05/$04-$08`,
    one BG1 metatile `$43` is the complete torch, anchored at local `(8,11)`. The shared 2304×1792
    world contains 31 instances, so capture uses the same bounded `ActionBgMapView` contract as
    Bloodpool but publishes only cells within a 256px margin around the camera. Map `$08` is a
    separate 512×512 world containing ten occurrences of that same authored signature. The actual
    fireball family retains source `$E047`, handler `$8661`, and animation `$7E:4000`. Its
    state-`$0C` large orb is an eight-entry left/idle/right/idle cycle: exact
    visual/composition/velocity tuples are `$07/$451C/(0,0)`, `$08/$4528/(-1,0)` or `(-2,0)`,
    `$05/$4504/(0,0)`, and `$06/$4510/(+1,0)` or `(+2,0)`, all with resume `$E061` and 8px
    extents. On split, the inactive orb remains the parent at child `+$3A`; four 4px-extent
    children resume at `$A65D` and travel exactly `(0,+3)`, `(-3,0)`, `(0,-3)`, and `(+3,0)`.
    Vertical children use state/visual/composition `$0F/$32/$4BCD`, with V-flip on the upward
    child; horizontal children use `$10/$33/$4BD9`, with H-flip on the rightward child. Run
    `20260811-225534` exposes both omissions in the former left-only matcher.
    Run `20260811-232640` corrects the next projectile family: `$E0BA` is a reaper whose
    `$19-$1C` horizontal/aimed/vertical orb must not receive fire. The actual snake fireballs
    retain source `$DE96`, shared handler/resume/state `$8661/$A65D/$06`, 8/4/8/4 extents,
    local counter 6, and exact `$1D/$4869` or `$1E/$487C` artwork at velocity `(-4,0)` or
    H-flipped `(+4,0)`. Their backlink resolves to the source-`$DE96` snake lifecycle, preventing
    shared animation machinery from becoming identity.
    Run `20260811-221433` proves the six `$34/$4BE5` source roots previously classified as
    fireballs are moving-platform machinery and must fail closed.
13. **Marahna's two lightning orientations are linked endpoint connectors.** Connector children
    use handler `$8683`, source `$E18E`, resume `$E24F`, and backlink `+$3A` to the first endpoint.
    The `$E254` partner is the immediately following slot, and the connector hot point is their
    exact midpoint. Horizontal state/visual/composition `$27/$2E/$4AA1` has 40/4/40/4 extents;
    vertical `$28/$31/$4B82` has 5/40/5/40. Endpoint pairs `$0D/$45B8` + `$0F/$45D0` and
    `$0E/$45C4` + `$10/$45DC` validate orientation. The ten-part connector compositions support
    ten aligned host-ribbon segments without inferring an angle from screen pixels.
14. **Aitos lava pits are bounded BG1 structures, and their fireballs are cyclic actors.** On
    map `$04/$01`, every observed pit rim is `$DC`, one or more `$DD`, then `$DE`, with equally
    wide `$DF` then `$E7` bubbly rows below when map height permits (the rim at world Y=992
    legitimately ends after `$DF`). The four decoded pits are 64px wide at world `(1648,976)`,
    `(1888,992)`, and `(2144,976)`, plus the 128px pit at `(3616,928)`. Capture requires the whole
    full signature and publishes only a camera-local subset on BG1. Six persistent emitter slots
    retain source `$CF9E`, resume `$CFCD`, animation `$7E:4000`, 8/8/8/8 extents, no flip, and
    artwork `$2A/$4D21` or `$2B/$4D2D`. Rising state `$22` uses handler `$CFE3` and velocity
    `(0,-4)`; wait/reset state `$23` uses `$8661` and `(0,0)`; return state `$24` uses `$CFFE` and
    `(-1,+6)`. Source+resume continuity plus bounded motion starts a fresh particle generation when
    the cyclic slot jumps back to its launch point.
15. **Aitos volcano rocks and waterfall platforms have separate positive identities.** Run
    `20260812-000613` separates launched molten rocks from `$CF9E` fireballs: they retain source
    `$CEEC`, resume `$CF16`, handler/state `$8661/$27`, animation `$7E:4000`, artwork
    `$2B/$4D2D`, extents `8/8/8/8`, X velocity `-2` unflipped or `+2` H-flipped, and measured Y
    velocity `-1..+1`. Stationary `$CEEC/$CF1C` lava-mouth actors are excluded. Waterfall platforms
    in maps `$04/$02-$03` use an exact three-row BG1 structure:
    `$36/$5E*/$81`, `$4E/$F4*/$4F`, `$F6/$FC*/$FE`. Camera-local presence of that structure is
    also the live discriminator between map `$04/$02`'s waterfall section and its preceding cave,
    whose decoded BG2 map is otherwise the same. That discriminator is now published immutably to
    the Diorama layer resolver as section `waterfall`: it inherits the base room tuning, then selects
    `rom-04-01-bg2`, the generic ROM-backed **skybox** source for room `$04/$01` BG2. The same
    catalogue can select BG1 or BG2 from any valid action map; it is not a previous-frame/previous-
    room cache, and a failed decode falls back to the current room's captured BG2 skybox.
16. **Marahna's boss attack is a third electrical family with four presentation stages.** Only
    original map `$05/$08` or Death Heim rematch `$07/$06` admits it. The live boss parent retains
    source `$E483` in Marahna or `$F72A` in Death Heim, animation `$7E:5000`,
    and 48/40/48/8 extents. The original parent has no spawner backlink; the
    rematch parent retains `$001C`. Handler `$8661` owns its pre-impact stages: exact
    visual/composition `$07/$57C2` and `$08/$5868` are its hand-charge cycles; `$0A/$59DE` is the
    central orb. A launched child uses the same source,
    backlink `$12E0`, resume/state/visual/composition `$E578/$04/$11/$5CE0`, velocity
    `(-4,+4)` or `(+4,+4)`, and the corresponding left/down or right/down 32px quadrant. After
    impact, that child resumes at `$E57E` in state `$07` and rides the floor at `(-4,0)` unflipped
    or `(+4,0)` H-flipped. The complete loaded cycle is `$12/$5D01`, `$13/$5D0D`,
    `$14/$5D2E`, `$13/$5D0D`; its backlink parent is then the exact shared-repeat tuple
    `$8683/$0A/$E4D7/$00/$5307`. Death Heim's parent instead retains the room-owner backlink
    `$001C`; child linkage remains otherwise unchanged. These identities supply measured local
    geometry for both flat and Diorama projection without estimating an angle from pixels.
17. **Death Heim wraps original boss families instead of preserving their source records.** Runs
    `20260822-195453` and `20260822-195726` map the room-order aliases: Minotaur `$AF5D→$F6CA`
    (`0702`), Wizard `$BDFF→$F6E2` (`0703`), Flaming Wheel `$D838→$F712` (`0705`), Viper
    `$E483→$F72A` (`0706`), and Ice Dragon `$F161→$F760` (`0707`). The wrappers preserve the
    original attack control flow and `$7E:5000` artwork, so every matcher accepts exactly its
    original room/source pair or its rematch room/source pair; cross-paired aliases fail closed.
    Minotaur axes are exact `$8661/$B008`, state-3 spin frames with a same-source parent backlink.
    Flaming Wheel is the visible active `$4000` body, not every record sharing its source: the
    original body has backlink 0 and the rematch body has room-owner backlink `$001C`, while its
    handler legitimately changes. Ice balls are exact `$8661/$F2CA`, state `$19/$1A`, visual
    `$12-$19` children with a same-source parent. Tanzara (`0708`, source `$F80F`) uses an exact
    50-tuple projectile allowlist. Pharaoh (`0704`) remains intentionally undecorated. Wizard and
    Viper reuse their measured lightning styles, and Viper's Death Heim room also reuses Marahna's
    single-metatile `$43` BG1 torch rule.
18. **Aitos Act 2's lava and Flaming Wheel need their own measured geometry.** Run
    `20260823-211358` maps the side-view lakes in rooms `$04-$06` as maximal `$01` BG1 lips over
    `$05` lava body, with `$02-$04`/`$77` animated surface cells and exact `$33/$34`, `$2C/$32`,
    or `$33/$32` banks. The wide map-`$06` lake can begin outside the bounded camera scan, so
    capture walks left to its authentic bank before publishing one reservoir. Flaming Wheel's
    cyan shots are exact `$8661/$A65D`, state `$08-$0C`, visual/composition `$00/$51B5` through
    `$03/$51D9` children linked to the same-source root. Full-ring body compositions
    `$5276/$5398/$54BA/$55DC` place twelve fireballs at exact local centres from `(-24,-24)`
    through `(24,24)`; the ring effect is anchored to those centres rather than to a circular
    approximation.
19. **Priority is projection metadata, not Flaming Wheel identity; Aitos statue fire is a scoped
    timed actor.** Run `20260824-034218` shows the original wheel body and all five cyan children
    in OBJ priority 2. The family is still identified by `(stage, room, retained source)`, loaded
    `$7E:5000` animation/composition, and root/child ancestry. Its exact part words carry raw
    priority zero, so `$00:8D68` adds the live band from `$008F`; the host now copies that value
    after matching, allowing Death Heim `$F712` to choose a different priority without weakening
    the discriminator. Map `0406`'s statue-fire records likewise retain source `$D5B1/$D5C0`,
    loaded animation `$7E:4000`, and no flip/H-flip facing. State `$18` grows through exact plume
    art `$1C/$4763`, `$1D/$4776`, `$1E/$4790`; state `$19` sustains the full pillar with
    `$1F/$47B1` and `$1E/$4790`. State `$1A` is the inactive `$17/$46FD` hold. Several are drawable
    in the vertical Diorama margin while `$0400` remains set under the authentic-height test, so
    the enabled activation extension admits only this exact room/source/graphics tuple vertically.
    Their warm light and buoyant sparks follow the live OBJ plane and the authored plume bounds.

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
- Those four words are raw collision-header data, not a universal presentation rectangle. Most
  observed actors encode unsigned distances, but the sword beam uses signed offsets; its host
  geometry is decoded from the six authored OAM parts instead.
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
- `action_effect_render.c` owns spell style and geometry extraction;
  `action_effect_projection.c` owns the pure camera/widescreen/flat-or-Diorama mapping, and
  `present.c` copies immutable frame inputs into that context and submits the bounded batch. The
  renderer centres each glow on the published rectangle, then emits
  quadrant-aware embers from all four mirrored actors. Its pulse is an integer triangle wave rather
  than `libm` sine, so authored-tick output is repeatable across platforms. Unknown effect, phase,
  geometry, layer, or priority values fail closed.
- Flat mode maps through the resolved presentation viewport. Diorama mode consumes the compositor's
  exact resolved matrix, mesh dimensions, and paired UV-window/shape record for the source BG or
  OBJ plane, keeping the overlay registered without reading live WRAM, duplicating camera auto-fit,
  or guessing a parallel depth. Dynamic actors remain a world overlay above the composed world and
  below HUD/HD UI. BG-local accents use painter-aware paths: torches submit after BG1-low in Diorama
  and through an owning-screen BG1-winner mask in flat presentation; the waterfall submits after
  BG2-low in Diorama and through a main-screen BG2-winner mask in flat presentation. Later BG/OBJ
  occlusion is therefore preserved without a custom shader.
- Both modes share the same capability-checked SDL additive/untextured-geometry path as simulation
  effects. No platform shader is required. Settings expose independent `action_effect_lighting`
  and `action_effect_particles` switches (`AR_ACTION_EFFECT_LIGHTING` /
  `AR_ACTION_EFFECT_PARTICLES`), both on by default.

### Action-scene accent slice (2026-08-10)

Updated with the Aitos depth work on 2026-08-12 and the Death Heim rematch,
torch-occlusion, and audit contracts on 2026-08-23.

- `ActionSceneEffects_CaptureFrame` is a separate immutable frame contract from the spell cast.
  It scans BG1's bounded page map for Bloodpool torch pairs through the same pure
  `ActionBgMapView` validation/addressing contract used by `ActionBgWorld`, then walks the ordinary
  action-object table for the measured actor signatures above. The Wizard boss-lightning rule is
  limited to Bloodpool `$02/$08` or Death Heim `$07/$03` with the corresponding retained source,
  validates its linked parent through `+$3A`, and
  matches every pair decoded from animation states 2 through 7 and 9. Unknown lookalikes fail closed;
  inactive, no-draw, ineligible, and outside-activation records never submit geometry. Scene actor
  generations also combine stable resume/source identity with bounded motion continuity, so an
  immediately recycled same-kind slot cannot inherit its predecessor's particle clock. The boss
  child uses source/backlink continuity because its saved resume intentionally changes across
  repeated strike/blank cycles.
- Marahna adds a second bounded torch rule for maps `$05/$04-$08`, exact fireball/link actor rules
  for `$04-$07`, and an exact Viper boss-lightning rule for original `$05/$08` or rematch
  `$07/$06`. The rematch room also admits the same `$43` torch signature.
  The torch rule matches single metatile `$43` and applies a camera-local publication window; it
  uses shared metatile-aligned scan bounds rather than traversing the complete world, does not scan
  pixels, and does not consume one record for all 31 shared-world torches. The boss room's ten
  `$43` cells use that same rule. Fireballs reuse the established heading-aware fire style but
  retain distinct lifecycle keys: the complete `$E047` left/idle/right orb cycle and four
  correctly flipped cardinal children, plus the separately parent-validated `$DE96` snake-shot
  family. Lightning links
  validate the source endpoint, adjacent partner, orientation-specific compositions, backlink,
  and exact midpoint before publishing. Up to five authored links receive two ten-segment projected
  ribbons, a chord-aligned cyan/violet glow, crawling sparks, and endpoint fans; a sixth forged
  link fails the explicit geometry-capacity contract.
  The boss attack separately validates its live `$E483` parent and exact charge/orb/diagonal-bolt/
  ground-charge artwork.
  Charge and orb stages receive cold two-tier illumination and sparks; the launched 32px diagonal
  receives a two-layer eight-segment projected ribbon. The complete three-frame floor charge gets
  a grounded oval bloom, contact sparks, and a direction-aware wake. A forged second diagonal bolt
  exceeds the explicit one-bolt capacity and fails closed.
- Aitos map `$04/$01` adds a camera-windowed lava-surface scan through the same bounded BG map
  view and shared aligned scan bounds. It requires the complete `$DC/$DD+/$DE` rim over `$DF`
  and includes the following `$E7` bubbly row when it exists,
  derives the exact 64px or 128px
  surface rectangle, and projects it on BG1. A separate `$CF9E/$CFCD` actor rule covers all three
  exact fireball handler/state/velocity phases and both artwork pairs on OBJ priority 0. Lava surfaces use
  an elongated two-tier orange spill over the full bubbly volume plus twelve births spread across
  the rim width from a narrow band one quarter-height above its geometric midpoint; fireballs
  use the velocity-aligned flame body and wake, with an upward fallback during the stationary wait.
  Both animated BG fire styles sample presentation at 2× while retaining authentic 60Hz capture
  clocks.
- Aitos Act-2 maps `$04/$04-$06` use a separate side-lava capture. Overlapping at-most-96px light
  sections and 32 deterministic sparks follow the complete lip, while one fixed 16×14 textured mesh refracts the
  already-composited world by at most 6.5 output pixels before the HUD. This bounded geometry
  pass is room-gated for all three volcanic chambers rather than keyed to a camera-local lake
  signature, is shared by flat and Diorama rendering, and is disabled with Action Effect Particles.
  Snapshot `snap_02_gf5009` first proved map `$04/$05`'s black rectangles were in the authored BG2
  source rather than HLE loss; `20260823-232614/snap_00_gf3879` found the same convention at much
  larger scale in `$04/$06`. The source deliberately mixes colour-zero transparency with opaque
  black cells and linework. Hardware resolves the former against its final black backdrop, making
  the two visually continuous. Diorama rooms `0405` and `0406` therefore opt into
  `bg2 = transparent:black`: the complete live BG2-low presentation plane is filled black first,
  including untiled regions, and then all low/high tile art paints over it. Their immutable ROM BG2
  skyboxes follow the same rule. Every non-zero pixel remains intact; the residual Backdrop plane is
  disabled, and authentic 2D scanout is untouched.
- The separately identified `$CEEC/$CF16` rocks receive a compact molten halo and close tumbling
  sparks rather than a directional flame wake. Exact waterfall-platform structures receive cool
  lip spray plus downward drips. When at least one is camera-local, one bounded BG2 record adds a
  cool water veil and 96 slow vertical flow streaks over the source waterfall. A paired
  after-BG2 Diorama record adds two tiers of three mist banks and thirty-two foam motes at the end of the safe
  24px BG2 extension, where the intentionally finite backdrop would otherwise expose black.
  Run `20260812-224123/snap_00_gf11758` showed that six similarly sized additive banks
  still converged on one horizontal lower edge. The lower tier is now narrower, vertically
  staggered, and irregular, with visible bank depths differing by more than 80px and the
  deepest fade extending over 100px below the safe seam. This one concealment pass uses
  verified source-alpha blending; the waterfall veil and luminous actor effects remain additive.
  Map-derived accents have an
  independent 16-record frame from the 16 actor records: the measured maximum camera window
  publishes 14 splash structures plus one veil and one mist while still admitting the complete
  actor budget. Either list fails closed independently. These use the same portable untextured SDL
  geometry and standard blend modes as every other action accent; no renderer-specific shader blob
  is required.
- The player sword beam is deliberately outside the Bloodpool map gate. Capture requires handler
  `$9D1C`, animation `$06:8000`, attacker flag `$0001`, one of the two exact state/visual/
  composition tuples, and a backlink to the live player whose source descriptor matches the
  child. This prevents a polymorphic or immediately recycled slot from inheriting the effect.
- Aitos map `$04/$03` additionally admits exactly the dragon's two-child `$D646` sword volley
  in both measured facings.
  Capture validates each full child tuple, the inactive `$D793/$23/$56FE` controller, and its
  backlink to the live boss root. Normal children use no flip; the reflected family requires
  matching controller/child H+V flip, reversed velocity, and side-swapped extents. Both retain their authored priority-2 projection and exact
  OAM-local rectangles while sharing the player beam's portable halo/wake/star style.
- Death Heim rooms admit the measured rematch source only alongside its corresponding room and
  retain every original-room route. Wizard `$F6E2` and Viper `$F72A` reuse the complete existing
  lightning renderers. Minotaur `$F6CA` axes receive a warm spinning light and velocity-aligned
  sparks; the `$F712` Flaming Wheel body receives a persistent flame shell plus emitters at its
  twelve exact rim-fireball centres, and its exact cyan children receive cool light and wakes; `$F760`
  Ice Dragon balls receive cool light, crystalline particles, and a velocity trail; Tanzara
  source `$F80F` receives restrained generic projectile light/trails for only its exact tuple
  allowlist. Pharaoh remains a negative case. Death Heim Viper room `$07/$06` runs the same
  camera-local `$43` BG1 torch scan as Marahna `$05/$04-$08`.
- Aitos map `$04/$06` admits the measured `$D5B1/$D5C0` statue-fire family only with its
  `$7E:4000` animation source. The enabled vertical activation extension advances these visible
  timed hazards in the Diorama margin, while the effect itself admits only active state `$18/$19`
  and exact `$1C-$1F` pillar compositions. Idle state `$1A`/`$17` remains undecorated. Active
  pillars add a warm horizontal spill and distributed rising sparks without changing other enemies.
- Each source remains an independent light centre. Torches receive a warm two-tier wall spill and
  a small rising ember plume; fireballs receive a heading-aligned hot body, warm spill, and trailing
  sparks; trap lightning receives a tall cyan shaft aura, crawling sparks, and a small impact fan.
  Boss lightning receives a floor-impact bloom plus a strike-only warm spill, impact fan, and two
  projected filaments (amber corona and white-gold core). The filaments follow the real 8×8 OAM
  row centroids for all six paths, including horizontal mirroring and `$8D68`'s one-pixel Y draw
  bias; the surrounding glow rotates onto the same authored chord. All particle placement is an
  integer-hash function of authentic lifecycle clocks driven by the completed `$00:8C98`
  gameplay-pass serial. Publication occurs only at the routine's successful common epilogue,
  so native/host pauses and abnormal nested-HLE returns freeze effects, and repeat builds are
  byte-identical even though native pause continues running emulated frames.
- Sword-beam presentation uses a restrained cold halo/core, narrow 80px/56px connective haze
  layers, and forty-eight cyan-white crossed-star glints arranged as sixteen fixed cross-sections
  of three height lanes. Positions stay fixed from 4px to 88px behind the crescent while scrambled
  18-tick phases independently fade and scale each star into and out of existence; no glint streams
  backward along the path.
  All elements follow measured velocity—including the Aitos boss's four diagonal branches—mirror
  for leftward travel, and project through the same
  OBJ plane in flat or Diorama mode. This supersedes both the detached triangular wake in run
  `20260810-184935` and the barely visible five-glint correction in run `20260810-190012`.
- Flat presentation uses the normal action camera projection. Diorama presentation publishes the
  exact resolved BG1-low and BG2-low UV/shape records alongside the existing four OBJ records:
  BG planes pass the same visibility, texture, current-pixel, flat-HUD, and skybox gates as
  drawing. A visible current world-overlay actor also counts as current projection content for its
  exact OBJ priority band; this is required when the isolated band has no final winning pixels,
  and is requested through a four-bit mask derived from the immutable spell/scene frames. The OBJ
  layer request and texture-resource gates still fail closed; immutable request/content/success
  masks distinguish an intentionally empty band from a failed upload. Torches, lava,
  and platform splash/drips follow BG1 depth/rake/bow; the waterfall veil follows BG2 camera and
  geometry and is drawn immediately after BG2-low rather than after the complete world; fireballs
  and lightning follow OBJ priority 0. Flat mode clips that veil with the PPU's completed
  main-screen BG2-winner mask before additive composition. Flat wall torches instead use an
  owning-screen BG1-winner mask: it resolves TM when BG1 is a main-screen source and the resolved
  TS winner when BG1 is subscreen-only, preserving Viper OBJ occlusion in both Marahna and Death
  Heim. The BG-local intermediate is viewport-sized and uses target-local coordinates, so
  letterbox/pillarbox pixels are neither cleared nor multiplied. Mask capture is skipped entirely
  when both Action Effect lighting and Particles are disabled. A missing plane or mask fails closed instead of
  placing the effect at a guessed depth. The published projection also owns the hidden
  OBJ-resolve apron origin of the 640-column layer textures; callers remain in displayed-capture
  coordinates, preventing overlays from drifting across a raked plane while the flat path stays
  unchanged.
- This slice deliberately adds no GLSL, SPIR-V, or MSL artifact. It uses the established bounded,
  capability-checked SDL additive/untextured-geometry pass, so Metal, Vulkan, Direct3D, OpenGL,
  software, and headless builds share one implementation. The existing Action `Effect lighting`
  and `Particles` settings control both spell and scene accents. Spell and scene batches share one
  capacity-aware geometry writer and append directly into their final arrays; scene rendering does
  not allocate or copy through a spell-sized scratch batch. Dynamic actors and map decorations
  share the writer but keep independent capture capacities and reuse one scene render batch.

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

**Magical Stardust launch geometry, and why it looks wrong in widescreen (2026-08-05).**
`$00:A0E8` picks one of two launch sites from a random byte: `rand < $80` births the star at
the TOP edge (`world_y = camera_y & $FFF8`, screen y 0, `world_x = camera_x + $80..$FF`);
`rand >= $80` births it at the RIGHT edge (`world_x = camera_x + $100`, i.e. screen x **256**,
with `world_y = (camera_y + rand-$80) & $FFF8`, screen y **0..120**).

Two properties of that second path matter for any enhanced presentation. Screen x 256 is one
pixel outside the authentic window — the ROM is using "just offscreen" to hide the birth — and
the Y range tops out at exactly the player's own screen line, so about 12% of casts start a
star at the player's feet. Neither is visible on original hardware. At 446-wide with margin
objects enabled the birth point is on screen, which reads as "stardust spawning in the ground".
Accepted as authentic; see bug-ledger.md §33 for the full derivation, the measured/proven/
predicted split, and why it is not OAM wrapping.

Actors are also CREATED on the player with zero velocity before that handler relocates them,
which is what "not retained at the player" above means. Presentation must distinguish the two
by MOTION — they share animation state 0 and visual 0 — or it decorates a stationary actor
standing on the player.

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

Current project acceptance is tracked only in [progress.md](progress.md). All
four magic spells, Aitos, and Death Heim have completed their visual acceptance;
the remaining technical passes are listed here without restating completed
coverage.

No additional decompilation is required for the mapped effect families. Simulation Lightning,
Blue Dragon lightning, and blue ground fire now have strict replay coverage. The scripted red fire
has a frame-11,645 native snapshot proving palette-1 emission from the shared ground-fire pointers.
Run `20260803-130945` frame 19,950 separately proves the burning-house `$0A01/$A838/$DD33`
identity and placement; its post-implementation visual acceptance capture remains to be taken.
Before final visual tuning, finish these focused acceptance captures:

1. Revisit Bloodpool `$02/$03` and `$02/$05` in flat and diorama modes. Visually accept torch wall
   registration and synchronized faster cadence, the strengthened fireball trail, complete
   176px lightning-column coverage, source retirement, and the lighting/particle setting switches.
   The discovery anchors remain `snap_01_gf2479`, `snap_03_gf7397`, and `snap_04_gf9417` from run
   `20260810-124203`; the failure anchors are `snap_05_gf5415`, `snap_06_gf7654`, and
   `snap_07_gf10026` from `20260810-163044`. The latter run corrected the fireball matcher from
   non-live visual values to `$17/$45EF` and `$18/$4610`, proved `$47/$4F` torches in map `$05`,
   and measured lightning extents at 88px above plus 88px below the actor hot point.
2. Revisit the Bloodpool boss in `$02/$08` using run `20260810-180202` as the correction baseline.
   Accept all six vertical/diagonal and long/medium/short white-gold/amber strike paths, horizontal
   mirroring, the separate floor impact, pause behavior, retirement, and both Graphics switches in
   flat and Diorama presentation.
3. Revisit the second snapshot from `20260810-175403`, using `20260810-184935` as the failed
   alignment baseline, `20260810-190012` as the low-density baseline, and `20260810-190729` as
   the narrow-centreline baseline. Accept exact crescent registration for both `$13/$14` cycles
   and both travel directions, the restrained cold halo, full-height long haze, materializing
   forty-eight-star path, pause and retirement behavior, and both Graphics switches in flat and
   Diorama presentation.
4. Revisit Marahna maps `$05/$04-$08` using the first three snapshots of run
   `20260811-151353` plus snapshots 0-11 of `20260811-221433`. Accept torch registration/cadence
   across subsection transitions and all ten boss-room torches, flame lighting on the `$E047`
   large orb and four cardinal children, both `$4AA1` horizontal and `$4B82` vertical connector
   cycles, and all four boss stages: hand charge, central orb, left/right diagonal descent, and
   the three-frame left/right ground-riding charge. Confirm `$34/$4BE5` moving platforms and the
   `$E0BA` reaper orb remain undecorated, then accept pause/retirement, flat/Diorama registration,
   and both Graphics switches.
5. Cast simulation Rain and Sunlight once. Confirm the class-3 `$0503` transitions and the
   `$923E=0..140` sunlight ramp before implementing their distinct weather/scene-light styles.
6. Capture one Red Demon attack to visually accept its elevated red flame attachment, and one Skull
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
