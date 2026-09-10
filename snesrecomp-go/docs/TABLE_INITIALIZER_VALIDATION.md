# Table-initializer audit — 2026-09-09

## Scope and isolation

This extends [pointer-index evidence](POINTER_INDEX_VALIDATION.md) with decoded
initializer loads, one nested direct-page pointer-source hop, local index
bit constraints, and bank setup evidence. It remains report-only. No roots,
M/X choices, generated dispatch semantics, authored config, or HLE definitions
change. Addresses below describe validation inputs, not shared compiler policy.

Artifacts are under `/private/tmp/snesrecomp-initializer.PnIxEN`; the immediately
preceding baseline is `/private/tmp/snesrecomp-pointer-index.DoUHKt`. The before
tool was built before these edits. The three ROM identities and frozen cfg
copies are unchanged from that baseline. Native frontend builds remain isolated
in the earlier `snesrecomp-hle-inventory.hdVJKp` snapshots, excluding unrelated
rendering/localization changes in the live working tree.

## Power Rangers findings

Four existing script-pointer writers now have initializer records. Two expose
useful table chains; two correctly retain unknown entry-register/bank state.

| Writer | Recovered source | Index evidence | Remaining gap |
| --- | --- | --- | --- |
| `$00:D3A8` → `$02A2` | `$05:A3A2,X` at `$00:D3A5` | `$0282` shifted left once | State range, field alias/lifetime, table extent |
| `$06:9B35` → `$1622,X` | `($98),Y` at `$06:9B33` | `$1442,X` shifted left once | Pointer alias/wrap and state range |
| `$06:9B70` → `$1622,X` | `$0002,Y` at `$06:9B6D` | Y at function entry | Entry Y and DB |
| `$06:9B80` → `$1622,X` | `$1620,X` at `$06:9B7D` | X at function entry | Entry X and DB |

For the bank-five initializer, `LDA #$0500; PHA; PLB; PLB` establishes DB=$05
at `$00:D301`. This definition is farther back than the earlier 32-step bank
query allowed. The separate initializer query now recovers it on the decoded
unique path. The `$0282` index-source expression has one decoded writer,
`$00:D30A`, which copies `$0284` loaded at `$00:D307`. There are no literal
source values and no small local domain, so **no table words are sampled**
from this initializer and no script starts are guessed.

For the bank-six two-level lookup, `$06:9B2B STA $98` receives the word from
`$06:9B28 LDA $A828,Y`, with DB=$06 established by PHK/PLB. Its Y is twice
the word loaded from `$A0`. The outer `LDA ($98),Y` uses twice `$1442,X`.
The report keeps these as two distinct index expressions; it does not confuse
the pointer-table index with the per-object stream index.

Both shifts alone imply 32,768 possible even words, **not a table length**.
The decoded `$A0` scratch expression has 273 writer records across program
banks. Their literal values yield 25 hypothetical indices: 21 mapped word
samples and four bank-boundary exclusions. Many writers clearly serve other
uses of this scratch slot. These samples are not evidence for 21 valid tables
or streams, and none is promoted or substituted into the outer indirect read.
The `$1442,X` source has one matching zero writer in this decoded closure;
that is not a complete object-state domain either.

The useful next static target is the incoming state/object-type domain,
especially values entering `$06:9B00` through A and then being stored to `$A0`.
That requires local/reaching-definition or call-site argument evidence; a
global scratch-address match cannot replace it. A similar missing domain lies
behind the `$0284` → `$0282` chain in bank zero.

## Other games and safety boundaries

ActRaiser and Battletoads have no new initializer records in the existing
bounded pointer-producer query. Their previous findings remain intact:
ActRaiser's six memory-fed RTS sites / 47 writer PCs / 175 authored-address
candidates, and Battletoads' 25 writer-derived authored handler addresses.
This pass proves no new missing functions or removable configuration entries.

Synthetic redistributable fixtures cover mask/shift/transfer chains, sparse
domains, bitwise OR/XOR, word wrapping, unknown calls/origins, byte truncation,
unrelated comparisons, and exhaustive concrete checks of every 16-bit input
against the bit-domain abstraction. Bank fixtures cover low/high word pulls,
byte pushes and over-pulls, PHK/PEA, long local paths, joins, unknown replacements,
callee barriers, and walk budgets. Two-level fixtures check pointer clobbers,
D changes, and calls, and explicitly prohibit substituting inner table samples
through a conditional alias. Report/database tests retain HLE obligations and
check worker-count independence and unproven target classification.

## Preservation checks

Full `go test ./...`, tooling race tests/vet, strict standalone runtime build,
and all 33 C/C++ runtime CTests pass. The existing Go process-group test uses
read-only host process inspection. One-worker and eight-worker JSON reports
are byte-identical for all three games. Removing only the new
`table_initializers` field reproduces each preceding report exactly, including
all existing pointer candidates, RTS flows, summaries, and HLE obligations.
All three proven-fact databases are byte-identical to the prior milestone.

Normal and proven regeneration each reproduce their own preceding generated
files byte for byte. No live generated directory is touched:

| Game | Normal variants/files | Proven variants/files |
| --- | --- | --- |
| ActRaiser | 4,647 / 83 | 2,868 / 71 |
| Battletoads | 1,572 / 72 | 913 / 48 (+ synchronized `funcs.h`) |
| Power Rangers | 8,248 / 138 | 7,318 / 122 |

The isolated ActRaiser/Battletoads native builds compile against their newly
regenerated directories. Power Rangers validation here covers analysis and
generation only, not native gameplay or audio.

Seven warmed alternating analysis A/B pairs, run after compilation completed,
include JSON serialization/output:

| Game | Before | After | Change |
| --- | ---: | ---: | ---: |
| ActRaiser | 2,401.44 ms | 2,410.00 ms | +0.36% |
| Battletoads | 216.54 ms | 218.30 ms | +0.81% |
| Power Rangers | 635.88 ms | 644.51 ms | +1.36% |

The added scan costs about 2–9 ms at these medians; there is no material
analysis regression. This is analysis cost, not runtime cost or an optimization
claim (`analysis-bench.jsonl`).

All five ActRaiser workloads pass with one warmup and three adjacent A/B pairs:
Mode 7/world map (6,000 frames), Sky Palace wide (1,200), simulation actions
(6,000), Aitos wide (4,000), and Death Heim wide (4,000). WRAM, SRAM, CPU-state,
and captured dispatch-artifact hashes match the frozen reference and preceding
milestone. No gated hard diagnostic executes. Suite timing changes by -0.07%,
with the largest workload increase +0.37%, indicating no material runtime
regression (`actraiser-replays.json` / `actraiser-replays.log`).

Battletoads' 1,800-frame gameplay and 3,600-frame stress runs reproduce the
reference CPU/WRAM/framebuffer/SPC/PCM output hashes. One warmup and three
adjacent A/B pairs measure -0.03% and +0.18%, respectively: no material runtime
regression (`battletoads-bench.jsonl`). Separate dispatch-enabled runs reproduce
the preceding milestone's logs and captured JSONL byte for byte, with no hard
diagnostics (`battletoads-traces.log` and `battletoads-run/`). These captured
census/milestone traces are not exhaustive full-run semantic edge sequences;
generated-source equality is the stronger preservation evidence for this
report-only change.
