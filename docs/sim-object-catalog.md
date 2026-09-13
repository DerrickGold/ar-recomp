# Simulation-mode object catalogue

This catalogue describes the US ROM's simulation-town OAM records. Object
identities come from ROM tables; uncertain gameplay labels are marked below.

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

| Class | Handler | States | Current semantic identification |
|---:|---:|---:|---|
| `$00` | `$CD0C` | 8 | Town actor/person family; observed on construction people |
| `$01` | `$CD0C` | 8 | Shares the town-actor state machine |
| `$02` | `$CABD` | 6 | Spawn-list-driven special actor/controller |
| `$03` | `$CC38` | 4 | Spawn-list-driven special actor/controller |
| `$04` | `$CCDA` | 2 | Spawn-list-driven special actor/controller |
| `$05` | `$CA67` | 8 | Special actor/controller |
| `$06` | `$CA92` | 8 | Special actor/controller |
| `$07` | `$C997` | 8 | Special actor/controller |
| `$08` | `$C971` | 2 | Special actor/controller |
| `$09` | `$C8F1` | 2 | Observed in Fillmore; semantic noun unresolved |
| `$0A` | `$C971` | 2 | Shares class `$08` state machine |
| `$0B` | `$C936` | 2 | Special actor/controller |
| `$0C` | `$B904` | 0 | Angel special record observed at `$0AE4`; class handler is a no-op because another subsystem drives it |
| `$0D` | `$B904` | 0 | Second no-op/special record class |
| `$0E` | `$C8CD` | 1 | Spawn-list-driven special actor/controller |
| `$0F` | `$C8AA` | 1 | Spawn-list-driven special actor/controller |
| `$10` | `$C880` | 1 | Spawn-list-driven special actor/controller |
| `$11` | `$C7BF` | 2 | Town position/direction controller; observed with direction-cursor composition `$D2C4` |
| `$12` | `$B9EC` | 16 | Blue Dragon |
| `$13` | `$BE4F` | 16 | Napper Bat |
| `$14` | `$C237` | 16 | Red Demon |
| `$15` | `$C4E5` | 16 | Skull Head |
| `$16` | `$B92E` | 1 | Spawn-list/event helper |
| `$17` | `$B95F` | 1 | Spawn-list/event helper |
| `$18` | `$B905` | 1 | Timed event helper |
| `$19` | `$C1C0` | 1 | Timed world effect using behavior `$2C` |

Classes `$12-$15` each have a real 16-entry state table. Their state handlers
change the behavior identity passed to `$D072`, so a render implementation can
key altitude/attachment policy on `(record class, semantic state)` without
inventing a general-purpose Z coordinate.

## Ordinary world visual identities

Visual IDs are grouped as follows:

| Visual IDs | Identification |
|---|---|
| `$00` | Control/sentinel entry (`$831C`), not a normal composition |
| `$01-$08` | Blue Dragon bodies and directional frames |
| `$09-$0B` (`$E1BD/$E209/$E255`) | Building-zap lightning animation |
| `$0C` | Unreferenced one-part identity; blank in captured tiles |
| `$0D-$14` | Red Demon body/attack frames |
| `$15-$17` (`$E340/$E35A/$E383`) | Palette-1 Red Demon fire, small through large |
| `$18-$1F` | Napper Bat flight/dive frames |
| `$20-$22` | Skull Head bodies |
| `$23-$25` | Shared explosion/ring sequence |
| `$26` | Unreferenced one-part identity; blank in captured tiles |
| `$27-$2E` | Bat-with-passenger/carrying silhouettes |
| `$2F-$33` | Groups of people sprites |
| `$34-$36` (`$E6CA/$E6D0/$E6D6`) | Runtime-built ground-fire animation; emitted palette 1 is scripted red, palette 2 is post-Lightning blue |
| `$37-$39` | Blue orb effect sequence |
| `$3A-$3C` (`$E71B/$E73A/$E75E`) | Napper plucking people from the ground |
| `$3D`, `$3F` | Skull Head alternate/helper frames |
| `$3E` | Fire frame |

Every drawable ordinary visual identity uses OBJ priority 0 on every part.
The same is true for all 284 spawn-list compositions. Priority therefore
cannot distinguish people, monsters, the angel, cursors, or effects.

## Spawn-list/special composition groups

The special compositions include these groups:

| Composition range | Visible family |
|---|---|
| `$A627-$A792` | Angel directional and pose frames |
| `$D128-$D22D` | Miracle/menu icons |
| `$D233-$D302` | Direction and position cursors |
| `$D32B-$D686` | Town status/thought icons, including disabled variants. the range holds 79 entries, of which **75 are the 16x16 bubbles** (63 single-part, 12 four-part) and **four are 64x64 eight-part map selector squares** — `$D4E5`, `$D4FA`, `$D538`, `$D576` — structurally identical to the `$D233-$D302` cursor family. Those four lie on the map. Measured from OAM, a bubble is a 16x32 stack anchored at its TOP and the ROM bounces it about 3px |
| `$D4F6/$D4F8/$D4FA` | **NOT compositions — bank-`$03` step-PROGRAM addresses.** A structure record whose visual step-machine slot at `$7F:77E7 + idx*8` holds one of these is showing the destroyed-house marker; healthy houses hold `$D5EF`/`$D61F`-range programs. They share no numbering with the bank-`$01` composition table above, and the collision with `$D4FA` there — which is one of the selector squares — is a coincidence, not a shared identity |
| `$D687-$D988` | Angel action, arrow, cloud, fire, and hourglass effects |
| `$D967/$D972/$D97D/$D988` | Angel-arrow vertical A/B and horizontal A/B compositions |
| `$E85C-$E93x` | Town-person animation frames |
| `$E940-$E961` | Horse animation: two frames facing right, then two facing left |
| `$E96C-$E97E` | Dog animation: two frames facing right, then two facing left |
| `$E984-$E996` | Sheep animation: two frames facing right, then two facing left |
| `$E99C-$E9C6` | Eight sailboat frames: two each facing down, up, right, and left; the paired animation changes the sail |
| `$D993` | 64x64 hollow path/area selection square (palette 6) |
| `$D9E5-$DCD2` | Miracle cloud family (see below) |
| `$E9CC/$EA27/$EA82/$EAEC` | Town-creation lightning frames initialized by selector `$0504`; live records are world process `$000E` with script base `$A8BB` in raw `+$06`; `$E527` is the interleaved offscreen gap |
| `$EBE8-$EC09` | People/horse scene metatiles |
| `$EC14-$EC35` | Angel cloud tiles used by rain, thunder, and other miracles |
| `$EC40/$EC6E` | Two 48×48 navigation Sky Palace frames, each a 3×3 grid of 16×16 parts. `$A7C5` alternates these at 96 ticks per frame. Their tile IDs require the navigation atlas/palette, not the town snapshot |
| `$EC9C-$EDF8` | Remaining large aggregate strips/grids and event effects |

The sailboat frame order is:

| Direction | Sail frame 1 | Sail frame 2 |
|---|---:|---:|
| Down | `$E99C` | `$E9A2` |
| Up | `$E9A8` | `$E9AE` |
| Right | `$E9B4` | `$E9BA` |
| Left | `$E9C0` | `$E9C6` |

**Who picks these frames.** For scenery actors the selector is `$01:CF0A`: world-record
`+$0F` (the scenery *kind*, high byte of the pending-type word `$7F:7CA1`) indexes the 9-row
table `$01:CF2B` **by byte**, so the row is `kind/2` — kind 0 people, 2 horse, 4 dog,
**6 sheep**, 8 boat, 10 burning-house flame, 12 the `$DD3F` family. The chosen byte is a
variant index into the spawn-list-6 array `$01:A91C`, packed as `ORA #$0600` and staged
through `$01:CFF2`. Kinds come from the `$0A:C800` scenery records.

The animal frame order is:

| Animal | Right frame 1 | Right frame 2 | Left frame 1 | Left frame 2 |
|---|---:|---:|---:|---:|
| Horse | `$E940` | `$E94B` | `$E956` | `$E961` |
| Dog | `$E96C` | `$E972` | `$E978` | `$E97E` |
| Sheep | `$E984` | `$E98A` | `$E990` | `$E996` |
