# ActRaiser RAM Map

SNES WRAM: 128KB at banks $7E-$7F.
Direct page and stack in first 8KB ($7E:0000-$7E:1FFF), mirrored at $00-$3F:0000-$1FFF.
Unless a release is explicitly named, addresses below describe the US ROM.

## Core Game State ($7E:0000+)

### Game Mode & Navigation
| Address | Size | Description |
|---------|------|-------------|
| $7E:0018 | 1 | Mode/region group: `$00` non-action (town/world/UI); `$01-$06` six two-act kingdom action regions; `$07` Death Heim boss-rush/final-boss action region (no ordinary acts); `$08` ending/credits (post-Death-Heim: mode-0 world montage cycling `$19=09`↔towns, then `$18=08` — presenter at `$02:AA9C`, stamps 'ACT' into SRAM `$70:1FF0`, waits for Start, exits to `$00:8059`) |
| $7E:0019 | 1 | Current raw map/sub-flow number (second byte of map ID). With `$18=00`, `$01-$06` are the six simulation towns in order (Fillmore through Northwall), `$07` is Sky Palace, `$08` is the temple, and `$09` is the world map. In action mode it is not a uniform act selector: Act 2 starts at `$02/$02/$03/$04/$04/$05` for regions `$01-$06`; Death Heim uses `$01` for its hub, `$02-$07` for the six rematch arenas, and `$08` for the final boss |
| $7E:001A | 1 | Destination map number |
| $7E:001B | 1 | Destination map group (first byte of map ID) |

### Platformer Stats
| Address | Size | Description | Notes |
|---------|------|-------------|-------|
| $7E:001C | 1 | Lives remaining | BCD, cap `$99`. Award = `$00:8850`; act entry loads it from persistent `$02AB` (`$02:84D7`) |
| $7E:001D | 1 | Current HP | Set to `$1E` at stage entry (`$00:83CF`); damaged by `$00:8A21` (`$1D -= toucher.$2A`) and by terrain boxes (`$00:8C75`) |
| $7E:001E | 1 | Maximum HP | 8 at new game (`$02:BE5F`); **level-up `$03:B3DF` INCs it, hard cap `$18` (24)**; professional mode starts at 24 (`$02:AB20`). SRAM `$70:1246` |
| $7E:001F | 2 | Score | BCD **stored in tens of displayed points**; `$00:873C` adds and saturates at `$9999` (99,990 displayed). US `$02:C2E8` / JP `$04:9223` formats four digits at `$7F:B074`; template zero at `$7F:B07C` supplies the fifth digit. Item6 adds raw `$0100` = 1,000 displayed; item7 raw `$0050` = 500. Score-to-town formulas consume stored units. |
| $7E:0021 | 1 | Magic points | Act working copy; loaded from persistent `$0295` at `$02:84E0`. Scroll pickup INCs only this (`$00:887E`), cap `$FF` |
| $7E:00E3 | 1 | Heal queue | US `$00:88D6`, JP `$00:88C5`: drain one queued point when `$88 & 3 == 0`, increment HP only below max; otherwise clear residual queue. Both releases' item4 sets `floor(maxHP/4)`, item5 sets `maxHP-currentHP`; **replace**, not accumulate, pending recovery. Native normal/Special pickup/queue fixtures verify equality; differing apple placements are separate. |
| $7E:00E4 | 1 | Sword power-up | `$80` = item id `$03` collected; `$00:9DC8` then gives the player ATK 2 instead of 1. Never ticks down — cleared on act change |
| $7E:00E6 | 2 | Time remaining | BCD format |
| $7E:0349 | 2 | **Professional Mode** | Nonzero = pro mode. Doubles enemy ATK/HP when exactly 1 (`$00:9679`/`$968C`), rewrites statue drops (`$00:962B`), and indexes the stage-order table `$02:9013` at act clear (`$00:8781`) |
| $7E:08BC | 1 | Player **Crest** walking-cycle phase | TAS terminology; interacts with Boost to determine normal/pre-jump movement cadence. Player object `$08A0 + $1C`. |
| $7E:08C4 | 1 | Player **Boost** walking-speed countdown | Can produce temporary 3 px/frame movement; player object `$08A0 + $24`. Extended `AR_FRAMELOG=1` records both fields with input and position delta. |

### European state offsets

European research uses different state offsets; do not decode PAL snapshots
with the preceding US stats layout. Verified in EU English, German and French:

| PAL WRAM field | Size | Meaning |
| --- | ---: | --- |
| `$0205` | 2 | Difficulty1/2/3 = Beginner/Normal/Expert; selective placement, initial HP and contact-damage policy. Native Story save stores it at SRAM `$70:13B6`; cold Continue restores it before rebuilding the timer reload |
| `$0336/$0338` | 2 each | Title choice0/1/2 = Story/Continue/Action; save-valid flag. Neither is difficulty |
| `$034B` | 2 | Action-only progression counter, initialized1 on a fresh Action run; Game Over Start clears it while returning to title, rather than directly restarting Fillmore |
| `$034D` | 1 | Timer reload71/59/47 selected by difficulty |
| `$00E6/$00E7/$00E9` | 1/2/1 | Timer divider / BCD time remaining / hold gate |
| `$008B` | 2 | Active player-slot pointer; fresh Action fixtures resolve to `$08E0`, not US `$08A0` |
| `$0023/$0025` | 2 each | PAL camera X/Y; using US `$22/$24` overlaps the PAL magic-stock word at `$21` |
| `$0081/$0083` | 2 each | PAL cached camera-subject X/Y |
| `$0089` | 2 | PAL native frame tick; use for boss phase intervals, not US `$88` |
| `$0021` / `$1C00+` | 2 / byte entries | PAL Action spell-stack depth / ordered spell IDs, latest first when consumed. Push increments low byte; post-cast pop decrements word. Story uses the low byte as generic scroll count. Not a cross-mode interchangeable field |
| `$02AE` | 2 | PAL selected/active spell; Action accepts the current stack top here before casting |
| `$08A0` | `$40` | PAL HUD magic-icon actor, not the player. `+38` holds next spell or0; handler `$9254` updates VRAM after pickup/cast |
| `$00E4/$00E5` | 1 each | PAL heal queue / sword-power byte, shifted from US `$E3/$E4` |
| `$021A` / `$021E–0229` | 2 / 6 words | PAL total population / six per-town populations; native census/level fixtures, not published maximum-town caps |
| `$0284/$0286` | 2 each | PAL current/maximum SP |
| `$0288/$0289` | 1 each | PAL current/maximum angel HP |
| `$0293` | 2 | PAL earned Master level; native leaf awards upward only |

The [European contracts](regional-differences-technical.md#european-difficulty-and-placement-contracts)
separate these fields from PAL display timing and from authored hazard damage.

### Camera / Scroll

| Address | Size | Description |
|---------|------|-------------|
| $7E:0022 | 2 | BG1/camera X. Action writer/HLE seam `$02:B091`: native clamp `[0,$2E-$100]`; corrected action-wide clamp `[left,$2E-$100-right]` when the complete requested view fits, otherwise native. Town writer `$01:B4C6`: native `[0,$0100]`, corrected-wide `[extra,$0100-extra]` (16:9: `[$002B,$00D5]`, directly validated 2026-07-14). All six scroll regs upload from `$22-$2D` via `$02:ADC3` (10-bit). |
| $7E:0024 | 2 | BG1/camera Y. Action `$02:B091`: native `[0,$30-$E1]`; corrected Diorama interval `[top,$30-$E1-bottom]` when it fits. Town writer `$01:B4C6` clamps to `[0,$011F]`. |
| $7E:0026/$0028 | 2+2 | BG2 H/V scroll (parallax, $02:B9D5/$02:BA0B from ratio nibbles $3A-$45) |
| $7E:002A/$002C | 2+2 | BG3 H/V scroll ($2C pinned $FFFC: HUD up 4px) |
| $7E:002E/$0030 | 2+2 | **BG1 layer = LEVEL pixel width/height** (Fillmore act1: 4096x768) — the camera clamp bounds |
| $7E:0032/$0034 | 2+2 | BG2 layer width/height (scroll clamps only if width >= $300, else wraps) |
| $7E:003A-$0045 | 12 | per-plane parallax ratio nibbles (from section config table $02:893E+7..12) |
| $7E:0046/$004A | 2 ea | BG1/BG2 action map-page bases, indexed by layer stride 4; each points at the live chunk-paged metatile-id map in WRAM. |
| $7E:0048/$004C | 2 ea | BG1/BG2 tilemap VRAM word bases. Ordinary action world layers agree with PPU BGSC and own a 64x64 ring (`$6000/$7000`); disagreement or a non-64x64 BGSC is provider-ineligible. |
| $7E:0052/$0056 | 2 ea | BG1/BG2 metatile-definition table bases in WRAM; four little-endian tile words per metatile. |
| $7E:0054/$0058 | 2 ea | BG1/BG2 tile-word masks applied before the layer attribute merge. `$02:B6D3-$B6F6` writes `$ECFF` for action map groups (`$18 != 0`) and `$FDFF` otherwise. |
| $7E:005E/$0060/$0062/$0064 | 2 ea | record-buffer cursors: BG1col $3900 / BG1row $3A02 / BG2col $3B04 / BG2row $3C06 |
| $7E:006B/$006F | 1 ea | BG1/BG2 action tile-word attribute merges, indexed by layer stride 4. `$02:B4E8-$B54C` clears both, then installs `$10` for BG1 (palette-bank bit in the tile word's high byte) and `$01` for BG2 (tile-bank bit) when `$18 != 0`. Together with `$46/$52/$54`, these are the complete `ActionBgWorld` decoder records. |
| $7E:007C/$007E | 2+2 | camera H/V delta this frame (16-bit signed; strip/parallax/player input). `$02:B030` stages the requested motion; the corrected `$02:B091` HLE reconciles it to motion that actually fit a presentation-aware bound before downstream consumers. Native/fallback paths preserve the request. |
| $7E:0080/$0082 | 2+2 | Cached action-camera subject X/Y. The selected object's `+02/+04` coordinates initialize this pair; `$82` is the vertical focus consumed by `$02:B030`, while action-object code also uses the pair to derive subject motion. |
| $7E:008A | 2 | WRAM offset of the action object selected as the camera subject. `$02:B030` reads subject X from `[$8A]+$02`; object spawn/control paths update the selector when camera-follow ownership changes. Arrival handler `$97A6` does not install the player `$08A0` here until the approach reaches its target, so an entry-time host policy must not assume `$8A` already names the player on the first object scan. |
| $7E:008E | 1 | parallax disable bits (bit0 BG2H, bit1 BG2V = script-driven) |
| $7E:0093 | 1 | strip-request flags: $80 BG1col $40 BG1row $20 BG2col $10 BG2row (set by $02:B091 on 16px crossings, TRB-consumed by dispatcher $02:B127) |

### Action damage boxes and terrain lookup

These are room-owned gameplay data, separate from the sprite hitboxes and
render-layer maps. Native US/JP expansion and contact rules match; regional
differences are in the authored streams. See the
[terrain/damage contract](regional-differences-technical.md#terrain-and-damage-box-contracts)
for tested boundaries, overlaps and changed rooms.

| Address | Size | Description |
| --- | --- | --- |
| `$7E:1AE2` | 2 | Expanded damage-box count, US `$00:93A9` / JP `$00:93D1` |
| `$7E:1AE4` onward | 10 per box | Five words: left `16*x0-4`, width `16*(x1-x0)+24`, top `16*y0-16`, height `16*(y1-y0)+48`, zero-extended damage/flag byte. Coordinates wrap to16 bits; not unpadded tile bounds |
| US `$7E:00ED` / JP `$7E:00F0` | 1 | Nonzero skips the damage-box pass. Player pointer `$8A`, player flags `+$30 & $2058` also gate entry. Contact is hot-point-based; top extent `+$0C <25` subtracts16 from effective height. Overlapping boxes all execute in stream order |
| `$7E:05A0` | 256 | Metatile quadrant attributes, indexed through chunked `$7E:8000` map by US `$00:91C3` / JP `$00:91CF`; tile width/height `$84/$86`, chunk columns byte `$2F`, input tile X/Y `$14/$16`. X out of bounds returns `$0F`, Y out of bounds zero |

Ordinary box damage subtracts HP with zero saturation and sets player hit
flag `$0008`; it does not use Special-mode enemy stat promotion. Damage-byte
bit `$80` instead sets player flag `$8000` without subtracting HP. US
`$00:8EDE` / JP `$00:8EEA`, within the animation decoder, consumes it only
when X equals player pointer `$8A`: clear the flag and halve the newly
decoded signed X velocity at `+$06`, rounding toward negative infinity.
The animation terminator and non-player actors leave it untouched. Without
the flag, US `$F2` / JP `$F5` equal to 1 adds actor `+$3E` instead; slowdown
takes precedence. [Native tests](regional-differences-technical.md#shared-player-slowing-zones)
cover all eleven authored boxes. Do not assign this meaning to every
unrelated actor's use of bit `$8000`.

### OAM shadow + sprite-build working vars

The 544-byte shadow and DMA are common, but action (`$00:8C98/$00:8D68`)
and town (`$01:ACD9/$01:ADAD/$01:AE6F`) rebuild it independently.

The action-effect gameplay-pass serial derived from completed `$00:8C98`
calls has **no WRAM address**. It is game-thread-only host observer state,
excluded from savestates; reset/load invalidates the delta consumer, whose next
capture seeds from the current serial. Do not alias it to `$0088` or the
emulator frame counter: both continue advancing during ActRaiser's native pause.

| Address | Size | Description |
|---------|------|-------------|
| $7E:0380 | 512 | OAM shadow: 128 x 4-byte entries (x, y-1, tile, attr); cleared to x=$80,y=$E0 each frame via a stack-push fill. Both details are load-bearing for the diorama vertical band: the y field is 8 bits mod 256 against 224 lines, so the `$E0` park value IS screen -32 and collides with genuine above-screen positions, and a sprite near the screen BOTTOM aliases into the band through the same wrap. `PpuSetObjExactPosition` carries the emitter's un-truncated x AND y beside this table |
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

**The 7-byte sprite-definition part** the builder walks (ROM, at bank obj+`$18`
/ pointer obj+`$20`+5, after a 4-byte header plus a 1-byte part count):

| Offset | Size | Field |
|---:|---:|---|
| +0 | 1 | flags — **bit 0 is the OAM size bit**, ROR'd into the high-table accumulator alongside x bit 8 |
| +1 | 2 | x offsets: low byte = unflipped, high byte = used when the composition is H-flipped |
| +3 | 2 | y offsets: low byte = unflipped, high byte = used when V-flipped |
| +5 | 2 | tile + attribute word, XORed with the composition's flip word and OR'd with the `$008F` attr bias |

Both offset pairs carry BOTH orientations rather than being negated at runtime,
which is why an H-flipped part is not simply mirrored about the object origin.
Bit 0 of +0 is the only route to a part's pixel SIZE: it selects between the two
entries `OBSEL` picks, so a host that builds a part WITHOUT an OAM slot (the OBJ
apron channel) must read it here and resolve through
`PpuObjSizeForSizeBit` — there is no size information in the tile/attr word.

### Action objects and magic cohorts (action mode only)

| Address / field | Size | Description |
|---|---:|---|
| `$7E:06A0-$1A9F` | 80 × `$40` | Action object slots. Magic cohort slots are `$06A0-$0820`; cast controller is `$0860`; player is `$08A0` |
| `$7E:02D0-$02E0` | 17 | **US PRNG state pool.** `$00:84C0` advances it (a carry-chain `ADC` down the pool, then a multi-byte counter increment) and returns the byte at `$02D1` in A. Every randomized spell decision goes through it — e.g. Magical Stardust's launch site picks top-vs-right edge and its Y offset from one call (`$00:A0E8`). JP `$00:84BA` uses the relocated pool `$02CF-$02DF` and returns `$02D0`; do not seed/read JP at US offsets in regional fixtures. |
| `$7E:08A2/$08A4` | 2+2 | Player object world X/Y (`$08A0 + $02/+04`). The arrival gate uses initialized X `$08A2` to reconstruct the native horizontal activation camera before `$97A6` transfers camera-subject ownership through `$8A`; drawing uses horizontally fitted `$22` and native vertical `$24`. |
| `$7E:08B2` | 2 | Player primary handler (`$08A0 + $12`). Action entry advances `$97A6 → $97C9 → $97E4`; `$97E4` installs `$9832`, the first handler that reads held input. This lifecycle gates only extra horizontal activation, never widescreen drawing or camera presentation. |
| slot `+00` | 2 | Status. `$4000/$8000` high states are inactive/free; spell actors normally use active values 0 or `$0800` |
| slot `+02/+04` | 2+2 | World-space hot-point X/Y, updated by the spell handler and projected with camera `$22/$24` |
| slot `+06/+08` | 2+2 | Current per-tick X/Y velocity decoded by `$00:8E2F`; flip bits mirror the authored deltas |
| slot `+0A/+0C/+0E/+10` | 2 each | Current composition collision-header words after flip selection, in left/top/right/bottom order. The four source header bytes instead use left/right/top/bottom. Most actors use positive distances, but the reader sign-extends bytes and the sword beam retains signed offsets (`$FFE0` occurs live); decode authored parts for presentation geometry instead of assuming this is always an unsigned rectangle. Spawn subtracts the decoded bottom extent from authored Y, so header changes can also change actor placement. |
| slot `+12` | 2 | Primary per-frame handler dispatched by `$00:8915`. This is lifecycle identity as well as control flow: player `$08B2` uses `$97A6/$97C9/$97E4` for arrival and hands control to `$9832`; Bloodpool fireball flight uses `$BDF0`; a live lightning bolt transitions from `$BD36` to shared repeat handler `$8683` without becoming a new actor. Marahna's `$E047` orb/split fireballs, `$DE96` snake projectiles, and `$E483` boss electrical children use `$8661`; after the boss launches its ground charge the parent uses shared repeat handler `$8683`. The snake parent cycles through `$8661/$DF3E/$DF63`, while linked-lightning endpoints/children use `$8683`. Aitos lava fireballs use `$CFE3/$8661/$CFFE` for rise/wait/return states; its separate launched molten rocks use shared `$8661`. Minotaur axes and Ice Dragon ice balls use shared delay handler `$8661`. Flaming Wheel's visible body moves among delay, repeat, and boss-AI handlers, so its handler is deliberately not identity. Tanzara's admitted projectile tuples use `$8661`. |
| slot `+14` | 2 | Secondary handler or polymorphic spawn parameter. Do not treat it as a handler without validating the object type. |
| slot `+16..+18` | 2+**1** | Animation-table pointer: 16-bit address at `+16` then a **single BANK BYTE at `+18`** (`$07:C000` Fire/Stardust, `$07:C800` Aura/Light). **`+19` is a SEPARATE field, not the pointer's high half** — live Magical Fire reads `$3907` as a word there, so a 16-bit read of `+18` silently yields `bank \| next<<8`. Corroborated by `$00:95F0`, which copies spawn-record bytes `+2/+3` into `$18`/`$28` as BYTES. |
| slot `+1A/+1C` | 2+2 | Animation state and entry index |
| slot `+1E` | 2 | Nested-dispatch resume value. Yield helpers store the JSR return address, so the next executed instruction is `value+1`: live fireball `$BDD9` resumes at `$BDDA`; live lightning `$BD69` resumes at `$BD6A`; Marahna's large orb/split and snake children retain `$E061/$A65D`, while its boss diagonal/ground children retain `$E578/$E57E`; the post-impact boss parent repeats through `$E4D7`. Aitos lava fireballs retain `$CFCD`; launched `$CEEC` molten rocks retain `$CF16`, distinct from stationary `$CF1C` mouths. Flaming Wheel's cyan shots use shared `$A65D`; Minotaur axes retain `$B008`, Ice Dragon balls retain `$F2CA`, and Tanzara's exact admitted families retain `$FBEA/$FBF5/$FC13/$FC21/$FCA1/$FCAF/$FCB5/$FCD6/$FCED/$FCFB/$FD22/$FD44/$FD77/$FD9E`. |
| slot `+20/+22/+24` | 2 each | Current composition pointer, visual ID, and animation wait counter |
| slot `+28`/`+29` | 1+1 (see note) | Attribute/transform. Masking the 16-bit read at `+28` with `$C000` selects the horizontal/vertical flip, which works because the bits live in the **byte at `+29`**; `+28`'s own byte measured `$00` on every spell actor observed, and `$00:95F0` writes `+28` byte-wise. Treat as two bytes rather than one word until a case is found that needs the low half. `+19` carries the same base attribute value as `+29`. |
| slot `+2A/+2C/+2E` | 2 each | Attack, HP, and BCD death-score value copied from spawn-record bytes `+7/+8/+9` by `$00:95F0`. Death-score units are tens of displayed points: Aitos skull `$20` means 200 points, not20. |
| slot `+30` | 2 | Object flags. Bit `$0001` marks an attacker (including the player sword beam); bit `$0400` means outside the **currently selected activation window**, not necessarily outside the draw window or native viewport. With extended activation enabled it covers fitted camera `$22` plus live horizontal margins; during `$08B2=$97A6/$97C9/$97E4` it uses the reconstructed native 256px horizontal camera and authentic vertical `$24`. Object drawing is decided independently and may remain visible in the margins while `$0400` is set. The scene-effect observer keeps lifecycle identity but does not submit the object while the bit is set. |
| slot `+32` | 2 | Source/spawn-record pointer retained by ordinary action actors. Bloodpool trap lightning uses `$BD2A`, boss-lightning children use `$BDFF`, and the two fireball directions use `$BD76` and `$BD84`. Marahna orb/split fireballs retain `$E047`, snake enemies/projectiles retain `$DE96`, and the excluded reaper/orb family retains `$E0BA`; `$E2F3/$E304/$E315/$E326/$E351/$E368` are moving-platform roots. Its linked-lightning source endpoint/child retain `$E18E`, the partner retains `$E254`, and the boss electrical family retains `$E483`. Aitos lava fireballs retain `$CF9E` throughout their cyclic rise/wait/return phases; launched molten rocks and their stationary mouths retain the distinct `$CEEC` source. The original/Death Heim boss-family pairs are Minotaur `$AF5D/$F6CA`, Wizard `$BDFF/$F6E2`, Flaming Wheel `$D838/$F712`, Viper `$E483/$F72A`, and Ice Dragon `$F161/$F760`; Tanzara uses `$F80F`. Player sword-beam captures observed `$979A` and `$9810`; validate equality with the linked player's current source instead of hardcoding either. This is a useful slot-reuse discriminator, not a globally unique actor ID. |
| slot `+38` | 2 | Role-specific counter/flag. Controller `$0860+38` is selected spell ID; cohort spells reuse `+38` for repeat counts. Original/rematch Pharaoh root uses it as a pending-sphere flag: successful allocation increments it, the child clears it through `+$3A` after formation, and the root checks between waiting sequences. It is not a shared boss timer. |
| slot `+3A` | 2 | Spawner backlink. The cast controller and player sword-beam child point to player `$08A0`; Bloodpool boss-lightning strike child `$08E0` points to boss `$12E0`, while its floor child `$0920` points to `$08E0`. Marahna split fireballs point to their retired `$E047` orb, snake fireballs point to their validated `$DE96` parent, linked-lightning children point to the first `$E18E` endpoint while the `$E254` partner occupies the next slot, and both `$E483` boss bolt stages point to boss `$12E0`. Death Heim's room owner is `$001C`; the Viper parent and the visible Flaming Wheel body retain it in their rematches. Minotaur axes and Ice Dragon balls instead point to a live parent with the same original/rematch source. The original Flaming Wheel body is root-owned (`0`); helper/child records have action-object backlinks and are rejected. Combined with `+32`, this validates linked families and remains stable while other control-flow fields change. |
| `$7E:00F4/$00F8/$00F9` | 2 each | Input-enable mask, cast-active gate, and cast-transition state used by `$9DE1-$9F10` |

Regional Pharaoh fixtures distinguish original source US `$C1A2` / JP `$C239`
in raw room `$0603` from rematch `$F6FA/$F779` in `$0407`.
The HP-owning root, health helper, sphere, converted wall head and
arrows retain that source. The sphere/head backlink identifies the root;
arrows instead link to the head. Reinitializing a sphere from an ordinary
wall-head record changes its fields but **does not replace `+$32`**. Native
slot reuse after US head retirement requires generation-aware identity.
An arrow can outlive its head, so a live parent is not required throughout
its flight. The root's backlink is0 originally and `$001C` in the rematch;
`+$38` clears after child formation, not after head or arrow retirement.
See the [Pharaoh rematch family contract](regional-differences-technical.md#pharaoh-death-heim-rematch).

Regional Aitos fixtures add platform-skull source US `$D382` / JP `$D404`:
`+$30 & $0800` deflects a confirmed sword contact before HP subtraction,
distinct from the early `$0020` victim filter. Both skulls start with `+$2C=0`;
US death score `+$2E=$20` awards20 BCD points, JP stores0. Proximity is measured
from the live hot point; spawn Y subtracts the composition bottom extent12.

Volcano-fireball source US `$CF9E` / JP `$D01E` uses `+$38=48` as an
eight-frame-row counter during its4px/update rise. `+$3C/$3D` supply the
subframe/reload pair, initialized as word `$0200`; these are not another
48-frame timer. Reinitialization uses authored X/Y at `+$34/+$36`.
All six phase-aligned rise/return/reset paths match in both regions and modes.
See [Aitos collision and timing](regional-differences-technical.md#aitos-act-1-platform-skulls-and-volcano-fireballs).

The Aitos bamboo trap is a separate source US `$CF2E` / JP `$CFB3`, type
`$06` in raw room `$0104`. Its initial `+$1A=38` and bottom extent32 put
the first shared trap's hot point at `(400,528)`. The entry requires
`+$30 & $0400` clear and `abs(cachedPlayerX - trapX) < 32`; it does not
check Y. States37/38 partition the same219-update movement differently:
27/192 US,43/176 JP. State numbers alone must not select a regional speed.
See [trap placement and phase contract](regional-differences-technical.md#aitos-bamboo-spike-traps).

Antlion source US `$C66F` / JP `$C6FE` in raw room `$0203` uses cached
player X `$7E:0080` for its encounter threshold2432US/2304JP. Its
post-volley distance decision uses absolute X<64 for the next phase;
equality64 repeats firing. US state12 consumes36 animation updates before
the decision. JP instead decides immediately and, on the far branch,
stores native-delay `+$24=60`, yielding61 updates. That delay and the
animation row timer reuse a field but have different continuations; do not
convert an in-flight wait just because a regional option changes. Source,
handler and saved resume together distinguish them. See the
[Antlion decision contract](regional-differences-technical.md#antlion-trigger-and-post-volley-decision).

First-act boss roots retain sources US/JP `$AD45/$ADD9` (Centaur),
`$B786/$B81A` (Bloodpool), `$D646/$D6C8` (Aitos dragon) and
`$E7C6/$E845` (Northwall). Together with Antlion they have HP24,
attack1 normal/2 Special, and boss flag `$4000`. Source alone also matches
their children: validate role/backlink before treating a slot as the root.
Phase comparisons use active-update counter `$88`; repeated video frames
with an unchanged counter are not additional animation updates. The
[paired first-act observations](regional-differences-technical.md#first-act-boss-program-comparison)
find no regional attack-program or animation-data change for these four.

Regional Minotaur fixtures distinguish original source US `$AF5D` / JP `$AFF1`
from rematch `$F6CA/$F749`. The root's `+$38` latches player X before ascent;
it is not a countdown. Root `+$3A` is0 originally and `$001C` in the rematch;
the axe instead links to the live root, inherits its source/attack, and has
HP/score0. Encounter identity is required for animation metadata.
See [Minotaur phases and family contract](regional-differences-technical.md#minotaur-timing-and-room-inheritance).

The Wizard's `+$2C` health check selects the second form below 12 HP; its
US-only 31-update post-spread wait is separate from animation `+$24` and
does not imply that all first-form sequences are slower. Viper's regional
lightning choice consumes one RNG result: low two bits zero in US, low bit
zero in JP. Neither is a global Special-mode multiplier.

Final-boss source US `$F80F` / JP `$F88E` spans both forms and their parts.
Second-form projectile `$FD25/$FDA2` overwrites `+$2A` with 3/4 attack even
in Special; it does not overwrite HP. First-form states 5/10 leave victim
filter `$20` clear; state 10's final row adds 28 US frames of vulnerability.
State 48 belongs to the upper body, not a projectile; its JP first row adds
one moving frame. See [final-boss contracts](regional-differences-technical.md#tanzra-forms-timer-and-projectile-strength).

| Action clock field (both regions) | Size | Native meaning |
| --- | ---: | --- |
| `$7E:00E5` | 1 | Timer divider; decremented only with `$E8=0`, reloads to 59 on underflow. |
| `$7E:00E6/$00E7` | 2 | Packed-BCD stage countdown, saturates at zero; not binary seconds. |
| `$7E:00E8` | 1-byte timer read | Nonzero stops timer service. Generic boss-death handling increments the word at `$E8`; US final second-form entry clears it, JP preserves it. Preserve each caller's access width. A policy toggle must not refill or retrospectively restart the clock. |

Action-scene identities measured in runs `20260810-124203`, `20260810-163044`,
`20260810-174202`, the six-cycle correction run `20260810-180202`, and sword-beam run
`20260810-175403`, `20260810-184935`, `20260810-190012`, and
`20260810-190729`, plus Marahna run `20260811-151353` and Death Heim runs
`20260822-195453`/`20260822-195726`, combine those fields rather than matching
artwork alone:

Regional Marahna Act-1 fixtures add source US `$D974` / JP `$D9F6` in room
`$0305`: the HP-owning root `$12E0` has parent0; linked `$1320/$1360` and
health/flash helpers share its source. JP sets root flags `+$30` bit `$0020`
during state20, causing the normal collision victim filter to reject damage.
The US root stays in state2. This is a per-encounter behavior contract, not a
global meaning for animation state20 or every record with that source.
See [regional boss ownership](regional-differences-technical.md#marahna-act-1-boss-vulnerability-and-placement).

Northwall Act-2 / Death Heim Ice Dragon fixtures (`$0806/$0707`) use source
US `$F161/$F760`, JP `$F1E0/$F7DF`. The HP-owning root was `$1320`, linked
body `$13A0`. During the wind-up, root `+$1A=$12` and body `+$1A=$11`;
root `+$3A` is branch0/1, while the body `+$3A=$1320` is a real backlink.
Do not require parent0 to recognize this root or treat every `+$3A` as an
address. Slots are observations, not stable allocation IDs. The JP rematch's
two shorter sequences affect `+$1C/+$24` row/timer progression and the time
spent at each composition's bounds; the visual composition records themselves
match. See [regional timing contract](regional-differences-technical.md#northwall-act-2-boss-original-versus-death-heim).

Fillmore Act-1 tree head source US `$A934` / JP `$A8F3` owns HP3. Its separately
authored next-slot peer source US `$A9B3` / JP `$A97E` has HP0. JP head `+$78`
increments that peer's `+$38` request; peer clears it after its two allocation
attempts. Seed children retain the peer source and a `+$3A` backlink; their
visual children link to the seed. Source equality alone does not distinguish
controller, seed and visuals. Slots observed after controlled room reload:
US head/peer `$0F60/$0FA0`, JP `$1020/$1060`; these are not universal IDs.
See [tree timing/ownership](regional-differences-technical.md#fillmore-act-1-tree-seed-controller-and-pre-shot-wait).

| Kind | Positive live identity |
|---|---|
| Enemy fireball (`$18=$02`, `$19=$02-$08`) | `+32=$BD76/$BD84`, `+12=$BDF0`, `+1E=$BDD9`, `+16/+18=$4000/$7E`, `+1A=$23`, and `(+22,+20)=($17,$45EF)` or `($18,$4610)`. The full Bloodpool Act-2 range is valid because its ordinary-enemy blob is shared across all seven rooms. |
| Bloodpool statue / startup child (US/JP) | Types `$26/$1E` sources US `$BD76/$BD84`, JP `$BE0A/$BE18` identify a **family**, not just a projectile. Parents play `$0E/$0F`, HP3; children retain source/facing/attack with HP0 and `+$3A` parent backlink, start `$21` then fly `$23`. JP's second `$0F` uses a different saved resume, so state-only logs miss that phase. Flight culls on `$0400` only at the two-row sequence boundary. Allocation exhaustion scratch `$1AA2` is outside the live pool and must not become an actor identity. See [regional contract](regional-differences-technical.md#bloodpool-act-2-statues-single-versus-double-volley). |
| Lightning trap (`$18=$02`, `$19=$02-$08`) | `+32=$BD2A`, `+1E=$BD69`, `+16/+18=$4000/$7E`, `+1A=$14`, `+12=$BD36` or `$8683`, and `(+22,+20)=($1F,$46FE)` or `($20,$479D)`; the live vertical extents are `+0C=+10=$58` (88px each side). Identical records outside Bloodpool Act 2 are rejected. |
| Marahna fireball (`$18=$05`, `$19=$04-$07`) | `+32=$E047`, `+12=$8661`, `+16/+18=$4000/$7E`. The state-`$0C` orb cycle uses exact visual/composition/velocity tuples `$07/$451C/(0,0)`, `$08/$4528/(-1,0)` or `(-2,0)`, `$05/$4504/(0,0)`, and `$06/$4510/(+1,0)` or `(+2,0)`, all unflipped with extents `8/8/8/8` and resume `$E061`. Four split children point `+$3A` to the inactive parent in resume/state/visual/composition `$E0A6/$0E/$0C/$4597`, resume at `$A65D`, use extents `4/4/4/4`, and carry exact velocity/state/visual/composition/flip tuples `(0,+3)/$0F/$32/$4BCD/0`, `(-3,0)/$10/$33/$4BD9/0`, `(0,-3)/$0F/$32/$4BCD/V`, or `(+3,0)/$10/$33/$4BD9/H`. Source+resume plus bounded position continuity distinguishes same-slot reuse. `$34/$4BE5` is excluded moving-platform artwork. |
| Marahna snake fireball (`$18=$05`, `$19=$04-$07`) | Source `$DE96`, handler/resume/state `$8661/$A65D/$06`, animation `$7E:4000`, extents `8/4/8/4`, local counter `+38=6`, and exact pairs `$1D/$4869` or `$1E/$487C`. Velocity is `(-4,0)` unflipped or `(+4,0)` H-flipped. `+3A` must resolve to an active source-`$DE96` snake with matching H-flip, extents `16/24/16/24`, and exact wait/rise/fall lifecycle. Run `20260811-232640` proves the source-`$E0BA` reaper orb is a negative case. |
| Marahna lightning link (`$18=$05`, `$19=$04-$07`) | Child `+12=$8683`, `+32=$E18E`, `+1E=$E24F`, animation `$7E:4000`, no flip. Horizontal is state/visual/composition `$27/$2E/$4AA1`, extents `40/4/40/4`; vertical is `$28/$31/$4B82`, extents `5/40/5/40`. `+3A` must resolve to an active `$E18E` endpoint in state `$1A`, with an active `$E254` state-`$1D` partner in the next slot; endpoint visual/composition pairs are `$0D/$45B8` + `$0F/$45D0` horizontally and `$0E/$45C4` + `$10/$45DC` vertically. The child hot point must be their exact midpoint. |
| Marahna boss lightning (`$18/$19=$05/$08`) | Boss parent `+32=$E483`, animation `$7E:5000`, extents `48/40/48/8`, no backlink. With handler `$8661`, charge artwork is `$07/$57C2` or `$08/$5868`; the orb is `$0A/$59DE`. Launched child `+$3A=$12E0` uses resume/state/visual/composition `$E578/$04/$11/$5CE0`, velocity `(-4,+4)` with extents `32/0/0/32` and no flip or `(+4,+4)` with `0/0/32/32` and H-flip. After impact, the same child resumes at `$E57E`, state `$07`, and rides the floor at `(-4,0)` unflipped or `(+4,0)` H-flipped. Its complete loaded cycle is `$12/$5D01` with `8/8/8/8` extents, then `$13/$5D0D`, `$14/$5D2E`, `$13/$5D0D` with `16/16/16/16`. During that stage the backlink parent is the exact shared-repeat tuple handler/state/resume/visual/composition `$8683/$0A/$E4D7/$00/$5307`. |
| Aitos lava fireball (`$18/$19=$04/$01`) | `+32=$CF9E`, `+1E=$CFCD`, animation `$7E:4000`, no flip, extents `8/8/8/8`, and `(+22,+20)=($2A,$4D21)` or `($2B,$4D2D)`. Rising state `$22` uses `+12=$CFE3`, velocity `(0,-4)`; wait/reset state `$23` uses `$8661`, `(0,0)`; return state `$24` uses `$CFFE`, `(-1,+6)`. Source+resume plus bounded position continuity starts a fresh generation when a persistent slot relaunches at the pit. |
| Aitos statue fire (`$18/$19=$04/$06`) | The two facing spawn/graphics records retain `+32=$D5B1/$D5C0`, animation `$7E:4000`, zero velocity/backlink, and no flip/H-flip respectively. Active state `$18` grows the pillar through exact visual/composition `$1C/$4763`, `$1D/$4776`, and `$1E/$4790`; state `$19` sustains it with `$1F/$47B1` and `$1E/$4790`. State `$1A` is the long inactive `$17/$46FD` hold and is deliberately undecorated. In the vertically extended Diorama band these timed hazards may be drawable while authentic-height activation still sets `$0400`; only this exact room/source/animation tuple inherits the enabled vertical draw window for activation. Active effects follow `$008F` OBJ priority after identity is established. |
| Aitos molten rock (`$18/$19=$04/$01`) | `+32=$CEEC`, `+1E=$CF16`, `+12=$8661`, state `$27`, animation `$7E:4000`, artwork `$2B/$4D2D`, extents `8/8/8/8`; X velocity is `-2` unflipped or `+2` H-flipped and measured Y is `-1..+1`. Stationary mouths use resume `$CF1C` and are excluded. |
| Boss lightning (`$18/$19=$02/$08`) | Base identity `+32=$BDFF`, `+12=$8661`, `+16/+18=$5000/$7E`, no V-flip, and `+3A` resolving to an active `$BDFF/$7E:5000` parent. Strikes: states/visuals/compositions `$02/$00/$5346`, `$03/$01/$5401`, `$04/$02/$5492` are vertical long/medium/short; `$05/$03/$54F2`, `$06/$04/$55C2`, `$07/$05/$5661` are diagonal long/medium/short. Normal left/top/right/bottom extents are `6/83/11/117`, `6/83/11/69`, `1/83/11/21`, `48/83/8/117`, `36/83/8/69`, `30/83/8/21`; H-flip swaps left/right. `$20/$5D2B` is the blank half-cycle and is not decorated. Observed strike resumes `$C02B/$C04B/$C051` are control flow, not shape identity. Floor impact is state/resume `$09/$C06A`, pairs `$08/$570A`, `$09/$5716`, or `$0A/$5729`. |
| Death Heim Wizard lightning (`$18/$19=$07/$03`) | Same exact `$7E:5000` child states, artwork, extents, handler, resumes, and parent-validation contract as Bloodpool, with the owning source changed consistently from `$BDFF` to `$F6E2`. The room gate accepts both the original and rematch pair; mixed-room or mixed-source tuples fail closed. |
| Minotaur axe (`$01/$04` or `$07/$02`) | Original/rematch source `$AF5D/$F6CA`, handler/resume `$8661/$B008`, animation `$7E:5000`, state `$03`, backlink to a live same-source parent. Original visuals `$00-$07` use `$50FD/$513A/$515B/$5198/$51B9/$51F6/$5217/$5254`, alternating with tiny visual/composition `$11/$5A9E`. Rematch uses `$50FB/$5138/$5159/$5196/$51B7/$51F4/$5215/$5252`, alternating with `$10/$59B0`. Tiny frames have4px extents and occur every other update, not only at sequence end. Original speed alternates3/1, rematch4/2 pixels/update; source-specific metadata is required. |
| Flaming Wheel (`$04/$07` or `$07/$05`) | Visible body uses source `$D838` with root backlink `0`, or `$F712` with Death Heim room-owner backlink `$001C`; both require active boss flag `$4000`, animation `$7E:5000`, and nonzero composition. Handler and priority are intentionally excluded from identity because the same body moves among handlers and the room supplies its OBJ band through `$008F`. Same-source helpers carry action-object backlinks and fail closed. Full-ring compositions `$5276/$5398/$54BA/$55DC` place twelve authored fireballs at local centres `x/y=-24,-8,8,24` with the four interior corners omitted. |
| Flaming Wheel cyan shot (`$04/$07` or `$07/$05`) | Same room/source pair and a backlink to its active same-source root, handler/resume `$8661/$A65D`, animation `$7E:5000`, index 1, flags `$0020`, and 8px extents. States `$08-$0C` use local counter equal to state, velocities `(-1,+1)/(0,+1)/(+1,+1)/(-1,0)/(+1,0)`, and exact visuals/compositions `$00/$51B5`, `$01/$51C1`, `$02/$51CD`, or `$03/$51D9`. All five simultaneous children are independent effect records. Their raw part priority is zero, so presentation inherits bits 12-13 of live `$008F`; original Aitos measured priority 2, while Death Heim remains free to select a different band. |
| Death Heim Viper lightning (`$18/$19=$07/$06`) | Same charge/orb/bolt/ground tuples as Marahna `$05/$08`, with source `$F72A` replacing `$E483` consistently on parent and children. The rematch parent retains backlink `$001C`; diagonal and floor children retain their normal parent link. |
| Ice Dragon ball (`$06/$08` or `$07/$07`) | Original/rematch source `$F161/$F760`, handler/resume `$8661/$F2CA`, animation `$7E:5000`, and backlink to a live same-source parent. Visuals `$12-$15` are state `$19` with compositions `$5D9C/$5DA8/$5DB4/$5DC0`; visuals `$16-$19` are state `$1A` with `$5DCC/$5DD8/$5DE4/$5DF0`. |
| Tanzara projectile (`$07/$08`) | Source `$F80F`, handler `$8661`, animation `$7E:5000`, and an exact allowlist of 50 resume/state/visual/composition tuples covering the observed projectile families. The tuple table is authoritative in `src/action/action_effects.c` and its regression fixture; unlisted boss-body or helper artwork is rejected. |
| Wall torch | Not an action slot. Bloodpool uses exact BG1 pair `$47` over `$4F` throughout `$18=$02`, anchored at `(8,15)` in the 16×32 pair. Marahna maps `$05/$04-$08` use one complete `$43` metatile anchored at `(8,11)` in its 16×16 cell; Death Heim Viper room `$07/$06` reuses that same authored `$43` rule. All use the shared bounded map view; Marahna limits publication to a 256px camera margin because `$04-$07` share 31 torches; original boss map `$05/$08` has ten in its separate 512×512 map. |
| Aitos lava pit (`$18/$19=$04/$01`) | Not an action slot. Exact BG1 signature is `$DC`, one-to-six `$DD`, then `$DE`, over equally wide `$DF` and (when map height permits) `$E7` bubbly rows. Observed 64px rims begin at world `(1648,976)`, `(1888,992)`, and `(2144,976)`; the 128px rim begins at `(3616,928)`. Capture publishes the full bubbly volume on BG1 within a 256px camera margin. |
| Aitos Act-2 side lava (`$18=$04`, `$19=$04-$06`) | Not an action slot. A maximal three-to-63-cell `$01` BG1 lip sits above animated/transparent `$02-$04` cells (`$77` also occurs in map `$06`) and `$05` lava body, bounded by measured bank pairs `$33/$34`, `$2C/$32`, or `$33/$32`. Capture can walk left beyond the bounded scan to recover the bank of map `$06`'s 640px reservoir, then publishes one broad world-overlay emitter. |
| Aitos waterfall platform (`$18=$04`, `$19=$02-$03`) | Not an action slot. Exact 2-to-8-cell BG1 signature is top `$36/$5E*/$81`, body `$4E/$F4*/$4F`, drip `$F6/$FC*/$FE`. Camera-local presence admits platform spray/drips, one BG2 waterfall veil, and its paired after-BG2 Diorama bottom-mist record; absence rejects the shared `$04/$02` cave section. The mist adds no emulated state or signature. |
| Player sword beam (all action maps) | `+12=$9D1C`, `+16/+18=$8000/$06`, `+30&$0001`, `+3A=$08A0`, nonzero `+32` equal to the active linked player's source, no V-flip, and state/visual/composition `$13/$30/$99E8` or `$14/$31/$9A17`. Measured velocity is `+8` or `-8`. State `$13` normal/H-flip drawable bounds are `(32,-33)..(48,-1)` / `(-48,-33)..(-32,-1)`; state `$14` bounds are `(40,-9)..(56,23)` / `(-56,-9)..(-40,23)`. Run `20260810-184935` proves the state-`$13` normal rectangle byte-for-byte against captured OAM. Raw collision words include signed byte origins and are retained only for diagnostics/gameplay fidelity. |
| Aitos boss sword volley (`$18/$19=$04/$03`) | Source `$D646` emits two `$7E:5000` crescent children linked through an inactive state-`$00` controller. The controller is exact resume/visual/composition `$D793/$23/$56FE`, flags/local counter `$0020/$000D`, 8px extents, and `+3A` linking the active `$D646` boss root. Both children use handler/resume `$8661/$A65D`, animation index 1, flags `$0020`, and OBJ priority 2. Normal lower state/visual/composition/local-counter/velocity is `$01/$21/$56D8/$01/(-3,+1)` with L/T/R/B `8/16/16/8`; upper is `$02/$20/$56BE/$02/(-3,-1)` with `8/8/16/16`. Their exact drawable rectangles including `$8D68`'s Y bias are `(-8,-17)..(16,7)` and `(-8,-9)..(16,15)`. The reflected facing requires matching controller/child H+V flip `$C000`, reverses both velocity components, swaps L↔R and T↔B, and produces rectangles `(-16,-9)..(8,15)` / `(-16,-17)..(8,7)`. Run `20260812-000613` snapshot 5 proves the normal pair; run `20260812-224123` snapshot 1 proves reflected state 1 byte-for-byte against OAM `(202,33)..(226,57)`. |

### Town simulation render records and camera auxiliaries

| Address | Size | Description |
|---------|------|-------------|
| $7E:06A0-$09FF | 48 × $12 | Fixed-screen/overlay animation records. `$01:ACD9` tests `+10 & $8000`, runs `$01:AC70`, and emits with camera-independent origins. |
| $7E:0A00+ | 44 × $26 | Town world-object records. Known render fields: `+08` frame-composition pointer, `+0A/+0C` world X/Y, `+10` render status (`$C000` = skip), `+25` delay/timer. `+12` is a behavior dispatch selector outside the OAM leaf. |
| $7E:0A00+`+0E` | 2 | World-record **class**, indexing the `$01:B8D0` dispatch: `$0C` angel, `$11` town position controller, `$12` Blue Dragon, `$13` Napper Bat, `$14` Red Demon, `$15` Skull Head. Slots are recycled, so a class can change under a stable composition. |
| $7E:0A00+`+0E` | 2 | **Packed form.** Scripted town actors additionally carry their spawn list in the high byte over class `$01` in the low byte, so the field reads as a 16-bit identity rather than a small class index: `$0A01` burning house, `$0E01` volcanic eruption. Runs `20260818-070141`/`073455` see `$0E01` on all eight live eruption records and on nothing else. Effect classifiers gate on the packed word **and** an exact composition, so neither the list nor the art can claim a family alone. |
| $7E:0A00+`+00` | 2 | **Animation frame timer** for the record's `+$02` script cursor. Decrements once per game frame (measured Δ of -18/-14/-12 across snapshot gaps of 18/14/12) and cycles `+1..+4` on the eruption ground fire's authored four-tick frames. A held (`0`-duration) frame lets it free-run negative, which is why a staged record reaches -70. **It is not an altitude** — the sim town has none, and drawn position is exactly `world - camera`. |
| $7E:0A00+`+1A/+1C` | 2+2 | **Per-tick map velocity**, X and Y, applied by the record's own class handler; `$01:B44B` is the angel-arrow case. For the volcanic eruption `+1C` names which of the ROM's three phases a record is in: `-8` climbing out of the crater, `+8` falling back onto the town, `0` staged offscreen. Composition follows it exactly — `$E7A6` always reads `+8` and `$E7D0` never does. One record walks all three in turn, so this is a state machine rather than two populations. Recorded here as measurement only: the eruption presentation is keyed on the script's own clock and on which column the record stands in, so nothing in the tree currently reads this field. |
| $7E:0A00+`+22` | 2 | **Wait counter**, written by actor-script command `$09` (`$01:CE5F`): the command fetches two script bytes, assembles a 16-bit value, `STA $0022,X`, and sets state 2. Decrements once per game frame alongside `+00`. Observed 1..76 on staged eruption records — and the script that drives record `$0FA4` opens `09 4C 00`, i.e. wait $004C = 76, exactly the value seen. It does not encode the landing row; the `$03` run that follows does. **It is the only live source for a wait already in progress**, because `$01:CE5F` advances the cursor past the `$09` before the countdown starts: walking the script from the cursor sees no wait at all, so a wait's remaining frames exist nowhere else. The eruption presentation reads it for exactly that reason. |
| $7E:0A00+`+14/+16` | 2+2 | **Actor-script base and cursor.** `$01:CFC7` fetches the next command byte and post-increments `+16`; the bank it fetches from is selected by the class byte `+$0E & $00FF` — **zero reads `$7F:0000,X` (RAM), non-zero reads `$0A:0000,X` (ROM)**. Townspeople are class 0 and run generated RAM scripts; the eruption is class `$01`, so its scripts are **static bank-`$0A` ROM data** and are decodable offline. `$7F` = end of script (`$01:CD35` branches to `$B891`); anything else indexes the 18-entry command table `$01:CD6F`. |
| $7E:0A00+`+1E` | 2 | Per-command step scale/duration written by the command handlers — cmd `$03` sets `$0010`, and one branch of cmd `$04` sets `$0002` after scaling `+1A/+1C` by 8. With cmd `$03`'s `+1C = +1` this yields the measured 16 map pixels of descent per command. |
| $7E:0A00+`+12` | 2 | Masked `& $7FFF`, the **state** index inside that class's own table. `(class $12, state 6)` is the Blue Dragon's 33-frame building strike. sim3d keys presentation height on the `(class, state)` pair. |
| $7E:0A00+`+14/+16/+18/+1E` (class `$15` only) | 2 each | Skull Head state timer / target pixel X / target pixel Y / overloaded interruption countdown and resume marker. State `$0B` uses `+1E` for24 calls, then writes1 and returns to state3; state5 retains remaining `+14` when resuming. These are not the scripted-town-actor cursor fields above. [Native target/timer contract](regional-differences-technical.md#skull-head-target-and-earthquake-state-contract). |
| $7E:0F0C-$1016 | 8 × $26 | The volcanic eruption story event (fires once a town's region has no lairs left), mapped 2026-08-18. All eight records carry `+0E=$0E01` and run one of three consecutive spawn scripts (`$01:A853`/`$A857`/`$A85B` in `+06`). Positive live identity: staged = `+08=$E7D0`, `+1C=0`, `world_y=-16`, `+22` counting down; crater jet = `+08=$E7D0`, `+1C=-8`, fixed map column (144 in Aitos); falling = `+08=$E7A6`, `+1C=+8`, constant column, `world_y` rising 8 a tick from -16; landed = `+08=$DD9F/$DDA5/$DDAB`, `+1C=0`. Measured fall ranges are 80-368 map pixels to fourteen distinct landing rows (64..352, all multiples of 16). |
| $7E:0AE4 | $26 | Angel world record (index 6), class `$0C`. Its class handler `$B904` is a no-op because another subsystem drives it. Identify the angel by this address plus class — the `$A627-$A792` pose compositions are also borrowed by miracle effect records. |
| $7E:0B0A | $26 | Dedicated angel-arrow world record (index 7). `$01:B41A` state-dispatches idle/spawn/move through `$B423`; movement `$B44B` applies velocities `+1A/+1C`, and `$B473` returns carry set when the projectile should be released. |
| $7E:0AEE/$0AF0 | 2+2 | Town camera-follow target X/Y read by `$01:B4C6`; camera derives `$22=$0AEE-$80`, `$24=$0AF0-$70` before clamping. |
| $7F:9752 | 1+ | bit 1 selects town alternate OAM emitter `$01:AE6F` for the world segment. |
| $7F:9754 | 1+ | nonzero reduces the normal 44-record town world scan to one record. |
| $7F:9F65/$9F67 | 2+2 | transient town camera shake X/Y. Applied only if resulting camera remains inside `$22<=$0100`, `$24<=$011F`, then cleared. |

### Upload records + NMI descriptors
| Address | Size | Description |
|---------|------|-------------|
| $7E:0076/$0079 (+banks $78/$7B) | 2+1 ea | NMI record-drain pointers — reset EVERY NMI by $02:ACC8 to $3900/$3A02 then $3B04/$3C06 (game-side reads see the resting values; not a game variable) |
| $7E:3900/$3A02/$3B04/$3C06 | $102 ea | the four one-record upload buffers (BG1 col/row, BG2 col/row): +0 header = VRAM base word (0=empty, zeroed after drain), data = 4x64B chunks at +2/+$42/+$82/+$C2. Column records use VMAIN=$81 and target `base,+1,+$800,+$801` (32 words, stride `$20`); row records use VMAIN=$80 and target `base,+$20,+$400,+$420` (32 contiguous words). |
| $7E:00C4-$00CA | — | Fade gate/config and background page-flip counters: `$C5/$C6` are page masks, `$C7/$C8` current offsets, `$C9/$CA` packed reload/countdown bytes. `$02:BC27` services both pairs. Aitos Act 1 rooms 2/3 use mask `$0C`, counter `$40`: offsets 4/8/12/0 held five calls each. Same checked settings/cycle in all five ROMs; PAL addresses are one higher. [Native profile checks](regional-differences-technical.md#aitos-background-animation-and-video-profiles). |
| $7E:00CB/$00CD/$00CE/$00CF | 2+1+1+1 | CGRAM upload descriptor: src addr/bank, CGADD, row count ($02:AE75) |
| $7E:00D0-$00D6 | 7 | VRAM DMA descriptor slot 0: src16/bank/VMADD/size (size=0 idle; $02:AF30) |
| $7E:00D7-$00DD | 7 | VRAM DMA descriptor slot 1 = tile-anim upload. Action/town `$02:BC56` uses `[$D9]:$D7 = $7F:B800+n*$E1`; world navigation's `$02:AF86` instead fixes bank `$0A`, with `$D7 = $B000/$B040/$B080/$B0C0` for the four water frames. `$D7` remains after `$DC` is drained, so the host-owned map can synchronize phase without reading VRAM. |
| $7E:00DE-$00E1 | 1+1+1+2 | tile-anim: tick period mask / frame count-1 / frame index / frame stride (bytes); $FF/$FF/-/0 = disabled |
| $7E:00F1 | 1 | one-shot flag: re-stream BG3 map rows 4-26 ($7F:B100 -> VRAM $5880) next NMI |
| $7F:B000-$B6BF | 1728 | HUD/BG3 tilemap compose buffer (rows 0-3 streamed every frame to VRAM $5800; rows 4-26 on $F1) |
| $7F:0000-$1FFF | 8192 | **Full town BG1 tilemap**, the whole 64x64-tile (512x512 pixel) town, not just the on-screen window. Quadrant-paged: `$03:9B5A/$03:9C43` write each cell's 2x2 tile block at `quadrant*2048 + (cellY & 15)*128 + (cellX & 15)*4`, four words at `+$00/+$02/+$40/+$42`, using terrain/structure definitions respectively. Both HLE wrappers and bridge-side rendering share `ActRaiser_CopyTownMetatile`. Row stride is 32 tiles, quadrant stride 32x32 tiles. A row-major read looks like an unrelated layer — it was mistaken for BG2 twice before `$9C43` was disassembled. This is the authoritative displayed cell artwork across staged construction and Marahna's water-to-land event; the semantic `$7F:2000` value can lead the visible redraw, so presentation observes this range plus live VRAM/CGRAM rather than reconstructing the image from cell ids |
| $7F:1000-$1FFF | 4096 | (Within the above.) The lower two quadrant pages happen to be the range the graphics orchestrator streams to VRAM; this is part of the same buffer |
| $7E:2100-$28FF | 2048 | Mode-dependent BG1 metatile definitions, 8 bytes (four tilemap words) per ID. In towns, `$03:9B5A` expands this terrain atlas into the live tilemap and `$03:96EF` tests top-left bit `$0200` as its impassable marker. In action rooms, command 5 installs the BG1 rendering/collision definitions here. |
| $7E:2900-$30FF | 2048 | Action BG2 metatile definitions, 8 bytes (four tilemap words) per ID, installed by command 5. Outside action mode this range is shared and must not be treated as persistent BG2 authority. |
| $7F:2000-$37FF | 6144 | **Six town terrain cell maps**, one 32x32-cell, `$400`-byte block per town. Each block is quadrant-paged as four 16x16 pages at +0/+256/+512/+768. Values are semantic terrain ids or temporary/special structure marks: terrain redraws expand `$7E:2100` through `$03:9B5A`, while structure rebuilds expand `$7E:3100` through `$03:9C43`; structure records' `+0/+1` cell X/Y address the active block. During staged animation this semantic value may change before the displayed 2x2 words at `$7F:0000`, so it is not a presentation oracle. `$03:9710` computes its index through the shared `ActRaiser_CellMarkIndex` HLE; `$03:96EF` consumes the indexed terrain ID through the traversal-predicate HLE; `$02:865C` consumes all six blocks when stamping the authentic developed world map, while the host's pure `SimWorldMap_ComposeDeveloped` reads the same bytes explicitly. |
| $7E:3100+ | 2048 | Structure metatile table: 8 bytes (four BG1 tilemap words) per metatile index, consumed by the shared `$03:9C43` metatile-copy HLE and bridge-side renderer. Note the cell value is **not** a direct index — expansion is a write path the game runs on change, and cell → 2x2 block is only ~62-77% single-valued when inverted, so read `$7F:0000` rather than trying to rebuild it |
| $7F:B800-$BFFF | `$1000` | Contiguous 4 KiB character-animation snapshot used by action mode and sim towns (`$18=0`, `$19!=0,9`). During scene entry `$02:BAF5` reads `$1000` bytes of character VRAM beginning at word `$DA` (`$0000` or `$1000`) into this range; `$02:BC56` later selects `$E1`-byte phase `($E0 & $DF)` and `$02:AF30` uploads it back. Raw config cadence bit 7 marks a continuation and makes `$BAF5` retain the prior capture. Only the separate sim `$19=0 or 9` branch uses ROM bank `$0A` directly through `$02:AF86`. |

### Mode 7 / World Map
| Address | Size | Description |
|---------|------|-------------|
| $7E:C000-$FFFF | 16384 | **Shared scratch; world-map tilemap shadow only while `$19=09` is being built/presented.** Row-major 128x128, one byte per tile, and byte-identical to Mode-7 VRAM after `$02:B475` completes. It is not persistent world-map state: action stages durably clobber rows 0-79 and town frames reuse rows 0-7. Host rendering never reads or writes it: `SimWorldMap_ComposeDeveloped` builds a separate complete map from the ROM base and explicit simulation inputs. Static tiles are `$0E:8000`, palette `$1C:BF93`; tiles `$00/$AA` are replaced by the four water frames at `$0A:B000-$B0FF`. |
| $7E:0300-$0303 | 4 | World-navigation focus X/Y in source pixels. At `$02:8213` movement advances these with `$22/$24`; the stable difference is the authentic half-screen `(128,112)` |
| $7E:0304-$030B | 8 | Current signed Mode-7 A/B/C/D matrix uploaded for the displayed `$09` frame |
| $7E:030C-$0313 | 8 | Staged next signed A/B/C/D matrix |
| $7E:0314 | 2 | Scripted world-navigation in-plane rotation; remains active during the action-entry zoom-and-spin |
| $7E:0316 | 2 | Current world-navigation zoom state |
| $7E:0318 | 2 | Target world-navigation zoom state |
| $7E:031A | 2 | **US-only Death Heim emergence latch.** Final-act departure writes `$FFFF`; `$02:8134/$8550/$AFCB` consume it for camera setup, reveal and tile upload. The reveal clears it before returning to the Palace. This is transient presentation state, not the persistent unlock/announcement bits at `$7F:9101`. JP `$031A` instead serves a return/respawn marker; do not alias them. |
| $7E:031C-$032B | 16 | **US-only emergence sprite mask.** Eight words initialize to `$00FF`; `$02:863E` clears one low-byte bit per step according to `$02:902F`. `$02:AFCB` uploads the bytes to VRAM word `$47F0` for repeated OBJ tile `$7F`. Not world-map rows or Mode-7 transform data. JP's song selector at `$0322` overlaps this range. |
| $7E:06D6 | `$12` | During US emergence, fixed-screen record with composition `$01:EDF8`: 64 repeated tile-`$7F` parts in an 8×8 grid. `$02:8601` moves `+0A/+0C` inversely to camera jitter to keep the mask aligned. This slot is not a persistent Death Heim identity in other scenes. |

## Action-room bootstrap background staging (mapped 2026-08-22)

These ranges are the exact resident background image produced by asset-script
commands 5 and 4. The guarded CPU HLE and `ActionRoomScene` staging path both
preserve bytes outside the active ranges, which matters because several of the
addresses are shared scratch in other game modes.

| Address | Size | Action-room meaning |
|---------|------|---------------------|
| $7E:2100-$28FF | 2048 | BG1 metatile definitions: 256 entries × four little-endian tilemap words. Command 5 decompresses through `$7E:6000`, then byte-swaps the ROM words into this table. This range is mode-dependent and serves the town terrain atlas outside action mode. |
| $7E:2900-$30FF | 2048 | BG2 metatile definitions in the same four-word layout. Command 5 owns the full range for an action BG2 load. |
| $7E:6000-$7FFF | 8192 | Shared action graphics workspace. Compressed command-7 character banks and command-5 metatiles expand here before VRAM/definition-table copies. Persistent raster builders later reuse `$6000`, `$6800`, or `$7000`; partially written raster entries intentionally retain bytes from the last decompression. |
| $7E:8000+ | `widthChunks × heightChunks × 256` | BG1 page-major metatile-id map loaded by command 4. `$46` points here in stock action rooms; `$2E/$30` publish `widthChunks/heightChunks × 256` pixels. This is also the collision map consumed by `$00:91C3`. |
| $7E:C000+ | `widthChunks × heightChunks × 256` | BG2 page-major metatile-id map loaded by command 4; `$4A` points here and `$32/$34` hold its pixel dimensions. Only the active prefix is action-owned. The enclosing `$C000-$FFFF` range is reused by the world-map and town paths. |
| $7F:B800-$BFFF | 4096 | Character-animation source snapshot captured by `$02:BAF5` from VRAM word `$DA`; `$02:BC56/$02:AF30` upload phase-sized windows back to the same target. Continuation profiles intentionally retain the prior capture. |

## Action terrain collision
| Address | Size | Description |
|---------|------|-------------|
| $7E:0014 / $7E:0016 | 2 each | Collision probe inputs: tile X / tile Y in 16px units, consumed by the oracle `$00:91C3`. These addresses are also reused as generic direct-page scratch elsewhere. |
| $7E:002E / $7E:0030 | 2 each | Level width / height in **pixels**. `$2F`, the high byte of the width, doubles as the **chunk-column count** in the map index formula |
| $7E:0084 / $7E:0086 | 2 each | Level width / height in **16px tiles**. `$00:91C3` bounds-checks against these: `tileX >= $84` returns `$0F` (wall), `tileY >= $86` returns `$00` (empty) |
| $7E:05A0 | 256 | **Metatile id → 4-bit quadrant-solidity attribute** (bit0 TL, bit1 TR, bit2 BL, bit3 BR). Built at level entry by `$02:BAC1` from bit 1 of each sub-tile's tilemap word. `$00` empty, `$0F` solid, `$03` top-half (standable platform), `$06`/`$09` the two slope diagonals |
| $7E:2100 | varies | Metatile definitions, 8 bytes each = four 16-bit BG tilemap words in TL, TR, BL, BR order. LZSS-decompressed at level entry |
| $7E:8000 | varies | **Metatile-id map**, one byte per 16px tile, stored in 16×16-tile chunks: `index = ((ty>>4)*$2F + (tx>>4))*256 + (ty&15)*16 + (tx&15)`. LZSS-decompressed at level entry. Fillmore act 1 = 256×48 tiles = 16×3 chunks = `$3000` bytes |

## Asset-script and decompression state ($7E:00A0+)

| Address | Size | Description |
|---------|------|-------------|
| $7E:00A2 | 3 | Asset-script long pointer. `$02:B1F7` and its command handlers address the current operand as `[$A2],Y`; the guarded action HLEs advance Y exactly as the native handlers do. |
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
| $7E:0336 | 1 | Title `CONTINUE / NEW GAME` selection index. Observed values are 0–2; `$02:A622` initializes/reads it and `$02:A7E9` remaps it for the unlocked ending/professional-state marker. |

The directly captured interactive title state at game-frame 821 also has
`$0300=$0100`, `$0302=$0110`, `$92=$0C`, and `$0336<=2`. Those presentation
values remain useful evidence about the native title renderer, but the
host-settings overlay no longer depends on them: Escape/F1 opens it globally
before emulated input dispatch.

The overlay itself has no emulated WRAM state to map. Its open flag, selection,
scroll position, decoded font/frame textures, and menu input live entirely on
the host. Opening and closing clear the host joypad accumulator before the next
NMI sample; while open, game-frame advancement is frozen, so no hidden
in-game “settings mode” byte or PPU page is introduced.

## Town Simulation Data ($7E:0200+)

### Population (two-byte entries, binary)
| Address | Size | Description |
|---------|------|-------------|
| $7E:0218 | 2 | Total population; `$03:8E10` sums the six population words, not support (JP `$0217`, helper `$03:8CFD`) |
| $7E:021A | 2 | Most recently refreshed/selected town's population, not a town ID (JP `$0219`) |
| ... | 2 each | Individual town populations (Fillmore→Northwall) |
| $7E:021C+2N | 2 each | ↑ the individual populations are recomputed by the structure census `$03:C07E`: sum of per-house people by civ level, +2, − `$7F:9F57+2N` — population is derived from standing house records. JP `$021B+2N`, adjustment `$7F:9F4B+2N`; PAL `$021E+2N` with the US adjustment address. No support clamp |

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

The town selector uses **eight usable slots**, starting at
`$024C + 9*($0341-1)`; the ninth byte is terminator/storage, not another item.
Held offerings likewise use `$02A2-$02A9` with the extra byte at `$02AA`.
IDs 12/13 (Ancient Tablet) and 16/17/18 (Bomb) share labels/art but have
different Use handlers. Preserve both item ID and slot. Native `$01:921B`
removes the first matching item ID, and `$01:9239` sorts the eight item IDs
in descending order, moving zeros to the end without touching the ninth byte;
with duplicates, the removed slot can differ from the selected slot. See the
[offering handoff contract](sim-menu-reference.md#use-offering-handoff-contract).

JP uses town base `$024B`, town selector `$032F` and held base `$02A1`;
PAL uses `$024E`, `$0343` and `$02A4`, respectively.
Insertion returns failure when all eight slots are occupied. A discovery's
fired/global flag does not prove its item was stored: the five checked
fishing/miracle Source owners set that guard before attempting insertion
and do not retry after a slot is freed. Keep inventory and story completion
separate; see [capacity and reward ownership](regional-differences-technical.md#full-inventories-and-one-time-discoveries).
Bloodpool's replenishment checks prerequisite event 5 (`$7F:910B & $04`) and
requires all eight town slots to be empty; held items do not block it. See
[the regional timing contract](regional-differences-technical.md#crop-offering-replenishment).

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
| $7E:0295 | 2 | Magic points — PERSISTENT copy. `$21` is the act/working copy, loaded from here at `$02:84E0` (`LDA $0295; STA $21`); act-mode pickups INC only `$21` ($00:887E); sim reward grants INC BOTH via long addressing (`$01:9CD6`). New-game STZ at $02:BE69. No other direct writers in ROM — stats-block writes use `AF/8F`-form long addressing |
| $7E:0297 | 2 | Population needed for next level |
| $7E:0299 | 9 | Magic inventory |
| $7E:02A2 | 9 | Offerings inventory |
| $7E:02AB | 1 | Persistent, zero-based starting lives. Source of Life `$01:9CBD` increments this byte; ordinary action entry loads it into `$1C`. Not an HP increase or level-up award. |
| $7E:02AC | 1 | Selected/equipped magic ID (`0` none, `1..4` Fire/Stardust/Aura/Light); save-backed at SRAM `$145D`, while the high bit of `$0299-$029C` marks the inventory slot containing that selection. **`$02AC = 0` does not suppress the HUD icon's sprites:** the game still emits OAM slots 0-3 with the complete icon signature (tiles `$D4-$D7`, x `$94/$9C`, y `$0B/$13`, attr `$3C`), but `$02:BC9E` leaves the VRAM `$2D40` window zeroed, so the icon renders as nothing. Measured 2026-08-05. Any check keyed on the OAM signature therefore "succeeds" on a blank icon — validate against VRAM `$2D40` (or `$02AC`) if you need to know it is actually *visible* |

**SRAM correspondence (USA ROM, 2026-07-16).** The persistent status block is
linear: for `$0282-$02AC`, the corresponding save offset is generally WRAM +
`$11B1`. This independently resolves the USA adjustment in the reference save
editor:

| WRAM | SRAM | Persistent field |
|---:|---:|---|
| `$0282/$0284` | `$1433/$1435` | Angel current/max SP |
| `$0286/$0287` | `$1437/$1438` | Angel current/max HP |
| `$0288` | `$1439` | Player name |
| `$0291/$0293/$0295/$0297` | `$1442/$1444/$1446/$1448` | Level, HP, MP, next-level population/experience |
| `$0299-$029C` | `$144A-$144D` | Four magic slots |
| `$02A2-$02A9` | `$1453-$145A` | Eight stored item slots |
| `$02AB/$02AC` | `$145C/$145D` | Zero-based lives / equipped magic |

Scores follow immediately at SRAM `$1464-$147B` (six towns × two acts ×
little-endian packed-BCD words). The large town/terrain save body does not use
this simple status-copy relationship and remains mapped only at the raw level;
see [save-format.md](save-format.md) §3.

### Platformer Score Records ($7E:02B3+)
12 packed-BCD words / 24 bytes (6 towns × 2 acts × 2 bytes), ending at
`$7E:02CA`. Display appends a decimal zero; these are not binary integers.
The US Master-status score page sums these in decimal mode at `$01:89FD`.
DP `$00/$02` temporarily hold the low four/carry digits; formatting reuses
that scratch. Japan's Master report has no corresponding second page.

## Dialogue and menu scratch (USA)

These meanings are consumer-scoped, not universal labels for reused scratch.
See [dialogue-system.md](dialogue-system.md) for the control and clear paths.

| Address / register | Meaning in this consumer |
| --- | --- |
| `DB:Y` | Interactive or fixed source cursor; caller determines bank |
| DP `$14` | Packed row/column saved by `$02:BF60` from A; not a global message ID |
| DP `$10/$12` (sound-test modal only) | Music/effect selection counters; the routine changes their low bytes in ranges 1–22 and 1–38. Native numeric formatting and the localization value adapter read the words. These are reused scratch, not global current-song/SFX state. |
| `$7E:0200` | Dialogue pacing/retained-row mode: `$901C` delays non-space glyphs by this many `$9284` frames; zero selects clear at `$02` continuation, nonzero selects row advancement/scroll. Enhanced sessions apply this interval per authored Unicode grapheme through caller-scoped `$9278` (return `$9026`), not per compressed source token. Authored pages retain the same zero/nonzero window policy. |
| `$7E:0201` | Text presentation-state byte initialized to `$FF` on interpreter entry; not a standalone host page counter |
| `$7E:0202` | Native dialogue cursor; reset and row advancement belong to the text interpreter, independently of the host's page/reveal snapshot |
| `$7E:0288-$028F` | Eight live native player-name slots read by interactive `$06`; can precede SRAM `$1439` until the next save. `$0290` is terminator/padding. Never UTF-8 storage. |
| `$7E:034B/$034C/$034D` | Name-entry selected column / row / entered length |
| `$7F:B000-$B7FF` | 32×32 BG3 tilemap staging; byte offset = `row*64 + column*2` |
| `$7E:4000-$DFFF` (ending 08/01 only) | Twenty decompressed credits page maps, `$0800` bytes each. `$02:AB30` copies the selected map to `$7F:B000` and increments `$F1`; the same RAM serves different asset owners in other scenes. |
| VRAM words `$5000-$57FF` (ending 08/01 only) | Separate 256-tile 2bpp credits alphabet; this replaces the dialogue font occupying that slot in ordinary scenes. CGRAM 0–15 is loaded with its palette. |
| `$7F:B100-$B7FF` | Whole-menu clear range at `$01:8CCE`; status rows survive |
| `$7F:B000-$B6FF` | General clear range at `$02:ABC4/$BA41` |
| `$7F:B040-$B0BF` (action) | Two-row HUD template from `$02:8E7E`; lives at `$B050`, timer `$B05E`, score `$B074`; labels use packed graphical lettering |
| `$7F:B0C0-$B0CB` (action) | ENEMY label strip from `$00:A4D6`, not READY; adjacent health cells are separate artwork |
| `$7E:6000/$6800` (title) | `$02:A92F` HDMA BGMODE/TM tables: Mode 7 BG1 logo followed by Mode 1 BG3 menu band; not language storage |

Partial erasure `$02:C1B7` clears each record cell and the cell one row above;
its footprint is neither of the whole-clear ranges. The NMI upload range can
be smaller than the allocated/cleared map; do not infer ownership from upload
length alone. Unicode names and authored dialogue state require separate host
state and explicit save/load handling, not writes into these native slots.

### Town command state

These USA addresses describe the native owner shared by the original and
modern presentations. The host's navigation phase, remembered category rows,
Describe session, scale and binding hint have no additional WRAM allocation.
See the [command flow](sim-menu-reference.md#entry-selection-and-dispatch)
and [dialogue boundaries](dialogue-system.md#town-command-dialogue-and-selectors).

| Address | Meaning while this menu/action owns it |
| --- | --- |
| `$7E:0338` / `$7E:033A` | Menu-list base / retained list-node pointer, both words; town base is ROM `$01:F32E`. These are pointers, not category or action IDs. |
| `$7E:033C` / `$7E:033D` | Animation-family / variant scratch bytes consumed by `$01:AC36`; variant 0 is selected and 1 is ordinary for menu icons. The same scratch serves other animation owners. |
| `$7E:033F` | Zero-based cached menu location; distinct from active location `$0341` and temple action `$033E`. |
| `$7E:00A0-$00A1` / `$7E:00F4-$00F5` | Held-input word / enable mask. Native menu polls the high byte `$A1`: `$80` confirms, `$40` cancels. Both must be released before a fresh activation. |
| `$7F:9217` | Town-state cache captured at menu entry from `$7F:6B18[$7F:7BFB]`; a zero value rejects miracle use before the SP gate. It is not a separately established miracle-unlock flag. |
| `$7F:9208` / `$7F:920A` | Item position-picker coordinates; do not conflate with the confirmed miracle cell fields `$90E1/$90E5`. |
| `$7F:9215` | Full-word target-picker activity, also used by non-miracle commands; [picker details](#story-event-and-scenery-spawner-state-7f9202-7f9228). |

Fixed-screen UI records have stride `$12`: timing at `+$00`, script cursor
at `+$02`, loop/script base at `+$06`, composition at `+$08`, X/Y anchor at
`+$0A/+$0C`, family at `+$0E`, and flags at `+$10` (`$8000` hides a record).

| Record base(s) | Menu role |
| --- | --- |
| `$7E:06A0-$0808` | First 21 records: six categories and 15 commands, in native list order |
| `$7E:081A/$082C` | Yes/No selectors |
| `$7E:083E` | HUD hourglass; independent of menu ownership |
| `$7E:0850` | Start of magic-icon records |
| `$7E:0898` | Start of inventory-icon records; these lie after the hourglass |

Record addresses are stable ownership clues; emitted OAM slot numbers and
screen coordinates are not. Native camera/screen offsets still apply to anchors.

## Temple & Gameplay State

| Address | Size | Description |
|---------|------|-------------|
| $7E:033E | 1 | Temple action (0x00=Give Oracle, 0x01=Listen, 0x02=Take Offering) |
| $7E:0334 | 1 | **Selected/requested song id** (earlier "Death Heim/ending state" reading was a misread of music state). The `[$A2]`-script song handler `$02:B64B` executes a declaration only when this byte matches its selector; a separate loaded-source comparison against DP `$AB/$AD` skips redundant uploads. Written by ~10 play sites (`$00:828F/A370/F650/FF04`, `$01:8602/8754/8856`, `$02:8345/BD2A`, `$03:8262`); zeroed by the transition stop at `$00:83F2`. `$00:A370` selects 4 after all six act counts reach two. `$00:FEFC` selects 1 at final-boss teleport-out; `$00:F650` selects 3 after the returning `0701` sky fade-in — too late for the black-frame BG page swap (`$00:F5F0-$F619`, BG1SC/BG2SC `$64/$74`). JP equivalent field is `$0322`. |
| $7E:0341 | 1 | Active world-location ID, 1-7. `$01:B6CA` first clears it, then writes the entry whose 256x256 source-pixel region from ROM table `$01:B73C` contains the `$0300/$0302` focus; zero therefore means outside every town border. The location label and 3D navigation clear-region mask consume the same value; zero keeps the full world hazed. Also read by `$00:A375` as the pending post-Death-Heim destination |
| $7E:0347 | 1 | Death Heim boss-rush/ending progress: `$00:FEEC` writes `$19 - 1` after each boss (hub stager `$F3D4` warps to `$0347+2` next); `$07` = final boss beaten. Epilogue completion `$01:8859` increments it to `$08`; world-scene handler `$02:84EC` then selects silent music ID 21 and runs the departure into credits. |

## Debug / System

| Address | Size | Description |
|---------|------|-------------|
| $7E:035A | 1 | Event-effect request port: COP vector `$00:8526` stores A's low byte here (`LDA #id; COP`). Consumed by the NMI tail `$02:AC33` every other frame as the LOW byte of one 16-bit load, forwarded to APU port `$2142`, then zeroed. The serial replay corpus covers `$01/$07/$12/$83/$85/$89/$8A/$90/$94/$9A/$9E/$A0/$A1`; high-bit ids duplicate their low-seven-bit sequence across both native effect lanes. See [native audio channels](snes-native-audio-channels.md) for the port protocol. |
| $7E:035B | 1 | SFX request port: BRK vector `$00:852F` stores A's low byte here (`LDA #id; BRK`). Forwarded as the HIGH byte of the same 16-bit NMI store to APU port `$2143` and zeroed together with `$035A`. **id `$00` = idle/clear, not a sound** — it is by far the most-written value in the early census (754 posts vs 12 key-ons, mostly from `$03:9E6B`), and the NMI forwards zero as "nothing pending". The later serial trace confirms exposed BRK ids `$02/$03/$09/$0C/$10/$1A/$1B/$20/$21` across its staged replay corpus. See [native audio channels](snes-native-audio-channels.md) for the external replacement seam. |

## High RAM ($7F:0000+)

### Graphics & Map Data
| Address | Size | Description |
|---------|------|-------------|
| $7E:4000+ | varies | Per-act decompressed ordinary-object animation/composition blob. Loaded by `$02:B69C` only at act-entry maps and inherited by later maps in the same act. Bloodpool scene composition pointers `$45EF/$4610/$46FE/$479D`, Marahna orb pointers `$4504/$4510/$451C/$4528`, snake-shot pointers `$4869/$487C`, split/link pointers `$4597/$4BCD/$4BD9/$45B8/$45C4/$45D0/$45DC/$4AA1/$4B82`, excluded reaper-orb pointers `$47E5/$4806/$4827/$4848`, excluded platform pointer `$4BE5`, and Aitos `$4D21/$4D2D` are addresses inside this mutable WRAM blob, not ROM symbols. Marahna boss-room pointers `$57C2/$5868/$59DE/$5CE0/$5D01/$5D0D/$5D2E` live in its separate `$7E:5000` bank. |
| $7E:5000+ | varies | Per-map decompressed boss animation/composition blob selected by the same asset-script command with nonzero destination flag. Aitos boss-volley visuals `$20/$21/$23` resolve to mutable WRAM compositions `$56BE/$56D8/$56FE` in run `20260812-000613`; Flaming Wheel's full rings use `$5276/$5398/$54BA/$55DC` and its cyan shots use `$51B5/$51C1/$51CD/$51D9`. Like every loaded pointer, these addresses identify artwork only inside the validated map/source lifecycle. |
| $7E:6000-$7E:7FFF | 8KB | Shared action character/metatile decompression workspace and persistent-raster table storage. R1-R6/R8 use `$6000`, R7/R10 BG1 use `$6800`, R9 uses `$7000`, and R10 BG2 uses `$6000`; untouched bytes can remain presentation-visible. |
| $7F:2000+ | varies | Arrangement data |
| $7F:6800+ | varies | Road construction data (one word per 4x4 block) |
| $7F:B000-$7F:B7FF | 2KB | BG3 tilemap |

### Act Completion ($7F:6B18-$7F:6B23)
Two bytes per town tracking act completion counts. `$00:A343` (Death Heim exit
stager) requires all six words == 2 for the all-bosses-done path.
The town BG character-bank filter also reads this count: below 2 loads raw
file `0x60000`, at 2 or more loads `0x64000`. Both are 16 KiB uploads to the
same VRAM destination. This selector is not the house-development tier.
The rule matches all five ROMs; JP's early-bank skull-lair star is absent
from the later bank. [Source and tests](regional-differences-technical.md#town-title-and-death-heim-artwork).

### Building Direction UI
| Address | Description |
|---------|-------------|
| $7F:6B9F-$7F:6BAA | X positions (6 towns) |
| $7F:6BAB-$7F:6BB6 | Y positions (6 towns) |

### Structure records & town capacity
| Address | Description |
|---------|-------------|
| $7F:3800-$7F:53FF | Per-cell flag maps, `$400` per town (32×32 cells; bit0 set at road/build commit `$03:9623`, bit1 at `$03:8E48`, transient pathfinder visited bit2 set at `$03:9A50` and tested by the `$03:96EF` HLE). Construction predicate `$03:96BE` requires bit2 **set**, plus tile `$08` or `$D0-$DA`; a visually empty cell is not necessarily available. |
| $7F:6BCF+2N / $7F:6BDB+2N | X/Y plot coordinates consumed as the flood-fill seed by US/PAL `$03:9156` / JP `$03:8F3B`. Each is multiplied by four to obtain cell coordinates. The wrapper clears/rebuilds the town's visited bits; treating every eligible terrain cell as visited bypasses real construction constraints. |
| $7F:6B26+2N | Per-town **support capacity** (census `$03:C07E`: US32/48/72, JP16/24/32; bridges32US/16JP). Admission checks old population ≤ support+2; not a resident cap |
| $7F:6BE7-$7F:77E6 | Per-town **structure-record arrays**, `$200` each (base = `word[$03:DC74+town*2]`): 128 × 4-byte records `{cell X, cell Y, flags/type, action/progress}`. Flags byte: bit7 active, **bit6 not-yet-contributing / per-class visual variant — NOT a construction flag** (the allocator never sets it; on a class-3 windmill it is the "no wind" story state), bits 4-5 subtype (house civ level / wheat `$10` / bridge orientation), low nibble type class (0 house, 1 bridge, 2 field, 3/4 factory tier). Allocator `$03:9D9F`; the 128-slot exhaustion is the game's 128-structure cap |
| $7F:77E7-$7F:7BE6 | Per-record visual step-machine slots, 128 × 8 bytes (armed by the construction `$03:A4B8` / rebuild `$03:A4A8` HLE pair through one shared resolver/armer, then walked by the `$89F7`/`$8A7E` 8-frame stepper). Completed sidecar bridges bypass this pool: the `$89F0` HLE resolves and replays their single native rebuild draw through the same model. Slot layout, from interpreter `$03:A4F7` (decoded 2026-08-17): `+0` countdown, decremented once per tick, entry executes when it hits 0; `+1` loop repeat counter; `+2` program cursor (bank-`$03` address of the NEXT entry); `+4` loop restart address, set by the program's `$FF` opcode; `+6` address of the CURRENT entry's draw-list pointer word, which `$03:A591` dereferences to redraw. The armer initialises `+0`/`+1`/`+2`/`+4` only, so `+6` is stale until the first tick |
| $7F:7BE7 | Step/tick scratch variable (record index during scanner passes) |
| $7F:7BE9 | Scanner gate: nonzero makes `$03:A4A8/$03:A4B8` (arm rebuild/construction visual step) and `$03:A4F7` early-out |

#### Record `+3` = action/progress byte, and the per-type state machines (mapped 2026-07-22)

Bit 7 is a separate flag; the **low nibble is the action state**. `$03:9F05` writes the low
nibble (`AND #$70` + ORA — note it also CLEARS bit7); `$03:9EF5` writes bits 4-6 (`AND #$8F`).
A healthy standing house reads `$80`; `$07` means destroyed-and-pending-free.

Scanners: `$03:9E6B` (houses) and `$03:9DE4` (fields/bridges/factories) process **16 records per
frame**, slice = `$88 & 7`, so all 128 records every 8 frames; `$03:9E5A` is a full 128-record
pass. Each dispatches on record `+2 & $0F` (type) to one of 7 outer handlers, which do
`LDY #<table>; BRL $9ED3`. `$03:9ED3` then indexes the per-type table by the `+3` low nibble and
RTS-tricks to the handler (see the `rts_dispatch 9EF3` web in `recomp/bank03.cfg`).

House table `$03:A017` (entries store *addr-1*):

| action | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| handler | `$A027` | `$A04C` | `$A050` | `$A05A` | `$A060` | `$A066` | `$A07F` | `$A44F` |

`$03:A44F` (action 7) is the **destroy/remove one-shot**: it arms visual set `$7D1F=$0E`/`$7D21=0`
via `$A4B8`, calls `$A4F7` and `$9FCD`, then `LDA #$00 / STA $0002,X` — zeroing the flags byte,
which **frees the slot**. It is unconditional once entered, so a record sitting at `+3` low
nibble 7 with `+2` still `$80` means the handler was never reached or never
completed. `$03:A477` is the sibling free handler with the same tail.

To watch record transitions live, trace the array directly (flat offset = `0x10000 + $7Fxxxx`):

```sh
AR_WRAM_TRACE=structrec.jsonl AR_TRACE_LO=0x16BE7 AR_TRACE_HI=0x16DE6 \
  ./build-release/ActRaiserRecomp ar.sfc
```

(that range is town 0 / Fillmore's 128 records; shift by the town's `$03:DC74` base for others.)
| $7F:7BF9 / $7F:7BFB | Current town id / town id ×2 (index into `$03:DC74`) |
| $7F:7C05 / $7F:7C07 | Shared census/scan scratch (house-population sum before +2/adjustment, support sum; reused for allocator slot index and other scans) |
| $7F:7C11/13/15/17 | Record-scan rectangle X0/Y0/X1/Y1 (cell coords); shared scratch. Miracle story dispatch `$03:F921` instead uses `$7C11/$7C13` for aim square X/Y, or `$7C11=$FFFF` to bypass location for Earthquake/Wind |
| $7F:7C1D | Record-scan remaining counter |
| $7F:7C33/$7C35/$7C37 | Plot-operation controls: stamp roads / attempt buildings / remaining allocation budget. Complete offscreen caller `$03:90DE` sets budget to 0 or 1 from the growth resource `$9560` (JP `$9554`) and debits that resource only if consumed. Shared addresses in all five ROMs; not a population cap. |
| $7F:7C3D | Construction-attempt marker used by growth-status calculation, not a completed-building count. Available house candidates set it in all five ROMs. JP also sets it for an available food footprint, including with budget0; Western versions do not. [Producer/store distinction](regional-differences-technical.md#regional-growth-status-producer). |
| $7F:7C41/$7C43/$7C49 | Construction plot X/Y (0–7) and template index (0–11). The iterator expands each plot to 4×4 cells and stages sixteen availability words at `$7C51-$7C70`. |
| $7F:7C9D/$7C9F/$7CA1 | Pending allocation request: cell X, cell Y, type byte |
| $7F:90E1/$90E5 | Miracle aimed map cell X/Y (`$96EA/$96EC >> 4`); `$90E3/$90E7` = square-aligned copies |
| $7F:90E9 | User-miracle operation active; set by `$01:97E5`, cleared after effect-wrapper cleanup, later than `$90F3` completion. Skull attempts retry while this is set. Subsequent result dialogue can hold the pending timer even after it clears. Posted enemy/script effects use `$90F5` instead |
| $7F:90EB | Active miracle kind: 0 silent clear, 1 Lightning, 2 Rain, 3 Sunlight, 4 Earthquake, 5 Wind |
| $7F:90F1/$90F3/$90F5 | Lightning/Rain visual actor finished; overall effect complete; posted/scripted-driver marker. `$90F5` suppresses the player's direct lair depletion but not destroyed-house feedback; it can remain set after completion, so it is not an active-effect boolean |
| $7F:90F7 | Miracle result/change marker: cleared at user-miracle entry, set by structure operation `$03:B274` and some story-trigger handlers, tested after cleanup by `$01:9832` to select the wrapper's return carry. Not solely a visual-refresh flag, nor set by every story flag change: Marahna Lightning's `$03:FBD1` sets global bit12 without writing this marker |
| $7F:9218/$921C/$923E | Wind remaining ticks (120); Earthquake remaining ticks (180); Sunlight phase (0-140) |
| $7F:9250-$7F:954F | Per-town built-square lists, `$80` each: 64 × 2-byte **square**-coord pairs (x,y ≤ 7), `$FFFF` = empty (append/dedup `$03:8EC1`; staged pair `$9550/$9552`; cursors `$9554+`; persisted at SRAM `0x0300+town*0x80`) |
| $7F:96E8/$96EA/$96EC | Miracle/effect post: kind, pixel X, pixel Y, consumed by master-loop `$03:820F`; Skull Head requests Earthquake kind4. Kind is cleared before the ordinary actor pass (US `$03:8204`, JP `$03:820F`), read afterward, and remains set throughout posted-effect servicing. JP fields `$96DC/$96DE/$96E0`. At the post threshold, existing kind/sealing `$7BEB` cancels the attack; otherwise user-active `$90E9` or picker `$9215` delays retry. Two-Skull native fixtures produce one effect, not a queued second attack. |

### Monster Lair Data ($7F:9500+)
Eight parallel arrays of **24 two-byte entries** (48 bytes each) — **4 lairs per town × 6 towns**,
indexed as `town*8 + slot*2` (`$03:B7A3` uses `LDY #$0004` against a base of `town*8`; the installer
`$03:B7C6` loops `$18` = 24 times). Seeded from ROM `$03:B825` (24 × 9 bytes) and saved/restored
through SRAM by `$03:A850`. Corrected 2026-08-02 — the arrays are **not** 16 lairs of 3 bytes.
| Address | Description |
|---------|-------------|
| $7F:9568-$7F:9597 | Lair X on town map — **16px cell units, 0..31**; selector square = X>>2 |
| $7F:9598-$7F:95C7 | Lair Y (same units). Spawn position = (X*16+$18, Y*16+8), written to world record +$0A/+$0C by `$03:B99C` |
| $7F:95C8-$7F:95F7 | Lair imagery ID; also carries runtime state bits ($8000 tested by `$03:B4EA`/`$B99C`, $2000 set by `$03:BAB6`, $1000 skips the respawn countdown) |
| $7F:95F8-$7F:9627 | Monster type — this value becomes the spawned world record's class field +$0E |
| $7F:9628-$7F:9657 | Respawn delay (reload value) |
| $7F:9658-$7F:9687 | Respawn countdown (`$03:B9BB` DECs; reloads from $9628 at 0) |
| $7F:9688-$7F:96B7 | **World-record address** the lair's monster occupies — always $0B30/$0B56/$0B7C/$0BA2 = `$0A00`-array records 8-11 |
| $7F:96B8-$7F:96E7 | Remaining monster count (population). `$03:BB04` DECs per kill; `$03:BAC8` subtracts 10 for a miracle strike |
| $7F:9750 | Lightning sequence trigger |

### Flags
| Address | Description |
|---------|-------------|
| $7F:9101 | World-state flags. Bit 0 unlocks Death Heim: `$02:865C` preserves its 8x8 block at world-tilemap offset `$0660`, otherwise clearing that block; `$01:B6CA` admits the seventh location rectangle only when set. Bit 1 means the Palace announcement has begun and suppresses its replay. US sets bit 0 at final-act departure, then bit 1 before dialogue; JP sets both before its Palace dialogue after checking all six act counts. Preserve unrelated bits. Neither bit is a transient reveal-progress counter. [Regional sequence](regional-differences-technical.md#death-heim-transition-and-music). |
| $7F:9102 | Scene-local flags within the global bit array. Mask `$40` (index 25) guards both Fillmore fishing and Northwall lake-scene creation; `$20` (index 26) guards Marahna fishing and is tested by Aitos's dying-man callback, which writes the wrong flag family (see below). Palace initialization writes `$01` to the whole byte in every ROM, clearing both masks without clearing counters, Compass knowledge or fired events. The fishing callbacks differ: Fillmore/Marahna reset their counter on initialization, Northwall does not. Do not model these guards as permanent discoveries. |
| $7F:910B | Bloodpool's story-event **prereq** bitmap, byte 0 (= `$9107 + 1*4`). The PAR-derived "bridge technology (bit 0x20)" label is event id 2 of that town — see the event-bitmap table below |
| $7F:916E | Fillmore fishing counter, same address in all five ROMs. Target 255 Western / 128 JP. Counts are callback updates, ordinarily 8 live frames apart in the West / 40 in JP. Palace entry leaves it intact, but cleared initialization makes unfinished fishing restart on return. Fillmore fired 10 prevents normal repeat rewards. [Discovery and switch contract](regional-differences-technical.md#source-discoveries-and-compass-fishing). |
| $7F:9171 | Aitos dying-man timer byte in all five ROMs. Callback 2 increments without resetting during scene setup; equality with 128 marks fired 2 and requests message 11. Distinct from the actor's `+$22` pose wait of 120 updates. The global-26/fired-26 mismatch repeats scene initialization and interrupts that animation, but full timeout and early-Rain/message-12 paths complete in the checked live scenes. [Original-selector and live evidence](regional-differences-technical.md#aitos-dying-man-scene-flag-mismatch). |
| $7F:9173 | Shared Marahna fishing / Northwall lake-search counter in all five ROMs. Marahna resets on initialization and finishes at 128; Northwall retains the value and finishes at 255. Completed Marahna value 128 survives the tested Palace/native Northwall-load path, leaving 127 updates to its Magical Light reward. Guards remain separate: Marahna fired 10 / Northwall fired 3. [Native alias, lifecycle and limits](regional-differences-technical.md#northwalls-lake-search-shares-marahnas-counter). |
| $7F:918D | Teddy-return marker, shared address in all five ROMs. Successful Bread use writes 1 after consuming held item 7. Bloodpool's event 6 callback reads it to supply the Magic Skull, complete event 6, release the development hold and request event 7. Not itself event 7's prerequisite/fired bit. |
| $7F:9192 + town | Six crop-knowledge bytes, shared addresses across the five ROMs. Bloodpool event 5 writes `$9193=1`; its road-connection event 4 can write Fillmore's `$9192=1` and upgrade all Fillmore fields. Ordinary-field item-8 use writes the receiving town's byte after converting one field; already-upgraded use consumes the item without this write. Replenishment checks prerequisite 5 and inventory emptiness instead, not a timer here. [Crop-sharing contracts](regional-differences-technical.md#bridges-and-cross-town-crop-sharing). |
| $7F:91A5 | Bloodpool Music/state byte. Accepted item 11 writes 1 after its response; event 8 tests nonzero to finish disputes, clear `$7CF1` and mark fired 8. The ambient music selector also reads this byte; do not treat it as the event-8 bitmap or a standalone save flag. Kasandora's Music grant writes its neighboring `$91A6=1`. |
| $7F:91D4 + town | Compass-delivery bytes. Successful Use in Fillmore or Marahna writes 1 after consumption/response; the respective periodic handler latches prerequisite 10. Other towns are rejected by the Use handler. Not the fishing progress counter or the completed-event bit. |

### Story-event bitmaps ($7F:9107-$7F:914E)

Three parallel per-town arrays, 4 bytes = **32 event ids** per town, base pointers in ROM at
`$03:DCA2`/`$DCAE`/`$DCBA`. Bit order is **MSB-first**: id `k` → byte `k>>3`, mask
`$80 >> (k&7)` (mask table `$03:F4D7`). Helpers `$03:F46E` test / `$F479` set / `$F487` clear,
resolver `$F497` (scratch `$7F:914F`).

| Address | Description |
|---------|-------------|
| $7F:9107 + town*4 | **prereq/enabled** — the event may be selected. Persisted at SRAM `0x120E` |
| $7F:911F + town*4 | **fired** — excluded by normal event selection once set. Not a universal guard for forced dispatch or miracle handlers. Persisted at SRAM `0x1226` |
| $7F:9137 + town*4 | **dispatched this session** (set by `$03:E02B`/`$E0B0`); not persisted |
| $7F:914F | scratch byte holding the resolved bit mask (`$03:F497`) |

Fillmore's retained Magic Skull event uses mask `$02` in `$9108/$9120/$9138`
for enabled/fired/dispatched event 14. Its callback enables event 15 through
`$9108 & $01`; event 15 sets `$9120 & $01` and requests `$920E=$8F`.
These addresses agree across all five ROMs. No normal producer for Fillmore's
enabled-14 bit has been identified; setting it in a fixture is not a gameplay
acquisition route. See [the retained event and controls](regional-differences-technical.md#fillmores-additional-magic-skull-event).

Aitos callback 2 tests scene-global bit 26 (`$9102 & $20`) but writes
town fired bit 26 (`$912E & $20`) through its inherited fired-table pointer.
Its actual completion flag is fired 2 (`$912B & $20`). These are three
distinct states despite the shared mask value. The first write does not
enable the magic-discovery message assigned to slot 26. See the
[five-ROM flag mismatch](regional-differences-technical.md#aitos-dying-man-scene-flag-mismatch).

These were labelled "open lairs"/"spawned lairs" until 2026-08-17. Monster-lair state is the
separate `$7F:9568+` block above; these bits are consumed by the `$03:DFFB` event selector and
by every town-event handler in `$03:E6xx-$F3xx` (32 ids/town matches the 32-entry handler
tables at `$03:E66E`, not 4 lairs/town).

Population and road producers also latch these prerequisite bits. US/PAL
`$03:E122` / JP `$03:DC27` reads population at the active town's indexed
word and requires **strictly greater than** the authored threshold.
US/PAL `$03:E15D` / JP `$03:DC62` tests specific road-square words.
Neither clears a previously reached prerequisite when the condition stops
holding. Individual event callbacks can clear it: Bloodpool event 8 clears
prerequisite 8 if the act count is not 2, and Western versions also clear
prerequisite 9. Japan retains 9. The regular pipeline re-enables population
conditions before selecting again, so that difference alone does not bypass
event 8's priority. See [population tables](regional-differences-technical.md#population-and-road-story-prerequisites)
and [late Bloodpool progression](regional-differences-technical.md#bloodpool-disputes-music-and-compass).

Periodic maintenance latches prerequisites; it does not recompute these
arrays from scratch. For example, Aitos prerequisite 6 (`$9113 & $02`) and
fired 5 (`$912B & $04`) are set by `$03:F7D1` when global flag 8 is set and
**Fillmore** population is at least 20. The population word is US `$7E:021C`,
JP `$7E:021B`, PAL `$7E:021E`, not the active town's indexed word. This
dependency matches all five ROMs. See
[periodic maintenance](regional-differences-technical.md#periodic-town-event-maintenance)
for the other tested gates and their limits.

Bloodpool's bridge request can return from the temple with prerequisite 2
and dispatched 2 set but fired 2 still clear. Its callback `$03:E9ED` tests
the separate technology byte `$7F:919F`; when zero, it selects ambient scene
7 without marking the event fired. This path matches all five ROMs. Do not
derive a universal event-completion rule from a temple transition alone.

Bridge delivery through US/PAL `$01:9E28` / JP `$01:9E04` sets `$919F=1`
**before** the acceptance dialogue finishes and item 10 is consumed. The
later town callback marks fired 2. All five ROMs preserve this ordering;
`$919F` alone does not establish that the offering transaction has settled.
See [bridge delivery](regional-differences-technical.md#bridge-offering-delivery)
for native cancellation/rejection tests and regional held-inventory addresses.

Bloodpool's Teddy callback independently sets development hold word
`$7F:7CF1` (town 1 in `$7CEF+2N`) while the return marker is clear, then
clears it when Teddy returns. Global bits 10 and 25 at `$7F:90FF` guard the
Bread grant and actor spawn separately; the event 6 fired bit remains clear
until the return path. That path sets fired 6, clears dispatched 6 and requests
event 7 through `$7F:920E=$87`. See [the source and fixture boundaries](regional-differences-technical.md#bloodpool-crop-and-teddy-event-joins).

### Story-event and scenery-spawner state ($7F:9202-$7F:9228)

| Address | Description |
|---------|-------------|
| $7F:9202 | event-selector scan cursor, `0..$1F` (`$03:E015` loop) |
| $7F:920E | pending-event latch. Bit 7 set = "run id `& $7F` next"; `$03:DFFA/$E004` strips the bit and returns the id. Handlers self-latch, e.g. Aitos event 1 writes `$81` at `$03:EED8`, `$03:EFE5` writes `$87`, and `$03:FAE1` writes `$88`. In `broken_text.rec`, event 1's `$81` survives after its fired bit is set; event 4 then becomes eligible before the selector runs again, so the forced path returns event 1 and repeats the all-monsters message instead of the mountain discovery. `AR_FIX_AITOS_EVENT_QUEUE` clears only that exact stale-latch/bitmap combination before the next game frame, leaving the native selector to choose event 4. |
| $7F:9215 | target-picker active word, set to1 by US `$01:9754` / JP `$01:96FF`, cleared on B-confirm/Y-cancel. Confirm writes `$90E1/$90E5` from each angel coordinate as `((coordinate+8)&$FFC0)>>4`. Native Magic Skull flow verified; this is not a seal/completion flag. |
| $7F:9220 | late-bound **pool-allocator pointer** (callee−1) for the scenery/cutscene spawn trampoline `LDA #cont; PHA; LDA $9220; PHA; RTS`. Only three values: `$CA79`→`$03:CA7A` (`$01:B790`, 7-slot `$0E02` pool), `$CA7E`→`$03:CA7F` (`$01:B798`, `$0F0C` pool), `$B67C`→`$03:B67D` |
| $7F:9222 + town*2 | **active ambient scene index** into `$03:FD0E`; 0 = no ambient actors. Session-only — written solely by story-event handlers (Aitos `$9228`: `$03:EFF6` writes 4, `$03:F030` writes `$15`), never restored from SRAM |
| $7F:922E | compared against the active index by `$03:FCE8`; no decoded code ever writes it (dead compare) |
| $7F:8F6F | structure **class filter** for the filtered spawn loop `$03:CDB0` |
| $7F:8F70 | scene id, index into `$03:CE5B` |
| $7F:8F71 / $7F:8F73 | scene base offset in pixels (script record `+2`/`+4` × 64) |
| $7F:7C11 / $7F:7C13 | query cell for the exact-cell structure lookup `$03:BD84` |
| $7F:9F6B + town*2 | per-town development gate; `$03:FCE8` bails when zero |

World-record `+$0F` (high byte of the pending-type word `$7F:7CA1`, stored to `+$0E` by the
`$01:B778` allocator family) is the scenery **kind**, and `$01:CF0A` indexes `$01:CF2B` by it —
kind 0 people, 2 horse, 4 dog, 6 sheep, 8 boat, 10 flame.

### Saved SIM actor cache ($7F:97DA-$7F:9EF9)

Regional combat/AI metadata keeps four live policy snapshots and 24 cached
snapshots in the save companion, not in these native records. Native copies
at `$03:813F/$03:8168` synchronize the corresponding policy copies; `$03:B9EE`
pins only a verified new generation. The combat scan uses the first four slots
(`$0B30/$0B56/$0B7C/$0BA2`); the other four cached actors stay native. Accumulated
arrow damage remains the native byte at actor `+$24`, and never changes merely
because a regional option is edited. [Integration contract](regional-differences-technical.md#sim-combat-integration).

For these monsters, `+$0E` is the species byte; `+$0F` is behavior state,
including a Bat's carrying flag. Treating the pair as a 16-bit species ID
incorrectly loses ownership after that flag changes. `+$14` is state-local:
Japanese Dragon search uses an eight-update counter, while Bat state 4 uses
its abduction wait there. The strike helper can change X to its effect slot;
the parent Dragon remains in the native `PHX` word at `S+1` at `$01:BB5C`.

Six town slices of `$0130` bytes; eight `$26`-byte records each. JP base is
`$7F:97CE` (end `$9EED`). US ROM `$03:8111` / JP `$03:810E` selects the slice.
US `$03:8168` caches live `$0B30-$0C5F`; `$03:813F` restores on entry (JP
`$8165/$813C`). Native save copies all `$720` bytes to SRAM `$1633-$1D52`;
Continue restores them before entering a town. This is separate from the
unsaved ambient-story scene index. A class16 soul and its pending growth reward
survive a save/cold-load round trip, including a zero-stock lair.
Town-switch fixtures show that an off-town soul remains frozen here, resumes
once on return, and then frees its live slot for another monster. The cache
does not continuously mirror live records: it retains the old soul until the
next cache operation. New-town initialization US `$03:811D` / JP `$03:811A`
marks the eight cached records inactive by writing `$8000` at `+$10`; it does
not zero the entire cache. A native cache restore is not a new actor spawn.
See [save contracts](save-format.md#35-lairs-growth-and-sim-actor-cache) for the
restore loop's extra trailing-byte footprint and exact serializer owners.

### Town Growth Points ($7F:9EFA+)
Two-byte binary entries per town. JP equivalent is `$7F:9EEE+`.
Sources include post-action score conversion, qualifying miracle depletion,
destroyed-house feedback when all lairs are sealed, remaining-stock settlement
on guidance or Magic Skull sealing (`$03:B561`), and the delayed return of a defeated monster's
class-`$16` soul (`$01:B95A`, one point even with zero stock or a sealed lair).
The standalone kill decrement does not credit this pool; the later soul does.
Sealing credits remaining stock without clearing it or despawning a live monster.
Native `$03:B54E` adds A **with incoming
carry**, so callers' flag state is part of the contract. See the
[regional stock/growth audit](regional-differences-technical.md#lair-stock-is-not-monotonic).

Regional read/write cautions:

- Stock at US `$7F:96B8+` / JP `$7F:96AC+` can increase; it is not a lifetime
  kill counter. Imagery/state `$95C8+` / JP `$95BC+` carries the sealing bits.
- US `$7F:91DA+2N` also participates in status reporting; JP's report uses the
  corresponding array but a different classification path. A growth label is
  not a population cap.
- US `$7F:91E6+2N` is the previous-status snapshot copied by `$03:8612` after
  construction. `$03:8620` compares it with `$91DA+2N` to detect newly raised
  warning bits. Regional status rules leave this native copy/comparison intact.
- US `$7E:0B04/$0B05` are byte HP/SP recovery queues; JP `$7E:0B04` is a
  **word** recovery phase. Never project one ROM's structure onto the other.
- `$7F:91FE` is the long development clock; `$9200` is its subcycle. JP's
  `$7F:7CED` divider is also advanced by the miracle frame service while those
  clocks are held. Menus and miracle effects have different actor/recovery
  service ownership; none of these counters alone means all SIM is paused.
- US SP/current maximum are words `$7E:0282/$0284`, angel HP/max bytes
  `$0286/$0287`; JP equivalents are `$0281/$0283` and `$0285/$0286`.
- World-monster record `+$24` is accumulated arrow damage (byte), not remaining
  HP. Collision compares it strictly greater than the species threshold.
  US thresholds 2/0/3/7 differ from JP 1/0/2/7; contact damage differs too.
  Class-`$16` return/reward and action-object `+$24` timers are separate meanings.
- Guidance path data is US `$7F:9F12+` / JP `$7F:9F06+`, with cursor
  `$9F48` / `$9F3C`. The controlled one-step fixture sets path length 1 and
  direction 3 (left); the native state-2 service checks traversal and sealing.
  `$7F:7BEB` marks the sealing effect, not general actor/development activity.

### Road Construction Encoding ($7F:6800+)
- One word per 8×8 selector square, `$80` bytes per town (`$300` total, all six
  towns at `$6800+town*0x80`); initialised from ROM `$03:DCFA`, persisted at
  SRAM `0x0000+town*0x80` (save-format §3.4)
- Bit 0x40: Obstructs building direction selector
- Bit 0x80 / 0x100: river-crossing bridge state per axis — set when the bridge
  builders `$03:9985/$99CA` allocate a bridge record, checked so a crossing is
  never re-bridged
- Bit 0x200: Shows obstacle layer instead of base
- Bit 0x800: Skull Head state2 target eligibility, US `$01:C58F` / JP
  `$01:C519`. One random square per attempt; acceptance targets pixel
  `(64*x+16,64*y-16)`, rejection returns to state1. Not a direct house count:
  the temple square is eligible and native updates can restore its bit after
  a test clears it. Verified native lookup is US `$03:97A3` / JP `$03:958E`.
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
| `$7F:96B8-$96E7` | SAFE <town> 1-4 (00) | **Remaining monster count**, not sealed state: 2 bytes/lair × 4 lairs × 6 towns, town order Fillmore/Bloodpool/Kasandora/Aitos/Marahna/Northwall (codes 41-88). Setting zero exhausts stock; sealing flags are in `$95C8-$95F7`. See [US/JP lair evidence](regional-differences-technical.md#monster-lairs). |
