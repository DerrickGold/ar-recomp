# ActRaiser ROM Map

**ROM:** ACTRAISER-USA | LoROM | 1MB (32 banks x 32KB) | SlowROM | No coprocessors
**Checksum:** 0x83DB | **CRC32:** EAC3358D

Action/title text consumers are mapped in the
[dialogue reference](dialogue-system.md#action-hud-cards-and-title-options):
`$00:A851-$A8EF` fixed stage records, `$02:8E7E` packed action HUD template,
`$00:A4D6` ENEMY strip, `$02:A92F` title HDMA mode/screen bands, and
`$02:A9A7/$A9D6/$AA60` title choices. These use `$02:BF60` and its inverse
eraser `$02:C1B7`; they do not enter the interactive dialogue grammar.

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
| $0A | 0x050000-0x057FFF | Town maps, world-map water animation | DATA |
| $0B | 0x058000-0x05FFFF | Mixed | CODE+DATA |
| $0C-$0D | 0x060000-0x06FFFF | Uncompressed graphics | DATA |
| $0E-$1C | 0x070000-0x0E7FFF | Compressed data (graphics, maps, sprites) | DATA |
| $1D-$1E | 0x0E8000-0x0F7FFF | Sparse data (mostly empty) | DATA |
| $1F | 0x0F8000-0x0FFFFF | Empty | EMPTY |

## Detailed Data Regions

The bank overview above is **US-specific**. European English/German/French
store separate Story and Action placement indices at `$1F:8000/$8002`,
pointing to offsets`$0004/$1485` relative to `$1F:8000`; bank1F is not empty
in those ROMs. Their loader `$00:8D72–8DCD` selects by title choice2.
The initial/wave filters at `$00:8EDA–8F5F` / `$00:8FDD–9074` consume
difficulty markers only for regional object types. The initial-HP adjustment
is `$00:916B–9197`. These ranges match across the three PAL code profiles.
Title, timer and contact addresses relocate independently; use the
[European address table](regional-differences-technical.md#selection-and-timer).
Never use the PAL placement offset as a US donor address.

Two PAL boss source/program ranges are EU `$00:D30D–D4F7` (Aitos Act1
dragon04/0D) and `$00:D635–D848` (Marahna Act1 plant05/05). German adds2,
French adds5. Difficulty changes descendant spawning or tendril motion;
animation timings must be interpreted through PAL `$00:8967`, not the US
consumer address. See [boss contracts](regional-differences-technical.md#european-boss-difficulty-branches).

PAL pickup/icon art uses three distinct raw tables: eight128-byte entries
at `$06:A000` (Story), eight at `$06:A800` (Action), and five at `$06:AC00`
(current spell/empty icon). The native selectors request these independently
of logical item effects. [Item contracts](regional-differences-technical.md#european-items-and-spell-inventory)
record the verified source/VRAM addresses and mode-specific inventory rules.

The three PAL profiles retain US table addresses and bytes for level thresholds
`$03:B40E` (36 bytes), maximum SP `$03:B432` (36), lair seeds `$03:B825`
(216), and SIM species stats `$01:B061` (12). Native consumer fixtures use
the PAL RAM layout; matching ROM addresses do not make US RAM addresses safe.
Save/load `$03:A656/$A83A` persists difficulty in the existing0200-block.
See [SIM contracts](regional-differences-technical.md#european-simulation-numeric-rules)
and [native save ordering](regional-differences-technical.md#european-story-save-and-continue).

### SPC700 Audio Driver
- **0x11ACD-0x12621** (2,901 bytes): SPC700 program uploaded to audio RAM.
  This is the `$02:9ACD` boot upload image (block target ARAM `$0400`); the
  upload/playback protocol it speaks on APU port 0 is decoded in
  [native audio channels](snes-native-audio-channels.md).

### Song table and song images ($02:C7E5)
- **0x147E5-0x14826**: 22-entry pointer table, 3-byte (lo/hi/bank)
  pointers to SPC images. The earlier 17-entry survey covered only a prefix.
  Zero-based entry 7 (`$1A:94B8`) is the title theme; entry 20 (`$06:AB8F`)
  is silence. The playback API and Music Mode use **one-based** IDs 8 and 21
  for those entries. Scene scripts can also request images through inline
  `[$A2]` pointers read by `$02:B4C0`, independently of this table.
- **0x32C00+** (`$06:AC00`): the COMMON sample-bank image uploaded once at
  boot — sequence data at ARAM `$2400`, DSP sample directory page at `$2C00`,
  and the stage-2 script installing BRR chunks 0-11 (srcn `$00-$0B`, the
  SFX/shared instruments). Not a song despite living in the same upload path.
- Song images (e.g. title at 0xD14B8 = `$1A:94B8`) carry their own per-song
  instruments as stage-2 chunk indices installed from srcn `$0C` upward —
  the srcn split that lets host music replacement mute music voices while
  keeping SFX authentic.

### Debugging and unassigned routines

The following ranges were bounded with Go disassembly in all five pinned
ROMs. Range ends are exclusive. Separate memory-only patched-ROM tests
exercise the retained menus and a subset of US/JP controls; these do not
establish an original activation path.

| Release | Music Mode | Indexed playback | Music pointer table | Debug music range |
| --- | --- | --- | --- | --- |
| US | `$02:97D4–9871` | `$02:98B7–9914` | `$02:C7E5` | 1–22 |
| Japan | `$02:9567–9604` | `$02:964A–968E` | `$04:9845` | 1–20 |
| European English | `$02:97D4–9871` | `$02:98B7–9914` | `$02:CDFE` | 1–22 |
| German | `$02:97D4–9871` | `$02:98B7–9914` | `$02:CE07` | 1–22 |
| French | `$02:97BE–985B` | `$02:98A1–98FE` | `$02:CDF0` | 1–22 |

All menus initialize music/effect counters to 1; the effect maximum is 38.
Left/Right decrement/increment music, Up/Down decrement/increment effects,
B calls indexed playback, Y posts effect ID OR `$80` with COP, and Select
closes. US scratch counters are DP `$10/$12`. The close path composes blank
text, restores TM and waits for button release; it makes no audio request.
The local wait helper only polls `$4210`.

Controlled five-ROM runs confirm initial counters, both bounds, one step per
held direction, music selection 2 playback and effect 2 playback. The effect
test has a matched no-Y audio control. The action timer stays unchanged while
the menu is open; after Select, the selected music source stays unchanged,
the timer resumes and Right moves the player. This covers selected playback
commands, not every music/effect ID. The entry adapter runs before the native
pause call at `$00:8066`, preserves P/A/X/Y/DB and replays that replaced call.
It is research instrumentation, not recovered retail code.

Playback computes `(ID - 1) * 3`, loads the table's 24-bit pointer and uploads
that image. In all five ROMs, ID 21 points to the same silent payload: US/JP
`$06:AB8F`, all PAL `$06:B38F`. JP's smaller debug range is not a shorter
underlying table. The ordinary ending uses ID 21 directly; see the
[ending caller and scene-declaration distinction](regional-differences-technical.md#extra-palace-music-resource-silent-upload).

US `$00:8151–8228` / JP `$00:814B–8222` retain action debug controls, followed
by the data string `0123456789ABCDEF`. The additional scene/coordinate block
starts at US `$00:8179` / JP `$00:8173`. The supplied enable patch changes the
preceding BRA operand from `$E3` to `$0B` at US `$00:816D` / JP `$00:8167`,
redirecting the loop into that block. It also substitutes `JSR $8151` /
`JSR $814B` for three NOPs at US `$00:8475` / JP `$00:846F`. These are part
of the multiplication helper US `$00:846E–8481` / JP `$00:8468–847B`: write
`$4202/$4203`, wait through four NOPs, then read `$4217/$4216`. Do not label
the original NOPs a disabled debug call.

The supplied patch was tested on memory-only copies restored into
title-selected Fillmore action scenes. Both regions accepted Music Mode
counter changes, but failed to resume playable action after menu exit.
This is not an unmodified-game defect. The hook violates its surrounding
call contract: menu composition calls another hardware multiplier at US
`$02:BED3–BEE6` / JP `$04:8EC9–8EDC`, overwriting the pending product, and
the debug body does not preserve the caller's X/Y. Entry can also occur while
action processing has NMI disabled. Western playback re-enables NMI; JP
playback does not change that gate. These contexts explain why reaching the
menu does not establish a safe activation patch.

An independent frame-boundary controller adapter, retaining the branch
redirection but not the multiplication hook, successfully exercised US/JP
R+Start, room stepping, area stepping and coordinate display. Room/area
checks covered only `$01/01 → $01/02` and `$01/01 → $02/01`, using native
scene loading and spawn positions; no player/camera coordinates were edited.
R+L wrote the expected four hexadecimal values into US `$035D` / JP `$0348`
and rendered them on the bottom row. Start-pause and the controller's wait
can compete depending on input phase; these test adapters are not proposed
user-facing patches. X+A, complete room cycles and moving-coordinate safety
were not play-tested.

The corresponding 240-byte debug block is absent at the homologous site in
all three PAL ROMs. That block comprises 215 bytes of controller/display code,
the 16-byte hexadecimal alphabet and the nine-byte room-limit table
`09 04 08 06 07 08 08 08 FF`. The checked adjacent ranges are:

| Release | Preceding world-loop tail | Debug code and supporting data | Following scene-transition prefix |
| --- | --- | --- | --- |
| US | `$00:8129–8151` | `$00:8151–8241` | `$00:8241–8276` |
| Japan | `$00:8129–814B` | `$00:814B–823B` | `$00:823B–8270` |
| All PAL | `$00:8130–8158` | No intervening block | `$00:8158–818D` |

The following prefixes have the same 26-instruction mnemonic/addressing-mode
sequence, with regional addresses and direct-page fields retained in the
comparison. Whole-ROM searches find neither the exact debug-entry signature
nor `0123456789ABCDEF` in PAL. There are also no direct `JSL` byte patterns to
the respective PAL Music Mode entry, or matching bank-2 `JSR` patterns. US
and JP each have the known debug-controller `JSL`. These are bounded static
and raw-pattern checks, not a proof against every relocated or indirect
debug mechanism. Music Mode works through an explicit test adapter in all
five ROMs, but an original PAL activation route has not been established.

Two lair-code oddities also occur in all five ROMs:

| Routine | US and all PAL | Japan | Established behavior |
| --- | --- | --- | --- |
| Delay mutator | `$03:B6BF–B6E2` | `$03:B448–B46B` | Updates four current-town reload words to `(delay >> 2) + 1`; caller unknown |
| Empty helper | `$03:B560` | `$03:B2EC` | Single `RTS`; following routine has a separate entry |

The two empty-helper calls are US/all PAL `$03:BAC0/$03:BAFC`, JP
`$03:B849/$03:B885`. Exact Go windows trace them from the lair-processing
roots `$03:BA42/$03:BADD` (JP `$03:B7CB/$03:B866`). The configured US control
cross-reference agrees, but finds no caller for the delay mutator or debug controller.
This inventory is limited to its configured/static roots: it also omits the
known Music Mode call inside the dormant controller. An absent cross-reference
therefore does not prove a routine globally unreachable. Neither an empty
helper nor an unassigned routine establishes a removed feature.

### Developer inscription in the Palace graphics

The tile-drawn label `基本パーツ中世` / `(まち)` is present in all five pinned
ROMs, in the raw 4bpp graphics at file `$06C000–070000` (start `$0D:C000`,
16 KiB). It is not encoded dialogue or part of the Mode-7 world-map characters
at file `$070000`. The scene-script census finds this character source only
in Sky Palace `00/07` and temple `00/08`; both use command-7 operands
`80 20 00 00 C0 06`, loading the complete bank.

In a 16-tile-wide sheet, the two-line caption occupies rows 18–24, tile range
`$120–18F` including intervening blanks. Its raw file span is
`$06E400–06F200`, SNES `$0D:E400–F200`, end-exclusive. All five caption spans
match SHA-256 `0e8d395700b522c6204e36b62c346bce80275aa8d9293f8d945ac0068e43aba7`.
Independent renders using the installer Go 4bpp decoder and the existing
snapshot decoder match at all 128×56 pixels. These are diagnostic grayscale
tile sheets, not in-game screenshots or an assertion of native palette use.

Each scene's command-5 declaration uses the same raw 2,048-byte metatile
table within its release:

| Release | Table file offset | SNES address |
| --- | --- | --- |
| US | `$0CA01A` | `$19:A01A` |
| Japan | `$0C8000` | `$19:8000` |
| European English | `$0CA01E` | `$19:A01E` |
| German and French | `$0C9800` | `$19:9800` |

All 1,024 big-endian tile words per table, after the non-action `$FDFF` mask,
select only character indices `$000–0FF`. None selects a caption tile.
This establishes that the shared bank loads the label but the authored
Palace/temple backgrounds omit it. Dynamic tilemap writes and other indirect
consumers have not been exhaustively audited; no normal display route or
earlier-build history is claimed.

### Game Data Tables
- **US `$03:8111-$811C`, JP `$03:810E-$8119`**: six little-endian WRAM
  pointers to town actor-cache slices (US `$97DA + town*$130`, JP `$97CE + town*$130`).
  Each slice saves eight live SIM records; the `$720`-byte aggregate persists
  at SRAM `$1633-$1D52`. [Native cache/save ownership](save-format.md#35-lairs-growth-and-sim-actor-cache).
- **US `$01:B061-$B06C`, JP `$01:B031-$B03C`**: SIM enemy combat tables,
  four bytes each for SP reward, contact damage, and accumulated arrow-damage
  threshold, in Blue Dragon/Bat/Red Demon/Skull Head order. Threshold is one
  less than ordinary-arrow hits to kill. This is distinct from action spawn
  records and from SIM AI state tables; [regional values and consumers](regional-differences-technical.md#sim-enemy-state-differences).
- **US `$01:E099`, JP `$01:E023`**: SIM behavior-program pointer table.
  Skull programs `$16,$1F-$26,$2E-$33` have byte-identical contents at relocated
  pointers, including visual IDs, durations and signed X/Y steps. Set/advance
  consumers are US `$01:D072/$D08F`, JP `$01:CFFC/$D019`. Program equality is
  not sprite-pixel identity or whole-world AI equivalence; see the
  [native Skull contract](regional-differences-technical.md#skull-head-target-and-earthquake-state-contract).
- **US 0x1B40E-0x1B431 / JP 0x1B1DF-0x1B202**: 18 population-based level
  thresholds, final9999 sentinel. Consumed by US `$03:B3BA` / JP `$03:B18B`;
  the award wrapper handles level17 and publishes zero as its next threshold.
  [Native award and switching contracts](regional-differences-technical.md#population-switching-established-boundaries).
- **US 0x1B432-0x1B455 / JP 0x1B203-0x1B226**: 18 identical maximum-SP
  entries, indexed by the pre-award level. Not a current-SP refill table.
- **0x1B825-0x1B8FC** (`$03:B825`): **Monster-lair seed table** — 24 records × 9 bytes
  (4 lairs per town × 6 towns), installed by `$03:B7C6`:
  `[cellX, cellY, imageId, monsterType, count, respawnDelay(word), worldRecordAddr(word)]`.
  X/Y are 16px town-map cells 0..31. Dump with `tools/act_content.py --lairs`; field
  semantics in [RAM map: Monster Lair Data](ram-map.md#monster-lair-data-7f9500).
- **US `$03:BC8A`, JP `$03:BA13`**: 17 lair/landmark picture-list pointers,
  consumed by `$03:BC42/$03:B9CB`. Entries contain a byte count and
  `{cell_dx, cell_dy, structure_metatile}` triples. Skull lair IDs 6/9/10
  and pyramid ID 15 have confirmed regional pixels; see
  [artwork selectors and bank rules](regional-differences-technical.md#town-title-and-death-heim-artwork).
- **US `$01:CA4B`, JP `$01:C9D5`**: follower-symbol family/variant words,
  indexed by record `+$14` at `$01:C9AD/$01:C937`. Family `$08` variants
  0–6 resolve to 16×16 compositions at US `$D32B..D34F`, JP `$D2B5..D2D9`.
  Variants 1 and 4 are the confirmed angry-face/skull and skull/cross changes.

### Action content tables

| SNES address | File range | Meaning |
|---|---:|---|
| `$0A:B100` | `0x53100` | **Action level index** — `(key, offset)` word pairs, `$FFFF` terminated, key = `$19<<8 \| $18`. **50 entries = 49 action MAPS + 1 special (`$18=$00 $19=$09`)** — these are maps, not acts: an act spans several consecutive `$19` maps (Fillmore 4, Bloodpool 8, Kasandora 6, Aitos 7, Marahna 8, Northwall 8, Death Heim 8). The 12 acts are 6 kingdoms × 2, with act 2 beginning at `$19` = 2/2/3/4/4/5 for regions `$01-$06` |
| `$0A:B1CA-$C50x` | `0x531CA+` | **Action level layout streams** — per stage: player start (3B), `$FF`-terminated 5-byte terrain damage boxes, then 4-byte object placements `[tileX, tileY, $38 param, type]` with opcodes `$FC` goto / `$FD` reserve N slots / `$FE` checkpoint+wave gate / `$FF` end. Loader `$00:92CB`/`$00:941C` |
| `$06:A000-$A3FF` | `0x32000` | **Item sprites**, one 16×16 (128 B) per item id `$00-$07`; DMA'd to VRAM `$2D80` by `$00:96C3` when a statue breaks |
| `$06:A800+` | `0x32800` | Common/ROM-resident animation table used by the statue (type `$80`, anim `$08` = frame `$0D`) |
| `$06:8000+` | `0x30000` | Common/ROM-resident animation table used by the player and the other `$8x` common types |
| `$02:9013` | `0x11013` | **Professional-mode stage order** — 14 words, XBA'd into `$1A`/`$1B` by `$00:8781` |
| compressed blobs | linear offsets in the script | **Blob formats.** All are bit-packed Quintet LZSS (`$02:C5C9`; port = `tools/quintet_lzss.py`). Animation blobs (bit-0 cmds) and metatile tables (bit-5) start with a 16-bit decompressed size then the stream. Map blobs (bit-4) start `[widthChunks][heightChunks][size16]` — the width byte is `$2F`, the chunk-column count. Metatile tables are stored **byte-swapped** relative to `$7E:2100` |
| `$05:8000` | `0x028000` | **Per-map asset script** — walked by the VM at `$02:B1F7`. Entries `[$18, $19, cmd…, $00]` after a 3-byte `"SY\0"` header; each command dispatches on its highest set bit (operand sizes 6/6/7/4/1/3/5/6 for bits 7..0). **Pointers inside are 24-bit LINEAR file offsets**, converted by `$02:B4C0` as `bank = L>>15`, `addr = $8000\|(L&$7FFF)`. Bit-0 commands load the OBJ animation/composition blob to `$7E:4000` (objects) or `$7E:5000` (boss); dump with `tools/act_content.py --assets` |
| `$02:893E+` | `0x01093E+` | **Action section-video profile table.** Fixed 28-byte records selected by command 3. Bytes `+0..+6` define main/sub/window masks, colour math, priority, BGSC and BGMODE; `+7..+12` hold the six parallax ratios; `+13..+24` drive fade, BG2 page cycling and character animation; `+25..+26` are the timer and `+27` is renderer-nonoperative `$F2`. The 49 action-room scripts reference 43 distinct profiles (`$03-$2E`, excluding `$08`). |
| `$02:96D4-$97D3` | `0x0116D4-0x0117D3` | **Nominal 256-byte action-raster waveform.** Persistent presets R1/R7/R10 index this table directly. R4 deliberately forms a 16-bit index with inherited DP `$01`, so its settled source begins at `$02:97D4` and reads the adjacent ROM/code window; that overrun is an original-ROM behavior, not extra waveform-table storage. |
| Action BG ROM-source catalogue; measured anchor Aitos `$04/$01` BG2 | Aitos anchor: map `$1C:E7DF` / `0x0E67DF`; metatiles `$0A:C50D` / `0x05450D`; CHR `$12:992F` / `0x09192F` → VRAM word `$0000` and `$16:EB09` / `0x0B6B09` → `$1000`; palette `$1C:D8F8` / `0x0E58F8` → CGRAM `$00-$3F` | `DioramaRomBackdrop_LoadActionBg` interprets `$05:8000` bits 7/6/5/4 for every valid action `(group,map,BG1|BG2)` source and replays earlier entries in the same act to reconstruct inherited maps deterministically. It emits the first 256×256 page read-only; no emulated RAM/VRAM/CGRAM is written. The `rom-GG-MM-bgN` grammar covers 98 sources. A stock-ROM census decodes all 98; synthetic tests pin inheritance and malformed streams. For the measured `rom-04-01-bg2` anchor, the map expands to 256 bytes after its `1×1` chunk header; metatiles expand to `$0800` bytes and are word-swapped by `$02:B3CE`; each CHR stream expands to `$2000` bytes. `$02:B6D3-$B6F6` installs action tile-word mask `$ECFF`; `$02:B4E8-$B54C` merges attribute byte `$10` into BG1 and `$01` into BG2. The ROM renderer applies the same transform before character lookup. Comparison against `runs/20260812-000613/snapshots/snap_00_gf4152` matched live `$7E:C000`, `$7E:2900`, VRAM character bytes `$0000-$3FFF`, the complete 2,048-byte BG2 first-page tilemap at VRAM byte `$E000`, and all 65,536 rendered pixels. |
| `$02:B6D3-$B6F6` | `0x136D3-0x136F6` | **Action BG tile-word mask setup.** Stores `$FDFF` in `$54/$58/$5C` when map group `$18` is zero and `$ECFF` when it is nonzero. Action map rendering therefore clears definition bits 8, 9, and 12 before the per-layer attribute merge. |
| `$02:B4E8-$B54C` | `0x134E8-0x1354C` | **Per-section video config tile attributes.** Clears BG attribute bytes `$6B/$6F`, then for action map groups (`$18 != 0`) merges `$10` into BG1 and `$01` into BG2. The tile builder uses these as the high byte after applying `$54/$58`. |

The asset VM itself remains the owner of sequencing and commands 2/1/0. The
presentation data plane now has guarded, default-on CPU HLEs for commands 7
through 3; each predicate is read-only and retains the decoded native handler
for non-action modes or any operand shape outside the stock action census.

| Command bit | Handler / file offset | Operands | Stable contract | Current execution |
|---:|---|---:|---|---|
| 7 | `$02:B28E` / `0x01328E` | 6 | Character upload to VRAM. Stock action shapes decompress 8 KiB character banks or the 4 KiB dialog font through `$7E:6000`. | Guarded `ActRaiser_LoadActionCharacters`; native fallback |
| 6 | `$02:B330` / `0x013330` | 6 | Palette slice upload through CGRAM `$2121/$2122`; audited action slices are 128 bytes. | Guarded `ActRaiser_LoadActionPalette`; native fallback |
| 5 | `$02:B363` / `0x013363` | 7 | Decompress and byte-swap a 2 KiB metatile-definition table into `$7E:2100` (BG1) or `$7E:2900` (BG2). | Guarded `ActRaiser_LoadActionMetatiles`; native fallback |
| 4 | `$02:B3EB` / `0x0133EB` | 4 | Read `[widthChunks,heightChunks,size16]`, publish pixel dimensions, and decompress the page-major metatile-id map to `$7E:8000` (BG1) or `$7E:C000` (BG2). | Guarded `ActRaiser_LoadActionMap`; native fallback |
| 3 | `$02:B4E8` / `0x0134E8` | 1 | Apply one 28-byte `$02:893E` video profile to PPU and direct-page presentation state. | Guarded `ActRaiser_ApplyActionVideoConfig`; native fallback |
| 2 | `$02:B631` / `0x013631` | 3 | Semantics were not resolved by the presentation-loader work. | Native VM |
| 1 | `$02:B63B` / `0x01363B` | 5 | Script-driven song change. | Native VM |
| 0 | `$02:B69C` / `0x01369C` | 6 | Decompress ordinary/boss OBJ animation/composition data to `$7E:4000/$5000`; ending scene 08/01 uses this producer for twenty BG3 page maps. | Native VM |

The per-region object-type tables at `$00:96AF/$A8F6/$B449/$C11E/$CD9B/$D928/$E722/$F39A`
(already listed above) are the **enemy stat tables**: each 12-byte record carries ATK at `+7`,
HP at `+8` and death score at `+9`. `tools/act_content.py --tables` decodes all eight.

### Action camera and tile-streaming control

| SNES address | File range | Meaning |
|---|---:|---|
| `$02:B030-$B090` | `0x13030-0x13090` | **Action camera tracking request** — selects the tracked object's X through direct-page object base `$8A`, centres it against the native 256px viewport, applies the vertical dead zone around focus Y `$82`, and stages signed requests in `$7C/$7E`. Called immediately before `$02:B091` from each action frame loop. |
| `$02:B091-$B126` | `0x13091-0x13126` | **Action camera application/stream trigger** — applies `$7C/$7E` to BG1 camera `$22/$24`, clamps against dimensions `$2E/$30`, raises the 16px strip flags in `$93`, then derives BG2 parallax and player-relative coordinates. The HLE seam preserves that tail while optionally fitting the finite horizontal presentation canvas; vertical `$24` retains the native range. |

### Sprite identity and action OBJ assets

Unreferenced log/cave resources and questionable Bloodpool entries are
catalogued separately in [unused-content candidates](unused-content.md).
Their existence is not proof of beta provenance or an executed gameplay bug.

| SNES address | File range | Meaning |
|---|---:|---|
| `$00:8C98-$8D67` | `0x00C98-0x00D67` | Action OAM rebuild/cull. Clears the shadow, calls the fixed-HUD emitter, scans action objects, updates object `+$30` activation bit `$0400`, and calls `$00:8D68` for each draw-admitted composition before returning through one common epilogue. The host wide port preserves separate DRAW and ACTIVATION predicates: drawing uses horizontally fitted `$22`, native vertical `$24`, and presentation margins/apron, while `$0400` uses its independently selected activation camera/range. The native pause/freeze path skips this routine while vblanks continue, so its completed-call cadence is the verified presentation clock for host action lighting and particles. The serial itself is host state, not ROM or WRAM data. |
| `$00:9755-$9799` | `0x01755-0x01799` | Paired action-entry arrival actor. Advances and moves the companion/orb object, publishes its motion through `$7C/$7E` and cached focus `$80/$82`, then writes entry fade/raster controls `$CB/$CD/$CE/$CF`. This is a scripted actor in the same object table as enemies; activation-policy changes must therefore preserve the native entry object set rather than treating every non-player slot as an enemy. |
| `$00:97A6-$980F` | `0x017A6-0x0180F` | Player action-entry lifecycle. `$97A6` waits for the approach target, assigns player slot `$08A0` as camera subject `$8A`, and installs `$97C9`; `$97C9` runs the first transform animation and installs `$97E4`; `$97E4` runs the final materialization/fade update and, at sequence end, writes handler `$9832`, flags `$0003`, and resets animation state. This handler chain is the exact no-input interval used by the host margin-activation gate. |
| `$00:9832-$9883` | `0x01832-0x01883` | First normal player ground-control handler after arrival. It installs itself at player `+$12`, reads held input `$A1/$A0`, dispatches attack/jump/magic/walk states, and is the exact handoff that re-enables extended horizontal activation. |
| `$00:8683-$868F` | `0x00683-0x0068F` | Shared action animation-repeat dispatcher: advances through `$00:8631`, decrements object `+$38` at the authored sequence boundary, repeats while nonzero, and dispatches the saved `+$1E` resume when the repeat count reaches zero. A Bloodpool lightning bolt legitimately transitions here from its scene-specific root. |
| `$00:A66A-$A6FC` / JP `$00:A629-$A6BB` | US `0x0266A-0x026FC` / JP `0x02629-0x026BB` | Shared platform contact-child setup/update. Copies the parent record, then tests player contact and carries the player; checked log-state 18/22 paths do not select the extra Western state 23. PAL equivalent `$00:A23F-$A2D1`. [Five-ROM native checks](regional-differences-technical.md#log-contact-helper-and-animation-termination). |
| `$02:893E` / JP `$02:87E7` | US/PAL `0x1093E` / JP `0x107E7` | 28-byte video profiles. Aitos rooms 1–7 select `$16-$1C`, with identical profile bytes across all five ROMs. Rooms 2/3 use a four-page cycle with five updates per page; separate Act 1 character-animation transfers are disabled. [Profile and service addresses](regional-differences-technical.md#aitos-background-animation-and-video-profiles). |
| `$00:E18E-$E291` | `0x0618E-0x06291` | Marahna linked-lightning family measured in run `20260811-151353`. Source root `$E18E` is retained by the first endpoint and connector child; partner root `$E254` is retained by the adjacent second endpoint. Live connector children resume at `$E24F` and use exact horizontal/vertical `$7E:4000` composition families documented in `ram-map.md`. |
| `$00:E047-$E0B9` / JP `$00:E0DD-$E14A` | US `0x06047-0x060B9` / JP `0x060DD-0x0614A` | Marahna type05/10 hovering/splitting fireball. Shared48-update idle, abs-X/Y<80 gate,32-update charge, four cardinal shots,10-update root burst. Shared child entry US `$A655` / JP `$A614`; US measured resumes `$E061/$A65D` are stored return addresses. JP extra0505 `(39,10)` placement, US-only audio21 call, otherwise matching reviewed mechanics/owned composition data. [Regional contract](regional-differences-technical.md#marahna-splitting-fireballs). |
| `$00:B3BF-$B448` / JP `$00:B453-$B4DC` | US `0x033BF-0x03448` / JP `0x03453-0x034DC` | Fillmore type01/19 wall emitter, twelve matching0301 placements. US state36 repeated twice, JP once:360/180 active-frame firing interval. Source and rolling/falling child rules match; failed global allocation skips shot. [Native traces](regional-differences-technical.md#fillmore-act-2-wall-emitter-cadence). |
| `$00:DE96-$DF85` | `0x05E96-0x05F85` | Marahna snake family. Source `$DE96` cycles through wait/rise/fall handlers, allocates a copied child at `$DF0A`, and drives its state-`$06` horizontal lifecycle through shared `$A655`. Run `20260811-232640` measures exact child artwork, velocity, flip, counter, and parent backlink in `ram-map.md`. |
| `$00:E0BA-$E18D` | `0x060BA-0x0618D` | Marahna reaper and orb lifecycle. The parent rooted at `$E0BA` allocates a child, installs update handler `$E13A`, and selects loaded-animation states `$17/$3A-$3D` for horizontal, aimed, and vertical paths. Run `20260811-232640` proves this is a non-fire negative family. |
| `$00:E2F3-$E37E` | `0x062F3-0x0637E` | Marahna moving-platform roots and wait/resume tails. Run `20260811-221433` corrects the prior flame-projectile classification: live `$E2F3/$E304/$E315/$E326/$E351/$E368` actors use `$34/$4BE5` but are platform machinery and must not receive fire effects. `$4BE5` itself is decompressed WRAM animation data. |
| `$00:E483-$E600` | `0x06483-0x06600` | Marahna boss and electrical attack family measured in runs `20260811-221433` and `20260811-225534`. The boss and launched child retain source `$E483`; parent waits resume at `$E4E5/$E4F4`, the diagonal descent resumes at `$E578`, and `$E57E` is the live post-impact ground-charge resume—not a retired tail. While it travels, the boss parent repeats through `$E4D7`. Exact `$7E:5000` charge/orb/diagonal/ground artwork and backlink identity are documented in `ram-map.md`. |
| `$00:F16D-$F399`; `$00:F76C-$F777` | `0x0716D-0x07399`; `0x0776C-0x07777` | Northwall Act-2 Ice Dragon boss/child family and Death Heim rematch wrapper. JP family `$00:F1EC-$F418`, wrapper `$00:F7EB-$F7F6`. Source records precede entries by12 bytes and are data, not instructions. Regional timing delta is in the rematch sequence asset below, not a general boss-code speed multiplier. |
| US `$18:B137/$C32C`; JP `$18:891E/$A3FC` | US `0xC3137/0xC432C`; JP `0xC091E/0xC23FC` | Northwall original/rematch compressed animation/composition blobs, loaded to `$7E:5000`. Original blobs match byte-for-byte; rematch states `$11/$12` each omit two six-tick stationary rows in JP, verified as118→106 active frames. All26 visual composition records match across regions. States `$19/$1A` double horizontal ice-ball velocity in both rematches independently of region. [Native timing evidence](regional-differences-technical.md#northwall-act-2-boss-original-versus-death-heim). |
| `$00:879D-$884F`; `$00:88D6-$88F6` | `0x0079D-0x0084F`; `0x008D6-0x008F6` | Action pickup dispatch and heal queue. JP `$00:878C-$883E` / `$00:88C5-$88E5`. Half/full apple formulas and four-phase queued refill match; differences at particular locations are item placements, not different healing potency. |
| `$00:8C12-$8C97`; `$00:93A9-$941B` | `0x00C12-0x00C97`; `0x013A9-0x0141B` | Damage-box contact and expansion, JP `$00:8C01-$8C86` / `$00:93D1-$9443`. Shared padded hot-point tests, ordered overlap processing and hit/invulnerability gates; box damage is independent of enemy attack/Special promotion. Byte bit80 sets player flag8000 instead of subtracting HP. [Exact contract](regional-differences-technical.md#terrain-and-damage-box-contracts). |
| `$00:8E2F-$8F13` / JP `$00:8E3B-$8F1F` | US `0x00E2F-0x00F13` / JP `0x00E3B-0x00F1F` | Shared animation decoder. Player-only tail consumes flag8000 by clearing it and halving newly decoded signed DX, rounding down through US `$84EC` / JP `$84E6`; not128 damage. Terminator/non-player bypasses it. [Slow-zone contract](regional-differences-technical.md#shared-player-slowing-zones). |
| `$00:91C3-$920E` | `0x011C3-0x0120E` | Terrain-attribute lookup, JP `$00:91CF-$921A`. Native tests cover all348 changed cells across eight shared maps, plus bounds; paired boot loads reproduce full maps/attributes. Forty-one other grids match statically, not a claim of full traversal parity. |
| `$02:C2E8-$C33F`; `$02:C375-$C385` | `0x142E8-0x1433F`; `0x14375-0x14385` | Native action-score formatter and blank fill. JP `$04:9223-$927A` / `$04:92B0-$92C0`. Four stored BCD digits plus fixed trailing zero: raw score units are tens of displayed points. Native fixtures verify the live five-cell template. |
| `$00:A940-$A9B2`; `$00:A9BF` | `0x02940-0x029B2`; `0x029BF` | Fillmore Act-1 tree head and inactive US peer entry. JP `$00:A8FF-$A97D` and `$00:A98A-$A9D9` drive an adjacent peer and two seeds before the same two orb shots; JP seed/plant family `$00:A9F4-$AA6D`. State10's stored duration is not its JP wait: native code uses separate `$0080` delay. [Paired native evidence](regional-differences-technical.md#fillmore-act-1-tree-seed-controller-and-pre-shot-wait). |
| `$00:AF69-$B014` / JP `$00:AFFD-$B0A8` | US `0x02F69-0x03014` / JP `0x02FFD-0x030A8` | Minotaur root/axe program reused by Death Heim. Original sources `$AF5D/$AFF1`, rematch `$F6CA/$F749`. Regional instruction change: facing-relative axe offset−72US/−48JP; other attack timing differences reside in loaded `$5000` states. Original cycle180US/147JP; shared rematch117. Root target-X latch, pool failure and sequence-boundary axe retirement verified. |
| `$00:AA9A/$AC8E/$B041/$B0B4/$DCDB` / JP `$00:AB2E/$AD22/$B0D5/$B148/$DD71` | Source records; entry = source+12 | Bird, leaping enemy, cave types01/0F and01/0E, hooded caster05/0C. Local programs match after classified relocation/sound traps; loaded animation rows change horizontal speed or attack recovery/wind-up. [Native timing and identity limits](regional-differences-technical.md#ordinary-enemy-movement-and-attack-recovery). |
| `$00:8325-$83D5` / JP `$00:831F-$83CF`; PAL guard `$00:8258` | US `0x00325-0x003D5` / JP `0x0031F-0x003CF`; PAL `0x00258` | Scene bootstrap routes region0/sub9 directly to world initialization, bypassing action placement. Layout0900 survives in US and PAL Story, not JP or PAL Action; values5/6 exceed the four-slot universal action table. [Routing evidence](regional-differences-technical.md#unused-world-map-placement-root). |
| US `$19:B017/$C778`; JP `$19:8FFD/$AF2B` | US `0xCB017/0xCC778`; JP `0xC8FFD/0xCAF2B` | Original/rematch Minotaur animation/composition blobs at `$7E:5000`. Original regional states0 and2 differ in duration; rematch blobs match byte-for-byte. Original composition11 is absent in rematch, shifting later visual IDs and data pointers. [Phase/geometry evidence](regional-differences-technical.md#minotaur-timing-and-room-inheritance). |
| `$00:CF9E-$D024` / JP `$00:D01E-$D0A4` | US `0x04F9E-0x05024` / JP `0x0501E-0x050A4` | Aitos volcano-fireball source and cyclic lifecycle. Live US slots retain `$CF9E` and rise/return resume `$CFCD`; handlers `$CFE3/$CFFE` own rise/return (JP `$D063/$D07E`). Native RNG wait,16-frame state `$21`,16-frame hold `$23`,384-frame rise `$22`, then ground-tested return `$24`. Paired normal/Special traces match movement for all six placements. Exact WRAM artwork remains in `ram-map.md`; `$4D21/$4D2D` are mutable WRAM compositions, not ROM symbols. [Regional contract](regional-differences-technical.md#aitos-act-1-platform-skulls-and-volcano-fireballs). |
| `$00:D382-$D3BE` / JP `$00:D404-$D440` | US `0x05382-0x053BE` / JP `0x05404-0x05440` | Aitos platform-skull source and entry at record+12. JP victim flag `$0800` deflects swords; US awards20 stored score units (200 displayed points) on death. Proximity bounds are strict abs-X/Y below32/64US versus24/24JP; state `$2F` idle32 plus restart, state `$30` explosion12 then retirement without points. HP0 is shared, not a vulnerability discriminator. |
| `$00:CEEC-$CF2D` / JP `$00:CF71-$CFB2` | US `0x04EEC-0x04F2D` / JP `0x04F71-0x04FB2` | Aitos type05 molten-rock record and entry. US active instances retain source `$CEEC`, resume `$CF16`, handler/state `$8661/$27`; presentation excludes the stationary `$CF1C` phase. Regional programs, six placements and owned states39/40/compositions42/43 match. Native launch resets its own slot, not a new child. Abs-X<128; odd RNG gives inclusive delay2–65, even RNG flips and waits2 because the flip helper overwrites A with attributes. State39 lasts88 updates, then state40 repeats until outside-window at a row boundary. `$4D21/$4D2D` are decompressed WRAM composition pointers. [Native comparison](regional-differences-technical.md#aitos-molten-rock-launches). |
| `$00:BD2A-$BD35` | `0x03D2A-0x03D35` | Bloodpool vertical-lightning spawn record. Its computed primary handler is record+`$0C` = `$00:BD36`; live objects retain `$BD2A` in slot `+$32`. |
| `$00:BD36-$BD75` | `0x03D36-0x03D75` | Bloodpool vertical-lightning lifecycle: offscreen gate, packed animation/repeat commands `$0010/$1104/$1406`, SFX `$10`, and transition through the shared animation/retirement helpers. The saved nested resume value is `$BD69` (execution resumes at `$BD6A`). |
| `$00:BD76-$BD81`, `$00:BD84-$BD8F` | `0x03D76-0x03D81`, `0x03D84-0x03D8F` | Two direction/attribute variants of the Bloodpool enemy-fireball spawn record. Live fireballs retain the selected record address in slot `+$32`; `$BD84` is intentionally embedded behind the branch at handler `$BD82`. |
| `$00:BD90-$BDB0` / JP `$00:BE24-$BE4D` | US `0x03D90-0x03DB0` / JP `0x03E24-0x03E4D` | Bloodpool Act-2 firing-statue volley, types `$26/$1E`, sources US `$BD76/$BD84`, JP `$BE0A/$BE18`. US plays idle60/fire16/spawn/idle60; JP adds fire16/spawn at `$BE3C-$BE44`. Native restart yields full cycles137/153. Root flag `$0400` gates entry, not the intervening second shot. Both facings and normal/Special verified; [family contract](regional-differences-technical.md#bloodpool-act-2-statues-single-versus-double-volley). |
| `$00:BDB1-$BDCA` / JP `$00:BE4E-$BE67` | US `0x03DB1-0x03DCA` / JP `0x03E4E-0x03E67` | Statue projectile allocation after parent; copies facing/source/attack, child backlink, offsets X−16/+16 and Y−8, entry `$BDCB/$BE68`. Exhaustion returns scratch `$1AA2` and creates no live child; caller still progresses the volley. Startup state `$21` lasts4 frames; flight `$23` sets velocity±3 before next-frame movement. |
| `$00:BDF0-$BDFE` | `0x03DF0-0x03DFE` | Enemy-fireball flight tail: advance/loop animation through `$00:8631`, checking `$0400` at the two-row/eight-frame sequence boundary (not every frame); when outside the selected activation window, release through `$00:85B7`. Drawing and activation are independently selected by the `$8C98` host seam. JP counterpart `$BE8D-$BE9B` has the same retirement contract. |
| `$00:BDFF-$BE0A` | `0x03DFF-0x03E0A` | Bloodpool boss spawn record retained as `$BDFF` in the boss and its linked lightning children (`+$32`); computed primary boss handler is `$BE0B`. |
| `$00:BE78-$BE7D` (US only) | `0x03E78-0x03E7D` | Wizard first-form post-spread `LDA #30; JSR $86FA`, adding31 native updates. JP `$BF15` proceeds directly to the position test. Original/rematch and normal/Special verified; separate from shared rematch animation acceleration. [Wizard comparison](regional-differences-technical.md#wizard-original-fight-and-rematch). |
| `$00:D844-$D927` / JP `$00:D8C6-$D9A9` | US `0x05844-0x05927` / JP `0x058C6-0x059A9` | Flaming Wheel's83-instruction root/projectile-spawn program; reviewed differences are relocation/audio, not attack behavior. US/JP encounter blobs match. Shared rematch roll80→40 frames, projectile axis speed1→4. [Wheel comparison](regional-differences-technical.md#flaming-wheel-original-fight-and-rematch). |
| `$00:E4D8-$E4DF` / JP `$00:E559-$E55E` | US `0x064D8-0x064DF` / JP `0x06559-0x0655E` | Viper native RNG call and lightning predicate. US low two bits zero; JP low bit zero. Exhaustive256-byte fixtures yield64/128 qualifying values; one RNG draw per decision in original/rematch. [Viper comparison](regional-differences-technical.md#viper-attack-selection-and-rematch). |
| `$00:F8F5-$F8FF` / JP `$00:F974-$F97C` | US `0x078F5-0x078FF` / JP `0x07974-0x0797C` | Final second-form entry clears collision/cast gates. US additionally clears `$E8` at `$F8FC`, resuming the clock; JP retains the boss-defeat stop gate. Controlled native transition verified in both modes. |
| `$02:BC82-$BC9D` / JP `$04:8C7D-$8C98` | US `0x13C82-0x13C9D` / JP `0x20C7D-0x20C98` | Stage countdown service, entry M1/X0. `$E8` stop gate; `$E5` divider59; `$E6/$E7` saturating BCD decrement. Regional bodies match, so final-boss timer difference is caller policy, not a different clock rate. |
| `$00:FD25-$FD50` / JP `$00:FDA2-$FDCD` | US `0x07D25-0x07D50` / JP `0x07DA2-0x07DCD` | Final second-form projectile sets attack3US/4JP explicitly before state6 then `$22` flight; same values in Special. Final animation blobs fileUS `0xC7727` / JP `0xC46C9` differ only in first-form state10 duration64/36 and upper-body state48 duration37/38. [Final-boss evidence](regional-differences-technical.md#tanzra-forms-timer-and-projectile-strength). |
| `$00:BFDF-$BFF8` | `0x03FDF-0x03FF8` | Boss lightning-attack child allocation: allocates an action slot, assigns handler `$BFF9`, links/copies the boss through `$8709`, and offsets the child anchor vertically. |
| `$00:BFF9-$C06E` | `0x03FF9-0x0406E` | Linked boss-lightning sequence. `$BFF9` selects one of six strike states through the table at `$C056` (`$07,$04,$06,$03,$05,$02` = diagonal/vertical × short/medium/long), runs each strike/blank cycle through delay handler `$8661`, then creates the state-9 floor child handled at `$C062`. Strike saved resumes observed live are `$C02B/$C04B/$C051`; floor resume is `$C06A`. |
| `$00:C1AE-$C24D` / JP `$00:C245-$C2E4` | US `0x041AE-0x0424D` / JP `0x04245-0x042E4` | Pharaoh root reused by rematch. Sources `$C1A2/$C239` in raw `$0603`, `$F6FA/$F779` in `$0407`. Loaded state `$0B` consumes40US/24JP originally,56US/24JP in rematch; US adds a stationary16/32-frame row. Root `+$38` waits for sphere formation; failed allocation goes directly to takeoff. |
| `$00:C24E-$C2D4` / JP `$00:C2E5-$C369` | US `0x0424E-0x042D4` / JP `0x042E5-0x04369` | Pharaoh sphere→wall-head lifecycle. Both initialize a head at X<80 or X≥448 using `$C8C9/$C8D7` (JP `$C95A/$C968`) while retaining the encounter source/backlink. US fires once then withdraws30 frames originally/15 in rematch and retires; JP repeats16-frame firing/120-frame idle in both. [Encounter timing and policy boundary](regional-differences-technical.md#pharaoh-death-heim-rematch). |
| `$00:C2D5-$C2F1` / JP `$00:C36A-$C386` | US `0x042D5-0x042F1` / JP `0x0436A-0x04386` | Pharaoh arrow activation and arena-bound retirement. Loaded state4 moves3px/update originally,6 in rematch. Both retire at unsigned X≥512, not camera offscreen state. Source remains boss/rematch, backlink is the head; the arrow can outlive that head. Explicit HP1/BCD score1 even in Special. |
| US `$1B:CE6A`; JP `$1B:CD45` | US `0xDCE6A`; JP `0xDCD45` | Death Heim Pharaoh animation/composition blobs loaded at `$7E:5000`; originals at US `0xDD27A` / JP `0xDD132`. Shared rematch changes: sphere/arrow speed doubled, emergence26→13, pre-landing hold20→16. US-only changes: ground hold40→56, withdrawal30→15. Composition metadata remains unchanged within each region. |
| `$00:C8FF-$C943` / JP `$00:C990-$C9CE` | US `0x048FF-0x04943` / JP `0x04990-0x049CE` | Ordinary Kasandora wall-head loop. US adds explicit delay30 at `$C908`, costing31 native updates. `$4000` variant cycles130US/99JP; `$5000` variant168US/137JP. Do not conflate these with the Pharaoh descendant loop. |
| `$00:CB51-$CB7A` / JP `$00:CBD3-$CBFC` | US `0x04B51-0x04B7A` / JP `0x04BD3-0x04BFC` | Circling/darting blue enemy, type `$15`, source `$CB45/$CBC7`. Paired600-frame normal/Special traces match movement exactly; full cycle177 frames in both regions. No evidenced regional slowdown for this family. |
| `$00:9CF2-$9D1B` | `0x01CF2-0x01D1B` | Player ranged-sword creator. Allocates a linked action child, copies the player source/backlink, marks it as an attacker, selects animation state `$13` or `$14`, and advances animation through `$8E2F`. |
| `$00:9D1C-$9D3D` | `0x01D1C-0x01D3D` | Player sword-beam flight handler. Retires on timer/offscreen/end conditions and otherwise moves the child through `$86BB`; live velocity is horizontally mirrored `8px/tick`. Animation `$06:8000` maps state `$13` to visual/composition `$30/$99E8` and state `$14` to `$31/$9A17`. |
| `$00:D646-$D837` | `0x05646-0x05837` | Aitos Act-1 dragon boss family (raw room `$0304`). `$D785` runs the sword-volley controller through state 0, allocates two generic `$A655` children, and seeds local counters 1/2. Loaded `$7E:5000` states 1/2 supply the two exact diagonal crescent sequences; the children later wait in `$8661` with saved resume `$A65D`. Run `20260812-000613` snapshot 5 measures the normal controller, boss backlink, artwork, velocities, extents, and priority-2 OAM. Run `20260812-224123` snapshot 1 measures the H+V-reflected facing: controller and child both use `$C000`, velocity signs reverse, and extents swap sides as listed in `ram-map.md`. |
| `$00:AD51-$AF5C` / JP `$00:ADE5-$AFF0`; `$00:B792-$B918` / JP `$00:B826-$B9AC`; `$00:D652-$D837` / JP `$00:D6D4-$D8B9`; `$00:E7D2-$E951` / JP `$00:E851-$E9D0` | Bank-0 file offsets = address minus `0x8000` | Reviewed Centaur, Bloodpool Act-1, Aitos Act-1 dragon and Northwall Act-1 families. Programs match after relocation/audio accounting; spawn records and full `$5000` animation/composition blobs match. [Paired motion and allocation evidence](regional-differences-technical.md#first-act-boss-program-comparison), not a full-combat/presentation equivalence claim. |
| `$00:C67B-$C80D` / JP `$00:C70A-$C89E` | US `0x0467B-0x0480D` / JP `0x0470A-0x0489E` | Antlion source `$C66F/$C6FE`, raw room `$0203`. Trigger at player X≥2432US/2304JP. US `$C718` plays36-frame state12 before distance decision `$C71E`; JP `$C7A7` checks immediately, then far branch delays61 updates and returns to firing. At abs-X≥64, volley intervals48US/73JP. [Threshold and encounter policy](regional-differences-technical.md#antlion-trigger-and-post-volley-decision). |
| Antlion animation/composition assets | US `0xDB07E` / JP `0xD9758` | Resident `$7E:5000`. States0–11 and referenced compositions match; US-only state12 has durations3/3/30. No JP donor required for the examined behavior. |
| `$00:CF3A-$CF5F` / JP `$00:CFBF-$CFDF` | US `0x04F3A-0x04F5F` / JP `0x04FBF-0x04FDF` | Aitos falling bamboo trap, type06/source `$CF2E/$CFB3`. Same abs-X<32/activation gate and219-update motion, with JP extra placement `(28,33)`. Shared `$4000` blobs at `0xC4C17/0xC1B1C` divide states37/38 differently but flatten identically. US-only audio command23 in this entry is separate. [Trap evidence](regional-differences-technical.md#aitos-bamboo-spike-traps). |
| `$00:95DD-$95EC` | `0x015DD-0x015EC` | Eight action handler-table pointers: `$96AF,$A8F6,$B449,$C11E,$CD9B,$D928,$E722,$F39A` for `$18=$00-$07` |
| `$01:E099+` | `0x0E099+` | Town world-object type → behavior/animation-data pointer table |
| `$01:E7D9+` | `0x0E7D9+` | Parallel town world-object type → sprite-frame pointer table; frame lists continue around `$01:E838` |
| `$06:A000+` | `0x32000+` | Conditional 128-byte dynamic action effect-overlay windows selected from polymorphic object `+38`; uploaded to VRAM `$2D80` only for objects with `+30 & $0040` and an idle upload descriptor. Not a universal spell-ID table |
| `$06:A400+` | `0x32400+` | Selected action-magic character windows used by `$02:BC9E`: 256 bytes at `$A400 + (id-1)*$80`, uploaded to VRAM word `$2D40`; an unequipped Story slot selects entry 6. At action OBSEL `$01`, the write covers common OBJ tiles `$D4-$DB`, including the four HUD-icon tiles `$D4-$D7`. Re-read across all five ROMs: the `$0080`-iteration loop writes 16-bit words through `$2118`, so the 256-byte length and 128-byte source stride are both correct. Adjacent source windows overlap. PAL Action Mode instead selects from `$06:AC00` using its spell stack (empty entry 5). This entry-time upload is distinct from the queued 128-byte icon update. [Loader addresses and evidence](regional-differences-technical.md#shared-bank-and-palette-completion). |
| `$07:8000-$9FFF` | `0x38000-0x39FFF` | Common action OBJ atlas, 8192 bytes copied to VRAM `$2000-$2FFF` at level entry |
| `$02:ADFF-$AE34`; `$02:AE35-$AE74` | `0x12DFF-0x12E34`; `0x12E35-0x12E74` | Action palette-7 updater and two 32-byte source palettes. Frame-counter bit 1 selects the source uploaded to CGRAM `$F0-$FF`; both source palettes match across all five ROMs. Fillmore Act 1 composition `$1B` references this palette but uses an entirely transparent tile `$34`. [Regional addresses and residency checks](regional-differences-technical.md#shared-bank-and-palette-completion). |
| `$06:8000/$82BF`; `$06:8030/$803A/$80CC` | `0x30000/0x302BF`; `0x30030/0x3003A/0x300CC` | Master animation header / visual pointer table; idle state `$00`, walk `$02`, standing sword `$08`. Visuals: idle `$04`; walk `$00,$00,$00,$00,$00,$01,$02,$03`; sword `$0B-$0F`. Four-byte rows contain visual, stored duration, dX, dY; the walking/standing attack handlers use DEC/BMI, so a stored zero is one displayed tick. |
| `$06:805B/$8168/$80C7`; `$00:98D9/$9B95/$99BB` | `0x3005B/0x30168/0x300C7`; `0x018D9/0x01B95/0x019BB` | Master moving jump state `$03` (11 rows, visual `$36`), early moving-jump sword state `$0C` (15 rows, visuals `$36,$16,$19-$1C`), and fall state `$07` (visual `$16`). `$9B95` changes the state without clearing the existing sequence index, preserving jump progress. The native early-jump sword program's first seven rows occupy 21 DEC/BMI ticks before its sword-arc rows. Workshop choreography samples the complete program from launch; its paths, hit times and jump curves are authored UI motion, not a port of the controller/physics. |
| `$00:9810-$981B`; `$00:95F0/$8D68` | `0x01810-0x0181B`; `0x015F0/0x00D68` | Master spawn record supplies `$09` to object `+$28.high`. The action emitter XORs `$0100`: raw composition bank-zero parts therefore use OBJ bank 0, palette 4 (`$0800`), with sword-trail parts using palette 5. This is not the same transform as a raw enemy composition. Seven-byte parts retain separate normal/flipped offsets; signed extent bytes anchor all poses to the same world position. |
| `$10:8000`; `$19:D695`; `$1C:CEF8` | `0x80000`; `0xCD695`; `0xE4EF8` | Fillmore `$01/$01` enemy CHR (8192 decoded bytes → VRAM `$3000`), animation/composition blob (→ `$7E:4000`), and 128-byte palette slice (→ CGRAM `$80`). Type `$09` / handler `$00:AC9A` starts at state `$1D`, selecting bird visuals `$1F-$22`. These four compositions use only this enemy sheet/palette, and have been independently extracted in both facings. |
| `$00:ACE7/$C576`; `$00:A9E6/$AA29` | `0x02CE7/0x04576`; `0x029E6/0x02A29` | Fillmore type `$1B` club-wielder branches to the shared walker: alternates states `$00/$01` (visuals `$12/$13`, eight ticks each). Type `$02` leaper uses rest state `$2E` (visuals `$2A,$29`) and short advancing hop `$31` (six rows, visuals `$2A,$2C`). These reviewed sprites use the same `$19:D695` blob and ordinary enemy sheet/palette as the bird. Normal facing travels left; H-flipped travels right. |
| `$07:EFC7`; `$13:B12F`; `$0B:8000`; `$00:AD51-$AD78` | `0x3EFC7`; `0x9B12F`; `0x58000`; `0x02D51-0x02D78` | First Fillmore boss (centaur) has a separate 5353-byte decoded animation/composition blob at `$7E:5000` and 8192-byte decoded CHR upload at VRAM `$4000`. Its entry sets OBSEL `$09` (second OBJ name table now `$4000`, not ordinary `$3000`) and schedules 128 palette bytes from `$0B:8000` to CGRAM `$80-$BF`. Selected states: idle `$10` → visual `$06`; walk `$00` → `$0C,$0F,$12,$15`; charge `$01` → `$0E,$11,$14,$17`; cast `$02` → `$0A,$09,$08,$07,$0B,$09,$07`. Body compositions reach 45 parts and signed extents of 72 pixels above / 64 below the world anchor, so a 96px regular actor cell is insufficient. Workshop uses a centred 160px cell and does not include the separate spear/lightning child actors. |
| Fillmore `$01/$01` BG selections | CHR `0x74000/0x78000`; metatiles `0xD41CA/0xDD687`; maps `0xAF131/0xD0704`; palettes `0xAFF80/0x2FF80` | Workshop forest = BG2 page `(0,0)`; grass/ground = BG1 page `(0,2)`. Each 256×256 sparse image agrees pixel-for-pixel with game-side `ActionRoomScene_Load/LookupTile`, including big-endian metatile definitions, mask `$ECFF`, layer attributes `$0100/$1000`, flips, palette slices and transparent index zero. Workshop repetition/parallax is decorative, not native map traversal or raster playback. |
| `$07:C000+` | `0x3C000+` | Magical Fire and Magical Stardust animation state tables, four-byte sequence entries, and seven-byte OAM compositions. Compositions OBSERVED live (2026-08-05): Stardust flight `$C13F` (state 0, visual 0, 16x16), Stardust burst `$C14B` at visual 1 (8x8) growing to `$C199` at visual 4 (32x32); Fire bloom `$C352` (state 3, visual $12, 52x25). Useful as identity anchors — the animation pointer alone cannot tell Fire from Stardust, since both live in this bank. |
| `$07:C800+` | `0x3C800+` | Magical Aura and Magical Light animation state tables and OAM compositions; Light includes two authored 16x224 beam columns |
| `$07:D040-$D09F` | `0x3D040-0x3D09F` | Action OBJ palettes, 96 bytes copied to CGRAM `$C0-$EF` |

The bank-0 action handler tables are sparse object-type arrays with no explicit
count. Walk until the nearest forward pointer target (the payload boundary), and
treat zero words as unused type slots rather than termination. `$00:B449` is the
important proof: types `$19-$1D` are zero, while `$1E-$27` resume with ten valid
records; type `$21` points to record `$BB19` and exact handler `$BB25`.
Tables `$A8F6-$E722` correspond to the six ordinary two-act kingdom regions.
`$F39A` is Death Heim's distinct no-act boss-rush/final-boss table. Its `$19`
layout, verified end-to-end on 2026-07-14, is: `$19=1` =
teleport hub, whose spawn record `$F3C8` (handler `$F3D4`) stages the next boss
via `$1A = $0347 + 2`; `$19=2..7` = the six boss arenas; `$19=8` = final boss.
The rematch wrappers keep a Death Heim-local 12-byte spawn record in object
`+$32`, set room-owner backlink `$001C`, yield through `$F778`, and then
tail-call the corresponding original boss handler. That distinction matters:
the record address is persistent object identity, while the wrapper, `$F778`
continuation, and original handler are control flow.

| Room | Boss | Retained `+$32` source record | Spawn wrapper → boss family | Host accent |
|---|---|---:|---|---|
| `0702` | Minotaur | `$F6CA` (original `$AF5D`) | `$F6D6` → `$AF69` | spinning axe |
| `0703` | Wizard | `$F6E2` (original `$BDFF`) | `$F6EE` → `$BE0B` | complete lightning family |
| `0704` | Pharaoh | `$F6FA` | `$F706` → `$C1AE` | none |
| `0705` | Flaming Wheel | `$F712` (original `$D838`) | `$F71E` → `$D844` | body flame |
| `0706` | Viper | `$F72A` (original `$E483`) | `$F736` → `$E48F` | charge/orb/bolt/ground lightning plus BG1 torches |
| `0707` | Ice Dragon | `$F760` (original `$F161`) | `$F76C` → `$F16D` | ice-ball light, trail, and particles |
| `0708` | Tanzara | `$F80F` | `$F81B` → `$F82D+` final-boss family | exact projectile allowlist |

Key code: `$00:FEEC` (end of the `$FE89` teleport-out sequencer) writes
`$0347 = $19 - 1` (rush progress) and `LDA #$0701; STA $1A` (16-bit = stage
`$1A=$01/$1B=$07`, the hub warp), and sets `$0334=1` when `$19==8`. The
six rematch spawn wrappers `$F6D6/$F6EE/$F706/$F71E/$F736/$F76C` (plus the
final-boss `$F81B` wrapper) each
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

The final asset-script entry is **08/01**, not the preceding final-boss entry
07/08. It selects video profile `$2F`, uploads sixteen colours from file
`0x03C7C5` to CGRAM 0–15, decompresses a distinct 4096-byte credits alphabet
to VRAM word `$5000`, and decompresses twenty 2048-byte maps to `$7E:4000-DFFF`.
The bit-0 asset producer `$02:B69C` is therefore also a text-map source; treating
all its output as actor composition data misses the credits.

The localization adapter observes those resident maps without replacing this
native controller. Editable pages are 0–14, 17–19 in the US; 15/16 remain native
copyright artwork. The Go catalogue records all twenty page identities and
decodes the non-artwork compositions into Unicode. See the
[credits adapter contract](dialogue-system.md#graphical-credits-page-adapter).

| Release | Scene file offset | Font compressed offset | Page compressed offset | Page presenter | Hold frames |
| --- | --- | --- | --- | --- | --- |
| US | `0x028E1A` | `0x0D1B6F` | `0x03D1A0` | `$02:AB30` | 354 |
| EU English | `0x028E1A` | `0x0D14B2` | `0x03D1A0` | `$02:ABC9` | 286 |
| German | `0x028E1A` | `0x0D06F7` | `0x03D1A0` | `$02:ABD2` | 286 |
| French | `0x028E1A` | `0x0D0000` | `0x03D1A0` | `$02:ABBB` | 286 |
| Japanese | `0x028E14` | `0x0D1A2B` | `0x054549` | `$02:A876` | 382 |

Each page copy is an `$0800`-byte MVN into `$7F:B000`, followed by an increment
of `$F1` for upload. On carry clear the old page fades out first; both fade
loops have sixteen steps and two frame waits per step. The hold follows fade-in,
except for page 19. The US page 0 call clears carry, while the other immediate
terminal-page calls set it. Page indices and the sequence bound are extracted
from the native callers; not all twenty stored pages are active in each release.
The source asset/consumer evidence and raw page/atlas previews are available
through the [Go localization commands](language-pack-format.md#tooling).

These are different identity layers. The action object handler and composition
pointer select behavior/layout within a common resident atlas; the small bank-6
uploads replace reserved effect tiles. Town type tables select behavior and
frame composition, while the ROM-character upload that makes those frame tile
numbers resident in VRAM remains a separate seam to map. A decompilation should
not collapse any of these to raw OAM tile numbers.

### Town Building Data (0x1DCFA-0x1DFF9)
128 bytes per town, 6 towns. Identified 2026-07-17: this is the **initial road/
terrain-obstruction map** — `$03:AA1C` block-copies the whole 0x300-byte region
(`$03:DCFA,X`) into the road-map words at `$7F:6800` at new-game init (see
ram-map "Road Construction Encoding" for the bit layout):
| Town | Offset |
|------|--------|
| Fillmore | 0x1DCFA-0x1DD79 |
| Bloodpool | 0x1DD7A-0x1DDF9 |
| Kasandora | 0x1DDFA-0x1DE79 |
| Aitos | 0x1DE7A-0x1DEF9 |
| Marahna | 0x1DEFA-0x1DF79 |
| Northwall | 0x1DF7A-0x1DFF9 |

### Town Structure-System Tables (bank $03)
| SNES address | File offset | Meaning |
|---|---:|---|
| `$03:D3E2-$D3F9` | `0x1D3E2` | Twelve construction-template pointers, each to a 16-byte 4×4 pattern. Payloads `$D3FA-$D4B9` share five house positions and one 2×2 food footprint; only roads vary. JP pointer table `$03:CEE7`; all five ROMs have identical template payloads. [Native geometry contract](regional-differences-technical.md#building-geometry-and-conditional-fillmore-calculation). |
| `$03:91AE-$933B` / JP `$03:8F93-$9126` | `0x191AE` / `0x18F93` | Complete offscreen plot builder, including shared availability/template iteration. Ordinary caller `$03:90DE-$9155` / JP `$03:8EC3-$8F3A` supplies budget 0/1; arbitrary multi-house budgets are not evidence of natural construction behavior. |
| `$03:96BE-$96EE` / JP `$03:94A9-$94D9` | `0x196BE` / `0x194A9` | Construction cell test: tile `$08` or `$D0-$DA` and path flag `$04` required; carry clear means available. Alternate branch `$96DC` / JP `$94C7` enters M1/X0. |
| `$03:9156-$919C` / JP `$03:8F3B-$8F81` | `0x19156` / `0x18F3B` | Complete path-flag rebuild wrapper. Clears visited bit `$04`, seeds the original flood fill from `$7F:6BCF/$6BDB` plot coordinates multiplied by four, and restores its caller's stack. Native Fillmore base-terrain test reaches 16/18 candidate food footprints; not a developed-town maximum. [Evidence](regional-differences-technical.md#building-geometry-and-conditional-fillmore-calculation). |
| `$03:855C-$85C9` / JP `$03:851B-$8579` | `0x1855C` / `0x1851B` | Growth-status producer. Western store merges computed `$7C05` flags into preserved status bits `$0050`; JP computes temporary flags but stores only preserved bits. Low-growth thresholds and plot-count tables also differ; [bounded native evidence](regional-differences-technical.md#regional-growth-status-producer). |
| `$03:DC74-$03:DC7F` | `0x1DC74` | Per-town structure-record array base pointers (`$7F:6BE7 + town*0x200`, 128 × 4-byte records each) |
| `$03:AB6C+` | `0x1AB6C` | Per-town pointers to initial structure-record images (`$FF`-terminated 4-byte records, copied at new-game init `$03:AA51`) |
| `$03:A017/$A364/$A0D1/$A1A1/$A23D/$A29C/$A2F5` | `0x1A017+` | Per-type-class 8-entry action tables (pushed-address−1): house/bridge/field/factory-tier/4/5/6 × actions 0-7. Bridge rows 2-6 all point at the `$A435` no-op — the bridge-indestructibility row |
| `$03:D4D2+/$03:D4E2+` | `0x1D4D2/0x1D4E2` | Construction/rebuild structure-visual class table bases (class `$7D1F` + variant `$7D21` → step-program pointer, armed into `$7F:77E7+rec*8` by the `$03:A4B8/$03:A4A8` HLE pair). Two indirections: `program = word[ word[base + class] + variant ]`, both operands byte offsets, so classes and variants step by 2 |
| `$03:D591/$D5A5/$D5B9`, `$03:D716/$D73A/$D74E` | `0x1D591`, `0x1D716` | **Windmill (visual class 6)** rebuild and construction programs, variants 0/2/4 = turning / restarting / stopped. Record class 3 selects class 6 at `$03:9F37` (rebuild) and `$03:A21A` (construction) |
| `$03:D70C`, `$03:D58B` | `0x1D70C` | **Factory tier (visual class 8)** construction and rebuild programs, from record class 4 (`$03:A24D`, `$03:9F44`) |
| `$03:D5EF+`/`$D784+` | `0x1D5EF` | **House (visual class 0)** rebuild/construction programs, variants `$00`-`$0E` by development level and `$10`-`$1E` for the same levels with record flag `$40` set. A `+$10` pair shares both scaffold frames with its base and differs only in the finished metatile it lands on (`$02` vs `$03` at level 0) — which is why `$40` on a house is a finished-art variant, not a construction marker |
| `$03:D754-$D77E` | `0x1D754` | **Bridge (visual class 2)** construction programs: first stage `$4C/$4D`, completed `$44/$45`; variants `+8` select Northwall ice `$EA/$EB` then `$E2/$E3`. Orientation occupies variant bit 2 and build stage bit 1 |
| `$03:DBBD/$DBCA/$DBD7/$DBE4` | `0x1DBBD` | Windmill construction draw lists: top-left metatiles `$04`/`$06`/`$14`/`$16`. The fourth draws the finished mill, identical to blade frame 2 |
| `$03:DBF1/$DBFE/$DC0B` | `0x1DBF1` | Windmill blade cycle, top-left metatiles `$24`/`$26`/`$16` — three positions 30° apart in the wheel's 90° visual period. The lower row (`$1E`/`$1F`) is the static mill body in all three |
| `$03:DC38/$DC45` | `0x1DC38` | Factory-tier draw lists: scaffold `$34`, finished `$36` |
| `$03:D2FA/$03:D306` | `0x1D2FA` | Development target-site coordinate tables |

**Step-program format** (interpreter `$03:A4F7`, decoded 2026-08-17). A program is a
sequence of 4-byte entries, each two words. An entry whose first word is `$FD`/`$FE`/`$FF`
is an opcode; anything else is `{duration_in_ticks, draw_list_pointer}` and draws that list
through `$03:A591`:

| First word | Meaning |
|---|---|
| `$FF` | begin loop: store the following word as the repeat count in slot `+1`, and the cursor as the restart address in slot `+4`. The shipped programs all pass `0`, which the `$FE` pre-decrement turns into 256 iterations |
| `$FE` | end loop: decrement slot `+1`, jump to slot `+4` unless it reached zero |
| `$FD` | end. The slot stops advancing and the last drawn frame persists |

Draw lists are a **one-byte** count followed by that many `{dx, dy, metatile}` triples, added
to the record's cell X/Y. This is why a windmill's animation is not tile animation: the town
has exactly one animated CHR page and it is water (see rendering §7). A mill turns because
its step program rewrites its own 2×2 block in the BG1 tilemap.

### Town scenery / ambient-actor tables (banks $03/$01/$0A)

The data behind field workers, pen animals, boats and burning-house flames. Everything is
keyed by *structure class under a fixed absolute map cell*, so these tables are a clean mod
seam: moving an animal is a byte edit, not a code change.

| SNES address | File offset | Meaning |
|---|---:|---|
| `$03:FD0E-$03:FD3F` | `0x1FD0E` | Ambient scene-script pointer table, 25 entries indexed by `$7F:9222 + town*2` (index 0 unused). Entries 5-12 = class-2 field workers, 13-24 = class-5 pen sheep |
| `$03:FD40-$03:FE2F` | `0x1FD40` | The 10-byte scene-script records themselves: `{op, x, y, class, scene}`, all words. `x`/`y` are base offsets in units of 4 map cells (`XBA; LSR; LSR` = ×64 px); zero in every ambient record, so those scenes address cells absolutely |
| `$03:E54C-$03:E66D` | `0x1E54C` | Cutscene scene-script records, same 10-byte layout, invoked by story-event handlers via `LDX #record; JSR $CA93`. `$E628` = Aitos ranch horse (base 3,5 → cell 12,20), `$E63C` = Aitos fleece sheep (base 4,4 → cells 16,16 / 17,17) |
| `$03:CE5B-$03:CEC2` | `0x1CE5B` | Scene-id → object-list pointer table, **52 scenes**. Scenes `$24`-`$2F` are the twelve 6-cell sheep-pen groups covering the {4,5,8,9,12,13,16,17,20,21,24,25}² pen lattice |
| `$03:CEC3+` | `0x1CEC3` | The scene object lists: `$FF`-terminated bytes, each an index into `$0A:C800` |
| `$0A:C800-$0A:C95E` | `0x54800` | Scenery-object index: **190 words**, each an offset from `$0A:C800` to a record `{cell X, cell Y, kind, …script}`. `kind` is always even |
| `$03:E66E-$03:E679` | `0x1E66E` | Per-town story-event handler tables (`$E67A/$E93C/$EBC2/$EE3E/$F049/$F2D7`, exactly 32 entries each), dispatched by `$03:E1D2` |
| `$03:DCA2/$DCAE/$DCBA` | `0x1DCA2` | Story-event bitmap base pointers → `$7F:9107`/`$911F`/`$9137` (prereq / fired / dispatched), 4 bytes = 32 event ids per town. **Not** lair masks — corrected 2026-08-17 |
| `$03:F4D7-$03:F4DE` | `0x1F4D7` | Event-bit mask table `80 40 20 10 08 04 02 01` — the bitmaps are **MSB-first** |
| `$03:F531` / JP `$03:F00D` | `0x1F531` / `0x1F00D` | Six population-event list pointers; 30 `{threshold:u16,event:u8}` rows with single-byte `$FF` terminators. Consumer US/PAL `$03:E122-$E15C`, JP `$03:DC27-$DC61`, enables only when population **exceeds** the threshold. JP Fillmore event 5 uses 88 versus 110; Kasandora event 9 uses 400 versus 700. Other 28 rows match all five ROMs. [Population prerequisites](regional-differences-technical.md#population-and-road-story-prerequisites). |
| `$03:F59D` / JP `$03:F079` | `0x1F59D` / `0x1F079` | Six road-event list pointers, five `{square_x,square_y,event}` rows, `$FF` terminated. Consumer US/PAL `$03:E15D-$E19B`, JP `$03:DC62-$DCA0`; predicate `$03:9777` / `$03:9562` requires road-word bit `$0800` and rejects mask `$0240`. All five ROMs match. These producers latch prerequisites, not completed events. |
| `$03:EA72-$EB06` / JP `$03:E566-$E5FA` | `0x1EA72-0x1EB06` / `0x1E566-0x1E5FA` | Bloodpool crop event 5 and Teddy event 6 callbacks; selected by `$03:E93C` / `$03:E430`. Teddy reads return marker `$7F:918D`, supplied after Bread consumption by US/PAL `$01:9D44-$9D4E` / JP `$01:9D20-$9D2A`. [Callback/inventory/hold ordering](regional-differences-technical.md#bloodpool-crop-and-teddy-event-joins). |
| `$03:E092-$E0CD`, `$03:E19C-$E1F1` / JP `$03:DB97-$DBD2`, `$03:DCA1-$DCF6` | `0x1E092`, `0x1E19C` / `0x1DB97`, `0x1DCA1` | Town-event pipeline refreshes population/road prerequisites, then scans eligible unfired events in ascending order. The stacked-RTS dispatcher must preserve callback rejection unwinding. [Bloodpool priority and Compass evidence](regional-differences-technical.md#bloodpool-disputes-music-and-compass). |
| `$03:EA2E-$EA71`, `$03:E342-$E39D` / JP `$03:E522-$E565`, `$03:DE47-$DEA2` | `0x1EA2E`, `0x1E342` / `0x1E522`, `0x1DE47` | Bloodpool connection event 4 checks crop knowledge, teaches Fillmore and upgrades active class-2 fields across its 128 records. Without knowledge it still marks event 4 fired/dispatched. [Crop-sharing order](regional-differences-technical.md#bridges-and-cross-town-crop-sharing). |
| `$03:EB08-$EB94` / JP `$03:E5FC-$E680` | `0x1EB08-0x1EB94` / `0x1E5FC-0x1E680` | Bloodpool dispute and Compass callbacks. Failed act-count gate clears prerequisites 8 and 9 in Western ROMs, only 8 in JP. Compass itself has no bitmap-test call before its prologue BEQ. Preserve actual caller priority, not inferred gates. |
| `$01:CF2B-$01:CFA8` | `0x0CF2B` | Kind → variant table: 9 row pointers followed by 9 × 12 variant bytes. Indexed **by byte** with the raw `kind`, so the row is `kind/2`. kind 0 people, 2 horse, 4 dog, **6 sheep**, 8 boat, 10 flame, 12 `$DD3F` family |
| `$01:A91C-$01:A96D` | `0x0A91C` | Spawn-list 6 variant array (41 entries, reached via `$01:A227[6]`); variants `$0C`/`$0D` horse, `$0E`/`$0F` dog, `$10`/`$11` sheep, `$12`-`$15` boat. Compositions follow at `$01:A96E+` |

### Town command menu tables (USA)

The menu has six categories, 15 commands and 20 offering identities.
Bank `$01` interleaves executable code, fixed labels, interactive dialogue and
sprite data; decode each through its actual consumer. File offsets below are
headerless LoROM offsets. The [SIM menu reference](sim-menu-reference.md)
contains the action, item and dialogue-source inventories; the
[symbol map](research-symbol-map.md#town-command-navigation-and-offerings)
records the callable boundaries.

| SNES address | File offset/range | Meaning |
| --- | --- | --- |
| `$01:F32E-$F349` | `0x0F32E-0x0F349` | Town navigation bytes: high nibble = category 0–5, low nibble = action 1–15 or zero for a category node; `$FF` separates groups and a second `$FF` ends the list. |
| `$01:F34A` | `0x0F34A` | Packed fixed-label destination `$0512` (column 18, row 5), followed by the action pointers. |
| `$01:F34C-$F369` | `0x0F34C-0x0F369` | Fifteen action-label pointers, indexed in native action-ID order. |
| `$01:F36A-$F375` | `0x0F36A-0x0F375` | Six category-label pointers. |
| `$01:F08C` / `$F08E-$F0B5` | `0x0F08C` / `0x0F08E-0x0F0B5` | Held-inventory base word `$02A2`, then 20 item-label pointers. Each pointed record begins with its icon family byte, followed by fixed text. Duplicate label pointers do not merge item identities. |
| `$01:9C94-$9CBB` | `0x09C94-0x09CBB` | Twenty item Use/reward handler-minus-one words for the stacked RTS dispatcher `$9C6E/$9C6F`; not callable label/description records. |
| `$01:8916-$8927` / JP `$01:88FF-$8910` | `0x08916-0x08927` / `0x088FF-0x08910` | Source of Life/Magic collection: Western IDs5/6 invoke Use immediately, JP stores them in held inventory. A Western automatic grant can remove an older held same-ID Source; preserve carry-over inventory when implementing regional switches. [Native transactions](regional-differences-technical.md#sources-of-life-and-magic-collection-versus-use). |
| `$03:E865-$E8B5`, `$03:F247-$F297` / JP `$03:E359-$E3A9`, `$03:ED23-$ED73` | `0x1E865`, `0x1F247` / `0x1E359`, `0x1ED23` | Fillmore / Marahna fishing completion. Fillmore target 255 Western / 128 JP; Marahna 128 everywhere. Grant item5/6, set fired10, clear dispatched10. [Counter, Compass and one-shot contracts](regional-differences-technical.md#source-discoveries-and-compass-fishing). |
| `$01:8105-$810A` / JP `$01:80FE-$8103` | `0x08105-0x0810A` / `0x080FE-0x08103` | Palace initialization writes `$01` to `$7F:9102` in all five ROMs. Clears fishing-init masks `$40/$20` without deleting town Compass knowledge or completion; unfinished callbacks reset their counter on re-entry. [Live excursion and source evidence](regional-differences-technical.md#source-discoveries-and-compass-fishing). |
| `$03:F424-$F457` / JP `$03:EF00-$EF33` | `0x1F424-0x1F457` / `0x1EF00-0x1EF33` | Northwall scroll callback. `LDA #$A3` makes its local rejection BEQ untaken, not a read of `$91A3`. Normal selector prerequisites remain effective. [Native controls](regional-differences-technical.md#northwall-scroll-callbacks-constant-condition). |
| `$01:AB20` → `$01:AB32` | `0x0AB20` → `0x0AB32` | Scene initialization pointers; all six towns use `$AB32`. Six-byte records hold X, Y and family; `$FFFD` jumps, `$FFFE` skips a record, `$FFFF` ends. The first 21 records initialize the root menu. |
| `$01:A227` | `0x0A227` | Family-to-variant-table pointers. Menu variant 0 selects color artwork and variant 1 selects grey artwork; scripts resolve composition pointers. |
| `$01:A365` → `$A451/$A531` → `$D36D/$D61F` | `0x0A365`; `0x0A451/0x0A531`; `0x0D36D/0x0D61F` | Offering 8 (Wheat; JP rice) selects family `$3C`, using one 16×16 part at tile `$1C4` with palettes 5/7. Both menu variants are pixel-identical across five ROMs, despite translated labels. JP variant table/scripts/compositions relocate to `$A334`, `$A420/$A500`, `$D2F7/$D5A9`. [Native resolver and artwork evidence](regional-differences-technical.md#crop-offering-menu-artwork). |
| `$01:A385/$A389` → `$D3AC/$D3B2` | `0x0A385/0x0A389`; `0x0D3AC/0x0D3B2` | Dog/fertilizer icon families `$44/$45`; selected/grey scripts and matching pixels survive in all five ROMs. JP assigns them item IDs 16/17; Western labels alias Bomb. Both Use handlers are bare returns in every release. [Full reference chain](sim-object-catalog.md#dog-and-fertilizer-item-remnants). |
| `$01:A2C1` → `$A3A1/$A48D` | `0x0A2C1` → `0x0A3A1/0x0A48D` | Family `$0B`, Observe the People angel: selected/grey scripts resolve `$D134/$D3E6`. Used for the modern Describe hint. |
| `$01:A321/$A325` | `0x0A321/0x0A325` | Yes/No family `$23/$24` variant tables. Selected compositions `$D1E2/$D1F7`; grey compositions `$D494/$D4A9`. |
| `$04:C6AE-$C6D5` | `0x246AE-0x246D5` | Twenty offering-receipt dialogue pointers, indexed by `(item_id-1)*2`; receipt text follows inventory transfer and is not a read-only item description. IDs 5/6 use immediate-grant dialogue instead. |
| `$01:899B-$8A3E` | `0x0899B-0x08A3E` | Master report and optional score page. JP `$01:8978-$89CE` closes after the first report. US `$89FD-$8A21` sums twelve stored BCD scores before page composition. |
| `$03:F5ED` / `$03:F791-$F7AD` | `0x1F5ED` / `0x1F791-0x1F7AD` | Six town-maintenance list pointers / crop-replenishment leaf. Lists use RTS-target-minus-one words and `$FFFF` terminators; only Bloodpool owns the crop leaf. JP `$03:F0C9` / `$03:F26D-$F289`; shared gate, development-scheduled. |
| `$03:F621-$F670` / JP `$03:F0FD-$F14C` | `0x1F621-0x1F670` / `0x1F0FD-0x1F14C` | Fillmore Bridge discovery requires prerequisite 2 clear and lair slots 1/2/3 sealed (zero-based); slot 0 is irrelevant. Sets technology/prerequisite/dispatched state and grants item 10 before the temple transaction. |
| `$01:9D6F-$9E02`, `$01:9E82-$9EB6` / JP `$01:9D4B-$9DDE`, `$01:9E5E-$9E8C` | `0x09D6F`, `0x09E82` / `0x09D4B`, `0x09E5E` | Crop item-8 and Music item-11 Use wrappers. Crop upgrades one field, unlike automatic cross-town teaching. Music accepts Bloodpool and writes `$91A5` after consumption/response. Western Music wrapper brackets upload with `$01:93BE/$93CB` interrupt-mask helpers; JP omits them. [Item and event contracts](regional-differences-technical.md#bloodpool-disputes-music-and-compass). |
| `$01:8530-$8563` | `0x08530-0x08563` | Four town status/log/speed wrappers; US closes the menu/C=0, JP `$01:84FA-$850D` returns C=1 to selection. |
| `$01:8AF5-$8B7C` | `0x08AF5-0x08B7C` | Message-speed selector, range0–9. JP `$01:8A8C-$8B13` offers0–7; selector origin differs by one column. |

Composition records contain a part count then five-byte OBJ parts. Tile indices,
palette, size and flips resolve against the scene's resident VRAM/CGRAM; a
composition address alone is not a standalone bitmap. The native menu's grey
variant denotes selection state, not whether a command is available.

### Town OBJ composition landmarks (bank $01, classified 2026-07-22)

Composition addresses consumed by `$01:ADAD`/`$AE6F` via world record `+08`.
`docs/sim-object-catalog.md` is the full catalogue; these are the ones the
sim3d height/anchor classifier keys on, recorded here because several are easy
to mistake for their neighbours.

| Address(es) | Identity | Notes |
|---|---|---|
| `$A627-$A792` | Angel directional/pose frames | **Not** an angel signal on their own — borrowed by miracle effect records |
| `$A589-$A5BC` | Four directional angel animation programs | Four 4-tick poses per direction; `$A627/$A67B/$A6CF/$A705` are the respective first compositions. Preserve part origins rather than tight-cropping each pose |
| `$D134/$D3E6` | Observe the People small angel, color/grey | Family `$0B`, variant table `$A2C1`. This is the ROM icon reused for Describe, distinct from the Listen portrait `$D1D6/$D488` and world-angel pose family. |
| `$A7C5`, `$EC40/$EC6E` | Navigation palace animation and compositions | Two 96-tick, 48×48 frames. Map `$00/$09` asset entry at file `$0282EF` selects raw OBJ chars at file `$02CE7F` → VRAM word `$4000`; palette at file `$0E4093` → CGRAM `$80`. Parts use OBJ palette 1 |
| `$D233-$D302` | Position/direction cursor family | class-`$11` town position controller |
| `$D967/$D972/$D97D/$D988` | Angel arrow vertical/horizontal A/B | record `$0B0A` |
| `$D993` | 64x64 hollow path/area selection square | palette 6, class-`$09` record; a **second** map-plane cursor outside `$D233-$D302` |
| `$D9E5` | Miracle cloud alone | palette 2 |
| `$DA22` | Miracle cloud's own ground shadow ellipse | palette 7, colour-math eligible, drawn +40..+72 below the shared anchor by a co-located record |
| `$DA4B/$DAA1/$DAF7/$DB5C` | Cloud + lightning bolt | one composition spanning cloud to ground (64x76-80) |
| `$DC77/$DBC1/$DC1C/$DCD2` | Cloud + rain streaks | one composition spanning cloud to ground (64x72) |
| `$E1BD/$E209/$E255` | Blue Dragon building-zap bolt | emitted on the dragon's own record, alternating with flight frames; the ROM drops the record onto the target |
| `$DD2D/$DD33/$DD39` | Burning-house fire | tiles `$086/$088/$08A` palette 1, 16x16, corner-anchored (part x=0); spawn script `$01:A838`, one tick a frame |
| `$DD9F/$DDA5/$DDAB` | Volcanic-eruption ground fire | **the same three tiles and palette** as the burning house, re-anchored to the sprite centre (part x=-8); spawn script `$01:A85B`, four ticks a frame |
| `$E6CA/$E6D0/$E6D6` | Ground fire | the same three tiles again, in palette 2 — **the composition triple identifies a family here, not the tile** |
| `$E7D0` | Eruption fireball, not-falling frame | tiles `$11C/$035` palette 1, two 8x8 parts, V-flipped, bounds y 0..16; spawn script `$01:A853`. Worn while climbing out of the crater (`+$1C = -8`) or staged for release (`+$1C = 0`) |
| `$E7A6` | Eruption fireball, falling frame | the same two tiles unflipped, bounds y -4..12; spawn script `$01:A857`. Carries `+$1C = +8` in every observation |
| `$E71B/$E73A/$E75E` | Napper ground-pluck frames | the near-ground phase of class `$13` state 5 |
| `$E99C-$E9C6` | Sailboat frames | water plane |

### Town spawn scripts (bank $01, volcanic eruption mapped 2026-08-18)

Animation scripts held in world record `+$06` (base) and `+$02` (cursor), each
a run of `duration, composition` frames terminated by a `loop`/`cycle` or
`hide` control. Crawlable with `tools/sim_object_catalog.py crawl`; only the
entries this work needed are listed. All four sit in spawn list 6 alongside
the rest of the town's ambient art.

| Address | Frames | Role |
|---|---|---|
| `$01:A838` | `$DD2D/$DD33/$DD39`, 1 tick each, looping | burning house |
| `$01:A849` | `$E6CA/$E6D0/$E6D6`, 4 ticks each, looping | ground fire (palette-2 blue variant of the same art) |
| `$01:A853` | `$E7D0` held, `hide` | eruption fireball while climbing or staged |
| `$01:A857` | `$E7A6` held, `hide` | eruption fireball while falling |
| `$01:A85B` | `$DD9F/$DDA5/$DDAB`, 4 ticks each, looping | eruption ground fire |

The eruption's three scripts are consecutive and are run by records that all
publish packed identity `$0E01` in `+$0E`; see `docs/ram-map.md`. One record
walks `$A853 -> $A857 -> $A85B` across a single flight, so these are phases of
one fireball rather than three actors.

### Town actor scripts (bank $0A, class-$01 VM decoded 2026-08-18)

Motion and lifecycle for stride-`$26` world actors are a **byte-code script**,
not a table. `$01:CFC7` fetches one command per frame from `+$16` and
post-increments it; `+$0E & $00FF` picks the bank — class 0 reads `$7F` RAM
(generated townspeople paths), anything else reads `$0A:0000,X`, so class-`$01`
actors including the whole volcanic eruption run **static ROM scripts**.
`$01:CD35` zeroes `+$1A/+$1C/+$1E`, fetches a byte, treats `$7F` as end-of-script
(`BRL $B891`) and otherwise dispatches through the 18-entry table `$01:CD6F`.
The table stores *target - 1* (`PHA`/`RTS` idiom), and each handler tail-jumps
to `$01:AC70` via `$CD6C` — which is what makes it exactly one command a frame.

| Cmd | Handler | Operands | Effect |
|---|---|---|---|
| `$01` | `$CD94` | – | `+$1C = -1` (up-map) |
| `$03` | `$CDCC` | – | `+$1C = +1` (down-map), `+$1E = $10`, state 3 — **16 map pixels per command** |
| `$04` | `$CDE8` | – | scales `+$1A/+$1C` by 8; one branch sets `+$1E = 2`, the other `+$1E = 8` |
| `$09` | `$CE5F` | 2 (LE16) | wait N frames — writes `+$22`, state 2 |
| `$0A` | `$CE74` | – | end actor (`JSR $B891`) |
| `$0B` | `$CE78` | 2 (LE16) | relative jump: `+$16 += operand` |
| `$05`-`$08`, `$0C`, `$0D` | `$CE43`/`$CE4A`/`$CE51`/`$CE58`/`$CE8F`/`$CE96` | – | select visual 4-9 via `$CF0A` |
| `$0E` | `$CE9D` | 1 | speed modifier: stores the byte to **both** `+$18` and `+$19`. `$CE04` then reads `+$19` — `$FF` scales `+$1A/+$1C` by 8 (`+$1E = 2`), `$FE` by 2. `0E FF` is what gives the eruption its ±8. |
| `$0F` | `$CEAB` | – | land: `$03:AF65(2)` result + 10 into `$CF0A` |
| `$10` | `$CEC0` | 2 | set position from **cell** coordinates: each byte sign-extended and shifted left four, into `+$0A`/`+$0C` |
| `$11` | `$CEE5` | 1 | trap (`BRK`) — request with a byte selector |
| `$02` | `$CDB0` | – | `+$1A = +1` (right) |
| `$04` | `$CDE8` | – | `+$1A = -1` (left) |
| `$7F` | — | – | end of script |

`$00` is a bare `RTS`. Every command tail-jumps to `$01:AC70` via `$CD6C`, and
`$CF0A` resolves a visual index through the per-class table at `$01:CF2B` into
a composition pointer — which is how one script selects `$E7D0`, `$E7A6` and
the ground-fire frames in turn.

A complete eruption fireball, from record `$0FA4`'s script at `$0A:D330`:

```
09 50 00        wait 80 frames
0E FF           speed modifier $FF -- from here velocities scale by 8
11 10           trap, selector $10
10 09 08        set position to cell (9,8) = world (144,128) -- THE CRATER
01 x9           nine up commands   -- the jet climbing out of it
10 0D FF        set position to cell (13,-1) = world (208,-16) -- staging
09 4C 00        wait 76 frames     -- matches the +$22 = 76 seen on this record
03 x7           seven down commands -- the fall: 7 x 16 = 112 px
0F              land               -- hand over to the ground fire
```

**The crater is authored too** -- `10 09 08` = world (144,128), identical in
every eruption script, so nothing about the launch point needs learning at
runtime. And the staging columns match the captured records exactly: cell 13
-> 208, cell 17 -> 272, cell 5 -> 80.

**The landing row is therefore static ROM data**, not a runtime decision:
`fall = (length of the $03 run) x 16` map pixels from the entry row of -16.
Checked against captured landings — record `$0FA4` 7x16 = 112 px against an
observed 112, and `$1016` 8x16 = 128 against 128. Scripts loop, so a run must
be matched to the right cycle.

Watching that same record execute (run `20260818-190000`-era replay of
`saves/aitos-eruption.rec`, sim build serials):

| builds | record position | `+$1C` | phase |
|--------|-----------------|--------|-------|
| 718 | `(144,128)` | 0 | crater placement |
| 720–745 | `(144,120)` → `(144,-16)` | −8 | climb, 144 px at 8 a build |
| 746 | `(208,-16)` | 0 | **staging teleport, sideways** |
| 746–823 | `(208,-16)` | 0 | wait, 77 builds |
| 824–844 | `(208,-16)` → `(208,96)` | +8 | descent, 112 px |

**One record is a fireball OR its fire, never both.** `$0F` hands the record
over to the ground-fire script `$01:A85B`, and the next cycle of the throw
script takes it straight back — so the eight eruption records cap the combined
population of airborne fireballs and burning cells at eight. Measured across
the captured eruption: 45 throws, every one landing on a cell that catches
fire, but with 7 air/1 fire and 8 air/0 fire as the two commonest instants.
A projected view that draws the whole flight therefore shows far more arcs
than flames, and that is the ROM's arithmetic rather than a dropped landing.

**The wait is legible only from the record.** `$01:CE5C` stores a `$09`
operand into `+$22` and then advances the cursor PAST the command, so during a
countdown the cursor is already on whatever follows and the script alone
cannot say how much of the wait is left. Traced on `$0FA4`: cursor parks at
`$D349` for all 76 frames while `+$22` counts 76 down to 0, and the same holds
for the 80-frame `09 50 00` at the top (cursor `$D333`). Total frames to the
landing is therefore **`+$22` plus one frame per remaining command plus the
full operand of every wait not yet reached** — 176 from a cursor at the script
base, which is exactly what a parked record reports.

Two more things worth pinning. The climb happens **in the crater column** and
only crosses `y = 0` on its way out of the map — so "above the map" alone does not
mean "staged", and a presentation that opens a throw on a negative row catches
the climber and throws every fireball to the crater's own column. And the
sideways move is the teleport at build 746, not a gradual drift; the landing
column is knowable at the crater placement (the `$10` operand is ahead of the
cursor) and unreadable afterwards (it is behind it).

### Text Data (Bank $04: 0x20000-0x27FFF)

These are USA coarse ranges, not a sequential-record grammar. For exact
consumer-rooted extraction use the verified profiles in
`installer/internal/localization/data/*-profiles.json` and the Go
`actraiser-builder localization-extract` command; the [dialogue reference](dialogue-system.md)
explains native controls and source identity. Menu-bank text is interleaved
with code and tables and is not covered by the bank-04 ranges alone.

| Range | Content |
|-------|---------|
| 0x20000-0x2000D | Town name pointers |
| 0x2000E-0x20042 | Town names |
| 0x20043-0x2004A | Enemy name pointers |
| 0x2004B-0x020076 | Enemy names |
| 0x20077-0x21396 | Angel dialogue |
| 0x21532-0x246AD | Town dialogue (additional event roots are consumer-resolved) |
| 0x246AE-0x24C99 | Offering descriptions |
| 0x24C8A-0x258F2 | Ending sequence text |
| 0x258F3-0x25EF2 | Text compression dictionary |

USA fixed-text source tables (SNES bank `$01`, same numeric file offset):

| Address | Entries / meaning |
| --- | --- |
| `$01:F04F` / `$01:F08E` | 4 selected-magic / 20 selected-possession pointers; one-based native selections |
| `$01:F1BD` | 7 city-name pointers |
| `$01:F272` / `$01:F290` | 8 Sky root / 4 Sky choice pointers |
| `$01:F34C` / `$01:F36A` | 15 simulation root / 6 simulation choice pointers |
| `$01:F484` / `$01:F4DC` / `$01:F5BC` | Master / Cities / Score dynamic records |
| `$01:EF3B` / `$01:EFE6` | Name-entry prompt/alphabet / selector records |
| `$01:F46D` | Numeric-only fixed record, not a prose message |

Western dictionaries each occupy 128 × 12 bytes: file offsets USA `0x258F3`,
Europe English `0x258F1`, German `0x25A75`, French `0x25989`. Japanese has no
equivalent dictionary. Fixed-composer table and RAM addresses also move across
releases; do not apply a single bank-wide offset to translate them.

### World-map construction, navigation, and presentation (mapped 2026-07-27)

The native build is cleanly separable from Mode-7 presentation. `$02:B475`
copies or decompresses the 16 KiB base into `$7E:C000`, calls the bounded and
yield-free `$02:865C` development pass, then uploads the result through
`$2118`. `$02:865C` requires `$19=09` and destination pointer bank `$AA=7E`,
but it does not configure Mode 7, upload VRAM/CGRAM, or wait for a frame. Its
only dynamic inputs are the six town cell maps at `$7F:2000-$37FF`, their
enable words at `$7F:6B18-$6B23`, and `$7F:9101` bit 0.

The host now implements that middle phase as a pure HLE and treats
`$7E:C000-$FFFF` as shared scratch, not persistent map state. Fixture and live
differential tests against the ROM routine match all 16,384 bytes. This matters
for direct act-to-town transitions: action stages durably overwrite rows 0-79,
while ordinary town frames reuse rows 0-7, so no fingerprint or staleness
policy can make the shadow authoritative.

| SNES address | File range | Meaning |
|---|---:|---|
| `$02:8000-$80FF` | `0x010000-0x0100FF` | 256-byte ordinary town-cell → world-tile translation; zero preserves the base tile |
| `$02:8100-$8133` | `0x010100-0x010133` | Thirteen four-byte 2x2 expansions for special cells `$E3-$EF` |
| `$02:8134-$81F9` | `0x010134-0x0101F9` | World camera initialization. Nonzero US `$031A` takes the emergence branch at `$81BE`, initializes focus/camera and the sprite mask; ordinary navigation uses the other branches. |
| `$02:8550-$865B` | `0x010550-0x01065B` | US Death Heim emergence sequencer, camera/mask jitter and mask-bit update. Reveals existing terrain through repeated sprites, then fades and returns to the Palace; not a Mode-7 island-height animation. |
| `$02:87A5-$87B0` | `0x0107A5-0x0107B0` | Six little-endian destinations for the 32x32 town overlays |
| `$02:902F-$912E` | `0x01102F-0x01112E` | 64 four-byte emergence steps: word offset into `$031C`, then word AND mask. Each of the eight low bytes loses each bit exactly once; fixed scattered order. |
| `$02:AFCB-$AFF7` | `0x012FCB-0x012FF7` | Conditional 16-byte DMA of the emergence mask to VRAM word `$47F0` (OBJ tile `$7F` first two bitplanes). |
| `$01:EDF8-$EF38` | `0x00EDF8-0x00EF38` | Emergence mask composition: count 64 followed by 64 five-byte parts, tile `$7F` repeated over a 64×64 screen-pixel square. Fixed record `$06D6` owns it during the cutscene. |
| `$01:B73C-$B757` | `0x00B73C-0x00B757` | Seven `(x,y)` top-left pairs for the 256x256 location-label regions |
| `$06:B341-$F340` | `0x033341-0x037340` | Uncompressed row-major 128x128 base tilemap, 16 KiB |
| `$0A:B000-$B0FF` | `0x053000-0x0530FF` | Four 64-byte water frames, selected every eight game frames |
| `$0E:8000-$BFFF` | `0x070000-0x073FFF` | 256 uncompressed 8x8 8bpp Mode-7 characters, 64 bytes each |
| `$1C:BF93-$C192` | `0x0E3F93-0x0E4192` | Complete 256-entry BGR555 world-map palette |

World-navigation state is likewise explicit. `$02:8213` updates focus
`$0300/$0302` and zoom target `$0318`; `$02:8384` uploads current matrix
`$0304-$030A` and focus to the Mode-7 registers. `$01:B6CA` clears `$0341`,
then selects the first `$01:B73C` rectangle containing the focus; zero means
the Palace is outside every town border. That same 1-based identity selects
the native location glyphs and the host `city.*.name` localization key; the
native OAM is a variable glyph prefix followed by a fixed 6x2 plaque and 3x3
Palace. `$02:AF86` supplies animation by
copying one `$0A:B000/B040/B080/B0C0` high-byte plane into both Mode-7 tiles
`$00` and `$AA`.

### Graphics & Maps
| Range | Content |
|-------|---------|
| 0x28000-0x28E3F | Map metadata |
| 0x2CE7F-0x2EE7E | Uncompressed graphics |
| 0x2EE7F-0x2FF7F | Compressed graphics (LZSS) |
| 0x2FF80-0x2FFFF | Map palettes |
| 0x50000-0x52FFF | Town maps (base + obstacle layers) |
| 0x53000-0x530FF | World-map water animation (four 64-byte frames) |
| 0x60000-0x6FFFF | Uncompressed graphics (large block) |

### Audio Samples (0x40000-0x4FD2D)
32 BRR-encoded sound samples (indices 0x00-0x21).
Each sample has a 16-bit length header followed by BRR audio data.
This is the stage-2 chunk pool (`$08:8000`) scanned linearly by the `$02:9964`
upload HLE: each song image's terminator doubles as a script of chunk indices
selecting which samples stream into ARAM (common bank = chunks 0-11 → srcn
`$00-$0B`; per-song instruments land at srcn `$0C+`). See [native audio channels](snes-native-audio-channels.md).

The resulting common-bank directory is stable and verified 1:1 (DIR page
`$2C00`, never rewritten at runtime): srcn `00`→`$3000`, `01`→`$3B01`,
`02`→`$44EB`, `03`→`$4545`, `04`→`$4F2F`, `05`→`$5814`, `06`→`$5DB4`,
`07`→`$5DD8`, `08`→`$6906`, `09`→`$6DA1`, `0A`→`$6DF2`, `0B`→`$6E4C`.
`srcn 00` has start == loop == `$3000`, so a key-on with no key-off sustains
indefinitely. Music key-ons observed in this range were not intentional
shared-bank instruments: they came from the fixed bootstrap race that cleared
the sequencer's `$11FF=$0C` instrument-base byte after the common upload (see
[Workshop guide](builder-workshop.md)).

### Compressed Data (0x70000+)
Extensive compressed sprite composition, map arrangement, and tileset data
using Quintet's standard LZSS compression algorithm.

## Compression Format

ActRaiser uses **Quintet's standard LZSS** with a 256-byte sliding window.
The same compression is used across other Quintet games (Soul Blazer, Illusion of Gaia, Terranigma).

For ActRaiser, tokens share one MSB-first bit stream: a set control bit selects
an 8-bit literal; a clear bit selects an 8-bit ring offset and a 4-bit length
minus two. The ring starts filled with `$20`, with its write cursor at `$EF`;
matches may overlap their own output and wrap the ring. A blob's decoded-size
word precedes the stream. Do not decode tokens as byte-aligned records.

The diagnostic `compressed_byte_count` in localization source IR preserves
`tools/quintet_lzss.py`'s stream-cursor convention: `floor(bits_read / 8) + 1`,
excluding the two-byte size header. On an exactly byte-aligned ending it counts
one beyond the consumed stream. It is not an exact source-blob span for copying
or rewriting ROM bytes; use the actual bit count and header format for that.

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
