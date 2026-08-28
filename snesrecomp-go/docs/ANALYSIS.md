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

This mode changes generated control flow and therefore requires the normal
runtime gate. Validation builds may define
`SNESRECOMP_SEMANTIC_DISPATCH_TRACE=1`. In that mode generated direct
dispatches and generic registry dispatches emit the same semantic
source-PC/target-PC event, while lowering-dependent registry events are
suppressed. Dispatch hashes can therefore be compared directly instead of
accepting a difference caused only by replacing a registry lookup with a
compiled switch.

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
is metadata in the shadow phase. The experimental overlay also uses proven
PHA/RTS return PCs as internal resume points: the containing generated region
decodes the continuation and reaches it with a local `goto`, while a separate
registered entry remains available for genuinely external dispatch. Ordinary
sibling entries still stop region growth.

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
reported separately and never turned into missing-handler suggestions.

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
