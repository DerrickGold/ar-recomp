# Direct-call return contracts — 2026-09-09

## Scope and isolation

This report-only milestone follows the [return-address provenance pass](RETURN_PROVENANCE_VALIDATION.md).
It replaces the local query's unconditional stop at JSR/JSL with bounded,
context-specific abstract execution of already-decoded exact variants. There
is no runtime interpreter, general callee-summary assumption, production
dispatch change, or new code discovery.

Artifacts are under `/private/tmp/snesrecomp-return-calls.KzoN0f`; the immediate
baseline is `/private/tmp/snesrecomp-return-provenance.FXsXAy`. The before binary
includes all earlier uncommitted work. The same frozen ActRaiser, Battletoads,
and Power Rangers configs and verified ROM identities are used. No game config,
generated directory, runtime, renderer, HLE declaration, or adjacent project
was changed. Existing unrelated working-tree files were preserved.

## Compiler contracts

Synthetic ROM fixtures verify native JSR/JSL pushes, nested calls, paired
RTS/RTL return frames, program-bank wrapping, registers holding a caller's
return address across calls, saved status and binary/carry propagation, and
all available exact callee exit M/X continuations. They cover these distinctions:

- A call itself overwrites guest stack bytes, even when its callee is empty.
- A callee may return normally while corrupting its caller's return word or P.
- PHP/SEP/PLP can restore width while leaving X/Y high bytes truncated.
- A constructed, adjusted, wrong-kind, wrong-bank, or non-local return cannot
  resume the ordinary caller continuation.
- A callee hard-coding one return address may qualify for that particular call
  and fail for another; no universal preservation fact is cached.
- Missing exact variants are not replaced by another M/X state or a fresh
  decode merely because the ROM contains plausible leaf bytes.
- Failed sibling entries retain their existing jump boundary; fallthrough
  follows the original decoder's separate policy.
- HLE hooks, unsupported effects, memory-alias obligations, and incomplete
  callee branches remain blockers.
- Root/program/state/depth budgets are explicit and deterministic. Recursive
  base cases do not prove the recursive paths. Display limits cannot turn a
  partial query into a successful one or suppress call-root selection.

End-to-end tests check no project writes, no graph mutation, worker-count
independence, and exclusion of positive call-contract results from production
dispatch-database selection. All old local return-audit/value tests still pass.

## Three-game findings

The selected roots are local return-value queries that encountered a call
barrier. They are not all functions or runtime-reachable code. Program counts
include existing callees loaded on demand, not newly discovered routines.

| Game | Queried entry/M/X variants | Loaded exact programs | Newly closed conditional return contracts | Still unproven |
| --- | ---: | ---: | ---: | ---: |
| ActRaiser | 210 | 306 | 1 | 209 |
| Battletoads | 13 | 18 | 0 | 13 |
| Power Rangers | 28 | 64 | 0 | 28 |

No root/program inventory budget is exhausted on these snapshots. Per-query
state limits still affect 15 ActRaiser and five Battletoads roots; those remain
unproven. No inline-argument count, new handler target, or removable authored
entry is established by this pass.

ActRaiser's additional contract is `$02:BECA M1X0`: PHA saves A, JSR at
`$02:BECB` calls `$02:BEB8`, the matched return at `$02:BEC9` resumes `$02:BECE`,
and PLA restores A before the local decrement/loop and RTS at `$02:BED2`.
Every modeled exit preserves the original entry return PC under the stated
native/stack assumptions. This is a return property, not a proof that the
loop terminates. Both routines were already decoded; it is not newly recovered
game code or evidence that another width interpretation is impossible.

The main remaining blockers are memory writes whose non-aliasing with the
guest stack is unproven: 184 ActRaiser, 11 Battletoads, and 27 Power Rangers
root queries. Counts overlap across branches and other blockers. ActRaiser
also retains 28 HLE-affected roots. Five Battletoads queries require an exact
callee variant absent from the current shadow closure; that is not a claim
that production lacks a body or that the edge executes. Power Rangers retains
two roots with collapsed-dispatch barriers. Existing object-yield helpers are
not reclassified as ordinary inline-argument consumers.

## Validation and preservation

- Full `go test ./...`, tooling `go test -race`, and `go vet ./...` pass.
- All three final shadow reports are byte-identical between one and eight jobs.
- Removing only `return_call_contracts` reproduces every preceding report
  field, including local return provenance, bank/caller evidence, HLE data,
  and unresolved counts.
- All three machine-generated production fact databases are byte-identical
  to the immediate baseline.
- `git diff --check` passes.

Logs are `go-tests-final.log`, `race-tests-final.log`, `vet-final.log`, and
the three `*-validation.log` files. Each game has `*-after.json`,
`*-jobs1.json`, and `*-after-db.json` evidence.

Following the agreed validation gate, this pass did **not** regenerate or
rebuild games, run runtime replays/conformance tests, or run runtime benchmarks.
It changes only tooling/report analysis and leaves production inputs/facts
unchanged. Native validation remains required before a future step feeds any
of these contracts into generation or runtime behavior.

## Next boundary

Memory-address provenance must establish which writes can actually alias the
guest frame, and which can safely be excluded. That needs real D/DB/index/stack
and mapping evidence, not a game-specific assumption about object fields or
stack placement. More scalable loop and recursive summaries need independent
proofs as well. Context-specific call results must not be promoted to universal
exit summaries or inline-data ownership merely to remove these blockers.
