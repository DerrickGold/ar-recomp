# SNES native audio channels and ActRaiser effect ownership

Status: ROM-static channel map complete; runtime sound-name labeling is still
partial. Investigated 2026-08-23.

This document is authoritative for native channel allocation, effect-lane
ownership, loss mechanisms, and extended-channel design. `SEAMS.md` owns the
external audio hooks, port protocol, and replacement-audio integration and
links here instead of repeating these tables.

This note answers two separate questions:

1. Which native S-DSP voices does ActRaiser's driver use for music and effects?
2. At which stages can a sound request be overwritten, rejected, or steal a
   voice from music?

The short answer is that the game does not have a general eight-voice allocator.
It has eight song sequencer tracks plus two logical effect tracks which are
hard-wired over physical voices 6 and 7. Extending the DSP voice array alone
will therefore not remove the gaps. Extended mode also has to remove the
driver's voice-6/7 ownership mask and its one-request effect lanes.

## Confirmed channel layout

The SNES S-DSP exposes eight voices. Each voice has its own left/right volume,
pitch, source number, ADSR, and gain registers; the global KON, KOF, PMON, NON,
and EON registers address them with one bit per voice. The local DSP core models
the same fixed array as `DspChannel channel[8]` in
`snesrecomp-go/runtime/src/snes/dsp.h`.

ActRaiser's uploaded SPC700 program is the ROM block at file `$011ACD`, loaded
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

The BRK and COP hooks in `src/actraiser/actraiser_rtl.c` reproduce the ROM
software-interrupt vectors. The NMI tail at `$02:AC29-$02:AC3C` performs one
16-bit load from `$035A`, clears both bytes, and writes them to `$2142-$2143`.
Its carry gate forwards requests every other frame.

These mailboxes have depth one. Two BRKs before the next drain do not form a
queue: the later value replaces `$035B`. The same applies independently to
`$035A`.

## SPC700 allocation and suppression logic

The decisive driver routine is ARAM `$0DA0-$0E13`.

At `$0DB1` it reads event port `$F6`:

- A positive nonzero event starts track X=`$10` with mask `$40`, DSP voice 6.
- A high-bit event starts track X=`$10`, sets the driver's two-lane busy byte
  `$35`, then starts track X=`$12` with mask `$80`, DSP voice 7. The high bit is
  a mode flag: `$0E14` doubles the byte, discarding bit 7, so the sequence-table
  index is the low seven bits. Effective IDs above `$26` are clamped to the
  driver's fallback sequence `$07`.
- While `$35 != 0`, new port-2 events are rejected.

At `$0DFA` it reads ordinary-SFX port `$F7` and starts track X=`$12`, but only
when `$35 == 0`. A high-bit event therefore blocks ordinary SFX without even
reading port `$F7` until the dual-lane event ends.

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

## The four independent loss mechanisms

“Ran out of channels” describes only part of the behavior. The current path can
lose audio at four different stages:

1. **Game mailbox overwrite.** A later BRK/COP replaces the one-byte `$035B` or
   `$035A` value before the alternating NMI drain.
2. **SPC input-port overwrite.** A newer CPU port write can become visible
   before the SPC700 consumes the preceding value. The existing audio trace
   already counts this stage per port.
3. **Driver rejection/replacement.** A dual-voice high-bit event prevents new
   effects from being accepted: port 2 is read but rejected, while port 3 is
   not read. Otherwise, a new request reinitializes the same logical track,
   replacing an effect already active on that lane.
4. **Music voice stealing.** Accepted effects claim bits `$40/$80`; the music
   sequencer advances while its voice-6/7 DSP writes are suppressed.

A ten-voice mixer fixes only item 4 unless request transport and logical track
allocation are extended as well.

The recomp runtime already schedules APU writes and gives distinct values on a
port a 128-sample minimum dwell, which prevents a host-thread timing artifact
from compressing ordinary writes below the driver's roughly 64-sample poll
period. That protects an *unblocked* input port. It cannot fix the native
high-bit-event case, because the driver intentionally avoids reading port 3
for the much longer `$35` busy interval; a later scheduled value can still
replace the unread ordinary effect.

## Intentional song swaps are not dropped sounds

Port 0 music control is a separate path from the COP/BRK effect lanes. A full
song-image transition has a recognizable signature: `$2140 = $F0` halts the
current song, `$2140 = $FF` enters the uploader, `$02:9964` installs the new SPC
image and BRR chunks, and a normal port-0 value starts a song in that image.
`$F2` pause followed by the current song number is another deliberate control
operation. Neither should be counted as an effect drop or a stolen music voice.
Some deliberate transitions play another song from an already-loaded image via
the `$F1` echo handshake and a song number, so the classifier must record every
non-control port-0 play command rather than require an upload in all cases.

The simulation level-up sequence is a confirmed example:

1. `$03:E426` calls `Master_LevelUp` at `$03:B3BA`. If a level was actually
   awarded, `$03:E42F` calls `$03:E4B1`.
2. `$03:E4B1` selects song-table record `$02:C806`, entry `$0B`, whose image
   pointer is `$1C:AFEB`. The replacement manifest independently labels that
   image `level-up.ogg`.
3. The shared tail `$03:E4E6-$03:E52C` performs the complete
   `$F0 -> $FF -> $02:9964 upload -> play $01` transition.
4. After the level-up message and stat updates, `$03:E47A` calls `$03:E4C0`.
   That routine selects the current region's simulation image from
   `$02:C7E8/$C7F4/$C803/$C815` and runs the same full loader, restoring the
   simulation music.

This case should therefore be logged as `song_swap_begin(level-up)` followed by
`song_swap_restore(sim-region)`. The missing simulation music between those
events is intentional. In contrast, an effect ownership bit suppressing only
song voice 6 or 7 while the song sequencer keeps advancing is a genuine
`music_voice_masked` gap.

A useful loss/transition taxonomy is:

| Classification | Required evidence | Meaning |
|---|---|---|
| `request_overwritten_mailbox` | request serial written to `$035A/$035B`, then replaced before NMI drain | genuinely dropped before reaching the SPC |
| `request_overwritten_port` | CPU port write applied, then replaced before the SPC reads it | genuinely dropped in transport |
| `event_rejected_dual_busy` | nonzero port-2 event is read while SPC `$35 != 0`, but no X=`$10` start follows | genuinely rejected by native priority logic |
| `ordinary_blocked_dual_busy` | port 3 contains a nonzero request while `$35 != 0`, so `$0DFA` does not read it | pending/blocked; count as dropped only if a later port write replaces it before acceptance |
| `effect_lane_replaced` | accepted request reinitializes an already-active X=`$10` or X=`$12` track | preceding effect was truncated by another effect |
| `music_voice_masked` | `$1A & $47 != 0` skips a song-track DSP write | music note/parameter update was genuinely lost to voice stealing |
| `song_swap_begin/restore` | port-0 song-image identity and/or selected song number changes | deliberate whole-song replacement, not a drop |
| `song_pause/resume` | native `$F2` and matching resume command | deliberate pause, not a drop |

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
| `$91` | `$11` | 6 + 7 | `$02:8513` | `$02:84EC` |
| `$9C` | `$1C` | 6 + 7 | `$03:8365` | `$03:82DB` |
| `$A0` | `$20` | 6 + 7 | `$00:F67C` | `$00:F668/$F674` |

High-bit events are the worst authentic collision case: they take both
music voices, block both new event effects and ordinary SFX, and hold that state
until their shared sequence completes.

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
`$12`, physical voice 7. A sample appearing on voice 6 within the census's
two-frame correlation window is not proof that the BRK effect itself allocated
voice 6; port-2 events and driver re-triggers can overlap that heuristic window.

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

“Initial SRCN” is useful for validation, not a complete sound identity. A
sequence can change sample, pitch, volume, envelope, and pan over time. The
same sample is heavily reused at different pitches.

### Existing runtime observations

The earlier `AR_SFXCENSUS=1` capture agrees with the initial sequence mapping
for the strongest matches:

| Requested ID | Observed owning roots | Strongest matching result |
|---:|---|---|
| `$08` | `$01:B936/$01:B967` | initial SRCN `$06`; wide pitch range |
| `$10` | `$01:BB28` | SRCN `$09`, normally centered, native voice 7 |
| `$18` | `$01:BC16/$01:C1CB` | initial SRCN `$0A`; sequence-driven pan is clearly audible in DSP volumes |
| `$1F` | `$03:B97F` | initial SRCN `$09` |
| `$0C` | `$01:CEE5` | SRCN `$08`, fixed captured pitch/center volume |
| `$1E` | `$01:BF67` | SRCN `$0B`, fixed captured pitch |
| `$24` | `$01:BC06` | initial SRCN `$06` |

The extra samples/voices attributed to several IDs in that old report are a
known limitation of its two-frame “latest request” correlation. About 84% of
shared-bank key-ons in that run had no new BRK request because effect sequences
re-key themselves. Classification must use logical-track provenance, not only
request proximity or SRCN `< $0C`.

### Which effects are most exposed to truncation

Every ordinary BRK effect shares the single X=`$12` lane, so all of them can be
replaced by the next accepted ordinary effect. Every one is also blocked while
a high-bit COP event owns both effect lanes, and is usually lost when the next
CPU port write replaces the unread value. Within the available natural-play
census, request frequency gives this preliminary *exposure* ranking:

| ID | Requests in capture | Known context | Preliminary exposure |
|---:|---:|---|---|
| `$10` | 113 | Blue Dragon building strike at `$01:BB28`; also Bloodpool/action vertical-lightning sites | highest observed |
| `$1F` | 33 | simulation monster/lair processing at `$03:B97F` | high |
| `$08` | 32 | simulation event-helper pair `$01:B936/$01:B967` | high |
| `$18` | 31 | simulation enemy/world-effect states `$01:BC16/$01:C1CB` | high |
| `$0C` | 7 | town-actor script path `$01:CEE5` | lower observed |
| `$24` | 6 | simulation enemy state `$01:BC06` and related callers | lower observed |
| `$1E` | 3 | simulation enemy state `$01:BF67` | lowest in this capture |

These counts are not yet measured drop counts. They show how often each effect
enters the collision-prone lane; `$10` is therefore the strongest first test
case, especially during repeated Blue Dragon lightning. Sequence duration and
overlap matter too: a rare long sequence may be easier to truncate than a
frequent short click. Exact per-ID loss rates require request serials and the
driver-decision/end events described below.

## Recommended extended-channel design

### Compatibility contract

Keep authentic mode byte-for-byte behaviorally unchanged. Extended mode should
be opt-in and serialized in settings/save-state compatibility metadata. The
safest first version treats changing the setting as restart-required; migrating
live envelopes and driver tracks during a toggle is otherwise ambiguous.

### Phase 1: ten-voice “music-safe effects”

Add two virtual DSP voices and map by sequencer provenance:

| Extended voice | Source logical track | Purpose |
|---:|---:|---|
| 0-7 | `$00-$0E` | all eight song tracks remain continuously audible |
| 8 | `$10` | port-2 event-effect lane |
| 9 | `$12` | port-3 ordinary-SFX lane / second half of high-bit event |

This phase must do both of the following:

- Route track `$10/$12` register writes, KON, and KOF to virtual voices 8/9.
- Stop `$1A` ownership from suppressing song-track writes on native voices 6/7.

The original masks are only eight bits, so this cannot be implemented by merely
increasing `voice[8]` to `voice[10]`. The bridge needs explicit logical-track
provenance or an extended ownership representation outside the original SPC
direct page.

Phase 1 removes music gaps but deliberately preserves one active instance per
effect lane. A new ordinary SFX can still replace the preceding ordinary SFX,
and a dual-lane event can still block request processing unless that logic is
changed too.

### Phase 2: queued, polyphonic effects

For the requested “do not drop or suppress effects” behavior:

1. Capture BRK/COP requests at the software-interrupt hook with a monotonically
   increasing serial before they can overwrite `$035A/$035B`.
2. Preserve ordering in an extended-mode FIFO.
3. Give each accepted request an independent sequence state and allocate it
   from a configurable virtual-voice pool (for example 16 or 24 total voices).
4. Preserve high-bit event semantics as a paired/two-instance effect, including
   its small second-lane delay, without globally blocking unrelated requests.
5. Retain the original sequence interpreter, sample directory, pitch, envelope,
   pan, noise, pitch-modulation, and echo-routing semantics.

Copying only the first KON/SRCN to a host mixer is insufficient: many effects
re-key or alter parameters later without another CPU request. The robust choices
are either an extended clone of the SPC sequence-track state or a faithful
host-side implementation of this driver's effect sequencer.

If the virtual pool itself fills, make that a new explicit cap with telemetry.
Only then should an extended-mode priority policy run. That keeps “authentic
priority quirks” separate from a genuine user-configured resource limit.

### DSP integration details that must not be lost

- Virtual voices must feed the same final stereo and echo buses. EON behavior,
  FIR state, feedback, and echo RAM timing are global, not per independent
  post-mix player.
- PMON depends on the preceding voice. Remapping must define whether the logical
  predecessor or physical virtual index controls modulation.
- Noise clock and NON are global/masked DSP state.
- Save states need every added voice, BRR decoder position, envelope, KON delay,
  logical sequence cursor, request FIFO, and ownership flag.
- Replacement-music muting currently uses SRCN `$0C+` as a pragmatic music
  gate. Extended classification should instead carry explicit song/effect
  provenance so common-bank ambience is not misclassified.

## Instrumentation needed before implementation

`src/dev/sfx_census.c` is a useful first pass, but it keeps one pending request
and uses a two-frame key-on correlation window. Extend the trace with one record
per request serial and these timestamps/stages:

1. BRK/COP hook: ID, caller, X/Y, CPU frame, serial.
2. `$035A/$035B` write and NMI drain: prove game-mailbox overwrite.
3. CPU port write, SPC-visible apply, and SPC read: the runtime audio trace
   already has most of this and per-port overwrite counters.
4. Driver decision at `$0DB1/$0DFA`: accepted, zero, blocked by `$35`, or lane
   replacement.
5. Sequence start at `$0E14`: effective low-7-bit ID and logical X=`$10/$12`.
6. Every DSP write/KON/KOF: logical track, target native/virtual voice, SRCN,
   pitch, volumes, and ownership mask.
7. Sequence end at `$0E51`.
8. Port-0 command, uploaded image source, selected song number, and explicit
   transition owner/caller. This separates full-image swaps, in-bank song
   changes, pause/resume, and restore operations from effect-channel loss.

That trace turns every missing sound into one exact reason instead of an
“unmatched request” bucket.

## Verification matrix

- **Authentic-off parity:** same replay produces identical DSP register stream
  and PCM hash with extended mode disabled.
- **Music collision:** force active song notes on voices 6/7 while posting one
  low event, one ordinary SFX, and each high-bit event. Extended mode must retain
  all song-track writes.
- **Mailbox burst:** post multiple BRKs and COPs inside one NMI interval. The
  request serials must all reach independent extended sequence instances.
- **Long/re-keying effects:** prove later sequence KONs remain attached to the
  original logical request.
- **Pool stress:** exceed the configured extended pool and report only the new,
  intentional cap policy.
- **Echo/pan parity:** compare isolated authentic effects with their extended
  equivalents before testing overlap.
- **Save/load:** save during a dual-lane event and several ordinary effects,
  then require sample-identical continuation after load.

## Evidence and confidence

High confidence:

- eight native voices and their register layout;
- song X=`$00-$0E`, event X=`$10`, ordinary SFX X=`$12`;
- physical masks `$40/$80` and voices 6/7;
- high-bit events duplicate onto both lanes and block port `$F7` via `$35`;
- music DSP writes are suppressed by `$1A & $47` while sequencing continues;
- the common sequence table, pointers, and initial SRCN values;
- direct BRK/COP post sites listed above.

Still incomplete:

- player-facing names for every sequence ID;
- complete data-selected/dynamically computed BRK caller coverage;
- exact natural-game frequency of each of the four loss mechanisms.

Fresh replay attempts during this investigation did not reproduce the previous
BRK census because the available recordings are coupled to different save/run
state. They are not counted as validation. The static driver map is cross-checked
against the earlier live capture documented in `research-symbol-map.md`.

External register references: Nintendo's *SNES Development Manual*, Book I,
section 7.1 (DSP register map), and the ares SFC DSP implementation, which also
models `voice[8]` and the per-voice volume/pitch/source/envelope state.
