# HLE dispatch inventory validation — 2026-09-09

## Scope and isolation

This milestone changes analysis reporting, not decoding or generated control
flow. Report v18 separates dispatch inventory, routing, static target-set
evidence, and observed coverage. HLE definitions and unresolved generation
gates are unchanged. Existing v17 fact databases remain compatible.

The baseline compiler was built before the patch, starting from repository
commit `d624d792`. Both candidates used identical copied bank configurations
and explicit temporary generation directories. Neither game's `src/gen` or
authored configuration was regenerated in place. ActRaiser's native frontend
was built from a committed source snapshot so concurrent, uncommitted
localization/rendering changes did not participate in the comparison.
Battletoads' frontend was copied into the temporary validation project.

Local artifacts for this run are under
`/private/tmp/snesrecomp-hle-inventory.hdVJKp`. No ROM is redistributed.

ROM SHA-256 identities:

- ActRaiser: `b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0`
- Battletoads: `b0dbd4d7e5689c32234e80b0c5362ef67c425ab72d6ddb49d1cb1133ef630ef7`

## Compiler contracts

The complete Go suite passes. Its process-group test requires a host `ps`
probe outside the sandbox; build/cache tests used temporary Go/Zig caches.
`go test -race ./internal/tooling` passes. The standalone runtime builds with
`-Werror` for C and C++, and all 33 CTest targets pass.

Redistributable synthetic fixtures cover:

- HLE hooks with multiple decoded M/X contexts, without extra trap emissions.
- HLE-only declarations that remain unrooted and receive no invented decode.
- Existing tagged-stream classification remaining visible behind an HLE hook.
- Trapped/missing targets, mirrored targets, emulation state, and return
  continuations in imported evidence, including sources absent from analysis.
- Repeat imports without multiplying hit counts; deterministic worker counts;
  exact source-bank filtering; mismatched ROM rejection.
- No authored-config edits or new proven facts from inventory/census evidence.
- Continued v17 database acceptance without relaxing static-proof checks.

## Regeneration equivalence

All generated output files compare byte-for-byte equal between compiler
versions, separately for each mode below. Normal and proven modes are **not**
being claimed equal to each other.

| Game | Mode | Variants | Generated files | Raw unresolved emissions | Stub markers |
| --- | --- | ---: | ---: | ---: | ---: |
| ActRaiser | Normal | 4,647 | 83 | 68 | 206 |
| ActRaiser | Proven analysis | 2,868 | 71 | 25 | 42 |
| Battletoads | Normal | 1,572 | 72 | 36 | 134 |
| Battletoads | Proven analysis | 913 | 48 | 17 | 66 |

Equal semantic-source SHA-256 values, before and after:

- ActRaiser normal: `a765bd3d6bead8677de2e114d9336d4d046d25a4b6f4bbeb891899a1670f2c20`
- ActRaiser proven: `e05796bacda9704b92801b4c3075aabbb8fb4b49bc6ab2ffb7e79ca7dbfb7ee7`
- Battletoads normal: `3d4bcf2a0509ff660a3fec60f50f90f8999e79efd43d25b04b6b7507a5cd0b61`
- Battletoads proven: `323dba14d504469f17770940b8f663434cce886c032f4e86f4cdd813a8fcf593`

Existing comparison summaries and proven database facts are unchanged.
Database exports differ only in `shadow_report_version: 17 -> 18`.
Battletoads regeneration using the same pre-patch v17 database also produces
byte-identical before/after output.

## Real-game reporting results

ActRaiser has 98 inventoried sites and no `hle_dispatch` declarations in this
configuration. Its function-level HLE declarations remain untouched.

Battletoads has four HLE dispatch hooks, all with unproven target sets.
Importing its existing gameplay census now attaches all seven observed
targets and 9,401 hits to `$02:8000`, which was previously absent. This does
not infer the complete 149-word table or automatically add handlers.

A fresh ordinary-trace 3,600-frame stress capture contains 50 unique edge/state
observations with no overflow, missing bodies, or trapped sites. Its merged
inventory has 52 source sites, including 27 observed-only sources. The object
dispatch `$02:8000` retains nine observations and 4,497 hits. Observed-only
sources include continuation/implementation edges; they are not automatically
classified as missing routines or promoted into code.

Use ordinary tracing for a registry census. A separate semantic-equivalence
build suppresses implementation-level registry events and is not a substitute
for measuring HLE-to-registry coverage.

## Runtime and performance results

ActRaiser uses normal generation and the five workloads in
`tools/runner-bench.json`: Mode 7/world map (6,000 frames), Sky Palace wide
(1,200), simulation actions (6,000), Aitos wide (4,000), and Death Heim wide
(4,000). All WRAM, SRAM, CPU-state, and dispatch-log artifact hashes match.
One warmup and three adjacent A/B pairs per workload pass the equivalence and
performance gates: suite regression **+0.39%**, largest workload median-pair
regression **+1.62%**. These small differences are consistent with host noise.
A separate five-workload A/B pass also passed with the diagnostic gate expanded
to include missing-M/X variants and watchdog/wedge markers.

Battletoads uses proven generation, with Start at frames 120 and 500:

| Workload | Frames | WRAM CRC32 | Framebuffer CRC32 | Nonzero PCM samples | Peak |
| --- | ---: | --- | --- | ---: | ---: |
| Gameplay | 1,800 | `41239B5D` | `E3320F4F` | 2,020,003 | 29,576 |
| Stress input | 3,600 | `6D61B18E` | `857C951D` | 4,898,412 | 29,576 |

Before/after logs match byte-for-byte, including CPU state; both exit with
result zero and no hard dispatch/watchdog diagnostics. Semantic-dispatch
trace JSONL also matches byte-for-byte for both workloads. Three warmed A/B
pairs show no regression (stress median difference **-0.09%**; gameplay is
noisier). These comparisons use the same current runtime on both sides,
not an older downstream runtime snapshot.

The saved ActRaiser dispatch artifact contains an event total and a bounded
history; trace JSONL contains census milestones. Neither is an exhaustive
full-run edge-sequence recording. Here the stronger compiler check is that
all behavior-bearing generated source is byte-identical. A later milestone
that changes control flow still needs the full semantic-edge validation gate.

Seven warmed, alternating analyzer A/B pairs on the final implementation show
median wall-time differences of **+0.04%** for ActRaiser and **-0.16%** for
Battletoads. No material analyzer
or runtime performance regression was found.

## Next milestone

Connect decoded caller-side table loads and pointer stores to shared jump
trampolines, accounting for D/DB, M/X, aliases, and intervening writes. Table
extent and index-domain proof remain separate obligations. Do not replace
HLE routing or promote plausible table words merely because this inventory
now makes them visible.
