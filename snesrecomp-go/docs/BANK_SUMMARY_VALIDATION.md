# Bank-preservation audit — 2026-09-09

## Scope and isolation

This extends [caller-source evidence](CALLER_SOURCE_VALIDATION.md) with
on-demand native DB/normal-return summaries and bank queries at source loads
and their decoded direct callers. It is report-only. No code roots, target
sets, generated dispatch behavior, authored configuration, or HLE policy
change; old table bank fields and samples are not rewritten.

Artifacts are under `/private/tmp/snesrecomp-bank-summary.6qm4MS`. The immediate
baseline is `/private/tmp/snesrecomp-caller-source.CiifHP`; the before tool was
built before these edits and includes the preceding uncommitted analysis
passes. ROM identities, frozen configs, and isolated native frontends remain
the same. Addresses below are validation evidence, not compiler policy.

## Power Rangers findings

Only 21 existing exact entry programs are needed by this demand query; the
256-program budget is not reached. No additional bytes are promoted to code.

For the `$01:8078` table read:

- The direct caller at `$01:8012`, within `$01:8000` M0X0, supplies DB=$01
  on the decoded path to that call.
- Inside `$01:8026`, `$01:8050 JSR ($8085,X)` remains a bank barrier because
  its X input domain is open. Checking its known heuristic table prefix would
  not establish that every possible handler preserves DB.
- The alternative `$01:803A JSL $02:8000` branch returns without reaching
  `$01:8078`; it is deliberately excluded from this read's blocker list.

For the `$02:8109` table read:

- The local path in `$02:80D4` depends on entry DB.
- Its direct caller `$02:8021` follows `$02:801E JSR ($8046,X)`. The local
  AND #$00FF gives 256 possible byte offsets into the pointer table, producing
  181 distinct word destinations in this coarse superset.
- Many destinations have no exact decoded entry variant; others lack valid
  return/bank summaries under the conservative stack model. Thus the call
  cannot currently carry the earlier DB=$02 definition to `$02:8021`.
- **These are not 181 newly discovered handlers or missing `func` directives.**
  The superset includes offsets/words not proven to execute, including odd
  offsets. Tighter index/field invariants are needed, not speculative roots.

Both original table bank fields remain unknown, with no new ROM samples.
The nine `$0C8A` writer candidates and all earlier initializer evidence remain
unchanged. This pass proves no new removable authored entries or missing valid
functions. ActRaiser and Battletoads have no bank-context queries in this
bounded caller-table inventory, and their previous reports are unchanged.

## Contracts and tests

The abstract byte stack recognizes balanced PHB/PLB and PHP/PLP, temporary
register pushes, explicit width changes, and exact normal call/return kinds.
Ordinary memory writes poison locally saved DB/status values because aliasing
is unproven; callee writes also poison a caller's saved values. Summaries can
establish DB preservation for a write-containing routine that never restores
DB from a potentially corrupted saved byte. A surrounding PHB/PLB can protect
against a bank-changing callee only when the modeled saved byte remains safe.

The summaries assume the decoded native width and normal-frame contract;
they do not prove interrupt behavior, external entry conditions, emulation
mode, runtime reachability, or arbitrary stack writes/returns. HLE entry/site
hooks and conditional HLE/SPC-upload hooks are barriers, never inherited ROM
contracts. M/X exit modes must already exist in the caller's decoded
successors. Least-fixed-point recursion remains unknown without an independent
base summary. The report retains explicit budgets and incomplete-target
diagnostics instead of silently assuming preservation.

Synthetic redistributable fixtures cover these rules, direct-call fixed-point
propagation, finite indirect target supersets, missing targets, return-kind
and width mismatches, recursive cycles, stack budgets/underflow, unsupported
interrupt/block-move effects, HLE at the queried PC, and numeric JSON bank
arrays. A query-specific regression excludes a missing-callee branch that
cannot reach the queried read while retaining that blocker in the whole-routine
summary. End-to-end source tests retain old domains, samples, database facts,
HLE obligations, and worker-count independence.

Full `go test ./...`, tooling race tests, `go vet ./...`, the strict standalone
runtime build, and all 33 runtime CTests pass. The previously observed unrelated
language-reference test failure is no longer present in this run; this work
did not modify those files.

## Preservation and timing

All three JSON reports are byte-identical between one and eight workers.
Removing only `bank_context` reproduces the immediate baseline reports
exactly. All three proven-fact databases are unchanged. All six isolated
regenerations reproduce the preceding generated files byte for byte:

| Game | Normal variants/files | Proven variants/files |
| --- | --- | --- |
| ActRaiser | 4,647 / 83 | 2,868 / 71 |
| Battletoads | 1,572 / 72 | 913 / 48 (+ synchronized `funcs.h`) |
| Power Rangers | 8,248 / 138 | 7,318 / 122 |

Seven warmed alternating analysis A/B pairs, including JSON output, give
these medians (`analysis-bench.jsonl`):

| Game | Before | After | Change |
| --- | ---: | ---: | ---: |
| ActRaiser | 2,476.88 ms | 2,457.59 ms | -0.78% |
| Battletoads | 228.69 ms | 230.16 ms | +0.64% |
| Power Rangers | 670.97 ms | 682.68 ms | +1.75% |

The small unrelated-game differences are timing noise-scale, not an
optimization claim. Power Rangers adds approximately 12 ms for its demand
queries; no material analysis regression is measured. No new work is added
to generated runtime hot paths.

Both frozen native frontends build against the newly regenerated directories.
All five ActRaiser workloads pass with one warmup and three adjacent A/B pairs:
Mode 7/world map (6,000 frames), Sky Palace wide (1,200), simulation actions
(6,000), Aitos wide (4,000), and Death Heim wide (4,000). WRAM, SRAM, CPU-state,
and captured dispatch-artifact hashes match both the reference and the
preceding milestone, with no gated hard diagnostic. Suite timing changes
+0.08%; the largest workload median adjacent increase is +0.97%
(`actraiser-replays.json`, `actraiser-replays.log`, `replay-preservation.log`).

Battletoads' 1,800-frame gameplay and 3,600-frame stress runs reproduce the
reference CPU/WRAM/framebuffer/SPC/PCM output hashes. One warmup and three
adjacent A/B pairs measure -0.94% and +0.27%, respectively
(`battletoads-bench.jsonl`). Separate dispatch-enabled runs reproduce the
preceding logs and captured JSONL byte for byte, with no hard diagnostics
(`battletoads-traces.log`, `battletoads-run/`). No material runtime regression
is measured. Captured census/milestone traces are not exhaustive full-run
semantic edge sequences; generated-source equality is the stronger
preservation proof here. Power Rangers validation in this pass is
analysis/generation only, not native gameplay or audio.

The next useful dependency is a tighter indirect-call index domain, followed
by stack/address non-alias proofs where handlers save and restore DB. A broad
table-word census cannot replace either proof, and none of these conditional
bank summaries is eligible for production adoption as implemented here.
