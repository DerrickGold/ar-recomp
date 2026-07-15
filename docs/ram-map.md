# ActRaiser RAM Map

SNES WRAM: 128KB at banks $7E-$7F.
Direct page and stack in first 8KB ($7E:0000-$7E:1FFF), mirrored at $00-$3F:0000-$1FFF.

## Core Game State ($7E:0000+)

### Game Mode & Navigation
| Address | Size | Description |
|---------|------|-------------|
| $7E:0018 | 1 | Mode/region group: `$00` non-action (town/world/UI); `$01-$06` six two-act kingdom action regions; `$07` Death Heim boss-rush/final-boss action region (no ordinary acts); `$08` ending/credits (post-Death-Heim: mode-0 world montage cycling `$19=09`↔towns, then `$18=08` — presenter at `$02:AA9C`, stamps 'ACT' into SRAM `$70:1FF0`, waits for Start, exits to `$00:8059`) |
| $7E:0019 | 1 | Current raw map/sub-flow number (second byte of map ID). Not a uniform act selector: Act 2 starts at `$02/$02/$03/$04/$04/$05` for regions `$01-$06`; Death Heim uses `$01` for its hub, `$02-$07` for the six rematch arenas, and `$08` for the final boss |
| $7E:001A | 1 | Destination map number |
| $7E:001B | 1 | Destination map group (first byte of map ID) |

### Platformer Stats
| Address | Size | Description | Notes |
|---------|------|-------------|-------|
| $7E:001C | 1 | Lives remaining | |
| $7E:001D | 1 | Current HP | |
| $7E:001E | 1 | Maximum HP | |
| $7E:001F | 2 | Score | BCD format |
| $7E:0021 | 1 | Magic points | |
| $7E:00E6 | 2 | Time remaining | BCD format |
| $7E:08BC | 1 | Player **Crest** walking-cycle phase | TAS terminology; interacts with Boost to determine normal/pre-jump movement cadence. Player object `$08A0 + $1C`. |
| $7E:08C4 | 1 | Player **Boost** walking-speed countdown | Can produce temporary 3 px/frame movement; player object `$08A0 + $24`. Extended `AR_FRAMELOG=1` records both fields with input and position delta. |

### Camera / Scroll — full model in rendering-engine.md §4/§6/§11.1
| Address | Size | Description |
|---------|------|-------------|
| $7E:0022 | 2 | BG1/camera X. Action writer `$02:B091`, clamp `[0,$2E-$100]`; town writer `$01:B4C6`, clamp `[0,$0100]`. All six scroll regs upload from `$22-$2D` via `$02:ADC3` (10-bit). |
| $7E:0024 | 2 | BG1/camera Y. Action clamp `[0,$30-$E1]`; town writer `$01:B4C6`, clamp `[0,$011F]`. |
| $7E:0026/$0028 | 2+2 | BG2 H/V scroll (parallax, $02:B9D5/$02:BA0B from ratio nibbles $3A-$45) |
| $7E:002A/$002C | 2+2 | BG3 H/V scroll ($2C pinned $FFFC: HUD up 4px) |
| $7E:002E/$0030 | 2+2 | **BG1 layer = LEVEL pixel width/height** (Fillmore act1: 4096x768) — the camera clamp bounds |
| $7E:0032/$0034 | 2+2 | BG2 layer width/height (scroll clamps only if width >= $300, else wraps) |
| $7E:003A-$0045 | 12 | per-plane parallax ratio nibbles (from section config table $02:893E+7..12) |
| $7E:005E/$0060/$0062/$0064 | 2 ea | record-buffer cursors: BG1col $3900 / BG1row $3A02 / BG2col $3B04 / BG2row $3C06 |
| $7E:007C/$007E | 2+2 | camera H/V delta this frame (16-bit signed; strip triggers) |
| $7E:008E | 1 | parallax disable bits (bit0 BG2H, bit1 BG2V = script-driven) |
| $7E:0093 | 1 | strip-request flags: $80 BG1col $40 BG1row $20 BG2col $10 BG2row (set by $02:B091 on 16px crossings, TRB-consumed by dispatcher $02:B127) |

### OAM shadow + sprite-build working vars

The 544-byte shadow and DMA are common, but action (`$00:8C98/$00:8D68`)
and town (`$01:ACD9/$01:ADAD/$01:AE6F`) rebuild it independently.
| Address | Size | Description |
|---------|------|-------------|
| $7E:0380 | 512 | OAM shadow: 128 x 4-byte entries (x, y-1, tile, attr); cleared to x=$80,y=$E0 each frame via a stack-push fill |
| $7E:0580 | 32 | OAM high table shadow: 2 bits/sprite (bit0 = x bit 8, bit1 = size), packed 4 sprites/byte |
| $7E:0000 | 1 | (during sprite build) high-table bit accumulator — bits ROR'd in from the top, flushed every 4 sprites |
| $7E:000C/$000E | 2 ea | sprite-build counters/scratch; exact ownership is routine-specific. Town `ADAD/AE6F` obtains the part count from byte 0 of the frame definition, not from world record `+0E`. |
| $7E:0014 | 2 | (during sprite build) object screen-x + 16 (draw-window bias) |
| $7E:0016 | 2 | (during sprite build) object screen-y + 16 |
| $7E:008F | 2 | sprite attr OR-bias; $0E00 TSB'd while object has $30&$2008, TRB'd at builder exit |
| $7E:0094 | 2 | camera X - 16 (sprite draw origin, set by $00:8C98 prologue) |
| $7E:0096 | 2 | camera Y - 16 |
| $7E:009A | 2 | high-table write cursor (starts $0580) |
| $7E:009C | 2 | high-table bit slots remaining in current byte (4..1) |
| $7E:009E | 2 | current object's flip/attr word (obj+$28 ^ $0100) |

### Town simulation render records and camera auxiliaries

| Address | Size | Description |
|---------|------|-------------|
| $7E:06A0-$09FF | 48 × $12 | Fixed-screen/overlay animation records. `$01:ACD9` tests `+10 & $8000`, runs `$01:AC70`, and emits with camera-independent origins. |
| $7E:0A00+ | 44 × $26 | Town world-object records. Known render fields: `+08` frame-composition pointer, `+0A/+0C` world X/Y, `+10` render status (`$C000` = skip), `+25` delay/timer. `+12` is a behavior dispatch selector outside the OAM leaf. |
| $7E:0AEE/$0AF0 | 2+2 | Town camera-follow target X/Y read by `$01:B4C6`; camera derives `$22=$0AEE-$80`, `$24=$0AF0-$70` before clamping. |
| $7F:9752 | 1+ | bit 1 selects town alternate OAM emitter `$01:AE6F` for the world segment. |
| $7F:9754 | 1+ | nonzero reduces the normal 44-record town world scan to one record. |
| $7F:9F65/$9F67 | 2+2 | transient town camera shake X/Y. Applied only if resulting camera remains inside `$22<=$0100`, `$24<=$011F`, then cleared. |

### Upload records + NMI descriptors (rendering-engine.md §2/§3/§7/§10)
| Address | Size | Description |
|---------|------|-------------|
| $7E:0076/$0079 (+banks $78/$7B) | 2+1 ea | NMI record-drain pointers — reset EVERY NMI by $02:ACC8 to $3900/$3A02 then $3B04/$3C06 (game-side reads see the resting values; not a game variable) |
| $7E:3900/$3A02/$3B04/$3C06 | $102 ea | the four one-record upload buffers (BG1 col/row, BG2 col/row): +0 header = VRAM base word (0=empty, zeroed after drain), data = 4x64B chunks at +2/+$42/+$82/+$C2. Column records use VMAIN=$81 and target `base,+1,+$800,+$801` (32 words, stride `$20`); row records use VMAIN=$80 and target `base,+$20,+$400,+$420` (32 contiguous words). |
| $7E:00C4-$00CA | — | fade gate/config + BG2SC page-flip anim counters ($C5 arm, $C7 page) |
| $7E:00CB/$00CD/$00CE/$00CF | 2+1+1+1 | CGRAM upload descriptor: src addr/bank, CGADD, row count ($02:AE75) |
| $7E:00D0-$00D6 | 7 | VRAM DMA descriptor slot 0: src16/bank/VMADD/size (size=0 idle; $02:AF30) |
| $7E:00D7-$00DD | 7 | VRAM DMA descriptor slot 1 = tile-anim upload (armed by $02:BC56; frames at [$D9]:$D7 = $7F:B800+n*$E1) |
| $7E:00DE-$00E1 | 1+1+1+2 | tile-anim: tick period mask / frame count-1 / frame index / frame stride (bytes); $FF/$FF/-/0 = disabled |
| $7E:00F1 | 1 | one-shot flag: re-stream BG3 map rows 4-26 ($7F:B100 -> VRAM $5880) next NMI |
| $7F:B000-$B6BF | 1728 | HUD/BG3 tilemap compose buffer (rows 0-3 streamed every frame to VRAM $5800; rows 4-26 on $F1) |
| $7F:B800+ | var | tile-anim frame buffers (action default; sim uses ROM $0A directly) |

### Mode 7 / World Map
| Address | Size | Description |
|---------|------|-------------|
| $7E:0314 | 2 | Map rotation (intro zoom effect) |
| $7E:0316 | 2 | Current zoom level |
| $7E:0318 | 2 | Target zoom level |

## Decompression State ($7E:00A0+)

| Address | Size | Description |
|---------|------|-------------|
| $7E:00A5 | 3 | Long pointer to compressed input byte |
| $7E:00AB | 3 | Long pointer to current music data |
| $7E:00AE | 1 | Bit weight (0x80, 0x40... 0x01) |
| $7E:00AF | 2 | Sliding window position |
| $7E:00B1 | 2 | Source position in sliding window |
| $7E:00B3 | 2 | Output size |
| $7E:00B5 | 2 | Output destination |
| $7E:00B7 | 2 | Pastcopy scratch space |
| $7E:2000 | 256 | Sliding window buffer |

## Settings & Interface

| Address | Size | Description |
|---------|------|-------------|
| $7E:0200 | 1 | Text display speed |

## Town Simulation Data ($7E:0200+)

### Population (two-byte entries, BCD format)
| Address | Size | Description |
|---------|------|-------------|
| $7E:0218 | 2 | Total population |
| $7E:021A | 2 | Most recently visited town |
| ... | 2 each | Individual town populations (Fillmore→Northwall) |

### Growth Rates ($7E:0228-$7E:022D)
One byte per town. Values:
- 0x00 = None
- 0x01 = Stop
- 0x02 = Slow
- 0x03 = Normal
- 0x04 = Fast
- 0x05 = Maximum

### Technology Levels ($7E:022E-$7E:0239)
Two-byte entries per town.

### Offerings ($7E:023A-$7E:0281)
- Counts: $7E:023A-$7E:0245 (two-byte entries)
- Inventories: $7E:024C-$7E:0281 (nine bytes per town)

## Angel & Master Data ($7E:0280+)

### Angel
| Address | Size | Description |
|---------|------|-------------|
| $7E:0282 | 2 | Current skill points |
| $7E:0284 | 2 | Maximum SP |
| $7E:0286 | 1 | Current HP |
| $7E:0287 | 1 | Maximum HP |

### Master (Player Character)
| Address | Size | Description |
|---------|------|-------------|
| $7E:0288 | 9 | Name |
| $7E:0291 | 2 | Level |
| $7E:0293 | 2 | HP |
| $7E:0295 | 2 | Magic points — PERSISTENT copy. `$21` is the act/working copy, loaded from here at `$02:84E0` (`LDA $0295; STA $21`); act-mode pickups INC only `$21` ($00:887E); sim reward grants INC BOTH via long addressing (`$01:9CD6`, DEBUG.md #18b). New-game STZ at $02:BE69. No other direct writers in ROM — stats-block writes use `AF/8F`-form long addressing |
| $7E:0297 | 2 | Population needed for next level |
| $7E:0299 | 9 | Magic inventory |
| $7E:02A2 | 9 | Offerings inventory |
| $7E:02AB | 1 | Number of lives (max-HP-style grant handler `$01:9CBD` INCs it) |

### Platformer Score Records ($7E:02B3+)
24 entries (6 towns x 2 acts x 2 bytes each).

## Temple & Gameplay State

| Address | Size | Description |
|---------|------|-------------|
| $7E:033E | 1 | Temple action (0x00=Give Oracle, 0x01=Listen, 0x02=Take Offering) |
| $7E:0334 | 1 | Death Heim/ending state: `$00:FEFC` sets 1 when final-boss teleport-out runs with `$19==8`; `$00:F650` sets 3 only after the returning `0701` sky fade-in and `$0349` wait, so it is too late to identify the black-frame BG page swap (`$00:F5F0-$F619`, BG1SC/BG2SC `$64/$74`) |
| $7E:0341 | 1 | Current town ID (also read by the `$00:A375` post-Death-Heim return stager as the pending destination) |
| $7E:0347 | 1 | Death Heim boss-rush progress: `$00:FEEC` writes `$19 - 1` after each boss (hub stager `$F3D4` warps to `$0347+2` next); 0x07 = final boss beaten |

## Debug / System

| Address | Size | Description |
|---------|------|-------------|
| $7E:035A | 1 | Accumulator low byte at last COP instruction |
| $7E:035B | 1 | Accumulator low byte at last BRK instruction |

## High RAM ($7F:0000+)

### Graphics & Map Data
| Address | Size | Description |
|---------|------|-------------|
| $7E:4000+ | varies | Metadata for map variant [01 00] |
| $7E:5000+ | varies | Metadata for map variant [01 01] |
| $7E:6000-$7E:7FFF | 8KB | Graphics metadata |
| $7F:2000+ | varies | Arrangement data |
| $7F:6800+ | varies | Road construction data (one word per 4x4 block) |
| $7F:B000-$7F:B7FF | 2KB | BG3 tilemap |

### Act Completion ($7F:6B18-$7F:6B23)
Two bytes per town tracking act completion counts. `$00:A343` (Death Heim exit
stager) requires all six words == 2 for the all-bosses-done path.

### Building Direction UI
| Address | Description |
|---------|-------------|
| $7F:6B9F-$7F:6BAA | X positions (6 towns) |
| $7F:6BAB-$7F:6BB6 | Y positions (6 towns) |

### Monster Lair Data ($7F:9500+)
Arrays supporting up to 16 lairs (48 bytes each array):
| Address | Description |
|---------|-------------|
| $7F:9568-$7F:9597 | Lair X positions on town map |
| $7F:9598-$7F:95C7 | Lair Y positions |
| $7F:95C8-$7F:95F7 | Lair imagery ID |
| $7F:95F8-$7F:9627 | Monster type |
| $7F:9628-$7F:9657 | Respawn delay |
| $7F:9658-$7F:9687 | Respawn countdown |
| $7F:9688-$7F:96B7 | WRAM address of monster statistics |
| $7F:96B8-$7F:96E7 | Remaining monster count |
| $7F:9750 | Lightning sequence trigger |

### Flags
| Address | Description |
|---------|-------------|
| $7F:9101 | Death Heim-related (bits 0x01, 0x02) |
| $7F:910B | Bloodpool bridge technology (bit 0x20) |

### Town Growth Points ($7F:9EFA+)
Two-byte entries per town tracking accumulated growth from monster defeats and lair seals.

### Road Construction Encoding ($7F:6800+)
- Bit 0x40: Obstructs building direction selector
- Bit 0x200: Shows obstacle layer instead of base
- Example values: `[29 38]`=straight road up, `[38 F8]`=crossroads, `[3A C8]`=horizontal road

## Lair Reference Data

### Lair Image IDs
0x00-0x10: Cave, Castle, Great Tree, various Lair symbols, Hole, Pyramid, Temple

### Monster Types
- 0x12: Blue Dragon
- 0x13: Napper Bat
- 0x14: Red Demon
- 0x15: Skull Head

## Notes
- Most platformer-side numeric values use BCD encoding
- Save data is stored in 8KB battery-backed SRAM
- The game mode byte at $7E:0018 is the primary state machine driver

## Cheat-derived WRAM map (from ./codes.txt — flamingspinach's PAR codes, parsed 2026-07-06)

Independently-engineered address ground truth; every row doubles as a debug cheat via the
generic pinner: `AR_PIN=<8-hex PAR>[,...]` (applies every frame, all modes; see
actraiser_rtl.c). Pin VALUES are the cheat's pin, not a semantic constant. NOTE from the
source doc: many counters are stored as decimal-looking hex (screen "28" = $28).

| Addr | Label (pin value) | Notes |
|---|---|---|
| `$7E:001D` | INF HP ($08) | player HP (matches our AR_INF_HP cheat) |
| `$7E:001C` | INF LIVES ($01) |  |
| `$7E:00E6` | INF TIME ($01) |  |
| `$7E:08D1` | INVULNERABILITY ($20) |  |
| `$7E:0BCC` | NO BOSS HEALTH 1 ($00) |  |
| `$7E:134C` | NO BOSS HEALTH 2 ($00) |  |
| `$7E:0CCC` | NO BOSS HEALTH 3 ($00) |  |
| `$7E:130C` | NO BOSS HEALTH 4 ($00) |  |
| `$7E:0ECC` | NO BOSS HEALTH 5 ($00) |  |
| `$7E:0021` | INF MP ($0A) | **MP / magic-scroll count** — WORKING copy of persistent `$0295` (see Master block above); pinning this gives castable MP but does not persist |
| `$7E:0282` | INF SP ($FF) | SP is 16-bit ($0282/$0283) |
| `$7E:00E4` | RANGED SWORD ($80) |  |
| `$7E:001F` | MAX SCORE ($99) |  |
| `$7E:0020` | MAX SCORE ($99) |  |
| `$7E:00C3` | ROOM ALWAYS LIT ($00) |  |
| `$7E:02A2-$02A9` | item slots 1-8 (item ids) | 16 item ids exist; 8 story-critical (codes 16-23 pin the useful set) |
| `$7E:0286` | INF HP [SIM] ($08) |  |
| `$7E:022E` | MAX QUALITY 1 ($03) | town quality, stride 2, six towns ($022E..$0238) |
| `$7F:9EFA-$9F04` | INF SOUL POINTS 1-6 (40) | per-town soul/population points, stride 2 (codes 31-36) |
| `$7E:0299-$029C` | HAVE FIRE/STARDUST/AURA/LIGHT (01/02/03/04) | spell-unlock flags, one byte each: Fire/Stardust/Aura/Light (§7.18 secondary) |
| `$7F:96B8-$96E7` | SAFE <town> 1-4 (00) | **lair-sealed state array**: 2 bytes/lair × 4 lairs × 6 towns, town order Fillmore/Bloodpool/Kasandora/Aitos/Marahna/Northwall (codes 41-88) |
