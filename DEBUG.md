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
