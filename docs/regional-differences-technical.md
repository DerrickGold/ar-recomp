# Regional differences: technical evidence

For the plain-English comparison of the releases, read
[ActRaiser: regional differences](regional-differences.md). This companion
preserves the ROM identities, addresses, measurements, qualifications and
integration constraints behind that article. Implementation task tracking
remains separate in the project's private development notes.

This is a reverse-engineering reference, **not a list of implemented settings**.
The comparisons below were checked against the exact headerless retail ROMs
on 2026-09-20. Table/code comparisons were supplemented with 2,964 isolated
native-routine fixtures, 155 booted SIM/action/save traces, and 442 complete controlled
earthquake-matrix transactions in the Snes9x reference core. Booted traces also
include effects; do not count those casts again as separate matrix cases. These establish the
listed behaviors, not complete playthrough equivalence or safe live switching
between profiles. Booted fixtures use unmodified ROMs and separate scratch
states; their RAM setup is not a claim of unassisted playthrough coverage.

| Release | Size | SHA-256 |
| --- | ---: | --- |
| USA | 1,048,576 | `b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0` |
| Japan | 1,048,576 | `3655833fd0fb4985c3dbbf28141f65564f7a228ee92d81a12982ef7cb952a51b` |

Addresses are SNES `bank:address` unless explicitly called file offsets.
Do not use US RAM addresses to interpret Japanese snapshots: fields move by
different amounts in different structures. ROM relocation is not itself a
gameplay change. European releases require separate profiles.

## Reproducing code inspections

The generic disassembler is part of **snesrecomp-go**, not a game-specific
decoder. For example, with the project's locally built executable:

```sh
build/snesbuild disasm -rom ar.sfc -metadata '' -mx 0,0 -raw -count 75 03:C07E
build/snesbuild disasm -rom ar-jp.sfc -metadata '' -mx 0,0 -raw -count 75 03:BD27
```

`-format json` supplies machine-readable instructions. Use explicit entry M/X
widths and bounded, identified routines. Linear disassembly does not recover
caller flags through arbitrary calls/PLP, follow all branches, or distinguish
interleaved data from code. Do not attach US generated metadata to JP code.

## Structured census

- Only banks `$08`, `$09`, and `$1F` are identical at the same offsets.
- The 50 US action-layout roots and 49 JP roots have 49 common keys; US-only
  `$0900` is not evidence of an extra action stage. Of the common roots, 27
  differ after following object-stream jumps: one player start, 20 damage-box
  sequences, and 20 object/wave streams. These categories overlap.
- 22 used region/type spawn records differ in non-pointer fields. Handler
  relocation and unused/direct-handler entries are excluded from this count.
- Six of the 47 corresponding 28-byte video profiles differ, only in a timer
  byte. Both asset scripts contain the same 59 map keys.
- 31 of 49 action-map resource pipelines differ after decompressing and
  comparing CHR/palette/metatile/map/OBJ data; 18 match. This is a resource
  inventory, not 31 independent visual toggles or a complete collision audit.
- 15 of 17 US song images have byte-identical relocated JP copies. Song IDs 9
  and 12 have revised sequence payloads. Eleven map entries have different
  normalized song declarations.

Stable file ranges include town base maps `0x50000–0x52FFF`, water animation
`0x53000–0x530FF`, item graphics `0x32000–0x323FF`, sample banks
`0x40000–0x4FFFF`, and Mode-7 characters `0x70000–0x73FFF`.
Equal base maps do not establish equal development, events, or town capacity.

### Action timer profiles

The profile tables start at US `$02:893E` / JP `$02:87E7`; timer word at `+25`.
Timers are BCD, not binary. Map keys below use `region/room`, not act numbers.

| Profile | Consumers | US | JP |
| --- | --- | ---: | ---: |
| `$03` | Fillmore room 1 | 300 | 200 |
| `$05` | Fillmore room 3 | 200 | 100 |
| `$07` | Bloodpool room 1 | 300 | 200 |
| `$24` | Marahna room 8; Death Heim room 6 | 300 | 200 |
| `$25` | Northwall room 1 | 200 | 100 |
| `$26` | Northwall room 2 | 200 | 100 |

### Changed used object records

The eight pointer roots start at US `$00:95DD` / JP `$00:9605`.
Values below are **decimal**, except type/flags and explicitly marked BCD.
These are type identities within each region, not verified enemy display names.

| Region | Type | Changed field: US → JP |
| --- | --- | --- |
| Fillmore | `$10`, `$11`, `$12` | Attack 1 → 24 |
| Fillmore | `$17` | Attack 1 → 3 |
| Kasandora | `$07` | HP 2 → 3 |
| Kasandora | `$10` | HP 3 → 5 |
| Aitos | `$09`, `$0A` | HP 2 → 3 |
| Aitos | `$0B` | HP 1 → 2 |
| Aitos | `$0C` | Flags `$0000 → $0800`; score BCD `$20 → $00` |
| Aitos | `$16` | Attack 2 → 1 |
| Aitos | `$17` | Attack 2 → 4 |
| Marahna | `$07`, `$0B` | HP 3 → 5 |
| Marahna | `$13` | Attack 1 → 2 |
| Marahna | `$15`, `$17` | Attack 1 → 24 |
| Marahna | `$20` | Attack 1 → 3; HP 3 → 4 |
| Northwall | `$09` | HP 2 → 3 |
| Northwall | `$16` | HP 3 → 5 |
| Northwall | `$1A`, `$1B` | HP 1 → 2 |

The old scanner printed stats in hexadecimal: its `ATK=1/18` meant **24**,
not 18. Damage boxes, hitbox headers, vulnerability flags, animation timings,
and mode-dependent stat promotion remain distinct sources of behavior.

## Action rules in code

| Rule | US evidence | JP evidence |
| --- | --- | --- |
| Magic cost | `$00:9DFE–9E0C`: require MP > 0, decrement one | `$00:9E08–9E18`: subtract equipped spell ID at `$02AB`, reject negative result; IDs 1–4 imply costs 1–4 |
| Magic input | `$00:9843`: `$A0 & $00C0` (A/X) | `$00:9A72`: attack path checks `$A1 & $0008` (Up), then enters cast gate; standing player dispatch first tests attack bit `$40` |
| Respawn score | `$00:981C–9831`: consumes respawn marker `$032C` without clearing score | `$00:9844–985B`: consumes `$031A` and clears the 16-bit score at `$1F` |
| Special-mode initialization | `$02:AB05`, M=1/X=0: stored lives 4, HP 24, MP 0 | `$02:A84B`, M=1/X=0: stored lives 2, HP 24, MP 0 |

Stored lives do not share a display convention: US adds one for the HUD, JP
does not. The BCD decrement is at US `$00:82AE` / JP `$00:82A8`; see the native
[lives follow-up](#lives-convention) for death/display fixtures. Do not assume
US Professional mode equals JP Special mode in other respects.

## Simulation rules in code

| Rule | US | JP | Evidence |
| --- | --- | --- | --- |
| Development service divider | Every master-loop invocation | Every fifth invocation | US `$03:81B8`; JP `$03:81B5–81C0` |
| Long development cycle | Threshold 720 service steps | Threshold 480 service steps (behind divider) | US `$03:81E0`; JP `$03:81EB` |
| Town state-3 timer initialization | 1 | 150 | US `$03:AA9E`; JP `$03:A866`; countdown/reload US `$03:872A`, JP `$03:86DA` |
| Periodic SP queue | `floor(maxSP/10)` to `$7E:0B05` | No equivalent queue setup in JP cycle | US `$03:8271`; JP `$03:828C` starts directly with town services |
| Angel HP recovery | `floor(maxHP/4)` queued per long cycle; drain one each 16 game-frame phases | One HP per 60 eligible recovery calls; excludes angel state 4 | US `$03:8283`, `$01:B257`; JP `$01:9BFD` |
| Periodic SP drain | One queued SP each 4 game-frame phases | No corresponding drain in JP normal angel handler | US `$01:B26D–B280`; JP `$01:B222` onwards |
| Earthquake house selection | House subtype `$20` preserved; other subtypes use destruction path | RNG helper result `< $80` uses destruction path, otherwise preservation path | US `$03:A066`; JP `$03:9E16` |

World-actor passes are **outside** JP's development divider (US `$03:8207` /
JP `$03:8212`). Pauses, effects, and menu ownership can gate entry. Raw service
counts must not be presented as universal real-time delays.
The earthquake RNG helper reduces modulo its argument (`$FF` here); do not
silently replace it with an unrelated host coin flip.

Both normal angel shooting gates reject zero HP: US `$01:B296–B29B`, JP
`$01:B23D–B242`. A claim that JP simply permits shooting at zero HP is not
supported by these paths. Recovery timing could explain a different impression.

### Miracle costs

Each price is encoded in both an affordability comparison and a debit load,
not one contiguous ROM table. Both must agree with the displayed menu price.

| Miracle | US SP | JP SP | US gate/debit | JP gate/debit |
| --- | ---: | ---: | --- | --- |
| Lightning | 10 | 12 | `$01:82A2 / 82C2` | `$01:8269 / 8289` |
| Rain | 20 | 16 | `$01:830D / 832D` | `$01:82D4 / 82F4` |
| Sunlight | 30 | 18 | `$01:8378 / 8398` | `$01:833F / 835F` |
| Wind | 80 | 24 | `$01:8443 / 8458` | `$01:840A / 841F` |
| Earthquake | 160 | 60 | `$01:83E3 / 83F8` | `$01:83AA / 83BF` |

### Town census and level requirements

US `$03:C07E` / JP `$03:BD27` scan 128 structure records. Both sum houses as
4/6/8 people by subtype and publish `house sum + 2 - town bias`.
**Population and support are different outputs.** Non-house support values:

| Structure class | US | JP |
| --- | ---: | ---: |
| Class 2, completed regular/upgraded field | 32 / 48 | 16 / 24 |
| Class 3, completed | 72 | 32 |
| Class 4 | 72 | 32 |
| Other non-house class, including bridges | 32 | 16 |

Classes 2 and 3 contribute zero while their `$40` flag is set (including
withering/stopped-support states; this bit is not universally "building").
Do not replace actual population with support, or derive final town maxima
from these constants alone: allocation, growth, geography and biases matter.

Level thresholds (18 little-endian words, last value a sentinel):

```text
US $03:B40E: 0,80,200,400,700,950,1200,1500,1700,1900,2200,2500,2900,3300,3700,4100,4600,9999
JP $03:B1DF: 0,80,200,400,550,650,750,1050,1400,1600,1800,1900,2000,2200,2400,2600,3000,9999
```

The 18-entry maximum-SP tables (US `$03:B432`, JP `$03:B203`) are identical.
Level checks at US `$03:B3BA` / JP `$03:B18B` award upward, not downward.

### Monster lairs

US `$03:B825` / JP `$03:B5AE`: 24 nine-byte seeds, four per town. Position,
imagery ID, monster class and world-record slot match. Counts differ for 22
records; delays differ for all 24. Order is table order, not a screen order.

| Town | US counts (slots 1–4) | JP counts | US delays | JP delays |
| --- | --- | --- | --- | --- |
| Fillmore | 200,100,100,100 | 250,125,125,100 | 1,1,1,1 | 75,150,150,150 |
| Bloodpool | 100,50,90,90 | 150,50,100,100 | 1,1,1,1 | 200,100,150,150 |
| Kasandora | 110,125,125,90 | 220,250,250,180 | 1,1,1,1 | 60,200,200,75 |
| Aitos | 80,80,80,80 | 170,90,170,170 | 1,141,1,1 | 75,212,75,75 |
| Marahna | 50,60,50,50 | 200,130,200,70 | 1,1,1,1 | 180,37,75,90 |
| Northwall | 30,30,60,60 | 150,100,150,100 | 100,100,1,1 | 275,275,60,120 |

Spawn countdowns only advance on the native eligible path: population,
available world-record slot, and lair flags participate. They are not simply
wall-clock timers. US spawner `$03:B99C` / JP `$03:B725`.

Lair depletion has at least two paths: monster defeat subtracts one (US
`$03:BB04`, JP `$03:B88D`); a qualifying miracle subtracts ten with saturation
at zero (US `$03:BAC8`, JP `$03:B851`). The remaining-count array is US
`$7F:96B8`, JP `$7F:96AC`. Sealing flags belong to the imagery/state array
(US `$7F:95C8`, JP `$7F:95BC`), **not** the remaining-count array.

## Audio and scope limits

Song 9: US file `0x7769F`, JP `0xC0000`; sequence block 2224 / 2197 bytes.
Song 12: US file `0xCFA4B`, JP `0xD31E1`; sequence block 1326 / 1325 bytes.
Other block destinations/content and their sample scripts match for these two
images. Exact JP sequences are authored media, not just a song-selection rule.

The sole asset-command shape mismatch is scene `$00/$07` (**Sky Palace**).
US has an additional song command with operands `01 04 8F 2B 03`, absent in
JP. The [transition follow-up](#death-heim-transition-and-music) identifies the
music-ID-4 caller; audible scene playback still needs a trace. The action count
does not include non-action resources. Special bit-5 commands are now separately
mapped to US `$02:B363` / JP `$04:836B`: they copy an uncompressed `$0800`-byte
metatile source to scratch before selecting/byte-swapping BG slices. Of 22
matched consumer instances (four distinct source pointers per ROM), eight
consumers differ, representing two shared source pairs: six towns use US file
`0xC981A` / JP `0xC7787`; scenes `$00/$07–08` use US `0xCA01A` / JP `0xC8000`.
These are two source differences, not eight independently identified features;
named visual meaning still requires inspection.

Unresolved semantic areas include actor/boss state machines and hitboxes,
complete population maxima, status-page behavior, Death Heim emergence,
donor-art bundle boundaries, and European difficulty variants. No byte census
alone can prove that no further gameplay differences exist.

## Native-routine and content follow-up

The following results use the same exact ROM hashes. The native fixtures run
unchanged routine bodies and inspected callees from an in-memory research copy
with an isolated reset trampoline. Tests explicitly called *fragments* stop at
a named instruction before unrelated scene/render initialization. They do not
load player SRAM, validate full scene transitions, or emulate host HLE policies.
Code decoding and reference searches use the Go `snesbuild` tools.

### Lair stock is not monotonic

In addition to kills and miracle depletion, the native game **adds** stock:

| Operation | US | JP | Entry / contract |
| --- | --- | --- | --- |
| Destroyed house feedback | 4/6/8 units according to subtype | 4 units | US `$03:B4A6`, JP `$03:B22A`; distribute round-robin across unsealed lairs, restarting at the first slot for each house; if all sealed, credit town growth instead |
| Post-action stock adjustment | Add `argument >> 2` to each of four stocks | Subtract `argument >> 2`, saturating at zero | US `$03:B50F`, JP `$03:B287`; these leaves do not test the sealed flags |
| Score conversion | `2 * floor(score / 10)` | `10 * floor(max(score - 650, 0) / 32)` | US `$03:D0D4`, JP `$03:CD7A`; input is valid four-digit BCD score at `$1F`; output is binary |

The score caller is US `$03:D095` / JP `$03:CD3E`. With the town's completed-act
count equal to two it credits growth; US takes the stock branch only for count
one, JP for any count other than two. At score 1000, the stock adjustment is
therefore US `+50` per lair versus JP `-25`. Scores and destroyed houses must be
part of any dual-profile progress journal; `initial stock - kills` is not enough.
An exhausted but unsealed lair can gain stock again. Do not clamp to its seed.

The qualifying miracle path (US `$03:BA42`, JP `$03:B7CB`) skips flags `$C000`,
the `$90F5` busy gate and zero stock. Its depletion branch subtracts ten, but
**the growth credit is not always ten**: `$03:B54E` / JP `$03:B2DA` uses `ADC`
without clearing carry. For ordinary nonnegative stock, credit is eleven when
the subtraction does not borrow (stock >= 10), ten when it overshoots (1–9).
Seventy-two native fixtures cover this distinction, zero, both flag gates and
busy state. Preserve the CPU flag contract; do not casually normalize this
helper to a host `growth += argument`.

Kill fixtures cover all 24 slots, including zero and presealed cases. The leaf
only decrements nonzero stock; it does not award growth or modify sealing flags.
US `$03:B5BD` / JP `$03:B349` owns guidance sealing, including the population-10
gate and flag `$8000`. The all-sealed test at US `$03:B692` / JP `$03:B41B`
reads those flags, not stocks. The native helper called at US `$03:B560` / JP
`$03:B2EC` is a bare `RTS`; adjacent code must not be attributed to that call.

A delay mutator at US `$03:B6BF` / JP `$03:B448` sets each current-town reload
to `(delay >> 2) + 1`. Its ordinary gameplay caller has not been established;
do not expose it as a confirmed regional rule or overwrite potential event
modifications by reapplying seed delays every frame.

### Census, construction and status are separate contracts

The six structure bases are identical in both ROMs:
`$7F:6BE7,6DE7,6FE7,71E7,73E7,75E7`, each 128 records × four bytes.
The 180 census fixtures cover all towns, inactive records, house subtypes,
support classes and `$40` flags. House subtype `$30` falls through to four
people; it is not a fourth ten-person tier. `$40` does not exclude houses from
census, but does suppress class-2/class-3 support. Population output is binary.

The construction support-type tables US `$03:94B1` / JP `$03:929C` have identical
values. Allocation first compares actual population to support; the house path
at US `$03:95BE` / JP `$03:93A9` allows construction when
`population <= support + 2`, otherwise setting the shortage flag. Lower support
does not immediately delete residents or justify a hard population clamp.

The **report classification itself differs**:

- US `$03:BF8C–C071` recalculates low-growth status, examines population,
  development gates and the 128-record structure census, then chooses the
  report code. `$03:C072` contains six binary thresholds
  `921,889,905,753,533,649`. These are report thresholds, **not independently
  established attainable town maxima**.
- JP `$03:BCC1–BD26` examines population and development gates, then tests the
  town status bits at `$7F:91DA+2N`: bit 0 yields code 5, bit 1 code 1,
  bits `$40/$10/$04` code 2, bit `$80` code 1, otherwise code 3. It lacks the
  US report's structure scan and threshold table. Native fixtures verify both
  paths with matching population and flag inputs.

Text localization alone cannot reproduce this difference. Derived reports,
construction policy and underlying status-flag producers need one consistent
regional model. Geography, allocation, events and the bias still prevent a
claim that all twelve US/JP final population maxima have been proved.

### Population switching: established boundaries

The full house-admission handlers are US `$03:95B3–9611` / JP
`$03:939E–93FC`. The gate checks the **old** population; it does not reserve
room for the whole new house. Native fixtures with support64 admit a house
at population64 or66, but reject67 and set town-status bit `$0010`, leaving
the pending construction budget unspent. An admitted tier-three house adds
eight residents at the next census, including when the old population was66.
Thus `support+2` is an admission threshold, **not a final resident cap**.

A controlled cohort of seven tier-three houses, one regular field and one
bridge produces population58 in both ROMs, with support64 US /32 JP. Repeated
native admission/census steps yield `58 → 66 → 74 → 74` in US, while JP
rejects the initial house request. Evaluating the same nine-house records in
either census preserves every record and returns population74, with support
64/32 respectively. This proves the local contracts, not natural construction
reachability, maximum town populations or an implemented live-switch feature.

The existing `ActRaiser_TownCensus` HLE is the integration owner. Extend its
support coefficients, including completed extension bridges (32 US /16 JP),
rather than introducing a second town model. Preserve standing structures,
the adjustment word US `$7F:9F57+2N` / JP `$7F:9F4B+2N`, and earned levels.
Towns with no act completion skip census output stores; retain that gate.
The decoded US references to the adjustment are census plus save/restore;
they do not establish its full initialization/indirect-write lifecycle or a
separate regional adjustment rule. Do not reset it based on that limited scan.

Refresh order is explicit at US `$03:829E–82A6` / JP `$03:8292–829A`:
census, total-population refresh, then level-award wrapper. The total helper
US `$03:8E10–8E2F` / JP `$03:8CFD–8D1C` copies the selected town's population
to US `$7E:021A` / JP `$7E:0219` and sums all six population words into
US `$7E:0218` / JP `$7E:0217`. It does not sum support.

The level leaf US `$03:B3BA–B40D` / JP `$03:B18B–B1DE` awards **one level per
call** when population reaches the selected threshold; it never demotes. Below
maximum HP24, an award increments maximum HP and current angel HP by one and
sets maximum SP from the shared table, indexed by the pre-award level. It does
not refill current SP or current action HP. At maximum HP24, it skips those
HP/SP writes while still awarding the level. Native boundary, saturation and
repeated-call fixtures verify these effects; population5000 reaches level17
and stops below sentinel9999 in both profiles.

The caller US `$03:E414–E47E` / JP `$03:DF19–DF83` owns the rest: repeated
awards with sound/dialogue, next-level display US `$7E:0297` / JP `$7E:0296`,
current-SP refill after the award sequence, persistent HP update, and the
level17/max-level presentation (next threshold displayed as zero). These
positive-award audiovisual paths are code-mapped, not booted in this follow-up;
the no-award/max-level wrapper paths were executed. Policy activation should
refresh derived displays without invoking award side effects from a settings
callback, then let the normal wrapper process eligible awards. Repeatedly
toggling must not refill SP, replay awards or lower already-earned levels.

The US full-development scan `$03:C037–C071` is also not a maximum-population
formula: it returns carry set only after all128 records are active and every
house encountered has subtype `$20`. The first inactive record returns carry
clear/low-A byte0; the first lower-tier house returns carry clear/low-A byte1. Even
128 bridge records satisfy its predicate. These byte results are not whole
16-bit accumulator values. Do not expose its result or the six
report thresholds as independently proven population caps.

Two support add/subtract leaves, US `$03:BF45/$BF5B` and JP `$03:BC7A/$BC90`,
both use16, **not regional field-upgrade deltas**. Their bodies are mapped,
but no decoded US caller or raw bank03 pointer was found by the current xref
scan. Do not invent an active gameplay hook from their presence; indirect or
cross-bank reachability remains unproved.

These findings close the census/admission and level-award **design questions**.
They do not close developed-town/geography/event validation. A fixed “maximum
population” toggle remains unsupported. Support, level requirements and report
classification can be separately selected policies, but each consumer must use
the effective policy and consistent canonical state at a completed simulation
transaction boundary. Exact host activation hooks and replay tests remain
implementation work.

### Lives convention

US HUD `$02:C280–C2A3` draws stored lives **plus one** in BCD; JP
`$04:91C5–91DE` draws the stored byte unchanged. Death dispatch is at US
`$02:BCFC` / JP **`$04:8CF7`**, not the same bank in both releases. Both dispatch
stored zero to Special game-over (`$1A=$0801`) or normal Sky Palace return
(`$1A=$0007`); positive stock takes the checkpoint path. The scene-transition
debit can underflow BCD zero to `$99` on the normal-return path; this is not
evidence of 99 playable lives. HUD, debit and death-branch fixtures resolve the
off-by-one: stored Special starts 4/2 imply five/three attempts respectively,
although JP displays the spare count.

Twelve booted Fillmore death-dispatch fixtures (both ROMs, normal/Special flag,
stored lives 0/1/2) now follow the checkpoint, Palace and Game Over loaders.
Positive lives decrement once and resume action; US retains score `$1234`, JP
clears it when consuming the action-respawn marker. Stored zero in normal mode
returns to Palace with transient BCD `$99` and score still `$1234` in **both**
ROMs. Stored zero in Special reaches Game Over without debiting zero; pressing
Start then restarts Fillmore with stored lives 4/2 and score zero in both ROMs.
Thus “JP clears score on every death” is too broad: the owner is the action
respawn transaction. These tests enter the native death dispatcher by setting
its player flag; they do not replay lethal damage or every checkpoint.

Native Fight/Yes after normal Palace return reloads the persistent life allowance
(two in both fixtures), clears score, and reaches playable Fillmore. This is
not a hard-coded two-life reset: US `$02:84D4` reads `$02AB`, JP `$02:8493`
reads `$02AA`. Both clear `$1F/$20` immediately afterwards. Keep this ordinary
departure path distinct from Special initialization and checkpoint respawn.

Title-selected Professional/Special now also reaches playable Fillmore with
stored lives 4/2, HP24, MP0 and score0. Only the `ACT` marker in private,
same-region SRAM was staged; the mode flag was set by the native menu, not the
fixture. The marker enables a third selection in the native cycling routine
(US `$02:A7E9`, JP `$02:A55F`); the extra wording appears when selected.
This verifies marker consumption, not earning it through the ending.

### Earthquake class policy

Action 5 was traced through every structure-class table and exercised with
subtypes `$00/$10/$20/$40` and RNG boundary seeds:

| Class | US action-5 rule | JP rule | US / JP handler |
| --- | --- | --- | --- |
| 0, houses | Preserve subtype `$20`, destroy others | RNG | `$03:A066 / 9E16` |
| 1, bridges | Queue action 0 or 1 from town gate | Same form | `$03:A435 / A20D` |
| 2, fields | Preserve if subtype `$30` mask nonzero | RNG | `$03:A144 / 9EF5` |
| 3 | Preserve | RNG | `$03:A1E8 / 9F9C` |
| 4 | Preserve | RNG | `$03:A284 / A042` |
| 5 | Destroy | RNG | `$03:A2E3 / A0AB` |
| 6 | RNG | RNG | `$03:A33C / A114` |

Here “RNG” means native `Random($FF) < $80` queues destruction action 7;
otherwise preservation action 1. It is a modulo-reduced native byte, not a
host 50/50 coin flip. Different classes consume different numbers of draws
between profiles. Later booted fixtures also exercise the complete effect and
build-step/destruction transaction: both origins × both ROMs × five RNG seeds
× all-unsealed/all-sealed lairs × bridge gate 0/1. Thirteen controlled records
cover all seven classes, both bridge orientations, house upgrades, and field/
windmill subtypes. Both bridges survive every tested combination; the gate can
still change their queued action and total effect duration, so preservation
is not equivalent to doing no work.

The origins are semantically distinct:

- Player entry US `$01:97E5` / JP `$01:9784` sets `$7F:90E9` and clears
  `$90F5`. The confirmed cast charges US 160 / JP 60 SP once. Eligible lairs
  lose up to ten stock each and credit carry-sensitive growth.
- Posted entry US `$01:9840` / JP `$01:97DF` sets `$90F5`, not `$90E9`.
  Skull Head state 7 (US `$01:C6F0`, JP `$01:C67A`) posts kind 4 when its
  timer reaches 180 after decrement, subject to busy gates. The master consumes
  that post and runs the effect. It neither charges SP nor performs the
  player's direct lair depletion/growth operation.
- **Both origins still apply destroyed-house feedback.** With all lairs
  sealed, that feedback goes to growth instead of stock. Do not skip the
  destruction transaction merely because the origin is an enemy.

For example, initial unsealed stocks `[1,9,10,20]` and destruction of one US
4-person house plus one 6-person house finish at `[3,3,2,12]` with 42 growth
for a player cast, versus `[4,12,12,22]` with no growth for a Skull Head cast.
The player path depletes stock, then adds house feedback: some lairs exhausted
by the quake are replenished within that same effect. If all four lairs are
sealed, stocks remain unchanged and the houses credit 10 growth in either
origin. JP's destroyed houses contribute four each, including upgraded houses.

Thirty-two further completed effects cover all sixteen sealing masks in each
ROM, following native Skull spawning/targeting/windup without an injected AI
state or timer. Synthetic structures, stocks `[1,9,10,20]`, RNGzero and bridge
gate0 are staged immediately before the post. With only slot3 unsealed, US
finishes `[1,9,10,30]`, JP `[1,9,10,24]`, with no growth; with all sealed,
stocks stay unchanged and growth is10/4. Every mask preserves seal flags,
other-town stocks, SP and SRAM; both bridges survive.

House feedback restarts at the first unsealed lair **for each house callback**.
Do not batch several houses' credits into one round-robin operation. The shadow
regional stock model must process the same actual destroyed houses in order,
not invent a second region's counterfactual destroyed set.

The player origin now also covers every sealing mask, at five RNG seeds and
both bridge gates: 320 complete native-menu casts. Sealed stocks remain fixed;
only unsealed stocks deplete and receive house feedback. The SP charge remains
exactly160/60, with unchanged seals, other-town stocks and SRAM. With only slot3
unsealed, seed0/gate0 US ends at its original `[1,9,10,20]` but awards11 growth:
depletion10 and feedback10 cancel numerically. A semantic progress journal must
not infer “nothing happened” from equal before/after stock values.

Ten further basic-house cases demonstrate the batching error directly. With
slot0 sealed, three US four-person house losses produce `[1,6,3,13]` after
player depletion, not the aggregated `[1,4,4,14]`. Two JP four-person losses
(tested seeds127/128) produce `[1,4,2,12]`, not `[1,3,3,12]`. Both retain32
growth from direct depletion. These results positively verify per-house reset
semantics, not merely a plausible implementation concern.

Developed-town geography, live sealing/target-picker contention, other effect
combinations and complete selector RNG ordering remain separate tests. Do not
substitute synthetic coordinates for a geographically valid developed town.

### Scheduler and SIM enemies

Booted, natively initialized Fillmore traces now cross complete long cycles in
both ROMs, replacing the earlier zeroed-world limitation. The starting town has
two residents, no structure records yet, and active lairs/enemies. In these
fixtures, US wraps recur every 732 emulated frames and JP every 2,412: their
720/2,400 eligible master calls plus 12 frames spent in long-cycle services.
These totals are fixture-specific, **not universal real-time constants** for
developed towns or modal events.

| Tested context | Development clock | World enemy behavior | Angel recovery |
| --- | --- | --- | --- |
| Free SIM town | Advances under the regional divider | Advances independently of that divider | US queued recovery / JP eligible-call recovery |
| Open SIM movement menu, 608 frames | Held | Held | Held, including pending phase |
| Player earthquake | Held | Continues | Continues; existing US queues can drain |
| Skull Head earthquake | Held until effect completes | Effect service still invokes actor pass | JP recovery continues; not a general SIM freeze |

User effect service US `$01:9460–948D` / JP `$01:93F5–9433` is distinct from
the modal-menu service (US `$01:9284`, JP `$01:9219`) and the master tick.
JP's effect service **advances the same `$7F:7CED` divider** and calls structure
visual service `$03:9C15` every fifth pass even while `$7F:91FE/$9200` are held;
US calls its structure visual service `$03:9E40` each pass. Do not restore the
pre-effect JP divider as though every paused development clock were frozen.

JP angel recovery is one HP per 60 eligible calls, capped at the current max;
state low nibble 4 freezes the recovery phase. A controlled US cast with two
HP and three SP already queued starts at HP 3 / SP 200, pays 160, and finishes
at HP 5 / SP 43 with both queues drained, while the development clock never
advances. No new long-cycle queue is generated during that effect.

The helpers US `$03:AF54/$AF47` (JP `$03:AD1C/$AD0F`) around the posted effect
write interrupt-control `$4200` (`$A1` / `$01`). They must not be modeled as a
global gameplay pause/unpause flag. Actual service ownership determines which
systems continue. Further menus/events and developed-town scheduling remain
separate integration cases.

### Exhaustion versus live actors

With one already-live first-lair monster in booted Fillmore, setting only that
lair's stock to zero leaves the actor alive and the seal flags unchanged for
the measured 180 frames in both ROMs. With its slot vacant and countdown set
to one, zero stock prevents spawning; stock one spawns after one US master
call / five JP calls and remains one after spawning. Stock is consumed by
defeat, not admission of the new actor.

Thus a regional stock switch must not despawn an existing monster or infer a
seal from zero stock.

### Guidance, sealing and delayed soul rewards

The native guidance caller US `$03:884D` reaches `$03:B5BD` after traversal
checking. The lair lookup `$03:B794` (JP `$03:B51D`) excludes already-sealed
lairs and matches coordinates, **without checking remaining stock**. The seal
driver (JP `$03:B349`) requires population at least ten. Sixteen complete
controlled Fillmore fixtures cover population 6/10 and stock 0/1/10/200 in
both ROMs. The ten-person cases seal, play the effect/dialogue, award the
first-lair technology increase and refill SP; six-person cases do not seal.

US `$03:B561` / JP `$03:B2ED` credits the remaining stock as growth after the
seal effect, without zeroing that stock. The already-live first monster also
remains. The shared seal flag, remaining stock and one-shot reward are therefore
distinct state. Eight additional final/presealed guidance traces confirm this
ordering: JP enables event0 at fixture frame599 before technology/SP settlement
at801; US enables it with settlement at993. Stock200 credits growth200 at419
in both; stock remains unchanged. These are specific input-timed traces, not
universal regional timers. Re-entering the guidance check with all four flags
already sealed awards no technology, stock growth or SP refill and does not
enable event0; unrelated eligible town events can still run. Full developed-town
story progression and a revisit after completed event dialogues remain separate.

The alternate seal wrapper is the **Magic Skull** item path, not an unidentified
generic event: US `$01:9EE7` / JP `$01:9EBD` rejects already-sealed targets,
requires global lair-array byte index8 (Bloodpool's first slot), sets the seal
flag, then calls US `$03:B57B` / JP `$03:B307`. Those wrappers credit remaining
stock and check all-sealed, but do not call the guidance technology/item reward
router or SP refill. The item caller consumes the skull separately. US also
waits90 after the effect where JP proceeds directly to consumption. Sixteen
booted native item-picker traces verify stock0/1/200, last-lair sealing,
cancellation, wrong-lair/empty-land targets and an already-sealed target in
both ROMs. The skull succeeds at population2 (unlike guidance's ten-person
gate), credits the remaining stock without clearing it, leaves technology/SP
unchanged, and consumes the item only on success. Invalid/cancelled uses retain
it. In these fixtures US settlement occurs at frame3 and consumption at93;
JP does both at2. Final sealing enables event0 at settlement. These are
controlled Bloodpool fixtures, not a complete lake/story playthrough. Do not
give every seal the guidance reward bundle; keep a policy snapshot through
item consumption, not merely through the initial seal write.

Twelve further booted arrow-contact fixtures cover stock 0/1/200, sealed or
unsealed, in both ROMs. The original Blue Dragon dies, stock saturates at zero,
and seal flags do not change. There is also a **separate delayed growth award**:
the dead actor becomes class `$16`, returns to the town's fixed destination,
and credits one growth at US `$01:B95A` / JP `$01:B8FC`. That award occurs even
with zero stock or a sealed lair. The isolated stock-debit leaf alone cannot
describe the complete defeat transaction. A live regional switch must preserve
this pending soul reward exactly once, not grant both profiles' rewards or
re-award seal growth. These are controlled collision fixtures, not all species
or an exhaustive campaign/save lifecycle.

### Native save, actor cache and restored rewards

Native Progress Log → Save, cold boot → Continue, and reset → Continue preserve
all24 stocks, flags, reloads and countdowns, plus six growth balances in both
ROMs. The fixture values include stock0/1/10/200 and above-seed values301–320.
Returning to Fillmore preserves stocks/flags/reloads/growth; ordinary countdown
service resumes. The [save map](save-format.md#35-lairs-growth-and-sim-actor-cache)
records the verified SRAM fields and owners; this is not cross-region save
compatibility evidence.

There is also a native **eight-record actor cache per town**: six `$0130`-byte
blocks, each eight `$26`-byte records, stored at SRAM `$1633-$1D52`. US WRAM
base is `$7F:97DA`, JP `$7F:97CE`. US `$03:8168` caches live `$0B30-$0C5F`
before saving; `$03:813F` restores on town entry (JP `$8165/$813C`). This is
distinct from session-only ambient-story actors. A stock-zero monster after
loading may be a restored actor, not a fresh spawn that bypassed the stock gate.

Four booted save/load traces verify a pending class16 soul: native arrow defeat
debits stock1→0, Save preserves the soul with growth0, cold Continue/Observe
restores it, and return awards growth1 exactly once without another stock debit.
Regional metadata must preserve actor-generation policy and pending reward
identity across this cache/save boundary, not mark restored actors as new
spawns or blindly replay their kill/reward events.

Twelve further traces follow Fillmore → Palace → Bloodpool → Palace →
Fillmore with a pending soul, then leave/re-enter after its slot is reused.
The off-town cache freezes the soul; return credits one growth without another
stock debit. After that return the first vacant slot becomes a new Dragon:
US fixture frame196 (one frame after reward), JP frame567 (371 after reward).
Those intervals depend on the restored countdown; they are not fixed regional
spawn times. The cached copy still contains the old soul until the next native
cache operation. Leaving again replaces it with the Dragon, and re-entry does
not re-award growth. Actor policy identity must therefore follow native
cache/restore and spawn/free boundaries, not a continuously mirrored cache or
the record address alone. Native spawner US `$03:B97F–BA23` / JP `$03:B708–B7AC`
requires an inactive slot (`+$10 & $8000`), eligible lair flags, stock and
countdown; it does not debit stock. It resets selected actor fields, not the
entire `$26`-byte record.

Two reset/Continue traces start after an unsaved soul reward and reload the
earlier saved pending soul. Growth returns to0, then the restored soul awards1
once; SRAM is unchanged throughout. That is expected save rollback, not a
duplicate reward bug. A companion must restore the matching progress and
pending rewards too; session-wide deduplication would suppress a valid reward
on the restored timeline. Legacy/foreign companion reconciliation is separate.

In the controlled Save traces, payload writes are visible at frames61–63 and
the valid checksum is committed at67. US `$03:A82E/$A833`, JP `$03:A5FE/$A603`
write the final checksum words. These timings are fixtures, not an API promise.
A future regional companion must bind to a completed save transaction, not the
first changed SRAM byte. Do not equate reference-core frames with host yields:
the bounded native save body contains only DB-setup and checksum calls, no
frame waits; the host's checksum HLE is yield-free and auto-persistence runs
after the game coroutine returns. Static inspection therefore does not show
a normal-path partial-save exposure. A host trace (including abnormal exits)
is still needed before claiming such a bug or choosing a companion commit hook.

### SIM enemy state differences

The shared collision tables introduce combat differences independent of AI and
spawn timing. Ninety-six native fixtures cover strength 1–8 and contact HP
0/1/8/20 for every species in both ROMs:

| Species / class | Ordinary one-damage arrow hits, US / JP | Contact damage to angel, US / JP | SP per defeat, both |
| --- | ---: | ---: | ---: |
| Blue Dragon / `$12` | 3 / 2 | 3 / 2 | 2 |
| Napper Bat / `$13` | 1 / 1 | 1 / 1 | 1 |
| Red Demon / `$14` | 4 / 3 | 6 / 3 | 4 |
| Skull Head / `$15` | 8 / 8 | 8 / 4 | 12 |

US `$01:B061/B065/B069` and JP `$01:B031/B035/B039` hold SP rewards,
contact damage and accumulated-damage thresholds respectively. Death tests
**strictly greater than** the threshold, not greater-or-equal; enhanced arrows
add their strength to byte `actor+$24`. Contact damage saturates at zero and
enters the native knockback state. Consumers are arrow collision US `$01:AFC8`
/ JP `$01:AF98` and angel contact US `$01:B06D` / JP `$01:B03D`. Capture a
combat policy per actor generation; changing a threshold underneath an
already-damaged actor is not equivalent to changing only spawn stock.

The four SIM species' 64 state entries were paired separately from scheduling:

- Blue Dragon state 1: US target search every eligible update, JP only after
  the eight-count gate at `$01:BA0C–BA1A` (US state entry `$01:BA5F`).
- US Blue Dragon state 6 calls the world-actor pass again at `$01:BB5C` after
  spawning its strike effect; the corresponding JP state has no such call.
  Preserve/measure ordering before replacing this with a simple cooldown.
- Napper Bat's no-house-target fallback compares native `Random($FF)` to US
  `$FD` at `$01:BEE3`, JP `$FA` at `$01:BE6D`. Its state-4 timer is US 1 at
  `$01:BF72`, JP 60 at `$01:BEFC`.
- Shared target selection US `$01:BCB0–BD89` biases random candidates using
  town offsets `$7F:6B9F/$6BAB` and a smaller local range. JP
  `$01:BC5F–BD13` samples the wider 32-cell grid without those offsets.

Address-normalized state comparisons are leads, not equivalence proofs:
shared callees, animation, RNG order and US `BRK` versus JP `COP` audio dispatch
still matter. Red Demon cannot be called identical merely because its main
state instruction shapes match. The additional Skull contracts below have
positive native evidence, but still do not establish whole-world equivalence.

### Skull Head target and earthquake state contract

Two controlled 9,000-frame Fillmore traces use native Skull spawning, targeting,
travel, loiter, windup, earthquake posting and recovery through two casts in
each ROM. The fixture changes the first lair's species/countdown, suppresses
other monster spawns and moves the Angel away; it does not inject Skull AI
state, timer or target. This is not natural campaign placement of the species.
Another1,674 isolated native cases cover every target square in all six towns,
admission/rejection, timer boundaries, Angel alignment and post arbitration.

State2 US `$01:C58F` / JP `$01:C519` calls native `Random(8)` twice, then
tests bit `$0800` of the chosen road word at `$7F:6800+town*$80+2*(y*8+x)`.
It attempts only one candidate. Rejection returns state1 without replacing
the old target, even if every other square is eligible. Acceptance sets state3
and targets `(64*x+16,64*y-16)` in 16-bit coordinates; the top row's Y wraps to
`$FFF0`. The Dragon/Bat bias words are not used. This is road-square eligibility,
not direct enumeration of houses: the baseline temple square is eligible.

Both ROMs use these **eligible handler-call counts**, not wall-frame durations:

| State | Verified timing / transition |
| --- | --- |
| 1, target retry | 32 calls → state2 |
| 5, fresh loiter | Initializes1800, decrements on entry; call1800 leaves zero, call1801 → state `$0C` with180 |
| `$0C`, pre-windup | 180 calls → state6 |
| 6, windup | 52 calls → state7 |
| 7, quake attack | Initializes204; at post-decrement180 (24th call), evaluates posting gates; zero → state8 |
| 8, recovery | 54 calls → state1 |

In state5, sharing the Angel's 16px row or column selects a directional behavior
and state `$0B`. Its shared state9/A/B handler lasts24 calls, consuming remaining
loiter time with saturation at zero, then returns to state3 with resume marker
`actor+$1E=1`. Travel back to the old target preserves the remaining wait.

At state7's post threshold, an existing effect post or `$7F:7BEB` sealing first
cancels the attack into state8, preserving any existing post. Otherwise an
active user miracle (`$90E9`) or target picker (`$9215`) resets the timer to204
for a later retry. Only the remaining case posts kind4. One generic `busy`
boolean cannot represent these priorities. Master-loop post consumption is
separate from this actor-level arbitration.

Eight additional booted contention traces verify two staged Skull actors and
a staged Skull during a real player-menu cast, with unchanged ROM execution:

- If both reach the threshold together, the lower-index record posts and the
  later record cancels into state8. If the later record reaches it first, the
  earlier record cancels on its next eligible call. Both cases produce only one
  effect; the losing attempt is **not queued**. The actor scan's ordering matters.
- The accepted post stays set throughout effect servicing. Ordinary master
  US `$03:8204` / JP `$03:820F` clears it before the next world-actor pass, not
  when the effect driver first reads it. Preserve that lifetime, not a generic
  clear-on-read event channel.
- During the player's miracle, the Skull retries every24 eligible calls without
  posting. Effect-complete `$90F3` precedes user-active `$90E9` clearing; in these
  traces they occur at frames184/188. Keep the origin through wrapper cleanup.
- The post-miracle result dialogue holds the Skull's remaining countdown188
  through frame700 if left open. After native acknowledgement/menu close it
  resumes, casts once, and performs no second SP debit or direct lair depletion.

These are controlled actor/timer fixtures in an undeveloped town, not proof
of all naturally occurring contention, sealing or target-picker interactions.

The fifteen behavior programs `$16,$1F–$26,$2E–$33` are byte-identical at
relocated pointers (US table `$01:E099`, JP `$01:E023`), including visual IDs,
durations and signed motion steps. This is not a comparison of sprite pixels.
These tests support shared Skull targeting/timer logic, not a new JP-specific
cooldown option. Contact damage and earthquake selection/feedback still differ.
Effect policy must remain fixed through destruction and settlement; preserve
the existing actor state across safe changes, without restarting its timers.

### Collision and animation joins

Decoded terrain collision quadrants differ in eight shared raw map keys
(`room << 8 | region`): `$0101` (244 cells), `$0102` (4), `$0106` (6),
`$0203` (33), `$0306` (16), `$0403` (16), `$0502` (16), `$0505` (13).
Dimensions match. The other 41 keys' terrain grids match, but that says nothing
about live actor hitboxes or authored damage boxes. Eighteen keys have changed
type-`$80` item placements. Collision, layout and pickup identities therefore
need compatibility checks, not independent blind art/data swaps.

All 22 changed spawn records now join to their room consumers and initial
animation/composition data. Native spawn fragments verify normal/Special stat
promotion: unless flags `$0201` exclude it, Special promotes attack or HP 1 to
2 after reading the selected record. Comparing only base records misses this.

Across script-order room/animation-slot snapshots, 43 have changed sequence
data or composition extent headers. This includes inherited/repeated assets,
not 43 proven live-actor differences; unused inherited boss slots must be
excluded when deciding which gameplay rules to expose. Examples:

- Fillmore room 4 boss state 0 has stored duration US 47 / JP 15.
- Marahna room 3 boss compositions change fourth extent byte US112 / JP96;
  [native boss follow-up](#marahna-act-1-boss-vulnerability-and-placement) now
  separates placement, linked body geometry and vulnerable phases.
- Death Heim room 7 boss states 17/18 each omit two stationary sequence rows
  in JP; the [Northwall/rematch follow-up](#northwall-act-2-boss-original-versus-death-heim)
  verifies their 12-frame effect. Room 8 state 48 changes its first stored
  duration 10 → 11; its live timing remains a separate question.
- Kasandora type `$10` initial state 15 includes a one-byte extent difference.

These are stored values, not universal frame durations: the animation consumer
controls timing, facing and collision interpretation. Normal and rematch
assets must both be tested. The final sequence (state 61) of both Marahna room-4
ordinary-animation blobs lacks an `$FF` terminator before the composition
table; the research reader stops at that table and flags it. Native reachability
and termination for that particular sequence remain unproven.

### Death Heim transition and music

Score settlement has different callers, not just a different conversion
formula. JP `$00:A713` calls `$03:CD3E` from the stage-clear card owner
`$00:A6BC`, after writing the completed-act count. US's card owner `$00:A6FD`
does not call its converter; the action-departure owner calls `$03:D095` at
`$00:A30D`. A mixed-region completion transaction needs one settlement point
and a captured completed-act count; do not install both native calls and
double-credit growth/stock. Full score-card/emergence ordering remains open.

US `$00:A343–A381`, reached after the action completion animation, checks all
six completed-act counts. When all are two it sets `$7F:9101` bit 0, stages
scene `$0009`, and selects music ID 4 at `$0334`. JP's corresponding
`$00:A335–A340` instead returns to the current town. JP `$01:85C8–85FA` later
checks the six counts and sets `$9101` bits 0/1 before the announcement;
US `$01:861E–8645` announces only when bit 0 is already set and bit 1 clear.
Twenty-four native transition fragments verify the first branch, including
each possible current town. They do not play the entire emergence cutscene.

This establishes a caller for the extra US Sky Palace music-ID-4 declaration.
At `$02:B63B`, the command reads a song number, selector and source pointer.
It executes **only when `$0334` matches its selector**; otherwise it returns.
The separate loaded-source pointer `$AB/$AD` suppresses duplicate uploads.
Thus `$0334` is the selected/requested music identity, not proof that a given
source is already loaded. The end-to-end scene/audio presentation and JP
counterpart still require a booted trace before splitting the cutscene, music
and story-state behavior into independent settings.

### Minotaur timing and room inheritance

Four controlled, booted normal-mode Fillmore boss traces (both ROMs, two action
RNG seeds) join the source records US `$AF5D` / JP `$AFF1` to their live
Minotaur. After loading act-2 rooms 2 → 3 → 4, native boss activation runs with
the same initial player position. Subsequent state-0 waits last **48 US / 16 JP
frames**, confirming that stored animation duration 47/15 is a live behavior
difference. This is not a universal “JP bosses run three times faster” rule.
Other phases and actor scheduling require separate comparisons.

The axe child also has a different facing-relative horizontal spawn offset:
US `$00:AFDB` loads −72; JP `$00:B06F` loads −48. The helpers `$00:8709` /
`$00:86F8` mirror this by facing and add it to the child's X coordinate. This
is a position offset, **not projectile velocity**. A normal-room trace does
not by itself validate Special promotion or the Death Heim rematch.

Skipping directly from Fillmore room 1 to room 4 was rejected as a boss
fixture: it retained act-1 terrain/background resources and blocked movement.
Requesting rooms 2 → 3 → 4 first produces the castle room and allows native
activation. Regional room bundles must reconstruct their owning act's resource
inheritance; apparently valid actor records alone do not prove a valid room.

### Marahna Act-1 boss: vulnerability and placement

Marahna room3, map key `$0305`, places type `$05`; source US `$00:D974` /
JP `$00:D9F6`, entry US `$00:D980` / JP `$00:DA02`. Both records have attack1,
HP24 and BCD score `$50`; Special's shared spawn promotion makes attack2,
not HP25. Four480-frame controlled native traces (both ROMs, normal/Special)
use the native room1→2→3 resource chain, then an explicit player-position/HP
fixture. They are not natural travel or full victories.

The boss's root has a substantive phase difference, not a speed multiplier:

| Phase contract | US | JP |
| --- | --- | --- |
| Activation | Player X reaches320 | Same |
| Root cycle | State2 repeated99 times, then repeats that loop | State1 → state2 repeated5 times → state3 → state20 → repeat |
| Measured JP phase lengths | Not applicable | 12 /40 /12 /90 frames in this fixture |
| Phase-specific damage rejection | No closed-phase bit in this root loop | Sets object `+$30` bit `$0020` for state20; clears it before state1 |

JP measured state changes occur at frames1/13/53/65/155, then repeat every154
frames, in both tested modes. US stays in state2 over the480-frame observation;
its99-repeat loop is also code-mapped. These durations refer to this actively
serviced native fixture, not wall-clock time during pauses or unrelated freezes.

Ten complete **isolated native collision** calls use booted, unmodified boss
states and a native player sword animation captured after Y input. Only sword
contact coordinates are repositioned; actor physics is not advanced during
the collision call. US open and JP open lose one HP (24→23); JP closed stays24.
Clearing only bit `$0020` in that closed JP snapshot permits24→23 even with
the tiny closed composition unchanged. Both normal/Special give the same
result. Thus the bit is a real damage gate, not merely a smaller target or
an inference from artwork; this is not a natural sword-reachability test.

The geometry difference has a separate explanation:

- Animation-header bytes are **left, right, top, bottom**, not object-slot
  field order. State0's header `[24,40,96,112]` US / `[24,40,96,96]` JP becomes
  `+$0A/+$0C/+$0E/+$10 = left/top/right/bottom`. Native tests cover H/V flips.
- Common spawn US `$00:95F0–96AE` / JP `$00:9618–96D6` first applies the
  animation, then subtracts its bottom extent from the authored spawn Y224:
  Y112US /128JP. US boss entry adds8, giving a live root Y120 versus128JP.
  Patching only the composition header would therefore also move the boss.
- The vulnerable root's open state2 uses a small `[left16,top16,right8,bottom16]`
  box. The large state0/body headers must not be presented as its permanent
  vulnerable box. Linked visual/spawner objects use different flags/lifecycles.
- Boss compositions0/5/6/7/8 have four fewer sprite components in JP
  (44→40 for visual0;47→43 for the others). Closed visual48 is one component
  in both, but its header is `[4,4,3,5]` US / `[4,4,4,4]` JP and its component
  data differs. Geometry/presentation is not reproduced by changing one byte.

Native animation readers are US `$00:8E2F` / JP `$00:8E3B`. Collision US
`$00:8A3C` / JP `$00:8A2B` rejects victim flags with mask `$2429` (including
`$0020`), does broad rectangle rejection, then checks eligible **attacker
composition parts** through US `$00:8B67` / JP `$00:8B56` before applying
damage. Do not replace this with a single enlarged boss rectangle.

The root also owns same-source linked objects. In the fixture, root `$12E0`
has parent0, body/spawner `$1320` and lower child `$1360` link to it, and
`$08E0/$0920` are health/flash helpers. Source-pointer matching alone would
incorrectly treat every one as the vulnerable boss. Root and linked children
must retain a consistent encounter policy/generation.

Implementation boundary: capture this named boss's effective behavior at
room/spawn initialization, keep HP/damage/death handling shared, and defer
mid-fight selection changes until the next encounter. JP's root phases can
use states1/2/3/20 already present in US animation data; semantic AI does not
require shipping JP ROM code. Native JP geometry/art remains a separate,
validated combination—not an unchecked media/header swap. The linked spawner
also posts event-audio ID `$A1` US / `$21` JP; do not misclassify that COP as
an extra enemy spawn or claim audio parity from these collision tests.

This closes the named root phase/damage-gate and spawn-anchor questions.
Full victories, all child attack/death paths, magic interactions, mixed-media
presentation and actual host policy switching still need implementation or
encounter-level validation. It does not close other bosses or Death Heim.

### Northwall Act-2 boss: original versus Death Heim

Northwall room8, raw map key `$0806`, places type `$08`, source US `$00:F161`
/ JP `$00:F1E0`. Its Death Heim rematch, raw key `$0707`, places type `$05`,
source US `$00:F760` / JP `$00:F7DF`. The original 12-byte spawn records
match; both encounters have HP24 and attack1, promoted to attack2 in Special.

The 208 instructions in the boss/child family US `$00:F16D–F399` / JP
`$00:F1EC–F418` have matching structure and gameplay immediates. Differences
are reviewed code-pointer/helper relocations and the two COP sound requests
`$9E` US / `$1E` JP. This is a bounded family comparison, not proof of every
shared helper, audio effect or whole-encounter equivalence. Rematch wrappers
US `$00:F76C` / JP `$00:F7EB` eventually enter this same original boss family.

The meaningful regional timing change is in **rematch-specific animation
data**, not a general Northwall speed setting:

| Encounter | US wind-up | JP wind-up | Large/small ice-ball horizontal speed, both regions |
| --- | ---: | ---: | --- |
| Original Northwall Act-2 | 118 frames | 118 frames | 1 / 2 pixels per movement update |
| Death Heim rematch | 118 frames | 106 frames | 2 / 4 pixels per movement update |

These are native active-frame measurements from eight controlled 900-frame
traces: both encounters, both ROMs, normal/Special, with two completed wind-ups
per trace. Native room requests preserve the owning act's resource chain;
Special is set before loading and player HP/max is set to24. No boss phase,
RNG, animation asset or damage field is overridden. These are not natural
traversals, full victories or measurements during menu/clock freezes.

- The HP-owning root runs state `$12` (decimal18) while its linked body runs
  `$11` (decimal17). Both lose two stationary rows in the JP rematch: each row
  has stored duration5, consuming6 native frames. The final Y is unchanged
  (64→326); the root then starts state `$09` and spawns its first projectile
  12 frames earlier relative to that wind-up's start.
- The original Northwall animation/composition blobs are byte-identical.
  For the rematch, all 26 visual composition records—including extent headers
  and sprite parts—match across regions. Only states `$11/$12` differ in the
  decoded sequences. The shorter sequence still changes which pose/hitbox is
  present at a given time; it is not purely cosmetic.
- Root `+$3A` holds a phase-branch value0/1 during this loop; it is **not** a
  parent pointer there. The linked body has an actual backlink to that root.
  Same-source health/flash helpers and ice balls must not be mistaken for the
  HP owner. Both entry and live object role matter.
- States `$19/$1A` (decimal25/26) program doubled horizontal ice-ball speeds
  for **both** rematches. Their 60-frame duration, vertical program and
  regional equivalence remain intact. Do not put this shared encounter rule
  behind a JP-only option.

Reproduction data: original animation blob file offsets US `0xC3137` / JP
`0xC091E`; rematch US `0xC432C` / JP `0xC23FC`. Native animation consumers are
US `$00:8E2F` / JP `$00:8E3B`; yield/finish wrappers US `$00:8657–8668` / JP
`$00:8646–8657`. Twenty-four repeated isolated native fixtures check both
encounters' normal/Special spawn values and the changed stationary-row
boundaries, including the actual loaded collision extents.

Implementation boundary: use a named **Death Heim Ice Dragon wind-up policy**,
captured for the whole boss family at encounter initialization. US remains
default; a JP option can select the two shorter semantic sequences using US
visuals, without requiring a JP ROM. Do not globally edit state numbers or
replace the original fight's animation blob. Defer mid-fight setting changes
until the next encounter; do not reset HP, root/child generations, RNG or
projectiles to apply them. Full defeat/cleanup, magic/contact coverage and
actual mixed-policy runtime validation remain separate gates.

### Apples: equal recovery rules, different placements

For the pinned US/JP releases, the claim that a half apple intrinsically heals
less in JP is **not supported**. Item `$04` in both pickup dispatchers (US
`$00:879D`, JP `$00:878C`) sets the byte at `$7E:00E3` to
`floor(maxHP / 4)`. It does not directly add HP. Item `$05` sets the same queue
to `maxHP - currentHP`. These assignments **replace**, rather than add to,
any pending recovery. The routines do not branch on Special mode.

US `$00:88D6–88F6` / JP `$00:88C5–88E5` service that queue identically:

- If the queue is zero or frame byte `$88 & 3` is nonzero, do nothing.
- Otherwise decrement the queue. If current HP is below maximum, add one HP;
  otherwise clear the rest of the queue. Never heal above the maximum.
- Reaching maximum on an increment may leave a residual queue until the next
  eligible service call; do not reinterpret it as immediately available HP.

Forty-eight isolated native fixtures cover pickup formulas, odd-cap rounding,
normal/Special, existing-queue replacement, all four service phases, exhaustion
and saturation. Four controlled booted traces then break and collect the
**authored item4 statue** at tile `(122,22)` in Fillmore Act-2's entry room,
raw key `$0201`, in both regions and both modes. HP12/max24 becomes18, with
six increments separated by four active frames. Collection occurs at different
trace offsets; that is not a different healing rate. The item retires normally.
Fixtures explicitly position the player and set HP, but do not edit the statue,
its item ID, healing queue or pickup callback.

Placement remains a real, separate difference: Fillmore Act-1's statue at
`(150,18)` is item5/full recovery in US and item4/quarter recovery in JP.
That changes the benefit at that location without changing either item's
formula. Keep such changes under the item-placement policy, not a fabricated
regional healing multiplier. The equal formula is not a claim of equal item
availability or European-release behavior.

### Fillmore Act-1 tree: seed controller and pre-shot wait

Raw room `$0101` authors two adjacent objects at tile `(112,28)` and `(112,26)`:
type0 head (US source `$00:A934`, JP `$00:A8F3`) and type1 controller (US
`$00:A9B3`, JP `$00:A97E`). The head has HP3/attack1; native Special promotion
makes attack2. The peer has HP0 and is not a second damage-owning head.

The US peer entry `$00:A9BF` is just `RTS`. JP `$00:A98A–A9D9` implements
the seed controller. The JP head increments `+$78`, specifically `+$38` of
the **next 64-byte object slot**, to request this phase. This is an authored
adjacent-peer relationship, not a generic global seed flag or a parent pointer.

Four controlled 720-frame booted traces (both ROMs, normal/Special) verify:

| First-cycle event | US trace frame | JP trace frame |
| --- | ---: | ---: |
| Head state9 starts | 1 | 1 |
| Head state10 starts | 41 | 41; requests seed phase |
| Two seeds start state16 | Absent | 69 |
| Head state11 starts | 105 | 170 |
| First / second orb child becomes active | 106 / 158 | 171 / 223 |

The US state10 row has stored duration63, so its normal animation wait lasts
64 active frames. JP state10's row stores0, but the head applies the pose and
uses a **separate delay of `$0080`**, measured as129 frames. Reading only the
animation table would incorrectly conclude that the JP wait is shorter.
This is a seed phase **before the same two orb shots**, not evidence that the
game chooses between mutually exclusive seed-only and orb-only cycles.

JP's peer first plays state7 for28 frames, then tries to allocate seeds at
X−32/X+32, Y+24 relative to itself. The native fixtures observe `(1760,416)`
and `(1824,416)`. The seeds retain the peer's source and backlink, descend in
state16, land, and progress through states17→23→18→20. Each also creates two
short-lived linked visual children using states24/22. The plant's walking
phase repeats16 times with native terrain/facing checks. Do not replace these
with decorative sprites or attribute all same-source objects to the head.
Three isolated native allocation tests establish graceful zero/one/two-slot
results: partial allocation is retained, the request is cleared, and the peer
returns to its waiting entry instead of repeatedly spawning the missing seed.

US data retains the corresponding seed/plant states and matching visual
composition records for the15 reviewed head/controller/seed/plant visuals.
That establishes a useful US-data basis for semantic behavior integration;
it does not establish equality of all atlas pixels/palettes. Exact presentation
and collision combinations still need validation. Animation blobs are at file
US `0xCD695` / JP `0xCBE43`, loaded to `$7E:4000`; the JP seed/plant program is
`$00:A9F4–AA6D`, and head entries are US `$00:A940` / JP `$00:A8FF`.

Implementation boundary: one named tree attack-family policy, US default,
captured on room/family creation and shared by the head, peer and descendants.
Represent the peer relationship explicitly, preserve native allocation failure,
terrain, collision and yield behavior, and defer live changes until the next
room/family initialization. Neither live HP nor a pending attack should reset.
Full tree defeat and bridge extension, all seed/contact edge cases, exact art
compatibility and the actual mixed-policy implementation remain validation
gates; the existence and timing ownership of the extra phase are now settled.

### Bloodpool Act-2 statues: single versus double volley

Bloodpool types `$26/$1E` are the two facing variants of the firing statue.
Their source records are US `$00:BD76/$BD84`, JP `$00:BE0A/$BE18`.
The first record's entry branches over the second record; that embedded data
must not be decoded as code. Main volley entries are US `$00:BD90`, JP
`$00:BE24`. Both records have HP3/attack1, with attack2 in native Special mode.
The ten authored placements match between these ROMs: one in room2, one in
room4, four in room5, two in room6 and two in room7. Raw map keys end in `$02`;
these room numbers are not act numbers. This is a firing-rule difference,
not a changed count or placement of this particular enemy family.

The extra JP code at `$00:BE3C–BE44` replays state `$0F` and calls the same
spawn helper again. US proceeds directly to the post-shot idle. All four
reviewed animation sequences match between ROMs: state `$0E` waits 60 active
frames, `$0F` has four 4-frame firing rows, child `$21` has two 2-frame startup
rows, and child `$23` has two 4-frame flight rows with horizontal speed 3.

Eight controlled 320-frame traces cover both ROMs, both facings and
normal/Special. Native room loads create the real authored statues; fixtures
set player position and HP but do not fabricate an enemy or its attack state.
The first two full volleys and projectile retirement are observed:

| Event / interval | US | JP |
| --- | ---: | ---: |
| First wind-up begins, trace frame | 62 | 62 |
| First volley: child startup frames | 78 | 78, 94 |
| First volley: flight state begins | 82 | 82, 98 |
| Second volley: child startup frames | 215 | 231, 247 |
| Successive first-shot interval while active | 137 frames | 153 frames |

JP fires **two sequential shots 16 frames apart**, not two simultaneous shots
or an overall faster cooldown. The cycle is idle60 + wind-up16 + optional
second wind-up16 + idle60 + one restart frame. Startup lasts 4 frames; the
flight state sets velocity first, and position changes on the following
frame. Timing is in active native updates, not a wall-clock promise across
pauses or scene transitions. Normal/Special use the same sequence.

The spawn helpers US `$00:BDB1–BDCA` / JP `$00:BE4E–BE67` allocate after the
parent, retain its source and facing, set the child backlink `+$3A`, and offset
X by−16/+16 and Y by−8. Children have HP0 and inherit attack1/2. They start at
US `$00:BDCB` / JP `$00:BE68`, then use the existing flight handlers US
`$00:BDF0` / JP `$00:BE8D`. Matching only the source cannot distinguish the
statue from its startup/flight children or a later generation in a reused slot.

Twenty-four isolated native cases establish lifecycle edges in addition to
the booted traces:

- A root with offscreen/activation flag `$0400` does not start a new volley.
  The main program does not recheck that gate between the two JP shots.
- Zero/one available slot cases exercise the post-animation continuation,
  both facings and each regional shot phase. A failed allocation drops that
  shot but still advances the parent to its next animation; there is no retry
  queue or rollback of a successful preceding shot. The legacy allocator
  returns scratch record `$1AA2` on exhaustion and this caller writes there;
  no live child is created. A host integration should represent the failed
  spawn explicitly, not expose an out-of-pool pointer as a real actor.
- The child checks `$0400` after startup, before entering flight. In flight,
  animation helper yields mean the check is reached only at the end of the
  two-row/eight-frame sequence, not on every frame. The fixtures distinguish
  both row advances from the terminating sequence boundary before freeing.

Animation data comes from file US `0x5782E` / JP `0xC9FAB`, loaded to
`$7E:4000` at Bloodpool Act-2 entry and inherited by its later rooms. The seven
reviewed visual composition records also match. This supports a semantic
double-volley option using US data; it does **not** prove identical CHR,
palettes, every enemy sequence or complete combat compatibility.

Implementation boundary: a named statue-volley policy, US default, captured
at room/family initialization and shared with its children. Keep appearance
selection separate; defer an in-flight policy change until the next owning
initialization rather than resetting an existing volley or creating/removing
its second shot. Preserve active HP, attack promotion, allocation order and
retirement timing. Host mixed-policy tests, parent defeat during a pending
shot, full contact/magic interactions and exact artwork remain validation
gates; the shot-count and timing discovery is closed.
