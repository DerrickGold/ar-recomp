# Return-frame audit — 2026-09-09

## Scope and isolation

This adds a report-only stack-frame audit to the existing shadow decoder,
following the [bank-preservation milestone](BANK_SUMMARY_VALIDATION.md).
It is an independent implementation of a return-contract check, not a port
of upstream recognizers or their interpreter-dependent policies.

Artifacts are under `/private/tmp/snesrecomp-return-audit.QtH2is`. The immediate
baseline is `/private/tmp/snesrecomp-bank-summary.6qm4MS`; the before binary
includes all preceding uncommitted caller/bank analysis. The same frozen
configs, ROM identities, and native frontends are used. No live game config,
generated directory, HLE declaration, renderer, or runtime source is modified.

## What it establishes

Synthetic ROM fixtures expose several distinctions lost by an opcode-only
return collector: a PEA/PEI/PER plus RTS consumes a constructed frame; PHB plus
RTS mixes local and entry bytes; pulling and rewriting a return address can
leave stack height balanced while changing control flow. Temporary register
saves and correctly sized PHP/PLP brackets instead retain the entry-frame
position. No frame-position result is itself a proof of return-PC or M/X values.

Tests also cover multiple incoming depths at one return, RTS versus RTL frame
sizes, native stack-relative stores below saved registers, memory-alias and
callee obligations, HLE entry/site/conditional/upload barriers, collapsed
dispatches, RTI, unknown stack resets, and both depth/state budgets. The audit
does not mutate decoder graphs or legacy exit-mode results. End-to-end tests
check no-write behavior, worker-count independence, and exclusion from proven
dispatch-database selection.

The current implementation follows lexical post-call paths only under an
explicit normal-return/stack-effects obligation. It does not assume that this
obligation is true. In particular, an internal continuation need not have the
normal entry frame assumed by the query. Report findings are not executed
defects or additional missing configuration.

## Real-ROM interpretation

ActRaiser's `$00:8668` illustrates the distinction. From `$00:8657`, a PLA
extracts the incoming return address, stores it in an object field, and a later
PHA overwrites entry-frame slots before RTS. From `$00:8661`, the same RTS is
reached with a newly pushed word instead. `$00:868F` similarly has different
frame shapes from `$00:8669` and `$00:8683`. These match the already documented
object/yield machinery, not newly proven handlers or runtime bugs.

Battletoads reports several returns reached after pulling past the assumed
entry S, including `$01:A924` from multiple decoded entries. Their real entry
and parent-stack contracts must be established before changing any exit fact.
Power Rangers mostly exposes opaque/HLE-dependent paths rather than a new
concrete constructed-return shape in this bounded decoded inventory. The
existing bank/index blockers remain unchanged.

The inventory counts are source-PC counts, not runtime failures or missing
handlers. Categories overlap across entry/M/X contexts:

| Game | Raw contexts / unique PCs / PC-MX sites | Locally pushed | Mixed | Past entry | Entry slots written | Unknown position | RTI | HLE-affected |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| ActRaiser | 3,089 / 1,550 / 1,624 | 26 | 2 | 21 | 3 | 381 | 26 | 68 |
| Battletoads | 565 / 505 / 506 | 0 | 0 | 7 | 0 | 5 | 4 | 3 |
| Power Rangers | 2,162 / 2,020 / 2,046 | 0 | 0 | 0 | 0 | 176 | 2 | 6 |

Five ActRaiser entry variants reach an audit budget; their results remain
explicitly incomplete. Neither other game reaches a budget. Unknown positions
include collapsed dispatch constructs whose real push consumption is not in
the surviving graph. Resolving those requires construct contracts, not a larger
blind scan or assumed normal return.

## Preservation checks

All three reports are byte-identical between one and eight workers. Removing
only `return_frame_audit` exactly reproduces every preceding report field,
including bank-context findings, HLE obligations, and unresolved counts. All
three proven-fact databases are unchanged. The complete Go suite, tooling race
tests, vet, strict `-Werror` runtime build, and all 33 runtime CTests pass.

All six isolated regenerations reproduce the previous generated files byte
for byte:

| Game | Normal variants/files | Proven variants/files |
| --- | --- | --- |
| ActRaiser | 4,647 / 83 | 2,868 / 71 |
| Battletoads | 1,572 / 72 | 913 / 48 (+ synchronized `funcs.h`) |
| Power Rangers | 8,248 / 138 | 7,318 / 122 |

Seven warmed alternating analysis pairs, repeated after final aggregation
changes, produce these final medians (`analysis-bench-final.jsonl`):

| Game | Before | After | Change |
| --- | ---: | ---: | ---: |
| ActRaiser | 2,449.35 ms | 2,489.95 ms | +1.66% |
| Battletoads | 227.22 ms | 242.07 ms | +6.54% |
| Power Rangers | 681.86 ms | 713.93 ms | +4.70% |

These measurements include the new graph audit and expanded JSON output, not
production execution; no generated runtime operation is added. The overhead
is approximately 15–41 ms on these fixtures. The JSON inventory retains all
return contexts, increasing Battletoads' report from 395,747 to 764,465 bytes
and Power Rangers' from 1,519,169 to 2,953,498 bytes. The initial timing pass is
also retained (`analysis-bench.jsonl`).

Both frozen native frontends build against the new generated directories.
All five ActRaiser workloads pass with one warmup and three adjacent A/B pairs:
Mode 7/world map (6,000 frames), Sky Palace wide (1,200), simulation actions
(6,000), Aitos wide (4,000), and Death Heim wide (4,000). WRAM, SRAM, CPU-state,
and captured dispatch-artifact hashes match the reference and immediate prior
milestone. No gated hard diagnostic occurs. Suite timing changes -0.16%; the
largest median adjacent workload increase is +0.79%
(`actraiser-replays.json`, `actraiser-replays.log`, `replay-preservation.log`).

Battletoads' 1,800-frame gameplay and 3,600-frame stress runs reproduce the
reference CPU/WRAM/framebuffer/SPC/PCM output hashes. One warmup and three A/B
pairs measure -0.13% and -0.35%, respectively (`battletoads-bench.jsonl`).
Separate dispatch-enabled runs reproduce the preceding logs and captured JSONL
byte for byte, with no hard diagnostics (`battletoads-traces.log`,
`battletoads-run/`). These small timing differences are noise-scale, not speedup
claims. Captured census/milestone traces are not exhaustive semantic edge
sequences; byte-identical generated source is the stronger preservation check
for this report-only change. Power Rangers validation is analysis/regeneration
only in this pass, not native gameplay or audio.

## Next proof steps

Follow-up work should first qualify ordinary callable exits with proven entry,
callee, and stack contracts. Return-address value tracking can then support
report-only inline-argument detection; complete M/X exit sets and recursive
equations require separate tests and behavior-affecting validation before
production adoption. No interpreter fallback or suspicious-byte refutation
is introduced.

Subsequent strategy work should recognize bounded affine jumps into internal
instruction sequences without manufacturing standalone functions. Mapper-backed
address validity and ROM-to-WRAM execution views belong after the corresponding
mapping/copy-lifetime contracts. Existing bank/index provenance remains the
foundation; none of these recognizers may substitute plausible bytes for a
complete target or return proof, or consume an authored HLE definition.
