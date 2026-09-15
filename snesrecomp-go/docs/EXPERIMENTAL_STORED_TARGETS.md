# Experimental cold-entry discovery and HiROM bring-up

Status: experimental, not a one-shot recompilation guarantee. Use isolated
generated output and copied configuration. Do not remove authored HLE or data
boundaries merely because automatic discovery also mentions an address.

```sh
v2regen regen --rom game.sfc --cfg-dir copied-recomp \
  --out-dir isolated-gen --funcs-out copied-recomp/funcs.h \
  --experimental-stored-targets --allow-stubs
```

The flag inventories literal stack continuations, bounded PEI return words,
stored handler pointers, setter arguments, ROM tables feeding stacked object
callbacks, and conditional native command operands. It does not rewrite the
authored configuration. Ordinary
regeneration does not enable these discovery passes.

Immediate `PEA/PER continuation-1; JMP (abs)` and `JMP (abs,X)` envelopes
have a separate linear continuation inventory. A complex initialization loop
exhausting the bounded stack walk cannot erase these independent references.
The loop-dependent results are still abandoned on overflow; no arbitrary
traversal prefix or larger budget is used to claim completeness. New
continuations re-enter the same discovery worklist and retain native stack
operations/live-M/X dispatch. A long jump requires separate return-bank
evidence and is not covered by this immediate query. Stack inventories also
respect the source HLE/body/width admission gate.

Open jump inventories also admit two bounded cold-entry shapes:

- `ASL A` repeated before `TAX; JMP (abs,X)` can identify a pointer field
  in 2-, 4-, 8-, or 16-byte records. The query requires a unique predecessor
  chain at native M0X0, examines at most 256 records, and stops at a bank
  boundary, invalid/padding/data target, overlap with the table, or the first
  handler boundary. It skips the intervening metadata fields. This structural
  prefix is **not** proof of selector bounds or record count. Multiply-through-
  `ADC` patterns are not accepted without carry/decimal evidence.
  For a two-byte word table, null slots may be crossed only after two valid
  pointers establish a prefix and a forward pointed-to code address bounds
  the scan. A null is never a target or proof of termination. An unresolved
  table can use this cold query even when its early null prevented ordinary
  dispatch recovery; an authored closed dispatch is not extended. Nonzero
  invalid targets still stop scanning, and the native index domain remains
  open. Newly admitted handlers re-enter the same discovery worklist.
- An already address-taken target beginning with 3–63 identical one-byte
  accumulator shifts/rotates followed by `RTS` exposes its individual suffixes,
  including the final `RTS`. An explicit data boundary rejects the sequence.
  This is not a backwards scan for arbitrary instruction-looking bytes.

These roots enter the existing discovery fixed point: their direct callees
and stored references can expose further roots in later passes. The runtime
still computes the actual pointer and dispatches using live M/X; neither
query closes the target set or imports runtime observations. Existing authored
data, HLE and body-override admission barriers remain in force.

The same record-prefix query can follow a word read into A through an absolute
low-WRAM store to a separately decoded `JMP (abs)` slot consumer. This narrower
join requires native M0X0, adjacent `PHK; PLB` followed by the consecutive shifts
and `TAX`/`TAY`, and mapper-equivalent source/consumer ROM banks. It accepts up
to four intervening record loads into the other index register and four
redundant absolute A stores; clobbers, calls, joins and unknown bank state stop
it. A writer in a full-ROM bank is not a low-WRAM writer. Slot lifetime and
execution order remain unproven; these are cold address candidates, not a
license to replace the actual load/store/jump or choose a canonical M/X variant.

A complementary finite-domain query reuses the initializer value/bank analysis
for indexed ROM words immediately stored to DP or absolute low-WRAM slots used
by decoded `JMP (abs)` consumers. Masks and shifts can bound sparse indices to
at most 256 values; unknown larger domains are not replaced by guessed table
extents. Bank setup may follow `TAX`/`TAY`, and unrelated record fields may be
read/stored without losing index provenance. It reads only those finite cells:
a null cell does not hide later permitted cells. The first pointed code boundary
also stops the inventory: a finite mask is not evidence that following native
instructions are more records. This boundary uses physical mapper identity, not
merely equal numeric addresses in different banks. Cross-bank/wrapped word reads
are excluded. Source DB and consumer PB are distinct; absolute stores require
a known low-WRAM-mirroring bank. DP/DB aliasing, reaching definitions and slot
lifetime remain conditional. Mapper-aware HLE/body/width/data boundaries apply
at both endpoints, including internal and mirrored targets. Like all cold
queries, it does not rewrite native operations or export proven facts.

The inventory also recognizes indexed word fields forwarded through a DP
scratch pointer into `JMP (abs)`. Literal A values stored to a matching
`dp,x` or `abs,x` field spelling can supply open candidates in the consumer's
decoded program bank. This is not proof that two accesses use the same object,
D/DB/index, lifetime, or feasible path. Dynamic/ROM-stream-derived values remain
unknown. There is no raw-data scan, closed edge replacement, or inferred HLE
equivalence. The new query excludes HLE/body-overridden entries and banks with
exclude ranges or HLE dispatch overrides. It follows at most 32 predecessor
instructions and abandons an entire field inventory above 64 literal values;
it never retains an arbitrary overflow prefix. Real pointer reads, live M/X
dispatch, authored boundaries and hard missing-body guards remain in place.

Literal long-pointer inventory can reconstruct all three byte lanes from a
unique, bounded path of known low-WRAM stores. This includes three byte writes
and overlapping word writes in either order: the last write to each byte wins.
It does not combine independent writers or select an arm of a multi-valued
literal join. Unknown contributing values, calls, branches, stack operations,
indirect/indexed/hardware writes and changes of DP/absolute spelling stop the
32-instruction walk. DP/DB aliasing and lifetime relative to the decoded
`JML [abs]` consumer remain conditions, not proven facts. Both endpoints pass
the existing authored/HLE admission gate; native pointer operations and live
M/X dispatch are unchanged.

### Conditional native command operands

Once the cheaper discovery queries stabilize, regeneration reuses the existing
command-root, native data-arm, callback and refetch analysis over the current
decoded graphs. It admits the resulting operand addresses as open cold entries
in all four native M/X variants. Their bodies and direct/stored references feed
the same fixed point again. The pure `tooling.AnalyzeDecodedCommands` bridge
shares the report implementation rather than introducing another script
decoder. It reads no files or runtime observations, replaces no ROM operations,
and is not an interpreter fallback.

Two independently recovered pointer-table indices can also anchor bounded
neighbor probes when the decoded index expression is one to four accumulator
shifts (stride 2–16) and both table-read and stream-use banks are known.
Each original anchor probes at most 32 records in each direction. Probes stop
at nulls, bank/mapping boundaries, decoded code, or the first pointed stream;
they never recursively become anchors. At most 2048 references per contextual
root are retained, with truncation explicitly marked. These references carry
`open_neighbor_ROM_stream_reference`, not literal/call-path value provenance.
No arbitrary data byte is searched for a convenient command tag.

The native walker still needs an exact cursor and compatible bank/owned-stack
effects before following another command position. Its existing instruction,
path and call budgets and conservative unknown-effect stops remain. Explicit
HLE/body/width/data policies block source inference and target admission,
including mirrored addresses. Existing speculative byte ownership can stop
further walking; a mapped cold operand is not promoted to confirmed code just
because it decodes. Neither neighbor inventories nor conditional walks export
proven analysis-database entries or claim complete gameplay coverage.

The cold bridge can also query non-callback commands with the same bounded
native path/stack engine, starting with no command-owned return frame. Branch
arms stay separate. Entry D-clear and D-set conditions are explicit; unknown
decimal cursor arithmetic remains an unresolved alternative, not a substituted
binary calculation. Ordinary shadow reporting does not enable this extension.

Point-in-time cursor publications may supply a **separate conditional stream
root** when an independently decoded `LDY dp,X` or `LDY abs,X` path reloads
the same field spelling and reaches the known fetch with an exact cursor delta.
That query crosses only existing decoded edges, at native M0X0, for at most 16
instructions; unknown cursor/bank effects and conflicting ownership stop it.
The store must reach the reload without overwrite, its D/DB/X field aliases
must agree, and the stream bank must survive. These remain explicit lifetime
conditions: a store does not establish an immediate refetch or prove a return.
No actual wait, frame, callback, or memory operation is skipped by generated code.
Publications in a summarized command prefix are retained separately from
publications inside its callback. A callback scout starts after that prefix;
recognizing the complete callback must not erase an earlier saved-cursor store.
Its evidence is labeled `command_prefix:<stop reason>`, and still supplies only
a conditional later reload root, not an assumed callback return.

These derived data positions feed a fixed-point query worklist (at most
4096 unique selector/position roots). Already materialized suffix positions
reuse their analysis; positions whose outgoing alternatives were cut short
by a stream path/command budget remain eligible for a fresh query. This
avoids spending the root budget repeatedly on overlapping suffixes and does
not impose an arbitrary wave-depth cutoff. `published_cursor_truncated` reports
unprocessed new roots when the global position budget is reached. Other native
path/work-budget stops remain explicit in the walk records. Unlike proven-fact promotion, an incomplete cold
inventory is usable but is never called complete or gameplay-reachable.

## Evidence is not reachability

These are **open address references**, not closed dispatch facts. Matching a
field spelling does not prove DP/DB aliases or writer order. Literal words on a
stack may be data. Multiple M/X interpretations remain possible. Every admitted
entry therefore retains live-M/X dispatch and the real memory/stack operations;
it must not be promoted into a closed analysis-database fact.

The object-table query requires a decoded consumer that pushes object fields
for RTL, paired word/bank ROM reads stored into those fields, and matching index
provenance. Literal arguments can pass through at most four direct-call wrapper
levels. Two independent literal indices may anchor a structural table prefix:
the common stride must be 3–16 bytes, at most 256 records are considered, and
every record must map into the ROM. The literal-bounded query still stops at the
largest anchor and rejects an invalid interior rather than bridging a hole.

Experimental regeneration additionally permits a cold structural extension
beyond those anchors when **both writer and consumer** have passed the authored
override/HLE admission checks. The entire anchored prefix must satisfy the
stronger mapped, non-padding, non-data checks. Extension stops at the first
invalid record, mapped code-target boundary, source-bank edge, or 256-record
budget. It preserves the consumed three-byte pointer and native RTL PC+1
semantics. This is not proof of record count or selector bounds. Intermediate
and extended records remain speculative even when all decode; unknown indices
alone never authorize a scan. An overflowing literal set still rejects the
query rather than picking an arbitrary subset or guessing a stride.

New roots can expose much larger direct-call closures and speculative sibling
variants. With **both** experimental discovery and `--allow-stubs`, a body that
exceeds the instruction budget becomes a named, hard diagnostic stub, counted
separately in the regeneration report. It is never silently truncated or
replaced by another width. Ordinary regeneration still rejects budget failures.
Executing any such guard fails runtime acceptance.

Per-root serialized provenance/confidence and comprehensive HiROM shadow-tool
coverage are still pending; this flag is not a substitute for the proven-fact
overlay or its conflict gate.

## Control-flow invariants

- Statically known calls and branches keep compiled fast paths.
- Automatically recovered `JMP (abs)` and `JMP (abs,X)` target inventories
  remain open in ordinary regeneration too. Generated code reads the actual
  native pointer (including odd byte indices), uses exact live-M/X internal
  gotos where available, and falls back to the existing sparse AOT registry.
  A zero word or a selector past the discovered prefix is not a synthetic
  return. Missing bodies still emit the actual trapped edge and diagnostic.
  Unknown target exits propagate unknown M/X to callers; a known prefix's
  return widths do not summarize the whole runtime domain. Authored dispatch
  contracts and HLE overrides remain authoritative. This does not yet migrate
  legacy automatic indirect-JSR or ExecutePtr-helper emission.
- An unresolved indirect jump computes its native target first. It may enter
  only an existing exact live-M/X AOT body; a missing body remains a diagnostic.
- A callee that pops its frame and jumps to its saved PC can resume only its
  immediate active owner, with matching PC, frame width and stack position.
  No ancestor/address-only search can manufacture a continuation.
- A pushed RTS/RTL target below the active call's stack position retains that
  call's ownership through the flat dispatch driver. A registered continuation
  must not execute and then execute again when a suspended C caller resumes.
- JSL pushes the **live** PB, including mirrored entry banks.
- With the separate opt-in `--experimental-parked-waits`, a closed
  `WAI; BRA/BRL WAI` loop can return `RECOMP_RETURN_PARKED_WAIT` to the
  runner. Generated callers propagate that reason without restoring PB/S.
  The host adapter owns interrupt delivery and must preserve the parked CPU
  context. Existing adapters must not enable this flag without handling that
  return reason. Default generation retains the previous WAI contract.
  This is not a general coroutine transformation for arbitrary WAI.
- Unresolved edges and calls to uncertain sibling variants do not by themselves
  prove a wrong-width decode. Garbage-width pruning must not use those as proof.

No interpreter was added. Existing legacy runtime recovery behavior is not
evidence that newly generated code is correct; strict bring-up must reject
missing dispatches and must not register a game recovery callback.

## Diagnostic observed-root experiments

### Existing internal jump destinations

`--experimental-internal-tails` is a separate, isolated-output experiment
(full regeneration only; not `--banks`). It
exposes already decoded local `JMP abs`, `BRA` and `BRL` destinations through
cold wrappers into their existing generated regions. This helps when a command
stream jumps directly into shared cleanup that another handler reaches with a
normal branch. It does not decode additional code roots, read arbitrary ROM
words as pointers, or prove that any runtime dispatch edge executes.

Selection happens after ordinary variant discovery and pruning. The target must
be an exact existing block with no saved PHP history, and independently decoding
there must reproduce its native instruction/edge closure. Conflicting closures,
HLE entries, authored body/stack metadata, entry-dependent pruned graphs and
already-shared regions are conservatively skipped. Existing named entries win;
the registry does not fill in missing widths by borrowing another variant.

The output-only wrappers are **not** ordinary routine roots or new decoder
sibling boundaries. They push no native return frame, inherit the active tail
dispatch's return ownership, and enter the chosen internal label. Hot direct
edges stay gotos; no owner prologue or earlier callback code is replayed. Cold
entries have no ordinary void alias. Registry membership still does not mean
gameplay reachability, and newly exposed unknown live widths still trap.

This trades a larger sparse registry and extra small wrappers for fewer missing
internal targets. Benchmark isolated A/B builds before adoption. It is not yet
combined with existing proven shared-region plans, nor exported as closed
analysis-database evidence. Omit the flag to retain default output behavior.

### Observed native roots

`--observed-dispatches census.json` accepts the JSON produced by
`dispatch-census --rom game.sfc --trace startup.jsonl --out-analysis census.json`.
It adds missing native exact-M/X AOT bodies in memory and requires isolated
output. The input must match the ROM SHA-256, include a trace SHA-256, and have
no overflow. Authored data conflicts, unmapped targets, and emulation entries
are rejected. Entries marked as continuations are not imported as functions.

The option is repeatable: retain earlier census files when recording a later
run, because previously missing bodies will then be reported as generated.
Keep these observations separate from the proven static database and authored
configuration. An observed-root build is **not** a static-only one-shot result;
use its next missing targets to develop independently tested static queries.
Coverage proves only what that run visited, not all possible handler targets.

### Next static query: tagged command streams

An observed callback can identify a missing data relationship without becoming
a static fact. One recurring shape has two levels: a finite animation/state ID
selects a ROM stream pointer; a tagged command byte selects a native command
handler; that handler consumes a callback word and dispatches through a scratch
slot. The stream bank and callback bank need not be the same.

Recovery needs to join those relationships, not scan a bank for code-looking
words. Infer the command selector and table from decoded instructions, derive
operand consumption and branch/return behavior per command, and walk only
independently rooted streams within an explicit budget. Unknown command lengths,
mutable input, ambiguous bank state, or unbounded pointer changes stop the walk.
Do not assume that all callbacks return with RTS: some jump into shared stream
cleanup or choose between two continuations. Keep open address inventories
separate from proven closed dispatch sets, and test the resulting native
continuation behavior independently with synthetic ROMs.

The first increment is now available as the **report-only native command
layout query** in [analysis usage](ANALYSIS_USAGE.md#native-command-stream-layouts).
It summarizes decoded callback-operand prefixes, including sibling command
bodies, bank separation, cursor saves and literal stack words. The second
increment follows literal call arguments through decoded wrappers to mapped
ROM stream references, preserving separate pointer-table and cursor-use banks.
Both intentionally remain outside the experimental cold-root generator.
Report v29 can now read immediate/deferred callback operands from those
conditional stream references and follow agreeing, bounded native refetch
paths. Its open candidate addresses still do not enter this generator or the
proven-fact database. Report v31 additionally scouts operand-rooted callbacks
with command-owned saved Y/DB and matched native return bytes. Only a balanced
native refetch with a known cursor and restored DB extends the conditional
walk. These scout decodes do not acquire code ownership or enter this option's
generated target inventory; stack/ROM non-aliasing and root/branch feasibility
remain obligations. See `ANALYSIS_USAGE.md` for barriers and budgets.

Report v30 additionally follows conditional native data
arms that refetch directly, without treating published cursors as resumptions.
Unknown callback returns and cross-invocation object lifetimes remain barriers
to walking beyond those points and to proven-fact promotion. The experimental
cold-entry bridge described above can inventory already recovered conditional
operands without satisfying a closed-edge proof; the ordinary shadow report
remains report-only. Neither observed callback values nor a prefix's cursor
advance supplies missing successor proofs.

The experimental native-command walk also follows a word read through the
current ROM stream cursor into a replacement Y cursor. Its symbolic read
retains the load PC, old-cursor offset, source bank and subsequent INY/DEY
adjustment, including an owned push/pull of the word. Only a refetch with
restored entry DB and no outstanding owned stack/call bytes resolves the
operand against a rooted ROM stream. Unknown pointers, changed widths/banks,
bank wrapping and unmapped reads stop this path. The word is a script-data
reference, not a code target: only the existing command/handler analysis may
discover code from the new position. Existing path/cycle/work budgets and
authored HLE/data barriers remain unchanged. This query does not run at runtime
or add observations or proven facts.

## Shared handler setters

Literal slot discovery follows bounded predecessor joins at a shared store,
retaining the union only when every incoming path supplies an understood
literal of the store's width. Unknown values, register/status clobbers, calls,
cycles, more than 32 backwards steps, 128 predecessor instructions or 64 values
reject the joined query as a whole. Known values are not substituted for an
unknown arm. Short indirect-jump slots can also receive a literal parameter
through a decoded setter helper; their target bank remains the consumer's
program-bank context, not a fabricated long-pointer bank.

This remains an open, experimental cold-entry inventory. It neither proves
writer/reader aliasing or execution nor closes the runtime target set. Actual
pointer reads, live M/X dispatch, authored data/HLE behavior and hard missing
target diagnostics remain in place. Default regeneration does not enable it.

## Deferred long callbacks

The experimental cold query also joins decoded long-pointer consumers with
native callback-record setters. It tracks the three pointer byte lanes through
scratch stores, retaining the record index and address-space epochs across
overlapping word reads. A setter may pair its entry A word with the low byte of
native `LDA $03,S`: before any stack adjustment, that byte is the direct JSL
caller's program bank, not the setter's bank or DB. Finite arguments at those
JSL sites supply candidate long callback entries, which re-enter the ordinary
discovery worklist with all four native M/X variants.

This is a conditional inventory, not proof that two records alias or that the
queue executes. D/DB aliases, stack non-aliasing, publication and record lifetime
remain obligations. Unknown arguments, shifted stack frames, inconsistent
record indices, scratch clobbers and authored HLE/data boundaries reject the
corresponding query. Metadata overlapping the unused high byte of a bank-word
store is not treated as another pointer bank. The native enqueue/dequeue code,
live target lookup and continuation handling are unchanged. No observed target
is imported by this query.

## Computed returns and post-call widths

Cold discovery also joins finite ROM record-pointer initializers with a later
slot reload and `JSR (field,X)` / `JMP (field,X)`. It follows both ROM reads,
using the initializer's table bank and the consumer's program bank separately.
The finite index mask is not a table length: pointed record storage bounds the
pointer-table prefix where those physical ranges overlap. Null cells, bank
crossings, byte-width clobbers, HLE and data ownership retain their barriers.
New handlers and their direct callees re-enter the existing discovery worklist.

When local bank provenance stops at calls, this cold query can reuse bounded
native bank/stack summaries. Its saved-byte restoration is explicitly
conditional on writes not aliasing those local stack bytes. This can inventory
a candidate body, but cannot establish a DB preservation fact, table closure,
reachability or M/X override. Published bank analysis still poisons saved bytes
on potentially aliasing writes. Unknown pulls, recursion without a base proof,
authored replacements and analysis budgets do not acquire a bank assumption.
No runtime interpreter or observed-target import is involved in this query.

An unconfigured `JSR (abs,X)` is a real call, not a suppressed operation or
a successful return from its containing routine. Generated code pushes its
native return frame, reads the pointer in live PB with 16-bit address wrapping,
and looks up the target using live M/X. A missing body emits a trapped-dispatch
record and a runtime diagnostic; it never silently executes the continuation.
The active call owns that continuation, which selects an internal block using
the post-call live widths. Parked waits and owned/non-local returns use the same
call boundary as direct calls. This correctness fix affects ordinary output;
authored dispatches and existing recovered-table fast paths are unchanged.
The stack-before-pointer ordering follows WDC's
[W65C816S bus-cycle table](https://www.westerndesigncenter.com/wdc/documentation/w65c816s.pdf)
for absolute indexed indirect JSR. This is not a cycle-exact bus emulator.

Registry dispatch installs PB from the actual requested target before entering
the generated body, even when lookup selects a canonical mirrored body. This
also applies to unpaired RTL continuations and each flat-driver iteration.
Same-bank split-body and configured dispatch-return handoffs use live PB plus
the destination offset: a compiler body boundary is not a bank-changing native
instruction. These two rules are paired; canonicalizing the handoff would make
PHK and PB-relative jump tables observe a different bank. This correctness
contract applies to ordinary generation/runtime, not only cold discovery.

If native code discards inner call frames and an RTS/RTL pops the exact frame
of an active outer generated call, an owned-unwind token resumes that call's
existing C continuation. Matching requires the nearest active frame at that
stack position, its exact 24-bit continuation, native frame size and post-pop
S. A different PC/bank, a wrapped stack, a foreign CPU, reset ownership or a
non-monotone chain rejects this path. Matching a recursive ancestor's PC alone
is insufficient. Intermediate calls and split/cross-bank tails propagate the
token without restoring S/PB or decrementing a host-depth counter. Only the
matched call consumes it, once; abandoned scopes/reset clear pending ownership.
This does not add the return PC to the global function inventory. Ordinary
direct returns retain their fast path, and unknown cases retain diagnostics.

A native word push immediately followed by RTS is not evidence that the
routine returns to its caller with the dispatch-site M/X. A handler may execute
first and change either width. Exit analysis retains all possible native widths
for this shape instead of publishing a false exact summary. This uncertainty
propagates through its direct callers until native status changes establish an
exact exit again. Unrelated legacy mixed-exit inference is not migrated by this
bounded fix. Generated post-call continuations
select the corresponding internal block using live M/X; they do not choose the
first decoded variant or create a new host activation. Exact exit summaries
continue to use direct gotos. This correctness fix is not gated on experimental
cold-entry discovery, and can change normal generated output.

The current recognition is deliberately bounded to immediate word-push/RTS
edges, not a claim of complete abstract-stack analysis for arbitrary stack
manipulation. Authored exit overrides remain authoritative.

In trace builds, a missing RTS/RTL target receives explicit missing-handler
evidence when the native stack check proves a software-pushed frame above the
untouched active caller frame. This diagnostic is distinct from the ordinary
opcode-based return guard. It does not execute or skip the missing target.
Known targets produce no additional event, preserving successful semantic-edge
fingerprints. Older traces without this evidence cannot classify every
software-return dispatch correctly from the opcode alone.

## Mapper scope

ROM access in decoding, vector discovery and generation uses the image's mapper
rather than assuming every bank begins at $8000. Headerless synthetic fixtures
retain their historical LoROM default. HiROM supports its high-half mirrors and
full ROM banks, including power-of-two image mirroring. FastROM does not select
a different layout. ExHiROM and positively identified unsupported cartridge
extensions are rejected, not repacked into a false LoROM image.

CPU SRAM shortcuts defer to the cartridge's mapping for HiROM, so full-bank ROM
reads cannot be mistaken for LoROM SRAM. Non-power-of-two HiROM mirrors and the
remaining whole-bank shadow-analysis/import queries need further contracts.
The static analysis-database import still rejects non-LoROM databases; do not
interpret a partial HiROM shadow report as complete code coverage.

## Shared cold value queries

The experimental closure now separates reusable word-value evidence from its
dispatch consumers (`internal/tooling/value_provenance.go`). Register queries
feed scalar-slot publications/reloads, finite ROM reads, record-field reads,
and arguments passed through direct calls or tail thunks. Both indexed
`JSR/JMP (field,X)` and saved `JML [slot]` pointers consume the same queries.
New values requeue dependent queries even when no new function has been added;
new candidate functions still reenter the outer, all-strategy decode closure.

This is **conditional cold inventory**, not interprocedural execution proof.
The native instruction, stack behavior, live bank and M/X state remain the
runtime authority. HLE bodies, authored data and other entry-policy exclusions
remain barriers. Authored configuration is never rewritten, and ordinary
regeneration does not invoke this engine.

The first saved-pointer consumer recognizes a word followed by an explicit
bank-byte argument stored on one unique path, including caller arguments
forwarded through thunks. Both lanes retain the same bounded caller context;
unrelated callers' word/bank sets are never cross-multiplied. It admits literal
addresses and fields of self-delimiting record-pointer prefixes. A masked ROM
read alone does **not** establish table ownership or handler entries. Other
split/overlapping literal lanes remain handled by the existing literal pass;
general table-derived bank bytes are not yet supported by this consumer.

Uncertain slot alias/lifetime, saved-stack non-aliasing and bank hypotheses are
reported as conditions. If DB is unknown, a separately banked spelling of the
same table in the same native bank can provide a conditional table identity;
a record pointer can also suggest its table's bank. Neither hypothesis replaces
a known DB value, nor becomes a published bank-preservation fact. This can add
speculative cold entries, not a declaration that they execute.

The worklist is deterministic and bounded: 65,536 queries, 524,288 evaluations,
256 word/provenance alternatives per query, four caller-context levels and
1,024 contexts per queried entry. Unsupported operations, ambiguous predecessors,
unbound arguments, cardinality and context limits are explicit boundaries.
Unseeded value cycles remain empty. Global node/work exhaustion admits no
targets from this engine. `converged=true` means the bounded worklist settled,
**not** complete code coverage or an absence of those boundaries. Other existing
analysis passes have not all been migrated to this shared model yet.

### Rooted command inputs and rebased cursor fields

The command walker now supplies the shared engine with per-invocation Y and
DB from already rooted ROM commands. It supplies no native stack bytes,
branch outcomes, or observed values. These inputs can pass through direct
transfers into existing stream-table initializers. In particular, restoring
an opaque caller bank byte does not erase a previously loaded A value; it
also does not prove that the native command returns or refetches safely.
The ordinary stack scout keeps its unresolved boundary. New data-stream
pointers reenter the command worklist, and newly encountered commands supply
further inputs until it settles. The JSON inventory's `shared_value_queries` summary
reports inputs, query/evaluation counts, conditions and unresolved boundaries.

Cursor publications can additionally match a rooted reload when both use
`[D + pointer-slot] + finite-offset` as X. The current affine query accepts
a word DP pointer load, or a word `ADC dp` with locally established clear
carry and a singleton displacement from the shared value queries. Binary
arithmetic is an explicit condition; known decimal mode is rejected. A broad
mask domain is not a singleton field selection. The store and reload retain
their separate command-input provenance, and their normalized field offsets
and pointer-slot spellings must agree. `LDX` after a cursor load can restore
the object index without destroying the Y value being followed to the fetch.

The store and reload need not appear in the same discovery wave: each stays
registered so a newly rooted matching consumer can expose an earlier saved
cursor. This is a conditional data-flow relationship, **not** proof of object
identity, pointer-slot lifetime, native return ownership, or branch feasibility.
It neither scans unrooted script data nor turns these operands into closed
dispatch facts. All native reads, writes, waits and live-state dispatch remain.
HLE and authored data boundaries remain authoritative at both endpoints.

## Acceptance

Use redistributable synthetic ROMs for each control-flow contract. Real-game
checks must include native startup/interrupt behavior, hard diagnostics, CPU,
WRAM/SRAM and semantic dispatch edges—not just final framebuffer appearance.
Preserve the frozen baseline and investigate changes even when final state
converges. A corrected duplicate continuation can legitimately change an edge
sequence, but requires an independent exact-once contract rather than deleting
the extra event from the comparison. Benchmark only after compilation finishes
and the host is sufficiently quiet, with warm-up and repeated adjacent A/B runs.
