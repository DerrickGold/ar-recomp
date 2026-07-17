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
   `0` in most of the code paths debugged so far (confirmed via the sim branch probe's printed `D=` field
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

### THE DEBUG LOOP (the whole process, end to end)

Every issue goes through the same eight steps. The rest of this document is reference detail for
individual steps — this box is the process:

0. **`tools/cycle.sh` is steps 1-2-5-6 in one command** (2026-07-07): regen-iff-cfg-changed →
   build → run via `tools/run.sh` → on exit auto-diagnoses every anom capture AND runs
   `tools/resolve_miss.py` (the mechanized version of steps 2-3) → `runs/latest/cycle_report.txt`
   ends with a PROPOSED CFG PATCH. Apply with `resolve_miss.py <files> --apply`, review via
   git diff, `tools/cycle.sh` again. The manual steps below remain the reference for what
   the automation is doing (and for the classes it marks AMBIGUOUS).
   **All per-run artifacts live in `runs/<timestamp>/`** (2026-07-08, NATIVE via `src/run_dir.c` —
   plain `./build/ActRaiserRecomp ar.sfc --config dev-config.ini` gets it too, no wrapper needed):
   console.log (full stdout+stderr, tee'd through a child process so it survives crashes),
   run_info.txt (cmd + AR_* env), anom captures, F2 snapshots, Shift+F9/exit dumps, screenshots.
   `runs/latest` symlinks the newest; older runs stay intact for parallel analysis.
   Battery SRAM/save-states stay in `saves/`. `AR_NO_RUN_DIR=1` restores the flat legacy layout.
   Bare filenames in AR_TRACE/AR_INPUT_RECORD/AR_WRAM_TRACE/etc. are placed inside the run dir.
1. **Play with watch mode on** (`--config dev-config.ini` keeps `AR_TRACE_WATCH` always-on). When
   anything breaks, the lead-up window is ALREADY on disk (`runs/<ts>/anom_hf<frame>_<kind>.jsonl`;
   the watchdog auto-flushes on hangs). No replay, no flag guessing.
2. **`tools/trace_slice.py <dump> --diagnose`** — needs a fresh `saves/gen_meta.json` (regen.sh
   auto-refreshes it). Read the ranked verdicts: most give a paste-ready cfg line or an explicit
   DO-NOT-REGISTER.
3. **Classify via the m/x-leak decision tree below** — the judgment calls the tools now flag
   AUTOMATICALLY: construct-ret (B8C2 nested-reentry), paired JSR/JSL-return (§7.17
   double-execution), and continuation SHAPE (single-shot/loop-continue → `func`; suspect →
   manual). For anything AMBIGUOUS, decode it yourself with `tools/dis65.py` (m/x-tracked
   disassembly with registration marks) + `tools/romxref.py` (who calls/branches there).
   Wrong registrations no longer crash (the runner's dispatch recursion guard unwinds +
   prints `[dispatch-recursion]` naming the bad line) — but still do the check; the guard
   is a net, not a license.
4. **No trace signal at all?** → not the misdecode/dispatch class. Go by symptom: the channel
   table below (vram/wram/ppumem/hwread/stack/frame), then §5 static tools, then §6 oracle.
5. **Apply the fix** — cfg directive (regen required) or runner/`src` change (rebuild only, §8).
6. **Regen prints its own follow-ups**: the RTS-web census DELTA (newly-reachable uncovered
   continuations — triage them NOW, before they cost a playtest) and the stub/suppression
   reports. `find_rts_webs.py --suggest` emits shape-classified candidates for the delta.
7. **Verify by re-trace, not by eyeball**: the specific signal must flip (`--leaks` empty,
   `--vmadd` shows the right address, the anom dump stops appearing). Never declare a fix from
   "it built" — the `exit_mx_at 039D4D` false fix cost a full round.
8. **Document + commit**: track the bug as an OPEN entry in §7 while working it; on resolution,
   write the ledger entry (root cause + fix + the reusable lesson) into
   [docs/bug-ledger.md](docs/bug-ledger.md), memory update if the lesson generalizes, then
   commit cfg/tool/doc changes together.

### STEP 0 (ALWAYS FIRST): capture a unified `AR_TRACE` and read it before toggling anything

Do **not** start by reaching for a per-symptom flag. Almost every flag below is now folded into the
one-run **`AR_TRACE`** stream (§2), so the first move on *any* bug is: get a deterministic repro,
find the **host frame** of the symptom, and capture a windowed trace. This replaces the old
"guess a probe, run, discover it saw the wrong layer, re-run" loop that cost this project dozens of
runs.

```
# 1. deterministic repro (record once, then replay frame-exact):
AR_HEADLESS=1 AR_INPUT_RECORD=saves/bug.rec ./build/ActRaiserRecomp ar.sfc     # play to the bug
# 2. find the symptom's host frame (screenshots are cheap):
AR_HEADLESS=1 AR_INPUT_REPLAY=saves/bug.rec AR_SHOT_EVERY=10 ./build/ActRaiserRecomp ar.sfc
# 3. capture ALL layers over a tight window around it:
AR_HEADLESS=1 AR_INPUT_REPLAY=saves/bug.rec \
  AR_TRACE=/tmp/bug.jsonl AR_TRACE_HF_LO=<hf-2> AR_TRACE_HF_HI=<hf> ./build/ActRaiserRecomp ar.sfc
```

**Then read the trace in this fixed order — it classifies the bug before you touch a flag:**

1. **`trace_slice.py /tmp/bug.jsonl --summary`** — the triage line. It reports **m/x LEAKS**,
   **DISPATCH-MISSES**, and **GARBAGE-VARIANTS**. This alone tells you the *class*:
   - LEAKS > 0 → **misdecode** (an m/x leak). Go to `--leaks`.
   - DISPATCH-MISSES > 0 → **unregistered RTS-trick/computed target** (register a cfg `func`).
   - GARBAGE-VARIANTS > 0 → execution already ran a known-garbage variant (leak happened upstream).
   - All zero → **not the misdecode/dispatch class** → it's a logic/state/runtime bug; read the
     symptom's own channel (below) or fall through to §11 (silent-drop reports).
2. **`--leaks`** — names the exact call site where runtime m/x diverged from the decoder's
   expectation (**the leak boundary**). Walk back to the previous clean call site to bracket the
   leaking callee; that callee is your `exit_mx_at` target. (This is how the lair-seal `$9D4D` fix
   was found in one run.)
3. **The symptom's own channel** — confirm the *mechanism* and the culprit function, seq-correlated
   with the m/x/S/DB on every line:

| Symptom | Read these channels first |
|---|---|
| Garbled/wrong tiles, black BG | `--vram <addr>` (who wrote it, via which path) + `--vmadd` (crossed pointer?) + `reg` (forced-blank/screen-enable) |
| Wrong palette / colors | `ppumem` (CGRAM writes) |
| Missing/garbled sprites | `ppumem` (OAM writes) |
| Corrupt WRAM value / buffer | `--wram <range>` (all write paths incl. indirect + DMA) |
| Hang / freeze / spin | `hwread` (what `$4210`/`$4212`/APU value the loop keyed on) + `frame` (are NMIs still firing?) |
| Stack/transition crash (`S` walks off) | `stack` (the pushes) + watch **`S` in every prefix** for the drift point |
| Runs at 1/N speed | `frame` markers per game-loop iteration (N per iter = 1/N speed) |
| Wrong m/x width at a call | `--leaks` / `call` channel |

**The m/x-leak decision tree (run `--diagnose`, but understand it).** A `--leaks` hit is a
*symptom*, not the root cause — and the two root causes need OPPOSITE fixes, so **never apply
`exit_mx_at` on a leak alone** (that mistake cost a full round on the lair-seal `$9D8E` bug):

1. Is there a **`dispmiss`** in the window whose **target lands inside the leaking callee's
   function**? → **unregistered RTS-trick continuation.** A handler was reached via a manual
   "push return addr; branch" (`LDY #ret; PHY; BRL h` or `PHA;…;RTS`), and its RTS to the pushed
   address hit no registered entry → host-unwind → the SEP/REP that would restore m is skipped.
   **Fix:** `func bank_BB_TTTT TTTT entry_mx:m,x` (m,x = the width the continuation runs at — read
   `mnow/xnow` at the miss + the surrounding SEP/REP). *This class is INVISIBLE to the stderr
   `[dispatch-miss]` tripwire when `S<$0200`* — always trust the trace `dispmiss` channel, and
   watch `--summary`'s "HIDDEN by the stderr tripwire" callout.
   **⚠️ BUT check the continuation's SHAPE before registering (2026-07-06, the B8C2 crash):**
   a dispmiss target is only registrable as `func` when it's a **single-shot** continuation
   (a loop-continue like `$9D8E` that runs to its own RTS, or a plain RTS trampoline like
   `$03:8712`). If the target is the **`ret:` continuation of a dispatch construct whose frame
   is STILL ACTIVE at the miss** (miss sources = handlers *called by* that construct; target =
   an `indirect_dispatch … ret:` / mid-record-loop address like `$01:B8C2`) → the miss is
   **BENIGN** (host-unwind lands back in the live construct and the loop continues; check that
   `mnow/xnow` at the miss == the loop's width — if so nothing even leaks). Registering it
   converts the benign unwind into **nested re-entry per record → stack overflow** (sim-mode
   crash, dump3/4). Quick discriminator: is the target already a `ret:` in a cfg
   `indirect_dispatch`, or do the miss sources sit *inside* the target's own dispatch web?
2. **No** dispmiss near the leak? → likely an **ambiguous decoded exit** (a callee with two static
   exit paths at different m; the auto-router picked wrong). **Confirm before fixing:** decode the
   leaked region from the ROM at the decoder's EXPECTED m — **garbage/BRK ⇒ decode bug ⇒
   `exit_mx_at <callee> <m> <x>`**; **coherent ⇒ it's NOT exit-mx** (a runtime/value/branch
   divergence — keep digging, e.g. a mis-read RTS-trick as in #1, or a wrong dispatch value).
3. A **`goto`/comment "tail-call past end into <fn>"** in the emitted gen for the leaking function
   is a **red flag for #1** — the decoder ran off a function's end into a pushed-continuation
   handler. (`tools/find_tailcall_past_end.py` censuses these statically.)

**Always verify a cfg m/x fix with a re-trace** — `--vmadd`/`--leaks` must actually flip — *before*
declaring it fixed.

**Only reach for a targeted flag AFTER the trace has localized the site** — and only when you need
something the trace genuinely doesn't carry: a **single address's history across a whole run**
(wider than a practical window) → `AR_WATCHOBJ`/`AR_WATCH16`; **opcode-level** correctness →
`tools/opcode_diff.py`; a **real-hardware cross-check** → the oracle (§6); **whole-run leak
boundary without knowing the window** → `AR_MXHIST=1` (still the right first pass when you can't
yet name the host frame). The per-symptom table below is now mostly the *fall-through* detail for
those cases.

> **Why the stderr tripwire hid this (design note).** The default `[dispatch-miss]` stderr tripwire
> gates on `S>=$0200` (the relocated-stack / RTS-trick danger signature) to keep live stderr
> readable. But a page-1-stack RTS-trick miss (`$9D8E`, `S=$01F2`) slips through that gate. The fix
> is NOT to ungate stderr (it would flood live runs) — it's that the **trace `dispmiss` channel is
> ungated and complete**, and `--summary`/`--diagnose` surface the `S<$0200` class explicitly. The
> stderr tripwire stays as a zero-setup *live* alert; the trace is the source of truth.

---

| Symptom | First tool(s) | Then |
|---|---|---|
| **Hang / freeze / watchdog SIGSEGV** | `saves/dump_state.txt` (auto-written by watchdog: call stack + **block-history ring** with PC/m/X) | `AR_MXHIST=1` to check for a misdecode leak; trace the looping block |
| **Garbage / character disappears / stuck-but-animating / hard crash on an object** | On exit/Shift+F9 inspect `dump_dispatch_log.json` for non-benign `found:0` first (the ring is complete even when default stderr is silent); then `AR_MXHIST=1` if a misdecode/leak remains possible | feed live missing roots to `find_handler_chain.py` (§5); if the ring did not cover the onset, use `AR_DISPMISSALL=1 \| grep -v 'from 00896f'` or take an **F2 full-snapshot** (§9) and scan ≥64 object slots for an active `$12` with no converted `bank_00_*` fn |
| **"Who corrupted this byte/value?"** | `AR_WATCHOBJ=<hexaddr>` (writes to an object slot) or `AR_WATCH16=<hexval>` (a specific 16-bit write) — both log the **writing function + stack + frame** | — |
| **Missing object / event / spawn (logic)** | First check it's not an **unconverted spawn-data handler** (§11): F2 snapshot → object-table scan (**scan ≥64 slots, not 24**) for an active slot with an un-converted `$12`. Else differential oracle: `diff_seq.py` + oracle-only analysis (**must load SRAM** — see §6) | trace the spawn trigger / gate condition |
| **Silent soft-lock: a sequence/cutscene finishes, then NOTHING — no logs, no anomaly, game alive** (§7.20 Death Heim) | `tools/find_yield_helpers.py` (§5) — an unregistered yield continuation misses dispatch EVERY frame *invisibly* (stderr tripwire gates `S>=$0200`; loop runs at S≈$01F5; graceful fallback skips forever) | If census clean: `AR_WRAM_TRACE=wram.jsonl` + decode the object table (80 slots, `$06A0` stride `$40`) — find the slot whose `$12` stops changing / whose `$24` wait expires with no effects; who wrote its `$12/$1E/$3E` names the (new) helper |
| **Rendering wrong (no text, no HBlank, missing BG, black screen)** | `AR_PPULOG=1` (bgmode, brightness, forced-blank, layer enables, HDMAEN) + **oracle screenshots** (`AR_SHOT_AT_GF` vs `SNESREF_SHOT_AT_GF`) | audit the PPU/DMA runtime (`ActRaiserDrawPpuFrame`); WRAM oracle is **blind** to VRAM |
| **Missing/extra sound or music** | check **BRK/COP syscall hooks** (§7) — `$035B`=SFX (BRK), `$035A`=music/event (COP). `AR_COPLOG=1` now includes the exact current block PC and marks a suppressed dialogue glyph blip; the known glyph site is `$01:902D` (`COP #$07`) | `AR_COPLOG=1`; `AR_WATCH16` on the request port |
| **Game runs at exactly 1/2 or 1/3 speed in ONE mode/screen (smooth elsewhere, audio fine)** | This is usually the **pacing/yield-multiplication class**, not host performance. Confirm + count in one shot: quit (or Shift+F9) **while the mode is slow**, then in the dump's block ring **count `02ABF0` (NMI-handler) entries per game-loop iteration** — each entry = one host-frame yield; N entries per iteration = 1/N speed, and the ring block *preceding* each entry names the yield site. Cross-check with any per-frame `[wobj]`/`[frame]` log: updates at delta=N host frames. Known causes so far: a non-HLE'd `$4210` wait yielding per read (§7 the `$9284` fix), and spin-detector false pairing on a twice-per-frame ack helper (§7.12 `$93CB`) | `AR_PERF=1` separates the two classes numerically (fps<60 = host-bound; fps=60 + crawling = pacing). Its `run-ms` covers game execution; `[draw-perf]` covers `RtlDrawPpuFrame`, including host widescreen refresh. A low `run-ms` therefore does not by itself rule out draw-side load. `env AR_FRAMELOG=1 AR_VBLOG=1` names every yield's callsite/block live. NOTE: static `$4210` scans must include the long form `AF 10 42 00`, not just `AD 10 42` |
| **Suspect a single opcode is wrong** | `tools/opcode_diff.py` (Tom Harte differential, §5) | — |
| **Per-frame game-state progression** | `AR_FRAMELOG=1` (callsite, work delta, mode `$18/$19`, timer, HP) | `AR_OBJLOG=1` for the object table |
| **Is the CPU layer even the problem?** | `AR_MXCHECK=1` — if it stays silent through the repro, the m/x layer is clean → look at the **runtime/PPU layer** | — |
| **Crash on a mode/level TRANSITION; SNES stack corrupted (`S` walks to `$FFxx`/`$42xx`/I-O); `ppu_write`/`ppu_read` abort** | Check stderr for **`[dispatch-miss]`** (default-on tripwire) — it names the unresolved RTS-trick/computed target. Then `AR_SCHECK=1` (S-drift + impending-underflow path) | confirm with `AR_RTSLOG=0x<rts_pc>`; register the popped target as a cfg `func` (see §7.7) |
| **A feature/menu/effect just silently never happens — no crash, no garbage, nothing runs** | This is NOT the misdecode class (that produces garbage, not clean silence). Check the regen console output's three silent-drop report sections: **`JSR (abs,X) SUPPRESSED`** (cfg-required-dispatch-or-kill), **`DISPATCH TARGET SUPPRESSED BY DATA_REGION`**, **`Rejected JSR/JSL targets`** — grep the report for the bank/address range of the code you suspect. If none of those name the site, it's probably a genuine logic/state bug (e.g. a gate reading the wrong value) — see the DP-scratch-reuse gotcha above before assuming a memory address means what you think | `AR_INDIRLOG=1` if a suppressed `JSR (abs,X)` site is in range; otherwise trace the gate condition directly (a targeted branch probe — the 4-PC "which branch fired" pattern, bug-ledger "Methodology learnings") |
| **Wrong dispatch-case routing suspected (a runtime `(m,x)` switch calls a variant that doesn't match the caller's real width)** | Check the regen console's **`PROVEN-EQUIVALENT VARIANT ROUTING`** report section for the address — a `<== DIFFERS FROM CANONICAL/DEFAULT GUESS` entry means the OLD "nearest survivor" heuristic and the NEW proof disagree; a regen should already route it correctly. If the address is missing from the report entirely, `_find_equivalent_variants` couldn't prove any survivor equivalent (genuinely no safe target — see §7 "wrong-width dispatch, no provable target") | `AR_MXCHECK`/`AR_CALLMX` first to confirm it's actually reached at the wrong width at runtime (don't assume from static inspection alone — see gotcha #4 above) |

**Golden rule:** capture an **`AR_TRACE`** window first and read `--summary` → `--leaks` (STEP 0
above) before toggling any per-symptom flag. One windowed run classifies the bug (misdecode /
dispatch-miss / garbage / logic) *and* carries every layer (VRAM/WRAM/PPU/DMA/stack/hwread) with
`m/x/S/DB` on every line. Reach for `AR_MXHIST=1` only when you can't yet pin the host frame to
window on (it scans the whole run for the leak boundary); reach for single-address `AR_WATCH*`
only after the trace has named the site.

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

**Read this section in two tiers.** The FIRST tier is `AR_TRACE`/`AR_TRACE_WATCH` + the default-on
tripwires — that's the modern workflow (§1 THE DEBUG LOOP) and it subsumes most of what follows.
The SECOND tier (everything from `AR_MXCHECK` down) predates the unified trace: each flag captures
ONE layer the trace now carries as a channel. They remain for two legitimate uses — (a) whole-run
coverage when you can't name a host-frame window (`AR_MXHIST`), (b) single-address history across
a whole run (`AR_WATCHOBJ`/`AR_WATCH16`, §3) — and as cheap always-on regression guards
(`AR_MXCHECK`/`AR_CALLMX` in dev-config). Do not START an investigation with a tier-two flag.

*(The `AR_TRACE` reference lives just below at its original position — see "unified single-run
trace". Also default-ON with no flag: `[dispatch-miss]` (S≥$0200 only — the trace `dispmiss`
channel is the ungated truth), `[garbage-variant]`, `[dispatch-recursion]` (>24 live dispatches
of one target → self-healing unwind naming the bad cfg line), and `[4210-wedge]` (a whitelisted
vblank spin stuck 4096 reads → prints which gate refused).)*

### `AR_MXCHECK=1` — entry M/X invariant check *(tier two: permanent regression guard)*
Emitted in every function prologue (`ar_entry_mx_check`, see `emit_function.py`). Logs when a
function is entered with `(m,x)` ≠ the variant it was compiled for. Catches **direct-call
variant mismatches** = the emitter's static M/X analysis being wrong.
*Limit:* can't catch a wrongly-*leaked* runtime flag (dispatch always picks the matching
variant) — that's what `AR_MXHIST` is for. Leave it as a permanent regression guard.

### `AR_MXHIST=1` — runtime M/X histogram + live misdecode trap  *(tier two — the fallback misdecode finder when you can't window an AR_TRACE)*
Records per-PC `(m,x)` execution counts (`ar_mxhist_record`, `common_cpu_infra.c`). Once a PC is
established with a dominant `(m,x)` (≥64 hits), the **first** time it runs a different `(m,x)`
prints `[mxhist] MISDECODE? <pc> ran m=.. x=.. (1st time) after <dom> xN f=<frame> caller=<fn>`.
At exit dumps all multi-combo PCs (flags **LOPSIDED** ones). A misdecode = a function running a
variant it normally never runs → instant pinpoint of the leak boundary.

### `AR_DISPMISSALL=1` — computed-dispatch-miss log  *(tier two — superseded by the trace `dispmiss` channel, which is ungated and windowed)*
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

### `AR_SCHECK=1` — SNES stack-pointer corruption tracer *(tier two — `S` is on every trace line now; use this for whole-run drift hunts)*
In `cpu_trace_block` (`cpu_trace.h`). Two outputs: `[scheck]` logs each new high-water page of
`S`; **`[scheck-d]`** logs every block where `S` *jumps* > `$100` (a `TCS`/`TXS` relocation or the
corruption); and a one-shot **32-block path dump at the impending underflow** (`S < $0040`) so the
ring shows the routine draining the stack. Use for stack-corruption crashes. (Remember Golden
rule 3: high `S` is often a legit relocation — the underflow + `[dispatch-miss]` are the real
signals.)

### `AR_STACKPROV=1` — stack pusher-provenance  *(tier two — the trace `stack` channel covers windowed pushes; this one maps pusher-PC per byte whole-run)*
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

### `AR_CALLMX=1` — per-call-site m/x invariant check  *(tier two — folded into the trace `call` channel / `--leaks`; keep on in dev-config as a live tripwire)*
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

### `AR_TRACE=<file.jsonl>` / `AR_TRACE_WATCH=<prefix>` — unified single-run trace  *(TIER ONE — start here; see §1 THE DEBUG LOOP)*
The answer to "why did that take a dozen small runs that each missed a layer." ONE windowed run
emits a correlated JSONL stream across **every** layer, so you never again enable one probe, miss
the path that mattered, and re-run. Channels:
- **`call`** — every JSR/JSL site (from `ar_call_mx_check`) with the DECODER-expected m/x vs
  runtime m/x; `leak:1` when they differ. **This is the misdecode finder** (`AR_CALLMX` folded in):
  it catches a self-consistent m-leak that `func` cannot (the lair-seal `$9D4D` case). Walk the
  flagged site back to the previous clean call site to bracket the leaking callee.
- **`func`** — every function entry with runtime + the DISPATCHED variant's m/x. Note its
  `misdecode` is ~always 0 (the call switch picks the variant by runtime m/x) — only IRQ/NMI-style
  entries trip it. Use `call`/`leak`, not `func`/`misdecode`, to hunt m-leaks.
- **`vram`** — ALL write paths: byte `$2118`/`$2119`, the atomic `WriteVramWord` "word" path that
  bypasses the byte handlers, and DMA.
- **`vmadd`** — `$2116` sets + issuing func (catches a crossed VRAM pointer directly).
- **`reg`** — screen/BG control: INIDISP+forced-blank (`$2100`), BGMODE (`$2105`), mosaic (`$2106`),
  BG SC/NBA, main/sub screen enable (`$212C/D`) → answers "why is the screen black/wrong" in-trace.
- **`dma`** — channel triggers · **`dispmiss`** — computed-dispatch miss (`[dispatch-miss]` folded
  in — the RTS-trick / unregistered-handler root event) · **`garbage`** — entered a known-misdecode
  variant (`[garbage-variant]` folded in).
- **`wram`** — WRAM writes (range-gated `AR_TRACE_WLO/WHI`), across **all four** write paths:
  `cpu_write8/16`, indirect `STA [dp],Y` (IndirWrite), AND DMA-driven writes — so "who wrote this
  WRAM address" is complete in one run (subsumes `AR_WATCHOBJ`/`AR_WATCH*`/`AR_WRAM_TRACE`).
- **`stack`** — writes into the stack page (`$0100-$01FF`) = pushes, with value + who (the
  "who pushed the corrupt return frame" question; pairs with `S` in the prefix showing the drift).
- **`hwread`** — the control-flow-gating hardware reads: `$4210`(RDNMI)/`$4212`(HVBJOY) vblank
  spins, `$4016-7`/`$4218-F` joypad, `$2140-3` APU handshake — the value a spin/branch keyed on.
- **`ppumem`** — CGRAM (palette) + OAM (sprite) writes — the PPU memories `vram` doesn't cover.
  Covers **DMA uploads too** (verified 2026-07-06: `dma_transferByte → snes_writeBBus → ppu_write`
  hits the same `$2104`/`$2122` cases), so "did the actor emit OAM this frame" is one `--ch ppumem`
  query — attribution via `fn` = the function that triggered the DMA.
- **`frame`** — `nmi` / `vblank` boundary markers (attribute writes to NMI vs main thread).
Every event carries a **monotonic `seq`** (survives the non-monotonic `$0088` clock), `hf` (host
frame), `gf`, `fn`, last block PC, and the **live `mnow`/`xnow`/`S`/`DB`/`PB`** — so an m-flip, a
stack drift (`S`), and a wrong data/program bank (`DB`/`PB`) are all visible on every line.
`trace_slice.py` adds `--wram <lo-hi>` alongside `--vram`.
- Enable + window (ALWAYS window — a full run is huge):
  `AR_TRACE=/tmp/t.jsonl AR_TRACE_HF_LO=5089 AR_TRACE_HF_HI=5089` (host frames).
- **`AR_TRACE_WATCH=<prefix>` — ALWAYS-ON anomaly capture (no window, no replay needed).** For deep
  manual play where a replay is infeasible: keeps a rolling in-memory RING of the last N trace lines
  and **auto-dumps `<prefix>_hf<frame>_<kind><n>.jsonl`** (the ring + the next `AR_TRACE_POST` lines)
  the instant an anomaly fires — a **dispatch-miss with S<$0200** (the tripwire-hidden RTS-trick
  class), a **garbage-variant**, or an **m/x leak**; the **watchdog** also flushes the ring on a
  hang. Dedups per anomaly. Lean default channels (func/call/dispmiss/garbage/frame/vmadd/reg);
  widen with `AR_TRACE_CH`. Knobs: `AR_TRACE_RING` (default 4096), `AR_TRACE_POST` (default 400).
  Just play with it on; when something breaks, the window is already on disk → `trace_slice.py
  <dump> --diagnose`.
- Narrow: `AR_TRACE_CH=func,vram,vmadd` · `AR_TRACE_VLO/VHI` (vram word-addr) · `AR_TRACE_FUNC=<sub>`.
- Slice locally with **`tools/trace_slice.py t.jsonl`**: `--summary` (reports m/x LEAKS by site) ·
  `--leaks` (the m-leak boundary — the misdecode finder) · `--misdecodes` · `--vmadd` ·
  `--vram 0000-00ff` (who wrote it) · `--wram <range>` · `--fn 8053` · `--around <seq> --window N`
  (causal neighbours — e.g. VMADD=$0000 at seq N immediately followed by the junk writes at N+1…).
- **`--diagnose` + the `gen_meta.json` sidecar (2026-07-06): the auto-fix-suggester.** Run
  **`tools/gen_metadata.py` once after every regen** (~1s: scrapes all registered func entries,
  every decoder-created local label, tail-call-past-end sites, and the cfg directives into
  `saves/gen_meta.json`). `--diagnose` then joins runtime facts against static decode facts and
  prints, per dispatch-miss target: hit count + modal runtime (m,x) at the miss + tripwire-hidden
  flag + capture-end proximity (watchdog suspects), and the verdict — **NOT registered → the exact
  `func bank_BB_TTTT TTTT entry_mx:m,x` line to paste**; registered-but-variant-missing → width
  mismatch; already-registered-with-variant → stale trace or benign post-fix unwind. Plus which
  function(s) contain the target as a local label (names the dispatch web it belongs to). Works
  first-class for pure-miss captures (no leak needed — the case the old diagnose under-served).
- **The lair-seal in one run:** `AR_TRACE_CH=call,vmadd,vram` then `--leaks` → the leak surfaces at
  `$8053` sites `$80BF/$80C2/$80C5` (m=1, expected m=0); the previous clean call `$80BC` calls
  `$9D4D` → `$9D4D` is the ambiguous-exit culprit. Fix = `exit_mx_at 039D4D 0 0`.
- Choke points live in `ar_trace.c`/`.h` (build list `runner.cmake`), wired at `ar_entry_mx_check`
  (`cpu_state.h`, always-on — the `SNESRECOMP_TRACE` cpu_trace hooks are OFF in the fast build),
  ppu `WriteReg`/`$2139`, `WriteVramWord` (`common_rtl.c`), `dma_startDma`.
- Caveat: the `func` `misdecode` flag only catches **entry-variant** mismatches (runtime m/x ≠ the
  variant's expected). An INTERNAL m-flip after a clean entry (the lair-seal case: `$8053` enters
  m=1 legitimately, an internal REP fails to bring m→0 by `$80C9`) won't flag on `func` — but the
  `vmadd`/`vram` channels catch the *consequence* (VMADD=`$0000`, junk write) directly.

### Legacy misdecode/m-leak pipeline *(superseded — kept for context)*
The pre-trace-era loop was: `AR_MXHIST` (locate the leak boundary) → `AR_DISPMISSALL` (name the
missed handler) → register in cfg → regen. **The modern equivalent is §1 THE DEBUG LOOP**:
watch-mode dump → `--diagnose` does the locate+name+suggest in one shot, and knows the
DO-NOT-REGISTER cases these flags never did. Use the old pipeline only when there is no
capture-able window at all.

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

- **`AR_FRAMELOG=1`** — `[frame] f=N gf=N push+DELTA callsite=BB:PPPP … joy=…
  pos=X,Y d=DX,DY vel=VX,VY h=… state=… boost=… crest=…`. A steady push-delta with the timer ticking = engine running;
  tiny delta = spinning on a wait; frozen timer with large delta = a pause/gate. The callsite is
  the main loop's current vblank-wait point. `f` vs `gf` exposes extra yields; `joy` vs `d`
  exposes input-to-movement loss; Boost `$08C4` and Crest `$08BC` expose the authentic walking
  speed cycle (documented by ActRaiser TAS research). `joy`/`raw` is the
  `SwapInputBits`'d / raw joypad word — pair with a branch probe (§10 legacy inventory) to see whether input is reaching a
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
- **`AR_SETTING_SET=<key>=<value>`** with optional
  **`AR_SETTING_AT_GF=<N>`** — apply one descriptor-backed runtime setting at
  logical game frame N (default 0). This is the Phase-2 headless/live-mutation
  probe and uses the same validation/callback path as the future overlay, e.g.
  `AR_SETTING_SET=display_mode=1` or `AR_SETTING_SET=ws_sprites=0`.

### 4b. Widescreen / rendering probes (added 2026-07-09, policy fn in actraiser_rtl.c)

- **`AR_WS_LAYERS=1`** — per game-frame PPU layer state: BG mode, main/sub enables,
  `windowsel`, `cgwsel/cgadsub`, and per-BG tilemap width (`w0`=32/`w1`=64 tiles),
  tilemap base, hScroll, plus window1/2 edges. THE first probe for "which layer is
  this artifact on / is its tilemap wide enough to have margin content."
- **`AR_WS_ONLYBG=N`** (1..4) — masks the main screen to a single BG layer and forces
  raw wide margins: capture per-layer screenshots to attribute artifacts to a layer
  (how BG1=sky vs BG2=pillars+dialog was separated in the sky palace).
- **`AR_WS_CLAMP=<hex>`** — override the per-layer widescreen clamp mask for tuning
  without rebuilding.
- **`AR_WS_SURVEY=1`** — force raw symmetric wide margins in EVERY mode (the Phase-2
  survey knob; artifacts expected).
- **`AR_WS_ACTION=0`** — restore the pillarboxed action-stage baseline in the
  same binary.
- **`AR_WS_SIM=0`** — restore the pillarboxed simulation-town baseline in the
  same binary. Unset/nonzero enables the town stage: BG1 receives
  asymmetric margins capped by camera `$22` and the 512px `$01:B4C6` world
  bounds; BG2 remains center-clamped for dialogs. It also acts as the master
  gate for the separate `AR_WS_SIM_SPRITES` world-sprite/projectile policy.
- **`AR_WS_BGREFRESH=0`** — restore investigation Stage A: raw wide action
  renderer with stale/wrapped margins. Unset/nonzero selects Stage B: isolated
  true-content BG1/BG2 margin refresh. Both modes retain the original recompiled
  BG streamers and OAM builder.
- **`AR_WS_SKYPALACE_BG=0`** — disable the Sky Palace `$00/$07` render-only
  source-map repair and restore historical raw-wide output with dialogue
  staging visible in the margins. Default-on reads the 16x16 metatile page at
  ROM `$07:D0A0`, reconstructs the box-covered rows 9-12 per column class
  (shaft continuation; seam base halves at meta cols 0/15; floor's top two
  rows at plain columns; `$41/$49` base flare + `$40/$48`/`$42/$4A` skirts at
  shaft columns), expands it through the live `$7E:2900` metatile definitions
  (row-major quadrants: TL,TR,BL,BR), and writes only margin-sampled BG2
  columns for scanout. The game BG2 ring is restored afterward; center box,
  CPU, and WRAM state remain authentic. Validated 2026-07-13: margin decode is
  byte-identical to the game's boot-composed colonnade (rendering-engine.md
  §11 has the layout facts; widescreen-survey.md the fix history).
- **`AR_WS_BG2_MIRROR=0`** — for action sections whose BG2 declares only a
  256px width (`$32<$0200`), disable renderer-side presentation padding and
  restore the proven centered BG2 clamp. The default reflects only BG2's
  authentic rendered edge pixels into the margins, except Aitos Act 1 raw maps
  `$01-$03`, Northwall maps `$01-$05`, and Northwall boss map `$08`, whose
  parallax cloud/snow BG2 is cyclically repeated so its motion keeps the same
  direction across the seam. Bloodpool Act 1 (`0201`) uses both policies on
  BG2: reflected mountains above `y=136`, cyclically repeated animated water
  on `y=136-223`.
  Both modes operate on an isolated BG2 render and never copy BG1 or OBJ. This
  is a startup-cached switch—set it before launching.
- **`AR_WS_BGDBG=1`** — log Stage-B action column-strip and vertical-row counts,
  accepted margins, and any rejected record cursor/VRAM target. A fast vertical
  traversal should produce `rows=N/N` at each 16px camera crossing; long gaps
  while `$24` advances identify a stale-row cadence regression. Every line ends
  in `(state restored)`; rejects are safety stops and should be investigated,
  never bypassed.
  In Sky Palace it also reports `[ws-sky] source=$07:D0A0 meta=$7E:2900
  cols=N ... (render-only)` or a descriptor/header rejection.
- **`AR_WS_SPRITES=0`** — after the Stage-C regeneration, keep the isolated
  `$8D68` port on its authentic horizontal window for the fidelity gate. Unset/
  nonzero widens only per-sprite emission; `$8C98` activation stays original.
- **`AR_WS_SPRDBG=1`** — log every OAM definition emitted outside the authentic
  x range, including owner object, definition address, coordinate, and tile.
  Use with a short reproduction; it can be verbose.
- **`AR_WS_ACTDBG=1`** — read-only Stage-D probe for object slots that intersect
  a live side margin but remain outside `$8C98`'s authentic activation window.
  Logs enter/exit and handler, `$30`, type, and sprite-definition changes; it
  does not itself alter activation, object logic, graphics loading, or OAM.
  With Stage D2 enabled it additionally logs exact `[ws-activation-state]`
  `$0400` transitions, including authentic/draw/activation decisions.
- **`AR_WS_MARGIN_OBJECTS=0`** — keep the validated Stage-D1 `$8C98` port on
  authentic draw coverage. Unset/nonzero draws initialized margin-only objects
  using the live margins. This draw decision is independent of the following
  `$0400` activation switch.
- **`AR_WS_MARGIN_ACTIVATION=0`** — restore the authentic `$0400` activation
  boundary. Stage D2 is now default-on after direct Fillmore validation and
  uses live horizontal margins while retaining authentic vertical coverage
  and status gates. D1 drawing remains independently controlled by the prior
  flag.
- **`AR_WS_SIM_CAMDBG=1`** — log corrected-wide town camera transitions from
  the `$01:B4C6` HLE: follow target, native camera, wide camera, live
  bounds, and whether X/Y shake was accepted. At 16:9 the expected X interval
  is `$002B-$00D5`. Read-only.
- **`AR_WS_SIM_SPRITES=0`** — keep the faithful `$01:ADAD/$01:AE6F` ports on
  their authentic horizontal predicate and keep the angel arrow's `$01:B473`
  lifetime check at the authentic 256px camera
  bounds. Default-on widens `$0A00-$1087` town world-record composition and the
  dedicated `$0B0A` arrow lifetime using the same live asymmetric margins as
  BG1. `$06A0-$09FF` fixed/overlay records, the 512px hard world bounds, and all
  vertical clipping remain authentic.
- **`AR_WS_SIM_SPRDBG=1`** — log town sprite components newly admitted into a
  side margin (emitter, record, component, OAM slot, x/y, live margins, tile,
  attributes) plus `[ws-sim-projectile]` frames where the arrow remains alive
  beyond the authentic horizontal camera window. Read-only diagnostics; it
  does not alter records, OAM allocation, behavior, or activation.
- **`AR_WS_HEADLESS=1`** — opt in to WIDE geometry under `AR_HEADLESS=1` (normally
  headless forces authentic 256-wide so the oracle/differential harness never sees a
  wide framebuffer). THE flag for headless widescreen visual-regression: replay +
  `AR_SHOT_*` writes 342-wide PPMs with no window. The oracle harness leaves it unset.
  Stage B uses the isolated `src/actraiser_widescreen_bg.c` refresh; see
  docs/widescreen-survey.md for its transaction/state-identity gates.

### 4c. Visual-regression harness (widescreen work, generalizes to any rendering change)

1. **Deterministic replay**: `AR_INPUT_REPLAY=saves/simdev.rec` (or record a new one,
   §9) drives the exact same frames every run.
2. **Screenshots at fixed game-frames**: `AR_SHOT_AT_GF=N` / `AR_SHOT_EVERY=N` —
   PPMs land in `runs/<ts>/`. `AR_VRAMDUMP_GF=g1,g2,...` adds headless FULL
   snapshots (wram+vram+cgram+oam) at exact game-frames — no window needed.
3. **Compare NUMERICALLY, never by eye**: slice pixel rows/columns in python
   (`px[(y*w+x)*3]`...). HARD LESSON: pillarboxed shots with dark world content were
   repeatedly misread as widescreen when eyeballing PNGs — a whole survey phase was
   mis-verified that way. Margin checks = count nonblack margin rows + spot-sample
   exact pixel values at the seam columns (x = extra-1, extra, extra+256-1, ...).
4. **Faithful byte-identity gate** for any renderer/engine change: capture the
   deterministic attract frame (`AR_HEADLESS=1 AR_QUIT_FRAMES=1300 AR_SHOT_AT_GF=900`)
   before and after; `cmp` must be identical when the feature is off. (The WRAM
   oracle is blind to rendering — this PPM compare is the pixel-side gate.)
5. **Tilemap forensics**: dump VRAM, decode `| $7000+r*32+c |` words, and classify
   content per column/row (occupancy maps). This is how the dialog-box staging in
   BG2's offscreen half was proven (and how the "no clean pillar source during
   dialogs" conclusion was reached — see docs/widescreen-survey.md).
6. **OAM budget check** (is sprite loss = 128-entry exhaustion?): in a WRAM dump,
   count `$0380` shadow entries whose y byte (`$0381+i*4`) != `$E0` — the shadow
   is cleared to y=$E0 each frame, so non-$E0 = a real entry. 40/128 at a
   corrupted frame ruled exhaustion OUT for the missing-tree-tiles bug (which
   turned out to be the two-tier streaming gap, see widescreen-survey.md).
   The PPU has a render flag for lifting the 32-sprite/34-OBJ-tile limits, but
   ActRaiser currently passes `PpuBeginDrawing(..., 0)` and does not forward the
   parsed `NoSpriteLimits` config value. Current runs therefore always exercise
   authentic scanline pressure; do not claim a `0/1` comparison until the
   runtime-settings phase wires the field.
7. **Framebuffer-gap hygiene**: anything that renders less than the full wide
   framebuffer (asymmetric margin clamps) must CLEAR the unrendered edge strips
   EVERY frame. Change-detection is not enough — a steady clamp never repaints,
   and ghosts of the previous mode linger at the edges (the "stale strip at the
   screen edge" bug class).
8. **OAM-first sprite triage:** identify the expected component group in
   `$0380-$057F` and its high bits in `$0580-$059F`. Missing components point
   to activation/composition/limits; a complete OAM group with invisible
   pixels points to PPU priority/windowing or OBJ character data. Only then
   compare action OBJ VRAM. The regular atlas is `$2000-$2FFF`; dynamic
   magic/effect replacements are confined to `$2D40-$2DBF`, armed through
   `$D0-$D5` by `$00:96C3-$96F5`.

### 4d. Cross-region and town capture matrix

- **Action milestone (2026-07-12):** every action level in regions `$01-$06`
  is fully playable and renders correctly in widescreen. The verified raw-map
  warp targets are in the README and `docs/SEAMS.md`; do not assume the low byte
  is an act number (`0303`, not `0302`, enters Kasandora Act 2). Remaining
  action work is a presentation-aware camera/world-edge clamp. Death Heim
  `0701` currently reaches the first boss arena and crashes; after repair,
  record its `$19` changes through the six act-2 boss arenas and final boss.
  For the camera change or crash, capture stage start, map edges, dense
  encounter, boss/effect, and transition. On an anomaly, pair
  `AR_WS_SPRDBG=1` with `AR_WS_ACTDBG=1`, full
  F2 snapshots or `AR_VRAMDUMP_GF`, OAM counts, and `$D0-$D5`. The normal pass
  needs neither verbose flag.
- **Warp fidelity caveat (2026-07-12):** F6 currently stages only the game's
  destination bytes `$1B/$1A` and request `$FB|=$80`. Invoking it after
  Fillmore act 1 has already begun is an action→action path not observed in
  normal progression, so timing/object state may be inherited. The console now
  records the source `$18/$19` and warns for this case. Treat movement speed or
  core gameplay anomalies as warp suspects until reproduced naturally or with
  `AR_WS_ACTION=0`; do not automatically attribute them to widescreen.
- Town authentic baseline, before widening: replay `simdev.rec`,
  `lairseal.rec`, and `auto_sim.rec` at left/center/right camera positions;
  exercise dialogs, builders/people, lair sealing, rewards, and multi-actor
  cutscenes. Open bug #17 (partial actors) must be captured in 256-wide
  geometry so it cannot be mistaken for a new widescreen regression.
- Town internal attribution: `$01:ACD9` scans fixed records `$06A0` stride
  `$12` and world records `$0A00` stride `$26`; world `+08` is the frame
  pointer, `+0A/+0C` position, `+10` status, `+25` delay. The pointed-to frame
  starts with the component count. A bad count is usually a bad `+08` pointer,
  not record `+0E` and not an OAM cursor reset.

---

## 5. Static analysis (no run needed)

- **`tools/gen_metadata.py`** — **gen/cfg metadata sidecar** *(2026-07-06)*. Run once after every
  regen (~1s). Scrapes `src/gen/*.c` + `recomp/*.cfg` into `saves/gen_meta.json`: every registered
  func entry (pc24 → variants), every decoder-created local label (pc24 → containing functions),
  every tail-call-past-end site, and all cfg directives. This is the static half of
  `trace_slice.py --diagnose`'s join — it's what turns a raw dispatch-miss target into "NOT
  registered, paste this `func` line" vs "registered but variant missing" without grepping 41MB
  of generated C. Keep it fresh: a stale sidecar makes `--diagnose` mislabel new registrations.

- **The STATIC CLOSURE LOOP** *(2026-07-06 — how to stop discovering dispatch webs one
  playtest-hang at a time)*. The regen is already a fixpoint over PROVABLE edges (JSR/JSL/detected
  dispatches — that's what the auto-promote passes are), but pushed-continuation webs and
  data-table handlers are invisible to it (`autoroute_pha_rts` only knows the zelda3 canonical
  byte pattern — 0 hits on ActRaiser's `BF table,X` / `LDY #cont; PHY` idioms). Close the gap by
  iterating the census against the regen until quiescent:
  1. regen (`tools/regen.sh` — now auto-runs the census and prints the **UNC delta**: every cfg
     round makes new code reachable whose pushes were always statically visible).
  2. `tools/find_rts_webs.py --suggest` — emits shape-classified cfg candidates per uncovered
     push: auto-SKIPs `ret:`-of-construct targets (the B8C2 recursion class), suggests
     `func … entry_mx:m,0` with m inferred from which width decodes coherently. **Each suggestion
     still needs the §1 ⚠️ single-shot shape check by eye** — never blind-append.
  3. Where a dispatcher reads handlers from a ROM **data table** (the `$03:F99A` event-record
     table: 6-byte records w/ embedded `handler-1` words), walk the table once and register ALL
     handlers in one batch — 10 of 11 event handlers were missing when the rock-zap hang finally
     pointed there; the table walk would have caught every one of them (incl. the fire event)
     with zero playtests.
  4. Append accepted lines → regen → repeat until the UNC delta is empty.
  Truly runtime-computed dispatch (WRAM JMP vectors like `$6E20`/`$7920` in the event VM) cannot
  be closed statically — trace the vector writers once, then authorize via cfg.

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

- **`tools/find_yield_helpers.py`** — **yield-helper census by SHAPE, not by list** *(2026-07-14,
  built after §7.20's Death Heim pair: the hand-maintained helper list missed `$86FA` and
  `$F778`, costing a soft-lock and a crash in one day)*. A yield helper is any JSR target that
  captures its caller's return address into an object field; every `JSR helper` site makes
  site+3 a computed dispatch entry that MUST be a cfg `func`. Two byte-stereotyped idioms are
  auto-detected (linear decode at m=0,x=0 with push-balance tracking): **PULL** — `PLA/PLX/PLY`
  at balance 0 then a store (± one register transfer / `INC A`) into an object field
  (`$8657→$1E,X`, `$8669` PLY-form, `$86FA→$12,X` wait-N, `$F778→$3E,X` deferred re-push);
  **PEEK** — `LDA $01,S` at balance 0 then the same store (`$8623`/`$A673→$12,X`; non-`$01`
  offsets and nonzero balance are the routine reading its OWN temps — `$C09C`'s spawn-coords
  arg is the guarded false positive, whose continuations are ordinary paired returns =
  DO-NOT-REGISTER §7.17). Exits nonzero listing unregistered continuations; `--lines` emits
  paste-ready cfg lines. **Run after any bank00.cfg handler work.** If a symptom smells like
  "handler never resumes" and the census is clean, suspect a NEW capture idiom: find who
  writes the stuck object's `$12/$1E/$3E` (`AR_WRAM_TRACE` + `wram.py`), then teach the tool
  the shape. Deep-offset ancestor peeks (`$F8A6`-family `LDA $FD,S`) currently have no JSR
  callers; if one grows a site, review by hand.

- **THE CORE TOOLKIT (2026-07-07, built after §7.17's paired-resume arc — use these INSTEAD
  of ad-hoc python heredocs):** all share `tools/ar_lib.py` (LoROM mapping, gen_meta loader,
  ram-map.md symbol table, full 65816 disassembler with m/x tracking, and the canonical
  hazard guards: `paired_return_site` / `construct_ret_guard`).
  - **`tools/dis65.py BB:AAAA [--mx m,x] [-n N | --until-flow] [--raw]`** — disassembler
    with SEP/REP width tracking; inline gen_meta marks (`FUNC[...]`, `label`, `->FUNC`)
    show registration state while reading. Replaces eyeball hex decode (which misattributed
    a caller once during §7.17).
  - **`tools/romxref.py <addr> --kind write|read|call|branch|word`** — alignment-validated
    xref: decodes at each candidate site + forward-sanity + gen_meta proximity, so no more
    raw-byte-grep false positives (`82 88` / `85 A0` class). Long-form aware (found the
    `STA $00:0295` reward-grant write that every `8D`-form grep missed). WRAM targets match
    bank $00/$7E long mirrors automatically.
  - **`tools/wram.py get|diff|scan|syms`** — snapshot/dump inspector with ram-map.md names;
    `diff` = the bracket-snapshot protocol in one command (low page annotated, high WRAM
    clustered).
  - **`tools/resolve_miss.py <anom_*.jsonl|dump_dispatch_log.json> [--apply]`** — the
    mechanized §1 registration decision tree: construct-ret guard (B8C2) → paired-return
    guard (§7.17) → already-registered check → shape classification (single-shot /
    loop-continue / suspect-data) → handler-table evidence → SAFE `func` line or DO-NOT
    verdict. `--apply` appends SAFE lines to `recomp/bankXX.cfg` with evidence comments
    (AMBIGUOUS never auto-applies). Validation: reproduced all 9 §7.17 verdicts blind, and
    its first live run found `$03:CE57` (the third `$9220` coroutine sibling, closing
    ledger #13's untraced `$CE56` loose end).
  - **`tools/cycle.sh [--no-run|--triage]`** — the loop driver: regen-iff-cfg-changed →
    build → run via `tools/run.sh` (watch mode on, everything into `runs/<ts>/`) →
    auto-diagnose every anom capture in the run dir +
    resolve_miss dry-run → `runs/latest/cycle_report.txt` ending in a proposed cfg patch. The
    full bug loop is: `tools/cycle.sh` → repro → quit → read report → `resolve_miss --apply`
    → `tools/cycle.sh` again.

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
  **Diagnostic-only follow-up (2026-07-12):** the `[4210-wedge]` counter itself originally
  compared only the reader block PC. A legitimate once-per-frame ACK at `$00:8465` therefore
  accumulated 4,096 calls across a long Bloodpool playthrough and printed a false wedge despite
  never yielding. It now also requires block-ring adjacency (same index or +1), so only a true
  tight re-read grows the counter. This did not change the static yield whitelist or pacing.

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
  `$1E`/`$12` coroutine-yield idioms), so the static decoder can't follow them → dispatch miss
  → crash/freeze (see §11). Given seed handler addresses it does a 65816 (m0,x0) fixpoint that
  follows control flow including `$8657/$8668/$8669` field-`$1E` yields and instruction-shaped
  field-`$12` yield helpers (`$8623/$86FA/$A66A`; continuation = site+3 = a new entry),
  collecting only true *dispatch* entries (seeds + yield-continuations, not internal branch/jump
  targets), and prints ready-to-paste `func bank_00_… entry_mx:0,0` lines for the unconverted ones.
  - `find_handler_chain.py AC11 AC41 …` — expand specific seeds (from a crash snapshot).
  - `find_handler_chain.py --snapshot <wram.bin> […]` — derive live roots directly from the
    first 64 `$06A0` object slots in one or more F2/exit WRAM dumps, then close all yield chains.
    It handles the dispatch asymmetry correctly: field `$12` is the exact target, while field
    `$1E` stores `target-1`, so the nested seed is `$1E+1` (e.g. snapshot value `$BB15` means
    entry `$BB16`). Field `$14` is excluded here because it is polymorphic; use `--field14`.
  - `find_handler_chain.py --tables` — **comprehensive, all 8 region tables**: bounded-walks each sparse
    per-level table (§11), treating zero as an unused type slot rather than a terminator. It seeds
    exact descriptor roots `recordBase+0x0C`, JSR/JSL post-init roots `recordBase+0x0F`, and direct
    code-table roots, then closes their yield chains. This is how the game's object handlers are
    registered in batches instead of crashing room-by-room. Bloodpool's `$B449` slots `$19-$1D`
    are zero but `$1E-$27` are live; the old stop-on-zero walk skipped `$BB25`. (BRK/COP are
    treated as continuing, not terminal.)
  - `find_handler_chain.py --all-yields` — **the yield-continuation closure**: seeds from *every*
    converted handler and follows all three field-`$1E` helpers plus every detected field-`$12`
    helper, collecting every continuation that isn't itself an entry. Catches handlers reached via
    the **nested `$1E`
    dispatch (`$868F`)** that `--tables` can't (e.g. `$ABE5`, whose miss leaked m=0 → `B127`
    misdecode → `B90D` SIGSEGV). All results are immediately preceded by a yield `JSR` = valid
    entries; the scan fixpoints over chains, so one batch closes the class.
  - `find_handler_chain.py --field14` — **field-`$14` secondary handlers**: `record[0x0A]` is the
    object's initial field `$14`, which is *polymorphic* (handler for some object types, plain data
    — counter/coord/velocity — for others). Value can't disambiguate (no illegal 65816 opcodes), so
    the detector takes code-range `record[0x0A]` values, **drops consecutive-address clusters** (the
    data signature, e.g. `$8502-$8506`), requires a **handler-shaped + coherent decode**, and adds
    their yield chains. Conservative — it skips ambiguous (`COP`/`BNE`/`BRK`-led) values rather than
    register data as code; those fall back to runtime discovery. This is one residual tail that
    `--tables`/`--all-yields` can't reach (e.g. the bridge's original `$ACEA`). Runtime-installed
    roots are another: Bloodpool act 2's exit dispatch ring supplied six roots, whose fixpoint was
    12 entries. Always inspect `dump_dispatch_log.json` for non-benign `found:0` before declaring
    the static closure complete.
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

**Resolved entries (1–16, 20) moved to [docs/bug-ledger.md](docs/bug-ledger.md) — numbering
preserved, so `§7.N` references anywhere (cfg comments, memory, SEAMS) resolve to entry N
there.** New entries: OPEN bugs are tracked below; when resolved, write the ledger entry
(root cause + fix + reusable lesson) and move it to the ledger doc.

17. **OPEN — partial actor sprites in sim cutscenes (2026-07-06).** Two sightings, likely one
    cause: (a) lair-sealing shows ONLY the energy-ball person; the others that "attack" the lair
    never appear; (b) Bloodpool post-act1 lightning cutscene spawns 2 people (2 bolts) but only
    the LEFT one is visible. Pattern: multi-actor spawns render exactly ONE actor. Prior partial
    finding likely related: the **`$9FCD` dispatcher family** (`A21A/A27F/A498/A4A8/A4B8 →
    $9FCD ×4 each`, same `PHY;BRL` idiom as `$9D4D` — `find_tailcall_past_end.py`) was never
    registered/triaged. First moves: play the cutscene with watch mode on (any dispmiss →
    `--diagnose`); if silent, `AR_TRACE_CH=...,ppumem` + `--ch ppumem` to see whether actors 2..N
    ever write OAM (allocated-but-invisible vs never-spawned), then `find_rts_webs --suggest`
    entries for the `$9FCD` family.
    **PARTIAL FIX + MAJOR LESSON (2026-07-07):** dump14's watch captures around the seal
    events (~gf5009) named 11 dropped RETURN continuations; registering ALL of them fixed
    the seal-cutscene actors (user-confirmed) **but introduced double-execution regressions**
    (sim enemies/animations 2x speed, spawn-anim stall, one Fillmore lair stopped spawning).
    **NEW DO-NOT-REGISTER CLASS — the PAIRED-RESUME DOUBLE (3rd cfg hazard class, after
    B8C2 nested-reentry):** never `func`-register a MID-FUNCTION JSR/JSL-return continuation
    of a PAIRED host-C call site. The emitted call site restores S (`cpu->S = _call_s`) when
    the nested chain returns NORMAL and then FALLS THROUGH — so a registered return target
    runs the continuation twice: once nested via dispatch, once via the natural C resume
    (cpu_state.c "Not-found" comment: the miss path IS unwind-and-resume, exactly once,
    i.e. those misses were BENIGN). Reverted as poison: $03:80B3/$80BF (frame-tail after
    `JSR $9CFB`/`JSR $9D4D` — this pair alone doubles the whole sim actor tick
    `$BB94/$89F0/$B90D`), $01:B9F2/$BE55/$C1C6 (`JSR $D04E` returns), $01:C073,
    $01:9829 (`JSL $03F921` return), $03:8270/$86FC (bare-RTS returns — double one level
    up via re-pop). KEPT: $03:CA7A/$03:CDAD (coroutine resumes via slot $9220 — dispatch-
    only, no paired site falls into them; the likely real actor fix) and the reward web
    (§18b — TAIL dispatch, containing fn ends at the RTS; single-execution PROVEN by
    dump15 $0295=01 after one grant). Registration decision tree: TAIL dispatch or
    dispatch-only entry → register; mid-function paired return → NEVER (benign miss);
    construct ret: → indirect_dispatch directive. **VERIFIED 2026-07-07 (post-revert
    regen, two playthroughs): seal-cutscene actors ALL render, Bloodpool lightning
    cutscene shows BOTH people, enemy speed/animations normal (double-processing gone).**
    The kept fix = the $9220 coroutine resumes (CA7A/CDAD/+CE57). If a related loss ever
    resurfaces, the fix is ENGINE-level (cpu_resolve_ancestor_skip should match the
    paired boundary frame itself → single natural resume), not cfg. trace_slice
    --diagnose now DETECTS this class (2026-07-07): if the miss target is site+3/+4 of a
    ROM JSR/JSL whose callee is a known function AND the target is a live label inside a
    decoded function, it prints DO NOT REGISTER (paired-resume double) instead of a
    SUGGESTED FIX. Verified against all dump14/15 captures: flags all 9 poisons + the
    act-mode $00:896F trio ($80B4/$82ED/$8078 = `JSR $8915` returns — benign, never
    register), still suggests the safe ones (CA7A/CDAD), keeps the B8C2 ret: guard.
    $01:B8C2 misses (×27-76/event) remain BENIGN — do not register. The $9FCD family
    may still need entries; re-capture the cutscene post-regen.

18. **RESOLVED (2026-07-07, verify pending): magic casting dead — blocked by our own
    AR_NO_KNOCKBACK cheat, not a recomp bug.** Full chain proven healthy by static decode +
    F2 snapshots: equip menu writes `$02AC` (selected magic, from `$0299,X` HAVE flags via
    `$01:915D`); NMI joypad shadow `$00A0 = $4218 & $F4` at `$02:AC4E`; player handler
    `$00:9832` tests `$A0` BIT `#$00C0` at `$00:9843` → `BRL $9DE1` cast gate:
    `$F8`==0 (no cast in progress) → `$02AC`!=0 (magic equipped) → `$0030,X` (player state
    `$08D0`) BIT `#$2008` must be CLEAR → `$21` (MP) >0 → cast. Snapshots showed every gate
    passing (`$02AC`=01, `$21`=0A, `$F8`=00, `$A0`=40 while holding s) EXCEPT
    `$08D0`=$2003: bit `$2000` = the game's invuln flag, pinned ON every frame by
    `AR_NO_KNOCKBACK=1` (dev-config default since before magic was ever tested — why "it
    never worked"). The gate refuses to cast while hurt/invulnerable. Fix: the cheat is now
    MAGIC-SAFE (actraiser_rtl.c) — it lifts the invuln pin for frames where a cast button is
    held (`$00A0 & $C0`, the same held-A/X byte the $9843 trigger tests; 1-frame NMI lag is
    fine since the trigger is level-sensitive), re-pins otherwise; side effect: hits can
    register while holding the cast button. Lesson: when a game feature is dead with zero
    trace/dispatch signal and all WRAM state looks right, audit ACTIVE CHEATS for state
    bits the gating code reads — cheats are part of the system under test.
    (The earlier scroll-award/persist question — scrolls earned in act mode not carrying
    into the next act — was masked by AR_PIN during this arc; root-caused as #18b below.)

    **Additional confirmed interaction (2026-07-12):** permanent invulnerability also
    prevents the game from applying water drag. A Bloodpool movement comparison initially
    looked like the water handler had been lost during regeneration; the slowdown returned
    immediately when `AR_NO_KNOCKBACK` was disabled. Treat this as authentic game behavior
    under the pinned `$08D0:$2000` state, not a recompilation or handler-coverage failure.
    Disable the cheat when validating water, terrain, damage, knockback, or exact movement
    timing. This also means cheat-assisted input recordings are not physics-neutral.

18b. **FIX PENDING VERIFY (2026-07-07): sim-mode reward grants silently dropped — entire
    reward-handler web unregistered.** Repro (dump14, unpinned run): user received a magic
    scroll in Fillmore sim; before/after F2 snapshots (gf22129/gf22913) show `$21`, `$0295`,
    and the whole `$0290` stats block UNCHANGED — the grant wrote nothing. The watch-mode
    capture between the snapshots (anom_hf22391) held exactly one dispatch-miss:
    `$01:9C82 → $01:9CD6`. Decoded: `$01:9C6F` is an RTS-trick REWARD DISPATCHER
    (fall-through code, itself never an entry): `REP; (id-1)*2 → X; LDA long $019C94,X`
    (20-entry handler-1 table `$9C94-$9CBB`); `LDX #$9C82; PHX; PHA; SEP #$20; RTS`.
    `$9CD6` IS the MP-scroll grant: `INC long $0295` (persistent MP) + `INC long $0021`
    (working copy) + message (`LDY #$8994; JSR $93A8`) + sound + BRK syscall → RTS.
    None of the 17 unique handlers (nor the shared `$9C83` PLP;RTS continuation) were
    decoded → EVERY sim reward type silently host-unwound. Fix: 18 `func ... entry_mx:1,0`
    registrations in bank01.cfg (single-shot class per §1 — handlers are linear to their
    own RTS; `$9C83` is an `$03:8712`-style trampoline; NOT the B8C2 mid-loop kind).
    WRAM model learned: `$0295` = PERSISTENT MP (part of the `$0290` save-stats block:
    $0291 level, $0293 HP, $0295 MP, $0297 next-level pop, $0299+ HAVE flags); `$21` =
    act/working copy, loaded from `$0295` at `$02:84E0` (`LDA $0295; STA $21`); act-mode
    pickups INC only `$21` ($00:887E via the $00:87BD item dispatch). Grep found NO
    direct writer of `$0295` anywhere in ROM other than new-game STZ ($02:BE69) and this
    long-addressed handler — future stats-block bugs: suspect event/reward handlers with
    `AF/8F ... 00` long addressing, not `8D`-form stores. Verify: re-trigger any sim
    reward → `$0295`/`$21` increment + scroll usable in next act, no new dispmiss.
    **VERIFIED 2026-07-07 (dump15/17):** scroll granted, `$0295`=01 (exactly once — also
    the proof the reward web's TAIL-dispatch shape single-executes), persists across
    modes, castable in the next act.

19. **OPEN — NEW (2026-07-07, dump17 f≈25.8-26.2k): `bank_00_B8AB_M0X0` garbage-variant
    ×3 in act mode — first bug in the NEWLY-OPENED magic/projectile path.** Context: this
    was the first session where casting works at all (#18), the first with
    AR_RANGED_SWORD, and the user was testing spells in act stages — code that has never
    executed before. Signal: `[garbage-variant] entered MISDECODE variant bank_00_B8AB_M0X0
    (m=0 x=0)` from callers `bank_00_B82E/B842/B868` (a sibling family — plausibly
    per-spell/projectile spawn handlers); capture saved (anom_hf25833_garbage11.jsonl).
    Static check (tools/dis65.py): the m=0 decode of $B8AB is COHERENT
    (`JSR $853D; LDA #$B8E9; STA $0012,Y` — plants a handler pointer — `LDA #$FFE0;
    JSR $8709` — velocity setup) while m=1 decodes to garbage — so the RUNTIME width
    (m=0) is right and the emitted M0X0 variant is a mis-synthesized wrong-width sibling
    (decoder assumed the entry is m=1). Likely fix shape: `func bank_00_B8AB B8AB
    entry_mx:0,0` (or an entry_mx correction on the containing decode) — but VERIFY the
    visible symptom first (which spell/effect misbehaves?) and check the B82E/B842/B868
    callers' own decode assumptions before touching cfg (§1 decision tree; NOT obviously
    a paired-return — the callers dispatch it, garbage-variant not dispatch-miss).
    Same-session dumps also show the recurring act-mode `$00:896F` object-loop misses
    ($8078/$80B4/$82ED ×~25/frame-window) now correctly auto-classified DO-NOT-REGISTER
    (paired-returns of `JSR $8915`); their residual m/x-leak-on-unwind is the suspected
    feeder of the B8AB wrong-width dispatch — if #19 needs an engine fix, that's the
    thread to pull (ancestor-skip carrying restored m/x).

20. **RESOLVED + moved to [docs/bug-ledger.md](docs/bug-ledger.md) #20** — Death Heim boss
    rush (one crash + one silent soft-lock, both unknown yield helpers; user-verified
    end-to-end 2026-07-14). The reusable rule: **after any bank00.cfg handler work, run
    `python3 tools/find_yield_helpers.py`** (§5) — it derives the yield-helper census from
    ROM byte shape and exits nonzero on any unregistered continuation.

21. **FIX LANDED (2026-07-14, regen + verify pending): ending/credits never start —
    mode `$18=08`'s entry long-jump target was unconverted.** After the final boss the
    ending montage plays (mode 0, `$19=09`↔towns), then the fade routine's special case
    `$00:82C3` long-jumps into the credits presenter via `LDA #$02; PHA; LDX #$AA9B;
    PHX; RTL` → `$02:AA9C` — which had NO func and NO label → dispatch-miss (`from
    0082D0 to 02AA9C`, hf=47576 in runs/20260714-184728) → host-unwind skipped the
    whole presenter; the game sat alive in mode 8 with only NMI frames running
    (credits invisible; user report + healthy exit dump running bank_02_BF52/BEB8).
    Fixes: `func bank_02_AA9C AA9C entry_mx:1,0` (bank02.cfg) + `func bank_00_8059
    8059 entry_mx:1,0` (bank00.cfg — the presenter's own exit RTL at `$02:AAFD`
    returns to the main-loop top after a Start-button wait; registering both closes
    the ROM's ONLY two RTL-long-jump sites, verified by whole-ROM byte scan of the
    `LDA#/PHA/LDX#/PHX/RTL` and `PHK/PEA/RTL` shapes). Both are dispatch-only tail
    jumps (`$AA9C` relocates S to `$01FF` on entry, never returns — no paired-resume
    hazard). The presenter stamps 'ACT' into SRAM `$70:1FF0` = the beat-the-game
    marker. WATCH ITEM from the same run: one-shot `01ACD9 ran m=0 (1st time after
    31200× m=1)` from `bank_03_D020_M1X0` at f=46977 (ending-prelude flyover) — no
    garbage-variant fired and the montage displayed fine; re-check only if the ending
    shows visual corruption after the regen.
---

## 8. Build & regen workflow

- **Changed a runner source** (`src/*.c`, `third_party/snesrecomp/runner/src/*`) → **rebuild only**:
  `cmake --build build -j8`.
- **Changed the emitter** (`third_party/snesrecomp/recompiler/v2/*`) **or a cfg**
  (`recomp/*.cfg`) → **regenerate then rebuild**: `bash tools/regen.sh` (rewrites `src/gen/*.c`),
  then `cmake --build build -j8`. `src/gen` is generated — **never hand-edit it**; regenerate via
  `tools/regen.sh`.
- Large generated banks are now split into stable
  `bankXX_partNN_v2.c` translation units. Do not assume a monolithic
  `bankXX_v2.c` exists when checking freshness or build artifacts;
  `tools/cycle.sh` uses `src/gen/.v2_regen_stamp`, and CMake discovers all
  `src/gen/*.c`. A configure also removes stale generated-bank objects whose
  source basename no longer exists after a split/merge.
- Constraints: **no stubs, ever** (a stub is a hard build error — close the recompiler gap).
  Edit only the emitter, runner, `src/main.c`, `src/actraiser_rtl.c`, `recomp/*.cfg`, and
  `tools/`. Commit/push only when asked.
- **`hle_func` semantics (fixed 2026-07-10)**: the replaced function's REAL body is still
  decoded + fed through codegen during regen (its text discarded, the forwarding stub emitted
  instead) so callees reachable only through it keep getting auto-promoted/emitted. Before this,
  hle'ing a function silently dropped its exclusive callees from emission (e.g. `$00:923A` via
  `hle_func 8C98` → undefined-symbol link error). Register-everything-first, then substitute.
  An undecodable hle body falls back to stub-only, as before.

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
- **F3 scene inspector (manual play):** press **F3**, then left-click a visible
  graphic. The game pauses on that frame and a compact, color-coded host debug
  panel identifies
  matching BG tiles and OAM sprites; the console contains VRAM addresses,
  palette/priority/flips, census-compatible tile hashes, Mode-7 canvas
  coordinates, current `$18/$19`/camera/PPU state, and a starter replacement
  manifest gate. The compact panel appears opposite the clicked point and can
  be repositioned by left-dragging it. Right-click clears and resumes an
  inspector-owned pause; F3 disables. Drag the cyan lower-right grip to scale
  the whole panel down or up without losing report columns. This is the
  fastest first step for “what
  graphic is this and which replacement plane owns it?”
- **Init/forcing:** `AR_WRAM_FILL`/`AR_WRAM_INIT`/`AR_SRAM_FILL` (poison/seed memory),
  `AR_FORCE18` (pin game-mode), `AR_NOPOP` (disable the vblank-wait RTS-frame pop, for ABI tests).
- **Battery-save codec:** `python3 tools/srm.py check saves/save.srm` validates
  exact size/checksum and prints the six combined town-state fields
  (`$1200+r*2` base plus `$13B6+r*2` Act-2 bit). `decode`,
  `diff`, `edit --region northwall=act2-cleared`, and `convert` share the
  runtime codec's version-1 lossless schema. `AR_SAVE_NATIVE_PATH` and
  `AR_SAVE_INI_PATH` redirect the active runtime target for isolated tests;
  `AR_SAVE_IMPORT` selects a non-default import source. The in-game editor adds
  paged Status/Magic/Items/Scores fields through the transactional C codec; see
  `docs/save-format.md` §3 for their USA offsets and encodings.

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

**From the resolved 2026-07-01 sim-mode arc** (all theories they tested are closed; the probes
remain inert-unless-set): `AR_SIMTRACE`/`AR_SAVECHECK` (see the legacy inventory below +
bug-ledger "Methodology learnings"), `AR_INDIRLOG` (general-purpose — worth keeping for any
future suppressed-dispatch site), `AR_ADADTRACE`, `AR_WATCH14`.

### Case study archive
The full sim-mode bring-up arc narrative (2026-07-01→04) lives in
[docs/bug-ledger.md](docs/bug-ledger.md)'s appendix.

Current crop (this debugging arc): `AR_B90D_CATCH`, `AR_B127_CATCH`, `AR_896E_CATCH`,
`AR_8A3C_CATCH`, `AR_8664_CATCH`, `AR_STRACE`, `AR_EVTRACE`. Legacy from prior arcs:
`AR_CALLTRACE(_GF)`, `AR_CTACTION`, `AR_FUNCLOG`, `AR_MLOG`, `AR_SPAWNLOG`, `AR_8966X(_GF)`,
`AR_92CBLOG`, `AR_B127LOG`, `AR_DISP8465`, `AR_TRAP8465`, `AR_1EHIT`, `AR_SIMTRACE` (4-PC
sim-dispatch branch probe; the reusable "which branch fired" pattern — retarget the PC list in
cpu_trace.h), `AR_SAVECHECK` (save-checksum gate PASS/FAIL; gate semantics documented in
SEAMS "Save / persistence"). Grep the source for
specifics if one looks relevant; otherwise ignore.

---

## 11. Reference: object & spawn-handler model

Moved to [docs/SEAMS.md](docs/SEAMS.md) ("Object & spawn-handler model" section) — it is
game-architecture reference, not debug process. Quick anchors: object table `$06A0` stride
`$40`, **scan ≥64 slots**; handler `$12 = recordBase+0x0C` (init) → `+0x0F` (steady state);
unconverted-handler crash class + `find_handler_chain.py` derivation recipe are described there.

## Appendix: one-line cheat sheet

```
== TIER ONE — the loop (§1): ==
runs/<ts>/ (NATIVE)         every invocation ringfences console.log + run_info.txt + all dumps
                            (src/run_dir.c); runs/latest symlink; AR_NO_RUN_DIR=1 = legacy layout
AR_TRACE_WATCH              always-on anomaly capture (defaults into the run dir; dev-config's
                            saves/anom line = legacy fallback): auto-dumps the lead-up
                            window on hidden dispmiss / garbage-variant / m-x leak / watchdog hang
trace_slice.py <dump> --diagnose   ranked verdicts + paste-ready cfg lines (needs gen_meta.json)
AR_TRACE=<f> + HF_LO/HI     targeted windowed capture (beats watch mode when both set)
tools/gen_metadata.py       ~1s static sidecar refresh (auto-run by regen.sh)
AR_PIN=<par8>[,..]          generic PAR-code pinner (codes.txt catalogue -> instant debug cheat / state probe)
find_rts_webs.py --suggest  shape-classified cfg candidates for uncovered continuation pushes
regen.sh census delta       "NEW uncovered continuations" printed per regen — triage BEFORE playing
[dispatch-recursion]        DEFAULT-ON: >24 live dispatches of one target -> self-healing unwind;
                            names a bad (mid-loop) cfg func registration to REMOVE
[4210-wedge]                DEFAULT-ON: vblank spin stuck 4096 reads -> prints which gate refused

== TIER TWO — fallbacks / single-layer flags: ==
AR_MXHIST=1            misdecode leak boundary, WHOLE-RUN (fallback when you can't window a trace)
[dispatch-miss]        DEFAULT-ON stderr tripwire — but GATED S>=$0200; the trace dispmiss channel
                       is the ungated truth. AR_DISPWARN=1 adds stack; AR_NODISPWARN=1 silences.
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
regen report           "PROVEN-EQUIVALENT VARIANT ROUTING" section: wrong-width dispatch routing, proven not guessed (§5)
regen report           "JSR (abs,X) SUPPRESSED" / "Rejected JSR/JSL" / "DISPATCH TARGET SUPPRESSED" sections: silent-drop audit (§7.9)
AR_PERF=1              once-per-second game + draw budgets: [perf] fps/run-ms/gf/APU and [draw-perf] RtlDrawPpuFrame ms — separates host-bound (fps<60) from pacing (fps=60 but game crawls). CAVEAT: gf is NMI-driven, always 1:1 — it can NOT detect the pacing class; use the ring trick below
AR_APUPROF=<ms>        per-frame stall attribution: any game frame >= <ms> (bare `1` = 8 ms default) prints [apuprof] splitting the frame into game-thread lockwait / SPC catchup (ms+cycles+calls) / $2140 handshake reads+writes / music-hook / upload HLE, plus schedlat (samples the last port write was scheduled past `produced`), pushes+loops (execution volume — loops = loop-header count; pushes alone under-report straight-line decompression), and audiowait-max (worst blocked AudioCallback acquire since last report — >16 ms = missed fill deadline = audible dropout). This is the tool that root-caused the 2026-07-16 transition dropouts: collapsed multi-hardware-frame map loads (loops 25k-75k vs ~3k normal) under the then-frame-wide main-loop APU lock starved the callback 12-23 ms; the frame-wide lock is now removed (fine-grained locks inside every APU path already serialize) — audiowait-max stays ~0 through 55 ms loading frames
Shift+F9 mid-bug + ring exact-1/N-speed in one mode? quit/Shift+F9 WHILE slow, count 02ABF0 (NMI) entries per iteration in the block ring = yields per game frame; block before each = the yield site (found §7.12 in minutes)
find_yield_points.py   static census of ALL $4210/$4212 reads (incl. AF long form) classified SPIN/CLEAR/POST/ACK + HLE cross-ref; its 7 SPIN sites ARE the runtime yield whitelist (snes.c kSpinBlocks — keep in sync!); unlisted spin = watchdog hang naming the block (loud), never silent slowdown
find_rts_webs.py       static census of the PHA;RTS pushed-continuation dispatch idiom (A9../A0.. +48 pushes, 48 60 sites) vs cfg coverage; run FIRST on a silent-no-op sim subsystem to see the whole uncovered backlog in one pass (§5, §7.13). RAM-ptr handler targets still need runtime found:0
find_yield_helpers.py  yield-helper census BY SHAPE (pull/peek of caller frame -> object-field store) + every JSR site's continuation vs cfg; exit!=0 = unregistered = future silent soft-lock (§7.20). Run after ANY bank00.cfg handler work; --lines = paste-ready fixes
AR_RTSDISP_MISS=1      names any continuation a `rts_dispatch` list doesn't cover (site + popped target); benign JSR-return fall-throughs also print — check the popped value before adding a mapping
AR_GARBAGE_HIST=<n>    garbage-trap block-ring depth (default 24, max 1000) incl. per-block S — 1000 spans a whole sim frame; how the dev-cycle m-leak origin was found (§7.13)
AR_SIMDEV=1 / AR_SIMDEV2=1   dev-cycle gate-branch probes (cpu_trace.h; retarget the PC list per hunt — the reusable "which branch fired" pattern)
AR_SIMWALK=1           town-actor script executor: script byte + behavior state ($+12) + script ptr ($+16) at $01:CD41/$CD5E — "actor spawns but is frozen" (§7.14)
AR_VRAMWATCH=1         BG-tilemap VRAM-write tracer: who writes a VRAM region + game func, gated AR_VW_LO/HI (game-frame) + AR_VW_VLO/VHI (vram addr) — graphics-corruption hunts (§7.15)
AR_LAIRDMA=1           logs each VRAM-targeting DMA's source ($7E/$7F buffer + first bytes) + dest + size (dma.c) — finds the garbage tilemap/tile-gfx SOURCE behind a corrupt strip (§7.15)
AR_LOADSTATE=<slot>    boot-time savestate load (main.c). CAVEAT: renders the state but game LOGIC won't advance (coroutine host-stack not restored) — not usable to resume/instrument from a captured moment
AR_AUDIODBG=1          DSP health: mvol/mute/output peak/SPC cyc-per-ms/pending KON  (peak=0 = silence GENERATED, not a device problem)
AR_KONLOG=1            DSP key-on writes + per-voice state at key-on (vol/pitch/ADSR/BRR first-bytes — all-zero BRR = samples never uploaded, §7.11)
AR_APULOG=1            APU port traffic + SPC upload blocks (+ stage2 sample-chunk streaming)
AR_PPULOG=1            rendering: bgmode/bright/fblank/layers/hdmaen
AR_FRAMELOG=1          per-frame mode/timer/HP/callsite/joypad
AR_OBJLOG=1            object-table health
AR_SHOT_AT_GF=N        recomp screenshot -> saves/shot.ppm
F2 (windowed)          full snapshot WRAM+VRAM+CGRAM+OAM+ppm -> saves/snapshots/  (VRAM-visible!)
F3 + left click        freeze/inspect identity + manifest hints; drag panel; right click clears/resumes
diff_seq.py            timing-independent oracle value-divergence (LOAD SRAM!)
oracle-only pass       missing object/event (oracle writes nonzero, recomp never)
find_handler_chain.py  unconverted object handlers (<seeds> or --tables for all action regions); see §11
opcode_diff.py         single-opcode semantics vs Tom Harte
```

Object/spawn model & the spawn-handler crash class: **§11**. Object table = `$06A0` stride `$40`,
**scan ≥64 slots**; handler `$12 = recordBase+0x0C` (init) → `+0x0F` (steady state).
