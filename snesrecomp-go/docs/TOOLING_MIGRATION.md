# Go tooling boundary

Reusable compiler, ROM-inspection, trace, and bring-up mechanics belong in
`snesrecomp-go`. Game content knowledge, extraction profiles, user interfaces,
and release recipes belong in each consuming project.

The generic commands include disassembly, ROM cross-reference analysis, return
web and link audits, trace inspection/diffing, ROM metadata, SPC disassembly,
poll census, WRAM inspection, CHR rendering, and manifest-defined replay
benchmarks. These commands accept caller-provided inputs and use synthetic
fixtures in their tests.

Project-owned tools should integrate with `snesbuild` through the versioned
JSONL contract in [PROJECT_INTEGRATION.md](PROJECT_INTEGRATION.md). They must not
import `internal` packages, add game selectors to the generic manifest, or put
content addresses and hashes in this module.
