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
  ├─ camera request $02:B030: tracks subject $8A / focus $82 into $7C/$7E
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

Large scene loads are the intentional exception to that per-frame pipeline.
`$00:8433/$843E` waits for vblank, disables NMI through `$4200`, and forces
blank through INIDISP `$2100=$80`; the action loader then performs its bulk
CPU work before the next NMI. The recomp preserves the otherwise-collapsed
interval as display/audio-only host frames, so `$0088`, object logic, tile
animation, and the NMI upload chain above do not advance during the black hold.

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

SIM mode has two `$19`-gated branches. Town simulation (`$19 != 0,9`) runs
`$AF69` (CGRAM effect), `$BC56` (tile-animation scheduler), `$AEBB` (town
tilemap upload), `$AEEB` (HUD), then the same generic full-word descriptor
consumer `$AF30` used in action mode. The `$19 == 0 or 9` non-town branch
instead calls `JSL $02:8384`, copies `$030C-$0313` -> `$0304-$030B`, then
uses `$AEEB`, `$AF86`, and `$AFCB`:

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
| `$7C/$7E` | requested camera H/V delta (16-bit signed); corrected wide bounds replace it with effective motion before downstream consumers |
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

- **`$02:B030`** (JSL'd immediately before `$02:B091` at
  `$00:807E/$80BD/$82ED`): computes the requested camera movement. It reads the
  selected action-object base from `$8A`, centres object `+02` X in the native
  256px view, applies the vertical dead zone around focus Y `$82`, and writes
  signed requests to `$7C/$7E`. It does not move the camera or publish strip
  flags.
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

Corrected action-wide presentation HLEs this complete routine at the same
`$02:B091` seam. For a world extent `W`, native viewport `V`, and requested
before/after margins `B/A`, the camera interval becomes
`[B, W - V - A]` only when `B + V + A <= W`. Otherwise it remains the ROM's
`[0, W - V]`; zero/invalid load-state dimensions also retain the ROM's
unsigned fallback. Horizontal margins start with the live widescreen render
budget and vertical margins with the configured per-side Diorama extension;
each side is then limited by the canonical playfield layer's fixed extent, when
present. This makes a tuned 16/16 playfield such as Bloodpool `0208` fit only the
16 pixels it actually presents rather than shifting for an invisible full
margin. The correction also consumes the renderer's
canonical `ActionBgPlan_CanvasOwner` and provider-enable decision: authored
Death Heim hub/final scenes (`0701`/`0708`) and `AR_ACTION_BG_HLE=0` do not
acquire playfield camera policy. 4:3, Wide Raw, non-action modes, disabled
action widening, non-finite scene plans, and rooms too small for the complete
requested view are therefore behavior-preserving. BG2 parallax helpers,
player-relative tail, strip flags, and the single canonical `$22/$24` camera
remain native. A fitted presentation clamp also reconciles `$7C/$7E` before
those consumers run: while the old camera is already inside the corrected
interval they receive the camera displacement that actually occurred, not the
unfulfilled outward request; an initial correction from outside the interval
publishes zero motion. This matters at section entrances, where a native
`$7C=-120` request can coexist with the new minimum camera of 120. Leaving the
request live made BG2 parallax and the player state machine treat a stationary
boundary as movement (`runs/20260810-172649`, gf9652).

Bloodpool `0202` (BG1 `768x512`) resolves to horizontal `26..486` in flat
16:10 Full and to `120..392`, vertical `32..255` in Diorama-32. Death Heim
`0703` (BG1 `256x256`) cannot contain either widened view and stays at native
`0..0`, `0..31`. The `0701` hub stays native at `0,31` until its natural
transition to finite room `0702`; the explicit provider-off `0202` control
also remains `0,287`. Evidence: `runs/20260810-170205` (flat Full),
`runs/20260810-170240` (4:3), `runs/20260810-170857-2` (Raw),
`runs/20260810-171443` (Diorama-32), `runs/20260810-170857` (undersized),
`runs/20260810-171516` (hub transition), and
`runs/20260810-171443-2` (provider off). `AR_WS_ACTION_CAMDBG=1` reports the
live interval and whether each requested axis fit.
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
- Sim town (`$18=0`, `$19!=0,9`): same WRAM-buffered animation path as action
  mode, consumed by the generic `$02:AF30` full-word descriptor uploader.
  `$02:BAF5` first captures four VRAM pages into
  `$7F:B800/$B900/$BA00/$BB00`; later ticks re-upload one captured page to
  VRAM `$0000`. `$AF86`'s ROM-bank-$0A, high-byte-only dual upload belongs
  only to the separate `$19=0 or 9` non-town branch.
- World navigation (`$18=0/$19=9`) uses `$02:AF86` to upload the same 64-byte
  ROM frame into the high bytes of Mode-7 tile `$00` (VMADD `$0000`) and tile
  `$AA` (VMADD `$2A80`). The four frames are `$0A:B000/$B040/$B080/$B0C0`;
  a gf380-412 capture pins their order and eight-game-frame cadence. `$D7`
  retains the selected source after NMI drains `$DC`.
- The host-owned `SimWorldMap` mirrors that operation from immutable ROM
  instead of sampling live VRAM. Navigation consumes the retained `$D7`
  phase; simulation-town outer underlays continue the same pinned cycle from
  the global game-frame clock because town `$D7` describes different art.
- Recomp compatibility seam: `$02:BC56` is HLE'd to defer animation ticks
  while INIDISP force-blank is set. This prevents a slow SPC `$F0` ack from
  letting NMI upload the still-empty phase-0 buffer before `$BAF5` captures
  it; phase and descriptor state otherwise remain native.
- Independent second mechanism: `$02:AEAE` flips BG2SC between tilemap
  pages per `$C5/$C7` counters (`$02:BC34` phase helper) — tilemap-page
  animation, also disabled in Fillmore act 1.

## 8. Char/sheet loading + VRAM layout

| VRAM words | Contents | Writer |
|---|---|---|
| `$0000-$1FFF` | BG1+BG2 chars (BG12NBA=$00); Sky Palace capture is byte-identical to the `$4000`-byte ROM bank at `$0D:C000` (file `$06C000`) | `$02:B28E`+`$02:B475` decompressor pair (force-blank port loops) |
| `$2000-$2FFF` | common action OBJ atlas (OBSEL=$01, 8x8/16x16) | `$02:BC9E`: 4096 words from ROM `$07:8000-$07:9FFF` |
| `$2D40-$2DBF` | reserved OBJ magic/effect overlays inside the common atlas | `$02:BC9E` writes 128 words to `$2D40`; `$00:96C3-$96F5` can arm 128-byte slot-0 upload to `$2D80` |
| `$3000-$3FFF` | OBJ address space reachable through OBSEL name selection; resident contents/consumers not yet catalogued | `?` |
| `$4000-$4FFF` | extra char bank (user `?` — B28E loads it; no NBA points there in-game) | `$02:B28E` |
| `$5000-$57FF` | BG3 chars 2bpp (BG34NBA=$05); dialog font is the `$1000`-byte decode of compressed ROM `$17:ECFB` (file `$0BECFB`) | `$02:C5C9` decompressor |
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

### 9a. Host action lighting and particles

`ActionEffects_CaptureFrame` is the game-thread boundary for presentation-only
spell metadata. Its data-driven rules require the live controller and complete
per-slot animation/composition/transform identity; mirrored or cloned actors
remain separate rather than collapsing to a player-centred effect. The capture
publishes generic point/rectangle/segment geometry, authentic OBJ priority, and
separate actor, phase, and pulse clocks. Those clocks advance by captured
emulation ticks and therefore freeze during paused or retained-slot redraws.
Observer state is explicit, not hidden inside the capture module, and savestate
loads reset it before the next frame.

`ActionSceneEffects_CaptureFrame` is the parallel boundary for exact scene
accents, including Bloodpool's map-$08 boss attack resolved from the 30-snapshot
run `20260810-180202`, a complete decode of its `$7E:5000` animation bank, and
the global player sword beam measured in run `20260810-175403`.
BG wall torches are not actors: the observer uses the same validated
`ActionBgMapView` contract as the full-world provider and matches the exact BG1
metatile pair `$47` over `$4F` throughout map group `$02`. Fireballs and
lightning are ordinary action objects and require the positive handler,
resume/source, animation-state, visual, and composition tuples recorded in the
RAM map. Boss lightning further requires map `$02/$08`, animation bank
`$7E:5000`, and a validated backlink to its live `$BDFF` parent. States `$02`
through `$07` select vertical or diagonal long/medium/short strikes; their
resume PCs vary during normal control flow and are not shape identities.
Visual `$20` is the blank half of each strike cycle, so it intentionally emits
no host effect. Matching only a visual ID is invalid because action slots and
values are polymorphic. A recycled same-kind slot begins a fresh renderer
generation when its lifecycle key changes or its position is discontinuous
with its measured velocity.

The sword-beam rule is not map-specific. It requires handler `$9D1C`, animation
`$06:8000`, attacker flag `$0001`, backlink `$08A0`, a source descriptor shared
with the active player, and exact state/visual/composition `$13/$30/$99E8` or
`$14/$31/$9A17`. Its raw collision header includes signed byte origins, so
capture publishes four explicit state/direction rectangles instead of feeding
those words into the generic unsigned-extent path. Run `20260810-184935`
provides the decisive state-`$13` proof: hot point `(112,201)` versus OAM bounds
`(144,168)..(160,200)`, yielding local `(32,-33)..(48,-1)`.

`DrawActionEffects` runs after the authentic action image and before flat HUD,
HD-replacement, inspector, and settings overlays. Flat mode uses the resolved
physical viewport. Diorama mode receives a `DioramaProjection` value from the
same composite call that drew the BG and OBJ planes: camera matrix, capture
mesh dimensions, BG1/OBJ interpolated UV window, output dimensions, and exact
authored depth/rake/bow are not re-derived from live state. Scene metadata also
selects its authentic plane: torches use BG1-low, while fireballs and lightning
use OBJ priority 0. The projection value owns `texture_x_origin`, the hidden
64-column OBJ resolve apron that precedes caller-visible capture coordinates.
Keeping that origin in inverse projection prevents overlays from sliding
horizontally on raked planes without changing the flat path's contract. The
explicit render policy is a world overlay above the composed world and below
HUD/HD UI.

`action_effect_render.c` converts captured kind/phase/geometry into bounded,
renderer-independent spell and scene batches; unknown values fail closed. A
small capacity-aware geometry writer appends directly to the caller's final
arrays, avoiding a spell-sized scene scratch copy and its former stack peak.
Torch light/particles sample the shared authentic game clock at 2× visual rate
to follow the fast BG flame animation while all torch instances remain in
phase. Fireball sparks trail opposite measured velocity. Lightning lighting
uses the live `88+88` extents, and its last nontransparent ring reaches both
ends of the full 176px shaft rather than letting only a transparent falloff
cover them. The boss strike adds a warm spill plus two bounded filaments
following the actual per-row OAM centroids for all six authored
vertical/diagonal, long/medium/short shapes. The longest uses 24 segments,
horizontal flip mirrors the complete path, and the action-OBJ emitter's extra
one-pixel Y draw bias is applied before projection. Visual `$20` stays blank;
only the linked state-$09 child receives the separate floor-impact bloom.
The sword crescent receives a restrained cool halo/core, narrow 80px/56px
connective haze layers, and forty-eight crossed-diamond star glints arranged as
sixteen fixed cross-sections with top/centre/bottom lanes from 4px to 88px
behind the crescent. Scrambled 18-tick phases independently fade and scale each
glint into and out of existence without changing its local centre. The path
mirrors from measured velocity, and its local basis is projected through OBJ
priority 0, preserving the same form on a Diorama-raked plane. The mapped
player lifecycle permits one beam, so the batch reserves one explicit expanded
stream and rejects impossible duplicates instead of inflating all scene slots.
Integer-hash particles and integer triangle pulses make repeat builds
deterministic. `present.c` only supplies flat/Diorama projection and submits
through the same verified SDL additive blend plus untextured batched geometry
used by town effects; no optional Metal/Vulkan shader pipeline is required.

`action_effect_lighting` and `action_effect_particles` are independent,
default-on Graphics settings. Backend rejection latches the shared host-effect
capability off and both stages fail closed.

## 10. Palette paths

- General descriptor: game sets `$CB/$CD` (src), `$CE` (CGADD), `$CF`
  (32B-row count); NMI `$02:AE75` uploads + clears. (e.g. `$00:A1CE`:
  rows $C0+ from `$0B:8280`.) Palette data bank: `$0B`.
- `$02:ADFF`: fixed 2-frame flicker of CGRAM row 7 from ROM `$02:AE35/55`
  (sprite palette 7 = player hit-flash row).
- `$02:ADE2`: fixed-color fade (`$2132`) from `$BD/$BE/$BC` while `$C4`.
- Sky Palace dialog-frame palette 7 is an exact match for ROM `$1C:BF73`
  (file `$0E3F73`). Palette 1 at `$1C:BEB3` supplies scenery tile `$18`,
  explaining why the game's lower 16×16 box metatiles are not reusable as
  standalone host corners.

## 11. UI / dialog compose (sim engine)

- Town-map tile *content* (which house/road/bridge tiles exist where) is not
  decided in this engine layer: structure records drive per-record visual step
  programs (`$03:A4B8` → `$7F:77E7` slots → the `$89F7`/`$8A7E` stepper) that
  edit the town map, which the SEAMS "Sim-mode town-map GRAPHICS pipeline"
  then uploads. See SEAMS town §7 for the record/step system.
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
- Full snapshots `runs/20260716-072558/snapshots/snap_00_gf460` (ordinary
  dialogue) and `snap_01_gf668` (three differently sized native boxes)
  resolve the box itself down to reusable 8×8 characters. The 16×16 source-map
  nine-slice is `$36/$55/$37`, `$3E/$59/$3F`, `$4E/$56/$4F`, but `$4E/$4F`
  are scene-composition metatiles rather than pure corners: their outer halves
  are palette-1 palace tile `$18`. The actual frame is:

  | Position | 8×8 character |
  |---|---|
  | top-left / top-right | `$CE` / `$CF` |
  | top edge | vertical-flipped `$EE` |
  | left / right edge | `$DE` / `$DF` |
  | center | `$FF` (opaque black) |
  | bottom-left / bottom-right | vertical-flipped `$CE` / `$CF` |
  | bottom edge | `$EE` |

  These characters use palette 7 and form a clean arbitrary-size nine-slice;
  palette index zero supplies the transparent bevel cutouts. This is immutable
  asset identity useful to decompilation and replacement work, independent of
  the host settings implementation.
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
  corrected wide: camera $22 = clamp(..., extra, $0100-extra)
                  (16:9 => $002B-$00D5; directly validated 2026-07-14)

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
Apply the same margins to the town camera, BG, and world sprites to prevent map
wrap or cleared pixels at either edge. `$B4C6` runs before `ACD9`, so changing
the camera there keeps PPU scroll and OAM on the same origin.

The first town stage is now implemented at the host presentation seam. For
`$18=$00,$19=$01-$06`, `ActRaiser_ApplyWidescreenPolicy` keeps the PPU centering
budget fixed but grants live BG margins
`left=min(extra,$22)` and `right=min(extra,$0100-$22)`. BG2 remains clamped to
the authentic center for dialogs, and unrendered edge gaps are cleared every
frame. `AR_WS_SIM=0` restores the pillarboxed baseline. The `$B4C6` HLE keeps
the full wide viewport inside the 512px map; RAW wide retains the native camera.
Direct testing on 2026-07-14 confirmed the corrected-wide clamp works as
expected. The `ADAD/AE6F` sprite predicate remains a separate
composition seam. The sprite implementation is active behind cfg HLE replacements:
both leaves use one faithful component/OAM port, with AE6F retaining its exact
attribute rewrite. Only `$0A00-$1087` world-record bases receive the current
asymmetric BG margin bounds; `$06A0-$09FF` fixed records and vertical clipping
stay authentic. `AR_WS_SIM_SPRITES=0` restores the native horizontal predicate,
and `AR_WS_SIM_SPRDBG=1` logs newly admitted components. Fillmore direct testing
confirms complete enemy compositions in the margins; the other five towns use
the same range gate but remain content-matrix validation targets.

The angel arrow exposed the next layer of the same pipeline. Its dedicated
world record `$0B0A` does reach ADAD, but state-2 movement `$01:B44B` first calls
the single-use lifetime leaf `$01:B473`. That leaf releases the record whenever
`x+4` leaves the authentic camera interval, so the widened emitter never sees
the arrow. The `$B473` HLE extends only its horizontal camera interval
to the live finite-world margins; the 512x512 hard bounds, `$E0`-pixel vertical
window, DP scratch, and carry result remain faithful. This distinction should
survive a decompilation: composition visibility and actor lifetime are separate
policies even when they produce the same apparent edge cull. Regenerated direct
testing confirms the arrow now traverses both margins correctly.

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

### 12b. Margin drain spans must match the REFRESH CADENCE, not the camera (2026-08-06)

§12a gave the host refresher the right *rows*. This entry is about the right
*columns*, and it corrects two things the vertical-extend work inherited.

**The row record is indexed by absolute ring column.** `$B8A0` fills its record
so that record slot `c` is the tile whose world index is `≡ c (mod 64)` — NOT
slot 0 = the record's own `world_x`. This is why `ws_drain_row_record_range` can
compute `col = tile & 0x3F` and read the record at that same index, and why a
neighbouring band built at `page ± 256` (ring phase 32 away from the page row)
still lands correctly. **Proven, not assumed:** building the neighbours at
`± 512` instead — same ring phase as the page row, therefore correct under
*either* indexing convention — produces byte-identical tilemaps across a full
replay, provided the build-site guards are held fixed in both arms. (Vary the
guard too and pageX=256 silently drops the left neighbour, so the diff measures
"built vs not built" and tells you nothing. That mistake costs a run.)

**The drain span must cover the whole refresh window, not the current pixel.**
`WsLayerRefreshKey::camera_x_tile` quantizes the camera to 16px, so after a
rebuild the camera keeps moving up to 15px before the next one fires. Deriving
the drain range from the exact `camera_x` leaves the outermost one or two margin
tile columns undrained for the rest of that window — and **undrained is not
empty**: the page-aligned *full* drain has already written all 64 ring columns,
so those cells hold real world content from 512px away. The result is a
convincing fragment of the wrong place welded to the extreme edge, clearing at
the next 16px boundary. Walking left strands it on the left edge, walking right
on the right edge; the self-correction is what made it read as a transient
streaming glitch rather than a coverage bug.

Fix: compute `view_left`/`view_right` from `camera_x & ~0xF` (and `+ 15` on the
right), making the drained span a superset of anything displayable before the
next rebuild. The column-strip loops always did this — that is what their `+ 1`
strip is for; only the row drains were still per-pixel.

**The vertical band must be selected by intersection, not by a step count.**
`ws_build_band_rows` stepped `k * 16` down from `row_y = camera_y & ~0xF`
starting at `k = 1`, which assumes `camera_y` is already 16px-aligned. At
`camera_y = 504` with a 32px band it built `[464, 496)` for a band spanning
`[472, 504)` — missing the band's bottom 8px row entirely. That row sits above
the camera, so the game's own streamer never refreshes it either, and it stays
stale until vertical motion happens to rebuild it. Select rows by intersection
with `[camera_y - band_px, camera_y)` instead; correct at every phase, no
separate alignment case.

**Verification technique worth reusing.** The world is static, so a given
(world tile column, tile row) must read the same tilemap value every time it is
DISPLAYED. Recording that map over a replay and reporting contradictions turns
"does the margin ever show the wrong place?" into a number: 4 contradictions in
28818 displayed-tile samples before the fix, 0 after. Pixel gates could not see
it — the attract and flat-widescreen shot frames land before act entry, and the
headless diorama capture composites no band.

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
8. **HUD (BG3 + selected-magic OBJ)**: the 32-tile-wide compose in `$7F:B000`
   is streamed per frame. Widescreen-full now promotes its status band into a
   transparent host surface, with action/simulation-specific anchors and a
   separately promoted four-slot selected-magic OBJ signature. The host draws
   both after game upscale, so HUD size no longer depends on world scale.
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
11. **Action rendering is directly validated through region `$07`.** Complete
    playthroughs of every ordinary action level plus Death Heim confirm the
    streaming, sprite, activation, mirror/repeat padding, observed
    HDMA/parallax paths, boss rush, final boss, and return transition. The
    remaining presentation task is not tile streaming: map the camera's finite
    world-edge limits and make them widescreen-aware so background endpoints do
    not scroll into the wider viewport.

### 13.1 Promoted HUD host overlay (2026-07-15)

The HUD-scale implementation is deliberately a presentation seam rather than a
second SNES tilemap rewrite. Its reusable runner contract is implemented by the
bundled runtime's widescreen/PPU interfaces:

- `PpuSetWidescreenHudSplit` identifies the live BG3 status scanlines and their
  source boundaries. Action uses `height=40`, `left=0..87`, `center=88..167`,
  `right=168..255`, with three horizontal bands: the upper band (y 0-19)
  three-way splits ACT/TIME/SCORE+magic, the player-health band (y 20-27)
  two-way left+right at x 168, and the enemy-health band (y 28-39) anchors
  full-width left (boss health spans 256px). Simulation uses a two-way
  `0..167` / `168..255` split and no center or multi-band groups.
- While that policy is active, ActRaiser requests the generic BG3 overlay
  capture rectangle `(0,0)-(256,40)`. The normal 2bpp sampler writes its
  authentic, unsplit status pixels into the source-indexed overlay buffer; the
  `RemoveFromGame` flag omits the rectangle from both main and subscreen.
  Transparent tile pixels remain alpha zero. Palette lookup and master
  brightness are resolved by the PPU before exporting ARGB, so the host does
  not need to understand SNES tile formats.
- **Diorama mode extends that same rectangle to the full authentic height
  (2026-07-23).** BG3 carries more than the status bar: the act-title card
  (tilemap rows 8/10, y=64..88) and the pause text are the same layer, just
  below the split. In flat mode those rows stay in the game framebuffer and
  are simply visible; in diorama mode that framebuffer becomes the *backdrop
  plane*, drawn first and then painted over by the BG2/BG1/OBJ planes — the
  text disappeared behind the scene. With `diorama_hud_flat` on, the capture
  is therefore reissued as `(0,0)-(256,224)`, and `BuildHudPresentationChunks`
  emits one extra chunk for everything below `hud_split_height`: **centered,
  256 wide, at its authentic Y**, because that content is authored for the
  authentic screen and has no left/right anchor semantics. The three anchored
  bands are driven by `wsHudSplitHeight`, not by the capture rectangle, so
  their geometry is unchanged. Only an already-active capture is extended — a
  frame with no HUD split (non-action map, 4:3/RAW) must not get a
  `RemoveFromGame` BG3 capture that nothing on the present side draws. Flat
  mode leaves `hud_body_y1` zero and is bit-identical.
- `$00:923A`'s selected-magic icon is accepted only when OAM slots 0-3 exactly
  match tiles `$D4-$D7` and the expected 2x2 Y layout. Those slots are routed to
  the generic OBJ overlay buffer; every other OBJ remains on the normal sprite
  path. Earlier render-scoped OAM coordinate mutation is gone, so emulated
  state, savestates, future DMA, and game logic remain authentic.
- Simulation's hourglass (town maps 1-6) has a fixed four-sprite shape but a
  dynamic OAM allocation. The ordinary Fillmore snapshot at
  `runs/20260716-172322/snapshots/snap_00_gf1378` places it in slots 0-3 and identifies
  fixed/overlay record 23 (`$083E`, live frame pointer `$01:DD4B`) and its OAM
  signature: left/right x `$94/$9B`, upper/lower y `$0B/$13`, and output
  attributes `$31/$71` (the right halves are horizontally flipped). ROM
  compositions at `$01:DD4B/$DD60/$DD75/$DD8A` prove four animation phases:
  paired upper tiles `$EC-$EF` and lower tiles `$FC-$FF`. Validation accepts
  only that range and requires both halves and the `+$10` lower-tile
  relationship. Opening the sim menu consumes slots 0-10 and moves the same
  icon to slots 11-14 (`runs/20260810-231616`, gf61067), so promotion scans the
  complete OAM table for this signature. The same host placement then pins the
  16px capture four native pixels before simulation's right/score group.
  Enhanced-sim HUD handoff validates the promoted OBJ capture against that same
  per-frame scan result; it must not reimpose slots 0-3 after promotion has
  already discovered another range. `runs/20260811-145909`, gf18992 confirms a
  menu-open phase `$ED` hourglass in slots 11-14. A capture that names any other
  range remains an overlay conflict and fails closed.
- Sky Palace's selected-magic icon is a separate OAM capture path: the game
  dynamically allocates its sprites. With no dialog sprites the icon can occupy
  slots 0-3; dialog/menu sprites can push the same icon to slots 6-9 (both
  layouts measured in `runs/20260808-214848`). The host therefore scans the
  complete OAM table from slot 0 for the signature rather than imposing a
  minimum slot or hardcoding an index.
- **That icon has two OAM shapes, one per spell (corrected 2026-08-06).** The
  ROM draws the same 16x16 framed icon at the same fixed position (x `$94`,
  y `$0B`, attr `$39`) two different ways, measured one snapshot per spell in
  `runs/20260806-232552`: **Magical Fire** spends FOUR 8x8 sprites (tiles
  `$67/$67/$77/$77`, y `$0B`/`$13`, attrs `$39`/`$79` mirrored pairs), while
  **Stardust/Aura/Light** spend ONE 16x16 sprite (tile `$84`/`$86`/`$88`,
  attr `$39`) with no companion slots. `ActRaiser_SkyPalaceMagicIconSlots`
  (actraiser_game.h) is the single predicate both the promote and its test read;
  it discriminates on the high-OAM **size bit**, not the tile number, because
  the tile set is per-spell and the shape is not. The single-sprite form also
  requires OBSEL to resolve "large" to exactly 16, since the host projects this
  icon as a fixed 16x16 chunk. Until this was found, only Fire's quad was
  recognised, so the other three spells were never promoted and drew at their
  authentic centre-screen X while the rest of the HUD moved — ledger §38.
  `AR_HUDICON=1` reports the scan outcome (slot and count, misses included).
- `src/main.c` binds generic BG3/OBJ surfaces, then uploads the game and those
  two captured surfaces separately. It
  renders the game with the normal logical-size/PAR transform, then composites
  the HUD in physical renderer-output coordinates: left at the presentation
  viewport's left edge, action timer at center, and score/magic at its right
  edge. The lower action health row stays left. The selected-magic OBJ is placed
  immediately before the right BG3 group.
- `hud_scale_percent=0` (**Match game**) derives the current X/Y game scale and
  preserves the Phase-1 visual size. `100` is native host-output 1x vertically;
  the X scale additionally applies the configured 7:6 SNES pixel aspect when
  enabled. `-`/`+` adjust by 25 percent, clamped to 25-400. Authentic 4:3 and
  widescreen-raw never enable extraction and retain the ordinary in-frame HUD.
- Renderer-backed F2/`AR_SHOT_AT_GF` captures read the final composited output,
  so scaled-HUD regressions include the host overlay. Pure headless/oracle runs
  bind no overlay surfaces and preserve the historical internal framebuffer
  and deterministic emulated state. Visual automation can opt into the real
  compositor with `AR_HEADLESS_VIDEO=1` (normally paired with the SDL dummy
  video driver); this creates a hidden software renderer without enabling
  interactive input or frame pacing.

### 13.1.1 Manifest-driven HD replacements (2026-07-15)

Second consumer of the generic overlay contract, swapping captured graphics
for external high-resolution art. Replacements are data-driven: each
`[replace:<name>]` entry in `game-assets/manifest.ini` declares a
substitution. The manifest is tracked with every discovered hook shipped
active; art files beside it are gitignored, and an entry whose image is
absent stays silently inert, so users enable a hook by simply dropping in
an image with the matching filename. Each entry uses a
"plane" (the tool used), a gate, and an art file — see the manifest's header
comment for the full key/gate grammar. Planes are capability tiers:

- `screen` (live): host-overlay substitution of screen-locked, untransformed
  graphics via `PpuSetOverlayCapture` + `RemoveFromGame`. `src/hd_replacements.c`
  owns parsing and the per-frame gate policy (`HdReplacements_EvaluateFrame`,
  called after the HUD/OAM capture policies so busy sources are skipped, not
  clobbered); `src/main.c` decodes each entry's PNG (vendored
  `third_party/stb/stb_image.h`; `AR_HD_MANIFEST` overrides the path), binds
  overlay surfaces for the sources used, and draws active entries over their
  promoted rectangles in physical viewport coordinates, modulated by INIDISP
  brightness (forced blank suppresses). Missing manifest/art, headless runs,
  or `hd_replacements=0` all degrade to authentic rendering because an
  unbound source makes `RemoveFromGame` a no-op. One capture rect per source
  per frame is a renderer invariant; conflicting entries warn and skip.
- `mode7` (live, 2026-07-15 — **FROZEN, do not extend**: correct for its one
  shipped consumer (the sprite-free title swirl) but built on paste
  composition, which cannot express priority. Its backend migrates into the
  N-x RGBA pipeline (see [`nx-pipeline.md`](nx-pipeline.md)); the manifest
  schema is backend-agnostic and survives unchanged. Do not add mode7
  entries for scenes with sprites over the canvas, and do not build OBJ
  promotion — that was evaluated and rejected as a paste-path special
  case.): canvas-space texture override rendered through
  the live matrix. The engine API (`PpuBindMode7OverlaySurface` +
  `PpuSetMode7Override`, `snesrecomp-go/runtime/src/ppu.c`) samples the entry's ARGB art at
  the per-pixel canvas coordinates inside `PpuDrawBackground_mode7`, so
  rotation, zoom, per-scanline HDMA warps, windows, field wrap, and INIDISP
  brightness all apply. Output goes to a 4x-supersampled overlay surface
  (`kHdMode7Scale`) composited between the game frame and the OBJ/HUD
  overlays; opaque samples are removed from main+sub game buffers (no
  color-math ghost), translucent fringes blend over the authentic frame.
  First consumer: `[replace:title-swirl]` rides the intro warp with the same
  art as the settled screen-plane entry (`m7!=identity` vs `m7==identity`
  gates make the handoff seamless). `AR_M7_DUMP=1` dumps each distinct
  Mode-7 canvas as a paletted 1024x1024 PPM — the artist source for
  `canvas_rect`. Caveat for scenes with sprites above the canvas (world
  map): those need full-screen OBJ promotion so sprites composite above the
  substituted scenery — not yet wired.
- `tiles` (reserved): parsed but inert — the planned hash-keyed HD
  tile-pack path (needs the N-x RGBA-sideband renderer extension).

`AR_TILE_CENSUS=1` (src/hd_tile_census.c) is the tile-pack sizing survey for
the `tiles` plane: a read-only per-frame walk of visible BG/OAM/Mode-7 tiles
that writes unique-tile contact sheets (`tile_sheet_<class>.ppm`), a JSONL
census, and a palette-variance summary to the run dir. First results
(boot/title + level1-action.rec): 806 unique tiles, only 15 with more than
one palette variant — (tile bytes + palette) identity is viable and packs
are small.

#### Click-driven scene inspector (2026-07-16)

The host scene inspector (`src/scene_inspector.c`) is the interactive front end
to these replacement seams. Enable the persistent `scene_inspector` setting,
press F3, or seed `AR_SCENE_INSPECTOR=1`; a left click inside the presented
game viewport maps through the real renderer viewport/PAR and widescreen crop
back to SNES screen coordinates, freezes frame advancement, and walks the live
PPU state without mutating it. The inspector records whether it introduced the
pause: right click, F3, or P clears the selection and resumes only in that case,
so inspecting a frame that was already manually paused preserves that state.

For Mode 1/0 BG candidates it reports the layer/bpp, scroll and widescreen
source policy (center, wide, clamp absence, mirror/repeat band, or
promoted-HUD anchor), tilemap entry and VRAM word address, character word
address, tile number, sampled pixel, CGRAM index, palette, priority, and flips.
For OBJ candidates it reports every containing OAM slot, sprite size/rectangle,
base animation-frame tile, clicked subtile, character address, palette,
priority, flips, and sampled pixel. Mode 7 clicks follow the live matrix for
that screen sample and report canvas coordinate, tilemap tile/address, pixel,
matrix, and a starter `canvas_rect`. Game frame `$0088`, state `$18/$19`,
camera `$22/$24`, map dimensions `$2E/$30`, screen masks, brightness, and live
margin sizes accompany every report.

The compact result keeps the native dialog nine-slice but uses a dedicated
host-side 5x7 monospace atlas instead of the ROM font. Labels, numeric/VRAM
values, target layers/source policies, warnings, and control hints use distinct
debug colors. It caps its scale independently of the settings menu and initially
fits its frame to the longest visible report line before occupying the output
half opposite the selected point. A left press in the
panel's title strip is intercepted and begins a bounded drag. A cyan
lower-right grip uniformly scales the complete panel from 50% to 250%, keeping
its logical report width and columns intact. The remaining report body
deliberately passes clicks through to scene selection, so a panel covering the
new sample cannot silently retain an old crosshair. The complete
console result includes a manifest gate and an
honest backend recommendation. Its FNV-1a content hashes use exactly the class
seeds and raw tile representation of `AR_TILE_CENSUS`, so a clicked hash can be
searched directly in `tile_census.jsonl`. `screen` and the constrained `mode7`
plane remain the live backends. A scrolling BG/OBJ click is identification for
the reserved `tiles` plane, not a claim that hash-keyed replacement already
works. `tests/scene_inspector_test.c` guards center/mirror BG mapping and OAM
frame/subtile identity.

The overlay's Inspector category also provides a one-click resident asset dump
(`src/scene_asset_dump.c`). Unlike F2's framebuffer-oriented diagnostic
snapshot, it decodes complete data sets: every tilemap cell for each BG layer
(or the full 1024×1024 Mode-7 canvas), all 128 OAM compositions in a fixed
16×8 sheet, and both OBJ name bases repeated through all eight OBJ palettes as
a sprite-sheet atlas. Consequently, animation cels already loaded in OBJ VRAM
remain visible even when OAM currently references another frame. A CGRAM sheet,
raw VRAM/CGRAM/OAM/WRAM, and a JSON register/layer/OAM index accompany the PNGs
in a frame-unique `scene_assets_*` run-directory folder.

Presentation hit-testing is not limited to the base framebuffer transform.
`BuildHudPresentationChunks()` is the shared source of the promoted HUD's
texture-source, authentic-screen-source, and output-destination rectangles.
The compositor draws those chunks and the inspector walks the same list in
reverse draw order. It rejects transparent captured pixels, inverse-maps a hit
through independent HUD scale and left/center/right anchoring, and filters the
PPU walk to captured BG3 or captured OBJ as appropriate. Its marker and best
candidate rectangle are then projected forward through that same chunk.
`SDL_RenderSetLogicalSize` filters absolute mouse events before queuing them,
so while it is active `event.button/motion.x/y` already use the logical render
resolution. They map directly through the physical presentation viewport; a
second window-to-logical conversion or window/output scale would double-apply
the transform and discard far-edge clicks. Only the logical-size-zero fallback
uses the live window/output ratio (including high-DPI backing scale).
Independent HUD scale and anchoring are resolved afterward by the chunk
inverse.

First shipped entry: the title logo. The title screen ($18=00/$19=00) is a
single Mode-7 BG1 canvas (no OBJ sprites); the intro swirl is a per-scanline
HDMA matrix animation on channel 2 that lands on the identity matrix
(`m7 = [0100 0 0 0100]`), with INIDISP fading `00 -> 0f`. The manifest gate
(`wram[0018]==0, wram[0019]==0, mode==7, m7==identity`) therefore keeps the
swirl authentic and swaps the artwork band x=[11,248) y=[27,122) — menu text
at y>=140 is never captured — on the settled frame. `AR_TITLELOG=1` prints
the per-frame gate inputs used to derive this signature. Parser/evaluator
unit tests: `tests/hd_manifest_test.c`.

### 13.2 Stage-B implementation refinement (2026-07-12; retired by BH8)

The earlier `widescreen-bg` implementation proved the map-decoding idea but
did not isolate it: that branch also hle-replaced `$8C98/$8D68`, hle-wrapped
both streamers, and restored only selected DP scratch after calling `$B825`.

At that checkpoint, main kept all four original routines and placed the validated
margin decoder in `src/actraiser_widescreen_bg.c`. Static audit of
`$B825->$B90D` shows only upload-record WRAM writes, DP `$0E`, and `$BED3`
multiply-register use; there are no PPU/OAM/CGRAM writes. The host wrapper
therefore snapshots/restores the full `CpuState`, all 128 KiB WRAM, and SNES
math state. It validates the fixed record cursor and every destination against
the owning 4 KiB tilemap before directly copying to VRAM. Consequently its
only persistent state was BG1/BG2 tilemap content. `AR_WS_BGREFRESH=0` removed
the transaction for a byte-identical Stage-A A/B. BH8 later deleted this whole
host transaction after the default HLE provider passed the broader BH7 gates;
the setting name is now only a hidden load-only compatibility alias.

The successor HLE tile source is observable independently. With
`AR_ACTION_BG_HLE_COMPARE=1`, `src/actraiser/actraiser_action_bg.c` captures the
same two low-WRAM decoder records, expands
them through the bounded `ActionBgWorld`, and compares every tile touched by
the authentic 256x224 viewport with the live 64x64 VRAM ring. Invalid modes,
disabled layers, non-64x64/native tilemaps, malformed sources, and allocation
or comparison failures are counted as explicit fail-closed reasons. It writes
no emulated state and does not affect scanout. The first deterministic Fillmore
act-2 replay produced 6,729,804 matching tile words; enabling the observer left
the final WRAM, SRAM, dispatch log, and state dump byte-identical.

Scene policy is selectable now, without selecting HLE pixels. The pure
`ActionBgPlan` owns all 49 known action-map classifications and compiles into
the existing PPU clamp/mirror/repeat setters. The ActRaiser adapter is its only
live-state capture site; `actraiser_rtl.c` no longer contains the map-specific
action background table. A pre/post integration oracle found byte-identical
framebuffers, PPU snapshots, WRAM/SRAM, dispatch logs, and final state across
the 12 entry census, representative wide policies, and vertical/diorama cases.
Diagnostics name each planned source.

BH4 first made the finite-world source selectable only for synthetic margins.
At that checkpoint, explicit `AR_ACTION_BG_HLE=1` asked
`ActRaiserActionBg_BindPlan` to validate each planned
world layer, atomically update its `ActionBgWorld`, and bind a generic
`PpuVirtualTilemapBinding`. The binding carries the full per-layer camera and
the matching 10-bit PPU scroll phase. Scanout adds the nearest signed live
phase delta on every line, so HBlank/HDMA motion survives across the 1024px
hardware wrap. Only x outside 0..255 or scanlines outside 1..224 call the
provider; the authentic centre keeps the original pointer-walking VRAM-ring
path byte-for-byte unless the explicit BH5 ownership flag described below is
set. A finite miss is transparent. Native/decorative sources stay on raw,
mirror, repeat, or clamp presentation from the plan.

The provider changes only the tilemap word. Character bits remain live VRAM,
and the resulting z/color word continues through the existing palette,
transparency, tile flip, priority, window, mosaic, main/subscreen, brightness,
and color-math paths. Its bindings are render-only, excluded from savestates,
and cleared on reset and every frame-policy rebuild. The real-PPU harness pins
each of those effects plus signed `0/$3FF` scroll wrap and unchanged centre
priority words.

The historical positive controls established ownership before deletion: Wide
`0101`, `0201`, and `0401` pairs matched screenshots and state; HLE remained
correct with the then-live `AR_WS_BGREFRESH=0` and `AR_VEXT_BANDFIX=0` repairs
disabled. BH8 removes both repair paths. An unbound planned world layer is now
reclassified to an authentic-viewport clamp by the pure plan helper, so failure
cannot expose stale ring margins. Wide Raw remains the explicit raw control.

BH5 adds `kPpuVirtualTilemapFlag_IncludeAuthentic`. The ActRaiser adapter sets
it only when the full camera agrees with the live 10-bit PPU scroll phase and
an exact 1-based-scanline viewport comparison finds zero native-ring mismatch
and zero finite-world exit. A finite-world exit still rejects the layer. A
native-ring word mismatch with all coordinates inside the finite world instead
binds the provider without `IncludeAuthentic`: live VRAM remains authoritative
in the authentic centre while the immutable world supplies synthetic margins.
This is also the safe contract when the native ring is temporarily behind the
visible tile edge. Bloodpool `0207` proved that case in
`runs/20260810-172649`: BG1 ring `$6000-$6FFF`, the WRAM map, and the metatile
table were byte-identical between gf9652 (camera Y 767) and gf10040 (camera Y
760), but the 8px upward move exposed tile row 95 before the native 16px row
publication refreshed it. Exactly eight resident words on that newly visible
row contradicted the immutable decoder; there was no transition/actor writer
to symbolize. The old atomic fallback clamped BG1 to the authentic viewport,
making most of the wide playfield disappear.

Marahna's action BG2 is the mapped exception to finite horizontal topology, not
an exception to that fail-closed rule. Its separate 512px backdrop follows the
independently wider BG1 with the same full camera X. All 924 authentic BG2 ring
words in `0501` gf2331 match decoded X modulo 64 tiles; all 957 in `0502` gf9728
do too, despite different BG1 maps and subsection IDs. The planner therefore
detects that structural relationship rather than naming `$19`: Marahna, BG2
width 512, wider BG1, and equal camera X. `ActionBgLayerPlan.wrap_world_x` then
makes provider lookup and native-ring preflight apply the same modulo. BG1
remains the finite playable map, while the PPU still combines the two
independent layers through the live main/subscreen color-math state. See
§13.4 for the register-level finding and the separated-plane reproduction.

Room `0505` adds a presentation limit without changing that provider topology:
BG2 uses Repeat/fill with a fixed 128px extent on each side, while its source
remains the decoded world cycle and the BG1 playfield remains available.

Diagnostics separately count preflight, eligible, bound, phase, edge, mismatch,
and runtime lookup results. The modern PPU can bind at authentic 4:3 with zero
margins, while wide-raw and the legacy renderer remain native controls.
Character/palette/raster/priority ownership after the tile word is unchanged.

The provider-enabled 12-entry matrix
`runs/bg-hle-matrix-20260809-145341.json` bound all 19,522 eligible layer-frames,
performed 18,216,295 zero-mismatch/zero-outside preflight checks, and issued
150,579,968 successful provider fetches. All 204 framebuffer, emulated-state,
and PPU-snapshot artifacts are byte-identical to the earlier native matrix.
Fresh wide mixed/cyclic replays and the gf-2200 nine-plane diorama gate are also
exact, including with the vertical ring repair disabled. At the 4096x1024 Aitos
world and maximum 496px span, release/headless median cost is 0.067 ms/emulated
frame over native, below the accepted 0.10 ms BH5 budget. BH7 later promoted
this validated path to default-on; native streamers and the ring stay active as
fallback and oracle.

BH6 closes the post-scanout policy seam. `ActRaiser_ApplyWidescreenPolicy`
publishes the resolved `ActionBgPlan` into a pending frame record before
scanout; the draw tail latches that value, the live side margins, and the
independent captured-padding flag only after the pixels have been produced.
`FrameSlot_Capture` value-copies all of it. This ordering is load-bearing:
`ActRaiser_RebindPpuOutputSurfaces` may reset live PPU margin/policy fields
before presentation, so reading or reverse-classifying `g_ppu` there would
describe the next bind rather than the captured frame.

The diorama consumer now calls `DioramaBgValidSpanPlan_Build` with BG2's exact
default edge and every authentic-row override band. It translates each band by
the captured vertical-extension origin and emits one skybox quad per distinct
horizontal extent. Mirror/repeat rows use the fixed budget only when the
latched capture-padding flag says the PPU synthesized that budget; raw/world
rows use asymmetric live margins, and clamp/transparent rows use the authentic
256 columns. Thus Bloodpool's mirror/repeat split remains full width while the
Death Heim hub's clamp/repeat split remains two independently mapped regions.
The removed `DioramaBg2MarginSource` could express neither distinction and no
longer exists in the runtime or `FrameSlot` ABI.

Normal action frames retain the canonical plan. Explicit 4:3, Wide Raw,
`AR_WS_ONLYBG`, and `AR_WS_CLAMP` changes are projected at their producer site;
non-action scenes start from a native/raw plan and receive the exact final
clamp/mirror/repeat projection. That small inverse adapter is pure and tested;
it preserves source/world metadata, rejects conflicting or malformed policy,
and never reads PPU state. Focused live matrices cover Bloodpool, Aitos,
Northwall, every raw Death Heim room, and both deliberately native-only Death
Heim endpoints; see `docs/bg-hle-census.md` BH6.

BH7 makes the provider the ordinary path: unset, empty, or nonzero
`AR_ACTION_BG_HLE` enables it, while exact `AR_ACTION_BG_HLE=0` preserves the
native A/B. Five paired 12-entry matrices cover 4:3, Wide Full, Wide Raw, and
diorama vertical extension 0/32. Every authentic center and state/PPU artifact
is exact. The sole full-frame delta is an intended 30-pixel correction in the
synthetic left margin of Wide Full `0301`: independent BG2 camera/bounds reject
a wrapped native-ring sky column. The artifact comparator's
`authentic-center` policy still requires exact state and centered 256 pixels and
reports every accepted margin delta.

Long natural Fillmore replays cover Full, Raw, and diorama-32 through game frame
9425; a continuous Death Heim route covers hub/rematch transitions through
`0706`; provider preflight and runtime lookups report zero unexpected defect.
Same-frame redraw/rebind/reset, savestate load, fresh restart, and live Wide
Full-to-4:3 geometry change are covered. A non-headless Cocoa diorama-32 A/B
also matches all 14 ordinary-compositor artifacts. Debug/release builds and all
41 tests pass. See `docs/bg-hle-census.md` BH7 for manifests, counters, and the
remaining historical natural-transition evidence gaps.

`ActRaiser_FullSnapshot` also writes `.ppu.json` beside WRAM/VRAM/CGRAM/OAM.
This pins the BGSC geometry, character bases, enables, scroll, window and color
math state that a binary-memory-only snapshot used to leave implicit.
`tools/bg_hle_census.py` consumes both new and legacy snapshots, but marks old
captures' PPU eligibility unknown rather than substituting assumed registers.
`tools/bg_hle_matrix.py` builds on that format with a generated flat settings
fixture and the verified non-action warp seam. The 2026-08-09 region `$01-$06`
ordinary-entry sweep passed 12/12 targets: 19,072,823 runtime comparisons and
44,779 offline snapshot checks with zero mismatch after correcting the census
to PPU scanlines `1..224`, plus twelve distinct 256x224 framebuffers inspected
as a 4x3 contact sheet. The historical manifest's embedded 43,999 total used
the former `0..223` offline interval; its captured snapshots and runtime
comparisons are unchanged. All twelve BG1 layers were eligible at entry; BG2
split into six eligible layers, four explicit 32x32 decorative/native layers,
and two disabled samples. See
`docs/bg-hle-census.md` for the table and the still-open special-room gate.

The first special-room sweep adds an important boundary. Death Heim
`$0702-$0707` has eligible BG1/native-32x32 BG2 and passes 1,032,404 more
runtime comparisons; hub `$0701` and final `$0708` are deliberately native
32x32 scenes. A subsequent native route followed the real hub/victory loaders
through `0701 -> 0702 -> 0703 -> 0701 -> 0704 -> 0705 -> 0701 -> 0706`; eight
source activations and 6,646,861 in-world comparisons all matched. Its 364
finite exits are one explained `0705` BG2 frame: that decorative world is only
256px wide while camera X is 104, so the authentic viewport begins beyond tile
X 31. This is policy input for isolated repeat/clamp, not a decoder mismatch.
The `0707`/`0708`/ending tail remains open. Northwall `$0608` is a rejected
shortcut: its BG1 tile words match the ring while live CHR renders as patterned
garbage before the room self-exits. Therefore BH2 tile-word parity cannot stand
in for BH1 CHR residency or BH5 pixel/priority parity.

The decoder is intentionally scheduled at the authentic streamer's tile
cadence, not at scanout cadence. A host-only key contains the action room,
current margins, each camera rounded to its 16px column (vertical position to
its 256px map page), dimensions, and every layer descriptor marshalled into
`$B825`. An unchanged key means all required tilemap words already exist in
VRAM, so the draw skips the transaction. A rejected/partial build is never
cached. This matters for two-wide-layer rooms: `runs/20260712-202151/` proved
that invoking every BG1 and BG2 margin decoder every rendered frame can consume
the presentation budget even though game logic itself remains inexpensive.

### 13.3 Narrow-layer presentation padding (2026-07-12)

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
performs no PPU VRAM writes. Narrow action BG2 still selects its audited edge
strategy by default, but the later per-layer extent plan may independently cap
unique reflected art to the authentic viewport. Cyclic backdrops retain the
available full-canvas extent. The original clamp remains the same-binary
fidelity/fallback path.

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
animation remains automatic. Bloodpool acts 1 and 2 are now both mixed: their
upper moon/cloud family keeps the mirror edge classification, while BG2 tile
row 17 downward (`y=136-223`) cyclically repeats the live water scanline. Live
authoring establishes asymmetric `76/100` upper limits for `0201`, whose water
remains available, and a `68/68` whole-backdrop limit for `0202`, whose Repeat
band inherits that limit. This allows reflection only through each room's
known-good interval before unique landmarks repeat. Neither padding mode reads
the stale offscreen
tilemap half or mutates emulated state.

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
fog. The renderer supports up to four non-overlapping fill/motion bands per
layer. It first renders the authentic scanline in isolation, then applies the
selected Clamp/Mirror/Repeat fill into both margins while preserving
transparency, priority, live per-line scroll/HDMA, character animation, and
color math. For `0701`, the full symmetric canvas is enabled,
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
`snap_01_gf14676` already has boss-rush progress `$0347=$07` but current-song id
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
  does `$F64C-$F650` select song id `$0334=3`.

The policy now requires `$0347>=7` and observes the live BGSC page bases
`$64/$74`; song id `$0334>=3` remains a settled-state fallback. It keeps BG1
clamped and replaces the lower repeat band with whole-BG2 reflection
immediately when the sky pages become active. The current extent catalogue
then independently bounds that unique ending backdrop to the authentic
viewport. Edge selection and extent are separate: the former still describes
how the captured sky joins if a future canonical cap permits it. Direct testing
on 2026-07-14 confirmed that the page handoff occurs invisibly during the black
frame; a new natural-tail pixel fixture remains desirable for the later extent
decision.

Death Heim raw maps `$02-$07` (`0702-0707`) select the narrow-parallax repeat
policy. Capture
`runs/20260714-173750/snapshots/snap_00_gf4875` records `$18=$07`, `$19=$04`,
and BG2 logical width `$32=$0100`; the policy log showed `mirror=02`. Direct
observation found the padded mountain/parallax image moving opposite the
authentic center at the 256px boundaries. The same effect was then directly
reported on maps `$05-$07`. Maps `$02` and `$03` are provisionally
classified with that background family so boss-rush transitions cannot restore
reflection. The full `$02-$07` range therefore selects the same
isolated-scanline cyclic repeat as Aitos and Northwall. Direct 2026-08-10
matrices now cover all six rooms in Wide Full and Diorama-32 with zero provider
mismatch; the Wide Full artifacts are 102/102 byte-exact to their frozen
baseline.

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
performance. The role catalogue identifies BG1 as a primary `scene`, not a
playfield, so it can anchor Diorama's vertical raster without owning finite
world bounds. Its special classifier must also reset both planes to available
extents: the 2026-08-10 cross-mode gate caught a generic narrow-BG2 `0/0` cap
surviving the raw-wrap override and emptying the side starfield. Wide Full and
Diorama-32 are again 17/17 artifacts exact to their accepted `0708` controls.

### Per-layer role and extent seam (2026-08-10)

`ActionBgPlan` now carries two orthogonal facts for each action plane: semantic
role (`playfield`, `scene`, `backdrop`) and presentation extent. The unique
finite-world playfield owns horizontal canvas clamping; the primary playfield
or special scene anchors the vertical Diorama capture; a backdrop never gains
either responsibility merely from being BG1 or BG2. Native/non-action plans
remain unclassified and fail closed.

Horizontal extents are available or fixed independently on the left and right;
vertical extents do the same for top and bottom. Authentic pixels cannot be
removed. Sorted half-open authentic-row bands may inherit, remove or replace a
layer's horizontal cap. The producer resolves this once, applies it to PPU
scanout, latches the same immutable value through `FrameSlot`, and lets Diorama
build one UV span per distinct row policy. The presenter never reverses live
PPU masks or reads `g_ppu` after scanout.

The canonical Fillmore `0101` policy keeps BG1 fully available while limiting
its finite-world BG2 backdrop to 128px of additional canvas on each side. The
source remains WorldMap and its edge remains LiveWorld; only the presentation
extent is capped. The canonical Bloodpool composition keeps BG1 as the wide
playable platform layer and rows `136..224` as repeating water. BG2's unique
upper moon/cloud family is independently capped: `0201` uses the live-tuned
fixed `76/100` interval from `runs/20260810-122509` and leaves its water
available, while `0202` uses `68/68` for both its upper family and inherited
Repeat band. Bloodpool `0206` and `0207` independently limit their unbanded
Mirror BG2 planes to `68/68` and `92/92`, respectively, while leaving BG1
available. The initial all-`0/0` baseline accepted all 204
Wide Full artifacts against the pre-policy census—4,074 changed pixels, all in
the two Bloodpool side margins, with every authentic center and state/PPU/VRAM
artifact exact—and the complete twelve ordinary entries passed 4:3, Wide Raw
and Diorama-32. The later `0201` tuning supersedes only that room's synthetic
upper margin. Settings -> Layers -> BG Extents exposes a non-persistent sparse
draft, A/B, colored guides and a
normalized log dump without creating a second canonical policy store. Each BG
can author up to four non-overlapping bands, add/delete them in the overlay,
choose screen or world anchoring, edit the half-open row bounds, and select fill,
motion and extent independently. `motion=fill` is the behavior-compatible
legacy phase: reflecting the rendered scanline also reflects apparent movement.
`motion=normal` compensates the mirrored sample by the live horizontal-scroll
phase so clouds/water move in the authentic direction; Repeat is unchanged.
The PPU stores the compiled policy per authentic row, so multiple Mirror,
Repeat, Clamp, world/raw, or transparent families can share one BG without a
single-band runtime special case. Its per-BG `ignore side bounds` shortcut
resolves the layer and all of its row bands to the available horizontal extent
for the A/B. The independent `ignore
vertical bounds` shortcut resolves that layer's top/bottom extent to available.
Both retain stored caps for exact restoration and cannot outgrow the shared
canvas, finite world, or source/edge availability.

Mixed screen/world tables carry one additional invariant: adjacent intervals
must remain ordered across the layer's complete native camera travel, not only
at the frame where an edit is made. The tuner rejects a future crossing before
publishing it. Draft application remains atomic, and the runtime compiles the
unchanged canonical room plan if a later room-state transition makes stored
authoring data stale. This keeps developer tooling unable to take HLE offline.
The scene inspector obtains fill, motion, band precedence and reflected source
X from the PPU's shared inline resolver, so diagnostics cannot drift from the
scanline renderer's sampling formula.

Kasandora `0301` and natural-transition room `0302` use the same immutable
handoff for a content-anchored hybrid. Their 512x512 BG2 maps place sparse
cloud/sky art above world Y=256 and cyclic dunes at and below it. Because BG2
vertical parallax moves that source boundary through the viewport, the planner
stores the dune family once as the world band `256..512`; the common row
resolver projects it as `256 - cameraY - 1` each frame. Cloud rows use Mirror
and dune rows use Repeat. The captures in `runs/20260810-130310` pin row
82 at camera 173 in `0301` and row 93 at camera 162 in `0302`. The synthesized
BG2 source is the authentic viewport, avoiding the provider-invalid fallback
that occurs when a live WorldMap plan is manually changed to Mirror/Repeat;
the mirrored sky is capped to 128px per side while the repeat-safe dune band
retains an available extent. BG1 remains fully provider-backed and owns the
playable canvas.

Vertical extension makes one additional row-policy rule load-bearing: a band
with `y0=0` or `y1=224` owns the synthetic margin adjacent to that authentic
boundary. Internal bands remain bounded and outside rows at an unrelated edge
still use the layer default. This is derived from the existing half-open band
bounds rather than stored as another override. Both the immutable
`ActionBgPlan` row resolver and the mechanical PPU projection apply the rule,
including the band's fill, motion and horizontal extent. Existing baked bands
remain fill-relative by default, so this generalization does not retune or
invalidate the accepted Fillmore/Bloodpool/Death Heim policies.

Bloodpool run `runs/20260810-114943/snapshots/snap_00_gf8076` exposed the
omission: BG2 was correctly classified as whole-layer Mirror with a
`136..224` Repeat/Available water band, but scanlines below row 223 reverted to
Mirror plus the upper art's fixed extent. The authentic water and its synthetic
bottom continuation therefore moved in opposite apparent directions. PPU band
lookup now translates its internal 1-based line to the authored 0-based row
and clamps synthetic lines to row 0 or 223. Thus Bloodpool water and Death Heim
fog retain cyclic continuation below the screen, while their unique upper art
remains bounded. A real-PPU fixture uses different colors at the two authentic
edges to prove the bottom margin samples the opposite edge (Repeat), not the
near edge (Mirror), and that the Available band extent survives there.

This boundary-band rule is complementary to the previously audited moving
cloud/snow policy. Aitos `0401-0403`, Northwall `0601-0605` and `0608`, and
Death Heim `0702-0707` classify the complete narrow BG2 as cyclic Repeat, so
every authentic and synthetic top/bottom row already preserves its motion
direction; they do not need a `y0=0` band. `0708` remains the intentional
native RawWrap exception for its two-plane raster scene. The planner test lists
every member rather than only range endpoints, and the real-PPU fixture now
proves same-direction cyclic sampling on a synthetic top row as well as the
Bloodpool bottom band.

### 13.4 Action Diorama main/subscreen colour math (2026-08-11)

The SNES main screen and subscreen are not alternative complete views. They are
two independently priority-resolved inputs to the final pixel operation. The
game can place a useful visual source only on the subscreen and rely on colour
math to make it visible. Any host feature that extracts layers must therefore
treat `TM | TS` as source eligibility; using `TM` alone is a category error.

Marahna action mode is the measured counterexample that established this rule.
Both `runs/20260811-115422/snapshots/snap_00_gf2331` (`0501`) and
`runs/20260811-120243/snapshots/snap_00_gf9728` (`0502`) record the same state:

| Register | Value | Meaning in the measured frame |
| --- | --- | --- |
| `$212C` `TM` | `$06` | BG2 and BG3 participate in the main-screen priority resolve |
| `$212D` `TS` | `$11` | BG1 and OBJ participate only in the subscreen priority resolve |
| `$212E/$212F` `TMW/TSW` | `$06/$11` | matching main/sub window designation; the live PPU still evaluates it per scanline |
| `$2130` `CGWSEL` | `$02` | use the resolved subscreen pixel as the second colour operand; no direct-colour or colour-window mode |
| `$2131` `CGADSUB` | `$03` | full addition, enabled for main winners BG1 and BG2; no half or subtract bit |

The native result is therefore not “show BG2 instead of BG1.” For each pixel,
the PPU first resolves the highest-priority main winner and the highest-priority
subscreen winner. If the main winner is selected by `CGADSUB` and the colour
window permits math, it saturating-adds the subscreen colour in SNES 5-bit
component space. In this scene BG2 is the ordinary math-bearing main world,
BG1/OBJ supply the resolved subscreen addend, and non-math BG3 remains a main
foreground/HUD winner. That is why main-only Diorama capture lost the playable
level and sprites, while merely capturing BG1/OBJ as ordinary opaque planes
still lost the water lighting/detail.

The separated capture reproduces the measured full-add state with this exact
contract:

1. `ActRaiserDrawPpuFrame` gates BG/OBJ capture on
   `screenEnabled[0] | screenEnabled[1]`. The PPU exports the main rendering of
   a source when present there and otherwise its subscreen rendering. This
   choice happens during scanout, after scanline HDMA can update TM/TS.
2. `DioramaCaptureBlend_FullAddSubscreenSources` admits only
   `CGWSEL == $02`, full non-subtract math, disjoint main/sub visual-source
   masks, and at least one math-enabled main source. Overlapping ownership,
   direct colour, half/subtract variants, or colour-window modes fail closed
   rather than being approximated.
3. For an admitted source, the PPU resolves the authentic main and subscreen
   priority winners and exports a sparse addend only where that source wins TS
   and the main winner actually enables colour math. BG3 is excluded from the
   world resolve and reinserted later so a relocated/flat HUD cannot punch
   glyph-shaped holes in the addend. The separately relocated HUD OAM range is
   likewise omitted from the full-add OBJ scratch while remaining present in
   the ordinary OBJ capture.
4. `FrameSlot.diorama_plane_additive_mask` carries the immutable result beside
   the captured frame. Present intersects it with uploaded content and the
   Diorama compositor draws three passes: ordinary main-world planes, sparse TS
   planes with saturated additive blending, then BG3. Present never reads live
   PPU state.

This is one member of a small, explicit colour-math support table rather than a
claim that arbitrary SNES arithmetic maps to host alpha:

| Authored form | Separated-plane representation |
| --- | --- |
| Subscreen half-add on an eligible BG | source alpha `$80`; a layer also on TS is identity and remains opaque |
| OBJ colour math | existing per-palette-group `$80` alpha capture |
| Disjoint subscreen full-add | sparse resolved-TS plane plus saturated additive pass |
| Full fixed-colour BG subtraction | baked into the isolated plane in native 5-bit component space before brightness expansion |
| Overlapping main/sub ownership, general subtract/half-subtract, direct colour, or unsupported colour-window math | fail closed; do not infer a blend from layer bits alone |

The background provider has a related but independent Marahna rule. A 512px
BG2 driven by the same full camera X as a wider BG1 is one authored horizontal
cycle, not a finite backdrop. The classifier requires Marahna, BG2 width 512,
a wider BG1, equal camera X, and a WorldMap source; it never names subsection
`$19`. All 924 authentic BG2 ring words in `0501` gf2331 and all 957 in `0502`
gf9728 match the decoder at X modulo 64 tiles. `wrap_world_x` applies that same
modulo in lookup and native-ring preflight, so every qualifying subsection keeps
the HLE provider when the camera crosses the encoded period.

These are host/PPU presentation seams only. The work discovered no new WRAM
field, ROM routine/table, or recompiled ROM symbol.

## 13b. Simulation-town 3D presentation (pointer, 2026-07-22)

The enhanced town renderer is designed in `ar-recomp-sim-rendering-plan.md`
rather than here, because it is a presentation layer built on top of §11's
Mode-1 pipeline rather than a change to it. What matters when reading this
document: town simulation is **ordinary PPU Mode 1, not Mode 7**, so the
projection is a host-side transform of captured Mode-1 planes plus a semantic
OBJ atlas — the PPU/priority behaviour described above is unchanged, and the
feature-off path is byte-identical.

One deliberate deviation is worth knowing here, because §11 above would
otherwise lead you to expect it cannot happen: in the **projected** profile,
world billboards are painted back-to-front by map row *within* each hardware
priority band, not in pure OAM order. The bands still own the coarse layering
and OAM order remains the tiebreak. On the flat screen OAM order alone is
correct because every sprite shares one plane; once the map is projected, two
actors on different rows really are at different distances. The flat and
feature-off paths are untouched.

- Design, phases, and checkpoint results: `ar-recomp-sim-rendering-plan.md`
- Object height/anchor policy and composition identities:
  `docs/sim-object-catalog.md`
- Host seams (classification, height easing, shadow mask, shadow blur, rim
  light, billboard depth order, tuning handoff, picker build switch, D1 trace):
  `docs/SEAMS.md` "Sim 3D presentation seams"
- Player-facing stage toggles and tuning dials: `docs/settings-system.md`
  "Simulation 3D"
- Ground extension beyond the captured window (world-map underlay, full-town
  canvas): §13c below

## 13c. Simulation-town ground extension (2026-07-22)

The 3D town view draws a finite ground quad: the captured screen window,
projected. Outside it there was nothing, so the frame ended in flat backdrop
colour. Two layers now extend the ground past that window, both derived from
data the game already keeps resident.

**The world map underlay.** ActRaiser's Mode-7 world map is three flat
uncompressed ROM blobs — a 128x128 byte tilemap at `$06:B341`, 256 8bpp tiles
at `$0E:8000`, and a 256-entry palette at `$1C:BF93` (verified byte-for-byte
against a live capture: tilemap 16172/16384 and chr 16346/16384 identical, the
deltas being exactly the runtime edits; palette 512/512 identical). One
world-map tile covers exactly one town map cell, so the world map is the town
at **half linear resolution** and each town is a 32x32-tile window of it.
Established by correlating each town's terrain against the world map over every
scale and offset: 1:1 wins with a clean unimodal peak, and the per-town origins
are the world cathedral icon minus the town's own cathedral cell. Every origin
lands on a multiple of 16 and the six windows tile the map disjointly — no
other assignment of towns to icons has that property, which is what pins the
table. Bloodpool and Fillmore share an edge, as do Aitos and Kasandora, so
standing at one town's border shows the neighbour's real territory.

The underlay tracks current development without observing `$7E:C000`. That
range is shared scratch: action stages durably overwrite rows 0-79 and town
frames reuse rows 0-7, so map identity cannot make a stale buffer trustworthy.
The host instead owns the build. `$02:B475` cleanly separates into base
copy/decompress, `JSL $02:865C`, then a `$2118` VRAM upload. The host already
has the byte-identical ROM base and now implements the dynamic middle phase as
the pure `SimWorldMap_ComposeDeveloped`: ordinary cells translate through
`$02:8000`, `$E3-$EF` expand through `$02:8100`, and six quadrant-paged town
maps land at the `$02:87A5` destinations. It rebuilds on town or world-navigation
entry and on input changes, touching no emulator-visible state. The old bounded
ROM call remains only behind `AR_WORLDMAP_HLE_COMPARE=1`; fixture and live
differential checks match all 16,384 bytes.

**The full-town canvas.** The world map is half resolution, so the town's own
off-screen territory deserves better. The game keeps the whole town's BG1
tilemap resident: `$03:9C43` writes each cell's 2x2 tile block at
`$7F:0000 + quadrant*2048 + (cellY & 15)*128 + (cellX & 15)*4`, four words at
`+$00/+$02/+$40/+$42`, so the row stride is 32 tiles, the quadrant stride is
32x32 tiles, and the four quadrant pages are a 64x64-tile — 512x512 pixel — map
of the entire town. That paging is why a row-major read of the range looks like
an unrelated layer. The canvas renders it each frame from that tilemap plus
character data from VRAM `$0000` and CGRAM, re-rendering only when one of those
actually changes.

An earlier version accumulated captured frames into the same buffer instead.
It was replaced: accumulation could only ever show ground the camera had
already passed over, and only as it looked at the time, so construction
happening off-screen stayed invisible until the camera returned.

**Draw order** is `atmospheric backdrop -> world underlay -> town canvas ->
town ground quad -> shadows/objects/rim`. The renderer is painter-ordered with
no depth buffer, so there is no z-fighting; the town's own opaque ground quad
covers its visible window and the extension supplies everything beyond. The
canvas is opaque throughout, so it never punches a hole in the underlay
beneath it.

Sprites are extended separately and only horizontally — see
`docs/SEAMS.md` "Sim-mode OAM emit margin" for why the vertical direction
cannot follow.


## 13d. Simulation-town cull cues (2026-07-22)

The ground extension created a problem it could not solve. Ground now reaches
far past the sprite-drawable window, but OAM cannot place an actor out there,
so the extended ground is permanently empty — and worse, an actor walking
toward the edge simply stops being drawn while the ground under it is still
bright, sharp and plainly visible. The cues below exist to make that boundary
legible. They are presentation only; none of them changes what the emitter
culls, which is fixed by hardware (see §13c and ledger §25).

### The invariant

**If a record is being taken away by the sprite window, something must be over
it.** Not "the far field is mostly covered" — per record. That distinction is
the whole design. An earlier attempt drew a noise field over the far ground and
tried to make it dense enough that gaps were unlikely; the gaps were what the
player noticed, because a gap is exactly where a sprite vanishes over clear
ground. Coverage by probability cannot express a per-record guarantee.

### One boundary, every cue

Everything is driven by one pure function, `Sim3D_CullProximity`, evaluated
once per vertex and shared by every term that reads it — two terms describing
the same boundary must not be able to disagree about where it is. It is stated
in the emitter's own biased coordinates so the cull predicate and the things that
explain it are the same arithmetic rather than two derivations that agree by
inspection. `src/actraiser_widescreen_sprites.c` carries `_Static_assert`s
tying the mirrored window constants to the emitter's.

It returns 0 well inside the window and 1 at the edge, and every cue reads
it:

1. **Ground fade, and ground dimming.** Two independent terms on the same
   ramp, and they have to be independent. The *fade* is structural — the town
   canvas is drawn with per-vertex alpha `1 - proximity * fade`, so
   out-of-range town ground cross-fades into the world map underlay beneath
   it. The *dim* is photometric: a per-vertex brightness multiplier applied to
   every ground draw, canvas and underlay alike.

   They were one control until the sky landed. The underlay's own distance
   haze blends toward `separated_backdrop_argb`, which used to be flat black
   and is now a blue gradient, so turning the fade up washed the far field
   toward grey-blue instead of darkening it. Darkness has to multiply into the
   colour or the only way to get a dark far field is to pick a dark sky.

   The blurred underlay pass takes the dim but **not** the fade: it is the
   layer being revealed, so fading it would thin the very thing the canvas
   hands over to and the far field would go transparent rather than dark.

   The fade's own target brightness is deliberately not a number to be tuned
   into agreement — it is whatever the underlay happens to be, and
   cross-fading reaches it by construction. The dim is applied *after* that
   handover, to both layers equally, which is why it does not reintroduce the
   problem the very first attempt had: a separate darkening overlay that hazed
   the underlay a second time on top of `underlay_haze_pct` and took
   everything outside the town to near black.
2. **Focus falloff.** A 4x box-downsample of the same world-map bake, upscaled
   with linear filtering, is drawn first at the haze alpha; the sharp copy goes
   over it at `1 - proximity * defocus`. Distance haze and defocus therefore
   arrive on one ramp instead of as two boundaries the eye must reconcile.
   Blur says "too far to resolve" in a way dimming cannot.
3. **Cloud shroud.** Three noise banks at different scales, drifting at
   different rates so the field churns rather than sliding across as one
   image, lifted above the ground plane so a bank passes over a tree instead of
   lying across it.
4. **Per-record cover.** The guarantee itself. See below.

### Cull evidence

`SimSourceRecord` carries `anchor_x/anchor_y` — the emitter's own biased
composition origin, handed over rather than re-derived — plus `clipped_parts`
and `clip_reason`. The emitter reports at the two branches that previously just
parked an OAM slot at `$E000` and dropped the fact.

`Sim3D_SourceCullCover` gates eligibility. Only the sprite window may create
cover: a record that emitted nothing *and* was never clipped is the game
declining to draw it, and covering that would assert something false about the
world. Fixed-tier furniture is screen space and never qualifies.

### Two questions, not one

Cover timing and cover placement are separate queries, and conflating them is
what makes a lifted actor look like it vanished early.

- **When** cover arrives is a question about the emitter. It culls on the
  record's own y — the ROM knows nothing about virtual height — so
  `Sim3D_SourceCullCover` uses the unlifted anchor.
- **Where** cover goes is a question about the renderer.
  `Sim3D_SourceDrawLift` runs the same pure classifier the object pass uses,
  from the source record's own fields. It has to be reachable that way: a
  record the window took away entirely emitted no parts and so has no
  `objects[]` entry to read a height from, and that is precisely the record
  whose placement matters most.

### The lift inset

The lit region is painted on the ground, so it can only ever express the
height-zero boundary. It promises "actors can be here", and for flying actors
that promise is wrong by the lift amount along the bottom edge. The window's
bottom is therefore inset by `Sim3D_MaxDrawLift` — the classifier's ceiling,
not a measurement over the live record list, because an inset that tracked
whatever happens to be flying would drift the ground fade up and down while
nothing on screen moved.

The top edge is deliberately not inset: lift is toward negative y, so a record
approaching the top leaves the lit region *before* it culls, which is already
the safe direction.

Lifting the camera instead does not work and the reasoning is worth keeping.
The emitter reaches `base_y` through `dp $96`, which is one value for every
record, so biasing it moves grounded records too; it trades the bottom edge for
the top; and it cannot extend real OAM's authentic vertical window. The exact
synthetic channel can extend drawable reach, but only `sim_view_range` moves it
together with `ActRaiser_SimProjectileVisible`: that predicate's false result
destroys the record, making range gameplay rather than camera presentation.

### Mesh density is a correctness constraint

`kSimUnderlayColumns/Rows` was 24x18, chosen for affine UV correctness alone.
Once the fade began being *sampled* at those vertices, that number became
wrong: over an extent spanning source +/- 512px it is ~60px per cell, coarser
than the corner radius, so a rounded window was interpolated back into a
straight-edged box and the smoothstep feather was flattened with it. It is now
64x48. **The mesh must be finer than the smallest feature the fade is meant to
show** — a stricter constraint than the perspective one that set the old value.

### Draw order

Extending §13c: `backdrop -> world underlay (blurred, then sharp) -> town
canvas (faded) -> town ground quad -> shadows/objects/rim -> cloud shroud ->
menu planes`.

The menu planes are held back from the painter-order loop and drawn last, in
rank order among themselves so the box frame still composites under its own
text. A sprite drifting under the shroud is the effect doing its job; a cloud
drifting across a menu the player is reading is the effect damaging something
that is not part of the world at all.

The menu is three things on three kinds of layer, and it took two attempts to
get all of them:

- **Text** — `Bg3Low`/`Bg3High`. §11 records the ownership from a capture.
- **Box frame and panel fill** — `Bg2High`. Deferring BG3 alone lifted the text
  and left the panel under the shroud, so clouds showed inside the windows.
- **Icons and cursors** — **fixed-tier OBJ**, drawn through the billboard path
  at their own priority band. `Obj3` ranks *above* `Bg2High`, so deferring the
  panel by itself put its opaque fill over them and the menu rendered empty.

Fixed-tier OBJ are therefore deferred **by tier, not by plane**: they share the
OBJ ranks with world billboards that must stay under the shroud, so
`DrawSimObjectPriority` takes a `SimObjectTierFilter` and each band is drawn
twice — world-tier in the painter-order loop, fixed-tier in the deferred group.
That split is also why rim light is world-tier only, which it should always
have been.

The deferred group walks the full hardware rank rather than just the menu
planes, so order *within* the group is unchanged and only the group's depth
relative to the world moves. `Bg2Low` is deliberately excluded: the plan's
presentation order places it behind the projected ground, where it is a
background layer rather than UI, and promoting it would put whatever a town
keeps there on top of everything.

Lifting BG3 wholesale is safe because the town HUD's own BG3 pixels have
already been removed from the profile by the `sim3d.c` overlay handoff and are
composited separately afterward, so what remains on that layer in a town is
menu furniture.

### Diagnostics

`AR_SIMCULLMARK=1` draws one marker per record earning cover, over the shroud —
green while approaching the edge, red once the emitter is actually clipping its
parts. A red marker with no cover under it is exactly the artifact this section
exists to remove.



## 13e. Colour math the D2 gate accepts (2026-07-22)

The separated capture reproduces the frame from individual layers, so any PPU
colour math has to be reproduced too or the byte-exact fidelity gate reports a
mismatch on the frame (since ledger §31 it reports rather than drops — the
checkpoints are where that fails the build). Only states shown to be
reproducible are accepted, and there are currently three:

1. **No-op** — `cgwsel == 0`, `fixedColor == 0`, no half/subtract. The PPU's
   own fast path proves nothing happens.
2. **Targeted-miracle half-add** — BG1 on the subscreen, half-added beneath OBJ
   palettes 4-7. Stays a *compositing policy* (`object_half_add`) because it
   combines two planes.
3. **Fixed-colour add** — the sun miracle.

### The sun miracle

Diagnosed from the transition log, which now prints the registers when this is
the rejection reason: `cgwsel=$00 cgadsub=$01 fixed=$0001 screen=$15/$00`,
brightness 15 — a fixed-colour add, ramping from 1, onto **BG1 alone**. It
trips the moment the ramp leaves zero, which is why the effect starts and the
view drops in the same frame.

`cgwsel == 0` is what makes it reproducible: fixed colour rather than subscreen
as the math source, math enabled over the whole screen, no main-screen-black
region, no direct colour — so there is no window geometry to recover.

Unlike the half-add this is **baked into the captured plane pixels** rather
than made a compositing policy. A fixed-colour add is a property of one layer,
and the authentic rebuild, the flat recomposition and the projected textures
all read the same buffers, so one application serves all three and the D2 gate
verifies it against real hardware output.

### Why it could not be done the obvious way

The PPU adds in **5-bit component space** and only then maps through
`brightnessMult` to 8 bits. Overlay surfaces receive the 8-bit result, so
adding the expanded fixed colour to the expanded pixel is a different
operation — it differs on **168 of the 1024** (component, add) pairs, and the
gate is byte-exact, so the naive version would have produced exactly today's
behaviour plus code.

The reproduction inverts `brightnessMult` to recover the 5-bit component, adds
with the hardware's clamp at 31, and maps forward again. That is exact on all
1024 pairs. Inversion needs the table to be injective; it is, comfortably below
full brightness, but only brightness 15 has been checked against hardware
output so the gate admits only that. A miracle running under a screen fade is a
second effect layered on this one and wants its own evidence.

### Failure mode

Benign by construction. If the reproduction is ever wrong the D2 gate sees a
pixel mismatch and reports it, and the affected pixels render with the wrong
colour-math result for those frames — a bounded, local error rather than a
correctness risk. Since ledger §31 the gate no longer drops the frame to the
authentic view; the checkpoints, which assert zero mismatching pixels, are
where a wrong reproduction is caught.

## 13f. Simulation-town sky (2026-07-22)

### The horizon that is not there

`kSimFeature_Backdrop` replaces the flat clear behind the finite ground with a
vertical gradient between two authored sky colours, mixed *from* the scene's
own `separated_backdrop_argb` by the strength setting. Sky brightens toward the
horizon and deepens overhead, which is the one property of real sky that
survives being reduced to two colours. Strength 0 reproduces the previous flat
fill exactly, which is what makes D5a-2's "only pixels behind the finite
ground change" checkable against A8 rather than against a differently-coloured
screen.

The first version *derived* both endpoints from the backdrop — lifting toward
white, dropping toward black — on the reasoning that this preserves whatever
hue the game chose and cannot clash with a town palette. That reasoning holds
only where there is a hue to preserve: a simulation town's backdrop is black,
and black lifted toward white is grey, so the sky rendered greyscale. Mixing
toward an authored blue is well-defined for any backdrop, and a town that does
choose a coloured one still tints the result rather than being overruled.

**The ground-plane horizon is never on screen.** Across the whole settable
pitch range (-700..700 mrad) the vanishing line lands 544 to 5619 destination
pixels outside a 224-row viewport, and a pitch of exactly zero has no horizon
at all. What reads as sky in frame is where the ground *data* runs out, not
where the ground plane vanishes — in practice only the corners past the end of
the extended map, and only when fully zoomed out.

The sky is therefore graded around a **synthetic** horizon at
`backdrop_horizon_pct` of the viewport height (default 50%), with the gradient
completing at the top of the viewport so moving the anchor restretches it
rather than leaving a band of flat zenith above where it ran out. The real
horizon is used as the anchor only if it ever becomes visible — one comparison,
so widening the pitch range cannot silently produce sky below the horizon.

The synthetic anchor is honest about what it is: with no horizon line in frame
there is nothing for the eye to check it against, so its job is to look like
sky at those edges, not to agree with a vanishing point 1674 pixels off the top
of the screen. `Scene3D_GroundHorizonScreenY` remains the pure primitive,
solved as the limit of the projection rather than by projecting some "far
enough" point; the ground extension already reaches thousands of captured
pixels out, so any finite stand-in for infinity would need re-tuning whenever
the extent changed.

## 13g. Simulation-town camera (2026-07-22)

### Two modes, two poses

`sim3d_camera_mode` is Free or Dynamic, mutually exclusive, mirroring the
diorama split. **Each mode owns its own pose**: Free Cam keeps the
player-authored `sim3d_tilt_*`/`sim3d_distance_x100` that the right-drag edits
and that persists across a session; Dynamic Cam has dedicated
`sim3d_dyncam_baseline_*` settings, defaulting to the captured baseline, that
the reactive lean works around.

Two poses rather than one is the entire point. With a single shared pose,
switching to Dynamic would sway around wherever the last manual drag happened
to leave the camera, so the mode's look would depend on unrelated history.
The active pose is resolved once, on the game thread (`Sim3D_ActivePose`), and
published through `sim.projection_*` like any other tuning — two `Sim3DTuning`
sites read it, and a camera that differed between them would be a genuinely
confusing bug.

Consequences worth stating: the right-drag is inert in Dynamic Cam, because it
edits a pose the projection is not built from and a silent no-op is worse than
no response; "Reset camera" restores the pose of the mode in use rather than
always the free one; and a mode change **snaps** rather than eases, since
easing across it swings the camera between two unrelated poses and reads as a
knock instead of a switch.

### Reactive motion

Dynamic Cam leans the town camera toward the angel's direction of travel and
jolts it when the angel takes a hit — the same reactive camera the
action stages use in diorama mode, and deliberately the same construction: a
velocity lean eased toward on a **wall-clock** exponential, plus additive
impulses decaying on another. Both details were tuned against real failures
there and carry over unchanged. A fixed per-frame damping factor is twice as
stiff at 120Hz as at 60Hz; an impulse that replaces rather than stacks loses
back-to-back events.

Three things differ, because the mode differs:

- **The signal.** Action mode reads `PlayerVelocityX/Y`, which is an
  action-stage concept. The town reads the angel record's own `+$1A/+$1C`
  planar velocities. There is no jump and no ground here, so "vertical
  velocity" is simply the other axis of a planar drift, and pitch leans toward
  it exactly as yaw leans toward horizontal travel.
- **The magnitudes are smaller** (`kSimLeanYaw` 0.045 rad against the diorama's
  0.10). The action stages look at the player from the side, where a lean
  swings the whole scene across the screen. The town is viewed from near
  overhead, where the same angle mostly slides the ground under a camera that
  is already looking down, and very little of it is needed before the map
  appears to swim.
- **Town scoping.** Outside a town the angel record holds whatever the last
  action stage left there, so the capture reports a neutral camera and resets
  its edge state. The first town frame only seeds the previous HP: arriving
  with less health than the last town ended with is not a hit, and without
  that guard the camera jolts on entry.

Hit detection is an HP decrease rather than an invulnerability flag, matching
the correction made in action mode — the flag is set once hit-stun begins,
roughly ten frames after damage applies, whereas an HP decrease *is* the frame
damage applies.

The offsets are folded in **before** the view-projection matrix is built, so
every stage — ground, billboards, shadows, the cull boundary, the shroud —
sees one camera. Adjusting the matrix afterwards would leave object anchors on
the old one.

## 13h. World-navigation full-plane scene (2026-07-27)

Map `$09` reuses the owned developed world image but not the simulation-town
scene graph. `SimWorldNavigationScene_Build` publishes exactly one 1024x1024
texture serial and one four-corner plane over world-map tile coordinates
`[0,128) x [0,128)`. There is no captured town rectangle to extend, so this
path has no `kSimUnderlayMarginPixels`, full-town canvas, separated BG planes,
semantic town records, object atlas, sprite cull window, focus-falloff mesh, or
cloud shroud.

The camera is an affine top-down transform, not the town's perspective camera.
The captured signed 8.8 Mode-7 matrix maps screen deltas to texture-source
deltas:

```text
[source x - focus x]   1     [ A  B ] [screen x - 128]
[source y - focus y] = --- * [ C  D ] [screen y - 112]
                         256
```

Scene construction inverts that 2x2 matrix once on the game thread and carries
the resulting six-value source-to-authentic-screen affine map in the immutable
`SimFrameData`. This keeps steady movement, zoom, and the action-entry spin on
one exact camera model; the present path does not infer a transform from PPU
registers and does not read live WRAM.

The full map is mandatory content for this view. It is not conditional on the
town Ground projection or World map underlay stage toggles, and the independent
`AR_SIM3D_WORLD_NAV` master does not require `AR_SIM3D`. Compatible
lighting/weather/colour tuning is reused by two navigation-specific effect
gates: `AR_SIM3D_WORLD_NAV_LIGHTING` (on by default) and
`AR_SIM3D_WORLD_NAV_CLOUDS` (off by default). The latter reuses the town
renderer's procedural field over the complete world with no sprite-window
hole, cull cover, or underlay margin. Navigation uploads a padded 2x2 repeat
and samples each drifting bank with one affine quad, avoiding both
backend-dependent UV wrapping and geometry join seams. Light
direction/elevation, shadow darkness/softness, cloud density/altitude, and
drift remain immutable frame values. Cloud bodies use `$0316` as a camera
height axis: the default deck is above the camera at near `$0206`, crossed
smoothly during zoom, and visible at middle/far `$040A/$0562`. Ground shadows
remain visible below the deck.

The town atmospheric backdrop is also shared. Navigation uses its synthetic
horizon because an affine top-down camera has no perspective horizon, filling
every pixel exposed past the finite map during wide output and spin. `$0341`
provides the authoritative active location: `$01:B6CA` selects one of seven
256x256 source regions from ROM table `$01:B73C`, the same selection used for
the label/destination. A world-space mesh keeps that region fully sharp and
lit while blending the existing downsampled mip and backdrop haze over the
surrounding world. `$01:B6CA` clears `$0341` before scanning; when the Palace
is outside all borders, zero removes the clear-region cutout and the complete
world remains hazed.

A 2048x2048 high-fidelity world remains research. At that scale one world cell
can receive its native 16x16 town footprint and each 32x32 town can occupy a
512x512 window, but production must not switch until all six ROM tilesets,
palettes, metatile translations, animated/development cells, seams, and far
zoom mips can be reconstructed deterministically.

The game thread classifies navigation OAM separately from town records. Steady
navigation owns 20 packed priority-3 label/frame sprites followed by the
Palace's fixed-centre 3x3 grid; tile IDs and grid traversal change during
Palace animation, so position/attribute ownership is the invariant. An
all-hidden OAM table is the valid action-entry composition. The PPU-backed
capture rasterizes Palace and UI into separate immutable layers, and any other
layout, non-Mode-7 state, or forced blank selects authentic Mode 7. Partial
INIDISP brightness remains enhanced: presentation draws the full-intensity
backdrop, developed ground, colour/location haze, and weather, applies one
black master-fade overlay with exact 17/255 steps, then draws Palace/UI pixels
whose PPU rasterization already applied the same brightness. A gf380-451 replay
shows the view selected at brightness 0 before fade-in, retained through all
15 steps and the complete fade-out, and released only after the black endpoint.

## 13i. Vertical extend — widescreen's transpose (2026-08-03, symmetric 2026-08-10)

Diorama-only, default off (`diorama_vertical_extend`, 0..64 scanlines per
side). Renders finite world ABOVE AND BELOW the authentic 224-line viewport so
camera motion does not immediately discard useful platform art at the opposite
edge. The actual top and bottom counts resolve independently against the
primary layer's camera and world height.

### OAM bytes are not the ceiling

The hardware encoding explains why synthetic vertical rows need an exact
position channel:

| axis | OAM field | modulus | screen | free range |
|---|---|---|---|---|
| X | 9 bits | 512 | 256 | `[256,512)` — a whole screen, unambiguous |
| Y | 8 bits | 256 | 224 | `[224,256)` — 32 lines, **already means "above"** |

Vertically, a parked slot, a sprite below line 223 and a sprite above line 0 can
all encode to the same byte. `PpuSetObjExactPosition` carries the signed value
the action HLE emitter had before truncation. Margin scanlines therefore accept
only committed exact slots; authentic lines retain the normal OAM path for any
slot without one. OAM no longer limits presentation capacity. The current
`kPpuExtraTopBottom = 64` is a bounded host budget chosen to cover the measured
48px camera move in `runs/20260810-112529`, not a gameplay-camera range.

### Geometry

Row 0 of every captured surface is screen y = `-ws_extra_top`, the exact
transpose of column 0 meaning screen x = `-ws_extra`; total capture height is
`top + 224 + bottom`. `PpuOutputRow` is the one place that mapping lives; it
degenerates to the historic `y - 1` at zero margin, which is what keeps
authentic output bit-identical. Synthetic top rows use hold-first PPU state and
bottom rows use the symmetric hold-last state after authentic scanout.

The band contains real off-screen world rows only while the individual layer
has world above its own camera. That qualification is load-bearing: the capture
height follows BG1, but BG1 and BG2 have independent cameras and dimensions.
The PPU tilemap is a cyclic address space, not a declaration that the bottom of
a bounded layer is spatially adjacent to its top. Negative scanlines do not
exist on SNES hardware, so blindly applying the hardware wrap to them can expose
resident tilemap data from the opposite world edge.

Fillmore act 2 is the measured counterexample that corrected the first version
of this documentation. At gf 2200, BG1 is at `$24=$05E8` in a `$30=$0700`-high
world and legitimately owns all 32 requested rows above the viewport. BG2 is
independently at `$28=$0000` with `$34=$0200` height and owns **zero**. The old
`PpuBgTilemapRow` path mapped margin line -31 to 10-bit row 993; a 64-row-high
tilemap selects physical pixel row 481, so lines -31..-1 read BG2's bottom
rows 481..511 before authentic line 1 restarts at its transparent top. Direct
snapshot rendering proves BG1 is grey there and the bottom of BG2 contains the
red structures. The priority-split capture locates them on BG2-high, whose
half-add flag makes the grey BG1 bricks beneath look red. The repro has
`HDMAEN=$00`; neither HDMA nor HUD palette state creates the cutoff.

The original vext-32/vext-0 A/B (`runs/20260808-222048` versus
`runs/20260808-222203`) proved only that the pixels were confined to the added
rows; it did **not** prove spatial provenance. Plane dump
`runs/20260809-082943/diorama_dump/bg2_hi_gf2200.png` isolates the pre-fix red
band, while `runs/20260809-085004/diorama_dump/bg2_hi_gf2200.png` is transparent
after the fix with byte-identical WRAM (`b74e3362...`).

`PpuSetVerticalMarginLayerClip` is the boundary contract. The frontend gives
BG1 and BG2 independent real-row counts above and below their respective
`$24/$28` cameras, using dimensions `$30/$34`. For a camera `c`, height `h` and
budget `b`, top is `min(b,c)` and bottom is `min(b,max(0,h-225-c))`, matching
the game's `$02:B091` camera clamp. Synthetic rows farther away are transparent
rather than wrapped. Authentic lines bypass the clip, and one layer can remain
visible after another reaches either finite edge. The global capture follows
the semantic primary layer selected by `ActionBgPlan`, not a hard-coded BG
number.

The motivating lower-edge repro is `runs/20260810-112529`. From snapshot gf1992
to gf2120, BG1 camera `$24` and player `$08A4` both move upward exactly 48px
(`232 -> 184`, `312 -> 264`) while BG1 remains a 512px world. The capture was
still `top=32,bottom=0`, so the lower platform moved down in screen space and
fell through the captured floor even though it remained in the finite level.
At camera 184, 103 real rows exist below the authentic viewport; a 32- or
64-row bottom budget can retain them without changing camera/gameplay state.

The early capture/row traps, all found by measurement rather than by reading:

1. **`PpuSetOverlayCapture` clamped `y0` to 0** (while correctly allowing a
   negative `x`). The band rendered and was then silently clipped away — empty
   top band, no error anywhere. Bounds are now the render target's on both axes.
2. **`y == 0` early-returns** in the overlay clear/write paths. They were
   defensive against the line-0 frame-setup pass, which never reaches the line
   renderer; with a margin, line 0 became the real scanline directly above the
   screen and the guard dropped that row from every plane.
3. **The render target's origin is NOT the captures' origin.** Applying it to
   every overlay destination slid the promoted BG3 HUD, the HUD OBJ icon, HD
   replacements and the sim atlases down by the margin while their consumers
   went on reading authentic rows — every one of them mis-sampling by exactly
   `extraTopCur`, which is what "the UI is on the wrong tile positions" looks
   like. Fixed by `PpuOverlayRow`: **row 0 of a surface is the first row that
   surface's own capture rectangle asked for**, so only a capture that reaches
   above the screen (`y0 < 0` — the diorama band, and nothing else) shifts, and
   every other capture keeps writing absolute authentic rows exactly as before.
   The host side needs the same discipline: `ActRaiser_DioramaHudObjFinish`
   writes to two destinations with two different origins and must bias only the
   diorama-plane one. Measured, action stage, extend 0 vs 32: the HUD occupies
   rows `[11..26]` in **both**, while the BG2 plane grows `[0..223]` → `[0..255]`.
4. **The layers do not share a vertical world edge.** Using the primary layer
   to size the capture is correct, but treating that as permission for every BG
   is not. Each bounded world layer needs its own top AND bottom camera-derived
   clip or synthetic rows wrap to the opposite edge of its resident tilemap.
5. **Every capture consumer needs both counts.** The PPU already had a bottom
   scanout loop, but the frame slot, texture upload, BG2 valid-span plan and OBJ
   apron compositor previously sized themselves as `224 + top`. A real bottom
   band would therefore either be omitted or indexed past a shorter logical
   surface. `FrameSlot` now carries both immutable counts from the rendered
   frame.

`Diorama_Composite` normalizes world height against the AUTHENTIC 224, not the
captured height — dividing by the capture would make the taller plane span the
same 1.0 world unit, so the auto-fit would frame it to the same screen height
and the only visible effect would be everything ~14% smaller. It then lifts the
world (folded into the MVP so layers, skirts, depth shapes, shoebox and
`Diorama_ProjectCapturedPoint` cannot disagree) according to margin asymmetry.
Meshes are symmetric about `wy = 0`, so the pin is `(top-bottom)/(2*224)`:
equal margins require no shift, top-heavy captures shift up and bottom-heavy
captures use the mirrored signed solve to shift down.

### Sizing that lift (trap 6)

For a top-only capture, half the added height pins the authentic band perfectly
— and is wrong on its own. It is a fixed WORLD-space offset whose SCREEN effect depends on pitch:
pitching down tilts the plane so its projected centre sits low, leaving slack
above, and a full pin spends exactly that slack. That is why every pitched
configuration measured as well-centred and hid the problem. A flat camera has no
such slack — the composition already sat centred — so a full pin drives the
whole box against the top edge and opens a large gap along the bottom (reported
from a flat free-cam run, measured at 144px of top bias).

`DioramaVerticalShift` moves toward the signed pin but never past the point
where the drawn content is vertically centred in the viewport, with a floor so
centring does not trade authentic playfield rows for synthetic-band rows. The
bottom-heavy case mirrors world and projected Y through the same solver instead
of maintaining a second policy. Measured for the original top-only case,
3420x2128:

| camera | before | after |
|---|---|---|
| flat, zoomed out | top-flush, all slack at the bottom | gaps **314 top / 314 bottom** |
| flat, auto-fit (content overflows) | crops 152 of playfield | bottom held at the window edge |
| pitch 0.20 dist 3.25 | centre +11.8 | **unchanged** (still a full pin) |
| pitch 0.175 dist 5.75 | centre +6.0 | **unchanged** |

`pin == 0` (no band, or equal top/bottom margins) short-circuits to the
untouched matrix, so extend 0 is byte-identical and a symmetric capture remains
geometrically centred by construction.

### Sprite position outside the screen: the exact-position sideband

Three separate symptoms in the band all had one cause, and all three are gone
for the same reason.

The OAM Y field is 8 bits against a 224-line screen, so it does not say where a
sprite is — only where it is **modulo 256**. Everything outside the visible band
collapses into the same values:

| what it really is | OAM Y | what the band drew |
|---|---|---|
| slot parked off-screen (ActRaiser clears its shadow to `$E0`) | 224 | 103 of 128 slots stacked at screen centre — one garbled blob |
| sprite hanging off the screen BOTTOM (e.g. y=215) | 215 | `row = (uint8)(line - y)` wraps: rows 9..15 drawn at the top of the band as detached fragments |
| sprite genuinely ABOVE the screen | 224..255 | correct — the one case that works |

The first two were originally patched by *inferring intent from the byte* — a
`$E0` marker filter and a "reject positive Y on band lines" rule. Both worked,
both were guesses, and neither could lift the −32 ceiling.

`PpuSetObjExactPosition` removes the ambiguity at the source instead (Y-only as
`PpuSetObjYOverride` when the band shipped; generalised to both axes on
2026-08-06 for §13j's apron, which needs the same escape on X). ActRaiser's
action sprite emitter is a host HLE (`ActRaiser_BuildObjectSprites`), and it
computes the position as a full 16-bit value and only then truncates it to the
byte — so the exact value is available at the write site. The override carries
the **un-truncated form of exactly what the byte encodes**, not a recomputed
"true" position: that is what makes a slot with an override render identically
wherever the byte was not already lossy.

The rule on every margin scanline becomes: **draw only from slots that have an
override.** A parked slot was never written by the emitter, so it has none. A
bottom-edge sprite has its real positive Y; an above-screen sprite has its real
negative Y; ordinary signed subtraction selects only the correct rows. A part
more than 32 rows up — which used to alias to a positive byte and draw
mid-screen — stays in the top band, while a part below 223 can now occupy the
bottom band without aliasing above. Slots without an override (the HUD, emitted
by recompiled ROM at positive Y) keep the authentic mod-256 path untouched; for
`y` and `line` both in `[0,224)` the two agree exactly.

With position no longer routed through the byte, both the action object scan
and the per-part emitter widen their DRAW predicates by live top/bottom counts.
Activation remains on the authentic vertical window: it is game logic, not
presentation. That distinction fixed the missing tree head above the screen
and now carries actors/items standing on lower-band platforms as well.

Measured, Fillmore act 2 at extend 32: band OBJ pixels 104 → 570, 0 → 466,
104 → 735 on the frames where it fires, with the AUTHENTIC rows of all 32
plane dumps byte-identical, attract frame and flat widescreen byte-identical.

**Lifecycle (2026-08-05, expanded 2026-08-08; ledger §34): the sideband has
explicit action and simulation-town owners and must not outlive either
emitter.** Action rebuilds it from scratch in every
`ActRaiser_ObjectVisibilityScanWide` pass. The simulation composition pass now
does the same while publishing exact atlas parts. Because the renderer prefers
a valid override over the byte on EVERY line, `ActRaiserDrawPpuFrame` tracks
which owner supplied the current positions and clears them on every other
scene. Before that ownership clear existed, the last action frame's overrides
persisted into sim mode and re-drew slots the temple cutscene had parked
(x=0/y=$E0/tile $000/flip HV) at their stale action Ys. A redraw where the
emulated game did not advance keeps its owner's positions: action pause frames
and paused simulation towns both have frozen OAM and no new emitter pass.

### What the band actually contains

Verified in Fillmore act 1 (replay `saves/fillmore-act.rec`, diorama flipped on
at gf 1200, camera_y 400 in a 768-tall level so 32 rows are genuinely
available): BG2's band is 32/32 rows of real scenery, 28 of them distinct, not a
held copy of the first visible row. BG1 is empty there — and empty in the
adjacent VISIBLE rows too, because that part of the level is sky.

### Historical streaming repair (superseded by the HLE provider in BH8)

This is the axis where the §4 streaming seams show. A column strip decodes a
512px-tall window keyed to `cameraY & 0xFF00` and writes filler outside it;
row strips are its only refresher. The band reads rows ABOVE the camera, so
whenever `cameraY & 0xFF` is smaller than the band height those rows fall below
the page origin, outside the decode window, and the band inherits filler.

The former repair used `ws_build_band_rows` — the direct
transpose of `ws_build_visible_row`'s existing horizontal page-hole workaround.
It drove the game's own `$02:B8A0` row decoder for `world_y = camY - k*16` and
drained the record into VRAM host-side, inside the same WRAM/CPU snapshot
transaction the side margins used, so the band carried true map content and no
game state was perturbed.

It must run on the COLUMN path as well as the row path: rebuilding columns is
precisely what re-stomps the band, and moving down builds the leading edge
256px BELOW the camera, refreshing nothing above it.

Measured, Fillmore act 2, extend 32, `AR_VEXT_BANDFIX` off vs on:

| frame | BG1 band rows with content | opaque px |
|---|---|---|
| gf 2549 (camY 520, page phase 8) | 9/32 → **32/32** | 1462 → **5091** |
| gf 2999 | 32/32 | 2985 → **5124** |
| gf 3179 | 32/32 | 4686 → **5153** |

gf 2549 is the low-page-phase frame the mechanism predicts, and it is the one
that improves most — diagnosis and fix agree. VISIBLE rows are byte-identical in
every case: the repair touches only the band. Across four action replays, 2926
row builds with zero decoder rejections, and the cost is inside run-to-run
timing noise (~0.3%) because the refresh only runs on frames where the camera
actually moved. At that phase `AR_VEXT_BANDFIX=0` restored the pre-repair
behavior. BH8 removed the function, transaction, and switch after pre/post
diorama-32 matrices matched all 204 artifacts; bounded provider tile words now
supply the band directly.

BH8's final consumer census also removed the unused scanline clamp-band and
margin-source-gap renderer prototypes. Neither had a live nonzero caller; their
setters, `Ppu` fields, reset/raster branches, scene-inspector interpretations,
and frontend plumbing are gone. Whole-layer clamp/mirror/repeat, the live
repeat-band used by Bloodpool/Death Heim presentation, and
`PpuSetVerticalMarginLayerClip` remain. `ActionBgPresentationPolicy` is retained
only as the mechanical boundary between the map-owned `ActionBgPlan` and the
game-agnostic PPU masks/global diagnostic overrides. Three final release
matrices are byte-exact against the post-ring-repair baseline (612/612
artifacts).

The vertical-extension default is still 0. The symmetric plumbing is covered by
ROM-free finite-margin, bottom-layer-clip, exact-bottom-OBJ, tuner-capacity and
settings-overlay regressions plus the 2026-08-10 Bloodpool repro analysis; a
fresh manual visual sweep of every action stage remains prudent before changing
the default.

`AR_VEXT_LOG=1` prints the resolved margin with the camera/scroll state that
produced it, plus a `[vext-rows]` line showing where the HUD and the diorama
plane actually landed — the direct regression check for trap 3, since the two
must respond to the margin differently (HUD fixed, plane shifted).
`AR_VEXT_TILES=1` dumps the raw BG1 tilemap ids the band reads next to the
first visible row. Deliberately a raw dump and not a verdict: a first cut that
classified "uniform row" as filler reported 100% filler, because an all-sky BG1
row is uniform too.

## 13j. The OBJ apron — display margin vs resolve margin (2026-08-06)

Diorama-only. Captured OBJ planes are `kPpuObjApron = 64` columns WIDER per side
than the span the diorama displays. Those columns are **resolve headroom and are
never shown**.

### One constant was playing two roles

Before this, the captured/emitted region and the displayed region were the same
width, so "entering the viewport" and "hitting the buffer edge" happened at the
same instant. A part straddling the edge was not clipped — it was **abandoned
mid-write**. Measured on object `$10E0` (Fillmore act 1, `saves/artifacts2.rec`
gf1636), per-column occupancy across authentic columns 336→372:

```
before  0 6 10 22 27 28 29 35 37 38 36 40 40 41 41 │ buffer ends
after   0 6 10 22 27 28 29 35 37 38 36 40 40 41 41 41 41 40 40 37 36
        34 33 26 25 23 20 18 17 16 11 0
```

The object is 30 columns wide; the buffer held 15 and stopped **mid-plateau**.
A complete silhouette rises, plateaus and falls — stopping at the plateau is the
signature of the buffer running out, not of a sprite that shape. In a diorama
the cut is conspicuous because the scene is drawn as an INSET plane, so the edge
lands visibly inside the picture rather than at the screen border.

### Why the apron is NOT displayed (the finding that shaped the design)

The instinct is to show the extra columns so the object appears whole. That is
wrong, and the reason is a hard cap elsewhere:

**Background rendering now ends at `kPpuExtraLeftRight = 128` columns per side,
and the live diorama view ends at `kWsExtraMax = 120`.** The apron remains a
separate 64-column resolve band beyond both, so it can only ever hold OBJ
pixels. Displaying it would show sprites floating over empty background. The BG
width track deliberately widened the scanline buffers and accelerated tilemap
refresh without changing that resolve-only contract.

The corollary is worth stating plainly because it is easy to talk yourself out
of: **clipping at the shown edge is inherent to any finite shown region and the
apron does not remove it.** `$10E0` is still cut at the display edge. It is cut
cleanly instead of raggedly. Anyone who finds the remaining fragment
objectionable wants SUPPRESSION (widen the emitter's object-level cull so a
straddling object is not drawn at all, the way flat widescreen already behaves)
— a different, cheaper change that trades a ragged edge for a pop-in.

### What it actually buys

- **DOF / edge-AA / rim shaders only.** They sample the source texture across
  the whole `uv_u0..uv_u1` window and DO reach past the display edge, where they
  previously blended the last real texel against nothing. `DrawDioramaSkybox`
  documents the same failure for BG2 and works around it by insetting the UV
  range; the apron fixes it properly. **With GPU shaders off, filling the apron
  changes no pixel** — `BuildDioramaSupersample` blits the display window into
  its own target and the final draw samples that, whose edges clamp. The
  headless harness has no GPU device, so it cannot demonstrate this benefit.
- **The machinery the sim synthetic part channel needs**, built where a
  byte-identity gate can keep it honest.

### Geometry, and the invariant everything rests on

Screen x = 0 sits at surface column `apron + ws_extra`; the display window is
the MIDDLE of the surface. The apron bands are screen x
`[-(ws_extra+apron), -ws_extra)` and `[256+ws_extra, 256+ws_extra+apron)`, which
map to surface columns `[0, apron)` and `[apron+display, surface_width)`.

**No apron column is ever a display column.** Capture-time rasterization writes
only the two bands — enforced structurally by handing `PpuRasterizeParts` the
band as its `bounds` — so the display window stays byte-identical by
construction rather than by care. `tests/action_obj_apron_test.c` pins it.

### Real OAM is never widened

A part outside the display window stays PARKED in the OAM shadow exactly as the
ROM left it and rides a host part list instead, carrying its exact position
(these coordinates can be outside what the 9-bit OAM X can identify
unambiguously; the exact-position sideband is what made the live cap independent
of OAM). Only the object-level draw predicate widens,
because it gates whether the sprite builder runs at all; an object admitted
solely by that widening has every part rejected and parks a slot rather than
consuming one. Nothing writes OAM differently, so the invariant cannot drift.

Rasterization mirrors the hardware: OAM order decides who owns an overlapping
pixel through ONE shared z-test, and only the survivor's priority picks a plane.
So parts draw one at a time in list order and a pixel already opaque in ANY of
the four OBJ planes is skipped — first-writer-wins across bands, using the
planes themselves as the claimed-set (they start empty because
`PpuClearOverlayRenderLine` clears the full bound pitch every frame).

`kPpuObjApron = 0` collapses every site to its pre-apron expression and is the
A/B lever; verified byte-identical against a from-source pre-apron build.

### Trap: two widths, one variable

Three separate consumers read apron-wide surfaces with the DISPLAY width and
produced three distinct visible regressions in one commit (sheared HUD, black
stripe down the backdrop, HUD icon loose in the scene — ledger §36). The
arithmetic is trivial; the bug is always *which width am I holding*. Use
`ActionApron_SurfacePitch` / `ActionApron_DisplayOffset` /
`ActionApron_SurfaceColumn` rather than open-coding it.

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
6. Map the remaining `$02:AFCB` `$47F0` sim upload. World-navigation
   `$02:8384` is now verified as the current-matrix/focus Mode-7 register
   uploader; the town camera writer remains `$01:B4C6`.
7. Section config +27 -> `$F2` meaning; `$6A/$6E/$72` $2000-flag meaning.
8. Native camera/world-edge clamp ownership and its presentation-aware wide
   bounds. Distinguish changing the gameplay camera limit from merely hiding or
   padding pixels outside finite BG data; verify both axes and parallax layers.
9. Death Heim/`70X` is complete: the first-boss crash and later soft-lock were
   repaired, and the boss rush, final boss, and return transition were directly
   validated on 2026-07-14.
