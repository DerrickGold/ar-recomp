# ActRaiser — Logic ↔ Hardware Seams (HAL inventory)

This is the **living inventory of boundaries between game logic and SNES hardware** — the seams
we will eventually widen into a platform interface (HAL) to allow enhanced/custom graphics and
audio that exceed SNES limits. See `DEBUG.md` and the §"future" discussion for the rationale.

**Discipline (read once):** this is captured *opportunistically* while debugging — record a seam
only when you already understood it chasing a bug. **Do NOT go on documentation expeditions, and
do NOT design HAL signatures yet.** Correctness of the recomp comes first; the boundary isn't
stable enough to abstract until the game runs.

**The two columns that matter** are **Intent** (what the logic is *trying* to do) and **Logical
ID / table** (the index/pointer that carries asset identity — the future HAL's vocabulary and the
asset-substitution point). The *hardware* column is mechanically recoverable later; intent and
identity are the perishable, expensive-to-rederive parts.

**Status legend:**
- 🟢 **HOOKABLE** — already a clean semantic seam (an ID/event). Good HAL candidate; easy to
  intercept and reroute to an enhanced backend.
- 🟡 **CHOKE** — funnels through one runtime function; interceptable but the payload is still
  hardware-encoded (needs some decode to reach intent).
- 🔴 **LOW-LEVEL** — intent is entangled in raw hardware writes; surfacing it needs asset-pipeline
  decomp.

---

## Audio  (closest to a clean interface — start here)

| Seam | Routine / address | Hardware | Intent | Logical ID / table | Status |
|---|---|---|---|---|---|
| Music / event | `LDA #id; COP` → `$035A`; COP vector `$FFE4→$8526`; hook `ActRaiser_CopHook` | APU ports `$2140-43` | "play music / fire event N" | event/song id (A → `$035A`) — **song table TBD** | 🟢 |
| Sound effect | `LDA #id; BRK` → `$035B`; BRK vector `$FFE6→$852F`; hook `ActRaiser_BrkHook` | APU ports | "play SFX N" | sfx id (A → `$035B`) | 🟢 |
| SPC driver/sample upload | `RtlUploadSpcImageFromDp` (HLE; src ptr at DP+`$A5`, ActRaiser's `LDA [$A5],Y`) | APU ports + ARAM | "upload sound driver + BRR samples to APU" | DP+`$A5` source block; resident IPL uploader in ARAM `$0F0E` | 🟡 |
| Raw APU port write | `RtlApuWrite` (`$2140-$2143`) | APU I/O | low-level handshake / param | — | 🔴 |

> Audio is the highest-payoff first HAL target: the `$035A`/`$035B` events are already ID-based.
> **Next capture:** the song-id → ROM table mapping (which id = which track), so an enhanced
> backend can map id → modern audio. Found while fixing the boss-music handshake (memory:
> `spc-upload-dp-pointer-fix`, `cop-syscall-hook-fix`, `post-boss-four-issues`).

---

## Graphics / PPU  (low-level; intent lives in the *loaders*, not the *draws*)

| Seam | Routine / address | Hardware | Intent | Logical ID / table | Status |
|---|---|---|---|---|---|
| Per-frame display build / DMA | NMI path; `ActRaiserDrawPpuFrame`; DMA descriptors in ZP `$D0-$D5` | VRAM/OAM/CGRAM DMA, `$2100`-bus | "blit this frame's tiles/sprites/palette" | DMA descriptor tables | 🔴 |
| Sprite (OAM) build | object loop `$8915` → OAM | OAM | "place object's sprite" | `$06A0` object struct (X/Y/handler) | 🟡 |
| BG mode / layers | `$2105` BGMODE, `$212C/2D` main/sub, scroll regs | PPU | "set up background layers/scroll" | — | 🔴 |
| HDMA (raster fx, HBlank) | HDMAEN `$420C`; ActRaiser drives ch 2/3 (and others) | HDMA | "per-scanline effect" | HDMA tables | 🔴 |
| Mode 7 (overworld / transitions) | m7matrix; `$2134` multiply; the act-select spin | PPU mode 7 | "rotate/zoom the world map" | m7 transform params | 🔴 |
| Brightness / forced blank | `$2100` INIDISP | PPU | "fade in/out, blank during build" | — | 🔴 |
| Palette load | CGRAM writes | CGRAM (15-bit ×256) | "set palette N" | **palette id / table TBD** | 🔴→🟡 |
| Sprite-sheet / tile load (action-stage) | ROM→VRAM copy loaders (**TBD — not yet located**) | VRAM DMA | "load anim sheet / tileset" | **sheet/frame index — the key asset-identity seam** | 🔴 |
| Sim-mode object sprite/behavior identity | ROM tables `$01:E09B` (behavior/anim data ptrs) + `$01:E7E1` (sprite-graphics ptrs), one 16-bit entry per object slot — see "Sim-mode object/sprite spawn & OAM-build system" below | not VRAM directly — feeds object record `+00`/`+08`, which downstream OAM code (`ADAD`) reads | "this object slot's sprite/behavior asset" | **located 2026-07-01** — the per-object-type asset-identity seam for sim-mode decorations/actors | 🟡 (tables located + confirmed live; the spawn routine that COPIES table→object-record is unconverted — see below) |
| Sim-mode per-frame building/icon update | `$01:8000` (bank 1) — see "Sim-mode dispatch structure" below | VRAM/OAM (downstream, not yet traced) | "update this frame's city/building/icon visuals" | region (`$19`) gates which sub-block runs; several `JSR (abs,X)` tables (`$2920`, `$208E`, `$B420`) select per-building/icon variants | 🔴 (dispatch structure mapped; the actual VRAM writes inside the deep `$018170+` body not yet traced) |

> **The asset-substitution seam is the loaders, not the draws.** When you find the routines that
> copy graphics ROM→VRAM and select animation frames, capture the table index they use — that
> index is the logical sprite/tileset identity. For **sim-mode objects** this is now located (see
> row above + the detailed section below) — the action-stage equivalent is still TBD; F2 snapshots
> dump VRAM/CGRAM/OAM when hunting these — see `DEBUG.md` §9.

---

## Sim-mode dispatch structure (mapped 2026-07-01, chasing a graphics-corruption bug)

The main loop (`ResetHandler_M1X1`, bank 0) branches at `$00805F: LDA $18; BNE $8066; BRL $80E5` —
`$18 != 0` (action stage) goes to `$8066` (the `$8915` object loop + per-frame action routines,
already documented above); `$18 == 0` (intro/overworld/sim — see the "Game-state anchors" table
below for why sim mode reads as `$18==0`, not `08` as an earlier assumption had it) goes to `$80E5`,
the **sim-mode per-frame dispatcher**:

```
$0080E5  PHB; LDA #$1; PHA; PLB        ; DB = $1 for the rest of this dispatcher
$0080EA  LDA $19; CMP #$9; BEQ $8129   ; region 9 = a separate sub-flow (not yet traced)
$0080F0  JSL $018000                   ; sim-mode building/icon per-frame update (bank 1)
$0080F4  BCS $8125                     ; skip the rest of this frame's update if carry SET
$0080F6  JSL $2AFF8 / $1B21B / $1ACD9 / $3D06A   ; always-run subsystems
$008106  LDA $19; CMP #$7; BCS $8125   ; SECOND skip gate: region >= 7 skips the rest too
$00810C  JSL $2BEFC; JSR $845F; JSL $38193; JSL $19193; JSR $88D6; JSL $2C206; JSR $8465
$008125  PLB; BRL $8059                ; back to the main loop (re-yields at the vblank wait)
```

**Both `BCS $8125` gates were investigated and ruled out as the cause of the 2026-07-01
graphics-corruption bug** — `AR_SIMTRACE` confirmed the sim-mode update runs to completion (neither
skip fires) in the affected playthrough, and `$0019`'s real value never diverged from the oracle.
The actual corruption is still open as of this writing; whoever picks it up next should look
**downstream of `$008122`'s `JSR $8465`** (the last call before the frame's normal exit) or inside
`$01:8000`'s own body, not at these two gates.

**`$01:8000`** (bank 1) is the sim-mode building/icon updater itself. Its own entry does a similar
region-gated early-exit (`LDA $19; CMP #7`/`#8` at the top decides whether to even enter the deep
body at `$018170`), then a further gate at `$018010-$018024` (checks DP `$347`, DP `$A1`, and long
address `$7F9750` — meaning ALL THREE must be in specific states to reach the deep body) before
finally running `JSL $1B1C7` and the actual per-building update logic. This function ALSO drives
the `$2920`/`$208E`/`$B420` `JSR (abs,X)` tables flagged as `SUPPRESSED` in the regen report (see
`DEBUG.md` §7.9) — `$B420` is a genuine static ROM table (5 real entries, confirmed by reading the
bytes), but `$2920`/`$208E` resolve to SNES hardware-register space (`$2000-$5FFF`) under LoROM,
meaning either they're populated at runtime via DMA (not yet confirmed) or the `JSR (abs,X)`
instructions decoding there are themselves decode artifacts from a wrong entry width — not yet
resolved, `AR_INDIRLOG=1` is armed to help if a future investigation reaches these sites.

---

## Sim-mode object/sprite spawn & OAM-build system (mapped 2026-07-01, tracing a graphics-corruption bug)

This is the deepest-mapped seam in the codebase so far — a full pipeline from **ROM asset tables** →
**object records** → **per-frame OAM output**, mapped end-to-end while chasing a real bug (a graphics
corruption traced to one missing spawn). Read this before touching sim-mode sprites/decorations, or
before designing a HAL replacement for them — it's the clearest existing map of "how does a sprite
get from ROM to screen" anywhere in this codebase, action-stage included.

### The pipeline, ROM asset → screen

```
ROM def-tables (per-object-type asset identity)
  $01:E09B   behavior/anim-data pointer table (16-bit ptrs, one per object slot)
  $01:E7E1   sprite-graphics pointer table    (16-bit ptrs, one per object slot)
        │
        ▼  (spawn — see "Missing spawn" below; reads these tables, populates a live object record)
Live object record (WRAM $7E, 38-byte stride, base ~$06A0.."$0Fxx")
  +00/+02  data/behavior pointer (copy of the $01:E09B entry — points at small structured
           records in ROM, e.g. "$01:DDC2 = 03 08 00 00 04 08 00 00 00 01 08 00..." — NOT code)
  +06      object "type" word
  +08      sprite/graphics pointer (copy of the $01:E7E1 entry, e.g. $01:E15E)
  +0A/+0C  world position (X/Y), ALSO separately populated by the bank_03_813F staging copy
           (see below) — this is why a broken object can still have correct position but a
           null sprite/behavior
  +0E      per-object tile/animation-frame count (read by ADAD to bound its OAM sub-loop —
           see "OAM flood bug" below; this is what turns a null +08 into a 216-iteration loop)
  +12      status word — bit $8000 SET = inactive/skip (tested by ACD9's scan, see below);
           real HW shows e.g. $8001 for an active object (bit0 has some other meaning, TBD)
  +14..+24 scratch / unused for the objects inspected so far
        │
        ▼  (per-frame, unconditionally — see bank_01_ACD9 below)
Destination OAM table (WRAM $7E:$0380-$047F, ~46 fixed slots, DB=1)
        │
        ▼  (DMA, not yet traced — see Graphics/PPU table above)
Real OAM → screen
```

### `bank_03_813F` — the position-staging copy (misleading at first glance, actually correct)

Called from `bank_01_AA56` during sim-mode setup. Copies a `$130`-byte block from a WRAM bank-`$7F`
staging area into the live object table at `$0B30,Y`:

```
$038141  REP #$20                      ; A = 16-bit, NO subsequent SEP in this routine
$03814A  LDA $7F7BFB ; ... ; LDA $038111,X ; TAX ; LDY #0     ; X = ROM table lookup -> block base
$038157  LDA $7F0000,X                 ; 16-bit READ from staging
$03815B  STA $0B30,Y                   ; 16-bit WRITE, advance BOTH X and Y by 1 (not 2!)
$03815E  INX ; INY ; CPY #$0130 ; BNE $038157
```

**This is a deliberate SNES overlapping-byte-copy idiom**, not a bug: doing 16-bit stores while
advancing the index by 1 means each store's high byte gets immediately overwritten by the next
store's low byte, netting a clean byte-for-byte copy despite 16-bit access width. `cpu_write16`
reproduces it exactly — confirmed correct against the ROM disassembly (2026-07-01). **Don't mistake
this pattern for a misdecode/off-by-one if you see odd-offset 16-bit writes coming from here** — it's
correct, and matches real hardware bit for bit.

`X_start` for a given object comes from `ROM[$03:8111 + word_at($7F:7BFB)]` — a second indirection
table selecting *which* `$130`-byte staging block to copy from. This only fills the **position**
fields; it does NOT populate `+00/+06/+08/+12` (behavior/type/sprite/status) — those come from the
missing spawn below.

### `bank_01_ACD9` / `bank_01_ADAD` — the per-frame OAM rebuild (runs every frame, by design)

`ACD9` runs unconditionally every sim-mode frame (called from `bank_01_9284`). It is **NOT** a
one-time init despite superficially looking like one — real hardware ALSO rebuilds the entire
decoration-OAM table from scratch every single frame (confirmed via oracle: `$0380-$047F` gets
writes every frame on real HW too, just with *stable* output since nothing moves). This is standard
SNES practice — don't try to "fix" it into a one-shot.

Per frame, `ACD9`:
1. **Resets the destination cursor** `D:$0098 = 0` and hide-fills the OAM output range with
   `Y=$E0` (off-screen sentinel) via a `PHA ×16` idiom.
2. **Scans object candidates** in (at least) two separate segments — `L_ACEF` (X starts `$6A0`,
   stride `$12`=18, fixed count `$30`=48 iterations) and `L_AD71` (X starts `$0A00`) — each testing
   `[DB:$0010+X]` bit `$8000`; **bit SET = skip (inactive)**, bit CLEAR = active → calls `AC70` then
   `ADAD` for that object.
3. For each active object, **`ADAD`** re-derives the destination OAM write position from `D:$0098`
   (loaded once at its own entry, into `X`), computes screen position via `$0014`/`$0016` scratch
   (`source_word - D:$0094` camera-relative subtraction), and runs an inner tile sub-loop:
   `INX ×4` (advances the dest cursor by 4 = one OAM record), `CPX #$0200` exit check, `DEC D:$000E`
   (the per-object tile counter) as the other exit condition, looping back through `L_ADCC`. On
   normal exit (`L_AE6B`) it **saves the advanced `X` back to `D:$0098`** so the next object's call
   continues from where this one left off — this hand-off is correct and was extensively
   lldb-verified (2026-07-01) to chain properly across objects (`$00→$10→$14→$3C→$54...`).

**The OAM-flood bug mechanism** (found 2026-07-01, root cause is upstream — see next section): if an
object's `+0E` tile-count field is corrupt/garbage (in the traced case, `$00D8`=216 instead of a
sane `$0004`), `ADAD`'s inner loop iterates 216 times instead of ~4, flooding ~36 OAM slots with a
repeating garbage pattern (`X=77,Y=44,tile=$55` — coincidentally the Town Hall's own position/tile)
before hitting its `CPX #$0200` bound. This LOOKS like a cursor-collision or codegen bug (multiple
"different" objects appear to write the same slots) but isn't — it's one object's own loop running
far too long, stomping everything downstream of it in that frame's build. **If you see a destination
address get many rapid, differently-valued writes within one object's processing, check that
object's own `+0E`/`+08` fields before suspecting the loop/cursor logic.**

### Root cause found: a whole spawn cohort is missing, not a single field

*(2026-07-02 update: the "unconverted spawn / cutscene actors" hypothesis below was refined by the
full town-architecture mapping in the next section — the 4 records are the town's lair/decoration
ACTORS, their spawner `$01:D072` IS converted, and the actual break was the town handler dispatcher
`$03:F5BE` being misdecoded so its handler subsystem never ran. The forensics are kept because the
asset-identity conclusion stands.)*

Comparing our recomp's F2-snapshot object records against the oracle's (same save, same repro),
found a **clean cohort of 4 consecutive objects** (`$0B30, $0B56, $0B7C, $0BA2`, stride `$26`) that
are almost entirely **uninitialized** in our recomp — only their position fields (written by the
`bank_03_813F` staging copy above) are present; `+00/+02/+06/+08/+12` are all `$0000` where real HW
has real values. Every *other* active object in the same frame has correct (non-null) values in
these fields — this isn't a general corruption, it's **exactly these 4 objects never getting
spawned**.

**This is the cleanest illustration in the codebase of the asset-identity seam** flagged in the
Graphics/PPU table above ("the asset-substitution seam is the loaders, not the draws"): the ROM
tables at `$01:E099`/`$01:E7D9` (bases corrected 2026-07-02) ARE the per-object-type
behavior/sprite identity — a future graphics-replacement HAL would intercept here (read "which
asset slot", substitute new art) rather than in the OAM-write plumbing (`ADAD`), which is pure
mechanism with no asset knowledge of its own.

---

## Sim-mode town architecture — the full map (2026-07-02, root-caused + FIXED the corruption+freeze)

The complete town simulation decomposes into FOUR cooperating subsystems. This is the most
completely-mapped gameplay system in the project and the natural starting template for a full
decomp of sim mode. (Forensic trail: `DEBUG.md` #18-25; confirmed fixed in-game 2026-07-02.)

**Bug-hunt note for future readers:** the F5BE fix below (#2) was necessary but NOT sufficient
— it fixed the lair-mask/event dispatcher, but the actual actor corruption/freeze needed a
SECOND fix, in #3's per-type dispatch chain (`$01:B898`/`B8C0` → `D04E` family → the spawn
battery). Both are now fixed and confirmed working; the sections below describe the final,
correct architecture, not the intermediate broken states.

### 1. The per-frame master loop — `$03:8193`

Called every sim frame. Skeleton:

```
PHP; PHB; REP #$20; LDA #$007F; JSL $008519      ; DB=$7F for town state
JSR $8238                                          ; (per-frame sub)
LDA $00:0347; CMP #7; BEQ exit                     ; sub-phase gate
LDA $7F:9750; BNE -> JSR $C147                     ; demo/attract hook (always 0 -> skipped)
... flag checks ($7F:91xx) ...
JSR $F5BE                                          ; TOWN HANDLER DISPATCHER (see #2)
INC $7F:91FE (16-bit); CMP #$02D0                  ; 720-frame (12s) periodic counter
  >= 720: reset + JSR $8271                        ; the 12-second periodic event
else:      JSR $8E0C                               ; the every-frame sibling
JSR $B898 ($01), $B1B7 ($01), ...                  ; object-update loops
```

`$7F:91FE/$9200` are frame counters (`$9200` resets on some per-frame condition — an idle timer).
The `$0347` word is never written in normal play (stays at boot value) — the CMP #7 gate is for a
special mode.

### 2. The per-town handler dispatcher — `$03:F5BE` (the bug site)

```
PHP; PHB; REP #$20; LDA #$007F; JSL $008519  ; DB=$7F
LDX $7BFB                                     ; town index * 2 (set by the act<->sim transition)
LDA $03:F5ED,X; TAX                           ; X = this town's handler-list pointer
loop: LDA $03:0000,X; CMP #$FFFF; BEQ exit    ; read handler-1 word; $FFFF terminates
      PHX; LDY #$F5E2; PHY; PHA               ; push cursor, push return-1, push handler-1
      SEP #$20; RTS                           ; RTS-trick CALL: jump to handler+1 at m=1,x=0
$F5E3: REP #$20; PLX; INX; INX; BRA loop      ; each handler RTSes back here
exit: PLB; PLP; RTS                           ; flag-transparent to the caller
```

- Outer table `$03:F5ED`: **6 towns** (Fillmore, Bloodpool, Kasandora, Aitos, Marahna, Northwall)
  → inner list pointers `$F5F9/$F601/$F609/$F60D/$F615/$F61B`.
- Inner lists (packed at `$03:F5F9-$F620`, each `$FFFF`-terminated): per-town handler sets.
  14 unique handlers; code starts at `$F621, $F671, $F68A, $F6BF, $F6FF, $F791, $F7AE, $F7D1,
  $F7F8, $F822, $F857, $F870, $F8A5, $F8CC`.
- The handlers are the town's **lair/event logic**: they test and maintain the per-town lair
  bitmask state at `$7E:9107+` (4 bytes/town, "open lairs") and `$7E:911F+` ("spawned lairs")
  via the helpers `$03:F46E` (test) / `$F479` (set) / `$F484` (clear), which use `$03:F497`
  (bit compute, scratch `$7E:914F`) and the WRAM-pointer tables `$03:DCA2`/`$DCAE`. They also
  drive the spawn-list engine (see #3) and post events.
- **Recomp seam note:** the `PHY #ret; PHA handler; SEP; RTS` idiom is invisible to static
  decoding. Fixed 2026-07-02 with `indirect_dispatch F5DF 20 idx:A tables:F5F9 ret:F5E3 sep:20`
  (bank03.cfg) plus a new value-keyed `idx:A` + `sep:` form of the directive (cfg_loader/decoder/
  codegen). Before the fix the whole handler subsystem silently never ran — town lairs/monsters
  never spawned (the graphics-corruption/freeze root cause) and the SEP leaked m=1 to `$8193`.

### 3. The spawn-list engine — `$033C/$033D` + `$01:AC36` (processes) and `$01:D072` (actors)

The town is populated by numbered SPAWN LISTS run through one engine, with parallel machinery for
the two object tiers (see #4):

- `$033C` = list id, `$033D` = cycle sub-variant (0-3 = a rotating variant selector the oracle
  advances every ~180 frames — day-cycle/blink phases; 4 = the special B1C7 tail pass).
- `bank_01_CFF2(A)` = store A into `$033C/D`, call `AC36`. `bank_01_AC36(X=entry)` =
  `entry.+02/+06 = ROM[$01:A227[list*2] + sub*2]` (script ptr), `+04=0`, `+00=1` — assign +
  activate a stride-`$12` process (tier 1).
- `$01:D072(A=type, Y=record)` = the stride-`$26` twin: `record.+00 = ROM[$01:E099 + type*2]`
  (behavior script), `+02 = ptr-4`, `+04 = 1`. Sprite half at `$01:D0F5-D127`:
  `record.+08 = ROM[$01:E7D9 + type*2]` (frame ptr) — invoked via the **56-routine per-type
  spawn battery `$01:BA23-$C793`** (one setup routine per object type, each ending
  `JSR $D072`).
- Town-entry sweep: `bank_01_AA56` (after the `$7F:97DA -> $0B30` staging restore via
  `bank_03_813F`) walks the stride-12 table calling `CFF2(entry.+0E)` per entry — the `+0E`
  field IS the entry's current list id, restored from the save and advanced by gameplay.
  `bank_01_8029 -> B1C7` wraps the sweep (`B52F` = switch-all-to-variant pass, `B6AE` =
  hide-all pass, `CFB3` ($03) = hide-sweep on town exit).
- Event path: `$01:8819` dispatches the event code at `$033E` through a jump table at
  `$01:F223`; one-shot list runners at `$01:8E11/8E22` (`LDA $000E,X; STA $033C; JSR $AC36`).

**The missing link: how a record's per-frame TICK reaches the spawn battery.** `D072` only
INITIALIZES a record once; what runs it every frame (and is what actually populates a freshly-
placed record's position/script/sprite fields) is a chain starting from the master loop's
`JSR $B898` (see #1's skeleton):

```
$01:B898  per-record TICK entry, called once per active record by the $8193 loop.
  $01:B8C0  per-TYPE class dispatch: PHX (save record ptr); LDA $B8D0,X (X = type*2,
            byte offset, NOT the record index); PLX (restore X = record ptr, so X is
            NOT usable as the dispatch key at the PHA/RTS site below); PHA the table
            word; RTS -> class handler (26-entry table at $B8D0, e.g. type $12/$13 ->
            $B9EC/$BE4F). QUIRK worth remembering for any future manual decomp/asm
            work: because of the PHX/PLX bracket, the dispatch index must be read from
            the PUSHED VALUE (A at push time), not from X at the RTS -- X has already
            been overwritten with the record pointer by the time the RTS fires.
  class handler (e.g. $B9EC)
    JSR $D063           ; latch: "have I run my one-time init?" (record.+12 bit15)
    if not yet init:  LDY #<battery-table>; JSR $D04E   ; $01:D04E-D062 family --
                        selector = record.+12 & $7FFF, table ptr passed in Y by THIS
                        caller -> lands in the 56-routine spawn battery ($BA23-$C793)
    battery routine (e.g. $BA18)
      sets type-specific fields, JSR $D072              ; position/script/sprite init
```

So the full per-record lifecycle is: town-entry sweep (`AA56`) or event (`8819`) assigns a
type via `CFF2`/`AC36`-equivalent bookkeeping -> every frame, `B898` ticks the record -> `B8C0`
routes by type to a class handler -> the class handler's one-time latch (`D063`) triggers the
`D04E`-dispatched battery routine -> the battery calls `D072` to actually populate the record.
Skip ANY link in this chain (as the recomp did, for months, at `B8C0`/`D04E`/their targets) and
a record gets its type field only, never its position/script/sprite -> permanently-garbage
sprite. See `DEBUG.md` #18-25 for the full bug-hunt trail; `$01:B898`/`B8C0` also appear in
"Function roles discovered" below with the specific PHX/PLX index-model bug that hid this for
an extra round.

### 4. The two object tiers (shared design, separate engines)

| | Tier 1: stride-`$12` "processes" | Tier 2: stride-`$26` "actors/records" |
|---|---|---|
| Table base | `$7E:06A0+` | `$7E:0AE4+` (records 2-5 = `$0B30/56/7C/A2`) |
| Assigner | `$01:AC36` (list tables `$01:A227`) | `$01:D072` (type tables `$01:E099`/`$E7D9`) |
| Executor | `$01:AC70` (per frame, from `ACD9`) | `$01:D08F` stepper + `$01:B0xx-B1xx` update loop |
| Script format | `[delay, frameptr16]*`, `$FD`=hide, `$FE`=loop(count `+04`), `$FF`=set-loop | behavior scripts around `$01:DDxx-DFxx` |
| Key fields | `+00` timer, `+02` cursor, `+04` loop count, `+06` script base, `+08` current frame ptr, `+0A/+0C` position, `+0E` list id, `+10` status (bit0 visible, bit15 hidden) | `+00/+02` behavior ptrs, `+04` flag, `+08` frame ptr, `+0A/+0C` position, `+0E` list id, `+10` status word, `+12` dispatch selector |
| Behavior dispatch | (scripts only) | per-TYPE via `$01:B898`/`B8C0` -> class handler -> `$01:D04E-D062` (table ptr in **Y**, selector = `+12 & $7FFF`) -> spawn battery -> `D072` |
| Rendered by | `ACD9` scan -> `ADAD` OAM build | same (second scan segment, X from `$0A00`) |

What blinks/animates in a healthy town: the oracle shows the whole family toggle every ~131
frames (stride-12 `+10` words, records 0/2-5/20 status words) plus per-record frame animation
(`+08` stepping through `$01:E838`'s frame list, e.g. `$E6CA -> $E6D0 -> $E6D6`).

**Decomp guidance:** the four subsystems above are the natural C module boundaries
(`town_mainloop.c`, `town_handlers.c` per-town data-driven, `spawn_lists.c`, `objects.c` with the
two tiers as structs). The ROM data tables (`$03:F5ED+` handler lists, `$01:A227` script lists,
`$01:E099/$E7D9` type identities, `$01:D128+` placement records, `$01:E838` frame lists) are the
**level/asset script seam** — a future editor or HD-asset pipeline replaces THESE, not code.

---

## Frame / timing  (mostly already HLE'd — the model is understood)

| Seam | Routine / address | Hardware | Intent | Notes | Status |
|---|---|---|---|---|---|
| VBlank wait | `$00:8418`, `$02:A85E` (HLE → `ActRaiser_WaitForVblank`); `$01:9284` (inline) | RDNMI `$4210` | "wait one frame" | RDNMI modeled as once-per-frame token; inline waits spin-detected (`snes.c`) | 🟢 |
| NMI handler | `$8520` (`NmiHandler`) | NMI | "per-frame vblank service" | game frame `$0088` bumped here | 🟢 |
| Frame coroutine | `RunOneFrameOfGame` (`actraiser_rtl.c`) | — | host frame ↔ game frame mapping | coroutine yields at vblank-wait | 🟢 |

---

## Input

| Seam | Routine / address | Hardware | Intent | Status |
|---|---|---|---|---|
| Joypad read | auto-joypad enable `$4200` bit0; `$4218-$421F` | controller | "read player input" | 🟡 (TBD: where logic consumes it) |

---

## Save / persistence  (mapped 2026-07-01)

| Seam | Routine / address | Storage | Intent | Logical ID / table | Status |
|---|---|---|---|---|---|
| Save-file validity check | `$02:A88D` (checksum) called from `$02:A622` (title-screen gate) | SRAM `$700000-$701FEB` (checksum), stored expected values at `$701FEC`/`$701FEE` | "is this save data trustworthy enough to offer Continue?" | pass/fail via carry; no version/format ID beyond the checksum itself | 🟢 (clean pass/fail gate, algorithm fully understood) |
| Save-file body | SRAM `$700000+`, 8192-byte `saves/save.srm` | LoROM battery SRAM, banks `$70-$7D` | city/kingdom state, presumably per-region | **format TBD** — checksum covers the whole 8172-byte body as one blob, no sub-structure identified yet | 🔴 |

> **Load path:** `RtlReadSram` (`common_rtl.c`) does a straight `fread(g_sram, 1, g_sram_size, f)` —
> byte-for-byte, no remapping. `cpu_sram_offset` (`cpu_state.c`) maps `(bank, addr)` for bank `$70`
> to a literal 1:1 offset (`(bank & 0xF) << 15 | addr`, and bank `$70`'s `&0xF` is `0`, so addr IS
> the offset) — confirmed correct against the checksum algorithm 2026-07-01. If a future save-format
> HAL is built, this is the one seam that's ALREADY a clean pass/fail gate; the body itself still
> needs its internal structure (city stats? per-building state? population?) mapped out, which
> would make excellent future work for whoever's chasing sim-mode rendering next (the checksummed
> blob almost certainly contains the data `$01:8000`'s building-update logic reads).

---

## Game-state anchors (not hardware seams — symbol map for everything else)

These RAM addresses recur across all the debugging; keep `ram-map.md` authoritative.

| Addr | Meaning |
|---|---|
| `$18` | mode / region: `00`=**intro/overworld AND sim mode both** (see correction below), `01`–`07`=action stage region N (Fillmore=`01`), `$20+`=transitions |
| `$19` | sub-mode (act within region — act 1 vs 2 in action stages; region/sub-flow selector in sim mode, e.g. region `9` branches to a separate sim sub-flow at `$008129`) |
| `$1A`/`$1B` | transition **destination** (`$1B`→`$18`, `$1A`→`$19`, applied by the mode-switch `$00:8269`) |
| `$FB` | bit `0x80` = transition-request flag (set with `$1A`/`$1B` to stage a mode change; game consumes+clears it) |

> **Correction (2026-07-01):** the `08`=sim mapping above was never actually observed — real captures
> during confirmed sim-mode play consistently show `$18==0`. `ResetHandler`'s own dispatch (`$00805F:
> LDA $18; BNE $8066 [action]; BRL $80E5 [sim-mode dispatcher]`) branches to the sim-mode handler
> specifically WHEN `$18==0`, which is the actual ground truth this table should have reflected —
> `$18==0` is a shared bucket for BOTH intro/overworld AND sim mode; something else (not yet
> identified — check `$19` or another byte first) must distinguish which of those two the game is
> actually in. Don't trust old assumptions about this byte without re-verifying against a live
> capture; see the sim-mode dispatch structure section above for the actual branch logic.

**Level warp** (`ActRaiser_Warp`, F6 hotkey, `AR_WARP=<reghex><acthex>` e.g. `0202`): stages the
game's own sim→act transition — sets `$1B`=region, `$1A`=act, `$FB|=0x80` — so the game does the
full fade + level-load + switch itself. Trigger from the intro (`$18==00`, which works), bypassing
the broken post-act sim cascade. Observed: Fillmore act 1 entry = `$1B=01,$1A=01,$FB=80` (f=994) →
`$18` flips `00→01` (f=997) → act live (f=1004).
| `$1D` | player HP |
| `$E6`/`$E7` | action-stage timer (BCD) |
| `$0088`/`$0089` | game-frame counter (16-bit) |
| `$06A0` +stride `$40` | object table (≥64 slots; fields in `DEBUG.md` §11) |
| `$08A0` | player object (slot 8) |
| `$7D1B` | saved stack ptr (act→sim transition stack relocation) |
| `$0014`-`$0017` | **SHARED DP SCRATCH — not a fixed-meaning anchor.** Used as a 16-bit ADD/XOR
  checksum accumulator by `$02:84F3` (save-data validity check) AND as a message-type-ID parameter
  by `$02:BF60` (dialog-box draw dispatcher) — two unrelated subsystems reusing the same 4 bytes.
  Do not treat a read/write/oracle-divergence on this range as evidence about EITHER subsystem
  without checking which one is actually executing at that PC/frame first. |

> **Direct-page addressing gotcha:** all `$XX`-style addresses in this document (including this
> table) are conventionally DP (direct-page) offsets, which the CPU resolves as `D + $XX`, NOT a
> literal WRAM address — `D` (the direct-page register) happens to be `0` in every context checked
> so far (confirmed via runtime inspection, not assumed), making `D + $XX == $XX` in practice, but
> this has NOT been verified for every code path in the game. Before relying on a watch/trace
> pointed at a literal address, check the live `cpu->D` value for the code path in question.

---

## Gameplay / Tunable seams  (cheats, rebalance, mods)

A second class of seam: the **value-clamp and mechanic-intercept points** where game logic reads/
writes a tunable parameter or makes a gameplay decision. Hooking these enables infinite health,
moonjump, sword reach, score forcing, etc. — the gameplay analog of the AV HAL above.

**Two hook kinds:**
- 🅥 **VALUE** — a RAM/SRAM byte/word; hook = freeze/clamp/force it (e.g. infinite health = pin
  the HP byte). Easiest; just needs the address.
- 🅒 **CODE** — a routine/constant that *computes* a mechanic (sword reach, jump velocity, damage);
  hook = intercept the routine or patch the constant. Needs the code site located.

**Discipline:** the addresses below marked **TBD** are NOT yet found — do NOT invent them. Each row
carries the **discovery method** so we capture it the moment debugging takes us through it. Only
promote a row to a real address once confirmed (a wrong cheat address is worse than a TODO).

| Seam | Where (RAM / routine) | Mod use | Kind | Status / how to find |
|---|---|---|---|---|
| Player HP (current) | `$1D` | infinite health (pin), god-mode | 🅥 | **WIRED** — `AR_INF_HP=1` (high-water auto-pin) or `=<n>`; per-frame in `ActRaiser_ApplyCheats` (actraiser_rtl.c). |
| Player max HP / bar size | TBD | bigger/smaller health bar | 🅥/🅒 | find the HP-init constant (new-game / stage-entry sets `$1D` to max) — `AR_WATCHOBJ`/`AR_WATCH16` on `$1D` at stage start; the writer's immediate is max. |
| Invincibility frames (i-frames) | i-frame timer `$08C6` (+$26); **invuln flag = `$08D0` bit `0x2000`** (the gate) | **no-knockback / invuln** (speedrun "ignore hits") | 🅥 | **WIRED** — `AR_NO_KNOCKBACK=1` pins timer `$08C6`=0xFF AND sets flag `$08D0\|=0x2000` each frame -> invuln from frame ONE. (Hit-check gates on the FLAG; the game sets it on a hit and clears it when the timer hits 0 — so pin timer + set flag = permanent, no first-hit needed. `=26` alone only worked after one hit.) On hit: handler -> `$9C64` (hurt), knockback into `$08A6/$08A8`. |
| Player lives | TBD (RAM or SRAM) | infinite / set N lives | 🅥 | watch the lives display value, `AR_WATCH16` on it; the death routine decrements it. |
| Player sword damage (dealt) | routine that subtracts enemy HP | one-hit kills, weak sword | 🅒 | `AR_WATCHOBJ` on an enemy slot's HP field while you hit it → the writer is the damage routine; the amount is its operand. |
| Player sword length / reach | TBD (hitbox/collision calc) | double reach | 🅒 | the sword-vs-enemy hit test — the attack hitbox extent (a constant offset from player X). Hardest (geometry); find via the attack-frame collision routine. |
| Player fly / moonjump | Y-**position** = `$08A4` (+$04) | moonjump / fly | 🅥 | **WIRED** — `AR_MOONJUMP=1` (default 6 px/frame) or `=<n>`; `AR_MOONJUMP_BTN=<mask>` (def `0x8000`=B). Moves Y-pos up while B held (`ActRaiser_ApplyCheats`). NOTE: uses Y-pos, NOT Y-vel `$08A8` — `$08A8` is "Y-velocity" only in the AIR state (polymorphic field); writing it while grounded did nothing. |
| Boss HP / health bar | boss object slot HP field (offset TBD) | set boss HP, instant-kill | 🅥/🅒 | boss HP lives in the boss object's slot (we have `saves/act1-boss*.bin` snapshots). `AR_WATCHOBJ` on the boss slot while damaging it → the HP field + the boss-damage writer. |
| Enemy HP (general) | object slot HP field (offset TBD) | — | 🅥 | same as boss — a per-object HP field in the `$06A0` table (offset not yet mapped). |
| Act score / population | TBD (likely SRAM, per-act) | force score thresholds → sim gating | 🅥/🅒 | the routine that compares act score/population to a threshold to gate sim-mode progression — `AR_WATCH16` on the displayed score; find the threshold-compare site. |
| Action-stage timer | `$E6`/`$E7` (BCD) | freeze timer / infinite time | 🅥 | **WIRED** — `AR_FREEZE_TIMER=1` pins `$E6/$E7` (per-frame in `ActRaiser_ApplyCheats`). |

> **Anchor:** most player mechanics hang off the **player object `$08A0`** (slot 8 of the `$06A0`
> table). Mapped so far (via `AR_WATCHOBJ=08A0`, 2026-06-25):
>
> | Offset | Addr | Field |
> |---|---|---|
> | +$02 | `$08A2` | X position |
> | +$04 | `$08A4` | Y position |
> | +$06 | `$08A6` | **X velocity** (signed; knockback fallback target) |
> | +$08 | `$08A8` | **Y velocity** (signed, neg=up; gravity +1/frame — moonjump target) |
> | +$12 | `$08B2` | handler ptr (state machine: `$9832` ground, `$98D9`/`$993F` jump, `$9884` walk, `$9A07`, `$9C64` **hurt**) |
> | +$24 | `$08C4` | frame/anim counter |
> | +$26 | `$08C6` | **i-frame timer** (set 0x20 on hit, counts down) |
> | +$30 | `$08D0` | flags — bit `0x2000` = invuln (set during i-frames) |
>
> Still TBD and highest-value: the **HP field offset** (player/boss/enemy share the `$06A0`-table
> layout) — find once → unlocks boss-HP, enemy-HP, and damage seams together. Keep promoting these
> into `ram-map.md`.

---

## Function roles discovered (decomp groundwork)

Capture the *role* of a routine when you understand it — names are perishable. These are NOT yet
renamed in the cfg (see below); this is the candidate list.

| Address | Role |
|---|---|
| `$00:8000` `ResetHandler` | reset / boot (named) |
| `$00:8520` `NmiHandler` | per-frame NMI service (named) |
| `$00:8525` `IrqHandler` | IRQ (named) |
| `$00:8915` | object loop — dispatch each active object's `$12` handler |
| `$00:8526`/`852F` | COP / BRK syscall entry (audio events) |
| `$00:9557` | spawn dispatcher (reads `$18`, indexes per-act handler table at `$95DD`) |
| `$03:9156` | **act→sim transition handler dispatcher** (relocates stack to `$1FFF`, RTS-trick chain through `$9B22`/`$9B4A`/`$9195`) |
| `$03:8053` | **enter-sim SETUP** (runs on ANY entry to `$18=00`, incl. act→sim AND a warp to `$18=00`). Sequence of `JSR`s (`$9156` [fixed], `$AC8E`, …) → `$8193` → `$C147` → `$B20C`/`$B21F`. **ROOT-CAUSED + FIXED (2026-06-26):** the act→sim cascade was NOT an m/x flag-drift (the §0 "perfect storm" framing was a red herring) — it was a **1-byte SNES-stack-pointer leak** from one unconverted jump-table RTS-trick, `$01:B898` `$B8C0: LDA $01B8D0,X; PHA; RTS` (call-with-return to `$B8C2`). The −1/call ratcheted `S` up over `$B20C`'s `JSR $B21F` loop → over-pop → hang; the "x-leak" was a downstream `PLP`-reads-shifted-slot symptom. Found via per-instruction `S` trace (`AR_STRACE`), not the oracle. Fix: `indirect_dispatch B8C0 16 idx:X tables:B8D0 ret:B8C2` (bank01.cfg) — see DEBUG.md §7.7 jump-table CALL variant + §0 RESOLVED. |
| `$01:B898` | **per-record per-type dispatcher, called once per active actor record every frame** (from the `$8193` master loop — see the sim-mode town architecture section above for the full chain into the spawn battery). `$B8C0` PHA-dispatches through the handler table at `$01:B8D0` keyed by object type, returns to `$B8C2`. History of THREE separate bugs found at this one site, in order: (1) a 1-byte SNES-stack leak that hung the act→sim transition (fixed 2026-06-26, see `$03:8053` above); (2) the table's `count` was left capped at 16 as a workaround for a since-fixed label-emission bug, but town actor types are 18/19 — above the cap, so their class handlers never dispatched (fixed 2026-07-02: bumped to the real bound, 26); (3) even at count=26, `idx:X` was wrong AT THIS SITE specifically — the ROM wraps the table read in `PHX($B8AE)/PLX($B8BB)`, so by the `PHA/RTS` dispatch X has been restored to the RECORD POINTER, not the type index (fixed 2026-07-02: switched to the value-keyed `idx:A` form, which reads the PHA'd table word instead of a register). This is the site responsible for the sim-mode actor-spawn corruption/freeze (`DEBUG.md` #18-25) — NOT the earlier stack-leak hang, a different bug at the same address. |
| `$03:AC8E` | transition state-machine step (counter loop, calls `$97B0`) |
| `$00:80E5` (label inside `ResetHandler`) | **sim-mode per-frame dispatch entry** — reached when `$18==0`; see "Sim-mode dispatch structure" above. |
| `$01:8000` | **sim-mode building/icon per-frame updater** — region-gated (`$19`), drives the `$2920`/`$208E`/`$B420` `JSR (abs,X)` tables; deep body at `$018170` does the actual per-building work via `JSL $1B1C7`. |
| `$00:8465` | writes a hardware-register-style immediate (`LDA #$A1`) — same pattern as the NMITIMEN setup at `$008051`; called from `ResetHandler`'s sim-dispatch tail (`$008122`). Its own native width (`M1X0`) runs correctly — confirmed NOT a misdecode (2026-07-01), just legitimate register churn that was mistaken for corruption early in that investigation. |
| `$00:8241` | called twice per main-loop iteration (`$008056` and `$00805C`, sandwiching the `$8418` vblank wait) — role not yet traced. |
| `$02:A622` | **title-screen continue/new-game state machine.** Calls the save checksum (`$02:A88D`) at `$02A70A`, branches on the result (`$02A70D: BCC $A72F`) to one of two dialog flows — see "Save / persistence" below. |
| `$02:A88D` | **save-data validity checksum.** Computes a 16-bit ADD-sum and a 16-bit XOR-sum over SRAM `$700000-$701FEB` (calls `$02:84F3` to do the accumulation), compares against stored expected values at `$701FEC`/`$701FEE`. Returns pass/fail via carry (`CLC`=pass, `SEC`=fail). Confirmed correct 2026-07-01 (`AR_SAVECHECK`) — passes cleanly against a real mid-game save. |
| `$02:84F3` | **checksum accumulator loop** — `LDX #0; loop: LDA $700000,X; ADC $14; STA $14; EOR $16; STA $16; INX INX; CPX #$1FEC; BNE loop`. `$14`/`$16` are its scratch accumulator — see the DP-scratch-reuse gotcha in `DEBUG.md` §0. Confirmed byte-identical across its `M0X0`/`M1X0` width variants (not a misdecode candidate despite an early `mxhist` flag). |
| `$02:BF60` | **dialog/message-box draw dispatcher.** Takes a message-type ID via `A` (stored into the SAME `$14` DP scratch the checksum uses), branches on ID (`CMP #0/1/6/8/9/$B`) to different message-rendering sub-routines. Called by both `$02:A622` branches with different message sets. |
| `$01:ACD9` | **per-frame decoration-OAM rebuild driver** (bank 1, sim mode). Runs unconditionally every frame (called from `$01:9284`); resets the `D:$0098` dest cursor + hide-fills `$0380-$047F`, then scans object candidates in ≥2 segments testing `[DB:$0010+X]` bit `$8000` (active/inactive), calling `AC70`+`ADAD` per active object. See "Sim-mode object/sprite spawn & OAM-build system" above for the full mechanism. |
| `$01:ADAD` | **per-object OAM tile writer**, called from `ACD9`. Loads the dest cursor from `D:$0098`, writes one OAM record per tile (`INX ×4` per tile, `DEC D:$000E` = per-object tile counter as the loop bound), saves the advanced cursor back to `D:$0098` on exit (`L_AE6B`). Cursor hand-off verified correct via live lldb (2026-07-01) — do not re-suspect this without new evidence. |
| `$01:AC70` | called by `ACD9` immediately before `ADAD` per active object; body is just `PHB;PHY;REP;JSL bank_00_8519` (a `PHA;PLB` DB-set idiom) + fallthrough — does NOT touch `$0098`/`$0094`/`$0096`/`$0380` (checked). Its role beyond the DB-set is not yet traced. |
| `$00:8519` | trivial `PHP;SEP #$20;PHA;PLB;PLP;RTL` — the classic "`PLB` from A's low byte" idiom for setting the Data Bank register. Not a meaningful gate/hook; appears at several call sites across banks 0/1/3 wherever DB needs setting to a literal. |
| `$03:813F` | **position-staging copy** for sim-mode object records — see "Sim-mode object/sprite spawn" above. Copies `$130` bytes from a `$7F`-bank staging buffer (selected via `$7F:7BFB` → ROM table `$03:8111`) into the live object table at `$0B30,Y`. The odd-offset 16-bit writes this produces are a CORRECT overlapping-byte-copy idiom, verified against ROM disasm — not a bug. Called from `$01:AA56`. |
| `$01:E099` (ROM data, not code) | **actor behavior-script pointer table** (base corrected from the earlier `$E09B` guess), indexed by object TYPE (`type*2`): 16-bit pointers to behavior scripts around `$01:DDxx-DFxx`. Read by `$01:D072` (`LDA $01E099,X; STA $0000,Y`). Object-type identity data. |
| `$01:E7D9` (ROM data, not code) | **actor sprite-frame pointer table** (base corrected from `$E7E1`), parallel to `$E099`: per-type sprite/animation-frame pointers (frames list continues at `$01:E838`, e.g. `$E6CA/$E6D0/$E6D6...`). Read by the `$01:D0F5-D127` sprite-assign code. **The asset-identity seam for sim-mode actor sprites.** |
| `$03:8193` | **sim-mode per-frame master loop** — see "Sim-mode town architecture" above. Sets DB=`$7F`, runs `$8238`, the `$F5BE` handler dispatch, the 720-frame periodic counter (`$7F:91FE` vs `#$02D0` → `$8271`), then the bank-01 object loops. |
| `$03:F5BE` | **per-town handler dispatcher** (PHY/PHA/SEP/RTS trick; 6-town outer table `$03:F5ED`, packed inner lists `$F5F9-$F620`, 14 handlers `$F621+`). Was silently dead in the recomp until the `idx:A`/`sep:` `indirect_dispatch` fix (2026-07-02) — the town-corruption/freeze root cause. |
| `$03:F46E`/`$F479`/`$F484`/`$F497` | **lair-bitmask helpers**: test / set / clear a per-town lair bit; `$F497` computes the bit + cell (scratch `$7E:914F`) from the WRAM-pointer tables `$03:DCA2` (open-lair masks `$7E:9107+`) / `$DCAE` (spawned masks `$7E:911F+`). |
| `$01:AC36` | **process-script assigner**: `entry.+02/+06 = ROM[$01:A227[$033C*2] + $033D*2]`, `+04=0`, `+00=1`. The stride-12 tier's spawn primitive. |
| `$01:CFF2` | thin wrapper: `A -> $033C/$033D`, `JSR $AC36`. The sweep's per-entry call. |
| `$01:AA56` | **town-entry sweep driver**: staging restore (via `$03:813F`) + walk the stride-12 table calling `CFF2(entry.+0E)` per entry. |
| `$01:8029` / `$01:B1C7` / `$01:B52F` / `$01:B6AE` | town-init wrapper (`8029` → `B1C7` = `CFF2`+`AC70`); `B52F` = switch-all-processes-to-variant pass; `B6AE` = hide-all pass. `$03:CFB3` = hide-sweep on town EXIT. |
| `$01:D072` | **actor spawner** (stride-26 tier): `record.+00 = ROM[$01:E099+type*2]`, `+02 = ptr-4`, `+04 = 1`. Reached via the 56-routine per-type battery `$01:BA23-$C793` (each battery routine sets type-specific fields then `JSR $D072`). |
| `$01:D04E-D062` | **actor behavior RTS-trick dispatcher** (mechanics corrected 2026-07-02): table ptr passed in **Y** by each caller (`LDY #table; JSR/BRL $D04E`), NOT inline-after-JSR; selector = `record.+12 & $7FFF`; `A = sel*2 + Y`, `PHA` the table word (target-1), `RTS` at `$D062`. 24 call sites / 24 tables in bank01 (`B246 B423 B90B B934 B965 B9F8 BE58 C1C9 C243 C505 C7C5 C886 C8B0 C8D3 C8F7 C93C C977 C99D CA6D CA98 CAC3 CC3E CCE0 CD12`), incl. the spawn battery. All targets registered as funcs in bank01.cfg (the sim-mode corrupt-actors root-cause fix). |
| `$01:D063` | **mark-record helper**: sets `record.+12` bit15 (`ORA #$8000`) and returns Z per prior state — the standard first call of every battery handler (`JSR $D063; BNE already-init`), i.e. the "run once per spawn" latch. |
| `$03:8700-8711` | **per-town state dispatcher #2**: `LDX $7BFB; LDA $7CC9,X` (per-town state byte) → static table `$03:8713` (5 entries `871D 873C 87B5 872A 8E30`), PHA/RTS at `$8710/11`. Same PHA/RTS family as F5BE; targets registered in bank03.cfg. |
| `$03:E1D2-E1EB` | **per-town event-handler dispatcher #3** (F5BE-shaped): `LDX $7BFB` → per-town table base from `$03:E66E` (`E67A/E93C/EBC2/EE3E/F049/F2D7`, exactly 32 entries each), selector*2 added, pushes shared continuation `$E1EC` (`REP #$20; PLX; PLA; RTS`), PHA/RTS at `$E1EA/EB`. Targets + `E1EC` registered in bank03.cfg. |
| `$01:D08F` | **actor script stepper** (records' analog of `AC70`): counts down `+04`, reads the script at `+02`, `$FF` terminator handling. |
| `$01:AC70` | **process-script executor** (role found 2026-07-02, supersedes the "role not yet traced" note): per frame per active entry, DEC `+00` timer; on expiry read script at `+02`: `[delay, frameptr]` default op (→ `+08`, `+10|=1`), `$FD` hide, `$FE` loop (DEC `+04`), `$FF` set loop count+target. |
| `$01:8819` | **town event dispatcher**: `LDA $033E; ASL; ADC #$F223; TAX` — jump table at `$01:F223` keyed by the event code in `$033E`. |
| `$01:A227` (ROM data) | **spawn-list table**: `A227[list_id*2]` → per-list block of 5 sub-variant script pointers (sub = `$033D`, 0-3 = day-cycle phases, 4 = init special). |
| `$01:D128+` (ROM data) | **placement records** (stride 6: type, ?, x, y) — the "spawn WHAT at WHERE" data consumed by spawn scripts (e.g. script `$A3E5`'s operands `$D19A/$D1AF/...`). Level-layout seam. |

---

## Symbol renaming — mechanism & convention

**How to rename a function:** the cfg `func` directive's **first argument is the emitted symbol
name** (that's how `ResetHandler`/`NmiHandler`/`IrqHandler` got real names instead of
`bank_00_8000`). So in `recomp/<bank>.cfg`:
```
func ActSimTransition_Dispatch 9156 entry_mx:0,0
```
names the generated function `ActSimTransition_Dispatch` instead of `bank_03_9156_*`. Changing a
cfg requires **regen + rebuild**.

**RAM symbols:** the emitted code uses raw offsets (`cpu_read8(0x7E, 0x18)`), so RAM "renaming" is
documentation only for now — keep `ram-map.md` authoritative. (Symbolizing RAM accesses in the
emitted C would need an emitter change — a *future* nicety, not now.)

**Convention (proposed):**
- Functions: `Subsystem_Verb` PascalCase — `ActSimTransition_Dispatch`, `ObjectLoop`,
  `SpawnDispatcher`, `Audio_PlayMusic` (HLE wrappers already follow this: `ActRaiser_WaitForVblank`).
- Only rename what you've *confirmed* the role of. A wrong name is worse than `bank_03_9156`.

**When to do a rename pass:** rename *after* a fix is confirmed working, not bundled into the same
regen — so a rename can't be confused with a behavior change if something breaks. (E.g., hold the
`$9156`/chain renames until the `$9195` transition fix is verified.)

---

*Living doc — append seams as you cross them. Keep it honest: intent + logical ID are the point;
the hardware column is the easy part.*
