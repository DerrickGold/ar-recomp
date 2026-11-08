# Death Heim boss rush

This reference describes the USA AR1 runtime and the guarded HLE adapters for
Death Heim's optional boss rosters. Addresses are not interchangeable with
other releases or AR2. Supported regional rules still use their existing
adapters. For menu choices, see the [player manual](../manual.md#death-heim-boss-rush).
The [RAM map](ram-map.md), [ROM map](rom-map.md) and
[symbol map](research-symbol-map.md) index the underlying state, data and
native routines.

Standard selects six Act 2 rematches, Act boss swap selects six Act 1 bosses,
and Extended selects each city's Act 1 then Act 2. Independent shuffle permutes
the selected roster without repeats; the final boss remains last with both
native forms. Standard with shuffle off retains the native controller,
regardless of the unused shuffle seed.

## Implementation owners

| Module under `src/actraiser/` | Responsibility |
| --- | --- |
| [actraiser_death_heim_rush.c](../../src/actraiser/actraiser_death_heim_rush.c) | Value-only recipe codec, deterministic order, completion mask, phases and attempt tokens |
| [actraiser_death_heim_runtime.c](../../src/actraiser/actraiser_death_heim_runtime.c) | Native roots, hub routing, admission, victory, retry, teleport lifetime and replay fingerprint |
| [actraiser_death_heim_arena.c](../../src/actraiser/actraiser_death_heim_arena.c) | Six Act 1 arena recipes, prerequisite loading, placements, player anchors and retry destinations |
| [actraiser_death_heim_teleport_assets.c](../../src/actraiser/actraiser_death_heim_teleport_assets.c) | Scoped borrowing and restoration of native beam graphics |
| [actraiser_death_heim_eyes.c](../../src/actraiser/actraiser_death_heim_eyes.c) | Per-act eye filtering before shared sprite emission |
| [actraiser_scene_music.c](../../src/actraiser/actraiser_scene_music.c) | Preserve resident rush music through borrowed levels and spare-life retries |

Settings use the existing campaign/save owner. The regional session codec
stores the recipe; the regional runtime binds it on campaign acceptance and
reset. Generated C remains generated output. Hook declarations belong in
[bank00.cfg](../../recomp/bank00.cfg) and [bank02.cfg](../../recomp/bank02.cfg).

## Native dispatch contract

There are sixteen relevant guarded boundaries: fifteen Death Heim-specific
hooks and the shared scene-music hook. All require native CPU mode, direct
page zero, decimal clear and 16-bit index registers. `M/X` below names the
entry width flags, where zero means 16-bit. Native scene, owner, handler,
source and phase checks further restrict each hook; matching an address alone
does not authorize a call. Rejected predicates leave ordinary execution native.

| US entry | Owner / operation | PB / DB | M/X | Accepted continuation |
| --- | --- | --- | --- | --- |
| `$00:930E` | Arena placements | `00 / 0A` | `0/0` | `$00:9311` |
| `$00:9354` | Player anchor | `00 / 0A` | `0/0` | `$00:939C` |
| `$00:A3FE` | Boss music actor | `00 / 00` | `0/0` | `$00:A4B4` |
| `$00:A5FA` | Act 1 victory interception | `00 / 00` | `0/0` | `$00:FE89`; duplicate `$00:868F`; retired `$00:FF07` with M1 |
| `$00:F3E4` | Hub destination | `00 / 00` | `1/0` | `$00:F3F2` with M0 |
| `$00:F534` | Statue actors | `00 / 00` | `0/0` | `$00:F5A5` |
| `$00:F7B8` | Arrival beam resources | `00 / 00` | `0/0` | Re-enter `$00:F7B8`; retired `$00:FF07` with M1 |
| `$00:F7C9` | Arrival completion | `00 / 00` | `0/0` | `$00:868F`, native RTS |
| `$00:F7F2` | Act 2/final admission | `00 / 00` | `0/0` | `$00:F7F7` |
| `$00:FE9D` | Departure beam resources | `00 / 00` | `0/0` | Re-enter `$00:FE9D`; retired `$00:FF07` with M1 |
| `$00:FEEC` | Victory commit / hub return | `00 / 00` | `0/0` | `$00:FF07` with M1 |
| `$02:B1FC` | Begin prerequisite asset chain | `02 / 00` | `1/0` | Re-enter `$02:B1FC` with loader active |
| `$02:B24E` | Complete one room's assets | `02 / 00` | `1/0` | `$02:B1FC` for next room; otherwise native `$02:B24E` |
| `$02:B653` | Scene music declaration | `02 / 00` | `1/0` | Borrowed `$02:B69B`; ordinary `$02:B655` |
| `$02:BD14` | Spare-life music cache | `02 / 00` | `0/0` | `$02:BD16` |
| `$02:BD61` | Arena retry destination | `02 / 00` | `1/0` | `$02:BD65` |

The arrival handler reaches `$F7C9` through a native animation RTS trampoline.
It must tail to the existing pure-RTS leaf `$868F`: returning C `NORMAL` leaves
the native return address on the stack and leaks the actor loop's saved P into
the main loop. The duplicate Act 1 departure branch uses the same leaf. These
continuations are not general host callbacks or a foreign-game adapter API.

## Act 1 arena initialization

Natural map identity selects collision, effects and boss behavior throughout
combat. The controller's logical rush membership remains active in groups
1–6. Arena coordinates below are player ground-line inputs to the native
initializer, not the actor's final hot point. Placement sources are in bank
`$0A`; boss source records are in bank `$00`. Each source record is twelve bytes
before its handler entry.

| City | Group:room | Player X / ground Y | Placement | Boss source record |
| --- | --- | --- | --- | --- |
| Fillmore | `01:01` | `3920 / 720` | `$B28D` | `$AD45` |
| Bloodpool | `02:01` | `4016 / 416` | `$B55C` | `$B786` |
| Kasandora | `03:02` | `2624 / 704` | `$B8D6` | `$C66F` |
| Aitos | `04:03` | `248 / 160` | `$BBC2` | `$D646` |
| Marahna | `05:03` | `384 / 240` | `$BD9F` | `$D974` |
| Northwall | `06:04` | `368 / 480` | `$C23D` | `$E7C6` |

Loading runs each room's native asset script from room 1 through the target
inside the original force-blanked loader. `$B1F7` owns the single PHP;
`$B1FC` re-enters the native JSR `$B250` without adding another frame, and the
last `$B24E` retains its original PLP/RTL. This preserves inherited resources
that a direct target-room warp misses. Ordinary level-music requests are
suppressed throughout this owned chain, including temporary prerequisite maps.

Between prerequisite scripts, native `$02:BAF5` runs with M1/X0 through the
paired long-call adapter before advancing the room; the final room retains its
ordinary capture call. Kasandora profile `$10` (`42 08` at offsets 23/24)
captures 4 KiB of character VRAM into `$7F:B800–C7FF`. Profile `$11` (`42 88`)
retains that cache for animated sand phases. Replaying asset commands without
this capture leaves stale terrain data. Marahna and Northwall also inherit
character-animation frames from an earlier room. The cache is disjoint from
the beam resources; see
[native room assets](rom-map.md#death-heim-encounters-and-teleport-resources).

Player type `$83` initializes the anchor and cached camera through `$939C`;
the later native `$B727` builds the scrolled tilemap before fade-in. Thirty-two
object records starting at `$0AE0` remain available before the boss at `$12E0`;
`$1320` terminates the placement list. Only the boss placement and required
environmental actor are admitted, avoiding traversal enemies, pickups and
wave gates. Aitos's native placement Y `$FF` intentionally wraps its dragon
above the screen and must not be clamped.

Bloodpool requires its hidden environment placement `$0A:B4CB = 00 00 FF 07`.
Native type 7 creates handler `$00:B919`, which calls `$02:9204` each update to
build the water-perspective table and configure HDMA channel 2. It precedes
the boss so the ordinary pre-fade actor pass establishes the raster before
scanout. Boss graphics alone do not establish complete environment readiness.

The borrowed level supplies terrain, while the timer uses that city's native
Death Heim rematch profile: `$06,$0F,$15,$1C,$24,$2C`. These select 200 seconds
except Marahna's native 300, subject to the existing regional 200-second rule.
Hub return preserves current HP, magic and sword power; native boss-kill score
and extra-life rules still apply. Native spare-life retry refills HP, consumes
a life, retains magic and clears sword power. Ordinary act-clear rewards, town
counts and settlement are bypassed.

## Native teleport resource ownership

Act 1 arrival temporarily borrows the admitted boss's 64-byte record for the
native beam. The player is hidden and input `$F4` is zero until completion;
then resources and the saved boss record are restored before its original
initializer runs. Native multipart creation and boss behavior remain intact.
Departure uses `$FE89`'s native waits and engulf animation, committing victory
only at `$FEEC`. Both beams copy the player's horizontal and vertical flip
bits, preserving the remaining attributes, including Aitos's inverted case.

| Borrowed resource | Range / source |
| --- | --- |
| Animation WRAM | `$7E:4000–533D`, 4,926 bytes decoded from `$18:8000` |
| Character VRAM | Words `$3000–3FFF`, decoded from `$17:9058` |
| Object palette | CGRAM colors 128–191, from `$1C:E4F8` |
| Object selection | Temporary OBSEL `$01`; previous effective value restored |

The animation blob overlaps the first 830 bytes of the boss bank at `$5000`.
Saving only the nominal `$4000` bank corrupts the restored boss. Decoding is
bounded and length-checked; VRAM replacement uses a generation-checked atomic
patch. Copied restoration state belongs to the runner, CPU, scene and stable
reset/load generations, not the borrowed span's per-tick lifetime.

Restore happens before boss execution resumes or hub loading begins. Retry
cancels and retires the beam after restoration. Reset restores only while its
original owner and scene remain valid; a new scene/load discards old ownership
instead of overwriting new resources. Pending teleport-owned OBSEL and palette
requests are retired so the next NMI cannot undo restoration. This is a scoped
graphics transaction, not a general WRAM/PPU save state.

## Music, progress and lifetime

Death Heim's resident song is `$1B:9470`, track 1. The shared `$B653` hook skips
level upload/play declarations for the target and its owned prerequisite
chain. The exact-arena `$A3FE` hook skips the music actor's attention, halt,
upload and play states, retaining native `$A4B4` parent-wait release and actor
retirement. Neither path sends a replacement play command. BRK/COP sound
effects and the final boss's native `$18:D4FA` song remain unchanged.

Native death leaves the rush song playing but `$02:BD14` invalidates its source
cache with M0 `STZ $AC`. For a borrowed Act 1 arena with a spare life, skipping
only this store preserves `$AB–AD`, preventing a redundant restart on the next
hub return. Ordinary stages, native Act 2 fights and zero-life exits retain
their original cache policy and death sound effects.

Encounter IDs are `(city - 1) * 2 + act - 1`, with final ID 12. The model freezes
the recipe and deterministic Fisher–Yates permutation at entry. A run ID and
attempt token reject stale admissions and completions. Phases distinguish hub,
preparing, combat, returning, retry, final, complete and abandoned. The native
`$0347` compatibility projection stays within 0–6 before the final; native
final defeat and epilogue retain 7 and 8. It never becomes a twelve-fight count.

Victory requires the admitted aligned root, map, native source, animation
identity and current token. Child death or generic HP zero is insufficient.
Player death wins over simultaneous victory; retry preserves completed IDs
and order while invalidating the old attempt. Genuine exit, campaign switch
and session reset clear ownership; controlled transitions to natural Act 1
maps do not. Exhausted-life Story and Special retain native exit routes.

Statues retain city identity. Extended maps screen-left to Act 1 and
screen-right to Act 2; Aitos's single visible eye flashes on the first victory
and stays lit until both are complete. Standard/Act boss swap extinguish the
city's eyes together. Filtering precedes shared sprite emission, so authentic,
widescreen and captured Diorama/Voxel eyes use the same completion policy.

The paired flare actor's visual 0 is the forehead spark, not another eye.
Native state 6 holds that spark for 90 updates before burst visuals 25–38,
then finishes with blank visual 24. In Extended mode, only that state's burst
frames move six pixels toward the extinguished eye: left for Act 1, right for
Act 2. Aitos receives no offset. The forehead spark and surviving eye parts
retain their original statue anchors, including on mirrored statues.

## Save and replay compatibility

The independent seed is `0–999999999` (default 1), algorithm version 1. Campaign
metadata v71 stores a canonical 16-byte `ARDHRSH1` recipe after the randomizer
recipe. Legacy metadata defaults to Standard/off/1. An exact default keeps
the prior v69/v70 encoding; any recipe difference uses v71. Native 8 KiB SRAM
and checksums are unchanged. See the [v71 format](save-format.md#death-heim-rush-recipe-version-71).

Managed edits persist through the campaign transaction before publication;
Continue restores the campaign recipe over installation preferences. Active
rushes, including native Standard, and recording/replay lock these settings.
Default native replay digests remain unchanged; custom recipes and canonical
controller fields participate in the replay fingerprint without struct padding.
The recipe does not persist partial completion, native stacks or mid-rush saves.

## Verification and limits

ROM-free tests cover the model, runtime, arenas, eyes, teleport assets and scene
music, plus settings, campaign, codec and save integration. With configured
build directories, run the focused tier with:

```sh
cmake --build build-tests --target actraiser_death_heim_rush_test \
  actraiser_death_heim_runtime_test actraiser_death_heim_arena_test \
  actraiser_death_heim_eyes_test actraiser_death_heim_teleport_assets_test \
  actraiser_scene_music_test
ctest --test-dir build-tests \
  -R '^actraiser_(death_heim_(rush|runtime|arena|eyes|teleport_assets)|scene_music)$' \
  --output-on-failure
python3 tests/death_heim_generated_test.py --matrix
python3 tests/death_heim_generated_test.py --build-dir build-death-heim \
  --roster extended --pip-video --captures --output build-check/death-heim-pip
```

Add `--settings settings.ini` to the GPU command to copy the installation's
graphics settings into the isolated run, including its Voxel configuration.
The source settings file is read-only; without it the harness uses its fixture.

The same focused CTest targets support the sanitizer build. The generated
regression requires a current Unix Makefiles game build and the US ROM;
[test documentation](../../tests/README.md#generated-death-heim-regression-local-rom)
describes retries, captures and isolated fixtures. It relinks a diagnostic
observer with production objects, using invulnerability and controlled lethal
damage after live combat. It is not a combat-balance or human playthrough test.

GPU regression mode exercises actual compositor presentation with the PiP
inset, including a complete Extended roster and both final forms. It rejects
width, stack and render failure diagnostics. It does not exercise a separate
comparison window. Asset equality and successful rendering do not prove every
terrain pixel, regional combination or presentation mode correct. Environment
actors and inherited animation caches require explicit checks beyond boss
graphics. The harness also does not assert the complete ending-tour dialogue
traversal or arbitrary foreign-game campaign isolation.
