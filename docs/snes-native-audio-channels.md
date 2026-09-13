# Native audio channel reference

This reference describes the US game's SPC700 driver, voice allocation, and
sound-effect tables. For game settings and replacement music, see the
[manual](manual.md) and [Workshop guide](builder-workshop.md).

## Confirmed channel layout

The SNES S-DSP exposes eight voices. Each voice has its own left/right volume,
pitch, source number, ADSR, and gain registers; the global KON, KOF, PMON, NON,
and EON registers address them with one bit per voice. ActRaiser's uploaded SPC700 program is the ROM block at file `$011ACD`, loaded
to ARAM `$0400-$0F4C`. Its allocation is:

| Driver role | SPC track X | Mask | Native DSP voice | Request source |
|---|---:|---:|---:|---|
| Song track 0 | `$00` | `$01` | 0 | song sequencer |
| Song track 1 | `$02` | `$02` | 1 | song sequencer |
| Song track 2 | `$04` | `$04` | 2 | song sequencer |
| Song track 3 | `$06` | `$08` | 3 | song sequencer |
| Song track 4 | `$08` | `$10` | 4 | song sequencer |
| Song track 5 | `$0A` | `$20` | 5 | song sequencer |
| Song track 6 | `$0C` | `$40` | 6 | song sequencer, overlaid by event effects |
| Song track 7 | `$0E` | `$80` | 7 | song sequencer, overlaid by ordinary SFX |
| Event-effect lane | `$10` | `$40` | 6 | SPC input port `$F6` / CPU `$2142` / COP |
| Ordinary-SFX lane | `$12` | `$80` | 7 | SPC input port `$F7` / CPU `$2143` / BRK |

The important distinction is between a *logical SPC sequencer track* and a
*physical DSP voice*. Tracks `$10` and `$12` are extra sequencer state, but in
authentic mode they deliberately alias song voices 6 and 7.

### CPU-to-SPC request path

| Kind | Game-side call | WRAM mailbox | NMI port | Driver lane |
|---|---|---:|---:|---|
| Event effect | `LDA #id; COP` | `$035A` | `$2142` / SPC `$F6` | track `$10`, voice 6; high-bit IDs also use track `$12`, voice 7 |
| Ordinary SFX | `LDA #id; BRK` | `$035B` | `$2143` / SPC `$F7` | track `$12`, voice 7 |

The NMI tail at `$02:AC29-$02:AC3C` performs one
16-bit load from `$035A`, clears both bytes, and writes them to `$2142-$2143`.
Its carry gate forwards requests every other frame.

These mailboxes have depth one. Two BRKs before the next drain do not form a
queue: the later value replaces `$035B`. The same applies independently to
`$035A`.

## SPC700 allocation and suppression logic

The decisive driver routine is ARAM `$0DA0-$0E13`.

At `$0DB5` it reads event port `$F6`:

- A positive nonzero event starts track X=`$10` with mask `$40`, DSP voice 6.
- A high-bit event starts track X=`$10`, sets the driver's two-lane busy byte
  `$35`, then starts track X=`$12` with mask `$80`, DSP voice 7. The high bit is
  a mode flag: `$0E14` doubles the byte, discarding bit 7, so the sequence-table
  index is the low seven bits. Effective IDs above `$26` are clamped to the
  driver's fallback sequence `$07`.
- While `$35 != 0`, positive port-2 events are rejected. A new high-bit event
  takes the BMI path before the `$35` test, so it replaces the currently owned
  pair instead of being rejected.

At `$0DFA` it tests `$35`; at `$0DFE` it reads ordinary-SFX port `$F7` and
starts track X=`$12`, but only when `$35 == 0`. A high-bit event therefore
blocks ordinary SFX without even reading port `$F7` until the dual-lane event
ends.

The driver copies its effect-ownership mask through direct-page bytes `$37`
and `$1A`. The song sequencer continues to advance, but several DSP write paths
skip writes for an owned voice:

```text
$04D0  MOV A,$1A
$04D2  AND A,$47       ; current song voice mask
$04D4  BNE ...         ; skip this music write

$05B1  MOV A,$47
$05B3  AND A,$1A
$05B6  BNE ...         ; skip DSPDATA write at $05B8

$080A  MOV A,$1A
$080C  AND A,$47
$080E  BNE ...         ; skip another voice update path
```

This is why an effect can create a music gap rather than merely mix over it.
The music state is not paused or replayed while voice 6 or 7 is borrowed. Its
missed notes and parameter changes are already in the past when the effect
releases the voice.

Track cleanup begins at `$0E51`. It removes `$47` from the active ownership and
key masks and clears `$35` when the dual-lane event finishes.

## Representative event-effect posts (COP / voice 6 or voices 6+7)

The channel rule applies to every port-2 value: IDs `$01-$7F` use voice 6 and
IDs `$80-$FF` duplicate their low-seven-bit sequence on voices 6 and 7. A
low-seven-bit ID above `$26` selects fallback sequence `$07`. The following
confirmed immediate posts illustrate both forms; this is not an exhaustive COP
caller census.

| Posted ID | Effective sequence | Native voices | COP site | Owning routine roots |
|---:|---:|---:|---|---|
| `$07` | `$07` | 6 | `$01:902D-$902F`, among other sites | dialogue glyph blip at `$01:901C`; `$01:8B16` is another `$07` post |
| `$83` | `$03` | 6 + 7 | `$00:F68C` | `$00:F668/$F674/$F684` |
| `$85` | `$05` | 6 + 7 | `$00:A5CE` | `$00:A54A/$A560/$A593` |
| `$89` | `$09` | 6 + 7 | `$00:FD3D` | `$00:FD25/$FD3A` |
| `$8A` | `$0A` | 6 + 7 | `$00:AF9C/$AFC1/$C1F4/$E508/$F9C0/$FA57` | multiple regional and Death Heim boss states |
| `$91` | `$11` | 6 + 7 | `$02:8513` | `$02:84EC` |
| `$94` | `$14` | 6 + 7 | `$00:E54C/$F8B4` | Marahna and Death Heim final-boss states `$00:E540/$F8A2` |
| `$9A` | `$1A` | 6 + 7 | `$00:BFFF` | Bloodpool state `$00:BFF9` |
| `$9C` | `$1C` | 6 + 7 | `$03:8365` | `$03:82DB` |
| `$9E` | `$1E` | 6 + 7 | `$00:F1F3/$F217` | Northwall states `$00:F1ED/$F214` |
| `$A0` | `$20` | 6 + 7 | `$00:F67C` | `$00:F668/$F674` |
| `$A1` | `$21` | 6 + 7 | `$00:D90A` | Aitos child/event spawner `$00:D903` |

High-bit events are the worst authentic collision case: they take both music
voices, block positive event effects and ordinary SFX, and hold that state
until their shared sequence completes. A later high-bit event is the exception:
it is accepted and truncates/replaces the currently active pair.

## Conservative static ordinary-SFX post catalog (BRK / voice 7)

The table below includes unambiguous direct `LDA #id; BRK #$00` sites decoded in
the engine's normal 16-bit accumulator paths. It intentionally does not pretend
that data-selected or 8-bit-accumulator posts are statically complete. Raw hook
searches in generated code are unsafe because wrong-M/X “split immediate”
variants can interpret operand bytes as BRK or COP opcodes.

| ID | Exact BRK post sites |
|---:|---|
| `$02` | `$00:8B0A` |
| `$03` | `$00:8B47`, `$03:B641` |
| `$08` | `$01:B952`, `$01:B983` |
| `$09` | `$00:B8BD`, `$00:BDE5`, `$00:CFC6`, `$00:EB11`, `$00:FBEE`, `$00:FC44` |
| `$0A` | `$00:D15F` |
| `$0B` | `$00:E248` |
| `$0C` | `$00:8B27`; also posted through the dynamic town-actor path `$01:CEE8` |
| `$10` | `$00:BD62`, `$00:FD70`, `$01:BB6D`, `$01:C8EB` |
| `$12` | `$00:B6BC`, `$00:CA68` |
| `$14` | `$00:AD02`, `$00:B27C` |
| `$16` | `$01:C351`, `$01:C379` |
| `$18` | `$01:BC21`, `$01:C1D6` |
| `$1A` | `$00:D5D8` |
| `$1B` | `$00:A9C9`, `$00:B152`, `$00:B35F`, `$00:B5FC`, `$00:C2DE`, `$00:C37C`, `$00:C94D`, `$00:D500`, `$00:DE8A`, `$00:E042` |
| `$1D` | `$00:BF95` |
| `$1E` | `$01:BF86` |
| `$1F` | `$03:BA17` |
| `$20` | `$00:BED9` |
| `$21` | `$00:BE5B`, `$00:C555`, `$00:CB39`, `$00:D3AA`, `$00:D55C`, `$00:E07B` |
| `$23` | `$00:CF4F` |
| `$24` | `$01:BC09`, `$01:C124`, `$01:C482`, `$01:C7AB` |

`$00` is idle/clear, not a sound. The existing runtime census observed it most
often from `$03:9E6B/$03:9E5A`. Data-selected routines can also post IDs that do
not appear as immediate constants; `$01:CEE5-$01:CEE8` is one confirmed example.

All entries in this section use the same native destination: logical track
`$12`, physical voice 7.
## Common effect sequence and sample bank

The common audio image begins at ROM `$06:AC00`. It loads the effect pointer
table to ARAM `$2400`, sample directory to `$2C00`, and samples with SRCN
`$00-$0B`. Song-specific instruments begin at SRCN `$0C`.

The common sample starts are:

| SRCN | BRR start | SRCN | BRR start |
|---:|---:|---:|---:|
| `$00` | `$3000` | `$06` | `$5DB4` |
| `$01` | `$3B01` | `$07` | `$5DD8` |
| `$02` | `$44EB` | `$08` | `$6906` |
| `$03` | `$4545` | `$09` | `$6DA1` |
| `$04` | `$4F2F` | `$0A` | `$6DF2` |
| `$05` | `$5814` | `$0B` | `$6E4C` |

The driver accepts sequence IDs `$01-$26` (ID `$00` is idle in the game-side
protocol). The entry pointers and initial explicit sample selections are:

| ID | ARAM entry | Initial SRCN | ID | ARAM entry | Initial SRCN |
|---:|---:|---:|---:|---:|---:|
| `$01` | `$2450` | `$00` | `$14` | `$26D1` | `$09` |
| `$02` | `$2465` | `$01` | `$15` | `$26EA` | `$06` |
| `$03` | `$2478` | `$02` | `$16` | `$26F8` | `$0A` |
| `$04` | `$24B3` | `$03` | `$17` | `$276A` | `$06` |
| `$05` | `$24C3` | `$01` | `$18` | `$27A9` | `$0A` |
| `$06` | `$24D6` | `$05` | `$19` | `$27BD` | `$02` |
| `$07` | `$2549` | `$02` | `$1A` | `$28D9` | `$05` |
| `$08` | `$2553` | `$06` | `$1B` | `$28EE` | `$00` |
| `$09` | `$255E` | `$07` | `$1C` | `$2916` | `$06` |
| `$0A` | `$256F` | `$07` | `$1D` | `$294E` | `$0B` |
| `$0B` | `$257D` | `$06` | `$1E` | `$297A` | `$0B` |
| `$0C` | `$2593` | `$08` | `$1F` | `$2996` | `$09` |
| `$0D` | `$259D` | `$02` | `$20` | `$29AD` | `$0A` |
| `$0E` | `$25DA` | `$09` | `$21` | `$29C4` | `$09` |
| `$0F` | `$2627` | `$08` | `$22` | `$29DB` | `$0A` |
| `$10` | `$2676` | `$09` | `$23` | `$29ED` | `$03` |
| `$11` | `$2688` | `$07` | `$24` | `$29F7` | `$06` |
| `$12` | `$26A3` | `$07` | `$25` | `$2A21` | `$0A` |
| `$13` | `$26B4` | `$06` | `$26` | `$2A5C` | `$0A` (later `$09`) |

“Initial SRCN” identifies the starting sample, not the complete sound. A
sequence can change sample, pitch, volume, envelope, and pan over time. The
same sample is heavily reused at different pitches.
