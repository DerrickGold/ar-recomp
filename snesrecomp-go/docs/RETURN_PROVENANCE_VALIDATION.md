# Return-address provenance — 2026-09-09

## Scope and isolation

This independently implemented, report-only abstract execution follows the
[return-frame audit](RETURN_FRAME_VALIDATION.md). It tracks which bytes a return
actually consumes rather than interpreting stack height or a nearby arithmetic
instruction as sufficient evidence. No upstream recognizer implementation,
interpreter fallback, or suspicious-byte reachability policy is imported.

Artifacts are under `/private/tmp/snesrecomp-return-provenance.FXsXAy`.
The immediate before binary includes the previous uncommitted work validated
under `/private/tmp/snesrecomp-return-audit.QtH2is`. The same frozen configs,
ROM identities, and native frontends are used. Live game configs, generated
directories, HLE declarations, renderer, runtime, and adjacent projects are
untouched. Concurrent unrelated changes are not included in the native builds.

## Contract tests

Redistributable synthetic ROMs exercise stack-relative return-word adjustment,
offsets below saved registers/status, PLA/ADC/PHA and PLX/INX/PHX constructions,
SBC/decrement and 16-bit wrapping, register transfers including hidden B,
PHP/PLP width/carry/decimal restoration, JSL bank preservation, byte-sized
round trips, and equal versus unequal adjustments on separate paths.

Rejection tests cover unknown or set decimal mode, unknown/clobbered carry,
wrong stack slots, partial overwrite, X truncation, wrong pull widths,
object-field/constant replacement values, bank overwrite, mixed return kinds,
unknown callees, possibly aliasing stores and block moves, unproven PLP values,
stack resets, emulation transitions, HLE entry/site/conditional/dispatch/upload
barriers, collapsed dispatches, external edges, unsupported effects, and both
state and stack budgets. Blocker truncation must retain incomplete status even
when another branch has an individually qualified return.

Tests verify no graph or legacy exit-summary mutation. End-to-end shadow tests
check worker-count independence, no project writes, and exclusion of a positive
constant-adjustment result from production dispatch-fact selection. A repeated
unchanged abstract state does not claim that a loop terminates.

All results remain conditional on the entry call-frame kind, native mode,
decoded entry M/X, a writable/nonaliasing stack window, and preservation by
interrupts/hardware. No addend is published as an inline-data byte count.

## Three-game findings

The focused inventory selects existing normal-return graphs containing stack
pulls, stack-relative operands, or address pushes. Counts are entry/M/X variants,
not all functions, missing configuration, executed failures, or reachable code.

| Game | Selected | Conditional PC preserved | Constant-adjusted | Unproven |
| --- | ---: | ---: | ---: | ---: |
| ActRaiser | 592 | 18 | 0 | 574 |
| Battletoads | 90 | 0 | 0 | 90 |
| Power Rangers | 136 | 2 | 0 | 134 |

No game produces a uniform nonzero return-PC addend in this bounded pass.
There are no newly removable cfg entries, proven inline arguments, or new
handler roots. Power Rangers' two preservation contracts are `$00:D3FC M0X0`
under a native JSL frame and `$01:D7EB M0X0` under a native JSR frame. They are
existing decoded routines, not newly found functions.

ActRaiser's `$00:8623` and `$00:8669` read incoming return bytes but stop at
object-field writes (`$00:8626` and `$00:866E`) whose non-aliasing is not proven.
The earlier frame audit continues to show the different entry/yield shapes.
This pass does not reinterpret those helpers as ordinary inline-argument
consumers. A mixed-frame `$03:F98A` interpretation also encounters return-word
arithmetic without a known binary/carry contract; coherent-looking arithmetic
alone is not promoted to evidence. Battletoads' `$14:A766` similarly reads
incoming bytes but has unresolved callee and memory-write contracts. None of
these findings establishes a runtime defect.

The dominant blockers are potential memory aliasing (271 ActRaiser, 81
Battletoads, 96 Power Rangers selected variants) and missing callee contracts
(210, 13, 28). Counts overlap where branches encounter different blockers.
HLE barriers remain explicit (60 ActRaiser and two Power Rangers variants).
State-budget blockers affect two ActRaiser, one Battletoads, and two Power
Rangers variants; they remain unproven, never a clamped successful contract.

## Preservation checks

One- and eight-worker reports match byte for byte. Removing only the new
`return_address_provenance` subtree reproduces every preceding report field,
including the return-frame audit, bank queries, HLE obligations, and unresolved
counts. All three proven-fact databases are unchanged.

All six isolated regenerations reproduce the preceding generated files byte
for byte:

| Game | Normal variants/files | Proven variants/files |
| --- | --- | --- |
| ActRaiser | 4,647 / 83 | 2,868 / 71 |
| Battletoads | 1,572 / 72 | 913 / 48 (+ synchronized `funcs.h`) |
| Power Rangers | 8,248 / 138 | 7,318 / 122 |

The complete Go suite, tooling race tests, vet, strict `-Werror` runtime build,
and all 33 runtime CTests pass.

Seven warmed alternating analysis pairs were repeated after the final explicit
stack-memory obligation was added. Final medians (`analysis-bench-final.jsonl`)
are ActRaiser 2,510.68 → 2,513.55 ms (+0.11%), Battletoads 242.28 → 245.65 ms
(+1.39%), and Power Rangers 723.35 → 719.81 ms (-0.49%). The initial pass is
also retained. These small timing differences include JSON output and host
noise, not added production work or an asserted speedup.

Both frozen native frontends build. The five ActRaiser workloads (world map,
Sky Palace, simulation actions, Aitos, and Death Heim) reproduce all four
WRAM/SRAM/CPU/captured-dispatch artifact hashes against the reference and prior
milestone, with no gated hard diagnostics. One warmup and three adjacent pairs
give +0.17% suite timing, with the largest workload median change +0.82%.

Battletoads' 1,800-frame gameplay and 3,600-frame stress workloads reproduce
the reference CPU/WRAM/framebuffer/SPC/PCM output hashes. Separate dispatch
runs reproduce the previous logs and captured JSONL byte for byte, with no
hard diagnostics. The initial three-pair gameplay timing was +2.12%; a seven-
pair repeat reduced that to +0.45%, with stress +0.72%. No material repeatable
runtime regression is established. Results are in `actraiser-replays.json`,
`battletoads-bench.jsonl`, `battletoads-bench-repeat.jsonl`, `battletoads-run/`,
and `replay-preservation.log`.

Captured milestone/census traces are not exhaustive semantic edge sequences;
byte-identical generated source is the stronger preservation check here.
Power Rangers coverage in this pass is analysis/regeneration only, not native
gameplay or audio.

### Validation gate going forward

The native rebuilds, replays, and runtime benchmarks above were redundant for
this report-only change once generated code, runtime, and build inputs were
confirmed unchanged. Future analysis-only steps should use compiler/contract
tests, determinism and no-write checks, and preservation of production facts
and outputs where the paths overlap. Measure analysis cost when useful. Reserve
full native replay and runtime-performance validation for changes that can
affect generated behavior, runtime code, or build settings; do not rerun it
merely because report metadata changed.

## Next proof boundary

Actual callee contracts and memory-alias/value provenance are the next blockers,
not a lack of recognizer patterns or a reason to scan arbitrary data as code.
Only after those close can incoming-return adjustments support call-site data
ownership, continuation checks, and safe decode skipping. Complete M/X exit
sets and recursive callable groups still need separate implementation and
behavior-affecting validation before production adoption. Authored overrides
and HLE definitions remain independent escape hatches.
