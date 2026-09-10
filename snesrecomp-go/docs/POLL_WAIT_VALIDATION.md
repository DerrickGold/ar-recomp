# Cooperative WRAM polling — 2026-09-09

This records the **polling-only** milestone. The subsequent
[callee-clean return validation](CALLEE_CLEAN_VALIDATION.md) resolves the
remaining editor-entry failures and installs the passing candidate in Mario
Paint. Failure and deployment statements below describe the earlier control.
The old `/private/tmp/snesrecomp-*` artifacts were removed by external cleanup;
fresh controls, candidates and logs are now under `build/callee-clean.NCVuAh/`
in the enclosing ActRaiser checkout.

## Result and scope

The recorded Mario Paint title click exposes a second NMI-dependent wait,
outside the adapter's originally selected frame-wait PC. The compiler now
offers a narrow, structurally recognized WRAM load/test backedge to an
optional synchronous runner callback. An isolated Mario Paint adapter uses
that callback for its existing balanced native NMI/frame transaction; it no
longer schedules by a hardcoded PC or RAM flag. There are still zero authored
functions, dispatches, width overrides, or HLEs in that project.

**The original click watchdog is resolved in the isolated candidate, but
editor entry is still blocked by a separate callee-clean return defect.**
The production Mario Paint adapter/generated directory has not been replaced
by this polling candidate. Do not describe the full recorded run as passing.

The public contract and opt-in example requirements are documented under
"Integrate a recompiled frame loop" in
[API_REFERENCE.md](../runtime/docs/API_REFERENCE.md). Recognition does not
prove interrupt ownership, cycle-exact scheduling, or termination. It does
not skip a read, write a flag, suppress diagnostics, or interpret 65816 code.
MMIO polling and broader masked/indexed loops are intentionally deferred.

## Evidence and reproducibility

Local artifacts: `/private/tmp/snesrecomp-poll-wait.5D6j5s`.
`click-stall.csv` is a preserved copy of the user's `/tmp/mp-click-stall.csv`.
No ROM or generated game code is included in the repository.

```sh
ROOT=/private/tmp/snesrecomp-poll-wait.5D6j5s
"$ROOT/MarioPaint-before" /path/to/mp.sfc --frames 900 \
  --input "$ROOT/click-stall.csv"
"$ROOT/mario-build/MarioPaintHeadless" /path/to/mp.sfc --frames 265 \
  --input "$ROOT/click-stall.csv"
MP_POLL_TRACE=1 "$ROOT/mario-build/MarioPaintHeadless" /path/to/mp.sfc \
  --frames 900 --input "$ROOT/click-stall.csv"
```

The unchanged control reproduces the watchdog at completed frame 230:
`$01:D38E LDA $053A; BNE $D38E`. The original adapter only services the wait
at `$01:E2EB`, and therefore never runs the NMI which decrements this counter.
With the polling seam, sixteen real NMIs drain the counter and execution
returns to the ordinary wait at completed frame 246. No host memory patch
or new C activation is involved. The reported stale PC in the original
watchdog is also addressed in the isolated adapter: its diagnostic checkpoint
tracks the current basic block, without making scheduling decisions there.

The 265-frame candidate completes with zero hard diagnostics:

```text
frames=265 nmi=265 irq=0 context_checks=265 failures=0 watchdog=0
WRAM=2BF8B270 SRAM=011FFCA6 pixels=AA8E0D21
edges=96466:7247FC3D61495579 PCM nonzero=13318 peak=6508
A=0001 X=001C Y=000A S=1FF4 D=0000 DB=7F PB=01 P=30 M1X1 E0
```

Five serial 900-frame requests all stop at completed frame 266 with identical
CPU/WRAM/SRAM/pixel/PCM/semantic-edge output. That is repeatable failure,
not successful 900-frame coverage. An earlier run concurrent with large
builds hit the five-second watchdog inside native SPC upload at frame 264;
its watchdog-limited state is inconclusive, not a deterministic checkpoint.
The serial repeats, with and without poll logging, advance beyond it. Keep
that observation (`click265.log`) rather than silently dropping it, and run
future timing checks without competing builds.

## Separate next blocker: callee removes stack arguments

At `$01:8FB2`, an active caller invokes `$01:904A` with stack arguments. The
callee allocates eight local bytes, then its epilogue does:

```text
LDA $09,S; STA $0F,S       ; move the original two-byte return word
TSC; CLC; ADC #$000E; TCS  ; release locals plus six argument bytes
RTS                      ; $01:912F -> $01:8FB5
```

`$01:8FB5` is already a local continuation in the active caller, not an
undiscovered routine. Current generated RTS uses stack-depth equality for
the paired host-return fast path; the legitimate six-byte adjustment takes
its fallback into fresh registry dispatch. A further problem is the caller's
unconditional stack-neutrality restore, which would undo argument cleanup
even if the RTS were accepted as an ordinary host return.

The next compiler contract must establish which active call owns the actual
return PC and preserve the native post-return stack, including adjusted
returns, without breaking HLE contracts, non-local returns, interrupts, or
paired tail chains. Do not register the continuation as `func`, force a width,
or add a game-specific stack offset. The polling change does not implement
that broader return-ownership contract.

## Compiler/runtime contracts and cross-game gates

- Full uncached `go test -count=1 ./...`: pass, including native generated-C
  tests. All 35 strict-warning runtime C/C++ CTest targets pass.
- Emitter fixtures cover all four M/X variants, A/X/Y loads, D/DB-effective
  addresses, byte/word widths, and taken-edge-only hooks. Indexed/indirect,
  immediate, RMW, masked, state-changing, unrelated-condition, and forward
  edges are rejected by the initial recognizer.
- A redistributable generated-C fixture runs the original `LDA/BNE` loop
  with a compiled `DEC/RTI` ISR across all four M/X states. Zero/three counter
  values yield exactly zero/three callbacks and exactly one continuation.
  CPU/status/stack and live activation depth are checked.
- Runtime tests cover 10,000 resumptions, no nested checkpoint/poll callback,
  watchdog refresh without losing active frames, V3/V4/null-hook behavior,
  accepted WRAM mirrors, and rejection of MMIO/ROM/SRAM/wrapping/invalid reads.
- ActRaiser regenerated with its frozen full authored configuration and HLEs:
  4,647 variants, 83 C files; all five representative replays match WRAM,
  SRAM, CPU and semantic dispatch, with no hard diagnostics.
- Battletoads regenerated with its frozen authored configuration/database:
  913 variants, 48 C files; 1,800 idle and 3,600 stress output is byte-identical
  to control, with no hard diagnostics. Generated semantic hash is unchanged.
- Mario Paint: 1,803 variants, 64 C files; 300-frame idle record/replay matches
  the previous candidate exactly. The click recording's first 230 frames
  also matches control, including CPU/state/dispatch/PCM output. The later
  click transition intentionally differs because NMIs now run at the wait.

Three adjacent warmed A/B pairs, with no concurrent builds:

| Workload | Median wall-time change |
| --- | ---: |
| ActRaiser five-workload suite | -0.71% |
| Battletoads idle | -0.27% |
| Battletoads stress | +0.07% |
| Mario Paint recorded prefix, 230 frames | -1.04% |

These are no-material-regression results, not meaningful speedup claims.
Logs are `actraiser-ab.{log,json}`, `replay-bench.jsonl`, `go-all.log`,
`native-poll.log`, and `runtime-final-tests.log` in the artifact directory.
No post-click performance/fidelity claim is possible until the next blocker
is fixed. Renderer/backend code, authored configs/HLEs, and ROMs are untouched.

Generated semantic source SHA-256:

| Candidate | SHA-256 |
| --- | --- |
| Mario Paint | `24e43d3fd9ff027c29eb7c5105efa17b18b180368d3f295bf331f0c90c12f2a8` |
| ActRaiser | `66d232b2e61b6a00add8a495a97622f83447c8507f68c9518113cb2412ca0d88` |
| Battletoads | `18d1cfc1ced39390f49a7a7a370e5eec19a444b29fe98ca6f33bbd73a46192c1` |
