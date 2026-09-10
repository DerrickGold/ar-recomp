# Caller-source audit — 2026-09-09

## Scope and isolation

This follows [call-input evidence](CALL_INPUT_VALIDATION.md) one additional
bounded source hop: an indexed memory load's own index expression and bank
evidence, or a scalar load's candidate writers. The argument expression, its
source table's index, and the callee's initializer index remain separate.
The pass is report-only and does not modify authored configuration, HLE
definitions, code roots, proven target sets, or production generation.

Artifacts are under `/private/tmp/snesrecomp-caller-source.CiifHP`. The before
tool includes the preceding uncommitted call-input pass; its reports and
regenerated baseline are in `/private/tmp/snesrecomp-call-input.YUzx6u`.
ROM identities, frozen config copies, and isolated native frontends are
unchanged from that milestone. Addresses here describe validation inputs,
not shared compiler policies.

## Power Rangers findings

Both byte-masked arguments to `$06:9B00` originate in word reads, but their
source table indices differ:

| Caller | Source table expression | Source index | Local index superset |
| --- | --- | --- | --- |
| `$01:8080` | `$81B3,Y` at `$01:8078` | `$0E00,X` shifted right once | 0–32,767; unbounded table extent |
| `$02:8111` | `$8116,Y` at `$02:8109` | `($0E00,X AND $00FF)` shifted right once | 0–127 |

The AND #$00FF **after** each table read still describes the argument's
256-value superset. It must not be mistaken for either table's input bound.
Both reads retain unknown DB on the decoded predecessor paths, so neither
produces ROM samples. Using the current program bank would be an unsupported
assumption, not a recovered bank definition.

A read-only follow-up confirms why merely searching farther backward is
insufficient: `$02:8000` begins `PHB; PHK; PLB`, but `$02:801E` performs
`JSR ($8046,X)` before `$02:8021 JSR $80D4`, the entry containing the second
caller. The earlier DB=$02 setup cannot be propagated through that indirect
call without a preservation summary for its possible handlers. This is a
concrete interprocedural bank-proof gap, not evidence that DB is wrong at
runtime or that PB should be substituted for it.

The `$0E00,X` index-source expression has two exact-spelling writer candidates
in the decoded closure: a word zero store and a literal `$8024` store. These
are not a complete object-field census: other address spellings, index
registers/values, banks, lifetimes, and indirect updates may refer to the same
or different storage. Neither candidate is substituted as a reachable index.

The third caller, `$04:B1FC`, reads scalar `$0C8A`. Its nine exact-spelling
writer candidates include:

- three word copies from `$1124,X`;
- one word copy from `$9EA1,Y`;
- conditional ADC/SBC updates by five, with decimal-state obligations;
- one unresolved-origin store;
- literal/zero stores of three and zero.

The literals do not reduce that input to `{0,3}`. The copy, arithmetic, and
unknown writers remain visible, and no complete runtime domain is asserted.
This is a useful dependency shortlist, not a newly proven handler set.

ActRaiser and Battletoads have no new caller-source records in the current
bounded initializer query. Their existing reports are unchanged. No new
removable configuration entries or previously missing valid functions are
proven in this pass.

## Synthetic contracts

Redistributable fixtures keep source-table domains separate from argument
masks and callee indices. They test word sampling with known DB, no guessing
DB=PB, unknown/callee/pull/byte barriers, indirect-pointer expansion limits,
exact address-expression matching, context merging/deduplication, and owning
sample slices separately from shared decoded records. An end-to-end fixture
combines a bounded table caller with a scalar caller having both literal and
nonliteral writers; no samples or writer values are substituted into prior
domains, dispatch targets, roots, databases, or HLE decisions. Reports are
independent of worker count.

## Preservation checks

All three reports are byte-identical between one and eight workers. Removing
only `source_evidence` reproduces the preceding reports exactly, including all
old local-slot/caller records, samples, RTS flows, summaries, and HLE
obligations. All three proven-fact databases remain byte-identical.

All six isolated regenerations reproduce the preceding generated files byte
for byte:

| Game | Normal variants/files | Proven variants/files |
| --- | --- | --- |
| ActRaiser | 4,647 / 83 | 2,868 / 71 |
| Battletoads | 1,572 / 72 | 913 / 48 (+ synchronized `funcs.h`) |
| Power Rangers | 8,248 / 138 | 7,318 / 122 |

Tooling unit/race tests, vet, the strict standalone runtime build, and all 33
runtime CTests pass. At the time, an initial test of the enclosing game checkout
failed only an unrelated game-localization reference check because concurrent
language-contract edits did not match the shipped reference. Those unrelated
files were not modified by this work. Full `go test ./...` passes in
`/private/tmp/snesrecomp-caller-source-tests.jpdmJe`, a `d58ff739` snapshot
overlaid with only our seven analysis implementation/test files. That snapshot
includes both the preceding uncommitted call-input pass and this source hop.

Seven warmed alternating analysis A/B pairs, including JSON output, show no
material analysis regression (`analysis-bench.jsonl`):

| Game | Before | After | Change |
| --- | ---: | ---: | ---: |
| ActRaiser | 2,447.98 ms | 2,445.46 ms | -0.10% |
| Battletoads | 226.71 ms | 227.02 ms | +0.14% |
| Power Rangers | 664.67 ms | 666.19 ms | +0.23% |

These small differences are timing noise-scale, not an optimization claim.
The source expansion adds no work to generated runtime hot paths.

Both frozen native frontends build against the newly regenerated directories.
Power Rangers validation here is analysis/generation only, not native gameplay
or audio. All five ActRaiser workloads pass with one warmup and three adjacent
A/B pairs: Mode 7/world map (6,000 frames), Sky Palace wide (1,200), simulation
actions (6,000), Aitos wide (4,000), and Death Heim wide (4,000). WRAM, SRAM,
CPU-state, and captured dispatch-artifact hashes match the reference and the
immediately preceding milestone, with no gated hard diagnostic. Suite timing
changes -0.03%; the largest workload median adjacent increase is +0.61%
(`actraiser-replays.json` / `actraiser-replays.log`).

Battletoads' 1,800-frame gameplay and 3,600-frame stress runs reproduce reference
CPU/WRAM/framebuffer/SPC/PCM output hashes. One warmup and three adjacent A/B
pairs measure +0.04% and +0.34%, respectively (`battletoads-bench.jsonl`).
Separate dispatch-enabled runs reproduce the preceding logs and captured
JSONL byte for byte, with no hard diagnostics (`battletoads-traces.log` and
`battletoads-run/`). No material runtime regression is measured. Captured
census/milestone traces are not exhaustive full-run semantic edge sequences;
generated-source equality is the stronger preservation proof here.

The next unresolved dependencies are caller bank/calling contracts, actual
object-field writer aliases and domains, and the scalar copy/arithmetic
sources. These require proof across the relevant paths; more matching
addresses alone cannot establish completeness.
