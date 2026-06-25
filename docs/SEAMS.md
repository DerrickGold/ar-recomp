# ActRaiser — Logic ↔ Hardware Seams (HAL inventory)

This is the **living inventory of boundaries between game logic and SNES hardware** — the seams
we will eventually widen into a platform interface (HAL) to allow enhanced/custom graphics and
audio that exceed SNES limits. See `DEBUG.md` and the §"future" discussion for the rationale.

**Discipline (read once):** this is captured *opportunistically* while debugging — record a seam
only when you already understood it chasing a bug. **Do NOT go on documentation expeditions, and
do NOT design HAL signatures yet.** Correctness of the recomp comes first; the boundary isn't
stable enough to abstract until the game runs.

**The two columns that matter** are **Intent** (what the logic is *trying* to do) and **Logical
ID / table** (the index/pointer that carries asset identity — the future HAL's vocabulary and the
asset-substitution point). The *hardware* column is mechanically recoverable later; intent and
identity are the perishable, expensive-to-rederive parts.

**Status legend:**
- 🟢 **HOOKABLE** — already a clean semantic seam (an ID/event). Good HAL candidate; easy to
  intercept and reroute to an enhanced backend.
- 🟡 **CHOKE** — funnels through one runtime function; interceptable but the payload is still
  hardware-encoded (needs some decode to reach intent).
- 🔴 **LOW-LEVEL** — intent is entangled in raw hardware writes; surfacing it needs asset-pipeline
  decomp.

---

## Audio  (closest to a clean interface — start here)

| Seam | Routine / address | Hardware | Intent | Logical ID / table | Status |
|---|---|---|---|---|---|
| Music / event | `LDA #id; COP` → `$035A`; COP vector `$FFE4→$8526`; hook `ActRaiser_CopHook` | APU ports `$2140-43` | "play music / fire event N" | event/song id (A → `$035A`) — **song table TBD** | 🟢 |
| Sound effect | `LDA #id; BRK` → `$035B`; BRK vector `$FFE6→$852F`; hook `ActRaiser_BrkHook` | APU ports | "play SFX N" | sfx id (A → `$035B`) | 🟢 |
| SPC driver/sample upload | `RtlUploadSpcImageFromDp` (HLE; src ptr at DP+`$A5`, ActRaiser's `LDA [$A5],Y`) | APU ports + ARAM | "upload sound driver + BRR samples to APU" | DP+`$A5` source block; resident IPL uploader in ARAM `$0F0E` | 🟡 |
| Raw APU port write | `RtlApuWrite` (`$2140-$2143`) | APU I/O | low-level handshake / param | — | 🔴 |

> Audio is the highest-payoff first HAL target: the `$035A`/`$035B` events are already ID-based.
> **Next capture:** the song-id → ROM table mapping (which id = which track), so an enhanced
> backend can map id → modern audio. Found while fixing the boss-music handshake (memory:
> `spc-upload-dp-pointer-fix`, `cop-syscall-hook-fix`, `post-boss-four-issues`).

---

## Graphics / PPU  (low-level; intent lives in the *loaders*, not the *draws*)

| Seam | Routine / address | Hardware | Intent | Logical ID / table | Status |
|---|---|---|---|---|---|
| Per-frame display build / DMA | NMI path; `ActRaiserDrawPpuFrame`; DMA descriptors in ZP `$D0-$D5` | VRAM/OAM/CGRAM DMA, `$2100`-bus | "blit this frame's tiles/sprites/palette" | DMA descriptor tables | 🔴 |
| Sprite (OAM) build | object loop `$8915` → OAM | OAM | "place object's sprite" | `$06A0` object struct (X/Y/handler) | 🟡 |
| BG mode / layers | `$2105` BGMODE, `$212C/2D` main/sub, scroll regs | PPU | "set up background layers/scroll" | — | 🔴 |
| HDMA (raster fx, HBlank) | HDMAEN `$420C`; ActRaiser drives ch 2/3 (and others) | HDMA | "per-scanline effect" | HDMA tables | 🔴 |
| Mode 7 (overworld / transitions) | m7matrix; `$2134` multiply; the act-select spin | PPU mode 7 | "rotate/zoom the world map" | m7 transform params | 🔴 |
| Brightness / forced blank | `$2100` INIDISP | PPU | "fade in/out, blank during build" | — | 🔴 |
| Palette load | CGRAM writes | CGRAM (15-bit ×256) | "set palette N" | **palette id / table TBD** | 🔴→🟡 |
| Sprite-sheet / tile load | ROM→VRAM copy loaders (**TBD — not yet located**) | VRAM DMA | "load anim sheet / tileset" | **sheet/frame index — the key asset-identity seam** | 🔴 |

> **The asset-substitution seam is the loaders, not the draws.** When you find the routines that
> copy graphics ROM→VRAM and select animation frames, capture the table index they use — that
> index is the logical sprite/tileset identity. (Not yet located; F2 snapshots dump VRAM/CGRAM/OAM
> when hunting these — see `DEBUG.md` §9.)

---

## Frame / timing  (mostly already HLE'd — the model is understood)

| Seam | Routine / address | Hardware | Intent | Notes | Status |
|---|---|---|---|---|---|
| VBlank wait | `$00:8418`, `$02:A85E` (HLE → `ActRaiser_WaitForVblank`); `$01:9284` (inline) | RDNMI `$4210` | "wait one frame" | RDNMI modeled as once-per-frame token; inline waits spin-detected (`snes.c`) | 🟢 |
| NMI handler | `$8520` (`NmiHandler`) | NMI | "per-frame vblank service" | game frame `$0088` bumped here | 🟢 |
| Frame coroutine | `RunOneFrameOfGame` (`actraiser_rtl.c`) | — | host frame ↔ game frame mapping | coroutine yields at vblank-wait | 🟢 |

---

## Input

| Seam | Routine / address | Hardware | Intent | Status |
|---|---|---|---|---|
| Joypad read | auto-joypad enable `$4200` bit0; `$4218-$421F` | controller | "read player input" | 🟡 (TBD: where logic consumes it) |

---

## Game-state anchors (not hardware seams — symbol map for everything else)

These RAM addresses recur across all the debugging; keep `ram-map.md` authoritative.

| Addr | Meaning |
|---|---|
| `$18`/`$19` | game-mode byte(s) (act index / sub-mode); `$18==01` = action stage, `$27` = transition |
| `$1A`/`$1B` | transition dest / sub-state |
| `$1D` | player HP |
| `$E6`/`$E7` | action-stage timer (BCD) |
| `$0088`/`$0089` | game-frame counter (16-bit) |
| `$06A0` +stride `$40` | object table (≥64 slots; fields in `DEBUG.md` §11) |
| `$08A0` | player object (slot 8) |
| `$7D1B` | saved stack ptr (act→sim transition stack relocation) |

---

## Gameplay / Tunable seams  (cheats, rebalance, mods)

A second class of seam: the **value-clamp and mechanic-intercept points** where game logic reads/
writes a tunable parameter or makes a gameplay decision. Hooking these enables infinite health,
moonjump, sword reach, score forcing, etc. — the gameplay analog of the AV HAL above.

**Two hook kinds:**
- 🅥 **VALUE** — a RAM/SRAM byte/word; hook = freeze/clamp/force it (e.g. infinite health = pin
  the HP byte). Easiest; just needs the address.
- 🅒 **CODE** — a routine/constant that *computes* a mechanic (sword reach, jump velocity, damage);
  hook = intercept the routine or patch the constant. Needs the code site located.

**Discipline:** the addresses below marked **TBD** are NOT yet found — do NOT invent them. Each row
carries the **discovery method** so we capture it the moment debugging takes us through it. Only
promote a row to a real address once confirmed (a wrong cheat address is worse than a TODO).

| Seam | Where (RAM / routine) | Mod use | Kind | Status / how to find |
|---|---|---|---|---|
| Player HP (current) | `$1D` | infinite health (pin), god-mode | 🅥 | **KNOWN** (`$1D`, from B127 debugging). Infinite = clamp `$1D` in a per-frame hook or freeze the damage writer. |
| Player max HP / bar size | TBD | bigger/smaller health bar | 🅥/🅒 | find the HP-init constant (new-game / stage-entry sets `$1D` to max) — `AR_WATCHOBJ`/`AR_WATCH16` on `$1D` at stage start; the writer's immediate is max. |
| Player damage taken (per hit) | routine that writes `$1D` down | invuln, harder/easier | 🅒 | the damage applier — `AR_WATCH16`/watch `$1D` decreasing on a hit → names the routine + the per-hit amount. |
| Player lives | TBD (RAM or SRAM) | infinite / set N lives | 🅥 | watch the lives display value, `AR_WATCH16` on it; the death routine decrements it. |
| Player sword damage (dealt) | routine that subtracts enemy HP | one-hit kills, weak sword | 🅒 | `AR_WATCHOBJ` on an enemy slot's HP field while you hit it → the writer is the damage routine; the amount is its operand. |
| Player sword length / reach | TBD (hitbox/collision calc) | double reach | 🅒 | the sword-vs-enemy hit test — the attack hitbox extent (a constant offset from player X). Hardest (geometry); find via the attack-frame collision routine. |
| Player jump velocity / gravity | player object `$08A0` velocity field (offset TBD) | moonjump, low-grav | 🅒/🅥 | the jump-button handler sets a Y-velocity in the player object; gravity decrements it per frame. Find the Y-vel field (`AR_WATCHOBJ` on `$08A0` during a jump) + the jump-init routine. |
| Boss HP / health bar | boss object slot HP field (offset TBD) | set boss HP, instant-kill | 🅥/🅒 | boss HP lives in the boss object's slot (we have `saves/act1-boss*.bin` snapshots). `AR_WATCHOBJ` on the boss slot while damaging it → the HP field + the boss-damage writer. |
| Enemy HP (general) | object slot HP field (offset TBD) | — | 🅥 | same as boss — a per-object HP field in the `$06A0` table (offset not yet mapped). |
| Act score / population | TBD (likely SRAM, per-act) | force score thresholds → sim gating | 🅥/🅒 | the routine that compares act score/population to a threshold to gate sim-mode progression — `AR_WATCH16` on the displayed score; find the threshold-compare site. |
| Action-stage timer | `$E6`/`$E7` (BCD) | freeze timer / infinite time | 🅥 | **KNOWN** (`$E6`/`$E7`). Freeze = pin in a per-frame hook. |

> **Anchor:** most player mechanics (HP via `$1D`, jump/velocity, position) hang off the **player
> object `$08A0`** (slot 8 of the `$06A0` table) and the action-engine ZP. As we map the player
> object's fields (velocity, state, invuln timer), add them to `ram-map.md` and promote the TBD
> rows here. The object **HP field offset** (boss/enemy/player) is the single highest-value unknown
> — find it once and it unlocks boss-HP, enemy-HP, and damage seams together.

---

## Function roles discovered (decomp groundwork)

Capture the *role* of a routine when you understand it — names are perishable. These are NOT yet
renamed in the cfg (see below); this is the candidate list.

| Address | Role |
|---|---|
| `$00:8000` `ResetHandler` | reset / boot (named) |
| `$00:8520` `NmiHandler` | per-frame NMI service (named) |
| `$00:8525` `IrqHandler` | IRQ (named) |
| `$00:8915` | object loop — dispatch each active object's `$12` handler |
| `$00:8526`/`852F` | COP / BRK syscall entry (audio events) |
| `$00:9557` | spawn dispatcher (reads `$18`, indexes per-act handler table at `$95DD`) |
| `$03:9156` | **act→sim transition handler dispatcher** (relocates stack to `$1FFF`, RTS-trick chain through `$9B22`/`$9B4A`/`$9195`) |
| `$03:8053` | per-frame act→sim transition init (sequence of `JSR`s incl. `$9156`, `$AC8E`) |
| `$03:AC8E` | transition state-machine step (counter loop, calls `$97B0`) |

---

## Symbol renaming — mechanism & convention

**How to rename a function:** the cfg `func` directive's **first argument is the emitted symbol
name** (that's how `ResetHandler`/`NmiHandler`/`IrqHandler` got real names instead of
`bank_00_8000`). So in `recomp/<bank>.cfg`:
```
func ActSimTransition_Dispatch 9156 entry_mx:0,0
```
names the generated function `ActSimTransition_Dispatch` instead of `bank_03_9156_*`. Changing a
cfg requires **regen + rebuild**.

**RAM symbols:** the emitted code uses raw offsets (`cpu_read8(0x7E, 0x18)`), so RAM "renaming" is
documentation only for now — keep `ram-map.md` authoritative. (Symbolizing RAM accesses in the
emitted C would need an emitter change — a *future* nicety, not now.)

**Convention (proposed):**
- Functions: `Subsystem_Verb` PascalCase — `ActSimTransition_Dispatch`, `ObjectLoop`,
  `SpawnDispatcher`, `Audio_PlayMusic` (HLE wrappers already follow this: `ActRaiser_WaitForVblank`).
- Only rename what you've *confirmed* the role of. A wrong name is worse than `bank_03_9156`.

**When to do a rename pass:** rename *after* a fix is confirmed working, not bundled into the same
regen — so a rename can't be confused with a behavior change if something breaks. (E.g., hold the
`$9156`/chain renames until the `$9195` transition fix is verified.)

---

*Living doc — append seams as you cross them. Keep it honest: intent + logical ID are the point;
the hardware column is the easy part.*
