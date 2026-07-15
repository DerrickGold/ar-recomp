# ActRaiser Recomp — Battery Save (SRAM) Format

Map of ActRaiser's 8 KiB battery SRAM, for the save codec, persistence backend,
and editing feature in [settings-system.md](settings-system.md) (`CAT_SAVE` /
`APPLY_SAVE`). The
motivation is sim-mode testing: the `AR_WARP` path stages an act transition but
**cannot reach sim-mode progress state** (population, per-region advancement,
spells, lairs). Editing the save can.

The **file-level contract** is settled: size, written footprint, fill behavior,
and checksum are documented below. The semantic field map is deliberately not
claimed to be complete: the live town-state block in §3.4 and several fields in
§3.2/§3.3 are still unknown. That distinction is why the structured INI format
in §4 carries a lossless raw image as well as readable verified fields.

**Provenance — read this before trusting any offset below.** The starting field
map came from a third-party tool
([RyudoSynbios/game-tools-collection](https://github.com/RyudoSynbios/game-tools-collection/tree/master/src/lib/templates/actraiser/saveEditor),
`template.ts` + `utils.ts`). It was then checked against our own `.srm` files
(2026-07-14). **It is partly right and partly wrong for our ROM** — its checksum
is exactly correct, several character/city fields decode to impossible values,
and it omits the largest live region of our saves entirely. It is treated here
as a *hypothesis source, not truth*. Every field carries a verification status.

Companion docs: [ram-map.md](ram-map.md) (the WRAM side — the game copies
SRAM→WRAM on load; this is our best oracle for promoting fields to ✅),
[settings-system.md](settings-system.md) (how editing is exposed),
[progress.md](progress.md) (region/act naming).

**Legend:** ✅ Verified against our ROM (method noted) · 🟡 Hypothesis — from the
third-party map, not yet verified here · 🔴 Contradicted — decodes to impossible
values against our saves; do **not** use without re-derivation.

---

## 1. Geometry and written footprint ✅

8 KiB (`0x2000`) battery SRAM, held in `g_sram` / `g_sram_size`, loaded by
`RtlReadSram()` (`src/main.c:544`) and persisted by `RtlWriteSram()`.

| Range | Status | Notes |
|---|---|---|
| `0x0000`–`0x1d6a` | **written by the ROM** | the entire real save payload |
| `0x1d6b`–`0x1feb` | **never written** | retains power-on fill |
| `0x1fec`–`0x1fef` | **written** | 32-bit checksum (§2) |
| `0x1ff0`–`0x1fff` | **never written** | retains power-on fill |

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

This footprint **contradicts** the third-party "Professional Mode" field at
`0x1ff0` (§3.3): that offset is in a dead zone our ROM never writes.

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

### 3.1 Verified ✅

**Region progress — `0x1200`, stride 2 per region.**

| Offset | Region |
|---|---|
| `0x1200` | Fillmore |
| `0x1202` | Bloodpool |
| `0x1204` | Kasandora |
| `0x1206` | Aitos |
| `0x1208` | Marahna |
| `0x120a` | Northwall |
| `0x120c` | Death Heim (🟡 — see §3.2) |

Values:

| Value | Meaning | Source |
|---|---|---|
| `0x00` | Act-1 (not started) | third-party map |
| `0x01` | **observed in-repo; absent from the third-party enum** — appears on the *currently active* region (present on Fillmore in a blank save, on Bloodpool in a bloodpool-start save). Best reading: "region active / act-1 in progress". Confirm before exposing. | ours |
| `0x02` | Act-1 cleared | both |
| `0x03` | Act-2 | third-party map |
| `0x04` | Act-2 cleared | third-party map |

*Method:* `save.sim-bloodpool-start.bak.srm` decodes to Fillmore `0x02`
(Act-1 cleared) + Bloodpool `0x01`, with Fillmore population 2 / level 1 and all
later regions `0x00` — exactly matching the state its filename claims.
`save.sim-blank.bak.srm` decodes to Fillmore `0x01`, everything else `0x00`.

This block alone delivers the feature's core goal (jump to arbitrary sim-mode
region progress), which is why the editor can ship on §3.1 + §2 before anything
below is resolved.

### 3.2 Hypothesis 🟡 — from the third-party map, unverified here

Do not expose in the menu until promoted to ✅ via §6. Our saves either read
zero at these offsets (so the map is untested, not wrong) or the evidence is
mixed.

| Field | Offset | Type | Evidence in our saves |
|---|---|---|---|
| Act-2 access flag | `0x13b8` +2/region, bit 0 | bitflag | all our saves read 0 → untested. Paired with §3.1 (`0x1200 + 0x1b8`) |
| Death Heim progress | `0x120c` (+ unlocked bit at `0x1240` bit 0) | uint8 + bit | all read 0 → untested. Enum: `0x1` unlocked, `0x4` cleared |
| Difficulty | `0x13b6` | uint8 | reads 0 → untested. Enum: 1 Beginner / 2 Normal / 3 Expert |
| Message speed | `0x13b1` | uint8 | reads 0 → untested (0–9) |
| City population | `0x13cf` +2/city | uint16 | **weak positive** — Fillmore reads 2 in bloodpool-start, 0 in blank |
| City level | `0x13e1` +2/city | uint16 | **weak positive** — Fillmore reads 1 in bloodpool-start, 0 in blank |
| City offerings | `0x13ed` +2/city | uint16 | reads 0 → untested |
| City speed | `0x13db` +1/city | uint8 | **suspect** — Marahna reads Stop/Normal while every other city reads 0; smells misaligned |
| Total cities | `0x13cb` | uint16 | reads 2 in both blank and bloodpool-start → suspect |
| Master level / lives / next-XP / equipped | `0x1444` / `0x145e` / `0x144a` / `0x145f` | — | level reads 0 (blank) and 11 (bloodpool-start) — plausible but unconfirmed |
| Magic slots 1–4 | `0x144c`–`0x144f` | uint8, 7-bit | all read 0 → untested. Enum: 1 Fire / 2 Stardust / 3 Aura / 4 Light |
| Inventory items 1–8 | `0x1455`–`0x145c` | uint8 | slot 8 reads `0x2` in every save → suspect |
| City scores act-1/act-2 | `0x1466` / `0x1468` | uint16 BCD ×10 | untested |
| Player name | `0x143b` | 8B Shift-JIS | untested |

City enum (third-party): 0 Fillmore, 1 Bloodpool, 2 Kasandora, 3 Aitos,
4 Marahna, 5 Northwall — consistent with our region order.

### 3.3 Contradicted 🔴 — do not use

These decode to impossible values against our saves. Either the third-party tool
models a different version/region (its validator advertises europe/usa/japan and
a "Professional Mode" that reads as an unwritten byte for us — suggesting a
remaster or non-SNES target), or the offsets need re-derivation.

| Field | Offset | Claimed | Decodes in our saves as |
|---|---|---|---|
| Professional Mode | `0x1ff0` | 3-byte magic `"ACT"` | `0x60 60 60` / `0x27 27 27` — **the power-on fill**; §1 proves our ROM never writes this byte |
| Angel HP cur/max | `0x1439` / `0x143a` | max 1–24 | `65 / 0` — current 65 with max 0 is impossible |
| Angel SP cur/max | `0x1435` / `0x1437` | max ≤999 | `20 / 2056`, `80 / 2827` — max far exceeds the stated cap |
| Master MP | `0x1448` | max 10 | `80`, `700` |
| Master HP | `0x1246` | 1–24 | `8`, `11` — plausible, but the neighbouring fields' failure makes the whole block suspect |
| Current city | `0x13cd` | city index 0–5 | `422` |

Note these live in a contiguous `0x1435`–`0x1448` neighbourhood. A single
wrong base offset (or a different struct layout in our version) would explain
the whole cluster — which is exactly what §6.1 is designed to resolve.

### 3.4 Undocumented region 🔍 — `0x0000`–`0x07ff+`

The third-party map starts at `0x1200` and **documents nothing below it.** Our
saves have a large, live, progress-dependent block at the very start:

| Save | Live span | Size |
|---|---|---|
| `save.sim-blank.bak.srm` | `0x0000`–`0x05ff` | 1536 B |
| `save.sim-bloodpool-start.bak.srm` | `0x0000`–`0x076f` | 1904 B |

It grows as the sim progresses. Working hypothesis: **sim-mode town/terrain
state** (which map squares are developed/sealed). If so this is the
*highest-value* region for the feature's stated goal and the one thing no
existing tool documents. Needs derivation (§6).

---

## 4. Save codec and persistence backends

The runtime must keep one invariant regardless of the disk format:
**`g_sram` is always the canonical, exact 8 KiB image seen by the game.** A save
backend only translates between that buffer and durable storage. Game code must
never know whether the active file is native SRAM or structured INI.

Phase 6 supports two backends:

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
fillmore = active
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

---

## 5. Editing rules and hazards

### 5.1 ⚠️ Auto-persist will clobber the user's real save

`src/main.c:811-830` diffs `g_sram` every frame and calls `RtlWriteSram()` the
moment it changes, overwriting `saves/save.srm` — deliberately, so progress
survives a freeze. **A save editor mutating `g_sram` trips this instantly**, and
the user's real save is silently replaced by the edited one.

The editor **must** do at least one of:
1. suppress auto-persist while a save edit is staged (re-sync the shadow buffer
   after editing, so the edit is not seen as a game write); and/or
2. take a timestamped backup before the first edit (the repo already uses a
   `.bak` convention); and/or
3. stage edits into a scratch buffer and only commit on explicit user action.

Recommendation: **(1) + (2)** — re-sync the shadow *and* auto-backup. Never
silently overwrite a save the user did not ask to modify.

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
Edit a field → recompute checksum → boot → confirm the game accepts the save and
shows the intended state. **No field ships to the menu without this.** This also
retires the one open item in §2.

### 6.4 Tooling
The analysis scripts used for this doc live in the session scratchpad. Per the
repo's tooling convention (`tools/` with a shared `tools/ar_lib.py` — see
`dis65.py`, `romxref.py`, `wram.py`), they should be promoted to a
**`tools/srm.py`** with subcommands (`check`, `decode`, `diff`, `edit`) rather
than re-written as throwaway scripts each time.

---

## 7. Open questions

1. **Slot structure.** No mirroring at `0x400`/`0x800`/`0x1000` block sizes
   (7 of 8 `0x400` blocks unique). Does ActRaiser keep >1 save slot, and if so
   where? Affects whether the editor needs a slot selector.
2. **Progress value `0x01`** (§3.1) — observed on the active region, absent from
   the third-party enum. Exact semantics unconfirmed.
3. **The `0x0000`–`0x07ff` block** (§3.4) — town/terrain state? This is the
   highest-value unknown.
4. **Why the `0x1435`–`0x1448` cluster contradicts** (§3.3) — different game
   version, or a base-offset error?
5. **Act-2 access bit** (§3.2) — real for our ROM, or an artifact of the
   third-party target? All our saves read 0.
