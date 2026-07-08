# ActRaiser RAM Map

SNES WRAM: 128KB at banks $7E-$7F.
Direct page and stack in first 8KB ($7E:0000-$7E:1FFF), mirrored at $00-$3F:0000-$1FFF.

## Core Game State ($7E:0000+)

### Game Mode & Navigation
| Address | Size | Description |
|---------|------|-------------|
| $7E:0018 | 1 | Game mode (0x00=Non-Platformer/Sim, 0x01=Platformer) |
| $7E:0019 | 1 | Current map number (second byte of map ID) |
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
| $7E:0341 | 1 | Current town ID |
| $7E:0347 | 1 | Completion flag (0x07 after defeating Death Heim) |

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
Two bytes per town tracking act completion counts.

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
