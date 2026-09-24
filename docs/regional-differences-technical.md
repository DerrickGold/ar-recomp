# Regional differences: technical evidence

For the plain-English comparison of the releases, read
[ActRaiser: regional differences](regional-differences.md). This companion
preserves the ROM identities, addresses, measurements, qualifications and
integration constraints behind that article. Implementation task tracking
remains separate in the project's private development notes.

This is a reverse-engineering reference, **not a list of implemented settings**.
The comparisons below were checked against the exact headerless retail ROMs
on 2026-09-20–22. Table/code comparisons were supplemented with 84,499 isolated
native-routine fixtures, 836 booted SIM/action/save traces, and 442 complete controlled
earthquake-matrix transactions in the Snes9x reference core. Booted traces also
include effects; do not count those casts again as separate matrix cases. These establish the
listed behaviors, not complete playthrough equivalence or safe live switching
between profiles. Booted fixtures use unmodified ROMs and separate scratch
states; their RAM setup is not a claim of unassisted playthrough coverage.
Ten additional Ice Dragon positive controls use explicitly modified in-memory
waveform data; they are separate from those unmodified-ROM trace counts.
Twenty-five debug/menu controls are also counted separately because their
entry adapters or activation patches modify the in-memory ROM.

| Release | Size | SHA-256 |
| --- | ---: | --- |
| USA | 1,048,576 | `b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0` |
| Japan | 1,048,576 | `3655833fd0fb4985c3dbbf28141f65564f7a228ee92d81a12982ef7cb952a51b` |
| European English | 1,048,576 | `146a68436fa9dbe728ddc7355821384765325e356cb8b9b193a4f22333ed52a0` |
| German | 1,048,576 | `01923db83e0e8b19d476483649d956e3e24cfc918ca04a6aa04faa29ba8e4c41` |
| French | 1,048,576 | `6cc2cadfcb4fba4c1abb2a1d06b49b840bec65d75acaa0ac8831576442e7e96a` |

Addresses are SNES `bank:address` unless explicitly called file offsets.
Do not use US RAM addresses to interpret Japanese snapshots: fields move by
different amounts in different structures. ROM relocation is not itself a
gameplay change. European releases require separate profiles.
Unless a section explicitly names Europe, its comparison remains US/JP.

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
  `$0900` is bypassed by the normal world-map loader, not an extra action stage
  (see [routing evidence](#unused-world-map-placement-root)). Of the common roots, 27
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

The profile tables start at US/EU/DE/FR `$02:893E` / JP `$02:87E7`;
timer word at `+25`. All 43 used action records have matching bytes `+0..+24`
across all five ROMs. The six JP limits below are the only initial-time changes;
the three European releases match the US throughout this used-profile set.
Timers are BCD, not binary. Map keys below use `region/room`, not act numbers.

| Profile | Consumers | US / EU / DE / FR | JP |
| --- | --- | ---: | ---: |
| `$03` | Fillmore room 1 | 300 | 200 |
| `$05` | Fillmore room 3 | 200 | 100 |
| `$07` | Bloodpool room 1 | 300 | 200 |
| `$24` | Marahna room 8; Death Heim room 6 | 300 | 200 |
| `$25` | Northwall room 1 | 200 | 100 |
| `$26` | Northwall room 2 | 200 | 100 |

Recomp resolves this choice at the normal return of the audited action-only
`$02:B4E8` profile initializer, replacing only `$E6/$E7`. The native or HLE
profile body still owns PPU writes, CPU flags and stack return. Disabling the
video HLE retains the selected gameplay limit. No frame-time patch, live-clock
rescaling or setting-change refill is involved.

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

The Recomp construction-wait option replaces only the leaf at `$03:872A–873B`.
It decrements `$7F:7CE1[town]`, reloads it on expiry, and advances
`$7F:7CC9[town]` once. The pending rule activates at that expiry, preserving an
existing wait and leaving the native reload table `$7F:7CD5[town]`, other towns,
master divider and cycle untouched. All five ROMs have the same leaf opcodes
(JP `$03:86DA–86EB`); initialization loads1 in US/EU/DE/FR and150 in JP.

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

#### Level-goal integration

The US-rooted regional adapter replaces only the three population-table reads:
`$03:B3C7 → B3CB` performs the selected comparison, `$B3CD → B3D1` loads the
unmet goal, and `$B407 → B40B` loads the following goal after an award. Generated
entry boundaries keep all three reads behind their guards; HP/SP changes,
level increments and the original PHP/PLP/return behavior remain native.

`ActRaiserLevelGoalsRuntime` captures one table for the complete `$03:E414`
award wrapper, including its repeated `$B3BA` calls, yields and dialogue.
Standalone `$B3BA` entries also capture, while nested calls reuse the wrapper's
snapshot. Settings changes cannot replace the table midway through an award.
The existing Master-report owner refreshes only `$7E:0297` when Japanese goals
are active or being replaced with US goals; it never calls the award wrapper.
The maximum-level display stays zero, distinct from the table's 9999 sentinel.
Population support and redevelopment have separate activation contracts.

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
JP. The [transition follow-up](#death-heim-transition-and-music) distinguishes
the music-ID-4 request from the source actually retained during emergence;
the extra Palace declaration is not a separate emergence track. The action count
does not include non-action resources. Special bit-5 commands are now separately
mapped to US `$02:B363` / JP `$04:836B`: they copy an uncompressed `$0800`-byte
metatile source to scratch before selecting/byte-swapping BG slices. Of 22
matched consumer instances (four distinct source pointers per ROM), eight
consumers differ, representing two shared source pairs: six towns use US file
`0xC981A` / JP `0xC7787`; scenes `$00/$07–08` use US `0xCA01A` / JP `0xC8000`.
These are two source differences, not eight independently identified features;
named visual meaning still requires inspection.

Unresolved semantic areas include unexercised actor/boss branches and hitbox
ownership, complete developed-town population maxima, donor-art bundle
boundaries, and European difficulty variants. Native status-page navigation
and the final-act/emergence/announcement sequence are covered below; mixed-
policy implementation remains separate. No byte census alone can prove that
no further gameplay differences exist.

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
one, JP for any count other than two. These formulas use decoded **stored
score units**, each worth ten displayed points. At stored score 1000
(10,000 displayed), the stock adjustment is therefore US `+50` per lair
versus JP `-25`. Scores and destroyed houses must be
part of any dual-profile progress journal; `initial stock - kills` is not enough.
An exhausted but unsealed lair can gain stock again. Do not clamp to its seed.

The storage arithmetic has distinct edge contracts. House feedback uses word
`INC`, and US score feedback uses word `ADC`: both wrap at65536. JP score
subtraction uses carry to clamp a borrow to zero. The miracle debit instead
uses `SBC #10; BPL` and clears a negative **result**, so it is not equivalent
to unsigned saturation for high-bit stock words. Ordinary seeds are far below
that boundary, but an edited or overflowed counter must not silently acquire
different semantics in a host accountant. The house callback derives
`4 + ((subtype & $30) >> 3)`, including10 for subtype `$30`; this differs from
the census's four-person fallback for that subtype and does not establish a
normally reachable ten-person house tier.

For selectable house feedback, Recomp changes only the US `$03:B4B8` unit
prefix (`AND #$00FF; TAY`) to a fixed four under the Japanese policy, then
rejoins `$03:B4BC`. The shared native distributor still handles round-robin
stock increments, word overflow, all-sealed growth and register/stack cleanup.
US/European selections use the original prefix. The effective policy cannot
change inside a house callback, miracle or earthquake. Existing growth is not
reprojected when the retained stock history switches. The wrapper preserves
the US caller's flag contract rather than importing JP's pre-`PHP` `LDY` flags.

Selectable score arithmetic retains the US `$03:D095` transaction and replaces
three bounded prefixes: `$D0B3` chooses the destination, `$D0D4` converts the
score, and `$B525-$B548` performs JP stock subtraction. Explicit CFG boundaries
retain the native prologues and cleanup. Conversion returns through the original
`$D10A` RTS, rather than manually popping a host-invented frame. All10,000 valid
four-digit scores match the unmodified JP converter, including A, Y, flags and
scratch words `$7F:7C05/$7C07`. The converters leave carry clear for the shared
growth helper. Conversion, stock operation and destination have independent
internal policy keys; their UI bundle captures them for one settlement.
The separate clear-card-versus-departure timing member captures one completion
transaction, as described below. Phase is prospective; all retained stock
projections consume the same actual settled score rather than fabricating a
second tally when the player later switches rules.

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

Recomp's **Lair respawn delays** setting stages a whole-table replacement at
the `$03:8193` town-master boundary. It verifies all 24 old `$7F:9628` words
before writing any replacement and never writes `$7F:9658` countdowns, flags,
actors or stock. `$03:B97F` remains native, including the `$1000` bypass and
the `$B9C0/$B9C3` reload on expiry. US/Europe share numerical values; there is
no PAL clock conversion. The owner does no table scan when no edit is pending.

The `$03:B6BF` observer delegates the original M0/M1 body, then commits both
retained projections only when native return and table values match. It does
not add a caller for the unconfirmed path. New games initialize exact history
after `$03:B7C6`; old saves retain their actual US values and estimate the fewest
town-wide reductions consistent with those values. Fixed-point ambiguity
(`1 -> 1`) is explicitly approximate. Unknown patterns or later unobserved
writes quarantine history, rather than repairing native state. See
[save format](save-format.md) for the independent `ARLDELY1` block.

Recomp observes whole native transactions at `$03:BADD` (matched monster
defeat), `$03:BA42` (miracle), `$03:B4A6` (one house) and `$03:D095`
(action-score settlement). The first two capture eligible candidates before
the active stock's zero early-out. Defeat matches the first actor-record
address; it does not add a seal guard absent from the native leaf. Miracle
capture retains the `$C000` flags, effect kind and posted-effect busy gate.
House capture retains subtype and the four seal bits. Score capture uses the
completed region at `$0341`, not the previously selected town.

Each original body retains its native call frame, registers, flags and reward
side effects. After its return the observer computes a candidate history,
checks its active projection against native stocks, and commits only if they
match. An escape or mismatch retains the old history marked diverged; it never
repairs WRAM to fit the model. New-game initialization uses `$03:B7C6` or its
already-installed result, checking all24 seeds before starting exact tracking.
Completed saves perform another projection check before feature capture.
Live accounting switching runs before the `$03:8193` development controller
or the `$03:D095` score transaction,
outside active stock, miracle and earthquake transactions. It validates all24
native stocks against the effective projection before writing any counter,
then changes only `$7F:96B8-$96E7` and publishes the staged effective policy.
Registers, flags, actors, reloads, countdowns, sealing and rewards are untouched.
Unknown or quarantined history blocks switching; mismatches quarantine the
affected history without repairing WRAM. An unchanged policy takes a constant-time
early exit, with no per-frame history copy, stock scan or disk access.
For older saves, the normal Continue acknowledgement at `$02:A79F` reads the
24 durable stock words from SRAM `$1603-$1632`, not from transient WRAM. It
estimates only missing towns and persists the feature checkpoint before the
original `$03:A83A` restoration. Known histories must match their US projection;
divergent records are preserved for recovery. Cancel resumes the existing title
frame at `$02:A75B` after release of the native `$4219 & $D0` accept mask.
Record/replay follows its original route without fabricating acknowledgement.

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

### Town-status integration

The US-rooted runtime keeps five independent policy leaves: report classifier,
low-growth test, expected plot count, food-attempt marker and persistent flag
merge. The overlay groups these as **Town growth reports**. US and Europe use
the same numeric rules. Support coefficients and level goals are separate.

`ActRaiserTownStatusRuntime` captures a resolved snapshot around native
`$03:82DB` (complete construction calculation), `$03:BF8C` (six-town report),
or standalone `$03:91AE/$91BC` plot operations. Nested calls reuse that snapshot;
ordinary returns, coroutine yields and nonlocal return tokens retain native
ownership. Prefix replacements use the US RAM layout and native continuations:

| Prefix → continuation | Selected Japanese operation |
| --- | --- |
| `$8566 → $856B` | Load fixed threshold 4, retaining the US scratch store and comparison |
| `$85A3 → $85A7` | Compare against the Japanese plot count, preserving A and native CMP flags |
| `$85C3 → $85C6` | Omit scratch OR after the native `old & $50` |
| `$9271 → $9274` | Mark the eligible food attempt, then perform the displaced native load |
| `$BF9E → $C022` | Resolve the JP report code from US population, gate and stored flags |
| `$BFA2 → $BFA7` | Use threshold 4 when combined with the US report classifier |

The `$BF9A` loop root explicitly ends at `$BF9E`; otherwise generated code can
inline the US classifier on towns after the first. No menu callback rewrites
flags. A report before the next construction update intentionally sees stored
flags from the earlier policy. New Japanese status calculations keep only the
old `$10/$40` bits, even though temporary flags were computed.

### Regional growth-status producer

US/PAL `$03:855C–85C9` and JP `$03:851B–8579` rebuild the town status
at `$7F:91DA+2N`. This producer is distinct from the report-code consumer
above. The US-rooted caller is `$03:8346` in the development routine.

| Operation | US / European ROMs | Japan |
| --- | --- | --- |
| Low-growth scratch flag `$80` | Growth resource below `2*civilization+2` | Growth resource below 4 |
| Other scratch flags | `$08` from nonzero `$7CEF+2N`; `$04` from an active disabled class-2 field; `$01/$02` from the no-house-attempt/plot-count comparison | Same categories, relocated helpers and plot-count table |
| Expected plot-count table, town order | 28,29,39,25,15,20 | 29,30,40,26,16,21 |
| Final persistent write | `(old_status & $0050) | computed_flags` | `old_status & $0050` only |

Japan **does compute** the temporary flags in `$7C05`, but its final store
omits the Western `ORA $7C05`. Native tests distinguish the scratch value
from the stored result; this is not inferred solely from different bytes.
Both routines preserve only existing bits `$10/$40` before that final step.
The plot-count constants belong to status reporting, not population caps.

There is a related attempt-marker difference: with only the food footprint
available, JP's plot iterator sets `$7C3D=1` before attempting food allocation,
even with budget0; Western versions do not. Both mark an available house
candidate. Thus `$7C3D` must not be generalized to “a building was completed.”
The JP store difference above also means that changing the attempt marker
alone does not reproduce Western persisted status.

The five-ROM batch covers 490 native calls and ten bounded Go captures:
six towns, eight condition combinations, two prior-status values, and
food-only plots at budgets0/1. Every unrelated town status remains untouched.
The outputs repeat byte-for-byte. This establishes the producer semantics,
not developer intent, a complete development cycle or which messages a
naturally maximized JP town displays. Earlier forced report-code5 screenshots
prove that the text exists, not that this producer naturally selects it.

### Building geometry and conditional Fillmore calculation

The twelve 4×4 construction templates at US/PAL `$03:D3E2` (pointer table)
and JP `$03:CEE7` have byte-identical payloads across all five ROMs. With
zero-based coordinates inside a plot, every template places house marker
`$E9` at `(3,0), (3,1), (0,3), (1,3), (3,3)`. Food marker `$EA` is at
`(0,0)` and requires all four cells of the upper-left 2×2 footprint to pass
the availability check. The templates vary road placement, not house or
food positions. One food building consumes one structure record, not four.

The complete offscreen plot iterator is US/PAL `$03:91AE–933B`, JP
`$03:8F93–9126`. Its cell predicate is US/PAL `$03:96BE–96EE`, JP
`$03:94A9–94D9`: tile `$08` or `$D0–DA` **and** bit `$04` set in the
corresponding `$7F:3800` cell flag. That bit is required, not a prohibition;
terrain alone does not prove reachability. The predicate returns carry
clear for an available cell. Decode its alternate flag-test branch at
US `$03:96DC` / JP `$03:94C7` with M1/X0, not the M0 state left by the
preceding return path in a linear listing.

`$03:9710` / JP `$03:94FB` maps cell coordinates into four 16×16 quadrants:
`town*1024 + (y/16*2 + x/16)*256 + (y%16)*16 + x%16`, for cells 0–31
and integer division. The helper itself masks coordinates to five bits.
The plot iterator snapshots availability before allocating structures; it
does not run a census between candidates. Artificially supplying budget 5
can therefore allocate five houses against one cached population. However,
the **complete ordinary offscreen caller** US/PAL `$03:90DE–9155`, JP
`$03:8EC3–8F3A` supplies budget 0 or 1 according to the remaining growth
resource and consumes at most one unit per visit. Do not treat the
artificial five-house case as a demonstrated gameplay exploit.

A five-ROM batch covers 2,730 native calls: all 256 tile values with flag
`$04` clear/set, every template at budgets 1/5, individual food-footprint
obstructions, full offscreen caller at four resource values, and candidate
censuses. It includes 25 bounded Go disassemblies. Synthetic inputs test
these contracts; they do not establish a legal campaign or road network.

Counting eligible tile types at template positions in Fillmore's ROM base
layer (`0x50000–0x503FF`), excluding its cathedral plot, gives **142 house
sites and 18 food footprints**. This deliberately ignores path flags,
obstruction overlays, cursor access and event order. It is an input to a
model, **not 142 constructible houses** or an upper bound on every possible
event-modified layout.

A follow-up runs the complete native flood-fill wrapper, US/PAL
`$03:9156–919C` / JP `$03:8F3B–8F81`, from the cathedral plot `(3,3)` on
that unchanged base terrain. In every ROM it marks 363 cells, covering
**110 of the candidate house sites and 16 of the food footprints**. Food
sites `(4,12)` and `(4,16)` are disconnected. Replacing the terrain with
walkable tile `$08` is a positive control: all 1,024 cells and all candidate
sites become reachable. These ten native calls use the region's actual
metatile definitions, not manually injected path flags.

This is a failed constraint in the terrain-only candidate, not a developed
town simulation: it omits road/bridge construction, obstacle overlays and
event changes. It does not prove that those sites stay disconnected after
development or establish the minimum required bridge count.

For a restricted all-tier-three model with zero population adjustment,
mature factories, a census between admissions and 128 structure records,
enumerating 0–18 factories gives these conditional optima:

| Bridges assumed present | US residents (houses / factories) | JP residents (houses / factories) |
| ---: | --- | --- |
| 0 | 922 (115 / 13) | 586 (73 / 18) |
| 1 | 914 (114 / 13) | 602 (75 / 18) |
| 2 | 914 (114 / 12) | 618 (77 / 18) |
| 3 | 906 (113 / 12) | 634 (79 / 18) |

These follow from factory support 72 US /32 JP and bridge support 32 US /16
JP. For `H` houses, the last house's old population is `2+8*(H-1)`, which
must be at most `support+2`; record use must satisfy `H+F+B <= 128`.
Neither condition proves that the factories, bridges or houses can be
built in that order. Building at lower technology and later upgrading is
also outside this restricted model, so its optima are not general maxima.

The one-bridge US row matches the candidate in
[The Admiral's population guide](https://gamefaqs.gamespot.com/snes/563502-actraiser/faqs/47431);
the three-bridge JP row matches the total reported by
[GCGX's SFC population notes](https://gcgx.games/actraiser/tips.html).
The JP building composition is our model's hypothesis, not a claim taken
from that guide. Native censuses of synthetic records independently give
US population914/support968 and JP population634/support624. The remaining
test is coordinate-level reachability and a legal development history,
including why a larger alternative such as the zero-bridge US row cannot
be achieved. A matching number alone does not close that question.
The remaining requirement is a legal coordinate-level build/upgrade history
and an upper bound that includes alternative histories. The base-terrain
path test narrows that requirement; another synthetic census would not.

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

`ActRaiser_TownCensus` remains the integration owner, now separated into
`actraiser_town_census.c`. Its parameterized body accepts five support
coefficients, including completed extension bridges (32 US /16 JP), while
the bridge owner supplies validation and deduplication. The live entry resolves
the campaign's effective support policy; only confirmed redevelopment may change
that policy. Mixed-profile
tests cover every structure flag byte, and 6,144 US/JP ROM-decoded census cases
verify the native register/scratch/output contract. The census itself preserves
standing structures, the adjustment word US `$7F:9F57+2N` / JP
`$7F:9F4B+2N`, and earned levels.
Towns with no act completion skip census output stores; retain that gate.
The decoded US references to the adjustment are census plus save/restore;
they do not establish its full initialization/indirect-write lifecycle or a
separate regional adjustment rule. Do not reset it based on that limited scan.

The custom Recomp conversion uses `$01:85A2`, after Palace intro/arrival/event
processing and native `$03:8168` actor-cache retirement, before the next
`$01:8B7D` selector. It is not a native regional earthquake variant. It preserves
road/development lists `$7F:9250`, town construction timers and protected records,
retires removed current-town visual slots and the seven `$7F:9758` queue IDs,
and lets normal `$03:9D4D` town entry rebuild the display. No synthetic native
call or hidden town update is required to create the recovery image.

Empty-town population must remain positive: `$03:807F` treats zero as a new
town and initializes population2 and civilization1. Conversion therefore
rejects a developed town's adjustment of2 or more instead of resetting that
adjustment or letting subtraction underflow. Its rebuilding allowance is a
bounded reserve floor, not additive destruction credit: native offscreen
construction does not debit growth, so repeated additive refunds could be
farmed. The floor covers removed houses at the selected construction price,
or one start in a support-only town; larger existing reserves remain.
Player behavior and recovery are described in [Regional settings](regional-settings.md#population-rules-and-rebuilding).

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

**Planned host transition, approved September 22:** changes to effective
population-support rules will require confirmed redevelopment of affected
developed towns. A custom redevelopment earthquake will clear eligible
housing/support structures while retaining roads, bridges, story progress,
sealed lairs and earned levels. This supersedes the earlier non-destructive
support-switch proposal; it does not change what the original censuses do.
The ordinary player/Skull earthquake must retain its native behavior.
Reconstruction is planned to use normal town construction, with explicit
rebuilding resources so sealed or exhausted lairs cannot strand the town.
Checkpoint recovery, structure/cache cleanup, growth-budget reconciliation,
pending-event safety and supported policy combinations still require host
implementation and validation. Do not charge ordinary miracle SP or trigger
ordinary lair/house-loss rewards merely for conversion. This is not an
original regional mechanic or an implemented feature. Neither a fixed cap
nor a proof of every published population maximum is required for it.

The bounded redevelopment mutation core now preflights all affected towns
before writing any of them. Its removable allowlist is active classes0/2/3/4;
bridges and classes5/6 remain. Matching one-cell/2×2 marks become `$08`, and
the retired record's flags byte becomes zero, matching the non-reward portion
of native action7 (`$03:A44F` for houses, `$03:A477` for support buildings).
It rejects inconsistent/overlapping footprints, unsupported classes, pending
structure actions and a bias that would make the emptied census underflow.
Initialized action0 is not inherently busy: `$03:A004` latches bit7, and many
completed buildings remain in that state. This bit does not certify completion
of the separate visual program; that owner still needs a safe retirement boundary.

US construction charges `2*civilization+2` growth units per new structure,
including support buildings. `$03:8529` computes up to six affordable starts;
`$03:8422–8438` debits one such cost when the animated construction starts.
The custom core grants a reconstruction allowance for the houses actually
removed, using the selected construction price. Support buildings receive no
per-record credit: native visible construction returns their price at
completion. If a support-only reset would leave less than one start available,
the allowance tops up only that missing amount. It is not a reconstruction of
historical spending and invokes no ordinary house-loss rewards. Reapplying the
same preview is rejected; a fresh preview of the empty town grants no credit.
1,344 original-CPU checks cover budgeting, payment and support-return slices,
off-screen placement, and US action7 retirement with native redraw suppression. These bounded checks and
read-only previews of recorded developed towns are not a live redevelopment
or a demonstration of reconstruction. The switch remains unavailable until
checkpoint, quiescence/redraw, persistence, confirmation and rebuild acceptance
are integrated.

Japan uses a fixed four-unit cost instead: batch budgeting at `$03:84F2–851A`
subtracts4 up to six times, and the construction payment at `$03:8400–840C`
debits4. This is a separate gameplay rule from the low-growth *report* threshold
already selectable in Recomp. Recomp now pins the independent construction
selection across `$03:82DB` (visible batch) and `$03:84B9` (off-screen batch),
composing the existing town-status transaction without merging their settings.
Three prefixes substitute the fixed four-unit price: `$853B→8544` for budgeting,
`$8425→842E` for payment, and `$848E→8497` for the support-building return.
They preserve the original frames and native continuations; US/Europe retain
the original expressions. A change during a batch waits for the next batch.
Predecessors and loop-back targets have explicit generated roots; adding a
conditional prefix alone otherwise permits old arithmetic to remain inlined.
A 12,532-tick Continue/Fillmore comparison exercises23 visible payments:
eight units each in US mode, four with Japanese pricing. US final WRAM/SRAM
matches the pre-integration control byte-for-byte; both runs exit normally.

US/Europe support return `$848A–849B` explicitly clears carry during price
calculation. Japan `$845E–8464` loads4 without clearing carry before its growth
helper. Checks with both carry inputs produce4/5 in Japan and4/6/8 in the
Western versions; the replacement preserves this distinction rather than
inventing `growth += 4`. Off-screen house callbacks (US `$95B3`, JP `$939E`)
and Western support callback `$944B` consume the start token without debiting
growth. The off-screen selection changes affordability, not that native
accounting asymmetry. Unchanged policy snapshots resolve only their small
value fields; complete retained-history validation runs when a policy changes.

The US full-development scan `$03:C037–C071` is also not a maximum-population
formula: it returns carry set only after all128 records are active and every
house encountered has subtype `$20`. The first inactive record returns carry
clear/low-A byte0; the first lower-tier house returns carry clear/low-A byte1. Even
128 bridge records satisfy its predicate. These byte results are not whole
16-bit accumulator values. Do not expose its result or the six
report thresholds as independently proven population caps.

Two support add/subtract leaves, US/all PAL `$03:BF45/$BF5B` and JP
`$03:BC7A/$BC90`, both use16, **not regional field-upgrade deltas**. All five
retain identical 22-byte bodies for each operation. They select the support
word at `$7F:6B26 + [$7F:7BFB]`, wrap at 16 bits and do not saturate. The
360 isolated cases cover six towns, both entry accumulator widths and
0/16/65535 inputs; other town words and X/Y remain intact. Full A is restored
for M=0; M=1 restores only low A, leaving B from the calculation. M/carry
survive, but the final pulls update N/Z. These are leaf contracts, not normal
gameplay tests or a policy hook.

No direct JSR/JMP in bank03 or exact-bank long JSL/JML pattern was found in
any ROM, nor an ordinary bank03 pointer word. The Western target-minus-one
match at `$03:F8DF` spans `PHY; LDA long,X` opcodes, not a continuation table.
The configured US scan also found no caller. Indirect/cross-bank references
and why the helpers were retained remain unproved.

These findings close the census/admission and level-award **design questions**.
They do not close developed-town/geography/event validation. A fixed “maximum
population” toggle remains unsupported. Support, level requirements and report
classification can be separately selected policies, but each consumer must use
the effective policy and consistent canonical state at a completed simulation
transaction boundary. Exact host activation hooks and replay tests remain
implementation work.

### Magic gesture integration

US standing input `$00:9832` checks the attack bit first, then the dedicated
A/X gate at `$9843–984D`. JP `$00:985C` has no dedicated gate. Its ground
attack prefix `$9A6D–9A7C` removes held Y from the native release mask and tests
Up before choosing the sword or common cast body. The corresponding US
prefix begins at `$9A6E`; walking attacks also enter there. Airborne and
crouching attack entries are separate and are not replaced by this rule.

Recomp's Japanese gesture bypasses `$9843–984D` to `$984E` and replaces only
the ground prefix, continuing at US `$9A73` for a sword swing or `$9DE1` for
the existing cast gate. JP's mask word `$F9` maps to US `$F6`; the tested
input remains the word at `$A1`. The adapter preserves the prefix's register,
flag and mask effects, native return ownership and ordinary spell payment.
It does not remap controller buttons or make every Up + Y combination a cast.

A campaign request activates after completed NMI sampling when raw latched
Up/Y/A/X are all released. The debounced input word is not a release test:
it can be zero while Y remains physically held. The observer changes only
campaign metadata, not input latches, native masks, CPU registers or bindings.
The US/European paths remain generated native code. Unit tests cover every
16-bit release sample and 4,194,304 ground-prefix input/flag/mask cases; the
optional JP-ROM control interprets the original prefix, rather than using
the generated host code as its oracle.

### Lives convention

The integrated I02 option replaces only the two glyph writes in US
`$02:C280..C2A3`, resuming the common HUD at `$02:C2A4`. Entry is native
M0X0, PB2, D0, decimal clear, X `$0050`; the preceding `$C375` restores X.
JP writes the raw `$1C` nibbles as ASCII to `$7F:B050/$B052`, leaving their
attribute bytes untouched. Exit is M1 with A's high byte preserved, low byte
`$30+($1C&15)`, and N/Z/C/V clear. US/Europe delegates the original BCD
increment, including its debug-value behavior. Unsupported entry shapes stay
native. `$C206` ends at the seam so its generated body cannot bypass the hook.
No life debit or death dispatcher changes are needed for this display option.
Enhanced HUD text consumes the same final glyphs and invalidates only the
changed numeric cache entry.

The native oracle checks all 256 stock bytes in both ROMs; the host-prefix
test covers all input flag combinations with decimal clear and both high-byte
patterns. Two ordinary 1,800-tick Continue-to-Fillmore controls retain stored
`$1C=2` and differ only in the last lives glyph (`03` versus `02`) in the
HUD tile buffer. Their frame-1400 captures differ only within that glyph.
These are display controls, not new death/retry coverage.

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

Recomp's selectable score rule intercepts only the US `$00:981C–9825` prefix.
It retains `REP #$20`, the marker word load, marker consumption and resulting
A/N/Z; Japan adds the word clear at `$1F/$20`. Continuations `$9826` (retry)
and `$982F` (no marker) keep allocation and subsequent player logic native.
The captured rule is activated only for a nonzero marker. It does not insert
a score write into the death dispatcher, life debit or Palace loader.

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

The Recomp selector integration captures five independently stored choices at
the complete native player effect `$01:97E5` (kind4 only) or posted effect
`$01:9840` (pending kind4). It does not intercept bridges or the already-random
class6. US/Europe falls through to the original selectors. JP uses the native
`$03:AF65` RNG and rejoins the original preserve/destroy continuations, including
the house-only `$03:B4A6` feedback call. House-credit quantities remain a separate
regional rule. Settings edits cannot change a captured effect partway through.

Selector tests cover20,480 JP-prefix/native-US-continuation cases against the
JP ROM and5,120 US/PAL subtype cases across the four other ROMs, plus all243
mixed policies. Native helper contracts are modeled in these bounded tests;
they do not independently prove the RNG algorithm or the house-credit math.
The generated call paths and complete effects require separate live checks.

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

The Recomp development policy implements those three counts as independent
leaves, exposed as one pacing choice. Its bounded master coordinator preserves
scene-mode 7's early exit, the pending callback before the divider, and the
actor/recovery pass outside it. Both effect coordinators use the effective
policy and retain their existing actor/no-actor distinction. Their common
native callees still own events, actors, census, rendering and audio; choosing
JP pacing does not substitute JP media helpers. A pending choice activates
only in an eligible master call with both `$7F:91FE` and `$7F:9200` zero. It
does not reset those counters or `$7F:7CED`.

Implementation tests compare 26,880 master-coordinator cases (including all
five ROMs, with explicit address/callee normalization) and 896 effect cases
(including the US ROM). These compare CPU state, RAM, call order and native
frames, not the bodies of regional callees. All 27 source combinations also
test full cycles and independent actor counts; an interleaved effect/master
case verifies shared divider ownership. Native nonlocal returns are tested at
each called leaf. These are implementation tests, separate from the research
fixture totals below.

JP angel recovery is one HP per 60 eligible calls, capped at the current max;
state low nibble 4 freezes the recovery phase. A controlled US cast with two
HP and three SP already queued starts at HP 3 / SP 200, pays 160, and finishes
at HP 5 / SP 43 with both queues drained, while the development clock never
advances. No new long-cycle queue is generated during that effect.

Recomp keeps SP and angel recovery independently keyed under one town-recovery
menu choice. The cycle queue prefix and normal-angel drain retain the native
division/saturation callees. The JP eligible-call clock runs from both movement
decoder continuations, not just the US normal-state drain. Its native word at
`$0B04` overlaps the US SP queue byte at `$0B05`: the mixed-rule integration
therefore uses only byte `$0B04` for the bounded 0–59 phase. It never treats
pending SP as the high byte of that clock. On a numerical rule change, only
the changed queue/phase is retired at the next recovery service; current HP/SP
is retained. Equal US/European choices do not retire it. Initialization still
belongs to the native scene/load lifecycle, without a second persistent clock.

The recovery integration has 78,928 reference/ROM cases: US/PAL queue prefixes,
US drain and JP eligible-call leaf, with normalized PAL/JP stat addresses.
Native helper bodies remain observed contracts in these tests. Another 15,360
cases vary every valid JP phase against every possible adjacent SP queue byte.
Mixed rules, changed-leaf retirement, incoming widths and native escape tokens
have separate tests. These counts are not part of the earlier research totals.

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

The regional runtime captures S20 at `$01:9EE7`. Its Japanese wait choice
redirects only `$01:9F80` (M0/X0, PB=DB=1, D=0) to `$01:9F87`, bypassing
`LDA #$005A; JSL $03:B20C`. The destination's native `SEP`, item-ID load,
`JSR $921B`, `PLX` and `$9C85` cleanup remain intact. The donor counterpart
reaches consumption directly at `$01:9F56`. The setting does not replace the
target picker, seal wrapper, reward or inventory code.

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

### Sources of Life and Magic: collection versus Use

Items 5 and 6 are **Source of Life** and **Source of Magic**, not level-up HP
awards. Japan's Take Offering transfer stores both in held inventory; the
Western transfer invokes their Use handlers directly instead. The distinction
is present in all five ROMs, not inferred from translated receipt text.

| Boundary | US / all PAL | Japan |
| --- | --- | --- |
| Transfer and return-to-town flow | `$01:88FE–899B` | `$01:88E7–8978` |
| Special automatic-use branch | `$01:8916–8928`: IDs5/6 call `$9C6E` | `$01:88FF–8911`: IDs≥5 go to held insertion `$9141` |
| Source of Life Use | `$01:9CBD–9CD6` | `$01:9C99–9CB2` |
| Source of Magic Use | `$01:9CD6–9CF8` | `$01:9CB2–9CD4` |
| Remove first matching held ID | `$01:921B–9239` | `$01:915B–9179` |

Ranges above are end-exclusive. Source of Life increments the persistent
life-count byte (US `$02AB`, JP `$02AA`, PAL `$02AD`), which seeds ordinary
action entry; it does not change persistent HP. Source of Magic increments
the persistent magic byte (US `$0295`, JP `$0294`, PAL `$0297`) and working
scroll byte `$21`. Both handlers apply the increment, show acknowledgement,
then attempt to remove their item ID from held inventory. The live lives
display convention remains separate from these stored counters.

Ten controlled original-ROM transactions cover both items in every release.
Each starts from a previously booted Bloodpool scene with one offering seeded
in scratch town inventory. Western collection returns to town with the bonus
already applied and no held item. Japan returns with the held item and
unchanged counters; ordinary menu navigation to Use Offering then applies
the bonus once and removes the item after acknowledgement. Every transaction
returns to town with SRAM unchanged. These are collection/Use tests, not
natural discovery-event playthroughs. Capacity is tested separately
[below](#full-inventories-and-one-time-discoveries); counter overflow is not.

**Live-switch integration hazard.** Eight further controlled Western cases
start with a same-ID item already held, modeling a carry-over from Japanese
rules. Collecting a second Source applies only one bonus but the automatic
Use handler also removes the old held item after its acknowledgement. This
is a reproduced compatibility hazard for mixed policies, not an ordinary
Western acquisition bug. Simply calling the old Use handler during an
automatic collection would lose the carry-over item.

The integrated S18 policy retains US automatic collection as the default and
offers separate internal Life/Magic leaves under one overlay group. The
accepted collection prefix `$01:8916` captures the policy after the native
town slot is cleared and the item ID pushed. US resumes at `$8922`, Japan
at `$892D`; the original effect or held-insertion flow owns the remaining
transaction. Other item IDs keep the native branch. Policy edits do not
convert existing inventory or alter an acknowledgement already in progress.

Only the two Source consumption calls, `$01:9CCE/$9CF0`, can be bypassed.
At those sites, automatic collection has dispatcher return `$9C82` at S+1
and collection return `$8924` at S+4, with the saved P between them. If an
older same-ID Source is held, the adapter resumes at `$9CD1/$9CF3` without
removing it. Without that exact native chain and item match, the original
removal executes, including explicit Use under either policy. The native
effect, dialogue, sound request, P restore and town return remain intact.
There is no host modal flag, inventory snapshot/restore, or guessed refund.

Entries require M1X0, PB/DB1, D0, decimal clear. The collector reproduces the
native CMP flags: automatic C1/Z1/N0, manual C1 and Z only for item5, preserving
A and V. The bank coroutine root ends before `$88C3`; that owner ends at
`$8916`. The two effect owners end at their respective consumption calls,
preventing generated inlining from bypassing the guarded seams. No Japanese
graphics donor is required. The eighteen original-ROM transactions and twenty
Go windows above remain the independent reference, not host-game test claims.

Thirteen compiled-game controls follow normal Continue, Palace, Fillmore and
temple menus with a seeded offering in a scratch save. They cover automatic
Life/Magic with an older matching Source, manual collection, independent
Life/Magic choices, eight-held rejection, seven-held insertion, and explicit
Use after collection under both rules. Every control returns to town with
native SRAM unchanged. Automatic collection grants once while retaining the
older item; explicit Use subsequently grants once and removes that item.
These controlled offerings do not establish natural discovery reachability.

#### Source discoveries and Compass fishing

The checked town-grant census contains four item-5 sites and eight item-6
sites per ROM. The four Source of Life paths are:

| Town | US / PAL grant | JP grant | Discovery gate |
| --- | --- | --- | --- |
| Fillmore | `$03:E8AF` | `$03:E3A3` | Event 10's fishing counter reaches its regional threshold. Compass delivery enables this event. |
| Bloodpool | `$03:FB6D` | `$03:F649` | Rain in any of five authored lake squares, global bit 11 clear. No additional story test in this handler. |
| Kasandora | `$03:FBF9` | `$03:F6D5` | Earthquake, global bit 13 clear, prerequisite 8 set by exposing the pyramid, fired event 0 clear. Event 0 is the request to enter the pyramid. |
| Northwall | `$03:FC29` | `$03:F705` | Lightning in square `(2,5)`, global bit 14 clear. The six-town temple-coordinate table identifies this as Northwall's temple plot. |

The three miracle paths use the previously verified
[dispatcher and successful transactions](#miracle-triggered-story-events).
Their global bits make each discovery one-time; Bloodpool's five locations
are alternatives. These are item-grant sites, not five separate jewels or
an exhaustive proof against arbitrary direct inventory writes.

Compass Use is US/PAL `$01:9FF0–A02F`, JP `$01:9FBF–9FFE` (end-exclusive).
It accepts town indices 0 and 4, consumes item 19, shows its response, then
writes `1` to `$7F:91D4+town`. Other towns take rejection responses without
that write or consumption. The already-verified periodic handlers
`$03:F671/$F857` (JP `$F14D/$F333`) turn that byte into prerequisite 10.

| Fishing callback | US / PAL | JP | Completion counter | Reward / message |
| --- | --- | --- | --- | --- |
| Fillmore event 10 | `$03:E865–E8B6` | `$03:E359–E3AA` | `$7F:916E`: 255 Western, 128 JP | Item 5; request `$8B` (message 11) |
| Marahna event 10 | `$03:F247–F298` | `$03:ED23–ED74` | `$7F:9173`: 128 everywhere | Item 6; request `$89` (message 9) |

Each callback tests a scene-local initialization bit (global index 25/26).
When clear, it sets the bit, zeroes the counter and initializes the fishing
scene. Each callback increments the byte before testing equality. Completion sets fired 10,
clears dispatched 10 and grants the offering. Marahna slots 9 and 10 alias
the same callback; that alone does not make two independent discoveries.
The source-message catalogue and event-pointer tables establish these joins
without interpreting a message number as a second reward.
The dispatched-bit clear is an internal transition: outer event handling
can set it again before the next frame sample. Fired 10 is the completion guard.

One hundred native fixtures cover both counters at 0/1/126/127/128/253/254/255,
plus repeated original-selector calls through completion and the following
update. They preserve other towns' inventories and flags, and award once
through normal selector eligibility. Cinematic initialization is marked
already complete in these isolated fixtures. Those fixtures establish the
counter boundaries, not the modal or scheduling behavior below.

A separate 20-transaction live batch exercises original Compass Use,
initialization and completion in both towns, plus cancellation and Bloodpool
rejection, across all five ROMs. Each starts from a controlled same-town
fixture with a seeded Compass, unrelated events completed and enemies
retired; no ROM patches, substituted callees or coordinate warps are used.
Successful Use consumes the item before writing only the receiving town's
delivery byte. Each fishing event then awards one town offering; discovery
itself changes neither persistent lives nor scrolls. Cancellation and
rejection preserve the Compass. Bloodpool's ordinary crop refill can still
populate its empty town inventory; that is not a Compass reward.

| Live counter interval | US | JP | EU / DE / FR |
| --- | --- | --- | --- |
| Ordinary progress | 8 frames | 40 frames | 8 frames |
| Observed construction-clock crossing | 20 frames | 52 frames | 18 frames |

The development masters, US/PAL `$03:8193–8238` and JP `$03:8190–8243`,
service events at phase 1 of eight. Japan advances that phase only once per
five master calls. The longer observed gaps coincide with construction-clock
wraps; the first interval additionally includes the arrival dialogue. These
are controlled live traces, not universal waits under every menu or story
state. PAL's ordinary interval is 20% longer in real time at nominal 50 Hz
than the US interval at 60 Hz; the observed extra 10/12-frame boundary work
takes approximately 0.2 seconds in either timing system. All 342 retained
files from this batch match an independent repeat byte-for-byte.

Palace entry at US/PAL `$01:80D5–8111`, JP `$01:80CE–810A`, writes
`$01` to `$7F:9102` (store at `$01:8107` / JP `$01:8100`). This clears
the fishing-init masks `$40/$20`, but not the counters, Compass bytes or
per-town fired flags. Returning to unfinished Fillmore or Marahna fishing
therefore zeroes its old counter and starts again; the first frame-visible count is 1.
These initialization bits are not permanent discovery flags.

Five additional button-only transactions leave Fillmore mid-fishing, wait
1,800 frames in the Palace, return through Observe the People and complete
the reward. In every ROM the Palace wait leaves the counter unchanged,
return starts a new 1-to-target sequence, and the town keeps its Compass
marker without another held item. These excursions make no RAM edits and
leave SRAM unchanged. They start from the controlled Compass batch, not a
natural campaign. Twenty-five exact Go windows cover both town callbacks,
the scene route, Palace initialization and development master in all five
ROMs. Marahna's restart is source-established; the live same-town excursion
covers Fillmore only. Both batches have independent byte-identical repeats.

Live-switch design: keep Fillmore's counter and the shared Marahna/Northwall
counter, alongside each event's own fired state. The additional owner and
its different initialization rule are detailed [below](#northwalls-lake-search-shares-marahnas-counter).
If changing Fillmore's threshold during
an unfinished expedition, reconcile progress at a serviced event boundary
and complete once if the new threshold has already been reached. Do not
leave a counter above a new equality target to wrap around, restart the
expedition solely because a setting changed, or clear its completed flag.
Keep the native Palace-triggered restart distinct from a settings change;
preserving unfinished fishing across Palace visits would be a separate QoL
policy. This threshold and the offering's automatic/manual activation policy
are independent settings and require no
Japanese artwork donor.

The Recomp adapter replaces only Fillmore's `$03:E865–E88B` prefix. It keeps
the native initialization-bit helpers and fishing-scene call, then transfers
to `$E88C` (continue waiting) or `$E895` (completion). Native eligibility/fired
guards and reward insertion remain in charge. A numerical target change
reconciles progress already at or above the new target; unchanged targets keep
the original increment-before-equality behavior, including byte wrap in unusual
debug states. No setting write changes the counter or grants an item directly.

The other item-6 callbacks are Fillmore 7/13, Bloodpool 1, Kasandora 3/7,
Aitos 5 and Northwall 5. All eight grant sites have rooted callback joins
and hash-checked Go-extracted messages in all five ROMs; this census does
not replace full natural-event playthroughs.

#### Northwall's lake search shares Marahna's counter

Northwall event 3, US/PAL `$03:F3AB–F3F0` / JP `$03:EE87–EECC`
(end-exclusive), reads and increments `$7F:9173`, the same byte used by
Marahna's fishing callback. It tests equality with 255 in every release.
On completion it marks Northwall fired 3, clears dispatched 3, requests
`$920E=$9D` (message 29) and grants item 4, Magical Light.

| Counter owner | Initialization | Target | Completion guard |
| --- | --- | ---: | --- |
| Fillmore fishing | Reset `$916E` when global initialization bit 25 is clear | 255 West / 128 JP | Fillmore fired 10 |
| Marahna fishing | Reset `$9173` when global initialization bit 26 is clear | 128 | Marahna fired 10 |
| Northwall lake search | Keep `$9173`; global initialization bit 25 only guards scene creation | 255 | Northwall fired 3 |

Northwall's initialization sets global bit 25 and creates the scene from
US/PAL `$03:E5E2` / JP `$03:E0E1`; it does **not** zero the counter.
This distinguishes it from both Compass-led fishing callbacks. Palace
initialization clears the scene bits, but leaving the counter intact alone
does not imply that all three events restart or all three resume.

A fresh five-ROM direct-long-access census finds two reads and three stores
to `$7F:9173`, all within the Marahna and Northwall callbacks. The three
stores are Marahna's reset, Marahna's increment and Northwall's increment.
This is a bounded literal-access census, not a proof against indexed access,
bulk initialization or save restoration.

One hundred forty-five isolated original-selector controls cover Northwall's
counter boundaries, first-time scene creation, eligibility and fired guards,
full completion from 0/128/255, and Marahna reset controls. Northwall keeps
its initial value even when creating its scene; Marahna restarts at zero
when its own initialization bit is clear. From 255, an unfinished Northwall
event wraps and needs 256 updates to return to its equality target. Its fired
guard still prevents a second reward after completion.

Ten original-ROM scene transactions compare these cases in all five releases:

| Input to Northwall | Search updates to reward | Result |
| --- | ---: | --- |
| Preserve completed Marahna counter, 128 | 127 | One Magical Light; Northwall fired 3 set |
| Explicit zero-counter comparison | 255 | Same reward and completion state |

Each starts from the accepted completed Marahna fishing fixture, leaves for
the Palace through its menu, closes that menu and requests Northwall through
the original scene loader. The test explicitly supplies the destination,
act/eligibility state and neutralized enemies; only the comparison branch
changes the shared counter. Northwall's original loader places the angel;
no actor coordinates or ROM code are patched. The retained value survives
Palace entry, Northwall loading and its first-visit introduction. Original
announcement, temple dialogue and reward processing then complete normally.
Marahna's existing offering remains intact and SRAM is unchanged.

These establish the state transfer through the tested lifecycle, not a
continuous natural campaign, save/reload behavior or the reverse live
Northwall-to-Marahna journey. The 60 exact Go windows, 145 isolated controls
and ten scene transactions have a byte-identical independent repeat.

Message ownership matters here too: Northwall's slot-29 callback is only
`CLC; RTS`, yet the message is used by event 3 through pending `$9D`.
An empty callback is therefore not evidence that its dialogue is unused.
The regional slot-3/29 and Marahna slot-9/10 text spans were checked against
the Go catalogue and ROM hashes; their distinct roles must not be merged.

Integration: preserve this shared byte for native compatibility. Replacing
it with independent Marahna and Northwall timers would change the wait and
requires an explicit behavioral decision, not a routine data-model cleanup.
Keep completion flags town-specific so an unrelated reset cannot regrant
completed rewards. No regional toggle or QoL fix is implemented by this research.

#### Full inventories and one-time discoveries

Held inventory and each town's offerings have separate eight-slot limits.
The ninth allocated byte is not a spare slot. The following behavior is
shared by all five ROMs, rather than another regional policy difference.

| Boundary | US / PAL | JP | Full-inventory behavior |
| --- | --- | --- | --- |
| Take Offering entry | `$01:8491–84B2` | `$01:8458–8479` | Count held slots; eight blocks temple entry before item selection |
| Town grant wrapper | `$01:A076–A087` | `$01:A045–A056` | Resolve active town, call first-free insertion, return its carry |
| First-free insertion | `$01:9204–921B` | `$01:9144–915B` | Eight occupied slots return C=1 without changing inventory |

Ranges are end-exclusive. Twenty original-menu transactions test Sources
5/6 with seven or eight held items in every release. At eight, the game
shows its full-possession response, does not enter the temple and leaves
both inventories and all Source bonuses unchanged. At seven, Japan stores
the Source in the last free held slot; Western releases apply it immediately
without filling that slot. Bloodpool's ordinary crop refill runs after the
successful collection returns to town, independently of the Source reward.

The discovery side does not have this protective gate. The named callbacks
commit their one-time guard before asking the town inventory to accept the
reward, then ignore insertion failure:

| Discovery | Guard committed before grant | Reward |
| --- | --- | --- |
| Fillmore fishing | Town 0 fired event 10 | Source of Life (5) |
| Marahna fishing | Town 4 fired event 10 | Source of Magic (6) |
| Bloodpool lake Rain | Global flag 11 | Source of Life (5) |
| Kasandora pyramid Earthquake | Global flag 13 | Source of Life (5) |
| Northwall temple Lightning | Global flag 14 | Source of Life (5) |

One hundred isolated calls cover seven/eight occupied town slots for these
five owners across all releases, followed by a retry after freeing an
existing slot. Fishing goes through the original event selector. The first
miracle calls stop immediately after the native grant, before modal text;
their retries execute the original guard and return normally. Seven items
allow insertion; eight retain all existing items but store no reward. The
ninth byte and other towns' inventories remain unchanged. Clearing a slot
after either outcome does not replay a completed discovery.

Ten further live fishing transactions cover both towns in all five ROMs.
They restore the accepted in-progress Compass states and change only town
inventory bytes. With eight items present, the original scene reaches its
completion counter and fired flag but supplies no Source. After explicitly
freeing a slot, 1,800 more input-free frames do not recover the offering.
All thirty live transactions leave SRAM unchanged. Forty-five exact Go
windows and an independent repeat support the batch.

These are controlled capacity states, not a demonstrated route to eight
uncollected offerings in each town. The conditional reward loss is established;
its ordinary-play reachability and save/reload recovery remain unverified.
The [grant-owner census](#offering-grant-owners-and-capacity-limits) below
narrows the source accounting without supplying that campaign proof. Do not
label it a new JP-only bug or infer that every grant caller ignores carry.

Integration: inventory insertion must report success separately from story
completion. Keep the native held-capacity gate under both activation policies
unless an explicit QoL option changes it. Likewise, deferring a rejected
one-time reward would change shared native behavior, not select JP rules.
Any such policy needs explicit pending-reward ownership and exactly-once
delivery; clearing fired/global flags to retry would also replay story effects.

#### Offering grant owners and capacity limits

All five ROMs contain the same **28 direct calls** to the town-insertion
wrapper: 20 calls within 19 distinct story callbacks, seven within periodic
or miracle handlers, and one indexed lair-reward call. Fresh Go disassembly
joins every byte-pattern candidate to its event-table or separately identified
handler entry. This closes ownership of that bounded census, not indirect
calls or arbitrary writes to inventory.

The 24-word lair table at US/PAL `$03:B734` / JP `$03:B4BD` supplies one Bomb
(18) and one Strength of Angel (20) per town. Each town's other two entries
are `$8000`, selecting technology upgrades rather than inventory items.

| Town | Story, periodic and miracle reward opportunities | Lair items | Total listed opportunities |
| --- | --- | --- | ---: |
| Fillmore | Two Sources of Magic, Source of Life, Bridge, Magical Fire; additional Magic Skull with no identified trigger | Bomb, Strength of Angel | 7, plus the unexplained Skull event |
| Bloodpool | Source of Magic, crop, Bread, Magic Skull, Compass, Magical Stardust, Source of Life | Bomb, Strength of Angel | 9 |
| Kasandora | Two Sources of Magic, Music, Ancient Tablet, Source of Life | Bomb, Strength of Angel | 7 |
| Aitos | Source of Magic, Fleece | Bomb, Strength of Angel | 4 |
| Marahna | Herb, Magical Aura, Source of Magic | Bomb, Strength of Angel | 5 |
| Northwall | Magical Light, Source of Magic, Source of Life | Bomb, Strength of Angel | 5 |

These totals count recorded reward opportunities, **not maximum simultaneous
inventory** or proven complete routes. Marahna slots 9/10 alias one fishing
callback and are counted once. Bloodpool's later crop refill is a separate
call site, but requires an empty town inventory; it cannot stack extra crops
beside pending rewards. Its Bread must also be collected and consumed before
Teddy's return supplies the Skull. Thus the nine listed Bloodpool rewards
cannot simply be treated as nine items left waiting together. These owners
provide no demonstrated ordinary overflow route for the Source discoveries;
the controlled capacity result remains a compatibility edge case rather than
a confirmed player-facing loss.

Callback grant identities, excluding the indexed lair call, are:

| Town | Event slots → item IDs | Other owners → item IDs |
| --- | --- | --- |
| Fillmore | 7→6, 10→5, 13→6, 14→14 | Bridge→10; southeast-rock Lightning→1 |
| Bloodpool | 1→6, 5→8, 6→7 then 14, 9→19 | Lake clearing→2; crop refill→8; lake Rain→5 |
| Kasandora | 3→6, 4→11, 7→6, 9→13 | Pyramid Earthquake→5 |
| Aitos | 5→6, 8→15 | — |
| Marahna | 2→9, 6→3, 9/10→6 | — |
| Northwall | 3→4, 5→6 | Temple Lightning→5 |

#### Fillmore's additional Magic Skull event

The Fillmore callback table retains a separate item-14 grant, distinct from
Bloodpool's Teddy-return branch. Addresses below are end-exclusive; event
numbers and item IDs are decimal.

| Owner | US / PAL | JP | Effect |
| --- | --- | --- | --- |
| Fillmore event 14 | `$03:E8EF–E913` | `$03:E3E3–E407` | Attempt item-14 insertion; set fired/dispatched 14; enable event 15 |
| Fillmore event 15 | `$03:E913–E927` | `$03:E407–E41B` | Set fired 15; request pending message `$8F` |
| Original event selector | `$03:E19C–E1F2` | `$03:DCA1–DCF7` | Require enabled and not fired; dispatch through the active town's table |

Both Fillmore slots resolve to the same message within each release:
US/European English bank04 `$9DA3`, Japanese bank02 `$BA61`, German bank04
`$9DA4`, French bank04 `$9E0B`. The Go-extracted text describes finding and
offering a skull-shaped statue; the source spans were rehashed against each
ROM. This is retained translated content, not text inferred from item names.

One hundred twenty isolated original-selector controls cover all five ROMs,
enabled/not-enabled, fired/not-fired, empty/full inventory and one/two/three
passes. Only enabled, unfinished event 14 attempts the grant. The first pass
completes 14 and enables 15; the second completes 15 and requests `$8F`.
Further passes add no item. At full capacity, insertion fails but both events
still finish: this callback does not defer the reward. The ninth inventory
byte and other towns' inventories and event flags remain unchanged.

The activation search has a narrower conclusion. None of Fillmore's six
population rows or its road row selects event 14/15. Its three pointer-rooted
periodic handlers (`$F621/$F671/$F68A`, JP minus `$0524`) do not enable 14.
Across each complete ROM, neither adjacent literal setter idiom—load event
14 then the prerequisite pointer, or the reverse order—occurs. These checks
do not exclude nonadjacent, computed or direct bitmap writers. Event 15's
producer is identified, but event 14's ordinary enabling source is not.
Forced eligibility proves the retained code works, not that normal play can
reach it or that a pre-release build used it.

The owner/Skull batch retains 215 exact Go windows and the 120 isolated
controls above, with an independent byte-identical repeat. It adds no live
campaign trace. Do not merge Fillmore's unexplained event into Teddy's semantic
dialogue route or expose it as a regional toggle merely because it exists.

#### Dialogue-only slots and indirect message owners

A five-ROM follow-up distinguishes retained messages from their callback
slots. All addresses below are message starts, not executable entry points.
US and European English use the same starts in this table.

| Assigned slots | US / EU English | Japan | German | French | Finding |
| --- | --- | --- | --- | --- | --- |
| Fillmore 6 | `$04:99EE` | `$02:B764` | `$04:99FE` | `$04:9AD2` | Strange-jewel offering announcement; bare RTS callback; no identified ordinary producer |
| Kasandora 6 | `$04:AE0C` | `$02:C8F1` | `$04:AE73` | `$04:AE33` | End-only blank entry, not recovered dialogue |
| Kasandora 25/26 | `$04:B180` | `$02:CC10` | `$04:B1F5` | `$04:B195` | Prosperity/thanks message, used indirectly through slot 26 |
| Aitos 25–29 | `$04:B881` | `$02:D225` | `$04:B960` | `$04:B8F8` | Magic-discovery announcement; common CLC/RTS callback; no identified ordinary producer |

Kasandora event 0 owns the prosperity selection: US/PAL
`$03:EC02–EC7D`, JP `$03:E6EE–E769` (end-exclusive). With event 0 enabled
and unfinished, act count not equal to 2 and prerequisite 8 clear, it writes
pending `$9A`, then unwinds the callback dispatcher with carry set. The
Listen selector consequently returns message 26. If prerequisite 8 is set,
the callback takes its other scene path; act count 2 instead finishes event
0 and enables event 1. Eighty original-selector controls check these gates
across the five ROMs. The slot-25 alias is not evidence of another unused
message: it shares exactly the text already selected as 26.

For Fillmore 6 and Aitos 25–29, the population and road tables do not select
the slots. Whole-ROM searches for the two adjacent prerequisite-setter
idioms and immediate forced-message stores find no matching town owner.
Every resulting literal hit is decoded from a callback, periodic-list or
miracle-table root: the same event numbers in other towns are not references
to these messages. The indexed lair-reward path supplies messages 30/31
for its positive item rewards, not these candidates. The existing direct
grant census has no Aitos spell-1–4 grant.

These are bounded producer checks, not a proof against every computed,
nonadjacent or direct bitmap write or an independent text-pointer caller.
The blank Kasandora slot is not a cut story. The remaining offering texts
are unexplained, not confirmed pre-release features or regional options.

Two hundred twenty-five native controls cover all nine candidate slots:
the real message selector accepts explicit eligibility or a forced pending
ID, rejects disabled/completed ordinary entries, and the empty callbacks
do not grant rewards or mark themselves complete. These controls establish
selection behavior under supplied state, not ordinary gameplay reachability.
The Go catalogue source spans were rehashed against every ROM.

Together with the Aitos flag investigation below, this batch retains 440
isolated controls and 140 exact Go windows with a byte-identical independent
repeat. No new live campaign trace is included.

#### Northwall scroll callback's constant condition

Northwall event 5, US/PAL `$03:F424–F458` / JP `$03:EF00–EF34`, begins
`SEP #$20; LDA #$A3; BEQ ...`. The operand is immediate: it does **not**
read `$7F:91A3`, and the following rejection arm is never selected from
this entry. That arm would clear prerequisite 5 and unwind the event
dispatcher. Do not infer an intended RAM read or patch it as a bug without
separate evidence.

Forty-five native fixtures vary `$91A3` over 0/1/255 and use direct callback,
eligible selector and ineligible selector entries. Direct and eligible
entries grant item 6 and mark fired 5 regardless of `$91A3`; the ineligible
selector grants nothing. Thus the local test is inert, but ordinary event
eligibility still applies. No early natural acquisition route is claimed.
Together with fishing, the discovery batch adds 145 isolated fixtures and
80 exact Go windows; its independent repeat matches all retained evidence.

### Crop offering replenishment

The replenishment leaf is US `$03:F791–F7AD` / JP `$03:F26D–F289`, entered
with M=1, X=0 and DB=`$7F`, returning through RTS. After relocation, its
instructions and the reviewed inventory/flag helpers agree:

1. Test prerequisite **event ID 5** for the current town. Bitmaps are MSB-first;
   in Bloodpool this is `$7F:910B & $04`, not mask `$20`.
2. Resolve the current town's inventory and count all eight usable slots.
   Any nonzero item blocks refill; this is not a search for item 8 alone.
3. If empty, insert item 8 into the first free slot. Held inventory is not
   consulted. The ninth allocated byte is outside the count/insertion loops.

Normal ownership comes from the six-town maintenance lists at US `$03:F5ED`
/ JP `$03:F0C9`. Only Bloodpool's list includes this leaf. The list contains
RTS-target-minus-one words, terminated by `$FFFF`; dispatcher US `$03:F5BE`
/ JP `$03:F09A` runs the selected list. Calling the leaf under an arbitrary
town's context would bypass that ownership. Initial story-event grants are
separate from this replenishment path.

The master development scheduler calls town maintenance at subcycle 4 of 8.
US advances the subcycle every master call; JP advances it every fifth call,
using `$7F:7CED`. Consequently, repeated opportunities are eight versus forty
master calls apart while development is serviced. They are not full 720/480
development-step construction cycles, nor unconditional wall-clock timers.
The menu frame service at US `$01:92B7` / JP `$01:924C` does not run this
development pass and returns immediately in temple scene 8.

Controlled booted Bloodpool traces start with development phase/clock/divider
zero and completed story bitmaps to suppress unrelated pending dialogue.
An empty temple inventory receives item 8 at active frame 4 US / 20 JP while
the held inventory already contains item 8. Native Take Offering traces then
transfer a crop into held inventory, keep the temple empty throughout the
receipt dialogue, and replenish only after returning to town at development
phase 4. The Japanese receipt requires another acknowledgement in these
fixtures; absolute input-to-return durations are not crop cooldowns. SRAM
and story bitmaps remain unchanged. These are controlled lifecycle tests,
not complete playthroughs of Bloodpool's initial crop event.

The shared inventory helpers are:

| Operation | US | JP | Contract |
| --- | --- | --- | --- |
| Resolve town base | `$01:91D3` | `$01:9113` | Y = base `$024C/$024B + 9*(town_id-1)` |
| Count entries | `$01:91E7` | `$01:9127` | Y = inventory base; eight bytes; A = count, C = nonempty |
| Insert item | `$01:9204` | `$01:9144` | A = item, Y = base; first empty of eight; C = failure |
| Sort entries | `$01:9239` | `$01:9179` | Y = base; sort eight item IDs descending; zeros move to the end |

These bank 1 leaves use M=1, X=0 and DB=1. The bank-switching wrappers start
at US `$01:A052` / JP `$01:A021`. Sorting is **not stable compaction**:
surviving entries can change position. Removal still targets the first matching
item ID, not necessarily the UI's selected slot. Preserve the ninth byte.

There is no independent US/JP replenishment policy to expose from this result.
Keep the shared gate/insertion transaction and Bloodpool ownership; apply the
selected development cadence at its scheduler boundary. A settings toggle
must not run the refill leaf or grant an extra item.

### Crop offering menu artwork

Offering ID 8 is labelled `こめ` (rice) in Japan, `Wheat` in US/European
English, `Weizen` in German and `BLE` in French. The names come from the
Go-extracted fixed-text route `sim.menu.possession.slot_07`, with each raw
source span checked against its ROM. The selected and grey menu icons are
pixel-identical across all five releases; this is not a redrawn rice icon.

Every label record selects family `$3C`. Native initialization uses
US/PAL `$01:AC36–AC6F`, JP `$01:ABF9–AC32`, and family tables
`$01:A227` / `$01:A1F6`. The family resolves these two one-frame scripts:

| Variant | US / all PAL script → composition | JP script → composition | Part attribute |
| --- | --- | --- | --- |
| Selected | `$01:A451` → `$01:D36D` | `$01:A420` → `$01:D2F7` | `$0BC4` |
| Grey | `$01:A531` → `$01:D61F` | `$01:A500` → `$01:D5A9` | `$0FC4` |

Both compositions are one 16×16 part using tiles `$1C4/$1C5/$1D4/$1D5`
from the SIM OBJ upload at file `0x68000`, with palette 5 or 7. The four
tiles and both palette slices match byte-for-byte. Native asset scripts
declare the 16 KiB character upload to VRAM word `$2000` and the OBJ palette
to CGRAM `$80–FF`; palette source files are US `0xE3C93`, JP `0xE1EDD`,
EU English `0xE2FA6`, German `0xE2AEA`, French `0xE2ACE`.

Ten isolated original-ROM calls verify family/variant resolution, backed by
five Go-decoded initializer windows. The existing Go pixel renderer produces
the ten compared icons; all 17 evidence files reproduce exactly. This closes
the **offering-menu artwork** claim only. It does not establish equality of
town field graphics, revisit crop mechanics, or add a booted menu playthrough.

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

#### SIM combat integration

The host implements five independent combat leaves; it does not add switches
for unchanged values. Each verified native birth captures combat and AI snapshots
together through the shared `regional_sim_actors` owner.
The narrow prefixes `$01:B018→B01C` and `$01:B0CC→B0D0` replace the threshold
load and contact subtraction, preserving accumulator high bytes, native flags,
the strict death comparison and subsequent native effects. Arrow accumulation,
SP awards, knockback, lair stock changes and soul rewards retain their owners.
All five cartridge tables were compared; the four Western releases agree.

The collision scan `$01:B147` checks four `$26`-byte records starting at `$0B30`.
All 24 lair seed records map their town's four lairs to those same four slots.
The birth hook at `$03:B9EE` validates town, slot, species and the inactive flag
before displacing `LDA #0`; the uninterrupted native field clearing follows.
The spawner's `$B993` back edge is an explicit decode boundary, so later births
cannot bypass the hook through an inlined native loop.

Actor policy has separate active and six-town cached copies. Successful native
cache load/save bodies `$03:813F/$03:8168` synchronize the matching policy copy;
restoration is not treated as a new spawn. This preserves an older cached soul
even after its live slot becomes a new monster, until native caching actually
replaces it. The companion binds both snapshots to the native completed-save
image; old companions assign US rules to their existing actors. Collision
lookups are constant-time and never inspect other towns or activate a request.
See the [companion layout](save-format.md) and [player setting](regional-settings.md).

The four SIM species' 64 state entries were paired separately from scheduling:

- Blue Dragon state 1: US target search every eligible update, JP only after
  the eight-count gate at `$01:BA0C–BA1A` (US state entry `$01:BA5F`).
- US Blue Dragon state 6 calls the world-actor pass again at `$01:BB5C` after
  spawning its strike effect; the corresponding JP state has no such call.
  The native ordering checks below verify extra movement and timer updates
  for other active actors, including nested strikes.
- Napper Bat's no-house-target fallback compares native `Random($FF)` to US
  `$FD` at `$01:BEE3`, JP `$FA` at `$01:BE6D`. Its state-4 timer is US 1 at
  `$01:BF72`, JP 60 at `$01:BEFC`.
- Shared target selection US `$01:BCB0–BD89` biases random candidates using
  town offsets `$7F:6B9F/$6BAB` and a smaller local range. JP
  `$01:BC5F–BD13` samples the wider 32-cell grid without those offsets.

#### SIM AI integration

Six independent numeric leaves select the search interval, extra actor pass,
candidate range, lookup pool, Bat fallback threshold and Bat wait. Like combat,
they activate only on a verified birth and survive native actor cache copies.
An overlay edit cannot change a live monster's search or attack halfway through.

| US seam → continuation | Japanese-rule prefix | Native work retained |
| --- | --- | --- |
| `$01:BA67 → BA6A` | Reset Dragon search counter after animation setup | Original `$D072` call and its return contract |
| `$01:BA6A → BA6D` or `$BA79` | Increment counter; search every eighth eligible update | Fallback attack still runs on skipped searches |
| `$01:BB5C → BB60` | Omit only the extra world-actor pass | Strike effect, destruction, sound and state advance |
| `$01:BCBC → BD45` | Generate the wider-grid candidate | Sealing gate before the prefix; exactly two calls to native `$03:AF65` |
| `$01:BD46 → BD4A` | Call existing full-pool `$03:BDE1` instead of sliced `$BDEF` | Found/class/flag checks, stack restoration and target assignment |
| `$01:BEE3 → BEE5` | Compare the random byte with 250 | Native fallback branch and animation |
| `$01:BF72 → BF75` | Load the wait value 60 | Store, immediate decrement and subsequent Bat states |

The candidate prefix changes coordinate generation only. A generic `$FF` target
still follows the native acceptance rule; failed lookup does not erase an old
target. The pool and coordinate rules are separate leaves, so mixed selections
do not accidentally select both at once. All random draws and structure scans
remain native; the adapter neither invents a host RNG nor adds a per-frame scan.

`$01:BB5C` resolves ownership from the parent Dragon's stacked X. The preceding
strike-effect allocator may already have changed live X; using that effect or
a global “current monster” would fail during nested actor passes. Species checks
read `actor+$0E` as a byte, leaving Bat carrying state at `+$0F` independent.
Native helper escapes propagate without running a continuation; US-valued
leaves take their original code. The candidate prefix has ROM-decoded parity
tests over all 32×32 draws, both alignments, three target classes and four input
flag combinations. Separate tests cover helper call frames, continuations,
per-generation ownership, saved histories and old combat-only replay digests.

Address-normalized state comparisons are not whole-world equivalence proofs:
shared callees, animation, RNG order and US `BRK` versus JP `COP` audio dispatch
still matter. The target/strike checks and Skull contracts below now have
positive native evidence, but do not cover every state or campaign situation.

### Monster target search and retry gates

Another 10,919 isolated native calls and 30 exact Go windows cover all five
ROMs. Candidate arithmetic and the 16-byte RNG are checked against an
independent model; all outputs reproduce on a separate run.

US/PAL target search `$01:BCB0–BD89` uses `$03:BDEF–BE42`, which examines
only 16 of the town's 128 structure records. The selected block is
`(frame_counter & 7) * 16`, using `$88` in US and `$89` in PAL. JP search
`$01:BC5F–BD13` calls `$03:BB6A–BB77` / `$03:BB0D–BB44`, which examines
all 128 records. Every slot and all eight frame phases were tested with a
single matching structure. A Western search can therefore miss an existing
target simply because its record lies outside that call's slice. This is
record order, not a geographic map partition. Destruction-time lookup uses
the full-pool search in every version.

Both target routines try one coordinate candidate, reject inactive records,
and normally require a matching low-nibble class without flag `$40`. The
generic class `$FF` bypasses the class and `$40` checks, but not the active
record requirement. This is a tested helper contract, not proof that every
caller uses `$FF`. Rejection retains the actor's previous target coordinates.

Candidate selection differs as follows (coordinates are in map cells):

- JP field-class 2 aligns two `Random(32)` values down to multiples of four.
  Other classes align one axis to 3, 7, …, 31 and leave the other anywhere
  in 0–31; Angel X bit 1 chooses which axis is aligned.
- US/PAL class 2 uses two `Random(3)` values multiplied by four. Other
  classes use `Random(12)`, with one axis masked by `$0C` and decremented.
  The routine then adds its origin offsets from `$7F:6B9F/$6BAB` with
  byte arithmetic. Zero-origin edge fixtures verify wrapping rather than
  silently clamping the resulting candidate to the map.
- Sealing gate `$7F:7BEB` rejects before generating a candidate.

In fresh Blue Dragon state 1, US/PAL attempt a search on every eligible
handler call. JP resets a counter and searches on calls 8, 16, and so on.
Empty-pool tests through 33 calls verify both transitions and exact RNG
consumption; these are handler counts, not a measured wall-clock ratio.

For the Napper Bat's no-house fallback, `Random(255)` values 253–254 select
the Angel in US/PAL; JP accepts 250–254. All 256 initial one-byte seeds were
tested with house search blocked. The returned range is 0–254, so input
255 is not another accepted outcome. This does not assume a uniform RNG
distribution. State 4 lasts one eligible call in US/PAL and 60 in Japan
before selecting state 5. Its native timers agree with those boundaries.

### Dragon recursion, bat carrying and field damage

A further 3,660 native fixtures and 35 bounded Go windows cover five-ROM
strike effects, house destruction, bat departure and Red Demon field damage.
All results repeat independently.

US/PAL Blue Dragon state 6 calls `$01:B898–B8CF` before destroying its
target. That pass revisits every active world actor, not just the effect
it spawned. With one, two or three staged Dragons reaching their strike
together, a separate moving Bat receives two, three or four updates during
one outer pass. Both its animation/state timers and position advance that
many times, whether its record is before or after the Dragons. Flagged
inactive/suppressed observers do not advance. Full effect pools do not
prevent the extra pass. JP's `$01:BADB–BB1E` does not recurse; the observer
receives one update in each case.

The nested pass also revisits the striking Dragon before its outer call
sets state 1, underflowing its now-zero strike timer. This observed ordering
must not be replaced by a per-Dragon cooldown while claiming native parity.
The fixtures bound concurrency to three strikes; they are not a maximum
recursion-depth or real-time performance measurement.

Shared house destruction is US/PAL `$01:BDDA–BE27`, JP `$01:BD64–BDB1`.
It finds the target cell using `(target_x/16,(target_y+44)/16)`, selects
structure action 2 and runs two full structure updates. Tests cover all
towns, three housing tiers, redraw suppression, absent houses and several
sealed-lair masks. A destroyed house credits 4/6/8 points by tier in US/PAL,
4 in JP, round-robin to unsealed lairs or to town growth if all are sealed.
An absent target gives no house credit. These are the existing regional
house-feedback rules reached through monster attacks, not a second reward.

The Bat's state 5 checks that a structure still exists when its 100-count
timer reaches 68 (the 32nd eligible call). That check sets carrying flag 4;
an absent target returns to state 1. It checks presence rather than
revalidating the structure class, as synthetic class 0/2/3 fixtures verify.
State 7 retires the Bat when X or Y leaves 0–511. Only a carrying Bat then
calls the house-destruction helper. Thus the carrying check does not itself
remove the house. Map-edge checks at 511, 512 and wrapped negative coordinates
agree across all five versions.

Red Demon state 9, US/PAL `$01:C393–C3CF` / JP `$01:C31D–C359`, finds a
structure at its current cell. Class 2 receives flag `$40`, a field redraw
and shape 4 through US/PAL `$03:B25B–B273` / JP `$03:B02C–B044`.
It then selects state 5. Other classes or absent coordinates are untouched;
no house-feedback credit occurs. First/last pool slots, all towns and four
subtype masks match. Subsequent field repair is covered below; the crop-offering
story lifecycle is a separate system.

### Burned-field recovery and census refresh

A chained five-ROM batch follows healthy census → Red Demon state 9 → burned
census → Rain → recovered census → repeat Rain → census. Its 480 transactions
contain 3,360 original-CPU routine calls, covering all six towns, both crop
types, first/last structure slots, redraw suppression on/off, and Rain aimed
inside/outside the affected square. All three JSON evidence files reproduce
exactly in an independent repeat. No live game or player save is modified.

| Operation | US / European releases | Japan |
| --- | --- | --- |
| Apply miracle to structure records | `$03:B274–B324` | `$03:B045–B0F5` |
| Field action 3: repair | `$03:A124–A13D` | `$03:9ED5–9EEE` |
| Census | `$03:C07E–C146` | `$03:BD27–BDEF` |

Rain uses miracle kind 2 and structure action 3. The structure operation
aligns the aim coordinates down to a 4×4-cell square, queues the action on
affected active records and runs two structure passes. The field handler
clears `$40`, preserves the crop subtype, schedules recovery artwork and
returns the record to action 1. An already healthy field does not gain another
support increment. An outside field remains burned.

Neither burning nor repair immediately rewrites cached support. The next
census excludes a burned field or counts a repaired one at the existing
regional coefficient: ordinary/upgraded 32/48 in US/EU/DE/FR, 16/24 in JP.
The fixtures retain population 2 and leave all lair stocks and growth counters
unchanged. Repeated Rain still advances ordinary visual-program state; it is
the support/reward result, not every RAM byte, that is idempotent.

These are single-field, Act-1-completed towns with sealed lairs, not developed
towns or tests of the miracle's SP, input, dialogue and audiovisual wrapper.
Census calls are explicit; the batch does not measure the main loop's refresh
latency. A future policy switch should retain the field's health/subtype and
pending visual action, then recalculate derived support at a safe boundary.
It should not grant a separate regional recovery reward.

### Miracle-triggered story events

The dispatcher at US/PAL `$03:F921–F999` / JP `$03:F3FD–F475` matches miracle
kind, town and location against 15 six-byte records, reaching 11 distinct
handlers. It is not a general dialogue bytecode VM. The table starts at
US/PAL `$03:F99A` / JP `$03:F476`; each record is
`{kind, town, square_x, square_y, handler_minus_one:u16}`, followed by a
single `$FF` terminator. The coordinates below are 4×4-cell square indices,
not pixels or individual map cells.

| Town | Miracle | Square coordinates | US / PAL handler | JP handler |
| --- | --- | --- | --- | --- |
| Fillmore | Rain | (3,2) | `$F9F5` | `$F4D1` |
| Fillmore | Lightning | (3,2) | `$FA2A` | `$F506` |
| Fillmore | Lightning | (5,5) | `$FA5F` | `$F53B` |
| Bloodpool | Rain | (1,2), (2,2), (3,2), (1,3), (2,4) | `$FB5F` | `$F63B` |
| Kasandora | Rain | (5,1) | `$FAB8` | `$F594` |
| Kasandora | Earthquake | Any | `$FBD7` | `$F6B3` |
| Aitos | Wind | Any | `$FB3C` | `$F618` |
| Aitos | Rain | (4,5) | `$FB8F` | `$F66B` |
| Marahna | Earthquake | Any | `$FAF8` | `$F5D4` |
| Marahna | Lightning | (6,1) | `$FBD1` | `$F6AD` |
| Northwall | Lightning | (2,5) | `$FC1B` | `$F6F7` |

All five releases have these same entries and handler control flow after
accounting for code/RAM relocation and translated-message pointers. The
Marahna earthquake's five-pair terrain list also matches. Kind 3 (Sunlight)
has no entry in this table. Earthquake and Wind bypass coordinate matching;
the other kinds compare `aim_x >> 2` and `aim_y >> 2`. The dispatcher continues
scanning after a handler returns rather than stopping at its first match.

The compared handler bodies retain the same prerequisite, fired and global
flag tests. The three town-specific bitmap pointer tables resolve to the
same `$7F:9107/$911F/$9137` arrays in every ROM, four bytes per town. This
does not make every referenced callback regionally identical: modal dialogue,
terrain edits and other nested effects still require their own contracts.

The original-CPU batch covers 3,145 calls and 105 bounded Go windows; all five
JSON evidence files match an independent repeat:

- Individually blocked states for every table row, including inside/outside
  square coordinates. A set fired flag is not treated as a universal blocker.
- Marahna's Lightning handler sets global bit 12 at cells X24–27/Y4–7,
  preserves an already-set bit and rejects the tested neighboring cells,
  wrong towns and wrong kinds. This flag-only handler does **not** set `$90F7`.
- Aitos's Wind handler checks prerequisite bit 6. When set, it sets fired
  bit 6, clears that town's `$7CEF+2N` counter, retires six world records
  `$0CF8..$0DB6` by writing flags `$8000`, and sets `$90F7=1`. Existing fired
  state does not suppress this path. With prerequisite clear, the tested
  counter, flags and records remain unchanged. Counter values 0/1/65535 and
  widely separated aim coordinates cover the successful and blocked branches.

`$90F7` is therefore broader than a structure-redraw flag. User-miracle entry
clears it, structure work and some story handlers set it, and the wrapper
tests it after cleanup to select its return carry. A future HLE must preserve
each handler's actual writes rather than report every flag change as success.
That isolated batch covers dispatch, gates and the two named successful
paths. The booted follow-up below covers the other nine successful handlers;
neither establishes natural event order or complete town arcs.

#### Successful miracle-story transactions

Sixty-five booted fixtures exercise the remaining nine handlers across all
five ROMs, including each of Bloodpool's five Rain locations. Each starts
from an accepted same-ROM scratch town, loads the destination through the
native scene loader, selects the miracle through the original command menu
and confirms it normally. Eligibility bits, SP/HP, lair state and angel
position are controlled and recorded. No ROM instructions or callbacks are
replaced. Unrelated story events are marked complete to isolate the path.

The outcomes agree across all five releases:

| Handler / successful path | Observed outcome |
| --- | --- |
| Fillmore Rain `$F9F5` and Lightning `$FA2A`, square `(3,2)` | Set fired bit 3, show the response and return without a temple visit or offering. |
| Fillmore Lightning `$FA5F`, square `(5,5)` | Set prerequisite/dispatched bit 12, supply offering 1 (Magical Fire), visit the temple and return. |
| Bloodpool Rain `$FB5F`, all five authored squares | Set global bit 11, supply offering 5 (Source of Life), visit the temple and return. |
| Kasandora Rain `$FAB8`, square `(5,1)` | Set prerequisite bit 8, modify terrain, visit the temple and return without adding an offering. |
| Kasandora Earthquake `$FBD7` | With prerequisite 8 set and fired 0 clear, set global bit 13, supply offering 5 and return from the temple. Fired bit 0 remains clear. |
| Aitos Rain `$FB8F`, square `(4,5)` | Set fired bit 2 and return from the temple without adding an offering. |
| Marahna Earthquake `$FAF8` | Set global bit 9, process the five-pair terrain list and return without a temple visit or offering. |
| Northwall Lightning `$FC1B`, square `(2,5)` | Set global bit 14, supply offering 5 and return from the temple. |

All these paths set `$90F7=1`. Offerings above enter the town inventory,
not the player's held slots. Bloodpool's five Rain locations share global
bit 11: they are alternate places to trigger one discovery, not five
independent rewards. Each location test begins with that bit cleared.
Other bits already set in the eligibility
fixture are not counted as newly earned flags. The complete settled town
bitmaps, global flags, inventories, road words and semantic terrain hashes
agree across regions for each path. They remain unchanged during a further
180 input-free frames. Stock, seal flags and growth also retain their
fixture values; this is not a new earthquake-destruction matrix.

From these native starting layouts, Fillmore's treasure changes road-square
index 45 and three terrain bytes; Kasandora Rain changes road-square index
13 and twelve terrain bytes; Marahna's terrain-list processing changes road
indices 17/18 and 24 terrain bytes. These are observed deltas, not the size
of the authored lists or a claim that other starting maps change the same
number of cells. Marahna's list consumer leaves aim fields `$90E1/$90E5`
at `(8,12)`, the final list pair, rather than preserving the casting origin.

The tests verify the actual miracle kind, a single regional SP charge,
positive bounded angel health, return to the original town and unchanged
SRAM. Rain costs 20 Western / 16 JP, Lightning 10 / 12 and Earthquake
160 / 60 in these runs. Completion frames include translated text and
automated acknowledgements; they are not regional speed measurements.

All 522 files (132 JSON, plus states, WRAM and images) match an independent
repeat exactly. This closes successful execution of the nine remaining
handler paths under the stated fixtures. It does not establish unrestricted
event ordering, every inventory-capacity edge or a complete developed town.

### Population and road story prerequisites

US/PAL `$03:E122–E15C` and JP `$03:DC27–DC61` read the active town's
population at US `$7E:021C+2N`, JP `$021B+2N`, PAL `$021E+2N`. The six
list pointers are US/PAL `$03:F531`, JP `$03:F00D`. Records contain
`{threshold:u16, event_id:u8}` and end with a single `$FF` byte. The native
comparison enables an event only when **population > threshold**, not at
equality. Event IDs below are decimal, local to each town.

| Town | US / PAL records, written as event:threshold | JP differences |
| --- | --- | --- |
| Fillmore | 3:60, 4:8, 5:110, 8:30, 13:200, 24:1 | Event 5 uses 88 |
| Bloodpool | 8:300, 9:450, 5:35, 10:3, 6:90, 24:1 | None |
| Kasandora | 2:5, 4:60, 12:300, 9:700, 10:80, 24:1 | Event 9 uses 400 |
| Aitos | 7:30, 6:100, 8:150, 24:1 | None |
| Marahna | 2:120, 11:1, 7:20, 3:60, 24:1 | None |
| Northwall | 4:20, 5:45, 24:1 | None |

Go-extracted source messages identify Fillmore event 5 as the southeastern
rock/magic hint and Kasandora event 9 as the Ancient Tablet discovery. The
remaining 28 rows agree. These are prerequisites, not census limits, level
requirements, or proof of the first naturally attainable population at which
each scene appears. Normal selection and event callbacks have further state.

The regional runtime replaces only the threshold load at `$03:E13E` for
US record pointers `$F543` (Fillmore5) and `$F56C` (Kasandora9). It verifies
the town and original record identity before loading the selected value.
Native `$E142` keeps the strict comparison, `$F479` sets the prerequisite,
and the original list walk/selector handles all other records and fired flags.

The following road-location producer, US/PAL `$03:E15D–E19B` / JP
`$03:DC62–DCA0`, reads six lists at `$03:F59D` / `$03:F079`. Their
`{square_x, square_y, event_id}` records also end with `$FF`. All five ROMs
have the same five entries:

| Town | 4×4-cell square | Event |
| --- | --- | ---: |
| Fillmore | (5,4) | 7 |
| Bloodpool | (7,4) | 4 |
| Kasandora | (5,7) | 7 |
| Aitos | (6,2) | 4 |
| Marahna | (2,3) | 8 |
| Northwall | No entry | — |

The predicate at US/PAL `$03:9777–97A2` / JP `$03:9562–958D` requires
bit `$0800` set and both bits in mask `$0240` clear in the town's road-square
word. The index helper at `$03:97B0–97ED` / `$03:959B–95D8` rejects square
coordinates outside 0–7, then resolves `$7F:6800 + town*128 + y*16 + x*2`.
This is a local road-state check, not a fresh traversal of an inter-town road.

Both producers OR bits into the prerequisite array; they do not clear flags
when population or road state later fails the test. Re-evaluating prerequisites
under a new regional policy must therefore preserve reached flags and fired
state rather than rebuild progression or replay rewards.

The five-ROM batch contains 920 population-boundary fixtures (each authored
threshold minus one, equal and plus one, plus extremes and pre-existing flags)
and 540 road fixtures. Each invokes its producer twice, verifies idempotence
and preservation of the other town/prerequisite/fired/dispatched bytes. Road
words are controlled inputs, not evidence that a town can naturally build
that route. The callback tests below bring the batch to **1,520 isolated
fixtures and 35 bounded Go windows**. Six evidence files reproduce exactly;
no booted scene or full campaign is added to the totals.

### Bloodpool crop and Teddy event joins

The Bloodpool callback table is US/PAL `$03:E93C` / JP `$03:E430`, selected
through the six-town root table `$03:E66E` / `$03:E16D`. These source joins
separate independent prerequisites from a walkthrough's recommended order:

| Edge | Source contract, shared by all five ROMs |
| --- | --- |
| Population >3 → event 10 | Enables the early worried-about-Teddy dialogue. |
| Population >35 → event 5 | Crop callback `$03:EA72–EA96` / `$03:E566–E58A` sets `$7F:9193=1`, supplies offering 8, marks fired 5, requests dialogue 5 and selects ambient scene 7. Later replenishment tests prerequisite 5, not a new crop cooldown. |
| Road square (7,4) → event 4 | Enables the connection-to-Fillmore/crop-teaching message, independently of the crop population producer. |
| Population >90 → event 6 | Teddy callback `$03:EA97–EB06` / `$03:E58B–E5FA` sets Bloodpool's development hold `$7F:7CF1=1`. Global bits 10 and 25 guard the Bread offering and Teddy actor spawn separately; fired 6 remains clear. |
| Successful Bread use → return marker | US/PAL `$01:9CF8–9D6E` / JP `$01:9CD4–9D4A` checks Bloodpool and the rounded pixel rectangle X `$0090–00BF`, Y `$0120–013F` (map cells X9–11/Y18–19). After the success dialogue, it removes held item 7, then sets `$7F:918D=1`. |
| Return marker → Skull and return dialogue | The event-6 callback supplies offering 14 (Magic Skull), marks fired 6, clears dispatched 6, requests event 7 (`$920E=$87`), clears the development hold and selects ambient scene 3. Event 7's own handler is only RTS. |

The ten crop fixtures cover empty and seven-occupied offering inventories.
Forty Teddy fixtures cover both return-marker states and all combinations of
the two grant/spawn guards. Ten Bread fixtures execute only the post-dialogue
commit fragment, US/PAL `$01:9D44–9D4E` / JP `$01:9D20–9D2A`, stopping
before its COP wait. They verify held-slot 0/7 removal, preservation of other
slots and the return marker; disassembly establishes the write order. The
interactive targeting/dialogue wrapper is compared in Go disassembly, not
claimed as a newly booted transaction.

The authored lair seeds locate Bloodpool's first slot at cell `(4,24)` in
every ROM. It is the southwestern lair and the already-verified Magic Skull
target: global **byte index 8**, not global lair number 8. The next slot is
`(0,4)`, the separate bridge-request seal condition. Existing
[Skull tests](#guidance-sealing-and-delayed-soul-rewards) and
[maintenance tests](#periodic-town-event-maintenance) join the first slot's
seal to lake clearing without inventing a guide-derived coordinate.

Source-message identities and raw ROM spans were checked against the existing
Go extraction for all five releases. These tests close the named producer and
callback joins, not a continuous Bread pickup/use/temple/lake playthrough or
all inventory-capacity edges. The following batch closes the remaining
bridge, crop-sharing and late-story source joins.

#### Bridges and cross-town crop sharing

Fillmore's Bridge discovery at US/PAL `$03:F621–F670` / JP `$03:F0FD–F14C`
requires prerequisite 2 clear and the **last three** Fillmore lair slots
sealed. The first slot is irrelevant; “any three lairs” is not equivalent.
The tested flag words are US/PAL `$7F:95CA/$95CC/$95CE`, JP
`$95BE/$95C0/$95C2`. The path then sets prerequisite/dispatched 2,
technology `$919E=1` and grants item 10 before entering the already-tested
[temple transaction](#successful-maintenance-transactions). The new gate
matrix covers all 16 seal combinations with prerequisite 2 clear/set.
Successful fixtures stop before the first modal call; rejection fixtures
execute the original RTS return.

Bloodpool event 4, `$03:EA2E–EA71` / JP `$03:E522–E565`, reads crop
knowledge `$7F:9193`. With knowledge present it sets Fillmore's `$9192=1`,
marks fired 4, requests dialogue 4 and invokes the whole-town field upgrader
with target town 0. Without knowledge it marks fired **and dispatched** 4
and takes the dispatcher's rejection return. A later knowledge change does
not revive that fired event; native selector re-entry verifies this.

The upgrader, `$03:E342–E39D` / JP `$03:DE47–DEA2`, temporarily selects
the target town's 128 structure records, ORs `$10` into every active class-2
field, runs the original appearance/redraw helpers, then restores the town
index. Inactive fields and other classes are untouched. Burned-field bit
`$40` remains set. The fixtures check all 512 record bytes, including the
updated action/appearance bits, and preservation of the active Bloodpool
town indices. This is not an inventory transfer: no crop offering is consumed
by automatic teaching.

Manual item-8 use at `$01:9D6F–9E02` / JP `$01:9D4B–9DDE` snaps the
aim to a 32-pixel boundary and resolves a field through `$03:BDBC` / JP
`$03:BB45`. An ordinary field reaches `$03:BE77–BEB6` / JP
`$03:BBAC–BBEB`, which converts the field at its containing 4×4-cell plot
origin while preserving `$40`. The wrapper sets `$9192+town=1`, then
consumes item 8. An already-upgraded field takes a response/consumption path
without another conversion or knowledge write. Invalid targets bypass
consumption. Source comparison covers the modal targeting/response wrapper;
native commit fixtures cover all six towns, ordinary/upgraded and healthy/
burned fields, and held slots 0/7. They do not replace a full interactive
item-use/cancellation test.

#### Bloodpool disputes, Music and Compass

The population table enables event 8 above 300 and event 9 above 450.
The original selector `$03:E19C–E1F1` / JP `$03:DCA1–DCF6` scans eligible,
unfired events in ascending order and uses the real stacked-RTS dispatcher.
Event 8 therefore precedes event 9 when both are eligible.

| Step | US / PAL | JP | Established behavior |
| --- | --- | --- | --- |
| Dispute callback | `$03:EB08–EB5D` | `$03:E5FC–E649` | Requires completed-act word `$7F:6B1A=2`. Otherwise clears prerequisite 8; Western versions also clear prerequisite 9, Japan does not. |
| Music unresolved | Within event 8 | Within event 8 | Sets development hold `$7F:7CF1=1`, selects ambient scene 1 and leaves fired 8 clear. |
| Music delivered | Within event 8 | Within event 8 | Nonzero `$7F:91A5` marks fired 8, clears the hold and selects ambient scene 3. |
| Music source | `$03:ECF3–ED26` | `$03:E7DF–E812` | Kasandora event 4, enabled above population 60, grants item 11, sets fired 4, requests dialogue 4 and writes its music-state byte `$91A6=1` before audio/scene work. |
| Music use | `$01:9E82–9EB6` | `$01:9E5E–9E8C` | After the music routine, accepts Bloodpool, consumes held item 11, shows the response, then writes `$91A5=1`. Other towns take the non-consumption response. No requirement to wait for disputes first. |
| Compass callback | `$03:EB5E–EB94` | `$03:E64A–E680` | Grants item 19, marks fired 9 and requests dialogue 9. No direct act-count or Music-state gate. |

The Compass prologue loads `A=1` and `Y=bitmap_pointer` immediately before
BEQ, but makes no bitmap-test call. LDY supplies the branch's zero flag;
the nonzero pointer makes that rejection branch untaken in all five ROMs.
Direct callback fixtures confirm that prerequisite 1, act count and Music
state do not block its grant. This is a shared source property, not an
identified regional bug or proof of naturally bypassing the earlier events.

The regular pipeline `$03:E092–E0CD` / JP `$03:DB97–DBD2` re-runs the
population and road producers **before** selecting a callback. Consequently,
Japan retaining prerequisite 9 after a rejected event 8 does not by itself
allow a normal next-pass Compass grant: population re-enables event 8, which
still wins priority. Repeated pipeline fixtures confirm this, and confirm
that delivered Music permits event 8 completion followed by the Compass
when Act 2 and population conditions are satisfied. Already-fired event-8
fixtures distinguish this selector dependency from a nonexistent direct gate
inside the Compass callback. They are controlled states, not attainable-route
proofs. Live regional policy must preserve actual prerequisite/fired state
instead of reconstructing it from a guide's recommended order.

S22's runtime seam is `$03:EB35`, after native prerequisite-8 clearing.
Japan skips the three instructions that clear prerequisite9 and resumes at
`$EB3D`; US/Europe delegates that original body. The original `REP`, two
`PLX`, `PLA`, `SEC` and `RTS` retain rejection/stack ownership. Policy activation
does not set a prerequisite, alter fired flags or call the Compass reward.

Western Music use also brackets its audio routine with helpers
`$01:93BE–93DB` that write `$4200=$01`, then `$A1` after acknowledging
`$4210`; JP omits those two calls. This is an interrupt-handling difference
in the wrapper, not evidence of different song content or playback speed.

The five-ROM follow-up adds **740 isolated fixtures and 69 Go windows**:
160 bridge gates/pre-dialogue grants, 20 teaching/selector revisits, 240 crop
commit fragments, 240 twice-run late-story pipelines, 60 direct Compass
callbacks, ten pre-audio Music grants and ten post-response Music-marker
writes. Pipeline tests pre-set dispatched flags only to suppress modal
arrival; population refresh, selection, rejection unwinding and callbacks
are original. Audio and interactive Music use remain source-inspected, not
new complete playback transactions. Six evidence files match an independent
repeat; no booted traces or full campaigns are added.

### Periodic town-event maintenance

The six maintenance lists at US/PAL `$03:F5ED–F620` / JP `$03:F0C9–F0FC`
reach 14 handlers. Their ordering, handler-local control flow and embedded
terrain-coordinate lists match across all five ROMs after accounting for
code/RAM relocations and translated-message pointers. The dispatcher is
US/PAL `$03:F5BE–F5EC` / JP `$03:F09A–F0C8`; it preserves the caller's
processor flags while managing the handlers' accumulator widths.

| Town | US / PAL handler entries | JP handler entries |
| --- | --- | --- |
| Fillmore | `$F621`, `$F671`, `$F68A` | `$F0FD`, `$F14D`, `$F166` |
| Bloodpool | `$F6BF`, `$F6FF`, `$F791` | `$F19B`, `$F1DB`, `$F26D` |
| Kasandora | `$F7AE` | `$F28A` |
| Aitos | `$F7D1`, `$F7F8`, `$F822` | `$F2AD`, `$F2D4`, `$F2FE` |
| Marahna | `$F857`, `$F870` | `$F333`, `$F34C` |
| Northwall | `$F8A5`, `$F8CC` | `$F381`, `$F3A8` |

The native tests establish these non-modal gates, including already-set
flags and all 16 combinations of sealed lairs. Event-bit numbers are decimal
indices into the [per-town story arrays](ram-map.md#story-event-bitmaps-7f9107-7f914e),
not lair numbers:

| Handler | Condition and writes |
| --- | --- |
| Fillmore `$F671`, Marahna `$F857` | Nonzero byte `$7F:91D4+town` latches prerequisite bit 10. |
| Fillmore `$F68A` | With global bit 16 clear, fired bit 4 set and any of the last three lairs sealed, sets global bit 16 and retires eight auxiliary records `$0F0C..$1016`. |
| Kasandora `$F7AE` | All four lairs sealed latches prerequisite bit 0. |
| Aitos `$F7D1` | Global bit 8 and **Fillmore population at least 20** set Aitos prerequisite bit 6 and fired bit 5. The population read is unindexed; changing Aitos's own population does not change this gate. |
| Aitos `$F7F8` | Any sealed lair latches prerequisite bit 3. |
| Aitos `$F822` | Exactly two sealed lairs latch prerequisite bit 2—not two or more. |
| Northwall `$F8A5` | The second and fourth lairs sealed latch prerequisite bit 2 and write `$91A3=1`; an already-set prerequisite bypasses that write. |
| Northwall `$F8CC` | Bit `$0200` clear in all four road-square words at `(5,2)`, `(6,2)`, `(5,3)`, `(6,3)` latches prerequisite bit 3. |

These are shared rules, not additional regional options. The cross-town
population dependency is verified in every ROM; its intended meaning is
not established. Latched prerequisites remain set if their original
condition later becomes false.

Whole-list tests also repeat the maintenance pass twice for each town with
dialogue-producing paths blocked. Only Bloodpool replenishes crop item 8,
only when prerequisite bit 5 is ready and its first eight offering slots
are empty. Occupied first/last slots prevent replacement, and the ninth
sentinel byte remains intact. This extends the
[crop ownership result](#crop-offering-replenishment) across the five-ROM
dispatcher; it is not a new replenishment-policy difference.

Evidence: 1,760 native fixtures, 85 bounded Go windows and five JSON outputs
matching an independent repeat. The compared Bloodpool terrain list has
13 coordinate pairs plus `$FFFF`; it and Northwall's four-pair list are
data, excluded from the instruction comparison. This isolated batch does
not establish successful modal transactions or campaign event order.

#### Successful maintenance transactions

Twenty additional booted fixtures exercise the four dialogue-producing
maintenance paths below in all five releases. Native town loading supplies
the geography and resources; controlled eligibility flags and sealed lairs
trigger the original scheduler. Button presses dismiss the original text.
There are no substituted handlers, patched ROMs or imported player saves.

| Path | Shared observed outcome |
| --- | --- |
| Fillmore `$F621` | Sets prerequisite/dispatched bit 2 and `$919E=1`, adds offering item 10, visits the temple and returns to Fillmore. Event 2 sets fired bit 2 and ambient scene 1. |
| Bloodpool `$F6BF`, bridge technology absent | Sets prerequisite/dispatched bit 2, visits the temple and returns with **fired bit 2 still clear** and ambient scene 7. |
| Bloodpool `$F6FF`, lake clearing | Clears `$0200` in the 13 listed road squares, changing each tested word from `$0240` to `$0040`; changes the semantic terrain map, adds offering item 2 and returns from the temple with prerequisite/fired/dispatched bit 3 set. |
| Marahna `$F870`, last three lairs sealed | Displays its warning, sets global bit 15 and resumes the town without a temple visit. Global bit 9 stays clear; the road words and offering inventory are unchanged. |

Bloodpool's bridge callback explains the different flag result. US/PAL
`$03:E9ED–EA13` / JP `$03:E4E1–E507` tests `$7F:919F`: when zero, it skips
the fired/dispatched writes and scene-spawn call, then selects ambient scene
7. The maintenance entry itself requires that byte to be zero. Completion
must therefore be tracked per event; “temple returned” cannot universally
mean “fired flag set.” The fixture also receives crop item 8 through the
separate replenishment handler because prerequisite bit 5 was left set;
that is not a bridge reward.

The other captured callbacks are Fillmore event 2 at US/PAL `$03:E718–E730`
/ JP `$03:E217–E22F`, Bloodpool event 3 at `$03:EA1B–EA2D` / `$03:E50F–E521`,
and terrain-list processing at `$03:FC4C–FC9C` / `$03:F728–F778`.
All 43 JSON reports, including 20 bounded Go windows, match an independent
repeat. Scratch SRAM remains unchanged in every case.

These are successful transactions from synthetic prerequisite states, not
complete town playthroughs. Separate follow-ups cover
[bridge delivery](#bridge-offering-delivery) and
[successful miracle callbacks](#successful-miracle-story-transactions).
Developed-town maxima and unrestricted event ordering remain open.
Completion-frame counts include language-dependent dialogue and the test's
button schedule; they are not regional gameplay-speed measurements.

#### Bridge offering delivery

Fifteen additional booted fixtures cover delivery, rejection when already
known, and cancellation from the item picker in all five ROMs. They resume
the accepted Bloodpool bridge-request state, seed offering 10 in the held
inventory and use the original Use Offering menu. This verifies delivery,
not naturally obtaining and transporting the item from Fillmore or building
every subsequent bridge.

The delivery owner is US/PAL `$01:9E28–9E7A` / JP `$01:9E04–9E56`.
The Bloodpool success branch tests `$7F:919F`, sets it to 1, shows the
acceptance dialogue, consumes item 10 through US/PAL `$01:921B–9238` /
JP `$01:915B–9178`, and sets prerequisite bit 2 (`$910B & $20`). The
consumer uses held base US `$02A2`, JP `$02A1`, PAL `$02A4` and removes
the first matching ID, not a newly granted item.

These are distinct native milestones. In the recorded input sequence,
technology becomes available on frame 397, consumption occurs on 585, and
fired bit 2 becomes set on 594 Western / 598 JP. The elapsed counts include
the test's acknowledgement pauses; the verified contract is their order.
The later event callback `$03:E9ED` / JP `$03:E4E1` now takes its nonzero
technology branch, marks fired/dispatched bit 2, schedules its scene and
retains ambient scene 7. The original bridge request alone had left fired
bit 2 clear.

Already-known rejection preserves the offered bridge. Cancelling before
using it preserves both the item and the absent technology; it leaves fired
bit 2 clear. These cases do not test interruption after acceptance has begun.
After closing the menus and running a further 1,800 frames, all three cases
retain their expected inventory, technology and event flags with unchanged
SRAM. The successful inventory contains no duplicate reward.

Fifteen bounded Go windows preserve the delivery, consumption and callback
owners. Their instruction differences are relocated calls/table addresses,
translated text pointers and the regional held-inventory base. All 78 files
(33 JSON, plus states, WRAM and images) match an independent repeat exactly.

A future regional-policy switch must preserve this in-flight transaction:
the technology flag can already be set while the item is still held. Do not
restart delivery or infer complete settlement from that flag alone. No
runtime policy switch is implemented or tested by these native fixtures.

### Aitos dying-man scene flag mismatch

Aitos callback 2 is US/PAL `$03:EEE1–EF2E`, JP `$03:E9C5–EA12`
(end-exclusive). The normal dispatcher supplies Y = the fired-bitmap pointer
table (`$DCAE`, JP `$D7B3`). Its opening sequence tests global flag 26
through `$F4DF` / JP `$EFBB`, but on the clear branch calls the **town**
setter `$F479` / JP `$EF55`, not the global setter `$F4EA` / JP `$EFC6`.
The test preserves Y, and the callback does not replace it before that call.

| Operation | Actual state affected in all five ROMs |
| --- | --- |
| Initialization test | Global bit 26: `$7F:9102 & $20` |
| Setter at `$03:EEEC` / JP `$03:E9D0` | Aitos fired bit 26: `$7F:912E & $20` |
| Scene setup | Record `$03:E664` / JP `$03:E163`: kind 0, base 4/5, scene `$31`; retires the eight-slot `$0F0C` pool and creates the scene |
| Timer update | Increment byte `$7F:9171`; compare with `$80` |
| Timeout branch | Set Aitos fired 2 (`$7F:912B & $20`), request `$920E=$8B` (message 11), then enter the summons/dialogue path |

Because the first setter leaves global 26 clear, eligible callback updates
repeat scene creation. This is a persistent **fired** write, not prerequisite
26, and does not activate Aitos's magic-discovery text. The original selector
continues event 2 because fired 2 remains clear until timeout or another
event path finishes it. The callback does not reset its timer during scene
creation.

The normal periodic eligibility helper is `$03:F822–F857` / JP
`$03:F2FE–F333`. It latches prerequisite 2 when exactly two of Aitos's four
lair words have their high bit set; it leaves an existing prerequisite set.
Twenty-five native controls cover initial counts zero through four across
all five releases. This is an identified trigger, unlike the unexplained
offering announcements above, but is not a continuous campaign reproduction.

One hundred original-selector controls exercise timer wrap/noncompletion,
clear/set scene flag, repeated updates and enabled/fired guards. With the
scene flag clear, the output sets fired 26, leaves global 26 clear and
replaces the seeded pool flags with one active scene actor plus seven retired
slots. With the scene flag set, it leaves those pool flags untouched while
still advancing the timer. Disabled or completed event 2 does neither.

Ten additional timeout controls start the counter at 127 and execute the
unchanged callback through its fired-2/pending-11 writes. An explicit RTL
stop ends each test immediately **before** the modal-kernel JSL at
`$03:EF18` / JP `$03:E9FC`; Y is supplied exactly as the dispatcher leaves
it. These are completion fragments, not full dialogue playback. A first
attempt to run that modal boundary in the isolated harness did not return
and is excluded from the evidence counts; it was a harness limitation,
not a game crash.

#### Live scene, animation and rain response

A subsequent twenty-transaction batch runs the full scene in all five
unmodified ROMs. Each region has a native wait/no-rain path, a native Rain
path, and the same two paths with global 26 latched **after** the first
original spawn. That one-byte comparison prevents repeated setup without
replacing the actor, its program or any ROM instructions; it is not a shipped
fix or an assertion of intended behavior.

The fixtures load Aitos through the original town loader, block unrelated
events and supply act/resource state and exactly two sealed lairs. They
begin with prerequisite 2 clear: the native periodic check enables it.
The loader places the angel at `(288,352)`, already inside the correct Rain
square. No actor coordinates or miracle-target RAM are written. The original
summons, temple introduction, command menu, target confirmation, effects and
completion dialogue run normally. These are controlled transactions, not
unassisted two-lair campaign playthroughs.

| Path, in every region | Counter at completion | Pending message | Result |
| --- | ---: | --- | --- |
| Wait without Rain | 128 | `$8B` → 11 | No-rain death response; fired 2 set; actor retired |
| Select Rain after counter 16, confirm through native picker | 16 | `$8C` → 12 | Last-wish response; fired 2 set; actor retired; miracle result 1 |

Latching global 26 changes neither outcome nor frame count within each paired
run. Rain costs 16 SP in Japan and 20 in the West, targets cells `(16,20)`
and leaves the event counter stopped at 16 in these sequences. The pending
latch clears after either dialogue, the scene pool is retired, and SRAM is
unchanged. Near-timeout casting, save/reload and intervening travel are not
covered by this batch.

The visible difference is in the actor's retained program:

| Resource | US / PAL | Japan | Meaning |
| --- | --- | --- | --- |
| Authored actor program | `$0A:DB0C` | `$04:AB93` | `0D 09 78 00 0C 09 78 00 0B F5 FF` |
| Pose compositions | `$01:E85C` / `$01:E892` | `$01:E7E6` / `$01:E81C` | Single-part pictures using tiles `$A0` / `$A1` |
| Class-1 dispatcher | `$01:CD0C` | `$01:CC96` | Original town-person state table |
| Timed-wait state | `$01:CEEB` | `$01:CE75` | Decrement actor `+$22`, then resume commands at zero |

Table-rooted command decoding shows two pose selections, each followed by
a wait of 120 actor updates, then a relative loop back to the start. This
actor-local wait is not the event's `$9171` timeout counter. During the
checked native simulation waiting interval, repeated creation keeps the actor
on the first composition. The latched comparison reaches both compositions.
Selected paired stills differ only within the sprite's six-by-four-pixel
bounding area; per-frame video hashes reveal changes missed by screenshots
taken exactly at event-counter updates. This establishes a small visual
animation suppression, rather than merely a difference in hidden counters.

All twenty transactions and fifty additional Go windows have a byte-identical
independent repeat. The comparison traces are counted once alongside the
native branches, not as twenty ordinary-playthrough confirmations. This
closes the earlier full-timeout and early-Rain playback gaps, while leaving
the stated boundary cases untested.

The shared flag mismatch is consistent with a wrong-helper call. Preserve
native behavior unless an optional fix is separately approved; no repair was
implemented. It is unrelated to the Aitos mountain-event stale-message issue.

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
The later [layout follow-up](#terrain-and-damage-box-contracts) verifies every
changed cell natively and reproduces all eight affected maps in booted loads;
[pickup tables](#authored-pickup-differences) name all authored item changes.

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
  duration 10 → 11; the [final-boss follow-up](#tanzra-forms-timer-and-projectile-strength)
  verifies a 37/38-update upper-body turn.
- Kasandora type `$10` initial state 15 includes a one-byte extent difference.

These are stored values, not universal frame durations: the animation consumer
controls timing, facing and collision interpretation. Normal and rematch
assets must both be tested. The final sequence (state 61) of both Marahna room-4
ordinary-animation blobs lacks an `$FF` terminator before the composition
table; the research reader stops at that table and flags it. Native reachability
and termination for that particular sequence remain unproven.

### Master status and score page

The native Master report is US `$01:899B–8A3E` / JP `$01:8978–89CE`, shared
by the Palace and SIM menu callers. Both compose the report, show inventory
objects, and wait for a released-then-pressed face button. Wait owners are
US `$01:9261` / JP `$01:91F6`; the mask is `$A1 & $C0`.

| Input after the Master report | US | JP |
| --- | --- | --- |
| B (`$80`) | Open scores | Close report |
| Y (`$40`) | Close report | Close report |

US closes the first report's objects before drawing the score page and clears
`$29` while keeping `$27=1`. Either B or Y then closes it. Final cleanup
clears both flags and the report text; the root menu retains its selection.
JP has no second-page branch in this owner. Use these native controls, not
the host settings overlay's different Back mapping.

US `$01:89FD–8A21` sums twelve packed-BCD words at `$02B3–02CA` in decimal
mode, storing low four digits in DP `$00` and carry digits in DP `$02`, then
clears decimal mode before formatting the page. Values are stored in tens of
displayed points. These DP locations are temporary: later formatting clobbers
them, so they are not persistent total-score fields.

Five booted Palace traces cover B/Y in both ROMs and both exits from the US
score page. Distinct seeded act values render in order with the correct
displayed total (stored 100–1200, total 7800; displayed 1000–12000, total 78000).
Eight isolated sum cases additionally check decimal carry, including twelve
9999 values. The same batch has 180 crop-leaf cases, 66 inventory-helper cases
and four booted crop flows. Eight further town-menu traces below bring this batch
to 254 isolated cases, 17 booted fixtures and 32 bounded Go windows. These do
not establish all city-report, campaign, or European behavior.

Expose score-page availability as a presentation policy, independent of
recording/settling scores, language selection and gameplay rewards. Capture
it when the report opens; a mid-report setting change takes effect on the
next opening, without cancelling a page or replaying a transaction. The
Recomp option now wraps US `$01:899B`, delegates its native body, and, for the
captured JP policy, routes `$01:89EE` directly to native cleanup `$01:8A2F`.
The original `$01:9261` release/press wait still runs. Score recording and
settlement are untouched; the surrounding menu's lifetime remains independent.

### Town-menu return behavior and message-speed choices

The shared report does not own the surrounding command menu's lifetime.
US town wrappers close the menu, wait for face-button release and return C=0.
JP wrappers return C=1 without closing it. The corresponding menu loops,
US `$01:81AC–81D6` / JP `$01:818C–81B6`, interpret C=1 as redraw/reselect and
C=0 as return to town processing. Both Palace Master-report callers instead
return C=1, so Palace screenshots alone do not reveal this difference.

| Town command | US wrapper | JP wrapper | Booted path checked |
| --- | --- | --- | --- |
| Master status | `$01:8530` | `$01:84FA` | Open report, close with Y |
| City status | `$01:853B` | `$01:84FF` | Open report, close with Y |
| Progress Log | `$01:854A` | `$01:8504` | Decline save with Y, acknowledge response |
| Message Speed | `$01:8559` | `$01:8509` | Browse choices, cancel with Y, acknowledge response |

Eight controlled Bloodpool traces verify the original input/navigation and
the resulting menu presentation. After closing either report, the US long
development clock resumes; JP's stays frozen with the menu visible and its
selection retained. Cancellation of the two other commands gives the same
result. SRAM is unchanged throughout. The cancellation tests do not cover
accepted saving, quitting, accepted speed changes or every command's exit.

Message Speed is US `$01:8AF5–8B7C` / JP `$01:8A8C–8B13`. Rightward selection
stops at 9 US (`CMP #$09` at `$8B5B`) and 7 JP (`CMP #$07` at `$8AF2`); both
start their offered range at 0. Twelve separate Right presses in each native
menu reach and hold these endpoints. Cancelling leaves the stored `$0200`
unchanged. The number row and selector are positioned differently too:
native packed destination adds `$0B11` in US and `$0B12` in JP, through each
region's own fixed-text composer. Neither the cap nor its layout proves
equal pacing for corresponding values in the different text engines.

Keep **town-menu return behavior**, **score-page visibility** and **offered
speed range** separate in a regional presentation model. Snapshot return
behavior when dispatching a command, and range/page choices when opening
their respective screens. Preserve native cleanup, cache/save work, response
dialogues and release barriers. Reopening a menu must not perform the action
again. When integrating a narrower speed range, explicitly handle an existing
value 8/9 without changing stored pacing merely because settings were toggled;
the user should be able to cancel with their previous value intact. Native
and modern menu owners need the same effective policy, not renderer hooks.

The Recomp return option now captures the policy at the accepted `$01:81D7`
command boundary for actions12–15. A small game-owned adapter retains the US
report, city-census/cache preparation, save and message-speed callees with
their exact JSR/JSL return addresses. US/Europe also retains `$8CB6` close and
`$9270` release; JP omits only those two calls and returns carry set. This is
the JP menu-return rule on US report/save behavior, not a replacement of the
whole command with JP code. The native and modern menu owners consume the
same carry result. Escaped native returns terminate the command without
performing its remaining calls.

The message-speed adapter captures the offered range at `$01:8AF5`, separately
from command-menu return behavior. The original prompt, polling calls, movement
sound, sample/cancel text and cleanup remain native. Bounded prefixes at
`$8B18` and `$8B59` change cursor geometry and the upper limit. If an existing
speed is 8/9, only temporary `$0A` is clamped for the JP selector; `$0200` changes
only along the original confirmation branch. A captured range cannot change
under an open selector.

US scale `$01:FA9A` begins at `$0C12`; JP `$01:F9A5` begins at `$0C13`.
The JP eight-cell row is centered inside the US ten-cell claim. Native
composition first completes synchronously; the adapter changes only the ten
digit bytes at `$7F:B324 + 2*i`, preserving attributes and the normal upload
flag. Enhanced composition projects the authored numeric row to the same
captured grid, retaining translated labels, artwork offsets, styles, hard
boundaries and bidi spans. A JP eight-field source can also be used with US
choices; absent 8/9 fields get numeric fallbacks. Neither renderer nor language
locale decides the gameplay range.

### Death Heim transition and music

Score settlement has different callers, not just a different conversion
formula. JP `$00:A713` calls `$03:CD3E` from the stage-clear card owner
`$00:A6BC`, after writing the completed-act count. US's card owner `$00:A6FD`
does not call its converter; the action-departure owner calls `$03:D095` at
`$00:A30D`. A mixed-region completion transaction needs one settlement point
and a captured completed-act count; do not install both native calls and
double-credit growth/stock. Booted Northwall Act-2 defeat/departure fixtures
now verify this ordering: JP settles with the clear card, US later during
departure, once each. With the controlled starting score `$1234`, JP's growth
increase is 200 at the card (stored score `$1314`); US's is 358 after its later
score tally (stored score `$1799`). Neither changes lair stock in these
fixtures. This is not an equal-final-score formula
comparison; the [score settlement contract](#lair-stock-is-not-monotonic)
describes the independent conversion rules.

Recomp inserts the selected early settlement at US `$00:A754`, the equivalent
of JP `$00:A713`, after the native completion count and clear-card text have
been published. It then calls the original `$00:85B7` display-object finalizer
and returns through `$00:A757`. Departure at `$00:A30D` either retains its
original `$03:D095` call or skips it when this clear already settled, then
rejoins `$00:A311`. Policies are pinned from clear through departure; stock
projection changes wait until that transaction closes. Retry/new-room and
accepted title entry discard an interrupted completion marker. Debug snapshot
restoration remains rejected by the existing execution-lifecycle guard.

The departure tail is shared by several coroutine resumes. CFG ends at A30D
are required for A1CE, A1FC, A20D, A22F, A264, A285, A2B5 and A2C6;
splitting only A2B5 leaves other generated bodies bypassing the hook. The
clear-card body similarly ends at A754. The adapter uses public native-call
and paired-tail contracts, preserving native return ownership rather than
editing a return word or manually popping the clear/departure frame.

US `$00:A343–A381`, reached after the action completion animation, checks all
six completed-act counts. When all are two it sets `$7F:9101` bit 0, stages
scene `$0009`, and selects music ID 4 at `$0334`. JP's corresponding
`$00:A335–A340` instead returns to the current town. JP `$01:85C8–85FA` later
checks the six counts and sets `$9101` bits 0/1 before the announcement;
US `$01:861E–8645` announces only when bit 0 is already set and bit 1 clear.
Twenty-four earlier transition fragments verify the first branch, including
each possible current town. Six additional booted fixtures cover both final-
act departures, both incomplete-town controls, and native input chains through
the announcement, world map and Palace revisit. The setup dispatches an
already-loaded Ice Dragon into its original death handler with controlled
completion flags; it is not a natural boss kill or complete campaign. Native
defeat, score tally, departure and subsequent menus execute without ROM patches.
Both incomplete-town controls return to Northwall without unlocking Death
Heim. Both completed routes retain the island on a later map visit and do not
repeat the announcement. Fourteen isolated guard cases additionally verify
the already-announced return and every incomplete-town position.

The host's I06 adapters now join those owners at `$00:A343` and `$01:861E`.
Alternate `$A315/$A32A/$A337` generated entries terminate at the departure
boundary. The US route delegates both bodies; Japan reproduces the town-return
leaf and Palace guard, then uses the US message/cleanup suffix. Departure
retains word-wide completion comparisons, while the JP Palace guard retains
byte-wide comparisons and native X/Y/flag clobbers. There are no score,
reward, background-asset or music-upload overrides in this controller.

A campaign route lock is captured at the eligible final departure, before a
possible town save. An already-unlocked legacy event retains its effective
route. Later settings requests cannot change that transaction or clear native
story bits. The lock is separate from those native bits and is included in
the regional companion/replay identity. It is not a persistent command to
replay the transition.

Controller coverage includes 336 source/flag/incomplete-town/current-town
cases, plus all native escape tokens and delayed-setting cases. Another 336
optional original-JP-ROM differential cases compare both translated fragments;
the announcement body deliberately remains delegated. A production-runtime
scratch-save control with six completed acts and no unlock bits reaches the
Palace through native Continue/navigation: JP announces once and sets bits
0/1; the matching US control leaves them clear. This controlled eligibility
fixture does not claim a natural final-boss kill. Full reveal choreography
retains the native body documented below.

The US-only presentation is mapped separately from those persistent flags:

| Owner | Native behavior |
| --- | --- |
| `$02:8134`, emergence branch `$81BE` | Nonzero `$031A` selects focus `(800,128)` and camera `(672,16)`, initializes eight words `$031C–032B` to `$00FF`. |
| `$02:8550–85CF` | Wait 90 VBlanks; post sound command `$99`; shake for 30/30/60 updates with masks 1/3/7; reveal for 16/16/28/4 steps with masks 7/3/1/0; wait 120; write brightness 15 down to 0 at four-VBlank intervals. Clear `$031A`, stage Palace scene 7. |
| `$02:8601–863D` | Two native RNG draws per shake update. Add masked values to camera X/Y and subtract them from fixed record `$06D6` X/Y so the screen mask follows the same displacement. |
| `$02:863E–865B`, table `$02:902F–912E` | Clear one of 64 unique bits in the low bytes of the eight mask words. One step per two VBlanks; the table gives a fixed scattered order, not random pixel selection. |
| `$01:EDF8–EF38` | Fixed record's 64-part composition: an 8×8 grid of repeated OBJ tile `$7F`, covering 64×64 screen pixels. |
| `$02:AFCB–AFF7` | While `$031A` is nonzero, upload the 16 mask bytes to VRAM word `$47F0`, the tile's first two bitplanes. This changes a sprite stencil, not Mode-7 terrain or island height. |

All 64 mask steps execute natively in isolated tests with adjacent-byte
sentinels. The booted sequence also shows all 64 sprites, each bit clearing
once at two-frame spacing, and the completed reveal before the fade. The
underlying island is already in the world map: US `$02:865C` / JP `$02:850C`
preserves its 8×8 map block when `$7F:9101` bit 0 is set. JP has no equivalent
US reveal-state block; its song selector already occupies `$0322`. Do not
copy `$031A–032B` between regions as if it were a portable scene structure.

At `$02:B63B`, the command reads a song number, selector and source pointer.
It executes **only when `$0334` matches its selector**; otherwise it returns.
The separate loaded-source pointer `$AB/$AD` suppresses duplicate uploads.
Thus `$0334` is the selected/requested music identity, not proof that a given
source is already loaded. Although departure requests 4, the world-map asset
script has selectors 1/0/3, **not 4**. The observed US sequence retains the
stage-clear source `$1C:A5FB` throughout emergence, including after the selector
returns to zero, then loads Palace source `$1C:A988`. JP goes from the equivalent
stage-clear source `$11:FDFA` to town source `$1B:92C7`, then Palace `$1C:8BF5`.
The paired stage-clear and Palace upload payloads match across regions.
The extra US **Palace** selector-4 declaration points instead to file
`0x32B8F`; it is not selected on this route. Earlier notes connected that
declaration to emergence too strongly. Per-frame source/selector snapshots
and stereo PCM captures establish this sequence; the source cache is not an
audio playback-position API and can contain transient scratch during a stop.

Integration: capture a completion-flow policy for the whole departure/
announcement transaction. Settle the score once, preserve both story bits,
and apply setting changes only to a future eligible event. Never clear those
bits to replay the reveal. Presentation, map visibility, music residency and
score ownership need separate contracts even if initially exposed as one
regional-flow choice. Mixed-policy execution, alternate final-town routes,
the unused-on-this-route Palace music declaration, and host 2D/3D presentation
remain separate validation work; this closes the original-ROM Northwall-final
sequence, not every combination.

### Minotaur timing and room inheritance

The original Minotaur retains source US `$AF5D` / JP `$AFF1`, raw room
`$0401`; the Death Heim rematch retains `$F6CA/$F749`, raw room `$0207`.
Both enter the same regional family at US `$00:AF69` / JP `$00:AFFD`.
The rematch wrappers `$F6D6/$F755` first pass through the Death Heim entry
stager and preserve root backlink `$001C`, not an action-object parent.

Eight additional 700-frame booted traces cover both rooms, both regions and
normal/Special. They confirm the complete repeating attack loop:

| Phase / animation state | Original US | Original JP | Rematch, both |
| --- | ---: | ---: | ---: |
| Jump preparation and ascent / 4 | 54 | 54 | 44 |
| Descent / 5 | 29 | 29 | 29 |
| Landing hold / 6 | 8 | 8 | 8 |
| Throw wind-up / 1 | 29 | 29 | 9 |
| Throw follow-through / 2 | 12 | 11 | 11 |
| Idle / 0 | 48 | 16 | 16 |
| Complete repeated cycle | **180** | **147** | **117** |

Values are active frames. State 0 at fixture entry is a proximity wait, not
a completed idle phase; the measured cycle starts after activation. The
first descent/landing pair also precedes the repeating cycle. The one-frame
original throw difference comes from state 2's final row: stored duration
3 US versus 2 JP, giving four versus three active frames. This explains the
earlier first state-2→0 transition at frame 170 US versus 169 JP; it is not an
unexplained scheduler difference. The original region pair differs only in
states 0 and 2 within this nine-state asset; the rematch blobs are identical.

**Targeting and axe lifecycle.** Activation checks strict horizontal distance
below 128. Before ascent, `$AFA4/$B038` samples the camera-subject player's X
through `$8A` into root `+$38`. After ascent, `$AFB2/$B046` moves the boss to
that stored X even if the player has since moved. Facing is chosen before the
throw; no RNG call occurs in the examined boss/axe program.

Axe allocation at `$AFD2/$B066` copies the parent's source, facing and attack,
clears child HP/score, and links `+$3A` to the root. The child starts at
facing-relative X−72/+72 US or X−48/+48 JP, and Y+24. **The regional offset
persists in the rematch.** These are spawn positions, not speed values.
Normal/Special roots have HP24 and attack1/2; axes inherit attack1/2 but HP0.
If the pool is full, the caller writes only the allocator's scratch result,
creates no live axe, and continues state 2 without retrying the throw.

Child entries `$AFFA/$B08E` replace copied flags with `$0800` and play state 3.
That state has sixteen one-frame rows. Original axes alternate horizontal
movement of 3 and 1 pixels toward the facing direction, averaging 2 per
update; rematch axes alternate 4 and 2, averaging 3. Both regional pairs
share their respective movement program. The tiny four-pixel composition
is an **alternating frame**, not only a terminal frame. At the end of each
16-frame sequence, `$B009/$B09D` checks `$0400` and either repeats or retires
the axe. It does not retire immediately on the first offscreen update.

**Asset and geometry boundary.** `$7E:5000` loads from file US `0xCB017` /
JP `0xC8FFD` for the original fight, US `0xCC778` / JP `0xCAF2B` for the
rematch. The original US/JP composition metadata matches. Across original
and rematch, compositions 0–10 match; original 11 is absent from the rematch,
and original 12–17 map to rematch 11–16. The rematch ends its throw in the
idle pose instead of retaining original pose 11: unflipped left/right/top/
bottom extents change from 82/24/24/48 to 32/40/40/48 for that final row.
Animation IDs and WRAM composition addresses therefore cannot be copied
between encounter types without their owning table. This does not establish
CHR/palette equivalence or authorize swapping complete donor artwork.

**Verification and integration.** The new traces preserve lives, positive
player HP and root HP24. Special is selected before native room loading;
fixtures set HP/max24 before preparation and observation and player X176
before activation. No actor phase, animation or RNG is replaced. The 112
repeated isolated native cases cover source initialization, both facing
offsets, full/free pool, child activation, proximity edges, target latching,
final-row geometry and sequence-boundary retirement. Eighteen bounded Go
captures cover the family and helpers. An independent run reproduces all
121 evidence files byte-for-byte. These extend, rather than replace,
the four earlier original-only traces; neither their seed setup nor repeated
runs count as extra gameplay coverage.

Runtime integration keeps four original-encounter animation durations separate:
idle row0 of state0, throw row4 of state2, initial row0 of states1 and4. Only
source `$AF5D`, map1, bank`$7E`/base`$5000` is eligible; rematch `$F6CA` and
inherited-source axes are excluded. The common `$8E2F` reader owns the row,
pose, collision, facing and vertical motion; the adapter changes only a newly
acquired delay. An independent `$AFDB→AFDE` LDA prefix selects the axe offset
in both encounters before the original `$8709` facing helper. Allocation,
including its exhausted-pool scratch result, remains native. Five-ROM checks
cover470 rows across both assets, their velocities/extents and axe immediates.

Keep original-fight idle and follow-through durations, and the shared-across-
encounters regional axe offset, as separate semantic policy fields. Select
by encounter identity, freeze the policy for root and descendants, and defer
live changes until the next encounter. Do not apply the US original's 48-frame
idle to the US rematch, or expose the common rematch speed-up as a JP-only
rule. US assets already support the examined JP mechanics through these
parameters. Preserve source/backlink generations, pending throws and existing
damage. Full defeats, transition cleanup, magic contacts and mixed host-policy
validation remain separate tests.

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

The host phase adapter now selects the retained states through the real
`$00:D9DE JSR $8669`. Native repeat counts and the `$D9E0` yield word own
progress; the source/room/root/state guards exclude linked spawners and health
helpers. On each completed phase, `$D9DB` selects1, then2 repeated5 times,
then3, then20; it sets protection `$0020` only before20 and clears it on leaving
20. The original actor collision routine remains responsible for rejecting
damage. Native death/state changes abandon the loop through their existing
handlers. A missing host cache after a debug restore still clears completed
protection before returning to the US state2 loop.

The open-head timeline and high/low projectile wind-ups are independent
room-pinned leaves. The existing animation-reader adapter maps the four-row
European head sequence onto retained US compositions, using a tagged logical
cursor in `+$1C` and a temporary actor-local visual offset during decoding.
Native state/end handling clears it; no shared animation table is rewritten.
Exact program/pose signatures guard the mapping. Projectile preparation changes
only the first row's delay in body states4/6. CPU flags, motion, collision
decoding, allocation and waits remain native. This integrates the phase and
timing mechanics independently of the geometry profile below and Beginner's
tendril program.

**Host geometry profile.** `$00:D980` now has a guarded root-initializer
boundary before the first linked allocation at`$D98A`. For sourceD974,
room0305, first pose0 and the validated53-composition US layout, it shortens
compositions0/5/6/7/8 by removing their last four bottom-row parts, changes
bottom112→96 and subtracts16 from retained parts' vertically mirrored Y.
Closed composition48 becomes top/bottom4/4. All table addresses and original
CHR/palette references remain; discarded parts remain in their allocated
tail so the operation can be reversed. JP's one changed part palette and
closed-head tile are artwork, not geometry, and are not transplanted.

The first native row was already bottom-anchored. The adapter corrects only
that subtraction (+16 unreflected,0 vertically reflected), then skips US's
extra root+8. Children still use the real native allocators and linked-body−8
placement. A repeated birth from projected data receives no second shift.
Returning to US restores the original metadata and anchor, then re-enters
the now-native D980 prefix for its exact ADC/flags; this handles retained
animation RAM as well as reloads. Validation precedes every write and the
projection runs only at fresh root birth, not on each animation frame.

Unit checks cover all four flips, wrapping coordinates, idempotence, exact
US restoration and malformed/partial-profile refusal. Six complete pose
geometries match all five ROMs; graphics references remain byte-identical
to US. Held-player host runs confirm root/bodyY120/112 US versus128/120 JP,
with the JP open/closed/protected cycle continuing. Both14,500-tick runs exit
normally with zero background mismatches. This is controlled encounter
evidence, not natural traversal, a full victory or donor-artwork acceptance.

Matched host runs reached the encounter through its preceding room loads and
verified the resident animation bundle before holding the player at two heights.
JP completed25 full154-update cycles and Europe20 full194-update cycles; US
remained open. Protection was present only in closed state20. Seven high and
six low preparations per profile retained first-row delays7/7/23. All three
14,500-tick runs exited normally without background-data mismatches or changes
to the user's save. These controlled checks do not establish full-combat or
visual acceptance, and they deliberately retain US geometry.

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
  More precisely, body state17 omits zero-based US rows5/6 and retains row7
  (visual13); head state18 retains row5 (visual4) and omits rows6/7. Their
  remaining stationary poses are not the same ordinal. European variants
  retain the US118-update sequences.
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

The integrated `$8E2F` adapter selects those two row skips only for source
`$F760`, map7, bank`$7E`/base`$5000`, and the matching state/index/three-row
signature. It advances the native cursor before delegating the complete row
reader, which still owns pose/extents, facing, movement, flags and return state.
An unexpected escaped token restores only that speculative cursor edit and is
propagated unchanged. The original `$F161` owner and projectile states25/26
remain native. One room-captured rule controls both sequences; no live setting
change can shorten only one part of a wind-up. Five-ROM comparisons cover the
full head/body and projectile timelines, not merely their final durations.

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

Europe retains this controller too. The English PAL head requests its adjacent
peer at `$00:A553` and waits128 through `$8612`; peer entry is `$A5A0`.
German adds2 and French5 to these sites. Their state10 row still stores63,
but the separate wait overwrites that delay, so all three PAL releases use
the129-update hold. Five-ROM tests match the complete retained programs and13
composition records used by the restored seed family. The PAL animation blobs
are at file `0xCD696` English / `0xCCE78` German and French.

Implementation boundary: one named tree attack-family policy, US default,
captured on room/family creation and shared by the head, peer and descendants.
Represent the peer relationship explicitly, preserve native allocation failure,
terrain, collision and yield behavior, and defer live changes until the next
room/family initialization. Neither live HP nor a pending attack should reset.
The host integration now captures `action_tree_seed_family` with the room's
motion policy (US0, JP/Europe1). `$A975` applies state10, requests only a verified
adjacent peer, then lets native `+$24` expire before resuming the original
`$A97B` orb code. The restored peer, seeds and visual children share the retained
`$A9BF` handler. Slot-local `+$3E` tags distinguish their phases; existing
children finish after a debug-cache reset. The native dispatcher still owns
movement, active-frame waits, death and slot reuse. Bounded RTS calls reuse
the original allocation, animation, terrain and facing helpers. No donor code,
host countdown, foreign-PC dispatch or shared animation mutation is needed.

Zero/one/two seed-slot tests retain successful allocations without retrying
failures; visual allocation failures are also ignored like the original.
Controlled US/JP/European host runs verify64 versus129 preparation updates,
two seeds at X1760/1824,Y416 after28 updates, and subsequent landing, walking,
withering and retirement. Each12,500-tick run self-exits. The player/camera
position fixture yields the same background-check warning in all three, so
these are lifecycle checks, not visual acceptance or natural traversal.
Full tree defeat/bridge extension, magic and broader contact coverage remain
separate encounter-level validation gates.

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

The European, German and French programs also contain this extra wind-up and
spawn, at roots `$00:BA48/$BA4A/$BA4D`, with sequence helper `$856F` and spawn
helpers `$BA72/$BA74/$BA77`. Their relevant animation data is byte-identical
to US at file `0x5782E`. Their double-volley cycle is153 active updates, with
16 updates between shots, just as in JP. This is a regional gameplay choice,
not a PAL clock conversion; the US single volley must not stand in for Europe.

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

Implemented adapter: `$BD9F→BDA2` initializes the root's local counter `+$38`
to1 and executes the native first wind-up. `$BDA8→BDA2` consumes that counter
once and adds the second wind-up; the next `$BDA8` delegates the post-shot idle.
The root's ordinary `$8657` sequence helper does not use the local counter
(`$8669`, which does, is not called here). Native actor destruction/reuse owns
its lifetime; no parallel host slot table is needed. The inherited-source
projectiles are excluded by states `$21/$23`. The original allocator, scratch
exhaustion behavior, facing, child backlinks and native return/yield frames
remain delegated. Offscreen state is deliberately not rechecked mid-volley.
Policy is captured at complete room/retry initialization with the other action
rules. Five-ROM root-call traces and adapter boundary tests cover allocation
failure, activation changes and termination between shots; this is not a claim
of a new full combat/contact or manual visual pass.

### Kasandora Act-2: wall heads, Pharaoh and blue spheres

These are three distinct families. Ordinary wall heads use types `$0E/$0F`
with animation base `$4000`, or `$13/$14` with base `$5000`. The Pharaoh is
type `$0B` in raw room `$0603`; the circling blue enemy is type `$15` in
`$0403`. Neither a shared state number nor a similar appearance establishes
a shared timing policy.

**Ordinary wall heads.** US `$00:C8FF–C943` / JP `$00:C990–C9CE` own the
firing loop. US inserts `LDA #$001E; JSR $86FA` before state2. The separate
native delay costs31 active updates; it is not a longer animation row.
Four 450-frame traces, both ROMs and normal/Special, observe arrows at
frames41/171/301/431 US versus10/109/208/307/406 JP: **130/99-frame cycles**.
The `$4000` sequences themselves match: state2 lasts8 frames, state3 lasts90,
and the restart takes one frame. All14 authored wall-head placements match,
including a duplicated placement in room4; this is a count of records, not
unique coordinates.

The host implements two independent ordinary-head pause rules, one for each
animation-base variant. A complete room initialization captures them with the
other motion/timing choices. The `$00:C908` adapter only bypasses the new
pause for the selected Japanese variant and tail-transfers to `$C90E`, where
native LDA2 starts the ordinary animation/allocation/recovery sequence. Existing
wait continuations never re-enter this prefix. Source/base checks exclude the
Pharaoh family, other scenes, unknown actors and incompatible CPU modes.
Registers, RAM and the native return owner stay intact at the transfer.

Five-ROM opcode checks confirm the activation prefix and Western LDA30/delay
versus JP's immediate state2. Controller cases cover all four sources, four
short/long mixes and 32 flag combinations, with malformed-owner exclusions.
Generated boundaries split `$C8FF→C908→C90E`; the native offscreen gate remains
before the override and the complete US pause body is retained as fallback.

Sources are US `$C8E5/$C8F3/$C8C9/$C8D7`, JP
`$C976/$C984/$C95A/$C968`, respectively. They contain HP3/attack1; Special
promotes attack to2. The `$5000` variant has its own shared16-frame firing
and120-frame idle sequences, giving **168/137-frame cycles** with the same
extra US delay. The four blue-enemy walks below also activate an authored
`$5000` head at `(1504,224)`: arrows appear at82/250/418/586 US versus
51/188/325/462/599 JP. These observations share those traces, not additional
test cases. The arrows
spawn after the parent, at facing-relative X−14/+14 and Y+4, inherit its
source/attack/backlink, and receive HP1 explicitly even in Special mode.
Failed allocation skips the arrow and continues the idle/restart sequence.

**Pharaoh root and descendants.** Sources US `$C1A2` / JP `$C239` lead to
root entries `$C1AE/$C245`; child programs start at `$C24E/$C2E5`. Four
600-frame normal/Special traces verify the original fight with HP24 roots:

| First-cycle event, trace frame | US | JP |
| --- | ---: | ---: |
| Landing/bounce state `$0B` starts | 165 | 165 |
| Sphere allocated; root enters waiting state `$19` | 205 | 189 |
| Sphere enters flight state `$0E` | 236 | 220 |
| Sphere converts to left wall head, state `$0F` | 324 | 308 |
| First arrow | 366 | 350 |
| Head retires | 396 | Remains active |
| Same head's second arrow | None | 486 |

The root's state `$0B` has the same24-frame bounce program in both ROMs;
US appends one stationary16-frame row, giving40 frames. The flight sequence
and speed are shared: state `$0E` moves2 pixels horizontally per update.
Both versions convert the existing sphere slot into a wall head when X<80
or X≥448, fixing its position to `(64,176)` or `(448,176)` and assigning
HP3/attack1 (attack2 in Special). It retains the **boss source**, rather than
the ordinary head source used to initialize its fields.

Both play the26-frame emergence and16-frame firing sequences. JP then uses
the120-frame idle and repeats, for136-frame shot intervals. US plays its
additional30-frame withdrawal state `$1B` and retires. Thus the US arrows
also originate from sphere-derived heads; the difference is the head's
lifetime and repeated firing, not an independent random-arrow generator.
US withdrawal uses an additional visual, so the shared sequences must not
be conflated with the entire regional asset blob.

Root `+$38` is a pending-sphere flag. A successful pool allocation sets it;
the sphere clears it through backlink `+$3A` after its30-frame formation.
The root checks it between30-frame waiting sequences. Allocation failure
instead goes directly to takeoff; it must not leave a permanent wait.
The resulting wall head retains its root backlink, and each arrow links to
that head. Slots can be reused after retirement, so source plus address
alone is insufficient family/generation identity.

**Circling blue enemies.** US `$00:CB51–CB7A` / JP `$00:CBD3–CBFC` and
states `$10–$12` match in movement behavior. Four600-frame traces activate
the authored type `$15` at `(1360,344)` using180 frames of Right followed by
420 idle frames. Normalized position/velocity/state/row/timer/facing/flags
match at every frame across both regions and modes. Each half-cycle contains
three24-frame circling sequences and a16-frame diagonal dart at3 pixels per
axis per update; the full restart cycle is177 frames. All nine placements
match. This supplies no basis for a regional slowdown setting for this
enemy. It does not establish parity for unrelated projectiles or other acts.

**Evidence and integration.** Twelve booted traces preserve the room,
player lives and nonzero HP throughout. They use native act-entry loading
followed by the owning room chain; Special is selected before loading.
Fixtures set HP/max24 and, except for the blue-enemy walk, player position.
No actor phase, attack, RNG or loaded animation is replaced. Forty repeated
isolated native cases additionally check both wall-conversion thresholds,
normal/Special, zero/one free slot, and ordinary-arrow allocation in both
facings. Twenty bounded Go disassemblies supply code evidence. An independent
run reproduces all205 report/trace/state/WRAM/screenshot files byte-for-byte.

Loaded animation/composition assets: `$4000` from file US `0xD9943` / JP
`0xD731B`; `$5000` from US `0xDD27A` / JP `0xDD132`, inherited from Act-2
entry `$0303`. The three reviewed `$4000` wall-head/arrow compositions and
the first40 `$5000` compositions match byte-for-byte. This is metadata
compatibility, not verification of identical CHR pixels or palettes.

Represent **ordinary wall-head delay**, **Pharaoh landing hold**, and
**Pharaoh wall-head lifetime** as separate semantic choices, US by default.
Capture the two boss choices together in an encounter policy so descendants
keep a consistent snapshot. US assets already contain
the shared sequences needed for those JP behaviors; a JP ROM need not be a
runtime requirement for the mechanics. Capture the ordinary policy at room
initialization and the boss policy for the entire encounter family. Defer
setting changes until the next owning initialization; never restart a wait,
spawn a replacement sphere, clear existing heads or replay rewards when a
toggle changes. See the [rematch comparison below](#pharaoh-death-heim-rematch)
for encounter-specific timing. Exact regional artwork, death/magic/cleanup
interactions, full victories and host mixed-policy validation remain gates.

### Pharaoh Death Heim rematch

Raw room `$0407` retains source US `$F6FA` / JP `$F779`. Its wrappers at
`$00:F706–F711` / `$00:F785–F790` set the room-owner backlink `$001C`, call
the shared rematch setup, and tail-call the original root `$C1AE/$C245`.
The same sphere, head and arrow programs therefore consume a different
room-owned `$5000` animation blob. Boss HP remains24; attack is1 in normal
mode and2 in Special. Neither mode changes the timings below.

| Phase or movement | Kasandora US | Kasandora JP | Death Heim US | Death Heim JP |
| --- | ---: | ---: | ---: | ---: |
| Pre-landing hold, state `$0A` | 20 frames | 20 frames | 16 frames | 16 frames |
| Landing/bounce, state `$0B` | 40 frames | 24 frames | 56 frames | 24 frames |
| Sphere formation, state `$0D` | 30 frames | 30 frames | 30 frames | 30 frames |
| Sphere horizontal speed, state `$0E` | 2 px/update | 2 px/update | 4 px/update | 4 px/update |
| Sphere flight sequence / wall-test interval | 8 frames | 8 frames | 4 frames | 4 frames |
| Head emergence, state `$0F` | 26 frames | 26 frames | 13 frames | 13 frames |
| Firing sequence, state `$02` | 16 frames | 16 frames | 16 frames | 16 frames |
| Head after firing | Withdraw30 frames, retire | Idle120 frames, repeat | Withdraw15 frames, retire | Idle120 frames, repeat |
| Arrow horizontal speed, state `$04` | 3 px/update | 3 px/update | 6 px/update | 6 px/update |

The regional state `$0B` difference is still a stationary row after the shared
24-frame bounce: US adds16 frames originally and32 in the rematch. The
regional head-lifetime difference remains in executable code; JP returns to
the idle/fire loop instead of using US-only state `$1B`. Shared rematch
acceleration must not be mistaken for a JP difficulty option.

Eight paired600-frame fixtures cover both regions, rooms and modes. Original
controls retain the prior first-sphere/arrow frames205/366US and189/350JP.
With the same player start `(240,192)`, the rematch yields:

| First-cycle event, trace frame | US | JP |
| --- | ---: | ---: |
| Landing/bounce begins | 161 | 161 |
| Sphere allocated; root begins waiting | 217 | 185 |
| Formation ends; pending flag clears; sphere enters flight | 248 | 216 |
| Sphere converts to left head at `(64,176)` | 292 | 260 |
| Firing sequence begins | 305 | 273 |
| First arrow | 321 | 289 |
| Head retires | 336 | Remains active |
| First arrow retires | 394 | 362 |
| Same head's second arrow | None | 425 |

Root `+$38` clears31 measured frames after allocation, including the child's
first dispatch. The root observes the clear after two30-frame waiting
sequences and takes off60 frames after allocation; this does not make sphere
formation a60-frame animation. Full-pool allocation goes directly to takeoff
without setting a pending flag. These contracts are unchanged in the rematch.

The sphere converts at X<80 or X≥448, clamps to X64/448 and Y176, and receives
HP3 and attack1/2. Its source and root backlink survive initialization from
the ordinary head record. A head allocates an arrow at facing-relative
X−14/+14 and Y+4, with explicit HP1 and BCD score1 in both modes. A full pool
skips that shot; US still withdraws and JP still idles before its next attempt.
Arrow retirement tests unsigned X≥512, including leftward underflow, after
each one-frame movement sequence. It is not an offscreen-camera test.

US arrows remain alive after their head retires: in the rematch fixture,
the first arrow continues another58 frames. A future family observer must
retain the arrow's captured encounter/generation even when its immediate
parent no longer occupies that slot. Requiring a currently live head would
incorrectly discard it. This demonstrates ordinary retirement, not cleanup
after boss defeat or magic.

The rematch blobs come from file US `0xDCE6A` / JP `0xDCD45`; originals are
`0xDD27A/0xDD132`. Only states4,10,14,15 change from original to rematch in
both regions, plus states11 and27 in US. Composition metadata is identical
between encounters within each region, and the first40 visual compositions
match between regions; US has one additional withdrawal visual. CHR pixels
and palettes remain a separate media-verification task.

Validation adds140 repeated isolated cases and16 bounded Go disassemblies:
source-record initialization, both wall thresholds, both facings, normal/
Special, zero/one free slot for root/head allocation, pending-flag release,
and both arrow arena bounds. Booted fixtures use native room loading, verify
the loaded animation bytes, and retain room, lives and positive player HP
throughout observation. Normal/Special normalized root movement matches for
each region/encounter. No actor phase, animation or RNG is replaced.
An independent run reproduces all121 report/trace/state/WRAM/screenshot files
byte-for-byte.

Keep the landing-hold choice encounter-aware: JP means removing the extra
US row, not subtracting16 frames in every room. Freeze landing and head-lifetime
choices for the encounter, and propagate them to descendants. Keep shared
rematch speeds/timing attached to encounter identity. US assets contain the
sequences required for these mechanics without a JP donor ROM. Full defeat,
magic, room-exit cleanup and mixed-policy host execution remain validation
gates; attack-loop/rematch discovery is closed.

The host implements three room/retry-pinned choices: original landing,
rematch landing and repeated head firing. The animation reader recognizes
the ten-row bounce plus encounter-specific final hold, then lets the native
reader consume the real terminator when that hold is omitted. It changes no
shared resource bytes or root allocation/pending-flag logic.

At `$00:C2CB`, repeated heads select retained idle state3 instead of withdrawal
state27 and tail to the original `$C2CE` JSR. Its native return word `$C2D0`
and state3 identify completion at `$C2D1`, which returns to firing at `$C2A1`.
That in-flight idle can finish even if a debug restore loses the policy cache.
The ordinary withdrawal/retirement path remains native. Exact source, room,
animation base and CPU shape checks exclude ordinary heads, spheres and arrows;
no currently live parent is required. Upstream generated boundaries end at
the native sequence/allocation continuations so inlining cannot bypass either
adapter. Neither path creates an emulated return frame or host timer.

Five-ROM checks compare338 rows and their complete pose records, plus the
head-loop opcodes. Host tests cover mixed landing/lifetime choices, both
encounters, CPU flags/widths, facings, invalid ownership, dropped-shot context,
missing caches and unexpected animation-reader returns. These bounded tests
do not establish full-victory, magic or manual presentation parity.

Four controlled host runs now verify the complete US/JP attack loops in both
encounters. Native room loads supply the checked1863-byte US animation bundle;
only the player's position, hold and invulnerability are prepared. The original
landing measures40/24 updates over27/30 completed cycles, and the rematch56/24
over29/37. Japanese heads repeat at136-update intervals in both rooms; Western
heads retire after their30/15-update withdrawal while fired arrows continue.
All four runs exit normally, with no background preflight mismatches and no
changes to the player's SRAM. They are controlled encounters, not natural
traversals, full victories or a manual visual comparison.

### Aitos Act 1: platform skulls and volcano fireballs

**Skull collision and proximity.** Raw room `$0104`, type `$0C`, retains
source US `$00:D382` / JP `$00:D404`. Its 12-byte record sets HP0 and attack1
in both regions; Special promotes attack to2 but leaves HP0. The differences
are victim flag `$0800` in JP and BCD death score `$20` in US (`$00` in JP).
HP0 therefore does not mean the actor is already dead or invulnerable.

The complete native sword resolver, US `$00:8A3C` / JP `$00:8A2B`, tests
`$0800` after broad-box and eligible composition-part contact. A set bit
takes the deflection-sound path without subtracting HP. Otherwise a successful
hit on this HP0 victim enters generic death, sets state `$FF` and flag mask
`$0038`, and awards its BCD score. This differs from the `$0020` early victim
filter used by the Marahna boss. Captured native sword/skull fixtures verify
both outcomes, reverse them by changing only `$0800`, and verify that a second
resolver call does not award a second US reward: 20 stored score units,
or 200 displayed points. Clearing JP protection
alone still awards zero because its score field is independently zero.

US `$00:D38E–D3BE` / JP `$00:D410–D440` first rejects the offscreen `$0400`
flag, then compares absolute cached-player-to-skull hot-point distances:

| Proximity test | US | JP |
| --- | --- | --- |
| Horizontal | `abs(dx) < 32` | `abs(dx) < 24` |
| Vertical | `abs(dy) < 64` | `abs(dy) < 24` |

Both conditions must hold. A failed test plays state `$2F` (32 frames) and
restarts, producing observed33-update checks while active, not continuous
per-frame proximity tests. Success plays state `$30` (2+3+4+3 frames), then
retires through `$85B7/$85A6` without a kill reward. Once started, it does not
recheck proximity or cancel when the player moves away.

All five authored placements match: tile coordinates `(156,50)`, `(160,52)`,
`(168,57)`, `(173,56)`, `(177,54)`. The initial bottom extent12 makes the first
hot point `(2496,788)`, not `(2496,800)` or an assumed tile-aligned offset.
The captured native flying-platform journey triggers all five US skulls but
only the middle three JP skulls. This is an outcome of that route and sampling
phase, not a fixed rule about which skulls may explode. Each observed explosion
lasts12 frames, and the score stays unchanged.

**Volcano fireball timing.** Type `$08`, source US `$00:CF9E` / JP
`$00:D01E`, uses equivalent programs at `$00:CFAA–D024` / `$00:D02A–D0A4`.
The six placements are tile X228–233,Y64 in both ROMs. All four room damage
rectangles also match, including X226–233,Y59–63 with damage24 near the shaft.

Activation requires horizontal distance below256. A native RNG call selects
delay `(value & 63)+1`; initial RNG state and actor scheduling can make launch
timestamps differ without changing movement speed. After the wait:

1. State `$21` moves upward1 pixel/update for16 frames.
2. State `$23` holds for16 frames.
3. State `$22` rises4 pixels/update. Counter `+$38=48` is decremented at each
   eight-frame row boundary, giving384 rising frames, not48 frames.
4. The actor shifts X−48 and enters state `$24`, moving left1/down6 per update
   until the native ground check permits reinitialization from its source and
   authored coordinates.

Four650-frame normal/Special traces observe all six actors through their
first reset. Phase-aligned position, velocity, state, row, timer, counter and
subframe histories match across both regions/modes: 513 samples for the first
placement and505 for each of the others. These include128/120 return frames,
respectively, and the reset sample. The traces preserve native16-bit coordinate
arithmetic, including offscreen travel; they do not clamp it to the viewport.
Special promotes this family's HP/attack from1 to2 without changing motion.
There is no evidence for a regional speed setting for this family. Other lava
objects, terrain or presentation mechanisms are not established as equivalent
by these tests; the broader reported slowdown remains unassigned.

**Evidence and integration.** Eight booted traces and108 repeated isolated
cases cover both regions and modes. The92 proximity cases cover signed axis
thresholds, corners and the offscreen gate;16 sword cases cover native/reversed
protection and one/two resolver calls. Twenty bounded Go disassemblies supply
the code. A separate run reproduces all121 evidence files byte-for-byte.

Booted fixtures load the native Aitos entry and verify its `$4000` asset bytes.
They explicitly position the player and camera and set HP/max24. Platform
fixtures ride1800 preparation frames, then refill HP once before the1200-frame
skull observation; volcano fixtures run650 frames without healing. Preparation
and measured traces retain room, lives and positive HP. No enemy, animation,
RNG or phase fields are altered in booted tests. Isolated collision cases
use a captured sword pose and controlled contact coordinates; they are not
full sword-playthrough or magic-reachability tests.

The owning animation/composition blobs are file US `0xC4C17` / JP `0xC1B1C`.
States `$21–$24/$2F/$30` and the seven reviewed compositions42/43/47–51 match.

The B10 host policy integrates four independent room-scoped leaves: armor,
BCD death reward, exclusive horizontal range and exclusive vertical range.
At `$966C→966F` the initializer projects only fresh flags/reward, then executes
the native professional promotion, animation and bottom anchoring. First birth
uses descriptor Y: source field`+$32` is not installed until `$95B9`, after
the common initializer returns. Reused-slot source bytes are not identity.
At `$D39A→D39D` and `$D3A2→D3A5`, exact CMP24 prefixes retain the original
distance helpers, offscreen gate, waits, sound, explosion and retirement.
No hit resolver, score award routine, pool or per-frame scheduler is replaced.

Five-ROM checks confirm PAL uses the US armor/reward/proximity values, but
its base record has HP2/attack2 rather than US/JP's HP0/attack1. Those use
the independent A06 stat policy, not an implicit effect of B10. The new tests
cover all 16-bit CMP inputs and mixed armor/reward/range choices; the earlier
native collision fixtures establish deflection/reward semantics. This is not
a new full host playthrough of the platform ride or a full magic-interaction
test. Live setting changes remain deferred until room/retry entry.
This proves metadata compatibility, not identical CHR pixels or palettes.

Keep **skull sword protection**, **kill-score value** and **proximity bounds**
as separate semantic fields in a room-scoped policy, US by default. A regional
preset can select them together without coupling them to the language or donor
art. The shared US metadata supports the examined JP mechanics without a JP
ROM at runtime. Defer changes until the next room initialization; never revive
a retired skull, cancel an explosion or award points when toggling. Host mixed
policies, magic contacts and full traversal remain implementation tests. Do not
create a speculative volcano-speed option from the unverified broader claim.

### Wizard original fight and rematch

Original raw room `$0802` retains source US `$BDFF` / JP `$BE9C`; Death Heim
`$0307` uses `$F6E2/$F761`. Root entries are `$BE0B/$BEA8`. The first form
checks HP at `$BE41/$BEDE`; values below 12 branch into the second form,
while 12 or more retain the first. The check occurs between attack sequences,
not immediately when damage crosses the threshold.

US `$BE78–BE7D` adds `LDA #30; JSR $86FA` after state `$0B`, following the
three-projectile spread. JP proceeds directly from state `$0B` to its
position/next-attack decision at `$BF15`. The delay helper consumes 31 native
updates, so this portion lasts 55 updates in US versus 24 in JP. This is
verified in normal/Special and original/rematch fixtures. The changed audio
dispatches and relocated addresses are distinct from this timing change.

| Shared original → rematch change | Original | Rematch |
| --- | ---: | ---: |
| Lightning preparation, state `$0C` | 50 frames | 34 frames |
| Initial spread, states `$12–$14` | 3 px/update per moving axis | 6 px/update per moving axis |
| Second-form preparation, state `$1E` | 40 frames | 24 frames |
| Second-form launched child, state `$16` | 108 frames | 94 frames |

The last row changes more than duration: its moving rows increase horizontal
speed from 1/2/3 to 3/6/8 px/update, after a shorter stationary formation.
These six changed states are identical across regions within each encounter.
US/JP blobs match byte-for-byte: original file `0xC27CF/0xB7622`, rematch
`0xC1E65/0x7763A`, loaded at `$7E:5000`. Their composition metadata also
matches across regions; this does not establish CHR/palette identity.

Sixteen booted fixtures cover region × encounter × normal/Special × two
health phases. The second-form fixtures set root HP to 11 and let the native
check run; they do not claim an unassisted victory. The transformation's
complete observed phase lengths are 55, 80, 40, 40 and 90 frames in states
`$0E/$20/$21/$0F/$10`, matching both regions. Not every position-dependent
second-form attack is exercised by these idle fixtures. The later position
branch matrix below supplies isolated coverage of those decisions; changed
animation rows also have native-decoder coverage in both facings.

The second-form position logic at US `$BF17–BFAB` / JP `$BFAE–C042`
matches after code/audio relocation. The facing selects an exact X target
of 96 or 160. At that X, cached player Y equal to boss Y minus 24 repeats
state `$15`; a different Y selects `$17`. A mismatched X also returns to
`$15`. At boss X 32 or 224, the facing reverses; elsewhere it faces the
player. The subsequent vertical checks select `$18` versus `$1B`, and
`$19` versus `$1A`, according to whether the player is above the boss.
Equality follows the lower/not-above branch. These are shared rules,
not additional regional attack variations.

Use a first-form post-spread-delay policy, frozen with the encounter. Keep
the common rematch changes and HP threshold unchanged. Do not add a blanket
Wizard animation-speed setting or reset a pending native delay when toggling.

The European roots at `$BACC/$BACE/$BAD1` retain the US31-update pause, at
`$BB39/$BB3B/$BB3E`. Their corresponding delay helper is `$8612`.
The integrated room-stable prefix skips `$BE78→BE7E` only for the selected
Japanese pause rule and verified `$BDFF`/map2 or `$F6E2`/map7 owner, base`$5000`,
state`$0B`. It edits no registers, actor fields or return frames. The native
HP/position decisions and form transition remain in the shared suffix.

### Flaming Wheel original fight and rematch

Sources US `$D838/$F712` and JP `$D8BA/$F791` identify original raw `$0704`
and rematch `$0507`. The complete 83-instruction root/projectile-spawn program,
US `$D844–D927` / JP `$D8C6–D9A9`, differs only in reviewed relocation and
audio-command values. Both regions load identical animation/composition
blobs per encounter: original file `0x97C76/0xDEB14`, rematch
`0xDF9D7/0xDE061` at `$5000`.

State 0 traverses 224 pixels in 80 active frames originally and 40 in the
rematch: its 20 rows halve duration and double horizontal velocity. The
intervening repeated state-7 wait still takes 32 frames. A jump is selected
only with cached player Y < 192 and absolute horizontal distance < 112;
the equality cases roll instead. After the jump, the five-shot volley uses
12-frame state-4 intervals in both encounters. Projectile states 8–12 move
at 1 px/update per moving axis originally and 4 in the rematch. Their
eight-frame animation/retirement-test interval does not shrink.

The spawn helper `$D903/$D985` chooses one of each packed state pair according
to facing: `$0C0B,$0A08,$0909,$080A,$0B0C`. Descendants retain source, attack
1/2 and the root backlink, with HP/score 0 and flag `$20`. Zero-free-slot
fixtures produce no active child and do not introduce a retry. Like other
native allocators, failure returns the dummy slot rather than a valid actor;
an HLE replacement must preserve skipped attacks without treating that
pointer as a real object. Shared `$A655/$A614` retires children only at a
sequence boundary after the offscreen flag is set.

Eight booted normal/Special fixtures verify rolling timing in both encounters
and a complete rematch volley in each region/mode. Isolated tests cover
Y 191/192, horizontal distance 111/112 in both directions, all five packed
shot choices, both facings and full/free pools. All changed animation rows
are decoded natively with velocity and signed extents checked.

No regional Wheel attack-speed policy is justified by this comparison.
Keep shared rematch behavior attached to encounter identity. Full traversal,
boss-defeat cleanup and media identity remain separate checks.

### Viper attack selection and rematch

Original raw `$0805` uses source US `$E483` / JP `$E504`; rematch `$0607`
uses `$F72A/$F7A9`. At US `$E4D8`, the root makes one native RNG call and
selects lightning when `value & 3` is zero. JP `$E559` makes the same call,
then uses `LSR; BCS` to select lightning when the low bit is zero. Exhaustive
native fixtures cover all 256 return bytes: 64 select lightning in US and
128 in JP. This measures the predicate, not a promise of twice as many bolts
per unit of playing time. Preserve one RNG draw per decision; do not add a
second random test to emulate the Japanese choice.

After a non-lightning result, absolute X distance below 48 selects the close
attack; equality selects the directional movement branch. The lightning
child uses distance thresholds 32 and 96 for its three launch trajectories.
All boundaries and both signs are verified; normal/Special and original/
rematch checks retain the regional predicate. Booted scenes also reach the
native decision and subsequent bolt flight without injected RNG.

The room-owned animation blobs match byte-for-byte between regions: original
file `0xC8000/0xC5F6D`, rematch `0xCA81A/0xC8800`. The shared rematch changes
are not part of the regional random-choice setting:

| Sequence | Original | Rematch |
| --- | ---: | ---: |
| Bolt descent/impact, states 4–6 | 34 frames | 13 frames |
| Ground-travelling bolt, state 7 | 4 px/update | 8 px/update |
| Auxiliary vertical movements, states 8/9 | 186 frames | 178 frames |
| Close-attack descent, state 12 | 55 frames | 32 frames |
| Close-attack rebound, state 13 | 38 frames | 20 frames |
| Linked body sequence, state 14 | 170 frames | 117 frames |
| Return movement, state 18 | 82 frames | 66 frames |

The bolt's first descent row doubles speed while halving its duration; the
three stationary impact rows are removed. The final fast descent row remains.
Ground travel keeps the 12-frame animation/retirement-test interval. Eight
booted fixtures verify first-trajectory descent 34/13 and ground speed 4/8;
all changed rows have isolated native duration/velocity/extent checks.

The auxiliary-controller follow-up compares five US ranges
`$E58B–E5CE`, `$E5DB–E605`, `$E612–E639`, `$E63A–E67A` and `$E67B–E6BD`;
the JP equivalents are +`$7F`. The intervening 12-byte actor records are
excluded from instruction decoding. These controllers match after reviewed
code relocation:

- The two proximity-controlled parts react only while the parent is in
  state 2 or 3 and horizontal distance is below 32. Exactly 32 does not
  trigger them. They select state 9 or 8 respectively.
- The linked body reacts to parent state 12, clears flag `$0010` and runs
  state 14; its continuation restores that flag and stops its movement.
- A supervisor copies the parent's hit bit `$0008` and field `+$26` to
  the adjacent linked part. Another keeps the adjacent part at the parent's
  coordinates, and stops that part's handler and velocities when parent HP
  reaches zero. The supervisors retire once the defeated parent also has
  status `$0800`.

Together with the Wizard checks, 1,536 additional isolated native cases
cover original/rematch assets, normal/Special mode, both regions, equality
boundaries, parent states, HP and cleanup gates. Twelve bounded Go windows
and all native results repeat exactly. This closes the named US/JP position
and auxiliary-controller discovery gaps; it does not establish complete
fight, magic, recycled-slot or PAL auxiliary-controller parity.

Freeze the attack-choice predicate with the root and its descendants. Keep
existing pending decisions, children and damage when a new setting is queued.
Original/rematch source and generation remain separate from the policy;
the shared room-specific timing is not selected by language or JP artwork.

### Tanzra forms, timer and projectile strength

Final-room `$0807` source is US `$F80F` / JP `$F88E`. Its root begins at
`$F81B/$F89A`, reusing the room-owner setup. Three gameplay differences and
one movement-row difference are verified:

- **First-form closing sequence:** state 10 lasts 64 US / 36 JP frames.
  Its last stationary row is 32/4 frames; all other rows match. Flag `$20`
  stays clear during states 5 and 10 and is set afterward, so the first-form
  vulnerable interval is 100/72 active frames (36 shared plus 64/36).
- **Second-form timer:** generic boss-defeat handling increments `$E8` to
  stop the clock. US second-form entry `$F8F5` clears it at `$F8FC`; JP
  `$F974` clears relocated collision/cast gates but has no equivalent `$E8`
  clear. Both timer routines, US `$02:BC82` / JP `$04:8C7D`, return immediately
  while `$E8` is nonzero. With the gate clear, `$E5` divides updates by 60 and
  `$E6/$E7` count down in BCD, saturating at zero.
- **Second-form projectile:** activation `$FD25/$FDA2` explicitly sets
  `+$2A` attack to 3 US / 4 JP before state 6, then state `$22` flight. PAL
  activation is EU`$FA1E`, DE`$FA20`, FR`$FA23` and explicitly assigns5.
  These assignments override inherited attack, including mode promotions.
  It is not HP or a general increase to all final-boss attacks.
- **Upper-body turn:** state 48 lasts 37 US / 38 JP frames. JP extends the
  first horizontal row at −4 px/update by one frame, before the shared
  turning rows. This also adds four pixels of leftward displacement for the
  unmirrored sequence; it is not merely a longer stationary pause.

The `$5000` assets are file US `0xC7727` / JP `0xC46C9`. Only states 10 and
48 differ; their composition metadata matches across regions. Paired bounded
Go disassemblies cover the first form, second-form controllers and children;
reviewed differences outside the timer clear and attack immediate are address
relocations or audio routing/IDs. This is not a claim of audio or pixel parity.

Eight booted fixtures cover both regions/modes and forms. First-form traces
run 600 frames. Second-form traces run 1,100 frames after a **controlled
defeat dispatch**: the live first-form root is assigned HP 0, state `$FF`,
death flags and the native boss-death handler. Native death, stop-clock and
secondary-handler dispatch then run without replacing the ROM. This tests
the transition contract, not natural combat difficulty or a full victory.
The clock stops at fixture frame 131 in both, resumes at 132 only in US,
and stays stopped through JP's entire second-form observation. The first
3/4-strength projectile and complete 37/38-frame turn occur in both modes.
All accepted preparations and observations retain player lives and positive
HP. Initial HP/max24 and documented setup edits are confined to scratch states.

The combined Wizard/Wheel/Viper/final-boss batch also tests all changed
animation rows with the native decoder, signed extents and both facings,
the Viper's random-byte domain, allocation boundaries, health thresholds,
timer gate/BCD edges and explicit projectile attack initialization. Keep
first-form exposure, second-form timer behavior, projectile strength and
turn movement as separate semantic policy fields, frozen for the encounter.
Do not restart/refill the clock or modify already-launched attacks on a toggle.
US assets plus normalized timing/movement parameters support these mechanics
without requiring JP graphics. Complete victories, magic, pool contention
across whole families and host mixed-policy behavior remain validation gates.

The host now integrates closing duration, phase-two clock behavior and upper
turn as independent room-cached policies. The native animation reader owns
poses, mirrored movement and return state; the adapter changes only the
audited row delays (state10/row7 and state48/row0). `$00:F8FC→F8FE` skips the
8-bit `STZ $E8` for Japan, retaining the current divider, remaining time and
surrounding collision/cast-gate clears. Its guard requires the defeated first
form's native secondary handler, not simply any actor inheriting `$F80F`.
Five-ROM program tests verify row bytes, total durations, collision headers
and displacement; clock tests cover all256 low gate values and preserve
adjacent memory/CPU state.

Three host integration runs now load Death Heim through native pending-room
requests after an ordinary Continue/Palace/Fight entry. A held player remains
at the authored final-room entrance; one controlled native first-form defeat
dispatch reaches phase two. Production guards accept the Japanese clock skip
and Japanese/European projectile values. End WRAM retains the stopped Japanese
clock, running US/European clocks, and minion HP/reward2/2 versus European1/1.
All runs exit normally with positive HP and unchanged native SRAM. These are
controlled transitions, not evidence of a naturally completed campaign or
victory; the explicit room requests and defeat preparation are part of setup.

The accepted four-family batch comprises 40 booted fixtures, 1,928 isolated
native cases (852 logic/boundary cases and 1,076 animation-row decodes), and
45 bounded Go disassembly windows. An independent complete rerun reproduced
all 613 evidence files byte-for-byte. Preparatory loads, failed exploratory
setups and repeat executions are not additional cases. These are fixture
counts, not numbers of independent regional differences or full-game coverage.

### First-act boss program comparison

Four first-act families have matching reviewed root/child programs after
accounting for code/helper relocation and audio dispatch. Their complete
resident animation/composition blobs and spawn records also match:

| First-act boss | Raw room | US / JP source | US / JP reviewed code (bank `$00`) | US / JP animation file offset |
| --- | --- | --- | --- | --- |
| Fillmore Centaur | `$0101` | `$AD45/$ADD9` | `$AD51–AF5C` / `$ADE5–AFF0` | `0x3EFC7/0xAEF9F` |
| Bloodpool winged boss | `$0102` | `$B786/$B81A` | `$B792–B918` / `$B826–B9AC` | `0x31B77/0x31B70` |
| Aitos dragon | `$0304` | `$D646/$D6C8` | `$D652–D837` / `$D6D4–D8B9` | `0x3CC24/0x3CC24` |
| Northwall humanoid boss | `$0406` | `$E7C6/$E845` | `$E7D2–E951` / `$E851–E9D0` | `0xDC622/0xDACF1` |

All four load their metadata at `$7E:5000`, start with HP 24 and normal
attack 1, and promote attack to 2 in Special mode without changing HP. These
results also hold for the Antlion below. They do not support a blanket
regional boss-HP or attack-speed multiplier.

Sixteen booted fixtures cover these four families in both regions/modes.
After alignment to the same attack phase, each pair matches for 400 active
updates (401 samples): position, velocity, animation state/row/visual/timer,
facing, collision extents and role counter. Alignment uses the native update
counter `$88`, not raw video frames. One JP dragon video frame repeats the
counter and unchanged motion; treating it as another gameplay update would
produce a false speed difference. The Aitos dragon belongs to **Act 1**,
not Act 2, despite an older label in the effect research.

Native allocation tests cover zero/one free slot for Centaur, Bloodpool,
Antlion and Northwall projectiles, and zero/one/two for the dragon's two-shot
controller, in both facings and modes. Failed allocations do not publish a
child. The dragon publishes only the available shots and retires its volley
controller even on partial failure; it does not retry later. Children inherit
source and attack, with HP/score 0. Their backlink is to the allocating actor;
for the dragon this is the volley controller, not directly the boss root.

This comparison does not prove every position-dependent route, full defeat
and cleanup, every shared helper's behavior, or audio/CHR/palette identity.
No regional speed setting is justified for these examined programs. Any
future difference must be tied to its owning routine or data rather than
inferred from a different address, video-frame count or boss nickname.

### Antlion trigger and post-volley decision

Kasandora Act-1 room `$0203`, type `$04`, retains source US `$C66F` / JP
`$C6FE`. The reviewed bank-0 programs are `$C67B–C80D` / `$C70A–C89E`.
Two independent differences are established:

1. **Horizontal encounter gate:** US `$C67D` compares cached player X `$80`
   with `$0980` (2432); JP `$C70C` compares with `$0900` (2304). Japan's
   threshold is 128 pixels earlier. Actual appearance also depends on native
   actor loading and the intro; this is not a universal wall-frame timestamp.
2. **After the volley:** both play a 12-frame firing preparation and create
   six projectiles, three in each direction. US `$C718` then plays state 12
   through `$8657`: three rows lasting 3, 3 and 30 frames. Only after those
   36 frames does `$C71E` measure horizontal distance through `$85BE`.
   Japan `$C7A7` calls relocated `$85AD` immediately after the volley. If
   the absolute distance is below 64, it advances; otherwise `$C7AF` passes
   `$003C` to delay helper `$86E9`, waiting 61 updates before branching to
   the next firing preparation at `$C772`. That branch is **not** merely
   another distance poll.

For a player who stays at least 64 pixels away, the US repeat interval is
12+36 = **48 updates**, and JP is 12+61 = **73**. If close when the decision
runs, JP advances without the US post-volley hold. Equality at 64 takes the far
branch on either side. Near decisions continue through shared side-object,
burrowing and re-emergence phases; this is not a global boss-animation speed.

The placement is identical: authored tile `(166,55)`, world `(2656,880)`.
The initial bottom extent 66 puts the live hot point at Y814. Animation blobs
at US file `0xDB07E` / JP `0xD9758` share states 0–11 and all referenced
visual compositions; US adds state 12. The original US asset therefore
supports the examined JP strategy without donor graphics.

Eight booted Antlion traces cover both regions/modes with an idle player and
a player holding Left to stay far away. Each far trace verifies at least
seven firing starts with every interval 48 US / 73 JP; the US idle traces
verify the first 36-frame state-12 hold, absent in JP. Isolated native cases
check trigger X values `$08FF/$0900/$097F/$0980` and signed distances
±63/±64 in both modes. These are separate from the allocation tests above.

Expose **encounter threshold** and **post-volley strategy** independently,
US by default. Freeze the threshold before the room's intro and the attack
strategy for the encounter. A toggle must not restart the intro, replay a
volley, or reinterpret an existing native/animation wait. Full combat,
mixed-policy integration and family-wide contention remain validation gates.

Implementation uses the US artwork and original six-shot allocation. Prefix
`$C67D→C680` changes only the threshold CMP. `$C718→C71E` skips state12 for
the Japanese strategy, leaving `$85BE` to calculate distance. At `$C721`, a
near result resumes `$C726`; a far result follows the native `$86FA` wait
contract: zero motion, delay60 at slot+24, and `$C6E3` at slot+12. Returning
through Antlion's native RTS leaf `$C682` preserves the real dispatch owner;
there is no fabricated stack return, host timer, or recheck after the wait.
This equivalent continuation goes directly to the firing preparation instead
of storing Japan's relocated branch instruction. The room snapshot keeps
threshold and strategy independent, including when settings change mid-wait.
Five-ROM instruction signatures and1,944 mixed-source/slot/CPU cases cover
the two prefixes and decision outputs;32 invalid-owner cases reject children,
wrong rooms, widths and shifted slots.

Controlled recompilation runs then exercise the full native dispatcher for
12,500 ticks. After Continue and an ordinary Palace/Fight entry, a debug room
request loads Kasandora2; a held, invulnerable player supplies a fixed distant
target. The US control produces60 volleys at exactly48-update intervals;
the Japanese rule produces39 at exactly73. Both exit normally with positive
HP, unchanged lives and byte-identical private SRAM. A first implementation
returned from the HLE without consuming the native dispatch return; this live
test caught that error and the final path uses `$C682` as described above.
The forced room transition also produces background-preflight mismatches in
both controls; these runs validate encounter timing, not stage traversal,
terrain/render parity, a complete victory or the near-player phase sequence.

### Aitos bamboo spike traps

The previously unidentified Aitos Act-1 type `$06` is the falling bamboo
spike trap, not volcanic lava. Raw room `$0104` uses source US `$CF2E` / JP
`$CFB3`, with entry code `$CF3A–CF5F` / `$CFBF–CFDF`. Its record has HP 0,
attack 2 in both normal and Special mode, victim filter `$20`, and initial
state 38. Both releases place traps at authored tiles `(25,35)` and `(35,36)`;
JP adds `(28,33)`. These are world `(400,560)`, `(560,576)` and `(448,528)`
before subtracting the initial bottom extent 32 from the live Y hot point.

Both handlers require outside-activation flag `$0400` clear and absolute
horizontal distance below 32. Equality at 32 fails; this handler has no Y test.
Once triggered, it plays states 37 and 38, then restores the source entry.

| Sequence | US frames | JP frames |
| --- | ---: | ---: |
| State 37 | 27 | 43 |
| State 38 | 192 | 176 |
| Combined activation | 219 | 219 |

The flattened movement rows are identical: fall 1 px/update for 4 frames,
2 for 4, 4 for 7, then 6 for 12 (112 pixels total); pause 16; rise 1 for 112;
rest 64. Japan assigns the bottom pause to the end of state 37, while US puts
it at the start of state 38. The state split is **not a movement-speed change**.

Four booted traces verify the first shared trap from activation to source
entry restoration: 220 matching position/velocity/extent/facing/visual
samples spanning 219 updates, in both regions/modes. The trap starts and
ends at hot-point Y528 and reaches Y640. Native cases test signed proximity
boundaries and the outside-activation gate, plus every owned animation row
in both facings. The shared `$4000` blobs at US file `0xC4C17` / JP `0xC1B1C`
contain other actors; their unrelated changed states 10/45 are not evidence
of trap differences. The trap's visual-41 composition matches.

US `$CF4C` issues audio command `$23`; this JP entry has no equivalent
command. That is a code-level audio difference, not a listening test.
The regional gameplay choice here is **placement**, frozen at room entry;
do not add/remove live traps mid-room or invent a trap-speed option. The
broader reported lava slowdown remains unassigned to a confirmed mechanism.

**Batch evidence and limits.** The first-act/trap batch adds 28 booted
fixtures, 214 isolated native cases and 12 bounded Go disassembly windows.
The isolated suite covers 24 spawn/stat cases, 16 Antlion trigger cases,
16 post-volley decisions, 32 trap gates, 88 child-allocation cases and 38
native animation-row decodes. Repeat executions are not additional cases.
An independent complete rerun reproduces all 665 evidence files byte-for-byte.

Booted fixtures use each region's own accepted scratch state, native pending
room loads, explicit one-time player/camera positioning, and HP/max 24 before
observation. Four dragon traces run 500 frames; the other 24 run 900. All
accepted preparations and observations retain positive HP and unchanged
lives; observations stay in the intended room. No actor phase or RNG fields
are edited during observation. Large camera warps can leave stale background
strips, so these captures validate actor behavior, not full presentation or
natural traversal. Failed exploratory setups and overlong traces ending in
death are excluded, not silently treated as parity evidence.

### Authored pickup differences

The host's numerical placement catalogue now retains the ordered programs for
all 49 action roots in US, Japan, European Story and European Action modes.
All three European releases agree. Enemy/wave and pickup sources can be
resolved independently, and European enemy markers are filtered before any
randomization. A shared room-local row ID preserves identity through those
transforms. Tests compare every pure program with the ROM streams, following
native jumps, and validate all 2,646 mixed source/mode/difficulty combinations.
The runtime captures these choices at full room initialization. Preparation
after the asset VM at `$00:8329` resolves all act programs for the existing
randomizer and publishes only the selected room. There is no per-frame
placement scan, donor-code execution or mutation of the ROM.

The initial adapter replaces `$00:941C–946D`, calls the original `$9557`
initializer for each numerical object and resumes the native `$946E`
sentinel/return epilogue. The later-wave adapter enters at `$9500`, after the
original `$94F2` prologue, and resumes `$954D` for sentinel/stack restoration.
Calls use real native JSR return identities `$9461/$9540`. Gate records keep
the actual US after-`$FE` cursor at slot `+$38`, not a foreign or invented
pointer. Wave allocation preserves the native gate/player slots and checks
capacity before writes. Fillmore's retry height follows the terrain choice.
US/US delegates to the original loader. Unit coverage compares the complete
memory footprint, guards and escaped returns for all 2,646 combinations;
controlled live room/wave checks are not proof of natural traversal or every
deliberately mixed terrain route.

The randomizer exposes a value-only program transform alongside its existing
ROM adapter. Both share the same permutation code, keep statues within their
wave, and leave program IDs, wave gates and reservations alone. Type tables
also contain direct code entries: parameter `$FF` controllers must not be
shuffled as ordinary enemies, nor may their instruction bytes be scaled as
HP/attack. Spawn definitions are checked before either operation, and aliases
of one definition are scaled once. Correcting those older randomizer bugs
changes affected seed results; non-randomized native data remains untouched.

All 49 shared action-layout roots were joined to their owning acts, following
placement-stream jumps and retaining order, reservations and wave boundaries.
Eighteen roots change item placements. In the tables below, a raw key is
`room << 8 | region`, **not** `region/act`; coordinates are authored 16-pixel
tiles, before the live object's bottom-extent adjustment. Act 2 begins at raw
room 2/2/3/4/4/5 for Fillmore/Bloodpool/Kasandora/Aitos/Marahna/Northwall.

| Raw key | Area / act | Change from Japan to US |
| --- | --- | --- |
| `$0101` | Fillmore 1 | Add 500 points at `(44,36)`; half→whole apple at `(150,18)`; 1,000 points→1UP at `(169,36)` |
| `$0201` | Fillmore 2 | Half→whole apples at `(79,29)` and `(60,73)`; magic→1,000 points at `(115,60)` |
| `$0102` | Bloodpool 1 | Add whole apple at `(195,20)` |
| `$0702` | Bloodpool 2 | Half→whole apple at `(3,13)` |
| `$0103` | Kasandora 1 | Half→whole apple at `(183,38)` |
| `$0203` | Kasandora 1 | Add whole apple at `(146,33)`; move 1UP from `(75,42)` to `(75,41)` |
| `$0503` | Kasandora 2 | Half→whole apple at `(39,20)` |
| `$0104` | Aitos 1 | Half→whole apple at `(129,60)` |
| `$0105` | Marahna 1 | Half→whole apple at `(85,16)` |
| `$0205` | Marahna 1 | Half→whole apple at `(33,27)` |
| `$0505` | Marahna 2 | Magic→1UP at `(43,11)` |
| `$0605` | Marahna 2 | Half→whole apple at `(47,56)`; add whole apple at `(65,104)` |
| `$0705` | Marahna 2 | Add the same `(65,104)` whole apple in the alternate entry stream for the shared map; not a second physical location |
| `$0106` | Northwall 1 | Move magic from `(109,57)` to `(108,57)` |
| `$0206` | Northwall 1 | Half→whole apple at `(27,37)`; move 1UP from `(68,47)` to `(92,47)` |
| `$0306` | Northwall 1 | Half→whole apple at `(23,11)` |
| `$0506` | Northwall 2 | Magic→1UP at `(3,5)` |
| `$0706` | Northwall 2 | Half→whole apple at `(40,45)` |

These are **normal-mode source items**, not necessarily the items presented
in Special mode. The shared statue initializer (US `$00:95F0`, JP `$00:9618`)
rewrites item 0, magic, to item 2, screen clear; it rewrites item 3, sword
power, to item 1, extra life. The other IDs are unchanged. Thus a JP magic
versus US 1UP placement becomes screen-clear versus 1UP in Special mode.
Thirty-two native initialization fragments cover all eight IDs in both
regions/modes. Twenty-four pickup-dispatch cases cover IDs 0/1/4/5/6/7;
the apple recovery rules remain shared, not another regional potency option.

**Score units.** Item 6 adds BCD `$0100` and item 7 adds `$0050` to `$1F`.
These mean **1,000 and 500 displayed points**, respectively. Stored score is
in tens: the native HUD formats four BCD digits and retains a fifth, trailing
zero from its template. Ten native formatter fragments, initialized from the
live HUD field `$7F:B074–B07D`, verify stored 0/20/50/100/1000 as displayed
0/200/500/1000/10000 in both ROMs. Relevant windows are US
`$02:C2E8–C33F` / JP `$04:9223–927A`, with fill helper US `$02:C375–C385` /
JP `$04:92B0–92C0`. The stored cap `$9999` represents 99,990 displayed points.
The [score-to-town formulas](#lair-stock-is-not-monotonic) and actor records
use stored units; do not insert displayed values into those formulas.

This closes the authored item-stream comparison for the shared roots, not
every procedural drop, route traversal or pickup interaction. Preserve item
identity and collected state across toggles: apply the selected stream at
room entry, never respawn already collected items by rebuilding a live room.
Geometry, placement, wave gates and checkpoint payloads need compatibility
checks before offering independently mixed layouts.

### Terrain and damage-box contracts

**Implemented hazard seam.** `regional_hazards` owns numerical ordered lists
for all 49 shared rooms; `actraiser_stage_hazards` owns the native-memory
adapter. European English, German and French lists match in both Story and
Action mode. Twenty rooms differ from US in each non-US profile. Selection
is captured with the other action rules at complete room initialization.
At `$00:940C`, after native `$93A9` has expanded the US stream, the conditional
prefix validates its count, every record, X and source terminator. It replaces
only the selected records and scratch count, reproduces `LDA $00`, then tails
to `$940E`. Native `STA $1AE2 / INY / PLX / RTS` retains the US placement
cursor and real caller frame. US and identical lists delegate unchanged.
No ROM mutation, donor graphics, terrain replacement or new per-frame work
is involved. The later native contact pass, including overlap order, slowing
flags and hit/invulnerability gates, is unchanged. Complete terrain/placement
bundles remain separate implementation work.

Tests independently decode all five ROMs and both European modes; compare
the adapter's complete memory footprint, flags/registers and tail target;
reject malformed context and unknown resident records; and exercise pending
selection, room activation, v1–57 migration and nine replay-identity pairs.

**Implemented terrain projection.** `regional_terrain` owns numerical map
deltas and European solidity-bit changes. `action_room_terrain` adapts the
immutable US room scene; `actraiser_stage_terrain` adapts native WRAM through
the same two-plane contract. Neither portable policy nor scene loader reads
settings or owns CPU memory. Scene-loading tools and reference skybox artwork
continue to load the unmodified US reference unless explicitly projected.

The `$00:8329→832C` prefix runs after the complete asset VM, before actors,
`$02:BAC1` attribute generation and native tilemap staging. Full-plane FNV
signatures and dimensions recognize US and previously projected data before
any writes. Map and definition generations are validated independently because
native room scripts can reload only one plane. Repeated projection is
idempotent; switching to US restores the original data. The operation is
allocation-free and room-load-only. The host renderer receives the accepted
profile and invalidates its cached room, not a pointer into mutable settings.

All 898 changed JP map cells across 11 roots can be expressed through exact
matches in the corresponding US metatile tables. Eight of those roots have
348 changed collision cells; the other changes are decorative arrangement.
CHR and palette data match in all 11. No Japanese graphics are needed for
these layouts. The two Death Heim maps' 26 changed characters remain a separate
artwork feature, not part of terrain projection.

Fillmore's `$933C→9343` prefix substitutes the entry Y27 for US Y34; the
`$94B1→94B7` prefix substitutes checkpoint Y25 for Y23 while leaving trigger
coordinates and the US stream cursor intact. At room/retry load, only an
active cached `(156,23)` or `(156,25)` checkpoint is normalized to the selected
layout. Arbitrary debug coordinates are not rewritten. All European player
start records match US in both Story and Action tables. Wave ownership and
enemy/pickup stream replacement remain separate placement work.

**European terrain is not US-identical.** All three PAL BG1 map/definition
sets agree. Seventeen map IDs differ in two roots: `$0304` Aitos (12 cells,
four collision changes) and `$0406` Northwall (five cells, five collision
changes). Other changed definitions differ only in word bit `$0200`, with
identical character data. The native attribute builder is byte-identical:
US `$02:BAC1`, EU `$02:C0B9`, DE `$02:C0C2`, FR `$02:C0AB`. These flags therefore
have the same quadrant-solidity meaning, not a PAL encoding difference.

| European raw roots | Collision cells differing from US, per root |
| --- | --- |
| `$0201`, `$0301` | 74, 18 |
| `$0302`, `$0402`, `$0502`, `$0702` | 11, 12, 20, 20 |
| `$0403`, `$0503` | 20, 12 |
| `$0304` | 4 |
| `$0505`, `$0605`, `$0705` | 13, 36, 36 |
| `$0106`, `$0206`, `$0306`, `$0406` | 30, 17, 14, 5 |

This is 342 root-relative collision cells; alternate entry roots that share
a map are counted separately. Nine belong to rearranged map cells and 333
to definition changes. Some other rooms retain changed but unused definitions.
Neither difficulty nor PAL refresh timing changes these authored layouts.

`actraiser_stage_terrain_test` compares all 49 roots against all five ROMs,
including every combination of incoming map/definition profile and outgoing
profile, full CPU/RAM footprints, reversal, repeat application and malformed
data refusal. ROM-free tests cover anchor continuations; session/runtime tests
cover pending choices, v1–58 migration and nine replay identities. These checks
do not establish safe natural traversal for arbitrary mixed placements.
Controlled native loading of all 49 rooms under each profile also matches
every map byte, metatile definition and collision attribute, with zero
background-world/tile mismatches. The frame oracle reports separate BG2
vertical-scroll/screen-selection differences during this warped traversal;
that run is not evidence of full-frame or natural-play parity.

**The native terrain lookup matches regionally.** US `$00:91C3–920E` / JP
`$00:91CF–921A` reads the chunked metatile map at `$7E:8000` and the 256-byte
quadrant-attribute table at `$7E:05A0`. Width/height in tiles are `$84/$86`;
the chunk-column count is byte `$2F`. The caller supplies tile X/Y at
`$14/$16`. X outside width returns `$0F`; Y outside height returns zero.
All 348 differing cell coordinates were checked through the native routine
in both ROMs, plus 32 out-of-bounds controls. Other 41 shared roots have
matching decoded collision grids; they were not all boot-tested here.

| Raw key | Changed cells | Confirmed geometry / adjoining content |
| --- | ---: | --- |
| `$0101` Fillmore 1 | 244 | Only changed player start: JP `(5,27)`→US `(5,34)`, 112 px lower. Wave trigger remains `(155,0)`, but its respawn changes `(156,25)`→`(156,23)`, 32 px higher. US adds solid cells at `x154..162,y23..24`; other changes extend beyond that platform |
| `$0102` Bloodpool 1 | 4 | JP upper-half cells `(249,23)/(250,23)` become empty; US adds upper-half cells `(250,26)/(248,29)` |
| `$0106` Northwall 1 | 6 | US adds solid `x106..107,y57..59`, widening the first magic-pickup platform |
| `$0203` Kasandora 1 | 33 | Includes the 1UP support platform at `x74..75` moving from row42 to41, alongside the pickup; other platforms change too |
| `$0306` Northwall 1 | 16 | US adds solid `x80..87,y16..17` |
| `$0403` Kasandora 2 | 16 | US adds solid `x38..40,y43..46` and removes solid `x41..44,y47`; a nearby damage strip starts at x51 instead of38 |
| `$0502` Bloodpool 2 | 16 | US adds solid `x4..11,y48..49`; the corresponding JP 24-HP box is absent |
| `$0505` Marahna 2 | 13 | US fills lower halves at `x42..45,y23` and adds solid `x41..43,y24..26`. Same room changes magic→1UP and removes the type `$10` hovering/splitting fireball at `(39,10)`; see the [family comparison](#marahna-splitting-fireballs) |

Sixteen booted room-load fixtures reproduce the **complete resident maps,
all 256 attributes and expanded damage-box records** for these eight roots,
not just the changed cells. They use each region's own native pending-load
sequence through preceding rooms. This is load/ownership proof, not natural
traversal or evidence that arbitrary mixed terrain/placement bundles are safe.

**Damage boxes are separate from terrain solidity and enemy attack stats.**
US `$00:93A9–941B` / JP `$00:93D1–9443` expands each five-byte source record
`[x0,x1,y0,y1,damageOrFlag]` into five 16-bit words. Count is `$7E:1AE2`;
records start at `$7E:1AE4`, stride ten bytes:

| Offset | Expanded field |
| --- | --- |
| `+0` | Left = `16*x0 - 4`, modulo 65536 |
| `+2` | Width = `16*(x1-x0) + 24` |
| `+4` | Top = `16*y0 - 16`, modulo 65536 |
| `+6` | Height = `16*(y1-y0) + 48` |
| `+8` | Damage/flag byte, zero-extended |

All 98 expansion fixtures (49 roots × two ROMs) verify every word and the
returned stream cursor. These padded rectangles must not be replaced with
literal tile bounds. Contact, US `$00:8C12–8C97` / JP `$00:8C01–8C86`, uses
the selected player at pointer `$8A` and its **hot-point coordinates**:

- Skip the entire pass if US `$ED` / JP `$F0` is nonzero, the player's
  `+$30` intersects `$2058`, or the box count is zero.
- If player top extent `+$0C < 25`, reduce the effective height by 16.
  Extent25 and above keeps the full height.
- Test unsigned 16-bit `(x-left) < width` and `(y-top) < effectiveHeight`.
  Left/top are inclusive; right/bottom exclusive.
- For a source byte with bit `$80`, OR `$8000` into player flags without
  subtracting HP. The animation decoder consumes this as a horizontal
  slowdown request; see [the downstream contract](#shared-player-slowing-zones).
- Otherwise subtract the damage byte from HP, clamp a negative result to
  zero, and set hit flag `$0008`. There is **no Special-mode attack promotion**.
- Continue through every box in stream order. The hit flag just set does
  not stop overlapping boxes within this invocation: 1+2 and 2+1 both
  remove 3 HP; `$80`+1 and 1+`$80` both set `$8000` and remove 1 HP.

Consequently, a 24-HP trap can kill at the native health cap, but it is not
an unconditional death instruction: the hit/invulnerability gates still
apply. Do not reuse the enemy-contact policy or a first-hit-only collision API.

Twenty roots have changed damage-box streams. The following census describes
**ordinary HP damage**, omitting shared `$80` flag boxes. `N × D` means N
boxes with damage D, not N separate traps or damage per second. Geometry and
overlap matter as well as counts.

| Raw key(s) | US | Japan |
| --- | --- | --- |
| `$0101` | 5 × 1 | 4 × 2 |
| `$0103` | 8 × 1 | 9 × 1 |
| `$0106`, `$0302`, `$0402` | 4 × 1 each | 4 × 24 each |
| `$0201` | 14 × 1 | 14 × 24 |
| `$0206` | 5 × 1 | 6 × 24 |
| `$0301` | 6 × 1 | 4 × 24 |
| `$0306` | 6 × 1, 2 × 24 | 8 × 24 |
| `$0403`, `$0702` | 2 × 1 each | 2 × 24 each |
| `$0502` | 1 × 1, 6 × 24 | 8 × 24 |
| `$0503` | 1 × 1, 1 × 24 | 2 × 24 |
| `$0505`, `$0607`, `$0805` | 1 × 1 each | 1 × 24 each |
| `$0605`, `$0705` | 6 × 1, 1 × 24 each | 7 × 24 each |
| `$0606` | 11 × 1 | 11 × 2 |
| `$0706` | 7 × 1 | 8 × 2 |

Notable geometric removals include Kasandora1 `x105..106,y38..39`, Northwall1
`x69..95,y45..46`, and Northwall2 `x36..37,y42..43`. In Kasandora2 `$0403`,
the strip at `y45..46` changes from JP `x38..76`, damage24, to US `x51..76`,
damage1. Fillmore boxes are rearranged/split, so a simple damage multiplier
cannot reproduce the complete difference. Among these changed streams, the
unchanged `$80` boxes number seven in `$0201`, two in `$0206`, and one in
`$0502`. The complete inventory also includes one in unchanged root `$0406`.

Freeze the ordered box stream and collision/layout profile at room entry.
Preserve current hit/invulnerability state when applying other settings;
never swap the floor under an active player. Native contact coverage adds
256 damage/gate cases, 80 geometric-edge/extent cases and 16 overlap cases,
all spanning both regions and normal/Special mode.

### Aitos molten-rock launches

Type `$05` in raw room `$0104` is distinct from bamboo type `$06` and rising
fireball type `$08`. Source US `$00:CEEC` / JP `$00:CF71`, entry
`$00:CEF8–CF2D` / `$00:CF7D–CFB2`. All six authored positions match:
`(104,64)`, `(105,64)`, `(119,65)`, `(120,65)`, `(135,64)`, `(136,64)`.
The source record matches: initial state39, HP0, attack1 (Special2), stored
score1. Reviewed entry-program differences are helper relocations only.

The gate is absolute horizontal distance below 128, with no `$0400` test at
this entry. Equality 128 fails. Sixteen native boundary cases cover
±127/±128 in both regions/modes.

The random byte determines facing and delay through two different branches:

- Odd values preserve facing and set counter `((rng >> 1) & 63) + 1`.
- Even values call the horizontal-flip helper, US `$00:871E` / JP `$00:870D`.
  It **overwrites A with the attribute word**. The subsequent mask therefore
  uses attributes, not the shifted random value. With this family's authored
  zero low bits, the delay counter is always 1 on the flip branch.

Shared delay helper US `$00:86FA` / JP `$00:86E9` counts inclusively: the odd
branch waits 2–65 active updates and the even branch waits 2. Do not implement
a uniformly random wait independent of facing. Native fixtures cover every
random-byte result in both ROMs plus reversed initial-facing boundary cases;
the RNG routine itself still runs, using a controlled input state rather than
a patched return. Existing booted traces also verify flipped counter1→0→flight
in both regions/modes. This is a shared behavior, not a regional difference.

Owned animation states39/40 and visual compositions42/43 match in the shared
`$4000` asset (US file `0xC4C17`, JP `0xC1B1C`). State39 has eleven eight-frame
rows: 88 updates, net movement `(-152,-128)` unflipped. State40 moves `(-2,+8)`
per update in two-frame rows and repeats until the outside-window flag `$0400`
is observed at its sequence boundary. The actor then reinitializes **its own
slot** from the retained source/authored position; this launch is not a newly
allocated child actor.

Four booted normal/Special fixtures observe 500 frames without actor/RNG
edits. The first two rocks each reproduce the same 105 phase-aligned samples
in both ROMs/modes, including position, velocity, state, row, visual, timer,
facing and extents. The first flight reaches state40 after88 updates and
restores its entry after104. This 104-update duration is fixture-specific:
later flipped flights meet the visibility gate at different times. Absolute
launch frames differ because the preceding regional scene leaves a different
RNG state, not because the rock's delay or motion rule differs. Forty-eight
native animation-row cases cover both facings.

No molten-rock speed policy is justified by this evidence. The broader
reported Aitos lava slowdown still needs another mechanism; do not reopen
the matched type05/06/08 paths as if they were unexamined.

**Layout/rock batch evidence and limits.** The main suite adds 1,180 isolated
cases: 98 expansions, 728 terrain lookups (696 changed-cell checks plus32
bounds), 256 damage/gate, 80 edges, 16 overlaps, 24 pickups, 32 statue rewrites,
16 rock gates, 48 animation rows and 10 score formatters. Twenty booted
fixtures comprise 16 map loads and four rock observations. Sixteen bounded
Go windows retain exact instruction counts and endpoints; five paired
routines have all byte differences classified. An independent complete rerun
reproduces all156 evidence files byte-for-byte.

The final helper review adds 520 isolated rock-delay cases and six bounded
Go windows: all 256 random-byte results in both ROMs, plus four results with
reversed initial facing per ROM. Sixteen flip-branch countdown observations
come from the existing four booted traces, not new fixtures. The combined
batch therefore adds **1,700 isolated cases and 20 booted fixtures**.
The follow-up's independent rerun reproduces all four additional evidence
files byte-for-byte, bringing the combined repeated artifact count to 160.

Boot loads use 600 frames per requested preceding room, restoring HP/max24
before each request. Rock observation follows a 600-frame native load, with
one-time player `(1600,880)` and camera `(1472,752)` positioning and HP/max24.
All accepted preparations/observations retain positive HP and unchanged
lives; rocks stay in Aitos room1 throughout. The rejected lower player setup
fell below the map and lost a life despite initially retaining full HP; it
is not evidence of regional behavior. Full route traversal, procedural drops,
mixed-profile collision and complete death/respawn behavior remain distinct
validation/investigation tasks. The next batch resolves the `$80` consumer.

### Shared player slowing zones

The eleven `$80` boxes in raw roots `$0201`, `$0502`, `$0206` and `$0406`
match between US and Japan. They request **player horizontal slowdown**, not
128 damage or an unconditional death. The contact routine sets actor flag
`+$30 & $8000`; the actual consumer is the animation decoder, US
`$00:8E2F–8F13` / JP `$00:8E3B–8F1F`.

After decoding a valid animation row, the routine checks that X equals the
selected player pointer `$8A`. At US `$8EDE` / JP `$8EEA`, a set high flag is
cleared and the **newly decoded signed X velocity** at `+$06` is halved.
The signed-half helper is US `$00:84EC–84F2` / JP `$00:84E6–84EC`. Rounding
is toward negative infinity: `-3→-2`, `-1→-1`, `1→0`, `3→1`. Vertical
velocity, duration and extents retain their ordinary decoded values.

This is neither repeated division of the old velocity every frame nor a
promise that every player controller's final displacement is halved. The
`$FF` animation terminator resets the row index and returns carry without
consuming the flag. Non-player actors also bypass this consumer. When the
flag is absent, US `$F2` / JP `$F5` equal to 1 instead adds actor `+$3E` to
the decoded X velocity; slowdown takes precedence over that adjustment.

Native tests exercise all sixteen owned Marahna animation rows with both
horizontal facings, flag clear/set and environment values 0/1/2: 384 cases
across both ROMs. Another four cover terminators, two cover non-player
exclusion and eighteen cover signed-half boundaries. Every authored `$80`
box is tested through native contact and then the native animation consumer:
44 invocations across eleven boxes and two ROMs. ROM animation data supplies
the test rows; these are controlled routine fixtures, not full player
traversals of the zones.

No regional slowdown option is justified. Preserve this shared producer/
consumer contract if terrain or player-animation logic is later lifted.

### Fillmore Act-2 wall-emitter cadence

Type `$01/19`, source US `$00:B3BF` / JP `$00:B453`, is the wall fireball
emitter. Its complete entry/child program is US `$00:B3CB–B448` / JP
`$00:B45F–B4DC`. All twelve raw `$0301` placements match:
`(52,26)`, `(61,24)`, `(61,41)`, `(52,44)`, `(52,50)`, `(61,53)`,
`(61,65)`, `(52,67)`, `(52,84)`, `(61,86)`, `(52,92)`, `(61,94)`.
The source record also matches: state 36, flags `$0030`, HP 1, attack 1,
score 0. Shared Special initialization promotes HP and attack to 2.

Entry subtracts eight pixels from X once, then yields. The active gate at
US `$B3D8` / JP `$B46C` waits while flag `$0400` is set. Otherwise it faces
the player and waits through state 36 before allocating a shot:

| Behavior | US | Japan |
| --- | --- | --- |
| Wait call | `$B3E4`: state 36, repeat twice via `$8669` | `$B478`: state 36 once via `$8646` |
| One state-36 cycle | Two 90-update rows | Same |
| Repeated successful firing interval | 360 active frames | 180 active frames |
| First observed shot after controlled positioning | Frame 362 | Frame 182 |

The host implements cadence and launch offset as independent, room-stable
policies. `$B3CB→B3D5` substitutes only the PAL coordinate arithmetic, with
matching NZCV flags. `$B3E4→B3E7` selects one repetition for JP/PAL; generated
code still owns the repeat-helper call frame, activation, allocation and child
continuations. The US two-repetition path remains the native default.

PAL state36 is one stationary255-update row. Both US rows have the identical
visual37, zero movement and the same collision/composition record. The host
therefore uses one127+128-update cycle of those US rows, adjusting only their
fresh delays through `$8E2F`. This reproduces the255-update hold without a donor
asset or a virtual animation stream. It does not claim identical internal row
indices to PAL. Five-ROM tests verify the totals and stationary composition;
all65536 X inputs verify the position prefix's arithmetic and flags. Existing
room snapshots and live projectiles are never rewritten by a settings edit.

Owned states 10/11/36 and their composition records match. Apart from the
repeat argument/helper choice, all reviewed program differences are
relocations. The first-shot timestamps include this fixture's initial
activation delay; the repeating interval does not.

Allocation uses the global pool, US `$8538` / JP `$8527`. A full pool skips
that shot and returns to the gate; it does not retry immediately. A successful
child inherits the source, attack, facing and position, with HP/score cleared
and backlink `+$3A` set to the emitter. Its entry clears status/flags, runs
state 10 horizontally at one pixel per update, reverses on a wall, and uses
state 11 to fall at one pixel per update when unsupported. Landing resumes
state 10. An outside-window check at a row boundary retires the child.

Eight booted fixtures cover normal/Special and both ROMs, with two setups.
Four allow native player movement after a one-time reposition and verify the
first shots. Four hold only the player stationary/invulnerable, maintaining
the activation window for 900 native frames. Two emitters fire at frames
362/722 in US and 182/362/542/722 in Japan. Their first children match through
all 345 and 297 live phase-aligned samples respectively, including movement,
animation, bounds, facing and retirement; Special changes attack, not motion.
Eight native gate and sixteen full/free-pool allocation cases supplement
the boot traces. No enemy, RNG, terrain or per-frame state is edited.

This confirms a cadence difference, not the reported reduction in active
statue placements: the twelve authored emitters are present in both ROMs.
The complete decorative-background-to-emitter artwork join remains separate.
Implement a named emitter-cadence policy with US default, captured at room
initialization. Defer live changes until the next room; preserve an existing
wait, child lifetime and allocation failure. Do not couple this to statue art.

### Marahna splitting fireballs

Type `$05/10`, source US `$00:E047` / JP `$00:E0DD`, is a hovering orb which
splits into four cardinal shots. Its entry/helper span is US `$00:E053–E0B9`
/ JP `$00:E0E9–E14A`; children use US `$00:A655–A669` / JP `$00:A614–A628`.
Source data matches: animation `$7E:4000`, state 12, flags `$0020`, HP 0,
attack 2 and score 0. Special does not increase that attack value.

The following authored entries match: `$0405 (8,53)`; `$0605 (45,47),
(43,54)`; `$0705 (134,20),(135,29),(132,35),(111,51),(100,52)`.
Japan additionally has `$0505 (39,10)`, absent in US. That is the room whose
magic pickup becomes a US 1UP and whose floor is expanded. This is a
**placement difference**, not proof that the whole family was removed.

Both programs use the same sequence:

1. Wait for the outside-window flag `$0400` to clear, then play state 12.
   Eight six-update rows move X by `0,-1,-2,-1,0,+1,+2,+1` per update,
   with no Y movement and zero net displacement over the 48-update cycle.
2. Test absolute player distance on both axes. Each must be **below 80**;
   equality at 80 fails and restarts the idle cycle.
3. Play state 13 eight times: two two-update rows per cycle, 32 updates total.
4. Attempt four allocations in the same update: state 15 down, state 16 left,
   V-flipped state 15 up, H-flipped state 16 right. Unflipped speeds are
   `(0,+3)` and `(-3,0)`; each child retains the parent's source/backlink.
5. Play the ten-update state-14 burst, then retire the root. The children
   continue independently until their outside-window checks retire them.

The allocator searches after the parent, US `$853D` / JP `$852C`. This
family's wrapper does **not** inspect allocation carry. With 0–3 free slots,
only that prefix of the four shots becomes live; failed attempts write the
non-actor scratch target `$1AA2`. Later attempts still run, with no retry
queue or replacement of occupied actors. Native fixtures cover 0–4 holes,
both initial horizontal facings and both modes/ROMs. Preserve the observable
partial burst if lifted, but keep native scratch conventions behind an ABI.

All owned state 12–16 rows/composition records and the reviewed mechanics
match after relocation. US additionally issues audio command `$21` at
`$E078` before the split; the JP counterpart omits that call. Its audible
effect is not established here and is not a movement-policy difference.
Sprite pixels and palettes remain a separate donor-media comparison. Some
composition pointers relocate inside the resident blob: live pointers are
validated against each ROM's own table before comparing part contents.

Four booted normal/Special fixtures use the actual raw `$0405` load. A
one-time player-only logic hold and invulnerability keep a stationary target
at `(160,816)` in the open shaft; enemies, map and RNG remain native.
All 241 observed frames match in family membership and phase-aligned motion
fields. Charge starts at frame 50, all four children appear at 82, and the root
retires at 92. Children move in the four expected directions on 83. Another
100 native distance-boundary and 40 allocation cases cover the branch edges.
These are controlled scene observations, not unassisted route playthroughs
or a boot test of the extra `$0505` placement itself.

Implementation: include the extra orb in the room placement profile, frozen
on load with the adjoining collision/pickup bundle. Do not invent a separate
JP orb-speed/charge setting or restart live actors when a setting changes.

**Emitter/slow-zone/orb batch evidence.** Adds 616 isolated cases and 12 booted
fixtures. Sixteen exact-endpoint Go windows cover eight paired programs;
all differences are classified, including the emitter repeat and US-only
audio call. The complete independent rerun reproduces all 305 evidence files
byte-for-byte. Native routine tests retain ROM-derived rows and bounded
input/output records; boot preparations retain positive HP, unchanged lives
and the intended room. These counts are test fixtures, not unique mechanics
or evidence of exhaustive game coverage.

### Ordinary enemy movement and attack recovery

Five further US/JP families have matching local instruction structure after
known helper/internal-pointer relocations and the regional sound trap are
accounted for. Their movement and timing changes instead reside in loaded
animation rows. This is local program comparison, not blanket callee parity.
Source addresses below identify the 12-byte actor records; entry is source+12.
State numbers in this table are decimal.

| Family / native region,type | Source US / JP (bank `$00`) | Verified US → JP behavior |
| --- | --- | --- |
| Fillmore bird `$01/03` | `$AA9A / $AB2E` | State 21 horizontal magnitude 3→4 px/update |
| Fillmore leaping enemy `$01/09` | `$AC8E / $AD22` | Last moving row of state 30 and states 31–33: magnitude 2→3; earlier launch rows, vertical movement and durations match |
| Fillmore cave enemy `$01/0F` | `$B041 / $B0D5` | State 31 recovery 60→40 active frames |
| Fillmore cave enemy `$01/0E` | `$B0B4 / $B148` | State 39 recovery 58→42; state 41 recovery 64→48 |
| Marahna hooded caster `$05/0C` | `$DCDB / $DD71` | State 16 wind-up 56→28; state 19 wind-up 68→40; following state 17/18 recovery remains 41 |

The cave labels intentionally retain type IDs: no claim that type `$0E` is
the separate skeletal creature visible in the same room. Changed rows retain
their collision extents. Bird rows have stored duration 1 in both ROMs;
the cave final-row stored durations are 59→39 and 47→31, respectively.
Both caster wind-ups change their initial stored duration from 47 to 19.
The animation consumer's update/transition convention, not the raw byte alone,
determines the phase lengths above.

Twenty-eight booted fixtures cover both modes/regions and seven observations:
bird, leaping enemy, both cave types (two distance branches for `$0E`) and
both caster branches. Every native room is loaded in sequence from the
same-region action predecessor; its resident animation blob is checked.
A one-time player-only logic hold/invulnerability and position setup supplies
a stationary target. Enemies, RNG, map and subsequent updates remain native.
Each fixture observes 900 frames, preserves positive HP and lives, and leaves
SRAM unchanged. These are controlled encounters, not natural combat runs.

Bird flight exits after 64 US / 48 JP frames in this particular camera/target
setup; that is geometry-dependent, not a fixed cooldown. The leaping enemy
has different authored placements, so the fixtures use the same 56-pixel
relative target offset, not a claimed common spawn. Its measured states
30/31/32 last 72/24/24 frames in both ROMs. Caster repeat intervals with these
stationary targets are 97→69 and 109→81 frames. Normal/Special timings match
for every tested branch; damage changes are separate.

Another 156 isolated native animation cases cover all 39 reviewed rows,
both regions and both horizontal facings, checking visual selection, stored
duration, signed X/Y velocity and collision extents. Together with the two
routing checks below, this batch adds 158 isolated cases and 28 booted traces.
Fourteen exact-endpoint Go disassembly windows support the comparisons;
an independent repeat reproduces all 761 evidence files byte-for-byte.

Implementation: expose named family movement/wind-up/recovery values, captured
on room or actor-generation initialization. Do not swap whole animation blobs
merely to change artwork: those blobs also carry gameplay data. Preserve live
phase progress and descendants; a setting change must not restart an attack.

The host implements these seven speed/delay leaves, plus the two Bloodpool
swordsman delays described below, with a complete-room
snapshot. The accepted `$02:B4E8` profile boundary atomically captures room
time and motion; a failed validation publishes neither. This also covers
later spawns without maintaining a parallel lifetime table for action actors.
Changing a setting during a room leaves its existing snapshot unchanged.

At `$00:8E2F`, the adapter checks the native source identity, map group,
animation bank/base and exact row. It delegates the original reader, then
changes only actor `+$06` and `+$24` on a normal return. It never patches
shared animation bytes, graphics, collision extents, vertical movement or
CPU/stack state. Unknown shapes and the native-US snapshot stay native;
escape tokens propagate without postprocessing. No donor media is needed.

All 53 reviewed rows are compared with the five actual ROMs: 265 numerical,
vertical-motion, visual-ordinal and collision-extent checks. For this subset
all three PAL releases use the US values. Controller tests exercise those
53 rows across 512 mixed snapshots, four facing combinations and both M
widths, plus malformed ownership and return-token cases. The reader itself
is delegated in production; the unit stand-in is not an independent native
CPU proof. Existing original-CPU row evidence above supplies that reference.
The unchanged US row shape is intentional: future artwork integration must
preserve the independently selected motion metadata.

Controlled host saves additionally enter Fillmore Act 1 through the Palace's
ordinary Fight/confirmation flow. The JP motion capture observes a native
bird row acquire DX4; the US control bypasses the adapter. Both 11,500-tick
captures preserve positive HP and lives. Cold replays in the production
binary accept their initial identities, exit at the recording end, and match
their respective final WRAM/SRAM byte-for-byte. This is live bird/room-lifecycle
evidence, not a live test of every cave or caster branch.

### Unused world-map placement root

The `$0900` entry in the US action-placement table also survives in all
three European Story tables: index row `$1F:8004` points to `$1F:80CE`.
Japan and the European Action Mode tables omit it. The entry contains two
zeroed objects with type values 5 and 6. These are outside the four-entry
universal action type table in every release; treating them as two
undiscovered enemies decodes adjacent data as pointers.

Normal scene dispatch explicitly bypasses this root. US `$00:8340` tests
region `$18`; region zero branches to `$8373`, where submode 9 selects `$83A7`
and world initialization `$02:8134`. The action loader `$00:92CB` is called
on the nonzero-region path at `$8344`. JP has the matching routing at
`$833A/$836D/$83A1`, with action load `$92F3` at `$833E`.
Two native guard-fragment tests reach the world branch without touching the
sentinel action slot. The Go bank-00 cross-reference lists only `$8344` as
a caller of the US action loader; that is a CFG-rooted result, not proof
against every hypothetical indirect/debug entry. The normal-routing ownership
question is resolved without inventing an extra stage or actor implementation.

All three PAL scene paths have the same guard at `$00:8258`: region zero
branches to `$828B`, submode9 branches to `$82BF`, and `$82C4` calls world
initialization `$02:8134`. Action placement `$00:8D72` is called only on the
nonzero-region path at `$825C` in this dispatcher. Exact Go windows and the
structured Story/Action inventories establish the PAL counterpart; no new
forced execution of the invalid object types was needed.

## European difficulty and placement contracts

The three PAL ROMs above were examined independently. Cold-boot fixtures use
erased private SRAM, no WRAM edits and only controller input. All eighteen
combinations (three ROMs × two modes × three difficulties) reach Palace
opening dialogue or playable Fillmore. Action Mode needs no completion unlock
and begins with stored lives4 (five displayed), HP/max8. The reference core
reports 50.006978908188586 frames/s for all three, versus approximately60.099
for the US/JP fixtures. This establishes native PAL timing, not a recommendation
to change the recompilation's global frame clock.

### Selection and timer

European `$0205` is the difficulty word: 1=Beginner,2=Normal,3=Expert.
Title choice `$0336` is 0=Story,1=Continue,2=Action; `$0338` is save validity,
not difficulty. New Story/Action defaults difficulty to2. The title writes
timer reload byte `$034D` from `47 3B 2F` (71/59/47 decimal). `$034B` is
the action-only progression counter, initialized1, not a difficulty selector.

| Contract | EU English | German | French |
| --- | --- | --- | --- |
| Title choices through return | `$02:A71B–A892` | Same | `$02:A705–A87C` |
| Reload table (3 bytes, **data**) | `$02:A893` | Same | `$02:A87D` |
| Timer leaf | `$02:C27A–C296` | `$02:C283–C29F` | `$02:C26C–C288` |
| Contact collision routine | `$02:B091–B169` | `$02:B09A–B172` | `$02:B083–B15B` |
| Damage-box collision | `$02:B341–B3C6` | `$02:B34A–B3CF` | `$02:B333–B3B8` |

Timer byte `$E6` decrements each eligible update; underflow reloads `$034D`
and decrements BCD word `$E7` if nonzero. Nonzero `$E9` holds both. The native
leaf therefore uses72/60/48 updates per timer unit; uninterrupted booted
Action traces reproduce each interval. Do not read these PAL fields at the
US timer offsets. Continue skips difficulty selection and the load routine
restores the `$0200–02FF` block containing `$0205`; the complete
[save/Continue transactions](#european-story-save-and-continue) below verify
this separately from fresh starts.

### Spawn filtering, HP and contact damage

All three ROMs use `$00:8EDA–8F5F` for initial object-stream parsing and
`$00:8FDD–9074` for wave replacement. A four-byte object row is X,Y,param,type.
For regional types (type bit7 clear), param1 requires difficulty≥2 and param2
requires difficulty3. An admitted marker is normalized to0. Other params
retain their normal meaning. Universal types (bit7 set) bypass this check:
item IDs1/2 must not be mistaken for difficulty markers. Isolated native
tests cover both loaders, every difficulty, params0/1/2/3/64, and regional
bird versus universal pickup records. These check filtering/record fields,
not animation rendering from the isolated fixture's zeroed asset RAM.

Source initialization `$00:910E–91B7` copies attack/HP/score to slot
`+2A/+2C/+2E`. Its `$916B–9197` tail changes **HP**, not attack, only if
`flags & $8231 == 0`: Beginner2→1; Expert1→2; Normal and other values unchanged.
Seven flag patterns and HP0/1/2/3/24 are tested in all difficulties/releases.
The contact routine adds `(difficulty−1)&2`, shifted right once, to attack
before subtracting player HP. Thus only Expert adds1 for valid selections.
Both isolated subtraction and the complete collision caller are tested,
including zero base damage and lethal/clamped results. This is not a blanket
multiplier for magic, terrain damage or every object category.

### Room tables and hazards

The native loader `$00:8D72–8DCD` selects either `$1F:8000` (Story index
offset`$0004`) or `$1F:8002` (Action offset`$1485`) according to title choice2.
Entries are scene key plus offset relative to `$1F:8000`; object-stream goto
addresses are absolute within bank1F. The decoded50 Story/49 Action roots,
including followed wave/goto streams, match across all three European ROMs.
Whole bank1F does **not** match byte-for-byte; this is a structured comparison.

Against US, European Story has29 changed room streams: commands and/or damage
values, with all player starts matching. Twenty damage lists differ but their
box coordinates/order match. Across those lists,64 rows change1→24 and31
change1→2. Counts include alternative entry streams for shared rooms, not95
unique physical traps. Story and Action share all49 common damage/start lists
but seven command streams differ: `$0101,$0201,$0302,$0103,$0203,$0604,$0506`.
Their changed rows are pickups, including two removals. Pickup meanings must
be resolved through each mode's dispatcher, not copied from the US labels.

Both European modes contain78 Normal-or-higher markers and9 Expert-only
markers across the decoded streams. These are authored row counts, not a
promise of exactly that many additional enemies in a playthrough. Ordinary
unconditional rows, items, wave timing and repeated map entries remain distinct.

The European box expander `$00:8E67–8ED9` and collision caller preserve the
checked padded hot-point geometry. Native expansion tests cover each of the
20 differing lists in every ROM. Collision tests with values1/2/24/128,
all difficulties and a hit-gate control confirm that difficulty does not
alter box damage. Bit128 still requests slowing instead of damage;24 consumes
the normal24-HP maximum when an unprotected contact is accepted.

**Evidence scope.** This batch adds870 isolated native cases and18 cold-boot
traces, with36 exact bounded Go windows. An independent repeat reproduces
all320 evidence files byte-for-byte. No original ROM or player save is
modified. Boss difficulty, mode-specific items/magic, Story save transitions
and selected SIM numeric rules are examined below. Broader campaign/AI
behavior remains separate from this batch's scope.
Implementation should expose timer, placement, HP, contact and hazard policies
separately. Apply placement/hazard/initial-HP changes on future room loads;
do not respawn defeated actors or rewrite existing health in a settings callback.
Capture timing/damage policy at an agreed gameplay boundary, preserving active
countdowns and hit transactions. Selecting a PAL policy need not select PAL
video timing or import a foreign live-state blob.

### European boss difficulty branches

The Aitos Act 1 dragon is type `$04/0D`, not the type `$04/1A` Flaming Wheel.
Its EU English source/entry/end are `$00:D30D/$D319/$D4F8` (end exclusive).
German adds2 and French adds5 to these bank-00 addresses. Attack4 and HP24
are unchanged by difficulty at initialization. Expert's separate contact
addition still applies; this is not the US/JP dragon's base attack value.

EU `$D378/$D3AA` skip child allocation `$D426` on Beginner. Normal/Expert
allocate an eight-frame producer at `$D445`, which attempts two children at
`$A212` with counters1/2, then retires. Booted encounters contain this producer
and both children on Normal/Expert, neither on Beginner. The root's complete
states3/5/7/9 last160/72/45/72 native ticks in all three difficulties.
Partial phases at the start of the observation are excluded from these
durations. The root motion and the optional descendant attack are distinct
policy candidates; changing difficulty must not recreate an existing child.

The Marahna Act 1 plant is type `$05/05`: source/entry/end
`$00:D635/$D641/$D849`, with the same German+2/French+5 relocations. Its
HP24/attack1 also remain unchanged at initialization. Its root runs
state1 for12 updates, state2 for80, state3 for12, then protected state20 for90.
The protection is actor flag `$0020`; the complete native victim collision
path rejects a touching synthetic attack while it is set. This tests the
filter, not natural sword reachability. Japan's corresponding fully open
state2 lasts40 updates; the US examined head loop does not retract.

EU helper `$D7C2` makes the lower/right tendril run state24 on Beginner,
instead of state16 twice on Normal/Expert. Each choice totals48 updates.
State24 has one four-pixel vertical excursion; state16 has two over that
interval. Both retain horizontal velocity−1 and zero net vertical travel.
The controlled boot traces agree on following waits/attack-phase lengths;
do not describe the slower bobbing as a generally slower attack schedule.

Eighteen controlled encounters cover both families, all three difficulties
and all three PAL ROMs in **Action Mode**. Native room loads run sequentially
from same-ROM cold-start predecessors. A player-only hold, invulnerability
and position setup provides a stationary target; actors, RNG and assets stay
native. Every 1,200-frame trace retains positive HP, unchanged lives and SRAM.
The family-resident `$5000–5FFF` animation bytes match across the three ROMs
for each boss; this is a 4 KiB WRAM residency comparison, not a whole-ROM or
exact compressed-resource comparison. Native tick is PAL `$89`, camera is
`$23/$25`, and player pointer is `$8B`; US offsets would corrupt the fixture.
Magic-stock word `$21` is explicitly checked unchanged.

Seventy-eight isolated checks cover all10 tendril rows in both facings and
all releases, plus protected/unprotected victim contacts at every difficulty.
Fifteen bounded Go windows include both controllers, animation consumer
`$00:8967–8A4B`, the complete victim collision path and IRQ tick tail.
An independent repeat reproduces all436 files byte-for-byte. These are
attack-phase/difficulty results, not complete victories, Story playthroughs,
magic interactions or proof that every European boss matches another region.

### Difficulty integration in the US host

The game-owned difficulty policy has five independent source leaves: spawn
HP, contact damage, countdown reload, dragon attack availability and plant
tendril motion. The selected level is a separate host enum, not a regional
source or a write to European `$0205`. Requested/effective choices persist
in companion codec51; the native SRAM image retains its original layout.
All five effects are captured atomically at complete room/retry initialization.
No settings callback rewrites active HP, children, hit transactions or time.

- `$00:966F` runs after base-stat and fresh-flag projections at `$966C`.
  Descriptor Y and copied animation identity guard initialization before
  source `+32` is installed. A selected PAL HP rule reproduces the `$8231`
  exemption and Beginner2→1/Expert1→2 transformations, then rejoins `$968F`.
  This bypasses US Professional promotion, including its attack promotion;
  US/JP selections leave the original `$0201`/`$0349` path intact.
- `$00:8A24` changes only an accepted enemy-contact SBC. Expert adds1 to the
  attack in eight bits, then subtracts with the native carry/sign/overflow
  semantics. `$8A27` retains the store and original sign-based zero clamp.
  Damage-box contact `$8C12`, magic and native hit eligibility are untouched.
- `$02:BC8A` supplies reload71/59/47 only after the US divider has expired.
  US `$E5` is the divider, `$E6` the BCD time and `$E8` the hold gate. It does
  not substitute PAL offsets or rescale the host's60Hz simulation.
- `$00:D766` returns through native RTS `$D76B` on Beginner before allocating
  the Aitos dragon's producer. Existing producers/children are not removed.
- `$00:DADD` requests one repeat of retained state16 through real JSR `$DAE0`.
  The reader maps logical rows0–6 to native rows0/1/1/2/3/3/4, with delays
  3/3/15/3/3/15 and the native terminator. Pose27, mirrored velocities and
  collision remain native:48updates, one four-pixel bob, zero net Y travel.
  `+1C` marker100 owns in-progress logical rows across missing host caches;
  native state initialization/end clears it. No shared resource is patched.

The renderer-independent adapter tests cover all256×256 contact operands,
HP flag masks, pending/effective state, migration, timer guards and expanded
rows in both widths/facings. Five-ROM checks verify promotion masks, reload
tables, difficulty branches and the complete retained tendril pose/program.
Controlled Continue/Palace/Fight runs then loaded the plant and dragon rooms
through native pending-room transitions. All six Normal/Beginner/Expert runs
exited normally. Beginner countdown reload71 produced72-update intervals;
Expert reload47 produced48. Expert enemy contacts reduced20→18 and18→16
health for base attack1. Plant Beginner used the six expanded rows once per
48-update cycle; Normal/Expert retained two native bobs in the same period.
The dragon's Beginner gate skipped four producers, while Normal/Expert
allocated native projectile pairs. Existing actors and random state were not
edited. Plant background checks reported no mismatch; the controlled dragon
warp reported the same background mismatch in all three runs, so it is not
evidence of visual correctness. Complete fights and visual acceptance remain
separate from these numerical, ABI and controlled-lifecycle checks.

### European items and spell inventory

All three PAL ROMs share the following checked bank-00 addresses. Pickup
dispatcher `$86D7` uses title choice `$0336 == 2` for Action Mode; Story0
and Continue1 share the other path. Item IDs are logical entries, not labels
for a fixed graphic across modes.

| ID | Story / Continue | Action Mode |
| ---: | --- | --- |
| 0 | Add one generic scroll, capped255 | Push spell1 |
| 1 | Add one BCD spare life, capped99 | Add one current HP and one maximum HP, independently capped24 |
| 2 | Enter screen-clear dispatcher | Push spell2 |
| 3 | Set sword-power byte `$E5=$80` | Same |
| 4 | Set heal queue `$E4=floor(maxHP/4)` | Push spell3 |
| 5 | Set heal queue to missing HP | Same |
| 6 | Add BCD `$0100` (1,000 displayed points) | Same |
| 7 | Add BCD `$0050` (500 displayed points) | Push spell4 |

Pickup artwork selector `$91E0–920D` requests128 bytes from
`$06:A000 + ID*$80` for Story, or `$06:A800 + ID*$80` for Action, to VRAM
word `$2D80`. Therefore a US item-name substitution alone does not describe
the European presentation. This is a verified upload selection, not an
artistic identification of every tile. HUD-icon selector `$925F–9284` reads
`$06:AC00 + (spell−1)*$80`; zero selects the fifth entry. It requests128 bytes
to VRAM word `$2D40`.

Action push `$887F–8888` writes the spell byte into `$1C00 + stock`, using
word `$21` as the index and incrementing its low byte. Cast gate
`$9925–9980` requires a nonzero stock and accepts the last stored byte at
`$1BFF + stock`, writing active spell `$02AE`. It does **not** debit yet.
After the native spell/restore sequence, `$9A83–9AB9` decrements word `$21`,
then schedules icon actor `$08A0` with the next spell or zero. A Story cast
instead debits one low-byte scroll before the spell sequence and uses the
already selected `$02AE`. This distinction is not Japan's variable scroll cost.

Three booted Action fixtures seed legal stacks `[1,2,3,4]` and press A for
four complete casts: active IDs4→3→2→1, stock4→3→2→1→0, icon3→2→1→0.
All run native spell controllers/assets and input processing, retain positive
HP/lives and leave SRAM unchanged. They do not claim those four items were
naturally collected in Fillmore, prove maximum stack capacity, or test every
spell/enemy damage interaction. Different measured cast durations between
PAL languages are not treated as a timing rule from these unmatched RNG states.

Score helper `$8654–8687` adds in decimal mode, saturates at stored `$9999`,
and tests `(old XOR new) & $E000`. In Action only, a nonzero result adds one
BCD spare life. Ordinary small additions therefore award at20k/40k/60k/80k
displayed score. One large addition crossing multiple bands still awards
only once. Unlike the pickup helper, this addition has no99 cap: a checked
99→00 wrap is native behavior, not a host integer conversion recommendation.

The batch adds708 isolated cases:216 mode/difficulty/item combinations,
33 HP/life/scroll boundary checks,24 stack selection/pop fragments,48 Story
cast debits,324 score cases,48 pickup-art and15 HUD-art selections. Screen
clear uses an empty actor-list control; its complete target eligibility is
not newly validated here. Thirty exact Go windows handle M/X explicitly;
the post-PLP icon entry must be decoded at M0, and the push helper at M1.
An independent repeat reproduces all73 files byte-for-byte, including the
three complete casting traces.

The integrated score-life rule wraps US `$00:873C–874D` only after a room/
retry boundary captures its source. The original helper owns BCD score
addition, saturation and its RTS/PHP/PLP contract in both accumulator widths.
The adapter compares the old and resulting score, checks US Action-mode
progression `$0349`, and performs one uncapped decimal life increment. COP8D
uses the existing runtime interrupt boundary, preserving audio extension and
trace handling. PAL's final old-score accumulator return is also reproduced;
an escaped native return receives no postprocessing. The rule does not
change item pickups, lives display, retry debits or starting allowances.
Codec52 and replay identity retain requested/effective choices independently.
Tests cover36,000 width/mode/score/life transactions, all256 life bytes,
unsupported entry shapes, escaped returns and all five original helpers.
Three booted US/Japan/Europe controls each executed56 additional transactions
through the generated US helper after normal Continue/Palace/Fight. These
checked actual RTS balance, flags, score saturation, single/multiple band
crossings, the99→00 life edge and COP8D. Staged score, lives, mode, registers
and stack were restored immediately; these are bounded integration checks,
not naturally earned20,000-point playthroughs. All three exited normally with
zero action-background mismatches.

The US-host implementation captures the inventory model only on a confirmed
Action new run, after the bounded native initializer returns normally. Its
256-byte collection, count, displayed icon and in-flight spell are host-owned;
PAL `$1C00` is not presumed free in US memory. Native US stock byte `$21`
mirrors the count, without touching adjacent camera state `$22`. The host
collection is volatile Action-run state, not a change to cartridge SRAM.
Replay includes live ordered entries and transaction state, excluding dead
storage. Debug-state restore remains rejected before mutation.

Changed pickup effects enter at `$00:879D` and rejoin the original PLP/RTS
epilogue `$884E`. Shared sword/heal/score paths remain native. The regional
cast gate selects the last spell without quoting or spending generic scrolls;
the selected model remains fixed even if settings change during the effect.
Actual native cleanup `$9EFC` consumes it after graphics restoration. Accepted
room/retry initialization cancels only the pending cast and preserves stock.
Health/max health each increment below24, independently of each other.

Pickup source selection `$96E3` reuses US `$06:A400/A480/A500/A580`, which
match the four spell pickups in all PAL ROMs byte-for-byte. The full-apple
fallback distinguishes health growth from the incorrect native life graphic;
it is not claimed to match the European health-growth art. The HUD uses the
retained US spell graphics too, not the smaller PAL `$06:AC00` icons.
Dirty-only `$02:AC20` publishes128 bytes through native PPU ports, leaving
adjacent pickup tiles intact, then executes original JSR `$AF30` with caller
word `$AC22`. Ordinary unchanged frames have no additional upload.

Explicit generated boundaries prevent pickup-source, post-cast and NMI hooks
from being inlined away. Codec54 defaults older companions to US inventory.
Unit tests cover all256 push depths, delayed debit/interruption, independent
HP caps, CPU/stack/return contracts, rejected entry shapes, exact icon upload
length, migration, replay aliases and the five-ROM instructions/assets.
Controlled live checks complete all four effects, retain stock across an
interruption and retry, and compare nine HUD uploads against source tiles.
A separate staged boss-death/act-clear route retains three spells, increased
maximum HP and lives, clears sword power, then resets the populated collection
on Game Over restart. Its direct room warp is not evidence of natural encounter
or background-rendering parity. Full encounters and donor-media acceptance
remain separate checks.

### European Story save and Continue

The host's `$02:A622` title owner now exposes a separate new-game settings
draft while the native title coroutine is open. Accepted New Game/Action
publishes that draft; Continue instead loads the save-bound companion.
Abnormal title returns discard the draft without changing the active campaign.
Difficulty and inventory choices can therefore precede the first room/run
initializer rather than requiring a Game Over restart. This does not yet
couple difficulty selection to the translated title artwork: the overlay
provides the host difficulty selector.

New-game history starts with exact seed/reload projections, but observers and
regional projections remain gated until native baseline initialization has
been verified. Title population selection needs no destructive redevelopment:
there is no developed town in the draft. Existing-campaign conversion retains
its separate confirmation/recovery path. Draft and active edit tokens are
distinct, including after acceptance; neither path writes SRAM from settings.

Regional mode entry uses two independent leaves: completion-gated access and
Game Over destination. `$02:A70D` captures the original checksum result, then
chooses the native title branch. European access routes a no-save title into
the existing selector, with Start composed at `$A748/$A751` instead of a
misleading Continue label. `$A7E9→A813` cycles only eligible choices; the
completion marker remains untouched. Late enabling at the already-visible
Start prompt routes `$A72D` back to `$A72F`, while a populated menu can gain
the additional choice on its next navigation input.

The European Game Over prefix at `$AAF9` waits through the real `$AAEF` call
boundary, clears the retained US text map with bounded RTS leaf `$ABC4`, and
resets the corresponding US progression/sword fields and fourteen bytes at
`$7F:6B18`. `$AAFD` retains its native PHA and `$AB00` changes the target
operand from `$8058` to `$8023`. The `$AB03` adapter models the original
PHX/RTL stack effects, then explicitly retires all discarded host callers
through the runner's reset-root transfer contract. Ordinary dispatch here
would leave an extra compiled title/mainline activation on every Game Over.
The reset driver resumes at the new PC without resetting CPU or memory. This enters
the original US title setup at `$00:8024`, not a whole-CPU reset. PAL source
blocks are EU `$AB5F`, German `$AB68`, French `$AB51`; the tilemap clear
matches US byte-for-byte. US/Japanese restart branches remain native.

Four controlled host runs cover initial and late configuration with both
empty and valid, non-completion-unlocked saves. Each reaches its first
European-inventory Action run and returns through Game Over to a new title
draft, with Normal difficulty and retained rule choices. No save/marker is
written. A separate US Continue/Action/Game Over control retains its native
restart and allowances. These are selection/lifecycle checks with staged
death, not natural complete campaigns or final visual acceptance.

The generic transfer contract has a 200-cycle generated-code regression:
constant activation depth, no resumption of discarded callers, and a final
ordinary nested return. Invalid/foreign scopes, live hardware frames and
repeated consumption are rejected. This API is separate from ordinary
branch-only HLE tails and does not infer terminal transfers from stack height.
Three consecutive controlled in-game Action runs also return with one title
root each time and unchanged native stack balance. The private test exits
normally after13,000 ticks with zero action-background comparison mismatches.

Nine complete button-only transactions cover EU English/German/French and
all three difficulties. Each resumes its same-ROM fresh Story predecessor,
completes name entry and opening dialogue, opens Progress Log in the Palace,
and confirms the native save. Only that generated private SRAM is imported
into a fresh core instance; no player save or foreign-region state is used.
There are no WRAM edits. Each cold title selects Continue, skips a new
difficulty prompt and returns to the Palace with the original1/2/3 and
timer reload71/59/47. Continued SRAM remains unchanged.

The three ROMs share save/load entries `$03:A656/$A83A`, ending at `$A839`
and `$AA1B`. The save copies the full `$0200–02FF` block to SRAM
`$70:13B1–14B0`; difficulty word `$0205` therefore lives at `$70:13B6`.
The complete copied block is checked, not merely the difficulty bytes.
Load loop `$03:A9D2` restores the block. The title calls the load before
indexing the difficulty reload table at EU/DE `$02:A885` (FR `$A86F`).
That ordering prevents a cold/default difficulty from selecting the timer.

Checksum helper `$00:840B–8430` computes the sum and XOR over words below
SRAM `$1FEC`; the save commits them at `$03:A82E/$A833`. Boot traces expose
several intermediate SRAM writes before the checksum becomes valid. Future
regional companion data must attach to a completed save, not the first
payload write. This is not permission to alter the native cartridge format.

The nine save/cold-Continue traces and nine exact Go windows independently
repeat across all219 files. These tests close fresh Story difficulty
persistence, not cross-region save migration, interrupted writes, Action
inventory retention through deaths, or every developed-town payload.

### European simulation numeric rules

EU English, German and French match the US bytes for the18 population
thresholds at `$03:B40E`,18 maximum-SP values at `$03:B432`,24 nine-byte lair
seeds at `$03:B825`, and12 bytes of SIM species reward/HP/contact values at
`$01:B061`. Native consumer tests below independently check selected behavior
at each difficulty1/2/3; table equality alone is not a campaign-parity claim.

All five miracle affordability comparisons and debit immediates use the
US prices and addresses listed [above](#miracle-costs). Their current-SP
field is PAL `$0284`; common debit `$03:CA5E–CA79` saturates at zero. Gate
and debit fixtures cover one below, exactly at and one above every price.
They do not run target selection or the complete visual effect.

The shared-address lair installer `$03:B7C6–B824` reproduces every seed's
position, flags, species, stock, reload, countdown and assigned world slot.
Census `$03:C07E–C146` uses the US4/6/8 house occupancy and32/48/72 support
coefficients, including the tested stopped-field exclusions, in all six towns.
PAL per-town populations start at `$021E`; total population is `$021A`.
Town-record and lair storage in bank7F retains the checked US offsets.

Level leaf `$03:B3BA–B40D` uses PAL earned level `$0293`, angel HP/max
`$0288/$0289` and maximum SP `$0286`. It preserves earned levels below a
threshold, awards at most one per call, and retains the US HP24 saturation
behavior. Current SP and current action HP are not refilled by this leaf.
Tests cover both sides of all16 ordinary level thresholds, exact equality,
saturation, one-award behavior and a level17 below-sentinel control.

Cycle prefix `$03:8271–8295` queues `floor(maxSP/10)` and
`floor(maxAngelHP/4)` at `$0B05/$0B04`, matching US. Reviewed scheduler
`$03:8193–8237` retains the8-service subcycle and720-service long-cycle
threshold. These are service counts, not demonstrated equal wall-clock time.
Earthquake house selector `$03:A066–A07E` preserves subtype`$20` and queues
destruction for the tested other house subtypes. No extra difficulty-dependent
coefficient was observed in these consumers.

This batch adds1,620 isolated native cases:810 census,9 complete lair installs,
468 level checks,27 recovery-queue prefixes,36 house selectors, and270 miracle
gate/debit fragments. Thirty-six exact Go windows support it; all four
evidence files independently repeat byte-for-byte. It does **not** add a
booted SIM playthrough or another complete-earthquake matrix. Full actor AI,
story timing, level-award wrappers and developed-town maxima still require
their own coverage; do not generalize these results into “Europe equals US.”

### European Action retry and new-run inventory

The US-host starting-allowance adapter wraps the complete `$02:AB05–AB2F`
initializer with its original JSL/RTL frame. Requested spare-life and health
sources activate at that boundary, not at ordinary retry or room loading.
After a normal native return it writes only `$1C/$1D/$1E`, returning the
selected byte health in A with matching N/Z. Native score, stock, equipped
spell, sword power, progression and destination resets remain authoritative.
Escaped returns receive no postprocessing. Japan's equivalent initializer is
`$02:A84B`; PAL entries are EU `$AB9E`, German `$ABA7`, French `$AB90`.
PAL initializes health earlier and uses different progression/sword fields;
only verified allowance values are projected, not its RAM layout or ordering.

Codec53 preserves independent requested/effective choices; old companions
default to US. Tests cover all nine source pairs, both accumulator high-byte
cases, every valid entry flag combination, rejected shapes, escaped returns
and all five original initializers. First-run profile selection and European
Game Over/title routing remain separate mode-entry work.

Three US-host controls entered Game Over through the native death flag with
zero lives after Continue/Palace/Fight. Start invoked the generated initializer
with a balanced original return frame; the next Fillmore room had4/24,
2/24 and4/8 spares/health. Score, stock, equipped spell and sword power were
cleared. All three exited normally with no background mismatch. These test
restart routing, not a naturally lethal hit or selection from the first title.

Twenty-seven booted fixtures cover three routes × three difficulties × three
PAL ROMs. Each begins at native playable Fillmore with explicitly seeded
stack `[1,4,2]`, depth3, active spell2, score `$1234`, HP/max12 and sword
power `$80`. Retry/Game Over are entered by setting the player's native
death-dispatch flag `$0040`; these are controlled routing tests, not natural
lethal-hit or death-animation demonstrations. The separate room-load control
requests raw room `$0102` through `$1A` without defeating a boss.

- Positive-life retry: stored lives4→3; stack bytes/depth, active spell, score
  and maximum HP survive. Sword power clears.
- Direct room-load control: lives, spell inventory, score, maximum HP and
  sword power survive. **This is not the completed-act path**; it deliberately
  separates loader behavior from victory bookkeeping.
- Zero-life Action Game Over: native scene `$18/$19=08/01`. Start returns
  to the title and clears action progression; it does not immediately restart
  Fillmore. Selecting Action again opens difficulty with Normal selected.
  Confirming a new run resets stored lives4, HP/max8, depth0, active spell0,
  score0 and sword power0. Difficulty/timer use the newly confirmed choice.

Death router EU `$02:C315–C388`, Game Over exit `$02:AB5F–AB9D`, and new-run
initializer `$02:AB9E–ABC8` relocate by German+9/French−14. Common scene
transition `$00:8158–81F8` performs the BCD retry life debit and HP refill.
Do not reuse the earlier title-menu relocation for these bank-02 routines.

All fixtures end with positive HP and unchanged private SRAM. Twelve exact
Go windows and all246 evidence files reproduce independently. No claim of
natural boss victory, all checkpoints, maximum spell-stack capacity or
interrupted-cast behavior follows from these tests. Any inventory-policy
migration must distinguish ordinary room load, completed act, checkpoint
retry, title return and confirmed new run.

### European completed-act inventory

Eighteen additional booted transactions cover Aitos Act 1 and Marahna Act 1,
three difficulties and three PAL ROMs. The already loaded boss enters its
native lethal-hit dispatch at `$00:A0F1` with HP 0 and timer 0. All children,
death effects, sound upload, stage-clear card, player departure and next-act
loading remain native. This is controlled defeat dispatch, not proof of a
natural sword victory. The fixture seeds stack `[1,4,2]`, depth 3, active
spell 2, HP/max 6/12, score `$1234`, lives 4 and sword power `$80`.

The death owner creates the clear-card actor and hands the player to
`$00:9D75` via `$00:A1E1–A1FC`. The actor compaction here can change the
player slot: follow `$8B`, not a fixed `$08E0`. Action progression `$034B`
selects `$9E51`, bypassing Story's remaining-time/life conversion. Healing
queues 24 points, waits for current HP to reach maximum, then waits 60 updates.
`$9E73` routes to `$86BB`: sword power clears, progression increments, and
`$02:9013` supplies the next scene. Aitos 7→8 and Marahna 9→10 both load
their Act-2 room 4.

All 18 end at HP/max 12/12, with stack, depth, active spell and lives intact;
score is `$1284` after the boss's `$50` BCD award. Difficulty and private SRAM
are unchanged. Nine exact Go windows and independently identical output
support this result. A rejected exploratory Marahna fixture released the AI
probe's suspended player beyond a ledge, triggering a room exit/retry. The
accepted fixture places the player at the room's authored entry before
release; that failure is not evidence of a regional completion rule.

### Five-ROM placed actor-stat census

The census walks `act_content.iter_type_table` by `(region,type)` and joins
actual placement commands. Zero table slots, shared targets and `$FF`
direct-handler uses stay distinct; a direct handler is not parsed as a
12-byte stat record. Each ROM has 225 table slots. Placed data-record keys
number 173 in US and each PAL release, and 174 in Japan. Japan's extra
placed key is `01/11`. No action placement has an unresolved table lookup.
These are keys, not counts of enemies encountered in one playthrough.

Compared with US, Europe changes 58 joined record keys: 54 change attack or
HP, three change flags, and the shared pickup `00/00` moves its animation
base from `$A800` to `$B000`. The three PAL releases agree on all compared
record fields excluding relocated secondary-handler pointers. The complete
attack/HP differences are below; type numbers are hexadecimal, values decimal.
They are authored values before difficulty, contact and controller changes.

| Region | Attack: type, US→Europe | HP: type, US→Europe |
| --- | --- | --- |
| 01 Fillmore | 02: 1→2; 04: 1→2; 0A: 1→2; 0E: 1→2; 10: 1→24; 12: 1→24; 13: 2→5; 17: 1→3 | 02: 1→2; 0E: 1→2; 13: 6→5 |
| 02 Bloodpool | 04: 1→2; 25: 3→8; 27: 1→3 | 23: 1→2 |
| 03 Kasandora | 04: 1→2; 0B: 1→4; 0C: 1→2; 10: 1→2; 11: 1→3; 12: 1→2 | 06: 1→2; 07: 2→3; 0C: 1→2; 16: 1→2 |
| 04 Aitos | 05: 1→2; 06: 2→3; 08: 1→2; 09: 1→3; 0A: 1→3; 0C: 1→2; 0D: 1→4; 16: 2→4; 17: 2→4 | 0B: 1→2; 0C: 0→2; 14: 1→2 |
| 05 Marahna | 12: 1→2; 13: 1→2; 14: 1→2; 15: 1→3; 16: 1→3; 17: 1→3; 20: 1→3 | 07: 3→5; 0B: 3→5; 20: 3→5 |
| 06 Northwall | 08: 1→3; 1C: 1→2 | 09: 2→3; 16: 3→4; 1A: 1→2; 1B: 1→2 |
| 07 Death Heim | 00: 1→3; 01: 1→4; 02: 1→5; 03: 1→2; 04: 1→2; 05: 1→4; 15: 1→3 | — |

Fillmore types `1C/1D/1E` change flags `$0032→$8032`; the extra bit gates
their updates during casting, as checked [below](#european-linked-prop-cast-freeze).
It is not an independently implemented toggle. US/JP retain the previously
mapped 21 changed keys among 173 shared records. Animation resources are
joined through script load/inheritance and initial state, but their entire
state graphs and indirect child controllers are not declared equivalent.

There are 2,251 independently repeated native initializer-prefix fixtures:
346 US and 348 JP normal/Special cases, plus 519 per PAL release. Each uses
the original ROM record and a placed parameter. US `$00:95F0–968E`, JP
`$00:9618–96B6`, and PAL `$00:910E–9197` execute through the stat adjustment,
stopping before animation loading. Thus the PAL HP mask/rule is checked
against every joined record, not only synthetic HP examples. This does not
execute every record's controller, contact path or later animation phase.

The host now integrates the 63 changed base fields (21 HP,42 attack) across
54 US-owned initializers. Stable area/type keys and numerical tables live in
the portable policy; native source-address mapping stays in the game adapter.
The `$966C→966F` prefix uses descriptor Y, not a reused slot's `+$32`, and
applies selected values before the existing native promotion. Every changed
field is independently selectable. The shared room-policy cache is published
only after all room-boundary activations succeed; no lookup runs per animation
frame. Baseline settings bypass the projection without searching the table.

The skull armor/reward prefix composes at the same boundary; it does not
overwrite selected skull HP/attack. Native record signature/animation-identity
checks reject incompatible inputs. Five-ROM stat bytes and 1,944 combinations
of real records, HP/attack sources and skull rules verify the adapter; another
1,944 synthetic cases test untouched fields and reused-slot identities.
The table integration does not change difficulty or terrain rectangles.
Explicit child assignments are handled by separate initializer prefixes:
Tanzra minion HP`$FC96→FC99` and reward`$FC99→FC9C` replace their own store
values independently, preserving A=2 and all flags. Projectile`$FD2E→FD31`
replaces the attack LDA and its N/Z result; the native STA follows. The final
room, inherited source/animation identity, active handler and fresh flags
guard each prefix. No parent-state polling, allocator replacement or live-HP
rewrite is involved. All five ROM instruction signatures,2,592 mixed-source/
inherited-value cases and40 invalid-owner/CPU cases check this adapter.
Full encounters remain a distinct validation gate.

### Native narrative comparisons

The Go Builder's `localization-extract --format catalog` supplies all five
private source catalogs. Six complete messages per ROM were compared with
their semantic routes, ordered controls, source spans and raw SHA-256 checks.
No new Python language decoder, edited translation pack or external article
is the source of these findings. This is decoded-script evidence, not an
additional set of booted playthroughs.

| Conversation | US | JP | European English | German | French |
| --- | --- | --- | --- | --- | --- |
| Opening | `$04:8EFC` | `$02:EA01` | `$04:8EFC` | `$04:8EE2` | `$04:8F8F` |
| Kasandora missing-person report | `$04:AC05` | `$02:C703` | `$04:AC05` | `$04:AC43` | `$04:AC43` |
| Kasandora discovery/burial | `$04:AC50` | `$02:C759` | `$04:AC50` | `$04:AC8E` | `$04:AC8F` |
| Bloodpool sacrifice request | `$04:9EF9` | `$02:BBE0` | `$04:9EF9` | `$04:9EFF` | `$04:9F3F` |
| Bloodpool ending | `$04:CF6A` | `$02:EDB2` | `$04:CF68` | `$04:D0C5` | `$04:D041` |
| Death Heim announcement | `$04:914C` | `$02:F51C` | `$04:914C` | `$04:9156` | `$04:9206` |

The opening is wrapper 05/call 01/source 00. All five retain sealed/lost
power and restoration through human faith; the checked Western conversation
does not replace that with a centuries-long-sleep account. This does not
adjudicate separate manual or promotional backstories. Satan becomes Tanzra,
and the Japanese divine form of address becomes Master/Lord in the checked
Western conversations.

Kasandora event slots 02/03 explicitly report death and planned burial near
the shrine/temple in every release. US/European English operations match
exactly for both messages, as they also do for the opening and Bloodpool's
sacrifice request. The claim that Western death/burial is merely implied is
therefore contradicted by these ROMs.

The JP Bloodpool ending says Teddy **made the lots**. US, European English,
German and French instead reveal that the angel knew Teddy was selected.
Deliberate rigging/self-selection is an interpretation, not an explicit
statement in the checked Japanese passage. The European English text changes
only “boy name” to “boy named” among this message's text runs. Match the
ending by scene/content: JP wrapper 00/call 00/source **01** corresponds to
Western source **02**, not the same source ordinal.

Controls remain significant. The opening has five explicit page breaks in
JP versus four elsewhere; discovery/burial has three versus two. Those
messages terminate with `end`. The Bloodpool ending instead uses a text-state
toggle, line breaks and 30-update delays, then `yield`: JP has 14 delay
operations, Western scripts 17, without explicit page-break operations.
This closes the listed opening/names/burial/Teddy leads, not a line-by-line
literary audit of every conversation or permission to tie story policy to
font, locale or gameplay settings.

### European spell capacity and interrupted casts

The Action inventory count is a word at `$21`, but pickup push
`$00:887F–8888` increments only its low byte. Twelve isolated fixtures across
the three PAL ROMs test depths 0, 1, 254 and 255 through the original item
dispatcher `$00:86D7`. The collected spell goes to `$1C00+depth`; depth 255
writes `$1CFF` and wraps to zero, rather than clamping. Sentinels immediately
outside the 256-byte storage remain intact. The icon still shows the newly
collected spell. This establishes a boundary behavior, not that an ordinary
run can accumulate 256 spells.

The host retains this byte-count wrap. One defensive exception is a wrap
during an in-flight effect: completion clears the pending cast and leaves an
empty collection. It does not reproduce the PAL word-debit underflow into
unrelated WRAM, invent a spell, or terminate the game. A dedicated test covers
255 collected spells, a cast, a 256th pickup and completion.

Cast owner `$00:9925–9AB9` selects the top Action spell before the effect,
but debits it afterward. It sets player flag `$0010` and `$FA`, spawns the
effect, waits for completion, reloads graphics and clears the casting state.
At `$9A83` it checks the mode again, then pops the Action stack and updates
the icon. Three isolated debit fixtures independently verify `[1,4,2]` at
depth 3 becoming depth 2 with spell 4 selected. Story debits before the
effect instead; do not transplant the Action ordering into Story.

Twenty-four booted fixtures cover four spells, two interruption points and
three PAL releases on Normal. A pending-room request to Fillmore Act 2 is
posted either immediately after casting begins or after the native debit.
After the room transition, the first route retains its one spell; the second
has none. Neither leaves `$FA` or the player's casting flag active. All end
with positive HP and unchanged lives/private SRAM. These are forced room
requests, not proof of a player-accessible spell-cancellation exploit.

A host inventory-policy switch must therefore either wait for the cast to
finish or preserve its selected spell, payment state and owning policy.
Re-reading a newly selected policy at the old cast's completion could charge
the wrong inventory. Retry, completed act and ordinary room load remain
separate lifecycle events, as documented above.

### European linked-prop cast freeze

Fillmore types `$1C/$1D/$1E` retain flags `$8032` after full initialization
at `$00:910E–91B7`, including original animation loading. Twenty-seven
fixtures check all three records, all three PAL ROMs and `$F3` values 0/1/2.
The apparent `$8000` clear in animation service `$00:8A12–8A26` is guarded
by comparison with the player pointer `$8B`; it does not clear these props.

Dispatcher `$00:890C–8966` skips a non-player actor with flag `$8000` while
`$FA` is nonzero. Thirty-six fixtures compare flags `$0032/$8032` and both
cast states using the original linked-position continuations. Only
`$8032` with active `$FA` leaves the actor's old position untouched. Otherwise
the three tails copy their linked parent's position with offsets
`(-32,+24)`, `(0,-64)` and `(+32,-24)` respectively.

Record starts are EU `$00:A8AC/A8DC/A908`, German +2 and French +5;
the tested tails begin 15 bytes later. This resolves the purpose of the
three changed flags. It does not test every rider or the complete linked
platform group during a naturally initiated cast. The 63 fixtures here plus
the 15 inventory fixtures above form one 78-case isolated batch.

The recompilation exposes three room-scoped flag choices at US spawn prefix
`$00:966C→966F`. US descriptors `$AC02/$AC32/$AC5E` carry the same states
38/39/40 and base `$7E:4000`; only their fresh `$0032` flags become `$8032`.
The US dispatcher already has the corresponding hold at `$8943–894A`, using
`$F9` rather than PAL `$FA`. No imported dispatcher or per-frame policy is
needed. Twelve US suffix fixtures confirm the new flag survives original
promotion/animation initialization, and twelve dispatcher fixtures confirm
hold/resume against the original `$AC11/$AC41/$AC6D` continuations. This is
isolated native execution, not a complete natural rider/cast playthrough.
Adapter tests cover3,888 source/slot/flag cases,22 malformed contexts and all
five ROM descriptors. The native initializer's Y, not stale slot field+32,
owns the choice. Existing actors retain their flags until room reload.

### Fillmore Act 2 music residency

The `scene_music_route` policy is integrated at US `$02:B653–B655`, after
the native scene selector accepts the declaration and before the resident
source comparison. For scene keys `$0201/$0301` only, Japan replaces the
recognized track-1/selector-0 request `$0E:F69F` with US `$18:947F`. Room 4's
boss declaration, other tracks and unrecognized/custom resources are untouched.
All five ROMs' cave declarations and the complete shared Fillmore SPC upload
image are covered by the optional ROM test. No donor resource is required.

The adapter reproduces `LDX $A5` and its N/Z flags, then tails native `$B655`.
It never writes resident `$AB/$AD`, APU ports or playback commands: native
code retains upload suppression, handshakes and playback ownership. Activation
comes from the active campaign at the accepted music declaration, independently
of the action-room cache, since music command 2 precedes video command 3.
Settings edits leave the current resident track alone. Regional sequence
payloads remain a separate policy and extraction task.

An eight-load US-host integration control verifies US→Japan→Europe→US routing
and the unchanged boss source. Consecutive cave rooms using the same source
produce no extra upload/play commands; the settings edit itself leaves the
resident cache unchanged. The live entry has PB2/**DB0**, M1X0 and D0. The
run exits normally, captures nonzero PCM and reports no action-background tile
mismatches. These are controlled scene loads, not a full-act playthrough or
subjective audio approval.

Five booted scene loads request Fillmore room 2 through the native pending
scene field. Each produces nonzero audio, reaches the requested scene with
positive HP and retains lives/private SRAM. The actual loaded source cache,
not just the requested song number, identifies these resources:

| Releases | Loaded source in Act 2 | Resource identity |
| --- | --- | --- |
| JP | `$17:F04C` | Byte-identical SPC upload image to US `$18:947F`, catalogued as Fillmore; also used in JP Act 1 |
| US, EU English, German, French | `$0E:F69F` | Same source declared for Kasandora Act 2 and Marahna Act 1 |

The source cache starts at `$AB` in US/JP and `$AC` in PAL. The local music
catalog calls the second image `song-09` / “Track 09”; its scene association
is verified without treating an external soundtrack title as ROM metadata.
Private PCM captures and source-image hashes repeat exactly. This closes
the Fillmore theme-selection lead, not the separate reachability question
for the extra US/PAL Sky Palace song declaration.

### European SIM state-program comparison

US, EU English, German and French have identical bytes at
`$01:B8D0–C7BE`, including the four monster classes' roots and all 64 state
table entries. Reviewed captures cover 56 distinct state bodies per ROM;
aliased entries are retained rather than counted as new routines. Root/table
pairs are Blue Dragon `$B9EC/$B9F8`, Napper Bat `$BE4F/$BE58`, Red Demon
`$C237/$C243` and Skull Head `$C4E5/$C505`.

Four shared services also match exactly: state dispatch `$D04E–D062`,
entry-flag handling `$D063–D071`, behavior selection `$D072–D08E`, and
behavior advance/movement `$D08F–D126`. Together these yield 256 exact Go
windows, independently repeated. This is static evidence, not additional
native execution cases.

The direct control-flow frontier still includes 22 external targets,
including shared target search `$BCB0`, world-actor service `$B898`, RNG
`$03:AF65`, and map/effect helpers. Relative tail branches are included in
that frontier. Behavior streams selected through `$01:E099` and composition
data at `$01:E7D9` are separate dependencies. Identical local routines do not
by themselves establish identical full AI, scheduling or PAL wall-clock pace.

### Five-ROM ownership inventory

The ownership ledger now partitions every byte of each supplied ROM. It
joins reviewed code, action placement streams, keyed spawn records, language
records, cartridge headers and all 59 asset-script entries with their decoded
resource identities. Multiple owners can overlap; unknown ranges stay unknown.

| ROM | Bytes not yet assigned an owner |
| --- | ---: |
| US | 465,216 |
| JP | 475,374 |
| EU English | 457,912 |
| German | 458,556 |
| French | 458,843 |

These are coverage counts, **not counts of regional differences**. Evidence
breadth differs by release, and unknown ranges can contain code, artwork,
tables or unused material. No padding is inferred from repeated bytes.
Compressed resource ranges use the decoder's consumed input, including its
lookahead; they are not exact bit-level boundaries. The ledger and decoded
manifests reproduce independently, but do not establish exhaustive gameplay
coverage or permission to mix arbitrary resource fragments.

### City-report label selection

The authoritative Go catalogs join each growth code to its native pointer
table and complete source record. Thirty isolated composer calls exercise
all six selector values in all five ROMs through the original fixed-text
opcode `$08`. Both main glyph cells and Japanese diacritics in the row above
match the source bytes. The labels below omit padding spaces:

| Code | US / EU English | German | French | Japanese |
| --- | --- | --- | --- | --- |
| 0 | None | Kein | RIEN | むじん (uninhabited) |
| 1 | Stop | Stop | STOP | ていたい (stagnant) |
| 2 | Slow | Slow | LENT | おそい (slow) |
| 3 | Norm | Norm | NORM | あんてい (stable) |
| 4 | Fast | Fast | VITE | はやい (fast) |
| 5 | Max | Max | MAX | げんかい (limit) |

Lookup tables are US `$01:F66F`, JP `$01:F4EE`, EU English `$01:F677`,
German `$01:F689` and French `$01:F6C0`. Opcode `$08` reads the low byte of
the specified RAM field, doubles it and indexes that table. Japanese
composition also places dakuten/handakuten above their base glyphs.

Two booted Palace reports verify code 5 on screen with native fonts:
US “Max” and JP “げんかい”. Their population, act, growth and status inputs
are deliberately staged, not naturally developed towns. The US recalculates
the report when it opens; JP consumes its stored report classes. An initial
probe which assumed injected US classes would survive menu opening was
rejected, not counted as a display failure.

Thirty-six additional PAL classifier fixtures exercise all six towns one
below and exactly at the thresholds listed [earlier](#census-construction-and-status-are-separate-contracts).
With two completed acts and the specified development inputs, below selects
code 3 and equality selects code 5. These are native report thresholds, not
attainable-population measurements. Together with the composer calls this
batch adds 66 isolated cases and two booted traces, independently reproduced.

The Japanese report therefore does not lack a maximum/limit label or simply
translate code 5 as “slow.” Different classifiers can still make a particular
developed town display different codes. Resolving that natural-town outcome
belongs with the remaining geography/development tests, not with font or
translation policy.

### Ordinary action artwork survey

A five-ROM pass resolves 13 ordinary-action resource sets per release from
the native asset-script declarations: animation slot `$4000`, character data
at VRAM word `$3000`, and palette colors `$80–BF`. Resources inherited from
earlier rooms are retained. The existing Go builder sprite compositor renders
the original composition records; a synthetic index palette separately
distinguishes changed pixel indices from palette-only changes.

The repeated survey covers 65 bundles and all 3,363 bounded composition
requests now decode. The last 25 extraction rejections were missing resource
support in the survey tool, not observed game-rendering errors. Their bank
and palette sources are resolved below; no missing bytes are guessed.
The US/JP ordinal-aligned comparisons contain 224 changed rendered pictures,
including 30 palette-only changes. These are pictures, not distinct enemies
or exhaustive semantic matches. All successfully compared European pictures
match the corresponding US output.

The initial 160-pixel canvas accepted 3,253 pictures. A research-only
512-pixel canvas resolves another 85 compositions; it uses the same Go
bank-local pixel renderer, signed anchor, native Y adjustment and reverse
part order. Every old render remains pixel-identical in the centered crop,
with no extra pixels outside it. All 89 follow-up files, including the
85 new PNGs, reproduce byte-for-byte. The production renderer is unchanged.

#### Shared-bank and palette completion

Fillmore Act 2 pictures `$28–2B` reference both the ordinary enemy atlas and
the common action atlas at `$07:8000` (file `0x38000`). Under the ordinary
actor's zero attribute modifier, the native emitter XORs `$0100`: a clear
bank bit in the stored part selects VRAM word `$3000`, and a set bit selects
common bank `$2000`. This is distinct from the boss configuration that moves
the second OBJ bank to `$4000`. All four pictures decode identically across
regions. None has a reference in its bounded animation table; decoding them
does **not** establish that normal gameplay displays them.

Fillmore Act 1 picture `$1B` references palette 7, outside the original
survey's `$80–BF` slice. Its tile `$34` is entirely transparent in every
ROM. The palette source is nevertheless identified: native code alternates
two 32-byte palettes in CGRAM `$F0–FF`, selected by frame-counter bit 1.
The counter is direct-page `$88` in US/JP and `$89` in PAL. Both palettes
match across all five releases; this particular blank frame draws no pixels
in either phase. State `$1A` references it, unlike the four cave pictures.

| Release | Common CHR / `$C0–EF` palette loader | `$F0–FF` palette updater | First of two palette sources |
| --- | --- | --- | --- |
| US | `$02:BC9E–BCFB` | `$02:ADFF–AE34` | `$02:AE35` |
| JP | `$04:8C99–8CF6` | `$02:AB25–AB5A` | `$02:AB5B` |
| EU English | `$02:C297–C314` | `$02:AE98–AECD` | `$02:AECE` |
| German | `$02:C2A0–C31D` | `$02:AEA1–AED6` | `$02:AED7` |
| French | `$02:C289–C306` | `$02:AE8A–AEBF` | `$02:AEC0` |

Five same-ROM scratch-state traces verify resources in both Fillmore sets.
They request the native second-room loader without editing player/camera
coordinates or adding actors. Ordinary CHR, common tiles `$00–D3`, palette
`$80–EF`, and both flashing palette phases match their ROM sources. Lives
and SRAM remain unchanged and health stays positive. Palette inspection
temporarily changes PPU register state, then restores a byte-identical core
snapshot before execution resumes. These are resource-residency checks,
not unassisted traversal or unused-actor reachability proofs.

Twenty Go windows cover the loaders and attribute transforms. Re-rendering
preserves all 3,338 prior color/index hashes; the 25 newly resolved pictures
introduce no additional regional differences. All 87 follow-up files repeat
byte-for-byte, including the two palette-phase outputs for each blank frame.

The common loader also resolves the old magic-icon copy-size ambiguity:
its `$0080`-iteration loop writes **16-bit words**, copying 256 bytes from
`$06:A400 + (id−1)*$80` to VRAM word `$2D40`; an unequipped Story slot uses
entry 6. The source stride is 128 bytes, so adjacent windows overlap and the
write covers tiles `$D4–DB`, not just the four icon tiles `$D4–D7`. PAL Action
Mode instead derives the source from its inventory stack and adds `$0800`
(base `$06:AC00`, empty entry 5). Do not confuse this entry-time word loop
with the separate 128-byte queued HUD-icon update described under
[European items](#european-items-and-spell-inventory).

#### Extended-picture actor identities

The additional US/JP differences are Bloodpool Act 2 visuals `$1F/$20` and
Kasandora Act 1 visuals `$10/$11/$12/$13/$1D`. All seven change indexed pixels,
not only palette colors; the corresponding PAL pictures match US. Native
controller/resource joins identify them as the castle's electrical barrier
and the desert's tall flowering plant, respectively:

| Actor | Placement key | US record / controller | JP record / controller | EU English record / controller |
| --- | --- | --- | --- | --- |
| Bloodpool electrical barrier | `02/25`, room `$0502` | `$BD2A / BD36–BD75` | `$BDBE / BDCA–BE09` | `$B9E2 / B9EE–BA2D` |
| Kasandora flowering plant | `03/07`, room `$0103` | `$C45F / C46B–C569` | `$C4EE / C4FA–C5F8` | `$C126 / C132–C230` |

These addresses are in bank `$00`; German adds two and French adds five to
the EU English addresses. The barrier inherits the ordinary `$4000` resource
loaded in room `$0202`. Its state `$14` selects pictures `$1F/$20`. The plant
reaches pictures `$10/$11/$12/$13/$1D` through states `$17/$18/$19` in the
tested approach. Its related states `$1D/$25/$26` also reference this artwork.
The seven composition descriptors and the reviewed states' pictures, delays,
movement and collision extents agree across all five ROMs. This identifies
the redraws, not whole-actor gameplay equivalence; actor statistics remain a
separate comparison.

Ten booted fixtures use native sequential room loads, then hold only the
Master and camera near a placed actor. They verify the live source record,
animation slot, state and visual, and retain native screenshots for each
target picture. Enemy code and resource data are unmodified; health remains
positive, lives and private SRAM remain unchanged. Ten Go-decoded controller
windows support the joins. All 159 evidence files reproduce byte-for-byte.
A larger research canvas removes a survey limitation, not a limitation of
the original game; these fixtures do not exhaustively test OAM edge wrapping.

| Resource set | Named inspected differences |
| --- | --- |
| Fillmore Act 1 | Club goblins, birds and leaping beasts are redrawn; checked hanging-log differences are palette-only |
| Fillmore Act 2 | Ghoul and skeletal-swordsman faces/parts change |
| Bloodpool Act 1 | Lizard clothing/faces, water-jumper shape/palette and bird wingtips change |
| Bloodpool Act 2 | Floating skulls, fireball statues, goblins, skeletal swordsmen and the electrical barrier change |
| Kasandora Act 1 | Tall flowering-plant poses change pixels while retaining the checked composition and animation records |
| Kasandora Act 2 | Bird-headed swordsmen change clothing and sword composition |
| Aitos Act 1 | Humanoids and tornadoes are redrawn; birdman visuals `$21–25` change palette only |
| Marahna Act 1 | Spearman visual `$13` differs at five rendered pixels, with identical composition records |
| Marahna Act 2 | Arrow visual `$2C` uses two parts in US versus four in JP; US adds the bright `$36` pose |
| Northwall Acts 1/2 | Two-headed enemy faces change across several poses |

Aitos states `$0D/$1F/$20` retain identical six-entry programs, including
their three tornado pictures and intervening blank picture, durations,
velocities and collision extents. Kasandora states `$0E/$17/$18` retain their
animation entries and timing, but **not** all collision extents; see the
[native collision checks](#kasandora-swordsman-collision-extents). Neither comparison supports claiming an extra
Western animation step merely from changed sprite assembly.

JP Bloodpool Act 2 states `$1A/$1B` reference visual `$3F`, outside the
bounded 58-entry composition table. Their natural reachability remains
unresolved; they are not rendered by reading past the table and are not
labelled a confirmed runtime bug. Boss resources, other shared artwork, title,
simulation and Death Heim backgrounds remain outside this ordinary-artwork
pass. Extracted pictures and donor bytes stay in private research outputs.

### Town, title and Death Heim artwork

This bounded five-ROM comparison closes the named field, follower-symbol,
skull-lair, pyramid, title and first-statue artwork leads. It joins original
asset scripts and composition tables to **1,325 private source renders**,
**200 isolated native calls** and **30 bounded Go disassemblies**. A second
run reproduced all 1,332 evidence files exactly (excluding the output-path
overlay configuration). These are resource reconstructions, not new
playthrough screenshots. No player or camera positions were changed.

| Source / selector | Contract |
| --- | --- |
| Town asset entries `$18/$19 = 00/01..06` | Two raw 16 KiB BG character banks at file `0x60000/0x64000`; OBJ characters at `0x68000`. Raw big-endian terrain/structure definitions are separate resources. Town 6 selects the snow palette. |
| BG bank filter: US `$02:C58C`, JP `$04:95EC`, EU `$02:CBA5`, DE `$02:CBAE`, FR `$02:CB97` | Below two completed acts at `$7F:6B18 + town*2`, skip file `0x64000`; at two or more, skip `0x60000`. OBJ upload is unaffected. All four tested completion values and three sources agree across five ROMs; PAL source pointer is DP `$A6`, versus `$A5` in US/JP. This is act completion, not house-development tier. |
| Structure class 4, variants 0/2/4/6 | Ordinary, burned ordinary, improved, burned improved field programs. US table `$03:D4D2`, JP `$03:CFD7`; native initializers `$03:A4B8/$03:A290`. Each living field program has 16 transition frames followed by five loop entries (three distinct settled pictures); burned fields have one entry. All corresponding field pixels match in both character banks. Existing regional duration differences are not a new crop-art difference. |
| Follower family `$08`, variants 0–6 | Native state initializer US `$01:C9AD`, JP `$01:C937` indexes `$01:CA4B/$01:C9D5` from record `+$14`, selects family/variant and publishes the composition at `+$08`. The seven compositions are US `$D32B..D34F`, JP `$D2B5..D2D9`, step 6. Only variants 1 and 4 differ: angry face/skull and skull/yellow cross respectively. All retain `(0,-10)` origin and 16×16 size. The death branch also selects `$0804` explicitly at US `$01:CA25+` / JP `$01:C9AF+`. |
| Lair/landmark draw lists | US `$03:BC8A`, JP `$03:BA13`, 17 pointers. Native draw consumer `$03:BC42/$03:B9CB` expands triples through `$03:9C43` or its JP counterpart. All 17 lists were executed and their complete `$7F:0000` writes compared with the extracted definitions; native bit `$0200` is masked, not used as a tile-address bit. |
| Skull Head lair IDs 6, 9, 10 | Seed table identifies these with monster type `$15`. Draw lists use structure metatiles `$FE/$FF`, `$F4/$F5`, `$FE/$FF`. JP's earlier bank has the six-pointed star; Western banks have the diamond. The later bank matches across all five ROMs. Normal lair redraw caller is US `$03:B90D`, JP `$03:B696`. No claim that an unsealed late-bank lair is naturally reachable. |
| Pyramid ID 15 | The Kasandora special-landmark record specifies cell `(20,4)`, picture 15 and semantic mark `$EE`; composer US `$03:BB94`, JP `$03:B91D`. Its four structure metatiles are `$D6/$D7/$DE/$DF`. Eye pixels differ in both character banks. |
| Title entry `00/00` | Mode-7 raw character sheet at file `0x58300`, separately selected map/palette, reconstructed as a 1024×1024 canvas. Western canvases match; Japan changes the A, r, emblem and Japanese lettering. Copyright/company order is **not** baked into this canvas. |
| Death Heim entry `07/01` | Separate BG1/BG2 compressed CHR, metatiles and chunk maps. Use action mask `$ECFF`, BG1 attribute `$1000` and BG2 `$0100`. BG1 matches; BG2 contains the first-statue horn redraw. Western BG2 resources render identically. Spare pages in the resource are not evidence of visible extra scenery. |

Go-extracted `title.copyright` records independently establish the footer:
US `$02:A9DE` (1991 Enix/Quintet), JP `$02:A754` (1990 Quintet/Enix),
EU/DE `$02:AA1D` and FR `$02:AA07` (1992 Enix/Quintet). Japan's record lacks
the Western Nintendo licensing line. France translates the rights-reserved
line. The five catalog ROM hashes and their source-operation hashes are
retained; this comparison introduces no alternate text decoder.

The broad bubble-family render survey includes menu reuse and relocated
family layouts. Only the seven natively exercised family-8 identities above
are used to assert follower-symbol differences. Field and landmark comparison
canvases mask undrawn cells; metatile `$FF` is real artwork, not a blank.
All four Western releases match for every retained render. This does not
extend that result to every unexamined SIM or boss resource.

Evidence manifest SHA-256:
`66ae975a7c21b0c90c85c4e73d4fecc374684e77c723408d930c3316fd552571`.
Native results:
`69a8659ba846d0814e734a34a30748dbcf629d35e8d6cdbd414f41b7cfec126d`.
ROM bytes, decoded assets and research helpers remain private.

### Marahna trap-arrow motion and artwork

Placed types `05/0F` and `05/11` share the trap controller. Its body is
US `$00:DFFF–E046`, JP `$00:E095–E0DC`, EU English `$00:DCFE–DD45`, German
+2 and French +5. It plays states `$30/$31`, allocates a child, selects
state `$29`, sets 1 HP and zero flags, and offsets the child by 16 pixels
along its facing direction and 12 pixels downward. The child uses common
projectile service US `$A655`, JP `$A614`, PAL `$A1FD`.

| Releases | State `$29` entries: visual, delay, horizontal delta, vertical delta |
| --- | --- |
| US and all PAL | `($2C,1,-2,0)`, `($36,1,-2,0)` |
| JP | `($2C,0,-3,0)` |

Thirty-eight isolated native calls exercise every row of the projectile
and both trap states, in both facings, across the five ROMs. Five booted
Marahna room-4 fixtures then verify actual child positions on consecutive
frames: deltas are ±2 in US/PAL and ±3 in JP, with the expected visual sets.
The player is held at a controlled position after ordinary native room loads;
enemy logic, RNG, resource streams and per-frame updates are unmodified.
HP, lives and private SRAM remain intact. Both runs reproduce exactly.

This establishes speed per game frame, not equal real-time speed between
50 Hz and 60 Hz releases. Artwork and motion share the native animation
stream: a future appearance-only toggle must preserve the selected gameplay
velocities rather than blindly replacing that entire stream. Existing live
projectiles should retain their chosen policy through cleanup.

The host speed option uses the complete-room snapshot for both trap sources
`$DFE5/$DFF3` and their copied-source children. State41's two Western rows keep
their visual and stored delay1; only horizontal magnitude2 becomes3. Actual
JP data has one row with delay0, so importing its entire sequence would also
silently change flashing. US/PAL and JP velocities are checked against all
five ROMs while explicitly retaining the selected Western timeline.

This comparison also identified an independent collision difference: both
Western arrow compositions have left/right/top/bottom extents `8,8,8,0`,
whereas JP's single `$2C` composition is `16,16,8,0`. These are decoder input
bytes before facing mirroring. They belong to the A04 collision policy, not
the B12 speed or P01 artwork selector. The B12 speed adapter retains native US
extents unless the independent room-scoped collision policy selects Japanese
bounds. Both US flashing poses map to those same Japanese bounds; this does
not import Japanese art.

### Kasandora swordsman collision extents

The ordinary Kasandora Act 2 bundle has five changed composition collision
headers. Extents below are left/right/top/bottom before horizontal mirroring:

| Visual | US and all PAL | JP |
| --- | --- | --- |
| `$08` | `16,15,20,0` | `16,16,16,0` |
| `$0E/$0F` | `16,16,32,24` | `16,16,31,24` |
| `$16/$17` | `28,12,24,24` | `36,12,24,24` |

These affect 11 state programs: `$0A/$0E/$0F/$10/$11/$13/$14/$16/$17/$18/$1A`.
Their visual choices, delays and movement entries otherwise match. The
native animation decoder reads collision extents from the composition
header, independently of the number of sprite parts.

The two placed swordsman types are `03/10` and `03/11`. Their US records
start at `$00:C961/C9BE`, JP `$00:C9EC/CA49`, EU English `$00:C628/C685`,
German +2 and French +5. Reviewed bodies follow the 12-byte spawn records.
The first type selects state `$0E`; the second uses `$17/$18`, all reaching
the changed extended-sword poses. This is not merely a change in an unused
composition.

Five hundred ten isolated native calls exercise every row in all 11 changed
states, both horizontal facings and all five ROMs. Ten booted fixtures
exercise both placed swordsman types in each release, with a held player
target and unmodified native enemy logic. All reach visual `$16` or `$17`
and publish the region's expected 28/36-pixel extent. The native player
survives and private SRAM is unchanged; the suite repeats exactly.

The booted cases establish use of those headers, not every possible sword
contact or a complete fight. The additional `$08` header is verified through
the native decoder without assigning it an unsupported enemy name. An
appearance-only option should not import donor collision headers unless
the corresponding gameplay variation is also selected.

The host's collision policy now projects these headers at the shared `$8E2F`
row-acquisition boundary. It identifies the two swordsman sources above and
the type`03/0D` source`$C863` state10/visual8 path. The latter is retained in
the resource/controller but its ordinary entry unconditionally branches past
the state10 selection; supporting that header does not establish natural use.
Fresh extents are mirrored with `+$28` into `+$0A/$0E/$0C/$10`; the native
reader still owns pose, duration, motion, registers and return state. Header
signatures and bounded sequence/composition offsets reject incompatible assets.
No shared animation or composition bytes are rewritten.

The first pose has a separate `$969E→96A1` initializer continuation using the
descriptor restored in Y. The reader recognizes its real `$969D` return word
and leaves this call to that birth hook; it cannot mistake a stale `+$32` for
the new enemy's identity. Extents are installed before the original bottom
anchoring. All changed headers here retain their native bottom extent, so this
does not move the authored spawn position. Later rows use the installed source.

Native contact `$8970–8A3B` consumes these four extent words. Sword/magic
resolution `$8A3C–8B66` also reads victim extents, then checks **attacker**
composition parts through `$8B67`. The policy does not replace that narrow
test, damage subtraction, deflection, rewards or death. It is independent
from the Marahna arrow-speed rule and from graphics. Room/retry capture keeps
current enemies stable after a settings edit; old companions default to US.

### European SIM helper and behavior-resource follow-up

The 22 external targets from the earlier state-program comparison now have
reviewed direct-helper coverage. This adds 112 Go windows across US and the
three PAL ROMs, including allocation/dispatch, target search, proximity,
map lookup, object-list search and the complete RNG helper.

All changed instructions in those windows are explained by these mappings:

| Purpose | US | PAL |
| --- | --- | --- |
| Data-bank setter | `$00:8519` | `$00:8431` |
| Shared effect/spawn selectors | `$033C/$033D` | `$033E/$033F` |
| Frame phase used by sliced object search | `$88` | `$89` |

The data-bank setter itself is byte-identical. Target quadrants, proximity
limits, search sizes and RNG arithmetic do not change in these windows.
The world dispatcher still calls other actor classes indirectly; this is
not an all-class dispatch audit.

All 52 streams selected through `$01:E099` match exactly, including 173
animation/movement entries and their terminators. The 64 visual-table entries
at `$01:E7D9` also match: 63 bounded compositions and visual zero's `$831C`
control/sentinel value. Character pixels and palettes are separate assets.
The checked direction tables and six-town object-list bases also match.

Another 692 isolated native calls execute every behavior entry through
`$01:D08F` in each Western ROM. They agree on selected composition, duration,
signed velocities and resulting position. Code, resource and native results
repeat independently.

The subsequent shared-effect and seven-class comparisons below cover those
direct entries and the nonempty structure dispatch paths. Complete destruction transactions, other actor classes,
event wrappers and scheduling remain separate checks. Identical local
behavior does not imply identical 50/60 Hz wall-clock pace.

### European shared effects and structure redraws

The `$01:A227` table contains 73 effect families with 397 variant entries,
which resolve to 205 distinct scripts. All entries, 308 drawing rows, 250
control/termination records and 284 referenced sprite compositions match
across US, European English, German and French. This compares drawing
metadata, not the underlying character pixels or palettes.

The native initializer `$01:AC36–AC6F` and stepper `$01:AC70–ACD8` also
match after the known data-bank-setter and `$033C/$033D` selector relocations.
Initialization installs the selected script in the current and loop pointers,
clears the loop count, and starts the duration at one. The stepper supports
duration/composition rows, `$FF` loop setup, `$FE` decrement-and-repeat, and
`$FD` hide. A zero loop count underflows to 65,535; it is not interpreted as
an immediate loop exit.

The field redraw wrapper `$03:BEF7–BF14`, shape selector `$03:A15D–A17D`,
shape restore `$03:9F8D–9FA4`, and queue service `$03:A4A8–A4F6` are identical
in the four ROMs. They keep the same structure flags, redraw-suppression
gate, 128-entry animation-pool addressing and table pointers. The full-pool
wrapper `$03:9E4D–9E6A` changes only its data-bank-setter address. The sliced
entry `$03:9E6B–9E8F` changes only the frame-phase address `$88` → `$89`.
The seven-class dispatcher remains at `$03:9E90–9EF4`.

A further 5,532 isolated native fixtures cover every variant initializer;
each distinct script after 1, 2, 16 and 256 stepper calls; field subtype
bits; redraw suppression; both ends of the animation pool; shape-restore
inputs; and empty-pool traversal for all six towns. Every result repeats
exactly and agrees across the four Western ROMs. Calls deliberately
continued after a script hides test the helper, not whether its scheduler
would continue calling it. Synthetic shape combinations likewise do not
establish natural reachability. Empty-pool traversal does not establish
parity of all nonempty structure-class handlers or town-event lifecycles.

### European nonempty structure-action matrix

The seven structure roots, their eight-entry RTS-minus-one action tables,
and the bounded handler/redraw windows are byte-identical in US, European
English, German and French. Roots in bank `$03` are `$A011,$A35E,$A0CB,
$A19B,$A237,$A296,$A2EF` for classes 0–6. Inline action tables are data,
not disassembled as instructions. The redraw VM is `$03:A4F7–A590` and
its cell renderer is `$03:A591–A5DA`.

Another 10,752 isolated calls cover every class/action, subtype bits
`$00/$10/$20/$40`, all six towns, and redraw suppression on/off. Each
matching US/PAL call leaves the entire high WRAM bank identical, including
the structure record, animation queue, town growth and RNG state. All 256
Go windows and native results reproduce independently.

These fixtures place one structure at `(8,12)`, seal all lairs, leave the
bridge-event gate off and use one RNG seed. Some subtype/class combinations
are synthetic. This closes the bounded handler matrix, not natural town
development, event eligibility, every downstream service or full scheduling.
The additional identical intervals `$03:9ED3–A62E` and `$03:D4D2–DCFF`
contain mixed code/data; their hashes are not a decoded ownership claim.

### Aitos humanoid pose order

Aitos's ordinary animation resource has two four-row sequences whose first
and last pictures trade places between JP and the Western versions:

| Decimal state | US / all PAL visuals | JP visuals | Stored delays | Active duration |
| --- | --- | --- | --- | --- |
| 10 (`$0A`) | 1, 5, 7, 3 | 3, 5, 7, 1 | 5, 19, 5, 19 | 52 frames |
| 45 (`$2D`) | 0, 4, 6, 2 | 2, 4, 6, 0 | 7, 11, 3, 11 | 36 frames |

All rows have zero velocity. Corresponding durations and collision extents
are unchanged. Eighty isolated native decoder calls verify both sequences,
every row and both facings across the five ROMs.

State 45 also has positive runtime ownership: placed humanoid `04/04`
selects it in five 600-frame observations after native room loads. Its
source records are US `$00:CE48`, JP `$00:CECD`, European English
`$00:CB0F`, German `$00:CB11`, French `$00:CB14`. The captured bodies
start twelve bytes after each record and end 164 bytes after it. The
held-player fixtures reach the four pictures in the regional order with
unchanged SRAM. This establishes a live pose-order change, not a movement
speed change or whole-controller equivalence.

State 10 also has a native caller: placed ordinary enemy `04/03`, with source
records US `$00:CE39`, JP `$00:CEBE`, EU `$00:CB00`, DE `$00:CB02`, FR
`$00:CB05`. Its three-byte entry at source+12 branches to the controller
shared with Bloodpool `02/03`: US `$00:B57D`, JP `$00:B611`, EU `$00:B235`,
DE `$00:B237`, FR `$00:B23A`. The captured family is 157 bytes long.
Successful auxiliary allocation selects state 10; it is not an Aitos-local
controller or the boss's same-numbered `$5000` animation.

Five additional 900-frame held-player traces, each independently repeated,
observe eight complete native state-10 sequences per ROM. Each plays the
four regional pictures for 6, 20, 6 and 20 updates, respectively. Native
room loading supplies the `$4000` resource and placed enemies; their states,
timers and controllers are not injected. SRAM is unchanged. This closes
the ordinary state-10 ownership gap, not all branches of the shared controller.

### Extra Palace music resource: silent upload

The additional Western Palace selector-4 declaration references a small SPC
upload, not an extra composition. **The payload itself is present and used
in all five releases**, including Japan. US/JP source is file `0x32B8F`
(`$06:AB8F`); all PAL versions use file `0x3338F` (`$06:B38F`). It has
85 bytes in five segments: destinations
`$2E48/$2F00/$1200/$2C30/$11FD`, sizes 6/24/49/4/2. It occurs only in the
Palace declaration among each Western ROM's 59 scene command streams.
No JP scene declaration has that payload; this is not an inventory of direct
music-table requests.

#### Established debug and ending uses

All five music pointer tables contain the same payload at zero-based entry
20, selected through the indexed playback API as **ID 21**. Western Music
Mode exposes it as selection 21. Japan's menu stops at 20, but its ending
still calls ID 21 directly. The [ROM map](rom-map.md#debugging-and-unassigned-routines)
records all five menus, tables and playback routines. Their index calculation
is `(ID - 1) * 3`; menu selection 21 is not scene selector 4.

The normal ending-transition routine is US/all PAL `$02:84EC–8550`, JP
`$02:84A8–850C` (end-exclusive). It checks progress equals 8 at US `$0347`,
JP `$0335`, PAL `$0349`, invokes the audio-control handshake and explicitly
plays ID 21. It then waits, posts an effect, advances the departure/fade
animation and requests scene `$0801` (credits). The direct playback call is
US/all PAL `$02:84FF`, JP `$02:84BB`; France uses the relocated player
`$02:98A1`. This use is **before the credits**, not a stop-on-debug-exit or
post-credits restart operation.

The US predecessor is also mapped: `$01:8810–8817` branches on completed
Death Heim progress 7 to `$01:8849–885E`. After dialogue it requests world
scene 9, writes scene selector 3 and increments progress to 8. Main-loop
scene-9 dispatch `$00:8129` calls the ending-transition routine. The ending's
direct player does not write selector 4 or depend on its Palace declaration.
These are static control/data joins, not a new full-ending playthrough.

The 49-byte sequence block contains seven populated channel pointers and a
zero eighth pointer. Its channel bodies repeat `60 C9 00`, with an initial
`FA 01` command on the first. The retained SPC note-on gate at ARAM
`$04C3–04D5` skips note-on for event `$C9`; the image also retains
instrument/sample setup. No earlier audible composition has been recovered.
The current data and ending caller do not establish that a song was cut.

#### Playback evidence and remaining declaration question

Ten forced scene-load tests request selector 0 or 4 in each ROM, then run
600 native frames. All four Western selector-4 cases produce exactly zero
PCM samples during the final two seconds; ordinary selector 0 produces
nonzero Palace music. JP has no selector-4 match and retains the incoming
Fillmore music instead. This measures the settled music output, not whether
every subsequent sound effect would be silent. All traces, audio hashes and
results reproduce with unchanged SRAM.

Reachability of the **Palace selector-4 declaration** remains qualified,
separately from the established direct ending use. The configured US
cross-reference inventory identifies eleven direct writers of `$0334`. Its known literal-4
writer, `$00:A370`, selects scene 9 rather than Palace scene 7. Scene 9 has
no selector-4 music declaration, and the loader clears `$0334` before the
later Palace load. This agrees with the previously observed Death Heim
emergence route: stage-clear music persists, then normal Palace music loads.
The forced selector-4 test does not prove an ordinary route through that
declaration. Nor does a configured direct-write inventory rule out every
indirect or indexed writer. Twelve exact Go windows preserve this distinction.

The final bounded search includes overlapping word writes at `$0333–0335`
and long WRAM mirrors. The US configured inventory still contains the same
eleven writer PCs, with no decode issues. A separate raw-operand scan across
all five ROMs retains 101 candidates (US/JP/EU/DE/FR: 20/13/21/23/24), each
decoded individually by Go and explicitly **not** treated as rooted code.
The nine extra US matches are outside the eleven established writers; no
new producer is proved by those byte matches. JP's selector address is
`$0322`; US configuration was not applied to the other releases.

The computed town-song writer is now bounded by its scene-entry guard:
US `$00:8276–8292`, JP `$00:8270–828C`, all PAL `$00:818D–81A9`
(end-exclusive). It copies `$7F:91A3[current town]` only for mode 0 with
subscene below 7 or equal to 8. Palace subscene 7 and emergence subscene 9
skip the copy, as do action modes. Sixty isolated calls cover six scene
choices and town-song values 0/4 across all five ROMs. This rules out that
particular computed copy as a Palace-4 producer; it does not establish a
range restriction on every possible town-song value or every selector writer.

Reachability of this particular declaration remains parked, not declared
impossible. Reopen with a route or trace that carries selector 4 into the
Palace loader, or with a rooted computed/indexed writer. The resource's
identity and direct ending use are established; it should no longer be
listed as unused audio. The [instruction-level follow-up](#instruction-level-resource-selection-checks)
below confirms the selector's clearing on the controlled US emergence route
and checks five-ROM Palace excursions for indirect writes.

### Action resources without a proven gameplay owner

These are resource-usage questions, not evidence of graphical bugs in play.
The bounded search covers authored initial states, complete placed log
controllers, and exact immediate-state calls to the region's set/play/repeat
helpers. All 193 call-pattern candidates are retained as candidates; a
matching state number does not establish which animation bundle is resident.

| Resource | Established result | Missing evidence |
| --- | --- | --- |
| Western Bloodpool Act 1 state 23 (decimal) | Contains a floating-log animation. Placed records `02/00` and `02/01` start at states 18 and 22 and explicitly play 18 and 20; their checked bodies do not request 23. No authored initializer uses 23 with this bundle. The nearby ordinary state-23 caller belongs to `02/21` in room `$0602`, with the different Act 2 bundle. | A controller reaching state 23 while the Act 1 bundle is resident |
| Japanese Bloodpool Act 2 states 26/27 (decimal) | Reference visual 63 beyond that composition table; Western counterparts reference blank visual 58. No authored ordinary-slot initializer selects them. The known Wizard callers use the separate `$5000` boss bundle, not this `$4000` bundle. | A native ordinary actor selecting either state with the affected bundle; forcing an invalid resource is not a gameplay witness |
| Fillmore Act 2 visuals `$28–2B` | All four now decode with the correct shared graphics bank, but have no animation-state references in any of the five ROMs. | A native direct-composition or other owner selecting these pictures |

The five-ROM log-body captures start at US `$00:B4A5/$B4CC`, JP
`$B539/$B560`, EU `$B15D/$B184`, DE `$B15F/$B186`, FR `$B162/$B189`.
Each pair has 27/45 bytes. The complete `02/21` body is also captured;
the Wizard's resident-slot identity comes from its record and the separately
verified boss controllers. Computed state choices and direct composition
writes remain outside the literal-call search. The follow-up below tests
specific remaining callees and resource structure, without assuming a caller.

Together with the Fillmore path test and the Aitos exits below, this terminal
batch adds 95 isolated native calls and 349 bounded Go windows. All 17
output files reproduce exactly. Raw candidates are not additional native
tests, and the earlier exploratory run is not counted again.

#### Log contact helper and animation termination

The placed logs call a shared helper that allocates a copied child record.
That child checks player contact and carries the Master with the platform;
it does not select a new animation in the checked paths.

| Entry | US | JP | All PAL |
| --- | --- | --- | --- |
| Contact-child setup | `$00:A66A` | `$00:A629` | `$00:A23F` |
| Contact update | `$00:A686` | `$00:A645` | `$00:A25B` |
| Copied-child allocator | `$00:853D` | `$00:852C` | `$00:8455` |
| Animation-row decoder | `$00:8E2F` | `$00:8E3B` | `$00:8967` |

Ten successful child-allocation calls retain the parent's state 18 or 22
and `$4000` animation base. Eighty contact tests cover accepted heights
28–36, the horizontal boundary, excluded player flags and upward motion;
neither parent nor child changes animation state. Fifteen decoder calls at
the ends of states 18/20/22 reset object `+$1C` to zero and return carry set,
leaving state `+$1A` unchanged. There is no automatic fall-through to 23.

The Western extra state 23 has eight rows, all visual 22, with delays
`0,7,0,7,0,7,0,7` and Y velocities `1,0,1,0,-1,0,-1,0`. Thirty-two native
row-decoder calls confirm these values. With the established delay-plus-one
contract, the authored rows describe 36 active updates, a two-pixel downward
excursion and zero net displacement. These are deliberately selected rows,
not a naturally selected log or a new observed gameplay feature.

#### Cave composition identity and Bloodpool placeholders

The four cave definitions `$28–2B` are byte-identical across all five ROMs.
Their part counts are 4/4/5/4; shared-character-bank parts account for
4/4/3/4 of those parts. The third also uses two room-bank parts. An exact
definition survey of all 32 loaded action-animation assets plus the 55
Master compositions in each ROM finds each definition only at its existing
cave entry. None is referenced by the cave animation programs. This is not
a search of all possible raw ROM data, nor a claim that their tiles are
unique or that the neutral offline render supplies the intended attributes.
The captured native decoder also adds object `+$3C` to the authored visual
and masks the result to eight bits. A composition absent from the animation
rows can therefore still have an indirect owner; the unreferenced-row result
is not a global unused-content proof.

In the JP Bloodpool Act 2 ordinary bundle, the composition-pointer table
starts at decoded offset `$0338` and has 58 entries. Visual 63 instead reads
the word at `$03B6`, beyond that table; the word is zero. Ten isolated native
decoder calls for states 26/27 across the five ROMs resolve the JP picture
to `$7E:4000`, the bundle header. The Western bundle has 59 entries and
resolves its valid visual 58 to `$7E:4C78`, the previously rendered blank.
The paired one-row sequences occupy offsets `$0230/$0235` and have identical
contents within each release. They resemble placeholders, not two recovered
hidden animations. No invalid actor was spawned to manufacture a picture.

These results strengthen the candidate descriptions, not their natural
reachability. There is still no correct-bundle native owner for log 23,
ordinary Bloodpool 26/27 or the four cave pictures. No beta build or removed
feature history has been established. The reader-facing
[unused-content catalogue](unused-content.md) keeps these qualifications
with the findings.

### Instruction-level resource selection checks

Twenty-two controlled scene traces follow actual music-selector writes and
animation selections in the original ROMs. Each has an uninstrumented
comparison; final serialized state, WRAM, SRAM and the all-frame video hash
match exactly. An independent repeat reproduces all 246 output files
(75,889,578 bytes). These are 22 additional booted scenarios, not 44 because
of the observer controls, and not additional isolated-routine cases.

| Scenario | Releases | Setup and observation |
| --- | --- | --- |
| Fillmore → Palace → world map → Palace → Fillmore | All five | Button-only excursion from an existing controlled Fillmore state; 1,764 frames each |
| Bloodpool Act 1, first room | All five | Original room loader; health supplied, then neutral/right/attack/jump inputs; 1,996–3,000 frames including loading |
| Fillmore Act 2, first cave room | All five | Sequential native loading through rooms 1 and 2; same input rule; 3,600 frames each |
| Bloodpool Act 2, first room | All five | Sequential native loading through rooms 1–3; same input rule; 3,786–4,200 frames |
| Death Heim completion and emergence | US and JP | Previously accepted staged final-boss completion; 3,600 frames each |

No new player, camera or actor coordinate writes are used. The action traces
are bounded traversal attempts, sometimes ending in death or stalling, not
completed stages. The emergence setup supplies boss-completion and campaign
state; it is not a natural full-campaign witness.

The observer temporarily wraps instruction dispatch in the pinned Snes9x
core, without modifying ROM bytes. Go disassembly identifies these exact
observation points:

| Point | US | JP | European English | German | French |
| --- | --- | --- | --- | --- | --- |
| Scene music selector read | `$02:B64B` | `$04:8653` | `$02:BC43` | `$02:BC4C` | `$02:BC35` |
| Loader selector clear | `$00:83F2` | `$00:83EC` | `$00:830A` | `$00:830A` | `$00:830A` |
| Decoded composition-pointer store | `$00:8EA5` | `$00:8EB1` | `$00:89DD` | `$00:89DD` | `$00:89DD` |

Store, read-modify-write and block-move instructions are also observed for
actual changes to the selector word, including executed indirect/indexed
writes. For animation events, the entire resident ordinary `$7E:4000`
bundle must match its pinned decompressed source before attributing a
selection to one of the three suspect resources. State numbers alone are
not used to infer ownership. The batch retains 73,438 instruction events
and 15 single-instruction Go windows.

#### Palace result

In the US emergence trace, `$00:A370` writes selector 4 at frame 1,199 while
requesting scene 9. At frame 1,243, `$00:83F2` clears it during that scene's
load. The Palace music declaration reads selector 0 at frames 1,795 and
1,806. Thus the known literal-4 write does not reach the Palace declaration
on this route. All five button-only Palace excursions likewise read 0;
none exposes another selector-4 producer.

#### Animation result

None of the matched-bundle decoder events selects the extra log state 23,
the ordinary Bloodpool Act 2 states 26/27, or cave pictures `$28–2B`.
The trace does observe the suspected indirect-picture mechanism in use:
Bloodpool Act 1 actors have `+$3C` values `$0200/$0201` while states 4/11
resolve pictures 7/8. The decoder's eight-bit mask makes this an ordinary
two-picture alternation, not an index hundreds of pictures beyond the table.
It occurs in all five releases and supplies no owner for the questioned
resources. Log **state** 23 must also not be confused with ordinary
**picture** 23, which is observed under a different state.

These traces narrow the remaining usage questions; they do not prove global
unreachability. Direct custom composition paths, unvisited actor states,
and writes outside the observed CPU mechanisms remain outside the check.
No graphics-removal decision, new game bug or additional regional behavior
is inferred from the absent selections.

### Aitos room exits and the unassigned lava claim

The remaining position-gated direct handlers do not contain a lava timer.
They request another room using cached player coordinates. Their tested
boundaries match in all five ROMs:

| Direct handler | Condition | Requested area/room |
| --- | --- | --- |
| `04/01` | X ≥ `$0FF0` | `$0402` |
| `04/02` | X ≥ `$06F0` | `$0403` |
| `04/0E` | X ≥ `$02B0` and Y ≥ `$0370` | `$0405` |
| `04/0F` | X ≥ `$01F0` | `$0406` |
| `04/10` | X < `$0420` and Y ≥ `$0380` | `$0407` |

The first two US entries are `$00:CDE3/$CDF1`, JP `$CE68/$CE76`,
EU `$CAAA/$CAB8`, DE `$CAAC/$CABA`, FR `$CAAF/$CABD`. All 85 boundary
fixtures leave the pending scene unchanged on rejection and select the
expected room on acceptance. Coordinates are `$80/$82` in US/JP and
`$81/$83` in PAL; these are not frame-count thresholds.

This eliminates these exits as another candidate for the broad slowdown
report. Earlier tests already cover molten rocks `04/05`, bamboo traps
`04/06`, rising fireballs `04/08` and horizontal parallax `04/00`. The claim
remains unsupported, not disproved for every possible effect. A clip or
precise room/object description is needed to identify another mechanism.
The separately checked background-animation machinery is described below.

### Aitos background animation and video profiles

All five ROMs select video profiles `$16–1C` for Aitos rooms 1–7. Each
28-byte profile is identical across the releases, including parallax ratios,
background-page animation settings and character-animation transfer settings.
Thirty-five native command-handler calls verify the resulting animation
configuration. PAL fixtures initialize the Normal timer reload to 59; PAL
reads that configured value, whereas US/JP writes 59 directly. Equal video
profiles do not erase the separately documented difficulty-dependent clock.

| Routine or table | US | JP | European English | German | French |
| --- | --- | --- | --- | --- | --- |
| Video profile table | `$02:893E` | `$02:87E7` | `$02:893E` | `$02:893E` | `$02:893E` |
| Video command handler | `$02:B4E8` | `$04:84F0` | `$02:BADF` | `$02:BAE8` | `$02:BAD1` |
| Background-page cycle | `$02:BC27` | `$04:8C22` | `$02:C21F` | `$02:C228` | `$02:C211` |
| Character-animation transfer | `$02:BC56` | `$04:8C51` | `$02:C24E` | `$02:C257` | `$02:C240` |

The page-cycle routine updates two mask/counter pairs. In Act 1 rooms 2/3,
the first mask is `$0C` and its packed counter starts at `$40`; the second
mask is zero. The first page offset advances `4,8,12,0`, holding each value
for five service calls, then repeats. Room 1 and all four Act 2 profiles
leave both page masks zero. Twenty-one consecutive native calls per profile
and ROM (735 total) confirm the same cycle and inactive cases. These are
service-update counts, not a claim of equal wall-clock timing on PAL.

All three Act 1 profiles also set character-animation transfer length
`$E1` to zero and masks `$DE/$DF` to `$FF` (PAL direct-page addresses are
one higher). Another 105 calls sample seven frame-counter phases per
profile/ROM and confirm zero scheduled transfer bytes. The page animation
above is separate; a zero tile-transfer length does not mean a static scene.

This excludes these authored settings and checked background services as a
US/JP lava-speed difference. It does not identify the reported effect or
disprove every possible meaning of “lava slowdown.” No new player or camera
warp was used.

The resource, log-callee, scene-guard and Aitos-profile follow-up comprises
1,082 isolated native calls and 35 bounded Go windows. Its six JSON reports
reproduce byte-for-byte in an independent run. It adds no booted-play traces
and does not convert a deliberately selected resource into a reachability
witness.

### Remaining ordinary-enemy timing and motion

The next keyed-controller batch covers Fillmore `01/19`, Bloodpool `02/23`,
Kasandora `03/06` and Marahna `05/20`. It adds 554 isolated original-animation
calls, 28 booted traces and 25 exact Go windows. All results repeat; the
player is held after native room loads, with no per-frame enemy edits.

| Family | US | JP | All PAL |
| --- | --- | --- | --- |
| Fillmore cave emitter | Previously verified 360-frame cadence | Previously verified 180-frame cadence | 255-frame cadence |
| Bloodpool skeletal swordsman, state `$1D` | 62 active frames | 46 | 62 |
| Same enemy, state `$1E` | 63 | 55 | 63 |
| Marahna retracting head, state `$23` | 16 | 20 | 16 |

PAL cave-emitter records start at EU `$00:B069`, German +2 and French +5.
The EU body `$B075–B100` plays state `$24` once: its single row stores
duration 254. The US plays its two duration 89 rows twice; JP plays those
two rows once. Both active PAL emitters in the held-player fixture produce
children on frames 257/512/767/1022, a 255-frame repeat interval.

PAL also replaces the unconditional X−8 adjustment with X+6 below `$0380`
and X−22 otherwise. All twelve loaded emitter roots have the expected
adjusted X. In the active pair, authored X832/976 becomes 838/954, versus
824/968 in US/JP. Original placement records remain the same; replacing only
their coordinates would miss the regional controller adjustment.

Bloodpool records start at US `$00:BBA8`, JP `$00:BC3C`, EU `$00:B860`,
German +2 and French +5. The nine-entry straight attack's last delay is
31 in US/PAL and 15 in JP. The five-entry high attack ends with delay 31
versus 23. Both choices were reached natively with different player heights.
These are complete state durations, not whole fight or jump-cycle periods.

The host's room-stable motion adapter implements the two swordsman delays
independently. Only state29 row8 and state30 row4 of source `$BBA8` in map
group2 are eligible. The original reader still selects the pose and collision
header. Current attacks and later spawns keep their room snapshot until the
next full room load/retry; no Japanese artwork is required. All fourteen rows
of both attacks are included in the five-ROM and controller comparisons above.

Marahna head records start at US `$00:E3A1`, JP `$00:E422`, EU `$00:E0A0`,
German +2 and French +5. State `$23` is US/PAL visuals `$28,$26,$25` with
delays `7,3,3`; JP inserts visual `$27`, delay 3, after `$28`. Native playback
therefore differs by four frames. This belongs to the ordinary room-6 head
enemy, not the Act 1 plant boss or Viper.

All four compositions37–40—including the inserted39, its four sprite parts,
and bottom extent21—are retained byte-identically in the US bundle. The host
withdrawal policy therefore needs no Japanese artwork. The room-pinned reader
adapter maps the expanded logical sequence onto existing native rows/poses;
native motion, mirrored extents and end-of-sequence handling still execute.
It borrows only the current actor's row and visual-offset words during the
non-yielding decode, never shared asset bytes. A tagged logical cursor in
`+$1C` lets an in-flight sequence finish after a debug restore without depending
on a host-only counter; native state initialization and the terminator clear
it. Sources, room, complete US sequence and retained pose header are checked
before applying this mapping. It is not a general-purpose donor-table editor.

Paired held-player host runs follow the native Marahna room-loading chain and
verify the resident ordinary animation bundle before measuring the sequence.
They observe the three US poses over16 updates and four JP poses over20,
then an untagged cursor in the next native state. Both exit normally. Earlier
direct-warp fixtures retained the previous area's animation data and were
discarded. These checks establish the animation lifecycle, not natural
traversal, full contact/combat parity or manual visual acceptance.

### Kasandora fire-enemy motion and spawn decisions

Type `03/06` is the ordinary floating fire enemy, not the Act 2 blue sphere.
Records are US `$00:C3A5`, JP `$00:C43A`, EU `$00:C06C`, German +2 and
French +5. Its original state `$0C` movement matches. The close-player
branch differs:

- US/PAL play `$0D`, then `$27` twice, then `$0E` before turning. Native
  traces measure 16, 64, 16 active frames. `$27` is a small hovering loop,
  not a completely stationary wait.
- JP chooses `$0D` or `$0E` according to facing, then turns and returns to
  `$0C`. Each chosen movement lasts 16 active frames; there is no `$27`
  hover. Its vertical velocities reach ±4, versus ±3 in the Western curves.

Ten held-player traces cover targets on both sides in all five ROMs. The
animation-state data and native velocities match the regional programs.
The newly added US/PAL state `$27` is thus used by a live controller, not
merely an extra unreferenced animation record.

After its movement, the controller obtains a random byte and selects a
child type. Another 2,560 isolated native calls exhaust all 256 possible
returned values, in every ROM, with a free slot and with a full actor pool:

| Returned byte | US / PAL decision | JP decision |
| --- | --- | --- |
| `$00–7F` | No child | No child |
| `$80–9F` | No child | Horizontal flame |
| `$A0–D1` | Horizontal flame | Horizontal flame |
| `$D2–F1` | Horizontal flame | Bouncing flame |
| `$F2–FF` | Bouncing flame | Bouncing flame |

With a free slot, US/PAL split the 256 inputs into 160 no-child, 82 horizontal
and 14 bouncing decisions. JP splits them 128/82/46. A full pool produces
no child for any input. These are decision-space counts, not proof that
the RNG is uniformly distributed or a fixed firing rate per minute.

The tested decision entries are immediately after the RNG call: US
`$00:C402`, JP `$00:C491`, EU `$00:C0C9`, German +2 and French +5. Original
allocators and child-handler assignments execute unchanged. Horizontal/
bouncing children use US `$C447/$C429`, JP `$C4D6/$C4B8`, PAL
EU `$C10E/$C0F0` with the same language offsets. All results reproduce.

The runtime implements four room-pinned policies: movement curves,
close-player strategy and the two child-choice thresholds. The guarded
`$8E2F` reader adapter projects only verified US rows13/14 into Japanese
velocities, preserving native visual/duration outputs and facing mirrors.
`$C3DD` handles the already-computed mirrored near-player decision;
`$C3EA→C3F6` skips the Western hover and second curve. Threshold prefixes
`$C405→C408` and `$C40A→C40D` replace only CMP flags after the original RNG
call. Native allocators, child handlers, stack/yield ownership and pool-full
failure paths remain unchanged. Explicit `$C3DA/$C3DD/$C3F9` boundaries
prevent incoming branches from inlining past those hooks.

Curves and strategy can be mixed without importing artwork or replacing
shared animation buffers. Active rooms retain their captured policy through
all parent/child generations; edits apply on the next room/retry. Shared
initialization calls are excluded until descriptor ownership is established.

### European Wizard and Viper branch contracts

The previously checked US/JP Wizard positioning and five Viper auxiliary
bodies now have PAL coverage: 6,912 original native calls across European
English, German and French, every difficulty, Story/Action settings, and
original/rematch resources. Actor and linked-part outputs agree with the
accepted US fixtures after pointer relocation. Eighteen exact Go windows
classify the instruction changes as relocated calls/branches and the cached
player-Y address `$82` → `$83`. Independent repeats match byte-for-byte.

For European English, the Wizard position window is `$00:BBD8–BC6C`.
The five Viper windows are `$00:E28A–E2CD`, `$E2DA–E304`, `$E311–E338`,
`$E339–E379`, `$E37A–E3BC`; German adds 2 and French adds 5 to these
language-dependent addresses. HP and attack are deliberately fixed in these
fixtures to isolate controller decisions; they do not retest regional stat
initialization or establish complete fight equivalence.

Importantly, equal branch logic does **not** make Viper's animation resources
equal. The separate row and running-encounter checks below find PAL changes.

### European Viper lightning and floor attacks

The original encounter uses `$5000` data from file `0xC8000` in US/European
English, `0xC7332` in German/French, and `0xC5F6D` in JP. The Death Heim
rematch uses US `0xCA81A`, European English `0xCA81E`, German/French `0xCA000`
and JP `0xC8800`. All three PAL languages have the same changed programs.
US and JP animation programs agree here.

The main attack-choice prefix is a separate difference: US `$E4DB` uses
`AND #3/BNE`, JP `$E55C` uses `LSR/BCS`, and European English `$E1DA`
uses `AND #1/BNE` (German +2, French +5). Thus JP and PAL select lightning
for the same 128 of 256 byte inputs, versus 64 in US. Their accumulator and
carry results differ, so the integration retains separate choice programs.
All follow the existing native RNG call without drawing a second value.

States 4–6 select the three lightning directions. Their first movement
segment is shortened in PAL:

| Encounter | US / JP stored delay → active updates | PAL stored delay → active updates |
| --- | --- | --- |
| Original Viper | 21 → 22 | 17 → 18 |
| Death Heim rematch | 10 → 11 | 8 → 9 |

The corresponding velocities, visuals and collision extents stay unchanged.
The shorter segment therefore also reduces its travel distance; this is
not merely a cosmetic frame-rate adjustment. The subsequent animation rows
are unchanged.

Original-encounter states 8 and 9 belong to the independently placed floor
parts `05/25` and `05/24`, not actors retaining the main boss's source pointer.
Their second downward segment changes from velocities 1/2/4/6 over
2/2/2/16 updates to velocities 2/4/8/10 over 3/2/2/8 updates. Both travel
110 pixels in that segment, but PAL takes 15 updates instead of 22. The
earlier descent, pause and upward return are unchanged. Rematch resources
do not have this state-8/9 change.

The evidence includes 630 native decoder calls across all five ROMs, every
row of these five states and both facings. Twelve running US/European-English
Normal encounters hold the player at three horizontal positions after native
resource loads. They reach lightning states 4/5 in both encounters and floor
states 8/9 in the original fight. Observed row boundaries agree with the
regional delays; all five-ROM decoder outputs verify state 6 as well. No
per-frame enemy-state edits are made, and SRAM is unchanged. Independent
repeats have identical parsed JSON and trace data; ordering of some report
object keys varies because the private harness iterates a set.

The room-policy integration projects only the changed reader outputs at
`$00:8E2F`: root sources `$E483/$F72A`, rooms `$0805/$0607`, state4–6 row0;
floor sources `$E5CF/$E606`, original room only, state9/8 rows6–9. Exact US
row signatures and `$7E:5000` ownership guard the adapter. The original reader
still owns visuals, collision headers, register effects and coroutine state;
shared animation buffers are not rewritten. The choice prefix `$E4DB` resumes
native `$E4E0` or `$E4F7`. Explicit `$E4D2/$E4D8` boundaries prevent incoming
controller paths from bypassing the hook. Numerical options need no donor art.

Six controlled host encounters verify the integrated US/JP/European profiles
in both original and rematch rooms. They preserve native enemies and RNG,
hold a positioned invulnerable player, and exit normally without modifying
the player's save. Observed state4 lightning segments are22/22/18 updates
and11/11/9 in the rematch. Both original floor parts match their four-row
22-versus15-update descent. Other lightning directions retain five-ROM reader
and adapter coverage; these controls are not full victories or visual parity
tests, and direct debug room requests are not natural traversal evidence.

A separate static inventory now compares all 19 distinct boss-slot program
sets across five ROMs, including row counts, visuals, durations, velocities
and extents. It is a resource-difference inventory, not a claim that every
changed state has a verified live controller or equivalent composition.

### PAL timing interpretation

The reference core reports 60.098811862348406 Hz for the supplied US/JP ROMs
and 50.006978908188586 Hz for all three PAL ROMs. For an uninterrupted
one-update-per-frame sequence, duration is `updates / refresh_rate`.
Equal elapsed time requires a PAL count approximately 0.832079 times the
NTSC count. An unchanged count takes approximately 20.18% longer in PAL.
This conversion does not cover lag frames, suspended actors, extra nested
updates or other explicitly gated consumers.

| Measured sequence | US updates / seconds | PAL updates / seconds | Interpretation |
| --- | --- | --- | --- |
| Viper opening lightning segment | 22 / 0.366 | 18 / 0.360 | Near-equivalent duration; less travel at unchanged per-update velocity |
| Viper rematch lightning segment | 11 / 0.183 | 9 / 0.180 | Same qualification |
| Original Viper floor part, second descent | 22 / 0.366 | 15 / 0.300 | About 18% shorter in elapsed time, same 110-pixel displacement |
| Fillmore cave-emitter interval | 360 / 5.990 | 255 / 5.099 | About 15% shorter in elapsed time |
| Normal action countdown unit | 60 / 0.998 | 60 / 1.200 | Uncompensated refresh-rate slowdown |

Near-equivalent timings suggest compensation but do not establish authorial
intent. No overall percentage of PAL changes has been classified as
compensation. A host implementation should distinguish source gameplay
parameters from source clock/pacing policy: blindly using PAL update counts
with a US-rate clock can reproduce neither release's real-time behavior.

**Planned host policy, approved September 22:** regional options will retain
the existing nominal 60 Hz simulation, including European profiles. Selected
regional update counts will not receive automatic 50/60 rescaling. This is
European rules at normal host pacing, not native PAL wall-clock reproduction;
the original-ROM measurements above remain unchanged.

### Regional action raster tables

Ten placed direct-handler wrappers select the action raster family. Their
US bank-02 entries are `$9204`, `$92D8`, `$931A`, `$9382`, `$93DF`, `$945E`,
`$94E9`, `$9549`, `$95E9` and `$9665`. JP relocates the first seven by
`−$0257` and the final three by `−$026D`. European English/German retain
the US entries; French retains the first seven and shifts the final three
by `−$0016`. These are separately bounded original routines, not a blanket
relocation rule for other bank-02 code.

The nominal 256-byte waveform is identical in all five ROMs. It starts at
US/EU/DE `$02:96D4`, JP `$02:9467`, FR `$02:96BE`. The adjacent 256 bytes
are not equivalent. Aitos room `$0504`'s placed `$04/1B` wrapper selects
US `$02:9382` / JP `$02:912B`. This routine writes the low scratch byte
`$00` but loads a 16-bit index through `$00/$01`. With `$01 = 1`, it reads
the page after the waveform, including neighboring code bytes.

For phase `(tick & 255) >> 2`, its 112 two-scanline HDMA records have
count 2 and value `((source_byte << 4) & $10) | $02`, followed by a zero
terminator. The source is `waveform + $100 + phase + band`. All 64 reachable
phases differ from US in JP, European English and French; German's output
matches US despite different adjacent-page bytes. Only the selected bit
of each byte affects this pattern.

Five native room-load traces enter Aitos rooms 4 then 5 and observe 512
further frames each. In every release, scratch `$01` remains 1 and the
emitted table agrees with the adjacent-page formula at the sampled tick,
covering all 64 phases. This confirms live use, not merely a synthetic
out-of-range input. It is not a measurement of which scanline/frame displays
the generated table; the renderer's existing HDMA pipeline remains relevant.

The Ice Dragon raster uses `$6800` with inline hardware setup in US/EU/DE,
but `$6000` and the common setup in JP/FR. Its generated table agrees after
normalizing the destination and inherited workspace inputs. This does not
prove that every regional scene enters with equivalent workspace contents.
All other non-mosaic raster outputs match under the controlled inputs.

Evidence: 8,320 isolated native calls, 105 exact Go windows and the five
booted traces. Inputs vary frame, camera X, scratch high byte and workspace
pattern; independent repeats reproduce all JSON evidence. Player-only hold
and invulnerability are applied after native room loads; SRAM is unchanged.
The Aitos Act-1 `$04/00` raster is horizontal parallax, not a rising-lava
controller. Neither this result nor the mosaic difference resolves the
separate reported lava-speed claim.

**Host integration:** `action_room_mosaic` stores only the 175 reachable
one-bit values for each of the three distinct patterns. Room entry/retry
captures `aitos_mosaic_pattern` and projects the same immutable values into
the enhanced room source. The guarded native `$02:939C → $02:93BA` seam
replaces the complete 112-band loop, retaining native setup, terminator,
HDMA publication and return. It requires room `$0504`, scratch high byte1
and the expected entry registers; US and unrelated contexts delegate.
The final carry retains the US instruction's source-bit4 behavior, which
is independent of the displayed bit0. No foreign code is imported.

Tests compare all64 phases and eligible status-flag combinations, plus
the normalized values against all five ROMs. The host raster continues
to display the previous tick's table, including the native first-frame
hold. A 13,000-tick US/JP/European settings fixture covers all phases,
pending edits and room transitions: 4,361 compared frames and 4,923,569
raster-register comparisons have no mismatches, including MOSAIC. It also
reports no background tile mismatches or live-source fallbacks. These are
controlled room loads, not a natural full-act playthrough. Simulation
remains60Hz for every profile.

### Remaining European boss-program differences

The five outstanding PAL resource leads now have native row-consumer and
running-controller checks across all five ROMs. These results concern the
specific programs below; they do not establish every boss branch or full-fight
equivalence. PAL live fixtures use Normal Action Mode. Regional graphics
descriptors are compared separately from CHR and palettes.

#### Minotaur wind-ups

Original encounter `$0401`, states 1 and 4, shortens its first stationary
row from 24 to 16 and from 20 to 16 updates respectively. The complete axe
wind-up is 29 → 21 updates; the complete jump program is 54 → 50. The
remaining rows, velocities, extents and composition descriptors match.
The Death Heim program is not changed by these edits.

At source refresh rates, the first jump pause is approximately 0.333 →
0.320 seconds, but the whole jump is 0.899 → 1.000 seconds. Calling the
European jump globally faster would therefore be incorrect. The PAL
controller window is European English `$00:AC13–ACBE`; DE/FR add 2/5.

#### Aitos dragon projectile lifetime

States 1 and 2 retain their respective visuals, collision extents and
velocity `(−3,+1)` / `(−3,−1)` before facing transformation. Their single
row lasts one update in US/JP and 16 in PAL. This is a repeating movement
program, not a one-row fixed projectile lifetime.

US `$00:A655–A669` and JP `$A614–A628` check the offscreen flag `$0400`
after every completed sequence. PAL `$A212–A23E`, common to all three
languages, plays four sequences without that check, then plays a fifth
before its first check. Later checks occur after each further sequence.
It therefore guarantees 80 movement updates before the first offscreen
retirement opportunity, versus one US/JP update. A creation call followed
by the complete native actor-update loop retires an already-offscreen
fixture on invocation 81 PAL / 2 US/JP. Onscreen controls remain active.

The projectile-producing attack still obeys the previously documented
Beginner gate. Its animation delay, mandatory flight interval and actual
offscreen test are distinct mechanics; the delay alone is insufficient
to reproduce the European behavior.

The host's two room-pinned leaves now implement the delay and mandatory
flight interval separately. The native `$8E2F` adapter validates both complete
US one-row programs before changing their delay from0 to15. At `$A655`, the
regional prefix places four remaining repetitions in the high byte of the
projectile's local state selector `+$38`; its low byte stays1 or2. `$A65E`
decrements that count and tails to the real `$A65B JSR $8657`, feeding only
the original state index. Once the count reaches zero, the native flag test,
repeat branch and retirement code resume unchanged. This field is owned by
this projectile controller; its `$8657` helper does not use `+$38`.

Guards require room `$0304`, inherited descriptor `$D646`, animation
`$7E:5000`, matching child state and the real `$A65D` yield-return word.
There is no host timer or synthetic return frame. An initialized count is
honored even without the host room cache after a debug restore, preventing a
packed value from reaching the original animation-index load. The counter is
discarded on native death/slot reuse; no live projectile is restarted by an
overlay request. Beginner's producer gate remains a separate difficulty rule.

#### Marahna head and projectile preparation

The open-head state 2 uses US/JP visuals `3,4`, each lasting four updates.
PAL uses `2,3,4,3`, also four updates each. Five repetitions consequently
give the already-measured 80-update PAL open interval versus Japan's 40.
The added visual 2 has narrower horizontal extents (8/8 rather than 16/8
for visuals 3/4); this is not only a doubled timer.

The linked projectile-launching body uses state 6 when the player is within
24 pixels vertically and state 4 otherwise. Both programs' first rows
last 24 PAL updates versus eight US/JP; their final row lasts one, giving
25 versus nine updates. Native high/low player fixtures reach both branches
in all five ROMs. They are separate from the vulnerable head and the
difficulty-dependent lower tendril.

#### Northwall Act-1 throwing and impact

The PAL asset inserts four composition descriptors at visual ordinals 9–12.
Existing descriptors 0–8 match directly; PAL 13–24 match US 9–20 exactly.
Thus most changed animation visual numbers are relocation within the
composition table, not new poses or altered hitboxes.

Two programs do change substantially:

| Program | US / JP | PAL |
| --- | --- | --- |
| State 2, throw preparation | Seven poses, 50 updates total | Same seven composition descriptors, 15 updates total |
| State 1, projectile impact | Six poses, 42 updates total | Ten poses, 20 updates total |

PAL impact adds four expanding poses. Its last pose has horizontal extents
32/32, versus US/JP's 16/16, so the final collision footprint doubles in
width. PAL also moves the impact object down two pixels at activation.
The falling projectile's initial horizontal offset changes from −8 to −16
pixels before facing transformation; its +16 vertical offset is unchanged.
Both facing directions, allocator success/failure and wrapping impact-Y
inputs are exercised with the original routines.

European-English controller `$00:E4D1–E656` includes spawn `$E564–E582`
and impact activation `$E5B4–E5C6`; DE/FR add 2/5. US equivalents are
`$E7D2–E951`, `$E865–E883`, `$E8B5–E8C1`; JP adds `$7F` to these US
addresses. The existing absolute-memory comparisons in the linked visual
controller remain outside these gameplay changes; they are not silently
rewritten as immediate constants.

**Recomp integration.** Four room-pinned leaves select these timelines and
offsets independently. `$00:E878→E87B` changes only LDA/NZ before the real
facing helper. `$E8B5→E8BB` retains STZ, performs the two PAL Y increments and
LDA1, then resumes the actual animation call. No allocation or return frames
are synthesized. The guarded `$8E2F` adapter borrows only the actor's row and
visual-offset fields while the native non-yielding reader runs, then publishes
a high-byte-tagged logical cursor and selected delay.

US file`0xDC622` expands to1549 bytes at`$5000–560C`; its21 contiguous
compositions end exactly there. Command0's `$02:B69C` loader writes the stated
length; graphics/raster workspace begins at`$6000`. Only this validated
room0406/sourceE7C6/base5000 profile may populate the four64-byte slots at
`$5F00/$5F40/$5F80/$5FC0`. Each added pose repeats the seed pose's two CHR
references`$023E/$023F`; complete5–8-part records match all three PAL ROMs.
Existing asset bytes and ordinals remain intact. Native OAM, host widescreen
and collision consume the same full composition pointer/extents. No donor
pixels, general-purpose WRAM allocation or renderer-only substitution.

Tests cover81 source mixes, both accumulator widths, all four flips, native
CPU/return effects, cacheless active-program completion, malformed profiles,
escaped-return rollback, wrapping Y inputs and five-ROM row/composition/code
comparisons. Controlled live US/European encounters use cumulative room
loads and a held player, without enemy/RNG edits:24/36 completed impacts by
the three-thousand-update checkpoint, original/expanded widths32/64 and
Y448/450. Both15,500-tick runs exit normally with zero background mismatches.
These are controlled encounters, not natural traversal or full victories.

#### Tanzra second-form minion

State 22 (decimal, not `$22`) belongs to the minion initialized at
US `$00:FC8D`, JP `$FD0A`, European English `$F986` (DE/FR +2/+5).
Its initializer assigns HP and stored score value 2 in US/JP, 1 in PAL,
overwriting inherited values. Live minions confirm this; the boss root's
health is a separate field/owner.

The stationary turning sequence uses US/JP visuals `31,33,35,37` versus
PAL `35,37`, four updates per pose: 16 versus eight updates. The latter
two composition descriptors and their extents match the US poses. After
the sequence, the controller faces the player and selects movement state
23/24/25 according to vertical distance. Removing the first two poses
therefore changes both the interim collision footprint and when the next
direction is chosen, not just the displayed animation.

The host's independently selectable turn policy now advances the US state22
cursor from row0 to row2 before the native reader. All four original row
signatures must match before applying it. The reader then acquires the actual
remaining pose and collision header; no donor graphics or fabricated pose
metadata are needed. The phase-two stats have their own initializer policies,
so choosing the short turn alone does not reduce minion HP or score reward.
Mixed timing policies, both accumulator widths/facings, malformed owners and
unexpected reader-return tokens are covered by1,944 adapter cases. Native
full-fight, damage and pool-contention validation remains separate.

**Evidence scope.** 634 isolated animation-row inputs and 580 auxiliary
fixtures cover all five ROMs, both facings, native actor-loop delays,
offscreen controls, allocation and explicit initialization. Thirty running
2,000-frame traces cover the five boss families plus both plant branches.
The final encounter uses a controlled native first-form defeat dispatch;
it is not a naturally won fight. Player hold/invulnerability follows native
room loading, with no per-frame boss edits. HP/lives remain valid and SRAM
is unchanged. Forty exact Go windows support the two batches. Independent
repeats reproduce all JSON and the saved program-batch states, WRAM and
screenshots. These figures exclude exploratory and superseded runs.

### Ice Dragon raster workspace residency

A separate native-entry check now loads Northwall rooms 5 → 6 → 7 → 8 in
each release, then observes 512 frames. All 256 waveform phases are reached.
The 111 low scroll bytes follow the same accelerating-wave formula in all
five ROMs, but the unwritten high bytes differ between the selected buffers.
US/EU/DE agree at `$6800`; JP/FR agree at `$6000`. Between those groups,
99 of the 111 inherited bytes differ, including 75 differences in their
low two bits. These bytes stay constant throughout each observation.

This narrows the earlier controlled-workspace result: the original room
chains do **not** provide equivalent complete HDMA tables merely at different
addresses. Preserve the selected workspace and its initialization when
reconstructing the native effect; zero-filled fixtures hide this dependency.
The five traces and their independent repeat have identical JSON evidence,
valid player HP/lives and unchanged SRAM.

A subsequent same-ROM comparison supplies a working visual positive
control. The CPU rebuilds the low bytes every frame, which invalidated the
earlier attempt to change them before running a frame. The replacement
control changes the 256-byte waveform source to constant 0 or 64 in an
explicitly labelled **in-memory ROM copy**, then restores the same scratch
state and lets the original builder run. Both controls change all 512
rendered frames in each ROM; the generated low bytes retain the requested
constant and the high bytes remain untouched.

Against that baseline, swapping only the 111 inherited high bytes between
the regional patterns, or zeroing them, changes **none of the 512 rendered
frames in any ROM**. This is a within-ROM pixel comparison, not a claim that
the five regional scenes are identical. The scoped visible-effect question
is closed: the high-byte difference is not visible in these tested entries,
with a positive control demonstrating that the waveform affects the image.
It is not a proof for every possible approach or background state, nor a
reason to discard the original workspace contract.

This adds 15 unmodified-ROM scratch branches and ten separately counted
modified-waveform controls. All 102 saved outputs reproduce byte-for-byte,
and SRAM remains unchanged. The starting room chain uses held/protected
player state rather than an unassisted boss approach. The earlier failed
controls remain excluded from the accepted counts; no donor ROM or player
save file was modified.
