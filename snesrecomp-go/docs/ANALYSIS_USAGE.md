# Analysis and configuration export

The analyzer compares independently inferred control-flow facts with a game's
authored configuration. It can help identify missing entries or conflicting
dispatch declarations without changing the project.

## Read-only analysis

From a game project, run:

```sh
snesrecomp-go/build/snesbuild analyze --root . --rom game.sfc
# Or use the lower-level command:
snesrecomp-go/build/v2regen analyze --rom game.sfc --cfg-dir recomp --format json
```

Analysis does not modify configuration or generated C. Use `--format json` for
the full machine-readable report, `--verbose` for all text records, and
`--out-analysis PATH` to export a proven-fact database. `--strict` returns
nonzero when independent evidence proves a semantic conflict.

## Interpreting comparisons

| Result | Meaning |
|---|---|
| `exact_match` | All modeled semantic fields agree, including a closed target set. |
| `compatible_guard` | The proven continuations fit an authored safety guard; extra guarded cases are not claimed to execute. |
| `partial_match` | Known structure agrees, but some fields remain unresolved. |
| `conflict` | Independently known fields disagree. |
| `authored_only` | The declaration has not been independently proven; it is not necessarily wrong or gameplay-reachable. |
| `automatic` | Analysis found a dispatch fact with no corresponding authored declaration. |

Candidate addresses and runtime observations are not interchangeable with
static proofs. A decoded or configured routine is not necessarily reached in
gameplay, and a matching table address alone does not prove every table entry.
Do not remove configuration solely because a report looks plausible.

## Native command-stream layouts

### Command-owned callback frames (report v31)

Immediate callback prefixes now retain a `native_frame`: command-entry Y
offset, DB provenance, carry/decimal knowledge, and saved bytes in push order.
Saved Y and saved DB are data, not implicit JSR frames. A PEA word retains its
own push site; an opaque caller stack remains below the tracked command bytes.

For an immediate target already read from an independently rooted command
operand, the analyzer can scout a bounded native callback path at M0X0.
`conditional_native_callback_path` records PCs, required branch outcomes,
matched JSR/JSL returns or an exact PEA/RTS continuation, and the final stack,
DB and cursor state (`cursor_known` qualifies the reported cursor offset).
Calls are followed with tagged native return bytes; their stack, Y, DB and
status effects are not assumed to be preserved. Shared cleanup uses the
existing saved data rather than acquiring a new routine frame.

Only `owned_native_refetch` continues a conditional stream walk: an actual
edge must reach the selector's exact fetch with no outstanding call/command
bytes, restored entry DB and a known affine Y. For example, `PLA; INC A; PHA`
can adjust a saved cursor before `PLY`. `CLC; ADC #n` additionally requires
known binary mode; plausible pointer arithmetic does not establish CLD.
Unknown branch outcomes remain separate, conditional paths. A return through
the opaque caller stack ends the query rather than inventing a continuation.

Scout decodes have **no code ownership** and never create production entries,
closed dispatch facts, configuration or HLE replacements. They assume native
memory writes do not alias the tracked stack or change ROM/mapping, and do
not prove branch feasibility, termination, live PB or root reachability.
Authored data/exclusions/body/width overrides, HLE boundaries (including
physical mirrors and known fallback interiors), conflicting decoded/scouted
instruction ownership, unknown widths/status restoration, direct stack-relative
memory, dynamic transfers and unsupported operations stop the query.

Bounds are 256 instructions/path, eight callback alternatives, eight nested
native calls, 64 tracked bytes, and 65,536 total callback instruction queries
per report. Repeated native sites stop conservatively, including repeated
calls to the same routine. The existing 32 stream-step / 16 stream-path limits
also apply. Budget exits leave work unknown; they never certify a closed set.

### Conditional data arms (report v30)

`native_data_paths` describes the non-negative arm of a recognized tagged-word
fetch. The bounded native query keeps conditional branches separate, recording
their instruction PC, taken/not-taken outcome, successor, M/X context and
native instruction path. Unknown object values and flags do not select a
canonical branch. These alternatives are not a proof of path feasibility.

A path reaching the exact decoded fetch records its adjustment relative to
fetch-entry Y. The stream walker may follow that conditional edge. `STY` or
`TYA; ...; STA` into an indexed field instead records a point-in-time
`conditional_cursor_publications` value. Saving a cursor does **not** prove
that a later invocation reads it: aliases, object lifetime, intervening native
callbacks and host/interrupt behavior remain unresolved. Such publications
never create a resumed walk by themselves.

The query examines at most 64 instructions per native path and eight native
alternatives. Stream walks retain at most 16 alternative paths per selector
and starting position, with 32 command/data steps per path. Loop detection
carries across tagged/data boundaries. A budget stop explicitly leaves the
remaining alternatives unknown; no convenient prefix is selected as complete.
Calls, status/bank/stack/cursor clobbers, missing or pruned edges, HLE/body
overrides and unsupported memory effects remain barriers.

Walk data steps use `kind: untagged_data` and include their
`conditional_native_data_path`. The ordinary text summary separates path
counts, distinct selector/start pairs, raw operand emissions, unique native
load/ROM-source pairs and distinct candidate addresses. Repeated branch
histories are not new source sites. Shared-cleanup addresses may be candidates
too; an address reference does not authorize treating a continuation as an
ordinary routine.

No production roots, configuration entries or closed dispatch facts are
created. The static database's proof requirements and generated behavior are
unchanged.

### Rooted command walks (report v29)

`command_stream_walks` joins existing `command_stream_roots` references with
native command-prefix operands. It starts exactly at each independently
identified first-fetch address; it does not scan adjacent bytes for tags or
accept census targets as stream roots. `root_references` indexes the root and
reference records in this same report, retaining their input-path provenance.
Repeated origins share one walk without being counted as new source sites.

Each step records the actual ROM command word, selected native handler and any
immediate/deferred callback operand's source address and value. Matching native
consumers supply **open candidate addresses** in their decoded PB and M/X
contexts. The live PB may be a mirror, and matching object-field offsets still
does not establish object identity or lifetime. These are conditional ROM
references, not gameplay reachability, closed target sets, or proven entries.
They are not imported by regeneration or the proven analysis database.

Commands also expose `native_refetch_paths`: at most 64 already-decoded,
single-successor instructions from the command entry back to the recognized
fetch. A `linear_native_refetch` records the complete Y adjustment for that
native path, including any decrement immediately before the next fetch. It
does not assume the command length equals its prefix's operand size. A
separately owned fetch can be reached through its actual decoded direct edge;
the query never concatenates owner prologues or decodes through missing code.

A walk continues only when all available native refetch summaries agree.
Unknown calls, conditional branches, Y/DB/status/stack changes, unsupported
memory effects, HLE/body overrides, and missing/ambiguous command bodies stop
it. Report v30 separately handles bounded data-arm alternatives as described
above. Cursor cycles and a 32-step budget terminate it deterministically.
Indexed-read/bank boundary crossings
are conservatively rejected, not truncated or assumed physically contiguous.
Native field writes remain conditional on ordinary RAM/ROM ownership and no
I/O, mapping, asynchronous, or host interference.

Verbose text uses `[COMMAND-WALK]` and `native-refetch` lines; ordinary text
reports the conditional walk count. A stopped walk may contain a useful
operand reference without proving the callback's return or subsequent cursor.
No function/configuration entry is created, and no HLE is removed.

### Deferred field callbacks (report v28)

`deferred_field_dispatches` identifies the native pattern
`LDA field,X; ...; STA scratch; ...; JMP (scratch)` at a known 16-bit
accumulator width. Each record retains the load/store/jump PCs, native path,
M/X, addressing mode, field offset, decoded program bank and the D required
for the scratch spelling to alias the bank-zero jump pointer. The target bank
remains live PB. The bounded predecessor query stops at ambiguous joins,
external entry boundaries, calls, unknown writes/clobbers and status changes.
Branch paths and potential stack/scratch aliases remain conditional.

A command prefix that loads a stream word and stores it into an indexed field
now reports `deferred_field_operand` and stops at `deferred_field_store`.
Matching field-mode/offset consumer records appear under
`conditional_consumers`. This distinguishes a deferred callback from an
immediate scratch dispatch, but does **not** establish object identity, D/DB/X
aliasing, intervening writes, stream roots, command length, callback values or
return ownership. Different address modes/offsets are not joined. Neither these
relationships nor their command prefixes become proven database facts.

Verbose output uses `[DEFERRED-FIELD]` and `deferred-field` lines. Independently,
the optional stored-target generator can inventory literal word writes to such
fields as open cold-AOT candidates; see
[experimental cold entries](EXPERIMENTAL_STORED_TARGETS.md). Data-derived stream
operands are never converted into literal roots by that inventory.

Report v20 adds `command_streams` (JSON) and `[COMMAND-STREAM]` records
(`--verbose`). This first increment recognizes a word fetch through Y whose
high byte selects a native command using `BMI`, `XBA`, a mask, a word-table
index, and `JMP (table,X)`. Addresses, masks, slots, and bank values come from
decoded instructions; no game-specific command numbers are built in.

Each command record describes at most 64 already-decoded instructions before
its first unsupported operation or control transfer. Callback-operand records
include the load and scratch-store PCs, offset relative to command-entry Y,
cursor advance, bank provenance, and indirect-jump site. Indexed cursor saves
are retained as **conditional** writes, not assumed nonaliasing stores. A PEA
word still on top of the symbolic stack is reported as a pushed word, never
automatically as a return PC or a call contract. The eventual target uses live
PB; the decoded program bank is only contextual evidence.

Sibling command bodies can be joined only when their exact M0X0 entry is
already decoded. Unknown calls, changed widths, cursor clobbers, unmodeled
writes, ambiguous branches, cycles, and ownership boundaries stop the prefix.
Shared native handlers retain each command index. Repeated caller contexts are
deduplicated without discarding different prefix interpretations.

**Layout records alone do not walk script data or add AOT roots.** A selector mask is not
a valid-table-length proof, cursor advance before a callback is not the entire
command length, and a callback may alter the saved cursor or choose shared
cleanup. Independent stream roots, ROM ownership/bank state, scratch aliases,
complete command successors, and live-M/X/return contracts remain explicit
proof obligations. No command-layout evidence enters `--out-analysis` facts or
removes authored HLE/configuration.

The query uses the normal shadow-analysis entry closure. It does not import
experimental regeneration roots or runtime observations automatically. A study
seeded at a known interpreter entry validates its layout only, not automatic
discovery or reachability of that interpreter. Existing v17–v19 proven-fact
databases remain supported; v20 adds report-only evidence, not new fact rules.

## Call inputs and ROM stream references

Report v21 adds `command_stream_roots` and `[COMMAND-ROOT-INPUT]` records. These
describe **ROM data references**, not generated function roots. The query joins
a pointer-table read to a direct `TAY`/command-fetch path, then follows literal
arguments through decoded direct calls, tail jumps, and external block edges.
Each reference preserves the literal definition/value, every caller and passed
register value, the callee's transformed table index, the pointer-word read,
and the resulting stream/first-fetch addresses. An input ID is never confused
with its scaled table index.

Call destinations may join through mapper-equivalent ROM addresses only with
matching decoded M/X. This does not substitute canonical PB for live PB. The
table-read bank and the bank at cursor use are recovered separately, including
literal stack-based DB setup; a long pointer-table read may use another bank
entirely. Pointer words and initial fetches must be ROM-mapped. This increment
rejects bank-boundary arithmetic instead of silently truncating addresses.

Limits are four caller levels, 128 input-query steps, and 128 references per
initializer. Recursion, ambiguous predecessors, unknown calls, width changes,
mutable-memory arguments, unknown banks, and exhausted budgets remain explicit
issues. This first query does not turn masks on unknown inputs, same-spelling
memory writers, or neighboring table records into literal inputs. Results are
conditional on decoded call paths; they do not prove gameplay reachability or
the complete input domain. They never enter the proven-fact database, regenerate
code, or remove authored HLE/configuration. v17–v20 databases remain supported.

Report v22 extends this query to **ROM-derived inputs**. A word loaded by
absolute, long, absolute-indexed, or long-X addressing can supply an ID or an
index to another ROM read. The index must resolve to a literal or another
concrete ROM word along a decoded input path; masking an unknown value does
not authorize enumerating neighboring records. Up to four nested reads per
local expression are supported within the existing shared input-query budget.

`rom_input_reads` preserves each load PC/M/X, addressing mode, bank evidence,
index, effective ROM address and loaded word, in dependency order. The seed
literal (if any), values passed through wrappers, and final pointer-table index
remain separate. A fixed ROM load has no invented `literal_value` or
`literal_definition_pc`. Successful ROM-derived references have status
`ROM_data_call_path_stream_reference`; literal-only references retain their
v21 status and fields. Verbose output includes `ROM-input` lines.

Banks must be locally constant at each load. Entry DB, DB after unknown calls,
and `PHK`-derived DB are not guessed: live PB can differ from a decoded owner's
bank through a mirror. Each byte uses the cartridge mapper, including HiROM
ROM data below `$8000`. WRAM/hardware/unmapped addresses, bank crossings, narrow
loads, ambiguous indices and read-depth exhaustion stop the query explicitly.
This is still conditional ROM evidence, not proof of cartridge-write behavior,
reachable streams, command grammar, or executable callbacks. No new facts are
exported to the generation database; v17–v21 fact databases remain supported.

Report v23 adds a narrow **local WRAM store-to-load query**, recorded in
`local_wram_inputs` and verbose `local-WRAM` lines. Both accessed byte addresses
must be known on the decoded path. It supports scalar and indexed DP, absolute,
and long forms, requiring proven D/DB and locally literal index expressions.
WRAM mirrors are normalized by byte address, so a long store through bank `$7E`
can match a load through bank-zero low WRAM without equating every `$offset,X`
in the game. Different indices, D values, or banks are never silently merged.

The nearest overlapping write must be a full-word STA/STX/STY or STZ. Partial
overwrites, RMW aliases, unknown writes, hardware/DMA writes, calls, stack
operations, ambiguous paths and external entry boundaries stop the query.
Known disjoint WRAM writes may be crossed and are retained as proof witnesses.
A loop backedge cannot supply the first invocation's store or address setup.
The search is limited to 64 predecessor instructions; local and ROM dependencies
share the four-read depth limit and existing 128-step input budget.

The stored word may resolve through literal/direct-caller values, ROM reads,
or another bounded local store. The address may not come from a guessed object
allocation or a Cartesian product of callers. Successful records have status
`local_WRAM_call_path_stream_reference` and retain store/load PCs and M/X,
both bus address expressions, normalized WRAM offsets, disjoint writes and
the substituted word. STZ is not mislabeled as a literal-load definition.

These are still **conditional local dependencies**, not whole-game reaching
definitions: interrupt/HLE/external-write noninterference and cross-call object
lifetimes are not proven. No such dependency becomes a dispatch fact, changes
generated C, or removes an authored HLE. v17–v22 fact databases remain supported.

Report v24 adds `conditional_input_paths` to root initializers. A bounded query
can follow distinct decoded predecessors through a merge instead of discarding
every value at that merge. Each expression retains its required predecessor
edges (PC, M/X, opcode and operand); resulting references copy those choices in
`required_input_edges`. These are conditional paths, **not proven branch
outcomes**. Paths are never combined into a canonical input or a closed domain.
Verbose output shows `conditional-input` and `requires-edge` lines.

This specialized query supports native word INC/DEC and index increments, plus
immediate ADC/SBC only with proven local carry and binary (decimal-clear) state.
Unknown carry/decimal flags and explicit decimal mode stay unresolved; neither
ROM plausibility nor an observed target selects a binary interpretation. Flag
evidence remains on the operation in the expression. External entries cannot
borrow a later loop iteration's value or flag setup. Other call-input, table,
generation, and HLE policies retain their existing rules.

Limits are eight alternative histories, 64 query expansions and 16 recursive
levels, alongside the existing input/reference budgets. Exhaustion marks the
root truncated. A report can contain both recovered conditional references and
unresolved paths; the former do not establish completeness. No conditional
path becomes a generation fact. v17–v23 fact databases remain supported.

Report v25 applies the same bounded expression query to direct call, tail and
decoded-boundary inputs of stream initializers. It carries each argument's
predecessor choices in `calls[].required_input_edges` and native C/D evidence
in `native_flags_before_transfer`, with exact call M/X. An entry-preserved flag
can resolve only through the **same argument call chain**, joined by mapper
identity and M/X, never by combining independent callers' value/flag sets.
Successful ADC/SBC substitutions retain `arithmetic_evidence`, including the
operation context, flag values and their definition PCs. Verbose output adds
`caller-requires-edge` and `arithmetic` lines.

An unknown callee, PLP/RTI or other status barrier cannot supply a flag summary.
Explicit decimal mode and unknown flags remain unresolved; a rejected caller
does not erase another caller's independently supported reference. If a literal
argument has no correlated incoming call chain, an unknown entry flag stays
unknown. External-entry backedges cannot supply first-invocation flags or
values, even when the first native instruction is arithmetic. This does not
prove branch feasibility, native/HLE equivalence, all callers, or whole-stream
ownership. The existing depth/path/input bounds still apply; path exhaustion
is reported as `caller_input_path_budget`. Generation and HLE policy are
unchanged, and v17–v24 fact databases remain supported.

Report v26 can cross a native direct call while tracing C/D when its bounded
decoded body supplies a compatible summary. `native_call_dependency` retains
the caller context, call PC/target, entry M/X, decoded continuations, the flag
before the call, and the callee summary. A summary reports C/D as `preserve`,
`clear`, `set`, or `unknown`, together with return PCs/M/X and nested direct-call
dependencies. `entry_flag_source` retains the correlated caller evidence when
an entry flag is substituted. Verbose output includes `native-status` lines.

The summary uses a finite symbolic fixed point over local CFG paths; conflicting
exits are joined, not selected. Direct acyclic call summaries compose only when
RTS/RTL and every returned M/X are covered by the decoded continuation states.
Known push/pull widths must leave a balanced stack depth at return and agree
at joins. Limits are 256 instructions per body, 4,096 worklist steps per summary
request, eight nested bodies, and 32 tracked stack bytes. Recursion, absent or
ambiguous mapper-equivalent bodies, pruned/computed control flow, external
boundaries, HLE replacements (including conditional hooks), SP assignment,
PHP/PLP, RTI, waits and unsupported effects remain explicit barriers. PHP/PLP
preservation is deliberately not inferred from mere textual balance.

These summaries are conditional on ordinary native matched returns, native
instruction ownership, and no stack/control aliasing or asynchronous/HLE
interference. They do not prove termination, branch feasibility, reachable code,
or a production return contract. A caller argument is still not propagated
through a callee merely because its C/D summary is known. No generated code,
dispatch fact, authored override or HLE definition is changed. v17–v25 fact
databases remain supported.

Report v27 composes native status summaries across direct long jumps and
decoded ownership boundaries, including shared tails and separately decoded
call continuations. These dependencies have `transfer: JML` or
`transfer: decoded_boundary`; they retain the active routine's return kind,
not a synthetic new call frame. Returned M/X is propagated to the enclosing
summary. Every local or shared exit contributes to its C/D join.

Composition requires zero live tracked stack bytes and no saved-status history
at the boundary. A cross-bank long tail cannot use an RTS frame, because RTS
does not restore PB. Unknown target bodies, HLE/alias conflicts, mismatched
returns, cycles and budgets remain barriers. No neighboring bytes are decoded
to fill a missing body. This is still conditional native status evidence only;
it does not change generated control flow or export code roots. v17–v26 fact
databases remain supported.

The next proof obligation is the complete command grammar: operand lengths,
data records, cursor changes, script branches, and callback continuation
effects. A valid stream reference alone does not authorize searching adjacent
bytes for code-looking words or treating an observed callback as a static fact.

## Materializing a reduced configuration

To export a usable reduced configuration, choose a new, isolated output
directory whose parent already exists:

```sh
snesrecomp-go/build/snesbuild materialize --root . --rom game.sfc \
  --out-dir build/static-bundle --jobs 8 --allow-stubs
```

`v2regen materialize` accepts the same options. The command writes `recomp/`,
`analysis-db.json`, and `gen/` only after the full and reduced configurations
produce byte-identical C and headers in proven-analysis mode. It preserves
HLE definitions and overrides and does not edit the source configuration.

Keep the database with the reduced configuration: generation validates its
schema, ROM identity, and static evidence. Speculative findings and runtime-only
observations are not exportable replacement facts. A single-bank report cannot
serve as a whole-ROM database.

`--allow-stubs` permits existing diagnostics; it does not bypass the equality
check or establish gameplay coverage. Test the exported project before adopting
it as your working configuration.

See [project integration](PROJECT_INTEGRATION.md) for generation/build options
and [configuration syntax](CFG_FORMAT.md) for authored directives.
