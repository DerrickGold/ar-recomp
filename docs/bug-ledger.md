# ActRaiser Recomp — Bug Ledger (resolved entries)

Historical record of every root-caused bug class and its fix. **Entry numbers are preserved
from DEBUG.md §7** — a reference like "DEBUG.md §7.15" anywhere in the cfgs, memory notes, or
docs resolves to entry 15 HERE. OPEN entries live in DEBUG.md §7 (the working set) and move
here when resolved (THE DEBUG LOOP step 8).

Each entry records: root cause, the fix, and the reusable lesson. Read §0/§1 of DEBUG.md for
the current debugging process; this file is the case law.


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

14. **Town people (dev-cycle walkers AND the church-cutscene pair) spawn but never MOVE —
    a missing ACTOR-SCRIPT-COMMAND dispatch.** *(FIXED + user-confirmed 2026-07-04.)* This
    was the last layer of the town-people bug and a different mechanism than §13's dispatch
    web — zero garbage-variant / dispatch-miss, so NOT a decode or found:0 problem; a pure
    behavior-state stall. Full trace chain (reusable for any "actor exists but is frozen"):
    - Actors spawn fine (status `4→8`), but the recomp HIDES them ~22 frames later while the
      oracle holds them ~238 frames — the actor's walk *animation is racing ~17x too fast*, so
      "development" completes almost instantly and retires the people before they visibly move.
    - `AR_WATCHOBJ=<recordbase>` named the per-frame processor: the actor's behavior state
      (`record+$12`, e.g. `$0E14`) selects a handler via the `$D04E` dispatch
      (`$01:CD0C: LDY #$CD12; BRL $D04E`, table `$CD12`): state 0 → `CD22` (PACED, `JSR $AC70`
      decrements the pacing timer), state 1 → `CD35` (UNPACED, advances the walk script every
      frame). The recomp was **stuck in state 1** — `$0E14` never advanced to 3 (`CEFA`, the
      real paced walk), so it raced.
    - `AR_SIMWALK` (block probe on `$01:CD41`/`$CD5E`) showed `CD35` reads a script byte via
      `CFC7` (from a `$7F:xxxx` stream), and for a non-`$7F` byte dispatches it through table
      `$CD6F` (`$CD5E: LDY #$CD6B; PHY; PHX; ASL; TAX; LDA $01CD6F,X; PLX; PHA; RTS`). Byte 3's
      handler `$CDCC` sets `$001E,X` (walk counter) + `LDA #$3; STA $0012,X` (state=3). But
      `find_rts_webs.py` had flagged `PHA;RTS @01:cd6b` UNCOVERED, and the handlers `$CDCC`
      etc. had **0 emitted blocks** — so the dispatch silently no-op'd and state=3 never wrote.
    - Fix: `indirect_dispatch CD6A 18 idx:A tables:CD6F ret:CD6C` (bank01.cfg) — same idx:A /
      PHX-PLX / ret shape as `B8C0`. People now walk.
    - **Two lessons:** (1) a frozen-but-spawned actor whose per-frame processor DOES run but
      the state never advances = look for a script-command dispatch the state-machine needs;
      the script is byte-stereotyped so `find_rts_webs.py` finds it. (2) The engine's animation
      pacing is data-driven per-script-step, NOT a fixed delay — the "17x too fast" was the
      whole script being consumed at 1 byte/frame instead of honoring per-step delays that the
      script-command handlers install. Tools added: `AR_SIMWALK`, `AR_SIMDEV2`.

15. **Sim-mode lair-seal graphics corruption — FIXED 2026-07-05** (`func bank_03_9D8E 9D8E
    entry_mx:1,0` in bank03.cfg; user-confirmed). Symptom: sealing a lair garbled a broad set of
    shared BG1 tiles (map edges/rock, trees, river, lair, TEXT-BOX borders); persistent.
    **Root cause: an unregistered RTS-trick CONTINUATION.** `$03:8053` calls the actor dispatcher
    `$03:9D4D` at `$80BC` and must get m=0 back. `9D4D` is a table loop (`SEP #$20`; per entry
    `BPL $9D8E` to loop-continue, else `LDY #$9D8D; PHY; BRL <handler→$A4F7>` — PUSH return `$9D8D`,
    branch to handler which RTSes back to **`$9D8E`** to resume the loop; the loop ends `REP #$20;
    RTS` at `$9D9C/E` returning m=0). The decoder mis-read the `BRL`→`$A4F7` as a "tail-call past
    end", so the handler's RTS to `$9D8E` hit **no registered entry → host-unwind → returned to
    `$8053` at m=1**. Then `$8053`'s upload setup at `$80C9` (`A9 00 60` = LDA #$6000; STA $2116)
    ran at m=1 → LDA loaded only `$00` → **VMADD=`$0000` not `$6000`** → the `$8100` loop uploaded
    the tilemap's tile-INDICES into BG1 char VRAM `$0000` (`bgTileAdr=$0500`) = garbage tiles.
    ROM decode of `$80C9` proved it is coherent ONLY at m=0 → hardware's `9D4D` loop MUST complete
    to its m=0 RTS. **Fix:** register `$9D8E` as a dispatch target at its runtime width (m=1 after
    the SEP, x=0) so the handler RTS resumes the loop.
    **WATCH OUT — two false starts on this bug (both now guarded):**
    (a) `exit_mx_at 039D4D 0 0` was tried FIRST and did **nothing** — `exit_mx_at` steers only
    *decode-time* m; here the decode was already correct (m=0) and the leak was a *runtime*
    host-unwind. **An m/x leak is NOT automatically an exit-mx bug** — see the §1 decision tree.
    (b) The `[dispatch-miss]` stderr tripwire **hid** the root event because it gates on `S>=$0200`
    and this miss had `S=$01F2`; the ungated `AR_TRACE dispmiss` channel (`03A590 -> 039D8E`) is
    what found it. `tools/find_tailcall_past_end.py` flags the `9D4D→A4F7 ×7` fingerprint statically
    (and a whole `→$9FCD` sibling family — likely the same class, watch for missing-actor bugs).
    **The trap that cost ~10 runs on the *background* half (now permanently solved — see `AR_TRACE`):**
    the tilemap indices reach VRAM via the per-frame `AF3D` DMA whose source buffer `$7F:BB00` was
    filled by `BAF5`'s `$2139` readback of the (already-corrupt) VRAM `$0000` — a self-copy loop that
    PERPETUATES but doesn't inject. The injecting writes used atomic 16-bit `STA $2118` →
    `WriteRegWord`/`WriteVramWord` → direct `ppu->vram[]`, which
    **bypasses the `$2118`/`$2119` byte handlers** every VRAM watch hooked, so byte-level watches
    (`AR_VRAMWATCH`, ppu case `0x18`) never saw it. Also the game-frame `$0088` clock is
    NON-MONOTONIC near cutscenes → gate probes on **host frame** (`snes_frame_counter`), not `$0088`.
    Repro: `saves/lairseal.rec` (corrupt onset host-frame ~5089). Oracle still blocked (can't seed
    snes9x from a coroutine savestate; `AR_LOADSTATE` renders state but logic won't advance — the
    coroutine host-stack isn't restorable). Found in ONE run with `AR_TRACE` (below).

16. **Story-event system (rock-zap hang + fire event + the whole $F921 web) — FIXED 2026-07-06.**
    Three stacked fixes: (a) `func bank_03_FA5F/F98A` — the event dispatcher's pushed continuation
    chain, found link-by-link via `AR_TRACE_WATCH` + `--diagnose` (each registration exposed the
    next handler's RTS target); (b) the **static table walk** that ended the link-by-link cycle:
    the dispatcher's record table (`$03:F99A–$F9F4`, 6-byte records w/ embedded `handler-1`
    words) enumerated ALL 11 handlers — 10 were unregistered, batch-registered in one round
    (fire/quake/etc.); (c) the `$4210` non-yieldable-context fast-exit + `[4210-wedge]` tripwire
    in snes.c (the miss's host-unwind had left S +2 in the mode-$85 wait chain → the $9293 spin
    wedged). CONFIRMED 2026-07-06: rock-zap completes, fire event animates, Fillmore cleared
    end-to-end (act1→sim→act2→sim), Bloodpool act1 + sim transition clean.
    **Runner hardening from this arc:** the dispatch **recursion guard** (`cpu_state.c`
    `_cpu_dispatch_once`): >24 live dispatches of the same target → unwind + one-shot
    `[dispatch-recursion]` warning naming the bad cfg line, instead of C-stack overflow. This
    makes a wrong `func` registration self-healing, which is what allows the §5 static closure
    loop (`find_rts_webs --suggest`) to run without the manual shape check being fatal-if-wrong.

---

## Appendix: Case study archive: the sim-mode bring-up arc (2026-07-01 → 07-04, RESOLVED)

This section previously held the full ~550-line chronological narrative (wrong turns included) of
the sim-mode debugging arc. Everything in it was RESOLVED and its durable conclusions extracted;
the narrative lives in git history (`git log -p --follow DEBUG.md`, commits around 2026-07-01..04)
if archaeology is ever needed. The distilled outcomes:

- **Decoration-icon OAM corruption + silent town handlers** → the `$03:F5BE` per-town handler
  subsystem was undecodable statically; fixed via the value-keyed `indirect_dispatch F5DF …
  sep:20` directive + `_identify_leaders` promotion (§7.13 ledger; cfg comments in bank03.cfg).
- **4 corrupt town actors / missing init** → the `$01:D04E` actor-behavior RTS-trick family
  (~189 targets across 24 bank01 tables + `$03:8700`/`$E1D2`) registered wholesale from
  `dump_dispatch_log.json` — the precursor of today's §5 closure loop.
- **Path-tracing / lightning ½-speed** → spin-detector false pair on `$01:93CB`; replaced two
  generations of runtime heuristics with the static `kSpinBlocks` whitelist (§7.12,
  `find_yield_points.py`).
- **`$033C` spawn-list "divergence"** → DP-scratch reuse by `bank_01_CFF2`, a red herring; the
  oracle WRAM diff is address-sorted, NOT temporal — stack-page bytes in a diff are residue, not
  writes (this lesson is now §0 gotcha material).
- Methodology extracted into: §1 THE DEBUG LOOP, the m/x decision tree, §5 static tools
  (`find_rts_webs`, `find_yield_points`, `find_handler_chain`), and the AR_TRACE channels.
