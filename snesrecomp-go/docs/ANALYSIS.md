# Read-only shadow analysis

`snesbuild analyze` is the first stage of the recompiler's evidence-driven
workflow. It runs static inference without supplying authored
`indirect_dispatch` or `rts_dispatch` declarations to the decoder, normalizes
the independently inferred facts, and only then compares them with the cfg.
It does not generate C, update configuration, or create an analysis database.

From a game project root:

```sh
snesbuild analyze --root . --rom game.sfc --compare-authored --no-write
```

The low-level equivalent is:

```sh
v2regen analyze --rom game.sfc --cfg-dir recomp --jobs 8
```

Both commands are unconditionally read-only. `--dry-run=false` and
`--no-write=false` are rejected by `snesbuild analyze`; `v2regen analyze`
likewise rejects `--no-write=false`. Use `--format json` for the complete,
deterministically ordered machine-readable report and `--verbose` for every
text record. `--strict` reports everything and then returns nonzero only when
independent evidence proves a semantic conflict.

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

## Runtime dispatch census

For data-derived targets that static analysis cannot see, trace builds can
collect a bounded, game-agnostic census without a game-layer observer. Configure
the runner with `SNESRECOMP_ENABLE_TRACE=ON` and run the game or replay with:

```sh
SNESRECOMP_TRACE_FILE=saves/dispatch.jsonl \
SNESRECOMP_TRACE_CHANNELS=dispatch \
./build/MyGame game.sfc --frames 2400
```

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
