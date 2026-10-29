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
| `$01` | `$CD0C` | 8 | Authored town-person scripts; observed on Aitos's injured man; shares the town-actor state machine |
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

### Aitos's injured-man actor

The dying-man scene uses class `$01` at the first slot of the `$0F0C` pool
in the checked original-ROM sequences. Its authored program is
`$0A:DB0C` (JP `$04:AB93`): two pose selections separated by waits of 120
actor updates, looping back to the start. The pictures are single-part
compositions `$01:E85C/$E892` (JP `$E7E6/$E81C`), using tiles `$A0/$A1`.

The actor's `+$22` wait is separate from the event timer `$7F:9171`.
A mismatch between scene and event flags repeatedly recreates the actor
while event 2 is running, interrupting the pose cycle. Both the full timeout
and early-Rain completion paths still work in the five-ROM controlled tests.
See [the flag ownership, program and live comparisons](regional-differences-technical.md#aitos-dying-man-scene-flag-mismatch)
before treating a recreated slot as a new story event or changing its behavior.

## Ordinary world visual identities

Visual IDs are grouped as follows:

| Visual IDs | Identification |
|---|---|
| `$00` | Control/sentinel entry (`$831C`), not a normal composition |
| `$01-$08` | Blue Dragon bodies and directional frames |
| `$09-$0B` (`$E1BD/$E209/$E255`) | Building-zap lightning animation |
| `$0C` | Single transparent tile; no references in the 52 world-behavior programs |
| `$0D-$14` | Red Demon body/attack frames |
| `$15-$17` (`$E340/$E35A/$E383`) | Palette-1 Red Demon fire, small through large |
| `$18-$1F` | Napper Bat flight/dive frames |
| `$20-$22` | Skull Head bodies |
| `$23-$25` | Shared explosion/ring sequence |
| `$26` | Single transparent tile; referenced by behaviors `$18/$1A/$1C/$1E`, which the checked selection calls do not choose |
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

### Blank world pictures and unselected programs

The five-ROM follow-up corrects the earlier claim that both blank pictures
had no animation references. Their compositions and pixel data agree after
relocation:

| Visual | US / all PAL composition | JP composition | Tile / palette | Behavior references |
| --- | --- | --- | --- | --- |
| `$0C` | `$01:E2B0` | `$01:E23A` | `$0A8` / 2 | None among the 52 programs |
| `$26` | `$01:E527` | `$01:E4B1` | `$0AF` / 0 | `$18,$1A,$1C,$1E` |

Each is one 8×8 part at `(-56,-56)`. The corresponding 32-byte raw tiles at
file `$069500/$0695E0` are zero in every ROM. All six town scene scripts load
the same `$068000` OBJ bank; an adjacent ordinary Skull picture has nonzero
pixels in that bank. This is source-data evidence, not just a blank snapshot.
It does not exhaust arbitrary dynamic writes to VRAM.

The four programs each contain one visual-`$26` row with duration 1 and zero
X/Y motion, followed by a loop terminator. The program pointer tables are
US/PAL `$01:E099`, JP `$01:E023`; visual tables are `$E7D9/$E763`.
All 56 direct calls to the behavior setter in each ROM were checked from
paired state roots and the shared bat helper. Their immediate choices and
four-direction tables collectively select the other 48 programs, not these
four. The configured US cross-reference agrees with all 56 call sites.
This closes the direct-selector question without claiming that every
hypothetical indirect entry or earlier build has been excluded.

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

## Dog and fertilizer item remnants

The supplied dump's dog `$D3AC` and bag `$D3B2` are **bank-1 composition
addresses**, not item IDs or standalone bitmap offsets. They resolve to
icon families `$44/$45`. Their two menu variants have these references:

| Icon | US / all PAL variant table | US / all PAL script → composition (color; grey) | JP variant table | JP script → composition (color; grey) |
| --- | --- | --- | --- | --- |
| Dog, family `$44` | `$A385` | `$A471 → D3AC`; `$A551 → D65E` | `$A354` | `$A440 → D336`; `$A520 → D5E8` |
| Fertilizer, family `$45` | `$A389` | `$A475 → D3B2`; `$A555 → D664` | `$A358` | `$A444 → D33C`; `$A524 → D5EE` |

Each script has one duration-0 frame followed by `$FD`. Each composition is
one 16×16 part at `(0,0)`, base tile `$1E2` (dog) or `$1E4` (fertilizer),
palette 5 for color or 7 for grey, without flipping. The four constituent
8×8 tiles and each palette are byte-identical across all five ROMs. The
town graphics declaration selects raw OBJ data at file `$068000`; dog tile
rows are at `$06BC40/$06BE40`, fertilizer at `$06BC80/$06BE80`, 64 bytes per
row. Palette residency comes from the town scene, not the composition alone.

Japan's 20-entry inventory-label table `$01:EF48` assigns item 16 to record
`$01:F01F` (family `$44`, text `りっぱなイヌ`) and item 17 to `$01:F02B`
(family `$45`, text `ひりょう`). The text means approximately “fine dog” and
“fertilizer.” US/PAL item tables have no family `$44/$45` entries: both IDs
instead alias item 18's Bomb label and family `$46`. This is a presentation
alias, not equivalent item behavior.

The native inventory-icon builder US `$01:B60F–B692` follows
inventory ID → label pointer → leading family byte → family/variant resolver
`$01:AC36`. Selection update `$01:B5CD–B60A` chooses variant 0 or 1.
JP resolver is `$01:ABF9`, with family table `$01:A1F6` instead of `$01:A227`.
Ranges here are end-exclusive. A broad shared-array crawl can also reach
these scripts by indexing past a neighboring family's first two variants;
that is not evidence that an ordinary menu selects those aliases.

Use-handler slots 16/17 resolve to US/all PAL `$01:9FC2/$9FC3` and JP
`$01:9F91/$9F92`. Each is a single `RTS`. Item 18 starts at the following
address and has its own body; do not attribute that body to the two stubs.

The configured US instruction cross-reference finds no direct operands for
the four composition addresses. Its raw-word scan does find the expected
script operands at `$01:A472/$A476/$A552/$A556`; other ROM-wide word matches
have no established ownership. Generic indexed consumers explain why a
direct-code-reference count of zero does not mean the pictures are unreferenced.

The acquisition check covers 28 `JSL` byte-pattern matches to the town-grant
wrapper per ROM: 27 have a preceding immediate item ID, none 16/17. The one
table-fed case uses 24 lair rewards containing only item IDs 18/20 or the
`$8000` technology marker. The configured US roots independently identify
25 of those 28 call sites; the raw-pattern remainder is not promoted to
proven live code. These checks establish no grant of either item, not a
global proof against indirect calls, direct inventory writes or a normal
acquisition route outside the checked paths. No native playthrough or
injected-item menu test is claimed.

## Dragon's Egg item remnant

Item **12** (decimal) is distinct from the working Ancient Tablet, item 13.
Japan's label table `$01:EF48` points item 12 to record `$01:EFEE`: family
`$40`, followed by `りゅうのたまご` (Dragon's Egg). Item 13 points to
`$01:EFFA`: family `$41`, followed by `ふるいせきばん` (Ancient Tablet).
The Western tables alias both IDs to one label record and family `$41`:
US/European English `$01:F157`, German `$01:F1AA`, French `$01:F161`.

The family `$40` display definitions remain in every release, even where
the inventory table no longer selects them. Family `$41` is the working
tablet's icon. All addresses below are in bank `$01`:

| Icon family | US / all PAL variant table | US / all PAL script → composition (color; grey) | JP variant table | JP script → composition (color; grey) |
| --- | --- | --- | --- | --- |
| `$40`, assigned to Dragon's Egg in JP | `$A375` | `$A461 → D385`; `$A541 → D637` | `$A344` | `$A430 → D30F`; `$A510 → D5C1` |
| `$41`, working Ancient Tablet | `$A379` | `$A465 → D38B`; `$A545 → D63D` | `$A348` | `$A434 → D315`; `$A514 → D5C7` |

Each composition is one 16×16 part at `(0,0)`, without flipping, using
palette 4 for color or 6 for grey. Family `$40` starts at tile `$18C` and
family `$41` at `$1CC` in the town OBJ graphics at file `$068000`. Comparing
the four constituent 8×8 tiles finds two sets of family `$40` artwork:
Japan differs from the four matching Western releases. Family `$41`'s
tile bytes match across all five. These are tile-byte comparisons, not a
claim of identical palettes or a runtime visual test.

Item 12's Use slot resolves to `$01:9EB7` in US/all PAL and `$01:9E8D` in
Japan. Bounded Go disassembly confirms a single `RTS` in every release.
Item 13's body starts at the following address; it is not part of the empty
item-12 handler. As with the dog and fertilizer, a shared Western label does
not imply shared behavior.

Rechecking the five-ROM grant census described above finds no immediate
item-12 grant among its 27 immediate-ID patterns, and no item 12 in the
24 lair-reward entries. The same limits apply: this is not exhaustive proof
against indirect grants or direct inventory writes. No natural acquisition
playthrough or injected-item menu test is claimed. The isolated insertion
checks below cover storage, not menu interaction or discovery.

Japanese names were read from the Go-extracted source catalogue and checked
against the ROM identity and raw text-span hashes. This follow-up verifies
the label pointers, icon compositions, tile bytes and empty handlers; it
does not establish an egg-related event or an earlier implemented effect.

## Inventory storage of the unused items

The lower-level insertion path does not reject item IDs 12, 16 or 17. In all
five ROMs, controlled calls can put them into the first free held-item slot
or grant them to any of the six town inventories. This does not supply a
normal acquisition route: the item IDs are explicit test inputs.

The held-item entry is US/PAL `$01:9201`, JP `$01:9141`; it falls through to
the shared insertion body `$9204/$9144`. The town-grant wrapper is
`$01:A076/$A045`. Held bases are US `$02A2`, JP `$02A1`, PAL `$02A4`;
town bases are `$024C/$024B/$024E`, with a nine-byte stride and eight usable
slots. First/last-free and full-held-inventory cases preserve the ninth byte
and return carry set on failure. IDs 13 and 18 provide working-item controls.
The six-town grant tests also preserve every other town's inventory.

Direct call-pattern searches find only the Take Offering transfer calling
the held-item entry, and only the town-grant wrapper calling the common
inserter. The configured US cross-reference agrees. Its operand-range search
finds no direct writes based within the town/held inventory ranges, but does
not cover generic indexed/indirect writes or bulk save restoration. The
mapped Take Offering code transfers an existing item; it does not invent a
new item ID. No additional normal source for the three remnants was found.
