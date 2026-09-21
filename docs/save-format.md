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

Migration is transactional with the game's save command. The host recomputes
the live checksum immediately, then resynchronizes only the sidecar and
checksum ranges in the auto-persistence shadow. That makes the migration
session-only: exiting without a native save cannot leak newly migrated bridge
records to disk. The ROM's normal `$03:A656` save transaction later changes
the native town block; auto-persistence then commits the complete, already
checksummed 8 KiB image, including the sidecar. A save-system regression test
covers both halves of this boundary. The census, marks, and scene-finish
render hooks read the area directly (`src/actraiser/actraiser_bugfixes.c`); the ROM
itself never does.

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
ambient scene index in §3.3.2. Companion metadata for future regional rules
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
native fallback name. The save editor's import/export operations transfer the
game image, not an imported companion file.

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
