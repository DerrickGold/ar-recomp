# Local scratch-slot and caller-input audit — 2026-09-09

## Scope and isolation

This extends [initializer evidence](TABLE_INITIALIZER_VALIDATION.md) with local
scalar-slot candidate writers and direct-call A/X/Y input expressions. It is
report-only: no value substitution, new handler proofs, generation changes,
authored config changes, or HLE changes. Validation addresses below are inputs,
not shared compiler policies.

Artifacts are under `/private/tmp/snesrecomp-call-input.YUzx6u`; the immediate
baseline is `/private/tmp/snesrecomp-initializer.PnIxEN`. The before tool was
built from committed compiler state `7e803200`. The three ROM identities and
frozen config/frontend copies are unchanged from the preceding milestone.
Native validation excludes unrelated live game/localization work. No live
generated directory or adjacent project's authored files were modified.

## Power Rangers findings

The existing `$00:D3A5 LDA $A3A2,X` initializer now links its scalar `$0282`
index load at `$00:D3A0` to the nearest same-spelling writer `$00:D30A`.
That writer receives `$0284` loaded at `$00:D307`. The report retains 19
possible intervening alias writes rather than claiming a reaching definition.
The source range remains unknown; no table words are newly sampled.

The nested `$06:9B28 LDA $A828,Y` pointer-table load now links its `$A0`
index source at `$06:9B24` to `$06:9B03 STA $A0`. Tracing A through the
entry's `PHB; PHK; PLB` reaches **A at `$06:9B00` M0X0**. The local path
contains `$06:9B21 STZ $16C2,X`, recorded as a possible alias write because
its effective address/allocation is not proven disjoint from the scalar slot.

Three distinct decoded direct callers supply that entry register:

| Call PC | A source | Local input superset |
| --- | --- | --- |
| `$01:8080` | `$01:8078 LDA $81B3,Y; AND #$00FF` | 256 byte-valued words |
| `$02:8111` | `$02:8109 LDA $8116,Y; AND #$00FF` | 256 byte-valued words |
| `$04:B1FC` | `$04:B1F9 LDA $0C8A` | 65,536 words; range unknown |

All three are JSL to the exact `$06:9B00` target with M0X0 at the call.
The first two skip zero through BEQ, but this argument query deliberately does
not apply branch predicates, so its conservative superset includes zero.
The third caller prevents treating the entry's A as globally byte-bounded,
even before considering indirect callers, external entries, and HLE behavior.

The previous 273-record global `$A0` writer inventory and its hypothetical
ROM samples remain unchanged. The new local/caller chain is a more relevant
shortlist, not a replacement for that inventory or a sound narrowed domain.
It establishes no new missing function or removable authored entry.

ActRaiser and Battletoads have no new local-slot records in this bounded
initializer query; their entire reports remain unchanged. Their previous RTS
writer relationships and conditional handler-address overlap remain intact.

## Synthetic contracts and preservation

Redistributable fixtures cover passive stack/bank operations preserving A/X/Y;
pull, callee, status, truncation, join/backedge, arithmetic, and budget barriers;
nearest scalar word writers; partial/RMW/zero overwrites; possible alias writes
in execution order; indexed-slot exclusion; near/far call bank semantics;
indirect/collapsed-call exclusion; exact duplicate removal; and separate
M/X-matched and mismatched caller records.

An end-to-end fixture reproduces two masked callers plus one unbounded caller.
It checks that no caller value is substituted into the scalar slot, table
index, ROM sample set, dispatch target set, roots, or proven database; authored
HLE routing and obligations survive. One- and eight-worker reports agree.

Full `go test ./...`, tooling race tests, `go vet ./...`, strict standalone
runtime build, and all 33 runtime CTests pass. For all three games, one-worker
and eight-worker JSON reports are byte-identical. Removing only the additive
`local_slot_source` fields reproduces the preceding reports exactly. The
three proven-fact databases are byte-identical to the preceding milestone.

All six isolated regenerations reproduce the previous files byte for byte:

| Game | Normal variants/files | Proven variants/files |
| --- | --- | --- |
| ActRaiser | 4,647 / 83 | 2,868 / 71 |
| Battletoads | 1,572 / 72 | 913 / 48 (+ synchronized `funcs.h`) |
| Power Rangers | 8,248 / 138 | 7,318 / 122 |

The frozen ActRaiser and Battletoads frontends build successfully against the
new generated directories. Power Rangers validation here covers analysis and
generation only, not native gameplay/audio.

Seven warmed alternating analysis A/B pairs, including JSON output, measured
the following medians (`analysis-bench.jsonl`):

| Game | Before | After | Change |
| --- | ---: | ---: | ---: |
| ActRaiser | 2,445.62 ms | 2,463.63 ms | +0.74% |
| Battletoads | 462.41 ms | 477.62 ms | +3.29% |
| Power Rangers | 773.00 ms | 805.55 ms | +4.21% |

This pass collects bounded A/X/Y expressions at decoded direct calls, including
calls ultimately absent from the report. Its added analysis work is intentional;
the measured median differences are 15–33 ms. Host noise is substantial (for
example ActRaiser before-runs span 2.42–3.63 seconds), so these measurements are
not precise overhead estimates. They are not runtime performance measurements
or evidence of an optimization. Generated hot paths are byte-identical.

All five ActRaiser replay workloads pass with one warmup and three adjacent
A/B pairs: Mode 7/world map (6,000 frames), Sky Palace wide (1,200), simulation
actions (6,000), Aitos wide (4,000), and Death Heim wide (4,000). Their WRAM,
SRAM, CPU-state, and captured dispatch-artifact hashes match the reference and
immediate preceding milestone. No gated hard diagnostic executes. The suite
timing change is +0.10%; the largest workload median adjacent increase is
+0.64%. An initial Mode 7 +5.82% pair does not repeat (the next two are -0.33%
and -0.14%). These results show no material runtime regression
(`actraiser-replays.json` / `actraiser-replays.log`).

Battletoads' 1,800-frame gameplay and 3,600-frame stress workloads likewise
match reference CPU/WRAM/framebuffer/SPC/PCM output hashes. One warmup and
three adjacent A/B pairs measure +0.74% and +0.20%, respectively
(`battletoads-bench.jsonl`). Separate dispatch-enabled runs reproduce the
preceding milestone's logs and captured JSONL byte for byte, with no hard
diagnostics (`battletoads-traces.log` and `battletoads-run/`). Captured census
and milestone traces are not exhaustive full-run semantic edge sequences;
generated-source equality is the stronger preservation check for this
report-only change.

The next useful static work is the ROM-table index domain at the first two
callers and the writers/lifetime of the third caller's scalar input. Any
future promotion still needs alias, calling-contract, table-bound, code/data,
and entry-state proof; matching an address or observing a mask is not enough.
