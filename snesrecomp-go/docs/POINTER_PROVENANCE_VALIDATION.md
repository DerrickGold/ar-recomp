# Caller-side pointer provenance validation — 2026-09-09

## Scope

This extends the uncommitted report-v18 HLE inventory with a bounded,
report-only def-use query. No decoder successor, root-discovery rule, generated
dispatch behavior, target-set proof, HLE definition, or authored configuration
is changed. It does not yet enumerate or automatically compile newly suggested
handlers. Existing v17 fact databases remain compatible.

The baseline executable was built from the preceding HLE-inventory milestone
before the pointer-provenance edits. Artifacts are under
`/private/tmp/snesrecomp-pointer-provenance.Ozg2VQ`. The previous milestone's
isolated configurations, generated baselines, native frontend snapshots, and
frozen binaries are under `/private/tmp/snesrecomp-hle-inventory.hdVJKp`.
Both games were regenerated to new temporary directories; the isolated native
builds were repointed to those directories. Power Rangers uses a fresh copy of
the adjacent project's configuration and new before/after generation folders.
Its source project is not a Git checkout and was used read-only.

ROM SHA-256 identities (no ROM is redistributed):

- ActRaiser: `b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0`
- Battletoads: `b0dbd4d7e5689c32234e80b0c5362ef67c425ab72d6ddb49d1cb1133ef630ef7`
- Mighty Morphin Power Rangers: `624a66607caef2ca34920ea15b84b28cdd1916ee089d496cec4f1d43621fdbb3`

## Findings

ActRaiser has no findings from this first, deliberately narrow one-hop query.
That does not prove that it lacks other caller-side pointer producers.

Battletoads has two findings:

- `$02:DA40 LDA $8EFB,Y` -> `$02:DA43 STA $3A` -> `$02:DA45 JSR $8000`.
  The index originates at `$02:DA3A LDA $063E,X`, not the object-loop counter.
  The initial path defines DB=$82, yielding ROM-base candidate `$82:8EFB`.
  The loop backedge crosses a call, so DB remains unknown on other paths.
  D=0, index domain/stride, table extent/ownership, and handler-entry semantics
  remain explicit obligations. The 149-word/143-distinct-target table is **not**
  claimed independently enumerated or complete by this change.
- `$00:AC21 LDA $A8C9,X` -> `$00:AC24 STA $3A` -> `$00:AC26 JSR $FDE9`.
  The index comes from `$1A`; DB has local constant evidence. The pointer alias
  and target-set obligations remain open.

Power Rangers has two findings, both behind existing HLE routes:

- `$05:DB74 LDA $0000,Y` -> `$05:DB7E STA $98` -> `$05:DB80 JSR $DB84`.
  Y originates from the stream pointer at `$02A2`.
- `$06:9B3C LDA $0000,Y` -> `$06:9B41 STA $98` -> `$06:9B43 JSR $9B48`.
  Y originates from the indexed stream pointer at `$1622,X`.

Neither mutable stream pointer becomes a fixed ROM table. The existing
tagged-stream classification and structural handler candidates are retained;
the new query supplies register/slot/caller provenance alongside them.

For all three games, every report field outside the dispatch inventory and
its summary is unchanged. Exported proven-fact databases are byte-identical,
including report-version metadata. HLE target sets remain unproven.

## Synthetic contracts

Redistributable fixtures cover A/X/Y transfers, indexed word loads, conditional
and proven nonzero-D aliases, PHK/PLB and PEA/PLB bank evidence, explicit long
banks, exclusion of WRAM as a ROM base, variable stream pointers, and loop
backedges that cross arbitrary callees. Negative cases reject arithmetic,
register clobbers, byte/truncated transfers, intervening writes/calls,
unsupported stack shuffles, unreachable stores, ambiguous producer joins, and
data overlapping any trampoline byte. A forward branch over noncode remains
recoverable through its actual decoded predecessor edge.

Inventory tests verify deterministic worker counts, abstract-path deduplication,
unchanged fact selection, and actionable text output. Table bounds are never
inferred from an unrelated iteration counter.

## Regeneration and runtime validation

All generated files are byte-identical before/after, separately for normal and
proven-analysis modes. The modes are not being claimed equal to each other.
All three games also produce byte-identical reports with one and eight workers.

| Game | Mode | Variants | Generated files | Raw unresolved emissions | Stub markers |
| --- | --- | ---: | ---: | ---: | ---: |
| ActRaiser | Normal | 4,647 | 83 | 68 | 206 |
| ActRaiser | Proven | 2,868 | 71 | 25 | 42 |
| Battletoads | Normal | 1,572 | 72 | 36 | 134 |
| Battletoads | Proven | 913 | 48 | 17 | 66 |
| Power Rangers | Normal | 8,248 | 138 | 47 | 435 |
| Power Rangers | Proven | 7,318 | 122 | 29 | 349 |

Power Rangers uses its current copied 41-entry configuration, not the older
configuration from earlier downstream reports. Equal before/after semantic
source hashes are:

- Normal: `96a124a0a9131cfe6423166bb2b6e0041fe90651ef5766dd1800ace2a9d69eb3`
- Proven: `abb9c30a00aef78f539aad68dc2c788523ed8ae09a25a2e95c3061047bab4b97`

ActRaiser and Battletoads retain the hashes recorded in
[the inventory validation](HLE_DISPATCH_INVENTORY_VALIDATION.md).
The full Go suite, tooling race tests, tooling vet, strict standalone runtime
build, and all 33 runtime CTests pass. The process-group Go test uses a read-only
host process probe outside the sandbox; caches remain in temporary directories.

Battletoads was rebuilt against the newly regenerated proven directory. Its
1,800-frame gameplay and 3,600-frame stress workloads reproduce the previous
WRAM/framebuffer/CPU/PCM results and byte-identical dispatch trace JSONL and
stdout/stderr, without hard runtime diagnostics. One warmup and three warmed
adjacent A/B pairs give median changes of -0.11% and +0.02%, respectively.

ActRaiser was rebuilt against the newly regenerated normal directory. All five
representative workloads pass with one warmup and three adjacent A/B pairs:
Mode 7/world map (6,000 frames), Sky Palace wide (1,200), simulation actions
(6,000), Aitos wide (4,000), and Death Heim wide (4,000). WRAM, SRAM, CPU-state,
and dispatch-log artifact hashes match; no gated hard diagnostic executes.
The suite timing change is -0.13%, with the largest workload regression +0.05%:
no material performance regression. These tiny timing differences are noise,
not an optimization claim.

Power Rangers validation here covers analysis and isolated regeneration only;
its native runtime was not rebuilt or replayed for this report-only milestone.
As in the preceding milestone, census trace equality is not an exhaustive
full-run semantic-edge sequence proof. Byte equality of all generated code is
the stronger behavior-preservation check for these reporting-only changes.

Seven warmed alternating analyzer A/B pairs per game, run after regeneration
and runtime benchmarks completed, show median overhead of +0.23% for ActRaiser
(2.343 -> 2.349 seconds), +0.35% for Battletoads (0.199 -> 0.200 seconds), and
+0.33% for Power Rangers (0.591 -> 0.593 seconds). No material analyzer
regression was measured. Timing samples are retained in
`analysis-bench-final.jsonl` in the validation directory.
