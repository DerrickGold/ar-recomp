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
