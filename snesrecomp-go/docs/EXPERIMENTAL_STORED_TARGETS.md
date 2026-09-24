# Experimental cold-entry discovery

`--experimental-stored-targets` adds candidate entry discovery to regeneration.
It is useful when bringing up a new game whose indirect callbacks are not yet
fully configured. It is experimental and does not guarantee a playable result.

Use a copy of the game's configuration and a separate output directory:

```sh
v2regen regen --rom game.sfc --cfg-dir copied-recomp \
  --out-dir isolated-gen --funcs-out copied-recomp/funcs.h \
  --experimental-stored-targets --allow-stubs
```

Ordinary regeneration does not enable these passes. The flag does not rewrite
the authored configuration. Keep HLE, data boundaries and body overrides until
independent evidence supports changing them.

## Interpreting candidates

The inventory can find stack continuations, stored handler pointers, setter
arguments, ROM callback tables and conditional command operands. These are open
address references, not proof that a gameplay path reaches them. A matching
field offset does not establish its data bank, record identity, writer order
or lifetime. A word on the stack can also be data rather than a return address.

Generated calls retain native memory and stack operations and select M/X from
live CPU state. Do not copy speculative candidates into a closed dispatch table
or treat them as proven analysis-database facts. The
[analysis guide](ANALYSIS_USAGE.md) explains command walks, conditional evidence
and configuration comparisons; the [configuration reference](CFG_FORMAT.md)
describes explicit dispatch declarations.

New candidates can expose large direct-call closures. With this flag and
`--allow-stubs`, an instruction-budget overflow can produce a named hard
diagnostic stub in the report. `--allow-stubs` permits generation to finish;
it does not provide an interpreter fallback or make that body executable.
Treat any executed missing-body diagnostic as a failed bring-up check.

## Checking a new port

Keep a known baseline and compare startup, interrupts, CPU state, WRAM/SRAM
and ordered dispatch events. A matching final framebuffer alone cannot establish
correct control flow. Test save/load and continuation behavior separately, and
use redistributable synthetic ROMs for isolated instruction/stack contracts.

See [project integration](PROJECT_INTEGRATION.md) for frame scheduling and the
game/runner boundary. Measure performance after generation and compilation have
finished, with warm-up and repeated comparisons under the same workload.
