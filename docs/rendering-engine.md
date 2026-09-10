# ActRaiser Rendering Engine Reference

Reference map of the game's drawing and streaming machinery. It records routine
addresses, data layouts, and unknowns marked `?`. See [SEAMS.md](SEAMS.md) for
conversion status and [ram-map.md](ram-map.md) for variables.

This document owns the renderer's current mechanisms. Project acceptance lives
in [progress.md](progress.md).

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

`$B90D/$B95A` are whole-body HLEs backed by one orientation-aware strip
expander. It preserves their different column/row source strides and output
orders, while the tile-word rule `(definition & preserved_mask) |
common_attributes` lives in `ActionBg_ComposeTilemapWord`. The complete-world
provider, Sky Palace margin repair, and ROM-backdrop decoder use that same
named primitive instead of carrying independent copies of the transform.

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
| +4 | bits 0..2 -> `$6A/$6E/$72`=$2000; bit 3 -> `$8F`=$1000 | **common BG1/BG2/BG3 tile priority** + OAM attr-bias arm. The tile expanders OR `$6A/$6E/$72` into every output word. All action profiles set BG3; `0102/0103/0205/0602/0604` also force BG2 high |
| +5 | `$2107`=$60\|(v&3), `$2108`=$70\|((v>>2)&3) | **BG1SC/BG2SC size bits** (bases fixed $6000/$7000) |
| +6 | `$2105` | **BGMODE per section** |
| +7..+12 | nibble-split -> `$3A-$45` | parallax ratios (6 planes) |
| +13..+18 | `$BB/$BA/$B9/$BF/$C1/$C4` | fade/brightness config (+ mode bits in `$C4`) |
| +19..+22 | `$C5`, `$C9`=v<<4, `$C6`, `$CA`=v<<4 | **BG2SC page-flip anim** ($C5 arms `$AEAE`) |
| +23 | bit7 -> `$DA`=$1000/$0000; bits4-6 -> `$E1`=n<<7; bits0-3 -> `$DF`=n-1 | **tile-anim: char VRAM target, frame stride (bytes), frame count-1** |
| +24 | `$DE`=n-1 | anim tick period mask |
| +25-26 | `$E6` | **stage timer init** (decimal, ticked by `$BC82`) |
| +27 | `$F2` | Renderer-nonoperative: `$02:B4E8` writes it, but the complete registered/recompiled consumer census finds no direct-page read. Profiles `10`-`15` and `25`-`28` (hex) use value 1; a standalone background loader can ignore it |

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
unsigned fallback. This fitted interval is horizontal only: margins start with
the live widescreen render budget and each side is then limited by the
canonical playfield layer's fixed horizontal extent, when present. This makes
a tuned 16/16 playfield such as Bloodpool `0208` fit only the 16 pixels it
actually presents rather than shifting for an invisible full margin. Diorama's
vertical extension never constrains canonical camera `$24`; the per-frame
capture policy instead resolves the real rows independently available above and
below the native camera. The correction also consumes the renderer's
canonical `ActionBgPlan_CanvasOwner` and provider-enable decision: authored
Death Heim hub/final scenes (`0701`/`0708`) and `AR_ACTION_BG_HLE=0` do not
acquire playfield camera policy. 4:3, Wide Raw, non-action modes, disabled
action widening, non-finite scene plans, and rooms too small for the complete
requested horizontal view are therefore behavior-preserving. BG2 parallax helpers,
player-relative tail, strip flags, and the single canonical `$22/$24` camera
remain intact; only `$22` receives presentation fitting. A fitted horizontal
clamp also reconciles `$7C/$7E` before
those consumers run: while the old camera is already inside the corrected
interval they receive the camera displacement that actually occurred, not the
unfulfilled outward request; an initial correction from outside the interval
publishes zero motion. This matters at section entrances, where a native
`$7C=-120` request can coexist with the new minimum camera of 120. Leaving the
request live made BG2 parallax and the player state machine treat a stationary
boundary as movement (`runs/20260810-172649`, gf9652).

Bloodpool `0202` (BG1 `768x512`) resolves to horizontal `26..486` in flat
16:10 Full and to `120..392` in Diorama-32. Its vertical camera remains native
`0..287` in both modes; at the floor, the capture naturally resolves to
`top=32,bottom=0` rather than moving `$24` to manufacture a symmetric band.
Death Heim `0703` (BG1 `256x256`) stays at native vertical `0..31`. The `0701`
hub stays native at `0,31` until its natural
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
  Frame buffers live at `$7F:B800 + n*$E1`. `$02:BAF5`, called during action
  entry after the asset VM and whole-map rebuild, is the complete producer:
  when `$E1 != 0` it reads exactly `$1000` bytes of already-loaded character
  VRAM beginning at word `$DA` into `$7F:B800-$BFFF`. It is a snapshot, not a
  frame composer. Raw config `+24` bit 7 denotes a continuation room: `$BAF5`
  clears the bit and skips the redundant capture, retaining the owner's WRAM
  sheet. An offline cumulative asset-script build can always capture the
  current room's own reconstructed 4 KiB target window; it is byte-identical
  to the owner for every continuation family, including `0704`/`0303`.
- Sim town (`$18=0`, `$19!=0,9`): same WRAM-buffered animation path as action
  mode, consumed by the generic `$02:AF30` full-word descriptor uploader.
  `$02:BAF5` uses the same single contiguous 4 KiB capture; the town profile's
  `$E1/$DF` divides it into its configured phases. Later ticks re-upload the
  selected slice to VRAM `$0000`. `$AF86`'s ROM-bank-$0A, high-byte-only dual
  upload belongs only to the separate `$19=0 or 9` non-town branch.
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
| `$0000-$1FFF` | BG1+BG2 chars (BG12NBA=$00); Sky Palace capture is byte-identical to the `$4000`-byte ROM bank at `$0D:C000` (file `$06C000`) | `$02:B28E` (`ActRaiser_LoadActionCharacters` for guarded action shapes) + `$02:C5C9` decompressor pair |
| `$2000-$2FFF` | common action OBJ atlas (OBSEL=$01, 8x8/16x16) | `$02:BC9E`: 4096 words from ROM `$07:8000-$07:9FFF` |
| `$2D40-$2DBF` | reserved OBJ magic/effect overlays inside the common atlas | `$02:BC9E` writes 128 words to `$2D40`; `$00:96C3-$96F5` can arm 128-byte slot-0 upload to `$2D80` |
| `$3000-$3FFF` | per-room enemy OBJ sheet; Fillmore `$01/$01` uses 8192 decoded bytes from file `$080000`. Bird visuals `$1F-$22` in the decoded `$7E:4000` table consume it | asset-script command 7, `$02:B28E` |
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
states. Thus the **common action atlas is static**, with small dynamic
magic/effect replacements; the separate `$3000` enemy atlas is supplied by the
room script. There is no per-enemy sheet allocator to exhaust.

The Master uses the common sheet: spawn record `$00:9810` installs `$0900` in
object `+$28`; the emitter's XOR `$0100` cancels its bank-select bit and retains
palette 4. Sword-trail parts add palette 5. Both facings must preserve the
composition extent/offset pair rather than tight-cropping each pose. In normal
orientation, OAM places a part at `worldX - leftExtent + partX` and
`worldY - topExtent + partY - 1`; the Y subtraction includes the native
carry quirk. Earlier OAM parts win overlap. Idle/walk/sword programs in
`$06:8000` now also supply the builder's decorative actor poses; gameplay
handlers, hitboxes and movement remain untouched.

## 9. OAM / sprite pipeline (action)

See SEAMS.md "Action OAM pipeline".
- `$00:8C98` (HLE `ActRaiser_ObjectVisibilityScanWide`): shadow clear
  (x=$80/y=$E0 parked), high-table cursor reset (`$9A=$0580`), `$00:923A`
  HUD sprites (fixed positions from `$06:A800`), object walk (`$06A0`
  stride $40): skip $8000/$4C00, evaluate independent DRAW and ACTIVATION
  windows (extents at +`$0A/$0C/$0E/$10`), write selected-activation bit
  `$0400` in +`$30`, and respect draw-suppression bit `$2000`. DRAW uses fitted
  `$22/$24`, live presentation margins, and the resolve-only OBJ apron.
  ACTIVATION retains authentic vertical `$24`; horizontally it uses fitted
  `$22` plus live margins normally, or a reconstructed native 256px camera
  while player `$08B2` is `$97A6/$97C9/$97E4`. `$97E4` installs input-reading
  `$9832`, restoring wide activation without changing drawing or presentation.
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

The arrival gate must reconstruct the native horizontal camera, not merely set
margin widths to zero around the fitted one. `$02:B030` centres camera subject
X at 128 and clamps it to `[0, world_width-256]`; before `$97A6` assigns player
slot `$08A0` to subject pointer `$8A`, initialized player X `$08A2` supplies the
entry anchor. At Fillmore's left edge fitted `$22=120` but arrival X `$0050`
produces native camera 0. The failed first gate therefore chose `[120,376)`
instead of `[0,256)`: visuals remained wide, but `$0400` selected the wrong
gameplay/script objects and the intro fade regressed. Corrected replay
`runs/20260812-122927` reaches `$9832` at gf1185 with no second music
stop/restart.

### 9a. Host action lighting and particles

`ActionEffects_CaptureFrame` is the game-thread boundary for presentation-only
spell metadata. Its data-driven rules require the live controller and complete
per-slot animation/composition/transform identity; mirrored or cloned actors
remain separate rather than collapsing to a player-centred effect. The capture
publishes generic point/rectangle/segment geometry, authentic OBJ priority, and
separate actor, phase, and pulse clocks. `action_effect_clock.c` owns the small
publisher/read adapter: the successful common epilogue of the authentic
`$00:8C98` action object/OAM pass publishes its monotonic execution serial only
after both nested sprite builders have completed, and `FrameSlot_Capture` turns
that serial into a bounded delta. It is not a host/emulator-frame clock: the ROM
skips `$00:8C98` during native pause and freeze paths, while catch-up still
contributes one serial step per completed gameplay pass. Map-backed torch and
lava emitters share an observer-owned clock seeded from `$0088` on entry and
advanced by that same serial delta. Native pause, host pause, aborted OAM
passes, and retained-slot redraws therefore freeze every action light and
particle together. Observer state is explicit, not hidden inside the capture
module, and savestate loads reset it before the next frame. Both this clock and
the ordinary emulated-tick capture clock use `frame_timing.h`'s shared bounded-
delta limit; only the gameplay-pass clock filters ActRaiser's native pause.

`ActionSceneEffects_CaptureFrame` is the parallel boundary for exact scene
accents, including Bloodpool's map-$08 boss attack resolved from the 30-snapshot
run `20260810-180202`, a complete decode of its `$7E:5000` animation bank, the
global player sword beam measured in run `20260810-175403`, and Marahna's torch,
orb/split fireball, linked-lightning, and boss-lightning families plus Aitos lava
pits/fireballs measured in runs `20260811-151353` and `20260811-221433`. Run
`20260812-000613` additionally maps the Aitos boss's two-child sword volley.
BG wall torches are not actors: the observer uses the same validated
`ActionBgMapView` contract as the full-world provider and matches the exact BG1
metatile pair `$47` over `$4F` throughout map group `$02`. Marahna maps
`$05/$04-$08` instead match one complete `$43` metatile at `(8,11)` and publish
only a camera-local subset; `$04-$07` share a 31-instance world while boss map
`$08` contains ten cells in its 512×512 world. Fireballs and
lightning are ordinary action objects and require the positive handler,
resume/source, animation-state, visual, and composition tuples recorded in the
RAM map. Marahna connector children additionally validate their `$E18E` source
endpoint, adjacent `$E254` partner, backlink, orientation, and exact midpoint.
The Marahna fireball matcher validates source `$E047`, every exact frame of the
large `$05-$08/$4504-$4528` left/idle/right orb cycle, and four
`$32/$4BCD` or `$33/$4BD9` cardinal children against their retained orb backlink.
Up and right require their authored V/H flips. A second fireball rule validates
the `$DE96` snake parent and its `$1D/$4869` or `$1E/$487C` child lifecycle
through exact state/resume/velocity, local counter, backlink, and matching
horizontal flip. The reaper's source-`$E0BA` aimed/falling orb and the
superficially fiery `$34/$4BE5` actors are measured moving platforms and are
deliberately rejected.
Boss lightning further requires map `$02/$08`, animation bank
`$7E:5000`, and a validated backlink to its live `$BDFF` parent. States `$02`
through `$07` select vertical or diagonal long/medium/short strikes; their
resume PCs vary during normal control flow and are not shape identities.
Visual `$20` is the blank half of each strike cycle, so it intentionally emits
no host effect. Matching only a visual ID is invalid because action slots and
values are polymorphic. A recycled same-kind slot begins a fresh renderer
generation when its lifecycle key changes or its position is discontinuous
with its measured velocity.

Marahna boss map `$05/$08` has its own source `$E483` electrical rule. Parent
artwork `$07/$57C2` and `$08/$5868` receives hand-charge illumination;
`$0A/$59DE` receives a central orb bloom. Backlink-validated `$11/$5CE0`
children publish their exact 32px down-left or down-right local quadrant and
measured `±4,+4` velocity. The renderer adds two projected cyan ribbon layers,
so the same captured segment passes through the production flat/Diorama camera
adapter rather than a mode-specific shader path. At impact the same child
changes to resume/state `$E57E/$07` and travels horizontally at `±4,0` through
the complete `$12/$5D01`, `$13/$5D0D`, `$14/$5D2E`, `$13/$5D0D` loaded cycle.
That phase receives a compact ground-aligned bloom plus contact sparks and a
direction-aware electrical wake. Its exact OBJ-local geometry uses the same
production projection path in flat and Diorama modes.

Runs `20260822-195453` and `20260822-195726` close the Death Heim boss-rematch
seam without making the new effects Death-Heim-only. Every enhanced family is
admitted by an exact `(room, retained source)` pair for both its original arena
and rematch:

| Boss/effect | Original | Death Heim | Positive identity/style |
|---|---|---|---|
| Minotaur axe | `0104`, `$AF5D` | `0702`, `$F6CA` | `$8661/$B008`, state 3, exact spin artwork, same-source parent; warm spin light and sparks |
| Wizard lightning | `0208`, `$BDFF` | `0703`, `$F6E2` | existing six-strike plus floor-impact family |
| Flaming Wheel body/shots | `0407`, `$D838` | `0705`, `$F712` | active `$4000`, `$7E:5000` body with root backlink `0` or room-owner `$001C`; four full wheel compositions anchor twelve rim-fire emitters to exact OAM-local centres. `$8661/$A65D` state `$08-$0C` children with exact `$51B5-$51D9` art receive cyan light and wakes. |
| Viper lightning | `0508`, `$E483` | `0706`, `$F72A` | existing charge/orb/diagonal/ground family; rematch parent backlink `$001C` |
| Ice Dragon ball | `0608`, `$F161` | `0707`, `$F760` | `$8661/$F2CA`, exact state `$19/$1A` ice-ball artwork, same-source parent; cool light, crystalline trail and particles |
| Tanzara projectile | — | `0708`, `$F80F` | exact 50-tuple projectile allowlist; restrained generic light and trail |

Pharaoh room `0704` remains intentionally undecorated. Room/source pairing is
strict: an original source cannot claim a rematch room or vice versa, and the
Flaming Wheel handler is not used as identity because the same visible body
legitimately moves among delay, repeat, and boss-AI handlers. This rejects its
same-source helpers without duplicating a full-strength flame emitter.

Priority is also not identity. The admitted wheel/shot compositions carry raw
priority zero and the native sprite builder ORs the live `$008F` attribute bias
into every part. Effect capture therefore matches room + retained source +
`$7E:5000` graphics/composition + ancestry first, then copies `$008F` bits 12-13
to `obj_priority`. The original `0407` capture measures priority 2; a Death Heim
room may choose another band and will still project every one of its five shots
through the same plane as its source sprite.

Runs `20260824-034218` and `20260824-041410` close the `0406` statue-fire seam.
Its two facing records retain `$D5B1/$D5C0` and `$7E:4000`. State `$18` grows
the pillar through `$1C-$1E/$4763-$4790`; state `$19` sustains it with
`$1F/$47B1` and `$1E/$4790`. State `$1A`'s `$17/$46FD` hold is inactive and
receives no fire effect. Diorama vertical extension can draw one
before the authentic 224-line activation window clears `$0400`; when extended
activation is enabled, only this exact room/source/graphics tuple uses the
vertical draw window as its activation window. The actor effect inherits live
OBJ priority and adds a warm horizontal spill plus bounded rising sparks.

Snapshots `runs/20260823-211358/snapshots/snap_02_gf5009` and
`runs/20260823-232614/snapshots/snap_00_gf3879` prove that maps `0405` and
`0406` deliberately mix colour-zero BG2 transparency with opaque-black cells.
The authentic PPU ultimately resolves empty BG2 regions against a final black
backdrop, so both encodings blend; Diorama's coloured backing exposed the latter
as rectangles. Each room selects its immutable ROM BG2 source and authors
`bg2 = transparent:black`. Capture first fills the complete BG2-low presentation
plane opaque black, including untiled areas, and then paints low/high tile art;
BG2-high remains sparse and preserves its painter position. A named ROM skybox
uses the same fill-then-paint rule. Every non-zero black outline/cell remains
exact, and authentic 2D scanout plus emulated map/VRAM remain unchanged.
`backdrop alpha:0` prevents the residual framebuffer plane from duplicating the
room behind the configured presentation surfaces.

This is also a general base-BG property in the per-level Layers editor. BG1 and
BG2 can use fixed black or a live entry from the current 16x16 CGRAM palette;
the manifest stores `transparent:cgram-XX`, so palette animation is retained.
Camera-local sections can author `transparent:off` to suppress an inherited
room fill; Reset removes that local key and restores inheritance. The authored
Off bit is snapshotted separately from the resolved colour so immutable ROM
skyboxes do not mistake it for an unauthored default fill.
The backing is initialized across the whole captured plane before any tile
sampling. Mirroring, repeat, clamp, and live-world extension therefore paint
normally above it without transforming or clipping the backing, while split
high-priority surfaces remain sparse and preserve their normal paint order.

Aitos map `$04/$01` uses a separate camera-local BG1 semantic: `$DC`, one to
six `$DD`, then `$DE`, over an equally wide `$DF` row and the following `$E7`
bubbly row when it exists. This yields the exact 64px/128px lava-surface
rectangle without looking at raster colours. Its six
cyclic projectile slots retain source/resume `$CF9E/$CFCD`, exact artwork
`$2A/$4D21` or `$2B/$4D2D`, and 8px extents. Handler/state pairs
`$CFE3/$22`, `$8661/$23`, and `$CFFE/$24` cover rise, reset/wait, and return;
bounded position continuity resets the particle generation when a persistent
slot jumps back to its launch point.

Aitos Act 2 maps `$04-$06` use a different BG1 semantic for their side-on
lakes: a maximal three-to-63-cell `$01` bright lip, an animated/transparent
`$02-$04` surface row (`$77` in map `$06`), `$05` lava body, and one of the
measured `$33/$34`, `$2C/$32`, or `$33/$32` bank pairs. Capture walks back to
the authentic bank when the camera begins inside map `$06`'s 640px lake, then
publishes one broad lip identity. Rendering divides that identity into
overlapping at-most-96px light sections: run `20260823-220042` proved that one
reservoir-wide radial gradient faded to transparency at long-lake ends even
though capture was present. Each local section keeps its intense strip on the
side-view surface while a shallow low-alpha spill rises into the cave; 32
deterministic sparks are distributed over the whole reservoir instead of
forming a torch plume.

In flat presentation, an Aitos Act-2 lava room (`0404-0406`) with Action Effect
Particles enabled resolves the already-composited world through a fixed 16×14
textured geometry grid before the HUD, but only while a validated semantic lava
reservoir intersects the visible source rectangle. Interior UVs scale to less
than one authentic pixel and cap at 6.5 output pixels (the former 3.25px cap was
nearly invisible at 3420px output), weighted toward the lower room; every outer
vertex is pinned. Diorama never applies this full-screen warp. Its lava lighting
and sparks are attached to and culled by the finite published BG1-high source
window, while explicitly unbounded atmosphere and authored BG2 folded overflow
keep their separate projection contracts.

Run `20260812-000613` adds two deliberately separate Aitos styles. Volcano
rocks are `$CEEC/$CF16` actors with exact `$8661/$27`, `$2B/$4D2D`, 8px extents,
signed X/flip and `Y=-1..+1`; they receive a compact molten crust/glow and
non-directional surface sparks, not the `$CF9E` flame wake. Waterfall platforms
in maps `$04/$02-$03` match exact BG1 rows `$36/$5E*/$81`,
`$4E/$F4*/$4F`, `$F6/$FC*/$FE`. Their presence inside the bounded camera window
admits both BG1 drip/spray accents and one BG2 flow veil, preventing the shared
map `$04/$02` cave section from receiving water. `DioramaProjection` publishes
an independent BG2-low plane and interpolated UV window, while the production
adapter selects BG2 camera `$26/$28`; flat and Diorama modes therefore share
one portable geometry style without a platform shader dependency.

The player sword-beam rule is not map-specific. It requires handler `$9D1C`, animation
`$06:8000`, attacker flag `$0001`, backlink `$08A0`, a source descriptor shared
with the active player, and exact state/visual/composition `$13/$30/$99E8` or
`$14/$31/$9A17`. Its raw collision header includes signed byte origins, so
capture publishes four explicit state/direction rectangles instead of feeding
those words into the generic unsigned-extent path. Run `20260810-184935`
provides the decisive state-`$13` proof: hot point `(112,201)` versus OAM bounds
`(144,168)..(160,200)`, yielding local `(32,-33)..(48,-1)`.

The Aitos Act-2 boss uses a separate authored route into that same presentation
style. In map `$04/$03`, source `$D646` creates an inactive `$D793` volley
controller linked to the live dragon root, then two `$8661/$A65D` children.
Their complete tuples are `$01/$21/$56D8/(-3,+1)` and
`$02/$20/$56BE/(-3,-1)`, with distinct local counters and asymmetric extents.
Snapshot `snap_05_gf21056` proves both 24×24 rectangles and priority-2 OAM.
Run `20260812-224123/snap_01_gf15666` measures the opposite facing as a complete
180-degree transform: controller and child both use H+V flip `$C000`, both
velocity components reverse, and the four extents swap sides. The state-1 OAM
rectangle is exactly `(-16,-9)..(8,15)` relative to its hot point. The same
validated relation covers state 2, yielding all four authored diagonals.
Capture consequently publishes their real diagonal headings and OBJ band, but
reuses the portable cool halo, tapered wake, and materializing-star renderer.

Dynamic scene actors and ordinary world-overlay decorations run after the
authentic action image and before flat HUD, HD-replacement, inspector, and
settings overlays. BG-local decorations instead use the depth-aware contracts
below; merely projecting geometry onto a BG plane does not make a late submit
obey that plane's occlusion.
Flat mode uses the resolved physical viewport. Diorama mode receives a
`DioramaProjection` value from the same composite call that drew the BG and OBJ
planes: camera matrix, capture mesh dimensions, output dimensions, and one
paired UV-window/shape record per eligible BG1/BG2/OBJ plane are not re-derived
from live state. BG projections use the exact drawing predicate: a plane hidden
by its layer toggle, missing a current texture upload or pixel publication,
flattened as BG3 HUD, or displaced by skybox-only mode is neither drawn nor
published as projectable. An OBJ-attached world overlay is itself current
presentation content, so its exact priority bit may retain a visible,
texture-backed OBJ transform when the isolated hardware band has no final
winning pixels. The mask comes only from immutable captured effect frames and
is filtered through immutable requested/content/success plane masks, so an
upload failure still fails closed and the exception cannot make BG1/BG2
projectable. Scene metadata selects its
authentic plane: torches, lava surfaces, and platform water use BG1-low;
ordinary fireballs/lightning and the player beam use OBJ priority 0, while the
Aitos boss crescents retain authored OBJ priority 2. The projection value owns
`texture_x_origin`, the hidden
64-column OBJ resolve apron that precedes caller-visible capture coordinates.
Keeping that origin in inverse projection prevents overlays from sliding
horizontally on raked planes without changing the flat path's contract.

The camera-wide waterfall is a BG2-stage decoration, not a late world overlay.
Diorama invokes a bounded plane-effect callback immediately after the drawable
BG2-low mesh, before later BG1/OBJ planes. Flat presentation captures a current
black/white mask from the PPU's complete main-screen priority resolve, multiplies
the waterfall target by pixels where BG2 is the final winner, then composites
the premultiplied result. Both paths therefore keep platforms, enemies, and the
player in front. They use standard SDL additive/multiply blend modes and fail
closed if the backend substitutes an unsupported mode; no backend shader is
required. One-time success logs now report the first Diorama BG2-local submit
and the first flat winner-masked composite independently.

Wall-torch lighting and embers are likewise BG1-stage decorations. This is
load-bearing in Marahna's Viper arena and Death Heim rematch `$07/$06`: a late
world overlay made the torch spill visible through the boss even though the
authentic OBJ pixels correctly covered the source wall flame. Diorama now
submits each torch batch from the after-BG1-low callback, before OBJ2/BG-high/
OBJ3 painter bands. Flat mode renders the batch into the shared BG-local target
and multiplies it by a BG1 winner mask before compositing. That mask follows
BG1's owning PPU screen rather than assuming TM: in the measured Marahna form,
BG1 and OBJ are TS-only, so the sparse white region comes from the resolved
subscreen winner and is black wherever Viper's higher-priority OBJ wins. Main-
screen BG1 rooms use the same flag and resolve against TM. The capture reserves
BG1 only in exact torch-admitting room families, respects earlier HD/dump
capture ownership, and fails closed if no current mask was produced. It also
skips BG1/BG2 winner capture when both Action Effect lighting and Particles are
disabled. The shared BG-local target is sized to the resolved viewport and
uses target-local geometry; final composition restores the viewport offset, so
letterbox and pillarbox pixels incur neither clear nor multiply work.

The paired bottom atmosphere solves a different problem and therefore has a
different painter contract. `$04/$02` deliberately caps raw-wrap BG2 at 24px
of vertical extension so waterfall tiles cannot bleed into authored non-water
rows. Two tiers of three soft mist banks and thirty-two rising foam motes are anchored at
the end of that safe extension, with visible coloured rings spanning both the
last water rows and the unsupported lower band. The lower banks use unequal
widths, vertical anchors, depths, and silhouette flare so their visible alpha
does not terminate on one horizontal line. The deepest bank fades more than
100px below the seam while the six visible bottoms span over 80px. They retain
BG2 camera and rake/bow projection and submit unmasked from the same after-BG2
Diorama callback using verified source-alpha blending: unlike an additive
light, the mist can obscure and feather the BG2/skybox discontinuity. Later
BG1/OBJ planes and the HUD remain in front. Flat mode has no vertical extension
gap and therefore keeps only its winner-masked veil.

`action_effect_render.c` converts captured kind/phase/geometry into bounded,
renderer-independent spell and scene batches; unknown values fail closed.
`action_effect_projection.c` is the shared production/test adapter for camera
subtraction, widescreen/vertical margins, flat viewport placement, and
compositor-published BG1/BG2/OBJ Diorama projection. A
small capacity-aware geometry writer appends directly to the caller's final
arrays, avoiding a spell-sized scene scratch copy and its former stack peak.
The immutable scene frame also separates its 16 dynamic-actor records from 16
camera-local decoration records. The measured Aitos window can consequently
publish all 14 platform splashes plus one BG2 veil and one paired bottom-mist
record without consuming the complete actor budget; either list fails closed
independently, and presentation
reuses one scene scratch batch while submitting actor and decoration passes.
Torch light/particles sample the shared authentic game clock at 2× visual rate
to follow the fast BG flame animation while all torch instances remain in
phase; Aitos BG lava and platform spray use the same accelerated presentation
clock. Lava light spans the complete decoded bubbly volume, while its twelve
deterministic embers originate in a narrow band one quarter of the captured
height above its geometric midpoint, matching the isometric surface instead
of appearing from the lower volume. The waterfall veil instead uses 96 slow
deterministic streaks to soften the short source cycle; its bottom atmosphere
uses the six mist banks and thirty-two foam motes described above. Molten rocks
keep compact tumbling sparks rather than a flame tail.
Bloodpool, Marahna, and Aitos fireball sparks trail opposite measured velocity.
Marahna's horizontal/vertical connector rectangles rotate the same cyan/violet
glow and drive two projected ten-segment ribbons through their exact 80px
chords; crawling sparks and endpoint fans share the authentic lifecycle clock.
Five authored links are explicitly budgeted and a sixth fails closed. Marahna
boss charge/orb stages add cold blooms and sparks; its launched bolt
adds two eight-segment cyan layers, with a one-bolt capacity contract. Bloodpool
trap lightning lighting uses the live `88+88` extents, and its last nontransparent ring reaches both
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
follows measured velocity, and its local basis is projected through the
authored OBJ band—priority 0 for the player or priority 2 for the Aitos boss—
preserving the same form on a Diorama-raked plane. One player beam can coexist
with the boss's two crescent children, so the batch reserves the measured
three-stream peak and rejects a fourth instead of inflating all scene slots.
Integer-hash particles and integer triangle pulses make repeat builds
deterministic. `present.c` supplies immutable projection inputs and submits
through verified standard SDL additive/source-alpha blends plus untextured batched geometry
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

See [dialogue-system.md](dialogue-system.md) for the two native text grammars,
retained-row scrolling, partial-menu erasure, source identity, and special-cell
ownership. Text lifetime is not equivalent to box artwork or scene lifetime.

- Town-map tile *content* (which house/road/bridge tiles exist where) is not
  decided in this engine layer: structure records drive per-record visual step
  programs (`$03:A4B8` construction / `$03:A4A8` rebuild HLEs → `$7F:77E7`
  slots → the `$89F7`/`$8A7E` stepper) that
  edit the town map, which the SEAMS "Sim-mode town-map GRAPHICS pipeline"
  then uploads. See SEAMS town §7 for the record/step system.
- `$02:BF60`: fixed-text composer (packed row/column in A, saved in DP `$14`);
  interactive dialogue is the separate `$01:8E29` interpreter. Its tile
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
| `$00:8C98` cull + `$00:8D68` builder | `widescreen-sprites-v2`: regenerated and direct-play validated Stage C/D1/D2; independent wide drawing and `$0400` activation, with only extended horizontal activation held to the reconstructed native window during `$97A6/$97C9/$97E4` arrival |
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
   `AR_WS_MARGIN_ACTIVATION=0` restores native-width activation. The automatic
   action-entry gate uses that same native horizontal window until `$97E4`
   installs input-reading handler `$9832`; drawing, camera presentation and BG
   extension remain wide throughout. Regions/effects still need testing with
   hardware scanline limits enabled and lifted.
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
- **That icon has two OAM shapes, selected by spell (corrected 2026-08-06).** The
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
  authentic centre-screen X while the rest of the HUD moved.
  `AR_HUDICON=1` reports the scan outcome (slot and count, misses included).
- **The game-over return requires capture inside the OBJ evaluator (revised
  2026-08-12).** `runs/20260812-122258/snapshots/snap_02_gf5705` proves the
  failure with Magical Fire's valid slots 6-9; after the first attempted fix,
  `runs/20260812-220252/snapshots/snap_00_gf6889` reproduces it with selected
  spell 2 (`$02AC=2`) and the valid single-large slot 6 (`$84`, attr `$39`).
  The generic Diorama OBJ plane still receives the icon at its authentic
  position, while the separately reconstructed flat-HUD copy is empty. Moving
  `PpuRasterizeObjRange` beside each scanline did not make it scanout: the helper
  still re-resolved and rebuilt the complete sprite from a second register
  snapshot. `ActRaiser_DioramaHudObjPrepare` now registers the validated range
  with `PpuSetObjRangeCapture`; the normal sprite evaluator writes only those
  slots directly to `g_hud_obj_pixels` as it fetches them. `…Finish` uses that
  exact opacity mask to remove the original from its Diorama priority plane and
  restore any covered sprite. OAM, VRAM, CGRAM, and emulated game state remain
  untouched.
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
- A promoted HUD's effective scale is also capped so its combined authentic
  width fits the output viewport. This includes the display-density multiplier
  on a pinned scale and preserves pixel aspect. Otherwise separately anchored
  left and right groups can overlap (notably the full angel-health bar and
  magic icon in a small Sky Palace window). Only the projection is capped:
  the stored preference resumes when there is room, native HP/tile data stays
  unchanged, and lower dialogue/menu rows retain their scene projection.
- Enhanced town labels use these same HUD projection chunks, not a second
  screen-space scale. Their logical erasure mask stays within the label's
  eight-pixel row, above angel health. Single-line text keeps its requested
  font size but lowers the fitting floor when a small physical HUD row requires
  it; the accessibility font preference must not force a native fallback merely
  because HUD scale is independently small. Repeated presentation reuses the
  fitted bitmap through the text-surface cache.
- The enhanced-text cache uses both an entry bound and a soft texture-byte
  budget. Fitting probes run without pins and consume their results immediately;
  the final preparation pass pins every acquired surface until that frame has
  been consumed. Neither byte pressure nor entry pressure may evict a pinned
  handle. A miss with all slots pinned fails cleanly instead. An unpinned
  acquisition retains at least its newly returned entry even when that entry
  alone exceeds the byte budget. Statistics reset clears activity counters,
  not live resource ownership, and starts a new peak at the current byte size.
  Cache keys include the enabled shadow and its palette color. The portable
  shadow pass traverses against its offset to preserve original coverage
  without allocating a temporary alpha plane; invalid inputs remain distinct
  from retryable font/resource failures.
- Text raster requests (ABI 11) may accent the shaped cluster containing a
  logical UTF-8 grapheme end; zero disables the accent. Its RGB and logical
  offset participate in cache identity. The portable color pass preserves alpha
  and complete ligatures/combining marks, independent of visual direction;
  SDL applies it after bands and before shadows. Frame ABI 26 carries this
  pointer-free style and a grid `center_rows` option. Grid slices remap the
  accent into their own byte ranges; row centering is geometry, not re-shaping.
- The presenter accepts a frame only after `ArLocalizationFrame_IsValid`
  verifies ABI extent, fixed-pool counts, terminated stack/locale identifiers,
  cell ownership, nested text/grid/object/span ranges and UTF-8 reveal
  boundaries. Superseded snapshots remain legal but cannot be reached through
  active cells. Shared table fitting supports the frame contract's complete
  ten-cell row capacity; fixed and fitted layouts no longer have different
  accepted maxima.
- The desktop text backend resolves bidi paragraphs and script runs with the
  pinned, statically compiled SheenBidi dependency before SDL_ttf/HarfBuzz
  shaping. Logical UTF-8 text is never reversed. Wrapped lines are reordered
  at line boundaries, and each shaped run supplies both its pixels and its
  reveal rectangles. The cached result keeps monotonically increasing logical
  byte endpoints, even when visual glyph positions run right-to-left. Ordinary
  non-bidi text retains the existing fast path. Both paths share font resources,
  fitting, palette bands, shadows, numeral slant, mosaic and cached reveal.
  Logical `Leading`/`Trailing` alignment follows the resolved paragraph base;
  explicit `Left`/`Right` alignment preserves native HUD/selector geometry.
  Dialogue requests use logical leading alignment, not an already-flipped edge.
  Bitmap ABI 4 carries the first resolved paragraph direction through the
  surface cache so auto-direction cropped labels and direction-following cells
  can use it without duplicating bidi analysis in the presenter.
  `ArUiTextRun` ABI 2 uses a separate physical left/center/right placement enum,
  so host UI placement cannot be mistaken for paragraph direction. SheenBidi
  and SDL handles remain private to the desktop backend; alternate backends
  must provide equivalent paragraph/run processing, not just a font direction
  flag. Frame ABI 26 carries an effective `ArLocalizationTextLanguage` per
  snapshot, independent of the selected font stack/default locale. Both grid
  requests and flowed dialogue/labels use that source locale and direction;
  the existing raster cache identity includes them. A changed frame default
  cannot reinterpret cached fallback text. Raster request ABI 11 additionally
  borrows bounded logical source ranges for inserted names/numbers/terms. The
  session/frame/composer carry these beside UTF-8, not as control characters in
  it. Grid requests include their offset into the source. The backend inserts
  private isolate controls and maps every shaped endpoint back to original
  bytes, retaining the unchanged reveal clock and native anchors. Cache keys
  include ranges, direction and slice offset. Browser carets, interface font
  coverage and all-script visual qualification remain separate requirements.
  CRLF is one hard break; CR, NEL and U+2029 terminate paragraphs without
  rasterizing separator glyphs. U+2028 splits physical lines inside the same
  resolved paragraph, retaining its base. Both bidi and ordinary Latin requests
  containing these separators use the shared run-layout path. Fixed-field
  authoring rejects raw Unicode separators in favor of explicit structural
  operations; Go/C validation agree on that boundary.
- Font resources cross the renderer boundary through `ArFontResources` (ABI 1),
  not filenames. `ArTextBackendConfig` and `ArTextPresentationFont`/host ABI 2
  carry ordered, nonzero `ArFontResourceId` values. `ArLocalizationFrame` ABI 26
  copies these identities, not paths or pointers. The desktop host resolves
  bundled identifiers and pack-relative members, registers immutable file
  snapshots, and injects the provider through
  `ArLocalizedTextPresenter_SetFontResources`. A port may supply memory/archive
  resources without changing game or rendering code.
  Each resource is bounded at 64 MiB; the desktop store allows 64 resources and
  256 MiB total, including retired resources still leased by backends. A failed
  registration or preflight retains the previous working selection. The game
  retires registrations on rejection, successful replacement and shutdown;
  rejected selections also discard the staged preflight immediately, without
  touching active surfaces or holding unused bytes against the host budget.
  The backend closes all sized fonts and streams before releasing its leases.
  SDL_ttf opens read-only memory streams over those same bytes for every raster
  size, so size-cache misses neither reopen files nor see mid-session edits.
  High-resolution shaping, per-character palette bands, mosaic sampling and
  surface caching are unchanged.
  Providers and their contexts must outlive every lease; these APIs run on the
  host/presenter thread. Resource IDs are process-local and never reused or
  serialized into saves. A copied frame borrows its IDs: an already-pinned
  backend can still present it after registration retirement; after both have
  retired (including a device reset), it retains the captured native text,
  without erasure masks or a dangling handle. Shutdown resets the game
  presenter, destroys the independent interface backend, then destroys the
  host store. Store destruction refuses live leases without invalidating them.
- System-interface text has a separate `ArUiTextRenderer` instance, injected
  through `SettingsOverlay_SetTextBackend`. The host resolves the bundled
  Noto Sans/JP files; the overlay owns the font instance and a 256-entry,
  16 MiB GPU text cache. Non-ASCII runs and non-English interface runs are
  truncated at grapheme boundaries and shaped together inside the overlay's
  cell envelope; normal English
  keeps the ROM/host bitmap atlases. Output-space text width queries share
  the same cached layout as drawing, so centering uses actual shaped width.
  Tint and position changes don't rerasterize. The renderer owns no game
  routes, settings, ROM state or filesystem policy; it uses the portable text
  backend and render-device contracts. Texture resets clear only its GPU cache,
  and a rejected font initialization retains the previous working instance.
  Final destruction precedes render-device teardown. Missing backend/resources
  retain the compatibility atlas; dynamic game packs cannot replace interface
  font dependencies.
- Interface paragraph wrapping and direct-edit fields share
  `localization/interface_text`: word-space wrapping honors explicit newlines
  and falls back to complete-grapheme breaks for unspaced scripts, with separate
  byte and cell bounds. Appends reject malformed UTF-8 atomically and truncate
  only at whole-cluster boundaries; Backspace removes the last cluster. These
  are interface operations, not the native game dialogue paging/scheduling
  contract. Developer syntax-highlight spans remain technical byte-oriented
  fields rather than translated prose.
- Host interface messages use stable IDs in `localization/ui_catalog`, separate
  from game language packs. English/French/German/Japanese columns are authored
  in `snesrecomp-go/internal/uicatalog/messages.json`; `go generate
  ./internal/uicatalog` produces the static, sorted C table. Validation requires
  every entry's four translations and identical named-argument sets. C lookup
  allocates nothing; `{arguments}` are substituted literally with bounded,
  all-or-nothing writes, never as `printf` format strings. The overlay's
  `settings_overlay_localization` adapter derives label/help/value IDs from
  stable setting keys without modifying `Settings_FormatValue` or serialization.
  `interface_language = en|fr|de|ja` (also `AR_INTERFACE_LANGUAGE`) persists
  independently of game content and font presentation. The Localization →
  Interface tab changes it live. A host without ready interface fonts displays
  English while retaining that preference. Reset/save feedback wraps in the
  description panel rather than sharing space with the section title.
  Application recovery dialogs use the same four-language catalog through the
  pure `SessionRecovery` formatter, not the font backend. The first fatal
  request retains a typed reason plus literal technical details. Startup,
  battery-save, graphics-reset/loss and audio-device failures have localized
  recovery instructions, titles and shutdown warnings; raw diagnostics stay
  in logs. The saved interface preference (or explicit environment override)
  selects the locale, with English before preferences are available. Formatting
  is bounded and preserves warnings when details are too long. Headless runs
  still avoid modal dialogs, and localization does not change save/retry policy.
  The overlay's independent trusted Noto stack adds Japanese, Arabic and Hebrew fallback
  faces so package metadata can be read before activation without loading an
  unselected pack's fonts. These bundled fallbacks do not implicitly extend
  a game pack's font stack or add new host UI locales. File hashes, glyph samples
  and nonempty distribution members are gated; no universal Unicode claim is made.
  Catalogs cover navigation, interaction feedback, all compiled setting labels,
  help and built-in enum choices, plus layer-editor captions. Coverage tests
  compare the real compiled registry's English text with the catalog; new or
  drifted descriptors fail the gate. Controller names are keyed by typed binding
  kind/code, while platform keycap/device names and user-authored values remain
  literal. Live Vsync captions read host-reported status, not requested settings.
  Manual-reader controls consume the same effective interface locale. Their
  pure `manual_caption` layout binds one-based page/opening numbers to the
  input owner's device/zoom hints and wraps complete graphemes within the
  output viewport. Integer scale can decrease to fit; each line is centered
  using the overlay's actual cached width. Page images, reader input semantics
  and technical load diagnostics are unchanged.
  Generated C strings encode UTF-8 bytes with fixed-width escapes, independent
  of compiler execution code pages; builder-only IDs are excluded from that
  table. The builder embeds the same validated catalog subset in an inert,
  HTML-escaped JSON bootstrap. Its explicit leaf/attribute bindings never
  rewrite input values, scripts or author metadata. Browser language changes
  persist in `game-assets/workshop-settings.json` independently of the game,
  without navigation or editor reload; font fallback is a fixed, shipped
  Noto Sans JP endpoint, not an imported-pack file service.
- Auxiliary layer editors expose immutable, resolved row metadata (room scope,
  depth strategy/magnitude/direction, band bounds/anchor and enum values).
  `settings_overlay_layers_localization` consumes that snapshot without parsing
  English display strings or re-reading live room/draft state. Original English
  row formatting stays available for tools and parity checks. Manifest plane,
  section and ROM-source tokens remain literal; host captions cannot change
  edit targets or authoring rules. Only visible rows and selected help are
  translated, not navigation/count probes. Label/value and help-heading budgets
  reserve their separate columns, including nonselectable notices.
- Renderer-backed F2/`AR_SHOT_AT_GF` captures read the final composited output,
  so scaled-HUD regressions include the host overlay. Pure headless/oracle runs
  bind no overlay surfaces and preserve the historical internal framebuffer
  and deterministic emulated state. Visual automation opts into the real
  compositor with `AR_HEADLESS_VIDEO=1`; add `AR_WS_HEADLESS=1` to exercise
  configured widescreen geometry (otherwise the oracle-safe authentic width
  is forced). The boot `[video-geometry]` and `[display]` diagnostics confirm
  which geometry/profile actually activated. This creates a hidden window backed by
  the same mandatory GPU renderer and D32 pipeline as an interactive run,
  without enabling input or frame pacing. Dummy/offscreen software video
  drivers are intentionally rejected because they cannot exercise SIM3D's
  visibility model.

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
  priority-aware RGBA pipeline; the manifest schema is backend-agnostic and
  survives unchanged. Do not add mode7
  entries for scenes with sprites over the canvas, and do not build OBJ
  promotion — that was evaluated and rejected as a paste-path special
  case.): canvas-space texture override rendered through
  the live matrix. The engine API (`PpuBindMode7OverlaySurface` +
  `PpuSetMode7Override`, `snesrecomp-go/runtime/src/snes/ppu.c`) samples the entry's ARGB art at
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

`AR_TILE_CENSUS=1` (src/dev/hd_tile_census.c) is the tile-pack sizing survey for
the `tiles` plane: a read-only per-frame walk of visible BG/OAM/Mode-7 tiles
that writes unique-tile contact sheets (`tile_sheet_<class>.ppm`), a JSONL
census, and a palette-variance summary to the run dir. First results
(boot/title + level1-action.rec): 806 unique tiles, only 15 with more than
one palette variant — (tile bytes + palette) identity is viable and packs
are small.

#### Click-driven scene inspector (2026-07-16)

The host scene inspector (`src/dev/scene_inspector.c`) is the interactive front end
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
(`src/dev/scene_asset_dump.c`). Unlike F2's framebuffer-oriented diagnostic
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

First shipped entry: the title logo. The title screen ($18=00/$19=00) uses a
Mode-7 BG1 logo band (no OBJ sprites); the intro swirl is a per-scanline
HDMA matrix animation on channel 2 that lands on the identity matrix
(`m7 = [0100 0 0 0100]`), with INIDISP fading `00 -> 0f`. The manifest gate
(`wram[0018]==0, wram[0019]==0, mode==7, m7==identity`) therefore keeps the
swirl authentic and swaps the artwork band x=[11,248) y=[27,122) — menu text
at y>=140 is never captured — on the settled frame. `AR_TITLELOG=1` prints
the per-frame gate inputs used to derive this signature. Parser/evaluator
unit tests: `tests/hd_manifest_test.c`.

The settled screen is not Mode 7 on every scanline: `$02:A92F` switches the
lower menu band to Mode 1 BG3 via `$7E:6000/$6800` HDMA mode/screen tables.
Enhanced title options use that existing BG3 text path, preserve the native
selector and do not claim the logo/copyright. See [dialogue-system.md](dialogue-system.md#action-hud-cards-and-title-options).

### 13.2 Stage-B implementation refinement (2026-07-12; retired by BH8)

The earlier `widescreen-bg` implementation proved the map-decoding idea but
did not isolate it: that branch also hle-replaced `$8C98/$8D68`, hle-wrapped
both streamers, and restored only selected DP scratch after calling `$B825`.

At that checkpoint, main kept all four original routines and placed the validated
margin decoder in `src/actraiser/actraiser_widescreen_bg.c`. Static audit of
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
Heim endpoints.

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
41 tests pass; historical natural-transition gaps remain explicitly tracked by
the project acceptance matrix.

The action-room scene authority is the accepted first-stage production source.
The finite provider publishes from the cumulative immutable ROM scene by
default; `AR_ACTION_ROOM_SCENE_HLE=0` snapshots the already-staged WRAM map and
definition table as the exact source control. Both paths use the same
`ActionBgWorld` atomic
publication, metatile classifier, virtual-band lookup, and PPU binding. The
scene adapter explicitly consumes the compressed asset's high-byte-first
metatile words; the ordinary WRAM decoder retains its low-byte-first contract.
The live native ring still preflights every authentic tile before ownership, so
the source control changes only the input under test without removing the
oracle.

This is not a whole `$02:B1F7` replacement. Its command dispatcher remains
active and owns the native ring, level/gameplay data, actor startup, callbacks,
audio and transitions. Its action-room command-7/6 CHR/CGRAM handlers and
command-5/4 background handlers are guarded CPU HLEs; rejected
non-action/raw/BG3/unexpected shapes execute the decoded native handler body.
A missing scene, dimension drift or publication failure falls back to live
WRAM and is counted in the shutdown provider summary. Promotion followed an
exact 12-target ordinary-entry A/B:
immutable-source manifest
`runs/bg-hle-matrix-20260822-133346.json` reports 19,522 sourced/bound
layer-frames, zero live fallback, and 18,216,295 zero-mismatch/zero-outside
preflight tiles. Its live-WRAM control
`runs/bg-hle-matrix-20260822-133427.json` matches all 204 captured artifacts
byte-for-byte. Natural run `runs/20260822-134834/` then covered 6,227 action
frames and four Fillmore room loads with zero live fallback, finite exit, tile
mismatch, or raster-register mismatch. Post-fix combined-gate run
`runs/20260822-140544/` proves the provider and diagnostic caches remain
isolated and stable through scanout. Finally,
`runs/room-scene-default-matrix-20260822.json` repeats all 12 targets with the
environment variable absent: all 19,522 provider layers use the room scene,
with zero fallback and the same 18,216,295 exact preflight tiles.

`ActRaiserActionBg_StageRoomSceneLayer` writes the active BG dimensions, page
map and byte-swapped metatile definitions while preserving every other byte;
`AR_ACTION_ROOM_STAGE_COMPARE=1` is the independent immutable oracle for the
live `$02:B363/$02:B3EB` result. The native checkpoint manifest
`runs/action-room-stage-matrix-20260822-v3.json` covers all 24 loaded layers and
166,496 bytes with zero mismatch. The redirected manifest
`runs/action-room-load-hle-matrix-20260822-v2.json` executes 24 command-5 plus
24 command-4 HLEs, produces 166,400 asset bytes, and remains exact across the
same 166,496-byte oracle, native ring, framebuffer, map, definition, dimension,
and provider hashes. `AR_ACTION_ROOM_LOAD_HLE=0` restores the native handlers.
The same-binary graphics A/B manifests
`runs/action-room-gfx-native-control-matrix-20260822-v1.json` and
`runs/action-room-gfx-hle-matrix-20260822-v1.json` execute 39 command-7 and 36
command-6 HLEs (320,000 bytes) and match all 204 complete artifacts exactly.
`AR_ACTION_ROOM_GFX_HLE=0` restores the native graphics handlers. The
same-binary video-profile manifests
`runs/action-room-video-native-control-matrix-20260822-v1.json` and
`runs/action-room-video-hle-matrix-20260822-v1.json` execute 12 command-3 HLEs
(336 record bytes) and likewise match all 204 complete artifacts exactly.
`AR_ACTION_ROOM_VIDEO_HLE=0` restores the native profile handler.

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
and two disabled samples. The later special-room closure uses the same
eligibility boundary.

The first special-room sweep adds an important boundary. Death Heim
`$0702-$0707` has eligible BG1/native-32x32 BG2 and passes 1,032,404 more
runtime comparisons; hub `$0701` and final `$0708` are deliberately native
32x32 scenes. A subsequent native route followed the real hub/victory loaders
through `0701 -> 0702 -> 0703 -> 0701 -> 0704 -> 0705 -> 0701 -> 0706`; eight
source activations and 6,646,861 in-world comparisons all matched. Its 364
finite exits are one explained `0705` BG2 frame: that decorative world is only
256px wide while camera X is 104, so the authentic viewport begins beyond tile
X 31. This is policy input for isolated repeat/clamp, not a decoder mismatch.
At this checkpoint the `0707`/`0708`/ending tail remained open, and Northwall
`$0608` was a rejected shortcut: its BG1 tile words matched the ring while live
CHR rendered as patterned garbage before the room self-exited. The later
natural fixtures close both gaps: `runs/20260822-180657/` reaches coherent
`0608`, and `runs/20260822-180704/` covers every Death Heim rematch, `0708`,
and the ending with zero register mismatch; paired default/native-control runs
match their frame and final-state artifacts. The rejected shortcut still proves
that BH2 tile-word parity cannot stand in for BH1 CHR residency or BH5
pixel/priority parity.

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
cannot be the only padding policy. `0401`'s `$0100`-wide BG2 contains several
cloud bands moving at different apparent rates. The exact path is now traced:
callback `$00:CDD9` invokes `$02:93DF` every game frame, builds nine BG2HOFS
HDMA bands at `$7E:6000` (counts `63,16,8,8,16,8,16,40,80`), and
`$02:96B6` points channel 2 at `$210F`. `0402/0403` do not use that raster;
they cycle their four authored BG2SC pages every five frames. Reflection
reverses slope and apparent motion at each authentic-screen edge, making the
centered cloud field tear visibly from both margins. For this act,
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

Diorama now uses that same measured boundary as a depth split. BG2 metatile
rows 0-8 (screen `y=0-143`) move into a room-authored virtual plane at focal
`z=0.5`, while row 9 onward remains on ordinary BG2 so depth of field can still
soften the animated water independently. Because `0701` deliberately stays on
the native background path, this split is completed on the isolated capture
rather than by enabling the world-map provider. The seven red-eye ornaments
are a contiguous 26-piece priority-2 OAM group; their complete position,
attribute, X-high, and size signature is validated (tiles remain animation),
then only their winning pixels move from OBJ2 to the same focal face plane.
This seats the eyes in their sockets under every Diorama transform without
changing OAM or flat presentation. The policy requires the face-scene BG2SC
page `$70`; the post-final `$74` sky variant cannot inherit it.

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
immediately when the sky pages become active, and only in that post-handoff
state changes BG2's fixed horizontal extent from `0/0` to the live-tuned
`128/128`. Progress `$0347=7` alone therefore leaves the face scene clamped
with its lower Repeat band, preventing repeated faces before the fade. Edge
selection and extent remain separate. Direct testing on 2026-07-14 confirmed
that the page handoff occurs invisibly during the black frame; a fresh
natural-tail pixel fixture remains desirable for visual acceptance of the
promoted `128/128` extent.

Death Heim raw maps `$02-$07` (`0702-0707`) begin with the historically
validated narrow-BG2 Repeat classification, then apply source/edge-guarded room
tunings to the finite rematch art. Rooms `0702-0706` promote viewport BG2 to
Clamp/fill with available horizontal and vertical extent. Undersized `0703`
also mirrors its world-backed BG1 with a fixed `80/80` horizontal cap. `0707`
is the finite exception: viewport BG2 uses its decoded world edge with normal
motion, fixed asymmetric `100/96` horizontal extent, and available vertical
extent. BG1 otherwise retains the canonical world/fill/available policy.
These promoted policies supersede the 2026-07-14 provisional all-Repeat visual
classification while retaining it as the guarded fallback if a room's decoded
source topology changes.

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
cloud/snow policy. Aitos `0401-0403` and Northwall `0601-0605` and `0608`
classify the complete narrow BG2 as cyclic Repeat, so every authentic and
synthetic top/bottom row already preserves its motion direction; they do not
need a `y0=0` band. Tuned Aitos room `0405` likewise uses viewport Repeat/fill,
but bounds that backdrop to a fixed `128/128` horizontal extent while leaving
its vertical extent available. Death Heim rematches `0702-0706` instead
promote their finite viewport BG2 to Clamp/fill with available horizontal and
vertical extents. The undersized `0703` additionally gives world-backed BG1
Mirror/fill with an `80/80` horizontal cap. Room `0707` is the rematch
exception: viewport BG2 uses the world edge with normal motion, a fixed
asymmetric `100/96`
horizontal extent, and available vertical extent. Rematch BG1 otherwise keeps
the canonical world/fill/available policy. The special endpoints stay
independent: hub `0701` keeps its fog-band policy and final arena `0708` keeps
the intentional native RawWrap policy for its two-plane raster scene. The
planner test lists every cyclic member rather than only range endpoints, and
the real-PPU fixture now proves same-direction cyclic sampling on a synthetic
top row as well as the Bloodpool bottom band.

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

#### Additive planes and authored depth copies

`diorama_plane_additive_mask` is a compositing contract for one resolved TS
source pixel. It is not a material flag that can be copied across arbitrary
host geometry. `stack`, `voxel`, and `thick` submit additional meshes using the
same texture, and the current compositor assigns `SDL_BLENDMODE_ADD` to every
one of those submissions when the source plane is additive.

The failure is measured in `runs/20260811-145909`, gf2097. A live Marahna
`bg1 = voxel:0.18 slices:12 dir:backward` edit turns the intended
`main + BG1` operation into eleven solid voxel-copy additions plus the original
face. Voxel copies use uniform shade 0.88 and full alpha, so a fully overlapping
pixel receives roughly `main + (1 + 11*0.88)*BG1` before channel saturation.
Red and green clip first in that palette, producing the white/yellow image and
overlap-dependent bands seen in the snapshot. This is compositing amplification,
not corrupt CGRAM or texture data.

Per-copy intensity scaling is not a correct repair: projected overlap varies at
silhouette edges and across depth. The faithful design is to build the complete
extrusion into an intermediate target with non-additive internal occlusion, then
submit that flattened result to the additive pass exactly once. Until that path
exists, multi-draw depth strategies (`thick`, `stack`, `voxel`) are unsupported
on additive planes. Single-submit placement/shape controls (`z`, `rake`, `bow`)
do not multiply the colour-math contribution.

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

### 13.5 Scoped Diorama skyboxes from stock ROM assets (2026-08-12)

A room byte is sometimes too coarse for presentation policy. Aitos map `$04/$02`
contains both a cave and the waterfall screens; replacing the skybox
for the entire map would fix one area by damaging the other. The exact camera-
local three-row waterfall-platform signature already captured for spray effects
is therefore also the section discriminator. `FrameSlot` carries that immutable
result as `kDioramaLayerSection_AitosWaterfall`; present-time code never rereads
live WRAM or guesses from camera coordinates.

`diorama-layers.ini` sections may refine a room with a named suffix:

```ini
[layers:04:02]
bg1 = voxel:0.08 slices:16 dir:backward

[layers:04:02:waterfall]
backdrop = source:rom-04-01-bg2
```

Resolution applies the base room first and the active section second. Thus the
waterfall keeps the room's BG1 shape while changing only its skybox; the cave
sees the base record alone. For manifest compatibility, the source field lives
on the `backdrop` record, but it is not the Backdrop plane's texture. The Layers
menu therefore labels it **Skybox source**, shows the currently published scope,
and resets only that scope. Backdrop `alpha`, depth, order, and the layer toggle
still control only the residual in-box plane. In particular, `alpha:0` hides
that plane without disabling a selected skybox source; the global Diorama
skybox mode controls whether any skybox is drawn.

The Source enum is a ROM-wide action-background catalogue. `captured` uses the
current room's captured BG2; `rom-GG-MM-bgN` selects BG1 or BG2 from any valid
action map (98 combinations across the 49 maps). `DioramaRomBackdrop_LoadActionBg`
walks the stock `$05:8000` asset script, replaying earlier entries from the same
act so inherited maps are deterministic even when selected from another level.
It decompresses the selected map, word-swapped metatiles, character banks and
palettes into an opaque 256×256 first-page host image. Before character lookup,
the rasterizer reproduces action setup's tile-word transform: `$02:B6D3-$B6F6`
installs mask `$ECFF`, then `$02:B4E8-$B54C` merges attribute byte `$10` for BG1
or `$01` for BG2. This is required even when every decoded asset byte is exact:
without it, `rom-04-01-bg2` selects tile bank `$000` instead of `$100` and draws
unrelated cave/lava art. The old `aitos-sky` token
is accepted as a compatibility alias for `rom-04-01-bg2`, but saves use the
generic token.

The HLE never writes emulated WRAM, VRAM, or CGRAM and does not depend on visit
order. Decode is lazy and the cache key is the complete source identity, so a
Source edit replaces the texture immediately rather than retaining the previous
room's art. Source resolution happens before the far-background pass, and the
ordinary SDL skybox quad draws the 256×256 texture with horizontal wrap and
vertical clamp. This uses the same renderer/back-end and optional blur pipeline
as captured BG2 and introduces no new shader format. Decode or texture failure
falls back to the captured BG2 skybox. Renderer reset destroys the texture and
recreates it lazily from retained ROM
bytes. The editor records the exact live subsection in each row and seeds a
first scoped edit from its resolved inherited source, preventing a changing
camera scope from saving to the wrong record or displaying stale base state.

Run `20260812-220252/snapshots/snap_01_gf18194` exposed the original routing
mistake: logs proved that changing `rom-04-01-bg2` to `rom-03-04-bg2` decoded a
new texture, yet the surround remained the live waterfall because the named
texture had been submitted only as residual Backdrop geometry. The production
path now queries the resolved Backdrop record before drawing the skybox. Pure
regressions pin source resolution even when Backdrop alpha is zero, plus the
ROM-page widescreen repeat range.

Run `20260812-222309/snapshots/snap_00_gf7605` exposed the missing native
tile-word transform after routing was corrected. The fixed first-page tilemap
matches the live `$04/$01` BG2 VRAM publication in all 2,048 bytes, and its
65,536 output pixels match when rendered through the same decoded character and
palette assets. The synthetic room-script regression independently pins the
mask and both layer attributes.

## 13b. Simulation-town 3D presentation (pointer, 2026-07-22)

The enhanced town renderer is a presentation layer built on top of §11's
Mode-1 pipeline rather than a change to it. Town simulation is **ordinary PPU
Mode 1, not Mode 7**, so the
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

- Object height/anchor policy and composition identities:
  [sim-object-catalog.md](sim-object-catalog.md)
- Host seams (classification, height easing, shadow mask, shadow blur, rim
  light, billboard depth order, tuning handoff, picker build switch, D1 trace):
  [SEAMS.md](SEAMS.md) "Sim 3D presentation seams"
- Player-facing stage toggles and tuning dials:
  [manual.md](manual.md) "Simulation 3D"
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
town ground quad -> D32 background models -> authored actor/selection bands`.
Mountains, buildings and trees share one transparent SDL_GPU color target and
one `D32_FLOAT` depth attachment. Their visibility is therefore resolved per
fragment from projected clip depth, independent of CPU submission order. The
ground stays an earlier opaque base with no competing coplanar draw, while
flying actors and interaction feedback remain explicit overlays by design. The
canvas is opaque throughout, so it never punches a hole in the underlay beneath
it.

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
culls, which is fixed by hardware (see §13c).

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
inspection. `src/actraiser/actraiser_widescreen_sprites.c` carries `_Static_assert`s
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
mismatch on the frame (it reports rather than drops — the
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
correctness risk. The gate no longer drops the frame to the
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

**The ground-plane horizon is never on screen.** Across the supported SIM
pitch range (-1350..-575 mrad) the vanishing line remains outside a 224-row
viewport. What reads as sky in frame is where the ground *data* runs out, not
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

Right-drag in Dynamic Cam adds a temporary orbit around its dedicated baseline
and returns on release; it does not rewrite the saved Free Cam orientation.
"Reset camera" restores the pose of the mode in use rather than
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

## 13h. World-navigation globe scene (updated 2026-09-08)

Current status: native detailed town ground/water animation, bounded mountain
backs and boundary joins, the overhead Aitos crater/lava cap, full orbital
inspection and raised-geometry-safe Advent descent are implemented. Existing
town-entry/return menus and fade-covered loading are replay-verified; custom
seamless entry/exit and globe-under-SIM remain deferred. Physical input and
representative non-Metal acceptance are still open. The subsections below
retain the implementation history and historical measurements; later
follow-ups supersede their earlier open-work notes and performance numbers.
See [the focused code-quality audit](world-navigation-code-audit.md) for current
boundary, cache-failure, sanitizer and repeated-benchmark evidence.

Map `$09` reuses the owned developed world image but not the simulation-town
scene graph. `SimWorldNavigationScene_Build` publishes one 1024x1024 texture
serial and the bounds over world-map tile coordinates `[0,128) x [0,128)`.
Presentation tessellates that extent at one vertex per world tile, the same
topology in which one world tile is one simulation-town cell. There is no
captured town rectangle to extend, so this path has no
`kSimUnderlayMarginPixels`, resident set of six full-town canvases, separated
BG planes, or sprite cull window. All-town semantic object records are captured
separately from retained simulation data.

The captured signed 8.8 Mode-7 matrix remains the authoritative navigation
control. It maps screen deltas to texture-source deltas:

```text
[source x - focus x]   1     [ A  B ] [screen x - 128]
[source y - focus y] = --- * [ C  D ] [screen y - 112]
                         256
```

Scene construction inverts that 2x2 matrix once on the game thread and carries
the resulting six-value source-to-authentic-screen affine map in the immutable
`SimFrameData`. Presentation derives scale and heading from that map, and
uses the captured focus to rotate fixed spherical coordinates into the
oblique camera's frame. Movement no longer reshapes the terrain. The source point under the
Palace remains the projection origin, so adding perspective does not slide the
navigation cursor away from its destination.

`SimWorldNavigationTerrain_*` registers each audited 32x32 town heightfield at
the exact origin already proven by the developed-map compositor. Navigation
adds datum offsets `{0,3,4,4,0,4}` in Fillmore..Northwall order: Fillmore's
main plain is already 4 units, while Bloodpool is 1 and Kasandora is below 1.
This raises low plains to a shared world altitude without changing native town
terrain or flattening its local cliffs. Four cells on each side of town borders
blend the registered fields. Unknown lowlands continue that common plain,
rather than sinking into gaps between rectangular town windows.

Ocean-connected broad water is flood-classified separately from narrow rivers
and inland lakes. Its coastal envelope returns the combined field to sea datum
over four cells. Mountains are a separate additive layer because the native
town renderer also keeps mountains separate from its floor heightfield. Rock
coverage comes from the original world atlas's `$40-$45` palette ramp, not
luminance contrast: desert sand, snow plains and roofs cannot become peaks.
Distance inside connected rock silhouettes builds continuous ridges, shoulders
and saddles on both sides of town boundaries. `SimWorldMap_GeographySerial`
invalidates this field only on tilemap changes, not every water-animation tick.

A fixed stereographic chart now registers the authored continent on a complete
sphere. `SimWorldNavigationGlobe_*` maps source tiles to persistent unit-sphere
directions and exposes the inverse mapping, with an orthonormal focus/heading
frame. Palace movement rotates that frame, rather than warping every town
around a moving center. The conformal chart preserves local square blocks;
its metric also scales model height to retain the authored proportions. A
closed ocean sphere fills the uncharted hemisphere. This preserves native
bounded travel and destination coordinates. Temporary full-globe orbital
inspection is now implemented (acceptance notes below); seamless full-town
descent remains unfinished and entry/exit still uses the existing menus.
Per-vertex normals include both globe curvature and relief, so navigation light
is no longer a uniform tint.

The full map is mandatory content for this view. It is not conditional on the
town Ground projection or World map underlay stage toggles, and the independent
`AR_SIM3D_WORLD_NAV` master does not require `AR_SIM3D`. Compatible
lighting/weather/colour tuning is reused by two navigation-specific effect
gates: `AR_SIM3D_WORLD_NAV_LIGHTING` (on by default) and
`AR_SIM3D_WORLD_NAV_CLOUDS` (on by default). The latter reuses the town
renderer's density/tint language with no sprite-window hole or underlay margin.
Navigation now samples a continuous 3D noise field on the full unit sphere,
including unmapped ocean. Three 512x256 spherical banks share a padded 1024x768
atlas: longitude endpoints agree, pole rows are constant, and quad UVs unwrap
across longitude zero. Rigid field rotation supplies drift without sliding a
rectangular texture past the poles. A 48-ring/96-sector visible shell replaces
the old map-bound 48x48 mesh and its prominent rectangular coverage edge. Light
direction/elevation, shadow darkness/softness, cloud density/altitude, and
drift remain immutable frame values. Cloud bodies use `$0316` as a camera
height axis: the default deck is above the camera at near `$0206`, crossed
smoothly during zoom, and visible at middle/far `$040A/$0562`. Ground shadows
remain visible below the deck. Shadow displacement is in UV space over the
exact cached 128x128 colored ground mesh and the complete ocean sphere, so
coverage does not stop at the map edge. All shadow banks share one material
draw; bodies share another and fade only at their true spherical tangent.
Fully hidden back-side ocean shadow quads are culled, and cloud coordinates
are reused across softness samples. Ground coordinates also remain cached
while the field rotation is unchanged. Cloud/body/shadow settings stay live.

Navigation uses its own deep-navy space gradient and deterministic starfield;
the close-town sky is unchanged. A blue atmospheric rim encloses the globe,
and aerial haze remains blue rather than blending terrain into black space.
The cloud deck and outer air envelope derive from the cached global terrain
maximum, including the landscape-height multiplier and separately authored
native mountain rise, rather than the old fixed
four-unit plain. Clouds clear that maximum by at least 0.35 world tile and the
outer shell clears the clouds by another 0.75 tile. The common sea-relative
datum prevents the shell from changing size as focus moves between towns.
Its blue interior continues down to the ocean silhouette without a detached
halo/black gap. Regression coverage checks projected terrain enclosure from
all six towns at 0%, 50%, 100% and 150% landscape height.
`$0341`
provides the authoritative active location: `$01:B6CA` selects one of seven
256x256 source regions from ROM table `$01:B73C`, the same selection used for
the label/destination. A world-space mesh keeps that region fully sharp and
lit while blending the existing downsampled mip and backdrop haze over the
surrounding world. `$01:B6CA` clears `$0341` before scanning; when the Palace
is outside all borders, zero removes the clear-region cutout and the complete
world remains hazed.

Navigation now uploads a 2048x2048 Scale2x reconstruction, with compatible
shades feathered into the original world material at boundaries. Different
live materials always win: the pristine red lake must never bleed into a
cleansed blue Bloodpool shoreline, nor sand back into reclaimed land. This improves
sampling but is not six native 512x512 town canvases; reconstructing every
town's tileset, palette, metatile and animation at full native fidelity remains
open work.

`SimWorldNavigationTowns_Capture` publishes the shared voxel identities for
houses, factories, windmills, bridges, sanctuaries, landmarks and per-cell
foliage. Presentation uses the actual town model cache, proportions, regional
palettes and connected-tree variants, with projected-size LOD and a shared D32
colored terrain/ocean/model depth pass. There is no separate generic box/pyramid proxy
renderer. Building light is deliberately restrained to retain palette identity
at globe scale. Fields and unsupported plot kinds retain their developed map
art. Source-vs-footprint offsets keep angled town structures registered on the
top-down map without baking an angled screenshot into the globe.

Ground and models must use the same native `$7F:6B18` town-enable words.
Active structure records can survive a progress reset even though the loader
clears that town's ground. Never infer an unlocked town from those stale
records. Populated-save visual replays use
`tests/fixtures/sim3d/world-navigation-settings.ini` with `save_edit_armed=Off`,
not the sim-actions fixture that resets five towns to Act 1. With a temporary
copy of `backup-testing-2.srm`, the correct loader publishes 2623 changed world
tiles; the reset fixture published only 551. The original save is untouched.
Regressions cover stale records, rock-vs-sand/snow classification, town-boundary
continuity, elevated plains, ocean/inland-water distinction and wave-invariant
relief. The remaining orbital-inspection/descent work still requires acceptance
testing; the performance follow-up below covers the current navigation view.

#### Navigation performance follow-up

`AR_PERF=1` now includes navigation artwork upload, terrain projection, authored
model compilation/shading, model projection, depth submission and weather in
the existing portable SIM performance counters. Measurements use the same
populated temporary save, 1792x1344 output, Quality preset, 100% landscape
height, all cloud layers and the gf400-800 navigation tour.

The old Debug path (`runs/20260907-214746/`) spent about 54ms per stationary
frame recompiling models, rising above 200ms in flight. A 512-entry cache was
thrashing across several towns. The shared CPU cache now reserves working-set
headroom before any borrowed model pointers are obtained. Sixteen-way sets
reduce collision churn; storage grows on demand, is bounded at 8192 entries,
and is released by the existing resource reset. Allocation failure preserves
the smaller functional cache. This exchanges bounded CPU memory for retention,
without introducing backend types or changing the runner/frame ABI.

Cloud layers share four projected 49x49 grids (three shadow offsets and one
deck) per presentation instead of projecting every repeated quad corner in
every layer. Layer UVs, opacity, triangle count and wind remain unchanged.
Model columns reuse exact spherical normals at repeated XY coordinates;
position and depth use one matrix projection. Ground color and depth share
one vertex projection, cached until camera, viewport, light or geography
changes. Scale2x skips redundant blending and per-neighbor bounds work while
retaining the same output, checked against a scalar reference at every texel.

The optimized `play` build (`runs/20260907-215835/`) measured 67-99 fps through
the populated tour, around 8.3-13.6ms presentation time in the logged windows.
This is not an apples-to-apples compiler comparison with the old Debug
numbers: the final Debug replay (`runs/20260907-220013/`) separately measured
20-40 fps / 24-48ms presentation time, up from 3-8 fps before optimization.
The play build
still has a roughly 212ms first-entry compilation/upload hitch. Persistent
GPU geometry and staged warm-up are future opportunities; neither reducing
model detail nor dropping clouds/objects was used to obtain that initial result.

#### Overview LOD and opaque occlusion follow-up

The navigation LOD policy now biases toward the actual town **Low** models,
not replacement proxies: projected footprint below 32 pixels uses Low,
32-64 uses Balanced, 64-96 High, and 96 or more Ultra. The global town detail
setting remains an upper bound. Existing viewport, distance and subpixel
object rejection runs before model compilation; surviving geometry is
depth-tested per fragment, not CPU occlusion-query culled. The navigation
test checks that an overview-sized factory hits the shared Low model cache
even when the global setting permits Ultra.

Previously, only an invisible copy of the terrain participated in D32 while
its colored surface and ocean used painter-ordered 2D draws. That let rear
slopes paint through nearer slopes and omitted the ocean from building
occlusion. Navigation now batches the **colored** ground, mountain relief,
ocean and authored models into one depth target. Ground blur and regional
haze use the same projected vertices/depth with depth writes disabled, so
the old rear-slope haze cannot paint through a foreground ridge either.
The fixed-coordinate sphere follow-up below also moves clouds into shared depth;
the atmospheric envelope remains a backdrop silhouette.

Ground and downsampled blur atlases have separate pass-owned material slots
from the native town mountain atlas. The existing mountain upload wrapper
and layer IDs are preserved; the portable contract exposes ARGB pixels,
rectangles and semantic layers only. Native texture/transfer ownership stays
in the backend, and all slots are released by its resource reset. No shader,
runner ABI or frame-layout change was needed for this depth follow-up.
The GPU integration test reverses overlapping terrain submission order,
checks hidden/visible buildings and mountain atlas isolation, verifies
equal-depth haze and occluded overlays, and repeats across resource reset.

The same populated play tour (`runs/20260907-221650/`) now reports 104-108 fps
in the steady flight windows, with 5.9-8.0ms presentation time at 1792x1344.
This includes all cloud layers and uses the newly permitted overview LOD
policy. Bloodpool and Kasandora captures at gf500/gf700 verify occlusion and
retained town appearance. These are local Metal measurements, not claims
of tested performance on every supported GPU backend.

#### Fixed-coordinate sphere and low-end controls

The current sphere tour (`runs/20260907-223456/`, gf400-2200) visits all six
town regions with fixed geography, a closed ocean, tangent-bounded atmosphere,
depth-tested clouds and exact-ground cloud shadows. At 1792x1344 on local
Metal, the play build measured 97-105 fps / 7.1-9.2ms presentation windows.
Cloud shadows now cost more geometry than the former painter overlay, but
reuse cached terrain projection and batch into one draw. The chart transform
uses rational arithmetic instead of per-point trigonometric compression.

All optional stages can be switched at runtime. Defaults keep the complete
scene. Navigation-specific gates do not change settings inside SIM towns:

| Setting key | Work skipped when Off |
|---|---|
| `sim3d_world_navigation_lighting` | Terrain lighting, model relighting, color grading and directional cloud shadows |
| `sim3d_world_navigation_clouds` | Cloud atlas creation, cloud geometry and cloud shadows |
| `sim3d_world_navigation_cloud_shadows` | Shadow geometry/submission, while retaining visible cloud bodies |
| `sim3d_world_navigation_atmosphere` | Atmospheric envelope generation/drawing |
| `sim3d_world_navigation_towns` | Authored model compilation, projection and submission; live ground art remains |
| `sim3d_world_navigation_relief` | Heightfield preparation/sampling; the base globe remains spherical |
| `sim3d_world_navigation_ground_detail` | Native town-ground composition; overview artwork remains (native mountains can still need the shared source decoder) |
| `sim3d_world_navigation_mountains` | Native mountain scene/atlas preparation and projection; inferred overview relief remains |
| `sim3d_cull_haze` (shared) | Regional haze and blurred-ground layers |
| `sim3d_backdrop` (shared) | Space gradient/starfield |

The corresponding navigation environment aliases are `AR_SIM3D_WORLD_NAV_`
followed by `LIGHTING`, `CLOUDS`, `CLOUD_SHADOWS`, `ATMOSPHERE`, `TOWNS`,
`RELIEF`, `GROUND_DETAIL`, or `MOUNTAINS`. Ground and mountains share decoded
source variants; disabling one does not disable the other. The shared haze stage switch is `AR_SIM3D_CULL_HAZE_STAGE`, not
the numeric `AR_SIM3D_CULL_HAZE` strength. The town quality preset also caps
navigation models (Off disables them); Custom detail/style controls remain
available when only the navigation master is enabled. Town-only LOD/facing/
shading controls are not misleadingly enabled for a mode that does not use them.

`tests/fixtures/sim3d/world-navigation-low-settings.ini` provides a non-editing
low-end replay configuration. In `runs/20260907-224325/`, the same populated
gf400-800 tour retained live map/navigation with one depth draw, no cloud or
model work, and 1.8-1.9ms presentation windows (116-117 fps under display pacing).
This is a selectable reduced-effects profile, not the full-quality benchmark.
Tests pin disabled-stage work counts, Low model reuse, live restoration,
chart round-trip/conformality/rigid-distance invariants and GPU cloud occlusion.
The production replay `runs/20260907-224635/` additionally switches clouds Off
at gf420 and back On at gf620 without restarting: cloud time drops to 0ms,
depth batches drop from six to four, then both cloud batches return. Town 3D
is disabled throughout, verifying the navigation controls remain independent.

#### Native-resolution ground and perspective separation

Current sanctuary capture follows the complete retained tile signature, not
the town number: `$C2/$C3/$CA/$CB` selects the shared cathedral and the `$C0`
family selects the temple. This includes Marahna saves with the ordinary
cathedral. The same protected 2x2 footprint supplies clean model-owned ground.

The globe's chart-boundary fade applies only to pure ocean. All four corners
of every mixed shore/land cell remain opaque, including Marahna's southern
plots near world row 127. `SimWorldMap_CellIsOpenWater` classifies immutable
palette identities, conservatively including all wave variants, while the
presenter owns the fade and keys it to geography even with relief disabled.
See `docs/world-navigation-cache-audit.md` for the Marahna regression coverage.

Fallback checkpoint `a41b1e4` preserves the complete pre-native-ground globe.
The detailed-ground stage adds 16x16-pixel native terrain cells inside the
existing 2048x2048 atlas, without taking screenshots or requiring town visits.
`SimTownGroundArt` copies bounded immutable ROM assets at startup and lazily
decodes only requested variants. Stock map-00 asset scripts select raw
big-endian metatile definitions at file `$0C881A`, character banks at `$060000`
and `$064000`, and palettes at `$0E3B93` (temperate) / `$0E3D93` (Northwall).
The native `$02:C58C` gate selects the second character bank at development
tier 2 or greater. Traversal bit `$0200` is cleared before decoding, and
transparent character index zero preserves the existing globe texel.

The game-side town capture publishes row-major terrain for all six enabled
towns, the native tier byte and model-source ownership masks. These are value
snapshots, not WRAM/PPU pointers. Masked buildings and trees receive the native
plain tile; bridges receive their native river tile (`$41` north-south, `$3A`
east-west). When models are disabled, their overview glyphs remain instead.
Unresolved structure markers retain existing art. The atlas cache watches this
ground snapshot separately from Mode-7's image serial: source changes and live
toggle changes repaint; camera movement and animated model poses do not.

Mountain tiles are **not** ordinary ground material. Their authored slopes and
silhouettes already encode perspective. `SimBackgroundMountains_TileFlags`
excludes mountain cells from the native-ground overlay. Cliff faces are also
excluded unless the owned-corner geometry described below is active; only then
can their native art follow the correct inclined surface and closed walls.
The independent native-mountain stage below can replace
their overview relief and artwork with actual inclined geometry. Seamless
descent still requires a matched camera/terrain registration and visual
acceptance across boundaries, not just identical local mountain mesh formulas.
The native town CHR strip now animates through immutable phase variants as
described below; the existing world-water source remains independent. Aitos's
native lava palette pulse is now reproduced independently as described below;
other town palette effects and seamless animation-phase handoff remain open.

`actraiser_sim_town_ground_art_test` compares every pixel of all 256 terrain
metatiles in all four variants against the existing town-canvas renderer, using
synthetic assets in CTest; passing a local ROM path runs the same oracle on real
assets without committing ROM-derived fixtures. Composition tests cover exact
registration, feathering, padding, bridge restoration, disabled/unresolved
models, exclusion of mountain faces, and geometry-gated cliff artwork. Presenter tests cover
native-source revisions with an unchanged Mode-7 serial, tier changes, no
camera-triggered uploads and live toggle restoration. The ground addition
raised the suite to 142 tests; the mountain/cloud follow-up below raises it to 143.

The populated-save six-town tour `runs/20260907-231919/` covers gf400-2200
with native ground enabled; `runs/20260907-232440/` is the otherwise matched
overview-only control. Both use the play build, local Metal at 1792x1344 and
all effects. The 18 steady presentation windows average 8.36ms with native
ground (7.3-9.3ms; 97-104 fps) versus 8.19ms without it (7.3-9.0ms;
98-105 fps). This is local diagnostic evidence, not a cross-platform budget;
the one-time entry frame remains about 105-120ms. Geometry and six depth draws
are unchanged. The native overlay shares the existing atlas upload rather
than adding a draw or a per-town GPU texture.

Live-switch replay `runs/20260907-232607/` disables detailed ground at gf420
and restores it at gf620. Its gf500 capture is byte-identical to the Off
control and gf700 is byte-identical to the On control, with no restart.
The original `backup-testing-2.srm` and isolated replay copy retain SHA-256
`480a8375b6255ad681202986640c5b06889458125ec0f82641a0e4fb425c0d45`.
Visual inspection covers the six town regions, native shores/paths and
snow, model ground cleanup and the still-overview mountain faces. Native
ground is an independently switchable integration step, not final mountain
or descent acceptance. These earlier timings precede the full-sphere cloud and
native-mountain stages below.

#### Shared native mountains and globe-wide weather

`SimBackgroundMountainMesh` now owns the existing town renderer's inclined
plane, tapered stack and fitted silhouette-wall construction. It emits local
coordinates and source UVs through a portable callback; town projection, lean,
live source lookup and crater effects remain with the town renderer. Navigation
uses the same complete audited object stamps from immutable row-major retained
terrain, copies native cutouts with the same semantic silhouette masks, and
retains Low faces/walls for the overview. Native volcano rise remains 1.12x.
Unknown topology keeps its prior overview fallback rather than guessing stamps.

The native scene owns a separate 512x512 globe mountain atlas. Its appended
`WorldMountain` depth material can coexist with the active town's `Mountain`
material, avoiding an atlas-ownership collision during future combined views.
The game/runner ABI and backend texture handles do not enter these modules.
Native source/tier changes rebuild; camera motion and Mode-7 water animation
only reproject. Disabling the stage releases its CPU scene and restores ground
art/height ownership. Allocation or upload failure keeps the overview surface.
The original inferred mountain bump is removed under accepted native footprints
and a feathered shoulder, and mountain cells receive clean native ground/snow
so the old perspective stamp is not doubled underneath the raised model.

Native-mountain tour `runs/20260907-234057/` verifies the first integration.
The full-sphere weather tour `runs/20260907-234936/` covers gf400-2200 across all
six regions, including the previously uncovered ocean around Northwall.
Its late performance windows overlap a development build and are not an
isolated performance result.

The final isolated tour, `runs/20260907-235836/`, completes gf2210 with all
native stages and weather enabled at 1792x1344 on local Metal. Its 19 steady
presentation windows average 9.25 ms (7.8–10.3 ms), with 87–102 fps, seven
depth draws and roughly 1.00–1.05 million vertices per present. First-entry
maximum is 98 ms; steady globe-wide weather costs 3.14–3.19 ms. Seven exact
frame comparisons spanning all six regions and the final frame are byte-for-
byte identical to the pre-culling weather tour: opaque-planet shadow culling
and stationary-field ground-UV memoization preserve those rendered pixels.
These are local replay measurements, not a cross-platform frame-rate promise.

Live weather replay `runs/20260908-000105/` disables clouds at gf420 and restores
them at gf620. Steady cloud work becomes 0.000 ms and depth draws drop from
seven to five while disabled, then return to seven and 3.171 ms. Restored
gf700 is byte-identical to the isolated always-on tour. Both the source
`backup-testing-2.srm` and isolated retained save keep the SHA-256 above.

Drift-enabled replay `runs/20260908-000507/` completes gf1210, with visible
coverage beyond Northwall's map boundary and 84–102 fps (8.2–10.7 ms steady
presentation, 3.43–3.46 ms steady weather after the entry window). An additional
atlas regression caught and corrected one-texel phase drift in padded longitude
samples: endpoint-inclusive source rows repeat every width-minus-one intervals,
not every width texels. The presenter upload test checks that exact period in
every atlas row, in addition to the spherical-source continuity tests.
The post-correction drift replay `runs/20260908-000834/` also completes gf1210
with nine screenshots, 84–101 fps, 8.1–10.7 ms steady presentation and seven
depth draws. Debug/play builds and all 143 tests pass after the correction.

Tests cover local mesh height/contact and fitted walls across all four town
quality levels; native scene ownership, masks, atlas cutouts, malformed-town
fallback and resource destruction; row-major/paged classifier equality;
independent live stage restoration, source-tier invalidation, camera cache
hits and upload failure; and removal/restoration of inferred mountain relief.
The GPU test verifies simultaneous native town and globe mountain textures,
front/back depth relationships and reset. Spherical-cloud tests cover exact
longitude/pole continuity, UV seam unwrapping, bounded coordinates over every
hemisphere and density variation. Production-presenter tests verify full-ocean
shadow submission and unchanged independent effect gates. All 143 tests pass.

#### Native mountain boundary registration

`SimWorldNavigationMountainTransition` now builds a retained 12-cell transition
around accepted native mountain source footprints. The nearby inferred ridge
envelope approaches the native scene's maximum local rise through the globe
metric, then smoothly returns to the original broad-range profile farther away.
Town floor datums and ocean/river/lake heights are unchanged. Unsupported town
windows keep their original terrain exactly; a four-cell exterior fade approaches
that constraint without a new border step.

The material pass derives six-tone ramps from the already silhouette-masked
native mountain atlas, retaining its brown/snow palette identities. A categorical
`SimWorldMap_MountainShades` copy identifies only the developed map's authored
rock texels; it never classifies mountains from brightness or converts sand,
snow, water, roads or structures into rock. Overlapping town influences blend
continuously. Sparse 16x16 material patches cover only non-town rock cells, and
are blended into the existing world atlas without another GPU draw or material.
No perspective-authored mountain sprite is projected flat onto that surface.

Both ridge factors and material patches are rebuilt only on native-source or
geography changes. Camera motion and water animation reuse them. Disabling native
mountains, resource reset or transition-build failure restores the old surrounding
surface. The transition has no native renderer handles or runner-private state.
Tests cover mixed rock/non-rock cells, exact preservation of untouched pixels and
padding, multi-town colour interpolation, smooth height constraints, malformed
inputs, source destruction, unchanged inland-water heights, full height restoration
and geography invalidation without native atlas re-upload. All 143 tests pass.

Initial populated-save tour `runs/20260908-002006/` covers all six regions at
gf400–2200 with seven depth draws. Northwall/Bloodpool captures demonstrate the
snow/brown material transition and reduced surrounding ridge exaggeration.
Final isolated tour `runs/20260908-002550/` completes gf2210 after the smooth
fallback constraint refinement: 19 steady windows average 9.43 ms (8.3–10.5 ms),
86–99 fps at 1792x1344 on local Metal, with seven depth draws. First-entry maximum
is 118 ms. The transition adds CPU source preparation/composition, not a new GPU
batch; this is local measurement rather than a cross-platform performance claim.
Live replay `runs/20260908-002638/` switches native mountains Off at gf420 and
On at gf620. Disabled gf500/gf600 are byte-identical to always-off control
`runs/20260908-002715/`; restored gf700/gf800 are byte-identical to the final
always-on tour. Source/retained save hashes remain unchanged. Debug/play builds,
all 143 tests and `git diff --check` pass. The earlier overview-only checkpoint
`a41b1e4` remains available independently of this native-ground/mountain/weather
checkpoint.

#### Native cliff topology on the globe

The shared 129x129 overview grid could not represent the two distinct heights
at an authored cliff edge. It implicitly borrowed the southeast cell's corner
for both sides, turning town cliff bands into ramps. The portable
`SimWorldNavigationTerrain_TownCellCorners` now evaluates the same world datum,
coast, mountain contribution and four-cell town-border grading while keeping
the requested town cell's exact corner ownership. Town terrain data is unchanged.

`SimWorldNavigationCliffs` retains sparse replacement caps and closed skirts;
the six stock towns require 167 caps and 155 skirts, not a second full mesh.
Shared-grid cells remain unchanged elsewhere. Opposing height intervals use
`SimTownTerrain_ClipVisibleHigherEdge`, the town renderer's crossing-edge rule,
so a height reversal cannot create a self-crossing wall. Skirts borrow the
authored face's rock material, never the higher plateau's grass; cave skirts
sample the stone jamb rather than smearing the dark entrance. Cap UVs have
half-texel insets to prevent opposite-side material bleed. Native face artwork
uses the existing immutable ROM provider and 2048px atlas.

Presentation replaces the corresponding grid cells in **every** ground pass.
Opaque D32, cloud shadows, blur and haze reuse identical projected vertices;
model anchors sample the owned cap heights. Geometry, projection, and spherical
cloud UVs are cached separately. The atmosphere bound includes all owned
corners and restores when the stage is disabled. There are no new material
passes or backend dependencies, no game-state writes and no new ABI settings.
The existing detailed-ground and relief controls gate this stage; zero
landscape height, disabled towns, missing art and resource failure preserve
the overview fallback. Camera and water animation do not rebuild topology.

Verification: all 143 tests pass, including six-town corner ownership,
bounded sparse geometry, closed/non-crossing skirt endpoints, conditional
native face composition and exact opaque/effect vertex correspondence through
live ground/relief toggles. Debug and play builds pass. The isolated populated
save tour `runs/20260908-004737/` covers all six regions at 1792x1344 on local
Metal: 19 steady presentation windows average 9.52 ms (8.2–10.8 ms), 85–101 fps,
and seven depth draws; first-entry maximum is 109 ms. This is close to the
9.43 ms pre-cliff checkpoint, not a claim of a performance improvement.
Live toggle replay `runs/20260908-005150/` turns detailed ground Off at gf420
and On at gf620. Disabled gf500/gf600 are byte-identical to always-off control
`runs/20260908-005235/`; restored gf700/gf800 are byte-identical to the always-on
tour. Source and isolated-save SHA256 values remain unchanged. Checkpoint
`0b66d88` retains the earlier native-ground/mountain/weather integration.

#### Native town-water animation

All six stock SIM asset scripts select video profile 2. Its `$24/$08`
configuration at file `$01098D/$01098E` selects four `$100`-byte CHR slices
on an eight-game-tick cadence. `$02:BAF5` retains the loaded sheet at
`$7F:B800`; `$02:BC56` selects a slice and `$02:AF30` uploads it to VRAM
word zero. The globe art provider reproduces that bounded replacement from
its owned immutable character banks, not another town's live VRAM or DMA state.
Only characters 0–7 animate. Stock affected metatiles are `$25/$2E/$2F/$35/$37`
(water/shore), `$60/$68` (waterfalls) and `$B4`. Other river characters are
static and are not given invented texture scrolling.

`SimTownGroundArt_AnimatedMetatile` lazily decodes only affected metatiles for
the requested native character/palette variant and phase. Phase zero and
unaffected cells retain the static cache; all extra stock phases/variants
together need at most 96 KiB. Unknown profile bytes or phase allocation failure
keep static native art. Navigation selects `((uint16_t)(game_frame - 1) / 8) & 3`
from the captured game clock. This preserves native cadence, not the phase
offset of the last resident town; seamless descent will need explicit handoff.
Detailed-ground Off disables this stage. No new settings/frame ABI fields,
GPU material passes, backend dependencies or emulated-memory access are added.
Phase changes invalidate ground pixels, not cliff topology, mountain textures,
terrain heights or model geometry. Camera movement and frozen clocks do not
cause extra animation uploads.

Verification expands the independent town-canvas oracle to every pixel of all
256 metatiles, four variants and four phases, using synthetic sources in CTest
and the local ROM separately. Six-town composition checks isolate changed
water texels, preserve static shore quadrants and destination padding, and
reject invalid phases without output changes. Presenter checks cover the
eight-tick boundary, phase wrap, frozen clocks, disabled-stage work and stable
mountain uploads/geometry. Native SIM snapshots in `runs/20260908-011005/`
at gf900/908/916/924 independently match ROM phases 3/0/1/2 byte-for-byte;
the retained `$7F:B800` page matches the original character bank in all four.
All 143 tests pass with macOS GPU access, and Debug/Release builds pass.

The isolated six-town Metal tour `runs/20260908-011739/` at 1792x1344
averages 9.48 ms across 19 steady presentation windows (8.2–10.6 ms),
85–101 fps, and seven depth draws. Ground uploads remain approximately one
per eight game ticks; adding town phases does not double the existing world
water upload cadence. This is comparable to the preceding static-town-water
cliff run, not a claimed speedup. Toggle replay `runs/20260908-011850/`
disables detailed ground at gf420 and restores it at gf620: gf500/gf600
match the earlier always-off control `runs/20260908-005235/` byte-for-byte,
and gf700/gf800 match the new always-on tour. Both source and isolated-save
SHA256 hashes remain unchanged. These are local checks, not low-end or
cross-platform performance guarantees.

#### Native ridge backs and protected town ground

The globe's native inclined fronts and fitted side walls previously left
the back of each ridge open. Rear slopes now follow the original per-column
opaque silhouette, using the same native transform, art and palette. The
front occupies 62% of the original stamp depth; the rear uses the remaining
38%, ending on the original silhouette rather than extending a reflected
peak into neighboring roads or water. Linear skyline runs merge within half
a native pixel and never cross an atlas-cell boundary.

Geometry is then clipped against retained town occupancy, including a different
town across a boundary. Only mountain ground without a reserved model footprint
can receive a rear slope. Disabled/unknown town windows and all non-mountain
cells remain protected. Polygons crossing a forbidden cell are clipped at the
cell edge; checking only their vertices would incorrectly span buildings.
Wholly safe polygons retain their original quad, avoiding needless subdivision.
Existing native fronts and side walls are unchanged. The optional mountain
stage still owns one atlas/material pass, and source/model-mask changes rebuild
the retained geometry through its existing cache key. No frame/settings ABI,
backend dependency, live WRAM read or native gameplay write is added.

Tests invert the rear transform against the independent silhouette masks,
check every ground cell spanned by each polygon, reserve alternating building
footprints beneath synthetic rock, and verify original front/wall vertices
remain byte-identical. The optional local-ROM/WRAM oracle in
`actraiser_sim_world_navigation_materials_test` checks the populated save without
committing derived assets. The six-region snapshot contains five accepted
mountain-bearing towns (Marahna has no native mountain objects): 3,509 existing
front/wall quads plus 3,936 retained rear pieces. This closes the rear surfaces;
it does not yet claim final geometric/material continuity with every surrounding
inferred range or unrestricted orbital acceptance.

The captured-data test also reserves Aitos's southern border as model
footprints: 207 Kasandora rear pieces originally crossing that row are clipped
away, proving that the originating town's mask alone is not the clearance rule.
All 143 tests and both builds pass. Six-region visual/performance replay
`runs/20260908-013623/` averages 10.03 ms across 20 steady presentation windows
(8.9–11.2 ms, 82–97 fps at 1792x1344 on local Metal), with the same seven depth
draws. The preceding checkpoint averaged 9.48 ms; this added geometry is not
being presented as a free performance improvement. Live mountain Off at gf420
and On at gf620 in `runs/20260908-013912/` restores gf700/gf800 byte-for-byte
against the always-on tour. Source/isolated-save hashes remain unchanged.
Checkpoint `1521524` preserves the state before rear closure.

#### Protected native-to-overview height joins

Native peaks now anchor on a separately sampled registered floor, excluding
the independently inferred rock rise. This prevents a join from lifting its
own native reference mesh. On the populated navigation snapshot the previous
double lift was small (72 native vertices, maximum 0.0221 world units); it was
not the main cause of the coarse range mismatch.

The optional transition samples opaque native mesh edges at shared town-border
grid vertices, converts their rise through the globe metric, and fits a
four-cell exterior rock band to those constraints. In this snapshot, 61
anchors constrain 304 vertices. Every adjacent town cell must be enabled,
recognized mountain ground with no reserved model footprint. Interior town
vertices, building/road/water cells, and unsupported towns keep zero join
weight. Thus even a shared corner cannot tilt a protected cell. Propagation
stays within supported rock; transparent atlas margins never seed a plateau.
Premultiplied targets avoid a second fade during interpolation. The presenter
converts physical native rise to relief units when the landscape-height
setting changes, preserving native proportions and the floor reference.

These are retained portable fields, not new draw passes or frame ABI fields.
Native globe normals, floor anchors and rise are cached across camera motion;
projected vertices are also retained for unchanged camera, viewport and
lighting. Allocation failure uses the same uncached geometry. Water animation
does not rebuild either cache. Tests cover exact anchor heights, zero influence
on all protected-cell corners, transparent/unsupported edges, live occupancy,
copied-field ownership, invalid input, landscape scaling, and exact projection
restoration after camera, lighting, viewport and native-stage changes.

Both builds and all 143 tests pass. Six-region visual/performance replay
`runs/20260908-020642/` averages 9.47 ms over 19 steady presentation windows
(8.0–10.2 ms, 88–101 fps at 1792x1344 on local Metal), with seven depth draws.
This recovers the small rear-closure cost in this run, not a cross-platform
speed guarantee. Mountain Off/On replay `runs/20260908-020858/` restores
gf700/gf800 byte-for-byte against the always-on tour. The local materials
oracle accepts a navigation-mode WRAM snapshot (`$18/$19=00/09`), using its
native tilemap only as diagnostic input; production retains HLE map ownership.
The broad inferred ridge profiles and low-resolution material remain visibly
different from native peaks. Shared-grid height constraints reduce selected
gaps, but are not a fully welded silhouette or final perspective acceptance.

#### Original-stamp exterior continuation experiment

Checkpoint `1ae188f` preserves the accepted original-art mismatch before this
experiment. The globe can now complete the missing east/west columns of the
same native mountain stamps, retaining their Low front transform, fitted side
walls, silhouette-following rear slopes, original texels and palette. This is
not a replacement proxy or a new synthetic mountain pattern. Original town
faces remain unchanged. New polygons are clipped against whole semantic rock
cells outside **every** registered town window; neighboring mountain cells are
also excluded because that town owns its own geometry. Water, non-rock and
unknown town ground cannot receive these continuations.

The addition is transactional: failure restores the original atlas, faces,
maximum rise and relief ownership. It waits for developed-map publication,
then follows geography and captured town-source changes. The cache separately
tracks developed availability, including a publication with unchanged pixels
and serial. Water animation and camera movement do not rebuild the scene.
There is no new frame/settings ABI field, backend resource type or draw pass.
The existing native-mountain switch still restores the overview fallback.

The populated snapshot adds 573 exterior faces (270 fronts/walls, 303 rear
pieces). The new physical footprint excludes the old boundary-height fit:
otherwise that inferred fill rises through the completed native slopes. All
61 previous shared-border anchors in this snapshot touch completed footprints,
so their inferred join weights become zero; the height-only join remains available at uncompleted
edges in other supported layouts. Broad inferred ridge silhouettes and their
lower-resolution material are still visibly different. This improves some
hard stamp cuts, not every mountain seam or camera angle.

Tests verify original face preservation, failed-addition rollback, original
rear-transform correspondence, complete polygon clearance, a live non-rock
lane through added slopes, unavailable/unchanged developed-map publication,
no-op fallback without rock and exclusion from height-fit ownership. The
six-region Metal replay `runs/20260908-022527/` averages 9.47 ms over 19 steady
presentation windows (8.0–10.4 ms, 87–101 fps, 1792x1344), with seven depth
draws. This is comparable to checkpoint `1ae188f`, not a claimed speedup or
low-end guarantee. Toggle replay `runs/20260908-022756/` matches its always-on
gf700/gf800 exactly; Off gf500/gf600 matches the pre-experiment fallback
`runs/20260908-020858/` byte-for-byte. Source and isolated save hashes remain
unchanged. Entry/exit continues to use the existing menus.

#### Incremental world/town animation uploads

Town water and the overview's waves advance together in normal navigation;
optimizing only an unchanged-overview town phase does not reduce live flight
work. Navigation now tracks geography separately from animated image serials.
An animation-only change reconstructs the affected cells from current source
pixels, then reapplies the native overlay, mountain cleanup and exterior
material patches only to those cells. The water mask includes both developed
and baseline wave tiles and their cardinal Scale2x neighbours. It does not
consume the shared world-map bake's dirty state. Town-only phase changes are
also supported, including reversed/wrapped clocks and static source variants.

Each rebuilt cell starts from its original feathered Scale2x background;
blending over the previous phase would accumulate shoreline colour errors and
leave trails when texels become transparent. The mask is portable cell data;
presentation groups horizontal/vertical runs for the existing regional atlas
upload API. More than 256 tiny runs switch to a 16x16 grid of upload blocks,
not lower-resolution art. Unchanged retained pixels inside those rectangles
are copied exactly. No extra persistent image cache, backend types, shaders,
settings or frame ABI fields are added. Geography/settings/source changes and
resource resets retain a full rebake; failed GPU updates invalidate the image
key so even a clock rewind retries with a complete upload.

Both builds and all 143 tests pass. Differential tests compare complete padded
2048px atlases against fresh full bakes through all phases, alpha changes,
feather edges, source-stencil boundaries and detail/model/cliff gates. Masked
mountain/material tests protect untouched pixels; the presenter maintains a
mock GPU atlas and checks every pixel after partial/coarse uploads and failed
upload recovery. Full flight `runs/20260908-025304/` matches all 19 gf400–2200
screenshots from the pre-optimization `runs/20260908-022527/` byte-for-byte.
The live mountain Off/On replay `runs/20260908-025406/` matches every gf400–800
capture from `runs/20260908-022756/`; source and isolated-save hashes are unchanged.

At 1792x1344 on local Metal, 18 steady presentation windows average 8.92 ms
(7.6–9.7 ms, 92–106 fps), compared with the earlier tour's 9.47 ms. Steady
artwork update time averages 0.764 ms versus 1.293 ms; each ordinary wave
upload is 12.125 MiB rather than 16 MiB. Seven depth draws and all geometry,
weather and image detail are unchanged. The 125 ms first-entry maximum still
includes the full initial build; this pass does not fix that hitch or establish
cross-platform/low-end performance guarantees.

#### Conservative weather viewport culling

The nine soft-shadow samples previously rebuilt and submitted every mapped
receiver, including terrain entirely outside the output target. Navigation
now retains per-vertex screen-edge outcodes alongside projected terrain,
native cliff and spherical shell vertices. A weather face is skipped only
when all four corners lie beyond the same edge, with a one-pixel guard band.
A face crossing the viewport is retained even if none of its corners is
inside. This works on the actual screen-space triangles consumed by the
depth pass, not an approximate world-space bounding sphere or horizon test.
All visible vertices, UVs, alpha, cloud banks, nine softness samples and
opaque/depth relationships stay unchanged. Ground/cliff outcodes follow their
existing geometry/projection keys; ocean and cloud shell outcodes follow each
new shell projection. No backend, settings or frame ABI changes are needed.

Presenter regressions independently derive visible receivers from the opaque
geometry's screen bounds, require all nine shadow samples for each overlapping
terrain/cliff face, and reject submitted fully offscreen weather. They cover
wide and close views, focus/pitch changes, output resizing, below-cloud views,
and disabled-effect controls. Both builds and all 143 tests pass. Six-town
replay `runs/20260908-030003/` matches all 19 gf400–2200 screenshots from
`runs/20260908-025304/` byte-for-byte. At the same 1792x1344 local Metal settings,
18 steady windows average 7.92 ms (6.1–9.6 ms, 93–109 fps), versus 8.92 ms
before this culling pass. Steady cloud work averages 2.47 ms versus 3.19 ms;
submitted vertices average about 836,000 rather than 1,049,000 per frame.
The seven depth draws remain. First-entry maximum is still 115 ms, not a
solved warm-up hitch. These deterministic comparisons freeze cloud drift;
they are not a moving-weather or cross-platform performance guarantee.
Live clouds Off at gf420 and On at gf620 in `runs/20260908-030315/` skip both
weather batches while disabled, then restore gf700/gf800 exactly to the
always-on capture. Source and isolated saves retain their original hashes.

#### Globe inspection and view-driven town visibility

`Sim3DCamera` owns a separate, visit-local globe orbit and zoom. Existing
right-drag/right-stick actions rotate the globe's orthonormal frame through a
full yaw turn and either pole; release returns the orbit to the native travel
view with the shared damped camera helper (0.65-second return parameter).
Wheel/triggers adjust distance within the existing 2–20 range, and
middle-click/R3 clears the visit's orbit and zoom. Leaving navigation or
disabling its master clears this temporary state. It works independently of
the town master and Free/Dynamic town mode without changing persisted poses,
native WRAM coordinates or save data. Retained-frame refresh also copies this
host-owned camera state while emulation is paused; the captured game scene
remains immutable. Orbit and zoom reuse the captured camera fields. Navigation
now uses an always-centered radial eye, so the former application-owned
inspection-centering blend is no longer captured or updated.

The Palace remains the original billboard, offset to its actual travel
location as the globe rotates and omitted on the far hemisphere. Destination
UI stays fixed. This is not a depth-clipped 3D Palace model or an automatic
town-entry gesture. In particular, geographic haze still follows the native
active-region selector rather than the inspection direction.

Town model selection now uses the same projected footprint/viewport thresholds
at zero and nonzero orbit. Native focus-distance cutoffs and active-label
promotion no longer make visible buildings appear or disappear when a drag
starts, ends, or crosses a destination label. The Low/Balanced/High/Ultra LOD
thresholds and user quality ceiling are unchanged. The compiled model bounds,
including roof overhangs and tree crowns, supply a conservative angular cap and
maximum radial height. A pure globe helper rejects the cap only beyond both
eye and object tangent horizons. Its occluder is inscribed below the inset,
faceted opaque ocean mesh, not the larger nominal sphere. Tall buildings at
the limb stay eligible; mountains and buildings still use shared GPU depth
for precise per-pixel occlusion. Fully hidden models skip face projection and
submission, though their cached geometry is still looked up for exact bounds.

Tests cover full rotations and poles, camera state isolation in both town
modes, reset/zoom limits, unchanged native navigation state, stable UI,
Palace return, zero artwork reuploads during orbit, and identical object
selection across tiny positive/negative/zero orbit and destination changes.
The horizon test uses an independent segment/sphere intersection oracle over
near/far eyes, multiple heights, cap widths and all angular separations.
`runs/20260908-032452/` confirms all 19 normal-travel screenshots (gf400–2200)
match `runs/20260908-030003/` exactly with the committed inspection camera at
rest, before the view-selection adjustment. Manual mouse/pad acceptance and
orbital GPU screenshot coverage were still pending at that point; the Mac was
locked, so algebraic/mock-render tests were not visual acceptance. The GPU
coverage follow-up below now addresses the rendering side of that gap.

The view-selection follow-up in `runs/20260908-032849/` completes the same
six-town replay and restores distant models that the old source-distance
cutoffs omitted (visible along the western limb from Marahna). Its 18 steady
windows average 8.53 ms (6.8–10.5 ms), compared with 7.93 ms (6.2–9.7 ms) in
the immediately preceding run. This is additional visible geometry, not a
performance win; first-entry maximum is still 122 ms. The unchanged save
hashes and deterministic clocks are verified. These runs do not yet prove
orbital horizon culling on the real GPU or its cross-platform performance.

#### Full-presenter orbital GPU verification

`actraiser_present_world_nav_gpu_test` links the actual navigation presenter,
art providers, authored model compiler/cache, production SDL GPU backend,
shipped shaders and D32 pass. Only host time is frozen. Shared cloud-bank
constants and the unchanged `SimShadowLight` calculation now live in
`present_sim3d_environment.c`, linked by the game and both presenter tests;
neither test duplicates the atmosphere style. The new source is in the common
build manifest and the portable renderer-boundary audit. No live game input,
settings persistence, runner internals or save writes are linked into the test.

The normal CTest case needs no ROM: a synthetic green continent/blue ocean
pins real GPU near/far geometry, an authored factory visible above the front
surface but invisible through the planet, cloud cover over the far ocean,
both poles and mixed-axis orbit, exact return to the starting image, and exact
restoration after a GPU resource reset. Distinct synthetic Palace/UI colours
verify billboard motion, far-side disappearance, fixed UI and exact return.
Unavailable GPU hosts report skip code 77 rather than a false pass.

For local-art acceptance, the same binary accepts an explicit read-only 1 MiB
ROM and a 128 KiB WRAM dump taken during world navigation, plus an existing
output directory for PPM captures:

```sh
cmake --build build --target actraiser_present_world_nav_gpu_test
ctest --test-dir build -R '^actraiser_present_world_nav_gpu$' --output-on-failure
globe_capture_dir=$(mktemp -d)
./build/actraiser_present_world_nav_gpu_test ./ar.sfc \
  ./runs/20260908-032849/dump_wram.bin "$globe_capture_dir"
```

The local Metal run in `/private/tmp/actraiser-globe-gpu.JEUVij/` captures all
six enabled towns and 1048 semantic objects at an explicit frozen middle zoom.
Front, east, back, west, north, south and mixed-axis images render successfully;
`captured-restored.ppm` equals `captured-front.ppm` pixel-for-pixel. Inspection
of those images confirms native terrain/model rotation and cloud cover over
unmapped ocean. The native-art fixture omits Palace/text captures and geographic
haze to expose the geometry; synthetic markers test that composition separately.
This is rendering evidence, not a live mouse/pad or moving-weather acceptance
test, and it is not a cross-platform performance result. The images also expose
remaining camera framing work: the town-style aim places the globe low in the
viewport at some zooms. Source and isolated testing saves retain their hashes.
Both Debug/Release builds and all 145 tests pass. The extraction-only gameplay
replay `runs/20260908-034816/` retains all 19 gf400–2200 screenshots byte-for-byte
against `runs/20260908-032849/`; sharing the environment definitions changes
neither the normal travel image nor the chosen town model detail.

#### Centered radial navigation framing

Navigation always looks radially through the native travel location and globe
center. Its camera pitch and yaw are zero, independently of the persisted
town camera's oblique pose. The terrain under the original top-down Palace
sprite therefore faces the eye; centering a still-tilted town camera would
not fix that perspective mismatch. Native heading and zoom scaling remain,
as does visit-local inspection zoom. Close zoom can crop the globe equally
at opposite viewport edges; this is not an automatic whole-planet fit.

Manual inspection rotates the globe's frame beneath the same centered eye.
Release returns that rotation with the existing damped orbit helper; no
separate centering transition is needed. The obsolete focus-blend state is
removed from the host camera, `FrameSlot`, and retained-frame refresh. This
simplifies application-owned state without changing runner ABI, settings,
emulated coordinates, save data, backend formats, shaders or effect controls.
Horizon culling, GPU depth, clouds, atmosphere and Advent's global clearance
bound all use the same view matrix and eye, with no screen-space correction.

Navigation uses a **2x diameter** globe (96-tile chart radius), with the local
tile/relief scale and native travel coordinates unchanged. Focus frames,
terrain, authored models, mountain joins, clouds and Advent clearance use that
same metric. The native Palace retains its animation, brightness and top-down
orientation, but scales with the resolved camera distance: 75% at distance 3,
capped at native size nearby and floored at 35% for a readable distant marker.
Scaling is about the travel focus, preserving the authored cloud/platform crop
offset. Inspection moves it to the actual travel location and hides it on the
far hemisphere; the destination UI remains unscaled and fixed. This adds no
textures or draw passes. The separate enhanced Sky Palace backdrop retains its
oblique daylight horizon camera and selected 3x diameter unchanged.

Camera tests cover held-neutral input without redundant redraws, full-turn
wrap, invalid elapsed time, frame-rate-independent return, zoom/reset and
visit isolation. GPU tests require the atmospheric envelope to stay centered
within two pixels at distances 3, 5 and 10, with exact image equality across
five inherited town poses. Marker/UI masks, front/return restoration,
near/far building occlusion, all-six-town quality/effect toggles and raised
terrain Advent clearance remain covered. The oblique offscreen-anchor/visible-
roof regression now uses the still-oblique Sky Palace camera. Its fake depth
pass explicitly binds the supplied device context, rather than a previous
test's expired output-setup context.

Checkpoint `b9be38a` retains the superseded inspection-only blend, with its
historical captures in `/private/tmp/actraiser-globe-framing.Jhdjfe/`. Current
centered navigation evidence is in
`/private/tmp/actraiser-centered-navigation.2e0hx3/`; six captured Sky Palace
town views remain byte-identical to the selected 3x implementation. See
`docs/world-navigation-cache-audit.md` for validation and measurement limits.

#### Density-shaped atmospheric halo and cloud limb

Centered orbital captures exposed a broad, nearly constant-opacity outer ring.
The halo now follows the view ray's closest approach to the planet, normalized
between sea radius and the existing atmosphere radius. Its art-directed opacity
is `0.32 * exp(-1.5*h) * (1-h)^2`, clamped at sea level and zero outside the
envelope. This concentrates blue air near the surface and tapers smoothly to
space with zero slope at the outer edge. It is an inexpensive artistic profile,
not a physical scattering simulation. Cloud bodies separately fade with the
actual normal-to-eye cosine over the last 0..0.5 facing band, rather than a tiny
fraction of cap angle that collapses into a sharp projected rim.

Both profiles are pure portable scene helpers. The presenter evaluates them
once per camera-tangent ring (49 times per enabled shell), not per sector or
fragment. No new shader, texture, draw pass, setting, ABI field or frame-state
dependency is introduced. Ocean/terrain geometry, source art, native mountain
footprints, cloud UVs, shadow receivers and all shell heights are unchanged.
The whole-world maximum still sets cloud/atmosphere clearance; the change
softens appearance without lowering the deck into mountains or restoring map
boundary edges. Atmosphere and clouds retain their independent controls.

Pure tests cover finite bounds, monotonic falloff, endpoint smoothness and
invalid inputs. Presenter tests additionally inspect every atmosphere ring's
opacity while retaining the raised-terrain enclosure sweep. The real GPU test
checks the sky pixels beyond a synthetic ocean edge for monotonic falloff and
bounded raster steps, plus exact restoration after Atmosphere Off. Existing
cloud toggle, full-world coverage, camera, building-depth and UI tests pass.
Both builds and all 145 tests pass. Seven no-atmosphere/no-cloud synthetic
captures remain byte-identical to the framing baseline.

Native orbital comparisons in `/private/tmp/actraiser-globe-atmosphere.osdoqF/`
were inspected at centered front, both sides, far side, both poles and mixed
orbit. The six-town Release replay `runs/20260908-041820/` retains the same
seven depth draws and averages 8.46 ms over 18 steady windows (6.5–10.6 ms),
versus 8.51 ms (6.6–10.7 ms) in `runs/20260908-040418/`. This is comparable local
Metal cost, not a speedup claim; first-entry maximum remains 119 ms. These
captures freeze cloud drift and do not replace moving-weather or cross-platform
acceptance. Both original/isolated saves retain their recorded SHA256 values.
Live Atmosphere Off at gf420 and On at gf620 in `runs/20260908-042021/` changes
the intervening gf500/gf600 captures, leaves clouds active, and restores
gf700/gf800 byte-for-byte to the always-on replay.

#### Continued mountain edge limits and sampler validation

Checkpoint `ca36a38` preserves the next experimental edge field without
rewriting the earlier `b9be38a` original-art fallback. Continued native stamps
now constrain nearby inferred rock using their actual opaque, oblique edge
samples. Integer-aligned boundary sampling found no usable anchors in the
populated map: these fronts and rear contacts generally lie between vertices.
The replacement samples the closest edge inside the boundary vertex's four
audited rock cells, including the native atlas alpha and local globe metric.

Only the first completely unowned exterior rock vertices receive limits; a
four-cell connected-rock blend returns to the original inferred profile.
Every corner touching a town window, native replacement footprint or non-rock
cell remains unchanged. Roads, water, building reservations, original native
fronts/rears and their floor datum are not altered. Targets are copied and
premultiplied by weight before interpolation, then applied with a minimum:
the field can lower an oversized inferred ridge but cannot raise one through
native geometry. Disabling native mountains, invalid input or resource reset
disables the field through the existing terrain invalidation path. It adds
retained application-owned grids, not meshes, textures, draw calls, frame-slot
fields, runner ABI changes or backend-specific work.

The captured six-town geometry audit finds 79 limit anchors, 306 supported
vertices and 215 actually lowered vertices (maximum 1.1098 world units at
100% relief). Original exact town-boundary joins have zero usable anchors
after continuation ownership, and are not counted as successful matching.
The audit checks every protected footprint corner and the neighboring Aitos
building row behind 207 crossing Kasandora rear faces. Synthetic tests cover
fractional edge coordinates, transparent source texels, disabled towns,
non-rock barriers, copied fields, interpolation and invalid inputs. These are
local height limits, not a welded surface or a replacement for the visible
original-art silhouette/material mismatch.

Post-checkpoint validation caught an incorrect floor-query optimization:
`HeightOwned` returned early whenever a caller requested its floor output,
but the full sampler requests both floor and total height. The existing test
correctly failed; that checkpoint must not be treated as a passing sampler
baseline. The corrected private `FloorOwned` separates floor-only work from
full height evaluation. Expanded tests require full-sample height, floor and
derivatives to agree with the separate query APIs for fractional, clamped,
mountain, town, lake and river points, with each optional constraint enabled.

Both builds and all 145 tests pass after that correction. Native GPU captures
in `/private/tmp/actraiser-globe-edge-verified.CwOXDy/` include all six towns
and 1048 semantic objects, with exact front/return restoration. The Release
flight `runs/20260908-045033/` completes gf2210 and all 19 captures; the six
named town views were inspected. Its 18 steady presentation windows average
8.49 ms (6.5–10.5 ms), versus 8.46 ms before the edge limits, with the same
seven depth draws. This is comparable local Metal cost, not a speedup claim;
first-entry maximum is still 111 ms. Live native-mountain Off/On in
`runs/20260908-045151/` changes gf500/gf600 and restores gf700/gf800 exactly
to the always-on run. Entry and exit still use the existing menus.

The retained terrain mesh also does not consume the full sampler's slope
derivatives. `SimWorldNavigationTerrain_SampleHeights` returns just total
height, floor and authored influence; the full sampler shares that evaluation
and retains its four additional derivative queries. The presenter uses the
smaller query, removing 66,564 unused neighboring height evaluations per
129x129 rebuild, without changing topology, LOD, source art or normal lighting.
This is application-local portable C, not a new runner/render-device contract.
After this query change, both builds and all 145 tests pass. All 19 GPU captures
in `/private/tmp/actraiser-globe-height-query.Wo04eQ/` and all 19 six-town flight
captures in `runs/20260908-045550/` match their corrected pre-optimization
baselines byte-for-byte. The 18 steady presentation windows average 8.53 ms
(6.6–10.8 ms); first-entry maximum is 115 ms. Removing unused queries does not
establish a measurable end-to-end speedup or resolve the initial warm-up hitch.
Final live toggle replay `runs/20260908-045656/` also matches all five gf400–800
captures from `runs/20260908-045151/`, including both fallback and restored
native views. Source and isolated saves retain SHA256
`480a8375b6255ad681202986640c5b06889458125ec0f82641a0e4fb425c0d45`.

#### Cold-build profiling and exact-work reductions

The existing `AR_PERF`/`AR_SIM3D_PERF` profiler now emits a separate
`[sim3d-perf-max]` line with the largest individual scope call in its rolling
window. Means remain summed time per presentation; maxima include nested work
and are not additive, nor must one call exceed a whole presentation's mean.
The navigation presenter also breaks cold setup into mountains, cliffs/terrain
and art when the combined setup exceeds 5 ms. These diagnostics are dormant
without the existing profiling switch and introduce no frame/runner ABI fields.

Navigation now attributes the previously unscoped preparation and shells too:
`world-prepare` sums terrain-field preparation and the projection/model-height
envelope, `world-atmosphere` measures the atmospheric cap, and `world-ocean`
includes depth-pass setup plus closed-ocean projection/submission. Existing
`backdrop` and `depth-mountain` scopes cover space and mountain projection.
Every scope closes before either success or early failure; a profiled presenter
test exercises failed space/atmosphere draws and depth setup. These remain
CPU-side inclusive scopes, not GPU timestamps. Lightweight output/UI work is
still outside these scopes; named times need not sum to total presentation.
The work counters record explicitly instrumented draws, not every driver call.

The closed ocean preserves its 9,120 original triangle-shaped quads but now
submits them in 143 groups through the existing `AppendQuads` value-copy
contract. A bounded 9 KiB stack buffer replaces per-triangle calls; vertex
order, duplicate triangle corners, geometry, materials and depth draw count
are unchanged. First/second-batch failures and complete retry recovery are
covered by the ordinary and profiling-enabled presenter tests.

Two complete ABBA–ABBA comparisons were retained after competing machine load
affected the first set. In the independent four-per-variant repeat, the ocean
scope improved from median 0.16826 ms (0.16809–0.17488) to 0.15124 ms
(0.15091–0.15199). This is about 0.017 ms, not a 10% whole-frame improvement.
Total-presentation ranges overlap, so no overall FPS gain is claimed. All
19 replay images match across all 16 flights, and the sanitizer GPU sweep's
246 images match the pre-batching reference. Details and reproducible manifests
are in `docs/world-navigation-code-audit.md`.

Local entry audit `runs/20260908-050151/` measured 25.153 ms in mountain setup,
13.941 ms in cliffs/terrain, 17.229 ms in art, and a 29.965 ms cloud scope peak.
This made the remaining stall attributable instead of hiding it in the rolling
mean. Mountain material preparation now identifies each cell's contributing
towns once: a bilinear field with four exactly zero corners contributes nothing
at any pixel in the cell. All nonzero contributors retain their original order,
weights and native ramps, including arbitrarily small influences. No geometric
pruning, palette approximation or change to protected mountain footprints is
involved.

Cloud baking now computes each longitude's sine/cosine once in fixed 512-column
blocks and evaluates each pole once. It keeps the original five-octave noise,
coordinates, density, resolution and tint. The 4 KiB temporary trigonometric
arrays are bounded regardless of input dimensions; there is no heap cache,
VLA, SIMD requirement, new artwork or renderer coupling. The expanded pure test
crosses two block boundaries, compares coincident directions at different bake
widths and protects row padding. A separate local comparison against the cloud
source in `ca36a38` checks 668,184 texels plus row padding at four dimensions and
four scales (including all three shipped banks): every byte is identical.

Debug/Release builds and all 145 tests pass. All 19 GPU captures in
`/private/tmp/actraiser-globe-cold-build.Q7VzEW/` and all 19 six-town flight
captures in `runs/20260908-050716/` match their pre-optimization baselines
byte-for-byte. The populated mountain audit retains its face counts, 79 limit
anchors and protected Aitos building row. In that flight, mountain setup is
18.074 ms and the cloud peak 23.898 ms; total first-entry maximum is 102 ms
versus 124 ms in the instrumented baseline. These local observations include
driver variability and do not establish a cross-platform latency guarantee.
Steady presentation remains comparable at 8.49 ms across 18 windows
(6.6–10.5 ms), with seven depth draws. The entry hitch remains despite lower
setup CPU costs; art preparation, terrain/cliffs and first GPU submission still
contribute, and the sampled total latency is not a stable speedup guarantee.
Repeat entry in `runs/20260908-051030/` measured mountain setup at 16.529 ms
and cloud peak at 26.299 ms, with total entry maximum 113 ms, illustrating that
the end-to-end maximum is still variable. Its live mountain Off/On sequence
matches all five gf400–800 captures from the previous build exactly. The
source and isolated saves keep their recorded hashes. Cold-setup reporting
uses the same enable predicate as mountain construction, so an intentionally
disabled stage is not mistaken for a missing cache on every water update.
Final guard verification `runs/20260908-051315/` retains the same five captures
and emits only the two genuine cold/re-enabled setup reports. All 145 tests
pass again after the guard cleanup; the original checkpoint remains unchanged.
Its total entry maximum is 122 ms (mountains 18.363 ms, cloud peak 27.491 ms),
so the observed post-change entry range is 102–122 ms, not a fixed 102 ms.

#### Moving weather and polar chart continuity

Checkpoint `992c6c8` preserves the current original-art mountain mismatch and
the weather-motion diagnostic before further experiments. The longer optional
GPU sweep found a real cloud-body jump at 150992–151008 ms: one rotating bank's
south pole crossed a mesh cell, changing the four-corner longitude unwrap and
producing a 21-level RGB step. Disabling shadows reproduced it. Static captures,
clock-wrap checks alone and the previous shorter sweeps did not expose it.

Cloud-body triangles crossing longitude quadrants are now clipped into
continuous charts using application-local C math. Poles have separate chart
coordinates, while their atlas texels remain identical. Canonical shared-edge
interpolation and double-precision intermediate weights avoid the single-pixel
raster cracks found during implementation. Unaffected cells retain their old
geometry and mapping; shadow receivers retain the exact original opaque mesh
and depth. Child faces are viewport-culled and submitted in bounded 128-quad
batches. The synthetic presenter test caps added body faces at 25% and retains
its original submission-call budget. No new noise, textures, shaders, renderer
contracts, runner ABI fields, terrain heights or mountain footprints are added.
Only body directions get an additional retained cache (about 162 KiB total).

Pure tests cover 46,080 rotated spherical cells, both poles, signed zero,
chart-plane degeneracies, positive winding, complete non-overlapping coverage,
bounded UVs and bit-identical projected cuts under reversed traversal. The GPU
test now includes the failing times in its regular five-view checks, plus
isolated bodies/shadows, repeat/reset/rewind stability, unchanged Palace/UI
masks, zero drift, zero density and weather disabled. The opt-in
`--weather-sequence` still captures 30 seconds at half-second intervals, with
adjacent 16 ms probes at every sample; it is not a low-frame-rate-only check.
The verified sweep has 122 small-step probes across synthetic and native scenes.
The 60 sequence probes stay at 3–4 RGB levels; all probes stay within the original
12-level / 0.25 mean thresholds (largest observed mean 0.09117). This verifies
these sampled cameras and times, not every resolution or possible camera path.

Native captures and the six-second timelapse are in
`/private/tmp/actraiser-weather-final.9yzOHV/`. The nine weather-free synthetic
baseline captures are byte-identical; weather-bearing images change at most
0.257% of pixels as their chart interpolation is corrected. All six named town
views from the moving-weather Release flight `runs/20260908-055008/` were
inspected. Its 19 steady presentation windows average 9.30 ms (6.9–11.1 ms),
with seven depth draws. The frozen-weather control `runs/20260908-055128/`
averages 9.03 ms (7.0–11.3 ms), versus the earlier 8.49 ms pre-clipping run:
this is a visual correctness improvement, not a performance win. Entry still
peaks at 107–113 ms in those two flights. Live clouds Off/On in
`runs/20260908-055231/` changes gf500/gf600 and restores gf700/gf800 byte-for-byte
to the frozen control; disabling weather removes both weather draws. That run
has a 126 ms entry peak, further illustrating the unresolved warm-up variability.
Both source and isolated save hashes remain unchanged. The user-facing entry
and exit flow still uses the existing menus; the fallback commit is untouched.
Final Debug/Release builds and all 145 tests pass, including a 100%-opacity
interpolation check. The final 122-probe GPU sweep also passes. Rounding color
only once changes just one channel level at one pixel in each of two images
relative to the preceding 89-image capture set; the other 87 are byte-identical.
The full material/terrain/native-footprint oracle also passes AddressSanitizer
and UndefinedBehaviorSanitizer with the read-only ROM/WRAM inputs, retaining
the protected building-row and continued-edge counts above.

#### Exact artwork working set and terrain influence rejection

Checkpoint `380772a` preserves the weather fix and the artwork working-set
reduction: the Scale2x compositor uses three bounded source rows (12,312 bytes)
instead of a 4 MiB scratch allocation. Adjacent dirty cells share their halo;
incompatible material changes bypass the spatial feather lookup. Atlas size,
pixel rules, native overlays and upload masks remain unchanged. The dormant
`world-animation` and `world-transfer` performance scopes separate composition
from atlas transfer without changing upload accounting. This reduces scratch
memory; the replay timings did not establish an end-to-end frame speedup.

Terrain sampling now rejects a town only outside its complete four-cell
feather, where its contribution is exactly zero. The previous implementation
still resolved and sampled all six towns at every point. Nonzero weights,
accumulation order, owned cliff corners, coast grading and mountain constraints
retain their original arithmetic. This adds no cache, allocation, settings,
renderer coupling or runner ABI field, and changes no terrain or artwork.

The independent CTest fallback oracle evaluates every half-tile point across
and beyond the world, floats immediately on either side of all town feather
edges, overlapping influences and all 24,576 town-cell corners. A separate
local comparison against the checkpoint source verifies byte-identical full
samples (including slopes/ownership) and every owned corner in five states:
fallback, native world prior, replacement/transition, join and continuation
limit. Its alternating 32-iteration process-CPU benchmark measures the shared
129x129 mesh at 1.271 -> 0.521 ms and six-town owned corners at 2.061 -> 1.059 ms.
These are isolated sampler results, not a claim of halving total entry time.
The comparison and sanitizer artifacts are in
`/private/tmp/actraiser-terrain-influence.Ul54VI/`.

Local Metal replay `runs/20260908-062045/` matches all 19 six-town captures
from `runs/20260908-060601/` byte-for-byte. Steady presentation averages 8.88 ms
over 19 windows (6.9–11.0 ms), with seven depth draws. Terrain/cliff setup is
12.249 ms versus 15.374 ms in the immediate checkpoint control
`runs/20260908-061847/`; total entry still reaches 121 ms versus 109 ms in that
control because other cold stages vary. The entry hitch remains unresolved.
Debug/Release builds, all 145 tests, and the terrain AddressSanitizer/
UndefinedBehaviorSanitizer check pass. Original mountain art and the existing
menu-based entry/exit flow remain intact; `380772a` is not amended.
The native 122-probe GPU/weather sweep also passes, with all 89 captures
byte-identical to `/private/tmp/actraiser-art-final.yIVTlS/`. The ROM/WRAM
mountain-clearance oracle retains the 573 exterior faces, 215 lowered relief
vertices and protection from 207 Kasandora rear faces at Aitos's building row.
Source and isolated-save hashes remain unchanged.

#### Bounded cloud-noise reuse during initial bake

The spherical cloud bake now evaluates one octave across a fixed longitude
block at a time. Latitude's lattice row and interpolation weight are constant
within that loop. Adjacent samples in the same X/Z noise cell reuse its eight
hash values; crossing a cell recomputes them. Each latitude, octave and block
resets that small cache. The five-octave accumulation, interpolation order,
density/tint rules, 512x256 layer size, pole values and duplicated seam texels
are unchanged. Temporary working storage is about 6 KiB (up from 4 KiB), not
a persistent cache or new allocation. No settings, ABI or renderer API changes.

A scalar oracle checks every output texel and destination padding across 42
size/scale combinations, including the three production scales, minimum
dimensions, multi-block widths and the maximum supported scale. Separate
32-iteration alternating process-CPU measurements against checkpoint source
in `/private/tmp/actraiser-cloud-bake.jNzELU/` report 5.691 -> 4.204 ms at scale
4, 5.748 -> 3.809 ms at 2.7 and 5.698 -> 4.704 ms at 6.3: 17–34% less bake CPU
time locally, not a cross-platform guarantee or a steady-frame speedup.

Release replay `runs/20260908-062944/` retains all 19 six-town images exactly
against `runs/20260908-062045/`. The first cloud scope peaks at 22.510 ms versus
28.706 ms; total entry is 105 ms versus 121 ms, with other cold stages also
varying. Entry is still a hitch, not a solved one-frame load. Steady presentation
averages 8.85 ms over 19 windows (7.0–10.6 ms), with seven depth draws, essentially
unchanged from 8.88 ms. Debug/Release builds and all 145 tests pass; the full
material/native-mountain oracle also passes AddressSanitizer and
UndefinedBehaviorSanitizer. Source and isolated-save hashes remain unchanged.
The 122-probe native GPU/weather sweep passes; all 89 captures are byte-identical
to `/private/tmp/actraiser-terrain-influence.Ul54VI/`, including moving poles,
disabled effects and below-cloud views. The rollback checkpoint is unchanged.

#### Repeated flight benchmarks before selecting an optimization

Do not accept or dismiss an option on one replay. Run at least three full,
identical flights per candidate, interleave/rotate their order, and keep builds,
tests and other benchmark jobs out of the measurement interval. Freeze weather
for frame comparisons; validate moving weather separately. Report each run and
the median/range across runs, with cold-entry peaks separate from steady flight.
If the difference overlaps observed noise, retain the option for more testing
instead of claiming a win or regression. Presentation time is not whole-frame
time and must not be converted directly into an FPS claim.

A shifted-coordinate cache was initially dismissed after one short flight;
that was insufficient evidence. It was restored as a separate candidate beside
a new indexed cloud-shadow prototype. Nine 2,210-frame six-town flights used
the order baseline/cache/indexed, cache/indexed/baseline, indexed/baseline/cache.
The three release executables shared compiler flags and all objects except
the presenter, including the same indexed-capable depth backend. No effects,
triangles, nine-sample shadow softness, draw order or shaders were changed.
Neither prototype is enabled in the retained implementation.

The following are medians (min–max) of three independent local Metal runs.
Steady presentation averages are weighted by the frame counts in logged
post-entry windows; cloud/submit scopes exclude their first cold window.

| Variant | Steady presentation ms | Cloud preparation ms | Depth submit ms |
| --- | --- | --- | --- |
| Baseline | 9.03 (9.02–9.05) | 2.83 (2.81–2.84) | 1.10 (1.10–1.12) |
| Coordinate cache | 9.23 (9.17–9.41) | 3.03 (3.01–3.08) | 1.12 (1.08–1.17) |
| Indexed shadows | 9.36 (9.28–9.44) | 3.59 (3.56–3.63) | 0.76 (0.73–0.78) |

Indexed geometry reduces submitted vertices from about 861k to 542k per
presentation, retaining seven draws and the same triangles, but its CPU
preparation overhead outweighs the lower submission cost. Both candidates
are slower in all three runs on this machine. This is evidence against these
implementations here, not against coordinate reuse or indexed meshes on all
hardware. Cold-entry peaks vary from 87 to 130 ms even for the baseline and
are not evidence of a candidate-specific startup improvement.

All 171 captures match the baseline and `runs/20260908-062944/` byte-for-byte.
Baseline runs: `065126`, `065425`, `065535`; cache: `065207`, `065329`, `065601`;
indexed: `065236`, `065358`, `065506` (all under `runs/20260908-*/`).
The candidates, replay manifest, analysis script and re-applicable patches
are preserved locally in `/private/tmp/actraiser-cloud-coordinates.BkmZjl/`.
The indexed prototype also passed its focused GPU pixel test for mixed
indexed/ordinary geometry, occlusion and resource reset, and the presenter
contract test. Its private API/backend additions were removed from the active
worktree with the prototype; the verified terrain/cloud-bake optimizations
above remain. No checkpoint was amended and no new commit was made.
The retained path rebuilds in Debug and Release and passes all 145 tests;
both the original save and isolated replay save retain their starting hashes.

#### Value-copy depth conversion

The depth backend now snapshots each incoming `Sim3DDepthVertex` into a local
value before assigning the GPU vertex's fields. The arithmetic and data layout
are unchanged. This gives the compiler the complete input before destination
stores, allowing grouped color/UV transfers without interleaved scalar alias
dependencies. Local release disassembly confirms those grouped transfers.
It is ordinary C11, not platform SIMD, pointer casts, a layout alias, or a new
`restrict` contract. No interface, shader, runner ABI, settings, allocation or
retained working-set change is needed. The same conversion serves town and
globe rendering.

Eight full 2,210-frame flights compare the retained scalar-load implementation
with the value-copy implementation in A/B/B/A, A/B/B/A order, with no competing
build/test jobs. All other release objects and settings are unchanged. As above,
figures are the median (min–max) across independent runs, with frame-weighted
logged steady windows and the first cold cloud window excluded.

| Variant | Steady presentation ms | Cloud preparation ms | Depth submit ms |
| --- | --- | --- | --- |
| Baseline, four runs | 8.77 (8.72–8.83) | 2.78 (2.78–2.79) | 0.97 (0.96–0.98) |
| Value copy, four runs | 7.91 (7.90–7.95) | 2.37 (2.37–2.38) | 0.97 (0.96–0.98) |

This is a repeatable local 9.8% reduction in steady presentation time, not
a whole-frame FPS or cross-platform guarantee. The cloud preparation scope
includes the vertex conversion and drops about 14.7%; GPU submission itself
does not materially change. Seven draws and about 861k vertices remain. Entry
still peaks at 98–99 ms in the optimized flights; that hitch is not resolved.

Baseline runs are `070432`, `070552`, `070720`, `070840`; optimized runs are
`070500`, `070526`, `070748`, `070813` (all under `runs/20260908-*/`). All 152
captures match each other and `runs/20260908-062944/` exactly. The 122-probe
native GPU/weather sweep passes, and all 89 native/weather images are
byte-identical to `/private/tmp/actraiser-cloud-bake.jNzELU/`. Executables,
run manifest, analysis and captures are in
`/private/tmp/actraiser-depth-conversion.Yiyluw/`.

The GPU regression now compares 2,053 varied translucent quads as individual
calls, uneven 127-quad chunks and one complete batch, across retained buffers
and renderer reset. It exceeds both initial CPU/GPU capacities, overwrites
caller storage before submission, verifies foreground occlusion, and checks
every output pixel against the single-quad rendering. Debug/Release builds,
all 145 tests and the depth GPU test under AddressSanitizer/UndefinedBehaviorSanitizer
pass. Both save hashes and checkpoint `380772a` remain unchanged. No custom
entry/exit handling or globe-under-SIM composition was added in this pass.

#### Block atlas conversion inside the SDL backend

Atlas uploads now use `SDL_ConvertPixels` for each packed dirty rectangle
instead of assigning four destination bytes per texel in a scalar loop.
`SDL_PIXELFORMAT_ARGB8888` describes the caller's native-endian integer values;
`SDL_PIXELFORMAT_RGBA32` describes the GPU's RGBA byte order on either endian
host. The adapter still owns that translation and reuses the same textures,
transfer buffers, region ordering and cycling rules. The portable ARGB/pitch/
region interface and runner ABI do not change. Conversion failures unmap the
transfer buffer and return failure before any GPU copy is submitted.

A scalar comparison verifies all output bytes, alpha and untouched padding
across 210 size/stride combinations, including odd byte pitches and one-pixel
dimensions. Thirty-two alternating process-CPU pairs measure 2048x2048
conversion at 0.794 -> 0.439 ms, 1024x768 at 0.124 -> 0.070 ms and 512x512 at
0.040 -> 0.024 ms locally. These isolated conversions exclude allocation and
GPU submission; the full-flight comparison below is the acceptance gate.

Eight identical 2,210-frame release flights run in A/B/B/A, A/B/B/A order,
with no competing build/test jobs. Both variants include the verified value-copy
depth conversion. Medians (min–max) across four runs each use the same logged,
frame-weighted steady-window method as above.

| Variant | Steady presentation ms | Average world-transfer ms | First world-transfer peak ms |
| --- | --- | --- | --- |
| Baseline | 7.92 (7.91–7.93) | 0.306 (0.305–0.307) | 5.88 (5.30–6.24) |
| Block conversion | 7.74 (7.72–7.79) | 0.109 (0.108–0.110) | 2.54 (2.27–3.08) |

Steady presentation improves about 2.3% locally, with identical art, sample
counts and seven draws. Cloud preparation and GPU submission remain essentially
unchanged. Cold-entry peaks have medians 100.5 -> 94.0 ms and ranges 96–113 ->
89–96 ms, but mountain/cliff/other cold stages also vary: do not attribute the
entire 6.5 ms difference to conversion or call the hitch solved.

Baseline runs: `071815`, `071933`, `072020`, `072137`; optimized: `071840`,
`071907`, `072046`, `072112` (all under `runs/20260908-*/`). All 152 captures
match the baseline and `runs/20260908-062944/` exactly. The 122-probe native
GPU/weather sweep passes, with all 89 captures byte-identical to
`/private/tmp/actraiser-depth-conversion.Yiyluw/`. The comparison executables,
manifest, analysis, scalar oracle and captures are in
`/private/tmp/actraiser-atlas-transfer.fOAgXe/`.

The GPU regression now compares a fully populated reference atlas against
multi-rectangle and individually submitted updates in reverse order. Source
rows have an odd padded pitch; unrequested source texels deliberately differ,
and caller storage is overwritten before drawing. Every output pixel must
agree across retained resources and reset. Debug/Release builds, all 145 tests
and the depth GPU suite under AddressSanitizer/UndefinedBehaviorSanitizer pass.
The original and isolated save hashes, original mountain art and menu behavior
are unchanged; checkpoint `380772a` is untouched and this pass is uncommitted.

#### Six-town visual acceptance matrix

The frozen-scene GPU test accepts `--town-matrix` alongside
`--weather-sequence`. It focuses each enabled retained town at near, oblique
middle and centred whole-globe views at 800x600, plus a close 1792x1344 view.
Each view exercises the authored Low model cap, a reduced-effects profile,
and ten independent switches: lighting, clouds, shadows, atmosphere, models,
relief, detailed ground, mountains, space backdrop and haze. Every switch and
profile must restore the full image exactly; resource resets must also be
pixel-exact without changing captured navigation/town data or the map serial.

The HD test compares each town's own objects against zero objects while keeping
model-owned ground cleanup and all other metadata identical. Merely disabling
the global models switch is insufficient: neighboring towns and changes to
source-art cleanup could falsely demonstrate that the focused town has models.
With the read-only `runs/20260908-032849/dump_wram.bin` fixture, isolated objects
change these numbers of pixels locally on Metal:

| Town | Captured objects | Pixels changed by its isolated models |
| --- | --- | --- |
| Fillmore | 251 | 150,299 |
| Bloodpool | 241 | 147,156 |
| Kasandora | 192 | 92,713 |
| Aitos | 157 | 83,856 |
| Marahna | 205 | 60,406 |
| Northwall | 2 | 3,559 |

Object counts include native-art-only plot classes, and occluded objects need
not contribute pixels. At 800x600 all 18 Low-cap images match their full-cap
counterparts: the projected-size selector already uses Low. All six HD views
show detail-cap differences, exercising finer authored geometry. This is
coverage of these sampled views, not every LOD threshold or possible save.

The initial HD attempt exposed faulty test plumbing: resizing without a full
frame-present lifecycle kept a smaller hidden GPU output alive and stretched
its last row/column into the enlarged readback. This falsely suggested that
Northwall's models were invisible. The test now completes frame presentation,
retires the previous-size output after resizing, and verifies distinct corner
markers plus the centre pixel before accepting captures. Ordinary ROM-free
CTest also covers larger, smaller and widescreen output sizes, exact resource
rebuilds at each size, and restoration of the original image. No production
town capture, model placement or renderer behavior was changed to fix this
test-only failure.

The corrected sweep in `/private/tmp/actraiser-town-matrix-verified.Q67KWR/`
passes all 24 views and 122 adjacent-16-ms weather probes, saving 227 captures
(138 town-matrix images plus the prior 89 synthetic/native/weather images).
All prior 89 images are byte-identical to the atlas-transfer validation above.
All ten switches visibly affect every matrix view and restore exactly. Visual
inspection of all six HD town captures confirms populated Kasandora ground,
the clean blue Bloodpool shoreline and visible Northwall models; the accepted
original-art mountain seams and steep native cliff edges remain. Captures are
local diagnostic artifacts, not distributable ROM-derived golden fixtures or
proof of other GPU backends. Existing menus and checkpoint `380772a` remain
unchanged. The regular 145-test suite, including the new resize coverage, passes.
The final rerun in `/private/tmp/actraiser-town-matrix-final.r79ZMm/` reproduces
all 227 images exactly. The same complete sweep, compiled with
AddressSanitizer/UndefinedBehaviorSanitizer, passes in
`/private/tmp/actraiser-town-matrix-sanitized.MDALvO/`. Compared with Debug,
that separately optimized build matches 191 images exactly; the other 36
differ by at most one RGB level over 68 pixels total (1–12 pixels per image).
The comparison script is retained with the final captures. All within-build
toggle/reset restoration assertions remain exact; no thresholds were relaxed.
Both save files retain their starting hashes. The Release build also passes;
this test/documentation-only audit makes no new performance claim.

The original-art mountain seams and steep native cliff edges are retained as
accepted. New zoom-triggered entry/exit gestures and continuous full-town
descent are deferred by request; the existing menus remain authoritative.
Broader cliff/camera acceptance and other live palette effects still need
coverage. The shared local mesh does not make the two camera-dependent
presentations identical.

#### Loading behind the native fade

A remaining entry cost is acceptable when it is hidden by the existing black
transition; prioritize smooth visible flight over eliminating every cold-start
millisecond. The navigation presenter deliberately prepares and draws the
complete scene at master brightness zero. Returning early on that black frame
would postpone model compilation, atlas uploads and GPU pipeline preparation
until the visible fade-in, defeating this loading opportunity.

The unchanged Release replay in `runs/20260908-075928/` captures every frame
340–390 together with public PPU snapshots. The previous screen reaches black
at gf351; globe setup occurs at gf353 with INIDISP zero. The complete output
remains exactly black through gf354. The first visible frame is gf355 at
brightness 1, followed by the original fifteen-level fade, reaching full
brightness at gf369. The Palace and labels fade with the world. The analysis
script and inspected first/full-brightness previews are in
`/private/tmp/actraiser-fade-audit.4W0Ngm/`. Dense screenshots and snapshots
perturb frame timing, so this is sequencing/visibility evidence, not another
performance benchmark.

Regression tests now require authored model compilation and native mountain
uploads while brightness is zero, with no repeated uploads at the visible
step. The actual GPU test resets render/model resources, verifies a fully
black frame with models/weather enabled, then checks all fifteen fade levels:
monotonic bounded RGB, no further model misses or relights for the frozen scene,
and exact restoration of the full-brightness image. The same check runs on
synthetic and read-only native town data. No new fade timer, gameplay pause,
runner ABI, asynchronous loader or custom town entry/exit handling is added.
This verifies the captured normal entry, not arbitrary mid-flight settings
changes or device-loss recovery; those still need separate latency handling.
All 145 tests pass. The native GPU check in
`/private/tmp/actraiser-black-entry-native.0vchMh/` also passes, preserving all
29 existing baseline captures exactly. Both save hashes and checkpoint
`380772a` are unchanged; this follow-up changes only tests and documentation.

#### Existing menu round trip

The populated-save replay in `runs/20260908-111902/` enters Fillmore using
**Observe the People**, opens the native town menu, and selects **Return to Sky
Palace**. It returns directly to enhanced globe navigation. The control in
`runs/20260908-112052/` uses the identical input/save/settings with only
`AR_SIM3D_WORLD_NAV=0`. All 77 sampled complete WRAM, VRAM, CGRAM, OAM/high-OAM
and public-PPU snapshots match exactly, including both transition windows.
Final WRAM/SRAM and the rendered town at gf1000 also match exactly: visiting
the globe does not change this town view or game state.

Town rendering becomes ready at gf643 with brightness zero; the native town
fade starts at gf645. On return, every captured pixel is black at gf1199–1202.
Globe presentation resumes at gf1201, before first visible gf1203, and reaches
full brightness at gf1217. All thirteen preceding town-fade/black images are
byte-identical to the flat-navigation control. Native initialization repeats
gf1200 across several host ticks in both runs; game-frame screenshots retain
only the last image for that number, so this is not a host-tick latency
measurement. No new waits, fade timers, gameplay hooks or renderer changes
were needed. Both original and isolated saves retain their starting hash.

The exact pad replay, construction/verification scripts and reproduction notes
are retained with the enhanced run. The camera, globe-coordinate and presenter
regressions also pass, including Free/Dynamic visit isolation and transient
orbit/zoom reset. This verifies Fillmore's native menu round trip, not every
town event or physical input device. Live mouse acceptance remains unverified
while the Mac is locked; no gamepad was connected for these replays.

#### Tall models at viewport edges

The footprint-based screen margin could reject a model whose raised crown
still intersected the viewport. A read-only probe using the populated six-town
capture and native Aitos Advent camera samples reproduced this at 400% object
height and a supported -1300 mrad pitch, with relief disabled. For example,
the Aitos tree at (26,2) has an anchor at y=652.90 with a 49.31px margin at
gf1210/zoom450, yet crown vertices remain visible above y=600 and clear of
the opaque globe.

The same sweep at yaw +650 mrad also finds a normal-height Marahna tree at
(24,19) during gf1315/zoom30: its anchor is y=614.24, its old margin 13.96px,
and twelve crown vertices are inside the viewport. This is a small edge pop,
not missing whole towns. The updated bound retains these candidates across
the checked yaw -650/0/+650, pitch -575/-1000/-1300, heights 100/400 and 25
native camera samples; globe visibility is independently checked with
segment/sphere intersections, rather than assuming on-screen means unoccluded.

Retained authoring bounds now include XY overhangs and minimum/maximum height
across allowed LODs and windmill poses. The chart metric bounds each model's
angular extent. A conservative sphere contains that angular cap and its full
radial height interval; normalized viewport side planes may reject it only
when the entire sphere is outside. This extra check runs only after the old
cheap margin would reject an object. Existing subpixel thresholds, quality
ceilings, shared cache, whole-globe horizon rejection and GPU depth remain
unchanged. Measurement stays portable; camera-dependent bounds remain in the
presenter. Three direct model test targets now link the existing
`actraiser_math` abstraction explicitly, including platforms with separate libm.

The portable test submits all castle faces when its anchor is at y=799.64
but spires enter the 600px viewport, still rejects the genuinely offscreen
100%-height variant, and checks repeat/reset/height restoration. The GPU test
compares visible spire pixels with a zero-area-footprint control at identical
projection, then verifies repeat/reset equality. Restoring only the old
viewport rejection in a temporary binary makes that GPU test fail. Native
GPU weather/flight coverage and the new edge case also pass ASan/UBSan.

Four isolated Release flights per variant in ABBA–ABBA order
(`runs/20260908-103902/`, `104046/`, `104259/`, `104414/`, `104457/`,
`104748/`, `104826/`, `104910/`) retain all 19 sampled normal-flight images
byte-for-byte within and across builds. Weighted steady presentation medians
are 8.206 ms (8.186–8.270) before and 8.184 ms (8.173–8.193) after, with
overlapping ranges and seven draws in both variants. The 0.022 ms difference
is not treated as a speedup. Cold presentation peaks have medians 100.5 and
103.5 ms with overlapping 92–112 and 99–109 ms ranges; cold work remains under
the existing black loading opportunity. The complete 145-test suite passes.

#### Native Advent clearance over raised terrain

The existing Aitos Act 2 Advent is a separate path from the deferred custom
globe/town entry controls. Its original Mode-7 matrix approaches zero while
the view is still visible. Applying that unbounded flat-map magnification to
a raised globe brings geometry through the eye plane: the preserved Release
build fails its atmosphere projection at game frame 1301 in
`runs/20260908-083017/`, before the native fade. Disabling atmosphere alone
also allows late volcanic faces to cross the projection plane.

During the captured empty-OAM Advent composition, the presenter now smoothly
limits scene magnification using the globe radius, terrain/mountain envelope,
cloud/atmosphere clearance, focused ground datum and camera direction. The
soft limit starts at 50% of the safe scale, with a continuous first derivative,
and leaves the enclosing sphere at least 0.25 view units ahead of the eye
(the projection near plane is 0.1). This is a minimum-clearance approach to
black, not a new landing animation or exact contact with a destination summit.
It does not flatten mountains, move buildings, change WRAM, pause gameplay,
advance the fade or add a backend/runner ABI.

The guard also includes independently scaled authored town models. With relief
and mountains disabled, cloud altitude zero and a straight-down camera at
distance 3, the previous terrain-only envelope let the 400%-height Bloodpool
castle cross the near plane at native zoom 30 (gf1315, still brightness 5):
200 of its 556 Ultra face vertices were behind the 0.1 near plane. The retained
authored-height envelope leaves all 556 in front, with minimum eye distance
0.681 there and 0.394 at final zoom 10. This changes only the empty-OAM approach
when models exceed the existing envelope; it does not raise clouds or alter
ordinary flight projection.

`SimBackgroundVoxelModel_HeightBound` uses the existing authoring functions
before buried-face removal and corner AO. It bounds all allowed distance LODs
and all three windmill poses without adding offscreen models to the shared
geometry cache. The presenter retains per-object bounds, invalidating only
changed captured objects or detail/style; stable captures need one byte
comparison. Height percentage, camera and landscape changes reuse those
unscaled bounds. The chart metric is at most one, so authored height times the
shared proportions bounds every radial model column. Preparation also runs
during ordinary travel/black loading, not just at the first visible Advent
frame. No live town state, GPU handles or new runner ABI cross this boundary.

The model tests compare the bound with compiled geometry across kinds, seeds,
regional houses, detail/style combinations and construction/animation states.
The presenter test requires every castle face to reach the depth pass during
forward/reversed zooms and heights 100–400%, all four LOD ceilings/styles,
repeated frames, changed objects, disable/enable and resource reset. Merely
checking a successful presentation would miss the original silently dropped
faces. The hidden far hemisphere may pass the far plane at the final top-down
zoom; normal GPU clipping handles it, while the near-side model stays intact.

Four isolated Release flights per variant, in ABBA–ABBA order
(`runs/20260908-100928/`, `101035/`, `101104/`, `101213/`, `101255/`,
`101327/`, `101415/`, `101440/`), retain all 19 sampled images byte-for-byte
within and across builds. Weighted steady presentation medians are 8.143 ms
(8.035–8.214) before and 8.178 ms (8.116–8.240) after: a 0.035 ms difference
inside overlapping run ranges, not a speedup claim. Draw count remains seven.
Initial presentation peaks have medians 85 and 91.5 ms respectively; retained
height authoring adds cold work under the existing black loading opportunity.

The final native replay `runs/20260908-101546/` matches
`runs/20260908-093115/` in all 25 complete gf1200–1320 game-state snapshots
and all 31 consecutive gf1290–1320 images. Default Advent timing/appearance
therefore remain unchanged, including black at gf1320 and Act 2 at gf1323.
All 145 tests pass; both the portable presenter regression and native GPU
weather/flight suite also pass AddressSanitizer/UndefinedBehaviorSanitizer.

The ordinary replay `saves/aitos-r4-natural.rec`, using an isolated copy of the
existing action-routes seed, succeeds with the capped volcano in
`runs/20260908-093115/`: the globe
remains active through gf1320, which is fully black; Aitos Act 2 enters at
gf1323. All nine overlapping complete WRAM/public-PPU snapshots at
gf1280–1320 (every five frames) match the authentic-renderer control
`runs/20260908-083225/` exactly, including VRAM, CGRAM and OAM. All 25 snapshots
from gf1200–1320 also match the earlier guarded replay
`runs/20260908-084302/` and timing-only replay `runs/20260908-090906/`.
The final replay captures every frame from gf1290 through black; inspected
gf1300/1315 images retain the single opening while the native spin/fade continues.
These readback-heavy runs
verify rendering and sequencing, not performance.

The GPU regression reuses seven real late-descent matrix/brightness samples
over Aitos, Fillmore hills and Marahna's plateau, at landscape heights 0%,
100% and 400%. Each visible frame must render successfully, held frames must
match exactly, and the final frame must be entirely black. Captured towns and
map serial must remain unchanged. It passes on both synthetic and populated
native scenes, including AddressSanitizer/UndefinedBehaviorSanitizer on Metal.
This covers those terrain/camera samples, not every possible combination of
independently exaggerated building heights or future destination models.

The first guard used an exponential ease-out, which reached 99.74% of its
safe scale at brightness 10 and visually exhausted the descent too early.
`SimWorldNavigationScene_AdventScale` now uses a reciprocal continuation:
for raw scale above half the limit, output is `limit - (limit/2)^2/raw`.
The ROM's `$02:849B` loop subtracts four zoom units each tick and derives its
fifteen fade levels from that same zoom. Because raw scale is reciprocal zoom,
the new curve retains finite approach motion through the existing fade,
without waiting at a clamped height. Scale and its first derivative match at
the join. Tests cover the join, monotonic/equal late progress, multiple limits,
invalid inputs and frozen/reversed clocks. Matrix quantization can still vary
the per-tick displacement; this is a safe continuing approach through black,
not exact physical contact with the volcano entrance or a new entry system.

#### Native Aitos lava palette animation

The original `$02:AF69` routine writes CGRAM entry `$21` in SIM town 4,
folding bit `$20` of the game-frame byte into a 64-tick red triangle wave.
The globe reproduces that cadence from its captured frame, using the same
completed-tick convention as town CHR animation. It does not borrow the
currently resident town's PPU palette. `SimTownGroundArt_ColorIndexMask`
decodes source palette identities; RGB equality is never used to identify lava.
The native town fixture has 51 silhouette-visible texels in crown metatiles
`$70/$71` using this entry. The globe instead animates the 84 index-zero texels
in its single overhead opening; both paths keep alpha, UVs and geometry fixed.

The portable mountain scene keeps two bounded masks, changes only those
texels and reports a dirty rectangle. The overhead cap transfers 16×16 ARGB
pixels (1 KiB); native-art fallback transfers at most 32×16 (2 KiB), through
the existing atlas-region interface. Held clocks
and identical turnaround colours skip uploads; failed transfers retain the
dirty rectangle for retry, including after a clock rewind. Disabling native
mountains skips this work. Initial colour is folded into the normal cold atlas
upload. Ground-transition rock colours are derived before applying the pulse,
so a resource reset cannot recolour cached terrain based on its entry phase.

The independent town-canvas oracle verifies source-index masks across every
metatile and all four native variants. Material tests check 132 clock states,
rollover, frozen frames and unchanged non-lava texels/scene ownership; the
fake-backend test verifies 1-KiB cap transfers, failure/rewind recovery, unchanged
geometry and no ground-atlas invalidation. Native GPU resource restoration
and sanitizer checks pass. Exact animation-phase handoff to live SIM remains
part of the deferred transition work.

Before the overhead cap, four independent Release flights per build,
interleaved ABBA–ABBA with the
same isolated save/replay/settings, are recorded in
`/private/tmp/actraiser-native-lava.AIGLid/runs.json` with the analysis script.
Weighted steady presentation medians are 7.804 ms (range 7.776–7.909) before
and 7.807 ms (7.792–7.876) after the lava/Advent changes. This is no measurable
whole-frame regression in this sample, not a speedup or an FPS measurement.
World-transfer cost rises from 0.110 to 0.130 ms; draws remain seven per
presentation. The separate cold peaks overlap at 89–104 versus 90–96 ms.
All 76 captures per build repeat exactly within that build; eleven of the
nineteen sampled views differ across builds, as expected for animated lava.

#### Single overhead Aitos crater

The angled town crown was visible on both the front and folded rear, making
two separate openings. Globe construction now removes that first crown row
from both faces and bridges the summit with one horizontal cap. Its texture
uses the original top-down Mode-7 stamp `$A6/$A7/$B6/$B7` (world cells
24,43 through 25,44), copied through the portable owned-output
`SimWorldMap_CopyTileArt` provider. The overhead outline and rim are retained;
the surrounding `$40..$45` rock shades map into the native Aitos rock palette.
Shoulder strips keep native town rock UVs rather than stretching the overhead
tile across the whole mountain. No generated replacement art or backend API
is required, and town-mode meshes are unchanged.

The summit is 32×28 native pixels, 1.68 world-tile units above its floor.
Shoulder depth tapers toward the original silhouette, and rear ground contacts
remain at their original source columns. Roof and rear share edge vertices
and pass through the same cell-level polygon clipping against all six towns'
terrain/model occupancy. The cap occupies an unused 16×16 corner of the
existing 512×512 mountain atlas; ground-transition palette extraction skips
that reserved cell. Missing overhead art falls back to the original native
crown, rather than failing navigation.

Native material tests verify exactly one 896-square-pixel cap, 32 central
strips, no old `$70/$71` UVs, the ROM's opening/rim pixels, native rock colours,
all 84 animated texels, original rear contacts and protected cells underneath
the new roof. The legacy 51-texel fallback remains tested. World-map tests
cover all 256 copied tiles, optional indices, unavailable inputs, owned
lifetime and unchanged publication serials. GPU captures inspect four
approach directions plus overhead, with exact held-frame restoration.
All 145 regression tests pass, alongside the populated Metal scene under
AddressSanitizer/UndefinedBehaviorSanitizer and the real Advent replay above.

Four Release flights per build, ABBA–ABBA, compare the timing-only build to
the cap in `/private/tmp/actraiser-advent-timing.JAfw0Q/runs.json` (analysis:
`/private/tmp/actraiser-native-lava.AIGLid/analyze.py` with that directory as
its argument). Weighted steady presentation medians are 7.841 ms
(7.823–7.874) before and 7.855 ms (7.836–7.875) after: a 0.013-ms difference
inside the overlapping run ranges, not a meaningful regression or an FPS
claim. Draws remain seven, and average submitted vertices increase about
0.24%. Cold peaks span 95–105 ms versus 78–98 ms; no cold-load speedup is
claimed. All 76 captures per build repeat exactly within that build.

#### View-driven town LOD stability

The ROM-free GPU test now follows a single real, finished factory across
camera distances and output sizes. It identifies the rendered tier through
the existing shared model-cache counters, without adding presenter diagnostics
or changing a production interface. The near view selects Low at 800×600,
High at 1792×1344, and Ultra at 2688×2016. A 1792×1344 distance sweep crosses
High/Balanced and Balanced/Low, including adjacent camera ticks on either side.
Matching the user ceiling to the selected tier preserves every pixel; lowering
it removes visible detail. Reversing the sweep restores every saved frame
exactly, including three stationary frames per distance as the game clock
advances. Once all four model keys are warm, these return/hold frames generate
no additional model misses or relights. Captured town/navigation data and the
map serial remain unchanged.

These are discrete native model variants, not smoothly morphed geometry.
The isolated diagnostic in `/private/tmp/actraiser-lod-motion.PrA5NQ/`
locates switches at distance-x100 205→206 and 409→410 for this fixture.
Same-camera comparisons against the next-lower cap change 342 and 94 pixels
respectively immediately before those switches. Inspected factory crops show
architectural details disappearing at the first boundary; this must not be
described as seamless LOD. No cross-fade, hysteresis, replacement geometry or
runtime optimization is introduced by this audit. It establishes deterministic
selection/cache behavior for this model, not temporal acceptance of every
authored object or GPU backend.

All 145 tests pass, as does the updated AddressSanitizer/UndefinedBehaviorSanitizer
GPU test (including the black-entry regression). The read-only populated-town
check in `/private/tmp/actraiser-lod-native.T1OqaR/` passes and reproduces all
29 prior captures byte-for-byte. Both save hashes remain unchanged. This is
correctness coverage, not a performance benchmark.

The same portable authored-model cache remains shared with town rendering.
Presentation memoization consumes captured scene/camera settings only and
continues through `ArRenderDevice`/`Sim3DDepthPass`; it does not introduce SDL,
Metal or runner-private dependencies. Rendering the globe under the active
SIM town is not wired yet: that future composition should reserve resources
before borrowing models and share a scene/depth pass, not call the standalone
navigation presenter (which owns its viewport, clear, Palace and UI).

Visual follow-up `runs/20260907-213220/shot_500.ppm` verifies the populated
Bloodpool shoreline without pre-cleansing red bleed, cloud bodies enabled,
and the navigation-only space backdrop. The matching gf400 capture covers
Fillmore; the plain/border grading pass was exercised through Aitos at gf800
in `runs/20260907-212325/`. These are local diagnostic captures, not portable
golden fixtures.

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

#### Enhanced Sky Palace horizon view

`sim3d_sky_palace` (`AR_SIM3D_SKY_PALACE`) defaults on but is available and
active only with `sim3d_world_navigation`. Map `$07` retains its native
foreground and menu/gameplay state. Its background shows the developed globe
from a low-orbit horizon camera, with Palace-only blue daylight
and drifting sky clouds. Map `$09` keeps its space backdrop. Globe terrain,
town models, native mountains, cloud body/shadows and effect controls are
shared, not copied into a second scene implementation.

The game-side producer claims an unused BG1 observational main-screen-winner
capture. It never requests `RemoveFromGame`, and does not displace existing
HD/dump captures. The adapter validates public PPU/frame snapshots and their
generation, compatible Mode-1/color math, and exact capture flags/extents.
During slot upload, presentation composes a bounded ARGB foreground from the
native main surface and winner mask: white winners become transparent;
everything else retains native RGB and opaque alpha, including black menus.
The original main texture remains available for same-frame fallback. Retained
frames use uploaded textures, never borrowed producer pixels. Device reset
destroys the Palace texture and clears publication validity.

The extracted globe scene stage leaves output target/viewport and native UI
ownership with its caller. Its Palace camera uses six-plane homogeneous
clipping from portable `scene3d_math`; exact clipped ground also supplies
shadow/haze receivers. Navigation keeps its former projection path. The sky
gradient uses ten vertices, holding a richer indigo-blue through the native
roof/HUD into the visible windows, then reaching pale blue at the projected
horizon. The Palace uses a **3x diameter** globe: a 144-tile chart radius and
12-unit physical radius, retaining the former 4/48-unit town tile scale,
3-unit minimum camera altitude and 1.05-radian vertical field of view. Keeping
altitude and local scale fixed makes the horizon gentler; proportionally
scaling the camera too would preserve the old curve. Global terrain/model/air
bounds still raise the eye if required by effect settings. The camera aims
slightly above the sea tangent (about 53% down the viewport), leaving more sky
around the angel. The horizon veil is narrower and less opaque so nearby
terrain keeps its color. Navigation separately uses a 96-tile chart and its
centered radial camera.
The globe rotates independently to put the selected region's raised centre
just below the sea horizon, about 56.5% down the viewport above the menu.
Presentation consumes captured region bounds (travel focus is the fallback),
not game memory or a duplicate town-location table. A near ray/sphere
intersection accounts for the region's terrain height without scaling relief
or moving native destination coordinates.
The chosen radius is presentation-owned. Explicit-radius portable globe and
mountain-transition functions have no mode/global-state dependency, and their
default wrappers preserve the original 48-tile math contract. Radius participates in private
projection, mountain, cliff, model-bound and weather-normal cache keys.
Switching Palace/navigation rebuilds chart-dependent surfaces from native art
(including pre-animation mountain colors), but retains compiled town models
and cloud atlases. Selecting another Palace town changes orientation only.
No runner ABI, frame schema, settings option or backend contract was added.
The revised sky deck uses twelve self-shadowed density volumes, each represented
by sixteen sorted depth-tested slices. An appended `VolumeCloud` material
owns a 384×960 atlas; it reuses the existing portable shader and no-depth-write
pipeline. No new shader, render target or runner ABI was added. The separate
`sim3d_sky_palace_volumetric` / `AR_SIM3D_SKY_PALACE_VOLUMETRIC` toggle selects
one precomposited slice per bank for low-end systems. Zero drift freezes both
cloud decks; clouds off removes both. Three middle and three fuller lower
banks drift across the globe at different speeds, behind the native angel
and palace, while three horizon banks and three staggered upper banks fill
the sky. The upper deck expands coverage through the high windows using
existing baked shapes; it adds no atlas, shader, pass, or setting. The decks repeat
offscreen and reuse the same four baked shapes. Low-end backdrop off uses flat blue.
Atmosphere off also removes the new artistic horizon-mist band. Native UI
stays above every effect; Palace depth composition uses premultiplied alpha
to preserve soft cloud edges. See the audit for cache and weather limitations.

Source/contract/performance findings are recorded in
[`world-navigation-code-audit.md`](world-navigation-code-audit.md). The real
PPU regression verifies native pixel parity through brightness changes and
foreground/winner ownership. Randomized clipping and fake-backend tests cover
frustum safety, exact receivers, and caller-owned output. Sanitized Metal
verification retains all 246 prior navigation reference captures exactly and
adds six Palace views, clock motion/freeze, shadow toggles, reset recovery and
return-to-navigation image parity.

Native 4:3 snapshots at gf420/500/750 in `runs/20260908-134843/` match the
original Palace control `runs/20260908-130529/` for WRAM, VRAM, CGRAM, OAM,
high OAM and public PPU registers. The earlier daylight/moving-weather preview
is `runs/20260908-135558/sky-palace-preview.gif` (480×360, 763 KiB).
It predates the density-volume cloud revision; the revised native still is
`runs/20260908-151754/sky-palace-volumetric.png`. The new effect's six-run
quality comparison and sanitized/reference checks are in the code audit.
Live on/off restores the original 4:3 gf500/gf600 captures byte-for-byte in
`runs/20260908-135709/`. True 16:9 capture/toggle coverage uses
`AR_WS_HEADLESS=1` and 43-column margins in `runs/20260908-140012/`.
Its off-state gf500/gf600 images and unchanged navigation gf400 match the
always-native Palace control `runs/20260908-140107/` exactly. The final full
suite passes all 148 tests. Both original and isolated populated save hashes
remain unchanged.
These captures are correctness checks, not performance measurements.

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

**Lifecycle (2026-08-05, expanded 2026-08-08): the sideband has
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
and the live diorama view ends at the game-owned
`kActRaiserWidescreenExtraMax = 120`.** The apron remains a
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
stripe down the backdrop, HUD icon loose in the scene). Use
`ActionApron_SurfacePitch` / `ActionApron_DisplayOffset` /
`ActionApron_SurfaceColumn` rather than open-coding it. Keeping pitch and
display width distinct prevents sheared HUD, backdrop stripes, and displaced
HUD icons.

## 14. Open questions (all remaining, none blocks the §13 design)

1. Map `$02AC` and object `$38` selector values to named magic/effect assets;
   the slot-0 armer itself is `$00:96C3-$96F5`.
2. VRAM `$4000-$4FFF` char bank consumer (loaded but unreferenced by the
   in-game NBA regs; another section's OBSEL/NBA values `?`).
3. BG2 filler tile id per section (`$17F` seen in act 1, `$18A` earlier
   in another context) — confirm from `$B825`'s filler constant per layer.
4. Boss-arena window/HDMA effects vs margins (survey doc has the known
   full-width-window fix; iris wipes stay 256-centered — audit per boss).
5. Map the remaining `$02:AFCB` `$47F0` sim upload. World-navigation
   `$02:8384` is now verified as the current-matrix/focus Mode-7 register
   uploader; the town camera writer remains `$01:B4C6`.
