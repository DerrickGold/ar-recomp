# SNES native audio channels and ActRaiser effect ownership

Status: ROM-static channel map, the opt-in 40-voice scheduler, shipped-effect
control audit, isolated PCM parity, and coupled action-stage/high-bit collision
replays are complete; runtime sound-name labeling and full-playthrough
frequency coverage are still partial.
Investigated 2026-08-23 and implemented 2026-08-24.

This document is authoritative for native channel allocation, effect-lane
ownership, loss mechanisms, and extended-channel design. `SEAMS.md` owns the
external audio hooks, port protocol, and replacement-audio integration and
links here instead of repeating these tables.

This note answers two separate questions:

1. Which native S-DSP voices does ActRaiser's driver use for music and effects?
2. At which stages can a sound request be overwritten, rejected, or steal a
   voice from music?

The game does not have a general eight-voice allocator. It has eight song
sequencer tracks plus two logical effect tracks which are hard-wired over
physical voices 6 and 7. Extending the DSP voice array alone therefore cannot
remove the gaps. The implemented extended mode also bypasses the voice-6/7
ownership conflict, captures requests before the native depth-one mailboxes,
and gives each accepted request an isolated copy of the original effect-track
state.

## Confirmed channel layout

The SNES S-DSP exposes eight voices. Each voice has its own left/right volume,
pitch, source number, ADSR, and gain registers; the global KON, KOF, PMON, NON,
and EON registers address them with one bit per voice. The local DSP core models
the authentic eight plus 32 optional game-owned virtual voices as
`DspChannel channel[kDspMaximumVoiceCount]` in
`snesrecomp-go/runtime/src/snes/dsp.h`; authentic mode cycles only the first
eight.

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

## The four independent loss mechanisms

“Ran out of channels” describes only part of the behavior. The current path can
lose audio at four different stages:

1. **Game mailbox overwrite.** A later BRK/COP replaces the one-byte `$035B` or
   `$035A` value before the alternating NMI drain.
2. **SPC input-port overwrite.** A newer CPU port write can become visible
   before the SPC700 consumes the preceding value. The existing audio trace
   already counts this stage per port.
3. **Driver rejection/replacement.** A dual-voice high-bit event rejects new
   positive port-2 effects and prevents port 3 from being read. A new high-bit
   event is still accepted and replaces both active lanes. Otherwise, a new
   accepted request reinitializes the same logical track, replacing an effect
   already active on that lane.
4. **Music voice stealing.** Accepted effects claim bits `$40/$80`; the music
   sequencer advances while its voice-6/7 DSP writes are suppressed.

A separate native behavior can look like item 3 unless it is tracked
explicitly. If an unchanged input-port value makes the driver enter `$0E14`
again without a new traced CPU request/read serial, it restarts the *same*
effect on the same lane. The trace classifies this as `native_lane_retrigger`,
retains the original request owner, and records the first/last retrigger cycle.
It can change the audible envelope or duration, but it is neither a different
sound replacing the request nor evidence of a dropped request.

A second restart case has a new CPU request serial but the same kind and ID as
the active lane owner. It still truncates and restarts the envelope/sequence,
so it is not harmless, but it does not prove that one *different* sound was
lost. The trace classifies it as `new_request_lane_retrigger`; only a new kind
or ID terminating the old owner is `effect_lane_replaced`. This distinction is
especially important for repeated explosion producers.

A mixer-only expansion fixes only item 4 unless request transport and logical
track allocation are extended as well. The ten-voice Phase-1 milestone did
exactly that; the current 40-voice mode addresses all four stages.

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
| `request_coalesced_mailbox_duplicate` | a request is replaced in `$035A/$035B` by the same kind and ID before NMI drain | native depth-one coalescing; keep separate from different-sound loss because blindly making every copy polyphonic can amplify producer loops such as per-glyph dialogue posts |
| `request_overwritten_mailbox` | request serial written to `$035A/$035B`, then replaced before NMI drain | genuinely dropped before reaching the SPC |
| `request_coalesced_port_duplicate` | an unread applied value is replaced by another serial with the same kind and ID | duplicate transport coalescing; potentially distinct emitters, but not evidence that two different sounds competed |
| `request_overwritten_port` | CPU port write applied, then replaced before the SPC reads it | genuinely dropped in transport |
| `event_rejected_dual_busy` | positive port-2 event is read while SPC `$35 != 0`, but no X=`$10` start follows | genuinely rejected by native priority logic; high-bit events bypass this test |
| `ordinary_blocked_dual_busy` | port 3 contains a nonzero request while `$35 != 0`, so `$0DFA` does not read it | pending/blocked; count as dropped only if a later port write replaces it before acceptance |
| `new_request_lane_retrigger` | a new request serial with the same kind and ID reinitializes an active `$10/$12` track | the earlier instance was truncated/restarted, but no different-sound loss is proven |
| `effect_lane_replaced` | accepted request reinitializes an already-active X=`$10` or X=`$12` track | preceding effect was truncated by another effect |
| `effect_canceled_song_transition` | a full SPC image upload clears a scheduled/applied/read/active effect serial | deliberate global driver/image replacement, not channel exhaustion |
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

## Implemented mixer foundation

Independent level control now uses the same logical-track evidence required by
the extended-channel design:

- `g_apu_spc_dsp_write_hook` runs before each applied DSP write. For ordinary
  per-voice writes, SPC X directly identifies song tracks `$00-$0E` and effect
  tracks `$10/$12`; ARAM `$47` proves the physical voice. Captured X=`$10`,
  mask=`$40` effect writes sometimes see `$1A=0`, so ownership alone is not a
  safe classifier. `$1A` remains the fallback when a shared helper has
  repurposed X for a DSP register address; if that fallback is also clear, the
  helper preserves the voice's last proven label instead of guessing Music.
- Each native DSP voice carries a presentation-only Music/SFX label. Music and
  SFX gains scale its dry contribution and echo send before the authentic
  per-voice clamp. The one hardware FIR/feedback echo stays shared, so an
  already accumulated tail decays naturally after a live fader change.
- Replacement OGG follows the Music gain. Its native-voice mute now uses the
  explicit Music label, retaining SRCN `$0C+` only for an unclassified startup
  voice. This avoids baking the common-bank heuristic into future virtual
  effects.
- At Music=100 and SFX=100 the DSP takes the legacy arithmetic path exactly.
  Voice labels sit outside the frozen emulated savestate region; player/debug
  state loads reconstruct them from `$1A`, and subsequent logical-track writes
  immediately refine them.

The mixer foundation itself changes levels only. Added voices carry the same
bus enum, so faders, echo sends, replacement mute, and the master stage need no
second mixer architecture. Phase 1 established the routing and music-safety
bridge with voices 8/9; Phase 2 expands that bridge to a 32-voice effect pool
and removes the native request/lane bottlenecks.

`AR_AUDIO_BUSLOG=1` logs live gain values and only label transitions (voice,
Music/SFX, SPC X, `$47`, `$1A`, and DSP register), which makes a replay-sized
validation readable without dumping every DSP write.

## Extended-channel design and implementation status

### Compatibility contract

Authentic mode remains the default eight-voice path. Extended mode is opt-in as
`audio_extended_channels` / `AR_EXTENDED_AUDIO_CHANNELS`, and changing it is
restart-required because migrating live envelopes and sequencer ownership
during a toggle is ambiguous. Quick-state format v9 serializes all 32 added
DSP channels, the request FIFO, every independent sequencer context, and the
pending CPU-to-SPC port queue with its sample clock. Its
header tags the active eight/40-voice mode and rejects older layouts or a state
from the other topology rather than misreading it.

### Phase 1: ten-voice “music-safe effects” — completed milestone

Add two virtual DSP voices and map by sequencer provenance:

| Extended voice | Source logical track | Purpose |
|---:|---:|---|
| 0-7 | `$00-$0E` | all eight song tracks remain continuously audible |
| 8 | `$10` | port-2 event-effect lane |
| 9 | `$12` | port-3 ordinary-SFX lane / second half of high-bit event |

The implementation does both of the following:

- Route track `$10/$12` register writes, KON, and KOF to virtual voices 8/9.
- Stop `$1A` ownership from suppressing song-track writes on native voices 6/7.

The original masks are only eight bits, so this cannot be implemented by merely
increasing `voice[8]` to `voice[10]`. The bridge needs explicit logical-track
provenance or an extended ownership representation outside the original SPC
direct page.

At this milestone, music gaps were removed while the native one-instance
effect lanes remained. A new ordinary SFX could still replace the preceding
ordinary SFX, and a dual-lane event could still block request processing. Phase
2 below supersedes those restrictions in current extended mode.

Implementation details:

- The first implementation stored two added serialized BRR/envelope voices.
  The same design now backs `DspChannel channel[40]`; authentic mode still
  cycles exactly eight and extended mode cycles all 40 through the same dry,
  Music/SFX gain, master, and shared FIR/feedback echo paths.
- `native_audio_extension.c` maps physical `$40/$80` provenance to virtual
  voices 8/9. Per-voice registers never reach physical song voices 6/7. KON,
  KOF, PMON, NON, and EON are split bitwise: the effect bit updates the virtual
  voice while the native register update preserves that physical song bit.
- The driver helper at SPC `$0834` has already replaced X with `$64-$67` or
  `$74-$77` and can see `$1A=0`. The bridge captures its logical sequencer track
  at `$080A`, ensuring SRCN, ADSR, and GAIN follow pitch/volume to the virtual
  voice rather than leaking onto music.
- At BNE instructions `$04D4`, `$05B6`, and `$080E`, extended mode forces
  fall-through only for a proven song track whose `$47` bit is currently owned.
  The original sequencer then executes its ordinary update; effect tracks and
  every other branch retain native behavior.
- `AR_AUDIO_EXTLOG=1` reports rerouted register/control transitions and each
  ownership-suppression branch that was prevented.

Paced replay `runs/20260824-154930/` routed complete event voice configuration
(SRCN/ADSR/GAIN/pitch/volume) to voice 8 and preserved six song-track updates
that authentic ownership would have skipped. A focused KON replay at
`runs/20260824-155009/` applied the expected event sample (SRCN `$02`) on voice
8 while native voice 6 continued keying song SRCN `$0C`. Unit tests cover both
virtual lanes, bitwise mask preservation, independent SFX gain/echo, and
serialization. The later Phase-2 replay below naturally exercises simultaneous
voices 8 and 9.

### Phase 2: queued, polyphonic effects — implemented

Current extended mode implements the requested “do not drop or suppress
effects” behavior as follows:

1. Capture BRK/COP requests at the software-interrupt hook with a monotonically
   increasing serial before they can overwrite `$035A/$035B`; extended-owned
   requests do not enter the native mailbox/port path.
2. Preserve ordering in a 128-entry extended-mode FIFO. Exact duplicate posts
   from the same producer, actor, and frame are coalesced; the dialogue glyph
   producer has a site-specific same-frame rule because its X/Y changes for
   every character.
3. Give each accepted request an independent sequence state and allocate it
   from the 32 virtual voices at indices 8-39. A full voice pool applies
   backpressure and leaves the request queued instead of replacing a lane.
4. Preserve high-bit event semantics as a paired/two-instance effect, including
   its three-tick second-lane delay, without globally blocking unrelated
   requests; allocation waits until two pool entries are available.
5. Retain the original sequence interpreter, sample directory, pitch, envelope,
   pan, re-keying, and noise commands. Each context saves all 29 two-byte
   per-track fields around the original `$0E7F-$0F0B` interpreter.

Copying only the first KON/SRCN to a host mixer is insufficient: many effects
re-key or alter parameters later without another CPU request. The robust choices
are either an extended clone of the SPC sequence-track state or a faithful
host-side implementation of this driver's effect sequencer. This implementation
uses the former: it time-multiplexes the original SPC700 interpreter and routes
each context's writes to its allocated virtual DSP voice.

The scheduler charges emulated SPC time for at most one `$10` and one `$12`
update per native driver tick. Additional contexts of the same lane execute in
host time, so adding overlapping sounds cannot slow music timers. A full FIFO
is the only new drop cap and is reported explicitly as
`extended_fifo_overflow`; the allocator has no extended-mode priority eviction.
An SPC image upload deliberately cancels queued/active effects because their
sequence pointers and samples no longer belong to the installed image.

### DSP integration and validated control semantics

- Virtual voices feed the same final stereo and echo buses. EON behavior, FIR
  state, feedback, and echo RAM timing remain global rather than becoming a
  separate post-mix player.
- NON is captured per context while every voice shares the authentic global
  noise clock. New contexts initialize PMON, NON, and EON clear; later mask
  writes are routed to their allocated voice.
- `tools/audit_spc_effects.py` statically walks all 38 common-bank sequences
  `$01-$26` using the resident driver's own command-length table. All 38
  terminate, none selects an instrument whose descriptor enables noise, and
  none executes an echo-control command `$F5-$F8`. The resident program has no
  decoded literal reference to the PMON source byte `$4B`, while `$0436-$043C`
  computes EON as `$38 = $4A & ~$36`, explicitly excluding effect-owned bits.
  Therefore every shipped effect uses clear PMON/NON/EON state; the extension's
  initialization is exact for this ROM rather than an assumption based on the
  replayed IDs.
- Virtual KON/KOF changes are captured per context and applied at the original
  driver's central `$0458` mask-writer cadence. An ending voice stays reserved
  through KOF assertion, KOF clear, and the matching KON clear, preventing a
  newly allocated effect from overlapping stale control state on that voice.
- Quick states include every added voice, BRR decoder position, envelope, KON
  delay, logical sequence cursor, FIFO entry, and scheduler ownership field.
- Replacement-music muting now uses explicit song/effect provenance, with SRCN
  `$0C+` only as an unclassified startup fallback. Every virtual voice must set
  the same bus label when allocated.

## Implemented baseline instrumentation

`src/dev/sfx_census.c` remains useful for rough sample-name correlation, but it
keeps one pending request and uses a two-frame key-on window. The behavior-neutral
serial tracer in `src/dev/native_audio_trace*.c` now records one row per request
and observes the following stages without modifying emulated state:

1. BRK/COP hook: ID, caller, X/Y, CPU frame, serial.
2. `$035A/$035B` write and NMI drain: prove game-mailbox overwrite.
3. CPU port write, SPC-visible apply, and SPC read: the runtime audio trace
   already has most of this and per-port overwrite counters.
4. Driver decision at `$0DB5/$0DFA`: accepted, zero, blocked by `$35`, or lane
   replacement.
5. Sequence start at `$0E14`: effective low-7-bit ID and logical X=`$10/$12`.
6. Every reached DSPDATA writer, aggregated by SPC PC, DSP register, current
   track mask, ownership mask, and write count; the three confirmed
   ownership-caused music-update skips are recorded separately and attributed
   to the active request serial.
7. Sequence end at `$0E51`.
8. Extended FIFO disposition plus every virtual-lane start, end, cancellation,
   and allocated voice index.
9. Port-0 command, uploaded image source, selected song number, and explicit
   transition owner/caller. This separates full-image swaps, in-bank song
   changes, pause/resume, and restore operations from effect-channel loss.
10. Same-request native lane retriggers, including count and first/last cycle,
    kept separate from lane replacement.
11. New-serial same-kind/ID lane restarts as `retriggered_lane`, kept separate
    from a different-kind/ID `replaced_lane` loss.

Enable it with:

```sh
AR_NATIVE_AUDIO_TRACE=1 ./build-release/ActRaiserRecomp ar.sfc --config config.ini
```

Set `AR_NATIVE_AUDIO_PCM=1` alongside the trace to dump the retained native-rate
PCM ring to `native_audio_pcm.wav`. This is intended for reproducible
authentic-versus-extended waveform comparisons, not normal logging.

At clean shutdown it writes these files under `runs/latest/`:

| File | Purpose |
|---|---|
| `native_audio_requests.csv` | request serial, caller/site, every native or extended transport/lane timestamp, terminal outcome, replacement/coalescing serial, native retrigger cycles/count, virtual-voice mask, and music-update suppression attributed to that request |
| `native_audio_song_events.csv` | port-0 controls, selected song number, image upload identity, frame, and caller; deliberate level-up/restore transitions stay separate from drops |
| `native_audio_dsp_provenance.csv` | every reached DSP writer aggregated by SPC PC, track/ownership masks, DSP register, and count |
| `native_audio_music_suppression.csv` | the three `$1A & $47` music-update skip sites aggregated by song track mask and ownership mask |
| `native_audio_pcm.wav` | optional S-DSP PCM ring dump when `AR_NATIVE_AUDIO_PCM=1` is also set |

The classifier has focused tests for mailbox overwrite/coalescing, port
overwrite/coalescing, positive-event/ordinary-SFX busy rejection, lane
replacement, high-bit paired completion, deliberate song transitions, settings
suppression, and ownership-caused music gaps. A 180-frame title smoke run at
`runs/20260824-143539/` produced the four files, captured the expected common-
image/title-image upload and play sequence, and observed 42 DSP writes with no
effect requests or false drop classifications.

### First paced replay result

`runs/20260824-143929/` replayed the first 1,100 frames of `sim-actions.rec`
with a dummy audio device and `AR_PACE=1`, so the CPU and SPC remained on a
normal-time relationship. Its 371 request posts classified as:

| Outcome | Count | Interpretation |
|---|---:|---|
| completed | 16 | accepted X=`$10` event sequences that reached `$0E51` |
| coalesced mailbox duplicate | 354 | repeated COP `$07` posts, overwhelmingly the dialogue composer drawing many glyphs in one game frame; not counted as 354 different-sound drops |
| overwritten port | 1 | the final dialogue COP `$07` at game frame 303 reached port 2 but was cleared before the SPC read it |
| different-ID mailbox overwrite / busy reject / lane replacement | 0 | none in this short capture |

One completed COP `$07` from `$01:8B82` at game frame 614 owned voice 6 long
enough to skip three song-track updates: two at SPC `$05B1` and one at `$04D0`.
This is the first serial-attributed proof in the new trace of an effect that
completed normally while still creating a genuine music gap. The same run
recorded title → sky-palace → simulation image transitions in the song-event
file; they are deliberate swaps and did not enter any request-drop count.

The duplicate result is also a design constraint: extended mode should not
automatically enqueue every identical per-glyph post as a simultaneous voice.
It needs either native duplicate coalescing by default or an explicit producer-
aware policy, while still preserving genuinely independent overlapping effects.

### Corrected authentic and extended paced replay

`runs/20260824-171030/` replayed 700 frames in authentic mode after adding the
same-request retrigger distinction. Its 214 posts classified as seven
completed requests, 205 same-producer mailbox duplicates, two actual port
overwrites, three same-request native lane retriggers, and zero lane
replacements. Both overwritten requests were dialogue COP `$07` posts from
`$01:902D`; this recording contains no ordinary BRK exposure, so it is not a
general SFX drop-rate census.

`runs/20260824-171219/` replayed the same 700 frames with
`AR_EXTENDED_AUDIO_CHANNELS=1`, dummy audio, and normal pacing. Its 214 posts
classified as:

| Outcome | Count | Interpretation |
|---|---:|---|
| completed | 10 | independent extended sequence instances reached their terminal return |
| coalesced extended duplicate | 204 | same-frame dialogue glyph posts deliberately folded into their producer's existing request |
| FIFO overflow / native mailbox or port loss / busy reject / lane replacement | 0 | no resource or native transport loss |

Natural overlaps used virtual voices 8 and 9 and completed independently. The
suppression CSV contained only its header: no song update was blocked, and
every completed row reported zero suppressed music updates. This capture
validates that deliberate same-producer coalescing remains distinct from
genuinely overlapping requests.

### Coupled action-stage drop census

`tools/analyze_native_audio_trace.py RUN [--sites] [--transitions]` summarizes
the request CSV by kind/ID while keeping genuine transport/different-sound
loss, deliberate suppression/cancellation, duplicate coalescing, same-ID
restarts, and requests still active when a recording stops in separate
columns. It also normalizes older captures made before `retriggered_lane` was
an explicit terminal outcome by comparing the old and replacement serials.

The following action recordings were replayed with `AR_SAVE_EDIT=0` and the
exact SRAM fixture used while recording. This coupling matters: staged save-
editor changes occur before replay protection and can otherwise change the
route while the input stream remains identical.

Fillmore run `runs/20260824-172558/` produced 356 requests. Dialogue settings
deliberately suppressed 247, leaving 109 exposed action requests:

| ID | Posted | Completed | Different-sound lane loss | Same-ID restart |
|---|---:|---:|---:|---:|
| BRK `$02` enemy hit | 3 | 1 | 1 (replaced by enemy-death `$03`) | 1 |
| BRK `$03` enemy death | 4 | 4 | 0 | 0 |
| BRK `$1B` | 22 | 19 | 2 (replaced by `$03`) | 1 |
| COP `$01` sword swing | 38 | 0 | 38 (replaced by `$12`) | 0 |
| COP `$12` sword beam | 38 | 23 | 15 (replaced by the next `$01`) | 0 |
| COP `$07` | 4 | 4 | 0 | 0 |

The COP `$01/$12` alternation comes from this project's ranged-sword
enhancement (`$00:9CF2` creates the beam); it proves the native lane-reuse
mechanism but is not attributed to the stock game's encounter balance. The
enemy hit/death transition is stock combat at `$00:8B07/$00:8B44`.

Extended replay `runs/20260824-172955/` retained the same 356 request
identities, completed all 109 exposed requests, and recorded zero transport,
busy, lane, FIFO, or music-suppression loss. Final WRAM, SRAM, and dispatch-log
hashes are byte-identical to the authentic run, as are song event identities;
the audio topology therefore did not perturb game execution.

Aitos run `runs/20260824-173312/` deliberately stresses high-bit events. Of 578
requests, 460 dialogue posts were suppressed and 118 were exposed. Thirty-five
completed before the recording ended; 47 were genuine losses, 34 were same-ID
new-request restarts, and two were still open at shutdown. Genuine losses were
two mailbox overwrites, one port overwrite, ten native dual-busy rejections,
and 34 different-sound lane replacements. In particular:

- COP `$A1` at `$00:D907` has effective sequence `$21`; its eight posts use
  distinct child Y slots. COP `$85` at `$00:A5CB` is the boss-death explosion;
  its 16 posts also correspond to separately allocated explosion objects.
  Their 20 same-ID restarts are therefore distinct cues being collapsed by the
  native lanes, not an intentional song transition.
- BRK `$03` at frame 2037 and COP `$12` at frame 4270 were overwritten in the
  CPU mailboxes. BRK `$02` at frame 4270 was overwritten at the SPC port.
  The ten busy rejections occur under the high-bit `$A1/$85` ownership period.
- The 23 COP `$01` requests incurred five busy rejections and 18 lane
  replacements. Of 23 COP `$12` requests, six completed, one was overwritten
  in the mailbox, three were busy-rejected, and 13 were lane-replaced.

The first extended Aitos replay with 16 added voices
(`runs/20260824-173609/`) removed every native loss but measured 22 simultaneous
effect lanes during the boss-death burst. Eight paired requests were active and
three more were queued when the recording ended; the largest observed start
delay was 241,358 APU cycles. This is backpressure rather than a drop, but its
roughly 0.24-second latency is avoidable.

The implemented pool therefore has 32 added voices. Replay
`runs/20260824-174757/` starts all 118 exposed requests: 107 finish and all 11
remaining boss explosions are already active when the recording ends. There
are no queued-at-shutdown rows and the largest request-to-sequence-start delay
falls to 10,400 APU cycles. It again records zero native/extended loss and zero
music suppression. Its final WRAM (`d8659055...7facca`), SRAM
(`11dcdfb6...b79858`), and dispatch-log (`e4736323...f651`) hashes are identical
to the authentic Aitos run, and all 24 song event identities match.

### Isolated PCM parity

The DSP regression test initializes the same looping BRR source, asymmetric
left/right pan, gain, and shared echo state on authentic physical voice 7 and
extended virtual voice 8. Across 512 stereo frames, the PCM output, complete
`DspChannel` state, and touched echo RAM are byte-identical.

A natural replay comparison adds the scheduling boundary that the unit test
deliberately removes. Authentic `runs/20260824-170303/` and extended
`runs/20260824-170825/` isolate the completed COP `$07` at frame 492 from site
`$01:8C1C`, with Music 0, SFX 100, and the dialogue blip disabled. After the
central KON/KOF cadence fix, `tools/compare_native_audio_pcm.py` measured 1,190
versus 1,194 active stereo frames, a four-frame alignment lag, and 3.5055% RMS
error relative to the signal. That is inside the 5% natural-replay threshold;
the remaining difference is consistent with the two runs entering on different
DSP/interpolation phases rather than different voice mixing.

### Quick-state continuation

The paced extended Aitos replay `runs/20260824-182100/` saved quick-state slot
99 at game frame 4611 in the natural boss-death multi-effect burst, loaded it
at frame 4651, and then replayed the same interval in the same process. The
native PCM following the restored sample clock matches the first pass byte for
byte for 6,524 stereo DSP frames (about 204 ms), beginning one DSP frame after
the repeated frame-4611 request. That exact interval crosses the next repeated
CPU request and ends only after its asynchronously scheduled port command can
affect the SPC. Later CPU-to-SPC arrival time is intentionally real-time and
may move slightly with host thread scheduling; the serialized APU/DSP/queue
continuation itself is sample-exact.

The result is reproducible with:

```sh
python3 tools/verify_quickstate_pcm.py runs/20260824-182100 \
  --frame 4611 --site a5cb --id 85 --minimum-exact-frames 6000
```

The rewound run reaches the recording endpoint normally, with no extended
overflow, replacement, retrigger loss, or music suppression. Its final WRAM
(`d8659055...7facca`) and SRAM (`11dcdfb6...b79858`) hashes exactly match the
uninterrupted reference `runs/20260824-181500/`. This is an in-process
quick-state contract: a boot-time load in a fresh process cannot reproduce the
recompiled game's live coroutine stack, so it is not used as a whole-run
continuation test.

## Verification matrix

- **Implemented and unit-tested:** music-update preservation on all three
  ownership branches; mailbox bypass/FIFO ordering; independent full sequencer
  contexts and later-tick restoration; paired high-bit delays; 32-voice pool
  allocation/backpressure and bit-31 trace coverage; per-lane emulated-cycle
  charging; DSP and scheduler-state serialization.
- **Implemented and live-tested:** simultaneous virtual voices 8/9, producer-
  aware duplicate coalescing, simultaneous 22-lane boss-burst demand, zero
  native transport/lane outcomes, and zero music suppression in coupled
  simulation, Fillmore, and Aitos replays; sample-exact in-process quick-state
  continuation through a natural multi-effect overlap.
- **Statically audited and parity-tested:** all 38 shipped effect sequences use
  clear PMON/NON/EON state; physical/virtual DSP voice rendering is byte-exact
  from identical state; one isolated natural request is within 3.5055% RMS
  after phase alignment.
- **Still required for broad release confidence:** full-playthrough recordings
  for frequency coverage beyond the coupled action-stage and high-bit collision
  fixtures.

## Evidence and confidence

High confidence:

- eight native voices and their register layout;
- song X=`$00-$0E`, event X=`$10`, ordinary SFX X=`$12`;
- physical masks `$40/$80` and voices 6/7;
- high-bit events duplicate onto both lanes, block port `$F7` via `$35`, reject
  positive port-2 events, and can themselves replace an existing pair;
- music DSP writes are suppressed by `$1A & $47` while sequencing continues;
- the common sequence table, pointers, and initial SRCN values;
- direct BRK/COP post sites listed above.

Still incomplete:

- player-facing names for every sequence ID;
- complete data-selected/dynamically computed BRK caller coverage;
- exact natural-game frequency of each of the four authentic-mode loss
  mechanisms across a full playthrough;
- broader content coverage for natural replay output parity.

The static driver map is cross-checked against the live captures here and the
earlier capture documented in `research-symbol-map.md`.

External register references: Nintendo's *SNES Development Manual*, Book I,
section 7.1 (DSP register map), and the ares SFC DSP implementation, which also
models `voice[8]` and the per-voice volume/pitch/source/envelope state.
