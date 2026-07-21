# Simulation-mode object catalogue

Status: structurally complete for the simulation-town OAM path; semantic
grounded/flying labels are intentionally provisional where the ROM does not
encode a physical role.

This catalogue is built from ROM tables, not from the objects that happened to
appear in an input recording. The deterministic Fillmore capture is used only
to supply the exact decoded OBJ characters and palette needed to draw the
contact sheets.

## Scope and coverage

The crawl covers every object submitted through the simulation renderer
`$01:ACD9 -> $01:ADAD/$AE6F`:

| ROM system | Table | Complete inventory |
|---|---:|---:|
| Live world-record class dispatch | `$01:B8D0` | 26 classes, 133 state entries |
| Ordinary world behavior programs | `$01:E099` | 52 programs, 173 frame steps |
| Ordinary world visual identities | `$01:E7D9` | 64 identities (`$00-$3F`) |
| Fixed/special spawn-list families | `$01:A227` | 73 lists, 397 variant references |
| Resolved fixed/special animation programs | derived from `$01:A227` | 205 unique scripts |
| Resolved fixed/special compositions | animation leaves | 284 unique compositions |

This does not treat houses, roads, fields, or other BG tilemap structures as
sprite objects. It does include menus, cursors, miracle effects, and status
icons because the original game submits them through the same OAM engine.

The machine-readable catalogue is reproducible with:

```sh
python3 tools/sim_object_catalog.py crawl --json /tmp/sim-object-catalog.json
```

The generated JSON records every dispatch/state address, directly assigned
behavior or spawn-list identity, animation step, composition part, tile,
palette, flip, bounds, and OBJ priority.

## Record classes

The `+$0E` field selects one of these top-level live-record classes. “Unknown”
means the structural identity is known but the gameplay noun has not yet been
proven. It does not mean that the record or its state table is missing.

| Class | Handler | States | Current semantic identification | Initial 3D policy |
|---:|---:|---:|---|---|
| `$00` | `$CD0C` | 8 | Town actor/person family; observed on construction people | grounded candidate |
| `$01` | `$CD0C` | 8 | Shares the town-actor state machine | unresolved |
| `$02` | `$CABD` | 6 | Spawn-list-driven special actor/controller | unresolved |
| `$03` | `$CC38` | 4 | Spawn-list-driven special actor/controller | unresolved |
| `$04` | `$CCDA` | 2 | Spawn-list-driven special actor/controller | unresolved |
| `$05` | `$CA67` | 8 | Special actor/controller | unresolved |
| `$06` | `$CA92` | 8 | Special actor/controller | unresolved |
| `$07` | `$C997` | 8 | Special actor/controller | unresolved |
| `$08` | `$C971` | 2 | Special actor/controller | unresolved |
| `$09` | `$C8F1` | 2 | Observed in Fillmore; semantic noun unresolved | unresolved |
| `$0A` | `$C971` | 2 | Shares class `$08` state machine | unresolved |
| `$0B` | `$C936` | 2 | Special actor/controller | unresolved |
| `$0C` | `$B904` | 0 | Angel special record observed at `$0AE4`; class handler is a no-op because another subsystem drives it | flying, fixed altitude |
| `$0D` | `$B904` | 0 | Second no-op/special record class | unresolved |
| `$0E` | `$C8CD` | 1 | Spawn-list-driven special actor/controller | unresolved |
| `$0F` | `$C8AA` | 1 | Spawn-list-driven special actor/controller | unresolved |
| `$10` | `$C880` | 1 | Spawn-list-driven special actor/controller | unresolved |
| `$11` | `$C7BF` | 2 | Town/UI position or direction controller; observed with direction-cursor composition `$D2C4` | UI/overlay |
| `$12` | `$B9EC` | 16 | Blue Dragon | flying; ground-targeted attack effect |
| `$13` | `$BE4F` | 16 | Napper Bat | flying with dynamic dive/carry phases |
| `$14` | `$C237` | 16 | Red Demon | flying |
| `$15` | `$C4E5` | 16 | Skull Head | flying |
| `$16` | `$B92E` | 1 | Spawn-list/event helper | UI/effect candidate |
| `$17` | `$B95F` | 1 | Spawn-list/event helper | UI/effect candidate |
| `$18` | `$B905` | 1 | Timed event helper | non-physical controller candidate |
| `$19` | `$C1C0` | 1 | Timed world effect using behavior `$2C` | effect candidate |

Classes `$12-$15` each have a real 16-entry state table. Their state handlers
change the behavior identity passed to `$D072`, so a render implementation can
key altitude/attachment policy on `(record class, semantic state)` without
inventing a general-purpose Z coordinate.

## Ordinary world visual identities

The complete rendered sheet is generated locally at
`research/sim-object-catalog/world_visual_ids_01.png`. Visual IDs are grouped
as follows:

| Visual IDs | Identification | 3D treatment |
|---|---|---|
| `$00` | Control/sentinel entry (`$831C`), not a normal composition | do not draw |
| `$01-$08` | Blue Dragon bodies and directional frames | flying actor |
| `$09-$0B` (`$E1BD/$E209/$E255`) | Building-zap lightning animation | ground-targeted effect at the selected building tile |
| `$0C` | Unreferenced one-part identity; blank in captured tiles | unresolved |
| `$0D-$17` | Red Demon bodies and fire/attack frames | flying actor or attached effect |
| `$18-$1F` | Napper Bat flight/dive frames | flying/dynamic |
| `$20-$22` | Skull Head bodies | flying |
| `$23-$25` | Shared explosion/ring sequence | effect; no shadow |
| `$26` | Unreferenced one-part identity; blank in captured tiles | unresolved |
| `$27-$2E` | Bat-with-passenger/carrying silhouettes | flying/dynamic; passenger attached |
| `$2F-$33` | Groups of people sprites | grounded group |
| `$34-$36` (`$E6CA/$E6D0/$E6D6`) | Ground fire animation | map-height ground effect |
| `$37-$39` | Blue orb effect sequence | effect; no shadow |
| `$3A-$3C` (`$E71B/$E73A/$E75E`) | Napper plucking people from the ground | semi-grounded/near-ground dynamic phase |
| `$3D`, `$3F` | Skull Head alternate/helper frames | flying |
| `$3E` | Fire frame | effect; no shadow |

Every drawable ordinary visual identity uses OBJ priority 0 on every part.
The same is true for all 284 spawn-list compositions. Priority therefore
cannot distinguish people, monsters, the angel, cursors, or effects.

## Spawn-list/special composition groups

The exhaustive special sheets are generated as
`research/sim-object-catalog/spawn_compositions_01.png` through `_05.png`.
They contain these broad groups:

| Composition range | Visible family | Initial 3D treatment |
|---|---|---|
| `$A627-$A792` | Angel directional and pose frames | flying, fixed altitude |
| `$D128-$D22D` | Miracle/menu icons | UI/overlay |
| `$D233-$D302` | Direction and position cursors | UI/overlay; switch to top-down selection view |
| `$D32B-$D6xx` | Town status/thought icons, including disabled variants | UI/overlay |
| `$D687-$D988` | Angel action, arrow, cloud, fire, and hourglass effects | split actor/attached effect/overlay by script; the arrow override below is authoritative |
| `$D967/$D972/$D97D/$D988` | Angel-arrow vertical A/B and horizontal A/B compositions | flying projectile at angel height; record-origin anchor; no shadow |
| `$E85C-$E93x` | Town-person animation frames | grounded at map height |
| `$E940-$E961` | Horse animation: two frames facing right, then two facing left | grounded |
| `$E96C-$E97E` | Dog animation: two frames facing right, then two facing left | grounded |
| `$E984-$E996` | Sheep animation: two frames facing right, then two facing left | grounded |
| `$E99C-$E9C6` | Eight sailboat frames: two each facing down, up, right, and left; the paired animation changes the sail | water plane; no flight shadow |
| `$E9CC-$EAEC` | Lightning and struck-ground/house effects | ground-targeted effect; no independent altitude |
| `$EBE8-$EC09` | People/horse scene metatiles | grounded scene composite, not a flying actor |
| `$EC14-$EC35` | Angel cloud tiles used by rain, thunder, and other miracles | flying effect |
| `$EC40-$EDF8` | Large aggregate strips/grids and event effects | effect/controller, not one physical actor |

The sailboat frame order is:

| Direction | Sail frame 1 | Sail frame 2 |
|---|---:|---:|
| Down | `$E99C` | `$E9A2` |
| Up | `$E9A8` | `$E9AE` |
| Right | `$E9B4` | `$E9BA` |
| Left | `$E9C0` | `$E9C6` |

The animal frame order is:

| Animal | Right frame 1 | Right frame 2 | Left frame 1 | Left frame 2 |
|---|---:|---:|---:|---:|
| Horse | `$E940` | `$E94B` | `$E956` | `$E961` |
| Dog | `$E96C` | `$E972` | `$E978` | `$E97E` |
| Sheep | `$E984` | `$E98A` | `$E990` | `$E996` |

## Classification model for the renderer

The catalogue should use a small semantic enum rather than infer altitude from
OAM attributes:

```text
grounded
grounded_group
flying_fixed
flying_dynamic
flying_projectile
water_plane
attached_passenger
ground_scene_metatile
ground_targeted_effect
ground_effect
semi_grounded
flying_effect
world_effect
ui_overlay
unresolved
```

For ordinary motion, `+$1A/+$1C` are planar X/Y velocities applied directly to
`+$0A/+$0C`; they are not altitude. A fixed flight plane is therefore enough
for the angel and ordinary enemy travel. Only explicitly mapped state changes
need a depth curve or attachment rule:

- Every enemy class (`$12-$15`) is a flying type. Napper Bat dive/carry states
  move between the normal flight plane and near-ground staging;
  the carried person follows the bat and must not independently become a
  grounded or floating actor.
- The angel arrow is its own world record at `$0B0A`. Project its record X/Y
  through the town ground transform, then apply the same fixed virtual height
  as the angel. Anchor its billboard at the record origin and do not give it a
  shadow. Its `+$1A/+$1C` velocity, collision, and `$01:B473` lifetime/culling
  remain authentic gameplay behavior, not presentation inputs to rewrite.
- Blue Dragon remains on its flight plane while compositions `$E1BD/$E209/$E255`
  are projected to the selected building/ground position.
- Napper compositions `$E71B/$E73A/$E75E` are the semi-grounded plucking phase.
- Fire compositions `$E6CA/$E6D0/$E6D6` are grounded effects.
- Selection cursors and miracle targeting are screen/world overlays and should
  trigger the top-down camera transition rather than participate in shadows.
- All non-enemy world graphics—people, groups of people, animals, boats, and
  scene metatiles—are anchored at map height. Boats use the same map-height
  anchor with a water-plane presentation policy. Ground contact shadows remain
  an art-direction choice.

## Human-reviewed classification reference

The actor-like visual review is complete.
`research/sim-object-catalog/classification_review_01.png` is retained as a
fully labeled reference for the people groups, horses, dogs, and sheep. The
labels use stable visual IDs or composition addresses, and the classifications
are also emitted in the machine-readable crawl output.

## Reproduction

Create an exact live snapshot at any stable simulation-town frame:

```sh
AR_HEADLESS=1 \
AR_INPUT_REPLAY=saves/simdev.rec \
AR_VRAMDUMP_GF=1000 \
AR_QUIT_FRAMES=1001 \
./build-release/ActRaiserRecomp ar.sfc --config config.ini
```

Then render every statically discovered identity with the bundled Python
runtime (or any Python containing Pillow):

```sh
python3 tools/sim_object_catalog.py render \
  --snapshot runs/<run>/snapshots/vd_gf1000 \
  --out-dir research/sim-object-catalog
```

`AR_SIMCAT=1` adds a read-only live probe at the composition-emission seam. It
logs only stable `(record class, semantic state, composition)` changes and is
useful for correlating a future recording with this static catalogue; it is not
required for completeness.
