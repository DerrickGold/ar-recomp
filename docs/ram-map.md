# ActRaiser RAM Map

SNES WRAM: 128KB at banks $7E-$7F.
Direct page and stack in first 8KB ($7E:0000-$7E:1FFF), mirrored at $00-$3F:0000-$1FFF.

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
| $7E:001F | 2 | Score | BCD format; `$00:873C` adds and saturates at `$9999` |
| $7E:0021 | 1 | Magic points | Act working copy; loaded from persistent `$0295` at `$02:84E0`. Scroll pickup INCs only this (`$00:887E`), cap `$FF` |
| $7E:00E3 | 1 | Heal queue | Pending HP refill; `$00:88D6` drains 1 HP every 4th frame while `$1D < $1E`. Set by item ids `$04`/`$05` |
| $7E:00E4 | 1 | Sword power-up | `$80` = item id `$03` collected; `$00:9DC8` then gives the player ATK 2 instead of 1. Never ticks down — cleared on act change |
| $7E:00E6 | 2 | Time remaining | BCD format |
| $7E:0349 | 2 | **Professional Mode** | Nonzero = pro mode. Doubles enemy ATK/HP when exactly 1 (`$00:9679`/`$968C`), rewrites statue drops (`$00:962B`), and indexes the stage-order table `$02:9013` at act clear (`$00:8781`) |
| $7E:08BC | 1 | Player **Crest** walking-cycle phase | TAS terminology; interacts with Boost to determine normal/pre-jump movement cadence. Player object `$08A0 + $1C`. |
| $7E:08C4 | 1 | Player **Boost** walking-speed countdown | Can produce temporary 3 px/frame movement; player object `$08A0 + $24`. Extended `AR_FRAMELOG=1` records both fields with input and position delta. |

### Camera / Scroll — full model in rendering-engine.md §4/§6/§11.1
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
| $7E:0380 | 512 | OAM shadow: 128 x 4-byte entries (x, y-1, tile, attr); cleared to x=$80,y=$E0 each frame via a stack-push fill. Both details are load-bearing for the diorama vertical band: the y field is 8 bits mod 256 against 224 lines, so the `$E0` park value IS screen -32 and collides with genuine above-screen positions, and a sprite near the screen BOTTOM aliases into the band through the same wrap. `PpuSetObjExactPosition` carries the emitter's un-truncated x AND y beside this table — see rendering-engine.md §13i (the band) and §13j (the apron) |
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
apron channel, rendering-engine.md §13j) must read it here and resolve through
`PpuObjSizeForSizeBit` — there is no size information in the tile/attr word.

### Action objects and magic cohorts (action mode only)

| Address / field | Size | Description |
|---|---:|---|
| `$7E:06A0-$1A9F` | 80 × `$40` | Action object slots. Magic cohort slots are `$06A0-$0820`; cast controller is `$0860`; player is `$08A0` |
| `$7E:02D0-$02E0` | 17 | **PRNG state pool.** `$00:84C0` advances it (a carry-chain `ADC` down the pool, then a multi-byte counter increment) and returns the byte at `$02D1` in A. Every randomized spell decision goes through it — e.g. Magical Stardust's launch site picks top-vs-right edge and its Y offset from one call (`$00:A0E8`, see bug-ledger.md §33). |
| `$7E:08A2/$08A4` | 2+2 | Player object world X/Y (`$08A0 + $02/+04`). The arrival gate uses initialized X `$08A2` to reconstruct the native horizontal activation camera before `$97A6` transfers camera-subject ownership through `$8A`; drawing uses horizontally fitted `$22` and native vertical `$24`. |
| `$7E:08B2` | 2 | Player primary handler (`$08A0 + $12`). Action entry advances `$97A6 → $97C9 → $97E4`; `$97E4` installs `$9832`, the first handler that reads held input. This lifecycle gates only extra horizontal activation, never widescreen drawing or camera presentation. |
| slot `+00` | 2 | Status. `$4000/$8000` high states are inactive/free; spell actors normally use active values 0 or `$0800` |
| slot `+02/+04` | 2+2 | World-space hot-point X/Y, updated by the spell handler and projected with camera `$22/$24` |
| slot `+06/+08` | 2+2 | Current per-tick X/Y velocity decoded by `$00:8E2F`; flip bits mirror the authored deltas |
| slot `+0A/+0C/+0E/+10` | 2 each | Current composition collision-header words after flip selection. Most actors use unsigned left/top/right/bottom distances, but the sword beam retains signed offsets (`$FFE0` occurs live); decode authored parts for presentation geometry instead of assuming this is always an unsigned rectangle. |
| slot `+12` | 2 | Primary per-frame handler dispatched by `$00:8915`. This is lifecycle identity as well as control flow: player `$08B2` uses `$97A6/$97C9/$97E4` for arrival and hands control to `$9832`; Bloodpool fireball flight uses `$BDF0`; a live lightning bolt transitions from `$BD36` to shared repeat handler `$8683` without becoming a new actor. Marahna's `$E047` orb/split fireballs, `$DE96` snake projectiles, and `$E483` boss electrical children use `$8661`; after the boss launches its ground charge the parent uses shared repeat handler `$8683`. The snake parent cycles through `$8661/$DF3E/$DF63`, while linked-lightning endpoints/children use `$8683`. Aitos lava fireballs use `$CFE3/$8661/$CFFE` for rise/wait/return states; its separate launched molten rocks use shared `$8661`. Minotaur axes and Ice Dragon ice balls use shared delay handler `$8661`. Flaming Wheel's visible body moves among delay, repeat, and boss-AI handlers, so its handler is deliberately not identity. Tanzara's admitted projectile tuples use `$8661`. |
| slot `+14` | 2 | Secondary handler or polymorphic spawn parameter. Do not treat it as a handler without validating the object type. |
| slot `+16..+18` | 2+**1** | Animation-table pointer: 16-bit address at `+16` then a **single BANK BYTE at `+18`** (`$07:C000` Fire/Stardust, `$07:C800` Aura/Light). **`+19` is a SEPARATE field, not the pointer's high half** — live Magical Fire reads `$3907` as a word there, so a 16-bit read of `+18` silently yields `bank \| next<<8`. This entry previously read "2+2" and that misreading is exactly what left the action-spell effects drawing nothing (bug-ledger.md §32). Corroborated by `$00:95F0`, which copies spawn-record bytes `+2/+3` into `$18`/`$28` as BYTES. |
| slot `+1A/+1C` | 2+2 | Animation state and entry index |
| slot `+1E` | 2 | Nested-dispatch resume value. Yield helpers store the JSR return address, so the next executed instruction is `value+1`: live fireball `$BDD9` resumes at `$BDDA`; live lightning `$BD69` resumes at `$BD6A`; Marahna's large orb/split and snake children retain `$E061/$A65D`, while its boss diagonal/ground children retain `$E578/$E57E`; the post-impact boss parent repeats through `$E4D7`. Aitos lava fireballs retain `$CFCD`; launched `$CEEC` molten rocks retain `$CF16`, distinct from stationary `$CF1C` mouths. Minotaur axes retain `$B008`, Ice Dragon balls retain `$F2CA`, and Tanzara's exact admitted families retain `$FBEA/$FBF5/$FC13/$FC21/$FCA1/$FCAF/$FCB5/$FCD6/$FCED/$FCFB/$FD22/$FD44/$FD77/$FD9E`. |
| slot `+20/+22/+24` | 2 each | Current composition pointer, visual ID, and animation wait counter |
| slot `+28`/`+29` | 1+1 (see note) | Attribute/transform. Masking the 16-bit read at `+28` with `$C000` selects the horizontal/vertical flip, which works because the bits live in the **byte at `+29`**; `+28`'s own byte measured `$00` on every spell actor observed, and `$00:95F0` writes `+28` byte-wise. Treat as two bytes rather than one word until a case is found that needs the low half. `+19` carries the same base attribute value as `+29`. |
| slot `+2A/+2C/+2E` | 2 each | Attack, HP, and BCD death-score value copied from spawn-record bytes `+7/+8/+9` by `$00:95F0`. |
| slot `+30` | 2 | Object flags. Bit `$0001` marks an attacker (including the player sword beam); bit `$0400` means outside the **currently selected activation window**, not necessarily outside the draw window or native viewport. With extended activation enabled it covers fitted camera `$22` plus live horizontal margins; during `$08B2=$97A6/$97C9/$97E4` it uses the reconstructed native 256px horizontal camera and authentic vertical `$24`. Object drawing is decided independently and may remain visible in the margins while `$0400` is set. The scene-effect observer keeps lifecycle identity but does not submit the object while the bit is set. |
| slot `+32` | 2 | Source/spawn-record pointer retained by ordinary action actors. Bloodpool trap lightning uses `$BD2A`, boss-lightning children use `$BDFF`, and the two fireball directions use `$BD76` and `$BD84`. Marahna orb/split fireballs retain `$E047`, snake enemies/projectiles retain `$DE96`, and the excluded reaper/orb family retains `$E0BA`; `$E2F3/$E304/$E315/$E326/$E351/$E368` are moving-platform roots. Its linked-lightning source endpoint/child retain `$E18E`, the partner retains `$E254`, and the boss electrical family retains `$E483`. Aitos lava fireballs retain `$CF9E` throughout their cyclic rise/wait/return phases; launched molten rocks and their stationary mouths retain the distinct `$CEEC` source. The original/Death Heim boss-family pairs are Minotaur `$AF5D/$F6CA`, Wizard `$BDFF/$F6E2`, Flaming Wheel `$D838/$F712`, Viper `$E483/$F72A`, and Ice Dragon `$F161/$F760`; Tanzara uses `$F80F`. Player sword-beam captures observed `$979A` and `$9810`; validate equality with the linked player's current source instead of hardcoding either. This is a useful slot-reuse discriminator, not a globally unique actor ID. |
| slot `+38` | 2 | Polymorphic spell-local counter. Controller `$0860+38` is selected spell ID; cohort spells reuse `+38` for repeat counts |
| slot `+3A` | 2 | Spawner backlink. The cast controller and player sword-beam child point to player `$08A0`; Bloodpool boss-lightning strike child `$08E0` points to boss `$12E0`, while its floor child `$0920` points to `$08E0`. Marahna split fireballs point to their retired `$E047` orb, snake fireballs point to their validated `$DE96` parent, linked-lightning children point to the first `$E18E` endpoint while the `$E254` partner occupies the next slot, and both `$E483` boss bolt stages point to boss `$12E0`. Death Heim's room owner is `$001C`; the Viper parent and the visible Flaming Wheel body retain it in their rematches. Minotaur axes and Ice Dragon balls instead point to a live parent with the same original/rematch source. The original Flaming Wheel body is root-owned (`0`); helper/child records have action-object backlinks and are rejected. Combined with `+32`, this validates linked families and remains stable while other control-flow fields change. |
| `$7E:00F4/$00F8/$00F9` | 2 each | Input-enable mask, cast-active gate, and cast-transition state used by `$9DE1-$9F10` |

Action-scene identities measured in runs `20260810-124203`, `20260810-163044`,
`20260810-174202`, the six-cycle correction run `20260810-180202`, and sword-beam run
`20260810-175403`, `20260810-184935`, `20260810-190012`, and
`20260810-190729`, plus Marahna run `20260811-151353` and Death Heim runs
`20260822-195453`/`20260822-195726`, combine those fields rather than matching
artwork alone:

| Kind | Positive live identity |
|---|---|
| Enemy fireball | `+12=$BDF0`, `+1E=$BDD9`, `+16/+18=$4000/$7E`, `+1A=$23`, and `(+22,+20)=($17,$45EF)` or `($18,$4610)`; `+32` is `$BD76` or `$BD84` |
| Lightning trap | `+32=$BD2A`, `+1E=$BD69`, `+16/+18=$4000/$7E`, `+1A=$14`, `+12=$BD36` or `$8683`, and `(+22,+20)=($1F,$46FE)` or `($20,$479D)`; the live vertical extents are `+0C=+10=$58` (88px each side) |
| Marahna fireball (`$18=$05`, `$19=$04-$07`) | `+32=$E047`, `+12=$8661`, `+16/+18=$4000/$7E`. The state-`$0C` orb cycle uses exact visual/composition/velocity tuples `$07/$451C/(0,0)`, `$08/$4528/(-1,0)` or `(-2,0)`, `$05/$4504/(0,0)`, and `$06/$4510/(+1,0)` or `(+2,0)`, all unflipped with extents `8/8/8/8` and resume `$E061`. Four split children point `+$3A` to the inactive parent in resume/state/visual/composition `$E0A6/$0E/$0C/$4597`, resume at `$A65D`, use extents `4/4/4/4`, and carry exact velocity/state/visual/composition/flip tuples `(0,+3)/$0F/$32/$4BCD/0`, `(-3,0)/$10/$33/$4BD9/0`, `(0,-3)/$0F/$32/$4BCD/V`, or `(+3,0)/$10/$33/$4BD9/H`. Source+resume plus bounded position continuity distinguishes same-slot reuse. `$34/$4BE5` is excluded moving-platform artwork. |
| Marahna snake fireball (`$18=$05`, `$19=$04-$07`) | Source `$DE96`, handler/resume/state `$8661/$A65D/$06`, animation `$7E:4000`, extents `8/4/8/4`, local counter `+38=6`, and exact pairs `$1D/$4869` or `$1E/$487C`. Velocity is `(-4,0)` unflipped or `(+4,0)` H-flipped. `+3A` must resolve to an active source-`$DE96` snake with matching H-flip, extents `16/24/16/24`, and exact wait/rise/fall lifecycle. Run `20260811-232640` proves the source-`$E0BA` reaper orb is a negative case. |
| Marahna lightning link (`$18=$05`, `$19=$04-$07`) | Child `+12=$8683`, `+32=$E18E`, `+1E=$E24F`, animation `$7E:4000`, no flip. Horizontal is state/visual/composition `$27/$2E/$4AA1`, extents `40/4/40/4`; vertical is `$28/$31/$4B82`, extents `5/40/5/40`. `+3A` must resolve to an active `$E18E` endpoint in state `$1A`, with an active `$E254` state-`$1D` partner in the next slot; endpoint visual/composition pairs are `$0D/$45B8` + `$0F/$45D0` horizontally and `$0E/$45C4` + `$10/$45DC` vertically. The child hot point must be their exact midpoint. |
| Marahna boss lightning (`$18/$19=$05/$08`) | Boss parent `+32=$E483`, animation `$7E:5000`, extents `48/40/48/8`, no backlink. With handler `$8661`, charge artwork is `$07/$57C2` or `$08/$5868`; the orb is `$0A/$59DE`. Launched child `+$3A=$12E0` uses resume/state/visual/composition `$E578/$04/$11/$5CE0`, velocity `(-4,+4)` with extents `32/0/0/32` and no flip or `(+4,+4)` with `0/0/32/32` and H-flip. After impact, the same child resumes at `$E57E`, state `$07`, and rides the floor at `(-4,0)` unflipped or `(+4,0)` H-flipped. Its complete loaded cycle is `$12/$5D01` with `8/8/8/8` extents, then `$13/$5D0D`, `$14/$5D2E`, `$13/$5D0D` with `16/16/16/16`. During that stage the backlink parent is the exact shared-repeat tuple handler/state/resume/visual/composition `$8683/$0A/$E4D7/$00/$5307`. |
| Aitos lava fireball (`$18/$19=$04/$01`) | `+32=$CF9E`, `+1E=$CFCD`, animation `$7E:4000`, no flip, extents `8/8/8/8`, and `(+22,+20)=($2A,$4D21)` or `($2B,$4D2D)`. Rising state `$22` uses `+12=$CFE3`, velocity `(0,-4)`; wait/reset state `$23` uses `$8661`, `(0,0)`; return state `$24` uses `$CFFE`, `(-1,+6)`. Source+resume plus bounded position continuity starts a fresh generation when a persistent slot relaunches at the pit. |
| Aitos molten rock (`$18/$19=$04/$01`) | `+32=$CEEC`, `+1E=$CF16`, `+12=$8661`, state `$27`, animation `$7E:4000`, artwork `$2B/$4D2D`, extents `8/8/8/8`; X velocity is `-2` unflipped or `+2` H-flipped and measured Y is `-1..+1`. Stationary mouths use resume `$CF1C` and are excluded. |
| Boss lightning (`$18/$19=$02/$08`) | Base identity `+32=$BDFF`, `+12=$8661`, `+16/+18=$5000/$7E`, no V-flip, and `+3A` resolving to an active `$BDFF/$7E:5000` parent. Strikes: states/visuals/compositions `$02/$00/$5346`, `$03/$01/$5401`, `$04/$02/$5492` are vertical long/medium/short; `$05/$03/$54F2`, `$06/$04/$55C2`, `$07/$05/$5661` are diagonal long/medium/short. Normal left/top/right/bottom extents are `6/83/11/117`, `6/83/11/69`, `1/83/11/21`, `48/83/8/117`, `36/83/8/69`, `30/83/8/21`; H-flip swaps left/right. `$20/$5D2B` is the blank half-cycle and is not decorated. Observed strike resumes `$C02B/$C04B/$C051` are control flow, not shape identity. Floor impact is state/resume `$09/$C06A`, pairs `$08/$570A`, `$09/$5716`, or `$0A/$5729`. |
| Death Heim Wizard lightning (`$18/$19=$07/$03`) | Same exact `$7E:5000` child states, artwork, extents, handler, resumes, and parent-validation contract as Bloodpool, with the owning source changed consistently from `$BDFF` to `$F6E2`. The room gate accepts both the original and rematch pair; mixed-room or mixed-source tuples fail closed. |
| Minotaur axe (`$01/$04` or `$07/$02`) | Original/rematch source `$AF5D/$F6CA`, handler/resume `$8661/$B008`, animation `$7E:5000`, state `$03`, and backlink to a live same-source parent. Visuals `$00-$07` map exactly to compositions `$50FB/$5138/$5159/$5196/$51B7/$51F4/$5215/$5252`; the small terminal frame is `$10/$59B0` with 4px extents. |
| Flaming Wheel (`$04/$07` or `$07/$05`) | Visible body uses source `$D838` with root backlink `0`, or `$F712` with Death Heim room-owner backlink `$001C`; both require active boss flag `$4000`, animation `$7E:5000`, and nonzero composition. Handler is intentionally excluded because the same body moves among delay, repeat, and AI handlers. Same-source helpers carry action-object backlinks and fail closed. |
| Death Heim Viper lightning (`$18/$19=$07/$06`) | Same charge/orb/bolt/ground tuples as Marahna `$05/$08`, with source `$F72A` replacing `$E483` consistently on parent and children. The rematch parent retains backlink `$001C`; diagonal and floor children retain their normal parent link. |
| Ice Dragon ball (`$06/$08` or `$07/$07`) | Original/rematch source `$F161/$F760`, handler/resume `$8661/$F2CA`, animation `$7E:5000`, and backlink to a live same-source parent. Visuals `$12-$15` are state `$19` with compositions `$5D9C/$5DA8/$5DB4/$5DC0`; visuals `$16-$19` are state `$1A` with `$5DCC/$5DD8/$5DE4/$5DF0`. |
| Tanzara projectile (`$07/$08`) | Source `$F80F`, handler `$8661`, animation `$7E:5000`, and an exact allowlist of 50 resume/state/visual/composition tuples covering the observed projectile families. The tuple table is authoritative in `src/action/action_effects.c` and its regression fixture; unlisted boss-body or helper artwork is rejected. |
| Wall torch | Not an action slot. Bloodpool uses exact BG1 pair `$47` over `$4F` throughout `$18=$02`, anchored at `(8,15)` in the 16×32 pair. Marahna maps `$05/$04-$08` use one complete `$43` metatile anchored at `(8,11)` in its 16×16 cell; Death Heim Viper room `$07/$06` reuses that same authored `$43` rule. All use the shared bounded map view; Marahna limits publication to a 256px camera margin because `$04-$07` share 31 torches; original boss map `$05/$08` has ten in its separate 512×512 map. |
| Aitos lava pit (`$18/$19=$04/$01`) | Not an action slot. Exact BG1 signature is `$DC`, one-to-six `$DD`, then `$DE`, over equally wide `$DF` and (when map height permits) `$E7` bubbly rows. Observed 64px rims begin at world `(1648,976)`, `(1888,992)`, and `(2144,976)`; the 128px rim begins at `(3616,928)`. Capture publishes the full bubbly volume on BG1 within a 256px camera margin. |
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
| $7E:0A00+`+22` | 2 | **Wait counter**, written by actor-script command `$09` (`$01:CE5F`): the command fetches two script bytes, assembles a 16-bit value, `STA $0022,X`, and sets state 2. Decrements once per game frame alongside `+00`. Observed 1..76 on staged eruption records — and the script that drives record `$0FA4` opens `09 4C 00`, i.e. wait $004C = 76, exactly the value seen. It does not encode the landing row; the `$03` run that follows does. **It is the only live source for a wait already in progress**, because `$01:CE5F` advances the cursor past the `$09` before the countdown starts: walking the script from the cursor sees no wait at all, so a wait's remaining frames exist nowhere else. The eruption presentation reads it for exactly that reason — see [SEAMS](SEAMS.md). |
| $7E:0A00+`+14/+16` | 2+2 | **Actor-script base and cursor.** `$01:CFC7` fetches the next command byte and post-increments `+16`; the bank it fetches from is selected by the class byte `+$0E & $00FF` — **zero reads `$7F:0000,X` (RAM), non-zero reads `$0A:0000,X` (ROM)**. Townspeople are class 0 and run generated RAM scripts; the eruption is class `$01`, so its scripts are **static bank-`$0A` ROM data** and are decodable offline. `$7F` = end of script (`$01:CD35` branches to `$B891`); anything else indexes the 18-entry command table `$01:CD6F`. |
| $7E:0A00+`+1E` | 2 | Per-command step scale/duration written by the command handlers — cmd `$03` sets `$0010`, and one branch of cmd `$04` sets `$0002` after scaling `+1A/+1C` by 8. With cmd `$03`'s `+1C = +1` this yields the measured 16 map pixels of descent per command. |
| $7E:0A00+`+12` | 2 | Masked `& $7FFF`, the **state** index inside that class's own table. `(class $12, state 6)` is the Blue Dragon's 33-frame building strike. sim3d keys presentation height on the `(class, state)` pair. |
| $7E:0F0C-$1016 | 8 × $26 | The volcanic eruption story event (fires once a town's region has no lairs left), mapped 2026-08-18. All eight records carry `+0E=$0E01` and run one of three consecutive spawn scripts (`$01:A853`/`$A857`/`$A85B` in `+06`). Positive live identity: staged = `+08=$E7D0`, `+1C=0`, `world_y=-16`, `+22` counting down; crater jet = `+08=$E7D0`, `+1C=-8`, fixed map column (144 in Aitos); falling = `+08=$E7A6`, `+1C=+8`, constant column, `world_y` rising 8 a tick from -16; landed = `+08=$DD9F/$DDA5/$DDAB`, `+1C=0`. Measured fall ranges are 80-368 map pixels to fourteen distinct landing rows (64..352, all multiples of 16). |
| $7E:0AE4 | $26 | Angel world record (index 6), class `$0C`. Its class handler `$B904` is a no-op because another subsystem drives it. Identify the angel by this address plus class — the `$A627-$A792` pose compositions are also borrowed by miracle effect records. |
| $7E:0B0A | $26 | Dedicated angel-arrow world record (index 7). `$01:B41A` state-dispatches idle/spawn/move through `$B423`; movement `$B44B` applies velocities `+1A/+1C`, and `$B473` returns carry set when the projectile should be released. |
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
| $7E:00D7-$00DD | 7 | VRAM DMA descriptor slot 1 = tile-anim upload. Action/town `$02:BC56` uses `[$D9]:$D7 = $7F:B800+n*$E1`; world navigation's `$02:AF86` instead fixes bank `$0A`, with `$D7 = $B000/$B040/$B080/$B0C0` for the four water frames. `$D7` remains after `$DC` is drained, so the host-owned map can synchronize phase without reading VRAM. |
| $7E:00DE-$00E1 | 1+1+1+2 | tile-anim: tick period mask / frame count-1 / frame index / frame stride (bytes); $FF/$FF/-/0 = disabled |
| $7E:00F1 | 1 | one-shot flag: re-stream BG3 map rows 4-26 ($7F:B100 -> VRAM $5880) next NMI |
| $7F:B000-$B6BF | 1728 | HUD/BG3 tilemap compose buffer (rows 0-3 streamed every frame to VRAM $5800; rows 4-26 on $F1) |
| $7F:0000-$1FFF | 8192 | **Full town BG1 tilemap**, the whole 64x64-tile (512x512 pixel) town, not just the on-screen window. Quadrant-paged: `$03:9B5A/$03:9C43` write each cell's 2x2 tile block at `quadrant*2048 + (cellY & 15)*128 + (cellX & 15)*4`, four words at `+$00/+$02/+$40/+$42`, using terrain/structure definitions respectively. Both HLE wrappers and bridge-side rendering share `ActRaiser_CopyTownMetatile`. Row stride is 32 tiles, quadrant stride 32x32 tiles. A row-major read looks like an unrelated layer — it was mistaken for BG2 twice before `$9C43` was disassembled. This is the authoritative displayed cell artwork across staged construction and Marahna's water-to-land event; the semantic `$7F:2000` value can lead the visible redraw, so presentation observes this range plus live VRAM/CGRAM rather than reconstructing the image from cell ids |
| $7F:1000-$1FFF | 4096 | (Within the above.) The lower two quadrant pages happen to be the range the graphics orchestrator streams to VRAM; SEAMS' "BG tilemap → VRAM" row describes that upload, not a separate buffer |
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

## Action terrain collision (mapped 2026-08-02 — SEAMS "Content / randomizer seams" §5b)
| Address | Size | Description |
|---------|------|-------------|
| $7E:0014 / $7E:0016 | 2 each | Collision probe inputs: tile X / tile Y in 16px units, consumed by the oracle `$00:91C3`. (Also reused as generic DP scratch elsewhere — see DEBUG.md §0) |
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

### Population (two-byte entries, BCD format)
| Address | Size | Description |
|---------|------|-------------|
| $7E:0218 | 2 | Total population |
| $7E:021A | 2 | Most recently visited town |
| ... | 2 each | Individual town populations (Fillmore→Northwall) |
| $7E:021C+2N | 2 each | ↑ the individual populations are recomputed by the structure census `$03:C07F`: sum of per-house people by civ level, +2, − `$7F:9F57+2N` — population is derived from standing house records (SEAMS town §7) |

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
| $7E:0295 | 2 | Magic points — PERSISTENT copy. `$21` is the act/working copy, loaded from here at `$02:84E0` (`LDA $0295; STA $21`); act-mode pickups INC only `$21` ($00:887E); sim reward grants INC BOTH via long addressing (`$01:9CD6`, bug ledger §18b). New-game STZ at $02:BE69. No other direct writers in ROM — stats-block writes use `AF/8F`-form long addressing |
| $7E:0297 | 2 | Population needed for next level |
| $7E:0299 | 9 | Magic inventory |
| $7E:02A2 | 9 | Offerings inventory |
| $7E:02AB | 1 | Number of lives (max-HP-style grant handler `$01:9CBD` INCs it) |
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
24 entries (6 towns x 2 acts x 2 bytes each).

## Temple & Gameplay State

| Address | Size | Description |
|---------|------|-------------|
| $7E:033E | 1 | Temple action (0x00=Give Oracle, 0x01=Listen, 0x02=Take Offering) |
| $7E:0334 | 1 | **Current song id** (RE-IDENTIFIED 2026-07-16 — earlier "Death Heim/ending state" reading was a misread of music state). The `[$A2]`-script song-change handler `$02:B64B` compares it to skip redundant reloads; written by ~10 play sites (`$00:828F/A370/F650/FF04`, `$01:8602/8754/8856`, `$02:8345/BD2A`, `$03:8262`); zeroed by the transition music-stop at `$00:83F2`. The old observations still hold reinterpreted: `$00:FEFC` "sets 1" = plays song 1 at final-boss teleport-out, `$00:F650` "sets 3" = starts song 3 after the returning `0701` sky fade-in — so it was always too late/irrelevant for the black-frame BG page swap (`$00:F5F0-$F619`, BG1SC/BG2SC `$64/$74`) |
| $7E:0341 | 1 | Active world-location ID, 1-7. `$01:B6CA` first clears it, then writes the entry whose 256x256 source-pixel region from ROM table `$01:B73C` contains the `$0300/$0302` focus; zero therefore means outside every town border. The location label and 3D navigation clear-region mask consume the same value; zero keeps the full world hazed. Also read by `$00:A375` as the pending post-Death-Heim destination |
| $7E:0347 | 1 | Death Heim boss-rush progress: `$00:FEEC` writes `$19 - 1` after each boss (hub stager `$F3D4` warps to `$0347+2` next); 0x07 = final boss beaten |

## Debug / System

| Address | Size | Description |
|---------|------|-------------|
| $7E:035A | 1 | Music/event request port: COP vector `$00:8526` stores A's low byte here (`LDA #id; COP`). Consumed by the NMI tail `$02:AC33` every other frame as the LOW byte of one 16-bit load, forwarded to APU port `$2142`, then zeroed (see SEAMS "APU port-0 command protocol") |
| $7E:035B | 1 | SFX request port: BRK vector `$00:852F` stores A's low byte here (`LDA #id; BRK`). Forwarded as the HIGH byte of the same 16-bit NMI store to APU port `$2143` and zeroed together with `$035A`. **id `$00` = idle/clear, not a sound** — it is by far the most-written value (754 posts vs 12 key-ons in one session, mostly from `$03:9E6B`), and the NMI forwards zero as "nothing pending". Ids observed in play so far: `$03 $08 $0C $0D $10 $18 $1E $1F $24` (callers + sample/pan per id in [research-symbol-map](research-symbol-map.md#audio-and-spc-transport), captured via `AR_SFXCENSUS=1`) |

## High RAM ($7F:0000+)

### Graphics & Map Data
| Address | Size | Description |
|---------|------|-------------|
| $7E:4000+ | varies | Per-act decompressed ordinary-object animation/composition blob. Loaded by `$02:B69C` only at act-entry maps and inherited by later maps in the same act. Bloodpool scene composition pointers `$45EF/$4610/$46FE/$479D`, Marahna orb pointers `$4504/$4510/$451C/$4528`, snake-shot pointers `$4869/$487C`, split/link pointers `$4597/$4BCD/$4BD9/$45B8/$45C4/$45D0/$45DC/$4AA1/$4B82`, excluded reaper-orb pointers `$47E5/$4806/$4827/$4848`, excluded platform pointer `$4BE5`, and Aitos `$4D21/$4D2D` are addresses inside this mutable WRAM blob, not ROM symbols. Marahna boss-room pointers `$57C2/$5868/$59DE/$5CE0/$5D01/$5D0D/$5D2E` live in its separate `$7E:5000` bank. |
| $7E:5000+ | varies | Per-map decompressed boss animation/composition blob selected by the same asset-script command with nonzero destination flag. Aitos boss-volley visuals `$20/$21/$23` resolve to mutable WRAM compositions `$56BE/$56D8/$56FE` in run `20260812-000613`; like every loaded pointer, these addresses identify artwork only inside the validated map/source lifecycle. |
| $7E:6000-$7E:7FFF | 8KB | Shared action character/metatile decompression workspace and persistent-raster table storage. R1-R6/R8 use `$6000`, R7/R10 BG1 use `$6800`, R9 uses `$7000`, and R10 BG2 uses `$6000`; untouched bytes can remain presentation-visible. |
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

### Structure records & town capacity (mapped 2026-07-17, SEAMS town §7)
| Address | Description |
|---------|-------------|
| $7F:3800-$7F:53FF | Per-cell flag maps, `$400` per town (32×32 cells; bit0 set at road/build commit `$03:9623`, bit1 at `$03:8E48`, transient pathfinder visited bit2 set at `$03:9A50` and tested by the `$03:96EF` HLE) |
| $7F:6B26+2N | Per-town population **support capacity** (census `$03:C07F` sum: 32/48/72 per completed support structure, bridges 32) |
| $7F:6BE7-$7F:77E6 | Per-town **structure-record arrays**, `$200` each (base = `word[$03:DC74+town*2]`): 128 × 4-byte records `{cell X, cell Y, flags/type, action/progress}`. Flags byte: bit7 active, **bit6 not-yet-contributing / per-class visual variant — NOT a construction flag** (the allocator never sets it; on a class-3 windmill it is the "no wind" story state — see SEAMS town §7 and ledger §61), bits 4-5 subtype (house civ level / wheat `$10` / bridge orientation), low nibble type class (0 house, 1 bridge, 2 field, 3/4 factory tier). Allocator `$03:9D9F`; the 128-slot exhaustion is the game's 128-structure cap |
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
nibble 7 with `+2` still `$80` means the handler was never reached or never completed
(bug-ledger §22). `$03:A477` is the sibling free handler with the same tail.

To watch record transitions live, trace the array directly (flat offset = `0x10000 + $7Fxxxx`):

```sh
AR_WRAM_TRACE=structrec.jsonl AR_TRACE_LO=0x16BE7 AR_TRACE_HI=0x16DE6 \
  ./build-release/ActRaiserRecomp ar.sfc
```

(that range is town 0 / Fillmore's 128 records; shift by the town's `$03:DC74` base for others.)
| $7F:7BF9 / $7F:7BFB | Current town id / town id ×2 (index into `$03:DC74`) |
| $7F:7C05 / $7F:7C07 | Census/scan accumulators (housing cap, support cap; allocator slot index) |
| $7F:7C11/13/15/17 | Record-scan rectangle X0/Y0/X1/Y1 (cell coords) |
| $7F:7C1D | Record-scan remaining counter |
| $7F:7C9D/$7C9F/$7CA1 | Pending allocation request: cell X, cell Y, type byte |
| $7F:90E1/$90E5 | Miracle aimed map cell X/Y (`$96EA/$96EC >> 4`); `$90E3/$90E7` = square-aligned copies |
| $7F:90E9 | User-miracle operation active; set by `$01:97E5`, cleared after the full effect/menu cleanup. Posted enemy/script effects use `$90F5` instead |
| $7F:90EB | Active miracle kind: 0 silent clear, 1 Lightning, 2 Rain, 3 Sunlight, 4 Earthquake, 5 Wind |
| $7F:90F1/$90F3/$90F5 | Lightning/Rain visual actor finished; overall effect complete; posted/scripted-driver marker |
| $7F:90F7 | Structure-visual refresh flag set by `$03:B274` |
| $7F:9218/$921C/$923E | Wind remaining ticks (120); Earthquake remaining ticks (180); Sunlight phase (0-140) |
| $7F:9250-$7F:954F | Per-town built-square lists, `$80` each: 64 × 2-byte **square**-coord pairs (x,y ≤ 7), `$FFFF` = empty (append/dedup `$03:8EC1`; staged pair `$9550/$9552`; cursors `$9554+`; persisted at SRAM `0x0300+town*0x80`) |
| $7F:96E8/$96EA/$96EC | Miracle/effect post: kind, pixel X, pixel Y, consumed by master-loop `$03:820F`; Skull Head uses it to request shared Earthquake kind 4 |

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
| $7F:9101 | World-state flags. `$02:865C` tests bit 0 before preserving an 8x8 block at world-tilemap offset `$0660`; when clear, that block is zeroed before town development is stamped. `$01:B6CA` also uses bit 0 to decide whether its location scan includes the seventh (Death Heim) rectangle. Bit 1 remains Death Heim-related but is not consumed by the host world-map composer. |
| $7F:910B | Bloodpool's story-event **prereq** bitmap, byte 0 (= `$9107 + 1*4`). The PAR-derived "bridge technology (bit 0x20)" label is event id 2 of that town — see the event-bitmap table below |

### Story-event bitmaps ($7F:9107-$7F:914E, corrected 2026-08-17 — SEAMS story-event VM)

Three parallel per-town arrays, 4 bytes = **32 event ids** per town, base pointers in ROM at
`$03:DCA2`/`$DCAE`/`$DCBA`. Bit order is **MSB-first**: id `k` → byte `k>>3`, mask
`$80 >> (k&7)` (mask table `$03:F4D7`). Helpers `$03:F46E` test / `$F479` set / `$F487` clear,
resolver `$F497` (scratch `$7F:914F`).

| Address | Description |
|---------|-------------|
| $7F:9107 + town*4 | **prereq/enabled** — the event may be selected. Persisted at SRAM `0x120E` |
| $7F:911F + town*4 | **fired** — already run; never selected again. Persisted at SRAM `0x1226` |
| $7F:9137 + town*4 | **dispatched this session** (set by `$03:E02B`/`$E0B0`); not persisted |
| $7F:914F | scratch byte holding the resolved bit mask (`$03:F497`) |

These were labelled "open lairs"/"spawned lairs" until 2026-08-17. Monster-lair state is the
separate `$7F:9568+` block above; these bits are consumed by the `$03:DFFB` event selector and
by every town-event handler in `$03:E6xx-$F3xx` (32 ids/town matches the 32-entry handler
tables at `$03:E66E`, not 4 lairs/town).

### Story-event and scenery-spawner state ($7F:9202-$7F:9228, mapped 2026-08-17 — SEAMS town §8)

| Address | Description |
|---------|-------------|
| $7F:9202 | event-selector scan cursor, `0..$1F` (`$03:E015` loop) |
| $7F:920E | pending-event latch. Bit 7 set = "run id `& $7F` next"; `$03:DFFB` strips the bit and returns the id. Handlers self-latch, e.g. `$03:EFE5` writes `$87`, `$03:FAE1` writes `$88` |
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

### Town Growth Points ($7F:9EFA+)
Two-byte entries per town tracking accumulated growth from monster defeats and lair seals.

### Road Construction Encoding ($7F:6800+)
- One word per 8×8 selector square, `$80` bytes per town (`$300` total, all six
  towns at `$6800+town*0x80`); initialised from ROM `$03:DCFA`, persisted at
  SRAM `0x0000+town*0x80` (save-format §3.4)
- Bit 0x40: Obstructs building direction selector
- Bit 0x80 / 0x100: river-crossing bridge state per axis — set when the bridge
  builders `$03:9985/$99CA` allocate a bridge record, checked so a crossing is
  never re-bridged (SEAMS town §7)
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
