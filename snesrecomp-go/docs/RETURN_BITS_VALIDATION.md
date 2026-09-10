# Return-query index bit bounds — 2026-09-09

## Baseline and scope

Baseline: `504e6158` (`wip recomp improvements`), with the compiler tree clean
and only unrelated `Testing/` files untracked. Those files were left alone.
Artifacts are under `/private/tmp/snesrecomp-index-bits.s0uhF5`; before reports
also match the [preceding stack-alias snapshots](RETURN_ALIAS_VALIDATION.md)
under `/private/tmp/snesrecomp-stack-alias.YjsRAm` byte-for-byte.

This step strengthens the existing `return_stack_aliases` query, not another
generation mode or another layer of code discovery. It retains constant and
partial known-bit values through immediate logical operations and accumulator
shifts/rotations, then uses the full resulting index range in write-footprint
proofs. Actual tracked carry, hidden accumulator bytes, register transfers,
and saved byte values are respected.

No runtime, code generator, production decoder, authored cfg, HLE definition,
game-generated output, adjacent project, or build input was changed. The
earlier return-local/call reports deliberately retain their previous behavior.
There is no new dependency or production interpreter fallback.

## Synthetic contracts

The tests exhaust all 6,561 known/unknown-bit patterns for an 8-bit value,
checking every represented concrete byte against immediate AND/ORA/EOR and
ASL/LSR/ROL/ROR results, including both possible inputs for unknown carry.
Representative 16-bit domains exercise cross-byte propagation, high-bit
carry, and partial masks. Independent concrete arithmetic is only a test
oracle, not an execution path available to generated games.

ROM fixtures additionally verify:

- `LDX #0; TXA; ASL; TAX` retains the exact zero index.
- Mask/shift combinations bound unknown inputs; alignment alone does not
  justify a small magnitude or a safe destination.
- ORA can establish a lower bound and EOR preserves known/unknown bit positions.
- Bounds survive register transfers, PHX/PLX, PHA/PLA, and inspected calls.
- Narrowing A preserves B; narrowing X clears its high byte even if PLP
  later restores wide index mode. XBA exchanges actual partial byte values.
- Unknown carry leaves the rotated-in bit unknown. A known shifted-out bit
  supplies carry to the next operation rather than being assumed zero.
- A valid first destination byte does not justify a second byte that crosses
  into a WRAM mirror. All possible index values are covered, not sampled.
- Memory reloads and memory-operand logical operations do not invent bounds.
- Numeric processing cannot turn an incoming return-PC/status token into a
  constant, restore a lost frame identity, or justify pruning another branch.
- A positive synthetic return contract remains excluded from production fact
  selection; reports are worker-independent and no project files are written.

The full existing return and alias suites still pass, including HLE barriers,
stack corruption, exact callee matching, ownership boundaries, recursive paths,
and analysis/display budgets.

## Three-game comparison

The same frozen configs and verified ROM identities were used for ActRaiser,
Battletoads, and Mighty Morphin Power Rangers. Counts below use untruncated
source-site inventories, not the first 64 footprint records per query.

| Game | Query roots | Sites with a disjoint write context, before → after | Loaded existing programs, before → after | Newly closed full return contracts |
| --- | ---: | ---: | ---: | ---: |
| ActRaiser | 184 | 72 → 76 | 283 → 284 | 0 |
| Battletoads | 11 | 0 → 0 | 16 → 16 | 0 |
| Power Rangers | 27 | 5 → 5 | 62 → 62 | 0 |

No previously qualifying store site is lost. No root/program inventory budget
is exhausted. State-budget counts remain 22/5/1 respectively; the displayed
blocker categories per root also retain their previous counts. All selected
roots remain unproven overall, and no authored declaration becomes removable.

ActRaiser's new sites are `$03:BFB8`, `$03:BFC3`, `$03:C041`, and `$03:C046`.
The first two use an index derived from `LDX #0; TXA; ASL; TAX`, which the old
audit unnecessarily widened to the full 16-bit range. They now qualify in
the `$03:BF8C M1X0` query and its `$01:8646 M1X0` caller context. This lets
the audit enter the already-known `$03:C037` callee and inspect its two
ordinary stores with established DB `$7F`. None of these are new code roots.

Further paths still stop at writes with unresolved alias/effect contracts,
including memory INC/DEC in that callee. Qualifying an ordinary store does
not authorize a read-modify-write instruction without separately accounting
for its memory and flag effects. This is a concrete next bounded extension;
low-memory writes still need stronger stack-placement/pointer evidence.

## Preservation and validation

`validate.js` runs before/after analysis at eight jobs and after analysis at
one job. For all three games:

- The final one/eight-job JSON reports are byte-identical.
- Removing only `return_stack_aliases` reproduces every previous report field,
  including HLE, unresolved, caller/bank evidence, and local/call contracts.
- The machine-generated production fact databases are byte-identical.
- ROM hashes and recursive frozen-config snapshots are unchanged.

Full `go test ./...`, tooling `go test -race`, `go vet ./...`, and
`git diff --check` pass. Logs are `go-tests.log`, `race-tests.log`, `vet.log`,
and the three `*-validation.json` files. Changes are report-only: no game
regeneration/build, runtime replay, or runtime benchmark was required or run.
Native validation remains required before using these contracts in production.

Changes remain uncommitted on top of `504e6158`.
