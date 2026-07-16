# ActRaiser ROM Map

**ROM:** ACTRAISER-USA | LoROM | 1MB (32 banks x 32KB) | SlowROM | No coprocessors
**Checksum:** 0x83DB | **CRC32:** EAC3358D

## Interrupt Vectors

| Vector | Address | Purpose |
|--------|---------|---------|
| Emu RESET | $00:8000 | Entry point (boot) |
| Native NMI | $00:8520 | VBlank interrupt handler |
| Native IRQ | $00:8525 | IRQ handler |
| Native COP | $00:8526 | COP software interrupt |
| Native BRK | $00:852F | BRK software interrupt |
| Emu IRQ | $00:D011 | Emulation mode IRQ |

## Bank Layout Overview

| Bank(s) | File Offset | Content | Type |
|---------|------------|---------|------|
| $00 | 0x000000-0x007FFF | Main program code, reset handler, NMI | CODE |
| $01 | 0x008000-0x00FFFF | Program code (continued) | CODE |
| $02 | 0x010000-0x017FFF | Program code, SPC700 driver (0x11ACD) | CODE+DATA |
| $03 | 0x018000-0x01FFFF | Program code, game data tables | CODE+DATA |
| $04 | 0x020000-0x027FFF | Text data (dialogue, names, descriptions) | DATA |
| $05 | 0x028000-0x02FFFF | Map metadata, graphics, palettes | DATA |
| $06-$07 | 0x030000-0x03FFFF | Mixed code and data | CODE+DATA |
| $08-$09 | 0x040000-0x04FFFF | Audio samples (BRR format) | DATA |
| $0A | 0x050000-0x057FFF | Town maps | DATA |
| $0B | 0x058000-0x05FFFF | Mixed | CODE+DATA |
| $0C-$0D | 0x060000-0x06FFFF | Uncompressed graphics | DATA |
| $0E-$1C | 0x070000-0x0E7FFF | Compressed data (graphics, maps, sprites) | DATA |
| $1D-$1E | 0x0E8000-0x0F7FFF | Sparse data (mostly empty) | DATA |
| $1F | 0x0F8000-0x0FFFFF | Empty | EMPTY |

## Detailed Data Regions

### SPC700 Audio Driver
- **0x11ACD-0x12621** (2,901 bytes): SPC700 program uploaded to audio RAM.
  This is the `$02:9ACD` boot upload image (block target ARAM `$0400`); the
  upload/playback protocol it speaks on APU port 0 is decoded in
  docs/SEAMS.md "APU port-0 command protocol".

### Song table and song images ($02:C7E5, decoded 2026-07-16)
- **0x147E5-0x14817**: 17-entry song pointer table, 3-byte (lo/hi/bank)
  pointers to each song's SPC image. Entry 7 (`$1A:94B8`) = the title theme.
  All 17 srcs are enumerated as `[music:]` entries in
  `game-assets/manifest.ini`; a few additional songs arrive via inline
  `[$A2]`-script pointers read through `$02:B4C0` rather than this table.
- **0x32C00+** (`$06:AC00`): the COMMON sample-bank image uploaded once at
  boot — sequence data at ARAM `$2400`, DSP sample directory page at `$2C00`,
  and the stage-2 script installing BRR chunks 0-11 (srcn `$00-$0B`, the
  SFX/shared instruments). Not a song despite living in the same upload path.
- Song images (e.g. title at 0xD14B8 = `$1A:94B8`) carry their own per-song
  instruments as stage-2 chunk indices installed from srcn `$0C` upward —
  the srcn split that lets host music replacement mute music voices while
  keeping SFX authentic.

### Game Data Tables
- **0x1B40E-0x1B431**: Experience level requirements (population-based)
- **0x1B432-0x1B455**: Experience level max SP values
- **0x1B825-0x1B8FC**: Lair enemy data

### Sprite identity and action OBJ assets

| SNES address | File range | Meaning |
|---|---:|---|
| `$00:95DD-$95EC` | `0x015DD-0x015EC` | Eight action handler-table pointers: `$96AF,$A8F6,$B449,$C11E,$CD9B,$D928,$E722,$F39A` for `$18=$00-$07` |
| `$01:E099+` | `0x0E099+` | Town world-object type → behavior/animation-data pointer table |
| `$01:E7D9+` | `0x0E7D9+` | Parallel town world-object type → sprite-frame pointer table; frame lists continue around `$01:E838` |
| `$06:A000+` | `0x32000+` | Dynamic action magic/effect overlay sources selected from object `$38`; uploaded to VRAM `$2D80` |
| `$06:A400+` | `0x32400+` | Action magic-selection table/source used by `$02:BC9E`; uploaded to VRAM `$2D40` |
| `$07:8000-$9FFF` | `0x38000-0x39FFF` | Common action OBJ atlas, 8192 bytes copied to VRAM `$2000-$2FFF` at level entry |
| `$07:D040-$D09F` | `0x3D040-0x3D09F` | Action OBJ palettes, 96 bytes copied to CGRAM `$C0-$EF` |

The bank-0 action handler tables are sparse object-type arrays with no explicit
count. Walk until the nearest forward pointer target (the payload boundary), and
treat zero words as unused type slots rather than termination. `$00:B449` is the
important proof: types `$19-$1D` are zero, while `$1E-$27` resume with ten valid
records; type `$21` points to record `$BB19` and exact handler `$BB25`.
Tables `$A8F6-$E722` correspond to the six ordinary two-act kingdom regions.
`$F39A` is Death Heim's distinct no-act boss-rush/final-boss table. Its `$19`
layout (repaired + user-verified end-to-end 2026-07-14, bug-ledger #20): `$19=1` =
teleport hub, whose spawn record `$F3C8` (handler `$F3D4`) stages the next boss
via `$1A = $0347 + 2`; `$19=2..7` = the six boss arenas; `$19=8` = final boss.
Key code: `$00:FEEC` (end of the `$FE89` teleport-out sequencer) writes
`$0347 = $19 - 1` (rush progress) and `LDA #$0701; STA $1A` (16-bit = stage
`$1A=$01/$1B=$07`, the hub warp), and sets `$0334=1` when `$19==8`. The
boss/summon spawn stubs `$F6D6/F6EE/F6FA/F706/F712/F71E` (+`$F81B`) each
`JSR $F778`, which stashes the stub continuation in object field `$3E,X`
(re-pushed by `$F7C9`, consumed by `$F807`'s RTS). The all-six-regions
completion check is `$00:A343` over `$7F:6B18`.

After Death Heim the ending runs: a mode-0 world montage (`$19=09` alternating
with each town map), then mode `$18=$08` — entered via the fade routine's
special case `$00:82C3` (`CMP #$08` → `LDA #$02; PHA; LDX #$AA9B; PHX; RTL`, a
cross-bank RTL long-jump; the only site of that byte shape in bank 0). The
ending/credits presenter `$02:AA9C` relocates S to `$01FF`, drives 17+ credit
entries via `JSR $02:AB30`, stamps `'A','C','T'` into SRAM `$70:1FF0-1FF2`
(the beat-the-game marker), waits for Start (`$4219` bit 4), and RTL-jumps
back to the main loop top `$00:8059` (the ROM's only other RTL-jump site,
`$02:AAFD`).

These are different identity layers. The action object handler and composition
pointer select behavior/layout within a common resident atlas; the small bank-6
uploads replace reserved effect tiles. Town type tables select behavior and
frame composition, while the ROM-character upload that makes those frame tile
numbers resident in VRAM remains a separate seam to map. A decompilation should
not collapse any of these to raw OAM tile numbers.

### Town Building Data (0x1DCFA-0x1DFF9)
128 bytes per town, 6 towns:
| Town | Offset |
|------|--------|
| Fillmore | 0x1DCFA-0x1DD79 |
| Bloodpool | 0x1DD7A-0x1DDF9 |
| Kasandora | 0x1DDFA-0x1DE79 |
| Aitos | 0x1DE7A-0x1DEF9 |
| Marahna | 0x1DEFA-0x1DF79 |
| Northwall | 0x1DF7A-0x1DFF9 |

### Text Data (Bank $04: 0x20000-0x27FFF)
| Range | Content |
|-------|---------|
| 0x20000-0x2000D | Town name pointers |
| 0x2000E-0x20042 | Town names |
| 0x20043-0x2004A | Enemy name pointers |
| 0x2004B-0x020076 | Enemy names |
| 0x20077-0x21396 | Angel dialogue |
| 0x21523-0x246AD | Town dialogue |
| 0x246AE-0x24C99 | Offering descriptions |
| 0x24C8A-0x258F2 | Ending sequence text |
| 0x258F3-0x25EF2 | Text compression dictionary |

### Graphics & Maps
| Range | Content |
|-------|---------|
| 0x28000-0x28E3F | Map metadata |
| 0x2CE7F-0x2EE7E | Uncompressed graphics |
| 0x2EE7F-0x2FF7F | Compressed graphics (LZSS) |
| 0x2FF80-0x2FFFF | Map palettes |
| 0x50000-0x52FFF | Town maps (base + obstacle layers) |
| 0x60000-0x6FFFF | Uncompressed graphics (large block) |

### Audio Samples (0x40000-0x4FD2D)
32 BRR-encoded sound samples (indices 0x00-0x21).
Each sample has a 16-bit length header followed by BRR audio data.
This is the stage-2 chunk pool (`$08:8000`) scanned linearly by the `$02:9964`
upload HLE: each song image's terminator doubles as a script of chunk indices
selecting which samples stream into ARAM (common bank = chunks 0-11 → srcn
`$00-$0B`; per-song instruments land at srcn `$0C+`). See SEAMS "Audio".

### Compressed Data (0x70000+)
Extensive compressed sprite composition, map arrangement, and tileset data
using Quintet's standard LZSS compression algorithm.

## Compression Format

ActRaiser uses **Quintet's standard LZSS** with a 256-byte sliding window.
The same compression is used across other Quintet games (Soul Blazer, Illusion of Gaia, Terranigma).

Decompression state in RAM:
- Sliding window buffer: $7E:2000-$7E:20FF (256 bytes)
- Input pointer: $7E:00A5-$7E:00A7
- Window position: $7E:00AF-$7E:00B0
- Bit weight: $7E:00AE (shifts 0x80→0x01)
- Output size: $7E:00B3-$7E:00B4
- Output destination: $7E:00B5-$7E:00B6

## Notes

- Map metadata format is similar to other Quintet games
- Most platformer-side numeric values use BCD encoding
- ~28% of ROM is mapped to known regions; 72% needs further analysis (mostly compressed data in banks $0E-$1C)
