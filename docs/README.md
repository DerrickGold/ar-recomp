# ActRaiser game documentation

This directory contains the maintained player documentation and durable
reverse-engineering knowledge that can support this project or an independent
ActRaiser decompilation. The hermetic builder distributes an explicit subset
of these files; implementation plans, debugging journals, resolved-defect
histories, and retired specifications are intentionally excluded.

## Player and project status

- [Manual](manual.md) — running, configuring, and extending the game.
- [Project progress](progress.md) — public gameplay, subsystem, and platform
  acceptance status.

## Reverse-engineering references

- [Logic and hardware seams](SEAMS.md) — semantic boundaries, subsystem roles,
  and hookable game identities.
- [RAM map](ram-map.md) and [ROM map](rom-map.md) — known state and data regions.
- [Semantic symbol map](research-symbol-map.md) — address-to-purpose mappings
  with confidence and evidence.
- [Rendering engine](rendering-engine.md) — original drawing, streaming, OAM,
  and presentation behavior.
- [Save format](save-format.md) — SRAM layout, checksum, and field encodings.
- [Simulation object catalogue](sim-object-catalog.md) — simulation-mode
  records and visual identities.
- [Simulation terrain](sim-town-terrain.md) — town-cell elevation and sampling
  contracts derived from the original maps.
- [Diorama depth shapes](diorama-depth-shapes.md) — stable authoring and geometry
  vocabulary used by enhanced presentation.

Research ledgers that still combine original-game findings with implementation
history remain source-tree material until the reusable findings are promoted
into the maintained references above.
