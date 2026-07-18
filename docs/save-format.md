# ActRaiser Recomp — Battery Save (SRAM) Format

Map of ActRaiser's 8 KiB battery SRAM, for the save codec, persistence backend,
and editing feature in [settings-system.md](settings-system.md) (`CAT_SAVE` /
`APPLY_SAVE`). The
motivation is sim-mode testing: the `AR_WARP` path stages an act transition but
**cannot reach sim-mode progress state** (population, per-region advancement,
spells, lairs). Editing the save can.

The **file-level contract** is settled: size, written footprint, fill behavior,
and checksum are documented below. The player/progress fields exposed by the
editor now have a USA address and encoding model, but the large live town-state
block in §3.4 remains unknown. That distinction is why the structured INI
format in §4 carries a lossless raw image as well as readable verified fields.

**Provenance — read this before trusting any offset below.** The starting field
map came from the open-source editor
([RyudoSynbios/game-tools-collection](https://github.com/RyudoSynbios/game-tools-collection/tree/master/src/lib/templates/actraiser/saveEditor),
`template.ts` + `utils.ts`). Its declared offsets are the European layout;
`getRegionOffset()` subtracts two bytes for USA saves. Our earlier audit missed
that translation and consequently made the whole status block look misaligned.
Applying it produces a linear SRAM→WRAM correspondence and plausible values in
all nine repository saves. The editor remains a *hypothesis source*, not an
authority: fields below also record our fixture, WRAM, or static-code evidence.

Companion docs: [ram-map.md](ram-map.md) (the WRAM side — the game copies
SRAM→WRAM on load; this is our best oracle for promoting fields to ✅),
[settings-system.md](settings-system.md) (how editing is exposed),
[progress.md](progress.md) (region/act naming).

**Legend:** ✅ Verified against our ROM (method noted) · 🟡 Address/codec tested,
but the user-visible result still needs an in-game round trip.

---

## 1. Geometry and written footprint ✅

8 KiB (`0x2000`) battery SRAM, held in `g_sram` / `g_sram_size`. ActRaiser now
attaches that canonical image to `src/save_system.c`; the selected backend owns
load, atomic persistence, explicit editor writes, and shadow synchronization.

| Range | Status | Notes |
|---|---|---|
| `0x0000`–`0x1d6a` | **written by the ROM** | the entire real save payload |
| `0x1d6b`–`0x1feb` | **never written** | retains power-on fill |
| `0x1fec`–`0x1fef` | **written** | 32-bit checksum (§2) |
| `0x1ff0`–`0x1fff` | normally retains fill; ending writes `$1ff0-$1ff2` | later static decompilation proved `$02:AA9C` stamps `ACT` after the ending; none of the nine analysed saves had reached that write |

Evidence: across 5 independent saves at different progress levels, `0x1d6b`–
`0x1feb` is an exact, unbroken run of the power-on fill byte. The last
non-fill byte is `0x1d6a` in every one.

**Power-on fill.** A never-written cartridge battery is not zero. `AR_SRAM_FILL`
(default `0x60`, matching snes9x, our reference emulator) fills SRAM at boot
(`src/main.c:530`) *because ActRaiser validates its save data* — an all-zero SRAM
is misread as corrupt/level-0. In-repo saves show two fill generations (`0x60`
and `0x27`), evidence that `AR_SRAM_FILL` was varied across dev runs. The fill
bytes sit **inside** the checksummed range but their value is semantically dead:
the game does not read them, and the checksum is simply recomputed over whatever
is there. An editor must therefore **preserve existing fill bytes rather than
zeroing them** (zeroing is harmless to validity once the checksum is recomputed,
but it destroys the fill-generation signal and gratuitously diverges from a
real cartridge).

The original nine-save footprint alone made `$1ff0` look dead. Later static
decompilation resolved the apparent contradiction: ending presenter `$02:AA9C`
does write the third-party `ACT` marker, but none of those saves contains it.
The marker lies outside the checksum. The editor can stage it explicitly as
Professional Mode Locked/Unlocked, but a real post-ending round trip is still
needed to verify the exact user-visible unlock behavior. Both codecs preserve
it losslessly regardless.

---

## 2. Checksum ✅ — verified 9/9

The single load-bearing fact for save editing: **without recomputing this, the
game rejects the edited save.**

```
c1 = XOR of every uint16 word over [0x0000, 0x1fec)     ; little-endian words
c2 = SUM of every uint16 word over [0x0000, 0x1fec)     ; truncated to 16 bits
checksum = ((c1 & 0xFFFF) << 16) | (c2 & 0xFFFF)        ; stored little-endian
stored at 0x1fec, 4 bytes
```

Verified against **all 9** `.srm` files in the repo (`save.srm`, `saves/save.srm`,
the `.bak` set, and `saves/save.srm - pre act 1`) — every one matches with
little-endian word reads *and* a little-endian stored uint32. No other
endianness combination matches any file, so both are settled.

```c
static uint32 SramChecksum(const uint8 *sram) {
  uint16 c1 = 0, c2 = 0;
  for (int i = 0; i < 0x1fec; i += 2) {
    uint16 w = (uint16)(sram[i] | (sram[i + 1] << 8));
    c1 ^= w;
    c2 = (uint16)(c2 + w);
  }
  return ((uint32)c1 << 16) | c2;
}
```

Side benefit: that 9/9 result independently confirms our recomp's SRAM
emulation produces checksum-correct saves.

**Not yet verified:** that the *game* accepts a save we wrote with a recomputed
checksum. That requires a runtime round-trip (§6.3) and is the gating test
before the editor is trusted.

---

## 3. Field map

### 3.1 Region state ✅ address/encoding

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

This explains the raw `$01` values previously mislabeled “Active”: in a blank
simulation save, Fillmore's `$1200=$01` means Act 1 cleared; Bloodpool's
`$1202=$01` plus its Act-2 flag means Act 2. The USA flag base is `$13B6`;
`$13B8` is the European-base offset shown in the external template before its
region adjustment. `src/save_system.c`, the INI codec, and `tools/srm.py` all
use the combined model.

The region order is Fillmore, Bloodpool, Kasandora, Aitos, Marahna, Northwall.
Menu labels use **“Town State”** to leave enough room for “Act 2 cleared.”

### 3.2 USA player/status block ✅ addresses; 🟡 gameplay round trip

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
`$02:AA9C` stamps `ACT`. Death Heim, inventory, score, and unlock semantics
still need the manual §6.3 game round trip even though their addresses,
encodings, range validation, checksum repair, and transactional writes are now
covered by `actraiser_save_system` tests.

### 3.4 Town simulation block ✅ — `0x0000`–`0x11ff` (decoded 2026-07-17)

The third-party map starts at `0x1200` and documents nothing below it. The
block below it is the persistent side of the structure-record system mapped in
SEAMS town §7 (validated against `save.sim-bloodpool-start.bak.srm`, whose
Fillmore array decodes to 92 active records = 77 houses + 13 fields + the two
bridge orientation variants `$91/$81` — matching the FAQ's developed-Fillmore
profile — while `complete.srm`, a warp-produced save with no sim play, shows
initial road data and all-`$FFFF` square lists):

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
render hooks read the area directly (`src/actraiser_bugfixes.c`); the ROM
itself never does.

---

## 4. Save codec and persistence backends

The runtime must keep one invariant regardless of the disk format:
**`g_sram` is always the canonical, exact 8 KiB image seen by the game.** A save
backend only translates between that buffer and durable storage. Game code must
never know whether the active file is native SRAM or structured INI.

The implemented Phase-6 runtime supports two backends:

| Backend | Default path | Purpose |
|---|---|---|
| `native-srm` | `saves/save.srm` | Existing behavior and byte-compatible interchange with emulators |
| `ini` | `saves/save.ini` | Human-readable verified fields plus a lossless copy of the complete SRAM image |

`native-srm` remains the default for backward compatibility. INI mode is an
explicit setting, not an automatic preference based on which files happen to
exist. Exactly **one active backend and path** is authoritative per session;
auto-persist writes only that target. Import/export are separate actions and do
not silently switch the active backend or update both files.

This `save.ini` is game data and is separate from the menu-owned
`settings.ini`. The latter stores runtime preferences such as the chosen save
backend; it must never contain the SRAM payload.

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
   shadow as specified in §5.1.

Writing performs the inverse operation from the current `g_sram`: copy the
whole image, ensure its checksum is current, emit readable verified fields, and
emit all 128 raw chunks. Consequently, an unedited
`.srm → .ini → .srm` conversion must be byte-identical. When a readable value is
edited, only that verified field and the four checksum bytes may change.

The duplicated readable/raw representation is intentional. It makes INI saves
editable today without pretending the entire semantic map is known, and it
lets future codec versions promote raw bytes to named fields while remaining
backward-compatible with version-1 files.

### 4.2 Backend API and ownership

Save serialization is a separate subsystem from `SettingDesc[]`. Runtime
preferences such as `save_backend` belong in `g_settings`; save payload fields
belong in a verified `SaveFieldDesc[]` registry so menu presentation, INI
decode/encode, validation, and editing share one field definition without
treating game data as ordinary app configuration.

The implementation boundary should be equivalent to:

```c
typedef enum SaveBackend {
  SAVE_BACKEND_NATIVE_SRM,
  SAVE_BACKEND_INI,
} SaveBackend;

bool Save_Load(const SaveSpec *spec, uint8 out[0x2000], SaveError *err);
bool Save_Write(const SaveSpec *spec, const uint8 sram[0x2000], SaveError *err);
bool Save_Import(const char *path, uint8 out[0x2000], SaveError *err);
bool Save_Export(const char *path, const uint8 sram[0x2000], SaveError *err);
uint32 Save_RecomputeChecksum(uint8 sram[0x2000]);
```

`Save_Load` and import decode into scratch storage and leave the live image
untouched on any error. `Save_Write` uses a temporary file plus flush/rename so
a crash cannot leave a truncated active save. Backup, commit, shadow re-sync,
and logging of the selected backend/path sit above the format adapters and are
identical for `.srm` and `.ini`.

### 4.3 Runtime implementation (2026-07-16)

`src/save_system.c` now implements this boundary. `src/main.c` attaches the
canonical `g_sram` buffer after the cartridge power-on fill, snapshots the
resolved backend, and routes boot load, per-frame change persistence, editor
actions, and clean shutdown through it. Native remains `saves/save.srm`; INI is
`saves/save.ini`. `AR_SAVE_NATIVE_PATH`/`AR_SAVE_INI_PATH` are isolated-test
overrides, not additional authoritative targets.

The test suite proves exact native size/checksum rejection, transactional
destination preservation on malformed/missing/duplicate INI chunks, unedited
cross-format byte identity, field-only mutation plus checksum, session-only
shadow re-sync, and persistent active-backend writes. The codec also validates
all nine repository fixtures. The remaining acceptance gate is the manual game
round trip in §6.3: persist one host edit, Restart Game, Continue, and confirm
the region is accepted and displayed as intended.

---

## 5. Editing rules and hazards

### 5.1 Auto-persist/editor interaction — mitigated

The former main-loop implementation diffed `g_sram` and immediately wrote
`saves/save.srm`. A naïve editor mutation would therefore have made an
ostensibly session-only edit permanent.

`SaveSystem_ApplyEdits()` now stages mutations in scratch, validates and
checksums the complete image, and re-syncs the auto-persist shadow after the
live swap. **Apply for session** consequently leaves disk untouched. **Apply
and save** takes a timestamped backup (when enabled) and performs an explicit
atomic active-backend write before the swap. `SaveSystem_AutoPersistIfChanged()`
continues to preserve game-originated SRAM changes, but only to the backend
selected at boot.

### 5.2 Checksum is mandatory
Any write must be followed by a `SramChecksum()` recompute over the mutated
buffer (§2). Skipping it means the game treats the save as corrupt.

### 5.3 Never edit mid-write
Only mutate `g_sram` between frames (the single-threaded coroutine model in
settings-system.md §7 guarantees this is safe) and never while the game is in
the middle of its own save routine.

---

## 6. Verification methodology (promoting 🟡 → ✅)

We have a better oracle than the third-party map: **our own WRAM map.** The game
copies SRAM→WRAM when a save is loaded, and [ram-map.md](ram-map.md) already
documents the WRAM side from cheat work — `$0282/84` SP, `$0286/87` angel HP,
`$0295` persistent scroll count, `$0299-$029C` spell HAVE flags, `$02AC`
equipped magic.

### 6.1 WRAM correspondence (best for the 🔴 cluster)
Load a save, take an `F2` full snapshot (already dumps WRAM + SRAM), and match
known WRAM values back to SRAM offsets. Because we know what `$0286/87` *means*,
finding the SRAM bytes that feed it settles the angel-HP/SP/MP block directly —
without trusting the third-party offsets at all. The alternative (static) route
is to find the SRAM→WRAM load routine in the ROM via `tools/romxref.py`.

### 6.2 Known-state diffing (best for §3.4)
Diff labelled saves at known checkpoints (`save.sim-blank` → `save.sim-bloodpool-start`
already isolates 752 changed bytes). Capture a save before/after a single
discrete sim action (seal one lair, gain one population tick) to bisect the
`0x0000`–`0x07ff` block field-by-field.

### 6.3 Round-trip (the gating test)
Turn **Allow save edits** on → stage one low-risk field → **Apply and save** →
**Restart Game** → Continue → confirm the game accepts the save and shows the
intended value. Repeat representative tests for town state, Status, Magic,
Items, Scores, Death Heim, and Professional mode. Phase 6 remains 🟡 until that
matrix passes. **Apply for session** is intentionally not the restart path: its
shadow re-sync guarantees that Restart/Exit leaves disk unchanged.

### 6.4 Tooling
`tools/srm.py` is the checked-in command-line companion. Its `check`, `decode`,
`diff`, `edit`, and `convert` subcommands validate the same exact size/checksum,
use the same six field offsets, and read/write the version-1 lossless INI. It
is suitable for fixture research; runtime persistence remains owned by the C
codec.

---

## 7. Open questions

1. **Slot structure.** No mirroring at `0x400`/`0x800`/`0x1000` block sizes
   (7 of 8 `0x400` blocks unique). Does ActRaiser keep >1 save slot, and if so
   where? Affects whether the editor needs a slot selector.
2. **The `0x0000`–`0x07ff` block** (§3.4) — town/terrain state? This is the
   highest-value unknown.
3. **City internals.** The reference template disables population, town level,
   offerings, and construction-speed fields. Derive these from single-action
   save diffs rather than exposing its dormant offsets.
4. **Manual acceptance matrix.** Which fields are immediately re-read on
   Continue, and which require a particular mode/town transition before their
   user-visible effect appears?
