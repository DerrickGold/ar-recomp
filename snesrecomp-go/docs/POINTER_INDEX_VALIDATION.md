# Pointer-index and script-source audit — 2026-09-09

## Scope

This extends [caller-to-trampoline provenance](POINTER_PROVENANCE_VALIDATION.md)
with a report-only writer index and conditional ROM-word sampling. It follows
the [stored-expression audit](STORED_EXPRESSION_VALIDATION.md), whose reports
and generated outputs serve as the immediate baseline. No game-specific
addresses, field offsets, or handler layouts are built into the implementation.

Artifacts are under `/private/tmp/snesrecomp-pointer-index.DoUHKt`. The before
executable was captured before these edits. All runs use the same frozen
configurations and ROM identities as the preceding audit: ActRaiser and
Battletoads configurations from `snesrecomp-hle-inventory.hdVJKp`, and Power
Rangers from `snesrecomp-pointer-provenance.Ozg2VQ/mmpr-cfg`. Adjacent project
files, authored configurations, live generated directories, and HLE policy
are not modified. Concurrent rendering/packaging work is outside this change.

## New cross-game evidence

| Game / trampoline | Index-source expression | Distinct writer PCs | Literal index values | Result |
| --- | --- | ---: | ---: | --- |
| Battletoads `$02:8000` | `$063E,X` | 104 | 28 | Zero skipped; 27 word samples select 25 distinct authored addresses |
| Battletoads `$00:FDE9` | `$1A` | 5 | 2 | Two conditional samples; neither establishes a new valid handler |
| Power Rangers `$05:DB84` | `$02A2` | 17 | 0 | Cross-bank table initializer and stream advances identified |
| Power Rangers `$06:9B48` | `$1622,X` | 20 | 0 | Indirect table initializer, stream reads, and object-field source identified |

### Battletoads

The first chain reads `$063E,X`, skips zero with BEQ, transfers the value to Y,
reads `$8EFB,Y`, and writes the trampoline's `$3A` slot before JSR. The report
finds 59 zero stores, 40 literal stores, and five unknown writers to the same
field expression across the decoded program banks. Among their 28 distinct
literal values, zero fails the decoded local guard. The remaining 27 select
25 distinct conditional target addresses, **all already authored**.

This is a writer-derived subset, not recovery of the whole manually inspected
149-word/143-target table. The nearby `CPX #$0050` bounds object iteration, not
the Y value read from the object, and is not used to infer a table extent.
The initial DB=$82 definition remains conditional because loop paths cross a
callee. No target set is closed and no configuration entries become removable.

At `$00:FDE9`, identically spelled `$1A` writers supply hypothetical indices
zero and one for `$A8C9,X`. They sample `$CA75` and `$07CA`, respectively.
Neither target is authored; only `$00:CA75` is ROM-mapped by the current
reader. **These are not newly discovered valid functions.** The odd index and
unmapped sample illustrate why an expression match is not an alias, stride,
lifetime, or reaching-definition proof. The query does not decode/promote either
address or assert that either executes.

### Mighty Morphin Power Rangers

The bank-five script-pointer writer census exposes a cross-bank initializer:
`$00:D3A8 STA $02A2` gets its value from `$00:D3A5 LDA $A3A2,X`. The other
matching writers include stream advances with unresolved inputs and RMWs.
There are no literal pointer values and no local DB constant at the consumer,
so the report does not invent stream locations or handler targets.

For bank six, `$06:9B35 STA $1622,X` receives a word from `$06:9B33 LDA ($98),Y`.
Two more memory sources are `$06:9B6D LDA $0002,Y` and
`$06:9B7D LDA $1620,X`. The 20-site inventory comprises three memory-derived
writers, 15 unknown writers, and two RMWs. Unsupported stack/register paths
remain unresolved rather than guessing which register holds the stream.

Both consumers have decoded sign-tag guards; the bank-five path also excludes
`$FFFF`. Those guards are now recorded for future candidate sampling. The
useful next static query is the initializer's table/index provenance, with
independent bounds and bank evidence—not scanning arbitrary stream words and
declaring their high-bit values to be code.

### ActRaiser

This query adds no index-writer or sample findings to ActRaiser's existing
inventory. The six memory-fed RTS sites, 47 matching writer PCs, 175 conditional
target addresses (all authored), and their expression/source dependencies are
unchanged. All authored HLE obligations remain intact.

## Synthetic contracts

Redistributable fixtures cover:

- Scalar and indexed word origins, immediate indices, register transfers, and
  rejection of byte/truncated values.
- BEQ zero skipping, BPL tagged-data rejection, CMP/BEQ `$FFFF` termination,
  immediate comparison bounds, flag clobbers, and unknown branch directions.
- BIT's immediate versus memory flag behavior, known/unknown carry/overflow,
  and an unrelated loop-counter comparison that must not bound the index.
- Cross-program-bank writer discovery while preserving source PCs, partial
  stores, RMWs, memory-derived values, and explicit alias obligations.
- Mapping `base + index` for `$0000,Y`, unknown DB paths, WRAM/unmapped reads,
  bank-boundary exclusions, deterministic sampling order, and the 512-sample cap.
- HLE retention, address-only authored overlap, unchanged proven-fact databases,
  unchanged unproven target classification, worker-count independence, and
  actionable verbose output.

All sampling is conditional report evidence. It does not seed decoding,
register handlers, choose M/X, change HLE, or become a production database fact.

## Preservation and validation

All three games produce byte-identical one-worker/eight-worker JSON reports.
Their proven-fact databases, every report field outside `pointer_producers`,
and all existing indexed-RTS flows/candidates are unchanged from the preceding
audit. Normal and proven generation each reproduce their own baseline byte
for byte, including separately synchronized Battletoads `funcs.h`:

| Game | Normal variants/files | Proven variants/files |
| --- | --- | --- |
| ActRaiser | 4,647 / 83 | 2,868 / 71 |
| Battletoads | 1,572 / 72 | 913 / 48 (+ `funcs.h`) |
| Power Rangers | 8,248 / 138 | 7,318 / 122 |

Full `go test ./...`, tooling race tests/vet, a strict `-Werror` standalone
runtime build, and all 33 runtime C/C++ CTests pass. The existing Go
process-group test uses read-only host process inspection. Power Rangers
validation here covers analysis and generation, not native gameplay or audio.

Seven warmed alternating analysis A/B pairs, run after the builds completed,
measure the following medians against this pass's before executable. JSON
serialization/output is included. These are small analysis costs, not runtime
costs or optimization claims:

| Game | Before | After | Change |
| --- | ---: | ---: | ---: |
| ActRaiser | 2,400.64 ms | 2,404.01 ms | +0.14% |
| Battletoads | 212.09 ms | 216.00 ms | +1.84% |
| Power Rangers | 620.09 ms | 632.25 ms | +1.96% |

The isolated Battletoads native build was repointed to the newly generated
proven directory. Separate 1,800-frame gameplay and 3,600-frame stress runs
reproduce the preceding CPU/WRAM/framebuffer/SPC/PCM logs and dispatch-enabled
JSONL byte for byte, without hard diagnostics. As in earlier audits, these
census/milestone traces are not exhaustive full-run semantic edge sequences;
generated-source equality is the stronger behavior-preservation evidence for
this report-only change.

The isolated ActRaiser build was likewise repointed to its newly generated
normal directory. All five workloads pass with one warmup and three adjacent
A/B pairs: Mode 7/world map (6,000 frames), Sky Palace wide (1,200), simulation
actions (6,000), Aitos wide (4,000), and Death Heim wide (4,000). WRAM, SRAM,
CPU-state, and captured dispatch-artifact hashes match both the frozen
reference and the preceding milestone; no gated hard diagnostic executes.
Suite timing changes by -0.15%, with the largest workload increase +0.28%:
no material runtime regression. Artifacts are `actraiser-replays.json` and
`actraiser-replays.log`.

Battletoads' warmed three-pair benchmarks also reproduce the frozen reference
output hashes. Gameplay changes by +0.14% and stress by -0.02%, with no material
runtime regression (`battletoads-bench.jsonl`). All generated-code comparisons
and tests are isolated; no live game regeneration or authored/HLE edits are
needed to obtain these findings.
