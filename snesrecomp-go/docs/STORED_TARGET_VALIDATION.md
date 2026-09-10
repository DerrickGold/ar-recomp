# Indexed-memory RTS target audit — 2026-09-09

## Scope and isolation

This extends the uncommitted shadow-v18 inventory with report-only
`stored_target_flows`: indexed word loads feeding pushed RTS targets,
same-expression writer candidates, and conditional literal/saved-JSR-return
target addresses. It does not change static proof selection, decoding roots,
entry M/X, generated control flow, authored configuration, or any HLE hook.
No game-specific offset, address, or record layout is encoded in the query.

The baseline executable includes the preceding caller-side pointer-provenance
milestone, before this query was added. All new artifacts are isolated under
`/private/tmp/snesrecomp-stored-target.eW9bXJ`. Baseline reports and generated
files are under `/private/tmp/snesrecomp-pointer-provenance.Ozg2VQ`; frozen
ActRaiser/Battletoads configs and frontend snapshots remain under
`/private/tmp/snesrecomp-hle-inventory.hdVJKp`. Power Rangers uses the same
copied 41-entry configuration as the preceding validation. No live generated
directory or authored cfg was modified. Unrelated concurrent frontend,
localization, and packaging changes were left untouched.

ROM identities and preceding generation/runtime results are recorded in
[the pointer-provenance validation](POINTER_PROVENANCE_VALIDATION.md).
No ROM contents are redistributed; compiler contracts use synthetic fixtures.

## ActRaiser findings

The inventory adds six indexed-memory-fed RTS sites, making 104 inventory
sites rather than 98. This is **not six additional unresolved trap emissions**;
the separate legacy unresolved count is unchanged.

| RTS source | Loaded expression | Net target adjustment | Distinct same-expression writer PCs |
| --- | --- | ---: | ---: |
| `$00:8668`, `$00:868F` | absolute `$001E,X` | +1 | 2 shared |
| `$00:8965` | absolute `$0012,X` | 0 | 45 |
| `$03:8711`, `$03:8759`, `$03:F989` | long indexed ROM expressions | +1 | 0 |

The `$0012,X` flow independently corroborates **175 distinct authored target
addresses** through the decoded writer/caller chains:

- 36 literal word-store sites supply 32 distinct target addresses.
- One helper-entry stack read, joined to direct JSR callers, supplies 144
  distinct continuation-address candidates.
- One address appears in both groups, giving 175 in the union.

All 175 addresses already occur in the authored entry list. This is address
overlap, not proof of authored entry kind, entry M/X, runtime reachability,
complete dispatch coverage, or safe cfg removal. In particular, saved return
addresses must not be promoted to ordinary routines with new C activations.
The query starts from the authored decoded closure, so this is corroboration,
not a roots-withheld rediscovery claim.

The 45 writer records also retain three unknown arithmetic values, three
memory-derived values, and two stack words without a supported helper-entry
origin. Those records are not discarded to manufacture a finite target set.
Multiple decoded owner contexts are merged deterministically; conflicting or
unknown provenance remains a separate alternative.

The three arithmetic stops are stores at `$00:86D7`, `$00:95B5`, and
`$00:A3F7`. Inspection shows record-pointer/record-base-plus-12 calculations.
They motivate generic carry/decimal-aware value analysis and bounded
record/table provenance, **not a hardcoded record offset**. `$00:9593` is now
reported as memory-derived through Y even though intervening accumulator
operations change A. Copying a loaded value into an unrelated register does
not make those accumulator operations clobber that saved value.

The helper at `$00:A673` is not recovered as an entry-origin stack read in the
current closure. Its stack value remains unclassified; the audit does not
invent an external call edge from nearby bytes.

## Other games and proof isolation

Battletoads and Power Rangers have no findings from this indexed-memory RTS
query. Their two caller-to-trampoline pointer-producer findings each remain
unchanged. Absence of a finding does not imply absence of other object or
script dispatch machinery.

All three games have byte-identical proven-fact databases, identical report
fields outside the dispatch inventory/summary, and byte-identical JSON reports
with one and eight decode workers. No additional funcs, HLE declarations,
target proofs, or configuration-removal certificates are produced.

## Synthetic contracts

Fixtures cover literal stores, helper-entry stack reads, two direct JSR
callers, return-word and RTS adjustments, 16-bit wrapping, word-sized A/X/Y
transfers and pushes, live M/X after a push, and unrelated A arithmetic while
a value is held in X/Y. Different program banks are not silently joined.

Negative cases retain/reject partial writes, read/modify/writes, unsupported
arithmetic, memory-derived values, arbitrary callees, byte/truncated pushes,
intervening stack pushes, non-entry stack reads, wrong stack offsets, entry
boundaries, and ambiguous predecessor joins. A branch over noncode is followed
using decoded edges rather than a lexical backward scan.

Ownership tests distinguish authored data, instruction interiors, unclaimed
ROM, and unmapped addresses without creating roots. Inventory tests check
address/context deduplication, deterministic worker counts, text diagnostics,
unchanged fact selection, and retention of an authored HLE obligation.

## Validation

The full Go suite, tooling race tests, tooling vet, strict standalone runtime
build, and all 33 runtime CTests pass. The Go process-group test uses a
read-only host process probe; caches remain in temporary directories.

Normal and proven regeneration are separately byte-identical to the preceding
milestone for all three games:

| Game | Normal variants/files | Proven variants/files |
| --- | --- | --- |
| ActRaiser | 4,647 / 83 | 2,868 / 71 |
| Battletoads | 1,572 / 72 | 913 / 48 |
| Power Rangers | 8,248 / 138 | 7,318 / 122 |

Battletoads' proven comparison additionally includes byte equality of the
separately synchronized `funcs.h` (49 files including that header). Proven
mode is not being claimed equal to normal mode; each is compared to its own
preceding baseline. The new candidate inventory has no consumer in generation.

Isolated ActRaiser and Battletoads native builds were repointed to the new
temporary generation directories and rebuilt. Battletoads' 1,800-frame gameplay
and 3,600-frame stress runs reproduce the previous WRAM, framebuffer, CPU,
SPC, and PCM results, with byte-identical logs and captured dispatch JSONL.
No hard runtime diagnostic executes. These census/milestone traces are not
an exhaustive full-run semantic-edge sequence proof; byte equality of generated
code is the stronger preservation check for this reporting-only change.
Power Rangers validation covers analysis and generation, not new runtime runs.

All five ActRaiser workloads pass with one warmup and three adjacent A/B pairs:
Mode 7/world map (6,000 frames), Sky Palace wide (1,200), simulation actions
(6,000), Aitos wide (4,000), and Death Heim wide (4,000). WRAM, SRAM, CPU-state,
and captured dispatch-log artifact hashes match both the frozen reference and
the preceding milestone, with no gated hard diagnostics. Suite median-based
timing changes by +0.10%; the largest workload change is +0.53%. No material
runtime regression was measured. Benchmarking was serialized after generation
and native builds completed to avoid contention from this validation's builds.

Battletoads' one warmup and three adjacent A/B pairs measure +0.01% for gameplay
and +0.03% for stress. These tiny differences are noise, not an optimization
claim. Timing and hash evidence are retained in `actraiser-replays.json`,
`battletoads-bench.jsonl`, and `battletoads-run/` in the validation directory.

The richer report does have an analysis-only cost. Seven warmed alternating
analyzer A/B pairs per game, after all other validation completed, measure:

| Game | Before median | After median | Change |
| --- | ---: | ---: | ---: |
| ActRaiser | 2.347 s | 2.389 s | +1.82% |
| Battletoads | 0.199 s | 0.210 s | +5.50% |
| Power Rangers | 0.593 s | 0.613 s | +3.25% |

The added decoded predecessor/writer queries explain the small absolute
overhead even on games with no final findings. Investigation removed global
writer/call/ownership aggregation entirely when there is no sink and excludes
unrelated address expressions before serializing writer identities. The
remaining roughly 11–43 ms is a reporting cost, not a runtime slowdown or
generated-output reduction. Samples are retained in
`analysis-bench-initial.jsonl`, `analysis-bench-filtered.jsonl`, and
`analysis-bench-final.jsonl`. Further caching across discovery passes is a
possible tooling optimization; no compiler fast path was replaced.
