# Simulation command menu: source reference

This reference maps the original town command menu, its icons, dialogue, and
action boundaries for modding and data extraction. Findings were checked
against the USA ROM and source tree at `2407b6d` on 2026-09-19. Unless specified
otherwise, program/data addresses below are in bank `$01` and WRAM is in bank
`$7E`. For the optional modern interface, see the
[player manual](manual.md#modern-sim-menu).

The six-town menu has **six categories and 15 actions**. The offering selector
has **20 item identities**, with eight usable inventory slots. The Sky Palace
and temple use related code but are separate menu contexts. Keeping those
contexts explicit is necessary when changing a shared menu or text routine.

Shared references carry the reusable findings: [ROM tables](rom-map.md#town-command-menu-tables-usa),
[WRAM/record ownership](ram-map.md#town-command-state),
[routine names and return contracts](research-symbol-map.md#town-command-navigation-and-offerings),
and [dialogue/selector boundaries](dialogue-system.md#town-command-dialogue-and-selectors).
This document supplies the complete per-command and per-item inventory.

## Reproduce the catalogue

```sh
python3 tools/sim_menu_catalog.py --rom ar.sfc \
  --out development/research/sim-menu/catalog.json
```

The [catalogue tool](../tools/sim_menu_catalog.py) verifies the ROM against the
existing USA text-route SHA-256, then resolves category/action selection bytes,
fixed-record anchors, icon families, both palette variants, animation scripts,
composition parts, item handlers, and 65 related dialogue routes. It accepts a
512-byte copier header. It exports metadata, not retail dialogue or pixels.
The result is research data, not a runtime configuration or portable ROM ABI.

For local wording and native control tokens, use the existing Go extractor:

```sh
build/actraiser-builder localization-extract --rom ar.sfc \
  --format catalog --out /tmp/actraiser-sim-menu-catalog.json
```

Use a fresh output path. Inspect `catalog.messages`,
`catalog.menu_source_catalog.segments`, and
`consumer_census.fixed_composer_sources`. Do not decode the whole menu bank as
dialogue: it interleaves code, pointers, icon IDs and text. For disassembly,
`v2regen disasm 01:8290 --rom ar.sfc --metadata '' --mx 1,0 --count 55`
shows the Lightning handler. Linear disassembly does not restore widths after
arbitrary `PLP`/calls or distinguish data automatically; restart at a proven
entry with its actual M/X widths.

## Entry, selection and dispatch

| Address | Responsibility / contract |
| --- | --- |
| `$8000` | SIM interaction entry. Routes scene 7 to Sky Palace, 8 to temple. Town interaction requires DP `$A1 & $80`, `$7F:9750 == 0`, and boss-rush progress `$0347 != 7`. |
| `$8170` | Town-menu owner. Resets angel input/pose with `$B1C7`, caches town state into `$7F:9217`, performs contextual lair inspection through `$03:BB11`, and opens the menu if that inspection does not consume the action. |
| `$81D7` | Dispatches native action IDs 1–15 in A. Common live path is M=1, X=0, DB=1. This is a native JSR/RTS contract, not a host-callable C action API. |
| `$8B7D` | Shared category/command navigation. X points at the selected byte, Y at the label descriptor; `$0338` is the menu-list base, `$033A` retained selection. Returns C=0 with action ID in A, C=1 on cancel. |
| `$8C43` | Runs `$9284`, then reads DP `$A1`. Menu navigation needs repeated release/press input, not an arbitrary delay. |
| `$8C49` | Resolves a category or action label, then calls fixed composer `$02:BF60`. |
| `$B52F` | Updates visibility and selected/unselected animation variants for menu fixed records. |
| `$8D92` | Shared Yes/No modal. C=1 means Yes; C=0 means No/cancel. Initially selects Yes. Its C convention differs from menu navigation. |
| `$8CF0` / `$8CE6` | Eight-position inventory selector / entry with a caller-provided inventory base. Returns C=0 and slot index in A on selection, C=1 on cancel. |
| `$8CB6` | Menu close: clears text, hides 21 command records and restores the screen configuration, including per-frame work. |
| `$8CCE` | Clears BG3 `$7F:B100–B7FF`; leaves the top four status rows. |
| `$9270` | Waits for release of both native menu face-button masks (`$A1 & $C0`). |
| `$9284` | Required menu frame service: `$0192B7`, `$01ACD9`, VBlank. Preserve this work while browsing or explaining. |

In the town owner, C=1 from an action redraws the retained command selection;
C=0 returns to SIM/transition processing. Successful miracle use, rejection at
Yes/No, and cancelling a target generally exit the menu; insufficient SP or
the initial town-state gate return to the command menu.

`$03:BB11` is **lair inspection**, not temple detection. It checks the angel's
position against a lair and displays the corresponding enemy/count message.
Preserve this contextual interaction when intercepting menu opening.

### Native list format

`$F32E–F349` contains the navigation list. Each byte's high nibble is its
category (0–5); a zero low nibble is the category node, otherwise the low nibble
is the action ID. `$FF` separates groups; a second `$FF` terminates the list.
`$F34A` is the packed label destination `$0512` (column 18, row 5).
`$F34C` holds 15 action-label pointers; `$F36A` holds six category-label pointers.

Native Up/Down changes category rows; Left/Right moves between that category
and its commands. Confirming a category node displays the generic command
selection instructions at `$F699`, not an action description. An action's
encoded ID is distinct from its position on screen.

## Complete town action inventory

`sim.menu.` is the prefix of every label suffix in this table. The native
Use Offering action is the entry corresponding to giving/using a held item.

| ID | Category | Label suffix | Handler | Existing action flow |
| ---: | --- | --- | --- | --- |
| 1 | Movement | `return_to_palace` | `$822E` | Set next scene `$1A=7`, run `$03:8168`, exit. No separate description or Yes/No. |
| 2 | Movement | `sky_palace_movement` | `$8239` | Set next scene `$1A=9`, run `$03:8168`, exit. |
| 3 | Direct the People | `building_direction` | `$8244` | Direction prompt; button-dependent cancel branch; `$94B0` development/picker flow; completion/cancel dialogue. |
| 4 | Direct the People | `listen` | `$8284` | Next scene 8, temple action `$033E=1`; listen to the current town event. |
| 5 | Miracles | `lightning` | `$8290` | Description → town gate → SP gate → Yes/No → target prompt → picker → debit/effect. |
| 6 | Miracles | `rain` | `$82FB` | Same sequence. |
| 7 | Miracles | `sun` | `$8366` | Same sequence. |
| 8 | Miracles | `wind` | `$8431` | Description → town gate → SP gate → Yes/No → debit/effect; no player target picker. |
| 9 | Miracles | `earthquake` | `$83D1` | Same sequence as Wind; no player target picker. |
| 10 | Offerings | `take_offering` | `$8491` | Check held-item capacity; next scene 8, temple action 2; temple inventory/transfer flow. |
| 11 | Offerings | `use_offering` | `$84B2` | Empty/choose prompt → held-item selector → `$9C6E` item-specific handler. |
| 12 | Status | `status_master` | `$8530` | `$899B` report; displays magic/item icons and can advance to the score view. |
| 13 | Status | `status_cities` | `$853B` | Refresh `$03:BF8C`, display `$8A3F` cities report. |
| 14 | Other | `progress_log` | `$854A` | `$03:8168`, then `$8A9A` save confirmation, save and continue-playing confirmation. |
| 15 | Other | `message_speed` | `$8559` | `$8AF5` prompt and 0–9 selector; accepted value writes `$0200`; sample/cancel message. |

Status text sources are `$F484` (Master), `$F4DC` (cities), `$F5BC` (scores).
Use the typed live-value resolver in
[`actraiser_localization_values.c`](../src/actraiser/actraiser_localization_values.c)
and existing `status.report.*` contracts instead of parsing their displayed
numbers. Master/score/cities are standalone native screens, with their own
composition, acknowledgement and cleanup. The cathedral owns its separate
offering-transfer menu; Use Offering owns the held-item selector.

The status/save/speed wrappers `$8530/$853B/$854A/$8559` each close through
`$8CB6`, wait for release through `$9270`, and return C=0. Their native return
path closes the town menu.

## Miracle description, confirmation and execution seams

All source and continuation columns are bank `$01`. The localization route's
`caller_pc24` is the **instruction after the JSR**, not the JSR opcode address.
Semantic IDs use `sim.miracle.<name>.<role>`.

| Miracle | Kind | SP | Description source / continuation | Confirm source / continuation | Target prompt source / continuation | Insufficient SP source |
| --- | ---: | ---: | --- | --- | --- | --- |
| Lightning | 1 | 10 | `$FC9C / $8296` | `$FD15 / $82AF` | `$FCE8 / $82BA` | `$FCCE` |
| Rain | 2 | 20 | `$FD25 / $8301` | `$FDB9 / $831A` | `$FD8E / $8325` | `$FD6F` |
| Sun | 3 | 30 | `$FEDC / $836C` | `$FF57 / $8385` | `$FF26 / $8390` | `$FF0C` |
| Wind | 5 | 80 | `$FDC8 / $8437` | `$FE2A / $8450` | none | `$FDE2` |
| Earthquake | 4 | 160 | `$FE3A / $83D7` | `$FEC7 / $83F0` | none | `$FE6A` |

The native descriptions explain destructible terrain/buildings for Lightning,
crop restoration for Rain, drying marshes/melting snow for Sun, removal of
flying creatures for Wind, and changes to the continent for Earthquake.
These are existing wording summaries, not a complete mechanical specification
of every effect or quest interaction. Detailed new help should also document
range, building damage, and special town effects from the effect handlers.

The source sequence is established by `$8290–8490`, not inferred from labels:

1. Description invokes `$8E29` and normally ends with terminal acknowledgement.
2. `$7F:9217` must be nonzero. It is captured from
   `$7F:6B18[$7F:7BFB]` at menu entry, not an independently named miracle unlock.
3. Compare 16-bit current SP `$0282` with the cost.
4. Confirmation invokes `$8E29`, yields with token `$01`, then calls `$8D92`.
5. Lightning/Rain/Sun invoke `$9754` after their target instruction. Cancellation
   returns before debit. Wind/Earthquake proceed directly.
6. `$03:CA5E` debits SP, then `$97E5` runs the selected miracle kind. A failed
   effect can report that nothing happened through `$FC7E` after spending SP.
7. Preserve effect completion, sprite cleanup, angel restoration and button release.

`$97E5` sets `$7F:90EB` kind and `$90E9` user-operation flag, resets the
completion/result fields, initializes through `$9898`, then pumps
`$9460/$99BE` until `$90F3` is set. `$90F7` is consulted for the result;
`$90F5` distinguishes posted/scripted effects. Calling `$97E5` alone bypasses
the caller's checks, confirmations and debit; it is not a replacement Use API.

Three tempting shortcuts are incorrect:

- Globally skipping `$8E29` also removes story, error and confirmation text.
- Empty translations suppress ink but retain native control/wait behavior.
- Changing message speed changes reveal pacing and clear/scroll behavior;
  it does not remove acknowledgements or create a separate Help action.

See [dialogue-system.md](dialogue-system.md) for interpreter controls.
Description streams start with `$05` (cursor/window reset); insufficient-SP
streams start with a line break and can append to the existing window. A skip
adapter must preserve or replace that window initialization, otherwise the
error can append to stale text. Terminal `$00`, menu yield `$01`, page break
`$02`, and reset `$05` have different contracts.

Some existing route names overstate their role: `$FC4F`, used by the initial
town gate, is an empty terminal source even where the route is named
`target_cancel`/`cancel`. Building Direction's `cancel_confirm` source
`$FAEE` ends with `$00` and is not a Yes/No modal. The code, terminal token and
caller continuation must establish a skip policy, not the semantic ID alone.

Building Direction specifically tests A bit `$40` after its description at
`$824A`. Bypassing it must return an explicit proceed result; a stale cancel
button could otherwise select the wrong branch. Inventory prompts and native
Yes/No loops also require release barriers to prevent a single press from
selecting an item or accepting Yes twice.

### Native menu state

These addresses are consumer-scoped; shared scratch values only have these
meanings while the corresponding menu or action owns them.

| State | Address |
| --- | --- |
| Map group / current map; requested next map | `$0018/$0019`; `$001A/$001B` |
| Active location / zero-based cached menu location | `$0341` / `$033F` |
| Menu-list base / selected list-node pointer | `$0338/$033A` |
| Animation family / selected variant scratch | `$033C/$033D` |
| Temple interaction (0 Give Oracle, 1 Listen, 2 Take Offering) | `$033E` |
| Held input word / input enable mask | `$00A0–A1` / `$00F4–F5` |
| Native text speed / presentation state / cursor | `$0200/$0201/$0202` |
| Current and maximum SP | `$0282/$0284`, 16-bit |
| Menu-entry town-state cache | `$7F:9217` |
| Picker active flag | `$7F:9215`, full word; includes non-miracle pickers |
| Item position-picker coordinates | `$7F:9208/$920A` |
| Confirmed miracle cell X/Y, aligned copies | `$7F:90E1/$90E5`, `$90E3/$90E7` |
| User operation / kind / visual done / actor done / posted operation / result | `$7F:90E9/$90EB/$90F1/$90F3/$90F5/$90F7` |
| Town/held-item inventory bases | `$024C + 9*($0341-1)` / `$02A2`; eight usable slots |
| BG3 map / upload request | `$7F:B000` / DP `$F1` |

Fixed UI records use `+$00` timing, `+$02` script cursor, `+$06` loop/script
base, `+$08` composition, `+$0A/+$0C` anchor, `+$0E` family and `+$10` flags.
The town root occupies the first 21 records. Inventory icons start at `$0898`,
magic icons at `$0850`, Yes/No selector records at `$081A/$082C`, and the
hourglass at `$083E`. These addresses describe ownership better than OAM slot
numbers, which change as sprites are emitted. Record anchors undergo native
screen/camera offsets; they are not final host viewport coordinates.

## Original icon sources

`$AA56` reads scene-specific initialization pointers at `$AB20`. All six towns
use `$AB32`: records of X, Y and animation family (three words), with
`$FFFD` jump, `$FFFE` skip-record and `$FFFF` end controls.
The first 21 entries initialize the six categories and 15 actions in list
order, at fixed records `$06A0–0808`, stride `$12`. These are record addresses,
not fixed OAM indices.

The resolution chain is:

```text
semantic entry → native fixed record +$0E (family)
              → $01:A227[family] → variant pointer array
              → variant 0 or 1 animation script
              → composition → 8×8/16×16 OBJ parts → resident VRAM + CGRAM
```

`$AC36` selects the script using `$033C/$033D`; `$AC70` advances it and writes
the composition to record `+$08`. `$B52F` uses variant 0 for the selected
entry and variant 1 for ordinary entries; other category children are hidden
with record `+$10` bit `$8000`. These are **selection palettes**, not proof of
whether an action is available. Composition priority is insufficient for
identifying UI because ordinary world sprites also use priority zero.

| Entry | Family | Selected composition | Unselected composition |
| --- | --- | --- | --- |
| Movement category | `$09` | `$D128` | `$D3DA` |
| Return to Sky Palace | `$0C` | `$D13A` | `$D3EC` |
| Sky Palace Movement | `$0A` | `$D12E` | `$D3E0` |
| Direct the People category | `$1F` | `$D1BB` | `$D46D` |
| Building Direction | `$20` | `$D1D0` | `$D482` |
| Listen | `$21` | `$D1D6` | `$D488` |
| Miracles category | `$19` | `$D188` | `$D43A` |
| Lightning | `$1A` | `$D18E` | `$D440` |
| Rain | `$1B` | `$D194` | `$D446` |
| Sun | `$1C` | `$D19A` | `$D44C` |
| Wind | `$1E` | `$D1B5` | `$D467` |
| Earthquake | `$1D` | `$D1AF` | `$D461` |
| Offerings category | `$16` | `$D176` | `$D428` |
| Take an Offering | `$17` | `$D17C` | `$D42E` |
| Use Offering | `$18` | `$D182` | `$D434` |
| Status category | `$10` | `$D152` | `$D404` |
| Status of Master | `$11` | `$D158` | `$D40A` |
| Status of Cities | `$12` | `$D15E` | `$D410` |
| Other category | `$13` | `$D164` | `$D416` |
| Progress Log | `$14` | `$D16A` | `$D41C` |
| Message Speed | `$15` | `$D170` | `$D422` |
| Observe the People angel (Describe hint) | `$0B` | `$D134` | `$D3E6` |
| Yes | `$23` | `$D1E2` | `$D494` |
| No | `$24` | `$D1F7` | `$D4A9` |

The last three rows are supplementary artwork, not additional root commands.
The angel uses variant table `$A2C1` and scripts `$A3A1/$A48D`; it is distinct
from the Listen portrait and the world angel. Yes/No use `$A321/$A325`.

Both variants use the same pixel tiles with different palettes (typically
4/5 selected, 6/7 unselected). Some 16×16 icons comprise four 8×8 parts;
do not assume a single tile. The catalogue includes each part's tile number,
palette, dimensions, flags and flips. Reuse
[`SimRenderAtlas_Build`](../src/sim/sim_render_atlas.c) and the runner's
`rasterize_ppu_obj_parts` API as the model for turning parts into owned RGBA
art. [`ActRaiserLocalizationArt_Capture`](../src/actraiser/actraiser_localization_art.c)
only handles small 2bpp BG3 objects; it is not a 4bpp menu-icon decoder.

The live atlas contains submitted/visible objects; it is not a complete static
menu asset library. Rendering previously hidden entries requires resolving
their compositions and resident graphics. Cache by
ROM/scene, tile/palette generation and replacement-asset identity, not merely
item ID. Honor actual OBJ bases, palette, transparency and replacement art.
`tools/sim_object_catalog.py render --snapshot <prefix> --out-dir <directory>`
can render source compositions from local `AR_VRAMDUMP_GF` captures.

## Offerings, stored items and temple flow

`$91D3` computes town inventory base `$024C + 9 * ($0341 - 1)`. `$91E7` counts
eight usable entries; `$9239` compacts them. Held items likewise have eight
usable slots `$02A2–02A9`, followed by the extra terminator/storage byte.
The nine-byte allocation is not nine selectable items. Town counts and other
persistent fields are described in [ram-map.md](ram-map.md#offerings-7e023a-7e0281).

Take Offering enters temple scene 8. `$8810` draws the heading selected by
`$033E`, runs the temple presentation, then routes Listen (1) to `$885E` and
the other ordinary path to `$8898`. The heading set contains Give Oracle (0),
Listen (1), and Take Offering (2); Give Oracle is not a sixteenth root command.
Its event-entry semantics need separate evidence before exposing it as a new
independent action. Scene 8 also has a distinct ending branch at `$8849`.

`$8898` contains a Marahna-specific no-residents check. `$88B5` checks whether
there is an offering. `$88C3` displays the prompt and the town's eight-slot
selector. After selection, `$890F–894E`:

- Removes the selected entry from the town inventory.
- Sends IDs 1–4 to magic ownership through `$90E5`.
- Runs the immediate grant handlers for IDs 5–6.
- Adds other IDs to held inventory through `$9201`.
- For the ordinary receipt path, resolves `$04:C6AE[(item_id-1)*2]` and
  displays `dialogue.offering.slot_XX` through caller continuation `$01:894E`.
- Checks remaining offerings, offers Take More through `$8974/$8D92`, and
  rechecks held-item capacity before repeating.

The transfer occurs **before** receipt dialogue. Reusing that whole flow to
describe an unclaimed item would take it. IDs 5/6 follow their immediate-grant
dialogue instead of the ordinary `$894E` receipt path. Acquisition messages
may also reference their town/event; they are not automatically neutral help.

Held-item Use reaches `$9C6E`, a stacked RTS dispatcher whose table `$9C94`
stores handler address minus one. Preserve its cleanup/unwind contract;
several targeted handlers unwind to `$9C85` rather than returning normally.

| Item ID | Label | Family | Use handler | Observed behavior relevant to the UI |
| ---: | --- | --- | --- | --- |
| 1–4 | Magical Fire / Stardust / Aura / Light | `$25–28` | `$9CBC` | No-op in held-item Use; acquired through magic ownership and equipped through Select Magic in the Palace. |
| 5 | Source of Life | `$39` | `$9CBD` | Immediate life grant; receipt/use acknowledgement, not a confirmation before the write. |
| 6 | Source of Magic | `$3A` | `$9CD6` | Immediate persistent and working MP grant. |
| 7 | Loaf of Bread | `$3B` | `$9CF8` | Position prompt and `$93DC` picker; town/coordinate checks. |
| 8 | Wheat | `$3C` | `$9D6F` | Position prompt and `$93DC` picker; terrain/structure checks. |
| 9 | Herb | `$3D` | `$9E03` | Town-specific use/check/message. |
| 10 | Bridge | `$3E` | `$9E28` | Town/event checks; success changes bridge-related event state. |
| 11 | Harmonious Music | `$3F` | `$9E82` | Event service plus town-specific success/failure. |
| 12 | Ancient Tablet | `$41` | `$9EB7` | No-op Use handler. Keep identity separate from ID 13. |
| 13 | Ancient Tablet | `$41` | `$9EB8` | Town-specific event use. |
| 14 | Magic Skull | `$42` | `$9EE7` | `$9754` position picker, then lair validation/effect; special cleanup. |
| 15 | Sheep's Fleece | `$43` | `$9F97` | Town-specific event use. |
| 16 / 17 | Bomb! | `$46` | `$9FC2 / $9FC3` | No-op Use handlers. |
| 18 | Bomb! | `$46` | `$9FC4` | Consumes item and changes the four monster slots. |
| 19 | Compass | `$47` | `$9FF0` | Town-specific navigation/event use. |
| 20 | Strength of Angel | `$48` | `$A02F` | Consumes item and modifies angel combat state. |

IDs 12/13 share a name/icon but have different handlers and receipt dialogue.
IDs 16/17/18 share a name/icon but only 18 has an active Use body. Preserve
item ID, inventory kind and slot in the command model; label/source-pointer
deduplication loses behavior. The item catalogue records all 20 individually.
Success/failure text inside these handlers is observable gameplay feedback,
not a blanket description-skipping candidate.

### Use Offering handoff contract

**Using a held offering does not enter the cathedral view.** The selected
item's native handler owns all dialogue and effects. Dialogue, consumption,
world selectors and effects occur in different orders; success depends on
native town/event/terrain checks.

The native selection boundary is `$84ED` (`JSR $8CF0`): C=0 and A=slot resumes
at `$84F0/$84FA`, widens the slot into X, reads the live item ID from
`$00:02A2,X`, then calls `$9C6E` at `$8503`. A replacement selector should
preserve that return contract through an audited adapter, including original
setup and stack/register widths, rather than invoke an item body as a host
callback. Validate the selected slot/ID against the current inventory and
session before dispatch; never infer behavior from the shared label/icon.

After dispatch, let native code own all dialogue pages, input waits, errors,
pickers, consumption, effects and cleanup. There is no universal extra Yes/No.
These source-verified examples distinguish the important paths:

| Item / successful path | Native sequence to preserve |
| --- | --- |
| Wheat (8), `$9D6F` | Position instruction `$04:8930` → close menu/remove inventory objects → placement selector `$93DC` → terrain/structure validation → branch-specific feedback/effect → consumption at `$9DF6`. Picker cancel and invalid-location branches do not reach consumption. |
| Loaf of Bread (7), `$9CF8` | Same position instruction → `$93DC` selector → town/coordinate checks → success dialogue `$04:89FE` → consumption and event changes. Failure has separate dialogue; cancel does not use the item. |
| Magic Skull (14), `$9EE7` | Same position instruction → native `$9754` selector → lair validation → native effect → consumption. Keep its distinct picker and special unwind/cleanup. |
| Herb (9), `$9E03` | Town check → success dialogue `$04:8AC2` → consumption at `$9E1A` → event-state write at `$9E1F`. This is a concrete dialogue-before-effect case with no picker. |
| Bridge (10), `$9E28` | Town/event checks → first event-state write at `$9E59` → success dialogue `$04:8B40` → consumption at `$9E65` → further event write at `$9E6E`. The sequence straddles the dialogue. |
| Harmonious Music (11), `$9E82` | Native event/audio service → town branch → consumption at `$9E9B` → **two-page** success dialogue `$04:8C44` → event write at `$9EA6`. Wrong-town feedback follows a separate branch. |
| Ancient Tablet (13), Sheep's Fleece (15), Compass (19) | Town checks → consumption → success dialogue → event-state write. Preserve distinct failures and IDs; Tablet 12 is a different, no-op handler. |
| Strength of Angel (20), `$A02F` | Consumption → combat-state changes → dialogue `$04:8ECC`. The change already precedes the acknowledgement. |
| Active Bomb (18), `$9FC4` | Consumption → native monster-slot changes; no dialogue call in this Use handler. IDs 16/17 are no-ops. |

The ordinary no-op and acquisition-only cases remain as recorded in the item
table above; do not invent a picker, effect or success message for them.

#### Dialogue and completion

Outcome/event dialogue occurs at the points shown above, sometimes before and
sometimes after consumption or state changes. It is not interchangeable with
an item's descriptive or acquisition text.

The shared position instruction `$04:8930` serves Bread/Wheat/Skull; its return
contexts are `$01:9CFE`, `$01:9D75` and `$01:9EEF`. It passes through
`$93A8/$8E29` with catalogue caller `$01:93B2` in the
[runtime dialogue routes](../tools/data/localization/us-runtime-dialogue-routes-v1.json).
`$93A8` also carries effect-adjacent dialogue. Music's two-page success message
must complete before its following event write. Native release/press barriers
separate inventory selection, acknowledgement and target acceptance.

Native `$921B` removes the **first matching item ID** from the eight held
slots; `$9239` compacts them later. This matters when duplicate items occupy
several slots: the selected slot is not necessarily the slot removed.

Normal return at `$8506–851D` compacts, removes inventory objects, clears `$29`,
closes the menu and waits for release before returning C=0. Targeted handlers
also have special unwinds through `$9C85`; these are distinct cleanup paths.

## Adjacent menus and text ownership

The Palace uses list `$F25F`, descriptor `$F270`, action pointers `$F272` and
category pointers `$F290`, also through `$8B7D`. Its dispatcher `$8646` has:

| ID | Action | Handler |
| ---: | --- | --- |
| 1 | Sky Palace Movement | `$866A` |
| 2 | Observe the People | `$8671` |
| 3 | Fight Monsters | `$86A0` |
| 4 | Select Magic | `$8781` |
| 5 | Status of Master | `$87F8` |
| 6 | Status of Cities | `$87FD` |
| 7 | Progress Log | `$8806` |
| 8 | Message Speed | `$880B` |

These use `sky.menu.*` identities. The same action ID has different meanings
in different owners: Palace action ID 5 is Status of Master, while town action
ID 5 is Lightning. The ending, world-navigation and temple contexts also use
shared routines with their own callers and return contracts.

Native UI has three graphical owners: BG3 text, BG2 box/frame tiles, and
fixed-tier OBJ icons/selectors. Hiding BG3 alone leaves panels and icons;
hiding all fixed sprites can also remove the HUD hourglass or target cursors.
Existing text routes identify heading region `(18,5,10,2)` and item-label
region `(18,10,10,2)` in native tile cells.

**Native SIM and the host settings overlay use different Back controls.**
The native menu reads `$A1`: `$80` confirms and `$40` cancels. `$02:AC51–AC56`
copies `$4218 & $F4` to `$A0/A1`; the runner's `SwapInputBits` maps input
bit 0 (SNES B, default Z) to `$8000` and bit 1 (SNES Y, default A) to `$4000`.
The host settings overlay instead uses SNES A/default X for Back, matching
the mapper's generic `A cancel` label. Do not infer native SIM controls from
those host labels.

## Scope and evidence limits

The catalogue verifies asset identities and control-flow boundaries in the
USA ROM. It is not an exhaustive account of every town/event outcome.
In particular, a call to `$9754` does not by itself identify miracle targeting:
item and repeated-use paths share that helper. Interpret it with its caller.
Other retail ROMs have separate extraction profiles; do not apply these
USA addresses to another release.
