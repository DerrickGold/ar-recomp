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
| $0A | 0x050000-0x057FFF | Town maps, world-map water animation | DATA |
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
- **0x1B825-0x1B8FC** (`$03:B825`): **Monster-lair seed table** — 24 records × 9 bytes
  (4 lairs per town × 6 towns), installed by `$03:B7C6`:
  `[cellX, cellY, imageId, monsterType, count, respawnDelay(word), worldRecordAddr(word)]`.
  X/Y are 16px town-map cells 0..31. Dump with `tools/act_content.py --lairs`; field
  semantics in SEAMS "Content / randomizer seams" §6 and ram-map "Monster Lair Data".

### Action content tables (mapped 2026-08-02 — SEAMS "Content / randomizer seams")

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
| Action BG ROM-source catalogue; measured anchor Aitos `$04/$01` BG2 | Aitos anchor: map `$1C:E7DF` / `0x0E67DF`; metatiles `$0A:C50D` / `0x05450D`; CHR `$12:992F` / `0x09192F` → VRAM word `$0000` and `$16:EB09` / `0x0B6B09` → `$1000`; palette `$1C:D8F8` / `0x0E58F8` → CGRAM `$00-$3F` | `DioramaRomBackdrop_LoadActionBg` interprets `$05:8000` bits 7/6/5/4 for every valid action `(group,map,BG1|BG2)` source and replays earlier entries in the same act to reconstruct inherited maps deterministically. It emits the first 256×256 page read-only; no emulated RAM/VRAM/CGRAM is written. The `rom-GG-MM-bgN` grammar covers 98 sources. A stock-ROM census decodes all 98; synthetic tests pin inheritance and malformed streams. For the measured `rom-04-01-bg2` anchor, the map expands to 256 bytes after its `1×1` chunk header; metatiles expand to `$0800` bytes and are word-swapped by `$02:B3CE`; each CHR stream expands to `$2000` bytes. `$02:B6D3-$B6F6` installs action tile-word mask `$ECFF`; `$02:B4E8-$B54C` merges attribute byte `$10` into BG1 and `$01` into BG2. The ROM renderer applies the same transform before character lookup. Comparison against `runs/20260812-000613/snapshots/snap_00_gf4152` matched live `$7E:C000`, `$7E:2900`, VRAM character bytes `$0000-$3FFF`, the complete 2,048-byte BG2 first-page tilemap at VRAM byte `$E000`, and all 65,536 rendered pixels. |
| `$02:B6D3-$B6F6` | `0x136D3-0x136F6` | **Action BG tile-word mask setup.** Stores `$FDFF` in `$54/$58/$5C` when map group `$18` is zero and `$ECFF` when it is nonzero. Action map rendering therefore clears definition bits 8, 9, and 12 before the per-layer attribute merge. |
| `$02:B4E8-$B54C` | `0x134E8-0x1354C` | **Per-section video config tile attributes.** Clears BG attribute bytes `$6B/$6F`, then for action map groups (`$18 != 0`) merges `$10` into BG1 and `$01` into BG2. The tile builder uses these as the high byte after applying `$54/$58`. |

The per-region object-type tables at `$00:96AF/$A8F6/$B449/$C11E/$CD9B/$D928/$E722/$F39A`
(already listed above) are the **enemy stat tables**: each 12-byte record carries ATK at `+7`,
HP at `+8` and death score at `+9`. `tools/act_content.py --tables` decodes all eight.

### Action camera and tile-streaming control

| SNES address | File range | Meaning |
|---|---:|---|
| `$02:B030-$B090` | `0x13030-0x13090` | **Action camera tracking request** — selects the tracked object's X through direct-page object base `$8A`, centres it against the native 256px viewport, applies the vertical dead zone around focus Y `$82`, and stages signed requests in `$7C/$7E`. Called immediately before `$02:B091` from each action frame loop. |
| `$02:B091-$B126` | `0x13091-0x13126` | **Action camera application/stream trigger** — applies `$7C/$7E` to BG1 camera `$22/$24`, clamps against dimensions `$2E/$30`, raises the 16px strip flags in `$93`, then derives BG2 parallax and player-relative coordinates. The HLE seam preserves that tail while optionally fitting the finite horizontal presentation canvas; vertical `$24` retains the native range. |

### Sprite identity and action OBJ assets

| SNES address | File range | Meaning |
|---|---:|---|
| `$00:8C98-$8D67` | `0x00C98-0x00D67` | Action OAM rebuild/cull. Clears the shadow, calls the fixed-HUD emitter, scans action objects, updates object `+$30` activation bit `$0400`, and calls `$00:8D68` for each draw-admitted composition before returning through one common epilogue. The host wide port preserves separate DRAW and ACTIVATION predicates: drawing uses horizontally fitted `$22`, native vertical `$24`, and presentation margins/apron, while `$0400` uses its independently selected activation camera/range. The native pause/freeze path skips this routine while vblanks continue, so its completed-call cadence is the verified presentation clock for host action lighting and particles. The serial itself is host state, not ROM or WRAM data. |
| `$00:9755-$9799` | `0x01755-0x01799` | Paired action-entry arrival actor. Advances and moves the companion/orb object, publishes its motion through `$7C/$7E` and cached focus `$80/$82`, then writes entry fade/raster controls `$CB/$CD/$CE/$CF`. This is a scripted actor in the same object table as enemies; activation-policy changes must therefore preserve the native entry object set rather than treating every non-player slot as an enemy. |
| `$00:97A6-$980F` | `0x017A6-0x0180F` | Player action-entry lifecycle. `$97A6` waits for the approach target, assigns player slot `$08A0` as camera subject `$8A`, and installs `$97C9`; `$97C9` runs the first transform animation and installs `$97E4`; `$97E4` runs the final materialization/fade update and, at sequence end, writes handler `$9832`, flags `$0003`, and resets animation state. This handler chain is the exact no-input interval used by the host margin-activation gate. |
| `$00:9832-$9883` | `0x01832-0x01883` | First normal player ground-control handler after arrival. It installs itself at player `+$12`, reads held input `$A1/$A0`, dispatches attack/jump/magic/walk states, and is the exact handoff that re-enables extended horizontal activation. |
| `$00:8683-$868F` | `0x00683-0x0068F` | Shared action animation-repeat dispatcher: advances through `$00:8631`, decrements object `+$38` at the authored sequence boundary, repeats while nonzero, and dispatches the saved `+$1E` resume when the repeat count reaches zero. A Bloodpool lightning bolt legitimately transitions here from its scene-specific root. |
| `$00:E18E-$E291` | `0x0618E-0x06291` | Marahna linked-lightning family measured in run `20260811-151353`. Source root `$E18E` is retained by the first endpoint and connector child; partner root `$E254` is retained by the adjacent second endpoint. Live connector children resume at `$E24F` and use exact horizontal/vertical `$7E:4000` composition families documented in `ram-map.md`. |
| `$00:E047-$E0A7` | `0x06047-0x060A7` | Marahna fireball spawn/split family measured in run `20260811-221433`. Live orb and four cardinal children retain source `$E047`; the orb resumes at `$E061`, while split children resume through the shared `$A65D` helper. Exact WRAM artwork, velocities, bounds, and parent backlink are documented in `ram-map.md`. |
| `$00:DE96-$DF85` | `0x05E96-0x05F85` | Marahna snake family. Source `$DE96` cycles through wait/rise/fall handlers, allocates a copied child at `$DF0A`, and drives its state-`$06` horizontal lifecycle through shared `$A655`. Run `20260811-232640` measures exact child artwork, velocity, flip, counter, and parent backlink in `ram-map.md`. |
| `$00:E0BA-$E18D` | `0x060BA-0x0618D` | Marahna reaper and orb lifecycle. The parent rooted at `$E0BA` allocates a child, installs update handler `$E13A`, and selects loaded-animation states `$17/$3A-$3D` for horizontal, aimed, and vertical paths. Run `20260811-232640` proves this is a non-fire negative family. |
| `$00:E2F3-$E37E` | `0x062F3-0x0637E` | Marahna moving-platform roots and wait/resume tails. Run `20260811-221433` corrects the prior flame-projectile classification: live `$E2F3/$E304/$E315/$E326/$E351/$E368` actors use `$34/$4BE5` but are platform machinery and must not receive fire effects. `$4BE5` itself is decompressed WRAM animation data. |
| `$00:E483-$E600` | `0x06483-0x06600` | Marahna boss and electrical attack family measured in runs `20260811-221433` and `20260811-225534`. The boss and launched child retain source `$E483`; parent waits resume at `$E4E5/$E4F4`, the diagonal descent resumes at `$E578`, and `$E57E` is the live post-impact ground-charge resume—not a retired tail. While it travels, the boss parent repeats through `$E4D7`. Exact `$7E:5000` charge/orb/diagonal/ground artwork and backlink identity are documented in `ram-map.md`. |
| `$00:CF9E-$D024` | `0x04F9E-0x05024` | Aitos lava-fireball spawn and cyclic lifecycle measured in run `20260811-151353`. Live slots retain source `$CF9E` and resume `$CFCD`; handlers `$CFE3` and `$CFFE` own the rising and return phases, while shared delay handler `$8661` owns reset/wait state `$23`. Exact WRAM artwork and velocities are documented in `ram-map.md`; `$4D21/$4D2D` are decompressed WRAM composition pointers, not ROM symbols. |
| `$00:CEEC-$CF5B` | `0x04EEC-0x04F5B` | Aitos lava-mouth / launched-molten-rock family separated in run `20260812-000613`. Active launch children retain source `$CEEC`, resume `$CF16`, handler/state `$8661/$27`, and exact motion/artwork documented in `ram-map.md`; stationary mouths resume at `$CF1C` and are deliberately excluded from the molten-rock accent. `$4D21/$4D2D` remain decompressed WRAM composition pointers. |
| `$00:BD2A-$BD35` | `0x03D2A-0x03D35` | Bloodpool vertical-lightning spawn record. Its computed primary handler is record+`$0C` = `$00:BD36`; live objects retain `$BD2A` in slot `+$32`. |
| `$00:BD36-$BD75` | `0x03D36-0x03D75` | Bloodpool vertical-lightning lifecycle: offscreen gate, packed animation/repeat commands `$0010/$1104/$1406`, SFX `$10`, and transition through the shared animation/retirement helpers. The saved nested resume value is `$BD69` (execution resumes at `$BD6A`). |
| `$00:BD76-$BD81`, `$00:BD84-$BD8F` | `0x03D76-0x03D81`, `0x03D84-0x03D8F` | Two direction/attribute variants of the Bloodpool enemy-fireball spawn record. Live fireballs retain the selected record address in slot `+$32`; `$BD84` is intentionally embedded behind the branch at handler `$BD82`. |
| `$00:BDF0-$BDFE` | `0x03DF0-0x03DFE` | Enemy-fireball flight tail: advance/loop animation through `$00:8631` until object flag `$0400` says it left the currently selected activation window, then release through `$00:85B7`. Drawing and activation are independently selected by the `$8C98` host seam. |
| `$00:BDFF-$BE0A` | `0x03DFF-0x03E0A` | Bloodpool boss spawn record retained as `$BDFF` in the boss and its linked lightning children (`+$32`); computed primary boss handler is `$BE0B`. |
| `$00:BFDF-$BFF8` | `0x03FDF-0x03FF8` | Boss lightning-attack child allocation: allocates an action slot, assigns handler `$BFF9`, links/copies the boss through `$8709`, and offsets the child anchor vertically. |
| `$00:BFF9-$C06E` | `0x03FF9-0x0406E` | Linked boss-lightning sequence. `$BFF9` selects one of six strike states through the table at `$C056` (`$07,$04,$06,$03,$05,$02` = diagonal/vertical × short/medium/long), runs each strike/blank cycle through delay handler `$8661`, then creates the state-9 floor child handled at `$C062`. Strike saved resumes observed live are `$C02B/$C04B/$C051`; floor resume is `$C06A`. |
| `$00:9CF2-$9D1B` | `0x01CF2-0x01D1B` | Player ranged-sword creator. Allocates a linked action child, copies the player source/backlink, marks it as an attacker, selects animation state `$13` or `$14`, and advances animation through `$8E2F`. |
| `$00:9D1C-$9D3D` | `0x01D1C-0x01D3D` | Player sword-beam flight handler. Retires on timer/offscreen/end conditions and otherwise moves the child through `$86BB`; live velocity is horizontally mirrored `8px/tick`. Animation `$06:8000` maps state `$13` to visual/composition `$30/$99E8` and state `$14` to `$31/$9A17`. |
| `$00:D646-$D837` | `0x05646-0x05837` | Aitos Act-2 dragon boss family. `$D785` runs the sword-volley controller through state 0, allocates two generic `$A655` children, and seeds local counters 1/2. Loaded `$7E:5000` states 1/2 supply the two exact diagonal crescent sequences; the children later wait in `$8661` with saved resume `$A65D`. Run `20260812-000613` snapshot 5 measures the normal controller, boss backlink, artwork, velocities, extents, and priority-2 OAM. Run `20260812-224123` snapshot 1 measures the H+V-reflected facing: controller and child both use `$C000`, velocity signs reverse, and extents swap sides as listed in `ram-map.md`. |
| `$00:95DD-$95EC` | `0x015DD-0x015EC` | Eight action handler-table pointers: `$96AF,$A8F6,$B449,$C11E,$CD9B,$D928,$E722,$F39A` for `$18=$00-$07` |
| `$01:E099+` | `0x0E099+` | Town world-object type → behavior/animation-data pointer table |
| `$01:E7D9+` | `0x0E7D9+` | Parallel town world-object type → sprite-frame pointer table; frame lists continue around `$01:E838` |
| `$06:A000+` | `0x32000+` | Conditional 128-byte dynamic action effect-overlay windows selected from polymorphic object `+38`; uploaded to VRAM `$2D80` only for objects with `+30 & $0040` and an idle upload descriptor. Not a universal spell-ID table |
| `$06:A400+` | `0x32400+` | Selected action-magic character windows used by `$02:BC9E`: 256 bytes at `$A400 + (id-1)*$80` for IDs 1-4, uploaded to VRAM `$2D40`. **VRAM word `$2D40` IS OBJ tile `$D4`** at the action OBSEL base (`obsel=$01` → objTileAdr1 = word `$2000`; `$2000 + $D4*16 = $2D40`), so the window lands on tiles `$D4-$D7` — the four 8×8 sprites of the HUD magic icon — and continues into `$D8-$DB`. Verified byte-exact 2026-08-05: ROM `$06:A400` tiles 0-3 decode identically to VRAM tiles `$D4-$D7` in a live Magical Fire snapshot. **Open:** the stated copy size (256 bytes) is twice the per-ID stride (`$80`), so consecutive IDs overlap in ROM and the copy spills past the icon into `$D8-$DB`; the DMA length has not been re-read from `$02:BC9E`, so one of the two figures may be a transcription slip |
| `$07:8000-$9FFF` | `0x38000-0x39FFF` | Common action OBJ atlas, 8192 bytes copied to VRAM `$2000-$2FFF` at level entry |
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

### Town Structure-System Tables (bank $03, mapped 2026-07-17 — SEAMS town §7)
| SNES address | File offset | Meaning |
|---|---:|---|
| `$03:DC74-$03:DC7F` | `0x1DC74` | Per-town structure-record array base pointers (`$7F:6BE7 + town*0x200`, 128 × 4-byte records each) |
| `$03:AB6C+` | `0x12B6C` | Per-town pointers to initial structure-record images (`$FF`-terminated 4-byte records, copied at new-game init `$03:AA51`) |
| `$03:A017/$A364/$A0D1/$A1A1/$A23D/$A29C/$A2F5` | `0x12017+` | Per-type-class 8-entry action tables (pushed-address−1): house/bridge/field/factory-tier/4/5/6 × actions 0-7. Bridge rows 2-6 all point at the `$A435` no-op — the bridge-indestructibility row |
| `$03:D4D2+/$03:D4E2+` | `0x1D4D2/0x1D4E2` | Construction/rebuild structure-visual class table bases (class `$7D1F` + variant `$7D21` → step-program pointer, armed into `$7F:77E7+rec*8` by the `$03:A4B8/$03:A4A8` HLE pair) |
| `$03:D2FA/$03:D306` | `0x1D2FA` | Development target-site coordinate tables (per SEAMS §5) |

### Town OBJ composition landmarks (bank $01, classified 2026-07-22)

Composition addresses consumed by `$01:ADAD`/`$AE6F` via world record `+08`.
`docs/sim-object-catalog.md` is the full catalogue; these are the ones the
sim3d height/anchor classifier keys on, recorded here because several are easy
to mistake for their neighbours.

| Address(es) | Identity | Notes |
|---|---|---|
| `$A627-$A792` | Angel directional/pose frames | **Not** an angel signal on their own — borrowed by miracle effect records |
| `$D233-$D302` | Position/direction cursor family | class-`$11` town position controller |
| `$D967/$D972/$D97D/$D988` | Angel arrow vertical/horizontal A/B | record `$0B0A` |
| `$D993` | 64x64 hollow path/area selection square | palette 6, class-`$09` record; a **second** map-plane cursor outside `$D233-$D302` |
| `$D9E5` | Miracle cloud alone | palette 2 |
| `$DA22` | Miracle cloud's own ground shadow ellipse | palette 7, colour-math eligible, drawn +40..+72 below the shared anchor by a co-located record |
| `$DA4B/$DAA1/$DAF7/$DB5C` | Cloud + lightning bolt | one composition spanning cloud to ground (64x76-80) |
| `$DC77/$DBC1/$DC1C/$DCD2` | Cloud + rain streaks | one composition spanning cloud to ground (64x72) |
| `$E1BD/$E209/$E255` | Blue Dragon building-zap bolt | emitted on the dragon's own record, alternating with flight frames; the ROM drops the record onto the target |
| `$E6CA/$E6D0/$E6D6` | Ground fire | |
| `$E71B/$E73A/$E75E` | Napper ground-pluck frames | the near-ground phase of class `$13` state 5 |
| `$E99C-$E9C6` | Sailboat frames | water plane |

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
| `$02:87A5-$87B0` | `0x0107A5-0x0107B0` | Six little-endian destinations for the 32x32 town overlays |
| `$01:B73C-$B757` | `0x00B73C-0x00B757` | Seven `(x,y)` top-left pairs for the 256x256 location-label regions |
| `$06:B341-$F340` | `0x033341-0x037340` | Uncompressed row-major 128x128 base tilemap, 16 KiB |
| `$0A:B000-$B0FF` | `0x053000-0x0530FF` | Four 64-byte water frames, selected every eight game frames |
| `$0E:8000-$BFFF` | `0x070000-0x073FFF` | 256 uncompressed 8x8 8bpp Mode-7 characters, 64 bytes each |
| `$1C:BF93-$C192` | `0x0E3F93-0x0E4192` | Complete 256-entry BGR555 world-map palette |

World-navigation state is likewise explicit. `$02:8213` updates focus
`$0300/$0302` and zoom target `$0318`; `$02:8384` uploads current matrix
`$0304-$030A` and focus to the Mode-7 registers. `$01:B6CA` clears `$0341`,
then selects the first `$01:B73C` rectangle containing the focus; zero means
the Palace is outside every town border. `$02:AF86` supplies animation by
copying one `$0A:B000/B040/B080/B0C0` high-byte plane into both Mode-7 tiles
`$00` and `$AA`. See
[rendering-engine.md §13h](rendering-engine.md#13h-world-navigation-full-plane-scene-2026-07-27)
for the immutable host scene and fade/effects contract.

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
`$00-$0B`; per-song instruments land at srcn `$0C+`). See SEAMS "Audio".

The resulting common-bank directory is stable and verified 1:1 (DIR page
`$2C00`, never rewritten at runtime): srcn `00`→`$3000`, `01`→`$3B01`,
`02`→`$44EB`, `03`→`$4545`, `04`→`$4F2F`, `05`→`$5814`, `06`→`$5DB4`,
`07`→`$5DD8`, `08`→`$6906`, `09`→`$6DA1`, `0A`→`$6DF2`, `0B`→`$6E4C`.
`srcn 00` has start == loop == `$3000`, so a key-on with no key-off sustains
indefinitely. Music key-ons observed in this range were not intentional
shared-bank instruments: they came from the fixed bootstrap race that cleared
the sequencer's `$11FF=$0C` instrument-base byte after the common upload (see
SEAMS "Audio swap tiers").

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
