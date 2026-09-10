# Return-stack alias contracts — 2026-09-09

## Scope and isolation

This report-only step follows [direct-call return validation](RETURN_CALL_VALIDATION.md).
It adds a separate `return_stack_aliases` query for ordinary stores whose
entire address footprint can be proved disjoint from bank-zero stack memory
under the standard SNES system-bus contract. It tracks DB through actual
PHK/PHB/PLB bytes and inspected calls, plus constant or live-width-bounded
index ranges. It does not model an interpreter fallback or create production
facts, entries, function bodies, or inline-data skips.

Artifacts: `/private/tmp/snesrecomp-stack-alias.YjsRAm`. The before binary was
built from the dirty tree before this step; its reports also match the prior
`/private/tmp/snesrecomp-return-calls.KzoN0f` snapshots byte-for-byte. All
preceding uncommitted compiler work and unrelated `Testing/` files were
preserved. No game configuration, generated directory, runtime, renderer,
build input, HLE definition, or adjacent project was edited.

The same frozen three-game configs and verified ROM hashes from the preceding
milestones were used. `validate.js` hashes the ROM and recursively snapshots
the frozen config before/after analysis. It runs the before and after tools,
checks the after report at one/eight jobs, compares the production fact
databases byte-for-byte, and removes only the new `return_stack_aliases`
subtree before comparing every previous report field.

## Compiler contracts

Synthetic redistributable ROM tests cover:

- Complete byte footprints at both ends of unmirrored WRAM, bank carry,
  24-bit overflow, full 8/16-bit index domains, and known-index singletons.
- M-selected STA/STZ widths and X-selected STX/STY widths.
- Known and unknown DB, PHK, PEA/PLB establishment, PHB restoration, and
  actual callee DB changes propagating back to the caller.
- PHX/PLX restoring the effective index rather than using a stale value.
- Rejection of low-WRAM mirrors, direct/indirect operands, cartridge writes,
  WRAM-port writes, DMA activation, and unmodeled RMW effects.
- Disjoint stores do not hide real stack-relative frame corruption, imply
  values for subsequent WRAM reads, bypass HLE, or close another blocked path.
- Display limits do not suppress complete context counts, disjoint-context
  source-site inventories, or blocker-driven follow-up selection.
- One/eight-job determinism, no project writes, unchanged old query answers,
  and exclusion of positive alias contracts from production fact selection.

The original return-local/call tests also pass, including exact-variant
ownership, failed sibling boundaries, call-depth/state/program/root budgets,
wrong/adjusted/nonlocal returns, and recursive paths. The follow-up has an
independent loader budget, so loading more programs cannot starve an earlier
query or change its result.

## Three-game findings

| Game | Queried entry/M/X variants | Loaded existing programs | Queries with a disjoint write context | Distinct store PCs with such a context | Newly closed return contracts |
| --- | ---: | ---: | ---: | ---: | ---: |
| ActRaiser | 184 | 283 | 35 | 72 | 0 |
| Battletoads | 11 | 16 | 0 | 0 | 0 |
| Power Rangers | 27 | 62 | 1 | 5 | 0 |

These are context-specific static proofs, not runtime coverage or universal
store-site summaries. Counts use the complete `sites_with_disjoint_write_context`
inventory, not the truncated display: ActRaiser has 143 omitted footprint
details and its displayed subset contains only 65 of the 72 source PCs.
No root/program inventory budget is exhausted. State budgets still block
22 ActRaiser, five Battletoads, and one Power Rangers queries.

ActRaiser examples include the long word store at `$01:ACE4` into
`$7F:9752–$7F:9753` and indexed absolute stores at `$02:91EE`/`$02:91F4`
with DB established as `$7E`. These were already decoded instructions; the
new result is that these particular writes need not stop the return query.

Power Rangers' `$03:A387 M0X0` query now traverses five indexed store sites
at `$03:A3D0`, `$03:A3DA`, `$03:A3E0`, `$03:A3E7`, and `$03:A3EE`. It has
known DB `$7E` and concrete index contexts, but a different write at
`$03:A391` remains unproven and another path reaches the state budget.
Battletoads' selected queries have no qualifying disjoint write contexts.

All selected roots remain unproven overall. No authored entry, dispatch
declaration, HLE hook, or width override becomes removable in this step.
The remaining memory barriers require stronger DB/pointer/range or actual
stack-placement evidence, not a blanket assumption that low-memory writes
are harmless. Hardware writes and HLE still require separate contracts.

## Validation gate

- Full `go test ./...`, tooling `go test -race`, and `go vet ./...` pass.
- All three before/after production fact databases are byte-identical.
- All prior report fields, including unresolved, HLE, caller/bank provenance,
  and return-contract evidence, are unchanged.
- Final reports are byte-identical between one and eight jobs.
- ROM/config snapshots and `git diff --check` are clean for this step.

Logs are `go-tests-final.log`, `race-tests-final.log`, `vet-final.log`, and
the three `*-validation.log` files. `summary.json` uses untruncated write-site
and context inventories. No game regeneration, native/runtime build, replay,
or runtime benchmark was needed or run: this step does not affect production
code generation, runtime behavior, or build inputs. Those validation gates
remain required before any future promotion into production analysis.

Changes remain uncommitted.
