# Attribution and provenance

## Python source

`snesrecomp-go` is a Go reimplementation of the Python SNES static recompiler
and a continuation of its C runtime. The source lineage used for the port is:

- Original project: [`mstan/snesrecomp`](https://github.com/mstan/snesrecomp),
  created and primarily developed by Matthew Stanley.
- Development fork used by ActRaiserRecomp:
  [`DerrickGold/snesrecomp`](https://github.com/DerrickGold/snesrecomp).
- Snapshot audited while preparing this standalone module:
  [`0caf875dfcb02b8fb78a4e6a1e71280a4d48535b`](https://github.com/DerrickGold/snesrecomp/commit/0caf875dfcb02b8fb78a4e6a1e71280a4d48535b)
  (2026-07-16).

The local source history at that snapshot credits Matthew Stanley/Matt Stanley
and Derrick Gold. Git remains the authoritative per-change record; this file
is not intended to replace individual commit authorship.

The Go port preserves algorithms, configuration semantics, generated ABI, and
runtime contracts from the Python project. Source comments that say a behavior
“mirrors Python” identify intentional compatibility, not a runtime dependency.
The Python checkout is not needed to build, test, or run this module.

## Documentation carried forward

The historical README's durable material has been incorporated into the live
documentation rather than copied as stale instructions:

- the shared-framework/per-game-project model and neutral naming conventions
  are in [`README.md`](README.md) and
  [`docs/PROJECT_INTEGRATION.md`](docs/PROJECT_INTEGRATION.md);
- runtime responsibilities, MSU-1, host-overlay extraction, shadow audio/color,
  and trace tripwires are summarized in
  [`docs/RUNTIME.md`](docs/RUNTIME.md);
- contributor-facing decoder/config guidance is represented by
  [`docs/CFG_FORMAT.md`](docs/CFG_FORMAT.md) and Go package tests.

The old `IMPROVEMENTS.md`, `ISSUES.md`, branch plans, and analyzer inventories
were not copied as current documentation because they describe the historical
Python directory layout, closed investigations, and game-specific sessions.
They remain available at the pinned source commit above. Current instructions
use `snesrecomp-go`, `runtime/`, and per-project paths consistently.

## Prior-project acknowledgements

The Python project's README credits the following work, which also underlies
this port and bundled runtime:

- [`snesrev`](https://github.com/snesrev), especially `snesrev/zelda3` and
  `snesrev/smw`, for the recompiled-port model, runner ecosystem, utilities,
  ROM verification path, and default input layout on which the historical
  runtime was based.
- [`LakeSnes`](https://github.com/elzo-d/LakeSnes) by elzo-d for the C SNES
  hardware core under `runtime/src/snes/`, with individual algorithms credited
  inline to Snes9x where applicable.
- [`IsoFrieze/SMWDisX`](https://github.com/IsoFrieze/SMWDisX) for the Super
  Mario World disassembly used by the source project during conformance work;
  SMWDisX in turn credits mikeyk's original disassembly and loveemu's SPC700
  work.
- [`JRickey/gba-recomp`](https://github.com/JR-ID/gba-recomp) for the
  `ShadowVerifier` design adapted into the runtime's optional shadow-audio
  work; the carried source comment records its MIT OR Apache-2.0 provenance.
- The RetroArch/libretro project for `libretro.h` used by external oracle
  tooling. That header is not part of this module.

## License status

At the pinned snapshot, the historical `snesrecomp` repository stated that an
overall license was not yet declared. Its README also identified inherited
LakeSnes/Snes9x terms and separately licensed components.

Consequently:

- attribution does not grant permission to copy, modify, or redistribute;
- the ActRaiserRecomp root MIT license explicitly excludes this derived module
  and its copied runtime unless/until the upstream rights are clarified; and
- downstream users must review the provenance and applicable terms before
  redistribution.

[`LICENSE`](LICENSE) records the resulting no-project-wide-license notice in a
standard repository location.

No game ROM, translated ROM code, game assets, or captured memory/gameplay data
is included here. Those works are outside the toolchain's provenance and must
not be added to this module.
