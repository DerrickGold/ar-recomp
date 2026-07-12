# ActRaiser Rendering Engine Reference

Consolidated map of the game's drawing/streaming machinery, reverse-engineered
during the widescreen work (2026-07; deep-dive completed 2026-07-11 via full
static disasm of the bank-02 video core + whole-level DMA/VRAM traces).
Written decomp-style: every routine with its address, data structures with
layouts, unknowns marked `?`. Companion docs: [SEAMS.md](SEAMS.md) (seam
inventory / conversion status), [widescreen-survey.md](widescreen-survey.md)
(widescreen policy + evidence trail), [ram-map.md](ram-map.md) (variables).

Evidence basis: `saves/level1-action.rec` replay traces (faithful config,
channels dma/vram/vmadd/wram), disassembly of `$02:ABF0-$C72B`, and the
user's F2 snapshots in `runs/20260711-092516/`.

## 1. Frame pipeline overview

```
game logic (per frame, between vblank yields)
  ├─ object updates ($00:8915 loop) ...................... gameplay
  ├─ camera update  $02:B091: applies deltas $7C/$7E to $22/$24 with the
  │    LEVEL-BOUNDS CLAMP ($2E/$30), derives BG2 parallax $26/$28
  │    ($02:B9D5/$02:BA0B), sets 16px-crossing flags in $93
  ├─ strip dispatcher $02:B127: per $93 bit (TRB test-and-clear):
  │    $80 -> JSR $B158 X=0 (BG1 col)   $40 -> JSR $B1AF X=0 (BG1 row)
  │    $20 -> JSR $B158 X=4 (BG2 col)   $10 -> JSR $B1AF X=4 (BG2 row)
  │    -> each builds ONE upload record into its fixed buffer
  ├─ tile-animation tick $02:BC56 (arms DMA-descriptor slot 1)
  ├─ stage timer tick $02:BC82 (decimal SBC on $E6 every 60 frames)
  ├─ OAM rebuild: $00:8C98 cull -> $00:8D68 sprites -> $0380 shadow
  └─ HUD recompose into WRAM $7F:B000 (BG3 tilemap text/digits)
NMI $00:8520 -> $02:ABF0 (the complete graphics uploader, see §2)
```

## 2. The NMI graphics chain ($02:ABF0)

Common head: `STZ $420C` (HDMA off), then:

| Routine | What it uploads | Gate |
|---|---|---|
| `$02:ACA3` | OAM DMA: 544B `$0380` -> `$2104` (128 entries + 32B high table) | always |
| `$02:ADC3` | scroll regs: `$22..$2D` word pairs -> `$210D-$2112`, high byte `AND #$03` (10-bit) | always |

Then IN-GAME branch (`$18 != 0`):

| Routine | What it uploads | Gate |
|---|---|---|
| `$02:ACC8/$ACE5` | the 4 record buffers -> VRAM (see §3) | per-record header != 0 |
| `$02:ADE2` | fixed-color fade: `$2132` <- `$BD/$BE/$BC` | `$C4` |
| `$02:AEAE` | **BG2SC ($2108) = `$C7`+$70** — tilemap-page-flip animation | `$C5` |
| `$02:ADFF` | CGRAM row 7 ($F0): 32B DMA from ROM `$02:AE35 + (frame&2)<<4` — 2-frame flicker | always in-game |
| `$02:AE75/$AE7A` | CGRAM descriptor: CGADD=`$CE`, src=`$CD:$CB`, size=`$CF`<<5 (rows of 32B) | `$CF` |
| `$02:AEEB` | **HUD stream**: `$7F:B000` -> VRAM `$5800`, 256B (BG3 map rows 0-3) EVERY frame; then `$F1`-gated one-shot `$7F:B100` -> `$5880`, 1472B (rows 4-26) | always / `$F1` |
| `$02:AF30` | **VRAM DMA-descriptor slots**: slot0 `$D0`(src16)/`$D2`(bank)/`$D3`(VMADD)/`$D5`(size), slot1 `$D7/$D9/$DA/$DC`; word-mode bAdr $18; size self-clears | size != 0 |

SIM-mode branch (`$18 == 0`, `$19` gated): `JSL $02:8384`, copies
`$030C-$0313` -> `$0304-$030B`, then `$AEEB` (HUD), `$AF86`, `$AFCB`:

| Routine | What it uploads | Gate |
|---|---|---|
| `$02:AF86` | sim tile anim: src `$0A`:`$D7` size `$DC`, TWO DMAs bAdr=$19 (high-byte-only!): ch1 -> VMADD `$0000` (BG water chars), ch2 -> `$2A80` (OBJ sparkle) | `$DC` |
| `$02:AFCB` | VMADD `$47F0` upload | `$031A` |

Common tail: hblank-wait (`$4212` bit0), APU mailbox `$2142` <- `$035A`
every 2nd frame, deferred byte write `($EA)`<-`$EC` when `$EB`, HDMA
re-enable from `$92`, `INC $88` (frame counter), joypad -> `$A0`.

## 3. The upload-record system (tilemap writes — ALL of them)

**Every tilemap word in action stages flows through this path** (trace-
verified across the whole of Fillmore act 1: sole writer fn = `$02:ADA8`).

### Buffers — four fixed one-record buffers in WRAM (set up at `$02:BE0A`):

| Buffer | DP cursor | Shape | Drained as |
|---|---|---|---|
| `$7E:3900` | `$5E` | BG1 **column** record | pair 1, `[$76]`, VMAIN=$81 (inc 32) |
| `$7E:3A02` | `$60` | BG1 **row** record | pair 1, `[$79]`, VMAIN=$80 (inc 1) |
| `$7E:3B04` | `$62` | BG2 **column** record | pair 2, `[$76]` |
| `$7E:3C06` | `$64` | BG2 **row** record | pair 2, `[$79]` |

Each buffer = $102 bytes: `+0` header word = VRAM base (0 = empty), data =
4 x 64B chunks at `+2, +$42, +$82, +$C2`. **Capacity: ONE record per buffer
per NMI.** A second build into the same buffer before the drain overwrites
the first (lost strip) — never happens faithfully (camera <= ~8px/frame,
one 16px crossing max per frame; trace: max 4 chunks/buffer/frame).

### Drain `$02:ACC8 -> $ACE5` (JSL'd twice, once per pair):

- Column record (VMAIN=$81, one chunk = 32 words stepping 32 = a column):
  chunks -> `base, base+1, base+$800, base+$801`
  = 2 adjacent columns x 64 rows (upper screen pair + lower screen pair).
- Row record (VMAIN=$80, one chunk = 32 consecutive words = a row):
  chunks -> `base, base+$20, base+$400, base+$420`
  = 2 rows x 64 columns (left screen + right screen).
- After the 4 chunks: header word is ZEROED (consumed). The DATA is not
  cleared — a later header-only rewrite re-drains stale chunk data.
- `$02:ADA8` = the shared 64B DMA helper (ch1, bAdr $18, src bank $7E).

The chunk offsets hardcode a **64x64-tile tilemap** (SC screens A/B/C/D at
`+0/$400/$800/$C00`): BG1 map = VRAM `$6000-$6FFF`, BG2 = `$7000-$7FFF`.

### Mega-bursts (level entry / whole-map refreshes)

The entry draw fills the ENTIRE ring in ONE frame: 32 column-records per
layer built+drained inline in a loop (256 chunk-DMAs at hf=1000 in the
trace) — during load the game drains directly instead of waiting for NMI
pacing. The sim engine's whole-map UI recomposes work the same way
(observed 256-chunk bursts on the map screen). So: **gameplay = <=1
record/buffer/frame via NMI; loads = unbounded inline bursts.**

## 4. BG tilemap streaming (action stages)

The resident window is a 512x512px ring per layer (64x64 tiles) that wraps
BOTH axes. Levels are bigger (Fillmore act 1: BG1 4096x768, BG2 2304x512 —
from `$2E/$30/$32/$34`), so ring cells are constantly re-decoded:

1. **Level-entry full draw** (`$02:B7xx`, caller of `$B825` at `$B7F9`):
   full 64-column ring for both layers in one frame (see mega-bursts).
   Also initializes the per-layer camera clamp (`$2E/$30` math at
   `$02:B73C..B76D`).
2. **Column strips** — `$02:B158` (hle: `ActRaiser_StreamStripH`), per
   16px H crossing (flag `$93` bit $80 BG1 / $20 BG2): 2 columns x 64 rows
   at `(cam_x + $100) & ~$F` moving right / `cam_x & ~$F` moving left
   (16-bit h-delta at DP `$7C`). Rows outside the strip's 512px vertical
   decode window get FILLER (observed BG1 filler tile `$04E`; BG2 filler
   `$17F`-family / `$18A` seen earlier — id is per-section `?`).
3. **Row strips** — `$02:B1AF` (hle: `ActRaiser_StreamStripV`), per 16px
   V crossing (bit $40/$10): 2 rows x 64 columns at `(cam_y + $100) & ~$F`
   down / `cam_y & ~$F` up, horizontally spanning `[cam_x & ~$FF, +512)`.
   `$B8A0`'s map fetch is page-keyed: the span MUST be 256-aligned
   (a 16px-aligned widened start was tried and decoded from the wrong map
   page — REVERTED). Row strips carry full detail and are the only
   refresher of whatever the column strips left as filler.

Builders: marshal a DP block, `JSR $BED3` (8x8->16 multiply; col path),
`JSR $B825` (col build) / `$B8A0` (row build) -> `$02:B90D` metatile
expansion (writes `STA 0,X / $40,X / 2,X / $42,X` per 16x16 metatile,
interleaving the record's two rows/columns), `$02:B95A` header/geometry.

**Consequences of the geometry** (the widescreen crux — see §13):
- A ring column's content = whatever the LAST writer decoded for it. Row
  strips re-decode all 64 columns for map span `[cam_x&~$FF, +512)`; ring
  cells whose true world-x (in the current view) falls outside that span
  get REWRAPPED with far-side content. Invisible at 256 wide; visible as
  margin garbage/holes at 342 wide on the bad camera phases.
- Same on the vertical axis: column strips decode a 512px-tall window of a
  768px-tall level; rows outside hold filler/stale content until a row
  strip covers them. (Proven: the snap_00 "black staircase" = BG1 filler
  `$04E` cells — a stale vertical window exposed by the wide view.)
- BG2 row strips NEVER fire in Fillmore act 1 (trace: 0 drains of `$3C06`;
  BG2 V never crosses 16px). BG2's filler rows are permanent there —
  hidden behind BG1/HUD at 256 wide, they peek through canopy gaps in
  margins (the repeating "17-glyph" tile `$17F` in the user snapshots).

### Per-layer scroll state (DP, X = 0 for BG1, 4 for BG2)

| Addr (X=0/X=4) | Meaning |
|---|---|
| `$22/$26` | H scroll (BG1 = THE camera x) |
| `$24/$28` | V scroll |
| `$2A/$2C` | BG3 H/V ($2C pinned $FFFC = HUD shifted up 4px) |
| `$2E/$32` | layer pixel WIDTH (Fillmore act1: 4096 / 2304) |
| `$30/$34` | layer pixel HEIGHT (768 / 512) |
| `$3A-$45` | parallax ratio nibbles (from section config, see §5) |
| `$5E/$60/$62/$64` | record-buffer cursors (see §3) |
| `$76/$79` (+banks `$78/$7B`) | NMI drain pointers (reset every NMI by `$ACC8`) |
| `$7C/$7E` | camera H/V delta for this frame (16-bit signed) |
| `$93` | strip-request flags: $80 BG1col $40 BG1row $20 BG2col $10 BG2row |

## 5. Per-section video config — `$02:B4E8` + table `$02:893E`

Levels are driven by a SCRIPT (interpreter stream pointer `[$A2]`,
initialized to `$05:8000`; `$02:B6C8` = read-next-byte). Script opcode
handler `$02:B4E8` reads a section index, multiplies by 28 (`$BED3`), and
applies the record at `$02:893E + 28n`:

| Off | -> | Meaning |
|---|---|---|
| +0..+3 | `$212C/E`, `$212D/F`, `$2130`, `$2131` | screen designation + color math |
| +4 | bit flags -> `$6A/$6E/$72`=$2000, `$8F`=$1000 | per-layer flags + OAM attr bias arm |
| +5 | `$2107`=$60\|(v&3), `$2108`=$70\|((v>>2)&3) | **BG1SC/BG2SC size bits** (bases fixed $6000/$7000) |
| +6 | `$2105` | **BGMODE per section** |
| +7..+12 | nibble-split -> `$3A-$45` | parallax ratios (6 planes) |
| +13..+18 | `$BB/$BA/$B9/$BF/$C1/$C4` | fade/brightness config (+ mode bits in `$C4`) |
| +19..+22 | `$C5`, `$C9`=v<<4, `$C6`, `$CA`=v<<4 | **BG2SC page-flip anim** ($C5 arms `$AEAE`) |
| +23 | bit7 -> `$DA`=$1000/$0000; bits4-6 -> `$E1`=n<<7; bits0-3 -> `$DF`=n-1 | **tile-anim: char VRAM target, frame stride (bytes), frame count-1** |
| +24 | `$DE`=n-1 | anim tick period mask |
| +25-26 | `$E6` | **stage timer init** (decimal, ticked by `$BC82`) |
| +27 | `$F2` | ? |

Fillmore act 1 arrives with `$DE/$DF=$FF, $E1=0` — tile anim disabled.

## 6. Camera, clamp, parallax

- **`$02:B091`** (JSL'd from the main loop at `$00:8082/$80C1/$82F1`):
  - H: `$22 += $7C`, clamped to `[0, $2E - $100]`. **The $100 here is the
    hardcoded 256px viewport width.** Sets `$93 |= $80` when the result
    crosses a 16px boundary.
  - V: `$24 += $7E`, clamped to `[0, $30 - $E1]` ($E1 = 225). Flag $40.
  - Then BG2 parallax unless masked by `$8E` (bit0 H / bit1 V manual):
    `$02:B9D5` (H, flag $20) / `$02:BA0B` (V, flag $10) with X=4:
    scroll = f(camera, ratio `$3A+X`) via `$02:B9A3`; clamped against
    `$2E,X` ONLY if that width >= $300 — narrower layers wrap freely.
  - Tail: `JSL $00:A1B0` — copies camera-relative fields into the player
    object (`$06A0` block; `$24+$70` -> +$144/+$184).
- `$02:ADC3` uploads `$22..$2D` to the six BGnHOFS/VOFS regs each NMI
  (10-bit `AND #$03` high mask).

## 7. Tile animation

- Tick `$02:BC56` every `($88 & $DE)==0` frame: n = `($E0 & $DF)`;
  `$D7 = $B800 + n*$E1`; `$DC = $E1` (arms the upload); `$E0++`.
- Action mode: consumed by `$02:AF30` slot 1 -> DMA `[$D9]:$D7` (bank
  default $7F from `$02:BE0A` init) size `$DC` -> VRAM `$DA` ($0000 or
  $1000 = BG char space): **char re-upload animation** (waterfalls etc.).
  Frame buffers live at `$7F:B800 + n*$E1`; composer = level loader `?`
  (Fillmore has $E1=0; find it on an animated level via a wram trace on
  off 0x1B800-0x1BFFF during load).
- Sim mode: same tick, consumed by `$02:AF86` with HARDCODED src bank $0A
  (ROM-direct frames) and dual targets $0000/$2A80, high-byte-only writes.
- Independent second mechanism: `$02:AEAE` flips BG2SC between tilemap
  pages per `$C5/$C7` counters (`$02:BC34` phase helper) — tilemap-page
  animation, also disabled in Fillmore act 1.

## 8. Char/sheet loading + VRAM layout

| VRAM words | Contents | Writer |
|---|---|---|
| `$0000-$1FFF` | BG1+BG2 chars (BG12NBA=$00) | `$02:B28E`+`$02:B475` decompressor pair (force-blank port loops) |
| `$2000-$3FFF` | OBJ chars (OBSEL=$01, 8x8/16x16) | level-entry one-shot `$02:BC9E` (act1: 4224 words) |
| `$4000-$4FFF` | extra char bank (user `?` — B28E loads it; no NBA points there in-game) | `$02:B28E` |
| `$5000-$57FF` | BG3 chars 2bpp (BG34NBA=$05) | decompressor |
| `$5800-$5BFF` | BG3 map (BG3SC=$58, 32x32) — THE HUD | `$02:AEEB` per-frame stream from `$7F:B000` |
| `$6000-$6FFF` | BG1 map 64x64 ring | record drain only |
| `$7000-$7FFF` | BG2 map 64x64 ring | record drain only |

Base regs set once by `$02:C6B5` (in-game) / zeroed by `$02:C6EE` (video
off). **OBJ chars are fully static during an action level** — descriptor
slot 0 (`$D0-$D5`) exists but NEVER fires in Fillmore act 1 (whole-level
trace); its armer is unknown (`?` — likely boss/effect one-shots in other
levels). There is no per-object sheet allocator to exhaust.

## 9. OAM / sprite pipeline (action)

See SEAMS.md "Action OAM pipeline" + widescreen-survey.md Phase 3.
- `$00:8C98` (hle `ActRaiser_ObjectVisibilityScan`): shadow clear
  (x=$80/y=$E0 parked), high-table cursor reset (`$9A=$0580`), `$00:923A`
  HUD sprites (fixed positions from `$06:A800`), object walk (`$06A0`
  stride $40): skip $8000/$4C00, cull vs camera window (extents at
  +`$0A/$0C/$0E/$10`), offscreen bit $0400 in +`$30`, draw gate $2000.
- `$00:8D68`: sprite-def walk (7B defs, ptr obj+`$20`+5, bank obj+`$18`),
  y/x window tests biased by `$94/$96` (camera-16), writes `$0380` entries
  + packed high-table bits (bit0 = x bit 8 — true 16-bit screen x).
- `$8F` = attr OR-bias ($0E00 = palette-7 hit-flash while obj `$30&$2008`).
- Upload `$02:ACA6`: 544B. Hardware caps (32 sprites/line, 34 tiles/line)
  are gated by `NoSpriteLimits` (config; PPU `renderFlags`).
- Budget reality (user snapshots, 16:9): max 60/128 entries live, margin
  sprites present and correct — **no OAM pressure in act 1 even wide**.

## 10. Palette paths

- General descriptor: game sets `$CB/$CD` (src), `$CE` (CGADD), `$CF`
  (32B-row count); NMI `$02:AE75` uploads + clears. (e.g. `$00:A1CE`:
  rows $C0+ from `$0B:8280`.) Palette data bank: `$0B`.
- `$02:ADFF`: fixed 2-frame flicker of CGRAM row 7 from ROM `$02:AE35/55`
  (sprite palette 7 = player hit-flash row).
- `$02:ADE2`: fixed-color fade (`$2132`) from `$BD/$BE/$BC` while `$C4`.

## 11. UI / dialog compose (sim engine)

- `$02:BF60`: dialog/message-box draw dispatcher (type in `$14`).
- Whole-map UI refreshes = the §3 mega-burst mechanism (record buffers
  re-filled + inline-drained repeatedly in one frame). The "[$76] ->
  `$3B04`" values seen game-side are just the NMI drain cursors at rest.
- Sky palace policy: plain full-wide, staging artifacts accepted (FINAL,
  per user decision — see widescreen-survey.md).

## 12. Conversion status

| Routine | Status |
|---|---|
| `$00:8418` / `$02:A85E` vblank wait | hle (host yield) |
| `$00:8C98` cull + `$00:8D68` builder | original recompiled path on `widescreen-investigation`; hle port only on the two earlier experimental branches |
| `$02:B158` col-strip builder | original recompiled path on `widescreen-investigation`; Stage B separately reuses `$B825` transactionally for margin-only VRAM writes |
| `$02:B1AF` row-strip builder | original recompiled path on `widescreen-investigation`; experimental hle port is not used |
| `$02:BED3/$B825/$B8A0/$B90D/$B95A`, drain chain, OAM DMA, camera `$B091` | recompiled |

## 13. Widescreen design constraints (read before the next implementation)

Facts the next design must satisfy (all trace/disasm-proven above):

1. **View widths**: 4:3 PAR 16:9 = 342px (43/side); ring = 512px. A 342px
   view + streaming lead fits the ring, but not on every camera phase with
   fixed 256-aligned row-strip spans:
   - row-strip span `[B, B+512)`, `B = cam & ~$FF`. Left margin cells
     (`cam-43..cam`) fall OUTSIDE when `cam mod 256 < 43`; right margin
     (`..cam+299`) when `cam mod 256 > 213`. Out-of-span ring cells get
     far-side content (re-wrap) -> margin holes like snap_01/05 trunks.
   - the SAME phase math applies vertically via column strips (level
     taller than ring): stale vertical windows -> filler cells like the
     snap_00 black staircase (proven filler tile `$04E`).
2. **Candidate fix (host-side record patching)**: keep faithful builds;
   before the NMI drain, for each ring column (row records) whose
   band-span map-x differs from the map-x it currently displays in the
   wide view, overwrite that record word with the CURRENT VRAM word
   (drain no-ops there). Word math: row record, ring col c -> chunk
   `(c<32 ? 0/1 : 2/3)` word `c&31` (chunk pairs = rowA/rowB). Column
   records: chunk = screen half (rows 0-31 / 32-63), word = row&31.
   Costs nothing when margins are inactive. The same hook can instead
   REBUILD the out-of-span words from map data in C once the metatile
   decode ($B90D + section map pointers) is ported — that is the full
   fix (margins always true content, zero re-wrap).
3. **Camera bounds are authoritative and cheap**: world exists only in
   `x ∈ [0, $2E]`, camera in `[0, $2E-$100]`. The margin policy must use
   `$22` vs these (plus `$32` etc. for BG2 with its own dims + the
   width>=$300 clamp rule) instead of the current heuristic min/max
   tracker — fixes the level-start black margins correctly (there IS no
   world left of x=0 until the camera has moved 43px+).
4. **Streaming bandwidth is 1 record/buffer/frame** during gameplay. Any
   wide-mode extra coverage must either ride the existing records (patch/
   rebuild, #2) or add host-side VRAM writes outside the SNES DMA path —
   do NOT queue extra records (they'd overwrite unfired ones).
5. **BG2 row strips are dead in act 1** — BG2 filler rows are permanent.
   Wide BG2 needs either its filler tiles accepted (they only peek
   through BG1 gaps), a host row-refresh for BG2 on H movement, or a
   margin clamp for BG2 only on the affected rows.
6. **H-strip +64 lead interacts with row-strip spans**: lead columns at
   `cam+320` exceed `B+512` when `B = cam&~$FF` is low — their detail
   rows re-wrap on the next row strip. If record patching (#2) lands,
   reduce/remove the +64 lead: margins then stay true without it.
7. **Sprite status is OPEN again (2026-07-12)**: earlier captures proved only
   that OAM capacity was not exhausted (<=60/128) and that some apparent
   corruption was BG gaps or the hit-flash cheat interaction. Later direct
   testing found extra boundary sprites and partial multi-tile enemies. Both
   experimental "BG" branches still replaced `$8C98/$8D68`, so they never
   isolated BG widening from the OAM port. The investigation must keep the ROM
   OAM path intact through raw-wide and BG-refresh stages before widening any
   sprite window.
8. **HUD (BG3)**: 32-tile-wide compose in `$7F:B000`, streamed per frame;
   margins under the HUD stay clamped (current policy) or need a host
   compose extension later.
9. **Every drawing path is now known**: any wide-mode VRAM the game won't
   supply can be host-written safely during the NMI window as long as it
   stays out of the four record buffers' way.

### 13.1 Stage-B implementation refinement (2026-07-12)

The earlier `widescreen-bg` implementation proved the map-decoding idea but
did not isolate it: that branch also hle-replaced `$8C98/$8D68`, hle-wrapped
both streamers, and restored only selected DP scratch after calling `$B825`.

`widescreen-investigation` keeps all four original routines and places the
margin decoder in `src/actraiser_widescreen_bg.c`. Static audit of
`$B825->$B90D` shows only upload-record WRAM writes, DP `$0E`, and `$BED3`
multiply-register use; there are no PPU/OAM/CGRAM writes. The host wrapper
therefore snapshots/restores the full `CpuState`, all 128 KiB WRAM, and SNES
math state. It validates the fixed record cursor and every destination against
the owning 4 KiB tilemap before directly copying to VRAM. Consequently its
only persistent state is BG1/BG2 tilemap content. `AR_WS_BGREFRESH=0` removes
the transaction entirely for a byte-identical Stage-A A/B run.

## 14. Open questions (all remaining, none blocks the §13 design)

1. `$7F:B800` action-anim frame composer (find on an animated level:
   wram trace off 0x1B800-0x1BFFF during load; Fillmore act 1 idles).
2. Descriptor slot-0 (`$D0-$D5`) armer — never fires in act 1.
3. VRAM `$4000-$4FFF` char bank consumer (loaded but unreferenced by the
   in-game NBA regs; another section's OBSEL/NBA values `?`).
4. BG2 filler tile id per section (`$17F` seen in act 1, `$18A` earlier
   in another context) — confirm from `$B825`'s filler constant per layer.
5. Boss-arena window/HDMA effects vs margins (survey doc has the known
   full-width-window fix; iris wipes stay 256-centered — audit per boss).
6. Sim-mode (Phase 4): bank-01 camera writers `$01:B1F8/$B7E9/$CE69/
   $CEEB`; sim streaming; `$02:AFCB` `$47F0` upload; `$02:8384`.
7. Section config +27 -> `$F2` meaning; `$6A/$6E/$72` $2000-flag meaning.
