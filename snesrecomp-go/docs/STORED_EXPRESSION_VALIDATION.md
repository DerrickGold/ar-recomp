# Stored-handler arithmetic and field-source audit — 2026-09-09

## Scope

This follows [the indexed-memory RTS audit](STORED_TARGET_VALIDATION.md).
Unknown word writers now include a bounded symbolic `value_expression`, with
per-operation carry/decimal evidence and explicit origin/status barriers.
Flows additionally expose two levels of conditional source-field dependencies.
Both are report-only. No code generation, root selection, database proof,
legacy target candidate, authored cfg, or HLE definition changes.

Artifacts are under `/private/tmp/snesrecomp-stored-arithmetic.xfSTIl`. The
before executable was built before these edits; its baseline reports and
generated outputs are under `/private/tmp/snesrecomp-stored-target.eW9bXJ`.
This validation uses the same frozen configurations, ROM identities, and
isolated frontend builds documented by the preceding audit. No live generated
directory, adjacent game project, or authored configuration is modified.

## ActRaiser findings

The existing inventory remains at six memory-fed RTS sites, 47 distinct
matching writer PCs, and 175 candidate target addresses, all already authored.
There are **no newly proven targets or newly removable configuration entries**.
The gain is a more precise explanation of the remaining unknown values:

| Store PC | Destination expression | New symbolic evidence | Still unproven |
| --- | --- | --- | --- |
| `$00:86D7` | `$0012,X` | Load `$0032,X`, then ADC immediate `$000C`; carry locally zero | Decimal mode and memory identity |
| `$00:A3F7` | `$0012,X` | Load `$0032,X`, then ADC immediate `$000C`; carry locally zero | Decimal mode and memory identity |
| `$00:95B5` | `$0012,X` | Y after JSR at `$00:95A7`, then ADC immediate `$000C`; carry locally zero | Callee register/status summary and record value |
| `$00:95B9` | `$0032,X` | The same Y register after the same JSR | Callee register summary and record value |

The latter pair connects the saved-record writer to the offset handler writer
without claiming that the helper at `$00:95F0` either changes or preserves Y.
The source is labeled `register_after_call`, not a guessed callee result.

The source-field index connects `$0012,X` to `$0014,X`, `$0032,X`, and `$0000,Y`,
then to `$000A,Y` and `$0000,X` at the next level. These are address-expression
relationships only. In particular `$0000,Y` and `$0000,X` match many unrelated
writers; they are not established object allocations or ROM tables. The
audit deliberately does not substitute their constants into dispatch targets.

Different containing decode contexts can explain an unknown decimal flag by
different barriers (an entry/join, a call, or an interrupt instruction). Those
alternatives retain their contexts and reasons; they do not increase unique
writer-site counts or become gameplay-reachability claims. The analysis does
not infer binary mode from a coherent decode, reset convention, or M/X state.

## Other games and preservation checks

Battletoads and Power Rangers have no indexed-memory RTS findings in this
bounded query, so their inventory contents remain unchanged. Their existing
caller-to-trampoline findings and HLE coverage remain intact.

All three games have byte-identical proven-fact databases and identical report
fields outside the inventory. Inventory summaries and existing target-candidate
lists also match exactly. One-worker and eight-worker JSON reports are
byte-identical. The synthetic contracts verify that even a locally proven
arithmetic expression and a literal store to an identically spelled source
field cannot create a target or a database fact.

## Synthetic contracts

Redistributable fixtures cover:

- ADC with carry zero/one, SBC with/without borrow, mixed INC/DEC adjustments,
  multiple arithmetic operations with explicit carry resets, and 16-bit wrap.
- CLD/SED, CLC/SEC, REP/SEP status bits, and replacement of an earlier status
  definition. BIT preserves carry; comparisons, shifts, and earlier arithmetic
  invalidate it when no later definition exists.
- Unknown entry decimal/carry, decimal mode set, calls, and PLP restoration:
  no binary constant or exact addend is manufactured.
- Register snapshots across intervening stores, register-after-call origins,
  truncated transfers, unsupported arithmetic, and ambiguous decoded joins.
- Same-expression memory dependencies without value substitution, bounded
  cycles/depth, deterministic ordering, HLE retention, and text diagnostics.

Constants and exact addends in `value_expression` remain local audit evidence.
The legacy writer `kind`, `stored_value`, and `value_addend` are not changed,
and the new expression fields are not inputs to generation or the database.

## Validation

The full Go suite, tooling race tests and vet, strict standalone runtime build,
and all 33 C/C++ runtime CTests pass. The Go process-group test uses a read-only
host process probe; caches and outputs are isolated under temporary paths.

Normal and proven regeneration are separately byte-identical to their own
preceding baselines for all three games:

| Game | Normal variants/files | Proven variants/files |
| --- | --- | --- |
| ActRaiser | 4,647 / 83 | 2,868 / 71 |
| Battletoads | 1,572 / 72 | 913 / 48 |
| Power Rangers | 8,248 / 138 | 7,318 / 122 |

Battletoads' proven comparison additionally checks the separately synchronized
`funcs.h` (49 files including that header). The modes are not claimed equal to
each other. Power Rangers validation covers analysis and generation; its
native frontend is not rebuilt or replayed here.

The isolated ActRaiser/Battletoads native builds were repointed to the newly
regenerated directories and rebuilt. All five ActRaiser workloads pass with
one warmup and three adjacent A/B pairs: Mode 7/world map (6,000 frames), Sky
Palace wide (1,200), simulation actions (6,000), Aitos wide (4,000), and Death
Heim wide (4,000). WRAM, SRAM, CPU-state, and captured dispatch-log artifact
hashes match both the frozen reference and the preceding milestone. No gated
hard diagnostic executes. Suite timing changes by +0.08%, with the largest
workload change +0.26%: no material runtime regression.

Battletoads' 1,800-frame gameplay and 3,600-frame stress benchmarks reproduce
the reference CPU/WRAM/framebuffer/SPC/PCM logs. One warmup and three adjacent
A/B pairs measure -0.05% and +0.20%, respectively. These small differences
are noise, not an optimization claim.

Separate dispatch-enabled runs of both Battletoads workloads also reproduce
the preceding milestone's logs and captured JSONL byte for byte, with no hard
diagnostics. Evidence is in `battletoads-run/` and `battletoads-traces.log`.
As before, these census/milestone traces are not exhaustive full-run semantic
edge sequences; generated-source byte equality is the stronger preservation
check for this reporting-only change.

Seven warmed alternating analyzer A/B pairs per game, serialized after the
generation/build/replay work completed, measure the additional report cost:

| Game | Before median | After median | Change |
| --- | ---: | ---: | ---: |
| ActRaiser | 2.395 s | 2.403 s | +0.35% |
| Battletoads | 0.212 s | 0.213 s | +0.48% |
| Power Rangers | 0.616 s | 0.617 s | +0.17% |

This is incremental to the preceding stored-target audit, not to the earlier
compiler without that audit. The final samples are retained in
`analysis-bench-final.jsonl`; runtime comparisons are in
`actraiser-replays.json` and `battletoads-bench.jsonl`.
