# Read-only shadow analysis

`snesbuild analyze` is the first stage of the recompiler's evidence-driven
workflow. It runs static inference without supplying authored
`indirect_dispatch` or `rts_dispatch` declarations to the decoder, normalizes
the independently inferred facts, and only then compares them with the cfg.
It does not generate C or update configuration. By default it writes nothing;
an explicit `--out-analysis` path exports the proven subset as a deterministic
analysis database.

From a game project root:

```sh
snesbuild analyze --root . --rom game.sfc --compare-authored --no-write
```

The low-level equivalent is:

```sh
v2regen analyze --rom game.sfc --cfg-dir recomp --jobs 8
```

To persist only facts that are safe for the AOT overlay:

```sh
snesbuild analyze --root . --rom game.sfc \
  --out-analysis saves/static-analysis.json
# low-level equivalent:
v2regen analyze --rom game.sfc --cfg-dir recomp \
  --out-analysis build/static-analysis.json
```

Both commands are unconditionally read-only. `--dry-run=false` and
`--no-write=false` are rejected by `snesbuild analyze`; `v2regen analyze`
likewise rejects `--no-write=false`. Use `--format json` for the complete,
deterministically ordered machine-readable report and `--verbose` for every
text record. `--strict` reports everything and then returns nonzero only when
independent evidence proves a semantic conflict.

The fact database has no timestamp or build-machine path. It records its
schema and inference-report versions, provenance, the headerless ROM SHA-256,
ROM size, mapper, and normalized static evidence. Runtime observations,
probable findings, open target sets, partial matches, compatible broad safety
guards, and garbage-only decodes are excluded. Exact authored dispatch matches
are retained as replacement facts so their cfg directives can be removed
later. Generation rejects an unknown schema, non-static evidence, a different
ROM, changed authored dispatch semantics, invalid instruction bytes, invalid
target tables, or an ownership edge that no longer decodes exactly. A
single-bank analysis cannot be exported as a whole-ROM database.

## Comparison meanings

- `exact_match`: every currently modeled semantic field agrees, including a
  closed target set.
- `compatible_guard`: independent analysis proves a narrower site-specific
  continuation set contained by an authored RTS safety guard, or proves every
  continuation allowed by a deliberately broad guard elsewhere in the same
  bank. This is compatible rather than exact: the extra authored cases remain
  guarded fallbacks and are not claimed to execute.
- `partial_match`: the known structure agrees, but at least one field remains
  open. A matching table base without a proven bound is intentionally partial.
- `conflict`: independently known fields disagree. Unknown or heuristic fields
  do not manufacture conflicts.
- `authored_only`: the declaration has not yet been independently proven. This
  does not mean the edge executes, is wrong, or is gameplay-reachable.
- `automatic`: analysis found a dispatch fact that has no corresponding
  authored declaration. Existing automatically recovered tables normally fall
  into this category and are not unresolved failures.

Unresolved reporting separates raw decode occurrences from unique instruction
sites. Each unique site records instruction bytes, mnemonic, addressing mode,
operand, live M/X state, and configuration-rooted callers. A configuration root
is not labeled gameplay-reachable.

## Decoded-instruction cross references

Use the compiler's decoded instruction index instead of searching ROM bytes for
opcode/operand patterns:

```sh
snesbuild xref '$1C' --root . --rom game.sfc
v2regen xref '$00:C210' --rom game.sfc --cfg-dir recomp --format json
```

Address width is intentional. `$1C` searches direct-page and stack-relative
operand offsets, `$001C` searches 16-bit operands, and `$00:001C` searches only
references whose architectural bank is known. Results contain the instruction
PC and bytes, access kind, addressing mode, live M/X, and every containing
configuration/static-call-rooted function. Indexed results identify a base;
they do not claim the runtime index or effective address is known. Absolute
data operands are DB-relative and direct-page operands are D-relative, which
is stated explicitly in the result rather than silently assuming DB=D=0.

The index contains only instruction boundaries reached by the shadow decoder.
It therefore avoids byte-pattern matches inside operands and tables. It does
not claim that every configured root is gameplay-reachable or that current
code/data ownership is globally complete.

## Dispatch-table ownership

The JSON report includes `table_spans`. An authored closed dispatch or a closed
static proof produces `confirmed_data`; a heuristic open table produces
`candidate_data`. Every span records its owning dispatch site, start and
exclusive end, entry count/width, confidence, and provenance.

Confirmed spans are suitable evidence for the future shared code/data
ownership model. Candidate spans remain report-only: they do not suppress
decoding, change generated C, or write `data_region` directives. This prevents
a plausible table prefix from hiding real code before its bound is proven.

Indexed tables may contain zero-valued holes. Automatic recovery retains a
zero run only when a later plausible target proves that the table continues
and the complete span lands exactly on the earliest same-bank handler. That
larger count is reported as a structurally supported but still open candidate;
normal generation keeps the conservative pre-zero count. If no later target
closes the hole, the analyzer reports only the heuristic prefix instead of
treating padding as table entries. Later plausible words without an exact
handler landing are also shown, but explicitly as an unbounded heuristic.
Promoting either larger count remains a behavior-affecting step gated by replay
evidence.

## Dispatch gap and code-island triage

The JSON report includes `dispatch_code_islands`, and verbose text prints
`[DISPATCH-CODE-ISLAND]` records. For each closed computed-dispatch target set,
the analyzer compares decoded byte ownership between adjacent same-bank
handlers. If a decoded `BRA`, `JMP`, return, or other flow terminator skips an
unclaimed byte range and sequential decoding of that range reaches an
`RTS`/`RTL`/`RTI` before the next claimed block, the range is reported with its
candidate entry, end, neighboring targets, and viable M/X states.

These are probable review hints, not generated entries. Shared handler tails,
data embedded between routines, and width ambiguity prevent the sweep from
proving a target set. Confirm a candidate with dispatch-census or an external
trace before adding it to configuration; the analyzer never feeds a code-island
finding into the proven-analysis overlay.

## Boundary landing sweep

The broader JSON report also includes `landing_candidates`, and verbose text
prints `[LANDING-CANDIDATE]` records. This pass generalizes the dispatch-only
gap check without reverse-disassembling variable-length 65816 instructions:

1. confirmed function entries provide forward landing boundaries;
2. the analyzer seeks backwards at most 256 bytes for an unclaimed byte after
   a decoded `RTS`, `RTL`, `RTI`, `BRA`, `BRL`, `JMP`, or `JML`;
3. it decodes forwards from that candidate under all four entry M/X states;
4. it retains only graphs whose every path stays in the candidate range and
   either reaches the confirmed boundary or terminates with a return.

Acceptance is intentionally stricter than plausible linear disassembly. The
candidate must contain at least two instructions, have consistent instruction
boundaries, avoid all decoded code, declared data, and confirmed table spans,
and contain no unresolved transfer, `BRK`, `COP`, `WAI`, `STP`, or unmodeled
`XCE`. General stack depth is propagated across the graph; underflow,
conflicting join depths, an unbalanced return/tail join, or an unmodeled
`TCS`/`TXS` rejects the candidate. PHP/PLP M/X restoration continues to use
the main decoder's abstract status stack.

Each finding reports the candidate range, the next confirmed anchor, viable
entry M/X states, predecessor terminator, termination shape, and instruction
count. A candidate already reported with the stronger closed-dispatch context
is omitted from this list. Stack-consistent regions with a stronger structural
shape are `probable`; a two-instruction return with neither a call nor an
explicit edge to the anchor remains visible as `speculative`. Both are
code-ownership evidence, not proof of a caller or runtime reachability, and
neither is ever added to generated code or the experimental proven-analysis
overlay. Overlap with a merely candidate table span is reported as
`candidate_table_conflict` and downgraded to speculative instead of letting
one heuristic silently suppress another. Pointer corroboration is layered on
top as a separate finding so the post-terminator seed remains independently
testable.

## Vector-root entry recovery

The `entry_recovery` object answers a different question from ordinary variant
discovery: how many authored `func` declarations can be found without using
those declarations as roots? A second read-only fixed point withholds every
authored function entry and starts only at the ROM's reset, native NMI, and
native IRQ vector targets. Direct calls, long calls, statically recovered
computed targets, and stack-proven continuations then grow the closure.

Each authored declaration receives one status:

- `exact_variant`: its address and authored entry M/X state were recovered;
- `address_other_mx`: the address was recovered, but only under different M/X;
- `proven_continuation`: stack provenance found an internal resume point, so it
  must not be registered as an ordinary callable routine merely to add code;
- `internal_owned`: the PC is an instruction boundary inside a recovered
  region, but no external entry edge has been established;
- `not_recovered`: the current vector-rooted closure has no evidence for the
  declaration.

Counts are authored declarations and root-generated `(PC,M,X)` variants, which
keeps extra width variants from masquerading as recovered configuration lines.
The complete deterministic JSON report contains every declaration; verbose
text prints the non-exact cases.

For declarations outside the root closure, the same audit scans both word
alignments for contiguous same-bank ROM words that name at least two distinct
authored entries. Each `pointer_clusters` record includes the table range,
entry/distinct-target counts, occurrences of that declaration, ownership, and
confidence. Confirmed dispatch-table ownership is proven corroboration;
candidate table ownership and unclaimed runs of three or more words are
probable; two-word runs and overlaps with decoded code remain speculative.
These are config-assisted corroboration facts, not automatic recovery: they
show where a hand-authored conclusion likely came from but do not establish a
runtime index, bound, or caller.

This audit is deliberately a lower bound. `not_recovered` does not mean dead,
garbage, or safe to remove: handler addresses may live in ROM streams, WRAM
state fields, split tables, save data, or runtime observations not yet imported
into the static closure. Authored data regions, HLE dispatch declarations, and
exit-M/X routes remain available so this phase measures `func` removal rather
than removing every escape hatch at once. A later strict audit can ablate those
inputs independently.

## Static authored-entry ablation

The `entry_ablation` report asks a less pessimistic question than withholding
all authored functions at once: which exact `(PC,M,X)` declarations are
recoverable through static dependencies when the other authored roots remain?
Every decoded variant retains the finite targets it demands through direct
JSR/JSL calls, direct long jumps, proven computed dispatches, and explicit
sibling edges. Those source-labelled demands form a deterministic variant
dependency graph; mirrored banks are canonicalized before comparison.

For each declaration, the analyzer removes only that declaration from the
root set and tests whether its exact variant remains reachable from another
authored declaration or reset/NMI/IRQ. `individually_recoverable` therefore
means what it says; a differently-sized M/X variant at the same address does
not count.

The analyzer also removes declarations in descending deterministic order,
retaining a declaration whenever its removal would leave any authored variant
outside the graph closure. The result is an inclusion-minimal root set:

- `vector_graph_covered` needs no authored graph root;
- `batch_recoverable` is reachable from vectors or the retained roots;
- `retained_static_root` is required by this particular deterministic root
  set.

Incoming edges retain their transfer provenance. The report derives an entry
kind hint rather than flattening every recovered address back into `func`:

- direct JSR/JSL edges imply `routine`;
- direct JMP/JML edges imply `tail_target`;
- closed computed-dispatch edges imply `computed_handler`;
- a target recovered only because decoding stopped at an authored sibling is
  an `internal_continuation`.

That final category is not a callable-function removal candidate. The summary
counts batch-recoverable declarations by these kinds so a large number of
sibling-only edges cannot be mistaken for immediately safe cfg deletions.
Each record includes its standalone `decoded_instructions` cost and the exact
live-width predecessor edges that stopped at the sibling boundary.

Inclusion-minimal does not mean globally minimum cardinality. More
importantly, dependency reachability does not yet prove that removing a cfg
line preserves generated-region boundaries or external entry semantics. The
graph is collected while all authored sibling boundaries are present, so this
full root set remains report-only. It identifies high-value ablation batches
and cyclic root groups for isolated regeneration; it never edits
configuration or feeds the unrestricted continuation/tail/computed sets into
production generation.

There is one deliberately narrow experimental consumer. A
`batch_recoverable` entry classified as `routine` and supported by an exact
direct JSR/JSL edge may be selected as a static entry fact. Isolated proven
regeneration withholds that exact `(PC,M,X)` variant from the initial root set
and requires ordinary variant discovery to demand it again. This validates
the static dependency without treating the broader ablation graph as proof.
It is not yet a cfg-rewriting feature. Each entry record lists
`template_blockers` for metadata that cannot be synthesized: a custom name,
`end`, `exit_mx`, `tail_call`, `entry_s_offset`, or an HLE obligation. A
routine with no blockers has the canonical `bank_NN_PPPP` name and default
function options. The isolated generator strips that template to a dormant
PC/M/X slot, removes its name and canonical-registry identity, and recreates
them only after an exact static call demand rediscovers the routine. The slot
keeps deterministic cfg ordering during this proof; it is not semantic entry
metadata.

Each entry record also lists `authored_hle` obligations (`hle_func`,
`hle_func_if`, and `hle_spc_upload`). An entry with any such obligation is
excluded from static root suppression. The separate `hle_obligations` inventory includes every HLE
directive even when there is no explicit `func` at that PC; `authored_entry`
distinguishes attached and HLE-only policy. This is intentional because a
common configuration style relies on static call discovery to create the
entry and keeps only the HLE directive authored.

A second experimental consumer validates an exact resumable-region contract.
It selects only a metadata-free `internal_continuation` with one or more exact
sibling-boundary owners and edges. Region size is not a semantic criterion. A
single-owner continuation remains registry-visible through a public wrapper,
while the owner and continuation share one private generated body whenever the
continuation's standalone decode closure exactly matches the owner subgraph.
The wrapper performs the continuation's entry M/X check and establishes one
recompiler activation; an internal edge remains an in-region `goto` and
therefore creates no recursive or duplicate activation.

Generation re-decodes the closed owner graph and rejects a fact whose claimed
edge is absent. It also checks exact decoded closure before replacing a
standalone body with a wrapper. Nested single-owner continuation trees are
flattened into one body so they do not add helper activations. If sharing is
still unavailable because the external closure differs, the exact local edge
remains valid but the standalone body is retained; regeneration reports this
as a resumable-region fallback so source cost is visible.
`batch_single_owner_continuations`, `batch_multi_owner_continuations`, and
`batch_region_eligible_continuations` describe the statically selectable set;
the regeneration summary separately reports shared wrappers and fallbacks.

An acyclic multi-owner continuation uses a different shared-body ABI. Its
registry-visible public wrapper establishes a new activation for a true
external entry, but each exact owner edge calls the same generated body with
the owner's live `_entry_s` and host-return context. That call does not push a
second recompiler activation, and the shared body owns the existing frame's
return/pop path. The report separates statically proven edges from emitted C
call sites because multiple proofs can collapse onto one decoded site. If an
owner is itself a single-owner continuation, the exact call is routed through
every generated ancestor body that contains it. If the multi-owner target owns
a single-owner region, that region's body is reused with external linkage.

The ownership graph is checked before either lowering.
`batch_acyclic_continuation_overlaps` counts overlap chains that can reuse the
ordinary target bodies. `batch_cyclic_continuation_overlaps` counts facts in a
strongly connected ownership region. A cyclic region is lowered as one
externally linked selector body: every continuation entry retains a public
wrapper, edges inside the component are local gotos, and only edges from
outside the component call a selector. This avoids recursive C helpers and
does not depend on host compiler tail-call optimization.

## Table-first unknown target discovery

The `table_first_targets` array crosses two independent review-only signals
without weakening either one. It scans both ROM word alignments for an unknown
same-bank program pointer that is either:

- between two distinct authored entry pointers with at most two missing
  words; or
- immediately before or after a contiguous run containing at least two
  distinct authored entry pointers.

If the unknown value already names a decoded instruction boundary, it is
reported separately as `address_taken_internal`. This is evidence for an
external block/resume entry, not permission to wrap the address in a new C
routine activation. A pointer into the middle of an existing instruction is
rejected.

Otherwise the value is subjected to the boundary landing sweep's same bounded,
all-M/X forward-decode contract. An existing post-terminator landing is reused
when available; otherwise the pointer value seeds a fresh decode. A confirmed
entry within 256 bytes is the preferred boundary. Without one, the pass may
accept a clean return before a 256-byte scan limit, but an edge that merely
joins that artificial limit is rejected. The candidate must still pass the
same instruction ownership, all-path range, stack balance, return or explicit
confirmed-anchor join, and unsafe-opcode rejection rules. This avoids treating
plausible linear disassembly as sufficient evidence. A pointer-shaped word
whose target fails those semantic checks is retained in
`table_first_rejections` with a reason rather than silently disappearing; a
value outside the current mapper's same-bank program range is discarded before
probing. Pointer-window-seeded regions also reject `RTI`: interrupt regions are
valid boundary-landings when rooted by vectors, but ordinary same-bank table
words must not turn compact data records ending in byte `$40` into invented
interrupt entries.

Each `[TABLE-FIRST-TARGET]` record includes its candidate entry M/X states,
landing anchor, classification, landing seed (`post_terminator`,
`pointer_window`, or `decoded_instruction`), and every minimal source window.
Confirmed or candidate table
ownership and unclaimed three-word windows can yield `probable`; decoded-code
overlap, a weak landing shape, or a candidate-table conflict remains
`speculative`. Findings do not enter variant discovery, generated entry
registries, or the proven-analysis overlay. Authored entries are used only to
anchor the source window, so this phase can expose a missing middle or edge
handler but still cannot prove the table's runtime base, index, bound, or
reachability.

The same report recognizes a deliberately narrower `base_plus_u16` encoding.
The base must have code provenance from a decoded 16-bit `CLC; ADC #base`
sequence, must itself be an authored entry, and the compact offset run must
contain zero (mapping back to the base) plus at least two other distinct known
entries. Only missing interior offsets are considered, addition may not wrap,
and source windows overlapping decoded code are discarded. Each source records
the base and arithmetic evidence PCs. This is intentionally stricter than
trying every authored entry as a possible base, which produces thousands of
coincidental matches in ordinary game data. Split low/high/bank tables and
other arithmetic forms remain later extensions.

## Experimental isolated regeneration

The low-level generator can consume the safest automatic facts without
changing authored configuration or the normal generated directory:

```sh
v2regen regen --rom game.sfc --cfg-dir recomp \
  --out-dir build/proven-analysis-candidate \
  --experimental-proven-analysis
```

The flag deliberately refuses `src/gen`. It reruns shadow analysis, rejects
authored conflicts, and selects only automatic facts whose target set is
closed, whose fields are fully known, and whose evidence is exclusively
static `proven` evidence. Observed and probable facts remain report-only. The
generator then validates each selected instruction and target table against
the exact ROM bytes before applying an in-memory overlay. No `.cfg` file is
written.

The persisted equivalent avoids rerunning inference during a hermetic build:

```sh
v2regen regen --rom game.sfc --cfg-dir recomp \
  --out-dir build/proven-analysis-candidate \
  --analysis-db build/static-analysis.json
```

`--analysis-db` and `--experimental-proven-analysis` are mutually exclusive.
The low-level command keeps the same isolated-output guard. The project driver
also accepts `snesbuild regen --analysis-db saves/static-analysis.json`; it
adds database-supplied canonical entries to `funcs.h` after their redundant
`func` declarations are removed. Matching authored dispatch declarations may
remain during migration, but they must exactly match the persisted fact. They
are normalized to that fact in memory, including exact live-M/X routing, so
removing one does not change database-mode generated C. A changed authored
directive fails closed instead of silently overriding the snapshot.

Supplying an analysis database explicitly selects the complete proven-analysis
generation mode even when the database contains zero persisted facts. Exact
static direct-call and variant discovery can therefore reduce variants and
change the semantic source hash relative to conservative generation without a
fact being applied. A zero-fact database is not a no-op flag: compare and test
it as a separate A/B candidate. The database remains useful in this case as a
ROM-hashed declaration that no inferred fact crossed the persistence threshold.

For proven dispatch sites, variant discovery requests the exact target M/X
state established by the current decode and any modeled SEP. It does not add
all four speculative handler variants. Authored or uncertain dispatches keep
the conservative live-state multi-variant path.

The same isolated mode propagates exact live M/X across ordinary direct
JSR/JSL calls. Those calls lower to one compiled callee variant instead of a
runtime four-way M/X switch. A `force_variant_at` override still takes
precedence. This is currently gated with the analysis overlay so it can be
validated on additional games before becoming the normal production policy;
the default regeneration path remains unchanged.

An exact static demand is also a prune root. Emit-truth may remove another
decode only when the variant-equivalence proof shows that the surviving body
has identical entry semantics. A canonical or nearby survivor is not such a
proof. Exact direct calls make generation fail with the caller PC, target, and
required M/X when this invariant is violated. Runtime-M/X dispatches retain
their compiled cases, but an absent unproved state calls
`sr_missing_mx_variant_warn` instead of executing a different-width decode.
The diagnostic is a safety guard: it prevents silent corruption but remains
coverage debt until representative runs establish that the state is cold or
analysis supplies the exact/equivalent body. Include `[missing-mx-variant]` in
replay hard-diagnostic lists.

The mode also performs a fail-closed root-suppression check for the narrow
static routine facts described above. Selected entries remain in their
original configuration order as dormant slots, but are excluded from initial
decoding and from sibling-boundary stopping. An exact static call demand must
reactivate each one during the variant fixpoint. Generation aborts with the
missing PC and M/X state if even one selected root is not rediscovered.
Blocked templates preserve their authored metadata; blocker-free templates
must synthesize the canonical name/default options and restore their registry
identity. Both paths preserve deterministic file layout while proving that
the routine no longer needs to be an unconditional decode seed. Internal
continuations, tail targets, computed handlers, and external roots are never
suppressed by this experiment. HLE-decorated roots
are also never suppressed; selection filters them and regeneration rejects a
manually supplied fact as a second line of defense. Any eventual cfg-deletion
workflow must migrate these obligations explicitly rather than infer their
absence from static reachability.

Exact static JSR/JSL discovery also works when the canonical `func` line is
already absent. Variant-demand provenance records the source edge and entry
kind, creates the canonical `bank_NN_PPPP` body at the live M/X state, and
continues to apply any independently authored HLE directive at that PC. A
new canonical routing preference is installed only when the generated address
has one M/X variant. Multi-variant addresses are still generated and reported,
but retain the existing conservative routing policy until the complete state
set is proven. Regeneration reports both the exact discoveries and the subset
receiving singleton canonical promotion.

Metadata-free routine and continuation facts can also supply a canonical entry
template that is already absent from cfg. The database records its original
bank-local ordinal so pruning does not perturb function or translation-unit
layout. All missing templates are seeded in memory before ownership validation,
so nested and cyclic continuation regions do not depend on fact order.
Continuations remain externally dispatchable;
routine templates are withheld and must still be rediscovered by an exact
static call. HLE directives remain authored policy and are never synthesized
or discarded. Any HLE obligation or special entry metadata at a supposedly
template-free PC makes generation fail closed.

Every regeneration computes a `generated semantic source` SHA-256 over sorted
function bodies, effective void-alias targets, the dispatch registry, and
unresolved trap bodies. The hash intentionally ignores cfg order,
translation-unit splitting, and forward-declaration order. Matching hashes
therefore establish source-level generation equivalence when deleting a
redundant canonical entry merely moves its body within a generated file. This
is a hermetic compiler check, not a substitute for runtime/replay validation
after a behavior-bearing source difference.

This mode changes generated control flow and therefore requires the normal
runtime gate. Validation builds may define
`SNESRECOMP_SEMANTIC_DISPATCH_TRACE=1`. In that mode generated direct
dispatches and generic registry dispatches emit the same semantic
source-PC/target-PC event, while lowering-dependent registry events are
suppressed. Dispatch hashes can therefore be compared directly instead of
accepting a difference caused only by replacing a registry lookup with a
compiled switch. Raw non-semantic dispatch logs can also differ when an
authored sibling boundary that formerly required an implementation-only
registry tail transfer becomes a local goto; that artificial edge is omitted
from both semantic builds.

## Verified configuration materialization

`materialize` is the explicit write/adoption counterpart to read-only
`analyze`. It is available in both the packaged native `snesbuild` driver and
the development `v2regen` tool; no Python or interpreter is involved.

```sh
snesbuild materialize --root . --rom game.sfc --cfg-dir recomp \
  --out-dir build/static-bundle --jobs 8 --allow-stubs
```

Every relative path is resolved from `--root` (default: current directory).
The output directory must not exist, its parent must exist, and it must not
overlap the input cfg or be `src/gen`. There is no force/in-place option.
Symlinks and special files inside the input tree are rejected. The ROM and
cfg are snapshotted into private staging; analysis and both regenerations
consume those same bytes without modifying the source project.

The command performs the following verified transformation:

1. Analyze the full cfg and create the existing ROM-hashed proven-fact database.
   An authored conflict aborts with the site/reason before publishing anything.
2. Copy the cfg/support files. Remove only metadata-free canonical `func`
   declarations covered by persisted routine or exact continuation facts,
   and authored dispatch declarations with closed, independently **exact**
   matches. Compatible bounds guards, partial matches, custom names, HLE,
   unknown/future entry options, and width/stack overrides remain authored.
   Preserve unchanged lines, line endings, and comments, including inline
   comments on removed declarations. Record each removed original line and
   its proof evidence in the manifest.
3. Reload the persisted DB and regenerate full and reduced cfg with that same
   DB, exact-M/X policy, and default chunk layout. Rediscovery, ownership,
   ROM-table, and metadata validation remain enabled. Generate `funcs.h` for
   both, including canonical declarations supplied by the database.
4. Require identical generated file sets and bytes, including `funcs.h`, plus
   matching semantic source hash, final variant count, unresolved count, and
   stub count. Any difference fails closed. Even a source-layout-only
   difference is rejected; the semantic hash alone is not this gate.
5. Exclusively reserve the output directory and publish the verified bundle.
   `materialization.json` is written last as the completion marker. An I/O
   failure during publication can leave an incomplete directory without that
   marker; it must not be adopted. Existing directories are never replaced.

The completed layout is:

```text
static-bundle/
  recomp/                 copied cfg/support files, reduced bank declarations
    funcs.h               regenerated and byte-checked, not copied stale
  analysis-db.json         required, machine-generated hermetic build input
  gen/                    verified generated C and headers
  materialization.json    removals/provenance, ROM and input/output SHA-256s
```

The manifest and database contain no timestamps or build-machine paths.
The ROM snapshot is not published. The original cfg (including its original
`funcs.h`) is untouched. Regenerate from the published inputs with:

```sh
v2regen regen --rom game.sfc \
  --cfg-dir build/static-bundle/recomp \
  --analysis-db build/static-bundle/analysis-db.json \
  --out-dir build/static-bundle/gen \
  --funcs-out build/static-bundle/recomp/funcs.h --allow-stubs
```

Keep cfg and DB together, and configure a test build to use the bundle's
generated sources and header. Do not run plain `sync-funcs` on the reduced cfg:
it cannot see database-supplied declarations. Do not re-analyze only the reduced
cfg and discard the original DB; materialization is a snapshot migration, not
yet an incremental database-maintenance workflow. Retain the authored source
and manifest for future changes and rematerialization.

The equality claim is **full versus reduced cfg under the same proven-analysis
mode**, not default versus proven-analysis generation. It neither proves that
unresolved sites are cold nor removes existing traps. `--allow-stubs` permits
those existing markers but never a generated difference. First adoption of
proven-analysis mode on a game still needs its normal runtime/replay validation.
Removing cfg declarations after this byte-equality gate introduces no further
generated behavior change. Conditional return, alias, known-bit, and heuristic
landing reports are not promoted by this workflow.

## Entry and transfer invariants

Entry classification and transfer behavior are separate axes:

| Entry kind | Meaning |
| --- | --- |
| `reset_interrupt` | Hardware reset, NMI, IRQ, or other interrupt root |
| `routine` | Normal JSR/JSL-callable routine |
| `tail_target` | Address entered without creating a new logical call |
| `computed_handler` | Address taken by a runtime-target dispatch |
| `continuation` | Internal point that resumes an active generated region |

Transfers are `call`, `tail`, `resume`, or `interrupt`. In particular, a
continuation reached by `resume` must not execute a routine-entry prologue,
create a duplicate C activation, or establish a new stack baseline. This model
is metadata in the shadow phase. The experimental overlay uses both proven
PHA/RTS return PCs and the narrow exact sibling-edge contract above as internal
resume points. The containing generated region reaches the continuation with a
local `goto`, while a separate registered entry remains available for genuinely
external dispatch. Ordinary sibling entries still stop region growth.

This distinction is behaviorally significant even when final machine state
converges. Re-entering a continuation through the sparse registry creates a
new host activation and can execute the continuation twice: once through that
activation and once when the original generated region resumes. Semantic edge
tracing is the validation contract that detects this class of error.

## Safe cfg-removal gate

An exact shadow match proves that analysis reproduced an authored conclusion;
it does not by itself prove that the original conclusion was correct. A
compatible guard proves coverage, not removability: replacing a broad guard
with a narrower inferred set is a behavior change until regeneration and
runtime evidence establish otherwise. Before any directive is removed,
regeneration must withhold that directive, substitute the inferred fact, and
produce byte-identical generated output. Behavior tests can then gate batches
of equivalent removals. A new target, missing target, semantic conflict, or
generated-code difference still requires a synthetic compiler fixture and the
normal replay/hash/performance validation.

The current pass deliberately starts with configured function entries and
their entry M/X states, then runs an in-memory fixed point over ordinary direct
call targets. Direct edges propagate their exact finite live M/X state instead
of speculatively decoding all four width combinations. Open control-flow facts
whose opcode byte lies inside another decoded instruction are reported as
`garbage_only`; those facts are excluded from automatic configuration debt and
from the experimental overlay. It recognizes self-delimiting and nested packed ROM tables,
finite immediate stores to selected handler words, branch-selected handler
tables, pushed-address RTS chains, and continuation propagation through
computed handlers. None of those discoveries are written back to cfg, and the
normal production regeneration path remains authored-only. The explicit
experimental mode can consume the closed static subset in an isolated output
directory. Full interprocedural M/X summaries, authored-entry elimination,
runtime evidence imports, persistent evidence, and mapper generalization remain
later milestones. The report identifies those gaps instead of treating
speculation as proof.

## Unresolved stream-interpreter triage

An unresolved indirect edge is not automatically reachable or important. The
shadow report nevertheless prioritizes a common sign-tagged stream-interpreter
shape because leaving it unresolved can wedge the interpreter before its stream
pointer advances:

```text
LDY <variable stream pointer>
LDA $0000,Y
BPL <ordinary data path>
...
STA <direct-page target slot>
JSR <trampoline>
...
<trampoline>: JMP (<direct-page target slot>)
```

On a LoROM bank, a negative word can encode a same-bank address at or above
`$8000`. The analyzer reports this as
`tagged_stream_handler_dispatch` with priority `likely_bringup_blocker` and
records the PCs for the pointer load, stream-word load, sign test, target-slot
store, and trampoline call. This is pattern evidence, not proof that every
negative word is executable or that the target set is finite.

As a fast first pass, the report also scans for stores to the identified stream
pointer and suggests the byte following the preceding RTS/RTL as a structural
handler candidate. This deliberately remains report-only. It under-counts when
a handler advances through shared code, and two handlers sharing one advance
site can collapse to one candidate. Raw opcode-like bytes in data can also
produce false candidates. Confirm entries and completeness with the runtime
dispatch census rather than feeding this list directly into generation.

## Dispatch inventory and HLE coverage

Shadow report schema v18 adds `dispatch_sites` and `dispatch_summary`. This
inventory is separate from `unresolved_sites` and its raw/unique emission
counts. It includes static dispatch comparisons, unresolved decoded sites,
every authored `hle_dispatch` hook, indexed-memory-fed RTS sites, and source
sites present only in an imported dispatch census. It never changes decoding,
entry discovery, generated code,
proven-fact selection, or the production unresolved-site gate. Exported database
facts stay unchanged; their `shadow_report_version` provenance advances to 18.
Existing v17 databases remain accepted with all ROM and proof checks intact.

Each record separates:

- `routing`: `compiler`, `hle` (with `hle_function`), or `unknown` for an
  observed-only source. HLE hooks remain owned by the project; no generated
  dispatch replaces them.
- `static_status`: the existing authored/inferred comparison category,
  `unresolved`, or `not_analyzed`.
- `target_set_status`: `unproven`, `open`, `authored_closed`,
  `statically_closed`, or `conflict`. A declared closed set is an override,
  not independent proof. None of these statuses certifies generated-body
  coverage or that a project helper delegates all targets to the registry.
- `runtime_status` and `runtime_observations`: exact observed targets, M/X/E,
  mirror and continuation flags, hit counts, generated-body availability,
  and trapped-before-dispatch status **in the captured build**. Successful
  observations never close an unproven target set. These fields are empty
  until a census is imported; zero observations is not evidence of safety.

A decoded hook records its instruction bytes and deduplicated containing
function/M/X contexts. `decoded_occurrences` counts shadow decode contexts,
not executed blocks or emitted traps. A hook without an analyzed root remains
`authored_only_not_decoded`: it gets no invented width, instruction decode, or
reachability claim. An `observed_only` site retains its runtime evidence but
does not become a static root. Site joins use the exact 24-bit source PC;
mirrored target identities are preserved rather than guessed equivalent.

The compact text report displays HLE hooks and observed missing/trapped sites;
`--verbose` includes every inventory site, containing function/M/X context,
and all observations. Imported continuations with no registry body are not
counted as missing functions. `--bank` scopes imported observations by exact
source bank. Existing unresolved counts continue to describe shadow decoding
with authored dispatch declarations withheld, not this wider inventory.

**Zero classified bring-up blockers is not a coverage certificate.** In
particular, routing an indirect jump through HLE does not prove that its
helper has every handler it needs. A source reported as HLE-routed with an
unproven target set still merits table/provenance analysis or runtime census.

### Caller-to-trampoline pointer provenance

The report-only `pointer_producers` field connects a decoded near `JSR` to a
one-instruction `JMP (abs)` trampoline, including HLE-routed trampolines. A
bounded predecessor-graph query finds a word-sized direct-page store and
follows its A/X/Y value through register transfers to an indexed absolute or
long table load. This is a def-use query over decoded control flow, not a raw
backward byte scan or an address-based match across unrelated functions.

Each finding records the containing entry, call PC and M/X, load/store PCs,
index register and (when available) index-value origin, pointer-slot address,
and conditional ROM base candidates. `dispatch_summary.pointer_producer_sites`
counts sites with findings; `--verbose` prints the chains and outstanding
`proof_obligations`. These findings do not create entry roots, handler bodies,
table spans, configuration edits, or proven database facts. Project HLE remains
in control; the original ROM chain does not prove a helper's implementation.

Direct-page stores address bank zero at `D + operand`, whereas `JMP (abs)`
reads an absolute bank-zero slot. Equal operands therefore require D=0, not
merely matching text. Local constant D evidence can establish a nonzero alias
or reject a mismatch; unknown D yields a conditional match only for equal
operands. DB evidence recognizes local `PEA/PLB` and `PHK/PLB` definitions (or
the explicit bank of a long load). Candidate definitions are reported alongside
`unknown_paths`: a backedge crossing a call prevents treating an initial DB
assignment as an all-path constant. Candidate bases use the current LoROM
reader, exclude WRAM banks, and are not mapper-independent code ownership.

Remaining obligations explicitly include the index domain/stride, table extent
and ownership, handler entry kind/live M/X, and preservation of the pointer
across the call stack and interrupts. A counter that bounds object iteration
does **not** bound an index read from the object. Syntactically valid table
words or a pointer just beyond a candidate span do not close a target set.

This first query is deliberately bounded to 32 predecessor steps per walk and
one near-call hop. Ambiguous producer joins, clobbers, unknown writes, byte
stores, unsupported stack shuffles, and overlapping authored data stop recovery.
Far calls, multi-hop trampolines, absolute/indexed pointer stores, split tables,
and interprocedural D/DB-preservation summaries remain future work. A missing
finding means this bounded query did not recover the relationship, not that a
site has no table-derived targets.

#### Index writers, script-pointer sources, and conditional ROM samples

When a pointer producer's index comes from a word load, `index_evidence`
records the origin-to-index path and writers to the same address expression.
This includes scalar direct-page/absolute/long slots and indexed fields.
Unlike the indexed RTS writer query below, this writer index includes **all
decoded program banks**: initialization may be in a different bank from the
script interpreter. `writer_match` explicitly says
`same_address_expression_across_program_banks`. This is not proof of a shared
allocation, D/DB/index identity, or a reaching definition. Only writers in the
existing decoded closure are included; unseen code and differently spelled
aliases can still write the field.

Writer records retain PCs, M/X, width, source loads, symbolic expressions,
partial/RMW writes, and unknown values. Literal word writers supply sorted
`literal_value_candidates` with writer PCs. An immediate index load instead
has `writer_match=literal_index_load`. Neither case establishes a complete
dispatch domain. Byte/truncated index transfers cannot provide word samples.

For each candidate index and locally encountered DB definition, the report
can sample one ROM word at the **effective** address `base + index`. Thus a
script load `$0000,Y` can be sampled if a candidate Y reaches mapped ROM; a
non-ROM unindexed base is not by itself a rejection. Unknown DB paths remain
visible and do not become all-path constants. WRAM banks, unmapped addresses,
and reads/index additions crossing a bank boundary are not sampled. This
uses the current ROM mapper and is not a claim about all cartridge mappings.

The bounded local path evaluator tracks load/transfer N/Z, immediate word
comparisons, BIT, CLC/SEC/CLV, and decoded conditional-branch direction. It
filters zero indices skipped by BEQ, nonnegative stream data skipped by BPL,
and `$FFFF` terminators excluded by CMP/BEQ. Unknown flags or paths remain
unknown, not accepted guards. A comparison on an unrelated object-loop
counter cannot bound the table index. Paths are limited to the existing
32-step unique-predecessor walks, not speculative linear decoding.

`rom_read_samples` includes the index, DB, effective read PC, word, and status
(`word_passes_local_guards`, `word_fails_local_guards`,
`word_unknown_local_guard`, corresponding `index_*` rejections,
`not_rom_mapped`, or `bank_boundary_not_sampled`). A passing word receives a
`conditional_target_pc` in the trampoline's program bank, plus **address-only**
authored overlap and ROM-mapping annotations. These annotations do not prove
an instruction boundary, code ownership, handler entry kind/M/X, reachability,
or compatibility with an HLE hook. Even an unconfigured, ROM-mapped candidate
is not a newly discovered valid function. Samples are capped at 512 per
producer; `samples_truncated` reports the cap rather than implying completeness.

This is explicitly report-only: conditional index substitutions never enter
the proven database, target-set classification, code roots, or generation.
They do not reduce unresolved counts or authorize removing authored funcs,
continuations, or HLE definitions. See
[the cross-game validation](POINTER_INDEX_VALIDATION.md) for reproduced
handler-address overlap and unresolved source relationships.

#### Initializer tables and local index bit constraints

`index_evidence.table_initializers` follows decoded word writers back to their
indexed table loads, preserving the writer PC, decode contexts, source load
M/X, and any writer addend. It includes one additional pointer-source hop for
`LDA (dp),Y` when a nearby word STA to the same DP operand comes from another
indexed load. Intervening writes, D changes, unsupported stack effects, and
callees stop that hop. Equal operands still do not prove D/wrapping/alias
behavior; the nested source is conditional and its sampled words are never
substituted into the outer indirect read.

The initializer's index query follows a word through transfers, immediate
AND/ORA/EOR, and accumulator ASL/LSR on bounded unique-predecessor paths. It
reports the origin plus operations and `known_zero_bits`/`known_one_bits`.
The resulting `local_domain_size` counts a **local overapproximation**, not
table entries or reachable states. For example AND #3 followed by ASL yields
the superset `{0,2,4,6}`; an unbounded word followed only by ASL yields 32,768
possible even words, not a small table bound. Unknown inputs, calls, joins,
truncation, and unsupported arithmetic cannot manufacture an origin constant.
Later masks can still constrain an otherwise unknown word. Branch predicates
are not used to narrow this initializer domain, and constraints on unrelated
registers are not applied to it.

Domains of at most 256 values are enumerated. Other domains retain their bit
constraints without a full enumeration. Identically spelled source-field
writers across decoded program banks are listed separately as
`conditional_source_writers`; literal words from those writers can supply
hypothetical indices after the recorded operations. These are not reaching
definitions or a complete input domain. Scratch slots may match hundreds of
unrelated writers, so a larger writer list does not imply stronger evidence.

Initializer bank evidence uses a separate 256-step unique-predecessor query.
It recognizes PHK, PEA, and literal PHA sources of PLB, including two consecutive
PLB pulls from a word PHA/PEA. Byte PHA cannot supply two bytes. Calls, joins,
unsupported bank definitions, and exhausted budgets leave the bank unknown.
This longer query does not change existing pointer-producer or code-generation
status analysis.

`rom_word_samples` records the effective read address, word, and index evidence
(`local_word_domain_superset` or `conditional_writer_value`). It excludes
unmapped/WRAM addresses and bank-boundary crossings and caps output at 512
samples per read. A word is labeled `conditional_rom_word_not_a_root`: it is
not automatically a stream start, a nested table pointer, or a handler. The
table's extent, data ownership, incoming state domain, aliases, and reachability
remain explicit proof obligations. No samples are fed back into consumer
index candidates, dispatch target sets, code roots, HLE policy, or the proven
database. See [initializer validation](TABLE_INITIALIZER_VALIDATION.md).

#### Local scratch-slot writers and direct-caller inputs

An initializer index loaded from a scalar DP/absolute/long slot may now carry
`local_slot_source`. This is the nearest **same-spelling candidate writer** on
a bounded, unique decoded predecessor path, not a proven reaching definition.
It records the entry PC/M/X, store and load PCs, stored register expression,
and every intervening explicit memory write as `possible_alias_writes`.
Different operands or addressing modes are not assumed disjoint. Indexed
destinations are excluded from this scalar query. A nearer partial, zero, or
read-modify-write overwrite blocks recovery of an older register writer;
calls, D/DB changes, unsupported stack effects, joins, and the 128-step budget
also stop it. An empty alias-write list is not a memory-lifetime proof.

The stored-value query can follow A/X/Y back through register transfers and
passive stack/bank operations to `entry_register`. It stops at callee effects,
pulls into registers, status restoration, truncation, unsupported arithmetic,
joins/backedges, or its 32-step budget. This is a separate argument query:
the existing initializer index domains are unchanged.

When the candidate receives an entry register, `direct_caller_inputs` lists
decoded direct JSR/JSL callers to that exact PC, with instruction bytes,
containing entry PC/M/X, call-site M/X, argument register, origin, and local
bit-domain superset. JSR retains its program bank; JSL supplies its explicit
24-bit target. Indirect or collapsed dispatches are not invented as direct
callers, and mirrored PCs are not silently combined. Exact duplicate records
are removed; differing caller contexts and M/X states remain separate, with
`entry_mx_matches` recording width compatibility rather than choosing a mode.

`call_scope=decoded_direct_callers_only_not_all_entries` is intentional. A few
masked callers cannot bound another unbounded caller, an indirect entry, or an
HLE implementation. Branch predicates do not narrow these argument domains.
Even matching widths do not prove a calling contract or reachability. The
report preserves the global scratch-slot writer inventory alongside this
local shortlist, and substitutes **neither** caller values into the slot nor
slot values into table reads. No samples, target sets, database facts, roots,
authored configuration, or HLE obligations change. See
[call-input validation](CALL_INPUT_VALIDATION.md).

Each direct caller can additionally carry `source_evidence`: one bounded
expansion of the argument's originating memory load. Indexed loads expose
`table_read`, including their own index origin/operations/domain, bank evidence,
exact-spelling index-field writer candidates, and conditional ROM word samples.
Scalar loads expose candidate writers of their exact address expression and,
when recoverable, a local scalar-slot candidate. The caller's **table index**,
the loaded/masked **argument**, and the callee's **initializer index** are three
different values; the report never substitutes or merges their domains.

`expansion_scope=one_caller_source_hop_no_recursive_expansion` bounds the query.
It does not follow another caller, expand a nested indirect pointer, or turn
writer candidates into additional code roots. Bank state is not presumed to
equal the caller's program bank, even when a nearby ROM table looks plausible.
Unknown DB therefore prevents ROM sampling. Word reads preserve both bytes;
an argument's later mask does not change what the table read actually fetched.
Existing sample caps and bank-crossing exclusions apply. Exact expression
matching is not an alias proof or complete writer census, and arithmetic or
unknown writers are retained alongside literals. This additive evidence does
not narrow prior reports, samples, facts, generated code, or HLE policy. See
[caller-source validation](CALLER_SOURCE_VALIDATION.md).

#### Bank preservation across calls (report-only)

Caller-table `source_evidence.bank_context` records a bounded bank-state query
at the exact load PC/M/X and at the directly decoded callers of its containing
entry. These are separate calling contexts, not a global entry-bank proof.
The old `data_bank`, table samples, input domains, and proven database remain
unchanged. A numeric `constant_banks` array is emitted only when every state
reaching the queried interpretation has a modeled constant DB; mixed entry-DB
or unknown states do not become a closed bank set.

The native decoded-width model tracks symbolic entry DB, PHB/PHK/PLB, local
byte pushes/pulls, PHP/PLP width restoration, and REP/SEP. Summaries describe
DB preservation **on decoded normal returns**, under the normal call-frame
and stack-lifetime contract. They are not reachability, emulation-mode,
interrupt-preservation, or code-ownership proofs. Explicit memory writes
conservatively poison saved DB/status bytes, including writes in callees;
different operands are not presumed disjoint. Unsupported stack/interrupt
effects, block moves, opaque tails, unbalanced frames, return-kind mismatches,
missing exit-width interpretations, and HLE hooks block the query. No summary
chooses a canonical M/X variant or supplies an HLE contract.

Direct JSR/JSL calls use exact target PCs and widths. JSR (abs,X) may use a
finite local **superset** of word-sized X values to enumerate immutable ROM
pointer reads. Every possible read must be ROM-mapped and non-boundary-crossing;
the decoder's heuristic table prefix is never treated as complete. Each target
needs its own valid return/DB summary. `target_superset_closed` therefore does
not mean that all listed words are reachable handlers, owned code, or missing
configuration obligations. In particular, an overly broad mask can produce
many irrelevant targets and prevent a preservation proof.

Only existing exact entry variants are re-decoded on demand; no new roots are
created. Work is bounded to 256 programs, 32 caller records per queried entry,
32 monotone summary passes, 4,096 states per evaluation, and 64 local/nested
stack bytes. The least fixed point leaves unresolved recursive groups unknown
rather than assuming their own preservation. Call diagnostics retain at most
eight target blockers and explicitly mark truncation. Queries consider only
decoded paths that can reach their exact PC/M/X, so unrelated exit branches
do not inflate a table load's debt. External/indirect entries, non-local
returns, and normal-frame assumptions remain explicit obligations. See
[bank-summary validation](BANK_SUMMARY_VALIDATION.md).

### Return-frame audit (report-only)

`return_frame_audit` inventories RTS/RTL/RTI sites in the existing shadow
decode closure. It checks a property that a return opcode alone cannot supply:
which part of the guest stack its return bytes occupy, relative to an assumed
normal entry frame. This does not change `AnalyzeExitMX`, regeneration,
dispatch facts, entry roots, authored configuration, or HLE behavior.

The native-width worklist retains finite sets of local stack depths, following
the existing CFG and its exact M/X interpretations. Positive depth means bytes
pushed below entry S; negative depth means bytes have been pulled past entry S.
RTS consumes two bytes and RTL three. Shapes distinguish:

- `entry_frame_position_only`: depth zero, without a modeled explicit overwrite
  of the relevant entry slots. **Not** a proof that the return PC or M/X is intact.
- `locally_pushed_frame`: all return bytes occupy newly pushed stack space.
- `mixed_local_and_entry_frame`: the return spans local and entry-frame bytes.
- `past_entry_frame`: the stack has already advanced beyond entry S.
- `entry_frame_written`: pushes after pulling entry bytes, or direct
  stack-relative writes, have overwritten entry return slots. The written value
  may equal the old value; height alone cannot establish that.
- `unknown_stack_position`: stack resets, HLE hooks, collapsed constructs,
  unsupported control effects, or depth limits prevent a position claim.
- Interrupt returns and recognized RTS dispatches have separate classifications;
  neither is treated as a new ordinary-call return proof.

The audit intentionally does not guess callee behavior. A lexical post-call
path uses a **conditional** zero net depth and carries
`callee_normal_return_and_stack_effects`; a callee may invalidate that entire
path or its frame shape. Explicit memory writes outside an exact stack-relative
slot carry an alias obligation. Saved-return values, PLP contents, interrupts,
emulation state, and the entry kind remain unproven. HLE contracts are never
inherited from their replaced ROM bodies. A copied/restored stack pointer is
still unknown until value provenance and alias safety establish the restoration.

`legacy_local_exit_candidate` records only whether the old opcode-based local
exit collector admits that site in this shadow graph. It does not mean that
production published an exit fact, that the fact changed M/X, or that a defect
executes. Different authored boundaries and entry kinds can give one PC several
legitimate frame shapes. Decodable, unobserved, or speculative paths are not
asserted reachable or discarded as garbage by this audit.

Counts distinguish raw entry/return/M/X contexts, source PCs, and source-M/X
sites. Shape counts overlap when contexts disagree; HLE-affected sites are
reported separately. These counts are not added to unresolved runtime debt.
JSON retains all contexts; verbose text lists non-entry/unknown shapes and
incomplete audits. Work is bounded to 4,096 states per graph and depths within
±64 bytes. Depth overflow becomes unknown, never a clamped valid frame; state
exhaustion and external CFG edges retain explicit incompleteness.

This is the prerequisite audit for future stack-qualified M/X exit sets and
inline-call-argument discovery, not an automatic cfg-removal gate. See
[return-frame validation](RETURN_FRAME_VALIDATION.md).

### Frame lifetime at calls and jumps (report-only)

`return_frame_lifetimes` (shadow report version 19) checks calls and jumps
after local pulls or exact stack-relative stores may have exposed or replaced
incoming return bytes. An early RTS does not describe every path: another
branch can consume the incoming frame and then make more calls or transition
elsewhere without returning to it. This is relevant to generated caller-frame
ownership limits, not just to finding missing compiled targets.

This bounded, path-sensitive audit tracks individual incoming byte identities
through A/B, X, Y, D, DB, stack saves, register transfers, and exact native
stack-relative loads/stores. `PLA; PHA` restores the original word; `PLA; LDA
#value; PHA` only restores height. PHP/PLP retain known saved M/X; unknown PLP
values and conflicting decoded widths stop that path. A nested call invalidates
register identities and inactive stack scratch, including its own pushed
return frame. Active stack bytes survive a call only under an explicit,
**unproven stack-neutral normal-return contract**. Potentially aliasing writes
also require an explicit assumption. These assumptions let the inventory
describe local shapes beyond unknown effects without promoting them to proofs.

Each review boundary includes source bytes, live decoded M/X, direct target
(when available), lexical continuation for calls, entry-relative S, the sites
that pulled/wrote entry bytes, and byte masks. Mask bit 0 denotes entry S+1,
bit 1 entry S+2, and bit 2 the additional native JSL bank byte:

- `unprotected_entry_byte_mask` marks touched entry slots whose original
  identity/active-stack position is not established at the boundary. Unknown
  values might still equal the original bytes; this is not a corruption proof.
- `modeled_saved_copy_byte_mask` records copies still in tracked registers or
  active stack storage, not inactive memory left behind by a pop. An extracted
  frame with saved copies is not classified as permanently consumed.
- `S_minus_entry_S` uses the opposite sign to the older depth audit: positive
  means S has advanced above entry S. A negative value at a call does not prove
  that the original frame still exists; later arguments can cover its slots.

RTS-only and RTL-only graphs select two-byte and three-byte **hypotheses**;
mixed or tail-only graphs retain both. A configured `func` or return opcode
does not prove a normal-call entry or that `_entry_s` owns that frame. Graphs
with RTI retain an explicit unsupported interrupt-frame contract. HLE hooks
(including conditional/dispatch/SPC hooks), collapsed dispatches, stack resets,
unsupported control effects, missing graph edges, and unknown status restores
remain blockers. The audit never inherits a replaced ROM body's HLE behavior.

Counts distinguish entry/frame contracts, boundary contexts, unique source
PCs, and source-M/X sites. Incomplete contracts and exhausted budgets are
reported separately; no review sites does not imply completeness or a safe
game. Selection requires an existing decoded pull/stack-relative store and a
call/jump. No new bytes are decoded. Analysis is bounded to 4,096 states per
entry/frame hypothesis and S within ±64 bytes; blockers are capped at eight
displayed reasons per hypothesis with an explicit omitted count. Verbose text
uses `[FRAME-LIFETIME]` and `[FRAME-LIFETIME-BLOCKED]`; JSON retains all review
boundary contexts. Nothing is added to unresolved runtime failure counts.

These findings cannot authorize changing caller limits, treating an internal
continuation as a new function, adding roots, or skipping instructions. The
next proof obligations are callee cleanup, possible saved/relocated frames,
entry kind, aliases, and ancestor ownership. Existing production fact payloads
and generation remain unchanged; a newly exported database records report
version 19 as provenance only. See
[frame-lifetime validation](FRAME_LIFETIME_VALIDATION.md).

A separate, behavior-affecting emitter contract now recognizes a narrow
single-block return-word relocation and checks the word's immediate-call
origin at runtime. It does **not** consume these conditional findings or
relax caller limits. See [return-word relocation validation](RETURN_WORD_RELOCATION_VALIDATION.md)
for the proof boundaries, synthetic tests, and game validation.

### Return-address value provenance (report-only)

`return_address_provenance` adds bounded symbolic execution after the frame
inventory. It selects existing decoded graphs with a normal return and stack
pulls, stack-relative operands, or address pushes; this is a focused inventory,
not a census of all callable routines. No new bytes are decoded or skipped.

The native entry contract is explicit: incoming stack bytes represent a JSR
return PC or a JSL return PC plus its bank. The engine follows the identities
of individual bytes through A/B, X, Y, pushes, pulls, transfers, and exact
stack-relative loads/stores. A word can denote the original stacked PC plus
a constant modulo 65,536. Partial overwrites and X-width truncation cannot
silently preserve a whole-word identity. Local saved registers are not confused
with the incoming PC merely because both occupy stack slots. RTL additionally
requires preservation of the incoming bank; a graph mixing RTS and RTL retains
an entry-frame blocker.

The bounded worklist follows all existing decoded successors. PHP/PLP save and
restore tracked M/X, carry, and decimal state, with a check against successor
decode widths. Entry carry and decimal mode are unknown. ADC/SBC of the return
word requires locally established binary mode and carry; a nearby CLC alone
does not suffice. Word INC/DEC and INX/DEX/INY/DEY do not require binary mode.
Unknown calls, potentially aliasing non-stack stores/RMW/block moves, stack
resets, unsupported effects, collapsed dispatches, and HLE hooks stop the path.
No ROM-derived contract is inherited by an HLE replacement.

The statuses are deliberately conditional:

- `conditional_incoming_PC_preserved`: every modeled return restores the entry
  frame and the unchanged incoming PC (and bank for JSL).
- `conditional_constant_adjustment`: every modeled return instead uses the
  same incoming-PC addend. It is **not** yet a count of inline argument bytes.
- `path_dependent_adjustment`: otherwise-closed paths have different addends;
  there is no uniform skip count.
- `unproven`: at least one path has an unsupported effect, external edge,
  incomplete budget, wrong frame position, or unknown returned value. Any
  retained addends describe only the individually qualified paths, not a
  complete contract.

Entry kind, native mode, and entry M/X remain assumptions. The modeled stack
window must be writable, nonaliasing memory rather than hardware or ROM, and
interrupts or hardware writes must preserve the guest frame. These are properties of all
modeled returns, **not** proofs that a routine terminates or executes. A constant
addend alone does not establish that caller bytes are inline data: call-site
ownership, continuation and bank wrapping, overlapping entries, and all incoming
references still need checking. Neither suspicious instructions nor absent
contracts establish dead code. Counts stay separate from unresolved runtime debt.

The engine is capped at 512 states, a ±32-byte local SP window with room for
the incoming frame, and eight reported blocker sites per entry (with an omitted
count). Budgets become explicit blockers. JSON entries are sorted and deduplicated
by entry/M/X; normal and experimental generation, exit-M/X summaries, dispatch
facts, cfg files, and HLE definitions are unchanged. See
[return-address validation](RETURN_PROVENANCE_VALIDATION.md).

### Context-specific direct-call return contracts (report-only)

`return_call_contracts` follows the direct JSR/JSL blockers in the local
return-address inventory. It performs bounded abstract execution across
already-known exact entry/M/X variants. It does not discover entries, change
decoding boundaries, or publish a general callee summary. The previous local
`return_address_provenance` results remain available unchanged for comparison.

Each call pushes its actual native return bytes into the same symbolic guest
stack used by its caller. JSL also pushes the program bank. Registers, saved
status bytes, carry, and decimal state flow through the inspected callee body;
they are neither assumed preserved nor indiscriminately discarded. In
particular, PHP/PLP can restore X width without restoring the high bytes of X/Y
that SEP cleared. A callee may also write into its caller's frame via a
stack-relative operand even when the callee's own return address is intact.

The caller resumes only after the callee reaches the matching RTS/RTL frame
position with the original call PC (and original bank for JSL). Adjusted,
unknown, constructed, or non-local returns remain blockers rather than
ordinary fallthrough. The return PC wraps within its program bank. Each
resulting live M/X state must have a corresponding existing decoded successor
at the true continuation PC; no canonical width, invented continuation, or
newly decoded variant is substituted. All modeled callee branches must qualify
for the entire root query to close.

`calls` lists exact targets, caller entries, continuation PCs, and individually
matched frame-return sites/M/X states. `entered_exact_variant` means only that
the query entered that decoded body. Neither that status nor an individually
matched return proves that every callee path succeeds. The enclosing root's
status and blockers remain authoritative. A callee that hard-codes one caller's
return address can qualify in that specific context and fail in another; it
never acquires an address-independent preservation summary.

Programs are loaded lazily from the existing shadow closure, using the same
end limits, data regions, exit-M/X inputs, HLE dispatch declarations, and sibling
boundaries. Failed sibling decodes still retain their boundary. These are the
decoder's existing boundaries: sibling jump edges are bounded while ordinary
fallthrough remains permitted. Missing exact variants, including differing M/X
or bank aliases, stay unproven rather than triggering canonical substitution or
extra discovery. HLE entry/site/conditional/upload hooks and collapsed dispatches
remain barriers; non-stack stores still require a separate alias proof.

Queries share a 512-state budget across their inspected call tree, retain the
±32-byte SP window, and allow at most eight nested abstract call frames. A
recursive group cannot bootstrap its own correctness from a base-case return.
The report selects at most 256 root variants and loads at most 512 exact
programs, deterministically ordered. Root/program omissions and depth/state
exhaustion are explicit. At most 64 call checks per root are displayed, with an
omitted count; that display limit does not stop analysis. Local blocker display
truncation does not hide the fact that a root encountered a call.

Native entry/frame/M/X and writable, nonaliasing stack-memory assumptions still
apply, as does preservation of the frame by interrupts and hardware. A repeated
abstract state is not a termination proof. No inline-data skip count, production
exit-M/X fact, authored cfg edit, HLE change, or runtime fallback results from
this query. Its counts are not unresolved runtime debt or reachability claims.
See [direct-call validation](RETURN_CALL_VALIDATION.md).

### Return-stack write alias contracts (report-only)

`return_stack_aliases` revisits the preceding return-call queries that stopped
at a potentially aliasing memory write. It is a separate query with a separate
program cache/budget: the existing local and call-contract answers stay intact.
It does not select every store-containing routine or change code ownership.

The first exclusion is deliberately narrow. Under the standard SNES system
bus, ordinary stores entirely within `$7E:2000–$7F:FFFF` cannot overwrite
bank-zero stack memory. The 65816 stack is in bank zero; only the first 8 KiB
of WRAM has a bank-zero mirror. This is a system-bus property, not a LoROM
code-address heuristic or an assumption that a game's S is on page one.
See the [WDC datasheet, stack and addressing sections](https://www.westerndesigncenter.com/wdc/documentation/w65c816s.pdf)
and the [SNES memory map](https://wiki.superfamicom.org/memory-mapping).
Special hardware that changes these system-bus properties would require a
different contract; this report is not a general cartridge-mapper proof.

The query models ordinary STA/STX/STY/STZ stores with long operands, or
absolute operands whose DB is known from actual byte-valued stack operations.
PHK pushes the active program bank, PHB saves the current tracked DB, and PLB
uses the byte actually pulled. DB starts unknown and flows through inspected
calls rather than being assumed equal to PB or preserved by every callee.

For indexed stores, a known register value supplies a singleton range.
Partial known-bit values supply a conservative minimum/maximum; otherwise
the live X flag bounds the full index domain to 0–255 or 0–65535.
The footprint includes every possible destination byte, with the store width
selected by M or X as appropriate. Bank carry and the second byte count:
a word store at `$7E:FFFF` stays in WRAM, but one at `$7F:FFFF` reaches the
bank-$80 low-WRAM mirror and is rejected. Overflowing sums are not masked
into an apparently safe interval. This pass does not infer bounds from a
nearby comparison or substitute a likely index value.

Within this query, immediate AND/ORA/EOR and accumulator ASL/LSR/ROL/ROR
preserve known bits instead of discarding the whole value. For example,
`AND #$00FF; ASL; ASL; TAX` gives X a 0–1020 range in 16-bit mode, even when
the original A is unknown. Shifts of constants remain constants. Rotation
uses the actual tracked incoming carry; a shifted-out bit establishes the
new carry only if that bit is known. Logical operations preserve carry.
The ordinary register-transfer and byte-stack model carries these bounds
through calls, pushes/pulls, and XBA. An 8-bit accumulator operation still
preserves the hidden B byte; it does not zero-extend A into a wide X/Y.
Conversely, narrowing X zeroes its high byte even after a later PLP restores
16-bit index width. See the
[WDC width/transfer caveats](https://www.westerndesigncenter.com/wdc/documentation/w65c816s.pdf).

No memory read is converted into a guessed constant. Memory-operand logical
operations remain conservative, incoming-PC/status symbols are not interpreted
as numeric bit masks, and these bounds do not prune branches or prove loop
termination. Only the existing report-only stack-alias query uses this domain;
local return provenance, the earlier call audit, and production analysis do not.

Low-WRAM mirrors, direct-page and indirect operands, unknown DB, hardware
registers, cartridge writes, RMW instructions, and block moves remain
barriers. In particular, `$2180` and `$420B` are not harmless non-stack
addresses: WRAM-port writes or DMA can overwrite the guest frame. A proven
disjoint store does not supply values to subsequent memory loads; those
remain unknown. Exact stack-relative writes still update the real symbolic
frame, including corruption of a parent frame. HLE hooks and other unsupported
control effects remain barriers even after a disjoint store.

Each root retains its call checks and adds `write_footprints`, with instruction
bytes, live M/X, known DB, index domain, inclusive unwrapped byte range, and
a proof or blocking reason. `index_known_bits`, when present, gives a mask and
value within the live index width: `(index & mask) == value`. The accompanying
range covers all represented values, including holes due to alignment; its
endpoints are not samples. Footprints are deduplicated context observations,
not execution counts or universal store-site summaries. `write_footprint_status_counts`
and `sites_with_disjoint_write_context` are computed before the 64-footprint
display limit; `write_footprints_omitted` makes truncation explicit. A site
may have a disjoint context and an unproven one. Only the complete enclosing
query can establish a conditional return contract.

The existing native/writable-stack, interrupt preservation, exact-variant,
depth/state/root/program limits, and non-termination caveats still apply.
Memory barriers suppressed by this exclusion do not become production facts,
new roots, inline-data skips, cfg edits, or HLE replacements. See
[stack-alias validation](RETURN_ALIAS_VALIDATION.md).
For the mask/shift extension and its three-game comparison, see
[known-bit validation](RETURN_BITS_VALIDATION.md).

### Indexed-memory RTS targets and writer candidates

The report-only `stored_target_flows` query connects a decoded indexed word
load to `PHA`/`PHX`/`PHY` followed by `RTS`. It follows bounded, unique decoded
predecessors, register transfers, and word-sized `INC`/`DEC` adjustments. Each
flow records the load and push PCs, the live M/X at RTS, and the net addend
including RTS's increment: loading an address, decrementing, and pushing it
has net addend zero. The destination stays in the RTS instruction's program
bank, with 16-bit wrapping. This does not assume that a low destination address
is invalid code; mapping and ownership are reported separately.

Writers are collected from the existing decoded closure, using the same
indexed addressing mode and operand in the same program bank. This is an
**address-expression match, not a proven alias or complete writer census**.
Different D, DB, index values, allocations, and lifetimes can make identical
expressions access unrelated memory; different expressions and banks can
also alias. No object-field offset or game-specific record layout is built in.

Literal word stores provide conditional target candidates. A helper whose
first decoded instruction reads `$01,S` or executes word-sized PLA can be
joined to decoded direct JSR callers with matching entry M/X. The saved near
return word is the call PC plus two, before any writer/consumer adjustments.
Only a net adjustment of one is labeled `continuation_candidate`; other
offsets are not called continuations. A stack read after PHP/PHA, or at an
unsupported offset, never becomes caller-return evidence. A continuation
candidate is not a normal callable routine, and no registration is generated.

The report retains partial writes, read/modify/writes, memory-derived values,
unknown values, and context-dependent alternatives. It does not silently
discard them to manufacture a closed set. Unsupported arithmetic (including
ADC/SBC with unproven carry/decimal state), clobbers, entry boundaries, and
ambiguous predecessor joins stop value recovery. Byte and truncated values
cannot supply a word target through this query.

`--verbose` prints writer provenance and target candidates. Summary counts
deduplicate writer PCs and target addresses across owner/M/X contexts;
`stored_target_authored_addresses` measures **address-only** overlap with
authored entries, not entry-kind or M/X equivalence. Candidate ownership
distinguishes decoded boundaries, instruction interiors, conflicting decodes,
authored data, unmapped addresses, and unclaimed ROM. Even an unclaimed
ROM address is not decoded or promoted into a root by this query.

These candidates neither close a dispatch set nor enter the proven-fact
database. Existing unresolved-emission counts are unchanged. Every flow
retains explicit obligations for aliases, all writers/clobbers, entry kind,
live M/X, HLE semantics, and reachability. Authored funcs, dispatch guards,
continuations, and all HLE directives remain intact. Record-base arithmetic,
finite table/index domains, cross-bank aliases, and interprocedural object
lifetimes remain future work.

#### Arithmetic evidence and source-field dependencies

An otherwise unknown word writer can now include a report-only
`value_expression`. It records a load/stack origin, an entry register, a
register after a call, or an unsupported/ambiguous origin, followed by
operations in execution order. Immediate ADC/SBC operations carry separate
`carry_before` and `decimal_before` evidence, including constant-definition PCs
or the reason the status is unknown. CLD/SED, CLC/SEC, and the corresponding
REP/SEP bits are recognized on bounded unique-predecessor paths. Comparisons,
shifts, and previous arithmetic invalidate carry evidence; PLP, RTI, calls,
interrupt instructions, and unsupported status changes are barriers. Entry
decimal mode is never assumed clear from a game-wide convention or M/X state.

`local_word_expression` permits an `exact_addend` only when the source and
all supported adjustments are established locally. ADC adds its immediate
operand plus carry; SBC subtracts its operand plus one minus carry. Both
require decimal mode locally proven clear. An immediate origin can then yield
`constant_word`, with 16-bit wrapping. Unknown/set decimal mode or unknown
carry produces `conditional_arithmetic`; missing register/callee/producer
information produces `unresolved_origin`. Operations and their individual
status evidence remain visible even when the origin is unresolved.

The expression walk can cross a store that does not clobber the tracked
register. A load origin denotes the value captured at its load PC, not a fresh
read of that location at the later store PC. Arbitrary callees, unsupported
operations, truncation, and ambiguous predecessor joins still block a complete
value expression. `register_after_call` does **not** claim that the callee
changed or preserved that register; it names the missing summary.

Flows also include `source_fields`: a two-hop, same-program-bank index of
writer expressions whose loads mention another indexed field expression.
Each field records its depth, consuming store PCs, and writer candidates.
Cycles terminate; `source_fields_truncated` explicitly marks deeper unexpanded
dependencies. This also exposes suspiciously broad expression matches such
as `$0000,Y`, which can refer to unrelated objects or ROM records. The index
does not establish D/DB/index identity, allocation/lifetime, or table bounds.

**No values are substituted through these field links.** Even a locally proven
addend and a literal writer to the same expression do not prove a reaching
definition or dispatch target. The new expression metadata does not alter the
legacy writer `kind`, `stored_value`, or `value_addend`, enumerate new targets,
close target sets, or enter the analysis database. In particular, neither
local arithmetic evidence nor memory-expression matches authorize removal of
funcs, continuations, HLE hooks, or other authored policy.

## Runtime dispatch census

For data-derived targets that static analysis cannot see, trace builds can
collect a bounded, game-agnostic census without a game-layer observer. Configure
the runner with `SNESRECOMP_ENABLE_TRACE=ON` and run the game or replay with:

```sh
SNESRECOMP_TRACE_FILE=saves/dispatch.jsonl \
SNESRECOMP_TRACE_CHANNELS=dispatch \
./build/MyGame game.sfc --frames 2400
```

For a registry-target census, leave `SNESRECOMP_SEMANTIC_DISPATCH_TRACE` at
its default of `0`. Semantic-equivalence builds suppress implementation-level
registry events; in particular, an HLE helper delegating directly to the
registry may then be absent from the census. Use a separate ordinary trace
build to measure that coverage.

Structured `dispatch` records are included in the default channel mask. An
explicit `SNESRECOMP_TRACE_CHANNELS` list can still omit them; in that case
`trace-inspect --diagnose` warns that legacy `dispmiss` rows cannot reliably
distinguish missing bodies from resumable continuations. Recapture with
`dispatch` enabled before treating a zero or nonzero dispatch finding count as
complete.

Use a normal dispatch trace build for this workflow; do not define
`SNESRECOMP_SEMANTIC_DISPATCH_TRACE`. Semantic tracing deliberately normalizes
compiled switches and registry lookups for A/B edge hashes, while the census
needs the raw registry result to distinguish a generated body from a miss.

The runtime groups observations by source PC, target PC, live M/X/E state, and
registry result. It writes the first hit and power-of-two milestones, followed
by a deterministic final count at clean shutdown. A wedged interpreter therefore
leaves useful evidence such as a million-hit target without producing a
million-line trace. Edges emitted by RTS/RTL are classified as continuation or
return guards even when their target has no standalone registry body; they are
excluded from missing-handler debt. Independently of trace builds, a computed
target with no live-M/X registry body prints `[dispatch-missing]` on its first
hit and at base-16 milestones (16, 256, 4096, ...), including site, target,
M/X, stack, frame, and hit count. Set
`SNESRECOMP_NO_DISPATCH_MISSING_WARNING=1` only when an integration has an
intentional, separately-audited recovery policy; the deterministic exit census
remains the complete paste-ready summary.

An unresolved `JMP (abs)`, `JMP (abs,X)`, or `JML [abs]` no longer needs an
authored `hle_dispatch` merely to become visible to the census. Before executing
the existing hard diagnostic, generated code calculates the architectural
target (including the live X register and the 24-bit bank-zero pointer for
`JML [abs]`, plus the bank-zero pointer used by `JMP (abs)`) and emits a record
marked `trapped`. It does not execute the target
or provide an interpreter fallback. A trapped missing target therefore needs
both an appropriately classified generated entry and an explicit route at the
source site until generic target dispatch is enabled.

Summarize it from the game project root with:

```sh
snesbuild dispatch-census --root . --trace saves/dispatch.jsonl \
  --rom game.sfc --out-analysis saves/dispatch-analysis.json
```

Merge that evidence back into the read-only static report with:

```sh
snesbuild analyze --root . --rom game.sfc \
  --dispatch-analysis saves/dispatch-analysis.json
```

The ROM hash must match. The report ranks observed trapped/missing sites first,
then other observed unresolved sites, likely blocking unobserved interpreter
patterns, and remaining unobserved static sites. It preserves exact targets,
M/X, hit counts, generated-body status, continuation classification, and
whether the observation occurred immediately before a hard trap. An overflowed
census remains visibly incomplete.

Relative `--trace`, `--rom`, and `--out-analysis` paths are resolved from
`--root`, not from the shell's working directory. Use absolute paths when the
trace or evidence directory is outside the game project.

## Unified trace inspection

`trace-inspect --diagnose` applies the same continuation contract before it
suggests entry roots. Structured `dispatch` records with
`continuation=1,trapped=0` are counted separately and never become
`missing_dispatch_target` findings. Trace builds also emit an older
`dispmiss` row for each failed registry lookup while structured rows are
sampled at milestones. When a structured continuation classifies the same
source, target, and M/X identity—and no structured actionable miss contradicts
it—the matching legacy rows are suppressed as duplicate unwind evidence.
Standalone or contradicted `dispmiss` records remain actionable.

The runtime `hits` field is cumulative. Diagnostics use its maximum/terminal
value per `(source, target, M, X)` identity rather than summing milestone
snapshots. Legacy rows without a cumulative counter are counted as rows. The
source PC remains part of the identity so distinct dispatch sites selecting
the same target are not merged into one misleading finding.

Trace-inspection report schema v3 exposes capture-quality notices in
`warnings`. Text output prints them beside the summary. `truncated_events`
counts only raw events selected for display; summary/diagnose-only reports no
longer claim that hidden source events were omitted from an empty findings
list.

The text report identifies missing generated bodies and prints candidate `func`
lines with the observed entry M/X. Those lines remain suggestions: before
authoring one, classify the target as a routine, computed handler, tail target,
or continuation. Registering a continuation as an ordinary routine can change
host-stack semantics. The optional JSON output contains the ROM hash, trace
hash, provenance, and observation counts; authored cfg is never modified.
The report preserves whether an observation was trapped before dispatch, so
such evidence cannot be mistaken for an executed handler edge.

## APU sample and port audit

Set `SNESRECOMP_APU_AUDIT_PREFIX` before runner initialization to enable the
otherwise-inert audio evidence recorder and byte-level ARAM write bitmap. A
clean teardown, or an explicit `RtlCaptureApuAudit`, produces four files:

- `.aram`: the coherent final 64 KiB ARAM image;
- `.dsp`: the 128-byte visible DSP register image;
- `.written`: one bit per ARAM byte written by the SPC, the shared HLE upload
  helpers, or a value-changing upload customization;
- `.audio.jsonl`: canonical chronological DSP writes and CPU/APU port events,
  with host PCM scheduling events omitted, plus overflow metadata and CPU
  source-block/function provenance.

`snesbuild apu-audit --prefix <prefix>` selects evidence only from currently
audible non-noise voices, observed key-ons, or explicit `--sources`. It never
interprets stale SRCN registers from a silent snapshot as a successful test.
For each source it reads the live DIR entry, follows nine-byte blocks with the
DSP's wrapping 16-bit ARAM semantics, requires an end block before the address
sequence cycles, validates an enabled loop as a visited block address, and
checks every consumed byte against the write bitmap. A later directory entry
does not terminate the walk: SNES sound drivers may deliberately share BRR
suffixes. With no bitmap, a structurally sound sample is reported as
inconclusive rather than fully valid.

The same report reconstructs CPU-to-APU handshakes. A different applied value
before an SPC read produces `PORT-OVERWRITE`, including the replacement's
source block, function, frame range, and hit count. Same-value rewrites are
counted separately because they can be intentional protocol markers without
proving that a different command was lost. While capture is enabled, the
runtime prints the first and base-16 changed-value hit milestones so a frozen
or collapsed protocol is visible before shutdown. Overwrites are evidence,
not automatically defects: a title may intentionally coalesce port values.

Historical key-ons are compared with the final ARAM snapshot. If a game swaps
sample banks, capture near the failure or take multiple explicit snapshots.
Direct customization writes of the same byte already present in ARAM are not
distinguishable from no write; shared upload helpers provide exact declared
destination coverage and are preferred.
