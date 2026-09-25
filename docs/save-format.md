# ActRaiser save formats

ActRaiser's native battery save is an 8 KiB SRAM image. This reference documents
the US layout and the Recomp's lossless INI alternative. For the in-game editor,
see the [manual](manual.md#save-editor); for corresponding memory locations,
see the [RAM map](ram-map.md).

The starting field map came from
[RyudoSynbios/game-tools-collection](https://github.com/RyudoSynbios/game-tools-collection/tree/master/src/lib/templates/actraiser/saveEditor).
That editor declares European offsets and subtracts two bytes for USA saves.
The offsets below already include the US adjustment. Verified and uncertain
fields are distinguished; preserve unnamed bytes when editing.

## 1. Geometry and written footprint

| Range | Purpose |
|---|---|
| `0x0000`–`0x1d6a` | Save payload written by the game |
| `0x1d6b`–`0x1feb` | Unwritten fill, included in the checksum |
| `0x1fec`–`0x1fef` | 32-bit checksum |
| `0x1ff0`–`0x1fff` | Normally fill; the ending routine writes `ACT` at `0x1ff0`–`0x1ff2` |

New SRAM uses `AR_SRAM_FILL` (default `0x60`). An all-zero initial image can
be interpreted as a corrupt or level-0 save. Preserve an existing file's fill
bytes rather than resetting them.

The `ACT` marker lies outside the checksum. Static code at `$02:AA9C`
writes it after the ending. The editor exposes it as Professional Mode
Locked/Unlocked. Controlled native US/JP boots with this marker now verify the
third title selection and Professional/Special initialization; earning the
marker through the full ending remains a separate test. Both codecs preserve it.

## 2. Checksum

The host defers automatic persistence while the native story-save routine
copies its payload and writes the checksum. Only a normal completion with a
valid checksum releases that transaction. An interrupted transaction preserves
the existing disk save, including at shutdown. Session-only editor changes
do not change the host's last-durable image.

Recompute the checksum after changing the SRAM payload:

```text
c1 = XOR of every uint16 word over [0x0000, 0x1fec)     ; little-endian words
c2 = SUM of every uint16 word over [0x0000, 0x1fec)     ; truncated to 16 bits
checksum = ((c1 & 0xFFFF) << 16) | (c2 & 0xFFFF)
stored at 0x1fec, 4 bytes, little-endian
```

## 3. Field map

### 3.1 Region state (verified address and encoding)

The six town states are **not standalone bytes**. For region `r` (`0..5`):

```text
base = SRAM[$1200 + r*2]
act2 = SRAM[$13B6 + r*2] & 1
state = base*2 + act2
```

| State | Meaning | Stored base / flag |
|---|---|---|
| `0` | Act 1 | `0 / 0` |
| `2` | Act 1 cleared | `1 / 0` |
| `3` | Act 2 | `1 / 1` |
| `4` | Act 2 cleared | `2 / 0` |

For example, `$1200=$01` means Fillmore Act 1 is cleared.

The region order is Fillmore, Bloodpool, Kasandora, Aitos, Marahna, Northwall.
Menu labels use **“Town State”** to leave enough room for “Act 2 cleared.”

### 3.2 USA player/status block ✅

Subtracting two from the external template's European offsets aligns the save
block with the known WRAM block (`SRAM - $11B1 = WRAM` through the player
status fields). Repository fixtures then decode to plausible values—for
example SP `20/20`, Angel HP `8/8`, Master level `1`, HP `8`, MP `0`, and next
experience `80` in a new save.

| Field | USA SRAM | Encoding/range | Menu |
|---|---:|---|---|
| Message speed | `$13B1` | uint8, `0..9` | Status |
| Angel SP current/max | `$1433/$1435` | little-endian uint16, `0..999` | Status |
| Angel HP current/max | `$1437/$1438` | uint8, current `0..24`, max `1..24` | Status |
| Player name | `$1439-$1441` | 8 characters plus terminator/padding; printable ASCII subset in observed saves | Status |
| Master level | `$1442` | little-endian uint16, `1..17` | Status |
| Master HP | `$1444` | little-endian uint16, `1..24` | Status |
| Master MP/scrolls | `$1446` | little-endian uint16, `0..10` | Status |
| Next-level experience | `$1448` | little-endian uint16; derived by the game/tool | not exposed |
| Lives | `$145C` | stored zero-based; displayed `1..9` | Status |

The external template lists Master HP as `$1246`; that lone address conflicts
with both the linear WRAM correspondence and every fixture. `$1444` is the
derived USA address used here.

### 3.3 Progress, inventory, and score fields

| Field | USA SRAM | Encoding | Menu |
|---|---:|---|---|
| Death Heim state | `$120C` + `$1240` bit 0 | locked `0/0`, unlocked `0/1`, cleared `3/1` | Progress |
| Professional mode | `$1FF0-$1FF2` | ASCII `ACT` unlocked; `FF FF FF` locked | Progress |
| Magic slots 1–4 | `$144A-$144D` | low 7 bits: 0 empty, 1 Fire, 2 Stardust, 3 Aura, 4 Light; high bit selects equipped slot | Magic |
| Equipped magic | `$145D` | spell ID `0..4`; writer sets the high bit on the slot containing that spell | Magic |
| Item slots 1–8 | `$1453-$145A` | enumerated item byte (§3.3.1) | Items |
| Act scores | `$1464 + region*4 + act*2` | little-endian packed BCD of score/10, `0..99990` by 10 | Scores |

#### 3.3.1 Item byte values

| Value | Item | Value | Item |
|---:|---|---:|---|
| `$00` | Empty | `$05` | Source of Life |
| `$06` | Source of Magic | `$07` | Loaf of Bread |
| `$08` | Wheat | `$09` | Herb |
| `$0A` | Bridge | `$0B` | Harmonious Music |
| `$0D` | Ancient Tablet | `$0E` | Magic Skull |
| `$0F` | Sheep's Fleece | `$12` | Bomb |
| `$13` | Compass | `$14` | Strength of Angel |

The Professional marker has an independent static proof: ending presenter
`$02:AA9C` stamps `ACT`. Marker consumption is verified as described above;
Death Heim, inventory, score and earning the unlock still need the manual §6.3
game round trip even though their addresses,
encodings, range validation, checksum repair, and transactional writes are now
covered by `actraiser_save_system` tests.

### 3.3.2 Story-event bitmaps ✅ — `0x120E`–`0x123D` (decoded 2026-08-17)

Two 24-byte blocks the third-party map does not cover, sitting between the Death Heim byte
(`$120C`) and the region act-2 flags (`$13B6`). Each is 6 towns × 4 bytes = **32 story-event
ids per town**, bit order **MSB-first** (id `k` → byte `k>>3`, mask `$80 >> (k&7)`):

| SRAM | WRAM live copy | Contents |
|---|---|---|
| `0x120E + town*4` | `$7F:9107 + town*4` | event **prereq/enabled** bitmap |
| `0x1226 + town*4` | `$7F:911F + town*4` | event **fired** bitmap |

The third runtime bitmap (`$7F:9137`, "dispatched this session") is **not** saved, and neither
is the ambient scene index `$7F:9222 + town*2` — so a freshly loaded save has no ambient
scenery actors until a town event re-arms it. See
[RAM map: Story-event bitmaps](ram-map.md#story-event-bitmaps-7f9107-7f914e).

Evidence: in two independent runs (`runs/20260817-180830`, `runs/20260817-184251`) the 48-byte
SRAM block matches the WRAM block byte-exactly for every town except the one being played,
which differs only in the bits for events that fired after the last in-game save.

### 3.4 Town simulation block ✅ — `0x0000`–`0x11ff` (decoded 2026-07-17)

The third-party map starts at `0x1200` and documents nothing below it. The
block below it stores the persistent side of the structure-record system:

| SRAM | WRAM live copy | Contents |
|---|---|---|
| `0x0000 + town*0x80` | `$7F:6800` (current town) | road-map words, one per 8×8 selector square (bit `$40` obstructs, `$80/$100` bridge-built per axis, `$200` obstacle layer) |
| `0x0300 + town*0x80` | `$7F:9250 + town*0x80` | built-square dedup list: 64 × 2-byte **square** coord pairs (x,y ≤ 7), `$FFFF` = empty |
| `0x0600 + town*0x200` | `$7F:6BE7 + town*0x200` | structure-record array: 128 × 4 bytes `{cell X, cell Y, flags/type, action/progress}` — ends exactly at `0x1200` where region progress (§3.1) begins |

The per-cell flag maps (`$7F:3800+`) and drawn tile maps are **not** saved —
they are regenerated on town entry from these arrays plus ROM terrain data.
Decode any save's town state with `tools/town_structs.py` (works on WRAM
dumps; the same record layout applies at the SRAM offsets above).

**Host extension area (recomp-only, 2026-07-17).** The `fix_bridge_limit`
setting migrates completed bridge records out of the 128-slot arrays into
otherwise-free SRAM the ROM never writes (`[0x1d6b, 0x1fec)` is inside the
checksummed range, so the game's own save path can persist it):

| SRAM | Contents |
|---|---|
| `0x1D70` | magic `"AXB1"` (area treated as empty when absent) |
| `0x1D74`–`0x1EF3` | 6 towns × 16 × 4-byte bridge records, same layout as the main arrays; `flags == 0` = free slot |

Old saves are untouched until the first migration writes the magic. Sidecar
records are accepted only when they describe an active, completed bridge at a
valid cell and the corresponding orientation bit (`$0080/$0100`) still exists
in that town's native road map. Invalid/stale records and later duplicates are
reclaimed; a matching bridge still present in the main array wins for census,
marks, and rendering, preventing an interrupted or older migration from being
double-counted.

Bridge migration remains session-only until the normal game save commits
the complete checksummed image. Exiting without saving does not persist newly
migrated records. The original ROM never reads this extension area.

### 3.5 Lairs, growth and SIM actor cache

Native US/JP Save/Continue and reset/Continue fixtures verify these fields.
The SRAM offsets in this table coincide; **the entire saves are not thereby
interchangeable**. Runtime addresses below are the low word in WRAM bank `$7F`.

| SRAM | Size | Meaning | US / JP WRAM |
|---|---:|---|---|
| `$14B3` / `$14E3` | `$30` each | 24 lair X / Y words | `$9568/$9598` / `$955C/$958C` |
| `$1513` | `$30` | Lair imagery/state flags, including sealed `$8000` | `$95C8` / `$95BC` |
| `$1543` | `$30` | Lair monster types | `$95F8` / `$95EC` |
| `$1573` / `$15A3` | `$30` each | Lair reload delays / active countdowns | `$9628/$9658` / `$961C/$964C` |
| `$15D3` | `$30` | Lair actor-record addresses | `$9688` / `$967C` |
| `$1603` | `$30` | Remaining monster stocks, not seal flags | `$96B8` / `$96AC` |
| `$1633-$1D52` | `$720` | Six towns × eight cached SIM actor records of `$26` bytes | `$97DA` / `$97CE` |
| `$1D53` | `$0C` | Six pending town-growth words | `$9EFA` / `$9EEE` |

Save entries are US `$03:A656`, JP `$03:A42E`; load entries US `$03:A83A`,
JP `$03:A60A`. The actor-cache table is US `$03:8111`, JP `$03:810E`, with
one `$0130`-byte slice per town. US `$03:8168` / JP `$03:8165` copies the live
eight records at `$0B30-$0C5F` to the current town's cache before Progress Log
saves. US `$03:813F` / JP `$03:813C` restores them on town entry. The native
restore loop uses overlapping word transfers with byte increments, including
one trailing byte past the nominal slice; preserve this CPU detail when
modeling the exact routine footprint.

Booted fixtures verify the cache payload byte-for-byte and a pending class16
soul surviving a cold load: stock stays zero and the soul credits one growth
on its later return. This actor cache must not be confused with the unsaved
ambient scene index in §3.3.2. Companion metadata for regional rules
must distinguish restored actors from new spawns.

Native town-switch fixtures also preserve the pending soul while visiting
another town, award once on return, and then reuse its live slot for a Dragon.
The cached slice is a snapshot: it keeps the old soul until the next cache
operation updates it. A second departure/re-entry preserves the new Dragon
without repeating the soul reward. Reset/Continue after an unsaved reward
restores the earlier saved soul and growth0, allowing its valid reward again.
Companion state must roll back with SRAM, not apply a session-global reward
deduplication set across restored timelines.

Saving is not one instantaneous SRAM write. In controlled native traces,
several payload-writing frames precede the final checksum stores at US
`$03:A82E/$A833`, JP `$03:A5FE/$A603`. Future companion persistence must follow
completed-save ownership; a changed-byte observation alone is not proof that
the payload and checksum already describe one complete save.
Reference-core video frames are not host coroutine yields: the native save
body has no frame wait, and the host checksum HLE is yield-free. Normal host
auto-persistence runs after the coroutine returns; partial host-save exposure
is not demonstrated by the native multi-frame trace. Abnormal-exit behavior
still needs a host-specific test before adding a companion commit contract.

---

## 4. Save codec and persistence backends

The game supports two save formats:

| Backend | Default path | Purpose |
|---|---|---|
| `native-srm` | `saves/save.srm` | Existing behavior and byte-compatible interchange with emulators |
| `ini` | `saves/save.ini` | Human-readable verified fields plus a lossless copy of the complete SRAM image |

`native-srm` is the default. INI mode is an
explicit setting, not an automatic preference based on which files happen to
exist. Exactly **one active backend and path** is authoritative per session;
auto-persist writes only that target. Import/export are separate actions and do
not silently switch the active backend or update both files.

This `save.ini` is game data and is separate from the menu-owned
`settings.ini`. The latter stores runtime preferences such as the chosen save
backend; it must never contain the SRAM payload.

### Managed save slots

Normal interactive boots use ten numbered directories, `saves/slots/01` through
`saves/slots/10`, beneath the launcher-selected game data root. Each holds
`save.srm` or `save.ini`, its `.archeckpoint`/`.arname` companions, and optional
`new-game.ardraft`. Visible numbers map to internal slot IDs 0–9. `save_backend`
selects the format for new slots; occupied slots retain their pinned format.
Diagnostic paths, recording/replay and headless boots keep the external-save
behavior. Import/export exchange files use `saves/imports` and `saves/exports`;
managed backups and redevelopment recovery directories use `saves/backups/01`
through `saves/backups/10`.

Native launchers resolve portable/custom/per-user storage and pass an absolute
`AR_USER_DATA_DIR`. The game honors it before executable-relative folder-bundle
anchoring, preserves absolute launch arguments for restart, and resolves all
save/settings leaves beneath that working root. Slot indexes contain only IDs,
formats and relative layout information. See [data locations](manual.md#launching-the-game)
for the Windows, macOS and Linux roots.

The collection owns a process lock at `saves/slots.lock`. It reads only the
selected format for each slot and does not select a backend by modification
time. Missing occupied images, orphan companions, corrupt indexes and malformed
prepared games require recovery. They cannot silently become fresh SRAM.

`slots.armanager` is a 264-byte canonical little-endian record: eight-byte
`ARSLOTS3` magic, active/previous slot bytes, a layout byte (2 or 3), a pending
legacy-native-adoption byte (0 or 1), four reserved zero bytes, ten
24-byte records, and an eight-byte FNV-1a checksum. Each slot record contains
backend, ever-saved, prepared, checkpoint-required and prepared-backend bytes,
three reserved zero bytes, a 64-bit Unix
last-commit time and a 64-bit hash of the committed 8 KiB image. Legacy or changed
images show filesystem modification time as approximate. Timestamps follow a
successful native image/checkpoint commit, rather than viewing or selecting a
slot; Unicode name-companion writes retain their existing retry behavior.

Version 1 and 2 indexes migrate on open under the same collection lock. Version
3 with layout 2 records an interrupted migration: old paths remain authoritative
until every destination copy is verified and layout 3 is atomically published.
The original index, root save files and drafts are retained in `legacy-layout`.
Root originals are removed only after publication and only when byte-identical
to their retained copies; cleanup can retry. Destination or retained-copy
conflicts stop migration without replacing either version. Pending slot-switch
fingerprints survive because they bind contents rather than absolute paths.
Historical `actraiser.srm` adoption converts to the selected backend and carries
matching companions; a genuine metadata-free save remains legacy.

An observed checkpoint, a prior managed
commit, or a prepared campaign establishes the checkpoint requirement. A genuine
legacy Slot 1 remains eligible for adoption. The requirement is persisted before
managed writes and after metadata-only upgrades; a missing physical or logical
checkpoint then blocks load/Continue, export and further writes. It never becomes
a legacy US/default campaign merely because its companion disappeared.

Preparing an empty slot stores its proposed format separately. The running
writer keeps the committed backend through failed requests. Only acknowledgement
of a validated new-game restart promotes the prepared backend to the slot's
committed backend.

`slot-switch.arrequest` is a 40-byte restart intent with `ARSWITCH` magic,
version 1, source/destination/new-game bytes, four reserved zero bytes, a 64-bit
destination fingerprint, eight reserved zero bytes, and an eight-byte FNV-1a
checksum. The fingerprint includes the selected file, `.archeckpoint`, `.arname`
and prepared draft with explicit presence and length markers. It detects files
changed after review. FNV is an integrity/change detector, not authentication.

Before requesting restart, the host completes outgoing durable writes, rejects
unfinished native transactions, flushes settings and disarms unscoped boot
editor overrides. The source stays active until the next boot has validated and
loaded the destination, initialized its regional/randomizer state, and
acknowledged the request. It then removes the intent before allowing gameplay
writes. Failed requests preserve the source; failed boots offer recovery.

An empty destination stores its copied recipe and starting rules in
`slots/02/new-game.ardraft` (numbered by visible slot) using the canonical regional
session codec. The normal title/new-game path consumes that setup without
rerolling the seed. The draft remains available across launches before the
first Progress Log save. It is retained afterward for recovery diagnostics;
the occupied slot's native save and checkpoint become authoritative. A durable
ever-saved intent precedes the first native write so an interrupted first save
cannot be mistaken for an unused slot.

### Unicode player names and emulator interchange

Unicode names do **not** change the 8 KiB SRAM layout, its native name field,
or its checksum algorithm. Enhanced name entry retains the original USA
keyboard's compatibility bytes in WRAM; the game copies those bytes into SRAM
when saving. These bytes are a fallback spelling, not a general transliteration
of every supported script.

The full UTF-8 name is stored in a separate companion file by appending
`.arname` to the active save path: `save.srm.arname` or `save.ini.arname`.
Copy both files together to retain the enhanced spelling on another Recomp
installation. A SNES emulator needs only the ordinary `.srm`; it displays the
native fallback name. Campaign archives include the enhanced name; raw SRAM/INI
exports contain only the game image. Raw import accepts a matching valid name
companion when one is supplied beside the source.

Companions contain `ARNAME1\0`, the native 32-bit checksum (little-endian),
nine compatibility-name bytes, a 16-bit UTF-8 byte length (little-endian), and
that many UTF-8 bytes. Names allow up to eight grapheme clusters and 256 UTF-8
bytes. On load, both checksum and compatibility spelling must match; malformed,
missing, or stale companions fall back to the native name without rejecting the
game save. Playing and saving in an emulator can change the checksum and thus
invalidate the old companion when returning to Recomp.

The accepted name can precede the first battery save. It remains host-owned
session metadata until SRAM contains its compatibility spelling. Subsequent
native saves update the companion's checksum association, even with enhanced
rendering disabled. Each file is replaced atomically, but the two files are
not one filesystem transaction: a companion write failure is retried, and an
interruption between writes can lose the enhanced spelling, not corrupt SRAM.

### Regional campaign checkpoints

Regional rules use a second companion, `.archeckpoint`,
appended to the active path (`save.srm.archeckpoint` or `save.ini.archeckpoint`).
It leaves the native image unchanged. Keep it with the save when moving between
Recomp installations; an emulator still needs only the `.srm`.

The companion binds its payload to all 8,192 native bytes, including the
completion marker outside the cartridge checksum. It retains the candidate and
one prior checkpoint. The host writes it before replacing the native file, so
an interrupted save can select the metadata matching whichever image reached
disk. A damaged, newer-format, or unmatched companion is preserved and reported
as an error, not silently applied to another campaign. Older rollbacks beyond
the retained pair need their corresponding companion backup.

Accepted New Game creates a fresh campaign identity in memory; it does not
overwrite the old saved campaign. The native story-save completion captures
that campaign's settings for the subsequent host write. Continue restores the
matching checkpoint. Legacy saves without metadata start with US rules in
memory; this does not claim that historical lair counts have been reconstructed.
Automatic completion-marker writes and persistent editor changes retain the
durable campaign's settings, even while a different unsaved game is running.
**Export campaign** and automatic backups use a single `.arsave` archive.
`SaveSystem_ExportCampaign` reads the durable disk image and its matching feature
payload and optional enhanced name. It rejects changed images, invalid companions
and incomplete native transactions. It never copies an unsaved New Game's rules.
The atomic archive contains:

| Field | Encoding |
| --- | --- |
| Magic | Eight bytes `ARSAVE01` |
| Payload/name sizes | Two little-endian 32-bit lengths |
| Native image | All 8,192 canonical SRAM bytes |
| Campaign payload | Canonical regional-session encoding, at most 32,768 bytes; zero for legacy |
| Enhanced name | UTF-8 bytes, no terminator; optional, at most 256 bytes |
| Integrity | Little-endian FNV-1a 64-bit hash of all preceding bytes |

Import validates bounds, exact length, integrity, SRAM checksum, name and feature
codec before replacing gameplay. The feature owner rebinds only the destination
slot ID; campaign identity, histories, requested/effective regional rules, seed
and generator recipe are retained. Import commits through the normal checkpoint
transaction, installs the donor name (or retires the previous name), and the
menu requests a restart. A post-commit name failure remains retryable and does
not falsely report that the gameplay import failed. The source archive remains
available for recovery.

Raw SRAM/INI exports still contain only cartridge data for emulator interchange.
Raw import accepts a matching regional companion and optional valid name beside
the source; missing regional metadata uses legacy defaults, and a companion from
a different slot is rejected. Use the campaign archive to transfer between slots.
Managed automatic backups use `backups/01/backup-YYYYMMDD-HHMMSS-NNN.arsave`,
numbered by slot, and preserve the entire pre-edit campaign once per process
session. Exports use `exports/slot-01-YYYYMMDD-HHMMSS-NNN.<format>`. An exclusive
`.pending` directory reserves each name; it is released on success or failure,
and an interrupted reservation is never reused. Failed backups block the edit.
External diagnostic saves retain their adjacent `<active-save>.bak-...arsave`
backup behavior. Default imports prefer `imports/import.arsave`, `.srm`, then
`.ini`, with the older root-level names as fallbacks. `AR_SAVE_IMPORT` wins.

The complete recovery-copy API is separate from ordinary Export. It reserves a
new directory and writes `save.srm`, its matching regional companion (if present)
and its Unicode-name companion (if present). It never replaces an existing
directory. The native file is written last, so a failed copy cannot look complete
while missing a required companion. Partial directories are retained on failure.
It accepts only a completed, persisted save whose disk image still matches;
the caller must first persist a coherent story image, including cached actors.
Regional metadata comes from that saved campaign, even if a different unsaved
New Game is running. Population conversion creates this copy after confirming
the current game state in the Sky Palace. Restoration is manual, as described
below. Campaign archives offer the portable import/export path; raw exports do
not substitute for a complete recovery copy.

`SaveSystem_CommitStorySnapshot` persists such a completed game-owned projection
with the current campaign's metadata, then replaces live SRAM and its shadow.
It rejects pending/aborted native saves. Failed commits leave live SRAM and
the durable image unchanged; the checkpoint journal can retain a retry
candidate without selecting it on reload. The result distinguishes an
uncommitted image from a committed image whose Unicode-name companion needs
retrying. A caller must never roll back committed gameplay or repeat a
destructive conversion because that companion write failed. Neither API
alone makes redevelopment safe. The game-owned conversion runs at the Palace
selector boundary `$01:85A2`, after native active-town cache retirement. It
revalidates the confirmed campaign/revision and town footprint, saves a complete
pre-change image, and reserves `backups/<slot>/redevelopment-<random-id>/`
(or `<active-save>.redevelopment-<random-id>/` for diagnostic saves) before
mutating structures. Rule changes and the post-conversion image commit together;
a failed candidate commit restores the exact changed WRAM ranges and old rules.

To restore a complete recovery copy manually, close the game and retain a copy
of the current save and companions first. For the native backend, replace the
active `.srm` and its companions together, using the active save's basename.
Remove an old companion if the recovery has none; do not pair old metadata with
the restored image. Recovery copies always use native `.srm`, including copies
made from the INI backend: select the native backend before using this manual
procedure. Campaign-archive Import restores the enhanced name automatically.

This storage covers [the implemented regional gameplay and report options](regional-settings.md)
and retained lair histories. Gameplay/presentation presets expand into the
same individual records; there is no second saved preset name or new wire
format. A pending population-changing preset is volatile until its confirmed
Palace transaction commits the complete selection and town reset together.
The pending intent belongs to the campaign and its requested rules. Normal
activation or town progress does not cancel it; another rule edit, title/Continue
or a restart does. The Palace owner creates a fresh revision-checked preview
before asking for confirmation. No queued demolition is stored in the companion.
Normal interactive overlay changes persist immediately for the selected slot;
they are not written to the global `settings.ini`. `ArRegionalCampaign_SaveSettings`
loads the checkpoint for the unchanged durable SRAM image, checks the campaign
and previous requested rules, and commits only the new requested rules through
the existing same-image checkpoint writer. Saved effective snapshots, lair and
actor histories, arrival lock and randomizer recipe are retained. Numerical
support conversion cannot pass this path. The host finishes any completed
native save first; it does not create a new gameplay snapshot for a setting edit.
Failure leaves the live request unchanged and is reported in the overlay.

Empty managed slots use `SaveSlots_UpdateDraft` and the existing
`new-game.ardraft` format without scheduling a restart or creating SRAM.
The interactive title restores the selected slot before the palette upload;
New Game inherits its choices but receives a fresh identity and histories.
Title-screen population conversions require entering the game for Palace review.
External diagnostic paths retain their session-only title drafts and story-save
persistence; replay/recording edits remain locked.

Older pricing-only companions
load with US room limits and retain all their prices. They are upgraded only
with the next completed save, not merely by loading them.
Pricing-only and initial-time companions also default to keeping score on
checkpoint retries. Existing choices are never interpreted as a complete
regional preset. Companions predating construction-wait support default to
US waits. Native Continue still restores its own town state; the regional
reload applies on the next wait expiry, not as a save-load reset.
Companions predating fishing-target support default to the US target without
inferring a Japanese target from other Japanese choices.
Companions predating development-clock support default its three independent
leaves to US pacing. Native Continue owns restoration of the town clocks;
loading the companion does not reset them to force a pending choice through.
Companions predating recovery support default to US recovery. The bounded angel
clock and the HP/SP queues live in their existing native RAM fields, not a
second host-side timer. Their initialization follows the native scene/load
lifecycle; the companion stores the requested/effective rules, not an extra
claim that transient clocks survive a battery save.
Companions predating earthquake selection default all five structure selectors
to US rules. Pending choices activate together at the next complete earthquake;
loading or changing them does not itself destroy any buildings.
Older companions keep the US Master score page available. Hiding that page
does not remove or reset the native saved act scores.
Companions predating town-menu return support default to US closing behavior.
The command captures the effective choice at acceptance; saving inside that
command stores the captured rule, without a second save or SRAM layout change.
Companions predating message-speed range support default to US 0–9 choices.
The companion records only requested/effective range policy. The confirmed
speed remains the native `$13B1` save byte, restored to `$0200`; selecting a
smaller range does not rewrite it. Cancel preserves an existing 8/9 value.
Companions predating magic-gesture support retain US dedicated-button casting.
The requested and effective gestures are distinct until a released input
sample activates the change; neither state changes physical input bindings.

New campaigns initialize exact lair histories from the native seed transaction.
The companion retains bounded alternative counters for the same observed
kills, miracles, destroyed houses and action-score settlements. It does not
contain extra monsters, sealed flags or rewards. Only the selected counters
are projected into the game's existing stock fields and subsequently saved
in native SRAM; inactive alternatives remain in the companion. Older companions have no history; loading
one does not silently claim that its earlier events were reconstructed. Normal
Continue pauses before native restoration to explain the approximation. Accepting
estimates only missing towns and atomically saves their companion metadata against
the unchanged durable image; Cancel returns to the title. A failed write offers
retry/cancel and does not activate the estimate. Existing exact or approximate
histories are retained. The acknowledgement does not repeat after a successful
write. Recording/replay never supplies implicit consent: legacy histories remain
unavailable in those protected sessions. A detected unaccounted counter write retains the affected
history but marks it diverged, rather than overwriting the game's counter.

ARREGION version 13 adds 24 named, resolved seed records to the previous 32
rule records. The whole seed table has one source, but each record binds its
actual numerical value; an inconsistent source or value is rejected. Earlier
companions default to US seeds and retain any existing history. Pending reserve
choices activate only after all 24 native counters match the current retained
projection, at a safe town update or before action-score settlement. A failed check leaves the effective choice
and native counters unchanged. The requested choice stays pending for recovery.
Version 14 adds `house_credit_tiered`, bringing the total to 57 named records.
Older companions retain tier-based US house feedback. Seed and house policies
activate together only when their complete retained stock projection can be
published safely. The native town-growth balance remains authoritative; no
alternative growth balance is retroactively applied.

Version 15 adds three independent resolved score records: `score_jp_conversion`,
`score_stock_subtract` and `score_stock_nonsecond`, for 60 named records total.
Older companions default these to US behavior while retaining their stock
histories. Seed, house and score accounting policies publish together only
after validating the complete stock projection. Score conversion/routing is
captured for the native transaction; neither a policy edit nor migration
replays a growth award. The companion still uses the same 32 retained stock
projections and does not store an alternative growth balance.

Version 16 adds `score_at_clear_card` (61 named records total). Earlier
companions keep departure-time settlement. Timing is captured at the accepted
clear-card boundary and does not reproject previous events. The 32 stock
histories use the same actual settled score, with their respective conversion,
operation and route rules. A clear's captured policies remain effective until
departure; settings changes during the tally stay pending. No native battery
save occurs midway through this sequence. Canonical replay checkpoints bind
the live completion phase and scene when non-native rules are involved.

Version 17 adds `lives_zero_based` (62 named records total). Versions 1–16
default to US display, without modifying native SRAM's zero-based life stock.
The requested source activates at the next action HUD redraw. It changes
neither the attempt allowance nor the Master report. Replay identity includes
pending/effective display behavior; US and Europe preserve previous digests.

Version 18 adds `source_life_on_take` and `source_magic_on_take` (64 named
records total). Both default to automatic collection for older companions.
The leaves are independent; collection captures the requested policy before
choosing effect or held insertion. Existing carried items are never converted
by a rule edit or migration. The native collection return chain identifies
an automatic effect, so no host-only modal state needs saving. Non-native
pending/effective Source policies extend replay identity; US/Europe aliases
preserve previous baseline digests.

Version 19 adds `skull_post_effect_frames` (65 named records total), with
90 frames for US/Europe and zero for Japan. Older companions default to US.
Use Offering captures this choice until the original item handler returns;
pending changes do not interrupt targeting or change an active wait. Native
seals, growth and item consumption remain in SRAM; the companion stores only
the requested/effective choice. Non-native timing extends replay identity
without changing earlier US/European digests.

Version 20 adds three independently sourced story-prerequisite records
(68 total): `story_fillmore_hint_threshold`,
`story_kasandora_tablet_threshold`, and `story_failed_act2_clear_compass`.
Older companions default to US. Activation occurs at the next relevant
native check; loading or changing a policy never edits event flags. Native
prerequisite/fired bitmaps remain in SRAM. Requested/effective numeric rules
extend replay identity only when behavior differs from US/Europe.

Version 24 adds five independently sourced SIM combat records (80 named
records total): `sim_dragon_threshold`, `sim_demon_threshold`,
`sim_dragon_contact`, `sim_demon_contact`, and `sim_skull_contact`.
Unchanged Bat values and Skull durability remain constants. Its feature payload
was 3905 bytes; the current version is described below.

After `ARLHIST1` and `ARLDELY1`, a 68-byte `ARSIMAC1` block preserves per-generation
rules: an 8-byte magic, active-town tag (0 unavailable, otherwise town + 1),
three zero reserved bytes, 24 cached and four active little-endian 16-bit
snapshots. Bits 0–4 select Japanese Dragon durability, Demon durability,
Dragon contact, Demon contact and Skull contact respectively; other bits are
rejected. An unavailable active town requires four zero active snapshots.
Native actor data is not duplicated or modified. Separate live/cached rule
snapshots follow the native `$03:813F/$03:8168` copy boundaries. A verified
`$03:B9EE` birth updates only its active slot, not its older cached image.
Older companions default policies and existing generations to US.
`ARSIMCOMBAT-R1` fingerprints requested/effective behavior;
`ARSIMACTOR-R1` includes retained non-US live/cached generations even after
both policy selections return to US. All-US actor snapshots preserve older
replay digests regardless of the active-town tag.

Version 25 adds six independently sourced behavior records (86 named records
total): `sim_dragon_search_interval`, `sim_dragon_extra_actor_pass`,
`sim_target_wide_coordinates`, `sim_target_full_pool`,
`sim_bat_fallback_threshold`, and `sim_bat_abduction_wait`. Its payload is
4163 bytes; the bounded companion limit is now 32768 bytes. The native SRAM
image remains exactly 8192 bytes and contains no host metadata.

Version 26 appends `construction_price_japanese` (87 named records, 4199-byte
payload). Resolved values are0/1/0 for US/JP/Europe. Requested and effective
prices remain separate until the next complete construction batch. Versions
1–25 default this rule to US and retain all existing histories and actor state.
`ARBUILDPRICE-R1` binds non-native requested/effective prices into replay
identity; US/European aliases preserve older digests.

Version 27 adds five support records (92 named records, 4357-byte payload):
`support_regular_fields`, `support_upgraded_fields`, `support_factory_class3`,
`support_factory_class4`, and `support_other_structures`. Older companions
default these to US. Unlike ordinary pending preferences, requested and
effective support must match: there is no persistent demolition request.
`ARSUPPORT-R1` includes resolved support amounts in replay identity; numerically
identical US/European choices preserve previous digests. Reduced support is
accepted only with Japanese level and population-event goals. The conversion
updates that compatible set together while preserving earned progress and
the independent Compass prerequisite rule. Conversion saves immediately,
including its recovery copy, rather than waiting for a later Progress Log.

Version 28 adds `final_island_japanese` (93 named records, 4396-byte payload)
and a nine-byte event block after `ARSIMAC2`: ASCII `ARARRIV1`, followed by a
0/1 route-lock byte. Older companions default to unlocked US behavior. An
already-unlocked native event keeps its effective route instead of adopting
a pending preference halfway through the reveal. Otherwise the eligible final
departure or Japanese Palace check captures the requested route once. The
lock survives saves before the Japanese announcement; New Game resets it.
`ARARRIVAL-R1` binds the requested/effective route and `ARARRLOCK-R1` binds its
lock into non-native replay identity. Numerically Western choices retain
existing replay identities. Native story flags and SRAM layout are unchanged.

Version 29 adds seven action-motion records (100 named records, 4616-byte
payload): `action_bird_speed`, `action_leaper_speed`,
`action_cave_recovery_delay`, `action_cave_straight_delay`,
`action_cave_high_delay`, `action_caster_low_delay`, and
`action_caster_high_delay`. Values are velocity magnitudes or stored row
delays, not total phase lengths. Versions 1–28 default them to US. Requested
and effective choices remain separate until a complete action-room
initialization; the cached room snapshot then remains fixed through later
spawns and animation phases. `ARACTIONMOVE-R1` includes the two numerical
snapshots in replay identity. US/European aliases preserve prior digests.
There is no action-actor data or new field in native SRAM.

Version 30 appends `action_sword_straight_delay` and `action_sword_high_delay`
(102 named records, 4684-byte payload). These are the Bloodpool swordsman's
stored recovery delays, not full attack lengths. Versions 1–29 default the
two new rules to US while retaining earlier requested/effective motion
choices. The snapshot uses bits7/8 without changing the replay domain or the
meaning of bits0–6, so older recordings retain their identities and behavior.

Version 31 adds `action_trap_arrow_speed` (103 named records, 4716-byte payload),
using motion snapshot bit9. Older companions default this independent velocity
to US without changing retained motion preferences. The selected speed does
not imply Japanese projectile visuals, flashing cadence or collision extents.

Version 32 adds `action_wall_head_short_hold` and `action_wall_head_long_hold`
(105 named records, 4787-byte payload). Values31/0/31 are extra active-update
pauses for the two ordinary Kasandora head variants. Snapshot bits10/11 select
their Japanese no-pause paths independently. Older formats preserve their
existing choices and initialize these leaves to US. The rules apply at the
next complete room initialization, never to an already-running native delay.

Version 33 adds `cave_emitter_interval` and `cave_emitter_pal_offset`
(107 records, 4849-byte payload). Values are 360/180/255 active updates and
0/0/1 for the PAL coordinate adjustment. They remain independent requested/
effective choices until the next complete room initialization. Older versions
default both to US. `AREMITTER-R1` binds their numeric pending/active snapshots
into replay identity only when non-native; US and equal-valued position aliases
keep previous identities. Native SRAM remains unchanged.

Version 34 adds `bloodpool_statue_shots` (108 records, 4880-byte payload),
with values1/2/2 for US/Japan/Europe. Requested/effective sources are separate
until the next complete room/retry initialization. Older versions default
this leaf to US. `ARVOLLEY-R1` binds only non-US requested/effective shot
counts into replay identity; equal Japanese/European behavior shares an
identity. The root's in-flight repeat counter lives in native action WRAM,
not the campaign companion; SRAM saves contain no active action actors.

Version 35 adds six independently keyed Minotaur/Wizard rules (114 records,
5073-byte payload): `minotaur_idle_delay`, `minotaur_throw_end_delay`,
`minotaur_throw_windup_delay`, `minotaur_jump_windup_delay`,
`minotaur_axe_offset` and `wizard_post_spread_pause`. Older companions default
all six to US while retaining their earlier requested/effective rules.
The cached room snapshot uses two bits per leaf, selecting the first regional
source with the resolved numeric value. `ARBOSS-R1` appends requested/effective
64-bit little-endian snapshots to replay identity only for non-US behavior;
equal-value aliases retain the same identity. Existing enemies, elapsed holds,
HP and projectiles are not changed by a settings request.

Version 36 adds `ice_dragon_rematch_windup` (115 records, 5107-byte payload),
with values118/106/118. It occupies boss snapshot bits12–13 without changing
the preceding six rules or `ARBOSS-R1` domain. Version35 and earlier companions
default this leaf to US. The head and body use one coherent rule captured at
the complete room/retry boundary. Their live animation cursors remain ordinary
action WRAM, not additional SRAM or companion actor state.

Version 37 adds `kasandora_pose_extents` and `marahna_arrow_extents`
(117 records, 5168-byte payload). Each stores a US/JP/Europe source and its
resolved Japanese-header selector (0/1/0). Older records retain their order;
versions1–36 default the new leaves to US. `ARCOLLISION-R1` appends the two
requested/effective bitmasks only when either is nonzero. US/European aliases
therefore preserve earlier replay identities. Extents are acquired through
ordinary actor WRAM at row boundaries; neither native SRAM nor shared assets
are altered, and settings changes activate only at complete room/retry entry.

Version 38 adds `aitos_skull_deflection`, `aitos_skull_death_reward`,
`aitos_skull_range_x` and `aitos_skull_range_y` (121 records, 5288-byte payload).
Their US/JP/Europe values are respectively0/1/0, BCD`$20/$00/$20`,32/24/32
and64/24/64. `ARPLATSKULL-R1` appends requested/effective four-bit snapshots
only for non-US behavior; numerical US/PAL aliases share replay identity.
Versions1–37 initialize all four to US. Policies freeze at room/retry entry;
live armor, score and native animation state remain ordinary actor WRAM.

Version 39 adds 63 `actor_AATT_hp` / `actor_AATT_attack` records (184 total,
6842-byte payload). `AA/TT` identify the stable area/type, not a ROM address.
Only fields whose authored values differ are stored, in the order of
`regional_actor_stats_data.inc`. Versions1–38 default all to US. Each retains
its requested/effective source and resolved numerical value. `ARACTORSTAT-R1`
extends replay identity with the two ordered 63-byte value arrays only when
either differs from US; equal-value source aliases preserve identity. A
room/retry captures all fields together. Live object HP is still native WRAM.

Version 40 appends `tanzra_minion_hp`, `tanzra_minion_reward` and
`tanzra_projectile_attack` (187 records). US/JP/Europe values are2/2/1,
BCD`$02/$02/$01` (20/20/10 points) and3/4/5. Versions1–39 default these to US
without changing existing base-stat choices. The first63 values keep the
version39 `ARACTORSTAT-R1` layout. `ARCHILDSTAT-R1` adds requested/effective
three-byte arrays only when a child value differs from US. These settings
join the room/retry snapshot; already spawned child actors remain untouched.

Version 41 appends four boss records after the version40 stat block (191
records): `tanzra_closing_delay`31/3/31, `tanzra_second_form_clock`1/0/1,
`tanzra_upper_turn_delay`10/11/10 and `tanzra_minion_turn`16/16/8. The first
and third are stored row delays, producing64/36/64 and37/38/37-update whole
sequences. Versions1–40 default these to US. Their canonical source pairs
occupy bits14–21 of the existing `ARBOSS-R1` snapshot; previous ordinals,
record layouts and baseline replay identities remain unchanged.

Version 42 appends `fillmore_left_prop_cast_hold`,
`fillmore_upper_prop_cast_hold` and `fillmore_right_prop_cast_hold` (194
records). Each stores requested/effective sources and resolved values0/0/1
for US/Japan/Europe. Versions1–41 initialize these to US. Room/retry activation
captures all three; live flags and positions remain native actor state.
The optional `ARCASTHOLD-R1` digest domain appends the requested/effective
three-bit masks after prior domains. All-US/Japanese aliases preserve the
previous replay identity.

Versions 65–69 extend the same named-record codec without changing cartridge
SRAM. Each older version defaults newly introduced fields to US; requested
and effective source choices remain separate.

| Version | Total records | Added fields | Activation owner |
| --- | ---: | --- | --- |
| 65 | 242 | `follower_symbols`, `lair_symbols`, `pyramid_detail` | Next accepted town graphics load |
| 66 | 243 | `title_background` | Next title palette/character/map load |
| 67 | 245 | `aitos_shared_pose_order`, `aitos_humanoid_pose_order` | Next room/retry |
| 68 | 247 | The two regional song-sequence fields | Actual upload of that song, independently |
| 69 | 254 | Seven `*_actor_art` fields, one per action area | Next stage entry in that area |

Town/title choices extend `ARARTWORK-R1`. Pose order uses `ARPOSES-R1`,
sequences use `ARSEQUENCE-R1`, and area artwork uses `ARACTORART-R1`. The
new domains are omitted when both requested and effective values resolve to
US, preserving previous replay identities for equivalent source aliases.
Artwork bytes, derived residency caches and file paths are never saved.

Actor artwork is pinned across the stage's rooms. Boss rooms can replace
only part of the character set, so their uploads retain that stage's choice
instead of combining a new palette with inherited graphics. The thirteen
stage-entry scripts replace the ordinary animation bank and provide the
activation boundary. Missing donor data preserves the requested choice but
uses US pixels; no donor bytes enter the companion.

Version 64 appends `action_item_art` (239 records, 8565 bytes), resolving
US/Japan/Europe to 0/0/1. Versions 1–63 initialize it to US without changing
the older Death Heim choice. The second `ARARTWORK-R1` mask bit binds requested
and effective item artwork; existing bit-zero-only fingerprints are unchanged.
Room/retry activation is separate from the new-run inventory model. No donor
pixels or file paths enter the native save or companion.

Version 63 appends `death_heim_art` (238 records, 8542 bytes). Sources
US/Japan/Europe resolve to 0/1/0. Versions 1–62 default to US. The optional
`ARARTWORK-R1` replay domain records the requested/effective semantic masks;
all-US/European aliases preserve earlier identities. Room entry/retry
captures this presentation policy. Donor bytes and availability are not
serialized: each installation validates its own private media at startup
and explicitly falls back to US graphics if the Japanese donor is absent.
Cartridge SRAM is unchanged.

Version 62 appends `aitos_mosaic_pattern` (237 records, 8519 bytes).
Requested/effective sources resolve to patterns 0/1/2 for US/Japan/Europe.
Versions 1–61 initialize it to US. Room entry/retry captures the selection;
there is no serialized HDMA table or imported code. The optional
`ARMOSAIC-R1` replay domain binds both choices; an all-US selection leaves
previous replay identities unchanged. German's measured pattern is US,
while French matches European English; language selection is independent.

Version 61 appends `enemy_placements` and `pickup_placements` (236 records,
8490 bytes). Each retains independent requested/effective sources 0/1/2 for
US/Japan/Europe. Versions 1–60 initialize both to US. Room entry/retry
captures the policy and European difficulty before building actors; it does
not rebuild the current room. `ARPLACEMENTS-R1` binds both source pairs and
the relevant European difficulty. `ARLIVEPLACE-R1` additionally binds the
prepared numerical room program after randomization, including later waves.
That transient program is not serialized into SRAM or the companion; ordinary
Continue prepares it through the next room load. US delegation omits both
domains and preserves the earlier replay identity.

Version 60 appends `scene_music_route` (234 records, 8439 bytes). Requested
and effective sources resolve to values 0/1/0 for US/Japan/Europe. Versions
1–59 default to US. An accepted scene music declaration activates the request;
there is no mid-track restart or serialized SPC state. The optional
`ARMUSICROUTE-R1` replay domain records both semantic choices; US/European
aliases leave previous replay identities unchanged. Native SRAM is unchanged.

Version 59 appends `terrain_layout` (233 records, 8413 bytes). Requested and
effective values 0/1/2 select US/Japanese/European terrain and its paired entry
and checkpoint coordinates. Versions 1–58 default to US. Room entry/retry
captures the profile; native WRAM retains the live geometry. The optional
`ARTERRAIN-R1` replay domain records requested/effective choices without changing
earlier identities when both are US. No SRAM bytes or graphics payloads are added.

Version 58 appends `terrain_hazards` (232 records, 8390 bytes). Requested and
effective values 0/1/2 identify the ordered US/Japanese/European rectangle and
damage profiles. Versions 1–57 default to US. Selection is captured on room
entry/retry; live boxes remain native WRAM, not new SRAM fields. The optional
`ARHAZARDS-R1` replay domain records both choices, preserving all-US identities.
The three profiles differ semantically, including Japanese trap geometry;
European language and mode variants share one profile.

Version 57 appends `plant_body_geometry` (231 records,8366 bytes). Requested/effective
values208/192/208 describe the coherent US/JP/European body-height profile,
including initial anchoring and closed-head bounds. Versions1–56 defaultUS.
Boss snapshot bits60–61 retain the existing `ARBOSS-R1` domain; US/European
aliases leave previous replay identities unchanged. Root/child positions and
derived metadata remain native encounter state, not cartridge save fields.

Version 56 appends `northwall_throw_sequence`, `northwall_impact_sequence`,
`northwall_projectile_offset` and `northwall_impact_offset` (230 records,
8338-byte payload).
Requested/effective values are respectively50/50/15,42/42/20,8/8/16 and0/0/2
for US/JP/Europe. Versions1–55 default these four leaves to US. They occupy
boss snapshot bits52–59 in the existing `ARBOSS-R1` digest domain; US/JP
aliases leave older identities unchanged. Room/retry entry activates them.
Derived sprite composition records are transient native animation workspace,
not cartridge SRAM or persistent host state.

Version 55 appends `action_mode_unlocked` and `action_game_over_title` (226
records, 8203 bytes). Each stores independent requested/effective sources and
values 0/0/1. Versions 1–54 default to US mode entry. Title choice construction
and Game Over acceptance activate the policy; no SRAM unlock byte is changed.
`ARMODEENTRY-R1` extends the rules digest with requested/effective two-bit
masks. US/Japanese aliases preserve previous identities. Game Over's pending
title handoff is volatile. Interactive empty-slot choices persist in the
prepared new-game file; diagnostic title drafts remain volatile.

Version 54 appends `action_spell_inventory` (224 total), requested/effective
source pairs with values0/0/1. Versions1–53 default to generic US inventory.
Only confirmed Action new-run initialization activates it. No cartridge SRAM
or PAL `$1C00` storage is repurposed: the collection belongs to the host Action
run, which has no native battery-save operation. Continue/New Game reset that
volatile owner; room/retry initialization only cancels an interrupted cast.
Unsupported debug-state restoration remains rejected before mutation.

`ARINVENTORY-R1` appends two rule booleans to the rules digest. Replay then
includes enabled run state under `ARSPELLSTACK-R1`: count, displayed icon,
selected in-flight spell, and the live collection bytes in order. Dead entries
after a pop/wrap are excluded. Disabled inventory preserves prior hashes.
The version 54 codec is 8143 bytes. Host payloads have a separate 32768-byte
bound, leaving room for remaining policies without changing the 8192-byte
cartridge image. Journals retain at most two records and allocate only during
save/load. Maximum-size two-record recovery, oversized-write rejection and
non-mutating short-buffer reads have explicit tests.

Version 53 appends `action_start_spares` and `action_start_health` (223 total).
Their requested/effective source pairs resolve to4/2/4 stored spare lives and
24/24/8 health for US/Japan/Europe. Versions1–52 default both to US. Only the
accepted Action new-run initializer activates them; ordinary rooms and
retries leave requests pending. The optional `ARACTIONSTART-R1` digest follows
the score-life domain and contains requested spares/health, then effective
spares/health. Numerical aliases preserve baseline identity. No new trailer
or native SRAM field is added.

Version 52 appends `action_score_lives` (221 total), with independent
requested/effective sources and values0/0/1 for US/Japan/Europe. Versions1–51
default it to US. Room/retry activation owns the cached rule; current lives
and score remain native state. No reward journal or SRAM field is added.
The optional `ARSCORELIVES-R1` replay domain appends two resolved booleans
after the difficulty domain; US/Japan aliases retain prior hashes. The
existing difficulty trailer is unchanged.

Version 51 appends five records (220 total): `action_difficulty_hp`,
`action_difficulty_contact`, `action_difficulty_clock`,
`action_difficulty_dragon` and `action_difficulty_tendril`. Each stores a
requested/effective source and value0/0/1 for US/Japan/Europe. A ten-byte
`ARDIFF01` trailer follows the arrival block: eight magic bytes followed by
requested and effective difficulty choice bytes, explicitly0=Normal,
1=Beginner,2=Expert. These are host choices, not PAL RAM or region ordinals.
Older versions default all five rules to US and the level to Normal.

Room/retry activation captures the five numerical effects together. The
optional `ARDIFF-R1` replay domain records two semantic bytes: HP mode in
bits0–1 (native, PAL Normal, Beginner, Expert), Expert contact addition in
bit2, clock mode in bits3–4 (60,72,48 updates), omitted dragon producer in
bit5, and single tendril bob in bit6. Inactive level choices and US/Japan
aliases preserve older hashes. Native SRAM is unchanged.

Version 50 appends `action_tree_seed_family` (215 total), value0 for US and1
for Japan/Europe. Versions1–49 default it to US. Requested/effective sources
use ordinary-motion snapshot bit13 in the existing `ARACTIONMOVE-R1` domain;
JP/European aliases share a numerical identity, and an unset bit preserves
older replay hashes. The tree's live peer/seed/visual phase tags reside in
native actor `+$3E`, not the campaign companion. Existing tagged children
finish after debug-cache loss; native death/slot replacement retains ownership.

Version 49 appends four plant records (214 total): `plant_retracting_cycle`
(0/1/1), `plant_open_sequence` (8/8/16 updates), and
`plant_high_shot_windup` / `plant_low_shot_windup` (stored delay7/7/23,
complete preparation9/9/25). Versions1–48 default them to US. Requested/effective
sources occupy boss snapshot pairs44–51; native aliases preserve older replay
identity. Native repeat state and the tagged expanded row cursor own progress.
A cacheless debug restore clears a completed protected phase before resuming
the US loop, so protection cannot remain stuck on.

Version 48 appends `pharaoh_landing` (40/24/40 updates),
`pharaoh_rematch_landing` (56/24/56) and `pharaoh_repeating_heads` (0/1/0),
for210 records. Versions1–47 initialize these to US. Requested/effective
sources occupy boss snapshot pairs38–43 in the existing `ARBOSS-R1` domain;
native aliases leave previous replay identity unchanged. An active head's
animation state and native yield frame own its current idle, not a host timer.

Version 47 appends `action_head_withdrawal` (16/20/16 updates), for207 records.
Earlier saves default it to US. Its requested/effective sources resolve to
ordinary-motion bit12, preserving earlier digest identities when unset. The
active expanded animation cursor is native actor state, not campaign metadata;
it is marked until the native sequence terminates or changes state.

Version 46 appends four Viper records (206 total): `viper_lightning_choice_program`
(0/1/2 for US AND3, JP LSR, European AND1), `viper_lightning_delay`
(21/21/17), `viper_rematch_lightning_delay` (10/10/8) and
`viper_floor_descent` (22/22/15 active updates). Requested/effective sources
occupy boss snapshot pairs30–37. Versions1–45 default them to US; earlier
replay identity is unchanged when these additions resolve to US. Policies
are captured at room/retry, independently of artwork and difficulty.

Version 45 appends `aitos_dragon_projectile_delay` (0/0/15 stored delay) and
`aitos_dragon_projectile_flight` (1/1/5 sequences before the first offscreen
check), for202 records. Versions1–44 default both to US. Their canonical
source pairs occupy boss snapshot bits26–29; zero-valued US/JP aliases leave
earlier replay identities unchanged. Requests take effect next room/retry.
The in-flight repeat count belongs to the native actor, not the campaign save.

Version 44 appends `kasandora_fire_curve`, `kasandora_fire_close_strategy`,
`kasandora_fire_child_threshold` and `kasandora_fire_bounce_threshold`, for
200 records. US/JP/Europe values are0/1/0,0/1/0,160/128/160 and242/210/242.
Versions1–43 default these to US. Requested and effective choices are stored
separately; active rooms retain their captured policy until the next room/retry.
The conditional `ARFIRE-R1` replay domain follows the existing cast-hold domain
and contains the prior digest plus requested/effective four-bit numerical
snapshots. US/European aliases preserve preceding baseline replay identities.

Version 43 appends `antlion_encounter_x` (2432/2304/2432) and
`antlion_post_volley_strategy` (0/1/0), for196 records. The strategy selects
the complete decision ordering, not a global animation multiplier. Versions
1–42 default both to US. Their canonical source pairs occupy boss snapshot
bits22–25; earlier pairs and baseline replay identities remain unchanged.

`ARSIMAC2` replaces the actor block with 124 bytes: the same 12-byte header,
then 24 cached and four live records, each holding a combat word followed by
an AI word. AI bits 0–5 select Japanese behavior in the record order above;
all remaining bits are rejected. Both words are captured together at birth.
Version 24 companions decode `ARSIMAC1` with zero AI words; older formats
default all actor metadata to US. The writer never silently drops AI state
into a version-1 block.

`ARSIMAI-R1` extends the policy fingerprint only for non-US numeric behavior.
Actor histories with no AI bits retain the original `ARSIMACTOR-R1` hash and
version-1 encoding; histories containing AI bits use `ARSIMACTOR-R2` and the
version-2 encoding. Thus combat-only and all-US recordings keep their identities,
while a surviving Japanese-rule actor still affects replay identity after the
requested setting returns to US.

Version 23 adds `level_goals_japanese` (75 named records total). Requested and
effective sources select the US/European or Japanese population table. Existing
companions default to US. Earned levels, HP/SP and the derived next-level target
remain in native SRAM; no alternate progression counters are stored. The award
owner pins a table through the complete native award sequence. A Master-report
refresh can activate it without running an award. `ARLEVELGOALS-R1` extends
replay identity only for non-US requested/effective behavior.

Version 22 adds five town-status records (74 named records total):
`town_report_flag_classifier`, `town_status_fixed_low_growth`,
`town_status_extra_plot`, `town_status_food_attempt`, and
`town_status_discard_computed_flags`. Each retains independent requested and
effective sources; US/Europe resolve to 0 and Japan to 1. Older companions
default these choices to US. They add no alternate town snapshots: structures,
status flags and population remain native. Activation is pinned through a
complete construction calculation or six-town report. `ARTOWNSTATUS-R1`
extends replay identity only for non-native requested/effective behavior.

Version 21 adds `lair_reload_japanese` (69 named records total) and a separate
108-byte `ARLDELY1` block after `ARLHIST1`: eight-byte magic, initialized,
approximate and diverged town masks, one reserved zero byte, then two sets of
24 little-endian reload words (US/Europe, Japan). The running countdowns remain
solely in native SRAM. Versions 1–20 default this choice to US and have no
retained delay block; their stock histories are unchanged.

New campaigns verify the native seed installation before initializing exact
delay histories. After legacy-history acknowledgement, older saves adopt the
actual US reloads and estimate the alternate values using the fewest native
quarter-plus-one reductions consistent with each town. A US delay of 1 cannot
reveal invisible past reductions, so adopted histories remain approximate.
Unexplained values are retained and quarantine the affected town. Interactive
Continue upgrades an already-acknowledged companion without changing SRAM or
asking again; replay performs only the corresponding in-memory migration.
Active Japanese or pending Japanese choices bind the retained delay block into
replay identity. US/European choices preserve prior baseline digests.

Canonical input recordings bind the requested and effective gameplay rules to
their initial-state identity and any recorded checkpoints. Equal-valued US and
European prices and initial room limits remain compatible with baseline
recordings. Baseline room limits also preserve older pricing-only replay
identities. Non-native rules
require a recording with an initial identity; unidentified legacy recordings
are rejected rather than played under different rules. Campaign IDs themselves
are not part of this gameplay identity.
Japanese requested or effective reserve, house or score-accounting policies also bind every retained counter and
its initialization, approximation and divergence flags to the recording.
When all accounting policies are US/European, inactive histories cannot affect
execution and preserve the baseline recording identity.
Rule edits are disabled during recording/replay, including live takeover,
because these overlay actions are not yet events in the input stream.

### Randomizer recipe (version 70)

Version 70 retains version 69's 254 regional records and appends a 28-byte
`ARRANDO1` recipe after `ARDIFF01`. No cartridge SRAM bytes change. The same
campaign checkpoint transaction captures both the recipe and the actual
requested/effective regional rules; Continue restores those rules directly.

| Recipe offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 8 | `ARRANDO1` |
| 8 | 1 | Generator version, currently 1 |
| 9 | 1 | Randomizer enabled, 0 or 1 |
| 10 | 4 | Little-endian seed, 0–999999999 |
| 14, 16 | 2 each | HP and attack percentages, 10–1000 |
| 18, 19 | 1 each | Enemy-type shuffle and map/act scope |
| 20, 21 | 1 each | Statue-drop mode and statue-position shuffle |
| 22, 23 | 1 each | Lair-position shuffle and lair-monster mode |
| 24, 25 | 1 each | Regional Action and Town rolls, 0 or 1 |
| 26 | 2 | Reserved; must be zero |

Shuffle fields use 0=off and 1=shuffle; statue drops and lair monsters also
accept 2=random. Scope uses 0=map and 1=act. Unknown generators, invalid values,
reserved bits or truncated records are rejected without replacing a save.
Replay fingerprints use these same canonical bytes, not C struct layout.

Versions 1–69 load with an explicitly unknown recipe. Their bytes remain
unchanged when merely copied or edited without adopting a campaign. Continue
uses unrandomized content rather than guessing a seed from current preferences;
the next completed story save records that baseline recipe. New campaigns
record generator 1 even with the randomizer off. See [Seeded campaigns](randomizer.md)
for user-facing setup and backup guidance.

### 4.1 Lossless INI schema (version 1)

A field-only INI is unsafe until every meaningful byte has been decoded. It
would discard the §3.4 town map, unknown fields, dead fill bytes, and future
data. Version 1 therefore stores the full image in dependency-free, chunked hex
and also emits readable views of fields that have passed the §6.3 round-trip
gate:

```ini
[Meta]
format = actraiser-sram
version = 1
size = 0x2000
rom = usa

[Regions]
fillmore = act1-cleared
bloodpool = act1
kasandora = act1
aitos = act1
marahna = act1
northwall = act1

[Raw]
; 64 bytes per line; all 128 lines are required in version 1.
0000 = 60606060...
0040 = 60606060...
...
1fc0 = 60606060...
```

The raw image is the base; recognized readable fields are authoritative
overrides. Loading proceeds transactionally into a scratch 8 KiB buffer:

1. Validate `format`, schema `version`, declared size, every raw chunk, and the
   optional ROM identity. Reject malformed, duplicate, missing, or overlapping
   chunks without changing `g_sram`.
2. Decode the complete raw image, verify its stored §2 checksum before applying
   overrides, and preserve every byte—including unknown data and the original
   power-on fill. A bad base image is an error, not something the loader
   silently repairs.
3. Parse and apply only fields described by the verified `SaveFieldDesc[]`
   registry. Unknown keys/sections are ignored with a diagnostic; they never
   become unvalidated SRAM writes.
4. Recompute and store the §2 checksum after applying overrides. A checksum
   printed in `[Meta]`, if added for diagnostics, is informational only.
5. Commit the scratch image between game frames, then re-sync the auto-persist
   persistence state.

Writing performs the inverse operation from the current `g_sram`: copy the
whole image, ensure its checksum is current, emit readable verified fields, and
emit all 128 raw chunks. Consequently, an unedited
`.srm → .ini → .srm` conversion must be byte-identical. When a readable value is
edited, only that verified field and the four checksum bytes may change.

The duplicated readable/raw representation is intentional. It makes INI saves
editable today without pretending the entire semantic map is known, and it
lets future codec versions promote raw bytes to named fields while remaining
backward-compatible with version-1 files.

## 5. Editing safely

- Back up the save before editing. Recompute its checksum after changing fields.
- **Apply for session** changes the live game without writing the save to disk.
  **Apply and save** writes the active backend and takes a timestamped backup
  when backups are enabled.
- Do not modify a file while the game is saving to it. For external editing,
  close the game first.
- Preserve unknown fields and the raw image when converting between formats.

The source checkout includes `tools/srm.py`. Its `check`, `decode`, `diff`,
`edit`, and `convert` commands support the native image and version-1 lossless
INI format. Run `python3 tools/srm.py --help` for usage.
