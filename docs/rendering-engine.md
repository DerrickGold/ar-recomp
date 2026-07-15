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
| `$2000-$2FFF` | common action OBJ atlas (OBSEL=$01, 8x8/16x16) | `$02:BC9E`: 4096 words from ROM `$07:8000-$07:9FFF` |
| `$2D40-$2DBF` | reserved OBJ magic/effect overlays inside the common atlas | `$02:BC9E` writes 128 words to `$2D40`; `$00:96C3-$96F5` can arm 128-byte slot-0 upload to `$2D80` |
| `$3000-$3FFF` | OBJ address space reachable through OBSEL name selection; resident contents/consumers not yet catalogued | `?` |
| `$4000-$4FFF` | extra char bank (user `?` — B28E loads it; no NBA points there in-game) | `$02:B28E` |
| `$5000-$57FF` | BG3 chars 2bpp (BG34NBA=$05) | decompressor |
| `$5800-$5BFF` | BG3 map (BG3SC=$58, 32x32) — THE HUD | `$02:AEEB` per-frame stream from `$7F:B000` |
| `$6000-$6FFF` | BG1 map 64x64 ring | record drain only |
| `$7000-$7FFF` | BG2 map 64x64 ring | record drain only |

Base regs are set by `$02:C6B5` (in-game) / zeroed by `$02:C6EE` (video
off). `$02:BC9E` also uploads the action sprite palettes from `$07:D040-$D09F`
to CGRAM `$C0-$EF`, then selects a magic overlay rooted at `$06:A400` for
VRAM `$2D40`. Descriptor slot 0's later armer is `$00:96C3-$96F5`: when
object `$30 & $0040` and `$D5==0`, object `$38` selects a source rooted at
`$06:A000`, target `$2D80`, size `$0080`. It then advances paired object
states. Thus the **regular action atlas is static/common**, with small dynamic
magic/effect replacements; there is no per-enemy sheet allocator to exhaust.

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
- Upload `$02:ACA6`: 544B. The PPU can gate the hardware caps (32 sprites/line,
  34 tiles/line) through `renderFlags`, but ActRaiser's current
  `PpuBeginDrawing(..., 0)` leaves authentic caps active; parsed
  `NoSpriteLimits` is not yet forwarded.
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

- `$02:BF60`: dialog/message-box draw dispatcher (type in `$14`); its tile
  writes target the BG3/HUD compose buffer at `$7F:B000`, later streamed by
  `$02:AEEB`. It is not a proven direct writer of Sky Palace's BG2 staging.
- Whole-map UI refreshes = the §3 mega-burst mechanism (record buffers
  re-filled + inline-drained repeatedly in one frame). The "[$76] ->
  `$3B04`" values seen game-side are just the NMI drain cursors at rest.
- Sky Palace has two observed completed BG2 states: dialogue states place a
  staged box in the offscreen columns, while the message-speed submenu shows
  clean pillar continuation. No separate clean-before-box upload has been
  observed inside one dialogue composition; whether the submenu rebuilds the
  ring or selects another BG2SC page remains to trace.
- A render-scoped `$B825` reconstruction was directly disproved by
  `runs/20260712-232230`: `[ws-sky]` reported all 9/9 requested strips built at
  width `$0200`, scroll `$0000`, and BG2SC `$73` (base `$7000`, 64x64), yet the
  snapshot still showed the staged boxes. The address and destination geometry
  were correct; the live decoder source/config already described the
  UI-composed state, so decoding faithfully reproduced it. That transaction
  has been removed.
- A follow-up renderer-only trial kept the first clean 16px of raw BG2 outside
  each authentic edge and reflected that narrow band outward. It successfully
  removed staging, but the band did not contain a whole architectural column;
  its transparent/cap fragments repeated as broken posts at the extreme sides
  (user capture `Screenshot 2026-07-12 at 11.31.20 PM.png`).
- Isolated authentic-center reflection supplied complete columns, but the
  11:36 PM user capture corrected the layer ownership: BG3 carries the text,
  while BG2 still carries the visible box frame. Center reflection therefore
  copied the left/right portions of that box into both margins. That policy was
  removed.
- Static source recovery found the original map feed at `$02:B6F8-$B726`:
  under its Sky Palace/submenu conditions, the game loops over 256 bytes from
  ROM `$07:D0A0` and stores them at `$7E:C200`. The ROM block is a 16x16
  metatile page containing the exact palace beam, capitals, shaft pattern, and
  floor. Its rows 9-12 are occupied by a dialog box, but rows 3-8 establish the
  unchanged shaft continuation underneath it.
- Current Sky Palace policy (implemented 2026-07-12, **validated 2026-07-13**)
  reads `$07:D0A0` itself and expands metatile IDs through the live BG2
  definition table at `$7E:2900` using the same mask/attribute operation as
  `$02:B90D`. Layout facts established during validation (cell-by-cell diffs
  vs the live map, `runs/20260713-*`):
  - Definition words are **row-major within the metatile** — quadrant
    `((y&1)<<1)|(x&1)` = TL,TR,BL,BR. The x-major order transposes every 2x2
    block (split shaft metatiles render as 8px checkerboards).
  - The 64x64 map is four quadrant canvases (2 x-pages x 2 y-bands) selected
    per UI state via scroll (menu `vscroll~504` = top band, dialogs
    `vscroll~248` = bottom band; hscroll 0). All share one scene layout.
  - The page's box rows 9-12 cover scene rows that must be reconstructed:
    rows 9-10 continue the shaft (row 8); the floor plane's top two rows sit
    under the box bottom (row 12 -> floor row 13 at plain columns; page rows
    13-15 only cover the lower floor); meta cols 0/15 keep rows 11-12 (the
    page-seam base halves `$42/$40` + `$4A/$48`).
  - Pillar base flares exist **only in the metatile table**, never in a page
    row: `$41/$49` center (`$41` top half = plain shaft, seamless splice)
    flanked by the `$40/$48` / `$42/$4A` skirts on the row-8 shaft neighbors.
  Only 8px tile columns sampled exclusively by the side margins are patched in
  VRAM; the authentic center and its BG2 box are untouched. The entire BG2 ring
  is restored immediately after scanout. The final margin decode is
  **byte-identical to the game's own boot-composed colonnade** (scratch cols
  56-63, rows 18-31). `AR_WS_SKYPALACE_BG=0` selects raw-wide output.

### 11.1 Town camera and OAM pipeline

Town simulation uses a separate bank-1 sprite system from action mode:

```
$01:B4C6 camera follow/clamp
  camera $22 = clamp($0AEE-$80, 0, $0100)
  camera $24 = clamp($0AF0-$70, 0, $011F)
  optional shake $7F:9F65/$9F67, accepted only inside those bounds

$01:ACD9 per-frame OAM driver
  fixed segment: 48 records, $06A0, stride $12, fixed-screen origin
  world segment: 44 records, $0A00, stride $26, camera-relative origin
    -> $01:ADAD normal composition emitter
    -> $01:AE6F alternate-attribute emitter when $7F:9752 & 2
  -> OAM shadow $0380-$059F -> common NMI DMA
```

The fixed array occupies `$06A0-$09FF` exactly; the world array starts at
`$0A00`. `$ACD9` already submits every active world record, so there is no
action-style `$0400` activation gate to widen. Each world record points at a
frame composition through `+08`: count byte followed by five-byte parts
(`flags/size`, signed x, signed y, tile/attributes). `ADAD/AE6F` apply the
authentic horizontal `<$0110` and vertical `<$00F0` biased bounds while packing
x-high/size into the OAM high table.

For a future decompilation, preserve four layers rather than merging them:
type/spawn identity (`$01:E099/$E7D9`), live record update/behavior, pure
composition emission (`ADAD/AE6F`), and ROM graphics upload/VRAM asset identity.
For widescreen, change only the world-segment horizontal predicate; keep the
fixed segment authentic. Town world width/height is 512×512 px, so usable
side margins are asymmetric: `left <= cameraX`, `right <= $0100-cameraX`.
Apply the same margins to town BG and world sprites to prevent map wrap.

## 12. Conversion status

| Routine | Status |
|---|---|
| `$00:8418` / `$02:A85E` vblank wait | hle (host yield) |
| `$00:8C98` cull + `$00:8D68` builder | `widescreen-sprites-v2`: regenerated and direct-play validated Stage C/D1; wide drawing with authentic activation |
| `$02:B158` col-strip builder | original recompiled path on main; validated BG refresh separately reuses `$B825` transactionally for margin-only VRAM writes |
| `$02:B1AF` row-strip builder | original recompiled path on main; experimental hle port is not used |
| `$02:BED3/$B825/$B8A0/$B90D/$B95A`, drain chain, OAM DMA, camera `$B091` | recompiled |

### 12a. Fast-vertical-motion margin repair (2026-07-12)

Run `runs/20260712-205507` exposed a second cadence requirement in the host
margin refresher. During the Stage 1 Act 2 opening fall, player/camera motion
advanced about 5px/frame, but the host's full-column cache keyed vertical
position only at `$24 & $FF00`. Refreshes therefore occurred at game frames
1885, 1903, 1918, 1933, and 1949: gaps of 15-18 frames, or roughly 75-90px
of fall, during which newly visible margin rows could retain stale ring data.
Horizontal 16px crossings occasionally refreshed the columns sooner, which
explains why the corruption eventually self-corrected.

The fix preserves the page-aligned column decode: `$B825` intentionally needs
`world_y = camera_y & $FF00` to populate its 512px vertical ring window. It
does **not** rebuild every margin column at each 16px vertical crossing (18
column decodes in the captured room, enough to risk the prior draw slowdown).
Instead the host transaction now reuses `$02:B8A0`, marshalled exactly like
`$02:B1AF`, for each newly exposed two-tile row. Its record drains as four
32-word horizontal chunks to `base`, `base+$20`, `base+$400`, and
`base+$420`. The normal `$24 & $FFF0` crossing refreshes the authentic
`camera_x & $FF00` 512px band; if a live wide margin is outside that fixed
band, one neighboring 256px-aligned band is decoded too. Because the two bands
alias the same 64-column VRAM ring, only the tile columns intersecting the
missing margin are drained from the neighboring record; draining all 64 would
repair one edge by corrupting the opposite visible half with data 512px away.
All CPU, WRAM, and math-unit state is restored, and only range-checked words in
that layer's 4KiB tilemap persist. This is source-only and uses already-emitted
routines; no regeneration is required.

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
7. **Action sprite isolation is validated through Stage D1 (2026-07-12).**
   Raw-wide, BG-only refresh, definition widening, and initialized margin-object
   drawing were each tested separately. The historical failures came from the
   coupled replacement of `$8C98/$8D68`, including an inaccurate normal-exit
   machine-state model, not from the background decoder. Stage D2 widens
   `$0400`-gated activation separately and is directly validated in Fillmore;
   `AR_WS_MARGIN_ACTIVATION=0` restores authentic activation. Regions/effects
   still need testing with hardware scanline limits enabled and lifted.
8. **HUD (BG3)**: 32-tile-wide compose in `$7F:B000`, streamed per frame;
   margins under the HUD stay clamped (current policy) or need a host
   compose extension later.
9. **Every drawing path is now known**: any wide-mode VRAM the game won't
   supply can be host-written safely during the NMI window as long as it
   stays out of the four record buffers' way.
10. **Narrow action layers must not be world-refreshed.** If BG2 declares
    width `$32<$0200` (observed `$0100` in Bloodpool acts), it has no horizontal
    world data for margins and the refresh skips it. Its offscreen tilemap half
    may contain stale/scratch graphics. Bloodpool act 2 directly confirmed that
    centering/clamping this layer is safe in `runs/20260712-193357/`. The normal
    presentation now uses that same authentic 256px render as a source and
    mirror-fills only BG2's margins at pixel precision; `AR_WS_BG2_MIRROR=0`
    restores the centered clamp. Neither policy changes VRAM or game state.
    Its simultaneous inert-enemy/platform symptom was
    not a rendering or clamp side effect: the dispatch ring records six live
    `$8915` object-loop targets as `found:0` (see SEAMS, “Object & spawn-handler
    model”), while the chain graphics continue through independent tile animation.
    The full retest later reduced this to one inert-but-drawable enemy at `$BB25`;
    its missing behavior handler was hidden after zero holes in the `$B449` type
    table. This further confirms that mirror/clamp and object behavior are separate
    seams: host BG2 presentation never writes object state or handler pointers.
11. **Action rendering is directly validated through region `$06`.** Complete
    playthroughs of every ordinary action level confirm the streaming, sprite,
    activation, mirror/repeat padding, and observed HDMA/parallax paths. The
    remaining presentation task is not tile streaming: map the camera's finite
    world-edge limits and make them widescreen-aware so background endpoints do
    not scroll into the wider viewport. Death Heim/`70X` is a separate broken
    flow that reaches its first boss arena and then crashes.

### 13.1 Stage-B implementation refinement (2026-07-12)

The earlier `widescreen-bg` implementation proved the map-decoding idea but
did not isolate it: that branch also hle-replaced `$8C98/$8D68`, hle-wrapped
both streamers, and restored only selected DP scratch after calling `$B825`.

Main keeps all four original routines and places the validated
margin decoder in `src/actraiser_widescreen_bg.c`. Static audit of
`$B825->$B90D` shows only upload-record WRAM writes, DP `$0E`, and `$BED3`
multiply-register use; there are no PPU/OAM/CGRAM writes. The host wrapper
therefore snapshots/restores the full `CpuState`, all 128 KiB WRAM, and SNES
math state. It validates the fixed record cursor and every destination against
the owning 4 KiB tilemap before directly copying to VRAM. Consequently its
only persistent state is BG1/BG2 tilemap content. `AR_WS_BGREFRESH=0` removes
the transaction entirely for a byte-identical Stage-A A/B run.

The decoder is intentionally scheduled at the authentic streamer's tile
cadence, not at scanout cadence. A host-only key contains the action room,
current margins, each camera rounded to its 16px column (vertical position to
its 256px map page), dimensions, and every layer descriptor marshalled into
`$B825`. An unchanged key means all required tilemap words already exist in
VRAM, so the draw skips the transaction. A rejected/partial build is never
cached. This matters for two-wide-layer rooms: `runs/20260712-202151/` proved
that invoking every BG1 and BG2 margin decoder every rendered frame can consume
the presentation budget even though game logic itself remains inexpensive.

### 13.2 Narrow-layer presentation padding (2026-07-12)

`PpuSetWidescreenLayerMirror` is a renderer capability for decorative layers
that contain a real 256px image but no valid offscreen world columns. The normal
BG renderer first decodes the authentic layer into an isolated priority buffer;
the compositor merges the center normally and reflects source `-x` into left
destination `x<0`, and source `510-x` into right destination `x>=256`. Thus the
edge pixel is not duplicated (`…3,2,1,2,3…`). At the current 48px/side aspect
only the nearest 48 pixels (six tiles) are reused, never the center of the image.

Isolation is required for correctness: mirroring the live composite buffer
would also duplicate BG1 and sprites visible through transparent BG2 pixels.
The isolated z/color words instead preserve BG2 transparency, priority, palette
animation, windowing, mosaic, main/sub-screen identity, and later color math.
This is a presentation enhancement, not recovered/decompiled level data, and it
performs no PPU VRAM writes. Narrow action BG2 opts in by default; the original
clamp remains the same-binary fidelity/fallback path.

Aitos Act 1 (`$18=04`, raw maps `$19=01-$03`) demonstrates why reflection
cannot be the only padding policy. Its `$0100`-wide BG2 contains several cloud
bands observed moving at different apparent rates; whether the native game
drives them through HDMA/HBlank or another raster path is not yet traced. Reflection
reverses their slope and apparent motion at each authentic-screen edge, making
the centered cloud field tear visibly from both margins. For this act,
`PpuSetWidescreenLayerRepeat` uses the same isolated render but cyclically
continues each authentic scanline: left `x<0` samples `256+x`, while right
`x>=256` samples `x-256`. Because the copy happens after that scanline's tile
decode/window/current scroll state, all bands keep the same direction and tile
animation remains automatic. Bloodpool retains reflection; neither padding
mode reads the stale offscreen tilemap half or mutates emulated state.

Northwall (`$18=06`, raw maps `$19=01-$05`) uses the same narrow,
parallax-cloud BG2 construction and therefore selects the same cyclic-repeat
policy. Direct state evidence from `runs/20260712-222626/` shows BG1 logical
width `$2E=$0A00`, BG2 logical width `$32=$0100`, and HDMA channel 2 active.
The live `0601` callback `$00:E7BC` invokes `$02:945E`, which builds the
scanline table at `$7E:6000`; common setup `$02:96B6` targets `$210F`
(`BG2HOFS`). Thus `$0100` means 256 unique BG2 pixels, not a stationary layer:
the PPU wraps those pixels while HDMA gives different scanline bands different
horizontal offsets. `0605` was subsequently observed to use the same visual
construction; leaving it outside the repeat range restored reflection and the
same reversed-motion seam. Covering `$01-$05` prevents that mid-stage policy
regression. Northwall raw map `$08` is the boss arena and has a similar
parallax-scrolling snow BG2; it independently selects cyclic repeat. Maps
`$06/$07` remain on the default policy; completed direct testing found no
equivalent seam there.

Death Heim's boss-warp room (`$18=$07`, `$19=$01`) needs a banded policy even
though both action layers declare `$0200` width. Snapshot
`runs/20260714-174654/snapshots/snap_00_gf1436` records camera/BG1 scroll
`$22=$0000`, `$24=$001F`, BG1 size `$2E/$30=$0200/$0100`, BG2 scroll
`$26/$28=$0000/$0000`, and BG2 size `$32/$34=$0200/$0100`. Reconstructing the
two layers directly from the captured VRAM/CGRAM proves that BG1 contains only
the central stone causeway, while BG2 contains both the face statues and the
animated border/fog/water. The black left margin in the composite is therefore
the camera-at-world-edge side budget (`cam=0`), not absent fog art.

A whole-layer policy cannot separate the bounded statues from the desired wide
fog. The renderer now supports a per-layer cyclic-repeat band: it first renders
the authentic scanline in isolation, then repeats that scanline into both
margins while preserving transparency, priority, scroll/HDMA phase, character
animation, and color math. For `0701`, the full symmetric canvas is enabled,
BG1 and BG2 are clamped (`mask=$03`), and BG2 tile rows 18-27 (screen
`y=144-223`) override the clamp with cyclic repeat. Row 18 contains the
decorative divider and the fog/water begins below it; all face art ends above
the split. The world-margin decoder is skipped for this room because the
presentation samples only authentic center pixels. This is render-only and
does not alter the native scroll registers or tilemaps. Direct testing on
2026-07-14 confirmed that the complete faces and causeway remain centered,
the divider/fog fills both margins cleanly, and the animated effect continues
normally.

The post-final-boss return reuses raw map `0701` with different presentation
state. Paired captures in `runs/20260714-184728/` separate the transition:
`snap_01_gf14676` already has boss-rush progress `$0347=$07` but ending state
`$0334=$00`, while the face scene is still visible; `snap_02_gf15031` has
`$0334=$03` after the sky/cloud/water has appeared. Thus `$0347` alone switches
too early, but `$0334>=3` is also visibly late in
`runs/20260714-185817/`. The ROM sequence supplies the precise render seam:

- `$00:F5C2-$F5E3` advances object field `+$38` to `$80` while driving the
  `$2132` fixed-color fade to black;
- `$F5E4-$F5EF` advances the sequencer and waits for the statue-removal child
  referenced by `+$3A` to report `+$24=0`;
- `$F5F0-$F619` stages BG1SC `$64` and BG2SC `$74`, selecting the sky maps
  while the display is black, then seeds the fade-in counter at `$F61C`;
- `$F625-$F642` performs the fade-in, and only after it plus the `$0349` wait
  does `$F64C-$F650` write `$0334=3`.

The policy now requires `$0347>=7` and observes the live BGSC page bases
`$64/$74`; `$0334>=3` remains a settled-state fallback. It keeps BG1 clamped
and replaces the lower repeat band with whole-BG2 reflection immediately when
the sky pages become active. This samples the transitioned live BG2 after its
scroll/window state, cannot resurrect face tiles still resident in VRAM, and
joins the non-periodic cloud edges without the hard seam produced by cyclic
repeat. Direct testing on 2026-07-14 confirmed that the handoff occurs invisibly
during the black frame and that the wide sky is ready before fade-in.

Death Heim raw maps `$02-$07` (`0702-0707`) select the narrow-parallax repeat
policy. Capture
`runs/20260714-173750/snapshots/snap_00_gf4875` records `$18=$07`, `$19=$04`,
and BG2 logical width `$32=$0100`; the policy log showed `mirror=02`. Direct
observation found the padded mountain/parallax image moving opposite the
authentic center at the 256px boundaries. The same effect was then directly
reported on maps `$05-$07`. Maps `$02` and `$03` are provisionally
classified with that background family so boss-rush transitions cannot restore
reflection. The full `$02-$07` range therefore selects the same
isolated-scanline cyclic repeat as Aitos and Northwall. Direct post-build
validation remains pending for these maps, especially the provisional entries.

Final-boss map `0708` is a distinct two-layer raster arena. Snapshots
`runs/20260714-183142/snapshots/snap_00_gf12574` and `snap_01_gf12654` record
camera `$22=$0000` and both BG widths `$2E/$32=$0100`; offline VRAM/CGRAM
reconstruction identifies BG1 as the colored star road and BG2 as the sparse
star field. Both are transparent stacked effects, not platform/world layers,
and both receive live scanline/sine displacement. The generic world-edge budget
therefore left the 43px margins black. The first fix used isolated repeat on
both layers (`repeat=$03`) and filled the margins, but direct testing in
`runs/20260714-184728/` found a large performance regression. The live BG1SC/
BG2SC values are `$60/$70`: both are native 32x32-tile maps whose PPU fetches
already wrap every 256px. `0708` now only opens the symmetric canvas and draws
both raw (`repeat=$00`), preserving each layer's current raster phase while
eliminating two temporary-buffer clears and two priority merges per scanline.
Direct testing on 2026-07-14 confirmed the full-width effect and normal
performance.

## 14. Open questions (all remaining, none blocks the §13 design)

1. `$7F:B800` action-anim frame composer (find on an animated level:
   wram trace off 0x1B800-0x1BFFF during load; Fillmore act 1 idles).
2. Map `$02AC` and object `$38` selector values to named magic/effect assets;
   the slot-0 armer itself is `$00:96C3-$96F5`.
3. VRAM `$4000-$4FFF` char bank consumer (loaded but unreferenced by the
   in-game NBA regs; another section's OBSEL/NBA values `?`).
4. BG2 filler tile id per section (`$17F` seen in act 1, `$18A` earlier
   in another context) — confirm from `$B825`'s filler constant per layer.
5. Boss-arena window/HDMA effects vs margins (survey doc has the known
   full-width-window fix; iris wipes stay 256-centered — audit per boss).
6. Sim-mode (Phase 4): widen town BG coverage and the world-only `ADAD/AE6F`
   predicate; map `$02:AFCB` `$47F0` upload and `$02:8384`. The earlier
   `$01:B1F8/$B7E9/$CE69/$CEEB` camera candidates were object-field writes;
   the town camera writer is `$01:B4C6`.
7. Section config +27 -> `$F2` meaning; `$6A/$6E/$72` $2000-flag meaning.
8. Native camera/world-edge clamp ownership and its presentation-aware wide
   bounds. Distinguish changing the gameplay camera limit from merely hiding or
   padding pixels outside finite BG data; verify both axes and parallax layers.
9. Death Heim/`70X` first-boss crash, then boss-rush/final-boss rendering and
   handler flow.
