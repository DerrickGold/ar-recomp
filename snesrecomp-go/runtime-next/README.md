# Portable runner (`next`)

This directory is the independently authored replacement for the inherited C
runner in `../runtime`. Both remain selectable so projects can run behavioral
comparisons during rollout.

Current status:

- `next` is the default standalone playable runner. Its manifest has no legacy
  sources or include paths and reports `legacy_source_count == 0`.
- `legacy` remains separately selectable for behavioral and performance A/B
  comparisons during the transition.
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

From the ActRaiserRecomp root, the normal build selects this runner:

```sh
cmake --preset play
cmake --build --preset play
```

Supported build targets use their compile-time SIMD implementation by default.
The portable C11 paths remain complete and can be selected explicitly for
portability checks and performance A/B tests:

```sh
cmake --preset play -DSNESRECOMP_ENABLE_SIMD=OFF
cmake --build --preset play
```

The compiler target, rather than the machine running CMake, selects ARM NEON
or x86 SSE2. Cross-compiles therefore cannot accidentally emit instructions
for the build host.

PPU sprite and resolved-pixel bitsets also follow the target's native pointer
width. A 32-bit target uses 32-bit set-bit iteration without changing the
64-bit NEON/SSE2 desktop path. The width can be forced to exercise either
representation through the parity suite on a different host:

```sh
cmake -S runtime-next -B <build-dir> \
  -DSNESRECOMP_PPU_BIT_WORD_BITS=32
```

`auto` is the default; the other accepted values are `32` and `64`.

The historical runner is still available explicitly:

```sh
cmake --preset play-legacy
go run ./snesrecomp-go/cmd/snesbuild build --runner legacy
```

The MIT grant covers this runner and its manifest. It does not cover ROMs,
generated game code, extracted media, or the separately selectable historical
runner; see `../LICENSE` and `PROVENANCE.md` for the precise boundary.

The post-cutover component-access and low-copy ABI plan is tracked in
[`ABI_ROADMAP.md`](ABI_ROADMAP.md). It deliberately keeps game semantics in
adapters above the portable runner.

Measured hotspots and the clean-room scanline optimization sequence are
tracked in [`PERFORMANCE_ROADMAP.md`](PERFORMANCE_ROADMAP.md).
