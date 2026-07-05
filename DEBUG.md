# ActRaiser Recomp — Debugging Guide

This document is the single reference for **how to debug this static-recompilation port**.
It lists every diagnostic tool, what it's for, and — most importantly — **which tool to reach
for given a symptom**. Read the Decision Guide first; the rest is reference detail.

All `AR_*` env vars are read by the recomp binary (`build/ActRaiserRecomp`). All `SNESREF_*`
env vars are read by the oracle (`tools/oracle/snesref`). Almost every tool is **env-gated and
inert by default**, so it's safe to leave the instrumentation in the build.

**Config-file shortcut:** every `AR_*`/`SNESREF_*` var can also be set in the `.ini` config
(`config.c` exports any such key via `setenv`), so you don't have to type them each run. Use
**`./build/ActRaiserRecomp ar.sfc --config dev-config.ini`** — `dev-config.ini` ships with the
cheat kit on and the common debug flags listed (commented). Precedence is **env > config**, so a
command-line `env AR_X=…` still overrides a value in the file. (Section headers and `#` comments
in the `.ini` are ignored; inline `# comments` after a value are stripped.)

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

### What actually causes m/x drift (read this before chasing a misdecode)

The single most important mental model, learned the hard way (the act→sim cascade):

> **Opcode correctness and decode-time WIDTH correctness are orthogonal.** A clean `opcode_diff.py`
> (Tom Harte, §5) does NOT mean "no misdecodes." Tom Harte tests each opcode with *known input
> flags* ("given m=1, does `SEP #$20` work?"). It can NEVER test "did the **decoder** correctly
> *assume* m=1 at this PC" — which byte-width it gave the instruction and which variant it emitted.
> That's a whole-program control/data-flow inference, and ALL our drift lives there.

So a misdecode is almost never "wrong opcode" or "unregistered handler." It's **right handler,
WRONG VARIANT**: the recomp emits one variant per `(routine × m/x)` and **dispatches by *runtime*
m/x**. One leaked flag bit → dispatch faithfully picks a wrong-width variant whose body is garbage
(the `CMP #$0004`→`CMP #$04`+`BRK` splits). The opcodes are fine; the m/x **input** to the dispatch
is wrong. **Four ways runtime m/x goes wrong even with perfect opcodes:**

1. **`PLP`/`RTI` restoring a runtime-determined `P`.** The decoder tracks m/x via SEP/REP/PHP/PLP
   (a per-function P-stack). Balanced *local* PHP/PLP works; but a PHP/PLP straddling a **call
   boundary**, or a **relocated stack** (`TCS`), breaks the model → the decoder *guesses* the
   restored m/x → every following byte decodes at the wrong width.
2. **Stack misalignment.** A `PHX`/`PHY`/`PHP` run at the wrong width pushes the wrong byte count →
   the SNES stack desyncs → a later `PLP`/`RTS` reads adjacent bytes → loads a garbage `P` →
   runtime m/x flips. (This is the x=1 cascade: wrong-width index pushes → misaligned stack.)
3. **Wrong exit-m/x propagation.** A callee returns at a different width than the decoder assumed,
   so the caller's *post-call* code is decoded at the wrong m/x — the `$9156` class. Override with
   cfg `exit_mx_at` / handle the RTS-trick with `rts_dispatch`.
4. **Computed dispatch** reaching a PC at a runtime m/x the decoder never decoded that target for.

A routine that hits *all four at once* (stack relocation + RTS-tricks + nested cross-function
PHP/PLP) is a "perfect storm" the static decoder can't track — e.g. `$03:8053` (the act→sim
enter-sim setup). The decoder guesses at every hop; on a *rare* code path (most of the game never
runs it) some guess is wrong and cascades.

**Why this is hard to pin, and the fix:** from inside the recomp, "m=0 here" looks identical whether
it's a correct rare path or a leaked guess (we burned a whole trace on `$02C206`, which actually
self-normalizes). `AR_MXCHECK` proved the *direct-call* layer clean; the leaks live in the
*dispatch + PLP-restore + stack-relocation* layer, invisible to per-opcode tests and any purely
static check. **Only a real-hardware CPU-flag reference can say which** — that's the (still-unbuilt)
ground-truth diff: serialize snes9x (`retro_serialize` savestates carry the CPU `P`), diff its m/x
against the recomp's per-block m/x, and the first divergence is the exact leaking PLP/push/dispatch.
Then fix with the override directives the recomp already has: `exit_mx_at`, `force_variant_at`,
`rts_dispatch`.

> **RESOLVED (2026-06-26) — and the lesson is a CORRECTION to the above: the act→sim "perfect
> storm" was mechanism #2 (stack misalignment), NOT a flag-tracking bug, and was found WITHOUT the
> oracle.** The apparent "x-leak" was a *symptom*: a **−1 SNES stack-pointer drift** made a `PLP`
> read the slot *next to* its own saved `P`, loading a byte with x=1. So x looked misdecoded, but
> m/x tracking was fine — `cpu->S` was off by one. Root: an **unconverted jump-table RTS-trick**
> (`bank_01_B898` `$B8C0: LDA $01B8D0,X; PHA; RTS` with a `PHY`-pushed return to `$B8C2`) — the
> recomp's generic RTS couldn't resolve the computed target and host-unwound `S` by −1 *per call*,
> which ratcheted up over a loop until the over-pop. **Diagnostic lesson: before assuming an m/x
> leak, RULE OUT a stack-pointer leak first.** A 1-byte `S` drift is indistinguishable from a flag
> misdecode at block granularity (the dispatch *and* a downstream `PLP` both go wrong). The tool
> that settles it is **`AR_STRACE`** (per-instruction `S`, scoped to a PC window) + the per-block
> `S` now in the diag dump: watch `S` across each call in the suspect routine — the one call that
> returns with `S` off is the bug, no oracle needed. Fixed with the new `indirect_dispatch …
> ret:<pc>` directive (jump-table CALL; §7.7). The CPU-flag oracle (§6) WAS built and is real, but
> it has a frame-granularity ceiling (can't see the crash frame, shows sampling noise) — the
> instruction-level `S` trace is what actually cracked this.

### Gotchas that have each burned a full investigation (read before building new instrumentation)

These aren't bugs — they're properties of the runtime/ROM that look like bugs until you know them.
Each one below cost real time (some cost days) before being understood; check this list before
adding a new probe.

1. **Direct-page addressing is `D`-relative, not literal.** `LDA $19` in decoded/generated code
   reads `cpu_read8(cpu, 0x7E, cpu->D + 0x0019)`, NOT literal WRAM offset `$0019`. `D` happens to be
   `0` in most of the code paths debugged so far (confirmed via `AR_SIMTRACE`'s printed `D=` field
   for the ResetHandler main-loop context), but **never assume it** — a watch pointed at a literal
   address can miss the real target entirely if `D != 0` at that call site. Always check `D` before
   trusting a watch's silence as "nothing writes here."
2. **Direct-page scratch bytes are reused across unrelated subsystems.** `$0014-$0017` is used as a
   16-bit ADD/XOR accumulator by the save-checksum routine (`$02:84F3`) AND as a message-type
   parameter by the dialog-draw dispatcher (`$02:BF60`) — two completely unrelated systems sharing
   the same 4 bytes. An oracle diff or watch hit on a low DP address does NOT mean "the SAME logical
   value differs" — check what's CURRENTLY using that address at the specific PC/frame in question
   before concluding anything about its "meaning." Assume DP scratch is polymorphic until proven
   otherwise (mirrors the object-table field-`$14` polymorphism in §11).
3. **There are (at least) three distinct WRAM write paths, and a watch only covers what it hooks.**
   `cpu_write8`/`cpu_write16` (`cpu_state.c`) is the direct-store path; `IndirWriteByte`/
   `IndirWriteWord` (`common_rtl.h`) is the indexed/indirect-store path (`STA (dp),Y` etc.); `snes_write`
   (`snes.c`, used by `dma_transferByte`) is the DMA path — DMA writes go straight into `g_ram`
   completely bypassing both CPU-instruction paths. `AR_WATCHOBJ`/`AR_WATCH16` originally only
   hooked the first path; both gaps were closed 2026-07-01 (see §3's coverage note and the
   `[wobj-dma]`/`[watch16-dma]` tags), but if you ever add a FOURTH write mechanism to the runtime,
   it needs its own hook too — a watch's silence is only as good as its coverage.
4. **A CPU register's value at a sampling point doesn't mean what you think it means without an
   explicit verification step.** A whole investigation (2026-07-01, the "`A=0x00A1`" chase) was
   built on the unverified assumption that `cpu->A`, sampled via `AR_FRAMELOG` at a vblank-wait
   yield point, reflected a specific memory read (`$0019`) executed earlier that frame. It didn't —
   `A` was legitimate, transient CPU state left over from an entirely unrelated `LDA #$A1` a few
   instructions earlier (`$00:8465`, a hardware-register-setup routine, same pattern as the
   NMITIMEN write at `$008051`). Ten rounds of write/read/DMA instrumentation correctly found no
   corruption, because there wasn't any — the bug was the initial assumed *link* between the
   register and the memory address, never checked before building on top of it. **Before chasing a
   suspicious register/memory value across multiple probes, spend one probe confirming the causal
   chain that made you suspicious of it in the first place.**
5. **A previously-documented, deliberately-accepted workaround can silently become the live bug
   the moment its original blocker is fixed — and nobody notices unless they go looking.**
   `$01:B898`'s `indirect_dispatch B8C0 … count=16` was capped below its real size (26) in an
   EARLIER arc (2026-06-26), with an explicit, correctly-reasoned comment: *"object types 16-26
   silently no-op instead of crashing — an acceptable gap, not a regression."* That comment was
   accurate **at the time**. Months later (2026-07-02), chasing the sim-mode actor-spawn bug, the
   label-emission bug that forced the cap got fixed as an unrelated side effect of a different
   fix — which silently turned "acceptable gap for types we don't use yet" into "the exact types
   the new feature needs." Nothing about the cap itself changed, and nothing flagged that its
   justification had quietly expired. It took **three full rounds** of registering dispatch
   targets, watching the census, and still seeing zero battery calls before anyone thought to grep
   the existing cfg comments and find the answer already written down. **Rule: before treating a
   silent no-op / dead dispatch / "nothing happens here" bug as newly discovered, grep
   `recomp/*.cfg` (and this file) for existing directives and comments touching the same address
   range FIRST** — a prior arc may have already characterized the exact gap you're staring at, and
   any accepted workaround is a candidate for having silently rotted since. This is also why
   `cpu_trace_dispatch_oob` was made loud-by-default in the same fix (§7, "dispatch-OOB
   tripwire") — process discipline (check first) is necessary but not sufficient; the tooling
   should also make this class of gap impossible to stay silent for long, so a future rotted
   workaround surfaces on its own instead of needing someone to remember to look.

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
| **Game runs at exactly 1/2 or 1/3 speed in ONE mode/screen (smooth elsewhere, audio fine)** | This is the **pacing/yield-multiplication class**, not host performance — smooth audio alone proves the host loop is healthy. Confirm + count in one shot: quit (or F9) **while the mode is slow**, then in the dump's block ring **count `02ABF0` (NMI-handler) entries per game-loop iteration** — each entry = one host-frame yield; N entries per iteration = 1/N speed, and the ring block *preceding* each entry names the yield site. Cross-check with any per-frame `[wobj]`/`[frame]` log: updates at delta=N host frames. Known causes so far: a non-HLE'd `$4210` wait yielding per read (§7 the `$9284` fix), and spin-detector false pairing on a twice-per-frame ack helper (§7.12 `$93CB`) | `AR_PERF=1` separates the two classes numerically (fps<60 = host-bound; fps=60 + crawling = pacing). `env AR_FRAMELOG=1 AR_VBLOG=1` names every yield's callsite/block live. NOTE: static `$4210` scans must include the long form `AF 10 42 00`, not just `AD 10 42` |
| **Suspect a single opcode is wrong** | `tools/opcode_diff.py` (Tom Harte differential, §5) | — |
| **Per-frame game-state progression** | `AR_FRAMELOG=1` (callsite, work delta, mode `$18/$19`, timer, HP) | `AR_OBJLOG=1` for the object table |
| **Is the CPU layer even the problem?** | `AR_MXCHECK=1` — if it stays silent through the repro, the m/x layer is clean → look at the **runtime/PPU layer** | — |
| **Crash on a mode/level TRANSITION; SNES stack corrupted (`S` walks to `$FFxx`/`$42xx`/I-O); `ppu_write`/`ppu_read` abort** | Check stderr for **`[dispatch-miss]`** (default-on tripwire) — it names the unresolved RTS-trick/computed target. Then `AR_SCHECK=1` (S-drift + impending-underflow path) | confirm with `AR_RTSLOG=0x<rts_pc>`; register the popped target as a cfg `func` (see §7.7) |
| **A feature/menu/effect just silently never happens — no crash, no garbage, nothing runs** | This is NOT the misdecode class (that produces garbage, not clean silence). Check the regen console output's three silent-drop report sections: **`JSR (abs,X) SUPPRESSED`** (cfg-required-dispatch-or-kill), **`DISPATCH TARGET SUPPRESSED BY DATA_REGION`**, **`Rejected JSR/JSL targets`** — grep the report for the bank/address range of the code you suspect. If none of those name the site, it's probably a genuine logic/state bug (e.g. a gate reading the wrong value) — see the DP-scratch-reuse gotcha above before assuming a memory address means what you think | `AR_INDIRLOG=1` if a suppressed `JSR (abs,X)` site is in range; otherwise trace the gate condition directly (§2 `AR_SAVECHECK`-style targeted branch probe) |
| **Wrong dispatch-case routing suspected (a runtime `(m,x)` switch calls a variant that doesn't match the caller's real width)** | Check the regen console's **`PROVEN-EQUIVALENT VARIANT ROUTING`** report section for the address — a `<== DIFFERS FROM CANONICAL/DEFAULT GUESS` entry means the OLD "nearest survivor" heuristic and the NEW proof disagree; a regen should already route it correctly. If the address is missing from the report entirely, `_find_equivalent_variants` couldn't prove any survivor equivalent (genuinely no safe target — see §7 "wrong-width dispatch, no provable target") | `AR_MXCHECK`/`AR_CALLMX` first to confirm it's actually reached at the wrong width at runtime (don't assume from static inspection alone — see gotcha #4 above) |

**Golden rule:** on any "stuck / garbage / disappearing" bug, run **`AR_MXHIST=1` first**. In one
run it tells you misdecode-vs-not and where the M flag leaked — this is what turned multi-hour
hunts into minutes.

**Golden rule 2 (learned the hard way):** when a variant looks like a misdecode, **trust the
emitted gen, not a hand ROM disassembly.** The instant, unambiguous "this variant is garbage"
check is `grep -c brk_hook` on the two variants — e.g. `bank_03_AC8E_M0X0` had **0** BRK calls
(real m=0 code) vs `bank_03_AC8E_M1X0`'s **7** (the m=1 misdecode splits `CMP #$0004` into
`CMP #$04` + `BRK`). Hand-decoding the bytes wastes time and mis-aligns; the recompiler already
did the decode correctly. Use the ROM only to *confirm*, never to *discover*.

**Golden rule 3:** ActRaiser **legitimately relocates its stack to high pages** (page `$05` via
`$057F`; **page `$1F` via `LDA #$1FFF; TCS` at `$03:9176`** for the act→sim transition). So a high
`S` is NOT corruption. The real corruption signatures are (a) `S` *draining down* and wrapping
`$0000→$FFxx` (underflow), and (b) a `[dispatch-miss]`. Don't waste a cycle flagging high pages.

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

### `[dispatch-miss]` — RTS-trick / computed-dispatch tripwire  *(default ON, the root-event finder)*
Printed by `cpu_dispatch_pc_from` (`cpu_state.c`) whenever an RTS/RTL/computed dispatch pops a
`(PB:PC)` that is **not a registered function entry**, so the runtime host-unwinds to the lexical
caller:
```
[dispatch-miss] 039B59 -> 039B22 has no entry; host-unwinding (m=1 x=0 S=1AB9 f=7735).
                If control/flags are wrong after this, register 039B22 as a cfg `func` ...
```
Host-unwinding is **correct for an ordinary mid-caller return**, but it is *also* exactly how an
**RTS-trick to an intra-function label goes silently wrong** (see §7.7): the unwind resumes the
wrong PC carrying whatever m/x the trick left set. This tripwire **names the offending computed
target on the first run** — it is the single signal that would have collapsed the multi-tool
act→sim hunt into one step (the symptoms — stack underflow, `AR_MXHIST` misdecode, the `BRK`
decode — were all downstream of this one miss). By default it flags only the **dangerous subset**:
a miss while the SNES stack is **relocated out of page 0/1** (`S >= $0200`) — the RTS-trick
signature — so ordinary mid-caller returns (which unwind here too, but with `S` in page 1) don't
bury the signal. Deduped per `(source,target)` and capped at 128. **`AR_DISPWARN=1`** removes the
`S` gate (shows *every* miss) **and** adds the recomp call stack; `AR_NODISPWARN=1` silences it.
Unlike `AR_DISPMISSALL` (action-stage `$18==01` only), this fires in **all** game states (the
transition is `$18==$27`).

### `[garbage-variant]` — split-immediate misdecode trap  *(default ON, closest-to-root, no oracle)*
Printed by `ar_garbage_variant_trap` (`common_cpu_infra.c`), emitted into the prologue of any
function variant the recompiler detected as a **split-immediate misdecode** at regen time
(`_detect_garbage_variant`, `emit_function.py`): a variant whose decode contains a `BRK` at a PC
that a *valid sibling* variant decodes as **mid-instruction** — i.e. the `BRK` is the high byte of a
16-bit immediate the wrong (narrow) width split off (`LDA #$0007`@m=0 → `LDA #$07`+`BRK`@m=1). Such
a variant is **never legitimately reached**, so entering it means **a leaked m/x flag dispatched us
into garbage** — and the trap fires at that *exact entry*, which is **far closer to the misdecode
root than the eventual downstream crash, and needs no oracle**. Logs the caller + runtime m/x +
frame, deduped per `(caller, variant)`. `AR_GARBAGE_STACK=1` adds the recomp call stack + block
ring; `AR_GARBAGE_ABORT=1` stops at the first hit; `AR_NOGARBAGEWARN=1` silences. This is the
sharpest tool we have for the m/x-drift class (§0): it turns "somewhere in this cascade m/x leaked"
into "the leak put us in a garbage variant **here**." (Complement to `[dispatch-miss]`: that catches
RTS-trick/computed misses; this catches wrong-*variant* dispatches.) *Known limitation:* a real
`LDA #id; BRK` syscall reached at both m could over-flag — rare, non-fatal, recognizable (it'd fire
during *normal* play, not a cascade).

### `AR_SCHECK=1` — SNES stack-pointer corruption tracer
In `cpu_trace_block` (`cpu_trace.h`). Two outputs: `[scheck]` logs each new high-water page of
`S`; **`[scheck-d]`** logs every block where `S` *jumps* > `$100` (a `TCS`/`TXS` relocation or the
corruption); and a one-shot **32-block path dump at the impending underflow** (`S < $0040`) so the
ring shows the routine draining the stack. Use for stack-corruption crashes. (Remember Golden
rule 3: high `S` is often a legit relocation — the underflow + `[dispatch-miss]` are the real
signals.)

### `AR_SIMTRACE=1` — sim-mode per-frame dispatch path watcher  *(2026-07-01, targeted probe)*
In `cpu_trace_block` (`cpu_trace.h`). Watches which of bank 0's sim-mode dispatch branch targets
executes each frame: `$008125` (skip the rest of the per-frame update — everything downstream of
the `$01:8000` building/icon call gets bypassed), `$0080F6` (continue — full per-frame update
runs), `$008066` (the action-stage path — should never fire while `$18==0`), `$0080E5` (sim-dispatch
entry, confirms the outer `$18` gate is even reached). Logs `[simtrace] gf=.. f=.. pc=$.. <tag>
A=.. X=..`, capped at 600 hits. Added to chase a sim-mode freeze where nearly all of WRAM stopped
changing frame-to-frame (F2 snapshot diff showed only 11 bytes changing over 545 frames) despite
the menu/save subsystems still working — i.e. the *movement/object-update* path specifically, not
a global hang. Pair with the `AR_FRAMELOG` joypad field (below) to see whether input is even
reaching this code when the skip path fires.

> **Update (2026-07-01):** the sim-mode freeze this probe was built for was NOT actually caused by
> the `$018000` skip gate — a static trace of `$018000` predicted (correctly, confirmed via
> `AR_SIMTRACE` itself) that it returns carry CLEAR under the observed conditions, so `$0080F6`
> (continue) DOES run. The graphics corruption's actual root is still open; see `AR_SAVECHECK` below
> and the sim-mode dispatch structure notes in `docs/SEAMS.md`.

### `AR_SAVECHECK=1` — save-data checksum gate outcome  *(2026-07-01, targeted probe)*
In `cpu_trace_block` (`cpu_trace.h`). Watches which branch `bank_02_A622`'s save-validity gate
(`$02A70D: BCC $A72F`) takes: `$02A72F` = checksum PASSED ("continue saved game" title-screen
flow — 3 dialog messages, sets `$0336=1`), `$02A70F` = checksum FAILED ("no valid save / new game"
flow — 2 messages, `$0336` untouched). Logs `[savecheck] gf=.. f=.. pc=$.. PASS/FAIL`. Neither
branch is a crash handler — both are normal title-screen dialogs (see `docs/SEAMS.md` "Save /
persistence" for the checksum algorithm and caller chain) — so a divergence here doesn't crash
anything directly, but it does mean state ONE branch is responsible for setting up (like `$0336`)
would be missing/wrong if the WRONG branch fires relative to what the save file represents. Used to
rule out (2026-07-01 investigation: checksum correctly PASSED, so this was NOT the cause of that
session's graphics corruption) a wrong-branch theory quickly rather than re-deriving the outcome
from noisy DP-scratch history (`$0014-$0017` is shared with an unrelated subsystem — see the
gotchas list in §0).

### `AR_STACKPROV=1` — stack pusher-provenance  *(who pushed the corrupt return frame)*
In `cpu_write8` (records) + the `[dispatch-miss]` site (reads), `cpu_state.c`. A shadow array
(`g_stack_pusher[0x10000]`, `common_cpu_infra.c`) stamps, for each bank-0 stack byte, the recomp
block-PC that last **pushed** there (detected by `bank==0 && addr==cpu->S` — the byte is written
before `S` decrements). On a bad-RTS `[dispatch-miss]` it dumps `[stackprov]` lines for the slots
around `S`, marking the two `<- return frame` bytes and naming `pushed-by PC $xxxxxx (f=N)`. The key
discriminator: a slot tagged **`NEVER PUSHED`** means the RTS read stale memory it never wrote ⇒
**`S` itself is wrong** (bad relocation/unwind), *not* a bad push — the opposite fix. Reach for it
when `[dispatch-miss]` shows a garbage target (e.g. `929D -> 010004`): `[scheck]`/`AR_RTSLOG` show
*where* `S` is, this shows *who put the bytes there*. Runner-only, ~256KB, gated.
Companion **`[overpop]`** (same flag, in `cpu_read8`): flags the FIRST pull/RTS that reads a
never-pushed slot (`addr==cpu->S` after the `S++`, `g_stack_pusher==0`) — i.e. the actual
unbalanced pop that drains the stack, *upstream* of the downstream garbage-return. Deduped by
block-PC, capped 64. This names the routine to fix; the `[dispatch-miss]`/`[stackprov]` pair is just
where the drain finally surfaces. If x is set at the over-pop, it also walks the block ring back to
the block where **x last flipped 0→1** (`aux` bit17, recorded in `cpu_trace_block`) and prints
`x flipped 0->1: block $A -> $B` — `$A` holds the `SEP`/`PLP` that leaked x and selected the garbage
(wrong-width) variant upstream of the drain. That line is the actual fix target for an x-leak crash.

### `AR_RTSLOG=0x<hex pc>` — RTS-dispatch chain tracer
In `cpu_dispatch_pc_from`. For a given RTS site, logs each dispatch hop's target PC + m/x + S +
the final result. `AR_RTSLOG=0x039b59` is what proved `$9156`'s RTS-trick dispatches to the
unregistered `$9B22` and host-unwinds (`final r=0`). Reach for it after `[dispatch-miss]` names a
suspicious RTS site, to see the whole chain and where it bails.

### `AR_TRAPFN=<substring>` — entry call-stack + block-path dump
In `ar_entry_mx_check`/`ar_entry_trapfn` (`common_cpu_infra.c`). The first time a function whose
name contains the substring is entered, dumps the **recomp call stack** + a **40-block pc/m ring**.
`AR_TRAPFN=bank_03_AC8E_M1X0` named the caller (`bank_03_8053`) and the m-flip path into a garbage
misdecode variant. Use to find *who* dispatched into a known-bad variant.

### `AR_CALLMX=1` — per-call-site m/x invariant check  *(2026-06-30)*
Set once at startup from env (`src/main.c`), read by `ar_call_mx_check` (inlined in every emitted
JSR/JSL, `cpu_state.h`). Unlike `AR_MXCHECK` (checks a function's *entry*), this fires at every
**call site**, comparing the decoder's static assumed `(m,x)` for that JSR/JSL against the runtime
CPU state right before the call. Prints `[call-mx] <fn> call-site $<pc>: runtime m=.. x=.. but
decoder assumed m=.. x=.. here -> (m,x) corrupted between fn entry and this call`. Narrows a leak
to a specific mid-function call site rather than just "somewhere in this function." Deduped by
call site.

### `AR_MXCHECK_BT=<fn-substring>` — real host C call-stack backtrace  *(2026-06-30)*
In `ar_entry_mx_fail` (`common_cpu_infra.c`), fires once (first hit) when a function whose name
contains the substring fails its entry `AR_MXCHECK` invariant. Captures the actual host
`backtrace()`/`backtrace_symbols_fd()` — the REAL C call stack, not the `g_recomp_stack`-based
approximation every other diagnostic relies on. Ground truth when a misdecode's caller chain is
in doubt; this is what finally proved `$01:933C_M1X0 -> $01:B898_M1X1`'s exact caller during the
2026-06-30/07-01 investigation.

### `AR_INDIRLOG=1` — suppressed `JSR (abs,X)` inspection  *(2026-07-01)*
In `ar_indirect_suppressed_log` (`cpu_state.c`), called from every codegen-emitted
`Call indirect SUPPRESSED` site (an unauthorised `JSR (abs,X)` the decoder severed — see
`_STUB_MARKERS`/`indirect_call_table`). Logs the site, table base, and effective address
(`table_base + X`), then classifies it: **WRAM** (prints the live table entry — a genuine
runtime-populated table, candidate for `indirect_call_table`/`indirect_dispatch` authorisation),
**SNES hardware-register space** (`$2000-$5FFF` — almost certainly NOT a real table; the "JSR
(abs,X)" itself is likely a decode artifact from a wrong entry m/x, the same bug class as
`$01:B898`), or **ROM** (prints the static table entry directly). Deduped per site, capped at 128
unique sites. Reach for this whenever the `JSR (abs,X) SUPPRESSED` regen-report section lists a
site you're trying to resolve.

### Standard misdecode/m-leak pipeline
`AR_MXHIST` (locate the leak boundary) → `AR_DISPMISSALL | grep -v 00896f` (name the missed
handler) → register it in `recomp/<bank>.cfg` → `tools/regen.sh` → rebuild.

For a **transition/stack-corruption crash**, the faster pipeline is: read **`[dispatch-miss]`**
(already in stderr) → `AR_RTSLOG=0x<that source pc>` to confirm the chain → register the popped
target as a cfg `func ... entry_mx:m,x` → regen → rebuild.

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

> **Coverage note (fixed 2026-07-01):** both watches originally only hooked `cpu_write8`/
> `cpu_write16` (`cpu_state.c`). Any store through an indexed/indirect addressing mode (`STA
> (dp),Y`, `STA [dp],Y`, `STA abs,X` when the effective address lands in the watched range) goes
> through `IndirWriteByte`/`IndirWriteWord` (`common_rtl.h`) instead, which wrote straight to
> `g_ram` with **zero instrumentation** — a real blind spot where a value only ever written
> indirectly was invisible to both watches no matter what you set them to. Found chasing a
> sim-mode freeze: `AR_WATCHOBJ=0` caught nothing on `$0019` despite other evidence proving it
> was being written every frame. Both watches now also fire from the indirect-write path, tagged
> `[wobj-ind]`/`[watch16-ind]` so you can tell which store form actually hit.

- **`blkpc=` in `[wobj]` (added 2026-07-01)** — `AR_WATCHOBJ`'s log line now also prints the most
  recently executed block PC (from the same ring buffer `AR_TRAPFN` uses:
  `g_ar_blk_ring[(g_ar_blk_idx-1)&1023]`) plus `X=`/`DB=`. Plain `cur=<func>` only names the C
  function; when a function is hundreds of lines with many internal labels/gotos, `blkpc=` tells
  you the *exact instruction* (65816 address) that did the store — the difference between "some
  code in `bank_01_ADAD` wrote this" and "line 81635, specifically."

### Live debugging with lldb (added 2026-07-01)

For a bug that needs *inspecting* state (registers, a real backtrace, evaluating an expression on
demand) rather than just logging a fixed set of fields, lldb on the Debug-mode build
(`CMAKE_BUILD_TYPE=Debug`, already the default — full `-g` symbols) beats another
print-statement-and-rebuild round trip. Recipe that worked (chasing the sim-mode decoration-OAM
bug, §10):

1. `cd` to the project root, `lldb ./build/ActRaiserRecomp`, then
   `settings set target.run-args ar.sfc --config dev-config.ini`.
2. **Don't set breakpoints yet — `run` first** and play up to the point the bug is visible on
   screen, then **Ctrl+C** in the terminal to interrupt. This avoids wading through boot/title/menu
   noise for a scratch byte or code path that's touched everywhere early on.
3. *Now* set breakpoints. A few gotchas specific to this codebase:
   - **Prefer `breakpoint set -f <file> -l <line>` over a name+condition on a hot generic function**
     like `cpu_write8`/`cpu_write16`. Those are called for *every* WRAM write in the whole game;
     evaluating a condition on every single call is slow enough to make the game appear hung. A
     line breakpoint inside the *specific* `bank_BB_PPPP_MmXn` variant you care about is unconditional
     and cheap.
   - `uint16`/`uint8` casts inside an lldb expression are **ambiguous** (multiple typedefs across
     translation units) — `error: reference to 'uint16' is ambiguous`. Use the builtin C types
     (`unsigned short`, `uint8_t`) instead, or read raw memory via `cpu->ram[...]` directly rather
     than calling through a function whose prototype uses the ambiguous typedef.
   - `strncmp`/other libc calls in a breakpoint **condition** need an explicit function-pointer cast
     (`((int(*)(const char*,const char*,unsigned long))strncmp)(...)`) or lldb refuses to parse the
     call ("unknown return type"). Simpler: compare `g_last_recomp_func[i]` bytes directly instead
     of calling `strncmp` at all.
   - `watchpoint set expression -- <ptr-expr>` defaults to **8-byte width** (pointer-size) —
     it will fire on writes to *neighboring* bytes/variables sharing that 8-byte-aligned region, not
     just the one you meant. Use `-s <bytes>` (e.g. `-s 2` for a 16-bit DP variable) to narrow it.
   - `g_ar_blk_ring`/`g_ar_blk_idx` (the same block-history ring `AR_TRAPFN` and the `blkpc=` field
     above use) are real globals — printable directly from lldb
     (`p/x g_ar_blk_ring[(g_ar_blk_idx-1)&1023]`) to see the last N executed 65816 block addresses,
     which is often more precise than a C-level `bt` for pinpointing which internal label of a large
     function actually ran.
4. `bt` at any stop gives the **real recompiled C call chain** — genuinely independent of
   `g_recomp_stack`/`AR_CALLMX`/every other stack-bookkeeping-based diagnostic, useful as a
   cross-check if you ever suspect our own instrumentation's bookkeeping (see `mxcheck-bt` memory
   entry for a prior case where this distinction mattered).

---

## 4. Per-frame observation logs

All fire once per host frame at the vblank-wait yield (`actraiser_rtl.c`):

- **`AR_FRAMELOG=1`** — `[frame] f=N push+DELTA callsite=BB:PPPP A=.. m=.. $18 $19 $1A $1B $F4 $F5
  $FB time$E6 HP$1D joy=.. (raw=..)`. A steady push-delta with the timer ticking = engine running;
  tiny delta = spinning on a wait; frozen timer with large delta = a pause/gate. The callsite is
  the main loop's current vblank-wait point. `joy`/`raw` (added 2026-07-01) is the
  `SwapInputBits`'d / raw joypad word — pair with `AR_SIMTRACE` to see whether input is reaching a
  stuck code path at all.
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

- **`tools/find_rts_webs.py`** — **pushed-continuation RTS-dispatch census** *(2026-07-04,
  built after §7.13's three-round hand-hunt)*. The town/scene engine nests this idiom
  arbitrarily deep and every layer used to cost one in-game repro + regen to find. The idiom
  is byte-stereotyped, so the whole class is statically enumerable in ONE pass: a continuation
  push is `A9 lo hi 48` (`LDA #imm16; PHA`) or `A0 lo hi 5A` (`LDY #imm16; PHY`) with `imm16+1`
  a plausible in-bank code address; a dispatch is `48 60` (`PHA; RTS`). The tool lists every
  hit per bank, cross-references the cfgs (`rts_dispatch` target lists, `indirect_dispatch
  ret:`, `func` entries), and marks each `[ok]`/`[UNC]` with a decode-score. It reconstructs
  every layer we found by hand ($9315/$9E31/$9D3B/$9B21/$F5E2 all `[ok]`), which is the
  regression guard: after any cfg edit, re-run and confirm no *known* web regressed to `[UNC]`.
  **What it does and doesn't give you:** it statically nails every dispatch SITE and
  CONTINUATION (fully enumerable from bytes) — that's the "which handful of sites" answer that
  used to take a repro each. It does NOT give the HANDLER TARGETS of a *RAM-pointer* dispatcher
  (e.g. `$03:CDAC` reads the handler from WRAM `$9220`; `$03:F97C` from `$030004,X`) — those,
  like `$8700`/`$E1D2`, still need one runtime `dispatch_log found:0` to enumerate, then plain
  `func` registration + the trampoline. **Known false-positive classes (so the signal stays
  trustworthy):** (1) a PHA;RTS site covered by `func`-registered targets (not a directive on
  the RTS pc) prints `UNC` — the tool only sees `rts_dispatch`/`indirect_dispatch` site
  coverage, so `$8711`/`$8759`/`$E1EB` are false-UNC; (2) low decode-scores (2-3) in
  action-stage banks are data bytes that happen to match the push opcodes. Triage by the
  printed decode preview: a continuation doing `PLA`/`PLX`/`PLY` of loop state needs
  `rts_dispatch` (NOT `func` — §7.13's model lesson), a bare dispatch needs the site + targets.
  `--bank NN` narrows to one bank (and then also lists the `[ok]` hits, useful as a coverage
  audit of a single subsystem).

- **`tools/find_yield_points.py`** — **yield-point / pacing census** *(2026-07-04, built after
  §7.12)*. Host-frame yields can only come from three places: cfg-HLE'd wait routines, the
  runtime `$4210` spin detector (snes.c), and the idle coroutine — so "verify pacing" reduces
  to enumerating and classifying every `$4210`/`$4212` read in the ROM. The tool scans ALL
  addressing forms (`AD` abs, **`AF` long — the form every historical scan missed**, `2C` BIT,
  `CD` CMP) and classifies each site by local shape: **SPIN** (read + BPL/BMI back to itself —
  the only shape allowed to yield, exactly once per wait), **CLEAR** / **POST** (the canonical
  3-read wait's bracket reads), **ACK** (isolated read — must NEVER yield; both historical
  pacing bugs, `$8465` and `$93CB`, were ACKs the spin heuristic false-paired), **OTHER**
  (unrecognized → exits nonzero, review by hand). Cross-references cfg-HLE'd routines (whose
  reads never execute). Current ground truth for ActRaiser (30 sites, zero OTHER):
  **7 live SPINs** = the complete legit runtime yield set (`01:9293` intro/menu wait,
  `01:92AA` the `$929E` long-form effect-loop wait, `02:87F3` fade helper, `02:9AC4` the
  boot-time APU bring-up wait, `02:BEBF` sound-code wait, `03:B013`, `03:E535`) and
  **3 live ACKs** (`00:8465`, `01:93CF`, and `03:AF58` — the last has not bitten yet; if a new
  mode shows 1/N-speed, check `[vbl]` for it first). Re-run after any cfg HLE change or on any
  new pacing symptom and diff the SPIN set.
  **Hardening LANDED (2026-07-04, after the §7.12 adjacency fix was user-confirmed in-game):**
  the runtime heuristic is now REPLACED by this static whitelist (snes.c `case 0x4210`,
  `kSpinBlocks[]`) — yield iff the reading block PC is one of the 7 SPIN sites (all are
  self-branch targets, hence block leaders, hence exactly the ring's block PCs); one yield on
  the first read, `0x82` returned to break the BPL. Pacing is deterministic and immune to both
  false-pair classes by construction; a spin missing from the list busy-spins into the
  watchdog (loud, block named in the dump) instead of running silently slow. **Census gotcha
  from landing this:** `$02:9AC1` sits inside the HLE'd `$9964`/`$9A56` upload's address range
  but is ALSO called natively by the reset-time APU bring-up (`JSR $9AC1` ×7 from
  `$02:98E0-990D`) — the first census marked it HLE'd-only and boot hung in its spin. Address
  containment proves nothing; only whole-routine `hle_func` coverage removes a site.

- **`tools/opcode_diff.py`** — **Layer B**. Differential-tests the emitter's C output for each
  opcode against the Tom Harte SingleStepTests/65816 vectors (20k tests/opcode). Use when you
  suspect a single opcode's *semantics* (flags, decimal mode, addressing width). `--all`,
  `--opcodes`, `--mode native|emu`. 227 single-opcodes verified clean; gaps remain in
  emulation-mode `.e` vectors, MVN/MVP/PER, and (by nature) cross-instruction bugs (stack ABI,
  M/X-tracking) which single-opcode tests can't catch.
  > **A clean `opcode_diff` does NOT mean "no misdecodes."** It validates opcode *behavior* given
  > known flags, never the decoder's *width assumption* at a PC. Decode-time m/x drift is a
  > separate, whole-program problem — see §0 "What actually causes m/x drift."
- **Proven-equivalent variant routing** *(2026-07-01, `tools/v2_regen.py` + `recompiler/v2/codegen.py`
  + `recompiler/v2/emit_function.py`)* — the emit-truth prune pass (which drops a wrong-width
  variant when a "clean" sibling proves the bytes are real code at that width) has to decide, for
  every PRUNED dispatch case, which SURVIVING variant to route callers to. The original heuristic
  (`_nearest_survivor` in `codegen.py`) just picked whichever survivor was numerically "closest" in
  `(m,x)` — a guess with no proof behind it, and the exact bug that misrouted `$01:B898`'s pruned
  `M1X0` dispatch case to the wrong-width `M1X1` body (see §7). The fix:
  `emit_function._find_equivalent_variants` decodes a candidate variant against all three other
  widths and does a byte-for-byte instruction-shape comparison (same `(pc16, mnemonic, operand
  length)` at every shared PC) — a real proof, not a distance guess. `codegen._route_pruned_variant`
  now checks, in order: (1) a variant PROVEN equivalent via this check, (2) the cfg-declared
  canonical width, (3) the `(1,1)` SNES-reset default, (4) the old distance heuristic as a last
  resort. Run `tools/regen.sh` and read the console's `=== PROVEN-EQUIVALENT VARIANT ROUTING ===`
  section: it lists every pruned-variant routing decision that came from tier 1, flagging any that
  `<== DIFFERS FROM CANONICAL/DEFAULT GUESS` (i.e. a case the old heuristic would have gotten
  wrong). **Not every wrong-width variant has a provable answer** — if `_find_equivalent_variants`
  can't match a pruned variant's decode to ANY surviving sibling (a genuine divergence, not just
  "never checked"), it's absent from the report and the routing falls back to tiers 2-4 exactly as
  before; that's not a bug in the checker, it means the ROM genuinely has no safe substitute for
  that dispatch case and the underlying caller-side leak needs its own investigation (`AR_CALLMX`/
  `AR_MXCHECK` to find who dispatches there with the wrong width).
  > **Regen fixpoint-convergence bug (found + fixed 2026-07-01).** The regen loop is an iterative
  > fixpoint: each pass re-emits every bank, stopping once nothing new is found. Equivalence facts
  > discovered DURING a pass only get applied to dispatch-switch routing starting the NEXT pass —
  > but if the pass that discovers a new fact is ALSO the one where every other convergence
  > condition is satisfied, the loop broke immediately, writing that pass's (stale-by-one-pass)
  > results as final. The end-of-run report (built separately, from the fully-accumulated data)
  > showed the fix correctly; the actual generated `.c` code never got the corresponding
  > re-emission. **Symptom: the console report says a routing case is "proven," but the generated
  > code still has the old "nearest survivor" comment.** Fixed by requiring the convergence check to
  > also confirm no new equivalence facts appeared during the pass before allowing the loop to
  > break (`equivalences_grew_this_pass` in `tools/v2_regen.py`). If you ever see report/generated-code
  > disagreement like this again for ANY report section (not just this one), suspect the same class
  > of bug: something computed during a pass whose effect is deferred to the NEXT pass, combined
  > with a convergence check that doesn't know to wait for it.
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

> **Confirmed working end-to-end (2026-07-01).** This exact recipe (fresh `AR_INPUT_RECORD` from a
> save-loaded session, `SNESREF_SRAM_IN` on the oracle side, `--lo`/`--hi` narrowed to the specific
> byte range under suspicion) was run for a real investigation and produced a clean, actionable
> diff on the first try. `--lo 0x0000 --hi 0x0040` (instead of the general `$0200-$1FFF` sweep) is
> a good move when you already have a specific low-DP address range in mind — narrows the
> `common(kept)` set to something you can read the whole output of directly, rather than skimming a
> "top 40" table. One real caveat found: **the WRAM tracer does not cover SRAM (bank `$70+`)** —
> addresses above `$1FFFF` never appear in either JSONL, so a checksum/save-data bug living in
> SRAM itself won't show up in this diff at all; you'd need a direct byte-level comparison of the
> `.srm` file / a raw SRAM dump instead.

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
7. **RTS-trick dispatch to an intra-function label** (act→sim transition crash, 2026-06-25) —
   the broadest *transition* crash class. Some shared engine routines hand-roll a computed RTS:
   they relocate the SNES stack (`LDA #$1FFF; TCS`), push **continuation addresses** built at
   runtime (`LDY #$9B21; PHY` …), then `SEP/REP #$20; RTS` to thread through them. Those targets
   are **intra-function labels the static decoder never sees** (computed pushes), so the recomp's
   trampoline RTS-dispatch (`cpu_dispatch_pc_from`) finds no entry, returns `NORMAL`, and
   **host-unwinds to the lexical caller carrying the wrong m/x** the trick left set. Downstream:
   the caller's next `JSR` enters an m=0 routine at m=1 → garbage misdecode variant → SNES stack
   underflow → `$2133` PPU-reg scribble → `ppu_write` abort. *Concrete case:* `$03:9156`'s RTS at
   `$9B59` dispatched to the unregistered `$9B22`; host-unwound to `$03:8053:$80B6` at m=1; `JSR
   $AC8E` ran its 7-`BRK` `M1X0` garbage → underflow. *Find:* the **`[dispatch-miss]`** tripwire
   (§2) names the target on the first run; `AR_RTSLOG=0x<rts_pc>` confirms the chain; `AR_SCHECK`
   shows the underflow.
   *DEAD-END (do NOT do this):* registering the RTS-chain targets as standalone cfg `func`s
   resolves the dispatch but **breaks the hand-rolled stack accounting across the function
   boundary** — each `func` gets its own `_entry_s` baseline, so the chain's net stack effect comes
   out wrong. Observed escalation: `$9B22`/`$9B4A` alone → infinite chain re-thread (S oscillates
   `$1AB9↔$1FFF`); adding the terminal `$9195` → chain terminates but S **over-pops to `$2001`** →
   garbage RTS targets → `B90D` m-misdecode watchdog hang. These are stateful coroutine fragments
   sharing the parent's relocated frame; they can't be sliced into separate functions.
   ***Fix = the `rts_dispatch` cfg directive*** (in-function dispatch). `rts_dispatch <rts_pc16>
   <target1> <target2> ...` tells the recompiler to decode the targets as blocks **inside** the
   enclosing function and emit the RTS as a `switch(popped+1){ case T: goto L_T; }` that preserves
   `cpu->S` across the whole chain. The targets decode at the RTS's `(m,x)`; the chain's terminal
   continuation (`$9195`'s `REP` → m=0 → its own normal RTS via the `$7D1B`-restored stack) is the
   true exit, so exit-mx analysis reports the correct m=0 to callers. For `$9156`:
   `rts_dispatch 9B59 9B22 9B4A 9195`. Implemented across `cfg_loader.py` (parse →
   `BankCfg.rts_dispatch`), `tools/v2_regen.py` (folds into the `indirect_dispatch` map with an
   `'rts_trick'` marker), `decoder.py` (in-function successors + `insn.rts_dispatch`; exit-mx
   analysis skips the dispatching RTS), `emit_function.py` (the `switch`/`goto` at the `Return`).
   *Proactive (Option A):* the targets are all the **immediate-push signature** (`$9195`
   `LDA #$9194;PHA`, `$9B22`/`$9B4A` `LDY #imm;PHY`; target = imm+1). An autoroute that detects
   `LD{A,X,Y} #imm`/`PEA #imm` feeding a reachable `RTS`/`RTL` (coherent-code guard, like
   `find_handler_chain --field14`) can **auto-generate `rts_dispatch` directives** for all 8 acts
   at regen time — build it once act 1 is confirmed working with the manual directive.
   ***Jump-table CALL variant (2026-06-26) — the act→sim ROOT, the `indirect_dispatch … ret:`
   directive.*** A data-driven cousin of the above: `bank_01_B898 $B8C0` does
   `LDA $01B8D0,X; PHA; RTS` (computed handler from a 16-entry ROM table) with a return address
   pushed FIRST (`LDY #$B8C1; PHY`) — so it's a **CALL** through the table (each handler `RTS`es
   back to the in-function continuation `$B8C2`), not a terminal tail-call. The recomp's generic RTS
   couldn't resolve the computed target and host-unwound `S` by **−1 per call**, which ratcheted up
   over `bank_03_B20C`'s loop until the over-pop → the entire boss→sim hang (the "x-leak" was a
   downstream symptom; see §0 RESOLVED). *Find:* `AR_STRACE` (per-instruction `S`, scoped to the
   loop's PC window) → the one call that returns with `S` off names the culprit. *Fix:* the existing
   `indirect_dispatch <PHA_pc> <count> idx:<X|Y> tables:<addr>` directive, **extended with
   `ret:<pc16>`** — when present, the PHA jump-table is emitted as a call-with-return (dispatch each
   handler, no synthetic frame push since the `PHY` already pushed the return, then `goto` the
   in-function `ret` label), keeping the continuation in-function (no `func`-split). For `$B898`:
   `indirect_dispatch B8C0 16 idx:X tables:B8D0 ret:B8C2` (bank01.cfg). Implemented: `snes65816.py`
   (`dispatch_ret` slot), `cfg_loader.py` (`ret:` parse), `decoder.py` (PHA block: `+1` the table
   words = handler-1 idiom, non-terminal, decode handlers + ret block as successors at the call's
   m/x), `codegen.py` `_emit_indirect_dispatch` (`is_call_ret` branch). NOTE the decoder `+1`: the
   table stores `handler-1` (RTS adds one); without it the labels are off by one.
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
8. **Wrong-width dispatch routing (the "nearest survivor" class)** — DIFFERENT from a misdecode:
   the ROM bytes decode correctly at each width, but when the emit-truth prune drops a wrong-width
   variant, the runtime dispatch switch has to route that case to SOME surviving variant, and the
   original heuristic just guessed the numerically-closest one rather than proving equivalence.
   Symptom: a caller genuinely reaching a dispatch case at the "pruned" width gets routed into a
   body that was never proven equivalent to what should have run there — silent wrong behavior
   (not necessarily a crash), hard to distinguish from an ordinary misdecode by symptom alone.
   *Concrete cases:* `$01:B898` (session-long chase, root-caused via `AR_MXCHECK_BT` real
   backtrace + direct instruction-shape comparison), `$00:8465`, `$00:845F`, `$00:A3E1`,
   `$02:AB05`, and 11 more addresses found by the same static check in one pass (see §5's
   proven-equivalent-routing entry). *Find:* the regen report's `PROVEN-EQUIVALENT VARIANT
   ROUTING` section, or `AR_MXCHECK`/`AR_CALLMX` catching the wrong-width entry live. *Fix:* the
   proven-equivalence pass now routes automatically wherever it CAN prove an answer; for the
   residual cases with no provable target, a manual cfg `entry_mx:` pin (like `$01:B898`'s
   `entry_mx:0,0`) is still needed — the static prover can only report "no answer," not invent one.
9. **Silent suppression (a feature/menu/effect that never runs, no crash, no garbage)** — a
   DIFFERENT failure shape from BOTH of the above: not wrong code running, but INTENDED code never
   running at all, invisibly. Three regen-time mechanisms can produce this: an unauthorised `JSR
   (abs,X)` gets its call site severed entirely (`Call indirect SUPPRESSED`, "cfg-required-
   dispatch-or-kill"); a dispatch-table entry whose target lands in a cfg `data_region` gets
   dropped from that ONE table slot; a `JSL`/`JSR` target the decoder judged out-of-LoROM (garbage
   operand past an unrelated `RTS`) gets its call skipped. All three are loud at REGEN time (the
   console report names every site) but completely silent at RUNTIME — no trap, no log, nothing to
   catch with the misdecode toolkit (§2), because nothing wrong-width ever dispatches; the call
   simply doesn't happen. *Find:* grep the regen console output for the address range you suspect,
   or `AR_INDIRLOG=1` for a specific suppressed `JSR (abs,X)` site (classifies the table base as
   WRAM/genuine-table, SNES-hardware-register-space/likely-decode-artifact, or ROM/genuine-table).
   *Fix:* author `indirect_call_table`/`indirect_dispatch` cfg directives once the real table shape
   is known (§7.7's `indirect_dispatch … ret:` directive is the same mechanism, just for the
   RTS-trick shape instead of the suppressed-call shape).
10. **Destination-cursor collision across sub-calls of a per-object loop** — a per-frame "rebuild
    the whole OAM list from scratch" routine (normal SNES practice, NOT itself a bug) calls a
    per-object sub-function once per active object; that sub-function writes several consecutive
    destination slots (one decoration icon = multiple tiles) and is supposed to **save its ending
    cursor back to a shared DP variable** so the next object's sub-call continues from there. If a
    caller has **two separate scan-loop segments** (two different call sites into the same
    sub-function, e.g. for two decoration categories) and they don't consistently share/persist that
    cursor, objects from the two segments collide on the same starting slot and overwrite each
    other — genuinely-correct per-object data ends up visually stacked/garbled. Symptom looks like
    "random garbage sprites," but every individual computation is provably correct in isolation;
    only the destination **allocation** is wrong. *Find:* this took a live lldb session (see
    `[watch14]`/blkpc-augmented `AR_WATCHOBJ` in §3, and the general lldb recipe in §3) — a
    conditional breakpoint at the destination-write call site plus a real backtrace was what
    finally distinguished "many different source objects, all landing on the same destination"
    from "the same object being reprocessed." A static-only read of the generated C repeatedly gave
    a *plausible-looking but wrong* theory (see the sim-mode case study below) until live register/
    backtrace inspection settled it. *Status 2026-07-01:* root-caused to this mechanism (see case
    study), exact broken hand-off between the two `ACD9` scan segments not yet pinned — open.

11. **Total silence with a healthy-looking sound engine — a *partially* HLE'd routine (the
    "stage 2" class).** *(FIXED 2026-07-03.)* Symptom: no audio ever, but every layer you probe
    looks alive: SPC runs at the right speed (~1024 cyc/ms), master volume set, mute off, the
    driver keys voices on constantly (`AR_KONLOG=1` shows KON writes on every beat), yet DSP
    output peak stays 0 (`AR_AUDIODBG=1`). Diagnosis chain that worked, each step one layer
    deeper: SDL device → `RtlRenderAudio` peak (0) → KON writes (present!) → per-voice state at
    key-on (`[konapply]`, temporary log in `dsp.c`): volumes/pitch/ADSR all sane, but the BRR
    bytes at each voice's sample pointer were **all zeros** — the DSP was faithfully playing
    empty sample RAM. Root cause: the game's `$02:9964` upload routine is TWO uploads: the
    `$9A56` block-image (sequence data + sample *directory* — which we HLE'd), then a second
    stage that re-handshakes and streams the actual BRR chunks from a length-prefixed pool at
    ROM `$08:8000` into ARAM (`$0358` = running dest; the image terminator's "final PC" word is
    really the stage-2 script: low byte = segment count, high byte onward = chunk indices). Our
    `hle_spc_upload` implemented stage 1 only, so every DIR entry pointed at zero-filled ARAM.
    Proof method worth remembering: simulate the suspected protocol in a 20-line Python script
    and check the predicted ARAM layout against the uploaded sample directory — title segments
    landed at `$3000/$3B01/…/$6E4C` = srcn `00-0B` starts *exactly*, song-7 at `$795F/…/$B89E` =
    srcn `0C-12` exactly. Fix: stage 2 added to `RtlUploadSpcImageFromDpInternal`
    (common_rtl.c), gated on the `9964` wrapper via `g_last_recomp_func` (the direct-`$9A56`
    boot mini-driver upload has no stage 2); it must also write back DP `$00/$02/$08` because
    the boot caller computes the next `$0358` from them. **Lesson (rhymes with the B8C0 gotcha):
    when you HLE a routine, diff your HLE against the FULL native disassembly to its final
    RTS/RTL — the first handshake loop you recognize is not necessarily the whole routine.**

12. **Mode-specific, exact-1/N-speed slowdown — spurious yield from the `$4210` spin
    detector's pairing rule (round 2).** *(FIXED 2026-07-04.)* Symptom: sim-mode effect
    screens (lightning strike, "direct the people") ran at exactly 1/2 speed; audio smooth
    (host at 60fps — that alone rules out host-bound and proves the pacing class). Diagnosis
    chain worth reusing: (1) `[wobj]` frame cadence in the user's log quantified it — object
    updates at delta=2 host frames inside the effect window, delta=1 outside; (2) user Esc'd
    MID-effect so the exit dump's 256-block ring captured the slow loop; counting NMI-handler
    entries (`02ABF0`) in the ring = counting yields per iteration → two, and the ring names
    the block preceding each. Culprit: `$01:93CB` — the NMI ack/re-enable bracket (single
    long-form `LDA $004210` + `$4200=#$A1`), called TWICE per frame by the effect path (end of
    the `$01:9460` iteration + again by its caller). Two same-block reads in one host frame →
    the spin detector's same-frame pairing rule (§ the `$8465` fix) classified it as a
    busy-wait → one spurious yield per frame → exactly 1/2 speed. Fix (snes.c `case 0x4210`):
    a spin must now also be RING-ADJACENT — block-ring index advanced ≤2 between the two reads
    (a self-looping spin re-records its block every lap; a twice-called helper has dozens of
    blocks in between). Gotchas recorded: static `$4210` scans MUST include the long form
    `AF 10 42 00` ($929E/$93CB/bank-03 waits all use it — an `AD 10 42`-only scan missed all
    of them); and the ROM has a second full wait-routine family at `$01:929E` (long-form clone
    of `$9284`) used by the effect loops.


13. **Sim-mode development cycle never fires / fires with corrupt output — a THREE-layer
    nested RTS-trick dispatch web, each layer invisible until the previous one was
    registered.** *(Registered 2026-07-04; in-game verification pending.)* Symptom
    evolution: (a) hourglass drains + refreshes but development never triggers, silently —
    zero dispatch-miss/oob output; (b) after layer 1 was fixed: development triggers but
    tiles land corrupted/misplaced + people/houses never appear + m=1 garbage variants in
    the town master loop. The chain (all in bank 03, docs/SEAMS.md has the full map):
    - **Layer 1 — the trigger:** `$91AE/$91BC` plant handler-1 words in WRAM `$7C45/$7C47`;
      the consumer loop does `LDA #$9315; PHA; LDA $7C4x; PHA; RTS` at `$9285/$92B5` →
      handlers `$9390/$944B/$9505/$95B3` (the four development-mode attempts, gating on
      population `$6B26,X` vs threshold `$021C,X`). Unregistered → the graceful RTS-follow
      silently skipped the whole step each cycle. **Found via the differential oracle in
      one run** (§6 recipe): user recorded ~78s of sim idling; oracle replay showed "Town
      Under Construction" + population 2→10 while our replay sat still; the trace diff
      named the missing chain (recomp never posts COP `$9C`, never activates spawn-list 6,
      never writes eligibility `$7F:9758`); the exit dispatch-log then named the exact site
      (`039285 -> 039390 found=0`).
    - **Layer 1 model lesson (repeat of the `$9156` class):** plain `func` registration of
      the four handlers made development RUN but corrupted tiles — their exit RTS returns
      to `$9316`, a MID-LOOP continuation doing `PLX/PLY` of loop registers. Separate-func
      slicing skips those pops on the miss-unwind → stack drift + m leak. `rts_dispatch`
      (in-function decode, S preserved, popped-value-guarded `goto`) is the required model
      whenever the continuation is stateful. Also fixed en route: the rts_dispatch emitter
      now guards `goto` emission on the target label existing in that VARIANT (wrong-width
      siblings reach the RTS without decoding the continuation → undeclared-label build
      error otherwise).
    - **Layer 2/3 — the build-step web:** the master loop's `JSR $9DE4` scans development
      records; per record it pushes continuation `$9E31`/`$9EC4` and `BRL`s to one of 7
      outer handlers (`$A011..$A35E`), each `LDY #table; BRL $9ED3`; `$9ED3` dispatches
      AGAIN via `PHA #$9EF3; PHA table-word; RTS` at `$9EF3` through per-type tables
      (7 tables × 8 words, 49 unique build-step handlers, entered m=1 x=0). The
      unregistered pop unwound the whole scan with a `SEP #$20` still open → the m=1 leak
      into `$8193` (→ `B97F`/`B1B7` garbage variants → misplaced tile writes) AND every
      build-step skipped. Registered as `rts_dispatch 9EF3 <49 targets>` + blanket
      `rts_dispatch <every RTS byte in $9EF5-$A4C8> 9EF4` + `rts_dispatch 9EF4 9E32 9EC5`
      — over-listing is SAFE (the guard falls through to a normal return when the popped
      word doesn't match), under-listing silently skips a step.
    - **Tools that cracked it:** `AR_GARBAGE_HIST=<n>` (garbage-trap ring dump now depth-
      configurable with S — 24 blocks couldn't reach the flip, 1000 could and showed the
      whole frame); `AR_SIMDEV=1` (gate-branch probe in cpu_trace.h); `AR_RTSDISP_MISS=1`
      (names any continuation the rts_dispatch list doesn't cover — the round-2 stragglers
      `$92E0/$9651` it reported turned out to be BENIGN normal-return fall-throughs, i.e.
      JSR returns through a blanket-mapped RTS; expect those, they're not faults).
    - **Class summary for next time:** this town engine nests pushed-continuation RTS
      dispatch arbitrarily deep. When a sim subsystem silently no-ops: dispatch_log
      `found:0` first (§ the heuristic above), then expect the fix to be `rts_dispatch`
      (stateful mid-loop continuation) not `func`, then re-run with `AR_RTSDISP_MISS=1`
      and expect one more nested layer. **`tools/find_rts_webs.py` (§5) now enumerates
      every dispatch site + continuation in the class statically — run it FIRST to see
      the whole backlog and register in one batch, instead of one regen per layer; it
      can't give RAM-pointer handler targets (still need one runtime `found:0`), but it
      names exactly which sites to watch.** Sites it flags uncovered in bank 03 as of
      2026-07-04: `$CDAC`/`$CE56` (RAM-ptr from `$9220`) and `$F97C`/`$F989` (RAM-ptr
      from `$030004,X`) — not yet traced to a subsystem/repro.
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

**Closed 2026-07-01 (the "`A=0x00A1`" false alarm — see §0 gotcha #4):** `AR_WATCH0019` (`cpu_state.c`
`cpu_write8`/`cpu_write16`, unconditional write watch on `$0019`), `AR_READ0019` (`cpu_state.c`
`cpu_read8`, unconditional read watch on `$0019`), `AR_TRACEA`/`AR_TRACEA_GF` (`cpu_state.h`
`cpu_write_a8`/`cpu_write_a16`, every write to the CPU `A` register from a given game-frame
onward — this is the one that actually settled it: `A` was legitimately loaded via `LDA #$A1` by
`$00:8465`, unrelated to `$0019`). All four proved their respective layers clean (no bug at that
layer) before the investigation found the real explanation (a transient, correct register value,
not memory corruption at all) — safe to remove whenever `cpu_state.c`/`cpu_state.h` next gets
cleaned up, or leave them (inert unless the env var is set) if another investigation might reuse
the pattern.

**Still open (2026-07-01, the sim-mode graphics-corruption investigation):** `AR_SIMTRACE` (see §2 —
ruled out the `$018000` skip gate as the cause, kept for its general "which branch fired" utility),
`AR_SAVECHECK` (see §2 — ruled out the save-checksum gate as the cause). `AR_INDIRLOG` (see §2,
originally for the sim-mode `$2920`/`$208E` suppressed-dispatch theory — confirmed those sites
never fire in the corrupted playthrough, so that specific theory is also closed, but the tool
itself is general-purpose and worth keeping armed for any future suppressed-dispatch site).
`AR_ADADTRACE` (`common_cpu_infra.c`, every `bank_01_ADAD*` entry — X/DB/D + the 3 source words),
`AR_WATCH14` (`cpu_state.c` `cpu_write16`, traces the `$0014`/`$0016` position-scratch writes inside
`ADAD`). Both were added for the case study below and are safe to leave armed (inert unless set).

### Case study: sim-mode decoration-icon corruption (root cause chain, 2026-07-01, still open)

**Symptom:** entering sim mode, decoration-icon sprites render as a garbled overlapping mess near
the HUD instead of correct/hidden placement; persists through the whole sim-mode session; the
sim-menu and save both still work fine (this is a rendering-data bug, not a hang/freeze).

**Root-cause chain, in the order it was actually established (many wrong turns included on
purpose — the wrong turns are as instructive as the right ones):**

1. **F2 snapshots looked clean at first glance** — 4 `.ppm` screenshots taken at narrative points
   (cutscene start/mid/end, sim-menu-open) all rendered correctly to the eye. The bug is *not*
   full-frame corruption; it's confined to a small icon cluster near the top-left HUD that's easy
   to miss without zooming (`magick ... -filter point -resize 400%`).
2. **OAM dump revealed the smoking gun:** ~40-70 consecutive sprite records in the `.oam.bin`
   snapshot held the *identical* value (`x=77,y=44,tile=0x55,attr=0x75`) — real games never
   legitimately stack dozens of sprites at one point with one tile.
3. **Traced to WRAM `$7E:03E8-$047F`** (a fixed 46-slot decoration-icon table) via byte-pattern
   search across the `.wram.bin` snapshot.
4. **Oracle (real snes9x) proved this is a genuine recomp bug, not ROM behavior:** ran `snesref`
   manually with the user's real save (`SNESREF_SRAM_IN=saves/save.srm`), windowed, driven live —
   real ActRaiser renders this exact scene with zero corruption (two full people in the circle
   scene, no stray icons). Then the WRAM trace (`SNESREF_TRACE_FILE`) showed the real hardware
   initializes `$03E8-$047F` **once** (hide-fill, `Y=0xE0` sentinel) and **never touches it again**
   for the rest of a 3400+-frame trace. Our recomp re-enters and rewrites this table repeatedly.
5. **First (wrong) theory — "X register frozen":** `AR_WATCHOBJ` showed the *destination* write
   walking sequentially through the table (`$03E8, $03EC, $03F0...`) with the *same* literal value
   (`4D 2C 55 75`) at every slot, in one single frame. Looked like a stuck loop-index.
6. **Correlating game-frame counters across separate captures is unreliable** — `snes_frame_counter`
   (host frames since boot) varies run-to-run based on how fast the user navigates the title
   screen; two different `dump*.txt` sessions showing "healthy" vs. "corrupted" at overlapping frame
   numbers are **not directly comparable** unless captured in the *same continuous run*. (The F2
   snapshot tool's `gf` filename suffix is `$7E:0088`, the shared logical game-frame both engines
   use — a better cross-engine anchor, but still not directly comparable to `snes_frame_counter`
   across separate host runs.)
7. **Live lldb (not static reading) was what actually resolved the ambiguity** — see the lldb
   recipe in §3. Breaking at the exact destination-write line
   (`bank01_v2.c:81635`, inside `bank_01_ADAD_M0X0`, the `cpu_write8(...0x0380+X...)` call) and
   inspecting `cpu->X`/the computed value live, across many continues:
   - The per-object **source** computation (`$0014`/`$0016` position scratch, fed from
     `DB:$000a+X`/`DB:$000c+X` where `X` is the *large* per-object index like `$0AE4`) is
     **correct** — genuinely different, correctly-computed values per object, confirmed via a
     memory **watchpoint** on `cpu->ram+0x14` (`-s 2`) proving *only* `ADAD` ever writes there
     (rules out a stray clobber from elsewhere).
   - Later in the *same* function, `X` gets **reassigned** (`LDX D:$0098`) to a *small* destination
     index used for the actual OAM-table write — this is normal/intended (X pulls double duty).
   - An inner per-tile sub-loop (`L_AE45`: four unconditional `INX`s, `CPX #$0200` exit check, loop
     back through `L_ADCC`) correctly advances this small `X` by 4 per tile **when watched within
     one object's own multi-tile icon** (`$00,$04,$08,$0C,$10...` — confirmed live).
   - But across **different objects in the same frame**, the destination write repeatedly restarts
     from the *same* base slot (`$0380+$68 = $03E8`) — i.e. the **inter-object** cursor hand-off is
     broken, not the intra-object tile loop. `D:$0098` (which `L_AE6B`, the loop-exit block, saves
     the ending `X` back into so the *next* object continues from there) sometimes shows correct
     progression (`$10→$14→$3C→$54` across objects) and sometimes doesn't — the backtraces show
     **two different call sites** inside `bank_01_ACD9_M1X0` reaching `ADAD`
     (`bank01_v2.c:19184` and `bank01_v2.c:19597`), i.e. **two separate per-object scan-loop
     segments** (likely two decoration categories) that may not consistently share/persist the
     cursor between them.
8. **regen1.txt cross-reference (a good general method, inconclusive for this specific bug):** the
   regen console log's `CONSTANT-Z BRANCH FOLDS` section (decoder statically proved a `BEQ`/`BNE`'s
   flag state from an immediately-preceding `LDA #imm`/`LDX #imm`/`LDY #imm` and rewrote it to an
   unconditional fall-through) flags exactly `$01:8E29` and `$01:8000` — both in this bug's live
   call stack — but only for the `M0X1`/`M1X1` variants. The actually-executing variant here is
   `M1X0`, which was checked directly and has **no corresponding block at all** at the flagged
   label (`$018F53`) — that variant's control flow diverges earlier and doesn't go through the
   folded branch. So this specific lead didn't pan out, but **the method is sound**: whenever a
   loop is suspected, grep `regen1.txt` for `CONSTANT-Z BRANCH FOLDS` / `JSR (abs,X) SUPPRESSED` /
   `UNRESOLVED INDIRECT DISPATCH` near the suspect address range *and check whether the flagged
   variant is the one actually executing* before spending time on it.

9. **"Rebuild stops after frame 665" was a MIRAGE (oracle-trace-is-change-only artifact) — the
   later-session correction.** Reconstructing the oracle WRAM at various frames and re-checking the
   *whole* destination region `$0380-$047F` (not just the `$03E8` byte) showed real HW writes that
   region **every frame** (2 bytes/frame after the initial 129-byte build at f=503, continuing to
   f=3423). The `oracle_*.jsonl` trace logs only **changed** bytes, so "2 bytes/frame" means real HW
   rebuilds all objects every frame but produces **stable/idempotent** output — only the ~1
   genuinely-moving object's 2 bytes actually change; `$03E8` specifically was rarely in the
   "changed" set because its object is static. **This kills the "find the gate that stops the
   rebuild" theory entirely** and *confirms* the destination-cursor collision as the real bug: real
   HW rebuilds-every-frame with stable output (each object → its own slot); we rebuild-every-frame
   with churning output (objects collide on shared slots). *General lesson:* a change-only WRAM trace
   showing "address X stopped being written" does NOT mean "the code that writes X stopped running" —
   it can mean "the code still runs but now writes the same value." Always check whether the
   surrounding region is still active before concluding a subsystem turned off.
10. **`$19` (sim-mode phase byte) is NOT stuck** — reading it from our F2 snapshots shows it reaches
    the interactive values (`01` and `08`), matching the oracle's post-transition sequence
    (`07→09→07→01→08→01`). So the frozen angel is **not** a stuck-phase problem, and the every-frame
    rebuild is **not** because we're re-running an entry phase. (Killed a tempting unifying theory —
    verified before building on it.)
11. **The `$0098` cursor logic is statically SOUND — so the collision is a runtime control-flow
    divergence INSIDE `ADAD`'s internal loop, not a missing/extra cursor write.** Exhaustive grep:
    `$0098` has exactly **two** writers (`ACD9` segment-1 init `=0` at `bank01_v2.c:18943`; `ADAD`'s
    exit-save at `:82014`) and one reader (`ADAD` entry load at `:81530`). `AC70` (called between the
    active-check and `ADAD`) touches **none** of `$0098`/`$0094`/`$0096`/`$0380`. The two scan
    segments are: seg1 `L_ACEF` (X=`$6A0`, stride `$12`, count `$30`=48; inits cursor=0), seg2
    `L_AD71` (X=`$0A00`; does *not* re-init the cursor — correctly continues seg1's). So under correct
    execution the cursor load→advance→save round-trip **must** chain across objects. The observed
    collision (many objects writing dest slot `$68`=`$03E8`) therefore comes from `ADAD` taking a
    **wrong internal branch/exit** for some objects — e.g. an exit path that skips the `:82014` save
    (so the next object re-reads the un-advanced cursor), or its internal tile loop (`L_AE45` INX×4 /
    `CPX #$0200`, `L_AE5E` `DEC $000e`) mis-terminating. **This is the likely codegen/decode locus**
    (a miscompiled loop back-edge or exit branch), which is why static reading of the *cursor* logic
    looked clean — the bug is in *which path ADAD takes*, not in the cursor arithmetic.

12. **RESOLVED to root cause (2026-07-01, later same session): a single missing init write to one
    object field.** The lldb `$0098`-cursor trace (ENTRY/SAVE per ADAD call, one frame) DISPROVED the
    cursor-hand-off theory — the cursor chains correctly (`$00→$10→$14→$3C→$54` across objects). The
    corruption is **one specific object, `scanX=$0BA2`**, whose ADAD call floods ~36 OAM slots
    (writes byte `$03E8` 17× then strides the `4d 2c` = X 77/Y 44 = Town-Hall-position pattern across
    `$03E8-$0425`). Comparing its ADAD internal loop-count `$000E` to a healthy object nailed it:
    healthy `$083E` has `$000E=$0004` (4 tiles); broken `$0BA2` has `$000E=$00D8` (**216 tiles**).
    Reason: ADAD derives the tile count by reading *through* the object's `$0008` field (a
    tile-data ROM pointer): `LDY $0008+X ; ... LDA $01:0000+Y`. Healthy `$083E` has `$0008=$DD4B`
    (valid ROM ptr → reads a real 4-tile table); broken `$0BA2` has `$0008=$0000` (**null**) → ADAD
    reads garbage from low WRAM `$7E:0000` → bogus count `$D8` → flood. **Oracle confirms the fix
    target exactly:** on real HW `$7E:0BAA` (= `$0008 + $0BA2`, the field) holds `$E6CA` (valid ROM
    ptr), written ONCE at frame 813 (sim-mode entry) and never again; the object's *other* fields
    (`$0BAC=$0058`, `$0BAE=$0048`) match ours exactly — **only the `$0008` pointer is wrong (0 vs
    `$E6CA`).** So the entire graphics corruption reduces to: **the one-time write that should set
    `$7E:0BAA=$E6CA` during sim-mode setup never happens (or computes 0) in our recomp.**

13. **Traced one level upstream — the copy loop is FAITHFUL; the bug is in its SOURCE buffer.**
    `AR_WATCHOBJ=ba0` showed the `$0BA2` record is populated by `bank_03_813F_M0X0` (called from
    `bank_01_AA56`), which at `L_8157` (`$03:8157`) runs a copy loop: `LDA $7F0000,X ; STA $0B30,Y ;
    INX ; INY ; CPY #$0130 ; BNE`. The writes landed at ODD offsets (`$0bab`,`$0bad`) which *looked*
    like a misalignment bug — but decoding the ROM at `$03:8140` cleared the recomp: `$8141 REP #$20`
    puts A in 16-bit with NO subsequent SEP, so the loop genuinely does **16-bit** stores while
    advancing X/Y by **1**. That's a deliberate SNES **overlapping-byte-copy idiom** — each store's
    high byte is immediately overwritten by the next store's low byte, netting a clean byte copy of
    `$0B30+Y = source_byte[X_start+Y]`. `cpu_write16` reproduces it exactly. So `bank_03_813F` is
    correct. The `$0BAA=$00` therefore comes from the **source**: `$7F:0000 + X` (a WRAM bank-`$7F`
    staging buffer) holds `$00` at the tile-pointer offset where real HW holds `$CA` (low byte of
    `$E6CA`). Specificity confirms it: ONLY the 2 tile-pointer bytes differ; every neighboring field
    copied correctly, so the staging buffer is right everywhere except that one field. `X_start` is
    derived via `$7F:7BFB → ROM table $03:8111 → X`, then the copy reads `$7F:0000 + X`.

14. **Staging-buffer hypothesis DISPROVEN — the real writer is a separate, missing frame-813
    record-init.** Computed the copy's source offset (`$7F:7BFB=$0000` → `X_start=ROM[$03:8111]=$97DA`
    → tile-ptr byte comes from `$7F:97DA+$7A = $7F:9854`) and checked it: `$7F:9854` is `$00` in BOTH
    our recomp AND the oracle (only the boot RAM-clear ever writes it). Yet the oracle's `$0BAA` ends
    up `$E6CA`. **Contradiction → `bank_03_813F`'s `$7F→$0B30` copy is NOT the source of the correct
    tile pointer** (it only writes the `$00`-based staging values; the `$0BAC=$58`/`$0BAE=$48` matches
    were coincidental — those offsets happened to be non-zero in the `$7F` block). The oracle's write
    timing shows the truth: the ENTIRE `$0BA2` record (`$0BA8=$0818`, `$0BAA=$E6CA`, `$0BAC=$0058`,
    `$0BAE=$0048`) is written together **at frame 813** by ONE routine — a proper record-init that
    supplies the real `$E6CA` pointer from somewhere other than the `$7F` staging buffer. In our
    recomp that frame-813 init never runs (or runs incompletely); only the earlier (frame-627)
    staging copy runs, leaving `$0BAA=$00`. *Lesson (again):* don't declare a source "the culprit"
    from a static offset computation without checking the actual byte — the computed source was `$00`
    on both sides, so it was never the origin.

15. **RESOLVED to origin via object-table scan: 4 cutscene-actor objects are never spawned.** Rather
    than watch a write that never happens, scanned all active ADAD objects' `$08` sprite-ptr fields
    (our F2 `.wram.bin` vs oracle-reconstructed). Result: a clean **cohort of 4 consecutive objects**
    `$0B30, $0B56, $0B7C, $0BA2` (stride `$26`) ALL have `$08=$0000` in ours vs valid `$01:Exxx`
    pointers in the oracle; every other active object has a non-zero `$08` (differences there are
    just animation-frame drift). Full-record dump confirms these 4 are **almost entirely
    uninitialized** in ours — only the position fields `+0A/+0C/+0E` (written by `bank_03_813F`'s
    staging copy) are set; the "live object" fields are all null: `+00/+02` (data ptrs), `+06`
    (type), `+08` (sprite ptr), `+12` (status word, oracle `$8001`=active). So it's not one field —
    **the entire spawn of these 4 objects never runs.** Their real `+00` values (`$DDC2/$DEDD/$DEEE/
    $DF3C`) point at sprite/animation DATA (verified: `$01:DDC2` = `03 08 00 00 04 08...`, structured
    records, NOT code), and their def data lives in ROM pointer-tables at `$01:E09B` (behavior/anim
    ptrs) and `$01:E7E1` (sprite ptrs). **No converted code reads those tables** → the spawn routine
    that reads the def-tables and populates these object records is **unconverted** (reached only via
    runtime spawn-dispatch the static decoder never followed) — same class as the Fillmore
    bridge-spawn (memory `bridge-spawn-data-handlers`), but at the spawn level.

16. **`A83A`'s SRAM-restore proven faithful — the save file itself is blank here (not a recomp bug),
    which reframes the whole search.** Traced `A83A`'s body fully: it's a **save-restore** routine
    (`bank_02_A622`, the title-continue state machine, calls it), not a ROM-template spawn as first
    guessed. Its inner loop (`$03:A9E3`, `X` from `$71F` down to `0`) copies **1824 bytes byte-for-
    byte from SRAM `$70:1633+X` into the `$7F:97DA+X` staging buffer** — i.e. it restores previously
    -saved sim-mode object state from the save file. Checked the RAW `save.srm` bytes at that exact
    SRAM offset (not our runtime state — the file itself): **they match our recomp's copy byte-for-
    byte**, AND the save file **genuinely has zero** at the relative offsets corresponding to
    `+00/+06/+08/+12` (behavior/type/sprite/status) for these objects. So `A83A` is completely
    innocent — it faithfully copies blank data because the source data IS blank. Since the oracle
    (same save file) still ends up with real values (`$E3FA` etc.) after loading, real hardware must
    run an *additional* step after this raw restore. **Reframing:** these 4 objects are most likely
    **cutscene actors** (not persistent world state) — that would explain why the save file never
    had their data serialized in the first place; they're probably (re-)spawned fresh by the
    cutscene/dialog EVENT system each time it plays, via a completely different code path than the
    continue-load flow.
17. **Two follow-up traces came back negative (useful eliminations, not progress toward the fix):**
    (a) `A83A` calls `bank_03_AAFC` right before its own return — traced `AAFC` fully; it dispatches
    into `bank_03_9156`/`AC8E`/`97B0` etc., the **already-solved act↔sim transition state machine**
    (see `$03:9156`/`$03:AC8E` in "Function roles discovered", SEAMS.md) — unrelated to object
    records, no writes to `$0Bxx`, no reference to the template tables. (b) checked `$02:BF60` (the
    dialog/message-box dispatcher, already mapped) and its direct sub-calls (`BED3`/`C0DF`/`C118`/
    `BFF6`/`C127`) for any reference to `$0B30` or the `$01:E09B`/`E7E1` tables — none found. Neither
    is proof the cutscene-event hypothesis is wrong (BF60 draws dialogue boxes; the ACTOR SPAWN would
    more likely be triggered by whatever decides *which* dialogue to show / advances the cutscene
    script, which hasn't been located), but static call-chain-following from these two entry points
    didn't find it directly.

18. **2026-07-02 fresh-eyes pass — the whole picture reframed (fn-census + trace-diff session).**
    New tooling: `AR_FNCENSUS=1` (rides the AR_MXHIST table; dumps EVERY function-entry PC +
    per-(m,x) counts to `saves/fn_census.txt` at exit — the decisive tool for never-runs bugs).
    Chain of findings, each verified:
    (a) **"cutscene actor" theory dead; the objects are the town's blinking lair/decoration
    records.** Real oracle spawn is at oracle f=901 (not 813) — part of a ~131-frame periodic
    "town tick" that also toggles a large blink family: the stride-`$12` table's `+10` words
    (`$06B0,$06C2..$0818`) AND the `+10` status words of stride-`$26` records based `$0AE4`
    (record 0 = `$0AF4`, records 2-5 = the broken `$0B40/66/8C/B2`, record 20 = `$0DEC/$0DEE`).
    (b) **The spawner is CONVERTED and never runs.** `$01:D072` = init-by-type (writes `+00` from
    behavior-ptr table `$01:E099,X` — earlier `$E09B` guess was off by 2, which is why the ROM
    byte-searches found nothing). 56 `JSR $D072` sites form a spawn-setup battery at
    `$01:BA23-$C793`. `AR_TRAPFN=bank_01_BFAA` stayed silent; census confirms the whole battery
    plus the `$01:D08F` script-stepper never enter. Sprite half: unconverted code at
    `$01:D0F5-D127` reads type→`$01:E7D9,X`→`+08`; placement records (stride 6: type,?,x,y) at
    `$01:D128+`.
    (c) **The generated "caller" of BFAA is a decode artifact** — `bank_01_8A75`'s emitted
    `JSR ($84FC,X)` dispatch at `$01:E289` decodes DATA as code (`$E250-E288` is a record table;
    `$84FC` is mid-instruction). BFAA's real caller is runtime-dispatched and invisible statically.
    (d) **`bank_02_C3DA` never runs — and that's NORMAL.** Its only caller `bank_01_8A62` sits
    behind `$19==7 && $0216==0` in `bank_01_8000` (the sim phase dispatcher), and `$0216` is 1
    before phase 7 begins in BOTH engines (oracle trace verified). Closes the old COP-fix loose end.
    (e) **Control flow is otherwise IDENTICAL:** `$19` phase sequence (0→7→9→7→1→8→1), `$0216`,
    COP-7 posts (`$035A`), frame counters (`$0088`, `$7F:9752`) all match oracle 1:1 (frame offsets
    differ; our phases run ~20% shorter — snes9x lag frames pad the oracle's timeline).
    (f) **Surgical trace diff** (oracle init burst f=895-905 vs our full-session write set): the
    ONLY object-region bytes the oracle writes that we never do = the 4 broken records' `+00-09`/
    `+10-14` + record 20's `$0DEE` word. Everything else in town init matches.
    (g) The blink/refresh routine itself RUNS in our engine — at phase transitions it writes
    `$0AF4`/`$0DEC` (records 0/20) correctly, skipping the EMPTY records 2-5. On the oracle the
    same tick also **(re)spawns empty lair records** (f=901 writes the full template `+00=$DF07`,
    `+08=$E6CA` into records 2 and 5 — same type in two slots, then animates `+08` through the
    frame table at `$01:E838` and blinks every ~131f). So the missing piece is narrowed to: **the
    periodic town-tick trigger fires on the oracle but never in our recomp** (our writes at
    f=748/842 are phase-INIT only, never periodic).

19. **2026-07-02 (cont.) — the spawn-list engine found; divergence isolated to one missing list
    invocation.** The `AR_WATCHOBJ=af4` run (dump12) caught the refresh chain red-handed:
    `bank_00_8325` (main loop) `L_83BF→L_83C3`: `bank_01_AA56` (staging restore) then
    `bank_01_8029` → `bank_01_B1C7` → { `bank_01_CFF2` + `bank_01_AC70` }. `B1C7` is a thin wrapper;
    `CFF2(A)` stores A into **`$033C/$033D` (the spawn-list selector)** then calls `bank_01_AC36`
    (the list-runner, runs constantly). Callers load the list id from a record's `+0E` field
    (`LDA $000E,X; STA $033C; JSR $AC36` at `$01:8E11/8E22`; other writers at `$01:914E/B593/
    B5A4/B5ED/B652/B732/C8BB/B7F1`). **Trace comparison of `$033C`:** oracle town-init runs lists
    `$13` (f=763) → **`$1C` (f=900)** → `$03` (f=901); ours runs `$13` (f=562) → `$03` (f=642),
    **skipping list `$1C` — the list that spawns the 4 broken records**. The oracle also cycles
    `$033D` `0→1→2→3` every ~180f afterwards (periodic town re-run); ours goes quiet.
    Dead ends eliminated en route (all verified in BOTH engines' traces, so genuinely-equal
    behavior, not bugs): `bank_03_C147` (gated on `$7F:9750`==0, true in both → never runs);
    `bank_03_E092`'s tick block `F479→B1C7→9314` (gated on carry from `bank_03_E19C` = "find
    open-but-unspawned lair in per-town masks `$7E:9107+`/scratch `$914F`" — masks are all-zero in
    both engines → always SEC → never fires); `$7E/$7F:9750`/`$9202` boot-state all equal.
    Also mapped: `$03:E19C` search loop (candidates 0-$1F vs pointer tables `$03:DCA2/$DCAE` →
    WRAM mask cells `$9107+4*town`/`$911F+4*town`, per-town data table `$03:E66E`), `bank_03_F46E/
    F479/F484` = mask test/set/clear via `$03:F497` (bit-compute, scratch `$914F`).

20. **2026-07-02 (cont.) — sweep protocol fully decoded (dump13, `AR_WATCHOBJ=33c`); divergence
    now one write from the root cause.** The stride-`$12` entries are SCRIPT PROCESSES. Protocol:
    (a) town-entry sweep `bank_01_AA56` → per entry: `CFF2(id from entry.+0E)` → `AC36` assigns
    `entry.+02/+06 = ROM[$01:A227[id*2] + $033D*2]` (script ptr), `+04=0`, `+00=1`.
    `AC36` disasm (25 bytes at `$01:AC36`): PHP/PHB/REP#$30; DB=$01 via 8519; Y=$033C*2;
    `LDA $A227,Y`; +`$033D*2` → `LDA $0000,Y`; STA `+02`/`+06`; STZ `+04`; `+00=1`; RTS.
    (b) `$033D` = cycle sub-variant 0-3 (oracle rotates it every ~180f = the periodic re-sweep /
    blink); `B1C7` wrapper ends each sweep with a `sub=4` special pass on the stride-26 base
    `$0AE4`. (c) per-frame `bank_01_9193` (blkpc `$0191A6`) re-runs list `$03` on main process
    `$083E` — in BOTH engines. (d) `$01:8E11/8E22` etc. are event-driven one-shot list runners
    (COP-7 posts from `bank_01_901C` blkpc `$01902D` correlate).
    **KEY RESULT: our engine DOES issue list `$1C` sub 0 for process `$0742`** (f=751, f=950 —
    id had advanced `13→1C` in `+0E` just like the oracle) → script `$A3E5` assigned, whose body
    (`00 9a d1 | fd | 00 af d1 | fd ...`) = "spawn placement `$D19A`/`$D1AF`/... , yield" — the
    exact 4 broken records. Oracle 1 frame later: cursor `+02=$A3E8` (one cmd consumed),
    `+08=$D19A`, spawn done. OURS at exit: `+02=+06=$A4C5` — a script ptr that belongs to list
    `$18` sub 3 / list `$19` sub 1, NEITHER ever issued for this process in the entire log.
    So after a correct assignment, something OVERWRITES process `$0742`'s script with a wrong
    pointer (executor failure-path jump? wrong-X write from a neighboring assignment? X-width
    misdecode?). Whoever writes `$A4C5` is the bug or its immediate symptom.

21. **2026-07-02 (cont., dump14) — stride-12 layer FULLY EXONERATED; the bug is the stride-26
    records' missing battery invocation at sweep time.** Findings:
    (a) `bank_01_AC70` disassembled = the per-frame ANIMATION-SCRIPT stepper for stride-12
    processes: `+00`=frame timer (DEC each call; negative normal — free-runs between assignments),
    on expiry read script at `+02`: default op = `[delay, frameptr16]` (writes `+08`, `+10|=1`);
    `$FD`=hide (`+10&=~1`); `$FE`=DEC `+04`, loop to `+06` unless 0; `$FF`=set loop count `+04` +
    loop target `+06`. It does NOT spawn anything.
    (b) OUR ENGINE RUNS THE `$1C` SPAWN SCRIPT CORRECTLY: at f=630/954 process `$0742` gets
    `+06=$A3E5`, executor consumes cmd 1 → `+08=$D19A`, cursor `$A3E8` — byte-identical to the
    oracle's post-901 state. The earlier "bogus `$A4C5`" is BENIGN: `bank_01_B52F` = "switch all
    decorations to variant N" pass (assigns the same script to several processes; oracle does it
    too). `bank_01_B6AE` = hide-all pass (`+10=$8001`). `$033E`=1 event: posted once in BOTH
    engines (equal). Stride-26 record 0 (`$0AE4`): alive and animating IDENTICALLY in both
    (script `$A5BD` via list `$00` sub 4 from `B1C7`'s tail pass).
    (c) **THE ACTUAL GAP:** stride-26 records use the SAME two-tier design with their own engine:
    `$01:D072` = the records' AC36-analog (writes `+00`=script ptr from table `$01:E099,X` — the
    `$DF07` the oracle writes to `$0B30` at f=901!), and the UNCONVERTED `$01:D0F5-D127` = the
    records' AC70-analog (writes `+08`=frame from table `$01:E7D9,X` — the `$E6CA`!). The battery
    (`$01:BA23-$C793`, 56 `JSR $D072` sites) = per-TYPE spawn-setup, invoked at sweep time for
    stride-26 records 1+ using their save-restored `+0E` ids (`$0D/$12/$13...`, visible in our
    WRAM). The oracle's f=901 init burst calls it; OUR sweep processes stride-26 record 0 only
    (B1C7 tail: single `CFF2(X=$0AE4)`) and never iterates records 1-5.
    (d) `$01:8819`: `LDA $033E; ASL; ADC #$F223; TAX` — an event-id jump table at `$01:F223`;
    `$01:8840` a second consumer. Possibly the record-sweep trigger path. Not yet traced.

22. **2026-07-02 (cont., dump15) — `bank_03_F5BE`: an entire per-town handler subsystem silently
    dead (RTS-trick double-push misdecode); caught by the new call-mx block-ring dump.** The
    `[call-mx]` failure at `$0381EB` is NOT just an m-leak. `ar_call_mx_fail` now dumps the block
    ring (added this session, `common_cpu_infra.c`), and it shows execution going `03F5DA →
    0381DA` — straight from F5BE's dispatch block back to the CALLER. F5BE's real structure
    (ROM disasm):
    `PHP;PHB;REP;DB=$7F` → `LDX $7BFB; LDA $03F5ED,X` (per-town handler-list ptr) → loop:
    `LDA $030000,X; CMP #$FFFF; BEQ exit; PHX; LDY #$F5E2; PHY; PHA; SEP #$20; RTS` (RTS-trick
    into handler, which returns to `$F5E3: REP; PLX; INX;INX; BRA loop`); exit `PLB;PLP;RTS`
    (flag-transparent on real HW). Handler lists live at `$03:F5F9+` (per town, `$FFFF`-terminated,
    entries `$F6xx-$F8xx`). The handlers are the LAIR/TOWN-EVENT logic: they call `F46E` with the
    `$DCA2/$DCAE` mask tables and read `$7F:91xx` lair state — the machinery §21's E19C analysis
    found "inert with all-zero masks" is inert BECAUSE its writers here never run.
    **Our decoder folds the `PHY #ret / PHA handler / SEP / RTS` double-push trick into F5BE's
    function exit: no handler ever executes, and the `SEP #$20` leaks m=1 to the caller** (the
    once-per-window call-mx hit; on frames where the branch isn't taken, no symptom — silent).
    Same family as the `$03:9156` exit-MX blindspot but a DIFFERENT idiom (push-computed-address
    dispatch, not table-JSR): the engine's build_indirect_dispatch_map/rts_dispatch handling must
    learn it (check exit_mx_autoroute + decoder for PHA/PHY-before-RTS pattern), then regen and
    census-diff to see what else comes alive.

23. **2026-07-02 — FIX IMPLEMENTED for the F5BE handler subsystem.** Extended the
    `indirect_dispatch` directive with two options and used them in bank03.cfg:
    - `idx:A` — value-keyed PHA/RTS dispatch: switch on the PHA'd A value (the handler-1 word)
      against the enumerated table words, instead of an X/Y table index. Needed because F5BE's
      X holds an absolute ROM pointer (per-town two-level walk), not an index. Decoder nulls
      entries whose +1 lands < `$8000` (the `$FFFF` terminators inside the enumeration window).
    - `sep:<mask>` — the real code executes `SEP #<mask>` between the PHA and the dispatching
      RTS (both replaced by the emitted switch), so the emitter applies the SEP before the
      switch and the decoder decodes handlers + ret continuation at the SEP'd (m,x).
    Directive: `indirect_dispatch F5DF 20 idx:A tables:F5F9 ret:F5E3 sep:20` (bank03.cfg, fully
    commented). Engine files: cfg_loader.py (parse), decoder.py (PHA path: sep flags + terminator
    guard), codegen.py (idx:A branch, suffix honors dispatch_sep, sanitizer accepts 'A'),
    snes65816.py (dispatch_sep slot). Handlers become real functions (auto-promoted); each RTSes
    back via the pre-pushed PHY frame (host-return) and the switch `goto`s `L_F5E3` — stack-
    neutral by construction, same contract as bank01's `B8C0 ... ret:B8C2` precedent.
    Validated: synthetic emit smoke test (value switch, SEP, terminator skip, PHY-undo default,
    M1X0 ret label) + full recompiler suites — top-level 57/58 and v2 186/198, both EXACTLY the
    committed tree's baselines (all failures pre-existing; the width lint was already failing
    with 4 pre-existing `_saved_pb` hits — my 5th copies the same sibling-emitter idiom).
    NOTE: first regen after this change ran BEFORE the codegen sanitizer fix (idx 'A' was coerced
    to 'X' → always-OOB switch) — a RE-REGEN is required. Also added a temporary env-gated
    trampoline probe `AR_F5BE_HANDLERS=1` (cpu_state.c) from the pre-fix investigation; harmless,
    remove with the next probe sweep. Hygiene same session: UBSan header-probe shifts clamped
    (`snes_other.c`), `B898` mxcheck hit analyzed benign (self-normalizing; pruned-variant
    fallback safe), NmiHandler f=0 call-mx = known benign wrapper.
    **SECOND engine bug found + fixed during the first post-fix rebuild** (build errors: `use of
    undeclared label 'L_F5E3_M1X0'`). Root cause: `cfg.py`'s `_identify_leaders()` only promotes
    an instruction's successors to "needs a block/label" when the SOURCE instruction's mnemonic is
    a hardcoded block-ender (RTS/JMP/branches/...) — but the dispatch is stamped on `PHA` (matching
    the existing `B8C0 ret:B8C2` precedent's own convention), which isn't in that set. `$F5E3` (the
    `ret:` continuation) decoded fine but got no label, so the emitted `goto L_F5E3_M1X0;` pointed
    nowhere. Fix (one line, `_identify_leaders`): also treat `len(di.successors) != 1` as
    leader-triggering, mirroring the check `_build_blocks` already used to stop the SOURCE block
    there. Verified via direct `emit_function()` harness call (label now present) + both full
    suites again (186/198 v2, 57/58 top-level — same baselines, zero new failures). This is a
    GENERAL engine fix (not ActRaiser-specific) and should hold for any future PHA/RTS-dispatch
    cfg use with a `ret:` continuation.
    **Verification for the next run: DONE, see finding #24 below** — census confirmed
    `bank_03_F621/F671/F68A` executing (town 0's 3 handlers; other towns' handlers untested since
    this playthrough never left Fillmore), `[call-mx]` at `$0381EB` confirmed gone. Records did
    NOT spawn — the fix was correct but not sufficient. Do not re-open this item; continue at #24.

24. **2026-07-02 (dump16, post-regen) — fix CONFIRMED partially correct; root cause NOT this.**
    Re-ran after the `cfg.py _identify_leaders` bugfix (see item below) forced a second regen.
    **Confirmed working, don't re-litigate:**
    - Census: `bank_03_F5BE M0X0=53`, `bank_03_F621/F671/F68A M1X0=53` each — a perfect 1:1
      match. F5BE dispatches ALL 3 of town 0's handlers, every single call, correctly. Town
      selection (`$7F:7BFB=0`, matching `$7E:0291=1`/Fillmore) is correct.
    - `[call-mx]` at `$0381EB` is GONE (grep confirms zero hits in dump16; only the pre-existing
      benign boot-frame `NmiHandler` hit remains). The m=1 leak this whole thread started from is
      fixed.
    - The handlers do REAL, ORACLE-MATCHING work: `$7F:9101/9102/910A/913A/914F` now get written
      by our engine with the SAME values at the SAME addresses as the oracle (lair-mask
      test/set/clear via `$03:F46E/F479`, previously totally inert — all-zero — in every prior
      dump this session).
    **Still broken — graphics corruption + freeze UNCHANGED:**
    - `$0B30/$0B56/$0B7C/$0BA2` still never populate (`AR_WATCHOBJ=b30` shows nothing but the
      already-understood `813F` staging-restore + `CFB3` hide-sweep — no NEW template write).
    - Root cause: **`$01:AC70`'s script format is animation-only, not a spawn mechanism.** Full
      ROM disassembly this session (`$01:AC70-ACD0`, byte-exact) confirms the ONLY opcodes are
      `$FD`=hide (`+10&=~1`), `$FE`=loop (`DEC +04`, branch to `+06`), `$FF`=set-loop-count+target,
      and a DEFAULT op = `[delay_byte, frameptr16]` writing `+00`(re-armed timer)/`+08`(frame
      ptr)/`+02`(cursor). **There is no "spawn an actor" opcode.** So list `$1C` running to
      completion on process `$0742` (proven correct in BOTH engines back in dump13/14 — the
      cursor advances identically) does NOT by itself populate the stride-26 records. The
      earlier "list $1C is the spawn trigger" theory (finding #20-21) is DISPROVEN as the
      *complete* mechanism — it's necessary scaffolding (assigns the process) but not sufficient.
    - **The real oracle event is a single massive one-shot burst, not periodic.** Pulled the FULL
      write-list for oracle frame 901 (322 writes, one line = one byte) and it is enormous: zero
      page `$00-$28`, the STACK PAGE `$01D9-$01FD` (36+ bytes — a deep call chain or big
      push-heavy routine, NOT normal per-frame churn), the full OAM shadow `$0380-$03A3`, ~10
      stride-12 process records (`$06B1` through `$0818`), process `$0742` fully assigned,
      stride-26 record 0 (`$0AE4`), ALL FOUR broken records fully populated in one shot, a small
      table at `$00DDC-$00DEF` (includes the `$0DEE/$0DEF` status word from finding #18), plus
      `$17CA1` and `$19752`. This is NOT F5BE's per-frame tick (which touches a handful of mask
      bytes) — it's a **separate, much bigger, one-time "town-entry finalize" event** that has
      not yet been located. `$033C: 0x1C->0x03` (the spawn-list handoff) happens INSIDE this
      SAME burst, not as an isolated event — meaning whatever triggers list `$1C`'s assignment
      also triggers this whole cascade, and OUR engine's version of that trigger either doesn't
      fire, fires without cascading, or fires via a different (broken) path.

**Status for a fresh session — START HERE:** the F5BE fix is DONE, verified, and should NOT be
revisited. The remaining bug is a **separate, unlocated one-shot town-entry event** that on real
hardware fires around oracle frame ~900 (our engine's equivalent frame differs — use `$033C`
transitioning `->$1C` or blkpc on the FIRST write to `$0AE4`/`$0B30` as the anchor, not a frame
number). Concrete next steps, in order of promise:
1. **Find what calls list `$1C`'s assignment in the first place.** We know `CFF2(A)` stores A into
   `$033C/D` then calls `AC36`; find the CALLER that passes `A=$1C` for process `$0742` specifically
   (dump13/14's blkpc showed `bank_01_CFF2_M0X0` called from `bank_01_AA56_M1X0` at town-entry sweep
   time — but AA56's sweep reads the id from `entry.+0E`, meaning `+0E` was ALREADY `$1C` before the
   sweep ran, i.e. save-restored. On real HW something must ALSO be calling a SEPARATE, one-shot
   "spawn now" routine when a fresh (non-restored) actor needs to appear — this is likely NOT AA56
   at all). Grep ROM for `JSR $D072` battery call sites once more, but this time trace BACKWARD from
   the battery entries to find what triggers them contextually (event code? `$033E`? a counter?).
2. **Watch `$01D9-$01FD` (the stack-page writes)** during a live run — an `AR_STRACE`-style probe on
   that PC/S window during the SAME burst would show which subroutine chain is executing, since a
   36-byte stack footprint at one instant is unusual and diagnostic.
3. **Do NOT re-chase**: `$01:AC70`'s opcode format (exhaustively disassembled, confirmed
   animation-only), the F5BE handler dispatch (fixed + verified), the stride-12 process
   assignment/cursor mechanics (fully correct in both engines).

25. **2026-07-02 (same session, post-#24): ROOT CAUSE FOUND AND FIX LANDED — dispatch-miss on the
    actor-behavior RTS-trick family.** Finding #24's "unlocated one-shot burst" framing was a red
    herring in two ways, both worth remembering:
    - **The stack-page writes were noise.** The oracle trace is a per-frame WRAM *diff*
      (address-sorted snapshot comparison, NOT temporal write order), so the `$01D9-$01FD` bytes
      were just leftover stack residue from a deep call chain that frame — return addresses
      (`$AC7A`, `$816F`, `$82B9`…), not a push-heavy spawn routine. Step 2 above (the `1d9` watch)
      was never needed.
    - **The burst is not "unlocated" and not special.** It's the tail of the town-entry LOAD
      sequence (oracle f≈891-901 is heavy load churn; the idle counter `$88` only starts ticking
      f=903). Our engine HAS the equivalent burst (trace f=642, matching `$0580/$0581` marker
      writes) and even writes the records **partially**: the type fields (+0A/+0C/+0D/+0E, values
      byte-identical to oracle) arrive via `$03:813F`'s staging copy (staging buffer `$7F:97DA`
      confirmed byte-identical between engines). What's missing is the per-type INIT that fills
      +00..+09 (behavior-script ptr from `$01:E099`, sprite ptr from `$01:E7D9`, position) — i.e.
      the spawn battery `$BA23-$C793` → `$01:D072`.
    - **The actual mechanism, straight from `saves/dump_dispatch_log.json`:** four unique
      `found=0` dispatch-miss pairs, repeating every frame — `01D062 -> 01B252/01B429`,
      `038711 -> 03871D`, `03E1EB -> 03E927`. `$01:D04E` is the actor-behavior dispatcher
      (`selector = record.+12 & $7FFF`, table ptr passed in **Y** by each caller, `PHA` table
      word, `RTS` at `$D062`). 24 call sites / 24 tables in bank01 (incl. the whole spawn
      battery); `$03:8700` (table `$8713`, 5 entries) and `$03:E1D2` (F5BE-shaped two-level walk,
      `$03:E66E` → 6 per-town tables of exactly 32 entries, continuation `$E1EC`) are the same
      idiom in bank03. None of the ~189 unique targets were decoded as functions, so the runtime
      trampoline computed the right pc24 every frame and the graceful-fallback silently skipped
      it. **Records got types but never behavior/position/sprite init — the 4 corrupt actors.**
    - **Fix:** registered all targets as `func … entry_mx:0,0` (dispatch log shows mx:0 for all)
      in `recomp/bank01.cfg` (106) and `recomp/bank03.cfg` (83, incl. the `$E1EC` continuation).
      No engine changes needed — the runtime dispatch already worked, only the targets were
      missing (same fix shape as the ~685-handler action-level registration). All 189 verified
      emit-clean via the direct `emit_function()` harness before handoff.
    - **Lesson for future misses:** check `saves/dump_dispatch_log.json` for `found: 0` entries
      FIRST when any subsystem silently no-ops. It names the source PC, target PC, enclosing
      function, m/x, and frame — this bug was fully identified by that log alone, after two
      sessions of oracle-diff archaeology pointed everywhere else.
    - **Round 2 (dump17, same day):** the registrations WORKED (freeze gone, town runs, map/HUD
      render) but exposed the next layer: three new per-frame misses (`01B429->01B22A`,
      `01B2DE->01B227`, `038759->03877F`). Two causes: (a) several D04E callers are TAIL
      dispatches (`LDY #table; BRL $D04E` inside a JSR'd helper), so the handler's RTS pops the
      OUTER caller's JSR-return — a mid-caller continuation, not a function -> host-unwind
      dropped the rest of the actor tick (incl. the `JSR $B41A` battery path — which is why
      record `$0B30` STILL only had `813F`'s type fields in dump17, and why it was choppy);
      (b) handler `$03:873C` contains its own PHA/RTS mini-dispatcher (18-entry inline table
      `$03:875B`, targets `877F/896A/879F`, pushed continuation `$8759`->`$875A`). Fixed by
      registering the systematically-enumerated set: every `JSR` return into the family
      (`bank_01_B227/B22A/C82A`) + the `873C` targets (`bank_03_875A/877F/879F/896A`). A scan
      for further `LDA table,X; PHA; RTS` dispatchers in banks 01/03 found none — any residue
      will show up as fresh `found:0` log entries.
    - **Round 3 (dump18): the REAL spawn blocker was the B8C0 table cap.** dump18's census
      proved the round-2 fix worked (the full actor tick `B23B→B227→B41A→B429→B22A` runs 219×
      = every frame, town handlers `873C/875A/879F/896A` all live) — but the battery still
      never fired, because it's reached per-TYPE through `$01:B898`'s `indirect_dispatch B8C0
      … tables:B8D0`, whose count was **deliberately capped at 16** (old comment: entries like
      `$B92E/$B905` decode-wandered and broke the `ret:B8C2` label). Town actor types are
      `0x12/0x13` = **18/19** — above the cap, OOB-skipped every frame. The label-breakage
      class was fixed by the `cfg.py _identify_leaders` change, verified: `B898` + all 26
      table handlers emit clean with `count=26`. Bumped to 26 (real bound: first handler
      `$B904` at B8D0+0x34). Two lessons: (a) **grep existing cfg directives for intentional
      caps/gaps when a subsystem's dispatch chain dead-ends** — the answer was written down in
      a comment; (b) old workarounds become load-bearing bugs after the blocker they dodge is
      fixed.
    - **Round 4 (dump19): count=26 landed but was inert — the idx:X model itself is broken at
      this site.** dump19 showed the regen HAD picked up `_disp_n = 26`, the runner RTS-follow
      HAD compiled in (the residual `found:0` lines are silent-but-correct mid-caller unwinds —
      the follow only logs `found:1` when a chain lands on a function entry), and yet census
      still showed zero battery entries. Reading the EMITTED dispatch revealed why: the ROM
      wraps the table read in `PHX($B8AE) .. PLX($B8BB)`, so at the `PHA/RTS` site X has been
      restored to the RECORD POINTER; the idx:X emitter computes `_idx = cpu->X / 2` AT the
      dispatch — `0x0B30/2 = 0x598 >= 26` — **OOB-skip on every typed record since the
      directive was first written** (the action stages never noticed: their objects dispatch
      through the bank00 `$8915` loop, not B8C0). Fix: switched the directive to the
      value-keyed `idx:A` form (keys the switch on the PHA'd table word, immune to PLX; the
      capability built for F5BE). Verified all 4 variants emit with `B9EC`/`BE4F` present.
      Lesson: when a dispatch "works" but its targets never appear in census, READ THE EMITTED
      C at the site — the decode/emit model can be wrong in ways no runtime log shows (OOB
      traces existed but only via `cpu_trace_dispatch_oob`, which nothing was watching).
    - **Round 3b: generic RTS/RTL-follow in the runner** (`_cpu_dispatch_once`, cpu_state.c).
      dump18's remaining misses (~94/frame, the progressive slowdown) were all bare-RTS
      continuation hops: `$03:86FD` passes its continuation in A (`#$8711`), so handler chains
      return through 1-byte RTS stubs (`$8712`, `$86FC`) that aren't function entries.
      Registering each is whack-a-mole; instead the miss path now emulates RTS/RTL chains
      (pop, retry lookup, hop-capped at 8, pops undone by the absolute S-restore on failure)
      — the generalization of the existing `$8965` BRA/BRL-follow. Flags untouched by RTS/RTL
      so it's semantically exact. Resolved follows are logged as `found:1` with the original
      source.
    - **Round 5: `cpu_trace_dispatch_oob` was compiling to a silent no-op in non-trace builds**
      — the exact reason round 4's OOB skip produced zero diagnostic output for as long as it
      did. Wired it to an always-on, deduped, capped tripwire (`ar_dispatch_oob_warn` in
      `common_cpu_infra.c`, `[dispatch-oob]` prefix) instead of a trace-only stub. Any future
      `indirect_dispatch` site that misindexes will now say so on the first hit — site, index,
      register state, calling function — rather than silently degrading. `AR_NOOOBWARN=1` to
      silence if it ever gets noisy.
    - **CONFIRMED FIXED IN-GAME (2026-07-02, post-round-5 build):** user report — actors spawn
      correctly, freeze is gone, `[dispatch-oob]`/`[dispatch-miss]` are both silent in a live
      run. This closes the sim-mode actor-spawn arc (findings #18-25).

**New, separate, open issue (2026-07-02): progressive slowdown in the "direct the people" path-
tracing sim-mode screen.** User-confirmed repro conditions rule out several tempting
hypotheses before any investigation starts:
- **NOT the round-3b dispatch-miss churn** — that's gone (0 `[dispatch-miss]`/`[dispatch-oob]`
  lines in the post-fix log). Don't re-suspect the dispatch machinery.
- **NOT actor/population-count-driven** — user confirmed it slows down specifically while using
  the path-tracing cursor tool (drawing where townsfolk should build next) with NO enemies and
  NO other people on screen, just the player and the map. So it's cursor/path-tracing-specific
  code, not a per-object per-frame cost that scales with population.
- **First hypothesis tried and DISPROVEN by static scan:** suspected this might be the same
  CLASS as the earlier angel-menu/mode7-spiral 1/3-speed bug ([[post-boss-four-issues]] — a
  `$4210` RDNMI spin-wait whose 3-read pattern wasn't recognized as a single logical wait,
  multiplying host-frame yields). Scanned the entire newly-registered actor-spawn code
  (battery `$BA23-D128`, both town dispatchers `$03:8700`/`$E1D2` regions) for inline `LDA
  $4210` or calls to the known un-HLE'd wait routine `$01:9284` — zero hits. That specific
  mechanism is not present in the code this session touched, so if it's the SAME BUG CLASS
  (a host-frame-yield multiplier, not real added CPU work), the offending wait/spin site must
  be somewhere in the path-tracing screen's OWN code, not the actor-spawn fix — likely a
  different, not-yet-found un-HLE'd copy of a wait loop, or some other yield-triggering
  pattern specific to that UI mode.
- **Next steps for a fresh investigation:** (1) find the path-tracing/cursor code (likely
  bank01 or bank03, a `$033x`-family mode distinct from the `$18` action-stage byte — check
  what mode/state byte this screen sets); (2) `AR_VBLOG=1` while in that screen to check for
  the same "3 host frames per logical frame" fingerprint the mode7-spiral bug had; (3) if VBL
  yields aren't it, check whether path length/waypoint count correlates with frame cost (an
  O(n) or O(n²) rescan of the traced path each frame would explain get-worse-as-you-draw
  behavior, if that's what's actually observed — confirm with the user whether it worsens as
  the path gets longer or is roughly constant once you're in the mode).
- Snapshots exist at two points during the slowdown plus one after — note snapshots capture a
  single frame's WRAM/PPU/OAM state, not timing, so they won't show the slowdown directly;
  compare them for something that GREW (counter, table, path buffer) between the three, not
  for visual differences.

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
[dispatch-miss]        DEFAULT-ON: RTS-trick/computed target had no entry -> host-unwound (§7.7)
                       AR_DISPWARN=1 adds stack; AR_NODISPWARN=1 silences. Read it on TRANSITION crashes.
AR_RTSLOG=0x<pc>       trace an RTS site's dispatch chain (target/m/x/S per hop)   (confirm §7.7)
AR_SCHECK=1            SNES stack corruption: S-drift + underflow path  (high S is often LEGIT, §GR3)
AR_STACKPROV=1         bad-RTS: who PUSHED the corrupt return frame (or NEVER-PUSHED = wrong-S, not bad-push)
AR_XFLIP_GF=<gframe>   the block where x flips 0->1 during game-frame N (N from the snes9x m/x oracle)
AR_XTRACE=1            x-flip ring auto-dumped at the first x=1 garbage variant -> the real fault's x history (no frame guess)
AR_STRACE=1            per-instruction cpu->S in a PC window (AR_STRACE_LO/HI, def $03B200-$03B260): find the call that returns with S off = the stack leak  (THE tool that root-caused act->sim)
(diag dump now shows per-block S — watch S drift across a call to spot an unbalanced subroutine)
AR_CALLMX=1            per-CALL-SITE m/x invariant (narrower than AR_MXCHECK's entry-only check)
AR_MXCHECK_BT=<fn>     real host backtrace() on the first AR_MXCHECK failure matching <fn> (ground truth, not g_recomp_stack)
AR_INDIRLOG=1          inspect a suppressed JSR (abs,X): WRAM table / HW-register decode-artifact / ROM table
SNESREF_MX_OUT / AR_MX_OUT + tools/oracle/diff_mx.py  snes9x CPU m/x ground-truth diff (finds the leak FRAME)
AR_TRAPFN=<fnsubstr>   who entered this (garbage) variant: call stack + 40-block m-path
AR_DISPMISSALL=1       unregistered handler (grep -v 00896f)     (then register in cfg + regen)
AR_MXCHECK=1           emitter m/x analysis wrong on direct calls
AR_WATCHOBJ=<addr>     who writes this object slot (also fires on indirect + DMA writes, tagged [wobj-ind]/[wobj-dma])
AR_WATCH16=<val>       who writes this 16-bit value (also fires on indirect + DMA writes, tagged [watch16-ind]/[watch16-dma])
AR_SIMTRACE=1          sim-mode dispatch: which branch fired this frame at a set of watched PCs (edit cpu_trace.h to retarget)
AR_SAVECHECK=1         save-checksum gate outcome: PASS (continue) vs FAIL (new game) — see §2
regen report           "PROVEN-EQUIVALENT VARIANT ROUTING" section: wrong-width dispatch routing, proven not guessed (§5)
regen report           "JSR (abs,X) SUPPRESSED" / "Rejected JSR/JSL" / "DISPATCH TARGET SUPPRESSED" sections: silent-drop audit (§7.9)
AR_PERF=1              once-per-second frame budget: fps / run-ms / gf-advance / apu-catchup — separates host-bound (fps<60) from pacing (fps=60 but game crawls). CAVEAT: gf is NMI-driven, always 1:1 — it can NOT detect the pacing class; use the ring trick below
F9 mid-bug + ring      exact-1/N-speed in one mode? quit/F9 WHILE slow, count 02ABF0 (NMI) entries per iteration in the block ring = yields per game frame; block before each = the yield site (found §7.12 in minutes)
find_yield_points.py   static census of ALL $4210/$4212 reads (incl. AF long form) classified SPIN/CLEAR/POST/ACK + HLE cross-ref; its 7 SPIN sites ARE the runtime yield whitelist (snes.c kSpinBlocks — keep in sync!); unlisted spin = watchdog hang naming the block (loud), never silent slowdown
find_rts_webs.py       static census of the PHA;RTS pushed-continuation dispatch idiom (A9../A0.. +48 pushes, 48 60 sites) vs cfg coverage; run FIRST on a silent-no-op sim subsystem to see the whole uncovered backlog in one pass (§5, §7.13). RAM-ptr handler targets still need runtime found:0
AR_RTSDISP_MISS=1      names any continuation a `rts_dispatch` list doesn't cover (site + popped target); benign JSR-return fall-throughs also print — check the popped value before adding a mapping
AR_GARBAGE_HIST=<n>    garbage-trap block-ring depth (default 24, max 1000) incl. per-block S — 1000 spans a whole sim frame; how the dev-cycle m-leak origin was found (§7.13)
AR_SIMDEV=1            dev-cycle gate-branch probe (cpu_trace.h; retarget its PC list per hunt — the reusable "which branch fired" pattern)
AR_AUDIODBG=1          DSP health: mvol/mute/output peak/SPC cyc-per-ms/pending KON  (peak=0 = silence GENERATED, not a device problem)
AR_KONLOG=1            DSP key-on writes + per-voice state at key-on (vol/pitch/ADSR/BRR first-bytes — all-zero BRR = samples never uploaded, §7.11)
AR_APULOG=1            APU port traffic + SPC upload blocks (+ stage2 sample-chunk streaming)
AR_PPULOG=1            rendering: bgmode/bright/fblank/layers/hdmaen
AR_FRAMELOG=1          per-frame mode/timer/HP/callsite/joypad
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
