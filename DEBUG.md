# ActRaiser Recomp — Debugging Guide

This document is the single reference for **how to debug this static-recompilation port**.
It lists every diagnostic tool, what it's for, and — most importantly — **which tool to reach
for given a symptom**. Read the Decision Guide first; the rest is reference detail.

All `AR_*` env vars are read by the recomp binary (`build/ActRaiserRecomp`). All `SNESREF_*`
env vars are read by the oracle (`tools/oracle/snesref`). Almost every tool is **env-gated and
inert by default**, so it's safe to leave the instrumentation in the build.

---

## 0. Mental model (read once)

- The recompiler emits **one C function per (65816 function × M/X flag state)**, named
  `bank_BB_PPPP_MmXn` (e.g. `bank_02_B90D_M0X0` = entry m=0, x=0). Calls are **dispatched** by a
  `switch` on the runtime `(m_flag,x_flag)`.
- **The ROM is correct.** Every bug is in *our* generated code or *our* hand-written runtime.
  Always compare recomp behavior against correct 65816 semantics / the oracle.
- The recurring failure mode is a **misdecode**: the same ROM bytes decode to *different*
  instructions depending on the M flag (e.g. `A9 80 14` = `LDA #$80; TRB $93` at m=1 but
  `LDA #$1480` at m=0). If the runtime M flag is **wrong** (leaked), dispatch picks a variant
  whose body is garbage → corruption.
- Two layers can be wrong: **(a) the emitter/CPU layer** (opcode semantics, addressing, M/X
  tracking, stack ABI) and **(b) the hand-written runtime** (PPU/DMA, NMI/IRQ, BRK/COP syscalls,
  vblank-wait HLE, SPC). Different tools target each — see below.

---

## 1. DECISION GUIDE — symptom → tool

| Symptom | First tool(s) | Then |
|---|---|---|
| **Hang / freeze / watchdog SIGSEGV** | `saves/dump_state.txt` (auto-written by watchdog: call stack + **block-history ring** with PC/m/X) | `AR_MXHIST=1` to check for a misdecode leak; trace the looping block |
| **Garbage / character disappears / stuck-but-animating / hard crash on an object** | `AR_MXHIST=1` (is it a misdecode? names the leak boundary) | if leak → either `AR_DISPMISSALL=1 \| grep -v 'from 00896f'`, **or** take an **F2 full-snapshot** (§9) and scan the object table for an active slot whose `$12` handler isn't a converted `bank_00_*` fn → feed it to `find_handler_chain.py` (§5) |
| **"Who corrupted this byte/value?"** | `AR_WATCHOBJ=<hexaddr>` (writes to an object slot) or `AR_WATCH16=<hexval>` (a specific 16-bit write) — both log the **writing function + stack + frame** | — |
| **Missing object / event / spawn (logic)** | First check it's not an **unconverted spawn-data handler** (§11): F2 snapshot → object-table scan (**scan ≥64 slots, not 24**) for an active slot with an un-converted `$12`. Else differential oracle: `diff_seq.py` + oracle-only analysis (**must load SRAM** — see §6) | trace the spawn trigger / gate condition |
| **Rendering wrong (no text, no HBlank, missing BG, black screen)** | `AR_PPULOG=1` (bgmode, brightness, forced-blank, layer enables, HDMAEN) + **oracle screenshots** (`AR_SHOT_AT_GF` vs `SNESREF_SHOT_AT_GF`) | audit the PPU/DMA runtime (`ActRaiserDrawPpuFrame`); WRAM oracle is **blind** to VRAM |
| **Missing/extra sound or music** | check **BRK/COP syscall hooks** (§7) — `$035B`=SFX (BRK), `$035A`=music/event (COP) | `AR_WATCH16` on the request port |
| **Suspect a single opcode is wrong** | `tools/opcode_diff.py` (Tom Harte differential, §5) | — |
| **Per-frame game-state progression** | `AR_FRAMELOG=1` (callsite, work delta, mode `$18/$19`, timer, HP) | `AR_OBJLOG=1` for the object table |
| **Is the CPU layer even the problem?** | `AR_MXCHECK=1` — if it stays silent through the repro, the m/x layer is clean → look at the **runtime/PPU layer** | — |

**Golden rule:** on any "stuck / garbage / disappearing" bug, run **`AR_MXHIST=1` first**. In one
run it tells you misdecode-vs-not and where the M flag leaked — this is what turned multi-hour
hunts into minutes.

---

## 2. The core detection toolkit (permanent, always-available)

### `AR_MXCHECK=1` — entry M/X invariant check
Emitted in every function prologue (`ar_entry_mx_check`, see `emit_function.py`). Logs when a
function is entered with `(m,x)` ≠ the variant it was compiled for. Catches **direct-call
variant mismatches** = the emitter's static M/X analysis being wrong.
*Limit:* can't catch a wrongly-*leaked* runtime flag (dispatch always picks the matching
variant) — that's what `AR_MXHIST` is for. Leave it as a permanent regression guard.

### `AR_MXHIST=1` — runtime M/X histogram + live misdecode trap  *(the misdecode finder)*
Records per-PC `(m,x)` execution counts (`ar_mxhist_record`, `common_cpu_infra.c`). Once a PC is
established with a dominant `(m,x)` (≥64 hits), the **first** time it runs a different `(m,x)`
prints `[mxhist] MISDECODE? <pc> ran m=.. x=.. (1st time) after <dom> xN f=<frame> caller=<fn>`.
At exit dumps all multi-combo PCs (flags **LOPSIDED** ones). A misdecode = a function running a
variant it normally never runs → instant pinpoint of the leak boundary.

### `AR_DISPMISSALL=1` — computed-dispatch-miss log  *(the unregistered-handler finder)*
Logs every dispatch miss in the action stage as `[missall] ->TARGET from SOURCE m=.. f=..`. The
object loop's **normal** exits are `from 00896f` — filter them out:
```
AR_DISPMISSALL=1 ... | grep -v 'from 00896f'
```
What remains is a **straggler**: an object handler reached via a *nested* dispatch (source ≠
`$8965/$8966`) that misses the loop-resume safety net and leaks m=0. The log **names the exact
handler to register** in the cfg (`func bank_XX_TARGET TARGET entry_mx:0,0`). Related lower-level
log: `AR_DISPMISS=1` ($8965/$8966-gated, also shows BRA/BRL-follow resolution); `AR_ANCLOG=1`
(RTS-return-to-ancestor resolution).

### Standard misdecode/m-leak pipeline
`AR_MXHIST` (locate the leak boundary) → `AR_DISPMISSALL | grep -v 00896f` (name the missed
handler) → register it in `recomp/<bank>.cfg` → `tools/regen.sh` → rebuild.

---

## 3. Watchpoints — "who wrote this?"

- **`AR_WATCHOBJ=<hexaddr>`** — traps any write into `[addr, addr+0x40)` (one object slot) where
  the value changes. Logs `[wobj] $OFF=VAL (was OLD) f=N PB=.. cur=<current fn> stk: <recomp stack>`.
  Use to find who spawns/corrupts an object. `cur=` is the executing function even when the
  recomp call stack is unwound (top-level/dispatch context).
- **`AR_WATCH16=<hexval>`** — traps the moment a specific 16-bit value is written anywhere in
  WRAM. Logs dest addr + writing function + m/x + short stack. Use to chase a known-bad value
  (e.g. a corrupt handler pointer) back to its writer.
- **`AR_WATCH18=1`** — logs changes to `$7E:0018` (game-mode byte).

---

## 4. Per-frame observation logs

All fire once per host frame at the vblank-wait yield (`actraiser_rtl.c`):

- **`AR_FRAMELOG=1`** — `[frame] f=N push+DELTA callsite=BB:PPPP A=.. m=.. $18 $19 $1A $1B $F4 $F5
  $FB time$E6 HP$1D`. A steady push-delta with the timer ticking = engine running; tiny delta =
  spinning on a wait; frozen timer with large delta = a pause/gate. The callsite is the main
  loop's current vblank-wait point.
- **`AR_OBJLOG=1`** — action-stage object table: game-frame, timer, HP, active object count, and
  object-0 status word ($06A0 stride $40) + handler ptr. Reveals the frame an object table is
  wiped or a handler goes bad.
- **`AR_PPULOG=1`** — `[ppu] f=N inidisp=.. bright=.. fblank=.. bgmode=.. main=.. sub=.. hdmaen=..`.
  Black screen → check brightness/forced-blank/layer-enable; missing raster effect → check
  `hdmaen` (which HDMA channels the game is driving).
- **`AR_YIELDLOG=1`** — recomp call stack + SNES return address at each vblank yield. For
  "what is the main loop doing frame to frame."
- **`AR_GFLOG=1`** — logs the game-frame counter per frame.

---

## 5. Static analysis (no run needed)

- **`tools/opcode_diff.py`** — **Layer B**. Differential-tests the emitter's C output for each
  opcode against the Tom Harte SingleStepTests/65816 vectors (20k tests/opcode). Use when you
  suspect a single opcode's *semantics* (flags, decimal mode, addressing width). `--all`,
  `--opcodes`, `--mode native|emu`. 227 single-opcodes verified clean; gaps remain in
  emulation-mode `.e` vectors, MVN/MVP/PER, and (by nature) cross-instruction bugs (stack ABI,
  M/X-tracking) which single-opcode tests can't catch.
- **`tools/link_audit.py`** — **Layer A**. Static call-graph audit over `src/gen`: orphan
  (dead-carved) functions, per-PC variant coverage, trap-site live/dead classification.
- **`tools/stub_census.py`** — scans `src/gen` for unresolved trap markers
  (`cpu_trace_unresolved_goto_trap`, `cpu_trace_dispatch_oob`). **Stubs are a hard build error** —
  resolve each, never allowlist.
- **`tools/find_handler_chain.py`** — **object handler-chain finder** (the spawn-handler tool).
  Object state handlers are reached only via runtime dispatch (handler pointers from spawn data +
  the `JSR $8657` coroutine-yield idiom), so the static decoder can't follow them → dispatch miss
  → crash/freeze (see §11). Given seed handler addresses it does a 65816 (m0,x0) fixpoint that
  follows control flow **including `JSR $8657` yields** (continuation = site+3 = a new entry),
  collecting only true *dispatch* entries (seeds + yield-continuations, not internal branch/jump
  targets), and prints ready-to-paste `func bank_00_… entry_mx:0,0` lines for the unconverted ones.
  - `find_handler_chain.py AC11 AC41 …` — expand specific seeds (from a crash snapshot).
  - `find_handler_chain.py --tables` — **comprehensive, all 8 acts**: derives every `recordBase+0x0F`
    steady-state handler from the per-level tables (§11) + their chains. This is how the whole
    game's object handlers were registered in one batch (the `bank00.cfg` "COMPREHENSIVE" block)
    instead of crashing room-by-room. (BRK/COP are treated as continuing, not terminal.)
  - `find_handler_chain.py --all-yields` — **the yield-continuation closure**: seeds from *every*
    converted handler and follows all three yield helpers (`JSR $8657`/`$8668`/`$8669`), collecting
    every continuation that isn't itself an entry. Catches handlers reached via the **nested `$1E`
    dispatch (`$868F`)** that `--tables` can't (e.g. `$ABE5`, whose miss leaked m=0 → `B127`
    misdecode → `B90D` SIGSEGV). All results are immediately preceded by a yield `JSR` = valid
    entries; the scan fixpoints over chains, so one batch closes the class.
  - `find_handler_chain.py --field14` — **field-`$14` secondary handlers**: `record[0x0A]` is the
    object's initial field `$14`, which is *polymorphic* (handler for some object types, plain data
    — counter/coord/velocity — for others). Value can't disambiguate (no illegal 65816 opcodes), so
    the detector takes code-range `record[0x0A]` values, **drops consecutive-address clusters** (the
    data signature, e.g. `$8502-$8506`), requires a **handler-shaped + coherent decode**, and adds
    their yield chains. Conservative — it skips ambiguous (`COP`/`BNE`/`BRK`-led) values rather than
    register data as code; those fall back to runtime discovery. This is the residual tail that
    `--tables`/`--all-yields` can't reach (e.g. the bridge's original `$ACEA`).
- **`tools/rom_info.py`**, **`tools/lzss_decompress.py`** — ROM header / LZSS data helpers.

> **Static can't find everything.** M/X-tracking bugs are self-consistent in the emitted output;
> the dispatch model makes nearly everything "reachable"; handler pointers are computed at
> runtime from data streams (not statically enumerable). For those, use the runtime toolkit
> (§2) and the oracle (§6).

---

## 6. The differential oracle (recomp vs real snes9x)

Finds where the recomp's behavior diverges from a known-good reference. Lives in `tools/oracle/`.

- **`tools/oracle/snesref`** — snes9x-libretro frontend; emits per-frame WRAM-change JSONL
  identical in shape to the recomp's `AR_WRAM_TRACE`.
- The recomp side: **`AR_WRAM_TRACE=path`** (+ `AR_TRACE_LO/HI` to narrow the byte range) emits
  the same JSONL.

### ⚠️ CRITICAL: load the SAME save into the oracle
The recomp auto-loads battery SRAM from `saves/save.srm` at startup. **snesref does NOT** unless
you pass `SNESREF_SRAM_IN=../../saves/save.srm`. Without it the oracle boots a *fresh game*
(new-game name-entry screen) while the recomp continues from your save — **completely different
playthroughs, so the diff is garbage.** Always pass `SNESREF_SRAM_IN` for save-based recordings.

### Run recipe
```sh
# Oracle (from tools/oracle/), WITH the save:
SNESREF_HEADLESS=1 SNESREF_QUIT_FRAMES=4300 SNESREF_SRAM_IN=../../saves/save.srm \
  SNESREF_INPUT_REPLAY=../../saves/<rec>.bin SNESREF_TRACE_FILE=/tmp/o.jsonl \
  ./snesref snes9x_libretro.dylib ../../ar.sfc

# Recomp (from repo root):
AR_HEADLESS=1 AR_QUIT_FRAMES=4300 AR_INPUT_REPLAY=saves/<rec>.bin \
  AR_WRAM_TRACE=/tmp/r.jsonl ./build/ActRaiserRecomp ar.sfc

# Compare:
python3 tools/oracle/diff_seq.py /tmp/o.jsonl /tmp/r.jsonl --lo 0x200 --hi 0x1fff
```

### Three diff tools (pick by question)
- **`diff_seq.py`** — *value-SEQUENCE* diff (**use this**). Compares each address's ordered
  change-sequence, which is **timing-independent** (lag frames write nothing; an A→B→A toggle
  matches regardless of rate). Reports addresses whose sequences genuinely diverge, earliest
  first. Flags: `--skip-zp`, `--lo/--hi`, `--min-prefix N`. Game state `$0200-$1FFF` runs ~89%
  sequence-consistent when execution matches — divergences there are real.
- **`diff_aligned.py`** — aligns both traces by the game's own frame counter `$7E:0088` (instead
  of host frame), then reports the first game-frame an address diverges.
- **`diff_trace.py`** — coarse cumulative-final-value diff. Misleading on its own (boot/timing
  skew); prefer `diff_seq`.

### Oracle-only analysis (missing objects/events)
A *missing* thing is an **oracle-only** write (oracle writes it nonzero, recomp never does) —
which `diff_seq`'s prefix compare won't surface. Find them with a short Python pass over the two
JSONLs: collect addresses the oracle writes **nonzero** that aren't in the recomp's nonzero set,
filtered to a **late first-nonzero frame** (skip boot-clear noise; the recomp's batched boot
isn't traced, so early writes look oracle-only). This is how the missing platform was found
(a `$7E:6800+` OAM/sprite buffer the recomp never builds).

### Screenshots (for VRAM/PPU bugs the WRAM oracle can't see)
- Recomp: `AR_SHOT_AT_GF=N` → `saves/shot.ppm` (also `AR_SHOT_EVERY=N`, `AR_SHOT_FROM/TO`).
- Oracle: `SNESREF_SHOT_AT_GF=N [SNESREF_SHOT_FILE=p]` → PPM (works headless).
- View: `sips -s format png x.ppm --out x.png` (or PIL). Compare the two screens at the same
  game-frame to confirm a rendering divergence and see the target.

### Other useful snesref / recomp knobs
`SNESREF_WRAM0` / `SNESREF_DUMP_AT_GF`+`SNESREF_DUMP_AT_FILE` (oracle WRAM dumps),
`SNESREF_FORCE_B_AFTER` / `AR_FORCE_INPUT_AFTER`+`AR_FORCE_INPUT_MASK`+`AR_FORCE_PULSES`
(scripted input without a recording), `SNESREF_ENTRY_GF`/`AR_...` (align stage-entry frames).

---

## 7. Known bug classes & how they were fixed

1. **Misdecode from an M-leak** — runtime m wrong → wrong variant → garbage writes.
   *Find:* `AR_MXHIST`. *Common cause:* an **unregistered object handler** reached via nested
   dispatch leaks m=0 out of the `$8915` object loop (it skips the `$896E` PLP that restores m).
   *Fix:* register the handler in `recomp/<bank>.cfg` (`func ... entry_mx:0,0`). Examples:
   `$A9D1`, `$AB05`.
2. **Software-interrupt syscall not hooked** — ActRaiser uses `BRK` (vector `$FFE6→$852F`,
   `STA $035B` = SFX) and `COP` (vector `$FFE4→$8526`, `STA $035A` = music/event). Each needs a
   runtime hook (`g_cpu_brk_hook`, `g_cpu_cop_hook` in `actraiser_rtl.c`); a missing hook silently
   drops the effect. **Static-scan strategy:** for each software-interrupt the ROM uses (opcode
   present + vector points to a real handler), assert a matching hook is installed. Generalizes
   to an **HLE-hook census** (vblank-wait, SPC upload, NMI, IRQ, BRK, COP).
3. **PPU/DMA runtime gap** — logic/WRAM is correct but rendering is wrong (title text, HBlank,
   missing BG element). Lives in `ActRaiserDrawPpuFrame`. Example: it only processed HDMA
   channels 5/6/7; ActRaiser drives HDMA on 2/3 → fixed to process all 8. *Find:* `AR_PPULOG`,
   oracle screenshots (WRAM oracle is blind here).
4. **Stack-ABI / PHP-PLP split across the recomp function boundary** — the `$8915` object loop's
   `PHP`/`PLP` straddle the `$8915`/`$8966` carve; mis-tracked stack baseline leaked 1 byte/frame
   (fixed in `codegen.py` `_emit_return`). *Find:* trace-build stack-drift tripwire
   (`AR_DRIFT_FRAME`/`AR_DRIFT_LOG`).
5. **BRK inline-syscall continuation** — BRK is a 2-byte syscall that must continue at PC+2;
   the decoder now trial-validates the continuation.
6. **Unconverted spawn/state object handler** — an object's per-frame `$12` handler is reached
   only by **runtime dispatch** (from spawn-data records or the `JSR $8657` yield idiom), so the
   static decoder never converts it → dispatch miss → m-leak/misdecode → **freeze or hard crash**
   on that object. This is the broadest crash class for in-level objects (the Fillmore **bridge**
   that wouldn't extend, and the **evil-tree** enemies that crashed, were both this). *Find:* F2
   full-snapshot (§9) → scan the object table (≥64 slots) for an active slot with an un-converted
   `$12`. *Fix:* `tools/find_handler_chain.py` (§5) → register the emitted `func` lines → regen.
   *Bulk fix:* `find_handler_chain.py --tables` registers all acts' steady-state handlers at once.
   See §11 for the full object/spawn model. Computed handlers (e.g. `$AC11 = recordBase+0x0F`)
   never appear as literal ROM bytes, so they're invisible to byte/pointer scans — only the
   table-derivation or a runtime snapshot finds them.

---

## 8. Build & regen workflow

- **Changed a runner source** (`src/*.c`, `third_party/snesrecomp/runner/src/*`) → **rebuild only**:
  `cmake --build build -j8`.
- **Changed the emitter** (`third_party/snesrecomp/recompiler/v2/*`) **or a cfg**
  (`recomp/*.cfg`) → **regenerate then rebuild**: `bash tools/regen.sh` (rewrites `src/gen/*.c`),
  then `cmake --build build -j8`. `src/gen` is generated — **never hand-edit it**; regenerate via
  `tools/regen.sh`.
- Constraints: **no stubs, ever** (a stub is a hard build error — close the recompiler gap).
  Edit only the emitter, runner, `src/main.c`, `src/actraiser_rtl.c`, `recomp/*.cfg`, and
  `tools/`. Commit/push only when asked.

---

## 9. Recording, replay, dumps

- **Record:** `AR_INPUT_RECORD=saves/<name>.bin ./build/ActRaiserRecomp ar.sfc` (play windowed,
  quit cleanly). Records `{game_frame, inputs}` keyed by `$7E:0088`, so it replays frame-exact
  in **both** the recomp and the oracle (one file drives both).
- **Replay:** `AR_HEADLESS=1 AR_INPUT_REPLAY=saves/<name>.bin ./build/ActRaiserRecomp ar.sfc`.
  Auto-stops at end of recording. `AR_QUIT_FRAMES=N` to cap.
- **Dumps:** `AR_DUMP_AT_GF=N` (WRAM/SRAM/state at game-frame N → `saves/dump_*`),
  `AR_DUMP_ACT=1` (each action-stage frame). The watchdog auto-dumps `saves/dump_state.txt`
  (call stack + block-history ring) on a hang.
- **F2 full-snapshot (manual play):** while running windowed, press **F2** to dump a complete
  state set to `saves/snapshots/snap_NN_gf<frame>.{wram,vram,cgram,oam,ppm}` — 128KB WRAM **plus
  the PPU memory** (VRAM/CGRAM/OAM) **plus a `.ppm` screenshot**, all tagged with the game-frame.
  Each press writes a new numbered set, so you can grab several moments (before/during/after an
  event) and line up the picture against every internal buffer. **VRAM is the key addition** —
  WRAM dumps/oracle are blind to BG tilemaps, so this is what you use when something is drawn (or
  *not* drawn) to a BG layer. View a `.ppm` with `sips -s format png x.ppm --out x.png`.
  (Implemented in `ActRaiser_FullSnapshot`, `actraiser_rtl.c`; F2 handler in `src/main.c`.)
- **Init/forcing:** `AR_WRAM_FILL`/`AR_WRAM_INIT`/`AR_SRAM_FILL` (poison/seed memory),
  `AR_FORCE18` (pin game-mode), `AR_NOPOP` (disable the vblank-wait RTS-frame pop, for ABI tests).

---

## 10. Targeted / bug-specific probes (temporary scaffolding)

These are narrow, one-bug probes left in `cpu_trace.h` / various sources. They're env-gated and
inert, but are **not** part of the permanent toolkit — prune them once their bug is closed.
Current crop (this debugging arc): `AR_B90D_CATCH`, `AR_B127_CATCH`, `AR_896E_CATCH`,
`AR_8A3C_CATCH`, `AR_8664_CATCH`, `AR_STRACE`, `AR_EVTRACE`. Legacy from prior arcs:
`AR_CALLTRACE(_GF)`, `AR_CTACTION`, `AR_FUNCLOG`, `AR_MLOG`, `AR_SPAWNLOG`, `AR_8966X(_GF)`,
`AR_92CBLOG`, `AR_B127LOG`, `AR_DISP8465`, `AR_TRAP8465`, `AR_1EHIT`. Grep the source for
specifics if one looks relevant; otherwise ignore.

---

## 11. Reference: object & spawn-handler model

The most common in-level crash/freeze class (§7.6) comes from this system, so it's worth knowing.

### Object table
- Base **`$06A0`**, stride **`$40`**, and it has **more than 24 slots** — active objects appear at
  least through slot ~49 (`$12E0`); the Fillmore bridge segments live in slots 36–49. `AR_OBJLOG`
  only scans 24 — **scan ≥64 slots** when hunting a missing/late object.
- Per-slot fields (offset from slot base): `+$00` **status word** (high bits `0x4000`/`0x8000` set
  ⇒ inactive/free), `+$02` X (16-bit world), `+$04` Y, `+$12` **handler pointer** (`$12` — the main
  per-frame dispatch target), `+$14` **secondary handler** (field `$14`), `+$16/$18/$28/$30` spawn
  params, `+$30` flags (e.g. bit `0x0400`), `+$34/$36` spawn-X/Y source, `+$1E` nested-dispatch
  resume handler. Player = slot 8 (`$08A0`). Game-frame = `$0088/$0089` (16-bit).

### Dispatch
- The **`$8915` object loop** iterates slots, dispatching each active object's `$12` via push-RTS
  (`$895C: LDA $12,X; DEC; PHA; RTS`); the handler's RTS lands at the `$8966` continuation; the
  loop exits at `$896E` (`PLP; RTS`, restoring M). `$896F` returns to the per-frame update routines
  `$8078`/`$80B4` — those `->008078/->0080b4 from 00896f` "dispatch misses" happen **every frame
  and are normal** (filter them out).
- Nested dispatch `$8664: LDA $1E,X; PHA; RTS` runs the field-`$1E` secondary handler.
- **`JSR $8657` / `$8668` / `$8669`** = coroutine yields: each stores its own return address as the
  object's `$1E` resume handler and dispatches it (`$8669` also takes a param in A → field `$38`) —
  so **the instruction right after each such `JSR` is itself a handler entry**, resumed next frame
  via the nested `$1E` dispatch (`$8664`/`$868F`). These form chains. `find_handler_chain.py
  --all-yields` (§5) closes the whole class. A miss on one of these (`->… from 00868f`) leaks m=0 →
  `B127` misdecode → `B90D` crash.

### Per-level handler tables + spawn dispatcher
- Spawn dispatcher **`$9557`** reads game-mode `$18` (act index) → indexes the 8-pointer list at
  **`$95DD`** → that act's **handler table**:

  | act table | `$96AF` | `$A8F6` | `$B449` | `$C11E` | `$CD9B` | `$D928` | `$E722` | `$F39A` |
  |---|---|---|---|---|---|---|---|---|
  | (Fillmore = `$A8F6`) |

- Each table is indexed by **object type** → a **record base `B`**. The dispatcher (`$95ED`) then:
  copies spawn X/Y (`$34/$36`→`$02/$04`); reads record params (`rec[0]→$16`, `rec[2]→$18/$28`,
  `rec[4]→$30`, **`rec[0x0A]→$14`**); and sets the handler:
  - normal object: **`$12 = B + 0x0C`** (always an init `JSR`); after the one-time init the
    steady-state handler is **`B + 0x0F`**.
  - special (`field $38 == $FF`): `$12 = B` directly.

### Why handlers go unconverted (the crash class)
- All 209 `B+0x0C` init handlers are statically reachable from the tables → converted.
- **`B+0x0F`** (steady state) and **field-`$14`** secondary handlers are reached *only* by runtime
  dispatch → the static decoder never converts them → dispatch miss → m-leak/misdecode → crash.
- The computed values (e.g. `$AC11 = $AC0E+3`) **never appear as literal bytes** in the ROM, so
  byte/pointer scans can't find them — only the table-derivation or a runtime snapshot can.

### Deriving them (the anti-whack-a-mole)
- `tools/find_handler_chain.py --tables` walks all 8 tables, emits each JSR-gated `B+0x0F` and
  follows its `JSR $8657` chains → `func` lines for every unconverted handler, all acts at once.
- **Field-`$14` secondary handlers** (`rec[0x0A]`, e.g. the bridge's `$ACEA`) are *polymorphic* — a
  handler for some object types, plain data (counter/coord/velocity) for others — so they can't be
  derived by value. `find_handler_chain.py --field14` handles them via the data signature instead
  (drop consecutive-address clusters + require handler-shaped coherent decode); it deliberately
  skips ambiguous (`COP`/`BNE`/`BRK`-led) values, which fall back to runtime discovery.

**Coverage:** the three modes together — `--tables` (`B+0x0C/+0x0F`), `--all-yields` (the three
yield helpers' continuations), `--field14` — cover all three handler-reach mechanisms. The only
thing left for the per-occurrence loop (F2 snapshot → object-table scan → `find_handler_chain.py
<seed>`) is a value the `--field14` heuristic conservatively skipped.

---

## Appendix: one-line cheat sheet

```
AR_MXHIST=1            misdecode? where did m/x leak?            (run this FIRST on garbage/stuck/crash)
AR_DISPMISSALL=1       unregistered handler (grep -v 00896f)     (then register in cfg + regen)
AR_MXCHECK=1           emitter m/x analysis wrong on direct calls
AR_WATCHOBJ=<addr>     who writes this object slot
AR_WATCH16=<val>       who writes this 16-bit value
AR_PPULOG=1            rendering: bgmode/bright/fblank/layers/hdmaen
AR_FRAMELOG=1          per-frame mode/timer/HP/callsite
AR_OBJLOG=1            object-table health
AR_SHOT_AT_GF=N        recomp screenshot -> saves/shot.ppm
F2 (windowed)          full snapshot WRAM+VRAM+CGRAM+OAM+ppm -> saves/snapshots/  (VRAM-visible!)
diff_seq.py            timing-independent oracle value-divergence (LOAD SRAM!)
oracle-only pass       missing object/event (oracle writes nonzero, recomp never)
find_handler_chain.py  unconverted object handlers (<seeds> or --tables for all acts); see §11
opcode_diff.py         single-opcode semantics vs Tom Harte
```

Object/spawn model & the spawn-handler crash class: **§11**. Object table = `$06A0` stride `$40`,
**scan ≥64 slots**; handler `$12 = recordBase+0x0C` (init) → `+0x0F` (steady state).
