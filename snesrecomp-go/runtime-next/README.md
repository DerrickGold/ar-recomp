# Portable runner (`next`)

This directory is the independently authored replacement for the inherited C
runner in `../runtime`. Both remain selectable so projects can run behavioral
comparisons during rollout.

Current status:

- `legacy` remains the default comparison runner until project release policy
  explicitly changes it.
- `next` is a standalone playable runner. Its manifest has no legacy sources
  or include paths and reports `legacy_source_count == 0`.
- The implementation covers hashing, ROM/SRAM and LoROM/HiROM mapping, 65816
  generated-code ABI and dispatch, DMA, the SNES bus/register model, PPU,
  APU/SPC700/S-DSP, MSU-1, frame/audio pacing, save state, diagnostics,
  widescreen and overlay presentation, key bindings, and launcher utilities.
- `cmake -S runtime-next -B <build-dir>` builds only independently authored MIT
  sources and runs without SDL, a ROM, generated game code, or the legacy
  runtime.

Design requirements for every replacement subsystem:

- portable C11 at the core, with fixed-width public ABI types and no compiler,
  operating-system, graphics, or audio-library types in core headers;
- host integration through narrow adapters rather than platform conditionals
  spread through emulation code;
- no allocation, locking, or mutable one-time initialization in hot paths;
- deterministic contract tests that can be compiled by Clang/GCC, MSVC, and
  Zig cross targets; and
- performance measurements before a replacement becomes the release default.

From the ActRaiserRecomp root, select a game build with either:

```sh
cmake -S . -B build-next -DSNESRECOMP_RUNNER=next
cmake --build build-next
```

or:

```sh
go run ./snesrecomp-go/cmd/snesbuild build --runner next
go run ./snesrecomp-go/cmd/snesbuild build --hermetic --runner next
```

The MIT grant covers this runner and its manifest. It does not cover ROMs,
generated game code, extracted media, or the separately selectable historical
runner; see `../LICENSE` and `PROVENANCE.md` for the precise boundary.

The post-cutover component-access and low-copy ABI plan is tracked in
[`ABI_ROADMAP.md`](ABI_ROADMAP.md). It deliberately keeps game semantics in
adapters above the portable runner.

Measured hotspots and the clean-room scanline optimization sequence are
tracked in [`PERFORMANCE_ROADMAP.md`](PERFORMANCE_ROADMAP.md).
